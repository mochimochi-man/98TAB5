/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * board26k.c - the PC-9801-26K sound board: a YM2203 (3 FM voices, 3 SSG voices, two timers) and the board's ROM.
 *
 * Board (jumpers in g_settings.board26k_jumpers):
 *   bit 4      port base: 1 = 188h / 18Ah, 0 = 088h / 08Ah
 *   bits 7-6   interrupt: 00 INT0 (IRQ3), 01 INT6 (IRQ13), 10 INT41 (IRQ10), 11 INT5 (IRQ12); the setting reads back on the
 *              SSG's I/O port A bits 7-6
 *   bits 2-0   ROM at C8000h / CC000h / D0000h / D4000h, 4-7 none (SOUND.ROM)
 *   SSG I/O port A (register 0Eh) reads the joystick port, active low (bits 3-0 directions, 5-4 buttons); register
 *   0Fh bit 6 selects the second pad connector, which has nothing on it here.
 * Chip (YM2203 application manual):
 *   188h write the address, read the status (bit 0 Timer A flag, bit 1 Timer B flag; never busy);
 *   18Ah write the addressed register, read registers 00h-0Fh (the SSG's).
 *   24h / 25h  Timer A, 10 bits NA: overflows every (1024 - NA) x 72 master clocks
 *   26h        Timer B, 8 bits NB: overflows every (256 - NB) x 1152 master clocks
 *   27h        bit 0 / 1 run Timer A / B (a timer starts from its value when the bit is set), bit 2 / 3 let an
 *              overflow set flag A / B, bit 4 / 5 clear flag A / B, bits 7-6 channel 3 mode (10 = CSM: Timer A
 *              overflows key channel 3 on and off)
 *   28h        key on / off;  30h-B6h the FM voices (fm.c);  00h-0Fh the SSG (ssg.c)
 *   The interrupt line is active while a flag is set.  The master clock is 3.9936 MHz. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <hostfile.h>
#include <host.h>
#include <io.h>
#include <devices.h>
#include <sound.h>
#include "chips.h"

extern int ets_printf(const char *fmt, ...);

#define MASTER_HZ	3993600u
#define ROM_SIZE	0x4000u

static struct {
	SsgChip	ssg;
	FmChip	fm;
	UINT8	addr;
	UINT8	ctl27;
	UINT8	ta_hi, ta_lo, tb;
	UINT8	status;
	UINT8	ssgreg[16];	/* the SSG registers as written: what the CPU reads back (the chip itself may be on the
				 * sound task, behind) */
	UINT8	irq;		/* FFh = none */
	UINT8	jumper_int;	/* bits 7-6 */
	UINT	port;		/* 188h or 088h */
	int	present;
} b;

static UINT irq_of(UINT jumpers)
{
	static const UINT8 irq[4] = { 3, 13, 10, 12 };	/* INT0, INT6, INT41, INT5 */
	return irq[(jumpers >> 6) & 3];
}

static SINT32 master_to_cpu(UINT32 master)
{
	return (SINT32)(((UINT64)master * g_mach.cpu_clock) / MASTER_HZ);
}

static void update_irq(void)
{
	if (b.irq == 0xff) return;
	if (b.status & 3) pic_raise(b.irq);
	else pic_lower(b.irq);
}

/* the changes, applied by the mixer at their time (sound.h snd_write) */
static void ap_ssg(void *c, UINT reg, REG8 v) { ssg_write((SsgChip *)c, reg, v); }
static void ap_fm(void *c, UINT reg, REG8 v) { fm_write((FmChip *)c, reg, v); }
static void ap_key(void *c, UINT reg, REG8 v) { (void)reg; fm_key((FmChip *)c, v); }
static void ap_csm(void *c, UINT reg, REG8 v) { (void)reg; (void)v; fm_csm((FmChip *)c); }
static void ap_ch3(void *c, UINT reg, REG8 v) { (void)reg; ((FmChip *)c)->ch3mode = (UINT8)v; }

static void timer_a(UINT id, int timed);
static void timer_b(UINT id, int timed);

static SINT32 period_a(void) { return master_to_cpu((1024u - ((b.ta_hi << 2) | (b.ta_lo & 3))) * 72u); }
static SINT32 period_b(void) { return master_to_cpu((256u - b.tb) * 1152u); }

static void timer_a(UINT id, int timed)
{
	(void)id;
	if (!timed) return;
	if (b.ctl27 & 0x04) {
		b.status |= 1;
		update_irq();
	}
	if ((b.ctl27 & 0xc0) == 0x80) snd_write(ap_csm, &b.fm, 0, 0);
	ev_next(EV_FMTIMER_A, period_a(), timer_a);
}

static void timer_b(UINT id, int timed)
{
	(void)id;
	if (!timed) return;
	if (b.ctl27 & 0x08) {
		b.status |= 2;
		update_irq();
	}
	ev_next(EV_FMTIMER_B, period_b(), timer_b);
}

static void write_27(REG8 v)
{
	const UINT8 old = b.ctl27;

	if ((v & 0x01) && !(old & 0x01)) ev_at(EV_FMTIMER_A, period_a(), timer_a);
	if (!(v & 0x01)) ev_cancel(EV_FMTIMER_A);
	if ((v & 0x02) && !(old & 0x02)) ev_at(EV_FMTIMER_B, period_b(), timer_b);
	if (!(v & 0x02)) ev_cancel(EV_FMTIMER_B);
	b.status &= (UINT8)~((v >> 4) & 3);
	if ((v ^ old) & 0xc0) snd_write(ap_ch3, &b.fm, 0, (REG8)(v & 0xc0));
	b.ctl27 = (UINT8)(v & 0xcf);
	update_irq();
}

static void write_reg(UINT reg, REG8 v)
{
	if (reg < 0x10) {
		b.ssgreg[reg] = (UINT8)v;
		snd_write(ap_ssg, &b.ssg, reg, v);
	}
	else if (reg == 0x24) b.ta_hi = (UINT8)v;
	else if (reg == 0x25) b.ta_lo = (UINT8)v;
	else if (reg == 0x26) b.tb = (UINT8)v;
	else if (reg == 0x27) write_27(v);
	else if (reg == 0x28) snd_write(ap_key, &b.fm, reg, v);
	else if (reg >= 0x30 && reg < 0xb8) snd_write(ap_fm, &b.fm, reg, v);
}

static REG8 joystick(void)
{
	REG8 v = 0x3f;

	if (!(b.ssgreg[15] & 0x40)) v &= host_joypad() & 0x3f;
	return (REG8)(v | b.jumper_int);
}

static void addr_w(UINT port, REG8 v) { (void)port; b.addr = (UINT8)v; }
static void data_w(UINT port, REG8 v) { (void)port; write_reg(b.addr, v); }
static REG8 status_r(UINT port) { (void)port; return b.status; }

static REG8 data_r(UINT port)
{
	(void)port;
	if (b.addr == 0x0e) return joystick();
	if (b.addr < 0x10) return b.ssgreg[b.addr];
	return 0xff;
}

static void load_rom(UINT sel)
{
	static const UINT32 at[4] = { 0xc8000, 0xcc000, 0xd0000, 0xd4000 };
	char path[MAX_PATH];
	HFILE fh;
	int ok = 0;

	if (sel >= 4) return;
	rom_path(path, "SOUND.ROM", NELEMENTS(path));
	fh = hf_open(path);
	if (fh != NULL) {
		ok = hf_read(fh, mem + at[sel], ROM_SIZE) == ROM_SIZE;
		hf_close(fh);
	}
	ets_printf("26K ROM: %s at %05X -> %s\n", path, (unsigned)at[sel], ok ? "loaded" : "absent");
}

void board_reset(const Pc98Settings *cfg, SoundBoard id)
{
	memset(&b, 0, sizeof(b));
	b.irq = 0xff;
	b.present = (id == SOUND_26K);
	ssg_reset(&b.ssg);
	fm_reset(&b.fm);
	if (!b.present) return;
	b.port = (cfg->board26k_jumpers & 0x10) ? 0x188 : 0x088;
	b.jumper_int = cfg->board26k_jumpers & 0xc0;
	b.irq = (UINT8)irq_of(cfg->board26k_jumpers);
	load_rom(cfg->board26k_jumpers & 7);
}

void board_bind(void)
{
	if (!b.present) return;
	io_board(b.port, addr_w, status_r);
	io_board(b.port + 2, data_w, data_r);
	snd_add_source(ssg_render, &b.ssg, 0);
	snd_add_source(fm_render, &b.fm, 0);
}
