/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * fdc765.c - the floppy disk controller: a uPD765A with the PC-98's two interfaces, written from the uPD765A data sheet
 * against the floppy media model (media/fdmedia.h).
 *
 * Ports
 *   1 MB interface 90h / 92h / 94h (IRQ 11, DMA channel 2), 640 KB interface C8h / CAh / CCh (IRQ 10, DMA channel 3);
 *   BEh bit 0 selects which of the two answers (1 = the 1 MB one), bit 1 the recording (1 = 2HD, 0 = 2DD).
 *   90h / C8h read: main status;  92h / CAh: data;
 *   94h / CCh write: bit 7 reset (the controller resets when it goes back to 0), bit 6 force ready, bit 3 motor on
 *   (when BEh bit 2 gives the motors to it; otherwise they always turn).  A drive raises READY 0.5 s after its
 *   motor started;
 *   read: interface identification
 *   4BEh (1.44 MB interface): write bit 4 set: drive (bits 6-5) turns at 300 rpm when bit 0 is set, else 360;
 *   read bit 0: the selected drive's setting.
 * Main status: bits 3-0 drive n seeking, bit 4 busy, bit 5 non-DMA execution, bit 6 data to the CPU, bit 7 request.
 *
 * Commands: READ DATA 06h, READ DELETED DATA 0Ch, WRITE DATA 05h, WRITE DELETED DATA 09h, READ DIAGNOSTIC 02h,
 * READ ID 0Ah, FORMAT 0Dh, SCAN EQUAL 11h / LOW OR EQUAL 19h / HIGH OR EQUAL 1Dh, RECALIBRATE 07h, SENSE INTERRUPT 08h,
 * SPECIFY 03h, SENSE DRIVE 04h, SEEK 0Fh; anything else answers ST0 = 80h.
 *   A sector is the next ID of the selected density (MF) that passes the head and matches C, H, R, N; when none comes
 *   in two turns of the disk: ND, or MA if no ID of that density passed at all.  A deleted mark ends READ DATA after
 *   the sector with CM (READ DELETED DATA the other way round), unless SK skips it.  A data CRC error ends the command
 *   after the sector with DE / DD.  After a sector R goes up; past EOT the command goes on to head 1 with MT on head 0,
 *   otherwise it ends with EN.  TC ends the command after the byte it comes with; the result then names the sector
 *   after the last one (R + 1, or the first sector of the next head / cylinder at EOT).
 *   SEEK and RECALIBRATE take SRT per step and interrupt when they are done; SENSE INTERRUPT reports them and the
 *   changes of the drives' READY lines, which the controller watches (and one for every drive after a reset).
 * Time: the result and the interrupt come when the operation would end on a real drive: the rotation turns with the
 * emulated time, a sector is found when its ID passes, its data take their bytes' time.
 * A track that an media backend is still reading holds the command (fdc_waiting) until
 * fdc_resume() finds it ready. */
#include <stdlib.h>
#include <string.h>
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <devices.h>
#include <host.h>
#include <media/fdmedia.h>
#include <media/drives.h>

#if defined(FDC_TRACE) || defined(PC98_DEBUG_LOG)
#include <stdio.h>
#define TRACE(...) printf(__VA_ARGS__)
#else
#define TRACE(...) ((void)0)
#endif

#define MSR_CB		0x10
#define MSR_NDM		0x20
#define MSR_DIO		0x40
#define MSR_RQM		0x80

#define ST0_NR		0x08
#define ST0_EC		0x10
#define ST0_SE		0x20
#define ST0_ABN		0x40
#define ST0_INV		0x80
#define ST0_RDY		0xc0
#define ST1_MA		0x01
#define ST1_NW		0x02
#define ST1_ND		0x04
#define ST1_OR		0x10
#define ST1_DE		0x20
#define ST1_EN		0x80
#define ST2_MD		0x01
#define ST2_BC		0x02
#define ST2_SN		0x04
#define ST2_SH		0x08
#define ST2_WC		0x10
#define ST2_DD		0x20
#define ST2_CM		0x40

enum { PH_CMD, PH_EXEC, PH_RESULT, PH_HOLD };
enum { X_NONE, X_READ, X_WRITE, X_DIAG, X_FORMAT, X_SCAN };

UINT8 fdc_waiting;

