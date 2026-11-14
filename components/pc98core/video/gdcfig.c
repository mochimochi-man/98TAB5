/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * gdcfig.c - the graphics GDC's drawing: FIGD (dot, line, arc, rectangle, area fill), GCHRD (graphic characters) and
 * WDAT, from the uPD7220 data sheet.
 *
 * The drawing works on display memory addresses: the execute address EAD (words) and the dot within the word dAD.
 * The eight directions: 0 next line (down), 1 down-right, 2 right (next dot), 3 up-right, 4 up, 5 up-left, 6 left,
 * 7 down-left; a line is PITCH words.  On the PC-98 EAD bits 15-14 select the plane (0 E, 1 B, 2 R, 3 G); the dots
 * 0-7 of a word are its low byte and 8-15 its high byte, each byte from its most significant bit (the leftmost dot).
 *
 * Every dot is a read-modify-write of its word: the word is read from the addressed plane, the operation of the last
 * WDAT command (REPLACE / COMPLEMENT / CLEAR / SET, its bits 1-0) applies the pattern bit at the dot, and the word is
 * written back - to the addressed plane, or through the GRCG (tile write: the enabled planes take their tile in the
 * whole word; read-modify-write: the bits set in the word take the tile) or the EGC when they are on.  Figures take
 * the 16-bit pattern of PRAM 8-9, one bit per dot from bit 0, rotating.
 *   line       DC + 1 dots; each step goes straight on if D < 0 (D += D1), else diagonally (D += D2)
 *   arc        D = r - 1, D2 = 2(r - 1), D1 = -1: the midpoint circle of one octant, dots DM..DC
 *   rectangle  D dots along DIR, D2 dots turned left, D back, D2 back
 *   area fill / graphic character   DC + 1 rows of D dots along DIR, the next row turned left (or diagonally with
 *              the slant bit); a character takes row r from PRAM 15 - r (8 rows), an area fill the PRAM 8-9 pattern;
 *              a 1 bit draws ZOOM dots, every row is drawn ZOOM times
 *   WDAT       each data unit is written DC + 1 times through MASK (a byte type limits it to one byte), the address
 *              moving one word (or line) along DIR each time
 * The figure parameters return to their idle values afterwards (gdc.c).  The DRAWING status bit stays on for the time
 * the GDC would take: a read-modify-write cycle of 4 GDC clocks (2.5 MHz) per dot. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <video.h>

#define PLANE	0x8000u

static const UINT32 s_plane[4] = { GVRAM_E, GVRAM_B, GVRAM_R, GVRAM_G };
static const UINT32 s_cpuplane[4] = { GVRAM_B, GVRAM_R, GVRAM_G, GVRAM_E };

static UINT s_mode;		/* the operation of the last WDAT command */

typedef struct {
	UINT32	ead;
	UINT	dad;
	UINT	pitch;
	UINT	dots;
} Pen;

void gdcfig_reset(void)
{
	s_mode = 0;
}

void gdcfig_init(void)
{
}

/* ---- drawing time ---- */

static void draw_done(UINT id, int timed)
{
	(void)id;
	if (timed) g_ggdc.drawing = 0;
}

static void busy(UINT dots)
{
	const SINT32 clk = (SINT32)(((UINT64)g_mach.cpu_clock * (dots * 4u + 20u)) / 2500000u);
	SINT32 left = ev_armed(EV_GDCDRAW) ? ev_left(EV_GDCDRAW) : 0;

	if (left < 0) left = 0;
	g_ggdc.drawing = 1;
	ev_at(EV_GDCDRAW, left + clk, draw_done);
}

/* ---- one word ---- */

static UINT16 dot_mask(UINT dad)
{
	return (UINT16)(dad < 8 ? 0x80u >> dad : 0x8000u >> (dad - 8));
}

