/*
 * Copyright (C) 2017-2019, Alex Saiko <solcmdr@gmail.com>
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

#define pr_fmt(fmt)	KBUILD_MODNAME ": %s: " fmt, __func__

#include <linux/of.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/platform_data/wcd9xxx-snd-ctrl.h>

#define CTRL_NAME_LEN	32

/* Flags for snd_global_data_ready() call */
enum {
	__WL,
	__RL
};

#define line_present(id)	\
	likely(ctrl_data->line[id].reg)

#define read_line(id)		\
	ctrl_data->read(ctrl_data->codec, ctrl_data->line[id].reg)

#define write_line(id, val)	\
	ctrl_data->write(ctrl_data->codec, ctrl_data->line[id].reg, (val))

/* Default control data that is likely to be used onto target control data */
static struct snd_ctrl_pdata *def_data;

/* Global sound control data which is used in all sysfs nodes */
static struct snd_ctrl_data *ctrl_data;

/* Kernel object where sysfs groups are located */
static struct kobject *snd_ctrl_kobj;

/* Mutex that protects access to linked list with registered codecs */
static DEFINE_MUTEX(list_mutex);

/* List of conjuncted control data */
static LIST_HEAD(ctrl_list);

static inline void snd_ctrl_switch_global(struct snd_ctrl_data *snd_data);
static inline bool snd_ctrl_data_global(struct snd_ctrl_data *snd_data);
static inline bool snd_ctrl_data_global_rw(struct snd_ctrl_data *snd_data);
static inline bool snd_ctrl_data_expected(struct snd_ctrl_data *snd_data);
static inline int snd_ctrl_data_fill(struct snd_ctrl_data *snd_data);
static struct snd_ctrl_data *find_ctrl_data(const char *ctrl_name);

/**
 * snd_ctrl_register() - register new sound control data.
 * @snd_data: pointer to sound control data.
 *
 * Tries to register passed control data. If one is incomplete or is already
 * registered, an appropriate negative will be returned. If this is the first
 * control data in a global control list, it will become a global one.
 *
 * In case Open Firmware or platform data is used, hence one of the control
 * data is expected, this function will fill the target one with the values
 * from OF/pdata source and immediately make it global, bypassing the queue.
 *
 * Returns 0 on success or -EINVAL[-EFAULT; -EEXIST] if a error occurred.
 */
