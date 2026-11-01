/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * host.h - what the machine asks of the board it runs on (main/platform_stubs.cpp on the Tab5,
 * tests/host/shim_machine/host_platform.c on the PC). */
#ifndef PC98_HOST_H
#define PC98_HOST_H

#include "compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* a free-running millisecond counter */
UINT32	host_ms(void);

/* the local date and time; 0 when the board has no clock (the machine then starts from 1980-01-01) */
typedef struct {
	UINT16	year;
	UINT8	month;		/* 1-12 */
	UINT8	day;		/* 1-31 */
	UINT8	hour, minute, second;
} HostDate;
int	host_date(HostDate *d);

/* the RGB565 picture the composite writes: *width x *height pixels, rows *stride bytes apart */
UINT16	*host_frame(int *width, int *height, int *stride);

/* the mouse: buttons as the 8255 reads them (active low, 80h left, 20h right), and the motion since the last call */
UINT8	host_mouse(SINT16 *dx, SINT16 *dy);

/* the joystick on the sound board's SSG port A: active low, FFh = nothing pressed */
UINT8	host_joypad(void);

/* drive activity, for the lamps */
void	host_fdd_access(UINT8 drv);
void	host_hdd_access(UINT8 drv);

#ifdef __cplusplus
}
#endif

#endif
