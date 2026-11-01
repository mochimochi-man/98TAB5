/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * grphview.c - the graphics screen: the four planes of a page into g_gpix[page], a byte per dot
 * (B | R << 1 | G << 2 | E << 3).
 *
 * The graphics GDC shows 2 partitions (PRAM: start word, number of rasters); a display address is shown on LR + 1
 * rasters (CCHAR, 2 on the 200-line screen) and the next line starts PITCH words further on - PITCH bytes with the
 * wide display bit (WD) set.  Addresses wrap inside the 32 KB plane.
 * In the monochrome mode a lit dot (its colour is not black) takes the colour of the text cell above it. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include "viewint.h"

static DRAM_ATTR UINT32 s_lo[256], s_hi[256];	/* two planes' nibbles -> 4 pixel bytes */

static UINT32 spread(UINT nib)
{
	UINT32 v = 0;
	UINT k;

	for (k = 0; k < 4; k++) {
		if (nib & (8u >> k)) v |= 1u << (k * 8);
	}
	return v;
}

void grphview_init(void)
{
	UINT i;

	for (i = 0; i < 256; i++) {
		s_lo[i] = spread(i & 15) | (spread(i >> 4) << 1);
		s_hi[i] = (spread(i & 15) << 2) | (spread(i >> 4) << 3);
	}
}

static inline void expand(UINT32 *d, UINT b, UINT r, UINT g, UINT e)
{
	d[0] = s_lo[(b >> 4) | (r & 0xf0)] | s_hi[(g >> 4) | (e & 0xf0)];
	d[1] = s_lo[(b & 15) | ((r & 15) << 4)] | s_hi[(g & 15) | ((e & 15) << 4)];
}

static int lit(UINT v)
{
	if (g_vid.mode2 & M2_ANALOG) return (g_vid.ana[v][0] | g_vid.ana[v][1] | g_vid.ana[v][2]) != 0;
	return ((v & 4 ? g_vid.digital[v & 3] : g_vid.digital[v & 3] >> 4) & 7) != 0;
}

/* one display address row: its 80 bytes of the four planes into 640 pixel bytes, two bytes a load */
static void expand_row(UINT32 *d, const UINT8 *pb, UINT32 addr)
{
	UINT i;

	if (!(addr & 3) && addr + 80 <= 0x8000) {	/* aligned and not wrapping: four bytes a load */
		const UINT32 *pB = (const UINT32 *)(pb + GVRAM_B + addr), *pR = (const UINT32 *)(pb + GVRAM_R + addr);
		const UINT32 *pG = (const UINT32 *)(pb + GVRAM_G + addr), *pE = (const UINT32 *)(pb + GVRAM_E + addr);

		for (i = 0; i < 20; i++, d += 8) {
			const UINT32 b = pB[i], r = pR[i], g = pG[i], e = pE[i];

			expand(d, b & 0xff, r & 0xff, g & 0xff, e & 0xff);
			expand(d + 2, (b >> 8) & 0xff, (r >> 8) & 0xff, (g >> 8) & 0xff, (e >> 8) & 0xff);
			expand(d + 4, (b >> 16) & 0xff, (r >> 16) & 0xff, (g >> 16) & 0xff, (e >> 16) & 0xff);
			expand(d + 6, b >> 24, r >> 24, g >> 24, e >> 24);
		}
		return;
	}
	for (i = 0; i < 80; i += 2) {
		const UINT32 o = (addr + i) & 0x7fff;	/* addr is even: a pair never wraps */
		const UINT b = *(const UINT16 *)(pb + GVRAM_B + o), r = *(const UINT16 *)(pb + GVRAM_R + o);
		const UINT g = *(const UINT16 *)(pb + GVRAM_G + o), e = *(const UINT16 *)(pb + GVRAM_E + o);

		expand(d + i * 2, b & 0xff, r & 0xff, g & 0xff, e & 0xff);
		expand(d + i * 2 + 2, b >> 8, r >> 8, g >> 8, e >> 8);
	}
}

