/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * textview.c - the text screen: text VRAM and the CG ROM into g_tpix.
 *
 * The text GDC shows up to 4 partitions (PRAM: start cell, number of rasters); a character row is LR + 1 rasters
 * (CCHAR) and starts PITCH cells after the previous one; cells wrap at 4096.  Raster r of a row shows line PL + r of
 * the character (PL 16-31 count as negative); lines below 0 or from CL on are blank.
 * A cell is a code word and an attribute byte (bit 0 shown, 1 blink, 2 reverse, 3 underline, 4 vertical line or,
 * with ATRSEL, semigraphics; bits 7-5 colour G R B).  A code with a zero high byte is a one-byte character (8 x 16
 * font, or 8 x 8 without FONT16: each font line on two rasters); otherwise it is a JIS
 * character: low byte the row - 20h, high byte the column; a cell shows the right half when bit 7 of the low byte
 * is set or when the cell before it showed the left half of the same character (rows 09h-0Bh are half width).
 * The cursor (EAD, when CCHAR enables it) reverses the lines CTOP..CBOT of its cell; underline is the row's last
 * raster; the vertical line is the cell's leftmost dot.  40-column mode shows every other cell, 16 dots wide. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include "viewint.h"

static DRAM_ATTR UINT32 s_mask8[256][2];	/* glyph byte -> 8 pixel bytes of FFh / 00h */
static DRAM_ATTR UINT32 s_mask16[256][4];	/* ... each dot twice */
static int s_has_blink;

void textview_init(void)
{
	UINT d, k;

	for (d = 0; d < 256; d++) {
		UINT8 px[16];
		for (k = 0; k < 8; k++) px[k] = (d & (0x80u >> k)) ? 0xff : 0;
		memcpy(s_mask8[d], px, 8);
		for (k = 0; k < 16; k++) px[k] = (d & (0x80u >> (k >> 1))) ? 0xff : 0;
		memcpy(s_mask16[d], px, 16);
	}
}

typedef struct {
	UINT32	glyph;		/* CG ROM offset of line 0 */
	UINT8	kind;		/* 0 blank, 1 ANK16, 2 ANK8, 3 semi16, 4 semi8, 5 kanji */
	UINT8	attr;
	UINT32	color;		/* the pixel value in all 4 bytes */
	UINT8	cursor;
} Cell;

static UINT s_prev_left;	/* the previous cell showed the left half of this JIS code (0 = none) */

static void decode(Cell *c, UINT cell, int blink_on)
{
	const UINT code = mem[0xa0000 + cell * 2] | (mem[0xa0001 + cell * 2] << 8);
	const UINT8 attr = mem[0xa2000 + cell * 2];

	c->attr = attr;
	c->color = (UINT32)((((attr >> 5) & 7) + 1) << 4) * 0x01010101u;
	if (!(attr & 1) || ((attr & 2) && !blink_on)) {
		c->kind = 0;
	}
	else if (!(code & 0xff00)) {
		const int semi = (g_vid.mode1 & M1_ATRSEL) && (attr & 0x10);
		if (g_vid.mode1 & M1_FONT16) {
			c->kind = semi ? 3 : 1;
			c->glyph = (semi ? CG_SEMI16 : CG_ANK16) + (code & 0xff) * 16;
		}
		else {
			c->kind = semi ? 4 : 2;
			c->glyph = CG_ANK8 + (code & 0xff) * 16 + (semi ? 8 : 0);
		}
	}
	else {
		const UINT jis = code & 0x7f7f, row = code & 0x7f;
		const int right = (code & 0x80) || s_prev_left == jis;
		c->kind = 5;
		c->glyph = ((UINT32)jis << 4) + (right ? 0x800 : 0);
		s_prev_left = (!right && (row < 0x09 || row > 0x0b)) ? jis : 0;
		goto attrs;
	}
	s_prev_left = 0;
attrs:
	if (attr & 2) s_has_blink = 1;
}

