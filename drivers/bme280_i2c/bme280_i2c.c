// SPDX-License-Identifier: GPL-2.0
/*
 * BME280 I2C environment sensor demo driver.
 *
 * Exposes one character device:
 *   /dev/bme280_env
 *
 * A read triggers one forced measurement and returns:
 *   temp_mC=<milli Celsius> pressure_Pa=<Pa> humidity_mpermille=<milli percent RH>
 */

#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define BME280_REG_ID              0xD0
#define BME280_REG_RESET           0xE0
#define BME280_REG_CTRL_HUM        0xF2
#define BME280_REG_STATUS          0xF3
#define BME280_REG_CTRL_MEAS       0xF4
#define BME280_REG_CONFIG          0xF5
#define BME280_REG_PRESS_MSB       0xF7
#define BME280_REG_CALIB00         0x88
#define BME280_REG_CALIB26         0xE1
#define BME280_REG_H1              0xA1

#define BME280_CHIP_ID             0x60
#define BME280_SOFT_RESET          0xB6

#define BME280_OSRS_X1             0x01
#define BME280_MODE_SLEEP          0x00
#define BME280_MODE_FORCED         0x01

#define BME280_DEVICE_NAME         "bme280_env"
#define BME280_CLASS_NAME          "bme280"

struct bme280_calib {
	u16 dig_T1;
	s16 dig_T2;
	s16 dig_T3;
	u16 dig_P1;
	s16 dig_P2;
	s16 dig_P3;
	s16 dig_P4;
	s16 dig_P5;
	s16 dig_P6;
	s16 dig_P7;
	s16 dig_P8;
	s16 dig_P9;
	u8 dig_H1;
	s16 dig_H2;
	u8 dig_H3;
	s16 dig_H4;
	s16 dig_H5;
	s8 dig_H6;
};

struct bme280_sample {
	s32 temp_mC;
	u32 pressure_Pa;
	u32 humidity_mpermille;
};

struct bme280_data {
	struct i2c_client *client;
	dev_t devt;
	struct cdev cdev;
	struct device *cdev_device;
	struct mutex lock;
	struct bme280_calib calib;
	s32 t_fine;
};

static u16 bme280_get_u16_le(const u8 *buf, int off)
{
	return buf[off] | (buf[off + 1] << 8);
}

static s16 bme280_get_s16_le(const u8 *buf, int off)
{
	return (s16)bme280_get_u16_le(buf, off);
}

static s16 bme280_sign_extend_12(int value)
{
	if (value & BIT(11))
		value |= 0xF000;

	return (s16)value;
}

static int bme280_read_calib(struct bme280_data *data)
{
	struct i2c_client *client = data->client;
	struct bme280_calib *c = &data->calib;
	u8 cal[24];
	u8 hcal[7];
	int ret;

	ret = i2c_smbus_read_i2c_block_data(client, BME280_REG_CALIB00,
					    sizeof(cal), cal);
	if (ret < 0)
		return ret;
	if (ret != sizeof(cal))
		return -EIO;

	ret = i2c_smbus_read_byte_data(client, BME280_REG_H1);
	if (ret < 0)
		return ret;
	c->dig_H1 = ret;

	ret = i2c_smbus_read_i2c_block_data(client, BME280_REG_CALIB26,
					    sizeof(hcal), hcal);
	if (ret < 0)
		return ret;
	if (ret != sizeof(hcal))
		return -EIO;

	c->dig_T1 = bme280_get_u16_le(cal, 0);
	c->dig_T2 = bme280_get_s16_le(cal, 2);
	c->dig_T3 = bme280_get_s16_le(cal, 4);
	c->dig_P1 = bme280_get_u16_le(cal, 6);
	c->dig_P2 = bme280_get_s16_le(cal, 8);
	c->dig_P3 = bme280_get_s16_le(cal, 10);
	c->dig_P4 = bme280_get_s16_le(cal, 12);
	c->dig_P5 = bme280_get_s16_le(cal, 14);
	c->dig_P6 = bme280_get_s16_le(cal, 16);
	c->dig_P7 = bme280_get_s16_le(cal, 18);
	c->dig_P8 = bme280_get_s16_le(cal, 20);
	c->dig_P9 = bme280_get_s16_le(cal, 22);

	c->dig_H2 = bme280_get_s16_le(hcal, 0);
	c->dig_H3 = hcal[2];
	c->dig_H4 = bme280_sign_extend_12((hcal[3] << 4) | (hcal[4] & 0x0F));
	c->dig_H5 = bme280_sign_extend_12((hcal[5] << 4) | (hcal[4] >> 4));
	c->dig_H6 = (s8)hcal[6];

	return 0;
}

static int bme280_comp_temp(struct bme280_data *data, s32 adc_T)
{
	const struct bme280_calib *c = &data->calib;
	s32 var1;
	s32 var2;
	s32 temp_centideg;

	var1 = ((((adc_T >> 3) - ((s32)c->dig_T1 << 1))) *
		((s32)c->dig_T2)) >> 11;
	var2 = (((((adc_T >> 4) - ((s32)c->dig_T1)) *
		  ((adc_T >> 4) - ((s32)c->dig_T1))) >> 12) *
		((s32)c->dig_T3)) >> 14;

	data->t_fine = var1 + var2;
	temp_centideg = (data->t_fine * 5 + 128) >> 8;

	return temp_centideg * 10;
}

