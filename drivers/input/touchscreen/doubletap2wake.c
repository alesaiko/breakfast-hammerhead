/*
 * Copyright (c) 2013, Dennis Rassmann <showp1984@gmail.com>
 *
 * Wake Gestures
 * Copyright (c) 2014, Aaron Segaert <asegaert@gmail.com>
 * Copyright (c) 2017, Alex Saiko <solcmdr@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/lcd_notify.h>
#include <linux/hrtimer.h>
#include <linux/wakelock.h>

#include <asm-generic/cputime.h>

#include <linux/input/wake_gestures.h>

#define TAG	"[DT2W]: "

static DEFINE_MUTEX(pwrkeyworklock);

int dt2w_switch = DT2W_DEFAULT;
extern int gestures_switch;
extern int vib_strength;

static struct wake_lock dt2w_wakelock;
static struct work_struct dt2w_input_work;
static struct workqueue_struct *dt2w_input_wq;
static struct input_dev *gesture_dev;
static struct input_dev *doubletap2wake_pwrdev;
static struct notifier_block dt2w_lcd_notif;

extern void set_vibrate(int value);

static int touch_x = 0;
static int touch_y = 0;
static int touch_nr = 0;
static int x_pre = 0;
static int y_pre = 0;
static bool touch_x_called = false;
static bool touch_y_called = false;
static bool touch_cnt = true;
static bool scr_suspended = false;
static bool exec_count = true;
static unsigned long pwrtrigger_time[2] = {0, 0};
static unsigned long long tap_time_pre = 0;

/* Report gesture data to input device */
static void report_gesture(int gest)
{
	pwrtrigger_time[1] = pwrtrigger_time[0];
	pwrtrigger_time[0] = jiffies;

	if (pwrtrigger_time[0] - pwrtrigger_time[1] < TRIGGER_TIMEOUT)
		return;

	pr_info(TAG "gesture = %d\n", gest);

	input_report_rel(gesture_dev, WAKE_GESTURE, gest);
	input_sync(gesture_dev);
}

/* Reset gesture data */
static void doubletap2wake_reset(void)
{
	if (wake_lock_active(&dt2w_wakelock))
		wake_unlock(&dt2w_wakelock);

	exec_count = true;
	touch_nr = 0;
	tap_time_pre = 0;

	x_pre = 0;
	y_pre = 0;
}

/* Emulate a press on the power button */
static void doubletap2wake_presspwr(
			struct work_struct *doubletap2wake_presspwr_work)
{
	if (!mutex_trylock(&pwrkeyworklock))
		return;

	input_event(doubletap2wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(doubletap2wake_pwrdev, EV_SYN, 0, 0);

	msleep(DT2W_PWRKEY_DUR);

	input_event(doubletap2wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(doubletap2wake_pwrdev, EV_SYN, 0, 0);

	msleep(DT2W_PWRKEY_DUR);

	mutex_unlock(&pwrkeyworklock);

	return;
}
static DECLARE_WORK(doubletap2wake_presspwr_work, doubletap2wake_presspwr);

/* Power button trigger for DT2W */
static void doubletap2wake_pwrtrigger(void)
{
	pwrtrigger_time[1] = pwrtrigger_time[0];
	pwrtrigger_time[0] = jiffies;

	if (pwrtrigger_time[0] - pwrtrigger_time[1] < TRIGGER_TIMEOUT)
		return;

	set_vibrate(vib_strength);

	schedule_work(&doubletap2wake_presspwr_work);

	return;
}

/* Calculate the scatter between touches */
static unsigned int calc_feather(int coord, int prev_coord)
{
	int calc_coord = 0;

	calc_coord = coord - prev_coord;
	if (calc_coord < 0)
		return -calc_coord;

	return calc_coord;
}

/* Record a new touch */
static void new_touch(int x, int y)
{
	tap_time_pre = jiffies;

	x_pre = x;
	y_pre = y;

	touch_nr++;

	wake_lock_timeout(&dt2w_wakelock, 150);
}

/* Main DT2W code */
static void detect_doubletap2wake(int x, int y, bool st)
{
	bool single_touch = st;

	if (x < 100 || x > 980)
		return;

	if (dt2w_switch < 2 && y < 1000)
		return;

	if ((single_touch) && (dt2w_switch > 0) &&
	    (exec_count) && (touch_cnt)) {
		touch_cnt = false;

		if (touch_nr == 0) {
			new_touch(x, y);
		} else if (touch_nr == 1) {
			if ((calc_feather(x, x_pre) < DT2W_FEATHER) &&
			    (calc_feather(y, y_pre) < DT2W_FEATHER) &&
			   ((jiffies - tap_time_pre) < DT2W_TIME)) {
				touch_nr++;
			} else {
				doubletap2wake_reset();
				new_touch(x, y);
			}
		} else {
			doubletap2wake_reset();
			new_touch(x, y);
		}

		if (touch_nr > 1) {
			pr_info(TAG "double tap\n");

			exec_count = false;

			if (gestures_switch)
				report_gesture(5);
			else
				doubletap2wake_pwrtrigger();

			doubletap2wake_reset();
		}
	}
}

/* Initialize input notifier */
static void dt2w_input_callback(struct work_struct *unused)
{
	detect_doubletap2wake(touch_x, touch_y, true);

	return;
}

static void dt2w_input_event(struct input_handle *handle,
			     unsigned int type, unsigned int code,
			     int value)
{
	if (!scr_suspended)
		return;

	if (code == ABS_MT_SLOT) {
		doubletap2wake_reset();
		return;
	}

	if (code == ABS_MT_TRACKING_ID && value == -1) {
		touch_cnt = true;
		queue_work_on(0, dt2w_input_wq, &dt2w_input_work);
		return;
	}

	if (code == ABS_MT_POSITION_X) {
		touch_x = value;
		touch_x_called = true;
	}

	if (code == ABS_MT_POSITION_Y) {
		touch_y = value;
		touch_y_called = true;
	}
}

static int input_dev_filter(struct input_dev *dev)
{
	if (strstr(dev->name, "touch"))
		return 0;
	else
		return 1;
}

static int dt2w_input_connect(struct input_handler *handler,
			      struct input_dev *dev,
			      const struct input_device_id *id)
{
	struct input_handle *handle;
	int error;

	if (input_dev_filter(dev))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "dt2w";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;

err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);

	return error;
}

static void dt2w_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id dt2w_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler dt2w_input_handler = {
	.event		= dt2w_input_event,
	.connect	= dt2w_input_connect,
	.disconnect	= dt2w_input_disconnect,
	.name		= "dt2w_inputreq",
	.id_table	= dt2w_ids,
};

/* Initialize LCD notifier callback */
static int lcd_notifier_callback(struct notifier_block *this,
				 unsigned long event, void *data)
{
	switch (event) {
		case LCD_EVENT_ON_END:
			scr_suspended = false;
			break;
		case LCD_EVENT_OFF_END:
			scr_suspended = true;
			break;
		default:
			break;
	}

	return 0;
}

/* Sysfs nodes */
static ssize_t dt2w_doubletap2wake_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", dt2w_switch);

