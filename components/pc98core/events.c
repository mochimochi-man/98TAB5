/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * events.c - timed callbacks (see events.h).
 *
 * Each slot keeps its deadline in CPU clocks counted from the start of the current slice.  When a slice ends, its
 * length is taken off every deadline; a deadline that reaches 0 or less is due.  Due events are called in deadline
 * order, ties in the order they were armed; an event armed while others are being called waits for the next round.
 * Single threaded. */
#include <compiler.h>
#include <cpulink.h>
#include <events.h>
#include <limits.h>
#include <string.h>

typedef struct {
	SINT32	deadline;
	UINT32	seq;		/* arming order, for ties */
	ev_fn	fn;
	UINT8	armed;
} Slot;

static Slot	s_slot[EV_COUNT];
static UINT32	s_seq;

static SINT32 now(void)
{
	return g_cpu.slice_len - g_cpu.slice_left;
}

static int before(UINT a, UINT b)
{
	if (s_slot[a].deadline != s_slot[b].deadline) {
		return s_slot[a].deadline < s_slot[b].deadline;
	}
	return s_slot[a].seq < s_slot[b].seq;
}

/* the slice must not run past the earliest deadline */
static void arm(UINT id, SINT32 deadline, ev_fn fn)
{
	Slot *s = &s_slot[id];

	s->deadline = deadline;
	s->fn = fn;
	s->seq = s_seq++;
	s->armed = 1;
	if (deadline < g_cpu.slice_len) {
		const SINT32 d = g_cpu.slice_len - deadline;
		g_cpu.slice_len -= d;
		g_cpu.slice_left -= d;
	}
}

void ev_reset_all(void)
{
	memset(s_slot, 0, sizeof(s_slot));
	s_seq = 0;
}

void ev_at(UINT id, SINT32 clocks, ev_fn fn)
{
	arm(id, now() + clocks, fn);
}

void ev_next(UINT id, SINT32 clocks, ev_fn fn)
{
	arm(id, s_slot[id].deadline + clocks, fn);
}

void ev_cancel(UINT id)
{
	s_slot[id].armed = 0;
}

int ev_armed(UINT id)
{
	return s_slot[id].armed;
}

SINT32 ev_left(UINT id)
{
	return s_slot[id].armed ? s_slot[id].deadline - now() : -1;
}

void ev_call(UINT id)
{
	if (s_slot[id].fn) {
		s_slot[id].fn(id, 0);
	}
}

void ev_slice_done(void)
{
	UINT due[EV_COUNT];
	UINT ndue = 0, i, j;
	const SINT32 len = g_cpu.slice_len;
	SINT32 next = EV_MAXCLOCK;

	g_cpu.clocks_done += len;
	for (i = 0; i < EV_COUNT; i++) {
		Slot *s = &s_slot[i];
		if (!s->armed) {
			continue;
		}
		s->deadline -= len;
		if (s->deadline > 0) {
			if (s->deadline < next) next = s->deadline;
			continue;
		}
		s->armed = 0;
		for (j = ndue; j > 0 && before(i, due[j - 1]); j--) {
			due[j] = due[j - 1];
		}
		due[j] = i;
		ndue++;
	}
	g_cpu.slice_len = next;
	g_cpu.slice_left += next;		/* REMCLOCK can be negative: the last instruction ran past the end */
	for (i = 0; i < ndue; i++) {
		if (s_slot[due[i]].fn) {
			s_slot[due[i]].fn(due[i], 1);
		}
	}
	if (s_seq > 0x7fffffffu) {		/* keep the arming order small: renumber in deadline order */
		UINT order[EV_COUNT], n = 0;
		for (i = 0; i < EV_COUNT; i++) {
			if (!s_slot[i].armed) continue;
			for (j = n; j > 0 && before(i, order[j - 1]); j--) order[j] = order[j - 1];
			order[j] = i;
			n++;
		}
		for (i = 0; i < n; i++) s_slot[order[i]].seq = i;
		s_seq = n;
	}
}

void ev_cut_slice(void)
{
	if (g_cpu.slice_left > 0) {
		g_cpu.slice_len -= g_cpu.slice_left;
		g_cpu.slice_left = 0;
	}
}

static SINT32 scale(SINT32 v, UINT32 oldclock, UINT32 newclock)
{
	SINT64 c = ((SINT64)v * newclock + oldclock / 2) / oldclock;

	if (c < 1) c = 1;
	if (c > INT_MAX) c = INT_MAX;
	return (SINT32)c;
}

void ev_rescale(UINT32 oldclock, UINT32 newclock)
{
	UINT i;
	SINT32 first = EV_MAXCLOCK;
	int any = 0;

	if (oldclock == 0) {
		return;
	}
	for (i = 0; i < EV_COUNT; i++) {
		if (s_slot[i].armed && s_slot[i].deadline > 0) {
			s_slot[i].deadline = scale(s_slot[i].deadline, oldclock, newclock);
		}
		if (s_slot[i].armed) {
			any = 1;
			if (s_slot[i].deadline < first) first = s_slot[i].deadline;
		}
	}
	if (!any) {
		return;
	}
	if (g_cpu.slice_len == g_cpu.slice_left) {		/* at a slice boundary: the slice ends at the earliest event */
		g_cpu.slice_len = first;
		g_cpu.slice_left = first;
	}
	else {						/* in mid-slice: scale the slice too (what is left may be 0 or less) */
		g_cpu.slice_len = (SINT32)(((SINT64)g_cpu.slice_len * newclock + oldclock / 2) / oldclock);
		g_cpu.slice_left = (SINT32)(((SINT64)g_cpu.slice_left * newclock + oldclock / 2) / oldclock);
	}
}

/* ---- state save / load (state.c) ---- */
#include <stateint.h>
void events_state(SsCtx *c)
{
	void *orig[EV_COUNT];
	UINT i;

	for (i = 0; i < EV_COUNT; i++) {
		if (!ss_loading(c)) ss_fn_encode(c, &s_slot[i].fn, &orig[i]);
	}
	SS_VAR(c, "EVSL", s_slot);
	for (i = 0; i < EV_COUNT; i++) {
		if (ss_loading(c)) ss_fn_decode(c, &s_slot[i].fn);
		else ss_fn_put_back(&s_slot[i].fn, &orig[i]);
	}
	SS_VAR(c, "EVSQ", s_seq);
}
