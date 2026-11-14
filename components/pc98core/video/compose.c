/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * compose.c - the frame: geometry, palette, blink, and the RGB565 composite of the text and graphics buffers into the
 * host surface (640 pixels wide, 400 lines).
 *
 * A pixel is one table lookup: pal[graphics value + text value], text value 0 or (colour + 1) << 4, so a text dot
 * covers the graphics.  Text colours are the 8 digital colours; graphics colours come from the digital palette, the
 * 16-colour palette (4 bits a component), or in the monochrome mode the 8 text colours.
 * When the graphics hide their odd lines (M1_HIDEODD) the
 * odd lines show only text, unless the host asked for the 200-line screen doubled (1) or interpolated (2).
 * The two GDCs' screens start at their own back porch: the one that starts later is moved down by the difference. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include "viewint.h"


#define DRAW_LINES	400

UINT8 g_tpix[SURFACE_WIDTH * VIEW_LINES] __attribute__((aligned(16)));
UINT8 g_gpix[2][SURFACE_WIDTH * VIEW_LINES] __attribute__((aligned(16)));
UINT8 g_line_dirty[VIEW_LINES] __attribute__((aligned(16)));
UINT8 g_tcolor[VIEW_LINES][80];
ViewGeom g_geom;
int (*g_comp_async)(void (*fn)(const void *), const void *arg);


static DRAM_ATTR UINT16 s_pal[16 + 8 * 16];	/* graphics + text value */
static DRAM_ATTR UINT16 s_tpal[256];		/* text value alone (black where there is none) */
static DRAM_ATTR UINT16 s_avg[256];		/* [a << 4 | b]: halfway between graphics values a and b */
static int s_line200;
static UINT s_frames, s_cursor_phase, s_blink_phase;
static UINT32 s_cursor_cell = 0xffffffffu;
static UINT8 s_shown_page = 0xff, s_last_kind = 0xff;

