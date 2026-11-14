/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * state.c - the whole machine to a byte stream and back (include/state.h).
 *
 * Stream: "98SS" and a version, then chunks (4 character tag, uint32 length, bytes; stateint.h) in a fixed order:
 *   HEAD  MEDI (the images in the drives)  CFG (the machine description the stream was made for)
 *   then the CPU, main memory and extended memory, the bus, the event scheduler, every device and the sound side,
 *   END.
 * Main memory (mem[]) carries the RAM, the VRAM planes, the BIOS image and the CG ROM image with its user-defined
 * characters; the ones behind it (page tables of the bus, the I/O handler tables) are built by machine_reset() from the
 * same settings and are not part of the stream. Not saved either: the renderer's pixel buffers and tables, the
 * mixer's output blocks, the diagnostic logs and g_settings (the user's choices).
 *
 * The stream holds no address of the firmware: function pointers are written as numbers (stateint.h SF_*), pointers into
 * static or heap memory as zeros that a load leaves as the running machine has them. A change to what a chunk holds is a
 * new chunk version (s_chunk_ver below).
 *
 * Images are never in the stream, only their paths (MEDI). A load mounts what the stream had, then restores the devices.
 * A load that gets that far and fails leaves the machine reset. */
#include <string.h>
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <devices.h>
#include <video.h>
#include <sound.h>
#include "state.h"
#include "stateint.h"
#include "media/drives.h"
#include "dev/atapi.h"

#define SS_MAGIC	"98SS"
#define SS_VERSION	1u

Pc98StateMedia g_state_media;

struct SsCtx {
	const Pc98StateOut	*out;
	const Pc98StateIn	*in;
	int			loading;
	int			err;
};

typedef struct {
	char	magic[4];
	UINT32	version;
} Head;

/* what has to be the same in the machine that loads a stream */
typedef struct {
	MachineDesc	mach;
	UINT8		v30;
	UINT8		x386;
	UINT32		ext_size;
} Cfg;

int ss_loading(const SsCtx *c)
{
	return c->loading;
}

static int ss_raw(SsCtx *c, void *p, size_t n)
{
	int r;

	if (c->err) return -1;
	if (!n) return 0;
	r = c->loading ? c->in->read(c->in->ctx, p, n) : c->out->write(c->out->ctx, p, n);
	if (r) c->err = -1;
	return r;
}

/* The version of a chunk's contents: 1 unless listed here. Change the layout of what a chunk holds (a structure that
 * gains a member, a variable that changes type), and its tag gets a new entry with the next number - or the next
 * number if it has one already. A load reads a chunk only when its version and length are the ones of this firmware;
 * an older stream is converted here one day (state_convert), until then it is refused. */
static const struct { char tag[4]; uint16_t ver; } s_chunk_ver[] = {
	{ { 'E', 'X', 'T', 'M' }, 2 },	/* 2: paged, all-zero pages left out (ss_mem) */
	{ { 'M', 'E', 'M', ' ' }, 2 },
	{ { 0, 0, 0, 0 }, 0 }
};

static uint16_t chunk_version(const char *tag)
{
	unsigned i;

	for (i = 0; i < sizeof(s_chunk_ver) / sizeof(s_chunk_ver[0]); i++) {
		if (s_chunk_ver[i].ver && memcmp(s_chunk_ver[i].tag, tag, 4) == 0) return s_chunk_ver[i].ver;
	}
	return 1;
}

#define KEEP_MAX	8
#define HDR_BYTES	10	/* tag 4, version 2, length 4 (little endian) */

