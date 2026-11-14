/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * egc.c - the EGC: registers 4A0h-4AEh (16 bits, reachable while the EGC mode is on) and CPU access to the
 * graphics VRAM through it.
 *
 *   4A0h  bits 3-0: write protect of planes E G R B (1 = left alone)
 *   4A2h  bits 9-8: the plane a single-plane read returns; bits 14-13: the pattern source P: 00 the pattern
 *         register, 01 the background colour, 10 the foreground colour
 *   4A4h  bits 7-0: the raster operation; bits 9-8: load the pattern register from 01 the data a read fetches,
 *         10 the destination before a write; bit 10: the source S comes from 0 VRAM reads, 1 the CPU's writes;
 *         bits 12-11: a write stores 00 the source S, 01 the raster operation's result, 10 the pattern P;
 *         bit 13: reads return 0 one plane (through the shifter when it is the source), 1 a comparison with the
 *         foreground colour
 *   4A6h  foreground colour;  4AAh  background colour;  4A8h  bit mask of the writes
 *   4ACh  bits 3-0 the source's first bit, bits 7-4 the destination's first bit, bit 12 descending addresses
 *   4AEh  bits 11-0: bits to transfer - 1
 *
 * The raster operation is a truth table: the result bit is bit (P * 4 + S * 2 + D) of the operation code, D being
 * the destination.  S passes through the shifter, which lines the source bits up with the destination: it is a
 * queue of bits per plane in transfer order (left to right on the screen, or right to left when descending).  The
 * first source bits up to the source bit address are dropped; the first destination unit starts with as many empty
 * places as the destination bit address says; a unit (8 or 16 bits) is put out only when the queue holds all the bits
 * it needs, else it is put out empty; the bits past the transfer length are empty as well, after which the transfer
 * starts again.  Empty places are not written. */
#include <compiler.h>
#include <cpulink.h>
#include <pc98.h>
#include <io.h>
#include <devices.h>
#include <video.h>

#define PLANE	0x8000u
#define QMAX	48			/* bits the shifter holds */

static const UINT32 s_plane[4] = { GVRAM_B, GVRAM_R, GVRAM_G, GVRAM_E };

static struct {
	UINT16	protect, psel, ope, fg, mask, bg, bitadr, length;
	UINT16	pat[4];			/* pattern register, per plane (memory order) */
	UINT16	fgc[4], bgc[4];		/* the colours as plane words */
	/* shifter */
	UINT64	q[4];
	UINT	n, skip, lead, left;
	UINT16	s[4];			/* the last unit it put out (memory order) */
	UINT16	svalid;			/* its valid places */
} e;

static UINT8 rev8(UINT8 x)
{
	x = (UINT8)((x >> 4) | (x << 4));
	x = (UINT8)(((x & 0xcc) >> 2) | ((x & 0x33) << 2));
	return (UINT8)(((x & 0xaa) >> 1) | ((x & 0x55) << 1));
}

/* memory order (byte: as is; word: low byte first) <-> transfer order (first bit in the most significant place) */
static UINT to_xfer(UINT v, UINT w)
{
	const int dec = (e.bitadr >> 12) & 1;

	if (w == 8) return dec ? rev8((UINT8)v) : (v & 0xff);
	return dec ? ((UINT)rev8((UINT8)(v >> 8)) << 8) | rev8((UINT8)v) : ((v & 0xff) << 8) | ((v >> 8) & 0xff);
}

static UINT from_xfer(UINT v, UINT w)
{
	const int dec = (e.bitadr >> 12) & 1;

	if (w == 8) return dec ? rev8((UINT8)v) : (v & 0xff);
	return dec ? ((UINT)rev8((UINT8)(v >> 8)) << 8) | rev8((UINT8)v) : ((v & 0xff) << 8) | ((v >> 8) & 0xff);
}

static void restart(void)
{
	UINT p;

	for (p = 0; p < 4; p++) e.q[p] = 0;
	e.n = 0;
	e.skip = e.bitadr & 15;
	e.lead = (e.bitadr >> 4) & 15;
	e.left = (e.length & 0xfff) + 1u;
}

/* put a unit of `w` bits per plane (memory order) into the queue */
static void push(const UINT16 *v, UINT w)
{
	const UINT drop = e.skip < w ? e.skip : w;
	const UINT take = w - drop;
	UINT p;

	e.skip -= drop;
	if (!take || e.n + take > QMAX) return;
	for (p = 0; p < 4; p++) {
		e.q[p] = (e.q[p] << take) | (to_xfer(v[p], w) & ((1u << take) - 1));
	}
	e.n += take;
}

