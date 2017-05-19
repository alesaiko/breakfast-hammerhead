/*
 * Touchscreen State Notifier
 *
 * Copyright (C) 2013-2017, Pranav Vashi <neobuddy89@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LINUX_TOUCHSCREEN_STATE_NOTIFIER_H
#define __LINUX_TOUCHSCREEN_STATE_NOTIFIER_H

#include <linux/notifier.h>

#define STATE_NOTIFIER_ACTIVE		0x01
#define STATE_NOTIFIER_SUSPEND		0x02

struct state_event {
	void *data;
};

extern bool state_suspended;
extern void state_suspend(void);
extern void state_resume(void);
int state_register_client(struct notifier_block *nb);
int state_unregister_client(struct notifier_block *nb);
int state_notifier_call_chain(unsigned long val, void *v);

#endif