/* read-modify-write of the word at `ead`: the bits of `mask` take `data` by the operation */
static void rmw(UINT32 ead, UINT16 data, UINT16 mask)
{
	const UINT page = g_vid.access_page & 1;
	const UINT32 o = (ead & 0x3fff) << 1;
	UINT8 *w = mem + s_plane[(ead >> 14) & 3] + o + page * GVRAM_PAGE1;
	const UINT16 old = (UINT16)(w[0] | (w[1] << 8));
	UINT16 v;
	UINT i;

	data &= mask;
	switch (s_mode & 3) {
	case 0: v = (UINT16)((old & ~mask) | data); break;
	case 1: v = (UINT16)(old ^ data); break;
	case 2: v = (UINT16)(old & ~data); break;
	default: v = (UINT16)(old | data); break;
	}
	if (g_vid.grcg_chip && (g_vid.grcg_mode & 0x80)) {
		if ((g_vid.mode2 & M2_EGC) && g_vid.grcg_chip == 3) {
			egc_gdc_dot(page, o, v);
			return;
		}
		for (i = 0; i < 4; i++) {
			UINT8 *p = mem + s_cpuplane[i] + o + page * GVRAM_PAGE1;
			const UINT8 t = g_vid.grcg_tile[i];
			if (g_vid.grcg_mode & (1u << i)) continue;
			if (g_vid.grcg_mode & 0x40) {
				p[0] = (UINT8)((p[0] & ~v) | (t & v));
				p[1] = (UINT8)((p[1] & ~(v >> 8)) | (t & (v >> 8)));
			}
			else {
				p[0] = p[1] = t;
			}
		}
	}
	else {
		w[0] = (UINT8)v;
		w[1] = (UINT8)(v >> 8);
	}
	g_grph_dirty[o] |= (UINT8)(1u << page);
	g_grph_dirty[o + 1] |= (UINT8)(1u << page);
	g_vreq.grph[page] |= VR_CHANGED;
}

static void plot(Pen *p, int on)
{
	const UINT16 m = dot_mask(p->dad);

	rmw(p->ead, on ? m : 0, m);
	p->dots++;
}

static void move(Pen *p, UINT dir)
{
	static const SINT8 dx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
	static const SINT8 dy[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };

	dir &= 7;
	if (dy[dir] > 0) p->ead += p->pitch;
	else if (dy[dir] < 0) p->ead -= p->pitch;
	if (dx[dir] > 0 && ++p->dad == 16) {
		p->dad = 0;
		p->ead++;
	}
	else if (dx[dir] < 0 && p->dad-- == 0) {
		p->dad = 15;
		p->ead--;
	}
	p->ead &= 0x3ffff;
}

static SINT32 s14(UINT v)
{
	return (v & 0x2000) ? (SINT32)v - 0x4000 : (SINT32)v;
}

static UINT pattern_bit(UINT16 *pat)
{
	const UINT b = *pat & 1;

	*pat = (UINT16)((*pat >> 1) | (b << 15));
	return b;
}

/* ---- figures ---- */

static void line(Pen *p, UINT oct, UINT16 pat)
{
	const UINT dc = GDC_FIG(&g_ggdc, 0);
	const UINT straight = (oct + 1) & 6, diag = oct | 1;
	const SINT32 d1 = s14(GDC_FIG(&g_ggdc, 3)), d2 = s14(GDC_FIG(&g_ggdc, 2));
	SINT32 d = s14(GDC_FIG(&g_ggdc, 1));
	UINT i;

	for (i = 0; ; i++) {
		plot(p, (int)pattern_bit(&pat));
		if (i >= dc) break;
		if (d < 0) {
			move(p, straight);
			d += d1;
		}
		else {
			move(p, diag);
			d += d2;
		}
	}
}

static void arc(Pen *p, UINT oct, UINT16 pat)
{
	const UINT dc = GDC_FIG(&g_ggdc, 0), dm = GDC_FIG(&g_ggdc, 4);
	const UINT straight = (oct + 1) & 6, diag = oct | 1;
	SINT32 err = s14(GDC_FIG(&g_ggdc, 1)), d2 = s14(GDC_FIG(&g_ggdc, 2)), d1 = s14(GDC_FIG(&g_ggdc, 3));
	UINT i;

	for (i = 0; ; i++) {
		if (i >= dm) plot(p, (int)pattern_bit(&pat));
		if (i >= dc) break;
		d1 += 2;
		err -= d1;
		if (err < 0) {
			d2 -= 2;
			err += d2;
			move(p, diag);
		}
		else {
			move(p, straight);
		}
	}
}

