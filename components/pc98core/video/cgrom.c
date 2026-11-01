/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * cgrom.c - the character generator ROM image (cgrom): setup and loading.
 *
 * cgrom layout (CGROM_SIZE = 84000h bytes; the renderer, the CG window and main/ read it directly)
 *   kanji  ku = first JIS byte - 20h (01h-5Dh), ten = second JIS byte (21h-7Eh):
 *            left half  16 lines at (ten << 12) | (ku << 4), right half at +800h.
 *            ku 56h / 57h (gaiji) are user-writable and are never cleared by a load.
 *   ANK 8x16        80000h + code * 16
 *   semigraphic 8x16 81000h + code * 16 (built by the hardware from the code bits)
 *   8x8             82000h + code * 16: first 8 bytes = ANK 8x8, next 8 = semigraphic 8x8
 *
 * cgrom_load() reads a T98-Next format FONT.ROM (46800h bytes, layout in rom_src/mkfont_esp.py):
 *   0000h 8x8 ANK x 256, 0800h 8x16 ANK 00-7Fh, 1000h 8x16 ANK 80-FFh,
 *   1800h kanji: per ku (starting at 01h) 60h characters (ten 20h-7Fh) x 32 bytes (left 16 + right 16).
 * Only that format is supported.  If the named file cannot be opened or is not 46800h bytes the
 * built-in ROM image (":builtin/FONT.ROM") is read instead.
 *
 * The built-in ROM is always read first.  A font file named by the user is read over it, and the ranges
 * a file may leave blank (the PC-98 half-width graphics 80h-9Fh / E0h-FFh, the two-byte half-width
 * characters 29xx-2Cxx) are taken back from the built-in ROM when the file has nothing there.
 *
 */
#include <compiler.h>
#include <hostfile.h>
#include <cpulink.h>
#include <pc98.h>
#include <video.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#define FONT_FILE_SIZE		0x46800u
#define FONT_LOADED		1		/* cgrom_load(): a font is in cgrom */
#define FONT_NONE		0
#define FONT_BUILTIN		":builtin/FONT.ROM"

#define KU_BYTES		(0x60 * 32)		/* 60h characters x (16 + 16) bytes */
#define KU_LAST			0x5c			/* the file holds ku 01h-5Ch */

/* ---- setup ---- */

void cgrom_init(void)
{
	ZeroMemory(cgrom, CGROM_SIZE);
	cgrom_semigraphics();
}

/* The semigraphic characters: for code c, bit j (0-3) is the left block and bit j+4 the right block
 * of the j-th block row from the top.  A block row is 4 lines of an 8x16 cell and 2 lines of an 8x8
 * cell; left column = F0h, right column = 0Fh. */
void cgrom_semigraphics(void)
{
	UINT code, j, k;

	for (code = 0; code < 256; code++) {
		UINT8 *g16 = cgrom + 0x81000 + code * 16;
		UINT8 *g8 = cgrom + 0x82000 + code * 16 + 8;
		for (j = 0; j < 4; j++) {
			UINT8 v = 0;
			if (code & (0x01u << j)) {
				v |= 0xf0;
			}
			if (code & (0x10u << j)) {
				v |= 0x0f;
			}
			for (k = 0; k < 4; k++) {
				g16[j * 4 + k] = v;
			}
			for (k = 0; k < 2; k++) {
				g8[j * 2 + k] = v;
			}
		}
	}
}

/* ---- loading ---- */

static BOOL area_is_blank(UINT32 off, UINT len)
{
	UINT i;
	for (i = 0; i < len; i++) {
		if (cgrom[off + i]) {
			return FALSE;
		}
	}
	return TRUE;
}

/* The glyph a file would give for the 8x8 ANK `code` is stored at 82000h + code * 16. */
static void ank8_put(const UINT8 *src, UINT code, UINT count)
{
	UINT i;
	for (i = 0; i < count; i++) {
		CopyMemory(cgrom + 0x82000 + (code + i) * 16, src + i * 8, 8);
	}
}

/* Reads a whole T98-Next font from `fh` into cgrom.  Returns FALSE if it is not one. */
static BOOL read_t98(HFILE fh)
{
	/* PSRAM: internal RAM on the Tab5 has only a few tens of KB to spare */
	static EXT_RAM_BSS_ATTR UINT8 buf[KU_BYTES];
	UINT ku, ten;

	if (hf_size(fh) != FONT_FILE_SIZE) {
		return FALSE;
	}
	/* 0000h: 8x8 ANK x 256 */
	if (hf_read(fh, buf, 0x800) != 0x800) {
		return FALSE;
	}
	ank8_put(buf, 0, 256);
	/* 0800h / 1000h: 8x16 ANK 00-7Fh and 80-FFh, contiguous in cgrom as well */
	if (hf_read(fh, cgrom + 0x80000, 0x1000) != 0x1000) {
		return FALSE;
	}
	/* 1800h: kanji, ku 01h-5Ch in order; the gaiji ku 56h / 57h are in the file but not loaded */
	for (ku = 1; ku <= KU_LAST; ku++) {
		if (hf_read(fh, buf, KU_BYTES) != KU_BYTES) {
			return FALSE;
		}
		if (ku == 0x56 || ku == 0x57) {
			continue;
		}
		for (ten = 0x20; ten < 0x80; ten++) {
			UINT8 *dst = cgrom + (ten << 12) + (ku << 4);
			const UINT8 *src = buf + (ten - 0x20) * 32;
			CopyMemory(dst, src, 16);
			CopyMemory(dst + 0x800, src + 16, 16);
		}
	}
	return TRUE;
}

