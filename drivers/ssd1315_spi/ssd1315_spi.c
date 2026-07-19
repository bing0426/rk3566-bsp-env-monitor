// SPDX-License-Identifier: GPL-2.0
/*
 * SSD1315 128x64 OLED SPI demo driver.
 *
 * Exposes one misc character device:
 *   /dev/ssd1315_oled
 *
 * Write exactly 1024 bytes to update the display framebuffer. The memory
 * layout is page-major: 8 pages x 128 columns, matching SSD13xx GDDRAM.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>

#include "env_monitor_uapi.h"

#define SSD1315_MAX_SPI_HZ 10000000

struct ssd1315_data {
	struct spi_device *spi;
	struct gpio_desc *dc_gpio;
	struct gpio_desc *reset_gpio;
	struct miscdevice miscdev;
	struct mutex lock;
	u8 fb[SSD1315_FB_SIZE];
};

static int ssd1315_write_cmds(struct ssd1315_data *data, const u8 *cmds,
			      size_t len)
{
	gpiod_set_value_cansleep(data->dc_gpio, 0);

	return spi_write(data->spi, cmds, len);
}

static int ssd1315_write_cmd(struct ssd1315_data *data, u8 cmd)
{
	return ssd1315_write_cmds(data, &cmd, 1);
}

static int ssd1315_write_data(struct ssd1315_data *data, const u8 *buf,
			      size_t len)
{
	gpiod_set_value_cansleep(data->dc_gpio, 1);

	return spi_write(data->spi, buf, len);
}

static int ssd1315_flush_locked(struct ssd1315_data *data)
{
	u8 window[] = {
		0x21, 0x00, SSD1315_WIDTH - 1,
		0x22, 0x00, SSD1315_PAGES - 1,
	};
	int ret;

	ret = ssd1315_write_cmds(data, window, sizeof(window));
	if (ret < 0)
		return ret;

	return ssd1315_write_data(data, data->fb, sizeof(data->fb));
}

static void ssd1315_reset(struct ssd1315_data *data)
{
	if (!data->reset_gpio)
		return;

	gpiod_set_value_cansleep(data->reset_gpio, 1);
	msleep(20);
	gpiod_set_value_cansleep(data->reset_gpio, 0);
	msleep(20);
}

static int ssd1315_hw_init(struct ssd1315_data *data)
{
	static const u8 init_cmds[] = {
		0xAE,       /* display off */
		0xD5, 0x80, /* display clock divide */
		0xA8, 0x3F, /* multiplex ratio: 64 */
		0xD3, 0x00, /* display offset */
		0x40,       /* display start line */
		0x8D, 0x14, /* charge pump on */
		0x20, 0x00, /* horizontal addressing mode */
		0xA1,       /* segment remap */
		0xC8,       /* COM scan direction remap */
		0xDA, 0x12, /* COM pins for 128x64 */
		0x81, 0xCF, /* contrast */
		0xD9, 0xF1, /* pre-charge period */
		0xDB, 0x40, /* VCOMH deselect level */
		0xA4,       /* resume RAM content display */
		0xA6,       /* normal display */
		0xAF,       /* display on */
	};
	int ret;

	ssd1315_reset(data);

	ret = ssd1315_write_cmds(data, init_cmds, sizeof(init_cmds));
	if (ret < 0)
		return ret;

	memset(data->fb, 0, sizeof(data->fb));

	return ssd1315_flush_locked(data);
}

static int ssd1315_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct ssd1315_data *data;

	data = container_of(miscdev, struct ssd1315_data, miscdev);
	file->private_data = data;

	return 0;
}

static ssize_t ssd1315_write(struct file *file, const char __user *buf,
			     size_t len, loff_t *ppos)
{
	struct ssd1315_data *data = file->private_data;
	u8 *tmp;
	int ret;

	if (len != SSD1315_FB_SIZE)
		return -EINVAL;

	tmp = memdup_user(buf, len);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	mutex_lock(&data->lock);
	memcpy(data->fb, tmp, SSD1315_FB_SIZE);
	ret = ssd1315_flush_locked(data);
	mutex_unlock(&data->lock);

	kfree(tmp);

	return ret < 0 ? ret : len;
}

