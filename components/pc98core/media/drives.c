/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * drives.c - see drives.h. */
#include <compiler.h>
#include <hostfile.h>
#include <string.h>
#include "util/pcstr.h"
#include "drives.h"

/* frames between the eject and the insert of drv_fd_swap() (about 0.4 s) */
#define SWAP_FRAMES	24

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

typedef struct {
	fm_disk_t	*own;			/* an image this file opened */
	fm_disk_t	*ext;
	void		(*on_eject)(int drv);
	int		door_open;
	int		swap_frames;		/* > 0: an insert is pending */
	char		swap_path[MAX_PATH];
} Drive;

static Drive s_drv[DRV_FDD_MAX];

static int valid(int drv) { return drv >= 0 && drv < DRV_FDD_MAX; }

static const char *ext_of(const char *path)
{
	const char *dot = strrchr(path, '.');
	const char *sep = strrchr(path, '/');

	return (dot && (!sep || dot > sep)) ? dot + 1 : "";
}

static fm_disk_t *open_image(const char *path, int ro)
{
	const char *e = ext_of(path);

	if (pcs_eqi(e, "d88") || pcs_eqi(e, "88d") || pcs_eqi(e, "d98") || pcs_eqi(e, "98d")) {
		return fm_d88_open(&s_io, path, ro);
	}
	if (pcs_eqi(e, "fdi")) {
		return fm_raw_open_fdi(&s_io, path, ro);
	}
	if (pcs_eqi(e, "hdm") || pcs_eqi(e, "xdf") || pcs_eqi(e, "dup") ||
        pcs_eqi(e, "2hd") || pcs_eqi(e, "tfd") || pcs_eqi(e, "ddb") ||
        pcs_eqi(e, "img") || pcs_eqi(e, "ima") || pcs_eqi(e, "bin") ||
        pcs_eqi(e, "fdd") || pcs_eqi(e, "nfd") ||
        pcs_eqi(e, "dip") || pcs_eqi(e, "fim") || pcs_eqi(e, "dd6")) {
        return fm_raw_open_xdf(&s_io, path, ro, pcs_eqi(e, "ddb"));
    }
    return NULL;
}

void drv_fd_eject(int drv)
{
	Drive *d;

	if (!valid(drv)) return;
	d = &s_drv[drv];
	if (d->own) {
		fm_disk_close(d->own);
		d->own = NULL;
	}
	if (d->ext && d->on_eject) {
		void (*cb)(int) = d->on_eject;

		d->on_eject = NULL;
		cb(drv);
	}
	d->ext = NULL;
	d->door_open = 0;
}

int drv_fd_mount(int drv, const char *path)
{
	fm_disk_t *disk;

	if (!valid(drv)) return -1;
	drv_fd_eject(drv);
	s_drv[drv].swap_frames = 0;
	if (!path || !path[0]) return -1;
	disk = open_image(path, 0);
	if (!disk) disk = open_image(path, 1);
	if (!disk) return -1;
	s_drv[drv].own = disk;
	return 0;
}

void drv_fd_swap(int drv, const char *path)
{
	if (!valid(drv)) return;
	drv_fd_eject(drv);
	s_drv[drv].swap_frames = 0;
	if (path && path[0]) {
		pcs_copy(s_drv[drv].swap_path, path, sizeof(s_drv[drv].swap_path));
		s_drv[drv].swap_frames = SWAP_FRAMES;
	}
}

void drv_fd_tick(void)
{
	int i;

	for (i = 0; i < DRV_FDD_MAX; i++) {
		if (s_drv[i].swap_frames > 0 && --s_drv[i].swap_frames == 0) {
			drv_fd_mount(i, s_drv[i].swap_path);
		}
	}
}

void drv_fd_set_external(int drv, fm_disk_t *d, void (*on_eject)(int drv))
{
	if (!valid(drv)) return;
	if (d && s_drv[drv].own) {
		fm_disk_close(s_drv[drv].own);
		s_drv[drv].own = NULL;
	}
	s_drv[drv].ext = d;
	s_drv[drv].on_eject = d ? on_eject : NULL;
	s_drv[drv].door_open = 0;
}

void drv_fd_door(int drv, int open)
{
	if (valid(drv)) s_drv[drv].door_open = open ? 1 : 0;
}

fm_disk_t *fdd_media_disk(int drv)
{
	if (!valid(drv)) return NULL;
	return s_drv[drv].ext ? s_drv[drv].ext : s_drv[drv].own;
}

int drv_fd_ready(int drv)
{
	return fdd_media_disk(drv) != NULL && !s_drv[drv].door_open;
}

int drv_fd_protect(int drv)
{
	const fm_disk_t *d = fdd_media_disk(drv);

	return d ? fm_disk_protected(d) : 0;
}

void fdd_media_flush_all(void)
{
	int i;

	for (i = 0; i < DRV_FDD_MAX; i++) {
		if (s_drv[i].own) fm_disk_flush(s_drv[i].own);
	}
}
