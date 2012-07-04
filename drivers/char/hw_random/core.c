/*
        Added support for the AMD Geode LX RNG
	(c) Copyright 2004-2005 Advanced Micro Devices, Inc.

	derived from

 	Hardware driver for the Intel/AMD/VIA Random Number Generators (RNG)
	(c) Copyright 2003 Red Hat Inc <jgarzik@redhat.com>

 	derived from

        Hardware driver for the AMD 768 Random Number Generator (RNG)
        (c) Copyright 2001 Red Hat Inc <alan@redhat.com>

 	derived from

	Hardware driver for Intel i810 Random Number Generator (RNG)
	Copyright 2000,2001 Jeff Garzik <jgarzik@pobox.com>
	Copyright 2000,2001 Philipp Rumpf <prumpf@mandrakesoft.com>

	Added generic RNG API
	Copyright 2006 Michael Buesch <m@bues.ch>
	Copyright 2005 (c) MontaVista Software, Inc.

	Please read Documentation/hw_random.txt for details on use.

	----------------------------------------------------------
	This software may be used and distributed according to the terms
        of the GNU General Public License, incorporated herein by reference.
 */

#define RNG_MODULE_NAME		"hw_random"
#define RNG_MISCDEV_MINOR	183 /* official */

#define pr_fmt(fmt)		RNG_MODULE_NAME ": " fmt

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/hw_random.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>

static struct hwrng *current_rng;
static struct task_struct *hwrng_fill;

static LIST_HEAD(rng_list);

/* Protects rng_list and current_rng */
static DEFINE_MUTEX(rng_mutex);

/* Protects rng read functions, data_avail, rng_buffer and rng_fillbuf */
static DEFINE_MUTEX(reading_mutex);

static int data_avail;
static unsigned char *rng_buffer, *rng_fillbuf;

static unsigned short __read_mostly default_quality; /* Default to "off" (0) */
module_param(default_quality, ushort, 0644);
MODULE_PARM_DESC(default_quality, "Default entropy content of HW RNG per mill");

static unsigned short __read_mostly current_quality;
module_param(current_quality, ushort, 0644);
MODULE_PARM_DESC(current_quality, "Current HW RNG entropy estimation per mill");

static inline int
rng_get_data(struct hwrng *rng, u8 *buffer, size_t size, int wait);
static inline int hwrng_init(struct hwrng *rng);

static __always_inline ssize_t rng_buffer_size(void)
{
	return max_t(size_t, SMP_CACHE_BYTES, 32);
}

static inline void add_early_randomness(struct hwrng *rng)
{
	int bytes_read;

	mutex_lock(&reading_mutex);
	bytes_read = rng_get_data(rng, rng_buffer, 16, 1);
	mutex_unlock(&reading_mutex);

	if (likely(bytes_read))
		add_device_randomness(rng_buffer, bytes_read);
}

static inline void cleanup_rng(struct kref *kref)
{
	struct hwrng *rng;

	rng = container_of(kref, struct hwrng, ref);
	if (rng->cleanup)
		rng->cleanup(rng);

	complete(&rng->cleanup_done);
}

static inline void put_rng(struct hwrng *rng)
{
	mutex_lock(&rng_mutex);
	if (likely(rng))
		kref_put(&rng->ref, cleanup_rng);
	mutex_unlock(&rng_mutex);
}

static inline void drop_current_rng(void)
{
	if (unlikely(!current_rng))
		return;

	/* Decrease last reference for triggering the cleanup */
	kref_put(&current_rng->ref, cleanup_rng);
	current_rng = NULL;
}

static inline int set_current_rng(struct hwrng *rng)
{
	int ret;

	ret = hwrng_init(rng);
	if (IS_ERR_VALUE(ret))
		return ret;

	/* Reassign current RNG device to a new one */
	drop_current_rng();
	current_rng = rng;

	return 0;
}

/* Returns ERR_PTR(), NULL or refcounted hwrng */
static struct hwrng *get_current_rng(void)
{
	struct hwrng *rng;

	if (unlikely(mutex_lock_interruptible(&rng_mutex)))
		return ERR_PTR(-ERESTARTSYS);

	rng = current_rng;
	if (likely(rng))
		kref_get(&rng->ref);
	mutex_unlock(&rng_mutex);

	return rng;
}

static int hwrng_fillfn(void *p)
{
	long rc;

	while (!kthread_should_stop()) {
		struct hwrng *rng;

		rng = get_current_rng();
		if (IS_ERR_OR_NULL(rng))
			break;

		mutex_lock(&reading_mutex);
		rc = rng_get_data(rng, rng_fillbuf, rng_buffer_size(), 1);
		mutex_unlock(&reading_mutex);
		put_rng(rng);

		if (unlikely(rc <= 0)) {
			pr_warn("%s: No data available\n", __func__);
			msleep_interruptible(10000);
			continue;
		}

		/* Outside lock, sure, but y'know: randomness */
		add_hwgenerator_randomness((void *)rng_fillbuf, rc,
					   (rc * current_quality * 8) >> 10);
	}

	hwrng_fill = NULL;

	return 0;
}

