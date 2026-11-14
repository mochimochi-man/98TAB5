/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * cdimg.c - CD-ROM images (see cdimg.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "cdimg.h"
#include "../include/hostfile.h"
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

/* One image at a time (there is one drive), kept in PSRAM: the S3 has little internal RAM to spare. */
#define CUE_MAX		(16 * 1024)

typedef struct {
	char		*path;
	uint32_t	base;		/* LBA of the file's first sector */
	uint32_t	sectors;
} cd_file_t;

typedef struct {
	cd_track_t	t;
	int		file;
	uint16_t	secsize;	/* 2048 or 2352 */
	uint16_t	skip;		/* bytes before the user data in a sector: 0, 16 (MODE1) or 24 (MODE2 form 1) */
	uint32_t	file_lba;	/* sector of INDEX 01 within its file */
} trk_t;

struct cd_image {
	int		ntracks, nfiles;
	trk_t		trk[CD_MAX_TRACKS];
	cd_file_t	file[CD_MAX_TRACKS];
	uint32_t	leadout;
	int		open_file;	/* the file behind fh, -1 = none */
	HFILE		fh;
};

static EXT_RAM_BSS_ATTR cd_image_t s_img;
static EXT_RAM_BSS_ATTR char s_cue[CUE_MAX + 1];
static int s_img_used;

static cd_image_t *img_new(void)
{
	if (s_img_used) return NULL;
	memset(&s_img, 0, sizeof(s_img));
	s_img.open_file = -1;
	s_img_used = 1;
	return &s_img;
}

static int ends_with(const char *s, const char *ext)
{
	size_t n = strlen(s), m = strlen(ext), i;

	if (n < m) return 0;
	for (i = 0; i < m; i++)
		if (tolower((unsigned char)s[n - m + i]) != tolower((unsigned char)ext[i])) return 0;
	return 1;
}

static char *dup_str(const char *s)
{
	char *d = (char *)malloc(strlen(s) + 1);

	if (d) strcpy(d, s);
	return d;
}

static long file_size(const char *path)
{
	HFILE f = hf_open(path);
	long n;

	if (!f) return -1;
	n = hf_size(f);
	hf_close(f);
	return n;
}

void cd_close(cd_image_t *c)
{
	int i;

	if (!c) return;
	if (c->fh) hf_close(c->fh);
	for (i = 0; i < c->nfiles; i++) free(c->file[i].path);
	if (c == &s_img) s_img_used = 0;
}

static cd_image_t *open_iso(const char *path)
{
	long size = file_size(path);
	cd_image_t *c;

	if (size < 2048 * 16) return NULL;
	c = img_new();
	if (!c) return NULL;
	c->nfiles = 1;
	c->file[0].path = dup_str(path);
	c->file[0].sectors = (uint32_t)(size / 2048);
	c->ntracks = 1;
	c->trk[0].t.number = 1;
	c->trk[0].t.length = c->file[0].sectors;
	c->trk[0].secsize = 2048;
	c->leadout = c->file[0].sectors;
	if (!c->file[0].path) { cd_close(c); return NULL; }
	return c;
}

/* Next whitespace-separated word of a cue line (a quoted one may hold spaces). */
static const char *word(const char *p, char *out, size_t cap)
{
	size_t n = 0;

	while (*p == ' ' || *p == '\t') p++;
	if (*p == '"') {
		p++;
		while (*p && *p != '"') { if (n + 1 < cap) out[n++] = *p; p++; }
		if (*p == '"') p++;
	} else {
		while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') { if (n + 1 < cap) out[n++] = *p; p++; }
	}
	out[n] = '\0';
	return p;
}

static cd_image_t *open_cue(const char *path)
{
	HFILE f = hf_open(path);
	long size;
	char *text, *line, *next, dir[256], w[256], fpath[512];
	cd_image_t *c;
	int i, cur_file = -1;
	uint32_t base;

	if (!f) return NULL;
	size = hf_size(f);
	if (size <= 0 || size > CUE_MAX) { hf_close(f); return NULL; }
	text = s_cue;
	hf_seek(f, 0, HF_SEEK_SET);
	size = (long)hf_read(f, text, (UINT)size);
	hf_close(f);
	text[size < 0 ? 0 : size] = '\0';

	c = img_new();
	if (!c) return NULL;
	strncpy(dir, path, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	{
		char *slash = strrchr(dir, '/');
		if (slash) slash[1] = '\0';
		else dir[0] = '\0';
	}

	for (line = text; line && *line; line = next) {
		const char *p;

		next = strchr(line, '\n');
		if (next) *next++ = '\0';
		p = word(line, w, sizeof(w));
		for (i = 0; w[i]; i++) w[i] = (char)toupper((unsigned char)w[i]);
		if (!strcmp(w, "FILE")) {
			char name[256], type[32];

			p = word(p, name, sizeof(name));
			word(p, type, sizeof(type));
			if (c->nfiles >= CD_MAX_TRACKS || !name[0]) goto bad;
			if (name[0] == '/') snprintf(fpath, sizeof(fpath), "%s", name);
			else snprintf(fpath, sizeof(fpath), "%s%s", dir, name);
			c->file[c->nfiles].path = dup_str(fpath);
			if (!c->file[c->nfiles].path) goto bad;
			cur_file = c->nfiles++;
		} else if (!strcmp(w, "TRACK")) {
			char num[8], mode[32];
			trk_t *t;

			p = word(p, num, sizeof(num));
			word(p, mode, sizeof(mode));
			for (i = 0; mode[i]; i++) mode[i] = (char)toupper((unsigned char)mode[i]);
			if (cur_file < 0 || c->ntracks >= CD_MAX_TRACKS) goto bad;
			t = &c->trk[c->ntracks++];
			t->t.number = (uint8_t)atoi(num);
			t->file = cur_file;
			t->file_lba = 0xffffffffu;
			if (!strcmp(mode, "AUDIO")) { t->t.audio = 1; t->secsize = 2352; t->skip = 0; }
			else if (!strcmp(mode, "MODE1/2048")) { t->secsize = 2048; t->skip = 0; }
			else if (!strcmp(mode, "MODE1/2352")) { t->secsize = 2352; t->skip = 16; }
			else if (!strcmp(mode, "MODE2/2352")) { t->secsize = 2352; t->skip = 24; }
			else goto bad;
		} else if (!strcmp(w, "INDEX")) {
			char num[8], msf[16];
			unsigned mm = 0, ss = 0, ff = 0;

			p = word(p, num, sizeof(num));
			word(p, msf, sizeof(msf));
			if (c->ntracks == 0) goto bad;
			if (atoi(num) == 1) {
				char *a = msf, *b, *e;

				mm = (unsigned)strtoul(a, &b, 10);
				if (*b == ':') ss = (unsigned)strtoul(b + 1, &e, 10), b = e;
				if (*b == ':') ff = (unsigned)strtoul(b + 1, &e, 10);
				c->trk[c->ntracks - 1].file_lba = (mm * 60u + ss) * 75u + ff;
			}
		}
	}
	if (c->ntracks == 0) goto bad;

	/* Lay the files end to end and give every track its start and length. */
	base = 0;
	for (i = 0; i < c->nfiles; i++) {
		long n = file_size(c->file[i].path);
		int k, first = -1;

		for (k = 0; k < c->ntracks; k++) if (c->trk[k].file == i) { first = k; break; }
		if (n < 0 || first < 0) goto bad;
		c->file[i].base = base;
		c->file[i].sectors = (uint32_t)(n / c->trk[first].secsize);
		base += c->file[i].sectors;
	}
	for (i = 0; i < c->ntracks; i++) {
		trk_t *t = &c->trk[i];
		const cd_file_t *fl = &c->file[t->file];
		uint32_t end;

		if (t->file_lba == 0xffffffffu || t->file_lba >= fl->sectors) goto bad;
		t->t.start = fl->base + t->file_lba;
		end = (i + 1 < c->ntracks && c->trk[i + 1].file == t->file) ? fl->base + c->trk[i + 1].file_lba
									      : fl->base + fl->sectors;
		if (end < t->t.start) goto bad;
		t->t.length = end - t->t.start;
	}
	c->leadout = base;
	return c;
bad:
	cd_close(c);
	return NULL;
}

cd_image_t *cd_open(const char *path)
{
	if (!path || !path[0]) return NULL;
	if (ends_with(path, ".cue")) return open_cue(path);
	return open_iso(path);
}

int cd_tracks(const cd_image_t *c) { return c ? c->ntracks : 0; }

const cd_track_t *cd_track(const cd_image_t *c, int index)
{
	return (c && index >= 0 && index < c->ntracks) ? &c->trk[index].t : NULL;
}

uint32_t cd_leadout(const cd_image_t *c) { return c ? c->leadout : 0; }

int cd_read_data(cd_image_t *c, uint32_t lba, uint8_t *buf)
{
	int i;

	if (!c) return -1;
	for (i = 0; i < c->ntracks; i++) {
		trk_t *t = &c->trk[i];
		long off;

		if (lba < t->t.start || lba >= t->t.start + t->t.length) continue;
		if (t->t.audio) return -1;
		if (c->open_file != t->file) {
			if (c->fh) hf_close(c->fh);
			c->fh = hf_open(c->file[t->file].path);
			c->open_file = c->fh ? t->file : -1;
			if (!c->fh) return -1;
		}
		off = (long)(lba - c->file[t->file].base) * t->secsize + t->skip;
		if (hf_seek(c->fh, off, HF_SEEK_SET) != off) return -1;
		return hf_read(c->fh, buf, 2048) == 2048 ? 0 : -1;
	}
	return -1;
}
