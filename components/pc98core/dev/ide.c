/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * ide.c - PC-98 IDE (ATA) hard disk interface (see ide.h).  Written from the ATA/ATAPI-4 specification;
 * the NEC board specifics (ports, bank select) from the PC-98 hardware documentation and the way
 * the PC-98 BIOS / MS-DOS drive it.
 */
#include <stdlib.h>
#include <string.h>
#include "ide.h"

#ifdef IDE_DIAG
extern int ets_printf(const char *fmt, ...);
static int s_logn;
#define IDE_LOG(...) do { if (s_logn < IDE_DIAG) { s_logn++; ets_printf(__VA_ARGS__); } } while (0)
#else
#define IDE_LOG(...) ((void)0)
#endif

/* status register */
#define ST_BSY	0x80
#define ST_DRDY	0x40
#define ST_DSC	0x10
#define ST_DRQ	0x08
#define ST_ERR	0x01
/* error register */
#define ER_UNC	0x40
#define ER_IDNF	0x10
#define ER_ABRT	0x04
/* device control */
#define DC_SRST	0x04
#define DC_NIEN	0x02
/* drive / head */
#define DH_LBA	0x40
#define DH_DRV	0x10

#define SECSZ		512
#define MAXMULT		16

typedef struct {
	hm_disk_t	*d;
	uint32_t	cyls, heads, spt;	/* native geometry (from the image) */
	uint32_t	total;			/* sectors */
	uint32_t	ccyls, cheads, cspt;	/* current translation (INITIALIZE DEVICE PARAMETERS) */
	uint8_t		mult;			/* SET MULTIPLE MODE block size, 0 = not set */
	uint8_t		status, error;
} drive_t;

enum { PH_NONE = 0, PH_IN, PH_OUT };

static struct {
	drive_t		drv[IDE_DRIVES];
	uint8_t		bank[2];		/* 430h / 432h */
	uint8_t		feature, count, sn, cl, ch, dh;
	uint8_t		devctl;
	uint8_t		intrq;
	/* PIO transfer */
	int		phase;
	uint8_t		cmd;
	uint8_t		*buf;
	uint32_t	pos, len;		/* byte position / length of the current DRQ block */
	uint32_t	rem;			/* sectors still to move including the current block */
	uint32_t	blk;			/* sectors per DRQ block */
	uint32_t	blkn;			/* sectors in the current block */
} s;

void (*ide_irq_hook)(void);

static drive_t *cur(void) { return &s.drv[(s.dh & DH_DRV) ? 1 : 0]; }
static int present(const drive_t *dr) { return dr->d != NULL; }
/* The controller answers only for bank 0 (the primary channel). */
static int channel_on(void) { return (s.bank[1] & 0x7f) == 0; }

int ide_accepts(const hm_disk_t *d) { return d && hm_ata_ok(d); }

int ide_attach(int drive, hm_disk_t *d)
{
	drive_t *dr;

	if (drive < 0 || drive >= IDE_DRIVES) {
		if (d) hm_close(d);
		return -1;
	}
	dr = &s.drv[drive];
	if (dr->d && dr->d != d) hm_close(dr->d);
	memset(dr, 0, sizeof(*dr));
	if (!d) return 0;
	if (!ide_accepts(d)) { hm_close(d); return -1; }
	if (!s.buf) {
		s.buf = (uint8_t *)malloc(MAXMULT * SECSZ);
		if (!s.buf) { hm_close(d); return -1; }
	}
	dr->d = d;
	dr->cyls = dr->ccyls = hm_info(d)->cylinders;
	dr->heads = dr->cheads = hm_info(d)->heads;
	dr->spt = dr->cspt = hm_info(d)->sectors;
	dr->total = hm_sectors(d);
	dr->status = ST_DRDY | ST_DSC;
	dr->error = 1;
	return 0;
}

hm_disk_t *ide_disk(int drive)
{
	return (drive >= 0 && drive < IDE_DRIVES) ? s.drv[drive].d : NULL;
}

/* ------------------------------------------------------------------ helpers */

static void raise_irq(void)
{
	if (s.devctl & DC_NIEN) return;
	s.intrq = 1;
	if (ide_irq_hook) ide_irq_hook();
}

static void set_signature(void)
{
	s.count = 1;
	s.sn = 1;
	s.cl = 0;
	s.ch = 0;
}