	return count;
}

static ssize_t dt2w_doubletap2wake_dump(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	sscanf(buf, "%d", &dt2w_switch);
	if (dt2w_switch < 0 || dt2w_switch > 2)
		dt2w_switch = 0;

	if (scr_suspended && !dt2w_switch && !s2w_switch)
		doubletap2wake_pwrtrigger();

	return count;
}

static DEVICE_ATTR(doubletap2wake, (S_IWUSR|S_IRUGO),
		   dt2w_doubletap2wake_show, dt2w_doubletap2wake_dump);

/* Extern kernel object from sweep2wake.c */
extern struct kobject *android_touch_kobj;

static int __init doubletap2wake_init(void)
{
	int rc = 0;

	/* Create input device */
	doubletap2wake_pwrdev = input_allocate_device();
	if (!doubletap2wake_pwrdev) {
		pr_err("%s: Can't allocate DT2W input device\n", __func__);
		goto err_alloc_dev;
	}

	input_set_capability(doubletap2wake_pwrdev, EV_KEY, KEY_POWER);

	doubletap2wake_pwrdev->name = "dt2w_pwrkey";
	doubletap2wake_pwrdev->phys = "dt2w_pwrkey/input0";

	rc = input_register_device(doubletap2wake_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err = %d\n", __func__, rc);
		goto err_input_dev;
	}

	/* Create workqueue */
	dt2w_input_wq = alloc_workqueue("dt2wiwq", WQ_HIGHPRI | WQ_FREEZABLE, 0);
	if (!dt2w_input_wq) {
		pr_err("%s: Failed to alloc dt2wiwq workqueue\n", __func__);
		return -EFAULT;
	}

	INIT_WORK(&dt2w_input_work, dt2w_input_callback);

	/* Register notifiers */
	rc = input_register_handler(&dt2w_input_handler);
	if (rc)
		pr_err("%s: Failed to register dt2w input handler\n", __func__);

	dt2w_lcd_notif.notifier_call = lcd_notifier_callback;
	if (lcd_register_client(&dt2w_lcd_notif) != 0)
		pr_err("%s: Failed to register lcd notifier\n", __func__);

	/* Initialize wakelock */
	wake_lock_init(&dt2w_wakelock, WAKE_LOCK_SUSPEND, "dt2w_wakelock");

	/* Push DT2W attribute to previously created sysfs group */
	rc = sysfs_create_file(android_touch_kobj,
			      &dev_attr_doubletap2wake.attr);
	if (rc)
		pr_warn("%s: Failed to create sysfs for DT2W\n", __func__);

err_input_dev:
	input_free_device(doubletap2wake_pwrdev);
err_alloc_dev:
	pr_info(TAG "%s done\n", __func__);

	return 0;
}

static void __exit doubletap2wake_exit(void)
{
	/* Destroy wakelock if it is still active */
	if (wake_lock_active(&dt2w_wakelock))
		wake_lock_destroy(&dt2w_wakelock);

	/* Unregister notifiers */
	lcd_unregister_client(&dt2w_lcd_notif);
	input_unregister_handler(&dt2w_input_handler);

	/* Destroy workqueue */
	flush_workqueue(dt2w_input_wq);
	cancel_work_sync(&dt2w_input_work);
	destroy_workqueue(dt2w_input_wq);

	/* Destroy input device */
	input_unregister_device(doubletap2wake_pwrdev);
	input_free_device(doubletap2wake_pwrdev);

	return;
}

module_init(doubletap2wake_init);
module_exit(doubletap2wake_exit);

MODULE_AUTHOR("Dennis Rassmann <showp1984@gmail.com>");
MODULE_DESCRIPTION("DT2W for LGE Synaptics");
MODULE_LICENSE("GPLv2");