static struct {
	UINT8	iface;			/* BEh */
	UINT8	ctrl;			/* 94h / CCh */
	UINT8	rpm[4], sel4be;
	UINT8	equip, has144;
	UINT8	phase;
	UINT8	cmd[9], ncmd, need;
	UINT8	res[7], nres, rpos;
	UINT8	pcn[4];
	UINT8	irq_ready[4];		/* SENSE INTERRUPT reports of READY changes: 0 none, else ST0 + 1 */
	UINT8	irq_seek[4];		/* ... of finished seeks */
	UINT64	seek_due[4];		/* clock when a seek of drive n ends (0 none) */
	UINT64	poll_due[4];		/* clock when the READY poll after a reset reaches drive n */
	UINT64	poll_base;		/* clock of the last reset: the drives' poll slots count from it */
	UINT8	seek_target[4], seek_st0[4];
	UINT8	srt, hut, hlt, nd;
	UINT8	irq_on;
	UINT8	ready[4];		/* the READY lines as last seen */
	UINT64	motor_since;		/* clock when the motors started */
	UINT64	motor_off;		/* clock when they were stopped */
	UINT8	ever_on;
	/* the running read / write type command */
	UINT8	kind, us, hd, c, h, r, n, eot, gpl, dtl, mt, mf, sk, fill, sc;
	UINT8	st0, st1, st2, tc;
	UINT8	buf[16384];
	UINT32	len, pos;
	UINT8	byte_ready;
	UINT64	data_start, data_clocks; /* absolute first-bit time and full data duration */
	int	sec;			/* index of the sector in the track */
	UINT32	nsec;			/* sectors done (READ DIAGNOSTIC, FORMAT) */
	UINT64	t;			/* the controller's time cursor (clocks) */
	void	(*stage)(void);		/* what to run again when a held command resumes */
	/* clock */
	UINT64	clk64;
	UINT32	clk_last;
} F;

static UINT64 now(void)
{
	const UINT32 c = (UINT32)(g_cpu.clocks_done + g_cpu.slice_len - g_cpu.slice_left);

	F.clk64 += (UINT32)(c - F.clk_last);
	F.clk_last = c;
	return F.clk64;
}

static UINT64 us_to_clk(UINT64 us) { return us * (g_mach.cpu_clock ? g_mach.cpu_clock : 1) / 1000000u; }
static UINT64 clk_to_us(UINT64 c) { return c * 1000000u / (g_mach.cpu_clock ? g_mach.cpu_clock : 1); }

static int big_iface(void) { return F.iface & 1; }
static UINT irq_line(void) { return big_iface() ? 11 : 10; }
static UINT dma_ch(void) { return big_iface() ? 2 : 3; }

/* ---- interrupt line ---- */

static void irq(int on)
{
	if (on && !F.irq_on) pic_raise((REG8)irq_line());
	if (!on && F.irq_on) pic_lower((REG8)irq_line());
	F.irq_on = (UINT8)on;
}

/* ---- drives ---- */

#define SPINUP_US	500000u		/* a drive raises READY this long after its motor started */

static UINT64 now(void);

/* 94h bit 3 runs the drive motors.  A motor that was stopped needs SPINUP_US before the drive is ready; a short stop
 * (under STOP_US) does not slow the disk down enough to matter. */
#define STOP_US		200000u

/* BEh bit 2 gives the motors to 94h bit 3; while it is clear they turn all the time (from power on) */
static int motor_on(void)
{
	return !(F.iface & 0x04) || (F.ctrl & 0x08) != 0;
}

static void motor_check(int was_on)
{
	const UINT64 t = now();

	if (motor_on() && !was_on && (!F.ever_on || t - F.motor_off >= us_to_clk(STOP_US))) F.motor_since = t;
	if (motor_on()) F.ever_on = 1;
	if (!motor_on() && was_on) F.motor_off = t;
}

/* the READY line of a drive: a disk in a fitted drive whose motor has been turning long enough */
static int line_ready(UINT us)
{
	return (F.equip & (1u << us)) && drv_fd_ready((int)us) && motor_on() &&
	       now() - F.motor_since >= us_to_clk(SPINUP_US);
}

/* READY as the controller sees it (force ready makes every drive select look ready) */
static int drive_ready(UINT us)
{
	return (F.ctrl & 0x40) || line_ready(us);
}

/* the controller watches the READY lines: a change is an interrupt, reported by SENSE INTERRUPT */
static void poll_ready(void)
{
	UINT d;

	for (d = 0; d < 4; d++) {
		const UINT8 r = (UINT8)drive_ready(d);
		if (r != F.ready[d]) {
			F.ready[d] = r;
			F.irq_ready[d] = (UINT8)((ST0_RDY | d | (r ? 0 : ST0_NR)) + 1);
			irq(1);
		}


	}
}

static fm_cfg_t drive_cfg(UINT us)
{
	fm_cfg_t cfg;

	cfg.media = (F.iface & 2) ? FM_MEDIA_2HD : FM_MEDIA_2DD;
	cfg.hd_rot = (F.iface & 2) ? F.rpm[us] : FM_ROT_360;
	return cfg;
}

/* ---- phases ---- */

static void idle(void)
{
	ev_cancel(EV_FDCBYTE);
	F.byte_ready = 0;
	F.phase = PH_CMD;
	F.ncmd = 0;
	F.kind = X_NONE;
	F.stage = NULL;
	fdc_waiting = 0;
}

static void result(UINT n)
{
	F.nres = (UINT8)n;
	F.rpos = 0;
	F.phase = PH_RESULT;
}

static void end_event(UINT id, int timed);

/* the command is over: the result phase and the interrupt come at time F.t */
static void finish_at_t(void)
{
	const UINT64 t = now();

	ev_cancel(EV_FDCBYTE);
	F.byte_ready = 0;
	TRACE("FDC finish st0 %02x st1 %02x pos %u/%u tc %u t+%lld\n", F.st0, F.st1, (unsigned)F.pos, (unsigned)F.len, F.tc, (long long)(F.t - t));
	dma_request(dma_ch(), 0);
	F.res[0] = (UINT8)(F.st0 | (F.hd << 2) | F.us);
	F.res[1] = F.st1;
	F.res[2] = F.st2;
	F.res[3] = F.c;
	F.res[4] = F.h;
	F.res[5] = F.r;
	F.res[6] = F.n;
	F.kind = X_NONE;
	if (F.t > t) {
		F.phase = PH_EXEC;
		ev_at(EV_FDCINT, (SINT32)(F.t - t), end_event);
	}
	else {
		result(7);
		irq(1);
	}
}

