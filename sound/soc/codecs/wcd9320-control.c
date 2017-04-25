/*
 * WCD9320 Sound Volume Controller
 *
 * Copyright (C) 2013, Paul Reioux <reioux@gmail.com>
 * Copyright (C) 2017, Alex Saiko <solcmdr@gmail.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 3, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/kallsyms.h>

#include <linux/mfd/wcd9xxx/wcd9320_registers.h>

#define SOUND_CONTROL_MAJOR_VERSION	4
#define SOUND_CONTROL_MINOR_VERSION	6

/* A Sound Control toggle */
unsigned int snd_ctrl_enabled;
EXPORT_SYMBOL(snd_ctrl_enabled);

/*
 * Local locking mechanism.
 *
 * It is used for proper modification and keeping of lines' registers.
 * See taiko_write function for more info.
 */
static int snd_ctrl_locked;
static unsigned int selected_reg = 0xdeadbeef;

/* Sound Codec data which gets modified by read/write functions */
extern struct snd_soc_codec *snd_engine_codec_ptr;

/* Main functions that are used to modify registers */
unsigned int taiko_read(struct snd_soc_codec *codec, unsigned int reg);
int taiko_write(struct snd_soc_codec *codec,
		unsigned int reg, unsigned int value);

/*
 * Previously cached registers.
 *
 * They are used when there is no access to existing registers. This keeps
 * modified values from unwanted resetting.
 */
static unsigned int cached_regs[] = {6, 6, 0, 0, 0, 0, 0, 0, 0, 0,
				     0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
				     0, 0, 0, 0, 0 };

/* Select a register and recieve its value in a cached array */
static unsigned int *cache_select(unsigned int reg)
{
	unsigned int *out = NULL;

	switch (reg) {
		case TAIKO_A_CDC_RX1_VOL_CTL_B2_CTL:
			out = &cached_regs[4];
			break;
		case TAIKO_A_CDC_RX2_VOL_CTL_B2_CTL:
			out = &cached_regs[5];
			break;
		case TAIKO_A_CDC_RX3_VOL_CTL_B2_CTL:
			out = &cached_regs[6];
			break;
		case TAIKO_A_CDC_RX4_VOL_CTL_B2_CTL:
			out = &cached_regs[7];
			break;
		case TAIKO_A_CDC_RX5_VOL_CTL_B2_CTL:
			out = &cached_regs[8];
			break;
		case TAIKO_A_CDC_RX6_VOL_CTL_B2_CTL:
			out = &cached_regs[9];
			break;
		case TAIKO_A_CDC_RX7_VOL_CTL_B2_CTL:
			out = &cached_regs[10];
			break;
		case TAIKO_A_CDC_TX1_VOL_CTL_GAIN:
			out = &cached_regs[11];
			break;
		case TAIKO_A_CDC_TX2_VOL_CTL_GAIN:
			out = &cached_regs[12];
			break;
		case TAIKO_A_CDC_TX3_VOL_CTL_GAIN:
			out = &cached_regs[13];
			break;
		case TAIKO_A_CDC_TX4_VOL_CTL_GAIN:
			out = &cached_regs[14];
			break;
		case TAIKO_A_CDC_TX5_VOL_CTL_GAIN:
			out = &cached_regs[15];
			break;
		case TAIKO_A_CDC_TX6_VOL_CTL_GAIN:
			out = &cached_regs[16];
			break;
		case TAIKO_A_CDC_TX7_VOL_CTL_GAIN:
			out = &cached_regs[17];
			break;
		case TAIKO_A_CDC_TX8_VOL_CTL_GAIN:
			out = &cached_regs[18];
			break;
		case TAIKO_A_CDC_TX9_VOL_CTL_GAIN:
			out = &cached_regs[19];
			break;
		case TAIKO_A_CDC_TX10_VOL_CTL_GAIN:
			out = &cached_regs[20];
			break;
		case TAIKO_A_RX_LINE_1_GAIN:
			out = &cached_regs[21];
			break;
		case TAIKO_A_RX_LINE_2_GAIN:
			out = &cached_regs[22];
			break;
		case TAIKO_A_RX_LINE_3_GAIN:
			out = &cached_regs[23];
			break;
		case TAIKO_A_RX_LINE_4_GAIN:
			out = &cached_regs[24];
			break;
	}

	return out;
}