/* take a unit of `w` bits out: e.s / e.svalid */
static void pull(UINT w)
{
	const UINT lead = e.lead < w ? e.lead : w;
	const UINT room = w - lead;
	const UINT need = room < e.left ? room : e.left;
	UINT p;

	e.svalid = 0;
	if (e.n < need) return;
	e.lead -= lead;
	if (need) {
		const UINT sh = room - need;
		const UINT m = ((1u << need) - 1) << sh;

		for (p = 0; p < 4; p++) {
			e.s[p] = (UINT16)from_xfer((UINT)((e.q[p] >> (e.n - need)) << sh) & m, w);
		}
		e.svalid = (UINT16)from_xfer(m, w);
		e.n -= need;
		e.left -= need;
	}
	if (!e.left) restart();
}

/* ---- the raster operation ---- */

static UINT16 rop(UINT code, UINT16 p, UINT16 s, UINT16 d)
{
	UINT r = 0, k;

	for (k = 0; k < 8; k++) {
		if (code & (1u << k)) {
			r |= ((k & 4) ? p : ~p) & ((k & 2) ? s : ~s) & ((k & 1) ? d : ~d);
		}
	}
	return (UINT16)r;
}

static UINT16 pattern(UINT p)
{
	switch ((e.psel >> 13) & 3) {
	case 1: return e.bgc[p];
	case 2: return e.fgc[p];
	default: return e.pat[p];
	}
}

/* ---- VRAM ---- */

static UINT32 base_of(void)
{
	return g_vid.access_page ? GVRAM_PAGE1 : 0;
}

static UINT16 vram_get(UINT32 a, UINT w)
{
	return (UINT16)(w == 8 ? mem[a] : (mem[a] | (mem[a + 1] << 8)));
}

static void vram_put(UINT32 a, UINT w, UINT16 v, UINT16 m)
{
	mem[a] = (UINT8)((mem[a] & ~m) | (v & m));
	if (w == 16) mem[a + 1] = (UINT8)((mem[a + 1] & ~(m >> 8)) | ((v >> 8) & (m >> 8)));
}

/* a read of `w` bits at plane offset o */
static UINT16 egc_read(UINT32 o, UINT w)
{
	const UINT32 b = base_of();
	UINT16 v[4];
	UINT p;

	for (p = 0; p < 4; p++) v[p] = vram_get(b + s_plane[p] + o, w);
	if (((e.ope >> 8) & 3) == 1) {
		for (p = 0; p < 4; p++) e.pat[p] = v[p];
	}
	if (!(e.ope & 0x0400)) {
		push(v, w);
		pull(w);
	}
	if (e.ope & 0x2000) {
		UINT16 diff = 0;
		for (p = 0; p < 4; p++) {
			if (!(e.protect & (1u << p))) diff |= v[p] ^ e.fgc[p];
		}
		return (UINT16)(~diff & (w == 8 ? 0xff : 0xffff));
	}
	if (!(e.ope & 0x0400)) return (UINT16)(e.s[(e.psel >> 8) & 3] & (w == 8 ? 0xff : 0xffff));
	return v[(e.psel >> 8) & 3];
}

/* a write of `w` bits at plane offset o; the byte lane of a byte write is lane (0 low, 1 high) */
static void egc_write(UINT32 o, UINT w, UINT16 cpu, UINT lane)
{
	const UINT32 b = base_of();
	const UINT page = g_vid.access_page & 1;
	UINT16 m = (w == 8) ? (UINT16)(lane ? e.mask >> 8 : e.mask & 0xff) : e.mask;
	UINT16 d[4], out[4];
	UINT p;

	for (p = 0; p < 4; p++) d[p] = vram_get(b + s_plane[p] + o, w);
	if (((e.ope >> 8) & 3) == 2) {
		for (p = 0; p < 4; p++) e.pat[p] = d[p];
	}
	if (e.ope & 0x0400) {				/* the CPU's data is the source */
		const UINT16 c[4] = { cpu, cpu, cpu, cpu };
		push(c, w);
		pull(w);
	}
	m &= e.svalid;
	for (p = 0; p < 4; p++) {
		const UINT16 pp = (w == 8) ? (UINT16)(lane ? pattern(p) >> 8 : pattern(p) & 0xff) : pattern(p);
		switch ((e.ope >> 11) & 3) {
		case 1: out[p] = rop(e.ope & 0xff, pp, e.s[p], d[p]); break;
		case 2: out[p] = pp; break;
		default: out[p] = e.s[p]; break;
		}
	}
	if (!m) return;
	for (p = 0; p < 4; p++) {
		if (!(e.protect & (1u << p))) vram_put(b + s_plane[p] + o, w, out[p], m);
	}
	g_grph_dirty[o] |= (UINT8)(1u << page);
	if (w == 16) g_grph_dirty[(o + 1) & (PLANE - 1)] |= (UINT8)(1u << page);
	g_vreq.grph[page] |= VR_CHANGED;
}