int ss_blk_keep(SsCtx *c, const char *tag, void *p, size_t n, const size_t *keep, unsigned nkeep)
{
	uint8_t h[HDR_BYTES];
	const uint16_t ver = chunk_version(tag);
	const UINT32 len = (UINT32)n;
	size_t ko[KEEP_MAX];
	unsigned i, j;

	if (c->err) return -1;
	if (nkeep > KEEP_MAX) {
		c->err = -2;
		return -1;
	}
	for (i = 0; i < nkeep; i++) {			/* ascending */
		for (j = i; j > 0 && ko[j - 1] > keep[i]; j--) ko[j] = ko[j - 1];
		ko[j] = keep[i];
	}
	memcpy(h, tag, 4);
	h[4] = (uint8_t)ver; h[5] = (uint8_t)(ver >> 8);
	h[6] = (uint8_t)len; h[7] = (uint8_t)(len >> 8); h[8] = (uint8_t)(len >> 16); h[9] = (uint8_t)(len >> 24);
	if (!c->loading) {
		size_t done = 0;
		static const uint8_t zero[sizeof(void *)];

		if (ss_raw(c, h, sizeof(h))) return -1;
		for (i = 0; i < nkeep; i++) {		/* a pointer member goes out as zeros */
			if (ss_raw(c, (uint8_t *)p + done, ko[i] - done) || ss_raw(c, (void *)zero, sizeof(zero))) return -1;
			done = ko[i] + sizeof(void *);
		}
		return ss_raw(c, (uint8_t *)p + done, n - done);
	}
	{
		uint8_t g[HDR_BYTES];
		void *saved[KEEP_MAX];
		int r;

		if (ss_raw(c, g, sizeof(g))) return -1;
		if (memcmp(g, h, sizeof(g)) != 0) {	/* another tag, version or length */
			c->err = -2;
			return -1;
		}
		for (i = 0; i < nkeep; i++) memcpy(&saved[i], (char *)p + ko[i], sizeof(void *));
		r = ss_raw(c, p, n);
		for (i = 0; i < nkeep; i++) memcpy((char *)p + ko[i], &saved[i], sizeof(void *));
		return r;
	}
}

/* A memory chunk: a bit per 4 KB page (set = the page is in the stream), then the pages that are. Mostly-empty
 * memory - the extended memory above all - then costs next to nothing to write. The chunk's length is still the
 * memory's own, so a firmware with a different amount refuses it as before. */
#define SS_PAGE	4096

static int page_zero(const uint8_t *p)
{
	const UINT32 *w = (const UINT32 *)p;
	unsigned i;

	for (i = 0; i < SS_PAGE / 4; i++) if (w[i]) return 0;
	return 1;
}

int ss_mem(SsCtx *c, const char *tag, void *p, size_t n)
{
	uint8_t h[HDR_BYTES], map[512];
	const uint16_t ver = chunk_version(tag);
	const UINT32 len = (UINT32)n;
	uint8_t *m = (uint8_t *)p;
	size_t pages = (n + SS_PAGE - 1) / SS_PAGE, i, done;

	if (c->err) return -1;
	memcpy(h, tag, 4);
	h[4] = (uint8_t)ver; h[5] = (uint8_t)(ver >> 8);
	h[6] = (uint8_t)len; h[7] = (uint8_t)(len >> 8); h[8] = (uint8_t)(len >> 16); h[9] = (uint8_t)(len >> 24);
	if (!c->loading) {
		if (ss_raw(c, h, sizeof(h))) return -1;
	} else {
		uint8_t g[HDR_BYTES];

		if (ss_raw(c, g, sizeof(g))) return -1;
		if (memcmp(g, h, sizeof(g)) != 0) {
			c->err = -2;
			return -1;
		}
	}
	/* the map goes in pieces of 512 bytes, each followed by the 4096 pages it covers */
	for (done = 0; done < pages; done += sizeof(map) * 8) {
		const size_t cnt = (pages - done < sizeof(map) * 8) ? pages - done : sizeof(map) * 8;
		const size_t mb = (cnt + 7) / 8;

		if (!c->loading) {
			memset(map, 0, mb);
			for (i = 0; i < cnt; i++) {
				const size_t off = (done + i) * SS_PAGE;

				if (n - off < SS_PAGE || !page_zero(m + off)) map[i / 8] |= (uint8_t)(1u << (i % 8));
			}
		}
		if (ss_raw(c, map, mb)) return -1;
		for (i = 0; i < cnt; i++) {
			const size_t off = (done + i) * SS_PAGE;
			const size_t sz = (n - off < SS_PAGE) ? n - off : SS_PAGE;

			if (map[i / 8] & (1u << (i % 8))) {
				if (ss_raw(c, m + off, sz)) return -1;
			} else if (c->loading) {
				memset(m + off, 0, sz);
			}
		}
	}
	return c->err ? -1 : 0;
}

/* ---- the known functions (stateint.h SF_*) ---- */

static const void *s_fn[SF_COUNT];

