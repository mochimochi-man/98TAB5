/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * fdm_d88.c - D88 / D98 / 88D / 98D disk images for the floppy media model.
 *
 * Format (public description of the D88 image):
 *   000h  name (17), reserved (9), 1Ah write protect (10h = protected), 1Bh media (00h 2D, 10h 2DD, 20h 2HD;
 *         a non-zero low nibble on 2HD: every sector records whether it was written at 360 or 300 rpm),
 *         1Ch image size (4)
 *   020h  164 track offsets (4 each, 0 = no such track); track n = cylinder * 2 + head
 *   a track: its sectors one after another, each a 16-byte header and its data:
 *         +0 C H R N, +4 sectors in the track (2), +6 density (40h = FM), +7 deleted data, +8 status,
 *         +0Dh 300 rpm, +0Eh data length (2)
 *   status: 00h good, 10h deleted data, A0h ID CRC error, B0h data CRC error, E0h no ID mark, F0h no data mark
 * A 2D image (40 cylinders) sits in an 80-cylinder drive: its cylinder c is under the head at cylinder 2c.
 *
 * The track being used is held in memory; writes change it and it goes back to the file when another track is
 * loaded, on flush and on close.  FORMAT TRACK rebuilds the whole file with the new track record. */
#include "fdmedia.h"

#include <stdlib.h>
#include <string.h>

#define HDR_SIZE	0x2b0
#define NTRACKS		164
#define SEC_HDR		16

typedef struct {
	uint8_t		hdr[HDR_SIZE];
	uint8_t		media;		/* high nibble of 1Bh */
	uint8_t		per_sector_rot;
	uint8_t		*rec;		/* the loaded track's record */
	uint32_t	rec_len, rec_cap, rec_off;
	int		dirty;
} D88;