static void end_event(UINT id, int timed)
{
	(void)id;
	TRACE("FDC end_event timed %d phase %u\n", timed, F.phase);
	if (!timed) return;
	result(7);
	irq(1);
}

static void fail(UINT8 st0, UINT8 st1, UINT8 st2)
{
	F.st0 |= st0;
	F.st1 |= st1;
	F.st2 |= st2;
	finish_at_t();
}

/* ---- tracks and sectors ---- */

static int get_track(const fm_track_t **t)
{
	fm_disk_t *d = fdd_media_disk(F.us);
	const fm_cfg_t cfg = drive_cfg(F.us);
	int r;

	*t = NULL;
	if (!d || !fm_disk_compatible(d, &cfg)) return 0;
	r = fm_disk_track_ex(d, F.pcn[F.us], F.hd, t);
	if (r == FM_E_NOTREADY) return -1;
	return r == FM_OK;
}

/* hold the command until the track arrives */
static void hold(void (*stage)(void))
{
	F.stage = stage;
	F.phase = PH_HOLD;
	fdc_waiting = 1;
}

static UINT32 phase_now(const fm_track_t *t)
{
	return t->rev_us ? (UINT32)(clk_to_us(F.t) % t->rev_us) : 0;
}

/* the next sector of the selected density whose ID matches C H R N, in the order they pass the head */
static int find_sector(const fm_track_t *t)
{
	fm_disk_t *d = fdd_media_disk(F.us);
	const fm_cfg_t cfg = drive_cfg(F.us);
	const UINT32 ph = phase_now(t);
	UINT32 best_w = 0;
	int i, best = -1, seen = 0;

	for (i = 0; i < t->nsect; i++) {
		const fm_sector_t *s = &t->sec[i];
		UINT32 w;
		if (s->no_id || (s->fm != 0) != (F.mf == 0) || !fm_sector_visible(d, s, &cfg)) continue;
		seen = 1;
		if (s->r != F.r || s->h != F.h || s->n != F.n) continue;
		if (s->c != F.c) {
			F.st2 |= (s->c == 0xff) ? ST2_BC : ST2_WC;
			continue;
		}
		w = fm_sector_wait_us(t, ph, i);
		if (best < 0 || w < best_w) {
			best = i;
			best_w = w;
		}
	}
	if (best < 0) {
		F.t += us_to_clk(2 * (UINT64)t->rev_us);
		F.st1 |= seen ? ST1_ND : ST1_MA;
		return -1;
	}
	F.st2 &= (UINT8)~(ST2_WC | ST2_BC);
	F.t += us_to_clk(best_w);
	return best;
}

static UINT32 data_size(void)
{
	return F.n ? (128u << (F.n > 7 ? 7 : F.n)) : F.dtl;
}

/* after sector R: the next one, or the end */
static void advance(void)
{
	if (F.r != F.eot) {
		F.r++;
		return;
	}
	F.r = 1;
	if (F.mt && !F.hd) {
		F.hd = 1;
		F.h ^= 1;
		return;
	}
	if (F.mt) {
		F.hd = 0;
		F.h ^= 1;
	}
	F.c++;
	F.st0 |= ST0_ABN;
	F.st1 |= ST1_EN;
}

/* ---- read / write / scan ---- */

static void rw_sector(void);

/* The 765 requests one byte when it passes the head, not a whole-sector DMA burst.
 * Deadlines are derived from the whole field so rounding/CPU acknowledgement latency cannot accumulate. */
static void byte_event(UINT id, int timed);

static void arm_byte(UINT32 ordinal)
{
    const UINT64 due = F.data_start + F.data_clocks * ordinal / F.len;
    const UINT64 t = now();
    ev_at(EV_FDCBYTE, due > t ? (SINT32)(due - t) : 0, byte_event);
}

static void byte_event(UINT id, int timed)
{
    (void)id;
    if (!timed || F.phase != PH_EXEC || F.kind == X_NONE || F.pos >= F.len) return;
    if (F.byte_ready) {
        fail(ST0_ABN, ST1_OR, 0);
        return;
    }
    F.byte_ready = 1;
    if (F.nd) irq(1); else dma_request(dma_ch(), 1);
    if (F.pos + 1 < F.len) arm_byte(F.pos + 2);
}

