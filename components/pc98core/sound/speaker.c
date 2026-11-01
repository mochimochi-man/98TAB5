/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * speaker.c - the beeper: the output of PIT counter 1, switched by the buzzer bit of the system port.
 *
 * The speaker signal is OUT1 AND (port C bit 3 = 0).  OUT1 follows the i8253's waveform for the counter's mode,
 * timed from the moment the count was loaded (N = the count in counter clocks):
 *   after a control word  mode 0: low until a count is loaded; the other modes: high
 *   mode 0   low for N clocks, then high
 *   mode 1/5 high (they need a gate edge, and the gate is wired high)
 *   mode 2   low for one clock at the end of every period of N
 *   mode 3   high for the first ceil(N / 2) clocks of every period of N, low for the rest
 *   mode 4   low for one clock N clocks after the load
 * Each output frame is the exact share of its time during which the signal was high (so pulse-width tricks - a
 * program rewriting the count in mode 0 or 4 to play samples - come out as their average level), scaled by the
 * volume.  The speaker's coupling is modelled by a high-pass filter at about 20 Hz, which removes the DC part.
 *
 * Changes are logged with their emulated time as they happen and applied by the renderer at that time. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <sound.h>

#define LOG_SIZE	512
#define AMP		2048		/* per volume step: the signal swings 0..AMP*vol */

enum { E_CONTROL, E_LOAD, E_GATE };

typedef struct {
	UINT32	t;
	UINT8	kind;
	UINT8	mode;
	UINT32	n;
} Change;

/* written by the machine (s_wr), read by the renderer (s_rd), which may be another core: the count is s_wr - s_rd */
static Change s_log[LOG_SIZE];
static volatile UINT s_wr, s_rd;
static UINT s_vol = 2;
volatile UINT32 g_spk_lost, g_spk_loads, g_spk_ctls, g_spk_same;	/* diagnostics: dropped changes, count loads, control words, loads of the running count */
static SINT32 s_hp_coef;	/* Q16 */

/* what the renderer is at */
static struct {
	UINT8	mode, waiting, gate, settled, pending;
	UINT32	n;		/* counter clocks */
	UINT32	t0;		/* CPU clock of the load */
	UINT32	pn, pt;		/* pending: a count written while modes 2 / 3 run takes over at the end of the period */
	SINT32	x1, y1;		/* high-pass filter memory (y1 in Q12) */
} r;

static UINT32 now(void)
{
	return (UINT32)(g_cpu.clocks_done + g_cpu.slice_len - g_cpu.slice_left);
}

static void log_change(UINT kind, UINT mode, UINT32 n)
{
	Change *c;

	if (!snd_rate()) return;
	if (!snd_is_async() && s_wr - s_rd >= LOG_SIZE * 3 / 4) snd_catch_up();
	if (s_wr - __atomic_load_n(&s_rd, __ATOMIC_ACQUIRE) >= LOG_SIZE) { g_spk_lost++; return; }
	c = &s_log[s_wr % LOG_SIZE];
	c->t = now();
	c->kind = (UINT8)kind;
	c->mode = (UINT8)mode;
	c->n = n;
	__atomic_store_n(&s_wr, s_wr + 1, __ATOMIC_RELEASE);
}

void spk_pit_control(UINT mode) { log_change(E_CONTROL, mode, 0); }
void spk_pit_load(UINT mode, UINT32 count) { log_change(E_LOAD, mode, count); }
void spk_gate(int on) { log_change(E_GATE, on ? 1 : 0, 0); }

static void apply(const Change *c)
{
	switch (c->kind) {
	case E_CONTROL:
		g_spk_ctls++;
		r.pending = 0;
		r.mode = c->mode;
		r.waiting = 1;
		break;
	case E_LOAD:
		g_spk_loads++;
		if (!r.waiting && r.mode == c->mode && r.n == (c->n ? c->n : 1)) g_spk_same++;
		if (!r.waiting && (r.mode == 2 || r.mode == 3) && c->mode == r.mode) {
			/* the i8253 goes on with the running period and reloads at its end: restarting here would cut the wave */
			const UINT32 per = r.n * g_mach.clock_mul;
			if (!r.pending) r.pt = r.t0 + ((UINT32)(c->t - r.t0) / per + 1) * per;
			r.pn = c->n ? c->n : 1;
			r.pending = 1;
			break;
		}
		r.pending = 0;
		r.mode = c->mode;
		r.n = c->n ? c->n : 1;
		r.t0 = c->t;
		r.waiting = 0;
		r.settled = 0;
		break;
	default:
		r.gate = c->mode;
		break;
	}
}

