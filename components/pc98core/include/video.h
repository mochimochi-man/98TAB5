/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * video.h - the display side of the machine:
 *   video/gdc.c       the two uPD7220 GDCs (text 60h-62h, graphics A0h-A2h), the display control ports (64h-6Eh,
 *                     A4h-AEh) and the frame timing
 *   video/gdcfig.c    the graphics GDC's drawing (figures, graphic characters, WDAT)
 *   video/crtc.c      the text row registers (70h-7Ah) and the GRCG (7Ch, 7Eh)
 *   video/vram.c      CPU access to text VRAM and graphics VRAM (plain and through the GRCG)
 *   video/egc.c       the EGC (4A0h-4AEh) and CPU access to graphics VRAM through it
 *   video/cgrom.c     the character generator ROM image
 *   video/textview.c, grphview.c, compose.c   the renderer: text and graphics into pixel buffers, the RGB565 composite
 */
#ifndef PC98_VIDEO_H
#define PC98_VIDEO_H

#include "compiler.h"
#include "memmap.h"
#include <events.h>
#include "host.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Pc98Settings;

/* ======================================================================== GDC */

#define GDC_FIFO	16

typedef struct {
	/* what the commands set */
	UINT8	sync[8];	/* SYNC */
	UINT8	zoom;		/* ZOOM: bits 7-4 display zoom - 1, bits 3-0 graphic character zoom - 1 */
	UINT32	ead;		/* CSRW: execute (cursor) address, 18 bits of words */
	UINT8	dad;		/* CSRW: the dot within the word, 0-15 */
	UINT16	mask;		/* MASK */
	UINT8	cchar[3];	/* CCHAR: lines per row - 1, cursor lines and blink rate */
	UINT8	figs[11];	/* FIGS: direction and figure, DC, D, D2, D1, DM (14 bits each, little endian) */
	UINT8	pram[16];	/* PRAM: 4 display partitions (SAD 18 bits, LEN 10 bits, WD, IM), pattern bytes 8-15 */
	UINT8	pitch;		/* PITCH: words per line */
	UINT8	enabled;	/* the display is on (START, or SYNC with DE set) */
	UINT8	drawing;	/* graphics GDC: a figure is being drawn */
	/* the command interface */
	UINT16	fifo[GDC_FIFO];	/* bit 8 set: a command */
	UINT8	nfifo;
	UINT8	cmd;		/* the command the next parameter bytes belong to */
	UINT8	nparam;		/* parameter bytes it has had */
	UINT8	rd[5];		/* bytes to be read back (CURD, LPRD) */
	UINT8	nrd, rdpos;
	UINT8	wdat[2];
} Gdc;

extern Gdc g_tgdc;		/* text, the master */
extern Gdc g_ggdc;		/* graphics, the slave */

/* the parameter fields as numbers */
#define GDC_AW(g)	((UINT)(g)->sync[1] + 2)				/* active words (characters) per line */
#define GDC_AL(g)	((((UINT)(g)->sync[6] | (((UINT)(g)->sync[7] & 3) << 8)) - 1) % 1024 + 1)	/* active lines */
#define GDC_VBP(g)	((UINT)(g)->sync[7] >> 2)
#define GDC_LR(g)	((UINT)(g)->cchar[0] & 0x1f)			/* lines per character row - 1 */
#define GDC_FIG(g, i)	((UINT)(g)->figs[1 + 2 * (i)] | (((UINT)(g)->figs[2 + 2 * (i)] & 0x3f) << 8))	/* 0 DC 1 D 2 D2 3 D1 4 DM */
#define GDC_SAD(g, p)	((UINT32)(g)->pram[(p) * 4] | ((UINT32)(g)->pram[(p) * 4 + 1] << 8) | (((UINT32)(g)->pram[(p) * 4 + 2] & 3) << 16))
#define GDC_LEN(g, p)	(((UINT)(g)->pram[(p) * 4 + 2] >> 4) | (((UINT)(g)->pram[(p) * 4 + 3] & 0x3f) << 4))
#define GDC_WD(g, p)	(((g)->pram[(p) * 4 + 3] >> 6) & 1)

