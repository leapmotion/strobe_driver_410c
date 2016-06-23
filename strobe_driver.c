#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <asm/uaccess.h>
#include <linux/fcntl.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/cdev.h>

#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/gpio.h>

#include <linux/device.h>
#include <linux/kdev_t.h>
#include <linux/sysfs.h>
#include <linux/string.h>

#include "strobe_driver.h"

//Board specific gpio in. Temporary values for dragonboard410c
#define STROBE_INT_IN 13
#define STROBE_OUT 12

static struct strobe_device device;

static int allocate_pin(unsigned int pin, int output)
{
	int ret;
	if (!gpio_is_valid(pin)) {
		printk("%d pin is not valid.\n", pin);
		return -1;
	}
	ret = gpio_request(pin, "v");
	if (ret) {
		printk("%d pin gpio_request failure\n", pin);
		return -1;
	}

	if (output)
		ret = gpio_direction_output(pin, 0);
	else
		ret = gpio_direction_input(pin);

	if (ret) {
		gpio_free(pin);
		printk("faile to set gpio_direction with %d pin\n", pin);
		return -1;
	}
	return 0;
}

static irqreturn_t strobe_isr(int irq, void *dev_id)
{
        struct strobe_device *sdev = dev_id;
        unsigned long irqflags;

        sdev->irq_received++;

        spin_lock_irqsave(&sdev->strobe_lock, irqflags);
        queue_work(sdev->wq, &sdev->work);
        spin_unlock_irqrestore(&sdev->strobe_lock, irqflags);

        return IRQ_HANDLED;
}

static int allocate_irq(unsigned int pin)
{
	int irq, error;

	irq = gpio_to_irq(pin);
	if (irq < 0) {
		printk("Unable to get irq: error %d\n", irq);
		return -1;
	}
	error = request_irq(irq, strobe_isr, IRQF_SHARED | IRQF_TRIGGER_RISING, MODNAME, (void *)&device);
	if (error) {
		printk("request_irq %d failure. error %d\n", irq, error);
		return -1;
	}
	device.irq = irq;
	return 0;
}

static int allocate_pins(void)
{
	int ret;
	ret = allocate_pin(device.strobe_out, 1);
	if (ret)
		return ret;
	ret = allocate_pin(device.strobe_in, 0);
	if (ret)
		return ret;
	ret = allocate_irq(device.strobe_in);
	return ret;
}

static void release_pins(void)
{
	free_irq(device.irq, &device);
	gpio_free(device.strobe_in);
	gpio_free(device.strobe_out);
}

static int strobe_open(struct inode *inode, struct file *file)
{
	struct strobe_device *sdev;
	sdev = container_of(inode->i_cdev, struct strobe_device, cdev);
	file->private_data = sdev;
	return 0;
}

static int strobe_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t strobe_read(struct file *file, char *buffer, size_t count, loff_t *offset)
{
	return 0;
}

static ssize_t strobe_write(struct file *file, const char *buffer, size_t count, loff_t *offset)
{
	return count;
}

struct file_operations strobe_funcs = {
	owner:		THIS_MODULE,
	open:		strobe_open,
	release: 	strobe_release,
	read:		strobe_read,
	write:		strobe_write,
};

static ssize_t duration_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	//struct strobe_device *sdev = container_of(dev, struct strobe_device, dev);
	//return sprintf(buf, "duration %d usec\n", sdev->u_duration);
	return sprintf(buf, "duration %d usec\n", device.u_duration);
}

static ssize_t duration_store(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	int var, err;
	//struct strobe_device *sdev = container_of(dev, struct strobe_device, dev);

	err = kstrtoint(buf, 10, &var);
	if (err < 0)
		return err;
	//sdev->u_duration = var;
	device.u_duration = var;
	return count;
}

static ssize_t offset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	//struct strobe_device *sdev = container_of(dev, struct strobe_device, dev);
	//return sprintf(buf, "offset %d usec\n", sdev->u_offset);
	return sprintf(buf, "offset %d usec\n", device.u_offset);
}

