/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * sound.h - the sound side: the mixer that renders the sources in step with the emulated time (sound/mixer.c),
 * the beeper (sound/speaker.c) and the PC-9801-26K board with its YM2203 (sound/board26k.c, the FM half
 * sound/fm.c, the SSG half sound/ssg.c). */
#ifndef PC98_SOUND_H
#define PC98_SOUND_H

#include "compiler.h"
#include <events.h>
#include "pc98.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Pc98Settings;

/* ======================================================================== mixer */

/* a source ADDS `frames` stereo frames (L, R interleaved) to `lr` */
typedef void (*snd_source_fn)(void *ctx, SINT32 *lr, UINT frames);

int	snd_open(UINT rate, UINT ms);		/* 0 = ok; ms = length of the block the host takes */
void	snd_close(void);
void	snd_reset(void);			/* forget the sources and what was rendered */
void	snd_clock_changed(void);		/* after g_mach.cpu_clock changed */
void	snd_add_source(snd_source_fn fn, void *ctx, int dry);	/* dry: into the second buffer, past the host's effects */
void	snd_catch_up(void);			/* render up to the present emulated time */
/* a source's change at the present emulated time: applied by fn(ctx, reg, v) once everything before it has rendered */
typedef void (*snd_apply_fn)(void *ctx, UINT reg, REG8 v);
void	snd_write(snd_apply_fn fn, void *ctx, UINT reg, REG8 v);
void	snd_publish(void);			/* end of a machine frame: the renderer may go up to now */
/* asynchronous rendering (the host's sound task calls snd_service / snd_take on another core) */
void	snd_set_async(int on);
int	snd_is_async(void);
void	snd_service(void);			/* render up to the published clock, applying the queued changes */
extern void (*g_snd_lock)(int lock);		/* host: 1 = keep the sound task out, 0 = let it in (NULL: one thread) */
extern void (*g_snd_kick)(void);		/* host: the change queue is full - let the sound task run */
static inline void snd_lock(int on) { if (g_snd_lock) g_snd_lock(on); }
UINT	snd_rate(void);
UINT	snd_pending(void);			/* frames rendered and not taken yet */
const SINT32 *snd_take(void);			/* the next block (rendered now if need be), or NULL */
const SINT32 *snd_take_dry(void);		/* the dry buffer's block (NULL without one) */
void	snd_release(const SINT32 *block);
/* while a source renders: the emulated CPU clock of its first frame (integer, Q16 fraction) and the CPU clocks per frame (Q16) */
void	snd_window(UINT32 *clock, UINT32 *frac, UINT32 *step);

extern volatile int g_snd_frames, g_snd_dropped, g_snd_takes;	/* diagnostics */

/* ======================================================================== beeper */

void	spk_open(UINT rate);
void	spk_set_volume(UINT vol);		/* 0..3 */
void	spk_reset(void);
void	spk_pit_control(UINT mode);		/* PIT counter 1 got a control word */
void	spk_pit_load(UINT mode, UINT32 count);	/* ... and a count (1..65536 counter clocks) */
void	spk_gate(int on);			/* system port C bit 3 low = the buzzer on */
void	spk_bind(void);

/* ======================================================================== PC-9801-26K */

void	fm_open(UINT rate);
void	fm_set_volume(UINT vol);
void fm_orchestra_enable(int on); /* optional synthesis; original register state stays intact */
void	ssg_open(UINT rate);
void	ssg_set_volume(UINT vol);
void	board_reset(const struct Pc98Settings *cfg, SoundBoard id);
void	board_bind(void);

#ifdef __cplusplus
}
#endif

#endif
