/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * vram.c - CPU access to the text VRAM (A0000h-A4FFFh) and the graphics VRAM (A8000h-BFFFFh, E0000h-E7FFFh).
 *
 * Text VRAM
 *   A0000h-A1FFFh  character codes, a word per cell (4096 cells)
 *   A2000h-A3FFFh  attributes: the even bytes; the odd bytes do not exist (read FFh)
 *   A3FE2h + 4n    the memory switches (n = 0..7): written only while mode flip-flop NVMW is set
 *   A4000h-A4FFFh  the CG window (dev/cgwin.c): even bytes the left half, odd bytes the right half of the selected
 *                  character's line (address bits 4-1); only the odd bytes of a user-defined character can be written
 *   A word access is the two byte accesses, low byte first.
 * Graphics VRAM
 *   Four planes of 32 KB: B A8000h, R B0000h, G B8000h, E E0000h (E only in 16-colour mode).  Two pages; the CPU
 *   reaches the one port A6h selects (page 1 is kept at + GVRAM_PAGE1 in mem[]).  With the GRCG on (7Ch bit 7) one
 *   access reaches the same offset of every plane whose disable bit is clear:
 *     compare read (TCR)       a bit reads 1 where every enabled plane equals its tile register
 *     tile write (TDW)         the enabled planes take their tile register, the CPU data is not used
 *     read-modify-write (RMW)  the CPU data is a bit mask: set bits take the tile, clear bits are kept;
 *                              reads are plain reads
 *   A word access writes the tile byte into both bytes.
 * Every access costs the wait of its kind (g_memwait_tram / VRAM / GRCG); every write marks the renderer's dirty maps. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <video.h>
#include <devices.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define PLANE		0x8000u

EXT_RAM_BSS_ATTR UINT8 g_text_dirty[0x1000];
EXT_RAM_BSS_ATTR UINT8 g_grph_dirty[0x8000] __attribute__((aligned(16)));

static const UINT32 s_plane[4] = { GVRAM_B, GVRAM_R, GVRAM_G, GVRAM_E };

void vram_reset(void)
{
	memset(g_text_dirty, 0, sizeof(g_text_dirty));
	memset(g_grph_dirty, 0, sizeof(g_grph_dirty));
	video_map_update();
}

/* ---- graphics: plain ---- */

static inline void grph_mark(UINT page, UINT32 a)
{
	g_grph_dirty[a & (PLANE - 1)] |= (UINT8)(1u << page);
	g_vreq.grph[page] |= VR_CHANGED;
}

static inline REG8 plain_rd8(UINT page, UINT32 a)
{
	g_cpu.slice_left -= g_memwait_vram;
	return mem[a + page * GVRAM_PAGE1];
}

static inline REG16 plain_rd16(UINT page, UINT32 a)
{
	const UINT8 *p = mem + a + page * GVRAM_PAGE1;

	g_cpu.slice_left -= g_memwait_vram;
	return (REG16)(p[0] | (p[1] << 8));
}

static inline void plain_wr8(UINT page, UINT32 a, REG8 v)
{
	g_cpu.slice_left -= g_memwait_vram;
	mem[a + page * GVRAM_PAGE1] = (UINT8)v;
	grph_mark(page, a);
}

static inline void plain_wr16(UINT page, UINT32 a, REG16 v)
{
	UINT8 *p = mem + a + page * GVRAM_PAGE1;

	g_cpu.slice_left -= g_memwait_vram;
	p[0] = (UINT8)v;
	p[1] = (UINT8)(v >> 8);
	grph_mark(page, a);
	grph_mark(page, a + 1);
}

REG8 gvram_rd8_0(UINT32 a) { return plain_rd8(0, a); }
REG8 gvram_rd8_1(UINT32 a) { return plain_rd8(1, a); }
REG16 gvram_rd16_0(UINT32 a) { return plain_rd16(0, a); }
REG16 gvram_rd16_1(UINT32 a) { return plain_rd16(1, a); }
void gvram_wr8_0(UINT32 a, REG8 v) { plain_wr8(0, a, v); }
void gvram_wr8_1(UINT32 a, REG8 v) { plain_wr8(1, a, v); }
void gvram_wr16_0(UINT32 a, REG16 v) { plain_wr16(0, a, v); }
void gvram_wr16_1(UINT32 a, REG16 v) { plain_wr16(1, a, v); }

/* for the CPU's fast loop, on the page bus.c names in g_gvram_plain (only called while it is 0 or 1) */
unsigned gvram_plain_rd8(UINT32 a) { return plain_rd8((UINT)g_gvram_plain, a); }
void gvram_plain_wr8(UINT32 a, unsigned v) { plain_wr8((UINT)g_gvram_plain, a, (REG8)v); }

/* ---- graphics: GRCG ---- */

static inline UINT grcg_planes(void)
{
	return ~(UINT)g_vid.grcg_mode & 0x0f;
}