/* ======================================================================== display control */

/* mode flip-flops of port 68h (bit n = flip-flop n) */
#define M1_ATRSEL	0x01		/* attribute bit 4: 0 vertical line, 1 semigraphics */
#define M1_MONO		0x02		/* graphics: monochrome, coloured by the text attributes */
#define M1_COL40	0x04		/* text: 40 columns */
#define M1_FONT16	0x08		/* text: the 8 x 16 font (else 8 x 8) */
#define M1_HIDEODD	0x10		/* graphics: the odd lines are dark (200-line display) */
#define M1_KAC		0x20		/* kanji access mode of the CG ROM ports */
#define M1_NVMW		0x40		/* the memory switches may be written */
#define M1_DISP		0x80		/* display on */
/* mode flip-flops of port 6Ah */
#define M2_ANALOG	0x01		/* 16-colour mode */
#define M2_EGC		0x04		/* the EGC replaces the GRCG */
#define M2_EGCEN	0x08		/* M2_EGC may be changed */

typedef struct {
	UINT8	mode1, mode2;
	UINT8	disp_page;		/* A4h: the graphics page shown */
	UINT8	access_page;		/* A6h: the graphics page the CPU reaches */
	UINT8	has_analog;		/* the 16-colour board is fitted */
	UINT8	pal_index;		/* A8h in 16-colour mode */
	UINT8	digital[4];		/* digital palette: [i] = colour i (bits 6-4) and colour i + 4 (bits 2-0) */
	UINT8	ana[16][3];		/* 16-colour palette: G, R, B, 4 bits each */
	UINT8	vsync;			/* 20h during the vertical retrace (status bit 5) */
	UINT8	vsync_irq;		/* 64h: interrupt at the next retrace */
	UINT8	crtc[6];		/* 70h-7Ah: PL BL CL SSL SUR SDR, 5 bits each */
	UINT8	grcg_chip;		/* 0 none, 1 GRCG, 2 GRCG with CG window, 3 EGC */
	UINT8	grcg_mode;		/* 7Ch: bit 7 on, bit 6 RMW (else TDW / TCR), bits 3-0 plane disable (E G R B) */
	UINT8	grcg_tile[4];		/* 7Eh: B R G E */
	UINT8	grcg_next;
	UINT32	raster_clk;		/* CPU clocks per raster */
	UINT32	hblank_clk;		/* ... of which the horizontal blanking */
	UINT32	disp_clk;		/* display period of a frame */
	UINT32	retrace_clk;		/* vertical retrace period */
} VideoCtl;

extern VideoCtl g_vid;

/* what the renderer has to do (set by the devices, cleared by the renderer) */
#define VR_CHANGED	0x01		/* some bytes changed: see the dirty maps */
#define VR_ALL		0x02		/* everything changed */
typedef struct {
	UINT8	text;
	UINT8	grph[2];		/* graphics page 0 / 1 */
	UINT8	layout;			/* display geometry or timing */
	UINT8	palette;
	UINT8	compose;		/* what is shown changed (page, on / off) */
} VideoReq;
extern VideoReq g_vreq;

/* dirty maps: text by cell (TD_CODE: code or attribute, TD_ATTR: attribute), graphics by plane byte (bit = page) */
#define TD_CODE		0x01
#define TD_ATTR		0x02
extern UINT8 g_text_dirty[0x1000];
extern UINT8 g_grph_dirty[0x8000];

void	gdc_reset(const struct Pc98Settings *cfg);
void	gdc_bind(void);
void	gdc_timing(void);			/* recompute the frame timing (also after a CPU clock change) */
void	gdc_frame_start(void);			/* start of a frame: arms the end of its display period */
void	video_map_update(void);			/* the CPU's graphics VRAM window follows the GRCG / EGC / page state */

