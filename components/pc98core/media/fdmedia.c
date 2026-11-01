/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * fdmedia.c - generic part of the floppy media model: disk object, track cache, timing,
 * CRC and the decoded-track builder.  Backends are in fdm_d88.c and fdm_raw.c.
 */
#include "fdmedia.h"

#include <stdlib.h>
#include <string.h>

/* Layout of a standard MFM track (bytes, the IBM System 34 format the uPD765 writes): GAP4a 80, SYNC 12,
 * index mark 4, GAP1 50; per sector SYNC 12, ID mark 4, ID 4, CRC 2, GAP2 22, SYNC 12, data mark 4, (data), CRC 2,
 * GAP3. */
#define L_PRIAMBLE	(80 + 12 + 4 + 50)
#define L_SECTOR	(12 + 4 + 4 + 2 + 22 + 12 + 4 + 2)
#define L_GAP3_MAX	255

/* ---------------------------------------------------------------- disk object */

fm_disk_t *fm_disk_alloc(const fm_backend_t *ops, const fm_io_t *io, const char *path)
{
	fm_disk_t *d = (fm_disk_t *)calloc(1, sizeof(*d));
	size_t n;

	if (!d) {
		return NULL;
	}
	d->ops = ops;
	d->io = *io;
	n = strlen(path) + 1;
	d->path = (char *)malloc(n);
	if (!d->path) {
		free(d);
		return NULL;
	}
	memcpy(d->path, path, n);
	return d;
}

void fm_disk_free(fm_disk_t *d)
{
	if (!d) {
		return;
	}
	free(d->path);
	free(d);
}

int fm_disk_flush(fm_disk_t *d)
{
	if (d && d->ops->flush) {
		return d->ops->flush(d);
	}
	return FM_OK;
}

void fm_disk_invalidate(fm_disk_t *d)
{
	if (d) {
		d->cur_valid = 0;
	}
}

void fm_disk_close(fm_disk_t *d)
{
	if (!d) {
		return;
	}
	fm_disk_flush(d);
	if (d->ops->close) {
		d->ops->close(d);
	}
	fm_disk_free(d);
}

int fm_disk_protected(const fm_disk_t *d)
{
	return d && d->protect;
}

void fm_disk_geometry(const fm_disk_t *d, fm_geom_t *g)
{
	memset(g, 0, sizeof(*g));
	if (d && d->ops->geometry) {
		d->ops->geometry(d, g);
	}
}

const char *fm_disk_format_name(const fm_disk_t *d)
{
	return d ? d->ops->name : "";
}

int fm_disk_compatible(const fm_disk_t *d, const fm_cfg_t *cfg)
{
	if (!d->ops->compat) {
		return 1;
	}
	return d->ops->compat(d, cfg);
}

int fm_sector_visible(const fm_disk_t *d, const fm_sector_t *s, const fm_cfg_t *cfg)
{
	if (!d->ops->visible) {
		return 1;
	}
	return d->ops->visible(d, s, cfg);
}

int fm_disk_track_ex(fm_disk_t *d, int cyl, int head, const fm_track_t **out)
{
	int r;

	*out = NULL;
	if (!d) {
		return FM_E_NOTRACK;
	}
	if (d->cur_valid && d->cur.cyl == cyl && d->cur.head == head) {
		*out = &d->cur;
		return FM_OK;
	}
	fm_disk_flush(d);				/* dirty data of the track being replaced */
	d->cur_valid = 0;
	memset(&d->cur, 0, offsetof(fm_track_t, sec));
	r = d->ops->load_track(d, cyl, head, &d->cur);
	if (r != 0) {
		return (r == FM_E_NOTREADY) ? FM_E_NOTREADY : FM_E_NOTRACK;
	}
	d->cur.cyl = cyl;
	d->cur.head = head;
	d->cur_valid = 1;
	*out = &d->cur;
	return FM_OK;
}

