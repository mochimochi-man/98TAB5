/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * pit.c - the i8253 interval timer: counters 0-2 at 71h / 73h / 75h, control word at 77h (the odd ports up to 7Fh
 * decode to the same four registers; 3FD9h / 3FDBh / 3FDDh / 3FDFh are a second address).
 *
 *   counter 0  system timer; OUT0 drives IRQ0 (a request on the rising edge)
 *   counter 1  beeper tone; the waveform is produced by sound/speaker.c from the count and the mode
 *   counter 2  RS-232C baud clock; no serial port is fitted, so the counter only counts
 * All gates are high.  Input clock: 2.4576 MHz (1.9968 MHz on the 8 MHz machines); one counter clock is
 * g_mach.clock_mul CPU clocks.
 *
 * Written from the i8253 data sheet:
 *   control word  SC1 SC0 RW1 RW0 M2 M1 M0 BCD.  RW = 0 latches the count; SC = 3 is not a command on the 8253.
 *                 A control word puts the counter into the new mode and waits for a count.
 *   count write   LSB, MSB, or LSB then MSB.  Count 0 = 65536 (binary) / 10000 (BCD).
 *   count read    the latched count if one is latched (it stays until it has been read completely, a second
 *                 latch command meanwhile is ignored), otherwise the live counter.
 *   modes         0 interrupt on terminal count, 1 retriggerable one-shot (needs a gate edge: never starts here),
 *                 2 rate generator, 3 square wave, 4 software strobe (OUT pulses one clock after the terminal
 *                 count), 5 hardware strobe (gate: never starts); 6 / 7 are 2 / 3.
 *   After the terminal count of a non-periodic mode the counter wraps and keeps counting down.
 *
 * Time: each counter keeps the CPU clock of its last load and its period, and the live count is derived from the
 * time elapsed since.  Counter 0's rising edges are scheduled on EV_TIMER0; counter 1 is told to the speaker.
 * A period shorter than MIN_EVENT counter clocks is scheduled as a full-scale period (the event queue would
 * otherwise be flooded by a nonsense count; no software depends on such rates).
 */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <io.h>
#include <devices.h>
#include <sound.h>

#define MIN_EVENT	16

typedef struct {
	UINT8	mode;		/* 0-5 */
	UINT8	rw;		/* 1 LSB, 2 MSB, 3 LSB then MSB */
	UINT8	bcd;
	UINT8	wr_hi;		/* the next count byte written is the MSB */
	UINT8	rd_hi;		/* the next byte read is the MSB */
	UINT8	latched;	/* bytes of `latch` left to read */
	UINT8	waiting;	/* a control word was written and no count yet */
	UINT8	edge;		/* counter 0: the current period ends in a rising edge */
	UINT16	count;		/* the count as written */
	UINT16	latch;
	UINT32	t0;		/* CPU clock of the load */
	UINT32	period;		/* counter clocks of the load */
} Counter;

static Counter s_ctr[3];

static UINT32 now(void)
{
	return (UINT32)(g_cpu.clocks_done + g_cpu.slice_len - g_cpu.slice_left);
}

static UINT32 mul(void)
{
	return g_mach.clock_mul ? g_mach.clock_mul : 1;
}

static UINT32 full_scale(const Counter *c)
{
	return c->bcd ? 10000 : 65536;
}

static UINT32 load_value(const Counter *c)
{
	UINT32 v = c->count;

	if (c->bcd) {
		v = (v & 15) + ((v >> 4) & 15) * 10 + ((v >> 8) & 15) * 100 + ((v >> 12) & 15) * 1000;
	}
	return v ? v : full_scale(c);
}

static UINT16 to_bus(const Counter *c, UINT32 n)
{
	if (c->bcd) {
		n %= 10000;
		return (UINT16)((n % 10) | ((n / 10 % 10) << 4) | ((n / 100 % 10) << 8) | ((n / 1000) << 12));
	}
	return (UINT16)n;
}

static int periodic(const Counter *c)
{
	return c->mode == 2 || c->mode == 3;
}

static void load(Counter *c)
{
	c->t0 = now();
	c->period = load_value(c);
}

/* the live count */
static UINT32 live(const Counter *c)
{
	const UINT32 dt = (now() - c->t0) / mul();

	if (c->waiting || !c->period) return c->count;
	if (periodic(c)) return c->period - dt % c->period;
	if (dt <= c->period) return c->period - dt;
	return full_scale(c) - (dt - c->period) % full_scale(c);
}

static SINT32 clocks(UINT32 n)
{
	if (n < MIN_EVENT) n = 65536;
	return (SINT32)(n * mul());
}

/* ---- counter 0 ---- */

