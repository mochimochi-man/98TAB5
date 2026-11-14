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

/* ---- Orchestra and Synthesizer (orchestra.c, orchestra_sampler.c): host-side timbre replacement ----
 * No guest register is changed. These structures are part of the board's state file (board26k.c saves it whole),
 * so their layout stays as it is. */

/* What kind of part an FM patch plays, as orchestra_family() tells them apart. */
enum { ORCH_BOWED, ORCH_BRASS, ORCH_REED, ORCH_PLUCKED, ORCH_PARTS };

/* The carriers of an FM algorithm, as an operator mask in register order. */
static inline UINT orchestra_carriers(UINT algorithm)
{
	return algorithm < 4 ? 8 : algorithm == 4 ? 12 : algorithm == 7 ? 15 : 14;
}

/* Synthesizer: one voice per audible carrier, including CH3 mode. */
typedef struct {
	UINT32	phase[2];
	SINT32	envelope;		/* Q12 linear amplitude */
	UINT32	vibrato;		/* not used any more; kept for the state file */
	UINT32	noise, transient;
	SINT32	air;
	UINT8	family, was_key;
} OrchestraVoice;

/* Orchestra: one recording playing. */
typedef struct {
	UINT32	pos[2];			/* playback position; [1] the recording being faded out */
	UINT32	key_serial;		/* last key-on event observed by the sampler */
	SINT32	envelope;
	UINT16	zone[2];		/* [1] the recording being faded out; 0xffff: none */
	UINT8	section;		/* the player (orchestra_sampler.c) */
	UINT8	family;			/* ORCH_*: the kind of part */
	UINT8	was_key, ready;
	UINT8	merge_group;		/* one-based group; zero means not grouped */
	UINT8	layer;			/* 0 loud recording, 1 soft recording */
	UINT8	fade;			/* what is left of zone[1], in 255ths */
} OrchestraSampleVoice;
/* In the state file as it is: the last field went into the padding. */
typedef char orchestra_sample_voice_size[sizeof(OrchestraSampleVoice) == 28 ? 1 : -1];

/* Loudness follower for one replaced part (SSG); see orchestra.c. */
typedef struct {
	UINT64	npow, rpow;
	UINT32	rpeak;
	SINT32	prev[2];
	UINT16	gain;
} OrchestraMatch;

typedef struct {
	UINT32	orchestra_key_serial[3][4];	/* host-only key-on events, outside native state */
	OrchestraSampleVoice samples[3][4];
	UINT16	orchestra_sample_mix;		/* 0..1024: Synthesizer to Orchestra */
	UINT8	orchestra_last_mode;
	OrchestraVoice orchestra[3][4];
	UINT16	orchestra_mix;			/* 0..1024: native to replaced, click-free mode transition */
	/* Loudness matching: each replaced part follows the power its native FM channel actually produces, so the
	 * game's own FM/SSG and part balance hold. */
	SINT32	orchestra_native[3];		/* each channel's native output: per native sample, then its frame average */
	SINT64	orchestra_nacc[3];		/* that average being summed */
	SINT32	orchestra_prev[2][3];		/* loudness pre-emphasis history */
	UINT64	orchestra_npow[3], orchestra_rpow[3];
	UINT32	orchestra_rpeak[3];		/* decaying pre-gain peak, for headroom */
	UINT16	orchestra_gain[3];		/* Q10; 0 = not yet measured (unity) */
	UINT8	orchestra_tick;
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

/* orchestra.c */
void	orchestra_open(UINT rate);
int	orchestra_enabled(void);			/* 0 native, 1 Synthesizer, 2 Orchestra */
void	orchestra_frame(FmChip *f, SINT32 *left, SINT32 *right);
UINT	orchestra_family(const FmCh *ch);
SINT32	orchestra_level(const FmOp *o);
UINT	orchestra_gain_now(UINT16 *gain, SINT32 peak);
void	orchestra_match_feed(OrchestraMatch *m, SINT32 native, SINT32 replaced, SINT32 peak);
void	orchestra_match_adjust(OrchestraMatch *m, UINT64 native_power);
/* orchestra_sampler.c */
void	orchestra_sample_open(UINT rate);
void	orchestra_sample_channels(FmChip *f, SINT32 l[3], SINT32 r[3], UINT8 owner[3]);
void	orchestra_sample_frame(FmChip *f, SINT32 *left, SINT32 *right);
void	orchestra_ssg_voice(OrchestraSampleVoice *v, UINT32 pitch, SINT32 level, int note, SINT32 *l, SINT32 *r);

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
	UINT16	orch_tp[3];	/* the tone period the voice is playing */
	UINT16	orch_age[3];	/* frames since the channel's note began */
	UINT8	orch_active;	/* a voice is still sounding */
	UINT8	orch_tick;	/* frames to the next loudness adjustment */
} SsgChip;

void	ssg_reset(SsgChip *s);
void	ssg_write(SsgChip *s, UINT reg, REG8 v);
void	ssg_render(void *s, SINT32 *lr, UINT frames);

#endif
