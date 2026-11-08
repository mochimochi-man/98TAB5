/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * cdimg.h - CD-ROM images: a plain .ISO (one data track of 2048-byte sectors) or a .CUE sheet with its
 * BIN files (MODE1/2048, MODE1/2352, MODE2/2352 and AUDIO tracks).  Addresses are logical block
 * addresses counted from the start of the first track (LBA 0 = MSF 00:02:00).
 */
#ifndef PC98CORE_CDIMG_H
#define PC98CORE_CDIMG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CD_MAX_TRACKS	99

typedef struct cd_image cd_image_t;

typedef struct {
	uint8_t		number;		/* 1..99 */
	uint8_t		audio;		/* 1 = audio track */
	uint32_t	start;		/* LBA of INDEX 01 */
	uint32_t	length;		/* sectors */
} cd_track_t;

cd_image_t	*cd_open(const char *path);	/* NULL: cannot open / not understood */
void		cd_close(cd_image_t *c);
int		cd_tracks(const cd_image_t *c);
const cd_track_t *cd_track(const cd_image_t *c, int index);	/* index 0 .. cd_tracks()-1 */
uint32_t	cd_leadout(const cd_image_t *c);	/* LBA of the lead-out (= total sectors) */
/* The 2048 user bytes of a data sector.  0 = done, -1 = out of range / audio / read error. */
int		cd_read_data(cd_image_t *c, uint32_t lba, uint8_t *buf);

#ifdef __cplusplus
}
#endif
#endif
