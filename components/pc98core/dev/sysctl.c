/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * sysctl.c - the main board's control ports.
 *
 *   50h  write: NMI masked            52h  write: NMI unmasked (nothing raises an NMI here; only the latch exists)
 *   F0h  write: CPU reset (the A20 gate closes)       read: 00h
 *   F2h  write: A20 gate open                        read: FEh while A20 is open, FFh while closed
 *   F6h  write: 02h opens, 03h closes the A20 gate   read: bit 0 = A20 closed, bit 1 = NMI unmasked
 *   0439h write: kept                 043Bh write: bit 2 kept, read back
 *   043Dh write: 10h maps the ITF ROM at F8000h, 12h the BIOS ROM
 * 50h-F6h are decoded on the low address byte (internal ports); 0439h-043Dh on all 16 bits. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <events.h>
#include <io.h>
#include <devices.h>

static UINT8 s_nmi_open;
static UINT8 s_p439 = 0xff, s_p43b;

static void a20(int open)
{
	cpu_set_a20(open ? TRUE : FALSE);
}

static int a20_open(void)
{
	return (g_cpu.addr_mask >> 20) & 1;
}

/* ---- 50h / 52h ---- */
static void nmi_mask(UINT port, REG8 v) { (void)port; (void)v; s_nmi_open = 0; }
static void nmi_unmask(UINT port, REG8 v) { (void)port; (void)v; s_nmi_open = 1; }

/* ---- F0h / F2h / F6h ---- */
static void cpu_reset(UINT port, REG8 v)
{
	(void)port;
	(void)v;
	a20(0);
	g_cpu.reset_pending = 1;
	ev_cut_slice();				/* the machine resets the CPU at the end of this slice */
}
static void a20_on(UINT port, REG8 v) { (void)port; (void)v; a20(1); }
static void a20_gate(UINT port, REG8 v)
{
	(void)port;
	if (v == 0x02) a20(1);
	else if (v == 0x03) a20(0);
}
static REG8 cpu_id(UINT port) { (void)port; return 0x00; }
static REG8 a20_read(UINT port) { (void)port; return a20_open() ? 0xfe : 0xff; }
static REG8 gate_read(UINT port)
{
	(void)port;
	return (REG8)((a20_open() ? 0 : 0x01) | (s_nmi_open ? 0x02 : 0));
}

/* ---- 0439h / 043Bh / 043Dh ---- */
static void p439_w(UINT port, REG8 v) { (void)port; s_p439 = (UINT8)v; }
static void p43b_w(UINT port, REG8 v) { (void)port; s_p43b = (UINT8)(v & 0x04); }
static REG8 p43b_r(UINT port) { (void)port; return s_p43b; }
static void bank_w(UINT port, REG8 v)
{
	(void)port;
	if (v == 0x10) g_cpu.itf_rom = 1;
	else if (v == 0x12) g_cpu.itf_rom = 0;
}

int sysctl_nmi_open(void)
{
	return s_nmi_open;
}

void sysctl_reset(const Pc98Settings *cfg)
{
	(void)cfg;
	s_nmi_open = 0;
	s_p439 = 0xff;
	s_p43b = 0;
}

void sysctl_bind(void)
{
	static const io_out_fn nmi_out[2] = { nmi_mask, nmi_unmask };
	static const io_out_fn cpu_out[8] = { cpu_reset, a20_on, NULL, a20_gate, NULL, NULL, NULL, NULL };
	static const io_in_fn cpu_in[8] = { cpu_id, a20_read, NULL, gate_read, NULL, NULL, NULL, NULL };

	io_sys(0x50, 0xf1, nmi_out, NULL, 2);		/* 50h, 52h (A1 selects; A2-A3 not decoded) */
	io_sys(0xf0, 0xf1, cpu_out, cpu_in, 8);		/* F0h-FEh, even */
	io_port(0x0439, p439_w, NULL);
	io_port(0x043b, p43b_w, p43b_r);
	io_port(0x043d, bank_w, NULL);
}

/* ---- state save / load (state.c) ---- */
#include <stateint.h>
void sysctl_state(SsCtx *c)
{
	SS_VAR(c, "SYSn", s_nmi_open);
	SS_VAR(c, "SYS9", s_p439);
	SS_VAR(c, "SYSb", s_p43b);
}