static UINT glyph_line(const Cell *c, int cl)
{
	int line;

	if (!c->kind || cl < 0 || cl >= (int)g_vid.crtc[2]) return 0;
	switch (c->kind) {
	case 2: case 4:
		line = cl >> 1;
		return line < 8 ? cgrom[c->glyph + line] : 0;
	case 5:
		line = cl;
		return line < 16 ? cgrom[c->glyph + line] : 0;
	default:
		return cl < 16 ? cgrom[c->glyph + cl] : 0;
	}
}

static int row_dirty(UINT cell, UINT n, UINT step)
{
	UINT i;

	for (i = 0; i < n * step; i++) {
		if (g_text_dirty[(cell + i) & 0x0fff]) return 1;
	}
	return 0;
}

void textview_draw(int all, int blink_on, int cursor_on)
{
	const int wide = (g_vid.mode1 & M1_COL40) != 0;
	const UINT n = wide ? 40 : 80, step = wide ? 2 : 1;
	const UINT rowh = GDC_LR(&g_tgdc) + 1;
	const int pl = (g_vid.crtc[0] < 16) ? g_vid.crtc[0] : (int)g_vid.crtc[0] - 32;
	const UINT ctop = g_tgdc.cchar[1] & 0x1f, cbot = g_tgdc.cchar[2] >> 3;
	const UINT cur = g_tgdc.ead & 0x0fff;
	const int show_cursor = cursor_on && (g_tgdc.cchar[0] & 0x80);
	UINT y = 0, part = 0;
	Cell cells[80];

	if (all) s_has_blink = 0;
	while (y < g_geom.t_lines && part < 4) {
		UINT cell = GDC_SAD(&g_tgdc, part) & 0x0fff;
		UINT len = GDC_LEN(&g_tgdc, part);
		UINT r = 0;
		int dirty = 0;

		if (!len) len = 1024;
		part++;
		for (; len && y < g_geom.t_lines; len--, y++) {
			const UINT ry = g_geom.t_top + y;
			UINT x;

			if (r == 0) {
				dirty = all || row_dirty(cell, n, step);
				if (dirty) {
					s_prev_left = 0;
					for (x = 0; x < n; x++) {
						const UINT a = (cell + x * step) & 0x0fff;
						decode(&cells[x], a, blink_on);
						cells[x].cursor = show_cursor && a == cur;
					}
				}
			}
			if (dirty && ry < VIEW_LINES) {
				UINT32 *q = (UINT32 *)(g_tpix + ry * SURFACE_WIDTH);
				const int cl = pl + (int)r;
				const int last = (r + 1 == rowh);

				for (x = 0; x < n; x++) {
					const Cell *c = &cells[x];
					UINT d = glyph_line(c, cl);
					if (c->attr & 4) d ^= 0xff;
					if (c->cursor && r >= ctop && r <= cbot) d ^= 0xff;
					if (last && (c->attr & 8) && c->kind) d = 0xff;
					if ((c->attr & 0x10) && !(g_vid.mode1 & M1_ATRSEL) && c->kind) d |= 0x80;
					if (!wide) {
						q[0] = s_mask8[d][0] & c->color;
						q[1] = s_mask8[d][1] & c->color;
						q += 2;
						g_tcolor[ry][x] = (UINT8)((c->attr >> 5) & 7);
					}
					else {
						q[0] = s_mask16[d][0] & c->color;
						q[1] = s_mask16[d][1] & c->color;
						q[2] = s_mask16[d][2] & c->color;
						q[3] = s_mask16[d][3] & c->color;
						q += 4;
						g_tcolor[ry][x * 2] = g_tcolor[ry][x * 2 + 1] = (UINT8)((c->attr >> 5) & 7);
					}
				}
				g_line_dirty[ry] |= LD_TEXT;
			}
			if (++r == rowh) {
				r = 0;
				cell = (cell + g_tgdc.pitch) & 0x0fff;
			}
		}
	}
	memset(g_text_dirty, 0, sizeof(g_text_dirty));
}

int textview_has_blink(void)
{
	return s_has_blink;
}