static void start_transfer(UINT32 len, int to_host)
{
    const fm_track_t *t = fm_disk_current(fdd_media_disk(F.us));
    const fm_cfg_t cfg = drive_cfg(F.us);
    UINT64 byte_ns = fm_media_byte_ns(cfg.media, cfg.hd_rot) * (F.mf ? 1u : 2u);
    UINT64 data_ns;
    const UINT64 clock = g_mach.cpu_clock ? g_mach.cpu_clock : 1;
    const UINT64 current = now();
    const int reading = F.kind == X_READ || F.kind == X_DIAG || F.kind == X_SCAN;
    if (t && t->byte_ns && reading) byte_ns = (UINT64)t->byte_ns * (F.mf ? 1u : 2u);
    data_ns = byte_ns * len;
    if (t && reading && F.sec >= 0 && F.sec < t->nsect) {
        const fm_sector_t *sec = &t->sec[F.sec];
        if (sec->data_ns && sec->data_len) data_ns = (UINT64)sec->data_ns * len / sec->data_len;
    }
    if (!len) { fail(ST0_ABN, ST1_ND, 0); return; }
    F.len = len;
    F.pos = 0;
    F.phase = PH_EXEC;
    F.byte_ready = 0;
    F.data_clocks = data_ns * clock / 1000000000u;
    if (F.data_clocks < len) F.data_clocks = len;
    /* F.t includes the field and trailing CRC; data starts before those bytes. */
    {
        const UINT64 tail = byte_ns * 2 * clock / 1000000000u;
        const UINT64 span = F.data_clocks + tail;
        F.data_start = F.t > span ? F.t - span : 0;
        if (F.kind == X_FORMAT || F.data_start < current) F.data_start = current;
        if (F.t < F.data_start + span) F.t = F.data_start + span;
    }
    (void)to_host;
    TRACE("FDC xfer len %u nd %u data_ns %llu t+%lld\n", (unsigned)len, (unsigned)F.nd,
          (unsigned long long)data_ns, (long long)(F.t - current));
    dma_request(dma_ch(), 0);
    arm_byte(1);
}

static void rw_sector(void)
{
	fm_disk_t *d = fdd_media_disk(F.us);
	const fm_track_t *t;
	const fm_sector_t *s;
	const int g = get_track(&t);
	int idx;

	if (g < 0) {
		hold(rw_sector);
		return;
	}
	fdc_waiting = 0;
	if (!g) {
		F.t += us_to_clk(400000);
		fail(ST0_ABN, ST1_MA, 0);
		return;
	}
	for (;;) {
		idx = find_sector(t);
		if (idx < 0) {
			fail(ST0_ABN, 0, 0);
			return;
		}
		s = &t->sec[idx];
		F.sec = idx;
		if (F.kind == X_WRITE) break;
		/* deleted mark: READ DATA wants a normal one, READ DELETED DATA a deleted one */
		if ((s->deleted != 0) != ((F.cmd[0] & 0x1f) == 0x0c)) {
			F.st2 |= ST2_CM;
			if (F.sk) {
				F.t += us_to_clk(fm_sector_span_us(t, idx));
				advance();
				if (F.st1 & ST1_EN) {
					finish_at_t();
					return;
				}
				continue;
			}
		}
		break;
	}
	if (!s->id_crc_ok) {
		fail(ST0_ABN, ST1_DE, 0);
		return;
	}
	if (s->no_data) {
		fail(ST0_ABN, ST1_MA, ST2_MD);
		return;
	}
	F.t += us_to_clk(fm_sector_span_us(t, idx));
	host_fdd_access(F.us);
	if (F.kind == X_WRITE) {
		if (fm_disk_protected(d)) {
			fail(ST0_ABN, ST1_NW, 0);
			return;
		}
		start_transfer(data_size(), 0);
		return;
	}
	memset(F.buf, 0, sizeof(F.buf));
	fm_sector_read(d, t, idx, F.buf, data_size() < sizeof(F.buf) ? data_size() : sizeof(F.buf));
	if (!s->data_crc_ok) {
		F.st0 |= ST0_ABN;
		F.st1 |= ST1_DE;
		F.st2 |= ST2_DD;
	}
	start_transfer(data_size(), 1);
}

/* the sector's bytes have all moved (or TC came) */
static void sector_done(void)
{
	fm_disk_t *d = fdd_media_disk(F.us);
	const fm_track_t *t;

	if (F.kind == X_WRITE && get_track(&t) > 0) {
		if (F.pos < F.len) memset(F.buf + F.pos, 0, F.len - F.pos);
		fm_sector_write(d, t, F.sec, F.buf, F.len);
		fm_sector_set_marks(d, t, F.sec, (F.cmd[0] & 0x1f) == 0x09);
	}
	if (F.kind == X_SCAN && !(F.st2 & ST2_SN)) {
		F.st2 |= ST2_SH;
		F.tc = 1;
	}
	dma_request(dma_ch(), 0);
	if (F.st0 & ST0_ABN) {
		finish_at_t();
		return;
	}
	if (F.kind == X_SCAN) {
		const UINT step = (F.cmd[8] ? F.cmd[8] : 1);
		F.r = (UINT8)(F.r + step - 1);
	}
	advance();
	if (F.tc || (F.st1 & ST1_EN)) {
		if (F.tc) {
			F.st0 &= (UINT8)~ST0_ABN;
			F.st1 &= (UINT8)~ST1_EN;
		}
		finish_at_t();
		return;
	}
	if (F.st2 & ST2_CM) {			/* the sector with the other mark was read: the command ends */
		finish_at_t();
		return;
	}
	rw_sector();
}

/* ---- READ DIAGNOSTIC ---- */

