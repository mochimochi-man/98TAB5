/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * stateint.h - what state.c and the modules share for state save / load (see include/state.h).
 *
 * Every module that owns state offers one function, xxx_state(SsCtx *), that walks its variables with ss_blk() in a
 * fixed order. The same function saves and loads: ss_blk writes the chunk when saving, and reads it back in place when
 * loading (the tag, the version and the length must match, else the context fails and every later call does nothing).
 * A chunk is a 4 character tag, a uint16 version, a uint32 length and the bytes.
 *
 * Plain data (registers, counters, memory) goes out as it lies in memory. A pointer never goes into the stream:
 *   - ss_blk_keep() names pointer members (heap, or into static data) that are written as zero and, on a load,
 *     left as the running machine has them;
 *   - a function pointer, or a pointer to a fixed object, that is part of the state (an event's callback ...) is
 *     replaced by its number in the table of known functions (SF_*, state.c) around the ss_blk call.
 *     A pointer the table does not know fails the save. */
#ifndef PC98_STATEINT_H
#define PC98_STATEINT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SsCtx SsCtx;

int	ss_loading(const SsCtx *c);
/* 0 = ok. `keep` lists byte offsets (inside p) of pointer members that a load leaves untouched. */
int	ss_blk_keep(SsCtx *c, const char *tag, void *p, size_t n, const size_t *keep, unsigned nkeep);
static inline int ss_blk(SsCtx *c, const char *tag, void *p, size_t n) { return ss_blk_keep(c, tag, p, n, NULL, 0); }
/* numbers of the known functions / objects (never renumber; append) */
enum {
	SF_NONE = 0,
	SF_GDC_RETRACE_START, SF_GDC_RETRACE_END, SF_GDCFIG_DRAW_DONE, SF_KBD_BYTE_TIME, SF_PIT_TIMER0, SF_MOUSE_IRQ,
	SF_FDC_END_EVENT, SF_FDC_BYTE_EVENT, SF_FDC_SEEK_EVENT, SF_B26_TIMER_A, SF_B26_TIMER_B,
	SF_FDC_RW_SECTOR, SF_FDC_DIAG_SECTOR, SF_FDC_COMMAND_READY,
	SF_DMA_NONE, SF_DMA_FDC,
	SF_COUNT
};
/* the slot at *slot (a pointer) becomes its number; *orig keeps the pointer for ss_fn_put_back() */
void	ss_fn_encode(SsCtx *c, void *slot, void *orig);
void	ss_fn_put_back(void *slot, const void *orig);
/* after a load: the number in *slot becomes the pointer again (NULL when the stream failed) */
void	ss_fn_decode(SsCtx *c, void *slot);
void	gdc_fns(const void **t);
void	gdcfig_fns(const void **t);
void	kbd_fns(const void **t);
void	pit_fns(const void **t);
void	mouse_fns(const void **t);
void	fdc_fns(const void **t);
void	board_fns(const void **t);
void	dma_fns(const void **t);

#define SS_VAR(c, tag, v)	ss_blk((c), (tag), (void *)&(v), sizeof(v))

void	bus_state(SsCtx *c);
void	events_state(SsCtx *c);
void	cpu_state(SsCtx *c);
void	pic_state(SsCtx *c);
void	pit_state(SsCtx *c);
void	ppi_state(SsCtx *c);
void	rtc_state(SsCtx *c);
void	kbd_state(SsCtx *c);
void	keyunit_state(SsCtx *c);
void	mouse_state(SsCtx *c);
void	dma_state(SsCtx *c);
void	fdc_state(SsCtx *c);
void	sasi_state(SsCtx *c);
void	ide_state(SsCtx *c);
void	atapi_state(SsCtx *c);
void	cgwin_state(SsCtx *c);
void	egc_state(SsCtx *c);
void	gdc_state(SsCtx *c);
void	gdcfig_state(SsCtx *c);
void	sysctl_state(SsCtx *c);
void	artic_state(SsCtx *c);
void	video_state(SsCtx *c);
void	board_state(SsCtx *c);
void	spk_state(SsCtx *c);
void	snd_state(SsCtx *c);

/* after a load: the renderer redraws everything, the mixer starts from the present */
void	video_state_loaded(void);
void	snd_state_loaded(void);

/* the image a hard disk unit has open ("" = none), and the one in the CD-ROM drive */
const char *sasi_unit_path(int unit);

#ifdef __cplusplus
}
#endif

#endif
