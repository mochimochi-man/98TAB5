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

/* Optional host timbre replacement. No guest registers are changed. */
typedef struct {
 UINT32 phase[2];
 SINT32 envelope;                 /* Q12 linear amplitude */
 UINT32 vibrato;
 UINT32 noise, transient;
 SINT32 air;
 UINT8 family, was_key;
} OrchestraVoice;

typedef struct {
 UINT32 pos[2];                 /* playback position (pos[1] spare) */
 UINT32 key_serial;             /* last key-on event observed by the sampler */
 SINT32 envelope;
 UINT16 zone[2];                /* 0xffff: layer unused (harp) */
 UINT8 section, family, was_key, ready; /* family: 0 bowed, 3 plucked */
 UINT8 merge_group;             /* one-based group; zero means not grouped */
 UINT8 layer;                   /* 0 loud recording, 1 soft recording */
} OrchestraSampleVoice;

/* Loudness follower for one replaced part (SSG); see orchestra.c. */
typedef struct {
 UINT64 npow, rpow;
 UINT32 rpeak;
 SINT32 prev[2];
 UINT16 gain;
} OrchestraMatch;

typedef struct {
 UINT32 orchestra_key_serial[3][4]; /* host-only key-on events, outside native state */
 OrchestraSampleVoice samples[3][4];
 UINT16 orchestra_sample_mix;
 UINT8 orchestra_last_mode;
 OrchestraVoice orchestra[3][4]; /* one voice per audible carrier, including CH3 mode */
 UINT16 orchestra_mix;            /* 0..1024, click-free mode transition */
 /* Loudness matching: each replaced part follows the power its native FM
  * channel actually produces, so the game's own FM/SSG and part balance hold. */
 SINT32 orchestra_native[3];      /* latest native channel output */
 SINT64 orchestra_nacc[3];        /* its average over one output frame */
 SINT32 orchestra_prev[2][3];     /* loudness pre-emphasis history */
 UINT64 orchestra_npow[3], orchestra_rpow[3];
 UINT32 orchestra_rpeak[3];       /* decaying pre-gain peak, for headroom */
 UINT16 orchestra_gain[3];        /* Q10; 0 = not yet measured (unity) */
 UINT8 orchestra_tick;
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
void orchestra_open(UINT rate);
void orchestra_sample_open(UINT rate);
void orchestra_sample_frame(FmChip*,SINT32*,SINT32*);
void orchestra_sample_channels(FmChip*,SINT32 l[3],SINT32 r[3],UINT8 owner[3]);
UINT orchestra_family(const FmCh*);
SINT32 orchestra_level(const FmOp*);
int orchestra_enabled(void);
void orchestra_frame(FmChip *f, SINT32 *left, SINT32 *right);
void orchestra_match_feed(OrchestraMatch *m, SINT32 native, SINT32 replaced, SINT32 peak);
void orchestra_match_adjust(OrchestraMatch *m, UINT64 native_power);
void orchestra_ssg_voice(OrchestraSampleVoice *v, UINT32 pitch, SINT32 level, int note, SINT32 *l, SINT32 *r);

/* ---- SSG half (ssg.c) ---- */
typedef struct {
	UINT8	reg[16];
	UINT32	tcnt[3], thalf[3];	/* tone: ticks to the next edge, half period (Q15 ticks of 8 clocks) */
	UINT32	ncnt, nhalf, lfsr;	/* noise */
	UINT32	ecnt, estep;		/* envelope */
	UINT8	tone_hi[3];
	UINT8	env_n, env_lvl, env_run;
	SINT32	x1, y1;			/* output coupling (high-pass; y1 in Q12) */
	/* Orchestra only, host-side: tone-only channels play bowed strings. */
	OrchestraSampleVoice orch[3];
	OrchestraMatch orch_match[3];
	UINT16	orch_tp[3];
	UINT16	orch_age[3];	/* frames since the channel's note began */
	UINT8	orch_active, orch_tick;
} SsgChip;

void	ssg_reset(SsgChip *s);
void	ssg_write(SsgChip *s, UINT reg, REG8 v);
void	ssg_render(void *s, SINT32 *lr, UINT frames);

#endif
