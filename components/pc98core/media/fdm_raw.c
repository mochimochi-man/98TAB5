/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * fdm_raw.c - plain sector dumps for the floppy media model: HDM / XDF / DUP / 2HD / TFD ... (no header), DIP and FIM
 * (a 256-byte header), FDI (a header that gives the geometry).
 *
 * A plain dump has no track structure: cylinder C, head H holds sectors R = 1..S of size code N, all MFM with good
 * CRCs, stored in the order C0 H0, C0 H1, C1 H0 ...  The geometry of a headerless dump is the one of the usual PC-98 /
 * PC formats whose size equals the file's.  The media class and the rotation follow from the bytes per track:
 * more than 6 KB is a high-density disk (2HD), and a high-density track of 18 or more 512-byte sectors is recorded at
 * 300 rpm (the 1.44 MB format), the others at 360 rpm.  FDI header (little endian): +4 type, +8 header size,
 * +0Ch data size, +10h sector size, +14h sectors, +18h heads (2), +1Ch cylinders.
 * Formatting rewrites a track with the fill byte when the requested layout is the image's own; a dump cannot take
 * any other layout, so other requests fail. */
#include "fdmedia.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
	uint32_t	hdr;
	uint8_t		cyls, secs, n, media, rot;
} Geo;

/* The open file is kept: opening it costs the card about 11 ms, and done for every sector it made a load several times
 * slower than the drive (the music of a game playing while it loads broke up). Writes still open, write and close, as
 * before, so that each one is on the card when it returns; the handle for reading is dropped first. */
typedef struct {
	Geo	g;			/* first: the code takes the private data as a Geo */
	void	*fh;			/* the file, open for reading; NULL until a read needs it */
} Raw;

static void drop_handle(fm_disk_t *d)
{
	Raw *x = (Raw *)d->priv;

	if (x && x->fh) {
		d->io.close(x->fh);
		x->fh = NULL;
	}
}