int snd_ctrl_register(struct snd_ctrl_data *snd_data)
{
#define fail_on_null(_ptr, _msg, _code)	\
	if (IS_ERR_OR_NULL(_ptr)) {	\
		pr_err(_msg);		\
		return _code;		\
	}

	fail_on_null(snd_data,
		    "Cannot register a NULL pointer\n", -EFAULT);

	fail_on_null(snd_data->name,
		    "Cannot register unnamed control data\n", -EINVAL);

	fail_on_null(snd_data->codec,
		    "Cannot register control data without a codec\n", -EINVAL);

	fail_on_null(snd_data->read,
		    "Cannot register control data without read fn\n", -EINVAL);

#undef fail_on_null

	mutex_lock(&list_mutex);
	if (find_ctrl_data(snd_data->name) != NULL) {
		pr_err("%s already exists\n", snd_data->name);
		mutex_unlock(&list_mutex);
		return -EEXIST;
	}

	list_add(&snd_data->member, &ctrl_list);
	pr_debug("%s is registered\n", snd_data->name);

	if (snd_ctrl_data_expected(snd_data) && snd_ctrl_data_fill(snd_data))
		pr_err("Cannot fill %s\n", snd_data->name);

	if (list_is_singular(&ctrl_list) || snd_ctrl_data_expected(snd_data))
		snd_ctrl_switch_global(snd_data);
	mutex_unlock(&list_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(snd_ctrl_register);

/**
 * snd_ctrl_unregister() - unregister sound control data.
 * @snd_data: pointer to sound control data.
 *
 * Tries to unregister passed control data.  If one is incomplete or even has
 * not been registered yet, this function will return early. If that control
 * data is a global one right now and is going to be unregistered, the first
 * codec in the list will pretend to be a replacement.  In case there are no
 * available control data left, global control data will be nulled, hence
 * controlling will be disabled.
 */
void snd_ctrl_unregister(struct snd_ctrl_data *snd_data)
{
#define fail_on_null(_ptr, _msg)	\
	if (IS_ERR_OR_NULL(_ptr)) {	\
		pr_err(_msg);		\
		return;			\
	}

	fail_on_null(snd_data,
		    "Cannot unregister a NULL pointer\n");

	fail_on_null(snd_data->name,
		    "Cannot unregister unnamed control data\n");

#undef fail_on_null

	mutex_lock(&list_mutex);
	if (!find_ctrl_data(snd_data->name)) {
		pr_err("%s does not exist\n", snd_data->name);
		mutex_unlock(&list_mutex);
		return;
	}

	list_del_init(&snd_data->member);
	pr_debug("%s is unregistered\n", snd_data->name);

	if (snd_ctrl_data_global(snd_data) && !list_empty(&ctrl_list))
		snd_ctrl_switch_global(list_first_entry(&ctrl_list,
				       struct snd_ctrl_data, member));
	else if (list_empty(&ctrl_list))
		snd_ctrl_switch_global(NULL);
	mutex_unlock(&list_mutex);
}
EXPORT_SYMBOL_GPL(snd_ctrl_unregister);

/**
 * snd_ctrl_data_handled() - check whether passed control data is handled now.
 * @snd_data: pointer to sound control data.
 *
 * Checks the presence of controlled lines in passed control data. Ensures that
 * passed control data and a global one are the same and have an ability to
 * write to. Returns true only if at least one sound register is filled.
 */
bool snd_ctrl_data_handled(struct snd_ctrl_data *snd_data)
{
	int i;

	if (IS_ERR_OR_NULL(snd_data) || IS_ERR_OR_NULL(snd_data->name))
		return false;

	if (!snd_ctrl_data_global_rw(snd_data))
		return false;

	for_each_snd_line(i)
		if (snd_data->line[i].reg)
			return true;

	return false;
}
EXPORT_SYMBOL_GPL(snd_ctrl_data_handled);

/**
 * snd_ctrl_switch_global() - helper to switch global control data.
 * @snd_data: pointer to a new global control data. Can be NULL.
 */
static inline void snd_ctrl_switch_global(struct snd_ctrl_data *snd_data)
{
	ctrl_data = snd_data;

	if (snd_data != NULL)
		pr_debug("New global control data => %s\n", ctrl_data->name);
	else
		pr_debug("No available control data yet\n");
}

/**
 * find_ctrl_data() - search for control data in a global control data list.
 * @ctrl_name: name of a target member.
 *
 * Returns target member structure if found or NULL to the contrary.
 * ! This function must be called with list_mutex taken.
 */
static struct snd_ctrl_data *find_ctrl_data(const char *ctrl_name)
{
	struct snd_ctrl_data *entry;

	if (list_empty(&ctrl_list))
		return NULL;

	list_for_each_entry_reverse(entry, &ctrl_list, member)
		if (!strncmp(ctrl_name, entry->name, CTRL_NAME_LEN))
			return entry;

	return NULL;
}

/**
 * snd_ctrl_data_expected() - check if passed control data is set in OF/pdata.
 * @snd_data: pointer to sound control data.
 *
 * Compares the names of local (expected) control data from OF/pdata and a
 * passed one. Returns true if they are the same, false otherwise.
 */
static inline bool snd_ctrl_data_expected(struct snd_ctrl_data *snd_data)
{
	if (IS_ERR_OR_NULL(def_data))
		return false;

	return !strncmp(def_data->name, snd_data->name, CTRL_NAME_LEN);
}

/**
 * snd_global_data_ready() - check whether there is global control data.
 * @read_only: ignore the absence of write call.
 *
 * Returns false if some of the critical parts of the global sound control
 * data are NULL. Otherwise, returns true.
 */
static inline bool snd_global_data_ready(int read_only)
{
	return (ctrl_data &&
		ctrl_data->name &&
		ctrl_data->codec &&
		ctrl_data->read &&
	       (ctrl_data->write || read_only));
}

/**
 * For internal usage only. See the information below.
 */
static inline bool
__snd_ctrl_data_global(struct snd_ctrl_data *snd_data, int read_only)
{
	if (!snd_global_data_ready(read_only))
		return false;

	return !strncmp(ctrl_data->name, snd_data->name, CTRL_NAME_LEN);
}

/**
 * snd_ctrl_data_global() - check whether passed control data is global.
 * @snd_data: pointer to sound control data.
 *
 * Compares a passed control data's name and a global's one.
 * Returns true if they are the same, false otherwise.
 */
static inline bool snd_ctrl_data_global(struct snd_ctrl_data *snd_data)
{
	return __snd_ctrl_data_global(snd_data, __RL);
}

/**
 * snd_ctrl_data_global_rw() - check whether passed control data is global and
 * has an ability to write to sound codec.
 * @snd_data: pointer to sound control data.
 *
 * Compares a passed control data's name and a global's one and checks the
 * presence of write call in it. Returns true if both statements are true,
 * false otherwise.
 */
static inline bool snd_ctrl_data_global_rw(struct snd_ctrl_data *snd_data)
{
	return __snd_ctrl_data_global(snd_data, __WL);
}

/**
 * snd_ctrl_data_fill() - fill passed control data with values from OF/pdata.
 * @snd_data: pointer to sound control data.
 *
 * Assigns the values from local data which are gained from OF/pdata to passed
 * data. Returns -EFAULT[-ENOENT] on failure or 0 on success.
 */
static inline int snd_ctrl_data_fill(struct snd_ctrl_data *snd_data)
{
	int ret = -EFAULT, i;

	if (IS_ERR_OR_NULL(def_data))
		return -ENOENT;

	for_each_snd_line(i) {
		snd_data->line[i] = def_data->line[i];
		if (snd_data->line[i].reg)
			ret &= snd_data->write(snd_data->codec,
					       snd_data->line[i].reg,
					       snd_data->line[i].val);
	}

	return ret;
}

/**
 * parse_ctrl_data() - try to switch to another control data.
 * @snd_data: double pointer to sound control data to be switched.
 * @ctrl_name: name of a desired codec whose control data will be used instead.
 *
 * Returns 0 on success or -EINVAL to the contrary.
 */
static inline ssize_t
parse_ctrl_data(struct snd_ctrl_data **snd_data, const char *ctrl_name)
{
	struct snd_ctrl_data *tmp = NULL, named = { .name = ctrl_name };

	mutex_lock(&list_mutex);
	if (snd_ctrl_data_global(&named)) {
		mutex_unlock(&list_mutex);
		return -EINVAL;
	}

	/* This is used for debugging only */
	if (unlikely(!strnicmp(ctrl_name, "none", 4)))
		goto empty;

	tmp = find_ctrl_data(ctrl_name);
	if (IS_ERR_OR_NULL(tmp)) {
		mutex_unlock(&list_mutex);
		return -EINVAL;
	}
empty:
	*snd_data = tmp;
	mutex_unlock(&list_mutex);

	return 0;
}

#define create_rw_kobj_attr(name)					\
static struct kobj_attribute name =					\
	__ATTR(gpl_##name, 0644, show_##name, store_##name)

#define create_one_single(name, id)					\
static ssize_t show_##name##_gain(struct kobject *kobj,			\
				  struct kobj_attribute *attr,		\
				  char *buf)				\
{									\
	if (!snd_global_data_ready(__RL) || !line_present(id))		\
		return scnprintf(buf, 15, "<unsupported>\n");		\
									\
	return scnprintf(buf, 5, "%u\n", read_line(id));		\
}									\
									\
static ssize_t store_##name##_gain(struct kobject *kobj,		\
				   struct kobj_attribute *attr,		\
				   const char *buf, size_t count)	\
{									\
	int ret, val;							\
									\
	if (!snd_global_data_ready(__WL) || !line_present(id))		\
		return -ENOENT;						\
									\
	ret = sscanf(buf, "%d", &val);					\
	if (ret != 1 || val < 0 || val > 256)				\
		return -EINVAL;						\
									\
	ret = write_line(id, val);					\
									\
	return IS_ERR_VALUE(ret) ? -EINVAL : count;			\
}									\
									\
create_rw_kobj_attr(name##_gain)

#define create_one_double(name, idl, idr)				\
static ssize_t show_##name##_gain(struct kobject *kobj,			\
				  struct kobj_attribute *attr,		\
				  char *buf)				\
{									\
	if (!snd_global_data_ready(__RL) ||				\
	    !line_present(idl) || !line_present(idr))			\
		return scnprintf(buf, 15, "<unsupported>\n");		\
									\
	return scnprintf(buf, 9, "%u %u\n",				\
			 read_line(idl), read_line(idr));		\
}									\
									\
static ssize_t store_##name##_gain(struct kobject *kobj,		\
				   struct kobj_attribute *attr,		\
				   const char *buf, size_t count)	\
{									\
	int ret, lval, rval;						\
									\
	if (!snd_global_data_ready(__WL) ||				\
	    !line_present(idl) || !line_present(idr))			\
		return -ENOENT;						\
									\
	ret = sscanf(buf, "%d %d", &lval, &rval);			\
	if (ret != 2 ||							\
	    lval < 0 || lval > 256 ||					\
	    rval < 0 || rval > 256)					\
		return -EINVAL;						\
									\
	ret  = write_line(idl, lval);					\
	ret |= write_line(idr, rval);					\
									\
	return IS_ERR_VALUE(ret) ? -EINVAL : count;			\
}									\
									\
