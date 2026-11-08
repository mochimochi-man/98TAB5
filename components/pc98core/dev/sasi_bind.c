/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * sasi_bind.c - connects the HDD interfaces (dev/sasi.c, dev/ide.c) to the rest of the emulator
 * (stage 4-4 SASI, stage 4-5 IDE).
 *
 * main/ names the images in g_settings.hdd_image[0] / [1]; at every machine reset this file opens them with the
 * HDD media model (media/hddmedia.c), which is what the SASI / IDE device reads and writes.  An image that
 * is already attached under the same name is kept.
 *
 * Which interface a unit gets is decided by the image geometry: 256-byte x 33-sector images with
 * 4/6/8 heads are SASI drives (ports 80h / 82h), 512-byte ATA-style images are IDE drives (ports
 * 640h..74Eh; unit 0 = master, unit 1 = slave).  Units may mix (PC98N's hdd_operate asks the SASI
 * media switch per unit and falls back to ATA for a unit without one).
 *
 * iobus.c calls sasi_reset() / ide_reset() and sasi_bind() / ide_bind() with the other devices (in
 * machine_reset()).  The ports 80h / 82h are only registered when a SASI image is
 * connected, so a machine without a SASI hard disk reads FFh there, as without the board (PC98N then
 * falls back to IDE); the IDE ports are only registered when an IDE image is connected.
 */
#include <string.h>
#include <stdlib.h>
#include <compiler.h>
#include <pc98.h>
#include <hostfile.h>
#include <devices.h>
#include <io.h>

#include "sasi.h"
#include "ide.h"
#include "atapi.h"

#include "esp_attr.h"
extern int ets_printf(const char *fmt, ...);

/* ---- file access through the SD layer (dosio) ---- */

static void *io_open(const char *path, int writable)
{
	HFILE h = writable ? hf_open_rw((const char *)path) : hf_open((const char *)path);

	return (h == NULL) ? NULL : (void *)h;
}
static void io_close(void *h) { hf_close((HFILE)h); }
static long io_size(void *h) { return (long)hf_size((HFILE)h); }
static int io_seek(void *h, long pos)
{
	return (hf_seek((HFILE)h, (long)pos, HF_SEEK_SET) == (long)pos) ? 0 : -1;
}
static long io_read(void *h, void *buf, long len) { return (long)hf_read((HFILE)h, buf, (UINT)len); }
static long io_write(void *h, const void *buf, long len) { return (long)hf_write((HFILE)h, buf, (UINT)len); }

static const fm_io_t s_io = { io_open, io_close, io_size, io_seek, io_read, io_write };

EXT_RAM_BSS_ATTR static char s_path[SASI_UNITS][MAX_PATH];

static void irq9(void)
{
	pic_raise(9);
}

static void mount_unit(int unit)
{
	const char *name = (const char *)g_settings.hdd_image[unit];
	hm_disk_t *d;
	const hm_info_t *i;

	if (!name[0]) {
		sasi_attach(unit, NULL);
		ide_attach(unit, NULL);
		s_path[unit][0] = '\0';
		return;
	}
	if ((sasi_disk(unit) || ide_disk(unit)) && strcmp(s_path[unit], name) == 0) return;	/* same image: keep it */

	d = hm_open(&s_io, name, 0);
	if (!d) {
		ets_printf("HDD: unit %d cannot open %s\n", unit, name);
		sasi_attach(unit, NULL);
		ide_attach(unit, NULL);
		s_path[unit][0] = '\0';
		return;
	}
	strncpy(s_path[unit], name, MAX_PATH - 1);
	s_path[unit][MAX_PATH - 1] = '\0';
	i = hm_info(d);
	ets_printf("HDD: unit %d %s %s C/H/S=%u/%u/%u secsize=%u hdr=%u blocks256=%u ro=%d\n", unit,
		   s_path[unit], i->format, (unsigned)i->cylinders, (unsigned)i->heads, (unsigned)i->sectors,
		   (unsigned)i->sector_size, (unsigned)i->header_size, (unsigned)i->blocks, i->read_only);
	sasi_attach(unit, NULL);
	ide_attach(unit, NULL);
	if (sasi_accepts(d)) {
		sasi_attach(unit, d);
		ets_printf("SASI: unit %d media switch value %d\n", unit, sasi_media_type(unit));
	} else if (ide_accepts(d)) {
		ide_attach(unit, d);
		ets_printf("IDE: unit %d attached as %s\n", unit, unit ? "slave" : "master");
	} else {
		hm_close(d);
		ets_printf("HDD: unit %d not attached: neither a SASI (256B x 33S, 4/6/8 heads) nor an IDE (512B, <=16 heads, <=255 S) geometry\n", unit);
		s_path[unit][0] = '\0';
	}
}

void sasi_reset(const Pc98Settings *pConfig)
{
	int u;

	(void)pConfig;
	sasi_machine_reset();
	sasi_irq_hook = irq9;
	for (u = 0; u < SASI_UNITS; u++) mount_unit(u);
}

static void o80(UINT port, REG8 dat) { sasi_out80(port, dat); }
static void o82(UINT port, REG8 dat) { sasi_out82(port, dat); }
static REG8 i80(UINT port) { return sasi_in80(port); }
static REG8 i82(UINT port) { return sasi_in82(port); }

void sasi_bind(void)
{
	if (!sasi_disk(0) && !sasi_disk(1)) return;
	io_port(0x0080, o80, NULL);
	io_port(0x0082, o82, NULL);
	io_port(0x0080, NULL, i80);
	io_port(0x0082, NULL, i82);
}

/* ---- IDE (dev/ide.c).  The drives were mounted by sasi_reset() above (it must run first); this only
 *      resets the controller and registers the ports when a drive is connected. ---- */

void ide_reset(const Pc98Settings *pConfig)
{
	(void)pConfig;
	ide_machine_reset();
	ide_irq_hook = irq9;
	{	/* the drive is fitted while a disc image is chosen; a new one goes in without a reset */
		const char *cd = (const char *)g_settings.cd_image;

		atapi_fit(cd[0] != 0);
		if (cd[0] && strcmp(atapi_disc(), cd) != 0) atapi_insert(cd);
	}
	atapi_reset();
	atapi_irq_hook = irq9;
}

static void oide(UINT port, REG8 dat) { ide_out(port, dat); }
static REG8 iide(UINT port) { return ide_in(port); }

void ide_bind(void)
{
	static const UINT ports[] = { 0x430, 0x432, 0x433, 0x435, 0x640, 0x641, 0x642, 0x644, 0x646, 0x648,
				      0x64a, 0x64c, 0x64e, 0x74c, 0x74e };
	unsigned k;

	if (!ide_disk(0) && !ide_disk(1) && !atapi_fitted()) return;
	for (k = 0; k < sizeof(ports) / sizeof(ports[0]); k++) {
		io_port(ports[k], oide, NULL);
		io_port(ports[k], NULL, iide);
	}
}