static void fn_table(void)
{
	memset(s_fn, 0, sizeof(s_fn));
	gdc_fns(s_fn);
	gdcfig_fns(s_fn);
	kbd_fns(s_fn);
	pit_fns(s_fn);
	mouse_fns(s_fn);
	fdc_fns(s_fn);
	board_fns(s_fn);
	dma_fns(s_fn);
}

void ss_fn_encode(SsCtx *c, void *slot, void *orig)
{
	const void *p;
	uintptr_t id = 0;
	unsigned i;

	memcpy(&p, slot, sizeof(p));
	memcpy(orig, &p, sizeof(p));
	if (p) {
		for (i = 1; i < SF_COUNT; i++) {
			if (s_fn[i] == p) {
				id = i;
				break;
			}
		}
		if (!id && !c->err) c->err = -3;	/* a function the table does not know: never written blind */
	}
	memset(slot, 0, sizeof(void *));
	memcpy(slot, &id, sizeof(id));
}

void ss_fn_put_back(void *slot, const void *orig)
{
	memcpy(slot, orig, sizeof(void *));
}

void ss_fn_decode(SsCtx *c, void *slot)
{
	uintptr_t id;
	const void *p = NULL;

	memcpy(&id, slot, sizeof(id));
	if (id && id < SF_COUNT && !c->err) p = s_fn[id];
	else if (id && !c->err) c->err = -2;
	memcpy(slot, &p, sizeof(p));
}

/* ---- the images in the drives ---- */

static void copy_path(char *dst, const char *src)
{
	strncpy(dst, src, PC98_STATE_PATH - 1);
	dst[PC98_STATE_PATH - 1] = '\0';
}

static void media_now(Pc98StateMedia *m)
{
	int i;

	memset(m, 0, sizeof(*m));
	for (i = 0; i < 4; i++) {
		if (fdd_media_disk(i) && g_settings.fdd_image[i][0] != ' ') copy_path(m->path[i], (const char *)g_settings.fdd_image[i]);
	}
	for (i = 0; i < 2; i++) copy_path(m->path[4 + i], sasi_unit_path(i));
	copy_path(m->path[6], atapi_disc());
}

/* put the images of the stream in the drives; 0 = ok */
static int media_apply(const Pc98StateMedia *want)
{
	Pc98StateMedia cur;
	int i, reset = 0;

	media_now(&cur);
	for (i = 0; i < 4; i++) {
		if (!strcmp(want->path[i], cur.path[i])) continue;
		if (want->path[i][0]) {
			if (drv_fd_mount(i, want->path[i]) != 0) {
				g_settings.fdd_image[i][0] = '\0';
				return -1;
			}
			copy_path((char *)g_settings.fdd_image[i], want->path[i]);
		} else {
			drv_fd_eject(i);
			g_settings.fdd_image[i][0] = '\0';
		}
	}
	/* the hard disks and the disc are opened by the reset (sasi_bind.c) */
	for (i = 0; i < 2; i++) {
		if (strcmp(want->path[4 + i], cur.path[4 + i])) {
			copy_path((char *)g_settings.hdd_image[i], want->path[4 + i]);
			reset = 1;
		}
	}
	if (strcmp(want->path[6], cur.path[6])) {
		copy_path((char *)g_settings.cd_image, want->path[6]);
		reset = 1;
	}
	if (reset) {
		machine_reset();
		snd_lock(1);			/* the reset let the sound task in */
		media_now(&cur);
		for (i = 4; i < PC98_STATE_MEDIA; i++) {
			if (strcmp(want->path[i], cur.path[i])) return -1;
		}
	}
	return 0;
}

/* ---- the stream ---- */

static void cfg_now(Cfg *f)
{
	memset(f, 0, sizeof(*f));
	f->mach = g_mach;
	f->v30 = g_cpu.v30;
	f->x386 = (UINT8)g_cpu.x386.enabled;
	f->ext_size = g_cpu.ext_size;
}

static int cfg_same(const Cfg *a, const Cfg *b)
{
	return a->mach.model == b->mach.model && a->mach.base_clock == b->mach.base_clock &&
	       a->mach.clock_mul == b->mach.clock_mul && a->mach.cpu_clock == b->mach.cpu_clock &&
	       a->mach.clk8mhz == b->mach.clk8mhz && a->mach.ext_mb == b->mach.ext_mb && a->mach.sound == b->mach.sound &&
	       a->v30 == b->v30 && a->x386 == b->x386 && a->ext_size == b->ext_size;
}

