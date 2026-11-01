/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * kbd.c - the keyboard interface: an i8251 USART receiving from the keyboard unit.
 *
 *   41h read   the received byte; RxRDY is cleared and IRQ1 withdrawn
 *   41h write  a byte for the keyboard unit (commands; only while the transmitter is enabled, command bit 0)
 *   43h read   status: bit 0 TxRDY, bit 2 TxEMPTY, bit 7 DSR always set (the unit is always there and always
 *              ready), bit 1 RxRDY, bit 4 overrun
 *   43h write  command word: bit 4 resets the error flags; bit 3 (send break) going from 1 to 0 is the reset
 *              signal of the keyboard unit
 * The line runs at 19200 bit/s, ten bits a byte: a byte arrives every 1/1920 s.  Bytes waiting in the unit go
 * out one per byte time, answers to commands before key codes.  While a byte is waiting and the received byte
 * has not been read, IRQ1 is raised again every byte time; a byte is only taken over when the receiver is empty.
 * More key codes than the unit holds are lost and flagged as overrun. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <io.h>
#include <devices.h>
#include <kbd.h>

#define ST_RXRDY	0x02
#define ST_OVERRUN	0x10
#define ST_ALWAYS	0x85
#define CMD_TXEN	0x01
#define CMD_BREAK	0x08
#define CMD_ERRRESET	0x10

#define KEYQ	32
#define REPLYQ	8

typedef struct {
	UINT8	q[KEYQ];
	UINT	head, n;
} Queue;

static struct {
	UINT8	cmd, status, rx;
	SINT32	byte_time;
	Queue	keys, replies;
} s;

static int q_put(Queue *q, UINT size, REG8 v)
{
	if (q->n >= size) return 0;
	q->q[(q->head + q->n) % size] = (UINT8)v;
	q->n++;
	return 1;
}

static UINT8 q_take(Queue *q, UINT size)
{
	const UINT8 v = q->q[q->head];

	q->head = (q->head + 1) % size;
	q->n--;
	return v;
}

static void byte_time(UINT id, int timed)
{
	(void)id;
	if (!timed) return;
	if (!s.keys.n && !s.replies.n) return;
	if (!(s.status & ST_RXRDY)) {
		s.rx = s.replies.n ? q_take(&s.replies, REPLYQ) : q_take(&s.keys, KEYQ);
		s.status |= ST_RXRDY;
	}
	pic_raise(1);
	ev_next(EV_KEYBOARD, s.byte_time, byte_time);
}

static void start_line(void)
{
	if (!ev_armed(EV_KEYBOARD)) ev_at(EV_KEYBOARD, s.byte_time, byte_time);
}

void kbd_put_key(REG8 code)
{
	if (q_put(&s.keys, KEYQ, code)) start_line();
	else s.status |= ST_OVERRUN;
}

void kbd_put_reply(REG8 data)
{
	if (q_put(&s.replies, REPLYQ, data)) start_line();
}

void kbd_drop_replies(void)
{
	s.replies.n = 0;
}

void kbd_changeclock(void)
{
	s.byte_time = (SINT32)(g_mach.cpu_clock / 1920);
}

static void data_w(UINT port, REG8 v)
{
	(void)port;
	if (s.cmd & CMD_TXEN) keyunit_command(v);
}

static void command_w(UINT port, REG8 v)
{
	(void)port;
	if ((s.cmd & CMD_BREAK) && !(v & CMD_BREAK)) {
		ev_cancel(EV_KEYBOARD);
		s.status = 0;
		s.keys.n = 0;
		s.replies.n = 0;
		s.cmd = 0;
		keyunit_reset_signal();
		return;
	}
	if (v & CMD_ERRRESET) s.status &= (UINT8)~ST_OVERRUN;
	s.cmd = (UINT8)v;
}

static REG8 data_r(UINT port)
{
	(void)port;
	s.status &= (UINT8)~ST_RXRDY;
	pic_lower(1);
	return s.rx;
}

static REG8 status_r(UINT port)
{
	(void)port;
	return (REG8)(s.status | ST_ALWAYS);
}

void kbd_reset(const Pc98Settings *cfg)
{
	(void)cfg;
	memset(&s, 0, sizeof(s));
	s.rx = 0xff;
}

void kbd_bind(void)
{
	static const io_out_fn out[2] = { data_w, command_w };
	static const io_in_fn in[2] = { data_r, status_r };

	kbd_changeclock();
	io_sys(0x41, 0xcf1, out, in, 2);
}
