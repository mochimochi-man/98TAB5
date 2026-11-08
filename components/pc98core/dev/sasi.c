/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * sasi.c - the SASI hard disk interface of the PC-9801 (data port 80h, control / status port 82h),
 *          written for the hard disk image model (media/hddmedia.h).  Stage 4-4.
 *
 * The board is a SASI host adapter with two drives behind one controller (target ID 0, logical
 * units 0 and 1, selected by bit 5 of byte 1 of the command).  The bus is driven by the CPU through
 * two ports:
 *
 *   80h  data register.  Written: the data byte on the bus (target ID during selection, command
 *        bytes, write data).  Read: the byte the target puts on the bus (read data, sense bytes,
 *        status byte, message byte).
 *   82h  write (output control register, OCR):
 *          bit 0 INTE  enable the interrupt (IRQ 9) at the end of a command
 *          bit 1 DMAE  enable DMA requests on channel 0 (stored; this model has PIO only)
 *          bit 3 RST   bus reset while 1
 *          bit 5 SEL   selection while 1
 *          bit 6 NRDSW 0: reading 82h returns the DIP switches (drive types); 1: the bus status
 *          bit 7 CHEN  channel enable (stored)
 *        read with NRDSW = 1 (input status register):
 *          bit 0 INT   interrupt latch (cleared by reading it)
 *          bit 2 I/O   bit 3 C/D   bit 4 MSG   bit 5 BSY   bit 6 ACK (always 0, PIO is instant)   bit 7 REQ
 *        read with NRDSW = 0: bits 5..3 = media switch value of unit 0, bits 2..0 = unit 1
 *          (0..6 = drive class, 7 = no drive).
 *
 * Bus phases (the C/D, I/O, MSG bits with BSY and REQ set)
 *   free      nothing set                      selection: data = 01h (ID 0), SEL 0 -> 1 : BSY
 *   selected  BSY                              SEL 1 -> 0 : command phase
 *   command   BSY REQ C/D            (A8h)     six command bytes written to 80h
 *   data out  BSY REQ                (A0h)     write data / the parameter bytes of command C2h
 *   data in   BSY REQ I/O            (A4h)     read data / the four sense bytes
 *   status    BSY REQ C/D I/O        (ACh)     one status byte (00h good, 02h check condition)
 *   message   BSY REQ MSG C/D I/O    (BCh)     one message byte (00h, command complete) -> free
 * REQ is always up while a byte is expected; the host reads or writes 80h as soon as it has seen it
 * (ACK is not modelled: the transfer is instantaneous).
 *
 * Commands (byte 0 of the command block; bits 7..5 of byte 1 = unit, 4..0 and bytes 2, 3 = block)
 *   00h test unit ready   01h recalibrate   03h request sense (4 bytes)   04h format drive
 *   06h format track (fills the track at the block with E5h)   08h read (6)   0Ah write (6)
 *   0Bh seek   C2h initialize drive characteristics (10 parameter bytes, accepted and ignored)
 *   E0h, E3h, E4h  diagnostics (always good)
 *   anything else: check condition, invalid command.
 * Blocks are 256 bytes, block count in byte 4 (0 is an error).  A command for a unit with no image
 * ends in check condition (error 04h, not ready); a block beyond the end of the image in check
 * condition with error 21h (illegal block address) before any data move.
 * Request sense returns: error code, (unit << 5 | block bits 20..16), block bits 15..8, block bits 7..0.
 * Error codes: 00h none, 04h drive not ready, 03h write fault (write-protected image, file error),
 * 11h uncorrectable data error (read error), 20h invalid command, 21h illegal block address.
 *
 * Not modelled: DMA (PC98N and the NEC BIOS routines it replaces use programmed I/O; the DMAE bit is
 * stored), seek / rotation time, the format of the whole drive (04h returns good without touching the
 * image), the ACK line.
 * The end-of-command interrupt (IRQ 9) is raised when INTE is set, through sasi_irq_hook.
 */
#include <string.h>
#include "sasi.h"
#include "../include/host.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define SASI_BIGMEM	EXT_RAM_BSS_ATTR
extern int ets_printf(const char *fmt, ...);
#define SASI_DIAG(...)	do { if (s_ndiag < 300) { s_ndiag++; ets_printf(__VA_ARGS__); } } while (0)
#else
#define SASI_BIGMEM
#define SASI_DIAG(...)	((void)0)
#endif

