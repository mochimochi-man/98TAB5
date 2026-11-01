/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * ssg.c - the SSG half of the YM2203 (the AY-3-8910 family design): three square-wave tones, one noise source and
 * one envelope generator.  Written from the YM2203 application manual and the AY-3-8910 data sheet.
 *
 *   clock      the board's 3.9936 MHz master / 2; time is counted here in ticks of 8 SSG clocks (249.6 kHz)
 *   tone       00h-05h: 12-bit TP per channel (0 acts as 1); the output toggles every TP ticks: f = clock / (16 TP)
 *   noise      06h: 5-bit NP (0 acts as 1); a 17-bit shift register steps every 2 NP ticks, the new bit 16 is
 *              bit 0 XOR bit 3, the output is bit 0
 *   mixer      07h: bit n = 1 turns channel n's tone off, bit n+3 its noise; the channel's signal is
 *              (tone OR tone off) AND (noise OR noise off), so with both off it is constantly high
 *   level      08h-0Ah: bits 3-0 the level, bit 4 = the envelope's level instead; 16 levels about 3 dB apart, 0 = silent
 *   envelope   0Bh / 0Ch: 16-bit EP (0 acts as 1), a step every 2 EP ticks; 0Dh: shape CONT ATT ALT HOLD, a write
 *              restarts it.  ATT: the first ramp rises.  CONT = 0: after the first ramp the level is 0.  CONT = 1,
 *              HOLD = 1: the level stays where the first ramp ended (inverted if ALT).  Otherwise the ramp repeats,
 *              changing direction every ramp if ALT.
 *   output     a channel's DAC gives its level while the signal is high and 0 while it is low.  The board couples the
 *              sum through a capacitor: a high-pass filter at about 20 Hz.
 * Each output frame is the exact average over its time (the generators walk their edges); the envelope level is
 * likewise averaged over the frame. */
#include <compiler.h>
#include <pc98.h>
#include <sound.h>
#include "chips.h"

#define TQ		15			/* fraction bits of a tick */
#define TICK_HZ		(3993600 / 2 / 8)

/* 3 dB steps: level(i) = 3072 * 2^(-(15 - i) / 2) */
static const UINT16 s_level[16] = {
	0, 24, 34, 48, 68, 96, 136, 192, 272, 384, 543, 768, 1086, 1536, 2172, 3072
};

static UINT32 s_samp;		/* ticks per output frame (Q15) */
static UINT32 s_recip;		/* 2^40 / s_samp */
static SINT32 s_amp8[16];	/* the level's output amplitude, Q8 */
static SINT32 s_hp_coef;	/* Q16 */

void ssg_open(UINT rate)
{
	if (!rate) rate = 22050;
	s_samp = (UINT32)((((UINT64)TICK_HZ << TQ) + rate / 2) / rate);
	s_recip = (UINT32)(((UINT64)1 << 40) / s_samp);
	s_hp_coef = (SINT32)(65536 - (65536ull * 1257 / 10) / rate);
}

void ssg_set_volume(UINT vol)
{
	UINT i;

	for (i = 0; i < 16; i++) s_amp8[i] = (SINT32)(s_level[i] * vol * 8);
}

/* ---- envelope ---- */

static UINT env_level(UINT shape, UINT n)
{
	UINT up = (shape >> 2) & 1;

	if (n >= 16) {
		if (!(shape & 8)) return 0;
		if (shape & 1) return (up ^ ((shape >> 1) & 1)) ? 15 : 0;
		up ^= (shape >> 1) & 1;
	}
	return up ? (n & 15) : (15 - (n & 15));
}

static void env_step(SsgChip *s)
{
	const UINT shape = s->reg[13] & 15;
	UINT n = s->env_n + 1u;

	if (n == 16) {
		if (!(shape & 8) || (shape & 1)) s->env_run = 0;
	}
	else if (n == 32) {
		n = 0;
	}
	s->env_n = (UINT8)n;
	s->env_lvl = (UINT8)env_level(shape, n);
}

