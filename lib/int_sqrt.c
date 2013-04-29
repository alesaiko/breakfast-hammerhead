/*
 * Copyright (C) 2013-2018, Davidlohr Bueso <davidlohr.bueso@hp.com>
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

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/bitops.h>

/**
 * int_sqrt() - compute the integer square root.
 * @x: integer of which to calculate the sqrt.
 *
 * Computes: floor(sqrt(x)).
 */
unsigned long int_sqrt(unsigned long x)
{
	unsigned long b, m, y = 0;

	if (unlikely(x <= 1))
		return x;

	m = 1UL << (__fls(x) & ~1UL);
	while (m != 0) {
		b = y + m;
		y >>= 1;

		if (x >= b) {
			x -= b;
			y += m;
		}

		m >>= 2;
	}

	return y;
}
EXPORT_SYMBOL(int_sqrt);
