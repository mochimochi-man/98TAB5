/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * atapi.c - ATAPI CD-ROM drive (see atapi.h).  Written from the ATA/ATAPI-4 and MMC specifications.
 */
#include <stdlib.h>
#include <string.h>
#include "atapi.h"
#include "../media/cdimg.h"
#include "../include/host.h"

/* status */
#define ST_BSY	0x80
#define ST_DRDY	0x40
#define ST_DSC	0x10
#define ST_DRQ	0x08
#define ST_ERR	0x01
/* interrupt reason (sector count register) */
#define IR_COD	0x01
#define IR_IO	0x02
#define DC_SRST	0x04
#define DC_NIEN	0x02
#define DH_DRV	0x10
#define ER_ABRT	0x04

/* sense keys */
#define SK_NOT_READY	0x02
#define SK_MEDIUM	0x03
#define SK_ILLEGAL	0x05
#define SK_UNIT_ATT	0x06

enum { PH_NONE = 0, PH_PACKET, PH_IN };

static struct {
	int		fitted;
	cd_image_t	*cd;
	char		path[256];
	int		changed;		/* unit attention pending */
	uint8_t		feature, count, sn, cl, ch, dh, devctl;
	uint8_t		status, error;
	int		phase;
	uint8_t		pkt[12];
	int		pktpos;
	uint8_t		buf[2048];
	uint32_t	bufpos, buflen;
	uint32_t	left;			/* bytes still to transfer in this command */
	uint32_t	chunk;			/* bytes left in the current DRQ chunk */
	uint32_t	limit;			/* byte count limit from the host */
	uint32_t	next_lba, sectors;	/* READ: sectors still to load into buf */
	uint8_t		sk, asc, ascq;
} a;

void (*atapi_irq_hook)(void);

static void irq(void)
{
	if (a.devctl & DC_NIEN) return;
	if (atapi_irq_hook) atapi_irq_hook();
}

static void signature(void)
{
	a.count = 1;
	a.sn = 1;
	a.cl = 0x14;
	a.ch = 0xeb;
}

void atapi_fit(int fitted) { a.fitted = fitted ? 1 : 0; }
int atapi_fitted(void) { return a.fitted; }
const char *atapi_disc(void) { return a.cd ? a.path : ""; }

int atapi_insert(const char *path)
{
	if (a.cd) { cd_close(a.cd); a.cd = NULL; }
	a.path[0] = '\0';
	a.changed = 1;
	if (!path || !path[0]) return 0;
	a.cd = cd_open(path);
	if (!a.cd) return -1;
	strncpy(a.path, path, sizeof(a.path) - 1);
	a.path[sizeof(a.path) - 1] = '\0';
	return 0;
}

void atapi_reset(void)
{
	a.phase = PH_NONE;
	a.devctl = 0;
	a.feature = 0;
	a.dh = 0;
	a.status = 0;
	a.error = 1;
	a.changed = 1;
	signature();
}

/* ------------------------------------------------------------------ command end */

static void sense(uint8_t sk, uint8_t asc, uint8_t ascq) { a.sk = sk; a.asc = asc; a.ascq = ascq; }

static void cmd_done(void)
{
	a.phase = PH_NONE;
	a.count = IR_IO | IR_COD;
	a.error = 0;
	a.status = ST_DRDY | ST_DSC;
	sense(0, 0, 0);
	irq();
}

static void cmd_fail(uint8_t sk, uint8_t asc, uint8_t ascq)
{
	a.phase = PH_NONE;
	a.count = IR_IO | IR_COD;
	a.error = (uint8_t)((sk << 4) | ER_ABRT);
	a.status = ST_DRDY | ST_DSC | ST_ERR;
	sense(sk, asc, ascq);
	irq();
}

/* ATA command refused (not a packet command the drive takes). */
static void ata_abort(void)
{
	a.phase = PH_NONE;
	a.error = ER_ABRT;
	a.status = ST_DRDY | ST_ERR;
	irq();
}

/* ------------------------------------------------------------------ data in */

