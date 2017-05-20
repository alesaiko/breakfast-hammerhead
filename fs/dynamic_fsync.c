/*
 * Copyright (C) 2012-2017, Paul Reioux <reioux@gmail.com>
 * Copyright (C) 2017, Alex Saiko <solcmdr@gmail.com>
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
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/writeback.h>

#include <linux/input/state_notifier.h>

#define TAG "[DFS]"

#define DFS_ENABLE 1
#define DFS_DISABLE 0

#define DFS_VERSION_MAJOR 3
#define DFS_VERSION_MINOR 1

/*
 * DFS boolean triggers. They control all the routine.
 */
bool suspended __read_mostly = false;
bool dfs_active __read_mostly = DFS_DISABLE;

/*
 * fsync_mutex protects dfs_active during suspend / late resume
 * transitions.
 */
static DEFINE_MUTEX(fsync_mutex);

/*
 * File synchronization API.
 */
extern void sync_filesystems(int wait);

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

/* Forcibly synchronize data */
static void dfs_force_flush(void)
{
	sync_filesystems(0);
	sync_filesystems(1);
}

/* DFS resume function */
static void dfs_resume(void)
{
	mutex_lock(&fsync_mutex);
	suspended = false;
	mutex_unlock(&fsync_mutex);

	dprintk("%s: Resumed!\n", TAG);
}

/* DFS suspend function */
static void dfs_suspend(void)
{
	mutex_lock(&fsync_mutex);
	if (dfs_active) {
		suspended = true;
		/* Push all data to storage after suspend */
		dfs_force_flush();
	}
	mutex_unlock(&fsync_mutex);

	dprintk("%s: Suspended!\n", TAG);
}

/* State Notifier is used to handle suspend/resume functions above */
static int state_notifier_callback(struct notifier_block *this,
				   unsigned long event, void *data)
{
	if (!dfs_active)
		return NOTIFY_OK;

	switch (event) {
		case STATE_NOTIFIER_ACTIVE:
			dfs_resume();
			break;
		case STATE_NOTIFIER_SUSPEND:
			dfs_suspend();
			break;
		default:
			break;
	}

	return NOTIFY_OK;
}

static struct notifier_block dfs_state_notif = {
	.notifier_call = state_notifier_callback,
};

/* DFS on panic notifiers */
static int dfs_panic_event(struct notifier_block *this,
			   unsigned long event, void *ptr)
{
	suspended = true;
	/*
	 * Urgently push all data to storage after system crash to avoid
	 * data loss.
	 */
	dfs_force_flush();

	return NOTIFY_DONE;
}

static struct notifier_block dfs_panic_block = {
	.notifier_call	= dfs_panic_event,
	.priority	= INT_MAX,
};

static int dfs_notify_sys(struct notifier_block *this, unsigned long code,
			  void *unused)
{
	if (code == SYS_DOWN || code == SYS_HALT) {
		suspended = true;
		/*
		 * Urgently push all data to storage after system crash to
		 * avoid data loss.
		 */
		dfs_force_flush();
	}

	return NOTIFY_DONE;
}

static struct notifier_block dfs_notifier = {
	.notifier_call = dfs_notify_sys,
};

/* DFS sysfs functions */
static ssize_t dfs_active_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", (dfs_active ? 1 : 0));
}

static ssize_t dfs_active_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int data;

	sscanf(buf, "%u\n", &data);

	switch (data) {
		case DFS_ENABLE:
			dfs_active = DFS_ENABLE;
			dprintk("%s: Enabled!\n", TAG);
			break;
		case DFS_DISABLE:
			dfs_active = DFS_DISABLE;
			dprintk("%s: Disabled!\n", TAG);
			break;
		default:
			return -EINVAL;
	}

	return count;
}

static ssize_t dfs_version_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "DFS Version: %u.%u\n",
		       DFS_VERSION_MAJOR, DFS_VERSION_MINOR);
}

static ssize_t dfs_suspended_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "DFS Suspended: %u\n", suspended);
}

/* Old names are used for backwards compatibility with third-party apps */
static struct kobj_attribute dfs_active_attribute =
	__ATTR(Dyn_fsync_active, 0666,
	       dfs_active_show, dfs_active_store);

static struct kobj_attribute dfs_version_attribute =
	__ATTR(Dyn_fsync_version, 0444,
	       dfs_version_show, NULL);

static struct kobj_attribute dfs_suspended_attribute =
	__ATTR(Dyn_fsync_earlysuspend, 0444,
	       dfs_suspended_show, NULL);

static struct attribute *dfs_active_attrs[] = {
	&dfs_active_attribute.attr,
	&dfs_version_attribute.attr,
	&dfs_suspended_attribute.attr,
	NULL,
};

static struct attribute_group dfs_active_attr_group = {
	.attrs = dfs_active_attrs,
};

static struct kobject *dfs_kobj;

static int dfs_init(void)
{
	int rc;

	rc = state_register_client(&dfs_state_notif);
	if (rc)
		pr_err("%s: Failed to register state notifier callback!\n",
									TAG);

	rc = register_reboot_notifier(&dfs_notifier);
	if (rc)
		pr_err("%s: Failed to register reboot notifier callback!\n",
									TAG);

	rc = atomic_notifier_chain_register(&panic_notifier_list,
					    &dfs_panic_block);
	if (rc)
		pr_err("%s: Failed to register panic notifier callback!\n",
									TAG);

	dfs_kobj = kobject_create_and_add("dyn_fsync", kernel_kobj);
	if (!dfs_kobj) {
		pr_err("%s: kobject create failed!\n", TAG);
		return -ENOMEM;
	}

	rc = sysfs_create_group(dfs_kobj, &dfs_active_attr_group);
	if (rc) {
		pr_info("%s: sysfs create failed!\n", TAG);
		kobject_put(dfs_kobj);
	}

	return rc;
}

static void dfs_exit(void)
{
	/* Unregister notifiers */
	state_unregister_client(&dfs_state_notif);
	dfs_state_notif.notifier_call = NULL;
	unregister_reboot_notifier(&dfs_notifier);
	atomic_notifier_chain_unregister(&panic_notifier_list,
		&dfs_panic_block);

	/* Free sysfs group */
	if (dfs_kobj != NULL)
		kobject_put(dfs_kobj);
}

module_init(dfs_init);
module_exit(dfs_exit);

MODULE_AUTHOR("Paul Reioux <reioux@gmail.com>");
MODULE_DESCRIPTION("Dynamic fsync - automatic fsync trigger.");
MODULE_LICENSE("GPLv3");