create_rw_kobj_attr(name##_gain)

#define create_line_control(name, id)					\
static ssize_t show_##name##_line(struct kobject *kobj,			\
				  struct kobj_attribute *attr,		\
				  char *buf)				\
{									\
	if (!snd_global_data_ready(__RL) || !line_present(id))		\
		return scnprintf(buf, 8, "<none>\n");			\
									\
	return scnprintf(buf, 7, "0x%03X\n", ctrl_data->line[id].reg);	\
}									\
									\
static ssize_t store_##name##_line(struct kobject *kobj,		\
				   struct kobj_attribute *attr,		\
				   const char *buf, size_t count)	\
{									\
	int ret, reg;							\
									\
	if (!snd_global_data_ready(__RL))				\
		return -ENODEV;						\
									\
	ret = sscanf(buf, "%X", &reg);					\
	if (ret != 1 || reg < 0 || reg > 0x3FF)				\
		return -EINVAL;						\
									\
	ctrl_data->line[id].reg = reg;					\
									\
	return count;							\
}									\
									\
create_rw_kobj_attr(name##_line)

static ssize_t show_active_codec(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	struct snd_ctrl_data *entry;
	ssize_t len = 0;

	mutex_lock(&list_mutex);
	if (list_empty(&ctrl_list)) {
		mutex_unlock(&list_mutex);
		return scnprintf(buf, 8, "<none>\n");
	}

	list_for_each_entry(entry, &ctrl_list, member)
		len += scnprintf(buf + len, CTRL_NAME_LEN + 4,
				 snd_ctrl_data_global(entry) ?
				 "[%s] " : "%s ", entry->name);
	mutex_unlock(&list_mutex);

	/* Remove whitespace and put a new line */
	scnprintf(buf + len - 1, 2, "\n");

	return len;
}

static ssize_t store_active_codec(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	char name[CTRL_NAME_LEN] = { 0 };
	ssize_t ret;

	ret = sscanf(buf, "%32s", name);
	if (ret != 1 || IS_ERR_OR_NULL(name))
		return -EINVAL;

	ret = parse_ctrl_data(&ctrl_data, name);

	return IS_ERR_VALUE(ret) ? ret : count;
}

static ssize_t show_ioctl_bypass(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	if (!snd_global_data_ready(__WL))
		return scnprintf(buf, 15, "<unsupported>\n");

	return snd_ctrl_has_bit(ctrl_data, SND_CTRL_BYPASS_IOCTL) ?
			scnprintf(buf, 17, "Restricted mode\n") :
			scnprintf(buf, 13, "Hybrid mode\n");
}

static ssize_t store_ioctl_bypass(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	int ret, bypass;

	if (!snd_global_data_ready(__WL))
		return -ENODEV;

	ret = kstrtouint(buf, 2, &bypass);
	if (ret || bypass == snd_ctrl_has_bit(ctrl_data, SND_CTRL_BYPASS_IOCTL))
		return -EINVAL;

	bypass ? snd_ctrl_set_bit(ctrl_data, SND_CTRL_BYPASS_IOCTL) :
		 snd_ctrl_rem_bit(ctrl_data, SND_CTRL_BYPASS_IOCTL);

	return count;
}

create_one_single(mic, MIC_LINE);
create_one_single(cam_mic, CAM_MIC_LINE);
create_one_double(speaker, SPEAKER_L_LINE, SPEAKER_R_LINE);
create_one_double(headphone, HEADPHONE_L_LINE, HEADPHONE_R_LINE);

create_line_control(mic, MIC_LINE);
create_line_control(cam_mic, CAM_MIC_LINE);
create_line_control(speaker_l, SPEAKER_L_LINE);
create_line_control(speaker_r, SPEAKER_R_LINE);
create_line_control(headphone_l, HEADPHONE_L_LINE);
create_line_control(headphone_r, HEADPHONE_R_LINE);

create_rw_kobj_attr(active_codec);
create_rw_kobj_attr(ioctl_bypass);

static struct attribute *snd_ctrl_attrs[] = {
	&mic_gain.attr,
	&cam_mic_gain.attr,
	&speaker_gain.attr,
	&headphone_gain.attr,
	&active_codec.attr,
	&ioctl_bypass.attr,
	NULL
};

static struct attribute *snd_ctrl_lines[] = {
	&mic_line.attr,
	&cam_mic_line.attr,
	&speaker_l_line.attr,
	&speaker_r_line.attr,
	&headphone_l_line.attr,
	&headphone_r_line.attr,
	NULL
};

static struct attribute_group snd_ctrl_attr_group = {
	.attrs = snd_ctrl_attrs
};

static struct attribute_group snd_ctrl_lines_group = {
	.name = "snd_lines",
	.attrs = snd_ctrl_lines
};

/**
 * is_enabled() - check whether Device Tree node is enabled.
 * @node: pointer to Device Tree node.
 *
 * Returns true if __status = "disabled"__ is not set. False otherwise.
 */
static inline bool __devinit is_enabled(struct device_node *node)
{
	if (IS_ERR_OR_NULL(node))
		return false;

	return of_property_match_string(node, "status", "disabled") < 0;
}

/**
 * snd_ctrl_parse_dt() - parse a passed Device Tree node and try to gather
 * default sound control data from it.
 * @node: pointer to Device Tree node.
 *
 * Tries to set up def_data according to the information gained from Device
 * Tree node.  Returns -EINVAL if either default codec name or all sound
 * lines are unset in OF, as it is undesirable to fail if some of the sound
 * lines are missing in node, so the only case is an absence of them all.
 *
 * This function expects sound line data to be passed in a 2-byte uint array:
 * [0] --> sound line register itself (0 - 0x3FF),
 * [1] --> default sound gain of a register (0 - 256);
 *
 * The transmitted data will be applied as soon as a codec with a specified
 * name will be registered.  Ensure the data is within the bounds above, as
 * this function omits any bounds checking.
 *
 * Returns 0 on success or -EINVAL[| -ENODATA | -EOVERFLOW] to the contrary.
 *
 * ! Note that def_data must be initialized before the call.
 */
static inline int __devinit snd_ctrl_parse_dt(struct device_node *node)
{
	u32 data[2] = { 0 }; /* 2-byte data is expected from Device Tree */
	char *key;
	int ret;

	key = "qcom,codec_name";
	ret = of_property_read_string(node, key, &def_data->name);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Cannot get codec name\n");
		return -EINVAL;
	}

	/**
	 * get_line() - try to get line data from Device Tree.
	 * @key: name of a Device Tree property.
	 * @id: identificator of a sound line to store the values into.
	 *
	 * ! Note that this macro does not do any bounds checking.
	 */
#define get_line(key, id) ({						    \
	int rc;								    \
									    \
	rc = of_property_read_u32_array(node, key, data, ARRAY_SIZE(data)); \
	if (IS_ERR_VALUE(rc)) {						    \
		pr_err("Cannot get %s\n", key);	    			    \
	} else {							    \
		def_data->line[id].reg = data[0];			    \
		def_data->line[id].val = data[1];			    \
	}								    \
									    \
	ret &= rc; })

	/* Fail only if all the keys are unstated */
	ret = -EINVAL | -ENODATA | -EOVERFLOW;
	get_line("qcom,mic_line", MIC_LINE);
	get_line("qcom,cam_mic_line", CAM_MIC_LINE);
	get_line("qcom,speaker_l_line", SPEAKER_L_LINE);
	get_line("qcom,speaker_r_line", SPEAKER_R_LINE);
	get_line("qcom,headphone_l_line", HEADPHONE_L_LINE);
	get_line("qcom,headphone_r_line", HEADPHONE_R_LINE);

	return ret;
}