static REG16 tcr(UINT page, UINT32 a, UINT bytes)
{
	const UINT en = grcg_planes();
	UINT diff = 0, i, k;

	g_cpu.slice_left -= g_memwait_grcg;
	for (k = 0; k < bytes; k++) {
		const UINT32 o = ((a + k) & (PLANE - 1)) + page * GVRAM_PAGE1;
		for (i = 0; i < 4; i++) {
			if (en & (1u << i)) diff |= (UINT)(mem[s_plane[i] + o] ^ g_vid.grcg_tile[i]) << (8 * k);
		}
	}
	return (REG16)(~diff & (bytes == 2 ? 0xffff : 0xff));
}

static void grcg_write(UINT page, UINT32 a, UINT data, UINT bytes, int rmw)
{
	const UINT en = grcg_planes();
	UINT i, k;

	g_cpu.slice_left -= g_memwait_grcg;
	for (k = 0; k < bytes; k++) {
		const UINT32 o = (a + k) & (PLANE - 1);
		const UINT8 bits = (UINT8)(data >> (8 * k));

		grph_mark(page, o);
		for (i = 0; i < 4; i++) {
			UINT8 *p;
			if (!(en & (1u << i))) continue;
			p = mem + s_plane[i] + o + page * GVRAM_PAGE1;
			*p = rmw ? (UINT8)((*p & ~bits) | (g_vid.grcg_tile[i] & bits)) : g_vid.grcg_tile[i];
		}
	}
}

REG8 grcg_tcr8_0(UINT32 a) { return (REG8)tcr(0, a, 1); }
REG8 grcg_tcr8_1(UINT32 a) { return (REG8)tcr(1, a, 1); }
REG16 grcg_tcr16_0(UINT32 a) { return tcr(0, a, 2); }
REG16 grcg_tcr16_1(UINT32 a) { return tcr(1, a, 2); }
void grcg_tdw8_0(UINT32 a, REG8 v) { grcg_write(0, a, v, 1, 0); }
void grcg_tdw8_1(UINT32 a, REG8 v) { grcg_write(1, a, v, 1, 0); }
void grcg_tdw16_0(UINT32 a, REG16 v) { grcg_write(0, a, v, 2, 0); }
void grcg_tdw16_1(UINT32 a, REG16 v) { grcg_write(1, a, v, 2, 0); }
void grcg_rmw8_0(UINT32 a, REG8 v) { grcg_write(0, a, v, 1, 1); }
void grcg_rmw8_1(UINT32 a, REG8 v) { grcg_write(1, a, v, 1, 1); }
void grcg_rmw16_0(UINT32 a, REG16 v) { grcg_write(0, a, v, 2, 1); }
void grcg_rmw16_1(UINT32 a, REG16 v) { grcg_write(1, a, v, 2, 1); }

/* ---- REP MOVS / STOS into graphics VRAM (the CPU's fast path) ----
 * The same effects as n single writes (plain, TDW or RMW: mode bit 0 the page, bits 2-1 the kind), with the clock
 * debit of the CPU's loop: the first iteration costs acc0, the others acc, each plus the VRAM wait; the run stops
 * after the iteration that uses up the time left above `off`.  A word that would cross the end of a plane is not
 * done here (the bus splits it).  Returns the iterations done. */

unsigned vram_store_run_ref(UINT32 da, int step, unsigned n, unsigned w, const UINT8 *src, UINT32 sa, unsigned val,
			unsigned mode, int32_t acc0, int32_t acc, int32_t *remclk, int32_t off)
{
	const UINT page = mode & 1u, kind = mode >> 1;
	const int32_t wait = (int32_t)(kind ? g_memwait_grcg : g_memwait_vram);
	const UINT en = grcg_planes();
	unsigned k = 0, b, i;

	while (k < n) {
		UINT data = src ? src[sa] | (w == 2 ? (UINT)src[sa + 1] << 8 : 0) : (w == 2 ? val & 0xffff : val & 0xff);

		if (w == 2 && (da & (PLANE - 1)) == PLANE - 1) break;
		for (b = 0; b < w; b++) {
			const UINT32 o = (da + b) & (PLANE - 1);
			const UINT8 bits = (UINT8)(data >> (8 * b));

			grph_mark(page, o);
			if (kind == 0) {
				mem[da + b + page * GVRAM_PAGE1] = bits;
				continue;
			}
			for (i = 0; i < 4; i++) {
				UINT8 *p;
				if (!(en & (1u << i))) continue;
				p = mem + s_plane[i] + o + page * GVRAM_PAGE1;
				*p = (kind == 2) ? (UINT8)((*p & ~bits) | (g_vid.grcg_tile[i] & bits)) : g_vid.grcg_tile[i];
			}
		}
		sa += (UINT32)step;
		da += (UINT32)step;
		k++;
		*remclk -= wait + (k == 1 ? acc0 : acc);
		if (*remclk - off <= 0) break;
	}
	return k;
}