/*
 * Callback - Sound Register is accessable.
 *
 * This is used to ensure whether we have an access to a particular register
 * and have an opportunity to override it.
 *
 * Locking mechanism is used here to prevent anything except this module from
 * modifying these values.
 */
int snd_reg_access(unsigned int reg)
{
	int ret = 1;

	switch (reg) {
		/* Headphones' registers */
		case TAIKO_A_CDC_RX1_VOL_CTL_B2_CTL:
		case TAIKO_A_CDC_RX2_VOL_CTL_B2_CTL:
			/* Prevent access if it is locked */
			if (snd_ctrl_locked)
				ret = 0;
			break;
		/* Not used */
		case TAIKO_A_CDC_RX3_VOL_CTL_B2_CTL:
		case TAIKO_A_CDC_RX4_VOL_CTL_B2_CTL:
		case TAIKO_A_CDC_RX5_VOL_CTL_B2_CTL:
		case TAIKO_A_CDC_RX6_VOL_CTL_B2_CTL:
			/* Prevent access if it is locked */
			if (snd_ctrl_locked)
				ret = 0;
			break;
		/* Speaker's register */
		case TAIKO_A_CDC_RX7_VOL_CTL_B2_CTL:
			/* Prevent access if it is locked */
			if (snd_ctrl_locked)
				ret = 0;
			break;
		/* Line out's registers */
		case TAIKO_A_RX_LINE_1_GAIN:
		case TAIKO_A_RX_LINE_2_GAIN:
		case TAIKO_A_RX_LINE_3_GAIN:
		case TAIKO_A_RX_LINE_4_GAIN:
			/* Prevent access if it is locked */
			if (snd_ctrl_locked)
				ret = 0;
			break;
		/* Not used */
		case TAIKO_A_CDC_TX1_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX2_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX3_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX4_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX5_VOL_CTL_GAIN:
			/* Prevent access if it is locked */
			if (snd_ctrl_locked)
				ret = 0;
			break;
		/* Camera microphone's register */
		case TAIKO_A_CDC_TX6_VOL_CTL_GAIN:
			/* Prevent access if it is locked */
			if (snd_ctrl_locked)
				ret = 0;
			break;
		/* Incall microphone's registers */
		case TAIKO_A_CDC_TX7_VOL_CTL_GAIN:
			/* Prevent access if it is locked */
			if (snd_ctrl_locked)
				ret = 0;
			break;
		/* Not used */
		case TAIKO_A_CDC_TX8_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX9_VOL_CTL_GAIN:
		case TAIKO_A_CDC_TX10_VOL_CTL_GAIN:
			/* Prevent access if it is locked */
			if (snd_ctrl_locked)
				ret = 0;
			break;
		default:
			break;
	}

	return ret;
}
EXPORT_SYMBOL(snd_reg_access);

/*
 * Write a value to a cached register.
 *
 * This function is used to write a value to a particular cached register if
 * it is available.
 */
void snd_cache_write(unsigned int reg, unsigned int value)
{
	unsigned int *tmp = cache_select(reg);

	if (tmp != NULL)
		*tmp = value;
}
EXPORT_SYMBOL(snd_cache_write);

/*
 * Read a value from a cached register.
 *
 * This function is used to read a value from a particular cached register if
 * it is available.
 */
int snd_cache_read(unsigned int reg)
{
	if (cache_select(reg) != NULL)
		return *cache_select(reg);
	else
		return -1;
}
EXPORT_SYMBOL(snd_cache_read);