static void next_chunk(void)
{
	uint32_t n = a.left < a.limit ? a.left : a.limit;

	a.chunk = n;
	a.cl = (uint8_t)n;
	a.ch = (uint8_t)(n >> 8);
	a.count = IR_IO;
	a.phase = PH_IN;
	a.status = ST_DRDY | ST_DSC | ST_DRQ;
	irq();
}

/* Start sending a.left bytes (buf already holds the first a.buflen). */
static void start_in(uint32_t total)
{
	a.left = total;
	a.bufpos = 0;
	if (a.left == 0) { cmd_done(); return; }
	next_chunk();
}

/* A reply built in buf, cut to the allocation length. */
static void reply(uint32_t len, uint32_t alloc)
{
	a.sectors = 0;
	a.buflen = len;
	start_in(len < alloc ? len : alloc);
}

static int load_sector(void)
{
	host_cd_access();
	if (cd_read_data(a.cd, a.next_lba, a.buf) != 0) return -1;
	a.next_lba++;
	a.sectors--;
	a.buflen = 2048;
	a.bufpos = 0;
	return 0;
}

static uint8_t data_in(void)
{
	uint8_t v;

	if (a.phase != PH_IN) return 0;
	if (a.bufpos >= a.buflen) {
		if (a.sectors == 0 || load_sector() != 0) {
			cmd_fail(SK_MEDIUM, 0x11, 0x00);	/* unrecovered read error */
			return 0;
		}
	}
	v = a.buf[a.bufpos++];
	a.left--;
	if (--a.chunk == 0) {
		if (a.left == 0) cmd_done();
		else next_chunk();
	}
	return v;
}

/* ------------------------------------------------------------------ packet commands */

static void lba_to_msf(uint32_t lba, uint8_t *p)
{
	lba += 150;
	p[0] = 0;
	p[1] = (uint8_t)(lba / (60 * 75));
	p[2] = (uint8_t)((lba / 75) % 60);
	p[3] = (uint8_t)(lba % 75);
}

static void put_addr(uint8_t *p, uint32_t lba, int msf)
{
	if (msf) { lba_to_msf(lba, p); return; }
	p[0] = (uint8_t)(lba >> 24);
	p[1] = (uint8_t)(lba >> 16);
	p[2] = (uint8_t)(lba >> 8);
	p[3] = (uint8_t)lba;
}

static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* Not ready / unit attention, for the commands that need a disc. */
static int need_disc(void)
{
	if (!a.cd) { cmd_fail(SK_NOT_READY, 0x3a, 0x00); return 0; }	/* medium not present */
	if (a.changed) { a.changed = 0; cmd_fail(SK_UNIT_ATT, 0x28, 0x00); return 0; }
	return 1;
}

static void cmd_inquiry(uint32_t alloc)
{
	memset(a.buf, 0, 36);
	a.buf[0] = 0x05;		/* CD-ROM */
	a.buf[1] = 0x80;		/* removable */
	a.buf[2] = 0x00;
	a.buf[3] = 0x21;		/* ATAPI, response format 1 */
	a.buf[4] = 31;
	memcpy(a.buf + 8, "PC98TAB5", 8);
	memcpy(a.buf + 16, "CD-ROM DRIVE    ", 16);
	memcpy(a.buf + 32, "1.20", 4);
	reply(36, alloc);
}

static void cmd_request_sense(uint32_t alloc)
{
	memset(a.buf, 0, 18);
	a.buf[0] = 0x70;
	a.buf[2] = a.sk;
	a.buf[7] = 10;
	a.buf[12] = a.asc;
	a.buf[13] = a.ascq;
	sense(0, 0, 0);
	{
		uint32_t n = alloc < 18 ? alloc : 18;

		a.sectors = 0;
		a.buflen = 18;
		a.left = n;
		a.bufpos = 0;
		if (n == 0) { a.phase = PH_NONE; a.count = IR_IO | IR_COD; a.error = 0; a.status = ST_DRDY | ST_DSC; irq(); return; }
		next_chunk();
	}
}

static void cmd_read_capacity(void)
{
	uint32_t last = cd_leadout(a.cd) - 1;

	a.buf[0] = (uint8_t)(last >> 24); a.buf[1] = (uint8_t)(last >> 16);
	a.buf[2] = (uint8_t)(last >> 8); a.buf[3] = (uint8_t)last;
	a.buf[4] = 0; a.buf[5] = 0; a.buf[6] = 0x08; a.buf[7] = 0x00;
	reply(8, 8);
}