/* A recorded track gives the bytes really on the track: the data field and, when N asks for more than the sector holds,
 * its CRC and the gap after it.  Read once per command (the n-byte runs after each data mark, from the index). */
static UINT8 s_diag[16384];
static int s_diag_got;

static int diag_raw(fm_disk_t *d)
{
	const UINT32 n = data_size();
	int bad;

	if (F.nsec == 0) {
		s_diag_got = fm_disk_read_diag(d, F.pcn[F.us], F.hd, F.n, s_diag, sizeof(s_diag), &bad);
	}
	if (s_diag_got < 0 || (UINT32)s_diag_got < (F.nsec + 1) * n || n > sizeof(F.buf)) return 0;
	memcpy(F.buf, s_diag + F.nsec * n, n);
	return 1;
}

static void diag_sector(void)
{
	fm_disk_t *d = fdd_media_disk(F.us);
	const fm_track_t *t;
	const int g = get_track(&t);
	const fm_sector_t *s;

	if (g < 0) {
		hold(diag_sector);
		return;
	}
	fdc_waiting = 0;
	if (!g || (int)F.nsec >= t->nsect) {
		if (!g) F.t += us_to_clk(400000);
		fail(ST0_ABN, g ? ST1_EN : ST1_MA, 0);
		return;
	}
	if (F.nsec == 0) F.t += us_to_clk(t->rev_us - phase_now(t));
	s = &t->sec[F.nsec];
	if (s->c != F.c || s->h != F.h || s->r != F.r || s->n != F.n) F.st1 |= ST1_ND;
	F.sec = (int)F.nsec;
	F.t += us_to_clk(fm_sector_span_us(t, F.sec));
	memset(F.buf, 0, sizeof(F.buf));
	if (!diag_raw(d)) fm_sector_read(d, t, F.sec, F.buf, data_size());
	host_fdd_access(F.us);
	start_transfer(data_size(), 1);
}

static void diag_done(void)
{
	dma_request(dma_ch(), 0);
	F.nsec++;
	if (F.tc) {
		F.st1 &= (UINT8)~ST1_ND;
		advance();
		F.st0 &= (UINT8)~ST0_ABN;
		F.st1 &= (UINT8)~ST1_EN;
		finish_at_t();
		return;
	}
	if (F.nsec >= F.eot) {
		advance();
		finish_at_t();
		return;
	}
	F.r++;
	diag_sector();
}

/* ---- FORMAT ---- */

static fm_format_t s_fmt;

static void format_done(void)
{
	fm_disk_t *d = fdd_media_disk(F.us);
	const fm_cfg_t cfg = drive_cfg(F.us);
	UINT i;
	int r;

	dma_request(dma_ch(), 0);
	s_fmt.nsect = F.sc;
	for (i = 0; i < F.sc; i++) {
		fm_fmt_sector_t *s = &s_fmt.sec[i];
		s->c = F.buf[i * 4];
		s->h = F.buf[i * 4 + 1];
		s->r = F.buf[i * 4 + 2];
		s->n = F.buf[i * 4 + 3];
		s->fm = (UINT8)!F.mf;
		s->rot = cfg.hd_rot;
		s->fill = F.fill;
		s->data_len = (UINT16)(128u << (F.n > 7 ? 7 : F.n));
	}
	r = d ? fm_disk_format_track(d, F.pcn[F.us], F.hd, &cfg, &s_fmt) : FM_E_IO;
	F.t += us_to_clk(2 * (UINT64)fm_media_rev_us(cfg.media, cfg.hd_rot));
	if (r == FM_E_PROTECT) F.st1 |= ST1_NW;
	if (r != FM_OK) F.st0 |= ST0_ABN;
	finish_at_t();
}

/* ---- the data path (DMA or the data port) ---- */

static void byte_moved(void)
{
	F.byte_ready = 0;
	if (F.nd) irq(0); else dma_request(dma_ch(), 0);
	if (++F.pos < F.len && !F.tc) return;
	switch (F.kind) {
	case X_DIAG: diag_done(); break;
	case X_FORMAT: format_done(); break;
	default: sector_done(); break;
	}
}

static REG8 to_host(void)
{
	REG8 v;

	if (F.phase != PH_EXEC || (F.kind != X_READ && F.kind != X_DIAG) || F.pos >= F.len || !F.byte_ready) return 0xff;
	v = F.buf[F.pos];
	byte_moved();
	return v;
}

static void from_host(REG8 v)
{
	if (F.phase != PH_EXEC || F.pos >= F.len || !F.byte_ready) return;
	if (F.kind == X_WRITE || F.kind == X_FORMAT) {
		F.buf[F.pos] = (UINT8)v;
	}
	else if (F.kind == X_SCAN) {
		const UINT8 m = F.buf[F.pos];
		const UINT op = F.cmd[0] & 0x1f;
		if (v != 0xff && m != 0xff) {
			if ((op == 0x11 && m != v) || (op == 0x19 && m > v) || (op == 0x1d && m < v)) F.st2 |= ST2_SN;
		}
	}
	else {
		return;
	}
	byte_moved();
}

static void dma_tc(void)
{
	F.tc = 1;
}

/* ---- commands ---- */

