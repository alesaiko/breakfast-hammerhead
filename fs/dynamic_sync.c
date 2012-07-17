/*
 * Copyright (C) 2018, Alex Saiko <solcmdr@gmail.com>
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

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/syscalls.h>
#include <linux/workqueue.h>
#include <linux/lcd_notify.h>
#include "internal.h"

/*
 * Enable the whole routine.  Think twice before doing this, as disabling
 * file synchronization is a crazy idea. You can hit data loss even if DFS
 * will try to orderly flush the data to the storage before the crash.
 */
#define DEF_DYNAMIC_FSYNC_ENABLED	(0)

/*
 * Use LCD notifier to automatically enable file synchronization after LCD
 * turns off and flush all the data to the storage. This way we can minimize
 * the risk of data loss on portable devices, whose screens change their
 * states quite often. If LCD notifier is unused in the kernel, this will
 * be dropped.
 */
#define DEF_LCD_NOTIFIER_IS_USED	(1)

/*
 * A time slice in milliseconds to wait after LCD was turned off before the
 * start of a graceful overall file synchronization. This delay is used only
 * if LCD notifier is enabled.
 */
#define DEF_SYNCHRONIZATION_DELAY	(3000)

static struct dynamic_sync {
	u32 enabled:1;
	u32 lcd_notify_used:1;
	u32 delay;
} dyn_sync = {
	.enabled = DEF_DYNAMIC_FSYNC_ENABLED,
	.lcd_notify_used = DEF_LCD_NOTIFIER_IS_USED,
	.delay = DEF_SYNCHRONIZATION_DELAY,
};

static struct workqueue_struct *dfs_wq;
static struct delayed_work force_sync_work;

/**
 * do_critical_sync() - enable file synchronization and call emergency
 * data synchronization as soon as possible.
 *
 * This function is used in critical context only, where we need to rapidly
 * flush all the data to the storage.  It is called in both power-off and
 * crash scenarios, but does not appear in watchdog bites, therefore there
 * is still a risk of a data loss.
 */
static inline void do_critical_sync(void)
{
	/*
	 * Force enable synchronization and try to flush data to
	 * the storage as soon as possible to avoid data loss.
	 */
	fsync_enabled = true;
	emergency_sync();
}

/**
 * do_force_sync() - synchronize file systems.
 *
 * This is a main work of Dynamic File Synchronization module. It is called
 * each time an appropriate work is queued, so the two dependencies are the
 * usage of LCD notifier and a timer fire after dyn_sync.delay milliseconds.
 * In other cases, it won't be used, hence the risk of a data loss will be
 * much higher.
 */
static void do_force_sync(struct work_struct *work)
{
	/* Synchronize dirty data gracefully via "sync" syscall */
	pr_debug("Syncing superblock data...\n");
	sys_sync();
}

static inline void dynamic_sync_suspend(void)
{
	/*
	 * Enable file synchronization instantly as userspace processes can
	 * use "sync" syscall to push the data after the panel turns off.
	 */
	fsync_enabled = true;

	/*
	 * Return early if synchronization is already in progress.
	 * This cannot happen in traditional ways.
	 */
	if (unlikely(delayed_work_pending(&force_sync_work)))
		return;

	/*
	 * Do not start an ordinary synchronization immediately as the user
	 * can turn on the display within some seconds.  Use a tunable value
	 * instead to delay the synchronization, so there will not be a huge
	 * latency after the display is shut down.
	 */
	queue_delayed_work(dfs_wq, &force_sync_work,
			   msecs_to_jiffies(dyn_sync.delay));
}

static inline void dynamic_sync_resume(void)
{
	/* Stop the work immediately to avoid significant jitter */
	if (delayed_work_pending(&force_sync_work))
		cancel_delayed_work(&force_sync_work);

	/* Disable file synchronization to get an I/O performance boost */
	fsync_enabled = false;
}