/* only the byte pairs of the row that are marked: sprites scattered over the screen mark most rows, a few bytes each */
static void expand_marked(UINT32 *d, const UINT8 *pb, UINT32 addr, UINT8 bit, const UINT8 *lut, const UINT8 *tc)
{
	const UINT16 m = (UINT16)(bit * 0x0101u);
	UINT i, k;

	for (i = 0; i < 80; i += 2) {
		const UINT32 o = (addr + i) & 0x7ffe;
		if (!(*(const UINT16 *)(g_grph_dirty + o) & m)) continue;
		{
			const UINT b = *(const UINT16 *)(pb + GVRAM_B + o), r = *(const UINT16 *)(pb + GVRAM_R + o);
			const UINT g = *(const UINT16 *)(pb + GVRAM_G + o), e = *(const UINT16 *)(pb + GVRAM_E + o);

			expand(d + i * 2, b & 0xff, r & 0xff, g & 0xff, e & 0xff);
			expand(d + i * 2 + 2, b >> 8, r >> 8, g >> 8, e >> 8);
		}
		if (lut) {
			UINT8 *px = (UINT8 *)(d + i * 2);
			for (k = 0; k < 16; k++) px[k] = lut[px[k]] ? tc[(i * 8 + k) >> 3] : 0;
		}
	}
}

#ifdef PC98_GV_VERIFY
/* check builds (PC98_GV_VERIFY, slow: every row against VRAM every frame): before a dirty row is redrawn whole, the byte pairs without a mark must already show VRAM as it is.
 * A pair that does not is reported (it would stay wrong if only the marked pairs were redrawn). */
unsigned g_gv_bad, g_gv_rows;
static void verify_unmarked(const UINT32 *d, const UINT8 *pb, UINT32 addr, UINT8 bit, UINT page, UINT ry)
{
	extern int ets_printf(const char *, ...);
	const UINT16 m = (UINT16)(bit * 0x0101u);
	UINT32 t[4];
	UINT i;

	g_gv_rows++;
	for (i = 0; i < 80; i += 2) {
		const UINT32 o = (addr + i) & 0x7ffe;
		if (*(const UINT16 *)(g_grph_dirty + o) & m) continue;
		{
			const UINT b = *(const UINT16 *)(pb + GVRAM_B + o), r = *(const UINT16 *)(pb + GVRAM_R + o);
			const UINT g = *(const UINT16 *)(pb + GVRAM_G + o), e = *(const UINT16 *)(pb + GVRAM_E + o);
			expand(t, b & 0xff, r & 0xff, g & 0xff, e & 0xff);
			expand(t + 2, b >> 8, r >> 8, g >> 8, e >> 8);
		}
		if (memcmp(t, d + i * 2, 16)) {
			if (g_gv_bad++ < 40) {
				ets_printf("GVBAD p%u ry%u addr%04x o%04x mk%02x%02x other%02x%02x px", page, ry, (unsigned)addr, (unsigned)o,
					   g_grph_dirty[o], g_grph_dirty[o + 1], g_grph_dirty[o] & (UINT8)~bit, 0);
				for (UINT k = 0; k < 4; k++) ets_printf(" %08x/%08x", (unsigned)d[i * 2 + k], (unsigned)t[k]);
				ets_printf("\n");
			}
		}
	}
}
#endif

#ifdef PC98_GV_VERIFY
/* debug builds: after a row's marked pairs are redrawn, the whole row must equal a full expansion */
static void verify_row(const UINT32 *d, const UINT8 *pb, UINT32 addr, UINT page, UINT ry)
{
	extern int ets_printf(const char *, ...);
	static UINT32 t[SURFACE_WIDTH / 4];
	UINT i;

	expand_row(t, pb, addr);
	for (i = 0; i < SURFACE_WIDTH / 4; i++) {
		if (t[i] != d[i]) {
			if (g_gv_bad++ < 40) ets_printf("GVROW p%u ry%u addr%04x px%u got %08x want %08x" "%c", page, ry, (unsigned)addr, i * 4,
							 (unsigned)d[i], (unsigned)t[i], 10);
			return;
		}
	}
}
#endif

