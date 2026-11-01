/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * cpulink.h - what the CPU (cpu/, adapter cpuadapt.c) and the rest of the machine share (cpu/cpustate.c owns it):
 * the clock budget of the running slice, the address mask (A20), a reset request, the ITF ROM bank, the D0000h RAM
 * switch, the CPU kind, extended memory, and a copy of the registers that the adapter refreshes at every slice
 * boundary (diagnostics and the boot code read it). */
#ifndef PC98_CPULINK_H
#define PC98_CPULINK_H

#include "compiler.h"
#include "memmap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	UINT16	ax, cx, dx, bx, sp, bp, si, di;
	UINT16	es, cs, ss, ds;
	UINT32	es_base, cs_base, ss_base, ds_base;
	UINT16	ip, flags, msw;
} CpuRegCopy;

/* the 386 part of CPU menu 2: upper halves of EAX..EDI, FS / GS, FLAGS bits 12-14 */
typedef struct {
	int	enabled;		/* 1 = menu 2 (286 + 386 extensions) */
	UINT16	flags_hi;
	UINT16	hi[8];
	UINT16	fs, gs;
	UINT32	fs_base, gs_base;
} Cpu386Copy;

typedef struct {
	CpuRegCopy	reg;
	Cpu386Copy	x386;
	/* time: clocks left in the running slice, the slice's length, and the clocks of the slices before */
	SINT32		slice_left;
	SINT32		slice_len;
	UINT32		clocks_done;
	UINT32		addr_mask;	/* FFFFFh with A20 off */
	UINT8		reset_pending;
	UINT8		itf_rom;	/* 043Dh: the ITF ROM is mapped at F8000h */
	UINT8		v30;		/* 1 = V30, 0 = 80286 */
	UINT16		d000_ram;	/* bit n: D0000h + n * 4KB is RAM */
	/* extended memory above 1MB, and the four 16KB EMS frames at C0000h */
	UINT8		*ext;
	UINT32		ext_size;
	UINT8		*ext_at1m;	/* ext - 100000h: indexed by the linear address */
	UINT32		ext_end16;	/* end of extended memory below 16MB */
	UINT8		*ems_frame[4];
} CpuLink;

extern CpuLink g_cpu;

/* A20: the address mask follows the gate (16MB space on the 80286) */
static inline void cpu_set_a20(int on) { g_cpu.addr_mask = on ? 0xffffffu : 0xfffffu; }

/* how many times the base clock the emulated CPU may run (config.c) */
#define CLOCK_MUL_MAX	42

/* the shared state (cpu/cpustate.c) */
void cpu_link_init(void);
void cpu_link_term(void);
void cpu_link_reset(void);		/* power on: registers to the reset values, the CPU kind kept */
void cpu_link_shut(void);		/* CPU reset by the machine (shutdown): registers only */
void cpu_link_ext_resize(UINT32 bytes);	/* extended memory */

/* the CPU proper (cpu/cpuadapt.c) */
void pc98_adapt_initialize(void);
void pc98_adapt_deinitialize(void);
void pc98_adapt_reset(void);
void pc98_adapt_shut(void);
void pc98_adapt_exec(void);
void pc98_adapt_interrupt(REG8 vect);

#ifdef __cplusplus
}
#endif

#endif