/* gdcfig.c */
void	gdcfig_init(void);
void	gdcfig_draw(int character);		/* FIGD (0) / GCHRD (1) with the graphics GDC's parameters */
void	gdcfig_wdat(UINT16 data);		/* one WDAT unit */
void	gdcfig_reset(void);
void	gdcfig_mode(UINT mod);			/* the operation bits of a WDAT command */

/* crtc.c */
void	crtc_reset(const struct Pc98Settings *cfg);
void	crtc_bind(void);

/* ======================================================================== CPU access to VRAM (vram.c, egc.c) */

/* the graphics window's map (bus.c bus_set_vram_access) */
#define VMAP_PAGE1	0x01
#define VMAP_EGC	0x02
#define VMAP_RMW	0x04
#define VMAP_GRCG	0x08
#define VMAP_EPLANE	0x10

void	vram_reset(void);
REG8	gvram_rd8_0(UINT32 a);
REG8	gvram_rd8_1(UINT32 a);
REG16	gvram_rd16_0(UINT32 a);
REG16	gvram_rd16_1(UINT32 a);
void	gvram_wr8_0(UINT32 a, REG8 v);
void	gvram_wr8_1(UINT32 a, REG8 v);
void	gvram_wr16_0(UINT32 a, REG16 v);
void	gvram_wr16_1(UINT32 a, REG16 v);
REG8	grcg_tcr8_0(UINT32 a);
REG8	grcg_tcr8_1(UINT32 a);
REG16	grcg_tcr16_0(UINT32 a);
REG16	grcg_tcr16_1(UINT32 a);
void	grcg_tdw8_0(UINT32 a, REG8 v);
void	grcg_tdw8_1(UINT32 a, REG8 v);
void	grcg_tdw16_0(UINT32 a, REG16 v);
void	grcg_tdw16_1(UINT32 a, REG16 v);
void	grcg_rmw8_0(UINT32 a, REG8 v);
void	grcg_rmw8_1(UINT32 a, REG8 v);
void	grcg_rmw16_0(UINT32 a, REG16 v);
void	grcg_rmw16_1(UINT32 a, REG16 v);
REG8	tvram_rd8(UINT32 a);
REG16	tvram_rd16(UINT32 a);
void	tvram_wr8(UINT32 a, REG8 v);
void	tvram_wr16(UINT32 a, REG16 v);
unsigned vram_store_run(UINT32 da, int step, unsigned n, unsigned w, const UINT8 *src, UINT32 sa, unsigned val,
			unsigned mode, int32_t acc0, int32_t acc, int32_t *remclk, int32_t off);

void	egc_reset(const struct Pc98Settings *cfg);
void	egc_bind(void);
REG8	egc_rd8(UINT32 a);
REG16	egc_rd16(UINT32 a);
void	egc_wr8(UINT32 a, REG8 v);
void	egc_wr16(UINT32 a, REG16 v);
void	egc_gdc_dot(UINT page, UINT32 plane_off, UINT16 bits);	/* the graphics GDC sets dots through the EGC */

/* ======================================================================== CG ROM (cgrom.c) */

#define CGROM_SIZE	0x84000
#define cgrom		(mem + CGROM_AT)
#define CG_ANK16	0x80000		/* 8 x 16 characters: code * 16 */
#define CG_SEMI16	0x81000		/* 8 x 16 semigraphics */
#define CG_ANK8		0x82000		/* 8 x 8 characters: code * 16, the 8 x 8 semigraphics at + 8 */
void	cgrom_init(void);
void	cgrom_semigraphics(void);
int	cgrom_load(const char *filename);

/* ======================================================================== renderer */

#define SURFACE_WIDTH	640
#define SURFACE_HEIGHT	480

void	video_init(void);
void	video_reset(void);
void	video_frame(void);			/* once per emulated frame */
void	video_redraw_all(void);
void	video_set_line200(int mode);		/* 200-line graphics: 0 dark odd lines, 1 doubled lines, 2 interpolated */
extern int (*g_comp_async)(void (*fn)(const void *), const void *arg);	/* run the composite elsewhere (firmware: core 0) */

#ifdef __cplusplus
}
#endif

#endif
