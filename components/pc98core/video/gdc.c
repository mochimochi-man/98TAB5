/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * gdc.c - the two uPD7220 GDCs, the display control ports and the frame timing.
 *
 * Ports
 *   60h / A0h  write a parameter, read the status       62h / A2h  write a command, read data
 *   64h        write: an interrupt (IRQ2) at the next vertical retrace
 *   68h        mode flip-flops 1: bits 3-1 the flip-flop, bit 0 its value (M1_*)
 *   6Ah        mode flip-flops 2: 00h-07h bits 2-1 the flip-flop, bit 0 its value (M2_*: 16 colours, EGC mode,
 *              EGC mode change enable); other values (plasma display, GDC clock) change nothing shown here
 *   A4h        the graphics page shown;  A6h  the graphics page the CPU reaches
 *   A8h-AEh    palette: 8 colours: A8h colours 3 / 7, AAh 1 / 5, ACh 2 / 6, AEh 0 / 4 (bits 6-4 / 2-0, G R B);
 *              16 colours: A8h the entry, AAh green, ACh red, AEh blue (4 bits)
 * Status: bit 0 data to read, bit 2 FIFO empty, bit 3 drawing, bit 5 vertical retrace, bit 6 horizontal blanking.
 * The GDCs carry out a command as soon as it and its parameters are complete, so their FIFO is always empty.
 *
 * Commands (uPD7220): RESET 00h (+ SYNC parameters, display off), SYNC 0Eh / 0Fh (display off / on), VSYNC 6Eh / 6Fh,
 * CCHAR 4Bh, START 6Bh, BCTRL 0Ch / 0Dh (and 05h) display off / on, ZOOM 46h, CURS 49h, PRAM 70h-7Fh, PITCH 47h,
 * MASK 4Ah, FIGS 4Ch, FIGD 6Ch, GCHRD 68h, WDAT 20h-3Fh, RDAT A0h-BFh, CURD E0h, LPRD C0h.
 *
 * Timing: the text GDC's SYNC parameters give the line (AW + HS + HFP + HBP characters of 8 dots; the dot clock is
 * 21.0526 MHz: only the 24 kHz monitor is emulated) and the frame (AL + VS + VFP + VBP lines).  A frame is its display
 * period followed by the vertical retrace; the retrace sets status bit 5, may raise IRQ2, and the renderer runs. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <io.h>
#include <devices.h>
#include <video.h>
#ifdef GDC_TRACE
#include <stdio.h>
#endif

Gdc g_tgdc, g_ggdc;
VideoCtl g_vid;
VideoReq g_vreq;

static UINT32 s_frame_t0;	/* CPU clock at the start of the current frame */

static UINT32 now(void)
{
	return (UINT32)(g_cpu.clocks_done + g_cpu.slice_len - g_cpu.slice_left);
}

static UINT8 *req_of(const Gdc *g)
{
	return (g == &g_tgdc) ? &g_vreq.text : &g_vreq.grph[0];
}

static void request_all(const Gdc *g)
{
	if (g == &g_tgdc) {
		g_vreq.text |= VR_ALL;
	}
	else {
		g_vreq.grph[0] |= VR_ALL;
		g_vreq.grph[1] |= VR_ALL;
	}
}

/* ---- the memory map of the graphics window ---- */

void video_map_update(void)
{
	UINT m = g_vid.access_page & 1;

	if (g_vid.grcg_chip && (g_vid.grcg_mode & 0x80)) {
		m |= VMAP_GRCG;
		if ((g_vid.mode2 & M2_EGC) && g_vid.grcg_chip == 3) m |= VMAP_EGC;
		else if (g_vid.grcg_mode & 0x40) m |= VMAP_RMW;
	}
	if (g_vid.mode2 & M2_ANALOG) m |= VMAP_EPLANE;
	bus_set_vram_access(m);
}

/* ---- timing ---- */