static SINT32 share(UINT32 ticks)
{
	return (SINT32)(((UINT64)ticks * s_recip) >> 24);	/* ticks / s_samp in Q16 */
}

/* the envelope's amplitude averaged over the coming frame */
static SINT32 env_amp(SsgChip *s)
{
	SINT64 acc = 0;
	UINT32 w = s_samp;

	if (!s->env_run) return s_amp8[s->env_lvl];
	if (s->ecnt > w) {
		s->ecnt -= w;
		return s_amp8[s->env_lvl];
	}
	for (;;) {
		if (s->ecnt > w) {
			s->ecnt -= w;
			acc += (SINT64)s_amp8[s->env_lvl] * share(w);
			break;
		}
		acc += (SINT64)s_amp8[s->env_lvl] * share(s->ecnt);
		w -= s->ecnt;
		env_step(s);
		if (!s->env_run) {
			acc += (SINT64)s_amp8[s->env_lvl] * share(w);
			break;
		}
		s->ecnt = s->estep;
	}
	return (SINT32)((acc + 32768) >> 16);
}

/* ---- registers ---- */

/* a new period keeps the time already spent in the current one */
static void retime(UINT32 *cnt, UINT32 *per, UINT32 newper)
{
	const UINT32 spent = *per - *cnt;

	if (!*per) {
		*per = *cnt = newper;
		return;
	}
	*per = newper;
	*cnt = newper > spent ? newper - spent : 1;
}

void ssg_write(SsgChip *s, UINT reg, REG8 v)
{
	UINT n;

	reg &= 15;
	s->reg[reg] = (UINT8)v;
	switch (reg) {
	case 0: case 1: case 2: case 3: case 4: case 5:
		n = (s->reg[reg & ~1u] | (s->reg[reg | 1u] << 8)) & 0xfff;
		retime(&s->tcnt[reg >> 1], &s->thalf[reg >> 1], (n ? n : 1) << TQ);
		break;
	case 6:
		n = v & 0x1f;
		retime(&s->ncnt, &s->nhalf, (n ? n : 1) * 2u << TQ);
		break;
	case 11: case 12:
		n = s->reg[11] | (s->reg[12] << 8);
		n = (n ? n : 1) * 2u << TQ;
		if (s->env_run) retime(&s->ecnt, &s->estep, n);
		else s->estep = n;
		break;
	case 13:
		s->env_n = 0;
		s->env_run = 1;
		s->env_lvl = (UINT8)env_level(v & 15, 0);
		s->ecnt = s->estep;
		break;
	default:
		break;
	}
}

void ssg_reset(SsgChip *s)
{
	static const UINT8 init[14] = { 0, 0, 0, 0, 0, 0, 0, 0xbf, 0, 0, 0, 0, 0, 0 };
	UINT i;

	memset(s, 0, sizeof(*s));
	s->lfsr = 0x1ffff;
	for (i = 0; i < 3; i++) s->tone_hi[i] = 1;
	for (i = 0; i < 14; i++) ssg_write(s, i, init[i]);
	s->reg[14] = s->reg[15] = 0xff;
}

/* ---- generation ---- */

/* advance a tone by `len` ticks; returns the ticks it was high */
static UINT32 tone_walk(SsgChip *s, UINT ch, UINT32 len)
{
	UINT32 hi = 0, cnt = s->tcnt[ch];

	while (cnt <= len) {
		if (s->tone_hi[ch]) hi += cnt;
		len -= cnt;
		s->tone_hi[ch] ^= 1;
		cnt = s->thalf[ch];
	}
	if (s->tone_hi[ch]) hi += len;
	s->tcnt[ch] = cnt - len;
	return hi;
}

