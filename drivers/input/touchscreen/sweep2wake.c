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

#include <linux/input/wake_gestures.h>

#define TAG	"[S2W/S2S]: "

static DEFINE_MUTEX(pwrkeyworklock);

int s2w_switch = S2W_DEFAULT;
int gestures_switch = S2W_DEFAULT;
int vib_strength = VIB_STRENGTH;

static int s2s_switch = S2W_DEFAULT;

static struct work_struct s2w_input_work;
static struct workqueue_struct *s2w_input_wq;
static struct input_dev *gesture_dev;
static struct input_dev *sweep2wake_pwrdev;
static struct notifier_block s2w_lcd_notif;

extern void set_vibrate(int value);

static int touch_x = 0;
static int touch_y = 0;
static int firstx = 0;
static int firsty = 0;
static bool touch_x_called = false;
static bool touch_y_called = false;
static bool scr_suspended = false;
static bool exec_countx = true;
static bool exec_county = true;
static bool barrierx[2] = {false, false};
static bool barriery[2] = {false, false};
static unsigned long firstx_time = 0;
static unsigned long firsty_time = 0;
static unsigned long pwrtrigger_time[2] = {0, 0};

/* I don't know what is this. Ask @flar2, please. */
void gestures_setdev(struct input_dev *input_device)
{
	gesture_dev = input_device;

	return;
}

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
static void sweep2wake_reset(void)
{
	exec_countx = true;

	barrierx[0] = false;
	barrierx[1] = false;

	firstx = 0;
	firstx_time = 0;

	exec_county = true;

	barriery[0] = false;
	barriery[1] = false;

	firsty = 0;
	firsty_time = 0;
}

/* Emulate a press on the power button */
static void sweep2wake_presspwr(
			struct work_struct *sweep2wake_presspwr_work)
{
	if (!mutex_trylock(&pwrkeyworklock))
		return;

	input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);

	msleep(S2W_PWRKEY_DUR);

	input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);

	msleep(S2W_PWRKEY_DUR);

	mutex_unlock(&pwrkeyworklock);

	return;
}
static DECLARE_WORK(sweep2wake_presspwr_work, sweep2wake_presspwr);

/* Power button trigger for S2W/S2S */
static void sweep2wake_pwrtrigger(void)
{
	pwrtrigger_time[1] = pwrtrigger_time[0];
	pwrtrigger_time[0] = jiffies;

	if (pwrtrigger_time[0] - pwrtrigger_time[1] < TRIGGER_TIMEOUT)
		return;

	set_vibrate(vib_strength);

	schedule_work(&sweep2wake_presspwr_work);

	return;
}

/* Main S2W/S2S code */
static void detect_sweep2wake_v(int x, int y, bool st)
{
	int prevy = 0, nexty = 0;
	bool single_touch = st;

	if (firsty == 0) {
		firsty = y;
		firsty_time = jiffies;
	}

	if (x > 100 && x < 980) {
		if (firsty > 960 && single_touch && (s2w_switch & SWEEP_UP)) {
			prevy = firsty;
			nexty = prevy - S2W_Y_NEXT;

			if (barriery[0] == true || (y < prevy && y > nexty)) {
				prevy = nexty;
				nexty -= S2W_Y_NEXT;
				barriery[0] = true;

				if (barriery[1] == true ||
				   (y < prevy && y > nexty)) {
					prevy = nexty;
					barriery[1] = true;

					if (y < prevy && y < (nexty - S2W_Y_NEXT)) {
						if (exec_county &&
						   (jiffies - firsty_time < SWEEP_TIMEOUT)) {
							pr_info(TAG "sweep up\n");

							if (gestures_switch)
								report_gesture(3);
							else
								sweep2wake_pwrtrigger();

							exec_county = false;
						}
					}
				}
			}
		} else if (firsty <= 960 && single_touch &&
			  (s2w_switch & SWEEP_DOWN)) {
			prevy = firsty;
			nexty = prevy + S2W_Y_NEXT;

			if (barriery[0] == true || (y > prevy && y < nexty)) {
				prevy = nexty;
				nexty += S2W_Y_NEXT;
				barriery[0] = true;

				if (barriery[1] == true ||
				   (y > prevy && y < nexty)) {
					prevy = nexty;
					barriery[1] = true;

					if (y > prevy && y > (nexty + S2W_Y_NEXT)) {
						if (exec_county &&
						   (jiffies - firsty_time < SWEEP_TIMEOUT)) {
							pr_info(TAG "sweep down\n");

							if (gestures_switch)
								report_gesture(4);
							else
								sweep2wake_pwrtrigger();

							exec_county = false;
						}
					}
				}
			}
		}
	}
}