static void rectangle(Pen *p, UINT dir, UINT16 pat)
{
	const UINT side[2] = { GDC_FIG(&g_ggdc, 1), GDC_FIG(&g_ggdc, 2) };
	UINT k, i;

	for (k = 0; k < 4; k++) {
		for (i = 0; i < side[k & 1]; i++) {
			plot(p, (int)pattern_bit(&pat));
			move(p, dir + 2 * k);
		}
	}
}

/* rows of dots: row r's bits from row_bits(r), `width` dots along DIR (each 1 bit drawn `zoom` times) */
static void fill(Pen *p, UINT dir, int slant, const UINT8 *bytes, UINT16 word, int character)
{
	const UINT zoom = (g_ggdc.zoom & 15) + 1;
	const UINT rows = GDC_FIG(&g_ggdc, 0) + 1, width = GDC_FIG(&g_ggdc, 1);
	const UINT next = slant ? dir + 1 : dir + 2;
	UINT r, z, x, k;

	for (r = 0; r < rows; r++) {
		for (z = 0; z < zoom; z++) {
			const Pen start = *p;
			UINT16 bits = character ? (UINT16)(bytes[(7 - r) & 7] * 0x0101u) : word;

			for (x = 0; x < width; x++) {
				const UINT b = pattern_bit(&bits);
				for (k = 0; k < zoom; k++) {
					if (b) plot(p, 1);
					move(p, dir);
				}
			}
			p->ead = start.ead;
			p->dad = start.dad;
			move(p, next);
		}
	}
}

void gdcfig_draw(int character)
{
	const UINT8 kind = g_ggdc.figs[0];
	const UINT dir = kind & 7;
	const UINT16 pat = (UINT16)(g_ggdc.pram[8] | (g_ggdc.pram[9] << 8));
	Pen p;

	p.ead = g_ggdc.ead & 0x3ffff;
	p.dad = g_ggdc.dad & 15;
	p.pitch = g_ggdc.pitch;
	p.dots = 0;
	if (character || (kind & 0x10)) fill(&p, dir, (kind >> 7) & 1, g_ggdc.pram + 8, pat, character);
	else if (kind & 0x08) line(&p, dir, pat);
	else if (kind & 0x20) arc(&p, dir, pat);
	else if (kind & 0x40) rectangle(&p, dir, pat);
	else plot(&p, (int)(pat & 1));
	busy(p.dots);
}

void gdcfig_mode(UINT mod)
{
	s_mode = mod & 3;
}

void gdcfig_wdat(UINT16 data)
{
	const UINT type = g_ggdc.cmd & 0x18;
	const UINT dir = g_ggdc.figs[0] & 7;
	UINT16 mask = g_ggdc.mask;
	UINT n = GDC_FIG(&g_ggdc, 0) + 1, i;
	Pen p;

	if (type == 0x10) mask &= 0x00ff;
	else if (type == 0x18) mask &= 0xff00;
	p.ead = g_ggdc.ead & 0x3ffff;
	p.dad = 0;
	p.pitch = g_ggdc.pitch;
	for (i = 0; i < n; i++) {
		rmw(p.ead, data, mask);
		if (dir == 2 || dir == 6) p.ead = (p.ead + (dir == 2 ? 1 : 0x3ffff)) & 0x3ffff;
		else move(&p, dir);
	}
	g_ggdc.ead = p.ead;
	busy(n);
	/* the figure parameters go idle: the next unit writes one word */
	g_ggdc.figs[1] = 0;
	g_ggdc.figs[2] = 0;
}

/* ---- state save / load (state.c) ---- */
#include <stateint.h>
void gdcfig_state(SsCtx *c)
{
	SS_VAR(c, "GFIG", s_mode);
}
void gdcfig_fns(const void **t)
{
	t[SF_GDCFIG_DRAW_DONE] = (const void *)draw_done;
}