/* time OUT1 was high during [0, tau) after the load; everything in Q16 CPU clocks */
static UINT64 high_until(UINT64 tau)
{
	const UINT64 per = (UINT64)r.n * g_mach.clock_mul << 16;
	const UINT64 one = (UINT64)g_mach.clock_mul << 16;
	UINT64 k, rem, hi;

	switch (r.mode) {
	case 0:
		return tau > per ? tau - per : 0;
	case 2:
		k = tau / per;
		rem = tau - k * per;
		hi = per - one;
		return k * hi + (rem < hi ? rem : hi);
	case 3:
		hi = (UINT64)((r.n + 1) / 2) * g_mach.clock_mul << 16;
		k = tau / per;
		rem = tau - k * per;
		return k * hi + (rem < hi ? rem : hi);
	case 4:
		if (tau <= per) return tau;
		if (tau <= per + one) return per;
		return tau - one;
	default:
		return tau;
	}
}

/* high time during [a, b), positions in Q16 CPU clocks from the window's base clock */
static UINT64 high_between(UINT32 base, UINT64 a, UINT64 b)
{
	UINT64 off;

	if (!r.gate || b <= a) return 0;
	if (r.waiting) return r.mode == 0 ? 0 : b - a;
	if (r.mode == 1 || r.mode == 5 || r.settled) return b - a;
	off = (UINT64)(UINT32)(base - r.t0) << 16;
	if ((r.mode == 0 || r.mode == 4) && a + off > ((UINT64)(r.n + 2) * g_mach.clock_mul << 16)) {
		r.settled = 1;
		return b - a;
	}
	return high_until(b + off) - high_until(a + off);
}

static void render(void *ctx, SINT32 *lr, UINT frames)
{
	UINT32 base, frac, step;
	UINT64 a;
	const SINT32 amp = (SINT32)(AMP * s_vol) * (SINT32)g_settings.volume_all / 100;

	(void)ctx;
	/* No change pending and OUT1 held at one level that the high-pass has already settled on: every frame would add
	 * zero and leave the filter as it is. */
	if (__atomic_load_n(&s_wr, __ATOMIC_ACQUIRE) == s_rd && !r.pending) {
		int level = -1;

		if (!r.gate) level = 0;
		else if (r.waiting) level = r.mode == 0 ? 0 : 1;
		else if (r.mode == 1 || r.mode == 5 || r.settled) level = 1;
		if (level >= 0 && !r.y1 && r.x1 == (level ? amp : 0)) return;
	}
	snd_window(&base, &frac, &step);
	a = frac;
	for (; frames; frames--, lr += 2) {
		const UINT64 b = a + step;
		UINT64 hi = 0, p = a;
		SINT32 x, y;

		for (;;) {
			const int more = __atomic_load_n(&s_wr, __ATOMIC_ACQUIRE) != s_rd;
			const Change *c = more ? &s_log[s_rd % LOG_SIZE] : NULL;
			const SINT32 d = c ? (SINT32)(c->t - base) : 0;
			const UINT64 at = d <= 0 ? 0 : (UINT64)d << 16;
			const SINT32 dp = r.pending ? (SINT32)(r.pt - base) : 0;
			const UINT64 pat = dp <= 0 ? 0 : (UINT64)dp << 16;

			if (r.pending && pat < b && (!c || pat <= at)) {	/* the period ends: the written count takes over */
				if (pat > p) {
					hi += high_between(base, p, pat);
					p = pat;
				}
				r.n = r.pn;
				r.t0 = r.pt;
				r.pending = 0;
				continue;
			}
			if (!c || at >= b) break;
			if (at > p) {
				hi += high_between(base, p, at);
				p = at;
			}
			apply(c);
			__atomic_store_n(&s_rd, s_rd + 1, __ATOMIC_RELEASE);
		}
		hi += high_between(base, p, b);
		x = (SINT32)((hi * (UINT64)amp) / step);
		r.y1 = ((x - r.x1) << 12) + (SINT32)(((SINT64)r.y1 * s_hp_coef) / 65536);	/* Q12; the decay truncates toward zero, so a silence settles at exactly 0 */
		r.x1 = x;
		y = r.y1 >> 12;
		lr[0] += y;
		lr[1] += y;
		a = b;
	}
}

void spk_open(UINT rate)
{
	/* one-pole high-pass at 20 Hz: coefficient = 1 - 2 pi fc / rate */
	s_hp_coef = rate ? (SINT32)(65536 - (65536ull * 1257 / 10) / rate) : 0;
}

void spk_set_volume(UINT vol)
{
	s_vol = vol & 3;
}

void spk_reset(void)
{
	s_wr = 0;
	s_rd = 0;
	memset(&r, 0, sizeof(r));
	r.mode = 3;
	r.waiting = 1;
}

void spk_bind(void)
{
	snd_add_source(render, NULL, 1);
}
