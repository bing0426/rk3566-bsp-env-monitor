// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO interrupt key demo driver.
 *
 * Exposes one misc character device:
 *   /dev/env_key
 *
 * The device supports poll(2). A read returns "press\n" for each pending
 * key interrupt event.
 */

#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

struct env_key_data {
	struct device *dev;
	struct gpio_desc *key_gpio;
	struct miscdevice miscdev;
	wait_queue_head_t waitq;
	atomic_t events;
	int irq;
};

static irqreturn_t env_key_irq_thread(int irq, void *dev_id)
{
	struct env_key_data *data = dev_id;

	atomic_inc(&data->events);
	wake_up_interruptible(&data->waitq);

	return IRQ_HANDLED;
}

static int env_key_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct env_key_data *data;

	data = container_of(miscdev, struct env_key_data, miscdev);
	file->private_data = data;

	return 0;
}

static ssize_t env_key_read(struct file *file, char __user *buf, size_t len,
			    loff_t *ppos)
{
	struct env_key_data *data = file->private_data;
	static const char event[] = "press\n";
	int ret;

	if (len < sizeof(event) - 1)
		return -EINVAL;

	while (atomic_read(&data->events) <= 0) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		ret = wait_event_interruptible(data->waitq,
					       atomic_read(&data->events) > 0);
		if (ret)
			return ret;
	}

	atomic_dec(&data->events);

	if (copy_to_user(buf, event, sizeof(event) - 1))
		return -EFAULT;

	return sizeof(event) - 1;
}

static __poll_t env_key_poll(struct file *file, poll_table *wait)
{
	struct env_key_data *data = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &data->waitq, wait);

	if (atomic_read(&data->events) > 0)
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static const struct file_operations env_key_fops = {
	.owner = THIS_MODULE,
	.open = env_key_open,
	.read = env_key_read,
	.poll = env_key_poll,
	.llseek = no_llseek,
};

static int env_key_probe(struct platform_device *pdev)
{
	struct env_key_data *data;
	u32 debounce_ms = 20;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->dev = &pdev->dev;
	init_waitqueue_head(&data->waitq);
	atomic_set(&data->events, 0);
	platform_set_drvdata(pdev, data);

	data->key_gpio = devm_gpiod_get(&pdev->dev, "key", GPIOD_IN);
	if (IS_ERR(data->key_gpio))
		return PTR_ERR(data->key_gpio);

	of_property_read_u32(pdev->dev.of_node, "debounce-ms", &debounce_ms);
	ret = gpiod_set_debounce(data->key_gpio, debounce_ms * 1000);
	if (ret)
		dev_warn(&pdev->dev, "GPIO debounce not supported, ret=%d\n",
			 ret);

	data->irq = gpiod_to_irq(data->key_gpio);
	if (data->irq < 0)
		return data->irq;

	ret = devm_request_threaded_irq(&pdev->dev, data->irq, NULL,
					env_key_irq_thread,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					dev_name(&pdev->dev), data);
	if (ret)
		return ret;

	data->miscdev.minor = MISC_DYNAMIC_MINOR;
	data->miscdev.name = "env_key";
	data->miscdev.fops = &env_key_fops;
	data->miscdev.parent = &pdev->dev;

	ret = misc_register(&data->miscdev);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "environment key IRQ registered on IRQ %d\n",
		 data->irq);

	return 0;
}

static int env_key_remove(struct platform_device *pdev)
{
	struct env_key_data *data = platform_get_drvdata(pdev);

	misc_deregister(&data->miscdev);

	return 0;
}

static const struct of_device_id env_key_of_match[] = {
	{ .compatible = "demo,env-key" },
	{ }
};
MODULE_DEVICE_TABLE(of, env_key_of_match);

static struct platform_driver env_key_driver = {
	.driver = {
		.name = "env_key_irq_demo",
		.of_match_table = env_key_of_match,
	},
	.probe = env_key_probe,
	.remove = env_key_remove,
};

module_platform_driver(env_key_driver);

MODULE_AUTHOR("Codex");
MODULE_DESCRIPTION("GPIO interrupt key demo driver");
MODULE_LICENSE("GPL");
