/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * dma8237.c - the i8237 DMA controller and the PC-98 page registers.
 *
 * Ports (odd addresses):
 *   01h + 4n / 03h + 4n   channel n address / count (base and current written together, read = current);
 *                         the two bytes go low then high through one flip-flop shared by all registers
 *   11h   read status (bits 3-0 terminal count reached, cleared by the read; bits 7-4 requests), write command
 *         (bit 2 disables the controller)
 *   13h   request register (bit 2 set / clear, bits 1-0 channel)
 *   15h   single mask bit (bit 2 set / clear, bits 1-0 channel)
 *   17h   mode: bits 1-0 channel, 3-2 type (00 verify, 01 write to memory, 10 read from memory), bit 4
 *         auto-initialise, bit 5 address decrement, bits 7-6 transfer mode
 *   19h   clear the flip-flop;  1Bh master clear;  1Dh clear all mask bits;  1Fh write all mask bits
 *   21h / 23h / 25h / 27h   page (A16-A23) of channel 1 / 2 / 3 / 0
 *   29h   address wrap of a channel: bits 1-0 channel, bits 3-2: 0 within 64 KB, 1 within 1 MB, 3 within 16 MB
 *   0E05h / 0E07h / 0E09h / 0E0Bh   A24-A31 of channel 0 / 1 / 2 / 3
 * The count register holds the number of bytes minus one; the terminal count is signalled to the device during
 * the last byte.  Without auto-initialise the channel's mask bit is then set; with it, the current address and
 * count are reloaded from the base registers.
 *
 * A channel transfers while the controller is enabled, the channel is unmasked and its device holds DREQ.  The
 * CPU loop calls dma_cycle() after each instruction while g_dma_active is non-zero; each call moves one byte on
 * every active channel. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <io.h>
#include <devices.h>

typedef struct {
	UINT16	base_addr, base_count;
	UINT16	addr, count;
	UINT8	page;		/* A16-A23 */
	UINT8	page_hi;	/* A24-A31 */
	UINT8	mode;
	UINT8	wrap;		/* 29h setting */
	UINT8	dreq;
	const DmaDevice *dev;
} Channel;

UINT8 g_dma_active;

static Channel s_ch[4];
static UINT8 s_mask, s_status, s_command, s_flip;

static void none_to(REG8 v) { (void)v; }
static REG8 none_from(void) { return 0xff; }
static void none_tc(void) { }
static const DmaDevice s_none = { none_to, none_from, none_tc };

void dma_update(void)
{
	UINT8 act = 0;
	UINT n;

	if (!(s_command & 0x04)) {
		for (n = 0; n < 4; n++) {
			if (s_ch[n].dreq && !(s_mask & (1u << n))) act |= (UINT8)(1u << n);
		}
	}
	if (act != g_dma_active) {
		g_dma_active = act;
		ev_cut_slice();
	}
}

void dma_connect(UINT ch, const DmaDevice *dev)
{
	s_ch[ch & 3].dev = dev ? dev : &s_none;
}

void dma_request(UINT ch, int on)
{
	s_ch[ch & 3].dreq = on ? 1 : 0;
	if (on) s_status |= (UINT8)(0x10u << (ch & 3));
	else s_status &= (UINT8)~(0x10u << (ch & 3));
	dma_update();
}

static UINT32 phys(const Channel *c)
{
	return ((UINT32)c->page_hi << 24) | ((UINT32)c->page << 16) | c->addr;
}

static void step_address(Channel *c)
{
	const int down = (c->mode & 0x20) != 0;
	const UINT16 before = c->addr;

	c->addr = (UINT16)(down ? before - 1 : before + 1);
	if ((down && before == 0) || (!down && before == 0xffff)) {
		if (c->wrap == 1) c->page = (UINT8)((c->page & 0xf0) | ((c->page + (down ? -1 : 1)) & 0x0f));
		else if (c->wrap == 3) c->page = (UINT8)(c->page + (down ? -1 : 1));
	}
}

void dma_cycle(void)
{
	UINT n;

	for (n = 0; n < 4; n++) {
		Channel *c = &s_ch[n];
		const UINT8 bit = (UINT8)(1u << n);
		int last;

		if (!(g_dma_active & bit)) continue;
		last = (c->count == 0);
		if (last) c->dev->tc();
		switch (c->mode & 0x0c) {
		case 0x00:
			(void)c->dev->from_dev();
			break;
		case 0x04:
			bus_wr8(phys(c), c->dev->from_dev());
			break;
		default:
			c->dev->to_dev(bus_rd8(phys(c)));
			break;
		}
		step_address(c);
		c->count--;
		if (last) {
			s_status |= bit;
			if (c->mode & 0x10) {
				c->addr = c->base_addr;
				c->count = c->base_count;
			}
			else {
				s_mask |= bit;
				dma_update();
			}
		}
	}
}