/* 82h: write = control register, read = status (NRDSW = 1) or the board's DIP switches (NRDSW = 0: bits 7-6 sector
 * length of unit 0 / 1, 1 = 512 bytes; bits 5-3 / 2-0 capacity of unit 0 / 1, 000 5MB, 001 10MB, 100 20MB, 110 40MB,
 * 111 none).  Undocumented 9801/9821 Vol.2, io_sasi.txt (Kodaka and Kono, 1994-1997). */
enum {
	OCR_INTE = 0x01, OCR_DMAE = 0x02, OCR_RST = 0x08, OCR_SEL = 0x20, OCR_NRDSW = 0x40, OCR_CHEN = 0x80,
	ISR_INT = 0x01, ISR_IO = 0x04, ISR_CD = 0x08, ISR_MSG = 0x10, ISR_BSY = 0x20, ISR_REQ = 0x80
};

enum { PH_FREE = 0, PH_SELECTED, PH_COMMAND, PH_DATAOUT, PH_DATAIN, PH_SENSE, PH_PARAM, PH_STATUS, PH_MESSAGE };

#define CHUNK	8			/* blocks moved to / from the image at a time */

static struct {
	hm_disk_t	*disk[SASI_UNITS];
	uint8_t		type[SASI_UNITS];	/* media switch value (7 = none) */
	uint8_t		ocr;
	uint8_t		datareg;		/* last byte written to 80h */
	uint8_t		intlatch;
	uint8_t		phase;
	uint8_t		cmd[6];
	uint8_t		cmdpos;
	uint8_t		unit;
	uint8_t		write;			/* the data phase writes */
	uint8_t		status;			/* status byte to deliver */
	uint8_t		sense[4];
	uint8_t		sensepos;
	uint8_t		parampos;
	uint32_t	lba;			/* next block to move to / from the image */
	uint32_t	left;			/* blocks still to move (including the chunk in buf) */
	uint32_t	chunk;			/* blocks in buf */
	uint32_t	pos;			/* byte position in buf */
	uint8_t		err;			/* error code of the last failure, for request sense */
	uint32_t	errlba;
	uint8_t		errunit;
} s;

/* The transfer buffer is big enough to matter for the internal RAM of the Tab5: keep it in PSRAM. */
SASI_BIGMEM static uint8_t s_buf[CHUNK * HM_BLOCK];

void (*sasi_irq_hook)(void);

#ifdef ESP_PLATFORM
static int s_ndiag;
#endif

/* ------------------------------------------------------------------ images */

/*
 * Media switch value (0..6) a drive of this geometry reports, or -1 when the board cannot present it.
 * The BIOS turns cylinder / head / sector into a block number with 33 sectors of 256 bytes and the head
 * count of the class, so only images with 256-byte sectors, 33 sectors per track and 4, 6 or 8 heads can be
 * addressed correctly.  A standard drive (153/310/615 cylinders) reports its own class; another cylinder count
 * (a THD, a bigger test image) reports the smallest class with the same heads that holds all the cylinders (the
 * biggest one when it is larger).  Anything else - the 512-byte-sector, 32-sector images made for the ATA
 * interface - is refused: reading it through the wrong geometry would hand the OS the wrong data and a write
 * would corrupt the image.
 */
static int class_of(const hm_disk_t *d)
{
	const hm_info_t *i = hm_info(d);
	int t, last = -1;

	if (i->sector_size != 256 || i->sectors != 33) return -1;
	for (t = 0; t < 7; t++) {
		if ((uint32_t)hm_sasi_type_heads(t) != i->heads) continue;
		last = t;
		if (i->cylinders <= (uint32_t)hm_sasi_type_cylinders(t)) return t;
	}
	return last;
}

int sasi_accepts(const hm_disk_t *d)
{
	return d && class_of(d) >= 0;
}

int sasi_attach(int unit, hm_disk_t *d)
{
	if (unit < 0 || unit >= SASI_UNITS) {
		if (d) hm_close(d);
		return -1;
	}
	if (s.disk[unit] && s.disk[unit] != d) hm_close(s.disk[unit]);
	s.disk[unit] = NULL;
	s.type[unit] = 7;
	if (!d) return 0;
	s.type[unit] = (uint8_t)class_of(d);
	if (s.type[unit] > 6) {
		s.type[unit] = 7;
		hm_close(d);
		return -1;
	}
	s.disk[unit] = d;
	return 0;
}

hm_disk_t *sasi_disk(int unit)
{
	return (unit >= 0 && unit < SASI_UNITS) ? s.disk[unit] : NULL;
}