static uint32_t get32(const uint8_t *p)
{
	return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void classify(Geo *g)
{
	const uint32_t track = (uint32_t)g->secs << (7 + g->n);

	g->media = track > 6144 ? FM_MEDIA_2HD : FM_MEDIA_2DD;
	g->rot = (g->media == FM_MEDIA_2HD && g->n == 2 && g->secs >= 18) ? FM_ROT_300 : FM_ROT_360;
}

/* the usual layouts: header, cylinders, sectors, size code */
static const Geo k[] = {
	{ 0, 77, 8, 3 },	/* 1.25 MB (PC-98 2HD) */
	{ 0, 80, 15, 2 },	/* 1.2 MB */
	{ 0, 80, 18, 2 },	/* 1.44 MB */
	{ 0, 80, 21, 2 },	/* 1.68 MB */
	{ 0, 77, 26, 1 },	/* 2HD, 26 x 256 */
	{ 0, 77, 9, 3 },
	{ 0, 80, 8, 3 },	/* 2HD 1024 x 8 using all 80 cylinders */
	{ 0, 80, 8, 2 },	/* 640 KB */
	{ 0, 80, 9, 2 },	/* 720 KB */
	{ 0, 80, 16, 1 },	/* 2DD 16 x 256 (the ".ddb" layout of the same size as 640 KB) */
	{ 256, 77, 8, 3 },	/* DIP / FIM */
	{ 256, 77, 26, 1 },	/* FIM */
};

static void raw_close(fm_disk_t *d)
{
	drop_handle(d);
	free(d->priv);
	d->priv = NULL;
}

static int raw_flush(fm_disk_t *d)
{
	(void)d;		/* writes go to the file at once */
	return FM_OK;
}

static int raw_compat(const fm_disk_t *d, const fm_cfg_t *cfg)
{
	const Geo *g = (const Geo *)d->priv;

	return cfg->media == g->media && cfg->hd_rot == g->rot;
}

static void raw_geometry(const fm_disk_t *d, fm_geom_t *out)
{
	const Geo *g = (const Geo *)d->priv;

	out->media = g->media;
	out->hd_rot = g->rot;
	out->uniform = 1;
	out->tracks = g->cyls * 2;
	out->sectors = g->secs;
	out->n = g->n;
	out->header_size = g->hdr;
}

static uint32_t sector_pos(const Geo *g, int cyl, int head, int r)
{
	return g->hdr + ((((uint32_t)cyl * 2 + (uint32_t)head) * g->secs + (uint32_t)r) << (7 + g->n));
}

static int raw_load_track(fm_disk_t *d, int cyl, int head, fm_track_t *out)
{
	const Geo *g = (const Geo *)d->priv;
	int i;

	if (cyl < 0 || cyl >= g->cyls || head < 0 || head > 1) return -1;
	out->nsect = g->secs;
	for (i = 0; i < g->secs; i++) {
		fm_sector_t *s = &out->sec[i];
		memset(s, 0, sizeof(*s));
		s->c = (uint8_t)cyl;
		s->h = (uint8_t)head;
		s->r = (uint8_t)(i + 1);
		s->n = g->n;
		s->id_crc_ok = s->data_crc_ok = 1;
		s->rot = FM_ROT_ANY;
		s->data_len = (uint16_t)(128u << g->n);
		s->tag = sector_pos(g, cyl, head, i);
	}
	fm_track_layout(out, g->media, g->rot);
	return 0;
}

static int file_io(fm_disk_t *d, uint32_t pos, void *buf, size_t len, int write)
{
	Raw *x = (Raw *)d->priv;
	void *f;
	int ok;

	if (!write) {
		if (!x->fh) x->fh = d->io.open(d->path, 0);
		if (!x->fh) return 0;
		return !d->io.seek(x->fh, (long)pos) && d->io.read(x->fh, buf, (long)len) == (long)len;
	}
	drop_handle(d);
	f = d->io.open(d->path, write);
	if (!f) return 0;
	ok = !d->io.seek(f, (long)pos) &&
	     (write ? d->io.write(f, buf, (long)len) : d->io.read(f, buf, (long)len)) == (long)len;
	d->io.close(f);
	return ok;
}

static int raw_read_data(fm_disk_t *d, const fm_track_t *t, int idx, uint8_t *dst, size_t cap)
{
	const size_t n = t->sec[idx].data_len < cap ? t->sec[idx].data_len : cap;

	return file_io(d, t->sec[idx].tag, dst, n, 0) ? (int)n : -1;
}

static int raw_write_data(fm_disk_t *d, const fm_track_t *t, int idx, const uint8_t *src, size_t len)
{
	if (len > t->sec[idx].data_len) len = t->sec[idx].data_len;
	return file_io(d, t->sec[idx].tag, (void *)src, len, 1) ? FM_OK : FM_E_IO;
}

/* A format of the first track in another layout re-lays the whole image out, as a real disk takes whatever format is
 * written to it: a blank 1.44 MB image formatted as 1.25 MB becomes a 1.25 MB image.  Only a plain image (no header)
 * and only a layout of the table; the file is resized to it, so that it opens as the same disk again. */
static int relayout(fm_disk_t *d, const fm_cfg_t *cfg, const fm_format_t *f)
{
	Geo *g = (Geo *)d->priv;
	Geo n;
	size_t i;
	int j;
	void *h;
	int ok;
	uint32_t size;

	if (g->hdr) return 0;
	for (j = 1; j < f->nsect; j++) if (f->sec[j].n != f->sec[0].n) return 0;
	for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		if (k[i].hdr || k[i].secs != f->nsect || k[i].n != f->sec[0].n) continue;
		n = k[i];
		classify(&n);
		if (n.media != cfg->media || n.rot != cfg->hd_rot) continue;
		size = ((uint32_t)n.cyls * 2 * n.secs) << (7 + n.n);
		drop_handle(d);
		h = d->io.open(d->path, 1);
		if (!h) return 0;
		if (d->io.size(h) > (long)size) ok = !d->io.seek(h, (long)size) && d->io.write(h, "", 0) == 0;
		else ok = !d->io.seek(h, (long)size - 1) && d->io.write(h, "", 1) == 1;
		d->io.close(h);
		if (!ok) return 0;
		*g = n;
		return 1;
	}
	return 0;
}

