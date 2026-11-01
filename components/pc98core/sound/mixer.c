/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * mixer.c - renders the sound sources in step with the emulated time and hands blocks to the host.
 *
 * The render position is an emulated CPU clock (with a 16-bit fraction).  snd_catch_up() renders the whole frames
 * between it and the present; a source that is about to change (a register write) calls it first, so the old
 * settings sound for the time they were in force.  The host takes fixed blocks: snd_take() renders whatever of the
 * block is missing even if the emulated time has not got there yet (the position then runs ahead of the machine,
 * by at most MAX_LEAD blocks; the host takes blocks at the pace of the emulated time, so this evens out).
 *
 * Two buffers of the same length are always filled together: the main one and the dry one (sources added with
 * dry = 1, the beeper: the host applies its effects to the main one only).  Frames beyond the buffer's room are
 * counted as dropped.
 *
 * Asynchronous mode (snd_set_async, the host renders on another core): a source's change does not render then and
 * there; snd_write() queues it with the emulated clock it happened at, and snd_service() - on the host's sound task -
 * renders up to each queued change, applies it, and goes on up to the clock the machine published at the end of its
 * last frame (snd_publish).  The order and the frame each change takes effect at are those of the synchronous mode.
 * The host excludes its sound task (g_snd_lock) while the machine resets the sound side or changes the clock. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <sound.h>

#define MAX_SOURCES	8
#define RESERVE_MS	40
#define MAX_LEAD	2

typedef struct {
	snd_source_fn	fn;
	void		*ctx;
	int		dry;
} Source;

static struct {
	void	*raw[2];
	SINT32	*buf[2];	/* main, dry */
	UINT	rate, block, cap, filled;
	UINT32	pos, frac;	/* render position: CPU clock + Q16 fraction */
	UINT32	step;		/* CPU clocks per frame, Q16 */
	UINT	nsrc;
	Source	src[MAX_SOURCES];
	int	busy;
	UINT32	win_pos, win_frac;
} m;

volatile int g_snd_frames, g_snd_dropped, g_snd_takes;

/* ---- asynchronous mode ---- */

#define QLEN	4096

typedef struct {
	UINT32		clk;
	snd_apply_fn	fn;
	void		*ctx;
	UINT16		reg;
	UINT8		v;
} Change;

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
static EXT_RAM_BSS_ATTR Change s_q[QLEN];
#else
static Change s_q[QLEN];
#endif
static volatile UINT s_qw, s_qr;		/* written by the machine / by the renderer */
static volatile int s_async;
static volatile UINT32 s_pub;			/* the emulated clock the machine has got to */
void (*g_snd_lock)(int lock);
void (*g_snd_kick)(void);

static UINT32 now(void)
{
	return (UINT32)(g_cpu.clocks_done + g_cpu.slice_len - g_cpu.slice_left);
}

/* the clock the renderer works up to */
static UINT32 present(void)
{
	return s_async ? s_pub : now();
}

static void advance(UINT frames)
{
	const UINT64 f = (UINT64)m.frac + (UINT64)m.step * frames;

	m.pos += (UINT32)(f >> 16);
	m.frac = (UINT32)(f & 0xffff);
}

#ifdef SND_PROF
#include <time.h>
unsigned long long g_snd_prof_ns[8], g_snd_prof_frames;
#endif

static void render(UINT frames)
{
	const UINT room = m.cap - m.filled;
	const UINT n = frames < room ? frames : room;
	SINT32 *main, *dry;
	UINT i;

	if (n < frames) g_snd_dropped += (int)(frames - n);
	if (n) {
		main = m.buf[0] + m.filled * 2;
		dry = m.buf[1] ? m.buf[1] + m.filled * 2 : main;
		memset(main, 0, n * 2 * sizeof(SINT32));
		if (dry != main) memset(dry, 0, n * 2 * sizeof(SINT32));
		m.win_pos = m.pos;
		m.win_frac = m.frac;
#ifdef SND_PROF
		for (i = 0; i < m.nsrc; i++) {
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			m.src[i].fn(m.src[i].ctx, m.src[i].dry ? dry : main, n);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			g_snd_prof_ns[i] += (unsigned long long)((t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec));
		}
		g_snd_prof_frames += n;
#else
		for (i = 0; i < m.nsrc; i++) m.src[i].fn(m.src[i].ctx, m.src[i].dry ? dry : main, n);
#endif
		m.filled += n;
		g_snd_frames += (int)n;
	}
	advance(frames);
}

void snd_window(UINT32 *clock, UINT32 *frac, UINT32 *step)
{
	*clock = m.win_pos;
	*frac = m.win_frac;
	*step = m.step;
}

void snd_clock_changed(void)
{
	if (!m.rate) return;
	m.step = (UINT32)(((UINT64)g_mach.cpu_clock << 16) / m.rate);
	m.pos = now();
	m.frac = 0;
	s_pub = m.pos;
}