static UINT16 rgb565(UINT r, UINT g, UINT b)		/* 8 bits each */
{
	return (UINT16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static UINT16 digital565(UINT c)			/* bit 0 B, 1 R, 2 G */
{
	return rgb565((c & 2) ? 255 : 0, (c & 4) ? 255 : 0, (c & 1) ? 255 : 0);
}

void compose_palette(void)
{
	UINT i, k;

	for (i = 0; i < 16; i++) {
		UINT16 c;
		if (g_vid.mode1 & M1_MONO) c = digital565(i & 7);
		else if (g_vid.mode2 & M2_ANALOG) c = rgb565(g_vid.ana[i][1] * 17u, g_vid.ana[i][0] * 17u, g_vid.ana[i][2] * 17u);
		else c = digital565((i & 4) ? g_vid.digital[i & 3] & 7 : (g_vid.digital[i & 3] >> 4) & 7);
		s_pal[i] = c;
	}
	memset(s_tpal, 0, sizeof(s_tpal));
	for (k = 1; k <= 8; k++) {
		for (i = 0; i < 16; i++) s_pal[k * 16 + i] = digital565(k - 1);
		s_tpal[k * 16] = digital565(k - 1);
	}
	for (i = 0; i < 256; i++) {
		const UINT a = s_pal[i >> 4], b = s_pal[i & 15];
		s_avg[i] = (UINT16)(((a & 0xf7de) >> 1) + ((b & 0xf7de) >> 1) + (a & b & 0x0821));
	}
	memset(g_line_dirty, LD_ALL, sizeof(g_line_dirty));
}

/* ---- geometry ---- */

static void geometry(void)
{
	const UINT tv = GDC_VBP(&g_tgdc), gv = GDC_VBP(&g_ggdc);
	const UINT cap = VIEW_LINES;
	UINT tl = GDC_AL(&g_tgdc), gl = GDC_AL(&g_ggdc);

	g_geom.t_top = tv > gv ? tv - gv : 0;
	g_geom.g_top = gv > tv ? gv - tv : 0;
	if (g_geom.t_top > cap) g_geom.t_top = cap;
	if (g_geom.g_top > cap) g_geom.g_top = cap;
	if (tl > cap - g_geom.t_top) tl = cap - g_geom.t_top;
	if (gl > cap - g_geom.g_top) gl = cap - g_geom.g_top;
	g_geom.t_lines = tl;
	g_geom.g_lines = gl;
	g_geom.lines = MAX(g_geom.t_top + tl, g_geom.g_top + gl);
	memset(g_tpix, 0, sizeof(g_tpix));
	memset(g_gpix, 0, sizeof(g_gpix));
	memset(g_line_dirty, LD_ALL, sizeof(g_line_dirty));
}

/* ---- rows ---- */

#define WORDS	(SURFACE_WIDTH / 4)

static inline void put4(UINT32 *d, UINT32 w, const UINT16 *pal)
{
	d[0] = (UINT32)pal[w & 0xff] | ((UINT32)pal[(w >> 8) & 0xff] << 16);
	d[1] = (UINT32)pal[(w >> 16) & 0xff] | ((UINT32)pal[w >> 24] << 16);
}

static void row1(UINT8 *dst, const UINT8 *a, const UINT16 *pal)
{
	UINT32 *d = (UINT32 *)dst;
	const UINT32 *p = (const UINT32 *)a;
	UINT i;

	for (i = 0; i < WORDS; i++) put4(d + i * 2, p[i], pal);
}

static void row2(UINT8 *dst, const UINT8 *a, const UINT8 *b, const UINT16 *pal)
{
	UINT32 *d = (UINT32 *)dst;
	const UINT32 *p = (const UINT32 *)a, *q = (const UINT32 *)b;
	UINT i;

	for (i = 0; i < WORDS; i++) put4(d + i * 2, p[i] + q[i], pal);
}

static void row_avg(UINT8 *dst, const UINT8 *a, const UINT8 *b, const UINT8 *t)
{
	UINT16 *d = (UINT16 *)dst;
	UINT i;

	for (i = 0; i < SURFACE_WIDTH; i++) {
		d[i] = t[i] ? s_tpal[t[i]] : s_avg[(a[i] << 4) | b[i]];
	}
}

static void row_fill(UINT8 *dst, UINT16 c)
{
	UINT32 *d = (UINT32 *)dst;
	const UINT32 v = c | ((UINT32)c << 16);
	UINT i;

	for (i = 0; i < SURFACE_WIDTH / 2; i++) d[i] = v;
}

typedef struct {
	UINT8	dirty[VIEW_LINES];
	const UINT8 *g, *t;
	UINT8	*dst;
	int	stride, lines, hideodd, line200;
} Job;
static Job s_job;

static void compose_rows(const void *arg)
{
	const Job *j = (const Job *)arg;
	static const UINT8 zero[SURFACE_WIDTH] __attribute__((aligned(4)));
	const UINT8 *const g = j->g, *const t = j->t;
	int y;

	for (y = 0; y < j->lines; y++) {
		const int src = y;
		const UINT8 *gl = g ? g + src * SURFACE_WIDTH : zero;
		const UINT8 *tl = t ? t + src * SURFACE_WIDTH : zero;
		UINT8 *d = j->dst + (size_t)y * j->stride;

		if (!j->dirty[src] && !(j->hideodd && j->line200 == 2 && (src & 1) && src + 1 < VIEW_LINES && j->dirty[src + 1])) continue;
		if (!g && !t) row_fill(d, 0);
		else if (g && j->hideodd && (src & 1)) {
			if (j->line200 == 0) row1(d, tl, s_tpal);
			else if (j->line200 == 1 || src + 1 >= j->lines) row2(d, gl - SURFACE_WIDTH, tl, s_pal);
			else row_avg(d, gl - SURFACE_WIDTH, gl + SURFACE_WIDTH, tl);
		}
		else if (!g) row1(d, tl, s_tpal);
		else if (!t) row1(d, gl, s_pal);
		else row2(d, gl, tl, s_pal);
	}
}

void compose_frame(int all)
{
	int width = 0, height = 0, stride = 0;
	UINT8 *const fb = (UINT8 *)host_frame(&width, &height, &stride);
	const int disp = (g_vid.mode1 & M1_DISP) != 0;
	const int show_g = disp && g_ggdc.enabled, show_t = disp && g_tgdc.enabled;
	const UINT page = g_vid.disp_page & 1;
	const UINT8 want = (UINT8)(LD_ALL | (show_g ? (1u << page) : 0) | (show_t ? LD_TEXT : 0));
	const UINT8 kind = (UINT8)(show_g | (show_t << 1) | (page << 2));
	int y, lines;

	if (!fb || width != SURFACE_WIDTH || (((size_t)fb | (size_t)stride) & 3)) return;
	if (kind != s_last_kind) {
		s_last_kind = kind;
		all = 1;
	}
	for (y = 0; y < VIEW_LINES; y++) {
		s_job.dirty[y] = (UINT8)(all || (g_line_dirty[y] & want));
		g_line_dirty[y] &= (UINT8)~want;
	}
	lines = (int)g_geom.lines;
	if (lines > DRAW_LINES) lines = DRAW_LINES;
	if (lines > height) lines = height;
	if (all && lines < DRAW_LINES && lines < height) {
		for (y = lines; y < MIN(DRAW_LINES, height); y++) row_fill(fb + (size_t)y * stride, 0);
	}
	s_job.g = show_g ? g_gpix[page] : NULL;
	s_job.t = show_t ? g_tpix : NULL;
	s_job.dst = fb;
	s_job.stride = stride;
	s_job.lines = lines;
	s_job.hideodd = (g_vid.mode1 & M1_HIDEODD) != 0;
	s_job.line200 = s_line200;
	if (!(g_comp_async && g_comp_async(compose_rows, &s_job))) compose_rows(&s_job);
	g_frames_composed++;
}

/* ---- the frame ---- */

void video_init(void)
{
	textview_init();
	grphview_init();
	gdcfig_init();
}

void video_reset(void)
{
	memset(g_tpix, 0, sizeof(g_tpix));
	memset(g_gpix, 0, sizeof(g_gpix));
	memset(g_line_dirty, LD_ALL, sizeof(g_line_dirty));
	s_frames = 0;
	s_cursor_cell = 0xffffffffu;
	s_shown_page = 0xff;
	s_last_kind = 0xff;
	g_vreq.layout = g_vreq.palette = 1;
	g_vreq.text = g_vreq.grph[0] = g_vreq.grph[1] = VR_ALL;
}

void video_redraw_all(void)
{
	g_vreq.palette = 1;
	g_vreq.text = g_vreq.grph[0] = g_vreq.grph[1] = VR_ALL;
	g_vreq.compose = 1;
}

void video_set_line200(int mode)
{
	s_line200 = (mode >= 0 && mode <= 2) ? mode : 0;
	memset(g_line_dirty, LD_ALL, sizeof(g_line_dirty));
}

int textview_has_blink(void);

void video_frame(void)
{
	const UINT br = (UINT)((g_tgdc.cchar[1] >> 6) | ((g_tgdc.cchar[2] & 7) << 2));
	const UINT period = br ? br : 16;
	const UINT page = g_vid.disp_page & 1;
	UINT cursor_phase, blink_phase;
	UINT32 cell;
	int changed = 0;

	if (!g_pc98_draw_enabled) return;
	s_frames++;
	cursor_phase = (g_tgdc.cchar[1] & 0x20) ? 1 : ((s_frames / period) & 1) ^ 1;
	blink_phase = ((s_frames / (period * 2)) & 1) ^ 1;
	cell = g_tgdc.ead & 0x0fff;
	if (cursor_phase != s_cursor_phase || cell != s_cursor_cell) {
		if (s_cursor_cell < 0x1000) g_text_dirty[s_cursor_cell] |= TD_CODE;
		g_text_dirty[cell] |= TD_CODE;
		s_cursor_phase = cursor_phase;
		s_cursor_cell = cell;
		g_vreq.text |= VR_CHANGED;
	}
	if (blink_phase != s_blink_phase) {
		s_blink_phase = blink_phase;
		if (textview_has_blink()) g_vreq.text |= VR_ALL;
	}
	/* Timing belongs to emulation even when rendering is postponed. Geometry
	 * clears shared index planes, which a queued compositor may still read. */
	if (g_vreq.layout) gdc_timing();
	if (!g_mstat.drawframe) return;
	if (g_vreq.layout) {
		g_vreq.layout = 0;
		geometry();
		g_vreq.text = g_vreq.grph[0] = g_vreq.grph[1] = VR_ALL;
		g_vreq.compose = 1;
	}
	if (g_vreq.palette) {
		g_vreq.palette = 0;
		compose_palette();
		changed = 1;
	}
	if (page != s_shown_page) {
		/* A page keeps its own dirty marks and pending full redraws while hidden, so it needs only its changed rows -
		 * except in the monochrome mode, whose dots take the text colours, which may have changed meanwhile. */
		s_shown_page = (UINT8)page;
		g_vreq.grph[page] |= (g_vid.mode1 & M1_MONO) ? VR_ALL : VR_CHANGED;
	}
	if (g_vreq.text) {
		textview_draw((g_vreq.text & VR_ALL) != 0, (int)blink_phase, (int)cursor_phase);
		if (g_vid.mode1 & M1_MONO) g_vreq.grph[page] |= VR_ALL;
		g_vreq.text = 0;
		changed = 1;
	}
	if (g_vreq.grph[page]) {
		grphview_draw(page, (g_vreq.grph[page] & VR_ALL) != 0);
		g_vreq.grph[page] = 0;
		g_vreq.grph[page ^ 1] |= VR_CHANGED;	/* the other page's marks were cleared only for this page */
		changed = 1;
	}
	if (changed || g_vreq.compose) {
		const int all = g_vreq.compose;
		g_vreq.compose = 0;
		compose_frame(all);
	}
}

/* ---- state save / load (state.c) ---- */
#include <stateint.h>
void video_state(SsCtx *c)
{
	/* only the phases of the blink and the cursor: the rest is drawn again from the machine state */
	SS_VAR(c, "VIDf", s_frames);
	SS_VAR(c, "VIDc", s_cursor_phase);
	SS_VAR(c, "VIDb", s_blink_phase);
}

/* after a load: forget what was shown and draw everything again */
void video_state_loaded(void)
{
	s_cursor_cell = 0xffffffffu;
	s_shown_page = 0xff;
	s_last_kind = 0xff;
	memset(g_text_dirty, 0xff, sizeof(g_text_dirty));
	memset(g_grph_dirty, 0xff, sizeof(g_grph_dirty));
	memset(g_line_dirty, LD_ALL, sizeof(g_line_dirty));
	g_vreq.layout = 1;
	video_redraw_all();
}