static void detect_sweep2wake_h(int x, int y, bool st, bool wake)
{
	int prevx = 0, nextx = 0;
	bool single_touch = st;

	if (firstx == 0) {
		firstx = x;
		firstx_time = jiffies;
	}

	if (!wake && y < S2W_Y_LIMIT) {
		sweep2wake_reset();
		return;
	}

	if (firstx < 510 && single_touch &&
	   ((wake && (s2w_switch & SWEEP_RIGHT)) ||
	   (!wake && (s2s_switch & SWEEP_RIGHT)))) {
		prevx = 0;
		nextx = S2W_X_B1;

		if ((barrierx[0] == true) || ((x > prevx) && (x < nextx))) {
			prevx = nextx;
			nextx = S2W_X_B2;
			barrierx[0] = true;

			if ((barrierx[1] == true) ||
			   ((x > prevx) && (x < nextx))) {
				prevx = nextx;
				barrierx[1] = true;

				if (x > prevx && x > (S2W_X_MAX - S2W_X_FINAL)) {
					if (exec_countx &&
					   (jiffies - firstx_time < SWEEP_TIMEOUT)) {
						pr_info(TAG "sweep right\n");

						if (gestures_switch && wake)
							report_gesture(1);
						else
							sweep2wake_pwrtrigger();

						exec_countx = false;
					}
				}
			}
		}
	} else if (firstx >= 510 && single_touch &&
		  ((wake && (s2w_switch & SWEEP_LEFT)) ||
		  (!wake && (s2s_switch & SWEEP_LEFT)))) {
		prevx = (S2W_X_MAX - S2W_X_FINAL);
		nextx = S2W_X_B2;

		if ((barrierx[0] == true) || ((x < prevx) && (x > nextx))) {
			prevx = nextx;
			nextx = S2W_X_B1;
			barrierx[0] = true;

			if ((barrierx[1] == true) ||
			   ((x < prevx) && (x > nextx))) {
				prevx = nextx;
				barrierx[1] = true;

				if (x < prevx && x < S2W_X_FINAL) {
					if (exec_countx) {
						pr_info(TAG "sweep left\n");

						if (gestures_switch && wake)
							report_gesture(2);
						else
							sweep2wake_pwrtrigger();

						exec_countx = false;
					}
				}
			}
		}
	}
}

/* Initialize input notifier */
static void s2w_input_callback(struct work_struct *unused)
{
	detect_sweep2wake_h(touch_x, touch_y, true, scr_suspended);

	if (scr_suspended)
		detect_sweep2wake_v(touch_x, touch_y, true);

	return;
}

static void s2w_input_event(struct input_handle *handle,
			    unsigned int type, unsigned int code,
			    int value)
{
	if (code == ABS_MT_SLOT) {
		sweep2wake_reset();
		return;
	}

	if (code == ABS_MT_TRACKING_ID && value == -1) {
		sweep2wake_reset();
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

	if (touch_x_called && touch_y_called) {
		touch_x_called = false;
		touch_y_called = false;
		queue_work_on(0, s2w_input_wq, &s2w_input_work);
	} else if (!scr_suspended && touch_x_called && !touch_y_called) {
		touch_x_called = false;
		touch_y_called = false;
		queue_work_on(0, s2w_input_wq, &s2w_input_work);
	}
}

static int input_dev_filter(struct input_dev *dev)
{
	if (strstr(dev->name, "touch"))
		return 0;
	else
		return 1;
}

static int s2w_input_connect(struct input_handler *handler,
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
	handle->name = "s2w";

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

static void s2w_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id s2w_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler s2w_input_handler = {
	.event		= s2w_input_event,
	.connect	= s2w_input_connect,
	.disconnect	= s2w_input_disconnect,
	.name		= "s2w_inputreq",
	.id_table	= s2w_ids,
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
static ssize_t s2w_sweep2wake_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", s2w_switch);

	return count;
}

static ssize_t s2w_sweep2wake_dump(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	sscanf(buf, "%d", &s2w_switch);
	if (s2w_switch < 0 || s2w_switch > 15)
		s2w_switch = 15;

	if (scr_suspended && !dt2w_switch && !s2w_switch)
		sweep2wake_pwrtrigger();

	return count;
}

static ssize_t sweep2sleep_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", s2s_switch);

	return count;
}

static ssize_t sweep2sleep_dump(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	sscanf(buf, "%d", &s2s_switch);
	if (s2s_switch < 0 || s2s_switch > 3)
		s2s_switch = 0;

	return count;
}

static ssize_t wake_gestures_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", gestures_switch);

	return count;
}

static ssize_t wake_gestures_dump(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	sscanf(buf, "%d", &gestures_switch);
	if (gestures_switch < 0 || gestures_switch > 3)
		gestures_switch = 0;

	return count;
}

static ssize_t vib_strength_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", vib_strength);

	return count;
}

static ssize_t vib_strength_dump(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	sscanf(buf, "%d", &vib_strength);
	if (vib_strength < 0 || vib_strength > 90)
		vib_strength = 20;

	return count;
}

