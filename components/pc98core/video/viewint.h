/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * viewint.h - what the renderer's parts (textview.c, grphview.c, compose.c) share. */
#ifndef PC98_VIEWINT_H
#define PC98_VIEWINT_H

#include <compiler.h>
#include <video.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif
#endif

#define VIEW_LINES	SURFACE_HEIGHT

/* pixel buffers, 640 bytes per raster:
 *   text      0 = nothing, else (colour + 1) << 4
 *   graphics  the 4-bit pixel value (monochrome mode: the text colour 0-7 of lit pixels)
 * line_dirty[y]: LD_TEXT / LD_G0 / LD_G1 the raster changed, LD_ALL everything */
#define LD_G0		0x01
#define LD_G1		0x02
#define LD_TEXT		0x04
#define LD_ALL		0x80
extern UINT8 g_tpix[SURFACE_WIDTH * VIEW_LINES];
extern UINT8 g_gpix[2][SURFACE_WIDTH * VIEW_LINES];
extern UINT8 g_line_dirty[VIEW_LINES];
extern UINT8 g_tcolor[VIEW_LINES][80];		/* text colour of each cell column per raster (monochrome graphics) */

/* the rasters of each screen inside the buffers */
typedef struct {
	UINT	t_top, t_lines;			/* text: first raster, rasters */
	UINT	g_top, g_lines;
	UINT	lines;				/* rasters shown */
} ViewGeom;
extern ViewGeom g_geom;

void	textview_init(void);
void	textview_draw(int all, int blink_phase, int cursor_on);
void	grphview_init(void);
void	grphview_draw(UINT page, int all);
void	compose_palette(void);
void	compose_frame(int all);

#endif
