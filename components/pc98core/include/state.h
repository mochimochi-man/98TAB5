/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * state.h - the whole machine saved to a byte stream and back (state save / load).
 *
 * The stream is only good for the firmware that wrote it: structures are written as they lie in memory,
 * function pointers and pointers into static data included. The host checks the build before loading
 * (main/statesave.cpp). Heap pointers are never taken from a stream; each module keeps its own.
 *
 * The host does the file I/O through the two callbacks, and calls these only while the machine is
 * stopped (from the menu, between machine_frame calls).
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int	(*write)(void *ctx, const void *p, size_t n);	/* 0 = written */
	void	*ctx;
} Pc98StateOut;

typedef struct {
	int	(*read)(void *ctx, void *p, size_t n);		/* 0 = all n bytes read */
	void	*ctx;
} Pc98StateIn;

/* 0 on success. */
int	pc98_state_save(const Pc98StateOut *o);

/* 0 on success. A stream that fails part-way leaves the machine reset (machine_reset), never half-loaded. */
int	pc98_state_load(const Pc98StateIn *in);

/* The images the stream had in its drives, for the host's own records and the slot list:
 * FDD 0..3, then HDD 0..1, then the CD-ROM ("" = empty). Filled by pc98_state_load before the machine
 * state is restored, and by pc98_state_peek without touching the machine. */
#define PC98_STATE_MEDIA	7
#define PC98_STATE_PATH		256
typedef struct {
	char	path[PC98_STATE_MEDIA][PC98_STATE_PATH];
} Pc98StateMedia;

int	pc98_state_peek(const Pc98StateIn *in, Pc98StateMedia *media);
extern Pc98StateMedia g_state_media;	/* what the last pc98_state_load found */

#ifdef __cplusplus
}
#endif