static void drive_reset(drive_t *dr)
{
	if (!dr->d) return;
	dr->ccyls = dr->cyls;
	dr->cheads = dr->heads;
	dr->cspt = dr->spt;
	dr->mult = 0;
	dr->status = ST_DRDY | ST_DSC;
	dr->error = 1;
}

void ide_machine_reset(void)
{
	int i;

	s.phase = PH_NONE;
	s.bank[0] = s.bank[1] = 0;
	s.devctl = 0;
	s.intrq = 0;
	s.feature = 0;
	s.dh = 0;
	set_signature();
	for (i = 0; i < IDE_DRIVES; i++) drive_reset(&s.drv[i]);
}

/* Command ends with an error. */
static void fail(drive_t *dr, uint8_t err)
{
	IDE_LOG("IDE fail err %02x cmd %02x\n", err, s.cmd);
	s.phase = PH_NONE;
	dr->error = err;
	dr->status = ST_DRDY | ST_DSC | ST_ERR;
	raise_irq();
}

/* Command ends successfully without data. */
static void done(drive_t *dr)
{
	s.phase = PH_NONE;
	dr->error = 0;
	dr->status = ST_DRDY | ST_DSC;
	raise_irq();
}

/* Resolve the task file into a sector number.  Returns 0 or an error code for the error register. */
static int resolve(const drive_t *dr, uint32_t *lba)
{
	uint32_t v;

	if (s.dh & DH_LBA) {
		v = ((uint32_t)(s.dh & 0x0f) << 24) | ((uint32_t)s.ch << 16) | ((uint32_t)s.cl << 8) | s.sn;
	} else {
		uint32_t cyl = ((uint32_t)s.ch << 8) | s.cl, head = s.dh & 0x0f, sec = s.sn;

		if (sec == 0 || sec > dr->cspt || head >= dr->cheads || cyl >= dr->ccyls) return ER_IDNF;
		v = (cyl * dr->cheads + head) * dr->cspt + (sec - 1);
	}
	if (v >= dr->total) return ER_IDNF;
	*lba = v;
	return 0;
}

/* Put an address back into the task file (the address of the next sector after a transfer). */
static void store_addr(const drive_t *dr, uint32_t lba)
{
	if (s.dh & DH_LBA) {
		s.sn = (uint8_t)lba;
		s.cl = (uint8_t)(lba >> 8);
		s.ch = (uint8_t)(lba >> 16);
		s.dh = (uint8_t)((s.dh & 0xf0) | ((lba >> 24) & 0x0f));
	} else {
		uint32_t spt = dr->cspt, heads = dr->cheads;
		uint32_t sec = lba % spt, t = lba / spt, head = t % heads, cyl = t / heads;

		s.sn = (uint8_t)(sec + 1);
		s.dh = (uint8_t)((s.dh & 0xf0) | head);
		s.cl = (uint8_t)cyl;
		s.ch = (uint8_t)(cyl >> 8);
	}
}

static uint32_t count_of(void) { return s.count ? s.count : 256; }

/* ------------------------------------------------------------------ PIO read / write */

/* Fetch the next block into the buffer and raise DRQ; on an error the status is set instead. */
static void read_block(drive_t *dr)
{
	uint32_t n = s.rem < s.blk ? s.rem : s.blk, i, lba;
	int e;

	for (i = 0; i < n; i++) {
		e = resolve(dr, &lba);
		if (e) { fail(dr, (uint8_t)e); return; }
		if (hm_read_sectors(dr->d, lba, s.buf + i * SECSZ, 1) != HM_OK) { fail(dr, ER_UNC); return; }
		store_addr(dr, lba + 1);
		s.count--;
	}
	s.blkn = n;
	s.len = n * SECSZ;
	s.pos = 0;
	s.phase = PH_IN;
	dr->error = 0;
	dr->status = ST_DRDY | ST_DSC | ST_DRQ;
	raise_irq();
}

static void start_read(drive_t *dr, int multiple)
{
	if (multiple && dr->mult == 0) { fail(dr, ER_ABRT); return; }
	s.rem = count_of();
	s.blk = multiple ? dr->mult : 1;
	read_block(dr);
}