void gdc_timing(void)
{
	const UINT8 *s = g_tgdc.sync;
	const UINT aw = GDC_AW(&g_tgdc);
	const UINT hs = (s[2] & 0x1f) + 1u;
	const UINT vs = (s[2] >> 5) | ((s[3] & 3u) << 3);
	const UINT hfp = (s[3] >> 2) + 1u;
	const UINT hbp = (s[4] & 0x3fu) + 1u;
	const UINT vfp = s[5] & 0x3fu;
	const UINT al = GDC_AL(&g_tgdc);
	const UINT32 dotclk = 21052600u;	/* the 24 kHz monitor (the only one emulated) */
	UINT htotal = aw + hs + hfp + hbp;
	UINT vtotal = al + vs + vfp + GDC_VBP(&g_tgdc);
	UINT active = al;

	/* the parameters are nonsense before the BIOS has set them: assume the standard frame */
	if (htotal < 64 || htotal > 160 || aw >= htotal) {
		htotal = 106;
		if (aw >= htotal) htotal = aw + 26;
	}
	if (vtotal < 200 || vtotal > 700 || active >= vtotal) {
		vtotal = 440;
		active = 400;
	}
	g_vid.raster_clk = (UINT32)(((UINT64)g_mach.cpu_clock * htotal * 8) / dotclk);
	if (!g_vid.raster_clk) g_vid.raster_clk = 1;
	g_vid.hblank_clk = (UINT32)(((UINT64)g_vid.raster_clk * (htotal - (aw < htotal ? aw : 0))) / htotal);
	g_vid.disp_clk = g_vid.raster_clk * active;
	g_vid.retrace_clk = g_vid.raster_clk * (vtotal - active);
}

static void retrace_end(UINT id, int timed);

static void retrace_start(UINT id, int timed)
{
	(void)id;
	(void)timed;
	g_memwait_tram = g_settings.vram_wait[1];
	g_memwait_vram = g_settings.vram_wait[3];
	g_memwait_grcg = g_settings.vram_wait[5];
	g_vid.vsync = 0x20;
	if (g_vid.vsync_irq) {
		g_vid.vsync_irq = 0;
		pic_raise(2);
	}
	ev_next(EV_FRAME, (SINT32)g_vid.retrace_clk, retrace_end);
	if (g_settings.draw_at_retrace) video_frame();
}

static void retrace_end(UINT id, int timed)
{
	(void)id;
	(void)timed;
	g_vid.vsync = 0;
	g_mstat.in_display = 0;
	if (!g_settings.draw_at_retrace) video_frame();
}

void gdc_frame_start(void)
{
	g_memwait_tram = g_settings.vram_wait[0];
	g_memwait_vram = g_settings.vram_wait[2];
	g_memwait_grcg = g_settings.vram_wait[4];
	s_frame_t0 = now();
	ev_next(EV_FRAME, (SINT32)g_vid.disp_clk, retrace_start);
}

static REG8 status(const Gdc *g)
{
	const UINT32 pos = now() - s_frame_t0;
	REG8 v = 0x04;

	if (g->nrd) v |= 0x01;
	if (g->drawing) v |= 0x08;
	if (pos >= g_vid.disp_clk) v |= 0x20;
	if ((pos % g_vid.raster_clk) >= g_vid.raster_clk - g_vid.hblank_clk) v |= 0x40;
	return v;
}

/* ---- commands ---- */

static void figs_idle(Gdc *g)
{
	static const UINT8 idle[10] = { 0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0xff, 0x3f, 0xff, 0x3f };

	memcpy(g->figs + 1, idle, sizeof(idle));
}

static void display(Gdc *g, int on)
{
	if (g->enabled != (on ? 1 : 0)) {
		g->enabled = on ? 1 : 0;
		request_all(g);
		g_vreq.compose = 1;
	}
}

