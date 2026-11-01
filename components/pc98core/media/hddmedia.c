/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * hddmedia.c - hard disk image model (see hddmedia.h).
 */
#include <stdlib.h>
#include <string.h>
#include "hddmedia.h"

struct hm_disk {
	fm_io_t		io;
	void		*fh;
	hm_info_t	info;
};

/* The standard SASI drive classes, indexed by the media switch value. */
static const struct { uint8_t heads; uint16_t cyls; } k_sasi[7] = {
	{ 4, 153 }, { 4, 310 }, { 6, 310 }, { 8, 310 }, { 4, 615 }, { 6, 615 }, { 8, 615 }
};

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | (le16(p + 2) << 16); }

static int ext_is(const char *path, const char *ext)
{
	const char *dot = strrchr(path, '.');
	size_t i;

	if (!dot) return 0;
	dot++;
	for (i = 0; ext[i]; i++) {
		char c = dot[i];
		if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
		if (c != ext[i]) return 0;
	}
	return dot[i] == '\0';
}

static int sane(uint32_t cyl, uint32_t heads, uint32_t secs, uint32_t ssize)
{
	if (cyl == 0 || cyl >= 65536 || heads == 0 || heads >= 256 || secs == 0 || secs >= 256) return 0;
	if (ssize < HM_BLOCK || ssize > 4096 || (ssize & (ssize - 1)) != 0) return 0;
	return 1;
}

static hm_disk_t *finish(const fm_io_t *io, void *fh, int ro, const char *fmt, uint32_t hdr,
			 uint32_t cyl, uint32_t heads, uint32_t secs, uint32_t ssize)
{
	hm_disk_t *d;

	if (!sane(cyl, heads, secs, ssize)) { io->close(fh); return NULL; }
	d = (hm_disk_t *)calloc(1, sizeof(*d));
	if (!d) { io->close(fh); return NULL; }
	d->io = *io;
	d->fh = fh;
	d->info.format = fmt;
	d->info.header_size = hdr;
	d->info.cylinders = cyl;
	d->info.heads = heads;
	d->info.sectors = secs;
	d->info.sector_size = ssize;
	d->info.blocks = (uint32_t)(((uint64_t)cyl * heads * secs * ssize) / HM_BLOCK);
	d->info.read_only = ro;
	return d;
}

hm_disk_t *hm_open_raw(const fm_io_t *io, const char *path, int ro, uint32_t heads, uint32_t sectors,
		       uint32_t sector_size, uint32_t cylinders)
{
	void *fh;

	if (!io || !io->open) return NULL;
	fh = io->open(path, !ro);
	if (!fh && !ro) { fh = io->open(path, 0); ro = 1; }
	if (!fh) return NULL;
	if (cylinders == 0 && heads && sectors && sector_size)
		cylinders = (uint32_t)((uint64_t)io->size(fh) / ((uint64_t)heads * sectors * sector_size));
	return finish(io, fh, ro, "RAW", 0, cylinders, heads, sectors, sector_size);
}

hm_disk_t *hm_open(const fm_io_t *io, const char *path, int ro)
{
	uint8_t h[0x120];
	void *fh;
	long n;

	if (!io || !io->open || !path) return NULL;
	if (ext_is(path, "hdn")) return hm_open_raw(io, path, ro, 8, 25, 512, 0);
	if (ext_is(path, "hds")) return hm_open_raw(io, path, ro, 8, 32, 512, 0);
	fh = io->open(path, !ro);
	if (!fh && !ro) { fh = io->open(path, 0); ro = 1; }
	if (!fh) return NULL;
	memset(h, 0, sizeof(h));
	if (io->seek(fh, 0) != 0) { io->close(fh); return NULL; }
	n = io->read(fh, h, sizeof(h));
	if (n < 0) n = 0;

	if (ext_is(path, "nhd")) {
		if (n < 0x11e || memcmp(h, "T98HDDIMAGE.R0", 15) != 0) goto bad;
		return finish(io, fh, ro, "NHD", le32(h + 0x110), le32(h + 0x114), le16(h + 0x118),
			      le16(h + 0x11a), le16(h + 0x11c));
	}
	if (ext_is(path, "hdi")) {
		if (n < 32) goto bad;
		return finish(io, fh, ro, "HDI", le32(h + 8), le32(h + 28), le32(h + 24), le32(h + 20), le32(h + 16));
	}
	if (ext_is(path, "thd")) {
		if (n < 2) goto bad;
		return finish(io, fh, ro, "THD", 256, le16(h), 8, 33, 256);
	}
bad:
	io->close(fh);
	return NULL;
}