static int raw_format(fm_disk_t *d, int cyl, int head, const fm_cfg_t *cfg, const fm_format_t *f)
{
	const Geo *g = (const Geo *)d->priv;
	uint8_t buf[1024];
	int i;

	if (d->protect) return FM_E_PROTECT;
	if (cyl == 0 && head == 0 && f->nsect > 0 && (f->nsect != g->secs || f->sec[0].n != g->n) && !relayout(d, cfg, f)) return FM_E_IO;
	g = (const Geo *)d->priv;
	if (cyl < 0 || cyl >= g->cyls || head < 0 || head > 1 || f->nsect != g->secs) return FM_E_IO;
	for (i = 0; i < f->nsect; i++) {
		const fm_fmt_sector_t *s = &f->sec[i];
		if (s->n != g->n || s->r < 1 || s->r > g->secs || s->fm) return FM_E_IO;
	}
	for (i = 0; i < f->nsect; i++) {
		const fm_fmt_sector_t *s = &f->sec[i];
		memset(buf, s->fill, sizeof(buf));
		if (!file_io(d, sector_pos(g, cyl, head, s->r - 1), buf, 128u << g->n, 1)) return FM_E_IO;
	}
	return FM_OK;
}

static const fm_backend_t raw_ops = {
	"RAW", raw_close, raw_flush, raw_compat, NULL,
	raw_load_track, NULL, raw_read_data, raw_write_data, raw_format, raw_geometry,
	NULL, NULL
};

static fm_disk_t *make(const fm_io_t *io, const char *path, Geo g, int ro)
{
	fm_disk_t *d = fm_disk_alloc(&raw_ops, io, path);
	Raw *x = (Raw *)calloc(1, sizeof(*x));

	if (!d || !x) {
		free(x);
		fm_disk_free(d);
		return NULL;
	}
	classify(&g);
	x->g = g;
	d->priv = x;
	d->protect = ro;
	return d;
}

static long file_size(const fm_io_t *io, const char *path, int ro, uint8_t *head32)
{
	void *f = io->open(path, !ro);
	long size;

	if (!f) return -1;
	size = io->size(f);
	if (head32 && (io->seek(f, 0) || io->read(f, head32, 32) != 32)) size = -1;
	io->close(f);
	return size;
}

fm_disk_t *fm_raw_open_xdf(const fm_io_t *io, const char *path, int ro, int ext_is_ddb)
{
	const long size = file_size(io, path, ro, NULL);
	size_t i;

	if (size < 0) return NULL;
	for (i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		const uint32_t s = k[i].hdr + (((uint32_t)k[i].cyls * 2 * k[i].secs) << (7 + k[i].n));
		if (s != (uint32_t)size) continue;
		if (k[i].secs == 8 && k[i].n == 2 && ext_is_ddb) continue;	/* .ddb: the 16 x 256 layout */
		if (k[i].secs == 16 && !ext_is_ddb) continue;
		return make(io, path, k[i], ro);
	}
	return NULL;
}

fm_disk_t *fm_raw_open_fdi(const fm_io_t *io, const char *path, int ro)
{
	uint8_t h[32];
	const long size = file_size(io, path, ro, h);
	uint32_t ssize, secs, heads, cyls, hdr;
	Geo g;

	if (size < 0) return NULL;
	hdr = get32(h + 8);
	ssize = get32(h + 0x10);
	secs = get32(h + 0x14);
	heads = get32(h + 0x18);
	cyls = get32(h + 0x1c);
	if (ssize < 128 || ssize > 8192 || (ssize & (ssize - 1)) || !secs || secs > 255 || heads != 2 || !cyls || cyls > 127) {
		return NULL;
	}
	if ((uint32_t)size != hdr + ssize * secs * heads * cyls) return NULL;
	memset(&g, 0, sizeof(g));
	g.hdr = hdr;
	g.cyls = (uint8_t)cyls;
	g.secs = (uint8_t)secs;
	while ((128u << g.n) < ssize) g.n++;
	return make(io, path, g, ro);
}