/* the number of parameter bytes a command takes (the rest are ignored) */
static UINT param_count(UINT8 cmd)
{
	if (cmd == 0x00 || cmd == 0x0e || cmd == 0x0f) return 8;
	if ((cmd & 0xf0) == 0x70) return 16 - (cmd & 0x0f);
	if ((cmd & 0xe0) == 0x20) return 0xff;			/* WDAT: data until the next command */
	switch (cmd) {
	case 0x46: case 0x47: return 1;
	case 0x4a: return 2;
	case 0x49: case 0x4b: return 3;
	case 0x4c: return 11;
	default: return 0;
	}
}

static void command(Gdc *g, UINT8 c)
{
#ifdef GDC_TRACE
	printf("GDC %c CMD %02x\n", g == &g_tgdc ? 'T' : 'G', (unsigned)c);
#endif
	g->cmd = c;
	g->nparam = 0;
	g->nrd = 0;
	g->rdpos = 0;
	if ((c & 0xe0) == 0x20) {				/* WDAT: the data follows */
		if (g == &g_ggdc) gdcfig_mode(c & 3);
		return;
	}
	switch (c) {
	case 0x00:						/* RESET */
		display(g, 0);
		break;
	case 0x0e: case 0x0f:					/* SYNC (the DE bit is taken with the parameters) */
		display(g, c & 1);
		break;
	case 0x6b: case 0x0d:					/* START, BCTRL on */
		display(g, 1);
		break;
	case 0x0c: case 0x05:					/* BCTRL off, STOP */
		display(g, 0);
		break;
	case 0x6c:						/* FIGD */
		if (g == &g_ggdc) gdcfig_draw(0);
		figs_idle(g);
		break;
	case 0x68:						/* GCHRD */
		if (g == &g_ggdc) gdcfig_draw(1);
		figs_idle(g);
		break;
	case 0xe0:						/* CURD */
		g->rd[0] = (UINT8)g->ead;
		g->rd[1] = (UINT8)(g->ead >> 8);
		g->rd[2] = (UINT8)((g->ead >> 16) & 3);
		g->rd[3] = (UINT8)(1u << (g->dad & 7));
		g->rd[4] = (UINT8)((1u << g->dad) >> 8);
		g->nrd = 5;
		break;
	case 0xc0:						/* LPRD: no light pen */
		memset(g->rd, 0, 3);
		g->nrd = 3;
		break;
	default:
		if ((c & 0xe0) == 0xa0 && g == &g_ggdc) {	/* RDAT: the word at the cursor */
			static const UINT32 plane[4] = { GVRAM_E, GVRAM_B, GVRAM_R, GVRAM_G };
			const UINT8 *p = mem + plane[(g->ead >> 14) & 3] + ((g->ead & 0x3fff) << 1) + (g_vid.access_page ? GVRAM_PAGE1 : 0);
			const UINT16 w = (UINT16)(p[0] | (p[1] << 8));
			switch (c & 0x18) {
			case 0x10: g->rd[0] = (UINT8)w; g->nrd = 1; break;
			case 0x18: g->rd[0] = (UINT8)(w >> 8); g->nrd = 1; break;
			default: g->rd[0] = (UINT8)w; g->rd[1] = (UINT8)(w >> 8); g->nrd = 2; break;
			}
		}
		break;
	}
}

static void set_byte(UINT8 *p, UINT8 v, const Gdc *g, UINT8 req)
{
	if (*p != v) {
		*p = v;
		*req_of(g) |= req;
		if (g == &g_ggdc && (req & VR_ALL)) g_vreq.grph[1] |= VR_ALL;
	}
}