static UINT param_bytes(UINT8 c)
{
	switch (c & 0x1f) {
	case 0x02: case 0x05: case 0x06: case 0x09: case 0x0c: case 0x11: case 0x19: case 0x1d: return 9;
	case 0x03: return 3;
	case 0x04: case 0x07: case 0x0a: return 2;
	case 0x0d: return 6;
	case 0x0f: return 3;
	default: return 1;
	}
}

static void take_chrn(void)
{
	F.us = F.cmd[1] & 3;
	F.hd = (F.cmd[1] >> 2) & 1;
	F.c = F.cmd[2];
	F.h = F.cmd[3];
	F.r = F.cmd[4];
	F.n = F.cmd[5];
	F.eot = F.cmd[6];
	F.gpl = F.cmd[7];
	F.dtl = F.cmd[8];
	F.mt = (F.cmd[0] >> 7) & 1;
	F.mf = (F.cmd[0] >> 6) & 1;
	F.sk = (F.cmd[0] >> 5) & 1;
	F.st0 = F.st1 = F.st2 = 0;
	F.tc = 0;
	F.nsec = 0;
	{
		const UINT64 t = now();
		if (F.t < t) F.t = t;
	}
	if (F.seek_due[F.us]) {			/* the head is still moving: the command starts where and when it stops */
		if (F.t < F.seek_due[F.us]) F.t = F.seek_due[F.us];
		F.pcn[F.us] = F.seek_target[F.us];
	}
}

static void seek_event(UINT id, int timed);

static void seek_rearm(void)
{
	const UINT64 t = now();
	UINT64 due = 0;
	UINT i;

	for (i = 0; i < 4; i++) {
		if (F.seek_due[i] && (!due || F.seek_due[i] < due)) due = F.seek_due[i];
		if (F.poll_due[i] && (!due || F.poll_due[i] < due)) due = F.poll_due[i];
	}
	if (due) ev_at(EV_FDCSTATE, (SINT32)(due > t ? due - t : 1), seek_event);
	else ev_cancel(EV_FDCSTATE);
}

static void seek_event(UINT id, int timed)
{
	const UINT64 t = now();
	UINT i;

	(void)id;
	if (!timed) return;
	for (i = 0; i < 4; i++) {
		if (F.seek_due[i] && F.seek_due[i] <= t) {
			F.seek_due[i] = 0;
			F.pcn[i] = F.seek_target[i];
			F.irq_seek[i] = (UINT8)(F.seek_st0[i] + 1);
			irq(1);
		}
		if (F.poll_due[i] && F.poll_due[i] <= t) {
			F.poll_due[i] = 0;
			F.irq_ready[i] = (UINT8)((ST0_RDY | i | (F.ready[i] ? 0 : ST0_NR)) + 1);
			irq(1);
		}
	}
	seek_rearm();
}

/* The controller looks at the drives in turn, drive n every POLL_US at its own slot ((n + 1) quarters into the round
 * counted from the last reset); a seek's end, like a READY change, is seen at the drive's next slot. */
#define POLL_US		1024u

static UINT64 next_poll(UINT us, UINT64 t)
{
	const UINT64 period = us_to_clk(POLL_US);
	UINT64 slot = F.poll_base + us_to_clk((UINT64)POLL_US / 4 * (us + 1));

	if (t > slot) slot += (t - slot + period - 1) / period * period;
	return slot;
}

static void seek(UINT us, UINT target, int recal)
{
	const UINT steps = recal ? F.pcn[us] : (target > F.pcn[us] ? target - F.pcn[us] : F.pcn[us] - target);
	const UINT64 step_us = (UINT64)(16 - (F.srt & 15)) * 1000u * ((F.iface & 2) ? 1u : 2u);
	UINT8 st0 = (UINT8)(ST0_SE | us);

	if (!drive_ready(us)) st0 |= ST0_ABN | ST0_NR;
	else if (recal && !(F.equip & (1u << us))) st0 |= ST0_ABN | ST0_EC;	/* no drive: track 0 never comes */
	F.seek_target[us] = (st0 & ST0_ABN) ? F.pcn[us] : (UINT8)target;
	F.seek_st0[us] = st0;
	/* the controller holds one interrupt status per drive: the seek's replaces a READY change not yet reported */
	F.irq_ready[us] = 0;
	F.poll_due[us] = 0;
	F.ready[us] = (UINT8)drive_ready(us);
	F.seek_due[us] = next_poll(us, now() + us_to_clk(steps * step_us));
	seek_rearm();
	idle();
}