static int lcd_notifier_callback(struct notifier_block *nb,
				 unsigned long event, void *data)
{
	if (likely(!dyn_sync.enabled || !dyn_sync.lcd_notify_used))
		return NOTIFY_OK;

	switch (event) {
	case LCD_EVENT_ON_START:
		dynamic_sync_resume();
		break;
	case LCD_EVENT_OFF_END:
		dynamic_sync_suspend();
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block lcd_notifier_block = {
	.notifier_call = lcd_notifier_callback,
};

static int panic_notifier_callback(struct notifier_block *nb,
				   unsigned long event, void *data)
{
	if (likely(!dyn_sync.enabled))
		return NOTIFY_OK;

	do_critical_sync();

	return NOTIFY_DONE;
}

static struct notifier_block panic_notifier_block = {
	.notifier_call = panic_notifier_callback,
	.priority = INT_MAX,
};

static int reboot_notifier_callback(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	if (likely(!dyn_sync.enabled))
		return NOTIFY_OK;

	switch (event) {
	case SYS_HALT:
	case SYS_RESTART:
	case SYS_POWER_OFF:
		do_critical_sync();
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block reboot_notifier_block = {
	.notifier_call = reboot_notifier_callback,
	.priority = SHRT_MAX,
};

static ssize_t show_dynamic_sync_enabled(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 char *buf)
{
	return scnprintf(buf, 3, "%u\n", dyn_sync.enabled);
}

static ssize_t store_dynamic_sync_enabled(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtouint(buf, 2, &val);
	if (ret || val == dyn_sync.enabled)
		return -EINVAL;

	dyn_sync.enabled = val;

	/*
	 * Synchronize file systems data right before the switch to ensure that
	 * all the data before the enablement was dropped on the storage. This
	 * way all the responsibility of data loss due to a sudden power loss
	 * or a hardware failure lays on user's shoulders.
	 */
	sys_sync();

	/*
	 * Enable file synchronization if display is off right now.
	 * If LCD notifier is not used, just change the synchronization
	 * state according to an input value.
	 */
	if (dyn_sync.enabled && dyn_sync.lcd_notify_used)
		fsync_enabled = lcd_panel_suspended() > 0;
	else
		fsync_enabled = !val;

	return count;
}

static struct kobj_attribute dynamic_sync_enabled =
	__ATTR(Dyn_fsync_active, 0644,
		show_dynamic_sync_enabled, store_dynamic_sync_enabled);

static ssize_t show_lcd_notify_used(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    char *buf)
{
	if (IS_ERR_VALUE(lcd_panel_suspended()))
		return scnprintf(buf, 15, "<unsupported>\n");

	return scnprintf(buf, 3, "%u\n", dyn_sync.lcd_notify_used);
}

static ssize_t store_lcd_notify_used(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	if (IS_ERR_VALUE(lcd_panel_suspended()))
		return -EINVAL;

	ret = kstrtouint(buf, 2, &val);
	if (ret || val == dyn_sync.lcd_notify_used)
		return -EINVAL;

	dyn_sync.lcd_notify_used = val;

	/*
	 * Update file synchronization state according to an input value if
	 * the screen is off right now.  This way synchronization will be
	 * available instantly after LCD notify enablement without the
	 * requirement to turn the screen on/off. In case user wants to throw
	 * LCD notify support away, just disable fsync as he might expect this
	 * to happen.
	 */
	if (dyn_sync.enabled && lcd_panel_suspended() > 0)
		fsync_enabled = val;

	/*
	 * Stop the synchronization work gracefully
	 * if LCD notify is going to be dropped.
	 */
	if (!val && delayed_work_pending(&force_sync_work))
		cancel_delayed_work_sync(&force_sync_work);

	return count;
}

static struct kobj_attribute dynamic_sync_lcd_notify =
	__ATTR(Dyn_fsync_lcd_notify, 0644,
		show_lcd_notify_used, store_lcd_notify_used);

static ssize_t show_dynamic_sync_delay(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       char *buf)
{
	if (!dyn_sync.lcd_notify_used)
		return scnprintf(buf, 15, "<unsupported>\n");

	return scnprintf(buf, 12, "%u\n", dyn_sync.delay);
}

static ssize_t store_dynamic_sync_delay(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	if (!dyn_sync.lcd_notify_used)
		return -EINVAL;

	ret = kstrtouint(buf, 10, &val);
	if (ret || val == dyn_sync.delay)
		return -EINVAL;

	dyn_sync.delay = val;

	/* Restart the work with a new delay if it was in progress */
	if (delayed_work_pending(&force_sync_work))
		mod_delayed_work(dfs_wq, &force_sync_work,
				 msecs_to_jiffies(dyn_sync.delay));

	return count;
}

static struct kobj_attribute dynamic_sync_delay =
	__ATTR(Dyn_fsync_delay, 0644,
		show_dynamic_sync_delay, store_dynamic_sync_delay);

static struct attribute *dynamic_sync_attrs[] = {
	&dynamic_sync_enabled.attr,
	&dynamic_sync_lcd_notify.attr,
	&dynamic_sync_delay.attr,
	NULL
};

static struct attribute_group dynamic_sync_attr_group = {
	.name = "dyn_fsync",
	.attrs = dynamic_sync_attrs,
};

static int __init dynamic_sync_init(void)
{
	int ret;

	dfs_wq = alloc_workqueue("dynamic_sync_wq", WQ_UNBOUND | WQ_HIGHPRI, 0);
	if (IS_ERR_OR_NULL(dfs_wq)) {
		pr_err("Unable to allocate high-priority workqueue\n");
		return -EFAULT;
	}

	INIT_DELAYED_WORK(&force_sync_work, do_force_sync);

	/*
	 * Do not fail if LCD notifier registration failed as it is
	 * expected to fail in case it is not used in kernel tree.
	 */
	ret = lcd_register_client(&lcd_notifier_block);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to register LCD notifier\n");
		dyn_sync.lcd_notify_used = dyn_sync.delay = 0;
	}

	ret = atomic_notifier_chain_register(&panic_notifier_list,
					     &panic_notifier_block);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to register panic notifier\n");
		goto fail_panic;
	}

	ret = register_reboot_notifier(&reboot_notifier_block);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to register reboot notifier\n");
		goto fail_reboot;
	}

	ret = sysfs_create_group(kernel_kobj, &dynamic_sync_attr_group);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to create sysfs group\n");
		goto fail_sysfs;
	}

	return 0;

fail_sysfs:
	unregister_reboot_notifier(&reboot_notifier_block);
fail_reboot:
	atomic_notifier_chain_unregister(&panic_notifier_list,
					 &panic_notifier_block);
fail_panic:
	lcd_unregister_client(&lcd_notifier_block);
	destroy_workqueue(dfs_wq);

	return ret;
}
late_initcall(dynamic_sync_init);