static BOOL load_file(const char *name)
{
	BOOL ok;
	HFILE fh;

	if (name == NULL || name[0] == '\0') {
		return FALSE;
	}
	fh = hf_open(name);
	if (fh == NULL) {
		return FALSE;
	}
	ok = read_t98(fh);
	hf_close(fh);
	return ok;
}

/* The ranges a font file may leave out, as (offset, length, stride, count) runs in cgrom:
 * ANK 8x16 80h-9Fh and E0h-FFh, ANK 8x8 of the same codes, and JIS rows 09h-0Ch (both halves). */
typedef struct { UINT32 off; UINT16 len, stride, count; } FillRange;
static const FillRange s_fill[] = {
	{ 0x80800, 0x200, 0, 1 }, { 0x80e00, 0x200, 0, 1 },
	{ 0x82000 + 0x80 * 16, 8, 16, 0x20 }, { 0x82000 + 0xe0 * 16, 8, 16, 0x20 },
	{ (0x21 << 12) + (0x09 << 4), 16, 0x1000, 0x5e }, { (0x21 << 12) + (0x09 << 4) + 0x800, 16, 0x1000, 0x5e },
	{ (0x21 << 12) + (0x0a << 4), 16, 0x1000, 0x5e }, { (0x21 << 12) + (0x0a << 4) + 0x800, 16, 0x1000, 0x5e },
	{ (0x21 << 12) + (0x0b << 4), 16, 0x1000, 0x5e }, { (0x21 << 12) + (0x0b << 4) + 0x800, 16, 0x1000, 0x5e },
	{ (0x21 << 12) + (0x0c << 4), 16, 0x1000, 0x5e }, { (0x21 << 12) + (0x0c << 4) + 0x800, 16, 0x1000, 0x5e },
};
#define FILL_BYTES	(2 * 0x200 + 2 * 0x20 * 8 + 8 * 0x5e * 16)

/* cgrom <-> save buffer for every fill range; to_save = 1 copies out of cgrom. */
static void fill_copy(UINT8 *save, int to_save)
{
	UINT r, k;
	for (r = 0; r < NELEMENTS(s_fill); r++) {
		for (k = 0; k < s_fill[r].count; k++) {
			UINT8 *f = cgrom + s_fill[r].off + k * s_fill[r].stride;
			if (to_save) CopyMemory(save, f, s_fill[r].len);
			else CopyMemory(f, save, s_fill[r].len);
			save += s_fill[r].len;
		}
	}
}

/* A range that is blank after the user's file gets the built-in ROM's glyphs back (save = the built-in ranges). */
static void fill_missing(const UINT8 *save)
{
	UINT r, k;
	for (r = 0; r < NELEMENTS(s_fill); r++) {
		BOOL blank = TRUE;
		for (k = 0; k < s_fill[r].count && blank; k++) {
			blank = area_is_blank(s_fill[r].off + k * s_fill[r].stride, s_fill[r].len);
		}
		for (k = 0; k < s_fill[r].count; k++) {
			if (blank) CopyMemory(cgrom + s_fill[r].off + k * s_fill[r].stride, save, s_fill[r].len);
			save += s_fill[r].len;
		}
	}
}

int cgrom_load(const char *filename)
{
	static EXT_RAM_BSS_ATTR UINT8 save[FILL_BYTES];
	UINT i;
	UINT8 *q;


	/* kanji area, except the gaiji rows (000h-55Fh, 580h-D5Fh, D80h-FFFh of each 4KB block), and the ANK faces */
	for (i = 0; i < 0x80; i++) {
		q = cgrom + (i << 12);
		ZeroMemory(q + 0x000, 0x560 - 0x000);
		ZeroMemory(q + 0x580, 0xd60 - 0x580);
		ZeroMemory(q + 0xd80, 0x1000 - 0xd80);
	}
	ZeroMemory(cgrom + 0x80000, 0x1000);
	for (i = 0; i < 256; i++) {
		ZeroMemory(cgrom + 0x82000 + i * 16, 8);
	}

	if (!load_file(FONT_BUILTIN)) {
		return load_file(filename) ? FONT_LOADED : FONT_NONE;
	}
	if (filename == NULL || filename[0] == '\0' || !strcmp((const char *)filename, FONT_BUILTIN)) {
		return FONT_LOADED;
	}
	fill_copy(save, 1);
	if (load_file(filename)) {
		fill_missing(save);
	}
	return FONT_LOADED;
}