const fm_track_t *fm_disk_track(fm_disk_t *d, int cyl, int head)
{
	const fm_track_t *t;

	return (fm_disk_track_ex(d, cyl, head, &t) == FM_OK) ? t : NULL;
}

const fm_track_t *fm_disk_current(fm_disk_t *d)
{
	return (d && d->cur_valid) ? &d->cur : NULL;
}

int fm_disk_track_key(const fm_disk_t *d, int cyl, int head)
{
	if (d && d->ops->track_key) {
		return d->ops->track_key(d, cyl, head);
	}
	return cyl * 2 + head;
}

int fm_sector_read(fm_disk_t *d, const fm_track_t *t, int idx, uint8_t *dst, size_t cap)
{
	if (!d || !t || idx < 0 || idx >= t->nsect) {
		return -1;
	}
	return d->ops->read_data(d, t, idx, dst, cap);
}

int fm_sector_write(fm_disk_t *d, const fm_track_t *t, int idx, const uint8_t *src, size_t len)
{
	if (!d || !t || idx < 0 || idx >= t->nsect) {
		return FM_E_NOTRACK;
	}
	if (d->protect) {
		return FM_E_PROTECT;
	}
	return d->ops->write_data(d, t, idx, src, len);
}

int fm_sector_set_marks(fm_disk_t *d, const fm_track_t *t, int idx, int deleted)
{
	if (!d || !t || idx < 0 || idx >= t->nsect) {
		return FM_E_NOTRACK;
	}
	if (d->protect) {
		return FM_E_PROTECT;
	}
	if (!d->ops->set_marks) {
		return FM_OK;				/* the format cannot hold the marks */
	}
	return d->ops->set_marks(d, t, idx, deleted);
}

int fm_disk_read_diag(fm_disk_t *d, int cyl, int head, int ncode, uint8_t *out, size_t cap, int *crc_bad)
{
	if (!d || !d->ops->read_diag) {
		return -1;
	}
	return d->ops->read_diag(d, cyl, head, ncode, out, cap, crc_bad);
}

int fm_disk_format_track(fm_disk_t *d, int cyl, int head, const fm_cfg_t *cfg, const fm_format_t *f)
{
	int r;

	if (!d) {
		return FM_E_NOTRACK;
	}
	fm_disk_flush(d);
	r = d->ops->format(d, cyl, head, cfg, f);
	if (r != FM_E_PROTECT) {		/* a refused format has changed nothing */
		d->cur_valid = 0;
	}
	return r;
}

/* ---------------------------------------------------------------- timing */

uint32_t fm_media_rev_us(int media, int hd_rot)
{
	if (media == FM_MEDIA_2HD && hd_rot == FM_ROT_360) {
		return 166667u;			/* 360 rpm */
	}
	return 200000u;				/* 300 rpm */
}

uint32_t fm_media_byte_ns(int media, int hd_rot)
{
	(void)hd_rot;				/* the 2HD data rate is 500 kbit/s at either speed */
	return (media == FM_MEDIA_2HD) ? 16000u : 32000u;
}

static uint32_t nominal_size(int n)
{
	return 128u << (n > 7 ? 7 : n);
}

void fm_track_layout(fm_track_t *t, int media, int hd_rot)
{
	uint64_t byte_ns, rev_ns, used, t_ns, gap_bytes;
	uint32_t g3;
	int i;

	t->media = (uint8_t)media;
	t->hd_rot = (uint8_t)hd_rot;
	t->rev_us = fm_media_rev_us(media, hd_rot);
	t->byte_ns = fm_media_byte_ns(media, hd_rot);
	byte_ns = t->byte_ns;
	rev_ns = (uint64_t)t->rev_us * 1000u;

	used = (uint64_t)L_PRIAMBLE * byte_ns;
	for (i = 0; i < t->nsect; i++) {
		used += (uint64_t)(L_SECTOR + nominal_size(t->sec[i].n)) * byte_ns * (t->sec[i].fm ? 2u : 1u);
	}
	g3 = 0;
	if (t->nsect > 0 && rev_ns > used) {
		gap_bytes = (rev_ns - used) / ((uint64_t)t->nsect * byte_ns);
		g3 = (gap_bytes > L_GAP3_MAX) ? L_GAP3_MAX : (uint32_t)gap_bytes;
	}
	t_ns = (uint64_t)L_PRIAMBLE * byte_ns;
	for (i = 0; i < t->nsect; i++) {
		t->sec[i].pos_us = (uint32_t)(t_ns / 1000u);
		t_ns += (uint64_t)(L_SECTOR + nominal_size(t->sec[i].n) + g3) * byte_ns * (t->sec[i].fm ? 2u : 1u);
	}
}