static void head_now(Head *h)
{
	memset(h, 0, sizeof(*h));
	memcpy(h->magic, SS_MAGIC, 4);
	h->version = SS_VERSION;
}

/* everything behind the head; the same walk saves and loads */
static void body(SsCtx *c)
{
	SS_VAR(c, "MACH", g_mach);
	SS_VAR(c, "MSTA", g_mstat);
	cpu_state(c);
	if (g_cpu.ext_size && g_cpu.ext) ss_mem(c, "EXTM", g_cpu.ext, g_cpu.ext_size);
	ss_mem(c, "MEM ", mem, sizeof(mem));
	bus_state(c);
	events_state(c);
	pic_state(c);
	pit_state(c);
	ppi_state(c);
	rtc_state(c);
	kbd_state(c);
	keyunit_state(c);
	mouse_state(c);
	dma_state(c);
	fdc_state(c);
	sasi_state(c);
	ide_state(c);
	atapi_state(c);
	sysctl_state(c);
	artic_state(c);
	cgwin_state(c);
	gdc_state(c);
	gdcfig_state(c);
	egc_state(c);
	video_state(c);
	board_state(c);
	spk_state(c);
	snd_state(c);
	ss_blk(c, "END ", NULL, 0);
}

int pc98_state_save(const Pc98StateOut *o)
{
	SsCtx c;
	Head h;
	Pc98StateMedia med;
	Cfg cfg;

	if (!o || !o->write) return -1;
	memset(&c, 0, sizeof(c));
	c.out = o;
	head_now(&h);
	media_now(&med);
	cfg_now(&cfg);
	fdd_media_flush_all();			/* the images on the card are what a load mounts again */
	fn_table();
	snd_lock(1);
	snd_catch_up();				/* the chips have taken every register write */
	ss_blk(&c, "HEAD", &h, sizeof(h));
	ss_blk(&c, "MEDI", &med, sizeof(med));
	ss_blk(&c, "CFG ", &cfg, sizeof(cfg));
	body(&c);
	snd_lock(0);
	return c.err ? -1 : 0;
}

/* HEAD and MEDI; <0 = not a stream of this build */
static int read_head(SsCtx *c, Pc98StateMedia *med)
{
	Head h;

	ss_blk(c, "HEAD", &h, sizeof(h));
	ss_blk(c, "MEDI", med, sizeof(*med));
	if (c->err) return -1;
	if (memcmp(h.magic, SS_MAGIC, 4) != 0 || h.version != SS_VERSION) return -1;
	return 0;
}

int pc98_state_peek(const Pc98StateIn *in, Pc98StateMedia *media)
{
	SsCtx c;
	int i;

	if (!in || !in->read || !media) return -1;
	memset(&c, 0, sizeof(c));
	c.in = in;
	c.loading = 1;
	if (read_head(&c, media) != 0) return -1;
	for (i = 0; i < PC98_STATE_MEDIA; i++) media->path[i][PC98_STATE_PATH - 1] = '\0';
	return 0;
}

/* -1 not a stream of this build (the machine is as it was), -2 made for another machine configuration (as it was),
 * -3 an image could not be mounted, -4 the stream ended or did not fit (the last two leave the machine reset) */
int pc98_state_load(const Pc98StateIn *in)
{
	SsCtx c;
	Pc98StateMedia med;
	Cfg saved, now;
	int i;

	if (!in || !in->read) return -1;
	memset(&c, 0, sizeof(c));
	c.in = in;
	c.loading = 1;
	if (read_head(&c, &med) != 0) return -1;
	ss_blk(&c, "CFG ", &saved, sizeof(saved));
	if (c.err) return -1;
	cfg_now(&now);
	if (!cfg_same(&saved, &now)) return -2;
	for (i = 0; i < PC98_STATE_MEDIA; i++) med.path[i][PC98_STATE_PATH - 1] = '\0';
	g_state_media = med;

	fn_table();
	snd_lock(1);
	if (media_apply(&med) != 0) {
		machine_reset();
		snd_lock(0);
		return -3;
	}
	body(&c);
	if (c.err) {
		machine_reset();
		snd_lock(0);
		return -4;
	}
	video_state_loaded();
	snd_state_loaded();
	snd_lock(0);
	return 0;
}