static void timer0_event(UINT id, int timed)
{
	Counter *c = &s_ctr[0];

	(void)id;
	if (!timed) return;
	if (c->edge) {
		pic_raise(0);
		c->edge = 0;
	}
	if (periodic(c)) {
		c->t0 = now();
		c->edge = 1;
		ev_next(EV_TIMER0, clocks(c->period), timer0_event);
	}
	else {
		ev_next(EV_TIMER0, clocks(full_scale(c)), timer0_event);
	}
}

static void timer0_start(void)
{
	Counter *c = &s_ctr[0];

	load(c);
	c->edge = 1;
	ev_at(EV_TIMER0, clocks(c->period + (c->mode == 4 ? 1 : 0)), timer0_event);
}

/* ---- counter 1: the speaker follows its output (sound/speaker.c) ---- */

static void speaker_start(void)
{
	Counter *c = &s_ctr[1];

	load(c);
	spk_pit_load(c->mode, c->period);
}

/* ---- ports ---- */

/* returns 1 when the write completed a count */
static int count_w(Counter *c, REG8 v)
{
	switch (c->rw) {
	case 1:
		c->count = v;
		break;
	case 2:
		c->count = (UINT16)(v << 8);
		break;
	default:
		if (!c->wr_hi) {
			c->count = (UINT16)((c->count & 0xff00) | v);
			c->wr_hi = 1;
			return 0;
		}
		c->count = (UINT16)((c->count & 0x00ff) | (v << 8));
		c->wr_hi = 0;
		break;
	}
	c->waiting = 0;
	return 1;
}

static void counter_w(UINT port, REG8 v)
{
	const UINT n = (port >> 1) & 3;
	Counter *c = &s_ctr[n];

	if (!count_w(c, v)) return;
	if (n == 0) pic_lower(0);
	if (c->mode == 1 || c->mode == 5) {		/* gate-triggered: waits for an edge that never comes */
		load(c);
		if (n == 1) spk_pit_load(c->mode, c->period);
		return;
	}
	switch (n) {
	case 0:
		timer0_start();
		break;
	case 1:
		speaker_start();
		break;
	default:
		load(c);
		break;
	}
}

static void control_w(UINT port, REG8 v)
{
	const UINT n = (v >> 6) & 3;
	Counter *c;

	(void)port;
	if (n == 3) return;
	c = &s_ctr[n];
	if (!(v & 0x30)) {				/* counter latch */
		if (!c->latched) {
			c->latch = to_bus(c, live(c));
			c->latched = (c->rw == 3) ? 2 : 1;
			c->rd_hi = 0;
		}
		return;
	}
	c->rw = (v >> 4) & 3;
	c->mode = (v >> 1) & 7;
	if (c->mode >= 6) c->mode -= 4;
	c->bcd = v & 1;
	c->wr_hi = 0;
	c->rd_hi = 0;
	c->latched = 0;
	c->waiting = 1;
	if (n == 0) {
		pic_lower(0);
		c->edge = 1;
	}
	else if (n == 1) {
		spk_pit_control(c->mode);
	}
}

static REG8 counter_r(UINT port)
{
	Counter *c = &s_ctr[(port >> 1) & 3];
	UINT16 w;
	REG8 r;

	w = c->latched ? c->latch : to_bus(c, live(c));
	if (c->rw == 1) r = (REG8)w;
	else if (c->rw == 2) r = (REG8)(w >> 8);
	else {
		r = c->rd_hi ? (REG8)(w >> 8) : (REG8)w;
		c->rd_hi ^= 1;
	}
	if (c->latched) c->latched--;
	return r;
}

void pit_reset(const Pc98Settings *cfg)
{
	const UINT16 tone = (g_mach.clk8mhz) ? 998 : 1229;	/* 2 kHz */

	(void)cfg;
	memset(s_ctr, 0, sizeof(s_ctr));
	s_ctr[0].rw = 1; s_ctr[0].mode = 3;
	s_ctr[1].rw = 3; s_ctr[1].mode = 3; s_ctr[1].count = tone;
	s_ctr[2].rw = 3; s_ctr[2].mode = 3;
	load(&s_ctr[1]);
	s_ctr[0].edge = 1;
	ev_at(EV_TIMER0, clocks(65536), timer0_event);
	spk_pit_load(3, tone);
}

void pit_bind(void)
{
	static const io_out_fn out[4] = { counter_w, counter_w, counter_w, control_w };
	static const io_in_fn in[4] = { counter_r, counter_r, counter_r, NULL };
	UINT i;

	io_sys(0x71, 0xcf1, out, in, 4);
	for (i = 0; i < 4; i++) io_port(0x3fd9 + i * 2, out[i], in[i]);
}

/* ---- state save / load (state.c) ---- */
#include <stateint.h>
void pit_state(SsCtx *c)
{
	SS_VAR(c, "PIT ", s_ctr);
}
void pit_fns(const void **t)
{
	t[SF_PIT_TIMER0] = (const void *)timer0_event;
}