/* tone_walk without the high time: the counter after len ticks */
static void tone_skip(SsgChip *s, UINT ch, UINT64 len)
{
	const UINT32 cnt = s->tcnt[ch], half = s->thalf[ch];
	UINT64 rest;

	if (cnt > len) {
		s->tcnt[ch] = (UINT32)(cnt - len);
		return;
	}
	rest = len - cnt;
	s->tone_hi[ch] ^= (UINT8)((1u + (UINT32)(rest / half)) & 1u);
	s->tcnt[ch] = half - (UINT32)(rest % half);
}

void ssg_render(void *ctx, SINT32 *lr, UINT frames)
{
	SsgChip *s = (SsgChip *)ctx;
	UINT32 seg_len[12];
	UINT8 seg_hi[12];

	/* All three at the fixed level 0 and the high-pass at rest: the output stays zero.  The tone and noise counters
	 * and the envelope still run, to the same state as below: the counters for the whole block by arithmetic, the
	 * envelope frame by frame while it runs. */
	if (!s->x1 && !s->y1 && !((s->reg[8] | s->reg[9] | s->reg[10]) & 0x1f) && s->thalf[0] && s->thalf[1] &&
	    s->thalf[2] && s->nhalf) {
		const UINT64 total = (UINT64)frames * s_samp;
		UINT64 w = total;
		UINT c, f;

		if (s->env_run) {
			for (f = 0; f < frames && s->env_run; f++) (void)env_amp(s);
		}
		while (s->ncnt <= w) {
			w -= s->ncnt;
			s->lfsr = (s->lfsr >> 1) | (((s->lfsr ^ (s->lfsr >> 3)) & 1) << 16);
			s->ncnt = s->nhalf;
		}
		s->ncnt -= (UINT32)w;
		for (c = 0; c < 3; c++) tone_skip(s, c, total);
		return;
	}
	for (; frames; frames--, lr += 2) {
		const UINT mix = s->reg[7];
		const SINT32 eamp = env_amp(s);
		UINT nseg = 0, c, k;
		UINT32 w = s_samp, noise_hi = 0;
		SINT32 sum = 0, y;

		/* the noise over this frame, as pieces of constant output */
		for (;;) {
			const UINT8 bit = (UINT8)(s->lfsr & 1);
			const UINT32 len = s->ncnt > w ? w : s->ncnt;

			if (nseg < 12) {
				seg_len[nseg] = len;
				seg_hi[nseg++] = bit;
			}
			else {
				seg_len[nseg - 1] += len;
			}
			if (bit) noise_hi += len;
			if (s->ncnt > w) {
				s->ncnt -= w;
				break;
			}
			w -= s->ncnt;
			s->lfsr = (s->lfsr >> 1) | (((s->lfsr ^ (s->lfsr >> 3)) & 1) << 16);
			s->ncnt = s->nhalf;
		}

		for (c = 0; c < 3; c++) {
			const UINT v = s->reg[8 + c];
			const SINT32 amp = (v & 0x10) ? eamp : s_amp8[v & 15];
			const int tone_on = !(mix & (1u << c)), noise_on = !(mix & (8u << c));
			UINT32 hi;

			if (tone_on && noise_on) {
				hi = 0;
				for (k = 0; k < nseg; k++) {
					const UINT32 t = tone_walk(s, c, seg_len[k]);
					if (seg_hi[k]) hi += t;
				}
			}
			else {
				const UINT32 t = tone_walk(s, c, s_samp);
				hi = tone_on ? t : noise_on ? noise_hi : s_samp;
			}
			if (amp) sum += (SINT32)(((SINT64)share(hi) * amp + (1 << 23)) >> 24);
		}
		s->y1 = ((sum - s->x1) << 12) + (SINT32)(((SINT64)s->y1 * s_hp_coef) / 65536);	/* Q12; the decay truncates toward zero, so a silence settles at exactly 0 */
		s->x1 = sum;
		y = s->y1 >> 12;
		lr[0] += y;
		lr[1] += y;
	}
}