static DEVICE_ATTR(sweep2wake, (S_IWUSR|S_IRUGO),
		   s2w_sweep2wake_show, s2w_sweep2wake_dump);

static DEVICE_ATTR(sweep2sleep, (S_IWUSR|S_IRUGO),
		   sweep2sleep_show, sweep2sleep_dump);

static DEVICE_ATTR(wake_gestures, (S_IWUSR|S_IRUGO),
		   wake_gestures_show, wake_gestures_dump);

static DEVICE_ATTR(vib_strength, (S_IWUSR|S_IRUGO),
		   vib_strength_show, vib_strength_dump);

/* Create global kernel object */
struct kobject *android_touch_kobj;
EXPORT_SYMBOL_GPL(android_touch_kobj);

static int __init sweep2wake_init(void)
{
	int rc = 0;

	/* Create input devices */
	sweep2wake_pwrdev = input_allocate_device();
	if (!sweep2wake_pwrdev) {
		pr_err("%s: Can't allocate S2W/S2S input device\n", __func__);
		goto err_alloc_dev;
	}

	input_set_capability(sweep2wake_pwrdev, EV_KEY, KEY_POWER);

	sweep2wake_pwrdev->name = "s2w_pwrkey";
	sweep2wake_pwrdev->phys = "s2w_pwrkey/input0";

	rc = input_register_device(sweep2wake_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err = %d\n", __func__, rc);
		goto err_input_dev;
	}

	gesture_dev = input_allocate_device();
	if (!gesture_dev) {
		pr_err("%s: Can't allocate Wake Gestures input device\n",
								__func__);
		goto err_alloc_dev;
	}

	gesture_dev->name = "wake_gesture";
	gesture_dev->phys = "wake_gesture/input0";

	input_set_capability(gesture_dev, EV_REL, WAKE_GESTURE);

	rc = input_register_device(gesture_dev);
	if (rc) {
		pr_err("%s: input_register_device err = %d\n", __func__, rc);
		goto err_input_dev;
	}

	gestures_setdev(gesture_dev);

	/* Create workqueue */
	s2w_input_wq = alloc_workqueue("s2wiwq", WQ_HIGHPRI | WQ_FREEZABLE, 0);
	if (!s2w_input_wq) {
		pr_err("%s: Failed to alloc s2wiwq workqueue\n", __func__);
		return -EFAULT;
	}

	INIT_WORK(&s2w_input_work, s2w_input_callback);

	/* Register notifiers */
	rc = input_register_handler(&s2w_input_handler);
	if (rc)
		pr_err("%s: Failed to register s2w input handler\n", __func__);

	s2w_lcd_notif.notifier_call = lcd_notifier_callback;
	if (lcd_register_client(&s2w_lcd_notif) != 0)
		pr_err("%s: Failed to register lcd notifier\n", __func__);

	/* Initialize sysfs group */
	android_touch_kobj = kobject_create_and_add("android_touch", NULL);
	if (android_touch_kobj == NULL)
		pr_warn("%s: Failed to create android_touch kobj\n", __func__);

	rc = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2wake.attr);
	if (rc)
		pr_warn("%s: Failed to create sysfs for S2W\n", __func__);

	rc = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2sleep.attr);
	if (rc)
		pr_warn("%s: Failed to create sysfs for S2S\n", __func__);

	rc = sysfs_create_file(android_touch_kobj, &dev_attr_wake_gestures.attr);
	if (rc)
		pr_warn("%s: Failed to create sysfs for WG\n", __func__);

	rc = sysfs_create_file(android_touch_kobj, &dev_attr_vib_strength.attr);
	if (rc)
		pr_warn("%s: Failed to create sysfs for vibration\n", __func__);

err_input_dev:
	input_free_device(sweep2wake_pwrdev);
err_alloc_dev:
	pr_info(TAG "%s done\n", __func__);

	return 0;
}

static void __exit sweep2wake_exit(void)
{
	/* Destroy sysfs group */
	kobject_del(android_touch_kobj);

	/* Unregister notifiers */
	lcd_unregister_client(&s2w_lcd_notif);
	input_unregister_handler(&s2w_input_handler);

	/* Destroy workqueue */
	flush_workqueue(s2w_input_wq);
	cancel_work_sync(&s2w_input_work);
	destroy_workqueue(s2w_input_wq);

	/* Destroy input device */
	input_unregister_device(sweep2wake_pwrdev);
	input_free_device(sweep2wake_pwrdev);

	return;
}

module_init(sweep2wake_init);
module_exit(sweep2wake_exit);

MODULE_AUTHOR("Dennis Rassmann <showp1984@gmail.com>");
MODULE_DESCRIPTION("S2W/S2S for LGE Synaptics");
MODULE_LICENSE("GPLv2");
