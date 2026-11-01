/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * artic.c - the ARTIC, a free-running 24-bit counter at 307.2 kHz (the base clock / 8; on the 8 MHz models the
 * 1.9968 MHz base clock gives 6.5 CPU-clock units per tick at multiple 1).
 *   5Ch read bits 7-0, 5Dh and 5Eh read bits 15-8, 5Fh read bits 23-16; a word read of 5Ch gives bits 15-0, of 5Eh
 *   bits 23-8.  Writing 5Fh is a short wait: the CPU loses 20 clocks.
 * The count follows the emulated time: CPU clocks since a reference point, converted at the current CPU multiple.
 * artic_callback() moves the reference point once a frame so the 32-bit clock difference never overflows. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <io.h>
#include <devices.h>

static UINT32 s_ref_clock;	/* CPU clock of the reference point */
static UINT32 s_ref_count;	/* the count there */
static UINT32 s_frac;		/* half CPU clocks after it not yet counted */

static UINT32 clock_now(void)
{
	return g_cpu.clocks_done + (UINT32)(g_cpu.slice_len - g_cpu.slice_left);
}

/* half CPU clocks per tick: 2 x multiple x (8, or 6.5 on the 8 MHz models) */
static UINT32 tick_halfclocks(void)
{
	return g_mach.clock_mul * ((g_mach.clk8mhz) ? 13u : 16u);
}

static UINT32 count_now(void)
{
	const UINT64 half = (UINT64)(clock_now() - s_ref_clock) * 2 + s_frac;

	return s_ref_count + (UINT32)(half / tick_halfclocks());
}

void artic_callback(void)
{
	const UINT32 now = clock_now();
	const UINT64 half = (UINT64)(now - s_ref_clock) * 2 + s_frac;
	const UINT32 t = tick_halfclocks();

	s_ref_count += (UINT32)(half / t);
	s_frac = (UINT32)(half % t);
	s_ref_clock = now;
}

static void wait_w(UINT port, REG8 v) { (void)port; (void)v; g_cpu.slice_left -= 20; }
static REG8 low_r(UINT port) { (void)port; return (REG8)(count_now() & 0xff); }
static REG8 mid_r(UINT port) { (void)port; return (REG8)((count_now() >> 8) & 0xff); }
static REG8 high_r(UINT port) { (void)port; return (REG8)((count_now() >> 16) & 0xff); }

REG16 artic_r16(UINT port)
{
	const UINT32 c = count_now();

	return (REG16)((port & 2) ? (c >> 8) : c);
}

void artic_reset(const Pc98Settings *cfg)
{
	(void)cfg;
	s_ref_clock = clock_now();
	s_ref_count = 0;
	s_frac = 0;
}

void artic_bind(void)
{
	io_port(0x5c, NULL, low_r);
	io_port(0x5d, NULL, mid_r);
	io_port(0x5e, NULL, mid_r);
	io_port(0x5f, wait_w, high_r);
	io_port16(0x5c, NULL, artic_r16);
	io_port16(0x5d, NULL, artic_r16);
	io_port16(0x5e, NULL, artic_r16);
}
