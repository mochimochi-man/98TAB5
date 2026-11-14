/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * fm.c - the FM half of the YM2203: 3 channels of 4 operators.  Written from the YM2203 / YM2608 application manuals
 * (register formats, the F-Number formula, the DT1 table, key code, rates, algorithms).
 *
 *   timing     the 3.9936 MHz master / 72: one native sample per 55466 Hz; the envelope steps every 3 native samples.
 *              The operators are evaluated every 2 native samples and box-averaged to the output rate.
 *   phase      20 bits; per native sample + ((F-Number << block) >> 1 + detune) x MUL (MUL 0 = x 1/2)
 *   key code   block << 2 | N4 N3 (N4 = F11, N3 = F11 & (F10 | F9 | F8) | !F11 & F10 & F9 & F8); KS scales it into the rates
 *   envelope   10 bits of attenuation (0.09375 dB); rate R = 2 x register + (key code >> (3 - KS)), release 4 x RR + 2 + ...;
 *              below R 48 a step every 2^(11 - R / 4) ticks, the step sizes following an 8-tick pattern by R & 3;
 *              attack: att -= (att + 1) x step / 16; decay to SL (15 = the bottom), then sustain rate to silence
 *   SSG-EG     (register 90h bit 3) the decay runs 4 times as fast and repeats at half level, alternating / holding
 *   output     a sine looked up as log-sine + attenuation, then exponent: 14 bits signed; a modulator's output shifts the
 *              phase of the next operator by its value / 2 of the 1024-step cycle; slot 1 feeds itself back by FB
 *   algorithms 0: 1>2>3>4  1: (1+2)>3>4  2: (1+(2>3))>4  3: ((1>2)+3)>4  4: (1>2)+(3>4)  5: 1>(2,3,4)  6: (1>2)+3+4  7: 1+2+3+4
 *   channel 3  in the special mode (27h bits 7-6 not 0) slots 1, 2, 3 take their own F-Numbers (A9h/ADh, AAh/AEh, A8h/ACh);
 *              CSM keys all four slots of channel 3 on and off at every Timer A overflow
 * The registers keep slot 1, 3, 2, 4 at offsets +0, +4, +8, +Ch; op[] is in that register order. */
#pragma GCC optimize ("O3")
#include <compiler.h>
#include <math.h>
#include <sound.h>
#include "chips.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define DRAM_ATTR
#endif

#define NATIVE_HZ	(3993600.0 / 72.0)

