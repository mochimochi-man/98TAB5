/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * cpustate.c - the state the CPU shares with the machine (include/cpulink.h). */
#include <stdlib.h>
#include <string.h>
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>

CpuLink g_cpu = { .x386 = { .enabled = 1 } };	/* main/ chooses the CPU menu before the CPU starts */
UINT8 g_memwait_tram, g_memwait_vram, g_memwait_grcg;

/* after a CPU reset the processor starts at F000:FFF0 with A20 off; on the V30 FLAGS bits 12-15 read as 1 */
static void reset_registers(void)
{
	memset(&g_cpu.reg, 0, sizeof(g_cpu.reg));
	g_cpu.reg.cs = 0xf000;
	g_cpu.reg.cs_base = 0xf0000;
	g_cpu.reg.ip = 0xfff0;
	g_cpu.reg.flags = g_cpu.v30 ? 0xf002 : 0x0002;
	g_cpu.addr_mask = 0xfffff;
}

void cpu_link_init(void)
{
	const int x386 = g_cpu.x386.enabled;

	memset(&g_cpu, 0, sizeof(g_cpu));
	g_cpu.x386.enabled = x386;
}

void cpu_link_term(void)
{
	cpu_link_ext_resize(0);
}

/* the clock counters stay: the scheduler owns them and resets them with the machine */
void cpu_link_reset(void)
{
	const int x386 = g_cpu.x386.enabled;

	g_cpu.reset_pending = 0;
	g_cpu.itf_rom = 0;
	g_cpu.d000_ram = 0;
	memset(&g_cpu.x386, 0, sizeof(g_cpu.x386));
	g_cpu.x386.enabled = x386;
	reset_registers();
}

void cpu_link_shut(void)
{
	reset_registers();
}

void cpu_link_ext_resize(UINT32 bytes)
{
	UINT i;

	if (g_cpu.ext_size != bytes) {
		free(g_cpu.ext);
		g_cpu.ext = bytes ? (UINT8 *)calloc(1, bytes + 16) : NULL;	/* zeroed: the state save leaves all-zero pages out */
		g_cpu.ext_size = g_cpu.ext ? bytes : 0;
	}
	g_cpu.ext_at1m = g_cpu.ext ? g_cpu.ext - 0x100000 : NULL;
	g_cpu.ext_end16 = g_cpu.ext ? MIN(g_cpu.ext_size + 0x100000u, 0xf00000u) : 0;
	for (i = 0; i < 4; i++) g_cpu.ems_frame[i] = mem + 0xc0000 + i * 0x4000;	/* no EMS board: plain memory */
}
