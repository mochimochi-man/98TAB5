/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * io.c - the I/O port space (see io.h).
 *
 * Every high address byte points at a page of 256 handler pairs.  Two pages are shared: the main-board page, used by
 * the high bytes whose A10 / A11 bits are 0 (the internal devices answer there), and the expansion page, used by all
 * the others.  A port attached with its full address (io_port) or a board port (io_board) gives the high bytes it
 * touches a private copy of their page first, so the shared pages stay as they were for every other high byte. */
#include <compiler.h>
#include <cpulink.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

typedef struct {
	io_out_fn	out[256];
	io_in_fn	in[256];
	UINT8		board;		/* the page is a private copy */
} Page;

#define MAX_PAGES	(2 + 256)
#define MAX_WIDE	8

static Page	*s_pages[MAX_PAGES];
static UINT	s_npages;
static Page	*s_map[256];		/* high byte -> page */
static Page	*s_sys, *s_ext;		/* the two shared pages */
static UINT	s_wait;

static struct { UINT port; io_out16_fn out; io_in16_fn in; } s_wide[MAX_WIDE];
static UINT	s_nwide;

static void none_out(UINT port, REG8 value) { (void)port; (void)value; }
static REG8 none_in(UINT port) { (void)port; return 0xff; }

static Page *page_new(const Page *from)
{
	Page *p;

	if (s_npages >= MAX_PAGES) {
		return NULL;
	}
#ifdef ESP_PLATFORM
	p = (Page *)heap_caps_malloc(sizeof(Page), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
	p = (Page *)malloc(sizeof(Page));
#endif
	if (p == NULL) {
		return NULL;
	}
	if (from) {
		memcpy(p, from, sizeof(Page));
	}
	else {
		UINT i;
		for (i = 0; i < 256; i++) {
			p->out[i] = none_out;
			p->in[i] = none_in;
		}
		p->board = 0;
	}
	s_pages[s_npages++] = p;
	return p;
}

void io_free(void)
{
	UINT i;

	for (i = 0; i < s_npages; i++) {
		free(s_pages[i]);
	}
	s_npages = 0;
	s_sys = s_ext = NULL;
	memset(s_map, 0, sizeof(s_map));
	s_nwide = 0;
}

void io_clear(void)
{
	UINT hi;

	io_free();
	s_sys = page_new(NULL);
	s_ext = page_new(NULL);
	for (hi = 0; hi < 256; hi++) {
		s_map[hi] = (hi & 0x0c) ? s_ext : s_sys;
	}
}

void io_set_waitclock(UINT clocks)
{
	s_wait = clocks;
}

/* register k of a device goes to the k-th low byte (in address order) that matches base on the decoded bits */
static void fill(Page *p, UINT base, UINT decode, const io_out_fn *out, const io_in_fn *in, UINT regs)
{
	UINT lo, k = 0;

	for (lo = 0; lo < 256; lo++) {
		if ((lo & decode & 0xff) == (base & decode & 0xff)) {
			if (out && out[k]) p->out[lo] = out[k];
			if (in && in[k]) p->in[lo] = in[k];
			k = (k + 1) % regs;
		}
	}
}

/* the main-board page and every private copy made from it */
static int is_sys_page(const Page *p)
{
	UINT hi;

	for (hi = 0; hi < 256; hi++) {
		if (s_map[hi] == p && !(hi & 0x0c)) return 1;
	}
	return 0;
}

void io_sys(UINT base, UINT decode, const io_out_fn *out, const io_in_fn *in, UINT regs)
{
	UINT i;

	for (i = 0; i < s_npages; i++) {
		if (s_pages[i] == s_sys || (s_pages[i]->board && is_sys_page(s_pages[i]))) {
			fill(s_pages[i], base, decode, out, in, regs);
		}
	}
}

void io_any(UINT base, UINT decode, const io_out_fn *out, const io_in_fn *in, UINT regs)
{
	UINT i;

	for (i = 0; i < s_npages; i++) {
		fill(s_pages[i], base, decode, out, in, regs);
	}
}

static Page *private_page(UINT hi)
{
	Page *p = s_map[hi & 0xff];

	if (!p->board) {
		p = page_new(p);
		if (p == NULL) {
			return NULL;
		}
		p->board = 1;
		s_map[hi & 0xff] = p;
	}
	return p;
}

void io_port(UINT port, io_out_fn out, io_in_fn in)
{
	Page *p = private_page(port >> 8);

	if (p) {
		if (out) p->out[port & 0xff] = out;
		if (in) p->in[port & 0xff] = in;
	}
}

void io_board(UINT port, io_out_fn out, io_in_fn in)
{
	UINT hi;

	for (hi = (port >> 8) & 0x0f; hi < 256; hi += 0x10) {
		Page *p = private_page(hi);
		if (p) {
			if (out) p->out[port & 0xff] = out;
			if (in) p->in[port & 0xff] = in;
		}
	}
}

void io_board_release(UINT port)
{
	UINT hi;

	for (hi = (port >> 8) & 0x0f; hi < 256; hi += 0x10) {
		Page *p = private_page(hi);
		if (p) {
			p->out[port & 0xff] = none_out;
			p->in[port & 0xff] = none_in;
		}
	}
}

void io_port16(UINT port, io_out16_fn out, io_in16_fn in)
{
	if (s_nwide < MAX_WIDE) {
		s_wide[s_nwide].port = port;
		s_wide[s_nwide].out = out;
		s_wide[s_nwide].in = in;
		s_nwide++;
	}
}

/* ---- access ---- */

#define OUTP(port)	(s_map[((port) >> 8) & 0xff]->out[(port) & 0xff])
#define INP(port)	(s_map[((port) >> 8) & 0xff]->in[(port) & 0xff])

void io_out8(UINT port, REG8 value)
{
	g_cpu.slice_left -= s_wait;
	OUTP(port)(port, value);
}

REG8 io_in8(UINT port)
{
	g_cpu.slice_left -= s_wait;
	return INP(port)(port);
}

void io_out16(UINT port, REG16 value)
{
	UINT i;

	g_cpu.slice_left -= s_wait;
	for (i = 0; i < s_nwide; i++) {
		if (s_wide[i].port == port && s_wide[i].out) {
			s_wide[i].out(port, value);
			return;
		}
	}
	OUTP(port)(port, (REG8)(value & 0xff));
	OUTP(port + 1)(port + 1, (REG8)(value >> 8));
}

REG16 io_in16(UINT port)
{
	UINT i;
	REG8 lo;

	g_cpu.slice_left -= s_wait;
	for (i = 0; i < s_nwide; i++) {
		if (s_wide[i].port == port && s_wide[i].in) {
			return s_wide[i].in(port);
		}
	}
	lo = INP(port)(port);
	return (REG16)(lo | (INP(port + 1)(port + 1) << 8));
}

void io_out32(UINT port, UINT32 value)
{
	io_out16(port, (REG16)value);
	io_out16(port + 2, (REG16)(value >> 16));
}

UINT32 io_in32(UINT port)
{
	UINT32 lo = io_in16(port);

	return lo | ((UINT32)io_in16(port + 2) << 16);
}