int sasi_media_type(int unit)
{
	return (unit >= 0 && unit < SASI_UNITS && s.disk[unit]) ? s.type[unit] : 7;
}

/* ------------------------------------------------------------------ bus */

static void raise_int(void)
{
	SASI_DIAG("SASI end: status %02x ocr %02x%s\n", s.status, s.ocr, ((s.ocr & OCR_INTE) && sasi_irq_hook) ? " -> IRQ 9" : "");
	s.intlatch = ISR_INT;
	if ((s.ocr & OCR_INTE) && sasi_irq_hook) sasi_irq_hook();
}

/* End of the command: status phase.  err != 0 is a check condition with that error code. */
static void finish(uint8_t err)
{
	s.status = err ? 0x02 : 0x00;
	if (err) {
		s.err = err;
		s.errunit = s.unit;
		s.errlba = s.lba;
	}
	s.phase = PH_STATUS;
	raise_int();
}

void sasi_bus_reset(void)
{
	s.phase = PH_FREE;
	s.cmdpos = 0;
	s.intlatch = 0;
	s.left = 0;
	s.chunk = 0;
	s.pos = 0;
}

void sasi_machine_reset(void)
{
	sasi_bus_reset();
	s.err = 0;
	s.ocr = 0;
	s.datareg = 0;
}

static uint32_t cmd_lba(void)
{
	return ((uint32_t)(s.cmd[1] & 0x1f) << 16) | ((uint32_t)s.cmd[2] << 8) | s.cmd[3];
}

/* Load the next chunk of a read into buf.  Returns the error code (0 = ok). */
static uint8_t fill_chunk(void)
{
	uint32_t n = s.left < CHUNK ? s.left : CHUNK;

	host_hdd_access(s.unit);
	if (hm_read(s.disk[s.unit], s.lba, s_buf, n) != HM_OK) return 0x11;
	s.chunk = n;
	s.pos = 0;
	return 0;
}

static uint8_t flush_chunk(void)
{
	int r;

	host_hdd_access(s.unit);
	r = hm_write(s.disk[s.unit], s.lba, s_buf, s.chunk);

	return r == HM_OK ? 0 : 0x03;
}

static void start_data(int write)
{
	uint32_t n;

	s.lba = cmd_lba();
	n = s.cmd[4];
	s.write = (uint8_t)write;
	s.left = n;
	s.pos = 0;
	s.chunk = 0;
	if (write) {
		s.chunk = n < CHUNK ? n : CHUNK;
		s.phase = PH_DATAOUT;
	} else {
		uint8_t e = fill_chunk();

		if (e) { finish(e); return; }
		s.phase = PH_DATAIN;
	}
}

static void sense_begin(void)
{
	s.sense[0] = s.err;
	s.sense[1] = (uint8_t)((s.errunit << 5) | ((s.errlba >> 16) & 0x1f));
	s.sense[2] = (uint8_t)(s.errlba >> 8);
	s.sense[3] = (uint8_t)s.errlba;
	s.sensepos = 0;
	s.err = 0;
	s.phase = PH_SENSE;
}

static void run_command(void)
{
	hm_disk_t *d;
	uint32_t lba, n;
	uint8_t op = s.cmd[0];

	s.unit = (uint8_t)((s.cmd[1] >> 5) & 7);
	d = (s.unit < SASI_UNITS) ? s.disk[s.unit] : NULL;
	SASI_DIAG("SASI cmd %02x %02x %02x %02x %02x %02x\n", s.cmd[0], s.cmd[1], s.cmd[2], s.cmd[3], s.cmd[4], s.cmd[5]);

	if (op == 0x03) { sense_begin(); return; }	/* works whatever the unit */
	s.lba = cmd_lba();
	switch (op) {
	case 0x00:
	case 0x01:
	case 0x04:
	case 0xe0:
	case 0xe3:
	case 0xe4:
		finish(d ? 0 : 0x04);
		return;
	case 0xc2:
		if (!d) { finish(0x04); return; }
		s.parampos = 0;
		s.phase = PH_PARAM;
		return;
	case 0x0b:
		if (!d) { finish(0x04); return; }
		finish(s.lba < hm_info(d)->blocks ? 0 : 0x21);
		return;
	case 0x06:
		if (!d) { finish(0x04); return; }
		/* format track: E5h in the sectors of the track that holds the block */
		lba = s.lba - (s.lba % hm_info(d)->sectors);
		n = hm_info(d)->sectors;
		if (lba >= hm_info(d)->blocks || n > hm_info(d)->blocks - lba) { finish(0x21); return; }
		memset(s_buf, 0xe5, HM_BLOCK);
		for (; n; n--, lba++)
			if (hm_write(d, lba, s_buf, 1) != HM_OK) { finish(0x03); return; }
		finish(0);
		return;
	case 0x08:
	case 0x0a:
		if (!d) { finish(0x04); return; }
		n = s.cmd[4];
		if (n == 0 || s.lba >= hm_info(d)->blocks || n > hm_info(d)->blocks - s.lba) { finish(0x21); return; }
		if (op == 0x0a && hm_info(d)->read_only) { finish(0x03); return; }
		start_data(op == 0x0a);
		return;
	default:
		finish(0x20);
		return;
	}
}

