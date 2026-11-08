/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * drives.h - the floppy drives: which image is in which drive, as the controller (dev/fdc765.c),
 * the disk menu and the boot code see it.  Images are opened with the media model (fdmedia.h). */
#ifndef PC98CORE_DRIVES_H
#define PC98CORE_DRIVES_H

#include "fdmedia.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRV_FDD_MAX	4

/* Open `path` (D88 / D98 / 88D / 98D, FDI, or a raw image: HDM, XDF, DUP, 2HD, TFD, DDB ...) into drive
 * `drv` at once.  0 = mounted, -1 = not (the drive is then empty). */
int drv_fd_mount(int drv, const char *path);
/* Take the disk out now and put `path` in about 0.4 s later, as a hand swapping disks does: DOS only
 * reads a drive again after it has seen it go not-ready.  path NULL / "" = just eject. */
void drv_fd_swap(int drv, const char *path);
void drv_fd_set_external(int drv, fm_disk_t *d, void (*on_eject)(int drv));
void drv_fd_door(int drv, int open);
void drv_fd_eject(int drv);
/* Once per emulated frame: finishes a pending swap. */
void drv_fd_tick(void);


int drv_fd_ready(int drv);
int drv_fd_protect(int drv);

/* The media-model disk of drive drv for the controller; NULL when the drive is empty. */
fm_disk_t *fdd_media_disk(int drv);
/* Write dirty track data of every mounted drive back to its file (before the SD card is handed over). */
void fdd_media_flush_all(void);

#ifdef __cplusplus
}
#endif

#endif