REG8 egc_rd8(UINT32 a)
{
	g_cpu.slice_left -= g_memwait_grcg;
	return (REG8)egc_read(a & (PLANE - 1), 8);
}

void egc_wr8(UINT32 a, REG8 v)
{
	g_cpu.slice_left -= g_memwait_grcg;
	egc_write(a & (PLANE - 1), 8, (UINT16)(v * 0x0101u), a & 1);
}

REG16 egc_rd16(UINT32 a)
{
	g_cpu.slice_left -= g_memwait_grcg;
	if (!(a & 1)) return egc_read(a & (PLANE - 1), 16);
	if (e.bitadr & 0x1000) {
		const UINT hi = egc_read((a + 1) & (PLANE - 1), 8);
		return (REG16)((hi << 8) | egc_read(a & (PLANE - 1), 8));
	}
	{
		const UINT lo = egc_read(a & (PLANE - 1), 8);
		return (REG16)(lo | (egc_read((a + 1) & (PLANE - 1), 8) << 8));
	}
}

void egc_wr16(UINT32 a, REG16 v)
{
	g_cpu.slice_left -= g_memwait_grcg;
	if (!(a & 1)) {
		egc_write(a & (PLANE - 1), 16, v, 0);
	}
	else if (e.bitadr & 0x1000) {
		egc_write((a + 1) & (PLANE - 1), 8, (UINT16)((v >> 8) * 0x0101u), 0);
		egc_write(a & (PLANE - 1), 8, (UINT16)((v & 0xff) * 0x0101u), 1);
	}
	else {
		egc_write(a & (PLANE - 1), 8, (UINT16)((v & 0xff) * 0x0101u), 1);
		egc_write((a + 1) & (PLANE - 1), 8, (UINT16)((v >> 8) * 0x0101u), 0);
	}
}

void egc_gdc_dot(UINT page, UINT32 o, UINT16 bits)
{
	const UINT8 keep = g_vid.access_page;

	g_vid.access_page = (UINT8)page;
	egc_write(o & (PLANE - 2), 16, bits, 0);
	g_vid.access_page = keep;
}

/* ---- registers ---- */

static void colours(UINT16 *dst, UINT v)
{
	UINT p;

	for (p = 0; p < 4; p++) dst[p] = (v & (1u << p)) ? 0xffff : 0;
}

static void reg_w(UINT r, UINT16 v)
{
	switch (r) {
	case 0: e.protect = v; break;
	case 1: e.psel = v; break;
	case 2: e.ope = v; break;
	case 3: e.fg = v; colours(e.fgc, v); break;
	case 4: e.mask = v; break;
	case 5: e.bg = v; colours(e.bgc, v); break;
	case 6: e.bitadr = v; restart(); break;
	default: e.length = v; restart(); break;
	}
}

static UINT16 *reg_p(UINT r)
{
	static UINT16 *const regs[8] = { &e.protect, &e.psel, &e.ope, &e.fg, &e.mask, &e.bg, &e.bitadr, &e.length };
	return regs[r & 7];
}

static void port_w8(UINT port, REG8 v)
{
	const UINT r = (port >> 1) & 7;
	const UINT16 old = *reg_p(r);

	if (!(g_vid.mode2 & M2_EGC) || g_vid.grcg_chip != 3) return;
	reg_w(r, (port & 1) ? (UINT16)((old & 0x00ff) | (v << 8)) : (UINT16)((old & 0xff00) | v));
}

static void port_w16(UINT port, REG16 v)
{
	if (!(g_vid.mode2 & M2_EGC) || g_vid.grcg_chip != 3) return;
	reg_w((port >> 1) & 7, v);
}

void egc_reset(const Pc98Settings *cfg)
{
	(void)cfg;
	memset(&e, 0, sizeof(e));
	e.protect = 0xfff0;
	e.psel = 0x00ff;
	e.mask = 0xffff;
	e.length = 0x000f;
	restart();
}

void egc_bind(void)
{
	UINT i;

	for (i = 0; i < 16; i++) io_port(0x4a0 + i, port_w8, NULL);
	for (i = 0; i < 16; i += 2) io_port16(0x4a0 + i, port_w16, NULL);
}

/* ---- state save / load (state.c) ---- */
#include <stateint.h>
void egc_state(SsCtx *c)
{
	SS_VAR(c, "EGC ", e);
}