static void start_write(drive_t *dr, int multiple)
{
	uint32_t lba;
	int e;

	if (multiple && dr->mult == 0) { fail(dr, ER_ABRT); return; }
	if (hm_info(dr->d)->read_only) { fail(dr, ER_ABRT); return; }
	e = resolve(dr, &lba);
	if (e) { fail(dr, (uint8_t)e); return; }
	s.rem = count_of();
	s.blk = multiple ? dr->mult : 1;
	s.blkn = s.rem < s.blk ? s.rem : s.blk;
	s.len = s.blkn * SECSZ;
	s.pos = 0;
	s.phase = PH_OUT;
	dr->error = 0;
	dr->status = ST_DRDY | ST_DSC | ST_DRQ;		/* no interrupt for the first block */
}

static void write_block(drive_t *dr)
{
	uint32_t i, lba;
	int e;

	for (i = 0; i < s.blkn; i++) {
		e = resolve(dr, &lba);
		if (e) { fail(dr, (uint8_t)e); return; }
		if (hm_write_sectors(dr->d, lba, s.buf + i * SECSZ, 1) != HM_OK) { fail(dr, ER_UNC); return; }
		store_addr(dr, lba + 1);
		s.count--;
	}
	s.rem -= s.blkn;
	if (s.rem == 0) { done(dr); return; }
	s.blkn = s.rem < s.blk ? s.rem : s.blk;
	s.len = s.blkn * SECSZ;
	s.pos = 0;
	dr->status = ST_DRDY | ST_DSC | ST_DRQ;
	raise_irq();
}

/* ------------------------------------------------------------------ IDENTIFY */

static void put_str(uint16_t *w, int first, int words, const char *str)
{
	int i, n = (int)strlen(str);

	for (i = 0; i < words; i++) {
		char a = (2 * i < n) ? str[2 * i] : ' ', b = (2 * i + 1 < n) ? str[2 * i + 1] : ' ';
		w[first + i] = (uint16_t)(((uint8_t)a << 8) | (uint8_t)b);
	}
}

static void identify(drive_t *dr)
{
	uint16_t w[256];
	uint32_t cap = dr->ccyls * dr->cheads * dr->cspt;
	char serial[16];
	int i;

	memset(w, 0, sizeof(w));
	w[0] = 0x0040;				/* fixed disk */
	w[1] = (uint16_t)dr->cyls;
	w[3] = (uint16_t)dr->heads;
	w[4] = (uint16_t)(SECSZ * dr->spt);	/* obsolete: bytes per track */
	w[5] = SECSZ;				/* obsolete: bytes per sector */
	w[6] = (uint16_t)dr->spt;
	strcpy(serial, "PC98TAB5-HDD-0");
	serial[13] = (char)('0' + (dr == &s.drv[1]));
	put_str(w, 10, 10, serial);
	put_str(w, 23, 4, "1.00");
	put_str(w, 27, 20, "PC98TAB5 HDD");
	w[47] = 0x8000 | MAXMULT;		/* READ/WRITE MULTIPLE: up to 16 sectors */
	w[49] = 0x0200;				/* LBA supported, no DMA */
	w[51] = 0x0200;				/* PIO mode 2 timing */
	w[53] = 0x0001;				/* words 54-58 valid */
	w[54] = (uint16_t)dr->ccyls;
	w[55] = (uint16_t)dr->cheads;
	w[56] = (uint16_t)dr->cspt;
	w[57] = (uint16_t)cap;
	w[58] = (uint16_t)(cap >> 16);
	w[59] = dr->mult ? (uint16_t)(0x0100 | dr->mult) : 0;
	w[60] = (uint16_t)dr->total;
	w[61] = (uint16_t)(dr->total >> 16);
	w[80] = 0x001e;				/* ATA-1 .. ATA-4 */
	w[83] = 0x4000;
	w[84] = 0x4000;
	for (i = 0; i < 256; i++) {
		s.buf[2 * i] = (uint8_t)w[i];
		s.buf[2 * i + 1] = (uint8_t)(w[i] >> 8);
	}
	s.rem = 1;
	s.blk = 1;
	s.blkn = 1;
	s.len = SECSZ;
	s.pos = 0;
	s.phase = PH_IN;
	dr->error = 0;
	dr->status = ST_DRDY | ST_DSC | ST_DRQ;
	raise_irq();
}

/* ------------------------------------------------------------------ other commands */