static uint32_t get16(const uint8_t *p) { return p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static uint32_t track_off(const D88 *x, int n) { return get32(x->hdr + 0x20 + 4 * n); }

/* the image's track number for a drive position, -1 when the image has nothing there */
static int image_track(const D88 *x, int cyl, int head)
{
	if (cyl < 0 || head < 0 || head > 1) return -1;
	if (x->media == 0) {
		if (cyl & 1) return -1;
		cyl >>= 1;
	}
	return (cyl * 2 + head < NTRACKS) ? cyl * 2 + head : -1;
}

/* the length of the record at off: up to the next record or the end of the image */
static uint32_t record_len(const D88 *x, uint32_t off)
{
	uint32_t end = get32(x->hdr + 0x1c);
	int n;

	for (n = 0; n < NTRACKS; n++) {
		const uint32_t o = track_off(x, n);
		if (o > off && o < end) end = o;
	}
	return end > off ? end - off : 0;
}

static int d88_flush(fm_disk_t *d)
{
	D88 *x = (D88 *)d->priv;
	void *f;
	int ok;

	if (!x->dirty) return FM_OK;
	x->dirty = 0;
	if (d->protect) return FM_OK;
	f = d->io.open(d->path, 1);
	if (!f) return FM_E_IO;
	ok = !d->io.seek(f, (long)x->rec_off) && d->io.write(f, x->rec, (long)x->rec_len) == (long)x->rec_len;
	d->io.close(f);
	return ok ? FM_OK : FM_E_IO;
}

static void d88_close(fm_disk_t *d)
{
	D88 *x = (D88 *)d->priv;

	if (x) {
		free(x->rec);
		free(x);
		d->priv = NULL;
	}
}

static int d88_compat(const fm_disk_t *d, const fm_cfg_t *cfg)
{
	const D88 *x = (const D88 *)d->priv;

	if (x->media <= 1) return cfg->media == FM_MEDIA_2DD && cfg->hd_rot == FM_ROT_360;
	if (x->media == 2) return cfg->media == FM_MEDIA_2HD && (x->per_sector_rot || cfg->hd_rot == FM_ROT_360);
	return 0;
}

static int d88_visible(const fm_disk_t *d, const fm_sector_t *s, const fm_cfg_t *cfg)
{
	const D88 *x = (const D88 *)d->priv;

	if (x->media == 2 && x->per_sector_rot) return s->rot == cfg->hd_rot;
	return cfg->hd_rot == FM_ROT_360;
}

static void d88_geometry(const fm_disk_t *d, fm_geom_t *g)
{
	const D88 *x = (const D88 *)d->priv;

	g->media = (uint8_t)(x->media <= 2 ? x->media : FM_MEDIA_2HD);
	g->tracks = NTRACKS;
}

static int d88_track_key(const fm_disk_t *d, int cyl, int head)
{
	return image_track((const D88 *)d->priv, cyl, head);
}

static int d88_load_track(fm_disk_t *d, int cyl, int head, fm_track_t *out)
{
	D88 *x = (D88 *)d->priv;
	const int n = image_track(x, cyl, head);
	uint32_t off, len, pos, count = 0;
	void *f;
	int rot = FM_ROT_360;

	if (n < 0 || !(off = track_off(x, n))) return -1;
	len = record_len(x, off);
	if (len > x->rec_cap) {
		uint8_t *b = (uint8_t *)realloc(x->rec, len);
		if (!b) return -1;
		x->rec = b;
		x->rec_cap = len;
	}
	f = d->io.open(d->path, 0);
	if (!f) return -1;
	if (d->io.seek(f, (long)off) || d->io.read(f, x->rec, (long)len) != (long)len) {
		d->io.close(f);
		return -1;
	}
	d->io.close(f);
	x->rec_off = off;
	x->rec_len = len;
	x->dirty = 0;

	out->nsect = 0;
	for (pos = 0; pos + SEC_HDR <= len && out->nsect < FM_MAX_SECTORS; ) {
		const uint8_t *h = x->rec + pos;
		fm_sector_t *s = &out->sec[out->nsect];
		uint32_t dlen = get16(h + 14);

		if (!out->nsect) count = get16(h + 4);
		memset(s, 0, sizeof(*s));
		s->c = h[0]; s->h = h[1]; s->r = h[2]; s->n = h[3];
		s->fm = (h[6] & 0x40) != 0;
		s->src_stat = h[8];
		s->deleted = h[7] != 0 || h[8] == 0x10;
		s->no_id = h[8] == 0xe0;
		s->id_crc_ok = h[8] != 0xa0 && h[8] != 0xe0;
		s->data_crc_ok = h[8] != 0xb0;
		s->no_data = h[8] == 0xf0;
		s->rot = (x->media == 2 && x->per_sector_rot) ? (h[13] ? FM_ROT_300 : FM_ROT_360) : FM_ROT_ANY;
		if (dlen > len - pos - SEC_HDR) dlen = len - pos - SEC_HDR;
		s->data_len = (uint16_t)dlen;
		s->tag = pos + SEC_HDR;
		out->nsect++;
		pos += SEC_HDR + dlen;
		if (count && (uint32_t)out->nsect >= count) break;
	}
	if (out->nsect && out->sec[0].rot == FM_ROT_300) rot = FM_ROT_300;
	fm_track_layout(out, x->media == 2 ? FM_MEDIA_2HD : FM_MEDIA_2DD, rot);
	out->media = (uint8_t)(x->media <= 2 ? x->media : FM_MEDIA_2HD);
	return 0;
}

static int d88_read_data(fm_disk_t *d, const fm_track_t *t, int idx, uint8_t *dst, size_t cap)
{
	const D88 *x = (const D88 *)d->priv;
	const size_t n = t->sec[idx].data_len < cap ? t->sec[idx].data_len : cap;

	memcpy(dst, x->rec + t->sec[idx].tag, n);
	return (int)n;
}

static int d88_write_data(fm_disk_t *d, const fm_track_t *t, int idx, const uint8_t *src, size_t len)
{
	D88 *x = (D88 *)d->priv;

	if (len > t->sec[idx].data_len) len = t->sec[idx].data_len;
	if (len) {
		memcpy(x->rec + t->sec[idx].tag, src, len);
		x->dirty = 1;
	}
	return FM_OK;
}

static int d88_set_marks(fm_disk_t *d, const fm_track_t *t, int idx, int deleted)
{
	D88 *x = (D88 *)d->priv;
	fm_sector_t *s;
	uint8_t *h;

	if (t != &d->cur || idx < 0 || idx >= d->cur.nsect) return FM_E_NOTRACK;
	s = &d->cur.sec[idx];
	h = x->rec + s->tag - SEC_HDR;
	h[7] = deleted ? 0x10 : 0;
	h[8] = deleted ? 0x10 : 0;
	s->src_stat = h[8];
	s->deleted = (uint8_t)(deleted != 0);
	s->data_crc_ok = 1;
	s->no_data = 0;
	x->dirty = 1;
	return FM_OK;
}

/* FORMAT TRACK: the file is written again, track by track in table order, with the new record in its place */
static int d88_format(fm_disk_t *d, int cyl, int head, const fm_cfg_t *cfg, const fm_format_t *fmt)
{
	D88 *x = (D88 *)d->priv;
	const int n = image_track(x, cyl, head);
	uint8_t *img = NULL, *rec = NULL, *out = NULL;
	uint32_t size, rlen = 0, olen, i;
	void *f;
	int t, ok = 0;

	(void)cfg;
	if (d->protect) return FM_E_PROTECT;
	if (n < 0) return FM_E_NOTRACK;
	for (i = 0; i < (uint32_t)fmt->nsect; i++) rlen += SEC_HDR + fmt->sec[i].data_len;
	rec = (uint8_t *)calloc(1, rlen ? rlen : 1);
	f = d->io.open(d->path, 1);
	if (!rec || !f) goto done;
	size = (uint32_t)d->io.size(f);
	img = (uint8_t *)malloc(size ? size : 1);
	out = (uint8_t *)malloc(size + rlen + HDR_SIZE);
	if (!img || !out || d->io.seek(f, 0) || d->io.read(f, img, (long)size) != (long)size) goto done;

	for (i = 0, rlen = 0; i < (uint32_t)fmt->nsect; i++) {
		const fm_fmt_sector_t *s = &fmt->sec[i];
		uint8_t *h = rec + rlen;
		h[0] = s->c; h[1] = s->h; h[2] = s->r; h[3] = s->n;
		put16(h + 4, (uint32_t)fmt->nsect);
		h[6] = s->fm ? 0x40 : 0;
		h[13] = (x->media == 2 && x->per_sector_rot && s->rot == FM_ROT_300) ? 1 : 0;
		put16(h + 14, s->data_len);
		memset(h + SEC_HDR, s->fill, s->data_len);
		rlen += SEC_HDR + s->data_len;
	}
	memcpy(out, x->hdr, HDR_SIZE);
	olen = HDR_SIZE;
	for (t = 0; t < NTRACKS; t++) {
		const uint32_t o = track_off(x, t);
		if (t == n) {
			put32(out + 0x20 + 4 * t, olen);
			memcpy(out + olen, rec, rlen);
			olen += rlen;
		}
		else if (o && o < size) {
			uint32_t l = record_len(x, o);
			if (l > size - o) l = size - o;
			put32(out + 0x20 + 4 * t, olen);
			memcpy(out + olen, img + o, l);
			olen += l;
		}
		else {
			put32(out + 0x20 + 4 * t, 0);
		}
	}
	put32(out + 0x1c, olen);
	ok = !d->io.seek(f, 0) && d->io.write(f, out, (long)olen) == (long)olen;
	if (ok && olen < size) d->io.write(f, out, 0);
	if (ok) memcpy(x->hdr, out, HDR_SIZE);
done:
	if (f) d->io.close(f);
	free(img);
	free(out);
	free(rec);
	x->dirty = 0;
	return ok ? FM_OK : FM_E_IO;
}

static const fm_backend_t d88_ops = {
	"D88", d88_close, d88_flush, d88_compat, d88_visible,
	d88_load_track, d88_track_key, d88_read_data, d88_write_data, d88_format, d88_geometry,
	d88_set_marks, NULL
};

fm_disk_t *fm_d88_open(const fm_io_t *io, const char *path, int ro)
{
	fm_disk_t *d;
	D88 *x;
	void *f = io->open(path, !ro);
	long got;

	if (!f) return NULL;
	d = fm_disk_alloc(&d88_ops, io, path);
	x = (D88 *)calloc(1, sizeof(*x));
	got = (d && x) ? io->read(f, x->hdr, HDR_SIZE) : 0;
	io->close(f);
	if (got != HDR_SIZE) {
		free(x);
		fm_disk_free(d);
		return NULL;
	}
	d->priv = x;
	d->protect = ro || (x->hdr[0x1a] & 0x10);
	x->media = x->hdr[0x1b] >> 4;
	x->per_sector_rot = (x->hdr[0x1b] & 0x0f) != 0;
	return d;
}
