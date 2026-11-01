/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * hostfile.h - the files the emulator reads and writes (main/hostfile_sd.cpp: the SD card, plus the ROMs built into
 * the firmware under ":builtin/..."). */
#ifndef PC98_HOSTFILE_H
#define PC98_HOSTFILE_H

#include "compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HostFileTag *HFILE;	/* NULL: no file */

#define HF_SEEK_SET	0
#define HF_SEEK_CUR	1
#define HF_SEEK_END	2

/* hf_attr(): -1 = no such file */
#define HF_ATTR_RO	0x01
#define HF_ATTR_DIR	0x10
#define HF_ATTR_FILE	0x20

void	hf_init(void);
void	hf_term(void);
HFILE	hf_open(const char *path);			/* read only */
HFILE	hf_open_rw(const char *path);
HFILE	hf_create(const char *path);
long	hf_seek(HFILE f, long pos, int whence);
UINT	hf_read(HFILE f, void *data, UINT length);
UINT	hf_write(HFILE f, const void *data, UINT length);	/* length 0: the file ends here */
short	hf_close(HFILE f);
long	hf_size(HFILE f);
short	hf_delete(const char *path);
short	hf_attr(const char *path);

#ifdef __cplusplus
}
#endif

#endif
