/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * events.h - timed callbacks, in CPU clocks (events.c).
 *
 * The CPU runs in slices: g_cpu.slice_len is the length of the current slice, g_cpu.slice_left what is left of it.  A slice
 * always ends at the earliest event; ev_slice_done() then calls the events that are due, in deadline order (equal
 * deadlines in the order they were armed), and starts the next slice. */
#ifndef PC98_EVENTS_H
#define PC98_EVENTS_H

#include "compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the event slots; each user owns one */
enum {
	EV_FRAME = 0,		/* display frame: display period / retrace (gdc7220.c) */
	EV_TIMER0,		/* PIT counter 0 (pit.c) */
	EV_BEEP,		/* PIT counter 1, the beeper (pit.c) */
	EV_SERIAL,		/* PIT counter 2, the RS-232C baud clock (pit.c) */
	EV_FMTIMER_A,		/* YM2203 timers (ym2203.c) */
	EV_FMTIMER_B,
	EV_MOUSE,		/* bus mouse interrupt (mouse.c) */
	EV_KEYBOARD,		/* a byte from the keyboard (kbd.c) */
	EV_PICMASK,		/* PIC: a masked timer request (pic.c) */
	EV_GDCDRAW,		/* graphics GDC drawing time (gdcdraw.c) */
	EV_FDCINT,		/* FDC interrupt / state (fdc765.c) */
	EV_FDCSTATE,
	EV_FDCBYTE,		/* one byte passes the floppy data register */
	EV_COUNT
};

/* timed = 1: called by the scheduler at the deadline; 0: called directly (ev_call, or by the device itself) */
typedef void (*ev_fn)(UINT id, int timed);

#define EV_MAXCLOCK	0x400000	/* the longest slice */

void	ev_reset_all(void);
void	ev_at(UINT id, SINT32 clocks, ev_fn fn);	/* due `clocks` from now */
void	ev_next(UINT id, SINT32 clocks, ev_fn fn);	/* due `clocks` after its previous deadline (periodic) */
void	ev_cancel(UINT id);
int	ev_armed(UINT id);
SINT32	ev_left(UINT id);				/* clocks until due, -1 when not armed */
void	ev_call(UINT id);				/* call the slot's function now (timed = 0) */

void	ev_slice_done(void);				/* the slice ran out: account it, call what is due */
void	ev_cut_slice(void);				/* end the current slice at the current clock */
void	ev_rescale(UINT32 oldclock, UINT32 newclock);	/* the CPU clock changed: keep the deadlines in real time */

#ifdef __cplusplus
}
#endif

#endif
