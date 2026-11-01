/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * fdmedia.h - physical floppy media model (stage 4-1).
 *
 * A disk is a set of tracks addressed by (cylinder, head).  A track is a list of sectors in
 * PHYSICAL order (the order in which they pass under the head after the index pulse).  Every
 * sector keeps what an FDC can observe: ID (C,H,R,N), whether the ID field CRC and the data CRC
 * are good, the deleted-data mark, FM/MFM, "no data field", the stored data bytes and the
 * position on the track (time after index).  The model does not know about the uPD765; the
 * controller (or, for now, the legacy fdd_* adapter in fdd_adapter.c) asks questions and gets
 * facts.
 *
 * This header is plain C99 with no dependency on ESP-IDF.
 * File access goes through a small ops table (fm_io_t) so that the model can be tested on the
 * host and so that the Tab5 build can route it through its SD layer.
 *
 * Units: time is in microseconds (byte time in nanoseconds), counted from the index pulse.
 */
#ifndef PC98CORE_FDMEDIA_H
#define PC98CORE_FDMEDIA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FM_MAX_SECTORS	255

/* Media class. */
enum { FM_MEDIA_2D = 0, FM_MEDIA_2DD = 1, FM_MEDIA_2HD = 2 };

/*
 * Rotation class of a 2HD disk (the FDC's 360 / 300 rpm selection, port 4BEh):
 *   FM_ROT_360 (0): 1.25MB/1.21MB PC-98 2HD, 360 rpm
 *   FM_ROT_300 (1): 1.44MB (IBM style) 2HD, 300 rpm
 * 2D/2DD media always turn at 300 rpm and use FM_ROT_360 as the class value.
 * FM_ROT_ANY in a sector means "recorded for any rotation" (every format but D88 type 2HD/minor!=0).
 */
enum { FM_ROT_360 = 0, FM_ROT_300 = 1, FM_ROT_ANY = 0xff };

/* Error codes of the write-type calls. */
enum { FM_OK = 0, FM_E_IO = -1, FM_E_PROTECT = -2, FM_E_NOTRACK = -3,
       FM_E_NOTREADY = -4 };	/* NOTREADY: the track exists but its data has not arrived yet (media backend).  The caller keeps its command waiting and asks again later. */

typedef struct {
	uint8_t		c, h, r, n;		/* ID field */
	uint8_t		fm;			/* 0 = MFM, 1 = FM */
	uint8_t		id_crc_ok;		/* ID field CRC good */
	uint8_t		data_crc_ok;		/* data field CRC good (meaningless when no_data) */
	uint8_t		deleted;		/* data address mark is "deleted data" (F8h) */
	uint8_t		no_data;		/* ID present but no data address mark / data field */
	uint8_t		rot;			/* FM_ROT_360 / FM_ROT_300 / FM_ROT_ANY */
	uint8_t		src_stat;		/* status byte of the image format (D88), opaque to the FDC */
	uint8_t		no_id;			/* no ID address mark at all (D88 status E0): the sector is invisible to the controller */
	uint16_t	data_len;		/* data bytes stored (may differ from 128 << n) */
	uint32_t	data_ns;		/* measured data-field duration, 0 = nominal track rate */
	uint32_t	pos_us;			/* start of the ID field's sync after the index, in us */
	uint32_t	tag;			/* backend private (where the data lives) */
} fm_sector_t;

typedef struct {
	int		cyl, head;
	int		nsect;
	uint8_t		media;			/* FM_MEDIA_* of the disk this track is on */
	uint8_t		hd_rot;			/* FM_ROT_360 / FM_ROT_300 */
	uint32_t	rev_us;			/* one revolution */
	uint32_t	byte_ns;		/* time of one MFM byte; an FM byte takes twice as long */
	const uint8_t	*raw;			/* decoded bytes of the whole track when the backend has them, else NULL */
	uint32_t	raw_len;
	fm_sector_t	sec[FM_MAX_SECTORS];
} fm_track_t;

/* What the drive/controller is set to at the time of an access. */
typedef struct {
	uint8_t		media;			/* FM_MEDIA_2DD or FM_MEDIA_2HD (fdc.ctrlfd) */
	uint8_t		hd_rot;			/* FM_ROT_* (fdc.rpm[us]) */
} fm_cfg_t;

/* Geometry of a disk.  uniform = 1 when every track has the same layout (raw images). */
typedef struct {
	uint8_t		media;
	uint8_t		hd_rot;
	int		uniform;
	int		tracks;			/* uniform: cylinders * 2 */
	int		sectors;		/* uniform: sectors per track */
	int		n;			/* uniform: size code */
	uint32_t	header_size;		/* raw: bytes before the first sector in the file */
} fm_geom_t;