static u32 bme280_comp_press(struct bme280_data *data, s32 adc_P)
{
	const struct bme280_calib *c = &data->calib;
	s64 var1;
	s64 var2;
	s64 p;

	var1 = (s64)data->t_fine - 128000;
	var2 = var1 * var1 * (s64)c->dig_P6;
	var2 = var2 + ((var1 * (s64)c->dig_P5) << 17);
	var2 = var2 + (((s64)c->dig_P4) << 35);
	var1 = ((var1 * var1 * (s64)c->dig_P3) >> 8) +
	       ((var1 * (s64)c->dig_P2) << 12);
	var1 = (((((s64)1) << 47) + var1)) * ((s64)c->dig_P1) >> 33;

	if (!var1)
		return 0;

	p = 1048576 - adc_P;
	p = (((p << 31) - var2) * 3125) / var1;
	var1 = (((s64)c->dig_P9) * (p >> 13) * (p >> 13)) >> 25;
	var2 = (((s64)c->dig_P8) * p) >> 19;
	p = ((p + var1 + var2) >> 8) + (((s64)c->dig_P7) << 4);

	return (u32)(p >> 8);
}

static u32 bme280_comp_hum(struct bme280_data *data, s32 adc_H)
{
	const struct bme280_calib *c = &data->calib;
	s32 v_x1;
	u32 humidity_q1024;

	v_x1 = data->t_fine - 76800;
	v_x1 = (((((adc_H << 14) - (((s32)c->dig_H4) << 20) -
		    (((s32)c->dig_H5) * v_x1)) + 16384) >> 15) *
		 (((((((v_x1 * ((s32)c->dig_H6)) >> 10) *
		      (((v_x1 * ((s32)c->dig_H3)) >> 11) + 32768)) >> 10) +
		    2097152) * ((s32)c->dig_H2) + 8192) >> 14));
	v_x1 = v_x1 - (((((v_x1 >> 15) * (v_x1 >> 15)) >> 7) *
			 ((s32)c->dig_H1)) >> 4);

	if (v_x1 < 0)
		v_x1 = 0;
	if (v_x1 > 419430400)
		v_x1 = 419430400;

	humidity_q1024 = (u32)(v_x1 >> 12);

	return (humidity_q1024 * 1000U) / 1024U;
}

static int bme280_wait_measurement(struct bme280_data *data)
{
	struct i2c_client *client = data->client;
	int ret;
	int tries;

	for (tries = 0; tries < 20; tries++) {
		ret = i2c_smbus_read_byte_data(client, BME280_REG_STATUS);
		if (ret < 0)
			return ret;
		if (!(ret & BIT(3)))
			return 0;
		usleep_range(2000, 3000);
	}

	return -ETIMEDOUT;
}

static int bme280_read_sample(struct bme280_data *data,
			      struct bme280_sample *sample)
{
	struct i2c_client *client = data->client;
	u8 raw[8];
	s32 adc_P;
	s32 adc_T;
	s32 adc_H;
	int ret;

	mutex_lock(&data->lock);
	/*再次配置温度湿度压力采集一次*/
	ret = i2c_smbus_write_byte_data(client, BME280_REG_CTRL_HUM,
					BME280_OSRS_X1);
	if (ret < 0)
		goto out_unlock;

	ret = i2c_smbus_write_byte_data(client, BME280_REG_CTRL_MEAS,
					(BME280_OSRS_X1 << 5) |
					(BME280_OSRS_X1 << 2) |
					BME280_MODE_FORCED);/*forced强制模式读取一次*/
	if (ret < 0)
		goto out_unlock;

	usleep_range(10000, 12000);
	/*轮询0xF3查看测量是否结束*/
	ret = bme280_wait_measurement(data);
	if (ret < 0)
		goto out_unlock;

	ret = i2c_smbus_read_i2c_block_data(client, BME280_REG_PRESS_MSB,
					    sizeof(raw), raw);
	if (ret < 0)
		goto out_unlock;
	if (ret != sizeof(raw)) {
		ret = -EIO;
		goto out_unlock;
	}

	adc_P = ((s32)raw[0] << 12) | ((s32)raw[1] << 4) | (raw[2] >> 4);
	adc_T = ((s32)raw[3] << 12) | ((s32)raw[4] << 4) | (raw[5] >> 4);
	adc_H = ((s32)raw[6] << 8) | raw[7];

	sample->temp_mC = bme280_comp_temp(data, adc_T);
	sample->pressure_Pa = bme280_comp_press(data, adc_P);
	sample->humidity_mpermille = bme280_comp_hum(data, adc_H);
	ret = 0;

out_unlock:
	mutex_unlock(&data->lock);
	return ret;
}