/* ------------------------------------------------------------------ ports */

void sasi_out80(uint32_t port, uint8_t data)
{
	(void)port;
	switch (s.phase) {
	case PH_COMMAND:
		s.cmd[s.cmdpos++] = data;
		if (s.cmdpos >= 6) run_command();
		break;
	case PH_PARAM:
		if (++s.parampos >= 10) finish(0);
		break;
	case PH_DATAOUT:
		s_buf[s.pos++] = data;
		if (s.pos >= s.chunk * HM_BLOCK) {
			uint8_t e = flush_chunk();

			if (e) { finish(e); break; }
			s.lba += s.chunk;
			s.left -= s.chunk;
			if (s.left == 0) { finish(0); break; }
			s.chunk = s.left < CHUNK ? s.left : CHUNK;
			s.pos = 0;
		}
		break;
	default:
		break;
	}
	s.datareg = data;
}

uint8_t sasi_in80(uint32_t port)
{
	uint8_t v = 0;

	(void)port;
	switch (s.phase) {
	case PH_DATAIN:
		v = s_buf[s.pos++];
		if (s.pos >= s.chunk * HM_BLOCK) {
			uint8_t e;

			s.lba += s.chunk;
			s.left -= s.chunk;
			if (s.left == 0) { finish(0); break; }
			e = fill_chunk();
			if (e) finish(e);
		}
		break;
	case PH_SENSE:
		v = s.sense[s.sensepos++];
		if (s.sensepos >= 4) finish(0);
		break;
	case PH_STATUS:
		v = s.status;
		s.phase = PH_MESSAGE;
		break;
	case PH_MESSAGE:
		v = 0x00;		/* command complete */
		s.phase = PH_FREE;
		break;
	default:
		break;
	}
	return v;
}

void sasi_out82(uint32_t port, uint8_t data)
{
	uint8_t old = s.ocr;

	(void)port;
	s.ocr = data;
	if (data & OCR_RST) {
		sasi_bus_reset();
		return;
	}
	if (!(old & OCR_SEL) && (data & OCR_SEL)) {
		/* selection: the target answers with BSY when its ID (bit 0) is on the data bus */
		if (s.phase == PH_FREE && (s.datareg & 0x01)) s.phase = PH_SELECTED;
	} else if ((old & OCR_SEL) && !(data & OCR_SEL)) {
		if (s.phase == PH_SELECTED) {
			s.phase = PH_COMMAND;
			s.cmdpos = 0;
		}
	}
}

uint8_t sasi_in82(uint32_t port)
{
	uint8_t v = 0;

	(void)port;
	if (!(s.ocr & OCR_NRDSW))
		return (uint8_t)((sasi_media_type(0) << 3) | sasi_media_type(1));
	v = s.intlatch;
	s.intlatch = 0;
	switch (s.phase) {
	case PH_SELECTED:	v |= ISR_BSY; break;
	case PH_COMMAND:	v |= ISR_BSY | ISR_REQ | ISR_CD; break;
	case PH_DATAOUT:
	case PH_PARAM:		v |= ISR_BSY | ISR_REQ; break;
	case PH_DATAIN:
	case PH_SENSE:		v |= ISR_BSY | ISR_REQ | ISR_IO; break;
	case PH_STATUS:		v |= ISR_BSY | ISR_REQ | ISR_CD | ISR_IO; break;
	case PH_MESSAGE:	v |= ISR_BSY | ISR_REQ | ISR_MSG | ISR_CD | ISR_IO; break;
	default:		break;
	}
	return v;
}

int sasi_phase(void)
{
	return s.phase;
}