static void cmd_read(uint32_t lba, uint32_t n)
{
	if (n == 0) { cmd_done(); return; }
	if (lba >= cd_leadout(a.cd) || n > cd_leadout(a.cd) - lba) { cmd_fail(SK_ILLEGAL, 0x21, 0x00); return; }
	a.next_lba = lba;
	a.sectors = n;
	if (load_sector() != 0) { cmd_fail(SK_ILLEGAL, 0x64, 0x00); return; }	/* not a data sector */
	a.left = n * 2048;
	next_chunk();
}

static void cmd_read_toc(const uint8_t *p)
{
	const int msf = (p[1] & 0x02) != 0, fmt = (p[9] >> 6) | (p[2] & 0x0f);
	const uint32_t alloc = be16(p + 7);
	int n = cd_tracks(a.cd), i, start = p[6];
	uint32_t len = 4;

	memset(a.buf, 0, sizeof(a.buf));
	if (fmt == 1) {				/* session information: one session */
		const cd_track_t *t = cd_track(a.cd, 0);

		a.buf[1] = 10; a.buf[2] = 1; a.buf[3] = 1;
		a.buf[5] = t->audio ? 0x10 : 0x14;
		a.buf[6] = t->number;
		put_addr(a.buf + 8, t->start, msf);
		reply(12, alloc);
		return;
	}
	if (fmt != 0) { cmd_fail(SK_ILLEGAL, 0x24, 0x00); return; }
	a.buf[2] = cd_track(a.cd, 0)->number;
	a.buf[3] = cd_track(a.cd, n - 1)->number;
	for (i = 0; i < n; i++) {
		const cd_track_t *t = cd_track(a.cd, i);

		if (t->number < start) continue;
		a.buf[len + 1] = t->audio ? 0x10 : 0x14;
		a.buf[len + 2] = t->number;
		put_addr(a.buf + len + 4, t->start, msf);
		len += 8;
	}
	if (start > 0xaa) { cmd_fail(SK_ILLEGAL, 0x24, 0x00); return; }
	a.buf[len + 1] = 0x14;
	a.buf[len + 2] = 0xaa;
	put_addr(a.buf + len + 4, cd_leadout(a.cd), msf);
	len += 8;
	a.buf[0] = (uint8_t)((len - 2) >> 8);
	a.buf[1] = (uint8_t)(len - 2);
	reply(len, alloc);
}

static void cmd_read_subchannel(const uint8_t *p)
{
	const uint32_t alloc = be16(p + 7);
	const int msf = (p[1] & 0x02) != 0;
	uint32_t len = 4;

	memset(a.buf, 0, 48);
	a.buf[1] = 0x15;			/* no current audio status */
	if (p[2] & 0x40) {
		if (p[3] == 1) {		/* current position */
			const cd_track_t *t = cd_track(a.cd, 0);

			a.buf[4] = 1;
			a.buf[5] = t->audio ? 0x10 : 0x14;
			a.buf[6] = t->number;
			a.buf[7] = 1;
			put_addr(a.buf + 8, t->start, msf);
			put_addr(a.buf + 12, 0, msf);
			len = 16;
		} else {			/* catalogue number / ISRC: none */
			a.buf[4] = p[3];
			len = 24;
		}
		a.buf[3] = (uint8_t)(len - 4);
	}
	reply(len, alloc);
}

static uint32_t mode_page(uint8_t page, uint8_t *o)
{
	switch (page) {
	case 0x01:				/* read error recovery */
		memset(o, 0, 12); o[0] = 0x01; o[1] = 10; return 12;
	case 0x0e:				/* CD audio control */
		memset(o, 0, 16); o[0] = 0x0e; o[1] = 14; o[2] = 0x04;
		o[8] = 0x01; o[9] = 0xff; o[10] = 0x02; o[11] = 0xff; return 16;
	case 0x2a:				/* capabilities */
		memset(o, 0, 20); o[0] = 0x2a; o[1] = 18;
		o[2] = 0x00; o[4] = 0x01; o[6] = 0x29; o[7] = 0x03;
		o[8] = 0x02; o[9] = 0xc2;	/* 706 KB/s = 4x */
		o[10] = 0x01; o[11] = 0x00;	/* 256 volume levels */
		o[12] = 0x00; o[13] = 0x40;
		o[14] = 0x02; o[15] = 0xc2;
		return 20;
	}
	return 0;
}

