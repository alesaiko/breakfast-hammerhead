/*
 * Copyright (C) 2017, Chad Froebel <chadfroebel@gmail.com>
 *		       Jean-Pierre Rasquin <yank555.lu@gmail.com>
 *		       Alex Saiko <solcmdr@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#include <linux/power/fastchg.h>

/*
 * Current charging level:
 * - NOT_FAST_CHARGING if Fast Charge is not used,
 * - Current mA if Fast Charging is used.
 */
unsigned int current_charge_level = NOT_FAST_CHARGING;

/* Fast Charge toggle */
unsigned int force_fast_charge = FAST_CHARGE_DISABLED;

/* AC Charge current limit */
unsigned int ac_charge_level = AC_CHARGE_2000;

/* USB Charge current limit */
unsigned int usb_charge_level = USB_CHARGE_1000;

/* Fall to stable charging level if Fast Charge failed */
unsigned int failsafe = FAIL_SAFE_ENABLED;

/*
 * debug = 1 will print all
 */
static unsigned int debug = 1;
module_param_named(debug_mask, debug, uint, 0644);

#define dprintk(msg...)		\
do {				\
	if (debug)		\
		pr_info(msg);	\
} while (0)

/* Fast Charge sysfs */
static ssize_t force_fast_charge_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", force_fast_charge);
}

static ssize_t force_fast_charge_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned int val;

	sscanf(buf, "%u", &val);

	switch (val) {
		case FAST_CHARGE_DISABLED:
		case FAST_CHARGE_ENABLED:
			force_fast_charge = val;
			dprintk("%s: Current state -> %u\n", TAG,
					force_fast_charge);
			return count;
		default:
			return -EINVAL;
	}
}

static ssize_t ac_charge_level_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", ac_charge_level);
}

static ssize_t ac_charge_level_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned int val;

	sscanf(buf, "%u", &val);

	if (failsafe == FAIL_SAFE_DISABLED &&
	    val <= MAX_CHARGE_LEVEL && val >= MIN_CHARGE_LEVEL) {
		ac_charge_level = val;
		dprintk("%s: Current AC chg level -> %u\n",
				TAG, ac_charge_level);
		return count;
	} else {
		/* Use predefined charging levels with failsafe */
		switch (val) {
			case AC_CHARGE_1000:
			case AC_CHARGE_1100:
			case AC_CHARGE_1200:
			case AC_CHARGE_1300:
			case AC_CHARGE_1400:
			case AC_CHARGE_1500:
			case AC_CHARGE_1600:
			case AC_CHARGE_1700:
			case AC_CHARGE_1800:
			case AC_CHARGE_1900:
			case AC_CHARGE_2000:
				ac_charge_level = val;
				dprintk("%s: Current AC chg level -> %u\n",
					TAG, ac_charge_level);
				return count;
			default:
				return -EINVAL;
		}
	}

	return -EINVAL;
}

static ssize_t usb_charge_level_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", usb_charge_level);
}

static ssize_t usb_charge_level_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	unsigned int val;

	sscanf(buf, "%u", &val);

	if (failsafe == FAIL_SAFE_DISABLED &&
	    val <= MAX_CHARGE_LEVEL && val >= MIN_CHARGE_LEVEL) {
		usb_charge_level = val;
		dprintk("%s: Current USB chg level -> %u\n",
				TAG, usb_charge_level);
		return count;
	} else {
		/* Use predefined charging levels with failsafe */
		switch (val) {
			case USB_CHARGE_500:
			case USB_CHARGE_600:
			case USB_CHARGE_700:
			case USB_CHARGE_800:
			case USB_CHARGE_900:
			case USB_CHARGE_1000:
				usb_charge_level = val;
				dprintk("%s: Current USB chg level -> %u\n",
					TAG, usb_charge_level);
				return count;
			default:
				return -EINVAL;
		}
	}

	return -EINVAL;
}

static ssize_t failsafe_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", failsafe);
}

static ssize_t failsafe_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int val;

	sscanf(buf, "%u", &val);

	switch (val) {
		case FAIL_SAFE_ENABLED:
			failsafe = val;
			/* Restore stock current limits */
			usb_charge_level = USB_CHARGE_500;
			ac_charge_level = AC_CHARGE_1500;
			dprintk("%s: Failsafe enabled!\n", TAG);
			return count;
		case FAIL_SAFE_DISABLED:
			failsafe = val;
			dprintk("%s: Failsafe disabled!\n", TAG);
			return count;
		default:
			return -EINVAL;
	}
}

static ssize_t ac_levels_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", AC_LEVELS);
}

static ssize_t usb_levels_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", USB_LEVELS);
}

static struct kobj_attribute force_fast_charge_attribute =
	__ATTR(force_fast_charge, 0644,
	       force_fast_charge_show, force_fast_charge_store);

static struct kobj_attribute ac_charge_level_attribute =
	__ATTR(ac_charge_level, 0644,
	       ac_charge_level_show, ac_charge_level_store);

static struct kobj_attribute usb_charge_level_attribute =
	__ATTR(usb_charge_level, 0644,
	       usb_charge_level_show, usb_charge_level_store);

static struct kobj_attribute failsafe_attribute =
	__ATTR(failsafe, 0644,
	       failsafe_show, failsafe_store);

static struct kobj_attribute ac_levels_attribute =
	__ATTR(ac_levels, 0444,
	       ac_levels_show, NULL);

static struct kobj_attribute usb_levels_attribute =
	__ATTR(usb_levels, 0444,
	       usb_levels_show, NULL);

static struct attribute *force_fast_charge_attrs[] = {
	&force_fast_charge_attribute.attr,
	&ac_charge_level_attribute.attr,
	&usb_charge_level_attribute.attr,
	&failsafe_attribute.attr,
	&ac_levels_attribute.attr,
	&usb_levels_attribute.attr,
	NULL,
};

static struct attribute_group force_fast_charge_attr_group = {
	.attrs = force_fast_charge_attrs,
};

static struct kobject *force_fast_charge_kobj;

int force_fast_charge_init(void)
{
	int ret;

	/* Create a kernel object and use it for a sysfs group */
	force_fast_charge_kobj = kobject_create_and_add("fast_charge",
							 kernel_kobj);
	if (!force_fast_charge_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(force_fast_charge_kobj,
				&force_fast_charge_attr_group);
	if (ret)
		kobject_put(force_fast_charge_kobj);

	return ret;
}

void force_fast_charge_exit(void)
{
	/* Free sysfs group */
	if (force_fast_charge_kobj != NULL)
		kobject_put(force_fast_charge_kobj);
}

module_init(force_fast_charge_init);
module_exit(force_fast_charge_exit);