static void command_ready(void)
{
	const UINT8 op = F.cmd[0] & 0x1f;

	TRACE("FDC cmd %02x %02x %02x %02x %02x %02x %02x %02x %02x pcn%u @%ums\n", F.cmd[0], F.cmd[1], F.cmd[2], F.cmd[3], F.cmd[4], F.cmd[5], F.cmd[6], F.cmd[7], F.cmd[8], F.pcn[F.cmd[1] & 3], (unsigned)(clk_to_us(now()) / 1000));

	irq(0);
	switch (op) {
	case 0x06: case 0x0c: case 0x05: case 0x09: case 0x11: case 0x19: case 0x1d:
		take_chrn();
		F.kind = (op == 0x05 || op == 0x09) ? X_WRITE : (op >= 0x11) ? X_SCAN : X_READ;
		if (!drive_ready(F.us)) {
			fail(ST0_ABN | ST0_NR, 0, 0);
			return;
		}
		rw_sector();
		return;
	case 0x02:
		take_chrn();
		F.kind = X_DIAG;
		if (!drive_ready(F.us)) {
			fail(ST0_ABN | ST0_NR, 0, 0);
			return;
		}
		diag_sector();
		return;
	case 0x0a: {
		const fm_track_t *t;
		int g, idx;
		UINT32 w;
		take_chrn();
		F.mf = (F.cmd[0] >> 6) & 1;
		if (!drive_ready(F.us)) {
			fail(ST0_ABN | ST0_NR, 0, 0);
			return;
		}
		g = get_track(&t);
		if (g < 0) {
			hold(command_ready);
			return;
		}
		fdc_waiting = 0;
		if (g && fm_track_next_id(t, phase_now(t), !F.mf, &idx, &w)) {
			F.t += us_to_clk(w + fm_sector_id_span_us(t, idx));
			F.c = t->sec[idx].c;
			F.h = t->sec[idx].h;
			F.r = t->sec[idx].r;
			F.n = t->sec[idx].n;
			if (!t->sec[idx].id_crc_ok) F.st0 |= ST0_ABN, F.st1 |= ST1_DE;
			finish_at_t();
		}
		else {
			F.t += us_to_clk(400000);
			fail(ST0_ABN, ST1_MA, 0);
		}
		return;
	}
	case 0x0d:
		take_chrn();
		F.n = F.cmd[2];
		F.sc = F.cmd[3];
		F.gpl = F.cmd[4];
		F.fill = F.cmd[5];
		F.c = F.h = F.r = 0;
		F.kind = X_FORMAT;
		if (!drive_ready(F.us)) {
			fail(ST0_ABN | ST0_NR, 0, 0);
			return;
		}
		if (fm_disk_protected(fdd_media_disk(F.us))) {
			fail(ST0_ABN, ST1_NW, 0);
			return;
		}
		start_transfer((UINT32)F.sc * 4, 0);
		return;
	case 0x03:
		F.srt = F.cmd[1] >> 4;
		F.hut = F.cmd[1] & 15;
		F.hlt = F.cmd[2] >> 1;
		F.nd = F.cmd[2] & 1;
		idle();
		return;
	case 0x04: {
		const UINT us = F.cmd[1] & 3, hd = (F.cmd[1] >> 2) & 1;
		UINT8 st3 = (UINT8)(us | (hd << 2) | 0x08);
		if (drive_ready(us)) st3 |= 0x20;
		if (!F.pcn[us]) st3 |= 0x10;
		if (drive_ready(us) && drv_fd_protect((int)us)) st3 |= 0x40;
		F.res[0] = st3;
		result(1);
		return;
	}
	case 0x07:
		seek(F.cmd[1] & 3, 0, 1);
		return;
	case 0x0f:
		seek(F.cmd[1] & 3, F.cmd[2], 0);
		return;
	case 0x08: {
		UINT i;
		for (i = 0; i < 4; i++) {
			UINT8 *const q = F.irq_ready[i] ? &F.irq_ready[i] : &F.irq_seek[i];
			if (*q) {
				F.res[0] = (UINT8)(*q - 1);
				F.res[1] = F.pcn[i];
				*q = 0;
				result(2);
				return;
			}
		}
		F.res[0] = ST0_INV;
		result(1);
		return;
	}
	default:
		F.res[0] = ST0_INV;
		result(1);
		return;
	}
}

static UINT s_tr;
static void data_w(UINT port, REG8 v)
{
	if (s_tr < 400) { s_tr++; TRACE("FDC dw%03x %02x ph%u\n", port, v, F.phase); }
	if (((port >> 4) ^ F.iface) & 1) return;
	if (F.phase == PH_EXEC && F.nd) {
		from_host(v);
		return;
	}
	if (F.phase != PH_CMD) return;
	if (!F.ncmd) F.need = (UINT8)param_bytes(v);
	F.cmd[F.ncmd++] = (UINT8)v;
	if (F.ncmd >= F.need) {
		F.ncmd = 0;
		command_ready();
	}
}

static REG8 data_r(UINT port)
{
	REG8 v;

	if (((port >> 4) ^ F.iface) & 1) return 0xff;
	if (F.phase == PH_EXEC && F.nd) return to_host();
	if (F.phase != PH_RESULT) return 0xff;
	v = F.res[F.rpos++];
	if (F.rpos == 1) TRACE("FDC res %02x %02x %02x %02x %02x %02x %02x n%u\n", F.res[0], F.res[1], F.res[2], F.res[3], F.res[4], F.res[5], F.res[6], F.nres);
	if (F.rpos == 1) irq(0);
	if (F.rpos >= F.nres) {
		UINT i;
		idle();
		for (i = 0; i < 4; i++) {
			if (F.irq_ready[i] || F.irq_seek[i]) irq(1);	/* more interrupts to report */
		}
	}
	return v;
}

