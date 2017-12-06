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

#ifndef __WCD9XXX_SND_CTRL_H__
#define __WCD9XXX_SND_CTRL_H__

#include <linux/list.h>
#include <sound/soc.h>

/*
 * Ignore write attemptions coming from IOCTL to handled registers.
 * Note that sound codec must check for this flag in its filter call,
 * otherwise it won't change anything.
 */
#define SND_CTRL_BYPASS_IOCTL	BIT(0)

/**
 * snd_ctrl_set_bit() - set a specified flag to sound control data.
 * @snd_ctrl: pointer to sound control data.
 * @bit: a flag to be set.
 *
 * ! Note that here @snd_ctrl data is checked on validity before setting.
 */
#define snd_ctrl_set_bit(snd_ctrl, bit) ({	\
	if (likely(snd_ctrl))			\
		snd_ctrl->flags |= (bit); })

/**
 * snd_ctrl_rem_bit() - remove a specified flag to sound control data.
 * @snd_ctrl: pointer to sound control data.
 * @bit: a flag to be removed.
 *
 * ! Note that here @snd_ctrl data is checked on validity before removing.
 */
#define snd_ctrl_rem_bit(snd_ctrl, bit) ({	\
	if (likely(snd_ctrl))			\
		snd_ctrl->flags &= ~(bit); })

/**
 * snd_ctrl_has_bit() - check whether sound control data has a specified flag.
 * @snd_ctrl: pointer to sound control data.
 * @bit: a flag to be checked.
 *
 * ! Note that @snd_ctrl is not checked on validity. Ensure it is valid before.
 */
#define snd_ctrl_has_bit(snd_ctrl, bit)		\
	(!!(snd_ctrl->flags & (bit)))

/**
 * for_each_snd_line() - iterate over all supported sound lines.
 * @line: counter used as a loop cursor.
 */
#define for_each_snd_line(line)			\
	for ((line) = 0; (line) < NUM_SND_LINES; (line)++)

/* All supported sound lines are defined below */
enum snd_ctrl_lines {
	MIC_LINE,
	CAM_MIC_LINE,
	SPEAKER_L_LINE,
	SPEAKER_R_LINE,
	HEADPHONE_L_LINE,
	HEADPHONE_R_LINE,
	NUM_SND_LINES /* Used as a number of all defined sound lines */
};

/**
 * Each sound line consists of 2 parts:
 * @reg: register which belongs to that sound line (from 0x001 to 0x3FF),
 * @val: default value used in expected codec logic (from 0 to 256);
 */
struct snd_ctrl_line {
	unsigned int reg;
	unsigned int val;
};

struct snd_ctrl_data {
	struct list_head member;

	/* Sound codec conjuncted to a control data */
	struct snd_soc_codec *codec;

	/* Basic audio lines */
	struct snd_ctrl_line line[NUM_SND_LINES];

	/* Name of control data */
	const char *name;

	/* Data-specific control flags */
	unsigned long flags;

	/* Codec's I/O functions used to access to sound registers */
	unsigned int	(*read)		(struct snd_soc_codec *codec,
					 unsigned int reg);
	int		(*write)	(struct snd_soc_codec *codec,
					 unsigned int reg, unsigned int val);
};

int snd_ctrl_register(struct snd_ctrl_data *snd_data);
void snd_ctrl_unregister(struct snd_ctrl_data *snd_data);
bool snd_ctrl_data_handled(struct snd_ctrl_data *snd_data);

#endif /* __WCD9XXX_SND_CTRL_H__ */