/* File access.  open(path, writable): handle or NULL.  seek returns 0 on success.  */
typedef struct {
	void	*(*open)(const char *path, int writable);
	void	(*close)(void *h);
	long	(*size)(void *h);
	int	(*seek)(void *h, long pos);
	long	(*read)(void *h, void *buf, long len);		/* bytes read */
	long	(*write)(void *h, const void *buf, long len);	/* bytes written; len 0 = cut the file at the current position (returns 0) */
} fm_io_t;

/* One sector as requested by a FORMAT TRACK.  data_len bytes of `fill` are written. */
typedef struct {
	uint8_t		c, h, r, n;
	uint8_t		fm;
	uint8_t		rot;
	uint8_t		fill;
	uint16_t	data_len;
} fm_fmt_sector_t;

typedef struct {
	int		nsect;
	fm_fmt_sector_t	sec[FM_MAX_SECTORS];
} fm_format_t;

typedef struct fm_disk fm_disk_t;

/* ---- opening (NULL on failure; the file must be openable for writing unless ro) ---- */
fm_disk_t	*fm_d88_open(const fm_io_t *io, const char *path, int ro);
/* ext_is_ddb: the file extension is "ddb" (BKDSK DDB and DD6 images have the same size). */
fm_disk_t	*fm_raw_open_xdf(const fm_io_t *io, const char *path, int ro, int ext_is_ddb);
fm_disk_t	*fm_raw_open_fdi(const fm_io_t *io, const char *path, int ro);

void		fm_disk_close(fm_disk_t *d);			/* flushes */
int		fm_disk_flush(fm_disk_t *d);			/* write dirty data back to the file */
void		fm_disk_invalidate(fm_disk_t *d);		/* drop the cached track (after flush) */
int		fm_disk_protected(const fm_disk_t *d);
void		fm_disk_geometry(const fm_disk_t *d, fm_geom_t *g);
const char	*fm_disk_format_name(const fm_disk_t *d);	/* "D88", "RAW" */

/* Can this disk be accessed by a drive in this configuration (media class / rotation)? */
int		fm_disk_compatible(const fm_disk_t *d, const fm_cfg_t *cfg);
/* Does a sector exist for the heads of a drive that spins at cfg->hd_rot? (D88 per-sector rotation flag) */
int		fm_sector_visible(const fm_disk_t *d, const fm_sector_t *s, const fm_cfg_t *cfg);

/* The track at (cyl, head), loaded on demand; NULL if there is no such track.  The pointer is valid
 * until the next call that loads another track of the same disk, or a write-type call. */
const fm_track_t *fm_disk_track(fm_disk_t *d, int cyl, int head);
/* Same, with the reason: FM_OK (*out set), FM_E_NOTRACK (no such track on this disk) or FM_E_NOTREADY
 * (asynchronous backend: the track is being fetched, ask again later; the request is already queued). */
int		fm_disk_track_ex(fm_disk_t *d, int cyl, int head, const fm_track_t **out);
/* The track that is currently cached (the one the last successful fm_disk_track() returned), or NULL. */
const fm_track_t *fm_disk_current(fm_disk_t *d);
/* The backend's own number for the track (D88: index into the track table after the 2D mapping; raw: cyl * 2 + head), -1 if none. */
int		fm_disk_track_key(const fm_disk_t *d, int cyl, int head);

/* Copy up to cap bytes of the stored data of sector idx; returns the number copied, or -1. */
int		fm_sector_read(fm_disk_t *d, const fm_track_t *t, int idx, uint8_t *dst, size_t cap);
/* Overwrite the first len bytes of the data of sector idx.  The CRC/deleted state of the sector is
 * not changed (stage 4-3 decides how a write affects them). */
int		fm_sector_write(fm_disk_t *d, const fm_track_t *t, int idx, const uint8_t *src, size_t len);
/* What a WRITE DATA / WRITE DELETED DATA does to the marks of a sector (stage 4-3, decision 5): the data
 * address mark becomes "deleted" or "normal" and the data CRC becomes good.  Backends that cannot represent
 * the marks (raw images) ignore it.  The sector description of the cached track is updated too. */
