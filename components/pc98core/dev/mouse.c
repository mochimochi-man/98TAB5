/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * mouse.c - the bus mouse interface: an 8255 at 7FD9h / 7FDBh / 7FDDh / 7FDFh, the interrupt rate at BFDBh, IRQ13.
 *
 *   7FD9h  port A (input)  bit 7 left button, bit 5 right button (0 = pressed), bit 6 = 1, bit 4 = 0,
 *                          bits 3-0 one nibble of a counter
 *   7FDBh  port B          nothing connected
 *   7FDDh  port C (output) bit 7 HC: a rising edge latches the X and Y counters and clears them
 *                          bit 6 SXY: 0 X, 1 Y;  bit 5 SHL: 0 low nibble, 1 high nibble
 *                          bit 4 INT#: 0 enables the periodic interrupt
 *                          bits 3-0 (input when so programmed): bit 3 = 1, bit 2 = DIP switch 3-8,
 *                          bits 1-0 = DIP switch 1-6 / 1-5 inverted
 *   7FDFh  control         bit 7 = 1: mode word (outputs reset to 0); bit 7 = 0: set / clear port C bit (bits 3-1)
 *   BFDBh  write           bits 1-0: interrupt rate 120 / 60 / 30 / 15 Hz
 * Software latches (HC 0 -> 1), reads the four nibbles while HC is high, and drops HC again; while HC is high the
 * latched values are read, otherwise the running counters.  The counters are 8 bits: read running they wrap (software
 * that never latches takes the difference of two readings); a latched value is saturated to -128..127.
 *
 * The host's motion arrives once per frame (mouse_frame).  It is fed into the counters spread over the following
 * frame time, in proportion to the CPU clocks elapsed, so a game that polls several times a frame sees a smooth
 * movement; whatever is left over when the next frame's motion arrives is added at once. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <io.h>
#include <devices.h>
#include <host.h>

#define HC	0x80
#define SXY	0x40
#define SHL	0x20
#define INTOFF	0x10

#define MODE_A_IN	0x10
#define MODE_CH_IN	0x08
#define MODE_B_IN	0x02
#define MODE_CL_IN	0x01

typedef struct {
	SINT32	count;		/* running counter */
	SINT32	todo;		/* host motion still to feed */
	SINT32	total;		/* host motion of this frame */
	SINT8	latch;
} Axis;

static struct {
	UINT8	mode, a, b, c;
	UINT8	buttons;
	UINT8	rate;
	Axis	x, y;
	UINT32	t0;		/* CPU clock of the frame start */
	UINT32	frame;		/* CPU clocks of one frame */
	UINT32	tick;		/* CPU clocks of one interrupt at 120 Hz */
} s;

static UINT32 now(void)
{
	return (UINT32)(g_cpu.clocks_done + g_cpu.slice_len - g_cpu.slice_left);
}

static void feed(Axis *ax, UINT32 dt)
{
	SINT32 want;

	if (!ax->total) return;
	want = (dt >= s.frame) ? ax->total : (SINT32)(((SINT64)ax->total * dt) / s.frame);
	want = ax->total - want;			/* what should still be left */
	ax->count += ax->todo - want;
	ax->todo = want;
}

static void catch_up(void)
{
	const UINT32 dt = now() - s.t0;

	feed(&s.x, dt);
	feed(&s.y, dt);
}

static void irq_event(UINT id, int timed)
{
	(void)id;
	if (!timed || (s.c & INTOFF)) return;
	pic_raise(13);
	ev_next(EV_MOUSE, (SINT32)(s.tick << s.rate), irq_event);
}

static void start_irq(void)
{
	if (!ev_armed(EV_MOUSE)) ev_at(EV_MOUSE, (SINT32)(s.tick << s.rate), irq_event);
}

void mouse_frame(void)
{
	SINT16 dx = 0, dy = 0;

	s.x.count += s.x.todo;
	s.y.count += s.y.todo;
	s.buttons = host_mouse(&dx, &dy);
	s.x.total = s.x.todo = dx;
	s.y.total = s.y.todo = dy;
	s.t0 = now();
	if (!(s.c & INTOFF)) start_irq();
}

static SINT8 sat8(SINT32 v)
{
	return (SINT8)(v > 127 ? 127 : v < -128 ? -128 : v);
}

static void set_c(UINT8 v)
{
	if ((v & HC) && !(s.c & HC)) {
		catch_up();
		s.x.latch = sat8(s.x.count);
		s.y.latch = sat8(s.y.count);
		s.x.count = 0;
		s.y.count = 0;
	}
	s.c = v;
	if (!(v & INTOFF)) start_irq();
}

static void a_w(UINT port, REG8 v) { (void)port; s.a = (UINT8)v; }
static void b_w(UINT port, REG8 v) { (void)port; s.b = (UINT8)v; }
static void c_w(UINT port, REG8 v) { (void)port; set_c((UINT8)v); }

static void ctrl_w(UINT port, REG8 v)
{
	(void)port;
	if (v & 0x80) {
		s.mode = (UINT8)v;
		set_c(0);
	}
	else {
		const UINT8 bit = (UINT8)(1u << ((v >> 1) & 7));
		set_c((v & 1) ? (s.c | bit) : (s.c & (UINT8)~bit));
	}
}

static REG8 a_r(UINT port)
{
	SINT32 v;

	(void)port;
	if (!(s.mode & MODE_A_IN)) return s.a;
	catch_up();
	if (s.c & HC) v = (s.c & SXY) ? s.y.latch : s.x.latch;
	else v = (SINT8)((s.c & SXY) ? s.y.count : s.x.count);	/* the running counter is 8 bits: it wraps, as software reading
								 * differences of it (without HC) expects */
	if (s.c & SHL) v >>= 4;
	return (REG8)((s.buttons & 0xa0) | 0x40 | (v & 0x0f));
}

static REG8 b_r(UINT port)
{
	(void)port;
	return (s.mode & MODE_B_IN) ? 0x40 : s.b;
}

static REG8 c_r(UINT port)
{
	REG8 v = s.c;

	(void)port;
	if (s.mode & MODE_CH_IN) v &= 0x1f;
	if (s.mode & MODE_CL_IN) {
		v = (REG8)((v & 0xf0) | 0x08 | ((g_mach.dip_switch[2] >> 5) & 0x04) | ((~g_mach.dip_switch[0] >> 4) & 0x03));
	}
	return v;
}

static void rate_w(UINT port, REG8 v)
{
	(void)port;
	s.rate = v & 3;
}

void mouse_changeclock(void)
{
	s.tick = g_mach.cpu_clock / 120;
	s.frame = g_mach.cpu_clock / 60;
	if (!s.tick) s.tick = 1;
	if (!s.frame) s.frame = 1;
}

void mouse_reset(const Pc98Settings *cfg)
{
	(void)cfg;
	memset(&s, 0, sizeof(s));
	s.mode = 0x93;
	s.c = 0xf0;
	s.buttons = 0xa0;
	s.x.latch = s.y.latch = -1;
	mouse_changeclock();
}

void mouse_bind(void)
{
	io_port(0x7fd9, a_w, a_r);
	io_port(0x7fdb, b_w, b_r);
	io_port(0x7fdd, c_w, c_r);
	io_port(0x7fdf, ctrl_w, NULL);
	io_port(0xbfdb, rate_w, NULL);
}
