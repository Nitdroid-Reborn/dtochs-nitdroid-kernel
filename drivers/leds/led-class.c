/*
 * LED Class Core
 *
 * Copyright (C) 2005 John Lenz <lenz@cs.wisc.edu>
 * Copyright (C) 2005-2007 Richard Purdie <rpurdie@openedhand.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/sysdev.h>
#include <linux/timer.h>
#include <linux/err.h>
#include <linux/ctype.h>
#include <linux/leds.h>
#include "leds.h"

#define LED_FLAG_DBRIGHTNESS 0x100

static struct class *leds_class;

static void led_update_brightness(struct led_classdev *led_cdev)
{
	if (led_cdev->brightness_get)
		led_cdev->brightness = led_cdev->brightness_get(led_cdev);
}

static ssize_t led_brightness_show(struct device *dev, 
		struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);

	/* no lock needed for this */
	led_update_brightness(led_cdev);

	return sprintf(buf, "%u\n", led_cdev->brightness);
}

static ssize_t led_brightness_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	ssize_t ret = -EINVAL;
	char *after;
	unsigned long state = simple_strtoul(buf, &after, 10);
	size_t count = after - buf;

	if (led_cdev->flags & LED_FLAG_DBRIGHTNESS) 
		return size;

	if (*after && isspace(*after))
		count++;

	if (count == size) {
		ret = count;

		if (state == LED_OFF)
			led_trigger_remove(led_cdev);
		led_set_brightness(led_cdev, state);
	}

	return ret;
}

static DEVICE_ATTR(brightness, 0644, led_brightness_show, led_brightness_store);
#ifdef CONFIG_LEDS_TRIGGERS
static DEVICE_ATTR(trigger, 0644, led_trigger_show, led_trigger_store);
#endif

static ssize_t led_dbrightness_show(struct device *dev, 
		struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);

	if (!(led_cdev->flags & LED_FLAG_DBRIGHTNESS)) 
		return sprintf(buf, "-1\n");
		
	/* no lock needed for this */
	led_update_brightness(led_cdev);

	return sprintf(buf, "%u\n", led_cdev->brightness);
}

static ssize_t led_dbrightness_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	ssize_t ret = -EINVAL;
	char *after;
	unsigned long state = simple_strtol(buf, &after, 10);
	size_t count = after - buf;

	if (state == -1) {
		led_cdev->flags &= ~LED_FLAG_DBRIGHTNESS;
		return size;
	}

	if (*after && isspace(*after))
		count++;

	if (count == size) {
		ret = count;

		led_cdev->flags |= LED_FLAG_DBRIGHTNESS;
		if (state == LED_OFF)
			led_trigger_remove(led_cdev);
		led_set_brightness(led_cdev, state);
	}

	return ret;
}

static DEVICE_ATTR(dbrightness, 0644, led_dbrightness_show, led_dbrightness_store);

/**
 * led_classdev_suspend - suspend an led_classdev.
 * @led_cdev: the led_classdev to suspend.
 */
void led_classdev_suspend(struct led_classdev *led_cdev)
{
	led_cdev->flags |= LED_SUSPENDED;
	led_cdev->brightness_set(led_cdev, 0);
}
EXPORT_SYMBOL_GPL(led_classdev_suspend);

/**
 * led_classdev_resume - resume an led_classdev.
 * @led_cdev: the led_classdev to resume.
 */
void led_classdev_resume(struct led_classdev *led_cdev)
{
	led_cdev->brightness_set(led_cdev, led_cdev->brightness);
	led_cdev->flags &= ~LED_SUSPENDED;
}
EXPORT_SYMBOL_GPL(led_classdev_resume);

/**
 * led_classdev_register - register a new object of led_classdev class.
 * @dev: The device to register.
 * @led_cdev: the led_classdev structure for this device.
 */
int led_classdev_register(struct device *parent, struct led_classdev *led_cdev)
{
	int rc;

	led_cdev->dev = device_create(leds_class, parent, 0, led_cdev,
				      "%s", led_cdev->name);
	if (IS_ERR(led_cdev->dev))
		return PTR_ERR(led_cdev->dev);

	/* register the attributes */
	rc = device_create_file(led_cdev->dev, &dev_attr_brightness);
	if (rc)
		goto err_out;

	rc = device_create_file(led_cdev->dev, &dev_attr_dbrightness);
	if (rc)
		goto err_out_dbrightness;

#ifdef CONFIG_LEDS_TRIGGERS
	init_rwsem(&led_cdev->trigger_lock);
#endif
	/* add to the list of leds */
	down_write(&leds_list_lock);
	list_add_tail(&led_cdev->node, &leds_list);
	up_write(&leds_list_lock);

	led_update_brightness(led_cdev);

#ifdef CONFIG_LEDS_TRIGGERS
	rc = device_create_file(led_cdev->dev, &dev_attr_trigger);
	if (rc)
		goto err_out_led_list;

	led_trigger_set_default(led_cdev);
#endif

	printk(KERN_INFO "Registered led device: %s\n",
			led_cdev->name);

	return 0;

#ifdef CONFIG_LEDS_TRIGGERS
err_out_led_list:
	device_remove_file(led_cdev->dev, &dev_attr_dbrightness);
	list_del(&led_cdev->node);
#endif
err_out_dbrightness:
	device_remove_file(led_cdev->dev, &dev_attr_brightness);
err_out:
	device_unregister(led_cdev->dev);
	return rc;
}
EXPORT_SYMBOL_GPL(led_classdev_register);

/**
 * led_classdev_unregister - unregisters a object of led_properties class.
 * @led_cdev: the led device to unregister
 *
 * Unregisters a previously registered via led_classdev_register object.
 */
void led_classdev_unregister(struct led_classdev *led_cdev)
{
	device_remove_file(led_cdev->dev, &dev_attr_brightness);
#ifdef CONFIG_LEDS_TRIGGERS
	device_remove_file(led_cdev->dev, &dev_attr_trigger);
	down_write(&led_cdev->trigger_lock);
	if (led_cdev->trigger)
		led_trigger_set(led_cdev, NULL);
	up_write(&led_cdev->trigger_lock);
#endif

	device_unregister(led_cdev->dev);

	down_write(&leds_list_lock);
	list_del(&led_cdev->node);
	up_write(&leds_list_lock);
}
EXPORT_SYMBOL_GPL(led_classdev_unregister);

static int __init leds_init(void)
{
	leds_class = class_create(THIS_MODULE, "leds");
	if (IS_ERR(leds_class))
		return PTR_ERR(leds_class);
	return 0;
}

static void __exit leds_exit(void)
{
	class_destroy(leds_class);
}

subsys_initcall(leds_init);
module_exit(leds_exit);

MODULE_AUTHOR("John Lenz, Richard Purdie");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LED Class Interface");
