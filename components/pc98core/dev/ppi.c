/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * ppi.c - the two i8255 parallel interfaces of the main board, and the serial / printer ports with nothing connected.
 *
 * System 8255 (31h / 33h / 35h / 37h = port A / B / C / control; the mode is fixed by the board wiring):
 *   31h read   DIP switch 2
 *   33h read   bit 3 = DIP switch 1-1 inverted, bit 0 = data out of the uPD4990A calendar (bits 7-5, the RS-232C
 *              modem lines, read 0: nothing is connected)
 *   35h        port C latch, read back.  Bit 3 stops the buzzer (1 = silent); the rest is only kept.
 *   37h write  bit 7 = 0: set (bit 0 = 1) or clear port C bit n (n = bits 3-1); bit 7 = 1 (mode set) is ignored.
 *   Port C is F9h after a reset.
 * Printer 8255 (40h / 42h / 44h / 46h): no printer is fitted, but port B (42h) also carries the machine's identity:
 *   bit 7 = printer not busy, bit 5 = 8 MHz clock, bit 4 = DIP switch 1-3, bit 3 = DIP switch 1-8, bit 2 = 1,
 *   bit 1 = V30.
 * RS-232C (i8251 at 30h / 32h) and the extended printer port (140h-14Eh): absent (reads FFh, writes ignored). */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <io.h>
#include <devices.h>
#include <sound.h>

#define PORTC_BUZ	0x08

static UINT8 s_portc;

UINT8 ppi_portc(void)
{
	return s_portc;
}

static void portc_w(UINT port, REG8 v)
{
	(void)port;
	s_portc = (UINT8)v;
	spk_gate(!(s_portc & PORTC_BUZ));
}

static void control_w(UINT port, REG8 v)
{
	(void)port;
	if (!(v & 0x80)) {
		const UINT8 bit = (UINT8)(1u << ((v >> 1) & 7));
		if (v & 1) s_portc |= bit;
		else s_portc &= (UINT8)~bit;
		if (bit == PORTC_BUZ) spk_gate(!(s_portc & PORTC_BUZ));
	}
}

static REG8 porta_r(UINT port) { (void)port; return g_mach.dip_switch[1]; }
static REG8 portb_r(UINT port)
{
	(void)port;
	return (REG8)(((~g_mach.dip_switch[0] & 1) << 3) | rtc_dout());
}
static REG8 portc_r(UINT port) { (void)port; return s_portc; }

static REG8 printer_id(UINT port)
{
	REG8 v = 0x84;

	(void)port;
	if (g_mach.clk8mhz) v |= 0x20;
	if (g_mach.dip_switch[0] & 0x04) v |= 0x10;
	if (g_mach.dip_switch[0] & 0x80) v |= 0x08;
	if (g_cpu.v30 & 1) v |= 0x02;
	return v;
}

static void none_w(UINT port, REG8 v) { (void)port; (void)v; }
static REG8 none_r(UINT port) { (void)port; return 0xff; }

void ppi_reset(const Pc98Settings *cfg)
{
	(void)cfg;
	s_portc = 0xf9;
	spk_gate(!(s_portc & PORTC_BUZ));
}

void ppi_bind(void)
{
	static const io_out_fn sys_out[4] = { NULL, NULL, portc_w, control_w };
	static const io_in_fn sys_in[4] = { porta_r, portb_r, portc_r, NULL };
	static const io_out_fn prn_out[4] = { none_w, NULL, none_w, NULL };
	static const io_in_fn prn_in[4] = { none_r, printer_id, none_r, NULL };
	static const io_out_fn ser_out[2] = { none_w, none_w };
	static const io_in_fn ser_in[2] = { none_r, none_r };
	static const UINT ext[8] = { 0x140, 0x141, 0x142, 0x149, 0x14b, 0x14c, 0x14d, 0x14e };
	UINT i;

	io_sys(0x31, 0xf1, sys_out, sys_in, 4);
	io_sys(0x40, 0xf1, prn_out, prn_in, 4);
	io_sys(0x30, 0xf1, ser_out, ser_in, 2);
	for (i = 0; i < 8; i++) io_port(ext[i], none_w, none_r);
}