static long ssd1315_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct ssd1315_data *data = file->private_data;
	int ret = 0;

	mutex_lock(&data->lock);

	switch (cmd) {
	case SSD1315_IOC_CLEAR:
		memset(data->fb, 0, sizeof(data->fb));
		ret = ssd1315_flush_locked(data);
		break;
	case SSD1315_IOC_DISPLAY_ON:
		ret = ssd1315_write_cmd(data, 0xAF);
		break;
	case SSD1315_IOC_DISPLAY_OFF:
		ret = ssd1315_write_cmd(data, 0xAE);
		break;
	case SSD1315_IOC_INVERT_ON:
		ret = ssd1315_write_cmd(data, 0xA7);
		break;
	case SSD1315_IOC_INVERT_OFF:
		ret = ssd1315_write_cmd(data, 0xA6);
		break;
	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&data->lock);

	return ret;
}

static const struct file_operations ssd1315_fops = {
	.owner = THIS_MODULE,
	.open = ssd1315_open,
	.write = ssd1315_write,
	.unlocked_ioctl = ssd1315_ioctl,
	.llseek = no_llseek,
};

static struct gpio_desc *ssd1315_get_reset_gpio(struct device *dev)
{
	struct gpio_desc *reset;

	reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset) || reset)
		return reset;

	return devm_gpiod_get_optional(dev, "res", GPIOD_OUT_LOW);
}

static int ssd1315_probe(struct spi_device *spi)
{
	struct ssd1315_data *data;
	int ret;

	data = devm_kzalloc(&spi->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->spi = spi;
	mutex_init(&data->lock);
	spi_set_drvdata(spi, data);

	data->dc_gpio = devm_gpiod_get(&spi->dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(data->dc_gpio))
		return PTR_ERR(data->dc_gpio);

	data->reset_gpio = ssd1315_get_reset_gpio(&spi->dev);
	if (IS_ERR(data->reset_gpio))
		return PTR_ERR(data->reset_gpio);

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	if (!spi->max_speed_hz || spi->max_speed_hz > SSD1315_MAX_SPI_HZ)
		spi->max_speed_hz = SSD1315_MAX_SPI_HZ;

	ret = spi_setup(spi);
	if (ret < 0)
		return ret;

	mutex_lock(&data->lock);
	ret = ssd1315_hw_init(data);
	mutex_unlock(&data->lock);
	if (ret < 0)
		return ret;

	data->miscdev.minor = MISC_DYNAMIC_MINOR;
	data->miscdev.name = "ssd1315_oled";
	data->miscdev.fops = &ssd1315_fops;
	data->miscdev.parent = &spi->dev;

	ret = misc_register(&data->miscdev);
	if (ret)
		return ret;

	dev_info(&spi->dev, "SSD1315 OLED registered\n");

	return 0;
}

static int ssd1315_remove(struct spi_device *spi)
{
	struct ssd1315_data *data = spi_get_drvdata(spi);

	misc_deregister(&data->miscdev);

	mutex_lock(&data->lock);
	ssd1315_write_cmd(data, 0xAE);
	mutex_unlock(&data->lock);

	return 0;
}

static const struct of_device_id ssd1315_of_match[] = {
	{ .compatible = "demo,ssd1315" },
	{ }
};
MODULE_DEVICE_TABLE(of, ssd1315_of_match);

static const struct spi_device_id ssd1315_id[] = {
	{ "ssd1315_demo", 0 },
	{ "ssd1315", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, ssd1315_id);

static struct spi_driver ssd1315_driver = {
	.driver = {
		.name = "ssd1315_spi_demo",
		.of_match_table = ssd1315_of_match,
	},
	.probe = ssd1315_probe,
	.remove = ssd1315_remove,
	.id_table = ssd1315_id,
};

module_spi_driver(ssd1315_driver);

MODULE_AUTHOR("Codex");
MODULE_DESCRIPTION("SSD1315 SPI OLED demo driver");
MODULE_LICENSE("GPL");
