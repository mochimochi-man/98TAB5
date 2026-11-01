/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * hddmedia.h - hard disk image model for the SASI controller (stage 4-4).
 *
 * An image is a file with an optional header followed by the data of C * H * S sectors of
 * `sector_size` bytes.  The SASI interface of the PC-9801 moves 256-byte blocks, so the model
 * offers the data area as a flat run of 256-byte blocks (block n = bytes n*256 .. n*256+255 of
 * the data area), whatever the sector size of the image is.  For a 256-byte-sector image that is
 * the natural sector number; for a 512-byte-sector image block 2k and 2k+1 form sector k.
 *
 * Formats (the header layouts are the ones the image tools of the PC-98 community document):
 *   NHD  "T98HDDIMAGE.R0" header: header size, cylinders, heads, sectors, sector size (little endian)
 *   HDI  32-byte header: header size, data size, sector size, sectors, heads, cylinders
 *   THD  2-byte cylinder count in a 256-byte header; 8 heads, 33 sectors, 256 bytes
 *   HDN / HDS  headerless flat 512-byte images (8 heads, 25 / 32 sectors; cylinders from the size)
 * Plain C99, no dependency on ESP-IDF.  File access goes through fm_io_t
 * (media/fdmedia.h).
 */
#ifndef PC98CORE_HDDMEDIA_H
#define PC98CORE_HDDMEDIA_H

#include <stdint.h>
#include "fdmedia.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HM_BLOCK	256

enum { HM_OK = 0, HM_E_IO = -1, HM_E_PROTECT = -2, HM_E_RANGE = -3 };

typedef struct {
	const char	*format;	/* "NHD", "HDI", "THD", "RAW" */
	uint32_t	header_size;	/* bytes before the first sector */
	uint32_t	cylinders;
	uint32_t	heads;
	uint32_t	sectors;	/* per track */
	uint32_t	sector_size;	/* bytes */
	uint32_t	blocks;		/* number of 256-byte blocks in the data area */
	int		read_only;
} hm_info_t;

typedef struct hm_disk hm_disk_t;

/* Open by file name extension (nhd, hdi, thd, hdn, hds; case-insensitive).  NULL when the file
 * cannot be opened or the header is not valid.  ro != 0 opens read-only. */
hm_disk_t	*hm_open(const fm_io_t *io, const char *path, int ro);
/* Open a headerless image with the given geometry (cylinders from the file size when 0). */
hm_disk_t	*hm_open_raw(const fm_io_t *io, const char *path, int ro, uint32_t heads, uint32_t sectors,
			     uint32_t sector_size, uint32_t cylinders);
void		hm_close(hm_disk_t *d);
const hm_info_t	*hm_info(const hm_disk_t *d);

/* n blocks starting at block `lba`.  HM_E_RANGE when any block is outside the data area (nothing is
 * transferred then); HM_E_IO on a short read/write; HM_E_PROTECT for a write to a read-only image. */
int		hm_read(hm_disk_t *d, uint32_t lba, void *buf, uint32_t n);
int		hm_write(hm_disk_t *d, uint32_t lba, const void *buf, uint32_t n);

/* SASI media switch value (0..6) when the geometry is exactly one of the standard PC-98 SASI
 * drives (33 sectors of 256 bytes; 153x4, 310x4, 310x6, 310x8, 615x4, 615x6, 615x8 =
 * 5/10/15/20/20/30/40 MB), else -1. */
int		hm_sasi_type(const hm_disk_t *d);
/* Non-zero when the image can be presented as an ATA (IDE) drive: 512-byte sectors, 1..16 heads, 1..255 sectors
 * per track, 1..65535 cylinders (stage 4-5). */
int		hm_ata_ok(const hm_disk_t *d);
/* Number of native sectors (sector_size bytes each) in the data area: cylinders * heads * sectors. */
uint32_t	hm_sectors(const hm_disk_t *d);
/* Native-sector access for 512-byte-sector images (n sectors from sector `sec`); same return codes as hm_read. */
int		hm_read_sectors(hm_disk_t *d, uint32_t sec, void *buf, uint32_t n);
int		hm_write_sectors(hm_disk_t *d, uint32_t sec, const void *buf, uint32_t n);
/* Heads / cylinders of a SASI media switch value 0..6 (0 for other values). */
int		hm_sasi_type_heads(int type);
int		hm_sasi_type_cylinders(int type);

#ifdef __cplusplus
}
#endif
#endif