/**
 * snd_ctrl_parse_pdata() - parse a passed platform data and try to gather
 * default sound control data from it.
 * @pdata: pointer to sound control platform data.
 *
 * The description is mostly the same as in snd_ctrl_parse_dt() with the
 * point that it is adapted to platform data implementation. Here a simple
 * memory copy is used to gain default sound lines. The rules of pdata filling
 * are the same as in DT variant.
 *
 * Returns 0 on success or -EINVAL to the contrary.
 *
 * ! Note that def_data must be initialized before the call.
 */
static inline int __devinit snd_ctrl_parse_pdata(struct snd_ctrl_pdata *pdata)
{
	def_data->name = pdata->name;
	if (IS_ERR_OR_NULL(def_data->name)) {
		pr_err("Unable to get codec name from platform data\n");
		return -EINVAL;
	}

	/* Copy default sound lines. Bounds checking is omitted. */
	memcpy(def_data->line, pdata->line, sizeof(def_data->line));

	return 0;
}

static int __devinit snd_ctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int ret = 0;

	def_data = devm_kzalloc(dev, sizeof(*def_data), GFP_KERNEL);
	if (IS_ERR_OR_NULL(def_data)) {
		dev_err(dev,
			"Unable to allocate memory for default control data\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, def_data);

	if (is_enabled(dev->of_node)) {
		ret = snd_ctrl_parse_dt(dev->of_node);
		if (IS_ERR_VALUE(ret)) {
			dev_err(dev, "Unable to parse device tree\n");
			goto fail_parse;
		}
	} else if (dev->platform_data) {
		ret = snd_ctrl_parse_pdata(dev->platform_data);
		if (IS_ERR_VALUE(ret)) {
			dev_err(dev, "Unable to parse platform data\n");
			goto fail_parse;
		}
	} else {
		goto fail_parse;
	}

	return 0;

fail_parse:
	platform_set_drvdata(pdev, NULL);
	devm_kfree(dev, def_data);

	return ret;
}

static int __devexit snd_ctrl_remove(struct platform_device *pdev)
{
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id snd_ctrl_match_table[] = {
	{ .compatible = "qcom,wcd9xxx-snd-ctrl" },
	{ }
};

static struct platform_driver snd_ctrl_driver = {
	.probe = snd_ctrl_probe,
	.remove = __devexit_p(snd_ctrl_remove),
	.driver = {
		.name = "wcd9xxx-snd-ctrl",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(snd_ctrl_match_table)
	}
};

static int __init snd_ctrl_init(void)
{
	int ret;

	ret = platform_driver_register(&snd_ctrl_driver);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to register platform driver\n");
		goto fail_pdrv;
	}

	snd_ctrl_kobj = kobject_create_and_add("sound_control_3", kernel_kobj);
	if (IS_ERR_OR_NULL(snd_ctrl_kobj)) {
		pr_err("Unable to create sysfs kernel object\n");
		ret = -ENOMEM;
		goto fail_kobj;
	}

	ret = sysfs_create_group(snd_ctrl_kobj, &snd_ctrl_attr_group);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to create sound attributes group\n");
		goto fail_attrs;
	}

	ret = sysfs_create_group(snd_ctrl_kobj, &snd_ctrl_lines_group);
	if (IS_ERR_VALUE(ret)) {
		pr_err("Unable to create sound lines group\n");
		goto fail_lines;
	}

	return 0;

fail_lines:
	sysfs_remove_group(snd_ctrl_kobj, &snd_ctrl_attr_group);
fail_attrs:
	kobject_del(snd_ctrl_kobj);
fail_kobj:
	platform_driver_unregister(&snd_ctrl_driver);
fail_pdrv:
	return ret;
}

static void __exit snd_ctrl_exit(void)
{
	sysfs_remove_group(snd_ctrl_kobj, &snd_ctrl_lines_group);
	sysfs_remove_group(snd_ctrl_kobj, &snd_ctrl_attr_group);
	kobject_del(snd_ctrl_kobj);

	platform_driver_unregister(&snd_ctrl_driver);
}

module_init(snd_ctrl_init);
module_exit(snd_ctrl_exit);

MODULE_AUTHOR("Alex Saiko <solcmdr@gmail.com>");
MODULE_DESCRIPTION("WCD9xxx Sound Register SysFS Controller");
MODULE_LICENSE("GPL v2");