UINT32 dma_address(UINT ch) { return phys(&s_ch[ch & 3]); }
UINT16 dma_count(UINT ch) { return s_ch[ch & 3].count; }
UINT8 dma_mode(UINT ch) { return s_ch[ch & 3].mode; }

/* ---- ports ---- */

static UINT16 put_byte(UINT16 w, REG8 v)
{
	const UINT16 r = s_flip ? (UINT16)((w & 0x00ff) | (v << 8)) : (UINT16)((w & 0xff00) | v);

	s_flip ^= 1;
	return r;
}

static REG8 get_byte(UINT16 w)
{
	const REG8 r = (REG8)(s_flip ? (w >> 8) : w);

	s_flip ^= 1;
	return r;
}

static void addr_w(UINT port, REG8 v)
{
	Channel *c = &s_ch[(port >> 2) & 3];

	c->base_addr = c->addr = put_byte(c->addr, v);
}

static void count_w(UINT port, REG8 v)
{
	Channel *c = &s_ch[(port >> 2) & 3];

	c->base_count = c->count = put_byte(c->count, v);
}

static REG8 addr_r(UINT port) { return get_byte(s_ch[(port >> 2) & 3].addr); }
static REG8 count_r(UINT port) { return get_byte(s_ch[(port >> 2) & 3].count); }

static REG8 status_r(UINT port)
{
	const REG8 v = s_status;

	(void)port;
	s_status &= 0xf0;
	return v;
}

static void command_w(UINT port, REG8 v) { (void)port; s_command = (UINT8)v; dma_update(); }
static void request_w(UINT port, REG8 v) { (void)port; (void)v; }	/* memory-to-memory is not wired */

static void single_mask_w(UINT port, REG8 v)
{
	(void)port;
	if (v & 4) s_mask |= (UINT8)(1u << (v & 3));
	else s_mask &= (UINT8)~(1u << (v & 3));
	dma_update();
}

static void mode_w(UINT port, REG8 v) { (void)port; s_ch[v & 3].mode = (UINT8)v; }
static void flip_w(UINT port, REG8 v) { (void)port; (void)v; s_flip = 0; }

static void master_clear_w(UINT port, REG8 v)
{
	(void)port; (void)v;
	s_command = 0;
	s_status &= 0xf0;
	s_flip = 0;
	s_mask = 0x0f;
	dma_update();
}

static void clear_mask_w(UINT port, REG8 v) { (void)port; (void)v; s_mask = 0; dma_update(); }
static void all_mask_w(UINT port, REG8 v) { (void)port; s_mask = v & 0x0f; dma_update(); }

static void page_w(UINT port, REG8 v)
{
	s_ch[((port >> 1) + 1) & 3].page = (UINT8)v;	/* 21h ch1, 23h ch2, 25h ch3, 27h ch0 */
}

static void wrap_w(UINT port, REG8 v)
{
	(void)port;
	s_ch[v & 3].wrap = (v >> 2) & 3;
}

static void page_hi_w(UINT port, REG8 v)
{
	s_ch[((port - 0x0e05) >> 1) & 3].page_hi = (UINT8)v;
}

void dma_reset(const Pc98Settings *cfg)
{
	UINT n;

	(void)cfg;
	for (n = 0; n < 4; n++) {
		const DmaDevice *dev = s_ch[n].dev;
		memset(&s_ch[n], 0, sizeof(s_ch[n]));
		s_ch[n].dev = dev ? dev : &s_none;
	}
	s_mask = 0x0f;
	s_status = 0;
	s_command = 0;
	s_flip = 0;
	g_dma_active = 0;
}

void dma_bind(void)
{
	static const io_out_fn out[16] = {
		addr_w, count_w, addr_w, count_w, addr_w, count_w, addr_w, count_w,
		command_w, request_w, single_mask_w, mode_w, flip_w, master_clear_w, clear_mask_w, all_mask_w
	};
	static const io_in_fn in[16] = {
		addr_r, count_r, addr_r, count_r, addr_r, count_r, addr_r, count_r,
		status_r, NULL, NULL, NULL, NULL, NULL, NULL, NULL
	};
	static const io_out_fn page_out[8] = { page_w, page_w, page_w, page_w, wrap_w, NULL, NULL, NULL };
	UINT n;

	io_sys(0x01, 0xce1, out, in, 16);
	io_sys(0x21, 0xcf1, page_out, NULL, 8);
	for (n = 0; n < 4; n++) io_port(0x0e05 + n * 2, page_hi_w, NULL);
}