/* marks [o, o + len) of the dirty map, which stays inside one plane */
static void mark_range(UINT page, UINT32 o, UINT32 len)
{
	const UINT8 bit = (UINT8)(1u << page);
	const UINT32 pat = 0x01010101u * bit;
	UINT8 *p = g_grph_dirty + o;

	while (len && ((uintptr_t)p & 3)) { *p++ |= bit; len--; }
	while (len >= 4) { *(UINT32 *)p |= pat; p += 4; len -= 4; }
	while (len) { *p++ |= bit; len--; }
	g_vreq.grph[page] |= VR_CHANGED;
}

unsigned vram_store_run(UINT32 da, int step, unsigned n, unsigned w, const UINT8 *src, UINT32 sa, unsigned val,
			unsigned mode, int32_t acc0, int32_t acc, int32_t *remclk, int32_t off)
{
	const UINT page = mode & 1u, kind = mode >> 1;
	UINT8 *const base = mem + page * GVRAM_PAGE1;
	const int32_t wait = (int32_t)(kind ? g_memwait_grcg : g_memwait_vram);
	const int32_t c0 = wait + acc0, c = wait + acc, room = *remclk - off;
	unsigned kmax, left;

	if (step < 0 || (w == 2 && (da & 1)) || !n || c <= 0) {
		return vram_store_run_ref(da, step, n, w, src, sa, val, mode, acc0, acc, remclk, off);
	}
	kmax = room <= c0 ? 1u : 1u + (unsigned)((room - c0 + c - 1) / c);
	if (n > kmax) n = kmax;
	left = n;
	while (left) {
		const UINT32 o = da & (PLANE - 1);
		unsigned it = (PLANE - o) / w, len, i, j;

		if (it > left) it = left;
		len = it * w;
		if (kind == 0) {
			if (src) memcpy(base + da, src + sa, len);
			else if (w == 1 || ((val ^ (val >> 8)) & 0xff) == 0) memset(base + da, (int)(val & 0xff), len);
			else for (j = 0; j < it; j++) { base[da + j * 2] = (UINT8)val; base[da + j * 2 + 1] = (UINT8)(val >> 8); }
		}
		else {
			const UINT en = grcg_planes();
			for (i = 0; i < 4; i++) {
				UINT8 *p = base + s_plane[i] + o;
				const UINT8 t = g_vid.grcg_tile[i];
				if (!(en & (1u << i))) continue;
				if (kind == 1) {
					memset(p, t, len);
					continue;
				}
				for (j = 0; j < len; j++) {
					const UINT8 bits = src ? src[sa + j] : (UINT8)(w == 2 && (j & 1) ? val >> 8 : val);
					p[j] = (UINT8)((p[j] & ~bits) | (t & bits));
				}
			}
		}
		mark_range(page, o, len);
		da += len;
		sa += len;
		left -= it;
	}
	*remclk -= c0 + (int32_t)(n - 1) * c;
	return n;
}

/* ---- text VRAM ---- */

#define T_CODE_END	0xa2000u
#define T_ATTR_END	0xa4000u
#define T_CG_END	0xa5000u

static UINT8 *cg_byte(UINT32 a)
{
	return cgrom + ((a & 1) ? g_cgwin.odd : g_cgwin.even) + ((a >> 1) & 0x0f);
}

static UINT8 t_peek(UINT32 a)
{
	if (a < T_CODE_END) return mem[a];
	if (a < T_ATTR_END) return (a & 1) ? 0xff : mem[a];
	if (a < T_CG_END) return *cg_byte(a);
	return 0xff;
}

static void t_poke(UINT32 a, UINT8 v)
{
	if (a < T_CODE_END) {
		mem[a] = v;
		g_text_dirty[(a >> 1) & 0x0fff] |= TD_CODE;
		g_vreq.text |= VR_CHANGED;
	}
	else if (a < T_ATTR_END) {
		if (a & 1) return;
		if (a >= 0xa3fe0u && (a & 2) && !(g_vid.mode1 & M1_NVMW)) return;	/* memory switch, protected */
		mem[a] = v;
		g_text_dirty[(a >> 1) & 0x0fff] |= TD_CODE | TD_ATTR;
		g_vreq.text |= VR_CHANGED;
	}
	else if (a < T_CG_END) {
		if ((a & 1) && (g_cgwin.flags & CGWIN_WRITABLE)) {
			*cg_byte(a) = v;
			g_cgwin.flags |= CGWIN_WRITTEN;
			g_vreq.text |= VR_ALL;	/* every cell showing the character changes */
		}
	}
}

REG8 tvram_rd8(UINT32 a)
{
	g_cpu.slice_left -= g_memwait_tram;
	return t_peek(a);
}

REG16 tvram_rd16(UINT32 a)
{
	g_cpu.slice_left -= g_memwait_tram;
	return (REG16)(t_peek(a) | (t_peek(a + 1) << 8));
}

void tvram_wr8(UINT32 a, REG8 v)
{
	g_cpu.slice_left -= g_memwait_tram;
	t_poke(a, (UINT8)v);
}

void tvram_wr16(UINT32 a, REG16 v)
{
	g_cpu.slice_left -= g_memwait_tram;
	t_poke(a, (UINT8)v);
	t_poke(a + 1, (UINT8)(v >> 8));
}