/* Camera microphone's sysfs */
static ssize_t cam_mic_gain_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n",
			taiko_read(snd_engine_codec_ptr,
				TAIKO_A_CDC_TX6_VOL_CTL_GAIN));
}

static ssize_t cam_mic_gain_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	unsigned int val;

	sscanf(buf, "%u", &val);

	if (!snd_ctrl_enabled)
		return count;

	snd_ctrl_locked = 0;
	taiko_write(snd_engine_codec_ptr,
		TAIKO_A_CDC_TX6_VOL_CTL_GAIN, val);
	snd_ctrl_locked = 1;

	return count;
}

/* Incall microphone's sysfs */
static ssize_t mic_gain_show(struct kobject *kobj,
			     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n",
			taiko_read(snd_engine_codec_ptr,
				TAIKO_A_CDC_TX7_VOL_CTL_GAIN));
}

static ssize_t mic_gain_store(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned int val;

	sscanf(buf, "%u", &val);

	if (!snd_ctrl_enabled)
		return count;

	snd_ctrl_locked = 0;
	taiko_write(snd_engine_codec_ptr,
		TAIKO_A_CDC_TX7_VOL_CTL_GAIN, val);
	snd_ctrl_locked = 1;

	return count;
}

/* Speaker's sysfs */
static ssize_t speaker_gain_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u %u\n",
			taiko_read(snd_engine_codec_ptr,
				TAIKO_A_CDC_RX7_VOL_CTL_B2_CTL),
			taiko_read(snd_engine_codec_ptr,
				TAIKO_A_CDC_RX7_VOL_CTL_B2_CTL));
}

static ssize_t speaker_gain_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	unsigned int lval, rval;

	sscanf(buf, "%u %u", &lval, &rval);

	if (!snd_ctrl_enabled)
		return count;

	/* For mono speaker lval = rval */
	snd_ctrl_locked = 0;
	taiko_write(snd_engine_codec_ptr,
		TAIKO_A_CDC_RX7_VOL_CTL_B2_CTL, lval);
	taiko_write(snd_engine_codec_ptr,
		TAIKO_A_CDC_RX7_VOL_CTL_B2_CTL, rval);
	snd_ctrl_locked = 1;

	return count;
}

/* Headphones' sysfs */
static ssize_t headphone_gain_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u %u\n",
			taiko_read(snd_engine_codec_ptr,
				TAIKO_A_CDC_RX1_VOL_CTL_B2_CTL),
			taiko_read(snd_engine_codec_ptr,
				TAIKO_A_CDC_RX2_VOL_CTL_B2_CTL));
}

static ssize_t headphone_gain_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	unsigned int lval, rval;

	sscanf(buf, "%u %u", &lval, &rval);

	if (!snd_ctrl_enabled)
		return count;

	snd_ctrl_locked = 0;
	taiko_write(snd_engine_codec_ptr,
		TAIKO_A_CDC_RX1_VOL_CTL_B2_CTL, lval);
	taiko_write(snd_engine_codec_ptr,
		TAIKO_A_CDC_RX2_VOL_CTL_B2_CTL, rval);
	snd_ctrl_locked = 1;

	return count;
}

/* Sound registers' sysfs */
static ssize_t sound_reg_select_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	if (!snd_ctrl_enabled)
		return count;

	sscanf(buf, "%u", &selected_reg);

	return count;
}

static ssize_t sound_reg_read_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	if (selected_reg == 0xdeadbeef)
		return -1;
	else
		return sprintf(buf, "%u\n",
			taiko_read(snd_engine_codec_ptr, selected_reg));
}

static ssize_t sound_reg_write_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned int val;

	sscanf(buf, "%u", &val);

	if (!snd_ctrl_enabled)
		return count;

	if (selected_reg != 0xdeadbeef)
		taiko_write(snd_engine_codec_ptr, selected_reg, val);

	return count;
}

