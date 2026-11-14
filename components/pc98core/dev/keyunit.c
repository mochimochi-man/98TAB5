/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * keyunit.c - the keyboard unit: turns the host's key presses into the bytes the PC-98 keyboard sends, and answers
 * the commands the CPU sends to it.
 *
 *   key bytes   make = key number, break = key number | 80h.  CAPS (71h) and KANA (72h) are locking keys on the
 *               PC-98: a press of the host key flips the lock, and the unit sends make when it locks, break when
 *               it unlocks.
 *   repeat      while a key is held, the newest one held repeats as break + make (g_settings.keyrepeat_*: the first
 *               after `delay` ms, then every `interval` ms, counted in the machine's own time: when it runs slower
 *               than real time the repeats slow down with it instead of piling up in the BIOS key buffer).  Function keys, VF keys, SHIFT, CAPS, KANA, GRPH and
 *               CTRL do not repeat.
 *   commands    9Ch p    repeat setting                  -> FAh
 *               9Dh 60h  read the lock LEDs              -> FAh, 70h | bit 3 KANA | bit 2 CAPS
 *               9Dh 7xh  set the lock LEDs (and locks)   -> FAh
 *               9Fh      identify                        -> FAh, A0h, 80h
 *               other    -> FCh
 *               An FAh / FCh starts a new answer: what was still waiting of an earlier one is dropped.
 *   reset       the interface's reset signal: a command waiting for its parameter is forgotten and every key
 *               still down (locks included) is sent again as make. */
#include <compiler.h>
#include <host.h>
#include <pc98.h>
#include <kbd.h>
#include <cpulink.h>

#define K_CAPS	0x71
#define K_KANA	0x72
#define ACK	0xfa
#define NAK	0xfc

static UINT8 s_down[128];	/* the key is held (or locked) */
static UINT8 s_wait;		/* the command byte whose parameter comes next, or 0 */
static UINT8 s_repeat_set;
static int s_rep_key = -1;	/* the key that repeats */
static UINT32 s_rep_due;	/* machine time (emu_ms()) of its next repeat */
static UINT32 s_ms, s_frac, s_last;

/* Milliseconds of machine time, from the clocks the CPU has run. */
static UINT32 emu_ms(void)
{
	const UINT32 c = g_cpu.clocks_done + (UINT32)(g_cpu.slice_len - g_cpu.slice_left);
	UINT32 d = c - s_last, per = g_mach.cpu_clock / 1000;

	s_last = c;
	if (per == 0) per = 1;
	if (d > g_mach.cpu_clock) d = 0;		/* the count was restarted */
	s_frac += d;
	s_ms += s_frac / per;
	s_frac %= per;
	return s_ms;
}

static int repeats(UINT k)
{
	return !((k >= 0x52 && k <= 0x56) || (k >= 0x62 && k <= 0x6b) || (k >= 0x70 && k <= 0x74) || k == 0x7d);
}

static void send(UINT k, int make)
{
	kbd_put_key((REG8)(make ? k : (k | 0x80)));
}

void keyunit_init(void)
{
	memset(s_down, 0, sizeof(s_down));
	s_wait = 0;
	s_repeat_set = 0;
	s_rep_key = -1;
}

void keyunit_key(UINT code, int down)
{
	const UINT k = code & 0x7f;

	if (code >= 0x80) return;
	if (k == K_CAPS || k == K_KANA) {
		if (down) {
			s_down[k] ^= 1;
			send(k, s_down[k]);
		}
		return;
	}
	if (down) {
		if (s_down[k]) return;
		s_down[k] = 1;
		send(k, 1);
		if (repeats(k)) {
			s_rep_key = (int)k;
			s_rep_due = emu_ms() + g_settings.key_repeat_delay_ms;
		}
	}
	else {
		if (!s_down[k]) return;
		s_down[k] = 0;
		send(k, 0);
		if (s_rep_key == (int)k) s_rep_key = -1;
	}
}

void keyunit_release_all(void)
{
	UINT k;

	for (k = 0; k < 128; k++) {
		if (s_down[k] && k != K_CAPS && k != K_KANA) keyunit_key(k, 0);
	}
}

void keyunit_tick(void)
{
	if (!g_settings.key_repeat || s_rep_key < 0) return;
	{
		const UINT32 now = emu_ms();

		if ((SINT32)(now - s_rep_due) < 0) return;
		s_rep_due = now + g_settings.key_repeat_rate_ms;
	}
	send((UINT)s_rep_key, 0);
	send((UINT)s_rep_key, 1);
}

static void reply(REG8 v)
{
	if (v == ACK || v == NAK) kbd_drop_replies();
	kbd_put_reply(v);
}

void keyunit_command(REG8 v)
{
	const UINT8 cmd = s_wait;

	s_wait = 0;
	if (cmd == 0x9c) {
		s_repeat_set = (UINT8)v;
		reply(ACK);
		return;
	}
	if (cmd == 0x9d) {
		if (v == 0x60) {
			reply(ACK);
			reply((REG8)(0x70 | (s_down[K_KANA] << 3) | (s_down[K_CAPS] << 2)));
		}
		else if ((v & 0xf0) == 0x70) {
			s_down[K_KANA] = (v >> 3) & 1;
			s_down[K_CAPS] = (v >> 2) & 1;
			reply(ACK);
		}
		return;
	}
	switch (v) {
	case 0x9c:
	case 0x9d:
		s_wait = (UINT8)v;
		reply(ACK);
		break;
	case 0x9f:
		reply(ACK);
		reply(0xa0);
		reply(0x80);
		break;
	default:
		reply(NAK);
		break;
	}
}

void keyunit_reset_signal(void)
{
	UINT k;

	s_wait = 0;
	s_rep_key = -1;
	for (k = 0; k < 128; k++) {
		if (s_down[k]) send(k, 1);
	}
}

/* ---- state save / load (state.c) ---- */
#include <stateint.h>
void keyunit_state(SsCtx *c)
{
	SS_VAR(c, "KUdn", s_down);
	SS_VAR(c, "KUwt", s_wait);
	SS_VAR(c, "KUrs", s_repeat_set);
	SS_VAR(c, "KUrk", s_rep_key);
	SS_VAR(c, "KUrd", s_rep_due);
	SS_VAR(c, "KUms", s_ms);
	SS_VAR(c, "KUfr", s_frac);
	SS_VAR(c, "KUls", s_last);
}
