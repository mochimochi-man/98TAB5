/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * cgwin.c - the character generator ports and the CG window.
 *
 *   A1h write  character code, high byte: the second JIS byte of a kanji, 0 for an ANK character
 *   A3h write  character code, low byte: the first JIS byte - 20h of a kanji (the row), the code of an ANK character
 *   A5h write  bits 4-0 = line, bit 5 = 1 left half / 0 right half
 *   A9h write  a line of a user-defined character (rows 56h and 57h)
 *   A9h read   the selected line of the selected character
 * The CG window (A4000h-A4FFFh, read in video/vmem.c) shows the selected character: even addresses the left half,
 * odd addresses the right half; the window exists only with the GRCG that has it (chip 2 and later).
 * Rows 09h-0Bh are the half-width characters (a left half only); rows 0Ch-0Fh and 58h-5Fh are shown one half at a
 * time; rows 56h-57h (user-defined) likewise, and the window can then be written. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <io.h>
#include <devices.h>
#include <video.h>

#define BLANK		0x7fff0u	/* 16 unused bytes at the end of the kanji area */
#define ANK16		0x80000u	/* the 8x16 ANK face: code * 16 */
#define ANK8		0x82000u	/* the 8x8 face */

CgWindow g_cgwin;

static UINT s_code;		/* A1h << 8 | A3h */
static UINT s_line;
static UINT s_half;		/* 0 left, 800h right */

static UINT32 kanji_base(void)
{
	return (UINT32)(s_code & 0x7f7f) << 4;
}

static void window_update(void)
{
	UINT32 even = BLANK, odd = BLANK;

	g_cgwin.flags &= (UINT8)~CGWIN_WRITABLE;
	if (g_vid.grcg_chip >= 2) {
		if ((s_code & 0xff00) == 0) {
			odd = ((g_vid.mode1 & M1_FONT16) ? ANK16 : ANK8) + (s_code << 4);	/* 16- or 8-line character mode */
		}
		else {
			const UINT row = s_code & 0x7f;
			const UINT32 base = kanji_base();
			if (row == 0x56 || row == 0x57) {
				odd = base + s_half;
				g_cgwin.flags |= CGWIN_WRITABLE;
			}
			else if (row >= 0x09 && row <= 0x0b) {
				odd = s_half ? BLANK : base;
			}
			else if ((row >= 0x0c && row <= 0x0f) || (row >= 0x58 && row <= 0x5f)) {
				odd = base + s_half;
			}
			else {
				even = base;
				odd = base + 0x800;
			}
		}
	}
	g_cgwin.even = even;
	g_cgwin.odd = odd;
}

static void code_high_w(UINT port, REG8 v) { (void)port; s_code = (v << 8) | (s_code & 0xff); window_update(); }
static void code_low_w(UINT port, REG8 v) { (void)port; s_code = (s_code & 0xff00) | v; window_update(); }
static void line_w(UINT port, REG8 v)
{
	(void)port;
	s_line = v & 0x1f;
	s_half = (v & 0x20) ? 0 : 0x800;
	window_update();
}

static void pattern_w(UINT port, REG8 v)
{
	(void)port;
	if ((s_code & 0x7e) == 0x56) {			/* rows 56h / 57h */
		cgrom[kanji_base() + s_half + (s_line & 15)] = (UINT8)v;
		g_cgwin.flags |= CGWIN_WRITTEN;
		g_vreq.text |= VR_ALL;		/* every cell showing the character changes */
	}
}

static REG8 pattern_r(UINT port)
{
	const UINT row = s_code & 0xff;

	(void)port;
	if ((s_code & 0xff00) == 0) {
		return s_line < 16 ? cgrom[ANK16 + (s_code << 4) + s_line] : 0;
	}
	if (row >= 0x09 && row <= 0x0b) {
		return s_half ? 0 : cgrom[kanji_base() + (s_line & 15)];
	}
	return cgrom[kanji_base() + s_half + (s_line & 15)];
}

void cgwin_reset(const Pc98Settings *cfg)
{
	(void)cfg;
	s_code = 0;
	s_line = 0;
	s_half = 0;
	g_cgwin.even = BLANK;
	g_cgwin.odd = BLANK;
	g_cgwin.flags = 0;
}

void cgwin_bind(void)
{
	static const io_out_fn out[8] = { code_high_w, code_low_w, line_w, NULL, pattern_w, NULL, NULL, NULL };
	static const io_in_fn in[8] = { NULL, NULL, NULL, NULL, pattern_r, NULL, NULL, NULL };

	io_sys(0xa1, 0xf1, out, in, 8);
}

/* ---- state save / load (state.c) ---- */
#include <stateint.h>
void cgwin_state(SsCtx *c)
{
	SS_VAR(c, "CGWn", g_cgwin);
	SS_VAR(c, "CGWc", s_code);
	SS_VAR(c, "CGWl", s_line);
	SS_VAR(c, "CGWh", s_half);
}