uint32_t fm_sector_wait_us(const fm_track_t *t, uint32_t phase_us, int idx)
{
	uint32_t rev = t->rev_us, pos, ph;

	if (!rev || idx < 0 || idx >= t->nsect) {
		return 0;
	}
	pos = t->sec[idx].pos_us % rev;
	ph = phase_us % rev;
	return (pos + rev - ph) % rev;
}

uint32_t fm_sector_span_us(const fm_track_t *t, int idx)
{
	uint64_t ns;

	if (idx < 0 || idx >= t->nsect) {
		return 0;
	}
	ns = (uint64_t)L_SECTOR * t->byte_ns * (t->sec[idx].fm ? 2u : 1u);
	ns += t->sec[idx].data_ns ? t->sec[idx].data_ns :
		(uint64_t)nominal_size(t->sec[idx].n) * t->byte_ns * (t->sec[idx].fm ? 2u : 1u);
	return (uint32_t)((ns + 999u) / 1000u);
}

int fm_track_next_id(const fm_track_t *t, uint32_t phase_us, int fm, int *idx, uint32_t *wait_us)
{
	int i, best = -1;
	uint32_t w, bw = 0;

	for (i = 0; i < t->nsect; i++) {
		if (!t->sec[i].id_crc_ok || (t->sec[i].fm != 0) != (fm != 0)) {
			continue;
		}
		w = fm_sector_wait_us(t, phase_us, i);
		if (best < 0 || w < bw) {
			best = i;
			bw = w;
		}
	}
	if (best < 0) {
		return 0;
	}
	if (idx) {
		*idx = best;
	}
	if (wait_us) {
		*wait_us = bw;
	}
	return 1;
}

int fm_track_find(const fm_track_t *t, int c, int h, int r, int n, int fm)
{
	int i;

	for (i = 0; i < t->nsect; i++) {
		const fm_sector_t *s = &t->sec[i];

		if (s->c == c && s->h == h && s->r == r && s->n == n &&
		    (s->fm != 0) == (fm != 0) && s->id_crc_ok) {
			return i;
		}
	}
	return -1;
}

/* ---------------------------------------------------------------- CRC and raw track */