static void cmd_mode_sense(const uint8_t *p, int ten)
{
	const uint8_t page = p[2] & 0x3f;
	const uint32_t alloc = ten ? be16(p + 7) : p[4];
	const uint32_t hdr = ten ? 8 : 4;
	uint32_t len = hdr, k;

	memset(a.buf, 0, hdr);
	if (page == 0x3f) {
		static const uint8_t all[] = { 0x01, 0x0e, 0x2a };
		for (k = 0; k < sizeof(all); k++) len += mode_page(all[k], a.buf + len);
	} else {
		k = mode_page(page, a.buf + len);
		if (!k) { cmd_fail(SK_ILLEGAL, 0x24, 0x00); return; }
		len += k;
	}
	a.buf[ten ? 2 : 1] = a.cd ? 0x01 : 0x70;	/* medium type: 120mm data / no disc */
	if (ten) { a.buf[0] = (uint8_t)((len - 2) >> 8); a.buf[1] = (uint8_t)(len - 2); }
	else a.buf[0] = (uint8_t)(len - 1);
	reply(len, alloc);
}

static void packet(void)
{
	const uint8_t *p = a.pkt;

	a.phase = PH_NONE;
	a.limit = ((uint32_t)a.ch << 8) | a.cl;
	if (a.limit == 0 || a.limit == 0xffff) a.limit = 0xfffe;
	a.limit &= ~1u;
	if (a.limit == 0) a.limit = 2;
	switch (p[0]) {
	case 0x00:				/* TEST UNIT READY */
		if (need_disc()) cmd_done();
		break;
	case 0x03: cmd_request_sense(p[4]); break;
	case 0x12: cmd_inquiry(p[4]); break;
	case 0x1b:				/* START STOP UNIT */
		if ((p[4] & 0x03) == 0x02) atapi_insert(NULL);	/* eject */
		cmd_done();
		break;
	case 0x1e:				/* PREVENT ALLOW MEDIUM REMOVAL */
	case 0x2b:				/* SEEK */
	case 0x47: case 0x45: case 0xa5:	/* PLAY AUDIO MSF / (10) / (12): no audio output */
	case 0x4b:				/* PAUSE RESUME */
	case 0x4e:				/* STOP PLAY SCAN */
	case 0xbb:				/* SET CD SPEED */
	case 0x55: case 0x15:			/* MODE SELECT */
		if (p[0] == 0x1e || p[0] == 0xbb || p[0] == 0x55 || p[0] == 0x15 || need_disc()) cmd_done();
		break;
	case 0x25: if (need_disc()) cmd_read_capacity(); break;
	case 0x28: if (need_disc()) cmd_read(be32(p + 2), be16(p + 7)); break;
	case 0xa8: if (need_disc()) cmd_read(be32(p + 2), be32(p + 6)); break;
	case 0x43: if (need_disc()) cmd_read_toc(p); break;
	case 0x42: if (need_disc()) cmd_read_subchannel(p); break;
	case 0x1a: cmd_mode_sense(p, 0); break;
	case 0x5a: cmd_mode_sense(p, 1); break;
	case 0x4a:				/* GET EVENT STATUS NOTIFICATION: nothing to report */
		memset(a.buf, 0, 8);
		a.buf[1] = 2; a.buf[2] = 0x80; a.buf[3] = 0x10;
		reply(4, be16(p + 7));
		break;
	default:
		cmd_fail(SK_ILLEGAL, 0x20, 0x00);	/* invalid command operation code */
		break;
	}
}

/* ------------------------------------------------------------------ ATA commands */