static void parameter(Gdc *g, UINT8 v)
{
	const UINT8 c = g->cmd;
	const UINT n = g->nparam;

	if (n >= param_count(c)) return;
#ifdef GDC_TRACE
	if ((c & 0xe0) != 0x20) printf("GDC %c PRM %02x P%u=%02x\n", g == &g_tgdc ? 'T' : 'G', (unsigned)c, n + 1, (unsigned)v);
#endif
	g->nparam++;
	if ((c & 0xe0) == 0x20) {				/* WDAT */
		const UINT type = c & 0x18;
		if (g != &g_ggdc || type == 0x08) return;
		if (type == 0x00) {
			g->wdat[n & 1] = v;
			if (n & 1) gdcfig_wdat((UINT16)(g->wdat[0] | (g->wdat[1] << 8)));
		}
		else {
			gdcfig_wdat(type == 0x10 ? v : (UINT16)(v << 8));
		}
		return;
	}
	switch (c) {
	case 0x00: case 0x0e: case 0x0f:			/* SYNC */
#ifdef GDC_TRACE
		printf("GDC %c SYNC P%u=%02x\n", g == &g_tgdc ? 'T' : 'G', n + 1, (unsigned)v);
#endif
		set_byte(&g->sync[n], v, g, VR_ALL);
		g_vreq.layout = 1;
		break;
	case 0x46:						/* ZOOM */
		set_byte(&g->zoom, v, g, VR_ALL);
		break;
	case 0x47:						/* PITCH */
		set_byte(&g->pitch, v, g, VR_ALL);
		break;
	case 0x49:						/* CURS */
		if (n == 0) g->ead = (g->ead & ~0xffu) | v;
		else if (n == 1) g->ead = (g->ead & ~0xff00u) | ((UINT32)v << 8);
		else {
			g->ead = (g->ead & 0xffffu) | ((UINT32)(v & 3) << 16);
			g->dad = v >> 4;
		}
		if (g == &g_tgdc) g_vreq.text |= VR_CHANGED;	/* the cursor moved */
		break;
	case 0x4a:						/* MASK */
		g->mask = (UINT16)(n ? ((g->mask & 0x00ff) | (v << 8)) : ((g->mask & 0xff00) | v));
		break;
	case 0x4b:						/* CCHAR */
		set_byte(&g->cchar[n], v, g, n == 0 ? VR_ALL : VR_CHANGED);
		break;
	case 0x4c:						/* FIGS */
		g->figs[n] = v;
		break;
	default:
		if ((c & 0xf0) == 0x70) set_byte(&g->pram[(c & 0x0f) + n], v, g, VR_ALL);
		break;
	}
}

static REG8 read_data(Gdc *g)
{
	if (g->rdpos < g->nrd) {
		const UINT8 v = g->rd[g->rdpos++];
		if (g->rdpos >= g->nrd) g->nrd = g->rdpos = 0;
		return v;
	}
	return 0xff;
}

static void t_param_w(UINT port, REG8 v) { (void)port; parameter(&g_tgdc, (UINT8)v); }
static void t_cmd_w(UINT port, REG8 v) { (void)port; command(&g_tgdc, (UINT8)v); }
static REG8 t_status_r(UINT port) { (void)port; return status(&g_tgdc); }
static REG8 t_data_r(UINT port) { (void)port; return read_data(&g_tgdc); }
static void g_param_w(UINT port, REG8 v) { (void)port; parameter(&g_ggdc, (UINT8)v); }
static void g_cmd_w(UINT port, REG8 v) { (void)port; command(&g_ggdc, (UINT8)v); }
static REG8 g_status_r(UINT port) { (void)port; return status(&g_ggdc); }
static REG8 g_data_r(UINT port) { (void)port; return read_data(&g_ggdc); }

/* ---- display control ---- */

static void vsync_irq_w(UINT port, REG8 v)
{
	(void)port;
	(void)v;
	g_vid.vsync_irq = 1;
}

static void mode1_w(UINT port, REG8 v)
{
	const UINT8 bit = (UINT8)(1u << ((v >> 1) & 7));
	const UINT8 old = g_vid.mode1;

	(void)port;
	if (v & 0xf0) return;
	if (v & 1) g_vid.mode1 |= bit;
	else g_vid.mode1 &= (UINT8)~bit;
	if (old == g_vid.mode1) return;
	switch (bit) {
	case M1_ATRSEL: case M1_COL40: case M1_FONT16:
		g_vreq.text |= VR_ALL;
		if (g_vid.mode1 & M1_MONO) g_vreq.grph[0] = g_vreq.grph[1] = VR_ALL;
		break;
	case M1_MONO:
		g_vreq.grph[0] = g_vreq.grph[1] = VR_ALL;
		g_vreq.palette = 1;
		break;
	case M1_HIDEODD:
	case M1_DISP:
		g_vreq.compose = 1;
		break;
	default:
		break;
	}
}