uint16_t fm_crc16(const uint8_t *p, size_t n, uint16_t crc)
{
	size_t i;
	int b;

	for (i = 0; i < n; i++) {
		crc ^= (uint16_t)((uint16_t)p[i] << 8);
		for (b = 0; b < 8; b++) {
			crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

typedef struct {
	uint8_t	*p;
	size_t	len, cap;
	int	over;
} wbuf_t;

static void wput(wbuf_t *w, uint8_t b, size_t count)
{
	while (count--) {
		if (w->len < w->cap) {
			w->p[w->len] = b;
		} else {
			w->over = 1;
		}
		w->len++;
	}
}

static void wmem(wbuf_t *w, const uint8_t *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		wput(w, s[i], 1);
	}
}

static void wmark(wbuf_t *w, int fm, uint8_t mark)
{
	if (fm) {
		wput(w, 0x00, 3);
		wput(w, mark, 1);
	} else {
		wput(w, 0xa1, 3);
		wput(w, mark, 1);
	}
}

size_t fm_track_build_raw(fm_disk_t *d, const fm_track_t *t, uint8_t *out, size_t cap)
{
	return fm_track_build_raw2(d, t, out, cap, NULL);
}

size_t fm_track_build_raw2(fm_disk_t *d, const fm_track_t *t, uint8_t *out, size_t cap, size_t *data_off)
{
	wbuf_t w;
	int i;
	uint8_t *data = NULL;

	if (data_off) {
		for (i = 0; i < t->nsect; i++) {
			data_off[i] = (size_t)-1;
		}
	}
	if (t->raw && t->raw_len) {
		if (t->raw_len > cap) {
			return 0;
		}
		memcpy(out, t->raw, t->raw_len);
		return t->raw_len;
	}
	w.p = out;
	w.len = 0;
	w.cap = cap;
	w.over = 0;

	wput(&w, 0x4e, 80);
	wput(&w, 0x00, 12);
	wput(&w, 0xc2, 3);
	wput(&w, 0xfc, 1);
	wput(&w, 0x4e, 50);

	for (i = 0; i < t->nsect; i++) {
		const fm_sector_t *s = &t->sec[i];
		uint8_t id[4], crcb[2];
		uint16_t crc;
		uint32_t size = nominal_size(s->n);
		uint8_t pre[4];
		uint32_t end_us, next_us;
		size_t gap3;
		int j;

		if (s->no_id) {				/* no ID address mark: nothing of this sector can be seen */
			continue;
		}
		wput(&w, 0x00, 12);
		wmark(&w, s->fm, 0xfe);
		id[0] = s->c; id[1] = s->h; id[2] = s->r; id[3] = s->n;
		wmem(&w, id, 4);
		pre[0] = s->fm ? 0xfe : 0xa1; pre[1] = 0xa1; pre[2] = 0xa1; pre[3] = 0xfe;
		crc = s->fm ? fm_crc16(pre + 3, 1, 0xffff)
			    : fm_crc16(pre, 4, 0xffff);
		crc = fm_crc16(id, 4, crc);
		if (!s->id_crc_ok) {
			crc ^= 0x5a5a;
		}
		crcb[0] = (uint8_t)(crc >> 8); crcb[1] = (uint8_t)crc;
		wmem(&w, crcb, 2);
		wput(&w, 0x4e, 22);

		if (!s->no_data) {
			uint8_t dm = s->deleted ? 0xf8 : 0xfb;

			wput(&w, 0x00, 12);
			wmark(&w, s->fm, dm);
			if (data_off) {
				data_off[i] = w.len;
			}
			free(data);
			data = (uint8_t *)calloc(1, size ? size : 1);
			if (!data) {
				return 0;
			}
			if (fm_sector_read(d, t, i, data, size) < 0 && d) {
				/* unreadable data: leave the zero fill */
			}
			wmem(&w, data, size);
			pre[0] = s->fm ? 0xfb : 0xa1;
			crc = s->fm ? fm_crc16(&dm, 1, 0xffff)
				    : fm_crc16(pre, 3, 0xffff);
			if (!s->fm) {
				crc = fm_crc16(&dm, 1, crc);
			}
			crc = fm_crc16(data, size, crc);
			if (!s->data_crc_ok) {
				crc ^= 0xa5a5;
			}
			crcb[0] = (uint8_t)(crc >> 8); crcb[1] = (uint8_t)crc;
			wmem(&w, crcb, 2);
		} else {
			wput(&w, 0x4e, 12 + 4 + size + 2);
		}
		end_us = s->pos_us + fm_sector_span_us(t, i);
		next_us = t->rev_us;
		for (j = i + 1; j < t->nsect; j++) {
			if (!t->sec[j].no_id) {
				next_us = t->sec[j].pos_us;
				break;
			}
		}
		gap3 = 0;
		if (next_us > end_us && t->byte_ns) {
			gap3 = (size_t)(((uint64_t)(next_us - end_us) * 1000u) /
					((uint64_t)t->byte_ns * (s->fm ? 2u : 1u)));
		}
		wput(&w, 0x4e, gap3);
	}
	free(data);
	return w.over ? 0 : w.len;
}