void hm_close(hm_disk_t *d)
{
	if (!d) return;
	d->io.close(d->fh);
	free(d);
}

const hm_info_t *hm_info(const hm_disk_t *d)
{
	return &d->info;
}

static int range_ok(const hm_disk_t *d, uint32_t lba, uint32_t n)
{
	return n != 0 && lba < d->info.blocks && n <= d->info.blocks - lba;
}

int hm_read(hm_disk_t *d, uint32_t lba, void *buf, uint32_t n)
{
	long len;

	if (!range_ok(d, lba, n)) return HM_E_RANGE;
	len = (long)(n * HM_BLOCK);
	if (d->io.seek(d->fh, (long)(d->info.header_size + (uint64_t)lba * HM_BLOCK)) != 0) return HM_E_IO;
	return (d->io.read(d->fh, buf, len) == len) ? HM_OK : HM_E_IO;
}

int hm_write(hm_disk_t *d, uint32_t lba, const void *buf, uint32_t n)
{
	long len;

	if (d->info.read_only) return HM_E_PROTECT;
	if (!range_ok(d, lba, n)) return HM_E_RANGE;
	len = (long)(n * HM_BLOCK);
	if (d->io.seek(d->fh, (long)(d->info.header_size + (uint64_t)lba * HM_BLOCK)) != 0) return HM_E_IO;
	return (d->io.write(d->fh, buf, len) == len) ? HM_OK : HM_E_IO;
}

int hm_sasi_type(const hm_disk_t *d)
{
	int t;

	if (d->info.sector_size != 256 || d->info.sectors != 33) return -1;
	for (t = 0; t < 7; t++)
		if (d->info.heads == k_sasi[t].heads && d->info.cylinders == k_sasi[t].cyls) return t;
	return -1;
}

int hm_sasi_type_heads(int type)
{
	return (type >= 0 && type < 7) ? k_sasi[type].heads : 0;
}

int hm_sasi_type_cylinders(int type)
{
	return (type >= 0 && type < 7) ? k_sasi[type].cyls : 0;
}

int hm_ata_ok(const hm_disk_t *d)
{
	const hm_info_t *i = &d->info;

	return i->sector_size == 512 && i->heads >= 1 && i->heads <= 16 && i->sectors >= 1 && i->sectors <= 255 &&
	       i->cylinders >= 1 && i->cylinders <= 65535;
}

uint32_t hm_sectors(const hm_disk_t *d)
{
	return d->info.cylinders * d->info.heads * d->info.sectors;
}

int hm_read_sectors(hm_disk_t *d, uint32_t sec, void *buf, uint32_t n)
{
	uint32_t per = d->info.sector_size / HM_BLOCK;

	if (n == 0 || sec >= hm_sectors(d) || n > hm_sectors(d) - sec) return HM_E_RANGE;
	return hm_read(d, sec * per, buf, n * per);
}

int hm_write_sectors(hm_disk_t *d, uint32_t sec, const void *buf, uint32_t n)
{
	uint32_t per = d->info.sector_size / HM_BLOCK;

	if (n == 0 || sec >= hm_sectors(d) || n > hm_sectors(d) - sec) return HM_E_RANGE;
	return hm_write(d, sec * per, buf, n * per);
}