static int bme280_open(struct inode *inode, struct file *file)
{
	struct bme280_data *data = container_of(inode->i_cdev,
						 struct bme280_data, cdev);

	file->private_data = data;

	return 0;
}

static ssize_t bme280_read(struct file *file, char __user *buf, size_t len,
			   loff_t *ppos)
{
	struct bme280_data *data = file->private_data;
	struct bme280_sample sample;
	char line[96];
	int ret;
	int n;

	if (*ppos)
		return 0;

	ret = bme280_read_sample(data, &sample);
	if (ret < 0)
		return ret;

	n = scnprintf(line, sizeof(line),
		      "temp_mC=%d pressure_Pa=%u humidity_mpermille=%u\n",
		      sample.temp_mC, sample.pressure_Pa,
		      sample.humidity_mpermille);

	return simple_read_from_buffer(buf, len, ppos, line, n);
}

static const struct file_operations bme280_fops = {
	.owner = THIS_MODULE,
	.open = bme280_open,
	.read = bme280_read,
	.llseek = no_llseek,
};

static struct class *bme280_class;

static int bme280_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct bme280_data *data;
	int chip_id;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA |
				    I2C_FUNC_SMBUS_I2C_BLOCK))
		return -EOPNOTSUPP;

	chip_id = i2c_smbus_read_byte_data(client, BME280_REG_ID);/*获取id应该是0x60*/
	if (chip_id < 0)
		return chip_id;
	if (chip_id != BME280_CHIP_ID) {
		dev_err(&client->dev, "unexpected chip id 0x%02x\n", chip_id);
		return -ENODEV;
	}

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	mutex_init(&data->lock);
	i2c_set_clientdata(client, data);/*把data 添加到i2c_client的driver_datadriver_data*/

	ret = i2c_smbus_write_byte_data(client, BME280_REG_RESET,
					BME280_SOFT_RESET);
	if (ret < 0)
		return ret;
	msleep(5);

	/*获取校准参数*/
	ret = bme280_read_calib(data);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_write_byte_data(client, BME280_REG_CONFIG, 0x00);
	if (ret < 0)
		return ret;
	/*配置每次只采集一次温度*/
	ret = i2c_smbus_write_byte_data(client, BME280_REG_CTRL_HUM,
					BME280_OSRS_X1);
	if (ret < 0)
		return ret;

	ret = i2c_smbus_write_byte_data(client, BME280_REG_CTRL_MEAS,
					(BME280_OSRS_X1 << 5) |
					(BME280_OSRS_X1 << 2) |
					BME280_MODE_SLEEP);
	if (ret < 0)
		return ret;

	ret = alloc_chrdev_region(&data->devt, 0, 1, BME280_DEVICE_NAME);
	if (ret)
		return ret;

	cdev_init(&data->cdev, &bme280_fops);
	data->cdev.owner = THIS_MODULE;

	ret = cdev_add(&data->cdev, data->devt, 1);
	if (ret)
		goto err_unregister_chrdev;

	data->cdev_device = device_create(bme280_class, &client->dev,
					  data->devt, data,
					  BME280_DEVICE_NAME);
	if (IS_ERR(data->cdev_device)) {
		ret = PTR_ERR(data->cdev_device);
		goto err_cdev_del;
	}

	dev_info(&client->dev, "BME280 environment sensor registered\n");

	return 0;

err_cdev_del:
	cdev_del(&data->cdev);
err_unregister_chrdev:
	unregister_chrdev_region(data->devt, 1);
	return ret;
}

static int bme280_remove(struct i2c_client *client)
{
	struct bme280_data *data = i2c_get_clientdata(client);

	device_destroy(bme280_class, data->devt);
	cdev_del(&data->cdev);
	unregister_chrdev_region(data->devt, 1);

	return 0;
}

static const struct of_device_id bme280_of_match[] = {
	{ .compatible = "demo,bme280" },
	{ }
};
MODULE_DEVICE_TABLE(of, bme280_of_match);

static const struct i2c_device_id bme280_id[] = {
	{ "bme280_demo", 0 },
	{ "bme280", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bme280_id);

static struct i2c_driver bme280_driver = {
	.driver = {
		.name = "bme280_i2c_demo",
		.of_match_table = bme280_of_match,
	},
	.probe = bme280_probe,
	.remove = bme280_remove,
	.id_table = bme280_id,
};

static int __init bme280_init(void)
{
	int ret;

	bme280_class = class_create(THIS_MODULE, BME280_CLASS_NAME);
	if (IS_ERR(bme280_class))
		return PTR_ERR(bme280_class);

	ret = i2c_add_driver(&bme280_driver);
	if (ret) {
		class_destroy(bme280_class);
		return ret;
	}

	return 0;
}

static void __exit bme280_exit(void)
{
	i2c_del_driver(&bme280_driver);
	class_destroy(bme280_class);
}

module_init(bme280_init);
module_exit(bme280_exit);

MODULE_AUTHOR("Codex");
MODULE_DESCRIPTION("BME280 I2C environment sensor demo driver");
MODULE_LICENSE("GPL");