static void identify_packet(void)
{
	uint16_t w[256];
	int i;

	memset(w, 0, sizeof(w));
	w[0] = 0x8580;				/* ATAPI, CD-ROM, removable, 12-byte packets, DRQ within 50us */
	{
		static const char *const str[3] = { "PC98TAB5-CD0        ", "1.20    ", "PC98TAB5 CD-ROM DRIVE                   " };
		static const int at[3] = { 10, 23, 27 }, words[3] = { 10, 4, 20 };
		int k, j;

		for (k = 0; k < 3; k++)
			for (j = 0; j < words[k]; j++)
				w[at[k] + j] = (uint16_t)(((uint8_t)str[k][2 * j] << 8) | (uint8_t)str[k][2 * j + 1]);
	}
	w[49] = 0x0200;				/* LBA, no DMA */
	w[51] = 0x0200;
	w[53] = 0x0002;
	w[64] = 0x0001;				/* PIO mode 3 */
	w[67] = 120;
	w[68] = 120;
	w[80] = 0x001e;
	for (i = 0; i < 256; i++) {
		a.buf[2 * i] = (uint8_t)w[i];
		a.buf[2 * i + 1] = (uint8_t)(w[i] >> 8);
	}
	a.sectors = 0;
	a.buflen = 512;
	a.bufpos = 0;
	a.left = 512;
	a.chunk = 512;
	a.phase = PH_IN;
	a.status = ST_DRDY | ST_DSC | ST_DRQ;
	irq();
}

static void execute(uint8_t c)
{
	if (a.dh & DH_DRV) return;		/* no slave on this channel */
	a.error = 0;
	switch (c) {
	case 0xa0:				/* PACKET */
		if (a.feature & 0x01) { ata_abort(); break; }	/* DMA */
		a.pktpos = 0;
		a.phase = PH_PACKET;
		a.count = IR_COD;
		a.status = ST_DRDY | ST_DSC | ST_DRQ;
		break;
	case 0xa1: identify_packet(); break;
	case 0x08:				/* DEVICE RESET */
	case 0x90:				/* EXECUTE DEVICE DIAGNOSTIC */
		a.phase = PH_NONE;
		signature();
		a.error = 1;
		a.status = 0;
		if (c == 0x90) irq();
		break;
	case 0xec:				/* IDENTIFY DEVICE: say "ATAPI" */
		signature();
		ata_abort();
		break;
	case 0xef:				/* SET FEATURES */
	case 0xe5:				/* CHECK POWER MODE */
	case 0xe0: case 0xe1: case 0xe3: case 0xe6:
		if (c == 0xe5) a.count = 0xff;
		a.phase = PH_NONE;
		a.status = ST_DRDY | ST_DSC;
		irq();
		break;
	default:
		ata_abort();
		break;
	}
}

/* ------------------------------------------------------------------ ports */

void atapi_out(uint32_t port, uint8_t v)
{
	switch (port) {
	case 0x640: case 0x641:
		if (a.phase == PH_PACKET && !(a.dh & DH_DRV)) {
			a.pkt[a.pktpos++] = v;
			if (a.pktpos >= 12) packet();
		}
		break;
	case 0x642: a.feature = v; break;
	case 0x644: a.count = v; break;
	case 0x646: a.sn = v; break;
	case 0x648: a.cl = v; break;
	case 0x64a: a.ch = v; break;
	case 0x64c: a.dh = v; break;
	case 0x64e: if (!(a.devctl & DC_SRST)) execute(v); break;
	case 0x74c: {
		const uint8_t old = a.devctl;

		a.devctl = v;
		if (!(v & DC_SRST) && (old & DC_SRST)) {
			a.phase = PH_NONE;
			a.dh = 0;
			signature();
			a.error = 1;
			a.status = 0;
		}
		break;
	}
	}
}

uint8_t atapi_in(uint32_t port)
{
	if (a.dh & DH_DRV) return (port == 0x640 || port == 0x641) ? 0xff : 0x00;
	switch (port) {
	case 0x640: case 0x641: return data_in();
	case 0x642: return a.error;
	case 0x644: return a.count;
	case 0x646: return a.sn;
	case 0x648: return a.cl;
	case 0x64a: return a.ch;
	case 0x64c: return a.dh;
	case 0x64e:
	case 0x74c: return (a.devctl & DC_SRST) ? ST_BSY : a.status;
	case 0x74e: return (uint8_t)(0xc0 | ((~a.dh & 0x0f) << 2) | 0x02);
	}
	return 0xff;
}