static ssize_t offset_store(struct device *dev,	struct device_attribute *attr,
			const char *buf, size_t count)
{
	int var, err;
	//struct strobe_device *sdev = container_of(dev, struct strobe_device, dev);

	err = kstrtoint(buf, 10, &var);
	if (err < 0)
		return err;
	//sdev->u_offset = var;
	device.u_offset = var;
	return count;
}

static DEVICE_ATTR(duration, 0664, duration_show, duration_store);
static DEVICE_ATTR(offset, 0664, offset_show, offset_store);

static void strobe_function(struct work_struct *work)
{
	struct strobe_device *sdev = container_of(work, struct strobe_device, work);
	printk("stobe interrupt: %lu\n", sdev->irq_received);
	if (sdev->u_offset)
		udelay(sdev->u_offset);

	gpio_set_value(sdev->strobe_out, 1);
	udelay(sdev->u_duration);
	gpio_set_value(sdev->strobe_out, 0);
	sdev->irq_handled++;
}

static int __init strobe_init_module(void)
{
	int err;

	err = alloc_chrdev_region(&device.devno, 1, 1, MODNAME);
	if (err < 0) {
		printk("Can't get major\n");
		return err;
	}
	memset(&device, 0, sizeof(struct strobe_device));

	sema_init(&device.sem, 1);
	cdev_init(&device.cdev, &strobe_funcs);
	device.cdev.owner = THIS_MODULE;
	device.cdev.ops = &strobe_funcs;
	spin_lock_init(&device.strobe_lock);
	INIT_WORK(&device.work, strobe_function);
	device.wq = create_singlethread_workqueue("WQ");

	//The following gpio pins are board specific data
	device.strobe_in = STROBE_INT_IN;
	device.strobe_out = STROBE_OUT;

	if (allocate_pins()) {
		printk("Failed to allocate pins\n");
		err = -ENOSR;
		goto fail_cdev;
	}

	err = cdev_add(&device.cdev, device.devno, 1);
	if (err) {
		printk("Error %d adding device\n", err);
		goto fail_cdev;
	}

	device.cls = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(device.cls)) {
		printk("cant create class %s\n", CLASS_NAME);
		goto fail_class;
	}

	device.dev = device_create(device.cls, NULL, device.devno, NULL, DEVICE_NAME);
	if (IS_ERR(device.dev)) {
		printk("cant create device %s\n", DEVICE_NAME);
		goto fail_device;
	}

	err = device_create_file(device.dev, &dev_attr_duration);
	if (err < 0) {
		printk("cant create device attribute %s %s\n",
		DEVICE_NAME, dev_attr_duration.attr.name);
		goto fail_dev;
	}

	err = device_create_file(device.dev, &dev_attr_offset);
	if (err < 0) {
		printk("cant create device attribute %s %s\n",
		DEVICE_NAME, dev_attr_offset.attr.name);
		goto fail_dev;
	}

	return 0;

fail_dev:
	device_destroy(device.cls, device.devno);
fail_device:
	class_unregister(device.cls);
	class_destroy(device.cls);
fail_class:
	cdev_del(&device.cdev);
fail_cdev:
	release_pins();
	unregister_chrdev_region(device.devno, 1);
	destroy_workqueue(device.wq);
	return err;
}

static void __exit strobe_cleanup_module(void)
{
	device_destroy(device.cls, device.devno);
	class_unregister(device.cls);
	class_destroy(device.cls);
	cdev_del(&device.cdev);
	release_pins();
	unregister_chrdev_region(device.devno, 1);
	destroy_workqueue(device.wq);
}

module_init(strobe_init_module);
module_exit(strobe_cleanup_module);

MODULE_AUTHOR("Hyoung Ham <hham@leapmotion.com>");
MODULE_DESCRIPTION("Strobe Controller Driver");
MODULE_LICENSE("GPL");
