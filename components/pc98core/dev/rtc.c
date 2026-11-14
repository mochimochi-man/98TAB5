/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * rtc.c - the uPD4990A serial calendar clock, and the time it reports.
 *
 * Port 20h (write) drives the chip: bits 2-0 = command C2-C0 (7 = take the command from the serial input), bit 3 =
 * STB (the command runs on its rising edge), bit 4 = CLK (shift on its rising edge), bit 5 = DIN.  DOUT is bit 0 of
 * system port 33h (ppi.c).
 * Commands: 0 register hold, 1 register shift (CLK shifts: DOUT = the bit at the bottom, DIN enters at the top),
 * 2 time set (the time shifted in is taken), 3 time read (the time is loaded into the register).  The rest (timing
 * pulse, interrupt, test) do nothing here.  With C = 7 the clock shifts the 4-bit command register instead.
 * The time in the register, from the bit that comes out first: second, minute, hour, day, month << 4 | weekday,
 * year, all BCD (48 bits).  The register here is 64 bits so a time set works whether 48 or 64 bits were shifted in:
 * the first 48 bits shifted in are the time.
 *
 * The time is the host's clock (host_date: on the Tab5 the RTC that main/rtc_tab5.cpp loads) plus an offset:
 * a time set never writes the host clock, it keeps the difference.  The offset lives in RAM, so it survives an
 * emulated reset but not a power cycle.  The weekday is computed from the date. */
#include <compiler.h>
#include <host.h>
#include <pc98.h>
#include <io.h>
#include <devices.h>

#define STB	0x08
#define CLK	0x10

static UINT8	s_reg[8];	/* s_reg[7] holds the bit that comes out next (bit 0) */
static UINT	s_shifted;	/* bits shifted in since the last register-shift command */
static UINT8	s_shifting;
static UINT8	s_cmd4;		/* serial command register */
static UINT8	s_last;		/* previous write to 20h */
static UINT8	s_dout;

static SINT64	s_offset;	/* seconds added to the host time */

/* ---- dates ---- */

static UINT8 bcd(UINT v) { return (UINT8)(((v / 10) << 4) | (v % 10)); }
static UINT unbcd(UINT8 v) { return (v >> 4) * 10u + (v & 15u); }
static int is_bcd(UINT8 v) { return (v & 15) <= 9 && (v >> 4) <= 9; }

/* days since 1970-01-01 (proleptic Gregorian) and back */
static SINT64 days_of(SINT64 y, UINT m, UINT d)
{
	SINT64 era;
	UINT yoe, doy, doe;

	y -= (m <= 2);
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (UINT)(y - era * 400);
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (SINT64)doe - 719468;
}

