/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * memmap.h - the machine's memory image (bus.c owns mem[]) and the CPU-side memory access.
 *
 * mem[] layout (2MB):
 *   000000h-10FFEFh  main memory, the ROM area (E8000h-FFFFFh: BIOS bank) and the HMA
 *   110000h-193FFFh  CG ROM image (video/cgrom.c)
 *   1A8000h-1BFFFFh, 1E0000h-1E7FFFh  graphics VRAM page 1 (page 0 + GVRAM_PAGE1)
 *   1C0000h-1C7FFFh  ITF image mirror, 1E8000h-1F7FFFh  BIOS image mirror (rom/rom.c)
 *   1F8000h-1FFFFFh  the ITF bank of F8000h-FFFFFh */
#ifndef PC98_MEMMAP_H
#define PC98_MEMMAP_H

#include "compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

extern UINT8 mem[0x200000];

#define HIMEM_END	0x110000u		/* first address above the HMA */
#define GVRAM_PAGE1	0x100000u		/* graphics VRAM page 1 = page 0 + this */
#define GVRAM_B		0x0a8000u		/* the planes of page 0: blue, red, green, and the fourth (16 colours) */
#define GVRAM_R		0x0b0000u
#define GVRAM_G		0x0b8000u
#define GVRAM_E		0x0e0000u
#define GVRAM1_B	(GVRAM_B + GVRAM_PAGE1)
#define GVRAM1_E	(GVRAM_E + GVRAM_PAGE1)
#define CGROM_AT	0x110000u		/* the character generator image */
#define ITF_AT		0x1f8000u		/* the ITF bank */

/* wait states (clocks) added to the CPU by text VRAM, graphics VRAM and GRCG accesses */
extern UINT8 g_memwait_tram, g_memwait_vram, g_memwait_grcg;

/* physical address access through the memory map */
REG8	bus_rd8(UINT32 address);
REG16	bus_rd16(UINT32 address);
UINT32	bus_rd32(UINT32 address);
void	bus_wr8(UINT32 address, REG8 value);
void	bus_wr16(UINT32 address, REG16 value);
void	bus_wr32(UINT32 address, UINT32 value);
void	bus_read_block(UINT32 address, void *dst, UINT leng);
void	bus_write_block(UINT32 address, const void *src, UINT leng);

/* memory map switches: graphics VRAM access mode (video/vram.c) and the machine type */
void	bus_set_vram_access(UINT operate);
extern int g_gvram_plain;	/* page of plain graphics VRAM access at A8000h-BFFFFh, or -1 */
void	bus_set_map(UINT type);
/* write mode of graphics VRAM at an address (REP store fast path), -1 = not graphics VRAM */
int	bus_vram_write_mode(UINT32 address);

#ifdef __cplusplus
}
#endif

#endif
