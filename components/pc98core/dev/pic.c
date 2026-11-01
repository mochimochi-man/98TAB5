/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * pic.c - the two i8259A interrupt controllers (master 00h / 02h, slave 08h / 0Ah; the slave's INT drives master IR7).
 *
 * Written from the i8259A data sheet:
 *   IRR latches a request (pic_raise is a rising edge, pic_lower takes the request away), ISR holds the levels in
 *   service, IMR masks.  Fully nested priority with rotation: `lowest` is the level with the lowest priority
 *   (7 after initialisation, so IR0 is the highest).
 *   ICW1 (port 0, bit 4 = 1) starts initialisation: IMR, IRR, ISR-read selection, special mask mode and rotation
 *   are cleared; ICW2 (vector base), ICW3 (unless single mode), ICW4 (if ICW1 bit 0) follow on port 1.
 *   OCW1 = IMR.  OCW2: EOI (non-specific: the highest level in service; specific: bits 2-0), rotate with EOI,
 *   set priority, rotate in automatic-EOI mode set / clear.  OCW3: read IRR / ISR, poll, special mask mode.
 *   ICW4: automatic EOI (bit 1), special fully nested mode (bit 4, used by the master: a slave request is let
 *   through while the cascade input is in service, the slave resolves its own priority).
 * pic_deliver() is called by the CPU loop when interrupts are enabled; it hands at most one vector to the CPU. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <events.h>
#include <io.h>
#include <devices.h>
#include <cpu/cpuadapt.h>

typedef struct {
	UINT8	irr, isr, imr;
	UINT8	vector;		/* ICW2: bits 7-3 */
	UINT8	cascade;	/* ICW3 */
	UINT8	icw1, icw4;
	UINT8	step;		/* next initialisation word: 2, 3, 4, or 0 = done */
	UINT8	lowest;		/* the level with the lowest priority */
	UINT8	read_isr;	/* OCW3: port 0 reads ISR (else IRR) */
	UINT8	poll;		/* OCW3: the next port 0 read is a poll */
	UINT8	smm;		/* special mask mode */
	UINT8	rot_aeoi;	/* rotate in automatic EOI mode */
} I8259;

static I8259 s_pic[2];

#define AEOI(p)	((p)->icw4 & 0x02)
#define SFNM(p)	((p)->icw4 & 0x10)

/* the highest-priority level of `set` (non-empty) */
static UINT highest(const I8259 *p, UINT set)
{
	UINT k, level;

	for (k = 1; k <= 8; k++) {
		level = (p->lowest + k) & 7;
		if (set & (1u << level)) return level;
	}
	return 0;
}

/* the levels in service that block others (in special mask mode a masked level does not) */
static UINT blocking(const I8259 *p)
{
	return p->smm ? (p->isr & ~p->imr) : p->isr;
}

/* the level a controller would hand out now, or -1: a request that outranks every blocking level */
static int candidate(const I8259 *p, UINT req)
{
	UINT level;

	if (!req) return -1;
	level = highest(p, req | blocking(p));
	return (req & (1u << level)) && !(blocking(p) & (1u << level)) ? (int)level : -1;
}

static void acknowledge(I8259 *p, UINT level)
{
	p->irr &= (UINT8)~(1u << level);
	if (!AEOI(p)) p->isr |= (UINT8)(1u << level);
	else if (p->rot_aeoi) p->lowest = (UINT8)level;
}

int pic_pending(void)
{
	return ((s_pic[0].irr & ~s_pic[0].imr) | (s_pic[1].irr & ~s_pic[1].imr)) != 0;
}

void pic_deliver(void)
{
	I8259 *m = &s_pic[0], *s = &s_pic[1];
	const UINT cas = s->cascade & 7;			/* the master input the slave is wired to */
	const UINT casbit = 1u << cas;
	const UINT sreq = s->irr & ~s->imr;
	UINT mreq, mblock, level;
	int slevel;

	if (!pc98_adapt_is_ei()) return;
	mreq = (m->irr & ~casbit) | (sreq ? casbit : 0);
	mreq &= ~m->imr;
	if (!mreq) return;
	mblock = blocking(m);
	if (SFNM(m)) mblock &= ~casbit;			/* special fully nested: the slave decides for itself */
	level = highest(m, mreq | mblock);
	if (!(mreq & (1u << level))) return;		/* a level in service outranks every request */
	if (level != cas) {
		acknowledge(m, level);
		pc98_adapt_interrupt((REG8)((m->vector & 0xf8) | level));
		return;
	}
	slevel = candidate(s, sreq);
	if (slevel < 0) return;
	if (!AEOI(m)) m->isr |= (UINT8)casbit;
	else if (m->rot_aeoi) m->lowest = (UINT8)cas;
	acknowledge(s, (UINT)slevel);
	pc98_adapt_interrupt((REG8)((s->vector & 0xf8) | slevel));
}

void pic_raise(REG8 irq)
{
	s_pic[(irq >> 3) & 1].irr |= (UINT8)(1u << (irq & 7));
}

void pic_lower(REG8 irq)
{
	s_pic[(irq >> 3) & 1].irr &= (UINT8)~(1u << (irq & 7));
}

UINT8 pic_isr(UINT chip)
{
	return s_pic[chip & 1].isr;
}

/* ---- ports ---- */

static void port0_w(UINT port, REG8 v)
{
	I8259 *p = &s_pic[(port >> 3) & 1];

	if (v & 0x10) {					/* ICW1 */
		p->icw1 = (UINT8)v;
		if (!(v & 0x01)) p->icw4 = 0;
		p->imr = 0;
		p->irr = 0;
		p->read_isr = 0;
		p->poll = 0;
		p->smm = 0;
		p->lowest = 7;
		p->rot_aeoi = 0;
		p->step = 2;
		return;
	}
	if (v & 0x08) {					/* OCW3 */
		if (v & 0x02) p->read_isr = v & 0x01;
		if (v & 0x40) p->smm = (v >> 5) & 1;
		p->poll = (v >> 2) & 1;
		return;
	}
	/* OCW2 */
	switch (v & 0xe0) {
	case 0x80:					/* rotate in automatic EOI mode: set */
	case 0x00:					/* ... clear */
		p->rot_aeoi = (v & 0x80) ? 1 : 0;
		return;
	case 0x20:					/* non-specific EOI */
	case 0xa0:					/* rotate on non-specific EOI */
		if (p->isr) {
			const UINT level = highest(p, p->isr);
			p->isr &= (UINT8)~(1u << level);
			if (v & 0x80) p->lowest = (UINT8)level;
		}
		break;
	case 0x60:					/* specific EOI */
		p->isr &= (UINT8)~(1u << (v & 7));
		break;
	case 0xe0:					/* rotate on specific EOI */
		p->isr &= (UINT8)~(1u << (v & 7));
		p->lowest = v & 7;
		break;
	case 0xc0:					/* set priority */
		p->lowest = v & 7;
		break;
	default:					/* 40h: no operation */
		return;
	}
	ev_cut_slice();					/* a lower level may be deliverable now */
}

static void port1_w(UINT port, REG8 v)
{
	I8259 *p = &s_pic[(port >> 3) & 1];

	switch (p->step) {
	case 2:
		p->vector = (UINT8)v;
		p->step = (p->icw1 & 0x02) ? ((p->icw1 & 0x01) ? 4 : 0) : 3;
		break;
	case 3:
		p->cascade = (UINT8)v;
		p->step = (p->icw1 & 0x01) ? 4 : 0;
		break;
	case 4:
		p->icw4 = (UINT8)v;
		p->step = 0;
		break;
	default:					/* OCW1 */
		p->imr = (UINT8)v;
		break;
	}
	ev_cut_slice();
}

static REG8 port0_r(UINT port)
{
	I8259 *p = &s_pic[(port >> 3) & 1];

	if (p->poll) {					/* a poll read acknowledges like an interrupt */
		const int level = candidate(p, p->irr & ~p->imr);
		p->poll = 0;
		if (level < 0) return 0x00;
		acknowledge(p, (UINT)level);
		return (REG8)(0x80 | level);
	}
	return p->read_isr ? p->isr : p->irr;
}

static REG8 port1_r(UINT port)
{
	return s_pic[(port >> 3) & 1].imr;
}

void pic_reset(const Pc98Settings *cfg)
{
	(void)cfg;
	memset(s_pic, 0, sizeof(s_pic));
	/* what the BIOS programs: master vectors 08h, slave on IR7, special fully nested; slave vectors 10h */
	s_pic[0].icw1 = 0x11; s_pic[0].vector = 0x08; s_pic[0].cascade = 0x80; s_pic[0].icw4 = 0x1d; s_pic[0].imr = 0x7d;
	s_pic[1].icw1 = 0x11; s_pic[1].vector = 0x10; s_pic[1].cascade = 0x07; s_pic[1].icw4 = 0x09; s_pic[1].imr = 0x71;
	s_pic[0].lowest = s_pic[1].lowest = 7;
}

void pic_bind(void)
{
	static const io_out_fn out[2] = { port0_w, port1_w };
	static const io_in_fn in[2] = { port0_r, port1_r };

	io_sys(0x00, 0xf1, out, in, 2);			/* 00h / 02h master, 08h / 0Ah slave (A3 selects the chip) */
}