int snd_open(UINT rate, UINT ms)
{
	UINT i;

	memset(&m, 0, sizeof(m));
	if (!rate || !ms) return -1;
	m.rate = rate;
	m.block = rate * ms / 1000;
	m.cap = m.block + rate * RESERVE_MS / 1000;
	for (i = 0; i < 2; i++) {
		m.raw[i] = malloc(m.cap * 2 * sizeof(SINT32) + 16);
		m.buf[i] = m.raw[i] ? (SINT32 *)(((uintptr_t)m.raw[i] + 15) & ~(uintptr_t)15) : NULL;
	}
	if (!m.buf[0]) {
		snd_close();
		return -1;
	}
	snd_clock_changed();
	return 0;
}

void snd_close(void)
{
	free(m.raw[0]);
	free(m.raw[1]);
	memset(&m, 0, sizeof(m));
}

void snd_reset(void)
{
	m.nsrc = 0;
	m.filled = 0;
	m.pos = now();
	m.frac = 0;
	s_pub = m.pos;
	s_qr = s_qw;
}

void snd_add_source(snd_source_fn fn, void *ctx, int dry)
{
	if (!m.buf[0] || !fn || m.nsrc >= MAX_SOURCES) return;
	m.src[m.nsrc].fn = fn;
	m.src[m.nsrc].ctx = ctx;
	m.src[m.nsrc].dry = dry;
	m.nsrc++;
}

/* render the whole frames up to the clock t */
static void render_to(UINT32 t)
{
	SINT32 ahead;
	UINT64 span;

	if (!m.buf[0] || m.busy) return;
	ahead = (SINT32)(m.pos - t);
	if (ahead >= 0) return;
	span = ((UINT64)(UINT32)(-ahead) << 16) - m.frac;
	if (span < m.step) return;
	m.busy = 1;
	render((UINT)(span / m.step));
	m.busy = 0;
}

void snd_catch_up(void)
{
	if (s_async) {
		snd_service();
		return;
	}
	render_to(now());
}

void snd_write(snd_apply_fn fn, void *ctx, UINT reg, REG8 v)
{
	Change *c;
	UINT w;

	if (!s_async) {
		render_to(now());
		fn(ctx, reg, v);
		return;
	}
	w = s_qw;
	while ((w + 1) % QLEN == __atomic_load_n(&s_qr, __ATOMIC_ACQUIRE)) {	/* full: the renderer must catch up first */
		if (!g_snd_kick) return;
		g_snd_kick();
	}
	c = &s_q[w];
	c->clk = now();
	c->fn = fn;
	c->ctx = ctx;
	c->reg = (UINT16)reg;
	c->v = (UINT8)v;
	__atomic_store_n(&s_qw, (w + 1) % QLEN, __ATOMIC_RELEASE);
}

void snd_publish(void)
{
	if (s_async) __atomic_store_n(&s_pub, now(), __ATOMIC_RELEASE);
	else render_to(now());
}

void snd_service(void)
{
	UINT r = s_qr;

	while (r != __atomic_load_n(&s_qw, __ATOMIC_ACQUIRE)) {
		const Change *c = &s_q[r];

		render_to(c->clk);
		c->fn(c->ctx, c->reg, (REG8)c->v);
		r = (r + 1) % QLEN;
		__atomic_store_n(&s_qr, r, __ATOMIC_RELEASE);
	}
	render_to(__atomic_load_n(&s_pub, __ATOMIC_ACQUIRE));
}

void snd_set_async(int on)
{
	s_async = on;
	s_pub = now();
	s_qr = s_qw;
}

int snd_is_async(void)
{
	return s_async;
}

UINT snd_rate(void)
{
	return m.rate;
}

UINT snd_pending(void)
{
	return m.filled;
}

const SINT32 *snd_take(void)
{
	SINT32 lead;
	const UINT32 maxlead = (UINT32)(((UINT64)m.step * m.block * MAX_LEAD) >> 16);

	if (!m.buf[0] || m.busy) return NULL;
	snd_catch_up();
	m.busy = 1;
	g_snd_takes++;
	if (m.filled < m.block) render(m.block - m.filled);
	lead = (SINT32)(m.pos - present());
	if (lead > (SINT32)maxlead) {
		m.pos = present() + maxlead;
		m.frac = 0;
	}
	return m.buf[0];
}

const SINT32 *snd_take_dry(void)
{
	return m.buf[1];
}

void snd_release(const SINT32 *block)
{
	UINT left;

	if (!block || !m.busy) return;
	left = m.filled > m.block ? m.filled - m.block : 0;
	if (left) {
		memmove(m.buf[0], m.buf[0] + m.block * 2, left * 2 * sizeof(SINT32));
		if (m.buf[1]) memmove(m.buf[1], m.buf[1] + m.block * 2, left * 2 * sizeof(SINT32));
	}
	m.filled = left;
	m.busy = 0;
}