/* Version of this module */
static ssize_t sound_control_version_show(struct kobject *kobj,
					  struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "version: %u.%u\n",
			SOUND_CONTROL_MAJOR_VERSION,
			SOUND_CONTROL_MINOR_VERSION);
}

/* Sound Control toggle's sysfs */
static ssize_t sound_control_enabled_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	unsigned int val;

	sscanf(buf, "%u", &val);
	if (val > 1)
		val = 1;

	snd_ctrl_enabled = val;

	return count;
}

static ssize_t sound_control_enabled_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *buf)
{
	return sprintf(buf, "%u\n", snd_ctrl_enabled);
}

/* Register kernel object's attributes and initialize them */
static struct kobj_attribute sound_reg_sel_attribute =
	__ATTR(sound_reg_sel, 0222,
	       NULL, sound_reg_select_store);

static struct kobj_attribute sound_reg_read_attribute =
	__ATTR(sound_reg_read, 0444,
	       sound_reg_read_show, NULL);

static struct kobj_attribute sound_reg_write_attribute =
	__ATTR(sound_reg_write, 0644,
	       NULL, sound_reg_write_store);

static struct kobj_attribute cam_mic_gain_attribute =
	__ATTR(gpl_cam_mic_gain, 0644,
	       cam_mic_gain_show, cam_mic_gain_store);

static struct kobj_attribute mic_gain_attribute =
	__ATTR(gpl_mic_gain, 0644,
	       mic_gain_show, mic_gain_store);

static struct kobj_attribute speaker_gain_attribute =
	__ATTR(gpl_speaker_gain, 0644,
	       speaker_gain_show, speaker_gain_store);

static struct kobj_attribute headphone_gain_attribute =
	__ATTR(gpl_headphone_gain, 0644,
	       headphone_gain_show, headphone_gain_store);

static struct kobj_attribute sound_control_version_attribute =
	__ATTR(gpl_sound_control_version, 0444,
	       sound_control_version_show, NULL);

static struct kobj_attribute sound_control_enabled_attribute =
	__ATTR(gpl_sound_control_enabled, 0644,
	       sound_control_enabled_show, sound_control_enabled_store);

static struct attribute *sound_control_attrs[] = {
	&sound_reg_sel_attribute.attr,
	&sound_reg_read_attribute.attr,
	&sound_reg_write_attribute.attr,
	&cam_mic_gain_attribute.attr,
	&mic_gain_attribute.attr,
	&speaker_gain_attribute.attr,
	&headphone_gain_attribute.attr,
	&sound_control_version_attribute.attr,
	&sound_control_enabled_attribute.attr,
	NULL,
};

static struct attribute_group sound_control_attr_group = {
	.attrs = sound_control_attrs,
};

static struct kobject *sound_control_kobj;

static int sound_control_init(void)
{
	int sysfs_result;

	/* Create a kernel object and use it for a sysfs group */
	sound_control_kobj = kobject_create_and_add("sound_control_3",
						     kernel_kobj);
	if (!sound_control_kobj) {
		pr_err("%s sound_control_kobj create failed!\n", __FUNCTION__);
		return -ENOMEM;
	}

	sysfs_result = sysfs_create_group(sound_control_kobj,
					 &sound_control_attr_group);
	if (sysfs_result) {
		pr_info("%s sysfs create failed!\n", __FUNCTION__);
		kobject_put(sound_control_kobj);
	}

	return sysfs_result;
}

static void sound_control_exit(void)
{
	/* Free sysfs group */
	if (sound_control_kobj != NULL)
		kobject_put(sound_control_kobj);
}

module_init(sound_control_init);
module_exit(sound_control_exit);

MODULE_LICENSE("GPLv3");
MODULE_AUTHOR("Paul Reioux <reioux@gmail.com>");
MODULE_DESCRIPTION("WCD9320 Sound Volume Controller");
