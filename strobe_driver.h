#ifndef _STROBE_DRIVER_H_
#define _STROBE_DRIVER_H_

#define LINE_IN 13

#define MODNAME "strobe_driver"
#define CLASS_NAME "strobe"
#define DEVICE_NAME "strobe_device"

struct strobe_device {
	struct cdev cdev;
	dev_t devno;
	struct semaphore sem;

	int irq;
	unsigned long irq_received;
	unsigned long irq_handled;
	spinlock_t strobe_lock;

	struct work_struct work;
	struct workqueue_struct *wq;

	struct device *dev;
	struct class *cls;

	unsigned int u_duration;
	unsigned int u_offset;
};
#endif