int		fm_sector_set_marks(fm_disk_t *d, const fm_track_t *t, int idx, int deleted);
/* READ DIAGNOSTIC data of a track for backends that hold the real recording.  Takes the
 * data fields in physical order, `128 << ncode` bytes from each data address mark, MFM only.  Returns the number
 * of bytes (0 = nothing usable, -1 = the backend has no such function), *crc_bad = fields with a bad CRC. */
int		fm_disk_read_diag(fm_disk_t *d, int cyl, int head, int ncode, uint8_t *out, size_t cap, int *crc_bad);
/* Rewrite a whole track.  Raw images are uniform: formatting cylinder 0 head 0 redefines the
 * geometry of the image, other tracks are overwritten with the fill byte. */
int		fm_disk_format_track(fm_disk_t *d, int cyl, int head, const fm_cfg_t *cfg, const fm_format_t *f);

/* ---- timing helpers (pure functions of a track) ---- */
uint32_t	fm_media_rev_us(int media, int hd_rot);
uint32_t	fm_media_byte_ns(int media, int hd_rot);	/* MFM byte */
/* Fill byte_ns / rev_us and the pos_us of every sector from the sector sizes (gap 3 spreads the sectors evenly). */
void		fm_track_layout(fm_track_t *t, int media, int hd_rot);
/* Time (us) from `phase_us` (time since the index) until the ID field of sector idx starts to pass the head. */
uint32_t	fm_sector_wait_us(const fm_track_t *t, uint32_t phase_us, int idx);
/* From ID start to the end of the data field CRC. */
uint32_t	fm_sector_span_us(const fm_track_t *t, int idx);
/* Which ID of the requested density comes next after phase_us (ignoring bad-ID-CRC sectors)? */
int		fm_track_next_id(const fm_track_t *t, uint32_t phase_us, int fm, int *idx, uint32_t *wait_us);
/* First sector (physical order) with this ID and density whose ID CRC is good; -1 if none. */
int		fm_track_find(const fm_track_t *t, int c, int h, int r, int n, int fm);

/* ---- decoded track image for READ DIAGNOSTIC ---- */
uint16_t	fm_crc16(const uint8_t *p, size_t n, uint16_t crc);	/* CRC-CCITT, start value 0xFFFF */
/* Build gap/sync/mark/ID/CRC/data/CRC bytes of the track (post-index gap included); returns the
 * length (0 on error).  Uses the track's own raw bytes when it has them.  fm_track_build_raw2 also reports
 * (data_off, optional, nsect entries) the offset of the first data byte of each sector in the output,
 * (size_t)-1 when the sector has no data field (or the track came from raw bytes). */
size_t		fm_track_build_raw(fm_disk_t *d, const fm_track_t *t, uint8_t *out, size_t cap);
size_t		fm_track_build_raw2(fm_disk_t *d, const fm_track_t *t, uint8_t *out, size_t cap, size_t *data_off);

/* ---- for backends ---- */
typedef struct {
	const char *name;
	void	(*close)(fm_disk_t *d);
	int	(*flush)(fm_disk_t *d);
	int	(*compat)(const fm_disk_t *d, const fm_cfg_t *cfg);
	int	(*visible)(const fm_disk_t *d, const fm_sector_t *s, const fm_cfg_t *cfg);
	int	(*load_track)(fm_disk_t *d, int cyl, int head, fm_track_t *out);
	int	(*track_key)(const fm_disk_t *d, int cyl, int head);
	int	(*read_data)(fm_disk_t *d, const fm_track_t *t, int idx, uint8_t *dst, size_t cap);
	int	(*write_data)(fm_disk_t *d, const fm_track_t *t, int idx, const uint8_t *src, size_t len);
	int	(*format)(fm_disk_t *d, int cyl, int head, const fm_cfg_t *cfg, const fm_format_t *f);
	void	(*geometry)(const fm_disk_t *d, fm_geom_t *g);
	/* optional (NULL = not supported) */
	int	(*set_marks)(fm_disk_t *d, const fm_track_t *t, int idx, int deleted);
	int	(*read_diag)(fm_disk_t *d, int cyl, int head, int ncode, uint8_t *out, size_t cap, int *crc_bad);
} fm_backend_t;

struct fm_disk {
	const fm_backend_t	*ops;
	fm_io_t			io;
	char			*path;
	int			protect;
	void			*priv;
	int			cur_valid;
	fm_track_t		cur;
};

fm_disk_t	*fm_disk_alloc(const fm_backend_t *ops, const fm_io_t *io, const char *path);
void		fm_disk_free(fm_disk_t *d);

#ifdef __cplusplus
}
#endif

#endif /* PC98CORE_FDMEDIA_H */
