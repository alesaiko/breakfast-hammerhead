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

#ifndef _LINUX_TOUCHSCREEN_WAKE_GESTURES_H
#define _LINUX_TOUCHSCREEN_WAKE_GESTURES_H

/* Gesture data */
#define S2W_DEFAULT		0
#define DT2W_DEFAULT		0

#define S2W_PWRKEY_DUR		60
#define DT2W_PWRKEY_DUR		60

#define SWEEP_TIMEOUT		75
#define DT2W_TIME		75

#define S2W_Y_MAX		1920
#define S2W_X_MAX		1080
#define S2W_Y_LIMIT		S2W_Y_MAX-130
#define S2W_X_B1		400
#define S2W_X_B2		700
#define S2W_X_FINAL		275
#define S2W_Y_NEXT		180
#define DT2W_FEATHER		60

#define VIB_STRENGTH		0
#define TRIGGER_TIMEOUT		60

#define WAKE_GESTURE		0x0b
#define SWEEP_RIGHT		0x01
#define SWEEP_LEFT		0x02
#define SWEEP_UP		0x04
#define SWEEP_DOWN		0x08

extern int s2w_switch;
extern int dt2w_switch;

extern int gestures_switch;
extern int vib_strength;

extern bool pwrkey_pressed;
extern bool in_phone_call;

#endif /* _LINUX_TOUCHSCREEN_WAKE_GESTURES_H */