static void date_of(SINT64 z, UINT *y, UINT *m, UINT *d)
{
	SINT64 era;
	UINT doe, yoe, doy, mp;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (UINT)(z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = (UINT)((SINT64)yoe + era * 400 + (*m <= 2));
}

static SINT64 seconds_of(const HostDate *t)
{
	return days_of(t->year, t->month, t->day) * 86400 + t->hour * 3600 + t->minute * 60 + t->second;
}

static SINT64 host_seconds(void)
{
	HostDate t = { 1980, 1, 1, 0, 0, 0 };

	if (!host_date(&t)) {
		t.year = 1980;
		t.month = t.day = 1;
		t.hour = t.minute = t.second = 0;
	}
	return seconds_of(&t);
}

/* b: year, month << 4 | weekday, day, hour, minute, second (BCD) */
static void time_read(UINT8 *b)
{
	const SINT64 s = host_seconds() + s_offset;
	SINT64 days = s / 86400, rem = s % 86400;
	UINT y, m, d;

	if (rem < 0) {
		rem += 86400;
		days--;
	}
	date_of(days, &y, &m, &d);
	b[0] = bcd(y % 100);
	b[1] = (UINT8)((m << 4) | (UINT)(((days % 7) + 11) % 7));	/* 1970-01-01 was a Thursday */
	b[2] = bcd(d);
	b[3] = bcd((UINT)(rem / 3600));
	b[4] = bcd((UINT)(rem % 3600 / 60));
	b[5] = bcd((UINT)(rem % 60));
}

/* the same layout; an impossible time is ignored */
static void time_set(const UINT8 *b)
{
	HostDate t;
	UINT year, month;
	SINT64 dim;

	if (!is_bcd(b[0]) || !is_bcd(b[2]) || !is_bcd(b[3]) || !is_bcd(b[4]) || !is_bcd(b[5])) return;
	year = 1900 + unbcd(b[0]);
	if (year < 1980) year += 100;
	month = b[1] >> 4;
	if (month < 1 || month > 12) return;
	dim = days_of(month == 12 ? year + 1 : year, month == 12 ? 1 : month + 1, 1) - days_of(year, month, 1);
	t.year = (UINT16)year;
	t.month = (UINT8)month;
	t.day = (UINT8)unbcd(b[2]);
	t.hour = (UINT8)unbcd(b[3]);
	t.minute = (UINT8)unbcd(b[4]);
	t.second = (UINT8)unbcd(b[5]);
	if (t.day < 1 || t.day > dim || t.hour > 23 || t.minute > 59 || t.second > 59) return;
	s_offset = seconds_of(&t) - host_seconds();
}

/* ---- the chip ---- */

UINT8 rtc_dout(void)
{
	return s_dout;
}

static void command(UINT c)
{
	switch (c) {
	case 1:					/* register shift */
		s_shifting = 1;
		s_shifted = 0;
		break;
	case 2:					/* time set: the first 48 bits that went in */
		if (s_shifting && s_shifted >= 48) {
			/* the first bit in is k - 1 bits below the top after k shifts: the second is in byte (k - 1) / 8,
			 * the year five bytes above it; past 64 bits the oldest bits have dropped out at the bottom */
			const UINT first = s_shifted >= 64 ? 2 : (s_shifted - 1) / 8 - 5;
			time_set(s_reg + first);
		}
		s_shifting = 0;
		break;
	case 3:					/* time read */
		ZeroMemory(s_reg, sizeof(s_reg));
		time_read(s_reg + 2);	/* year in s_reg[2] ... second in s_reg[7] */
		s_shifting = 0;
		break;
	case 0:
		s_shifting = 0;
		break;
	default:
		break;
	}
	s_dout = s_reg[7] & 1;
}

static void shift(UINT din)
{
	int i;

	for (i = 7; i > 0; i--) {
		s_reg[i] = (UINT8)((s_reg[i] >> 1) | (s_reg[i - 1] << 7));
	}
	s_reg[0] = (UINT8)((s_reg[0] >> 1) | (din << 7));
	s_shifted++;
	s_dout = s_reg[7] & 1;
}

static void port20(UINT port, REG8 v)
{
	const UINT rise = v & ~s_last;
	const UINT c = v & 7;

	(void)port;
	s_last = (UINT8)v;
	if (v & STB) {
		if (rise & STB) command(c == 7 ? (UINT)(s_cmd4 & 15) : c);
	}
	else if (rise & CLK) {
		const UINT din = (v >> 5) & 1;
		if (c == 7) s_cmd4 = (UINT8)(((s_cmd4 >> 1) & 7) | (din << 3));
		else if (s_shifting) shift(din);
	}
}

void rtc_reset(const Pc98Settings *cfg)
{
	(void)cfg;
	ZeroMemory(s_reg, sizeof(s_reg));
	s_shifted = 0;
	s_shifting = 0;
	s_cmd4 = 0;
	s_last = 0;
	s_dout = 0;
}

void rtc_bind(void)
{
	static const io_out_fn out[1] = { port20 };

	io_sys(0x20, 0xf1, out, NULL, 1);
}

/* ---- state save / load (state.c) ---- */
#include <stateint.h>
void rtc_state(SsCtx *c)
{
	SS_VAR(c, "RTCr", s_reg);
	SS_VAR(c, "RTCs", s_shifted);
	SS_VAR(c, "RTCh", s_shifting);
	SS_VAR(c, "RTCc", s_cmd4);
	SS_VAR(c, "RTCl", s_last);
	SS_VAR(c, "RTCd", s_dout);
	SS_VAR(c, "RTCo", s_offset);
}