static void mode2_w(UINT port, REG8 v)
{
	UINT8 bit;

	(void)port;
	if (v & 0xf8) return;
	bit = (UINT8)(1u << ((v >> 1) & 3));
	if (bit == M2_ANALOG && !g_vid.has_analog) return;
	if (bit == M2_EGC && (!(g_vid.mode2 & M2_EGCEN) || g_vid.grcg_chip != 3)) return;
	if (v & 1) g_vid.mode2 |= bit;
	else g_vid.mode2 &= (UINT8)~bit;
	if (bit == M2_ANALOG) g_vreq.palette = 1;
	video_map_update();
}

static void disp_page_w(UINT port, REG8 v)
{
	(void)port;
	if ((g_vid.disp_page ^ v) & 1) {
		g_vid.disp_page = v & 1;
		g_vreq.compose = 1;
	}
}

static void access_page_w(UINT port, REG8 v)
{
	(void)port;
	g_vid.access_page = v & 1;
	video_map_update();
}

static void set_pal(UINT8 *p, UINT8 v, UINT8 mask)
{
	if ((*p ^ v) & mask) g_vreq.palette = 1;
	*p = v;
}

static void pal_w(UINT port, REG8 v)
{
	static const UINT8 digital_entry[4] = { 3, 1, 2, 0 };	/* A8h AAh ACh AEh */
	const UINT k = (port >> 1) & 3;

	if (g_vid.mode2 & M2_ANALOG) {
		if (k == 0) g_vid.pal_index = v & 15;
		else set_pal(&g_vid.ana[g_vid.pal_index][k - 1], v & 15, 15);	/* AAh G, ACh R, AEh B */
	}
	else {
		set_pal(&g_vid.digital[digital_entry[k]], (UINT8)v, 0x77);
	}
}

/* ---- reset ---- */

/* SYNC parameters P1..P8 from the timing values (uPD7220 SYNC format):
 * P1 mode, P2 C/R, P3 VS low 3 bits << 5 | HS, P4 HFP << 2 | VS high 2 bits, P5 HBP, P6 VFP,
 * P7 L/F low 8 bits, P8 VBP << 2 | L/F high 2 bits */
static void sync_params(UINT8 *p, UINT mode, UINT cr, UINT hs, UINT hfp, UINT hbp, UINT vs, UINT vfp, UINT vbp, UINT lf)
{
	p[0] = (UINT8)mode;
	p[1] = (UINT8)cr;
	p[2] = (UINT8)(((vs & 7) << 5) | (hs & 0x1f));
	p[3] = (UINT8)(((hfp & 0x3f) << 2) | ((vs >> 3) & 3));
	p[4] = (UINT8)(hbp & 0x3f);
	p[5] = (UINT8)(vfp & 0x3f);
	p[6] = (UINT8)lf;
	p[7] = (UINT8)(((vbp & 0x3f) << 2) | ((lf >> 8) & 3));
}