static REG8 status_r(UINT port)
{
	REG8 v = 0;
	UINT i;
	static UINT last = 0x100;

	if (((port >> 4) ^ F.iface) & 1) return 0xff;
	for (i = 0; i < 4; i++) {
		if (F.seek_due[i]) v |= (REG8)(1u << i);
	}
	switch (F.phase) {
	case PH_CMD: v |= MSR_RQM | (F.ncmd ? MSR_CB : 0); break;
	case PH_RESULT: v |= MSR_RQM | MSR_DIO | MSR_CB; break;
	case PH_EXEC:
		v |= MSR_CB;
		if (F.nd && F.kind != X_NONE) v |= MSR_NDM | (F.byte_ready ? MSR_RQM : 0) | ((F.kind == X_READ || F.kind == X_DIAG) ? MSR_DIO : 0);
		break;
	default: v |= MSR_CB; break;
	}
	if (v != last && s_tr < 400) { s_tr++; last = v; TRACE("FDC sr%03x %02x\n", port, v); }
	return v;
}

static void reset_controller(void)
{
	UINT i;

	ev_cancel(EV_FDCBYTE);
	F.byte_ready = 0;
	ev_cancel(EV_FDCINT);
	dma_request(dma_ch(), 0);
	idle();
	/* after a reset the controller polls the drives one after another and reports their READY state */
	F.poll_base = now();
	for (i = 0; i < 4; i++) {
		F.seek_due[i] = 0;
		F.ready[i] = (UINT8)drive_ready(i);
		F.irq_seek[i] = 0;
		F.irq_ready[i] = 0;
		F.poll_due[i] = next_poll(i, F.poll_base);
	}
	irq(0);
	seek_rearm();
}

static void ctrl_w(UINT port, REG8 v)
{
	const UINT8 old = F.ctrl;

	TRACE("FDC w%03x %02x iface%02x\n", port, v, F.iface);
	if (((port >> 4) ^ F.iface) & 1) return;
	const int was_on = motor_on();
	F.ctrl = (UINT8)v;
	motor_check(was_on);
	if ((old & 0x80) && !(v & 0x80)) reset_controller();	/* the reset pulse ends */
	poll_ready();
}

static REG8 ctrl_r(UINT port)
{
	REG8 v = 0x40;

	if (((port >> 4) ^ F.iface) & 1) return 0xff;
	if (!(port & 0x10)) v |= 0x30;			/* the 640 KB interface: DMA and ready lines fitted */
	v |= (g_mach.dip_switch[0] & 8) ? 0x04 : 0x08;	/* internal / external drives first */
	return v;
}

static void iface_w(UINT port, REG8 v)
{
	const int was_on = motor_on();

	TRACE("FDC wBE %02x\n", v);
	(void)port;
	F.iface = (UINT8)v;
	motor_check(was_on);
	poll_ready();
}

static REG8 iface_r(UINT port)
{
	(void)port;
	return (REG8)((F.iface & 3) | 0xf8);
}

static void rpm_w(UINT port, REG8 v)
{
	(void)port;
	F.sel4be = (UINT8)v;
	if (v & 0x10) F.rpm[(v >> 5) & 3] = (v & 1) ? FM_ROT_300 : FM_ROT_360;
}

static REG8 rpm_r(UINT port)
{
	(void)port;
	return (REG8)(0xfe | (F.rpm[(F.sel4be >> 5) & 3] == FM_ROT_300));
}

/* ---- the machine ---- */

void fdc_resume(void)
{
	if (F.phase == PH_HOLD && F.stage) {
		const UINT64 t = now();
		if (F.t < t) F.t = t;
		F.stage();
	}
}

void fdc_frame(void)
{
	now();
	poll_ready();
}

void fdc_drive_changed(int drive, int ready)
{
	(void)drive;
	(void)ready;
	poll_ready();
}

void fdc_reset(const Pc98Settings *cfg)
{
	static const DmaDevice dev = { from_host, to_host, dma_tc };
	const UINT64 clk = F.clk64;

	memset(&F, 0, sizeof(F));
	F.clk64 = clk;
	F.clk_last = (UINT32)(g_cpu.clocks_done + g_cpu.slice_len - g_cpu.slice_left);
	F.equip = cfg->fdd_fitted;
	F.has144 = cfg->fdd_144;
	F.iface = 3;
	F.ever_on = 1;		/* the motors have turned since power on (motor_since = 0) */
	idle();
	ev_cancel(EV_FDCSTATE);
	ev_cancel(EV_FDCBYTE);
	F.byte_ready = 0;
	ev_cancel(EV_FDCINT);
	dma_connect(2, &dev);
	dma_connect(3, &dev);
}

void fdc_bind(void)
{
	static const io_out_fn out[4] = { NULL, data_w, ctrl_w, NULL };
	static const io_in_fn in[4] = { status_r, data_r, ctrl_r, NULL };
	static const io_out_fn be_out[1] = { iface_w };
	static const io_in_fn be_in[1] = { iface_r };

	io_any(0x90, 0xf9, out, in, 4);
	io_any(0xc8, 0xf9, out, in, 4);
	io_sys(0xbe, 0xcff, be_out, be_in, 1);
	if (F.has144) io_port(0x4be, rpm_w, rpm_r);
}
