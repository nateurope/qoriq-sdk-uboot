/*
 * Copyright 2013 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#ifndef __ZM7300_H_
#define __ZM7300_H	1_

#include <common.h>
#include <i2c.h>
#include <errno.h>
#include <asm/io.h>

#define ZM_STEP 125
int zm7300_set_voltage(int voltage_1_10mv);
int zm_write_voltage(int voltage);
int zm_read_voltage(void);
int zm_disable_wp(void);
int zm_enable_wp(void);

#endif	/* __ZM7300_H_ */