static int row_dirty(UINT32 addr, UINT8 bit)
{
	const UINT16 m = (UINT16)(bit * 0x0101u);
	UINT i;

	for (i = 0; i < 80; i += 2) {
		if (*(const UINT16 *)(g_grph_dirty + ((addr + i) & 0x7ffe)) & m) return 1;
	}
	return 0;
}


void grphview_draw(UINT page, int all)
{
	const UINT8 *const pb = mem + (page ? GVRAM_PAGE1 : 0);
	const UINT8 bit = (UINT8)(1u << page);
	const UINT rep = GDC_LR(&g_ggdc) + 1;
	const int mono = (g_vid.mode1 & M1_MONO) != 0;
	static UINT16 drawn[VIEW_LINES];	/* the address rows drawn: their dirty marks are cleared at the end */
	UINT ndrawn = 0;
	UINT y = 0, part = 0, i;
	UINT8 lut[16];

	if (mono) for (i = 0; i < 16; i++) lut[i] = (UINT8)lit(i);
	while (y < g_geom.g_lines) {
		UINT32 addr = (GDC_SAD(&g_ggdc, part & 1) << 1) & 0x7fff;
		UINT len = GDC_LEN(&g_ggdc, part & 1);
		const UINT pitch = GDC_WD(&g_ggdc, part & 1) ? (g_ggdc.pitch & 0xfe) : (g_ggdc.pitch * 2u);
		UINT r = 0;
		int changed = 0;

		if (!len) len = 1024;
		part++;
		for (; len && y < g_geom.g_lines; len--, y++) {
			const UINT ry = g_geom.g_top + y;

			if (r == 0) {
				changed = all || row_dirty(addr, bit);
#ifdef PC98_GV_VERIFY
				if (!changed && !mono && ry < VIEW_LINES) verify_row(page ? (const UINT32 *)(g_gpix[1] + ry * SURFACE_WIDTH) : (const UINT32 *)(g_gpix[0] + ry * SURFACE_WIDTH), pb, addr, page, ry | 0x1000);
#endif
				if (changed && ndrawn < VIEW_LINES) drawn[ndrawn++] = (UINT16)addr;
			}
			if (changed && ry < VIEW_LINES) {
				UINT32 *d = (UINT32 *)(g_gpix[page] + ry * SURFACE_WIDTH);
				if (!all) {
					expand_marked(d, pb, addr, bit, mono ? lut : NULL, g_tcolor[ry]);
#ifdef PC98_GV_VERIFY
					if (!mono) verify_row(d, pb, addr, page, ry);
#endif
					g_line_dirty[ry] |= bit;
				}
				else {
				expand_row(d, pb, addr);
				if (mono) {
					UINT8 *px = (UINT8 *)d;
					for (i = 0; i < SURFACE_WIDTH; i++) px[i] = lut[px[i]] ? g_tcolor[ry][i >> 3] : 0;
				}
				g_line_dirty[ry] |= bit;
				}
			}
			if (++r == rep) {
				r = 0;
				addr = (addr + pitch) & 0x7fff;
			}
		}
		if (part > 8) break;
	}
	/* Only the rows drawn are marked clean.  Marks on addresses not on the screen stay: when they come into view
	 * they are drawn once more than needed, never one less. */
	{
		const UINT8 keep = (UINT8)~bit;
		UINT k;

		for (k = 0; k < ndrawn; k++) {
			for (i = 0; i < 80; i++) g_grph_dirty[(drawn[k] + i) & 0x7fff] &= keep;
		}
	}
}
