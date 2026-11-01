/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * chips.h - the two halves of the YM2203, as the board (board26k.c) holds them. */
#ifndef PC98_SOUND_CHIPS_H
#define PC98_SOUND_CHIPS_H

#include <compiler.h>

/* ---- FM half (fm.c): 3 channels x 4 operators ---- */
typedef struct {
	UINT32	phase;		/* 20 bits */
	UINT32	inc;		/* per native sample */
	UINT16	att;		/* envelope attenuation, 10 bits */
	UINT8	state;		/* FM_EG_* */
	UINT8	key;
	UINT8	dt, mul, tl, ks, ar, dr, sr, sl, rr, ssg;
	UINT8	ssg_inv;
	UINT8	rate[4];	/* attack, decay, sustain, release rates (0..63) for the current key code */
} FmOp;

typedef struct {
	FmOp	op[4];		/* in register order: slot 1, 3, 2, 4 at 30h+0 / +4 / +8 / +Ch */
	UINT16	fnum;
	UINT8	block;
	UINT8	fhi;		/* A4h latch */
	UINT8	alg, fb;
	SINT32	fbmem[2];	/* the last two outputs of slot 1 */
} FmCh;

typedef struct {
	UINT8	ch3mode;	/* register 27h bits 7-6 */
	UINT8	ch3applied;	/* the mode the frequencies were computed for */
	FmCh	ch[3];
	UINT16	ch3_fnum[3];	/* A8h-AAh / ACh-AEh: slots 1, 2, 3 of channel 3 in the special mode */
	UINT8	ch3_block[3];
	UINT8	ch3_fhi;
	UINT32	egcount;	/* envelope clock ticks */
	UINT32	native;		/* native samples since the last envelope tick */
	SINT32	cur;		/* the evaluated sample being averaged */
	UINT32	cur_left;	/* its remaining weight (Q16 native samples) */
	UINT32	pos;		/* output position (Q16) */
} FmChip;

void	fm_reset(FmChip *f);
void	fm_write(FmChip *f, UINT reg, REG8 v);		/* 30h-B6h */
void	fm_key(FmChip *f, REG8 v);			/* register 28h */
void	fm_csm(FmChip *f);				/* Timer A overflow in CSM mode */
void	fm_render(void *f, SINT32 *lr, UINT frames);

/* ---- SSG half (ssg.c) ---- */
typedef struct {
	UINT8	reg[16];
	UINT32	tcnt[3], thalf[3];	/* tone: ticks to the next edge, half period (Q15 ticks of 8 clocks) */
	UINT32	ncnt, nhalf, lfsr;	/* noise */
	UINT32	ecnt, estep;		/* envelope */
	UINT8	tone_hi[3];
	UINT8	env_n, env_lvl, env_run;
	SINT32	x1, y1;			/* output coupling (high-pass; y1 in Q12) */
} SsgChip;

void	ssg_reset(SsgChip *s);
void	ssg_write(SsgChip *s, UINT reg, REG8 v);
void	ssg_render(void *s, SINT32 *lr, UINT frames);

#endif