static void power_on_state(void)
{
	UINT c;

	/* what the BIOS sets up before it hands over (always the 24 kHz monitor: DIP switch 1-1 is fixed on) */
	{
		g_vid.mode1 = M1_DISP | M1_HIDEODD | M1_FONT16;
		/* the standard 400-line values (PC-9801 Programmer's Bible, table 2-27): text GDC mixed mode,
		 * graphics GDC (2.5 MHz) graphics mode with refresh; C/R, HS, HFP, HBP, VS, VFP, VBP, L/F */
		sync_params(g_tgdc.sync, 0x00, 0x4e, 0x07, 0x09, 0x07, 0x08, 0x07, 0x19, 400);
		sync_params(g_ggdc.sync, 0x06, 0x26, 0x03, 0x04, 0x03, 0x08, 0x07, 0x19, 400);
		/* CSRFORM: L/R 0Fh (16 lines), no cursor, blinking, BL 0Ch, CST 0, CFI 0Fh (the book; the BIOS writes the same) */
		g_tgdc.cchar[0] = 0x0f;
		g_tgdc.cchar[1] = (UINT8)((0x0c & 3) << 6);
		g_tgdc.cchar[2] = (UINT8)((0x0f << 3) | (0x0c >> 2));
		g_ggdc.cchar[0] = 0x01;
	}
	g_tgdc.pitch = 80;
	g_ggdc.pitch = 40;
	figs_idle(&g_tgdc);
	figs_idle(&g_ggdc);
	for (c = 0; c < 4; c++) g_vid.digital[c] = (UINT8)((c << 4) | (c + 4));
	/* 16 colours: 0-7 the eight colours at level 7, 8 grey, 9-15 the colours at full level */
	for (c = 0; c < 16; c++) {
		const UINT level = (c < 8) ? 7 : 15;
		g_vid.ana[c][0] = (UINT8)((c & 4) ? level : 0);
		g_vid.ana[c][1] = (UINT8)((c & 2) ? level : 0);
		g_vid.ana[c][2] = (UINT8)((c & 1) ? level : 0);
	}
	g_vid.ana[8][0] = g_vid.ana[8][1] = g_vid.ana[8][2] = 4;
}

void gdc_reset(const Pc98Settings *cfg)
{
	const UINT8 chip = g_vid.grcg_chip;
	UINT8 crtc[6];

	memcpy(crtc, g_vid.crtc, sizeof(crtc));
	memset(&g_tgdc, 0, sizeof(g_tgdc));
	memset(&g_ggdc, 0, sizeof(g_ggdc));
	memset(&g_vid, 0, sizeof(g_vid));
	memcpy(g_vid.crtc, crtc, sizeof(crtc));
	g_vid.grcg_chip = chip;
	g_vid.has_analog = cfg->has_16color & 1;
	power_on_state();
	gdcfig_reset();
	memset(&g_vreq, 0, sizeof(g_vreq));
	g_vreq.text = g_vreq.grph[0] = g_vreq.grph[1] = VR_ALL;
	g_vreq.layout = g_vreq.palette = g_vreq.compose = 1;
	video_map_update();
}

void gdc_bind(void)
{
	static const io_out_fn t_out[8] = { t_param_w, t_cmd_w, vsync_irq_w, NULL, mode1_w, mode2_w, NULL, NULL };
	static const io_in_fn t_in[8] = { t_status_r, t_data_r, NULL, NULL, NULL, NULL, NULL, NULL };
	static const io_out_fn g_out[8] = { g_param_w, g_cmd_w, disp_page_w, access_page_w, pal_w, pal_w, pal_w, pal_w };
	static const io_in_fn g_in[8] = { g_status_r, g_data_r, NULL, NULL, NULL, NULL, NULL, NULL };

	gdc_timing();
	io_sys(0x60, 0xcf1, t_out, t_in, 8);
	io_sys(0xa0, 0xcf1, g_out, g_in, 8);
}

/* ---- state save / load (state.c) ---- */
#include <stateint.h>
void gdc_state(SsCtx *c)
{
	SS_VAR(c, "GDCt", g_tgdc);
	SS_VAR(c, "GDCg", g_ggdc);
	SS_VAR(c, "GDCv", g_vid);
	SS_VAR(c, "GDC0", s_frame_t0);
}
void gdc_fns(const void **t)
{
	t[SF_GDC_RETRACE_START] = (const void *)retrace_start;
	t[SF_GDC_RETRACE_END] = (const void *)retrace_end;
}