static inline void start_khwrngd(void)
{
	hwrng_fill = kthread_run(hwrng_fillfn, NULL, "hwrng");
	if (IS_ERR_OR_NULL(hwrng_fill)) {
		pr_err("%s: Unable to start HW RNG kernel thread\n", __func__);
		hwrng_fill = NULL;
	}
}

static inline int hwrng_init(struct hwrng *rng)
{
	if (kref_get_unless_zero(&rng->ref))
		goto skip_init;

	if (rng->init && rng->init(rng))
		return -EFAULT;

	kref_init(&rng->ref);
	reinit_completion(&rng->cleanup_done);
skip_init:
	add_early_randomness(rng);

	current_quality = min(rng->quality ? : default_quality, 1024);
	if (unlikely(!current_quality && hwrng_fill))
		kthread_stop(hwrng_fill);
	else if (likely(current_quality && !hwrng_fill))
		start_khwrngd();

	return 0;
}

static int rng_dev_open(struct inode *inode, struct file *filp)
{
	/* Enforce read-only access to this chardev */
	if (!(filp->f_mode & FMODE_READ && !(filp->f_mode & FMODE_WRITE)))
		return -EINVAL;

	return 0;
}

static inline int
rng_get_data(struct hwrng *rng, u8 *buffer, size_t size, int wait)
{
	int present;

	if (likely(rng->read))
		return rng->read(rng, (void *)buffer, size, wait);

	present = rng->data_present ? rng->data_present(rng, wait) : 1;
	if (likely(present))
		return rng->data_read(rng, (u32 *)buffer);

	return 0;
}

static ssize_t rng_dev_read(struct file *filp, char __user *buf,
			    size_t size, loff_t *offp)
{
	struct hwrng *rng;
	int err = 0, bytes, len;
	ssize_t ret = 0;

	while (size) {
		rng = get_current_rng();
		if (IS_ERR_OR_NULL(rng)) {
			err = PTR_ERR(rng);
			goto out;
		}

		mutex_lock(&reading_mutex);
		if (!data_avail) {
			bytes = rng_get_data(rng, rng_buffer, rng_buffer_size(),
					   !(filp->f_flags & O_NONBLOCK));
			if (IS_ERR_VALUE(bytes)) {
				err = bytes;
				goto out_unlock_reading;
			}

			data_avail = bytes;
		}

		if (!data_avail) {
			if (filp->f_flags & O_NONBLOCK) {
				err = -EAGAIN;
				goto out_unlock_reading;
			}
		} else {
			len = min_t(int, data_avail, size);
			data_avail -= len;

			if (copy_to_user(buf + ret,
			    rng_buffer + data_avail, len)) {
				err = -EFAULT;
				goto out_unlock_reading;
			}

			size -= len;
			ret += len;
		}
		mutex_unlock(&reading_mutex);
		put_rng(rng);

		if (need_resched())
			schedule_timeout_interruptible(1);

		if (signal_pending(current)) {
			err = -ERESTARTSYS;
			goto out;
		}
	}
out:
	return IS_ERR_VALUE(err) ? err : ret;
out_unlock_reading:
	mutex_unlock(&reading_mutex);
	put_rng(rng);
	goto out;
}

static const struct file_operations rng_chrdev_ops = {
	.owner		= THIS_MODULE,
	.open		= rng_dev_open,
	.read		= rng_dev_read,
	.llseek		= noop_llseek,
};

static struct miscdevice rng_miscdev = {
	.minor		= RNG_MISCDEV_MINOR,
	.name		= RNG_MODULE_NAME,
	.nodename	= "hwrng",
	.fops		= &rng_chrdev_ops,
};

static ssize_t hwrng_attr_current_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct hwrng *rng;
	ssize_t len;

	rng = get_current_rng();
	if (IS_ERR_OR_NULL(rng))
		return scnprintf(buf, 15, "<unsupported>\n");

	len = scnprintf(buf, PAGE_SIZE, "%s\n", rng ? rng->name : "none");
	put_rng(rng);

	return len;
}

static ssize_t hwrng_attr_current_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct hwrng *rng;
	int ret = -ENODEV;

	if (unlikely(mutex_lock_interruptible(&rng_mutex)))
		return -ERESTARTSYS;

	list_for_each_entry_reverse(rng, &rng_list, list) {
		if (sysfs_streq(rng->name, buf)) {
			ret = 0;
			if (likely(rng != current_rng))
				ret = set_current_rng(rng);
			break;
		}
	}
	mutex_unlock(&rng_mutex);

	return ret ? : len;
}

static DEVICE_ATTR(rng_current, 0644,
	hwrng_attr_current_show, hwrng_attr_current_store);