enum { EG_OFF, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

static DRAM_ATTR UINT16 s_logsin[256];	/* -log2(sin) of the quarter wave, 256 steps per octave */
static DRAM_ATTR UINT16 s_exp[256];	/* 2^(-i / 256) x 8191 */
static UINT32 s_step;			/* native samples per output frame, Q16 */
static UINT32 s_gain = 1;		/* Q16 */

/* DT1: detune by key code for DT 1..3, in phase increment units: YM2608 application manual table 2-6 (the detune
 * in Hz for each BLOCK / NOTE, at FD = 1..3) divided by its step of 0.053 Hz */
static const UINT8 s_dt[4][32] = {
	{0},
	{0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 8, 8},
	{1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 16, 16, 16},
	{2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 7, 8, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 20, 22, 22, 22, 22}
};

/* envelope step sizes over 8 ticks, by rate group */
static const UINT8 s_eginc[19][8] = {
	{0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1},
	{1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 2, 1, 1, 1, 2}, {1, 2, 1, 2, 1, 2, 1, 2}, {1, 2, 2, 2, 1, 2, 2, 2},
	{2, 2, 2, 2, 2, 2, 2, 2}, {2, 2, 2, 4, 2, 2, 2, 4}, {2, 4, 2, 4, 2, 4, 2, 4}, {2, 4, 4, 4, 2, 4, 4, 4},
	{4, 4, 4, 4, 4, 4, 4, 4}, {4, 4, 4, 8, 4, 4, 4, 8}, {4, 8, 4, 8, 4, 8, 4, 8}, {4, 8, 8, 8, 4, 8, 8, 8},
	{8, 8, 8, 8, 8, 8, 8, 8}, {0, 0, 0, 0, 0, 0, 0, 0}, {0}
};

void fm_open(UINT rate)
{
	UINT i;

	if (!rate) rate = 22050;
	for (i = 0; i < 256; i++) {
		s_logsin[i] = (UINT16)(-log2(sin((i + 0.5) * 3.14159265358979 / 512.0)) * 256.0 + 0.5);
		s_exp[i] = (UINT16)(pow(2.0, -(double)i / 256.0) * 8191.0 + 0.5);
	}
	s_step = (UINT32)(NATIVE_HZ * 65536.0 / rate + 0.5);
	orchestra_open(rate);
}

void fm_set_volume(UINT vol)
{
	s_gain = vol * 5u / 4u * 1024u;
}

/* ---- parameters ---- */

static UINT keycode(UINT fnum, UINT block)
{
	const UINT f11 = (fnum >> 10) & 1, f10 = (fnum >> 9) & 1, f9 = (fnum >> 8) & 1, f8 = (fnum >> 7) & 1;
	const UINT n3 = (f11 & (f10 | f9 | f8)) | ((f11 ^ 1) & f10 & f9 & f8);

	return (block << 2) | (f11 << 1) | n3;
}

static UINT clamp63(UINT r)
{
	return r > 63 ? 63 : r;
}

static void op_update(FmOp *o, UINT fnum, UINT block)
{
	const UINT kc = keycode(fnum, block);
	const UINT ksr = kc >> (3 - o->ks);
	SINT32 base = (SINT32)(((UINT32)fnum << block) >> 1);
	const UINT d = s_dt[o->dt & 3][kc];

	base += (o->dt & 4) ? -(SINT32)d : (SINT32)d;
	base &= 0x1ffff;
	o->inc = o->mul ? (UINT32)base * o->mul : (UINT32)base >> 1;
	o->rate[0] = (UINT8)(o->ar ? clamp63(2u * o->ar + ksr) : 0);
	o->rate[1] = (UINT8)(o->dr ? clamp63(2u * o->dr + ksr) : 0);
	o->rate[2] = (UINT8)(o->sr ? clamp63(2u * o->sr + ksr) : 0);
	o->rate[3] = (UINT8)clamp63(4u * o->rr + 2u + ksr);
}

static void ch_update(FmChip *f, UINT c)
{
	FmCh *ch = &f->ch[c];
	UINT i;

	for (i = 0; i < 4; i++) {
		/* op 0 slot 1, op 1 slot 3, op 2 slot 2 */
		static const UINT8 ch3reg[3] = { 1, 0, 2 };	/* slot 1 A9h, slot 3 A8h, slot 2 AAh */
		if (c == 2 && f->ch3mode && i < 3) {
			const UINT k = ch3reg[i];
			op_update(&ch->op[i], f->ch3_fnum[k], f->ch3_block[k]);
		}
		else {
			op_update(&ch->op[i], ch->fnum, ch->block);
		}
	}
}

static void key(FmOp *o, int on)
{
	if (on && !o->key) {
		o->state = EG_ATTACK;
		o->phase = 0;
		o->ssg_inv = (o->ssg & 0x0c) == 0x0c;
		if (o->rate[0] >= 62) {
			o->att = 0;
			o->state = EG_DECAY;
		}
	}
	else if (!on && o->key && o->state != EG_OFF) {
		if (o->ssg & 8) {
			if (o->ssg_inv) o->att = (UINT16)((0x200 - o->att) & 0x3ff);
			o->ssg_inv = 0;
		}
		o->state = EG_RELEASE;
	}
	o->key = (UINT8)on;
}

void fm_key(FmChip *f, REG8 v)
{
	static const UINT8 slot_op[4] = { 0, 2, 1, 3 };
	const UINT c = v & 3;
	UINT s;

	if (c == 3) return;
	for (s = 0; s < 4; s++) {
		const UINT i = slot_op[s], on = (v >> (4 + s)) & 1;

		if (on && !f->ch[c].op[i].key) f->orchestra_key_serial[c][i]++;	/* a key-on, for the replacement */
		key(&f->ch[c].op[i], on);
	}
}

void fm_csm(FmChip *f)
{
	UINT i;

	for (i = 0; i < 4; i++) {
		key(&f->ch[2].op[i], 0);
		f->orchestra_key_serial[2][i]++;
		key(&f->ch[2].op[i], 1);
	}
}

void fm_write(FmChip *f, UINT reg, REG8 v)
{
	const UINT c = reg & 3;
	FmCh *ch;
	FmOp *o;

	if (c == 3) return;
	ch = &f->ch[c];
	if (reg < 0xa0) {
		o = &ch->op[(reg >> 2) & 3];
		switch (reg & 0xf0) {
		case 0x30: o->dt = (v >> 4) & 7; o->mul = v & 15; break;
		case 0x40: o->tl = v & 0x7f; break;
		case 0x50: o->ks = v >> 6; o->ar = v & 0x1f; break;
		case 0x60: o->dr = v & 0x1f; break;
		case 0x70: o->sr = v & 0x1f; break;
		case 0x80: o->sl = v >> 4; o->rr = v & 15; break;
		default: o->ssg = v & 15; break;
		}
	}
	else switch (reg & 0xfc) {
	case 0xa0:
		ch->fnum = (UINT16)(((ch->fhi & 7) << 8) | v);
		ch->block = (ch->fhi >> 3) & 7;
		break;
	case 0xa4:
		ch->fhi = v & 0x3f;
		return;
	case 0xa8:
		f->ch3_fnum[c] = (UINT16)(((f->ch3_fhi & 7) << 8) | v);
		f->ch3_block[c] = (f->ch3_fhi >> 3) & 7;
		ch_update(f, 2);
		return;
	case 0xac:
		f->ch3_fhi = v & 0x3f;
		return;
	case 0xb0:
		ch->alg = v & 7;
		ch->fb = (v >> 3) & 7;
		return;
	default:
		return;
	}
	ch_update(f, c);
}

void fm_reset(FmChip *f)
{
	UINT c, i;

	memset(f, 0, sizeof(*f));
	for (c = 0; c < 3; c++) {
		for (i = 0; i < 4; i++) {
			f->ch[c].op[i].tl = 0x7f;
			f->ch[c].op[i].att = 0x3ff;
		}
		ch_update(f, c);
	}
}

/* ---- the envelope ---- */

static void eg_op(FmOp *o, UINT32 count)
{
	UINT r, sh, inc;

	switch (o->state) {
	case EG_ATTACK: r = o->rate[0]; break;
	case EG_DECAY: r = o->rate[1]; break;
	case EG_SUSTAIN: r = o->rate[2]; break;
	case EG_RELEASE: r = o->rate[3]; break;
	default: return;
	}
	if (r < 2) return;
	sh = r < 48 ? 11 - (r >> 2) : 0;
	if (count & ((1u << sh) - 1)) return;
	inc = (r >= 60) ? 8 : s_eginc[r < 48 ? (r & 3) : (r - 44)][(count >> sh) & 7];
	if (o->state == EG_ATTACK) {
		if (r >= 62) o->att = 0;
		else o->att = (UINT16)(o->att - (((o->att + 1u) * inc) >> 4));
		if (o->att == 0 || o->att > 0x3ff) {
			o->att = 0;
			o->state = EG_DECAY;
		}
		return;
	}
	if ((o->ssg & 8) && o->state != EG_RELEASE) {
		o->att = (UINT16)(o->att + inc * 4u);
		if (o->att >= 0x200) {
			if (o->ssg & 1) {
				o->att = 0x200;
				if (o->ssg & 2) o->ssg_inv ^= 1;
				o->state = EG_SUSTAIN;
				o->rate[2] = 0;
			}
			else {
				if (o->ssg & 2) o->ssg_inv ^= 1;
				else o->phase = 0;
				o->state = EG_ATTACK;
			}
			return;
		}
	}
	else {
		o->att = (UINT16)(o->att + inc);
	}
	if (o->state == EG_DECAY && o->att >= ((o->sl == 15 ? 31u : o->sl) << 5)) o->state = EG_SUSTAIN;
	if (o->att >= 0x3ff) {
		o->att = 0x3ff;
		if (o->state != EG_DECAY && !(o->ssg & 8)) o->state = EG_OFF;
	}
}

static void eg_tick(FmChip *f)
{
	UINT c, i;

	f->egcount++;
	for (c = 0; c < 3; c++) {
		for (i = 0; i < 4; i++) eg_op(&f->ch[c].op[i], f->egcount);
	}
}

/* ---- the operators ---- */

static inline SINT32 op_out(FmOp *o, SINT32 mod)
{
	UINT att = o->att;
	UINT idx, v;
	SINT32 out;

	if ((o->ssg & 8) && o->ssg_inv && o->state != EG_RELEASE) att = (0x200 - att) & 0x3ff;
	att += (UINT)o->tl << 3;
	idx = ((o->phase >> 10) + (UINT32)mod) & 1023;
	o->phase = (o->phase + 2 * o->inc) & 0xfffff;
	if (att >= 0x3ff || o->state == EG_OFF) return 0;
	v = s_logsin[(idx & 0x100) ? (~idx & 0xff) : (idx & 0xff)] + (att << 2);
	if (v >= 13 * 256) return 0;
	out = s_exp[v & 0xff] >> (v >> 8);
	return (idx & 0x200) ? -out : out;
}

static SINT32 ch_out(FmCh *ch)
{
	FmOp *const s1 = &ch->op[0], *const s3 = &ch->op[1], *const s2 = &ch->op[2], *const s4 = &ch->op[3];
	const SINT32 fb = ch->fb ? (ch->fbmem[0] + ch->fbmem[1]) >> (10 - ch->fb) : 0;
	const SINT32 o1 = op_out(s1, fb);
	SINT32 o2, o3, out;

	ch->fbmem[0] = ch->fbmem[1];
	ch->fbmem[1] = o1;
	switch (ch->alg) {
	case 0: o2 = op_out(s2, o1 >> 1); o3 = op_out(s3, o2 >> 1); out = op_out(s4, o3 >> 1); break;
	case 1: o2 = op_out(s2, 0); o3 = op_out(s3, (o1 + o2) >> 1); out = op_out(s4, o3 >> 1); break;
	case 2: o2 = op_out(s2, 0); o3 = op_out(s3, o2 >> 1); out = op_out(s4, (o1 + o3) >> 1); break;
	case 3: o2 = op_out(s2, o1 >> 1); o3 = op_out(s3, 0); out = op_out(s4, (o2 + o3) >> 1); break;
	case 4: o2 = op_out(s2, o1 >> 1); o3 = op_out(s3, 0); out = o2 + op_out(s4, o3 >> 1); break;
	case 5: out = op_out(s2, o1 >> 1) + op_out(s3, o1 >> 1) + op_out(s4, o1 >> 1); break;
	case 6: out = op_out(s2, o1 >> 1) + op_out(s3, 0) + op_out(s4, 0); break;
	default: out = o1 + op_out(s2, 0) + op_out(s3, 0) + op_out(s4, 0); break;
	}
	return out;
}

static int ch_silent(const FmCh *ch)
{
	UINT i;

	for (i = 0; i < 4; i++) {
		if (ch->op[i].state != EG_OFF) return 0;
	}
	return 1;
}

static SINT32 evaluate(FmChip *f)
{
	SINT32 sum = 0;
	UINT c;

	f->native += 2;
	while (f->native >= 3) {
		f->native -= 3;
		eg_tick(f);
	}
	for (c = 0; c < 3; c++) {
		const SINT32 out = ch_silent(&f->ch[c]) ? 0 : ch_out(&f->ch[c]);
		f->orchestra_native[c] = out;
		sum += out;
	}
	return sum;
}

void fm_render(void *ctx, SINT32 *lr, UINT frames)
{
	FmChip *f = (FmChip *)ctx;

	if (f->ch3applied != f->ch3mode) {
		f->ch3applied = f->ch3mode;
		ch_update(f, 2);
	}
	/* Every operator off and the held sample zero: the output stays zero, so only the native sample position and the
	 * envelope clock move on - by arithmetic, to the same state the loop below would reach. */
	if (!orchestra_enabled() && !f->orchestra_mix && !f->cur && ch_silent(&f->ch[0]) && ch_silent(&f->ch[1]) && ch_silent(&f->ch[2])) {
		const UINT64 total = (UINT64)frames * s_step;

		if (total <= f->cur_left) {
			f->cur_left -= (UINT32)total;
		}
		else {
			const UINT64 rest = total - f->cur_left;
			const UINT64 evals = (rest + (2u << 16) - 1) / (2u << 16);	/* evaluate() calls, each opening a native sample */
			const UINT64 native = f->native + 2 * evals;

			f->egcount += (UINT32)(native / 3);
			f->native = (UINT)(native % 3);
			f->cur_left = (UINT32)(evals * (2u << 16) - rest);
		}
		return;
	}
	for (; frames; frames--, lr += 2) {
		const int orch = orchestra_enabled() || f->orchestra_mix;	/* Synthesizer / Orchestra playing, or fading out */
		UINT32 need = s_step;
		SINT64 acc = 0;
		SINT32 v;

		while (need) {
			UINT32 take;
			if (!f->cur_left) {
				f->cur = evaluate(f);
				f->cur_left = 2u << 16;
			}
			take = need < f->cur_left ? need : f->cur_left;
			acc += (SINT64)f->cur * take;
			if (orch) {	/* each channel's own share, for the replacement's loudness matching */
				f->orchestra_nacc[0] += (SINT64)f->orchestra_native[0] * take;
				f->orchestra_nacc[1] += (SINT64)f->orchestra_native[1] * take;
				f->orchestra_nacc[2] += (SINT64)f->orchestra_native[2] * take;
			}
			need -= take;
			f->cur_left -= take;
		}
		v = (SINT32)((acc / (SINT64)s_step * (SINT64)s_gain) >> 16);
		if (orch) {
			SINT32 ol, oright;
			UINT c;

			for (c = 0; c < 3; c++) {
				f->orchestra_native[c] = (SINT32)(f->orchestra_nacc[c] / (SINT64)s_step);
				f->orchestra_nacc[c] = 0;
			}
			orchestra_frame(f, &ol, &oright);
			ol = (SINT32)(((SINT64)ol * s_gain) >> 16);
			oright = (SINT32)(((SINT64)oright * s_gain) >> 16);
			lr[0] += (SINT32)(((SINT64)v * (1024 - f->orchestra_mix) + (SINT64)ol * f->orchestra_mix) / 1024);
			lr[1] += (SINT32)(((SINT64)v * (1024 - f->orchestra_mix) + (SINT64)oright * f->orchestra_mix) / 1024);
		}
		else {
			lr[0] += v;
			lr[1] += v;
		}
	}
}