static void cmd_verify(drive_t *dr)
{
	uint32_t n = count_of(), lba;
	int e = resolve(dr, &lba);

	if (e) { fail(dr, (uint8_t)e); return; }
	if (n > dr->total - lba) {
		store_addr(dr, dr->total - 1);
		fail(dr, ER_IDNF);
		return;
	}
	store_addr(dr, lba + n - 1);
	s.count = 0;
	done(dr);
}

static void cmd_seek(drive_t *dr)
{
	uint32_t lba;

	if (s.dh & DH_LBA) {
		if (resolve(dr, &lba)) { fail(dr, ER_IDNF); return; }
	} else {
		uint32_t cyl = ((uint32_t)s.ch << 8) | s.cl;

		if (cyl >= dr->ccyls || (uint32_t)(s.dh & 0x0f) >= dr->cheads) { fail(dr, ER_IDNF); return; }
	}
	done(dr);
}

static void cmd_set_features(drive_t *dr)
{
	switch (s.feature) {
	case 0x02: case 0x82:		/* write cache on / off */
	case 0x01: case 0x81:		/* 8-bit transfers on / off */
	case 0x55: case 0xaa:		/* read look-ahead off / on */
	case 0x66: case 0xcc:		/* keep / restore power-on defaults */
	case 0x05: case 0x85:		/* advanced power management */
	case 0x9a: case 0xbb:
		done(dr);
		break;
	case 0x03:			/* set transfer mode: PIO modes only (no DMA) */
		if ((s.count & 0xf8) == 0x00 || (s.count & 0xf8) == 0x08) done(dr);
		else fail(dr, ER_ABRT);
		break;
	default:
		fail(dr, ER_ABRT);
		break;
	}
}

static void cmd_init_params(drive_t *dr)
{
	uint32_t heads = (uint32_t)(s.dh & 0x0f) + 1, spt = s.count;

	if (spt == 0) { fail(dr, ER_ABRT); return; }
	dr->cheads = heads;
	dr->cspt = spt;
	dr->ccyls = dr->total / (heads * spt);
	if (dr->ccyls > 65535) dr->ccyls = 65535;
	done(dr);
}

static void cmd_native_max(drive_t *dr)
{
	uint32_t m = dr->total - 1;

	s.sn = (uint8_t)m;
	s.cl = (uint8_t)(m >> 8);
	s.ch = (uint8_t)(m >> 16);
	s.dh = (uint8_t)((s.dh & 0xf0) | ((m >> 24) & 0x0f));
	done(dr);
}

static void execute(uint8_t c)
{
	drive_t *dr = cur();

	if (!present(dr)) return;
	IDE_LOG("IDE cmd %02x cnt %02x sn %02x cyl %02x%02x dh %02x feat %02x\n", c, s.count, s.sn, s.ch, s.cl, s.dh, s.feature);
	s.phase = PH_NONE;
	s.intrq = 0;
	s.cmd = c;
	dr->error = 0;
	if ((c & 0xf0) == 0x10) {		/* RECALIBRATE */
		s.cl = s.ch = 0;
		done(dr);
		return;
	}
	if ((c & 0xf0) == 0x70) { cmd_seek(dr); return; }
	switch (c) {
	case 0x20: case 0x21: start_read(dr, 0); break;
	case 0x30: case 0x31: start_write(dr, 0); break;
	case 0xc4: start_read(dr, 1); break;
	case 0xc5: start_write(dr, 1); break;
	case 0xc6:				/* SET MULTIPLE MODE */
		if (s.count == 0 || s.count > MAXMULT || (s.count & (s.count - 1))) { fail(dr, ER_ABRT); break; }
		dr->mult = s.count;
		done(dr);
		break;
	case 0x40: case 0x41: cmd_verify(dr); break;
	case 0x90:				/* EXECUTE DEVICE DIAGNOSTIC */
		set_signature();
		s.dh &= 0xf0;
		dr->error = 1;
		dr->status = ST_DRDY | ST_DSC;
		raise_irq();
		break;
	case 0x91: cmd_init_params(dr); break;
	case 0xec: identify(dr); break;
	case 0xef: cmd_set_features(dr); break;
	case 0xf8: cmd_native_max(dr); break;
	case 0xe0: case 0xe1: case 0xe2: case 0xe3:	/* standby / idle */
	case 0x94: case 0x95: case 0x96: case 0x97:
	case 0xe6:				/* sleep */
	case 0xe7: case 0xea:			/* flush cache */
		done(dr);
		break;
	case 0xe5:				/* CHECK POWER MODE: active */
		s.count = 0xff;
		done(dr);
		break;
	default:				/* NOP, DMA, ATAPI, 48-bit, ... */
		fail(dr, ER_ABRT);
		break;
	}
}