static ssize_t hwrng_attr_available_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct hwrng *rng;

	if (unlikely(mutex_lock_interruptible(&rng_mutex)))
		return -ERESTARTSYS;

	/* Clear the string first */
	buf[0] = '\0';

	list_for_each_entry_reverse(rng, &rng_list, list) {
		strlcat(buf, rng->name, PAGE_SIZE);
		strlcat(buf, " ", 2);
	}

	strlcat(--buf, "\n", 2);
	mutex_unlock(&rng_mutex);

	return strlen(buf);
}

static DEVICE_ATTR(rng_available, 0444, hwrng_attr_available_show, NULL);

static int __init register_miscdev(void)
{
	struct device *dev = rng_miscdev.this_device;
	int ret;

	ret = misc_register(&rng_miscdev);
	if (IS_ERR_VALUE(ret))
		return ret;

	ret = device_create_file(dev, &dev_attr_rng_current);
	if (IS_ERR_VALUE(ret))
		goto err_misc_dereg;

	ret = device_create_file(dev, &dev_attr_rng_available);
	if (IS_ERR_VALUE(ret))
		goto err_remove_current;

	return 0;

err_remove_current:
	device_remove_file(dev, &dev_attr_rng_current);
err_misc_dereg:
	misc_deregister(&rng_miscdev);

	return ret;
}

static void __exit unregister_miscdev(void)
{
	device_remove_file(rng_miscdev.this_device, &dev_attr_rng_available);
	device_remove_file(rng_miscdev.this_device, &dev_attr_rng_current);
	misc_deregister(&rng_miscdev);
}

int hwrng_register(struct hwrng *rng)
{
	struct hwrng *old_rng = current_rng, *tmp;
	int ret = -ENOMEM;

	if (IS_ERR_OR_NULL(rng) || IS_ERR_OR_NULL(rng->name) ||
	   (IS_ERR_OR_NULL(rng->read) && IS_ERR_OR_NULL(rng->data_read)))
		return -EINVAL;

	mutex_lock(&rng_mutex);
	if (IS_ERR_OR_NULL(rng_buffer)) {
		rng_buffer = kzalloc(rng_buffer_size(), GFP_KERNEL);
		if (IS_ERR_OR_NULL(rng_buffer))
			goto out_unlock;
	}

	if (IS_ERR_OR_NULL(rng_fillbuf)) {
		rng_fillbuf = kzalloc(rng_buffer_size(), GFP_KERNEL);
		if (IS_ERR_OR_NULL(rng_fillbuf))
			goto out_unlock;
	}

	/* Must not register two RNGs with the same name. */
	list_for_each_entry_reverse(tmp, &rng_list, list) {
		if (unlikely(!strnicmp(tmp->name, rng->name, PAGE_SIZE))) {
			ret = -EEXIST;
			goto out_unlock;
		}
	}

	init_completion(&rng->cleanup_done);
	complete(&rng->cleanup_done);

	ret = 0;
	if (IS_ERR_OR_NULL(old_rng) && set_current_rng(rng)) {
		ret = -EFAULT;
		goto out_unlock;
	}

	list_add_tail(&rng->list, &rng_list);
	if (old_rng && !rng->init) {
		/*
		 * Use a new device's input to add some randomness to the
		 * system. If this RNG device isn't going to be used right
		 * away, its init function hasn't been called yet; so only
		 * use the randomness from devices that don't need an init
		 * callback.
		 */
		add_early_randomness(rng);
	}
out_unlock:
	mutex_unlock(&rng_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(hwrng_register);

void hwrng_unregister(struct hwrng *rng)
{
	struct hwrng *new_rng;

	mutex_lock(&rng_mutex);
	list_del(&rng->list);

	if (current_rng == rng) {
		drop_current_rng();
		if (!list_empty(&rng_list)) {
			new_rng = list_entry(rng_list.prev, struct hwrng, list);
			set_current_rng(new_rng);
		}
	}

	if (list_empty(&rng_list)) {
		mutex_unlock(&rng_mutex);
		if (likely(hwrng_fill))
			kthread_stop(hwrng_fill);
	} else {
		mutex_unlock(&rng_mutex);
	}

	wait_for_completion(&rng->cleanup_done);
}
EXPORT_SYMBOL_GPL(hwrng_unregister);

static int __init hwrng_modinit(void)
{
	return register_miscdev();
}

static void __exit hwrng_modexit(void)
{
	mutex_lock(&rng_mutex);
	BUG_ON(current_rng);

	kzfree(rng_buffer);
	kzfree(rng_fillbuf);
	mutex_unlock(&rng_mutex);

	unregister_miscdev();
}

module_init(hwrng_modinit);
module_exit(hwrng_modexit);

MODULE_DESCRIPTION("H/W Random Number Generator (RNG) driver");
MODULE_LICENSE("GPL v2");
