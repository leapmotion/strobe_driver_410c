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

#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>

#include "strobe_driver.h"

static int line_in = LINE_IN;

static struct strobe_device device;
static int major = 0;
static int minor = 0;

static int allocate_pins(void)
{
	int ret;
	if (!gpio_is_valid(line_in)) {
		printk("%d line is not valid.\n", line_in);
		return -1;
	}
	ret = gpio_request(line_in, "v");
	if (ret) {
		printk("%d line gpio_request failure\n", line_in);
		return -1;
	}
	ret = gpio_direction_input(line_in);
	if (ret) {
		gpio_free(line_in);
		printk("%d line gpio_direction_input failure\n", line_in);
		return -1;
	}
	return 0;
}

static void release_pins(void)
{
	gpio_free(line_in);
}

static irqreturn_t strobe_isr(int irq, void *dev_id)
{
        struct strobe_device *dev = dev_id;
        unsigned long irqflags;

        dev->irq_received++;

        spin_lock_irqsave(&dev->strobe_lock, irqflags);
        queue_work(dev->wq, &dev->work);
        spin_unlock_irqrestore(&dev->strobe_lock, irqflags);

        return IRQ_HANDLED;
}

static int allocate_irq(void)
{
	int irq, error;

	irq = gpio_to_irq(line_in);
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

static void release_irq(void)
{
	free_irq(device.irq, &device);
}

static int strobe_open(struct inode *inode, struct file *file)
{
	struct strobe_device *dev;
	dev = container_of(inode->i_cdev, struct strobe_device, cdev);
	file->private_data = dev;
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

static ssize_t duration_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	//struct strobe_device *dev = container_of(kobj, struct strobe_device, kobj);
	//return sprintf(buf, "duration %d usec\n", dev->u_duration);
	return sprintf(buf, "duration %d usec\n", device.u_duration);
}

static ssize_t duration_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	int var, err;
	//struct strobe_device *dev = container_of(kobj, struct strobe_device, kobj);

	err = kstrtoint(buf, 10, &var);
	if (err < 0)
		return err;
	device.u_duration = var;
	//dev->u_duration = var;
	return count;
}

static ssize_t offset_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	//struct strobe_device *dev = container_of(kobj, struct strobe_device, kobj);
	//return sprintf(buf, "offset %d usec\n", dev->u_offset);
	return sprintf(buf, "offset %d usec\n", device.u_offset);
}

static ssize_t offset_store(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	int var, err;
	//struct strobe_device *dev = container_of(kobj, struct strobe_device, kobj);

	err = kstrtoint(buf, 10, &var);
	if (err < 0)
		return err;
	//dev->u_offset = var;
	device.u_offset = var;
	return count;
}

static struct kobj_attribute dev_attr_duration =
	__ATTR(duration, 0664, duration_show, duration_store);
static struct kobj_attribute dev_attr_offset =
	__ATTR(offset, 0664, offset_show, offset_store);

static struct attribute *strobe_attrs[] = {
	&dev_attr_duration.attr,
	&dev_attr_offset.attr,
	NULL
};

static struct attribute_group strobe_attr_group = {
	.attrs = strobe_attrs,
};

static void strobe_function(struct work_struct *work)
{
	struct strobe_device *dev = container_of(work, struct strobe_device, work);
	printk("stobe interrupt: %lu\n", dev->irq_received);
	dev->irq_handled++;
}

static int __init strobe_init_module(void)
{
	dev_t dev = 0;
	int err, devno;

	err = alloc_chrdev_region(&dev, minor, 1, MODNAME);
	major = MAJOR(dev);
	if (err < 0) {
		printk("Can't get major %d\n", major);
		return err;
	}
	devno = MKDEV(major, 0);
	memset(&device, 0, sizeof(struct strobe_device));

	if (allocate_pins()) {
		printk("Failed to allocate pins\n");
		err = -ENOSR;
		goto fail_pins;
	}

	if (allocate_irq()) {
		printk("Faile to allocate irqs\n");
		err = -ENOSR;
		goto fail_irq;

	}

	sema_init(&device.sem, 1);
	cdev_init(&device.cdev, &strobe_funcs);
	device.cdev.owner = THIS_MODULE;
	device.cdev.ops = &strobe_funcs;
	spin_lock_init(&device.strobe_lock);
	INIT_WORK(&device.work, strobe_function);
	device.wq = create_singlethread_workqueue("WQ");

	err = cdev_add(&device.cdev, devno, 1);
	if (err) {
		printk("Error %d adding device\n", err);
		goto fail_cdev;
	}

	device.kobj = kobject_create_and_add("strobe", kernel_kobj);
	if (!device.kobj) {
		printk("create kobject failure\n");
		goto fail_cdev;
	}
	err = sysfs_create_group(device.kobj, &strobe_attr_group);
	if (err) {
		printk("create sysfs group failure\n");
		goto fail_sysfs;
	}
	return 0;

fail_sysfs:
	kobject_put(device.kobj);
fail_cdev:
	release_irq();
fail_irq:
	release_pins();
fail_pins:
	unregister_chrdev_region(devno, 1);
	return err;
}

static void __exit strobe_cleanup_module(void)
{
	int devno;
	sysfs_remove_group(device.kobj, &strobe_attr_group);
	kobject_put(device.kobj);
	release_irq();
	release_pins();
	cdev_del(&device.cdev);
	devno = MKDEV(major, minor);
	unregister_chrdev_region(devno, 1);
	destroy_workqueue(device.wq);
}

module_init(strobe_init_module);
module_exit(strobe_cleanup_module);

MODULE_AUTHOR("Hyoung Ham <hham@leapmotion.com>");
MODULE_DESCRIPTION("Strobe Controller Driver");
MODULE_LICENSE("GPL");
