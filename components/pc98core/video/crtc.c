/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * crtc.c - the text row registers and the GRCG (port block 70h-7Eh, every even port; A8 and A9 are not decoded).
 *
 *   70h PL   the cell line shown on the first raster of a row (5 bits; 16-31 count as -16..-1)
 *   72h BL   the cell line shown on the last raster of a row
 *   74h CL   font lines drawn (lines past it are blank)
 *   76h SSL  smooth scroll: rasters the scroll area is moved up by
 *   78h SUR  smooth scroll: the area starts 32 - SUR rows from the top (0: at the top)
 *   7Ah SDR  smooth scroll: rows in the area - 1
 *   7Ch      GRCG mode: bit 7 on, bit 6 1 = read-modify-write, 0 = tile write / compare read; bits 3-0 disable
 *            planes E G R B.  Writing it restarts the tile sequence.
 *   7Eh      GRCG tiles: four writes load B, R, G, E.
 * All are write-only. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <io.h>
#include <devices.h>
#include <video.h>

static void crtc_w(UINT port, REG8 v)
{
	const UINT i = (port >> 1) & 7;

	v &= 0x1f;
	if (g_vid.crtc[i] != v) {
		g_vid.crtc[i] = (UINT8)v;
		g_vreq.text |= VR_ALL;
	}
}

static void grcg_mode_w(UINT port, REG8 v)
{
	(void)port;
	if (!g_vid.grcg_chip) return;
	g_vid.grcg_mode = (UINT8)v;
	g_vid.grcg_next = 0;
	video_map_update();
}

static void grcg_tile_w(UINT port, REG8 v)
{
	(void)port;
	if (!g_vid.grcg_chip) return;
	g_vid.grcg_tile[g_vid.grcg_next] = (UINT8)v;
	g_vid.grcg_next = (g_vid.grcg_next + 1) & 3;
}

void crtc_reset(const Pc98Settings *cfg)
{
	memset(g_vid.crtc, 0, sizeof(g_vid.crtc));
	g_vid.crtc[1] = 15;	/* rows of 16 rasters (the 24 kHz monitor) */
	g_vid.crtc[2] = 16;
	g_vid.grcg_chip = cfg->gfx_chip & 3;
	g_vid.grcg_mode = 0;
	g_vid.grcg_next = 0;
	memset(g_vid.grcg_tile, 0, sizeof(g_vid.grcg_tile));
}

void crtc_bind(void)
{
	static const io_out_fn out[8] = { crtc_w, crtc_w, crtc_w, crtc_w, crtc_w, crtc_w, grcg_mode_w, grcg_tile_w };

	io_sys(0x70, 0xcf1, out, NULL, 8);
}