/* ------------------------------------------------------------------ data port */

static uint8_t data_in(void)
{
	drive_t *dr = cur();
	uint8_t v;

	if (s.phase != PH_IN || !present(dr)) return 0;
	v = s.buf[s.pos++];
	if (s.pos >= s.len) {
		s.rem -= s.blkn;
		if (s.rem == 0) {
			s.phase = PH_NONE;
			dr->status = ST_DRDY | ST_DSC;
		} else {
			read_block(dr);
		}
	}
	return v;
}

static void data_out(uint8_t v)
{
	drive_t *dr = cur();

	if (s.phase != PH_OUT || !present(dr)) return;
	s.buf[s.pos++] = v;
	if (s.pos >= s.len) {
		dr->status = ST_DRDY | ST_DSC;
		write_block(dr);
	}
}

/* ------------------------------------------------------------------ ports */

static void soft_reset_end(void)
{
	int i;

	s.phase = PH_NONE;
	s.intrq = 0;
	s.dh = 0;
	s.feature = 0;
	set_signature();
	for (i = 0; i < IDE_DRIVES; i++) drive_reset(&s.drv[i]);
}

static uint8_t read_status(int clear_irq)
{
	drive_t *dr = cur();

	if (!present(dr)) return 0x00;
	if (clear_irq) s.intrq = 0;
	if (s.devctl & DC_SRST) return ST_BSY;
	return dr->status;
}

void ide_out(uint32_t port, uint8_t v)
{
	if (port != 0x640 && port != 0x641 && port != 0x64e) IDE_LOG("IDE out %04x <- %02x\n", (unsigned)port, v);
	switch (port) {
	case 0x430:
	case 0x432:
		if (!(v & 0x80)) s.bank[(port >> 1) & 1] = v & 0x71;
		return;
	case 0x433:
	case 0x435:
	case 0x74e:
		return;
	}
	if (!channel_on()) return;
	switch (port) {
	case 0x640: case 0x641: data_out(v); break;
	case 0x642: s.feature = v; break;
	case 0x644: s.count = v; break;
	case 0x646: s.sn = v; break;
	case 0x648: s.cl = v; break;
	case 0x64a: s.ch = v; break;
	case 0x64c: s.dh = v; break;
	case 0x64e:
		if (!(s.devctl & DC_SRST)) execute(v);
		break;
	case 0x74c: {
		uint8_t old = s.devctl;

		s.devctl = v;
		if ((v & DC_SRST) && !(old & DC_SRST)) {
			s.phase = PH_NONE;
			s.intrq = 0;
		} else if (!(v & DC_SRST) && (old & DC_SRST)) {
			soft_reset_end();
		}
		break;
	}
	}
}

uint8_t ide_in(uint32_t port)
{
	drive_t *dr = cur();

	switch (port) {
	case 0x430:
		/* bit 0: controller present; bit 6: a slave drive is connected */
		return (uint8_t)(1 | (s.drv[1].d ? 0x40 : 0));
	case 0x432:
		return s.bank[1] & 0x7f;
	case 0x433:
		return (uint8_t)((s.bank[0] & 1) ? 0x02 : 0x00);
	case 0x435:
		return 0x00;
	}
	if (!channel_on()) return 0xff;
	switch (port) {
	case 0x640: case 0x641: return data_in();
	case 0x64e: return read_status(1);
	case 0x74c: return read_status(0);
	case 0x74e:
		if (!present(dr)) return 0xff;
		return (uint8_t)(0xc0 | ((~s.dh & 0x0f) << 2) | ((s.dh & DH_DRV) ? 0x01 : 0x02));
	}
	if (!present(dr)) return 0x00;
	switch (port) {
	case 0x642: return dr->error;
	case 0x644: return s.count;
	case 0x646: return s.sn;
	case 0x648: return s.cl;
	case 0x64a: return s.ch;
	case 0x64c: return s.dh;
	}
	return 0xff;
}
