/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * pcstr.h - the few string helpers the firmware needs (names and paths are UTF-8). */
#ifndef PC98_PCSTR_H
#define PC98_PCSTR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when a and b are the same string, ASCII letters compared without case. */
int pcs_eqi(const char *a, const char *b);

/* Copy src into dst (size bytes, size > 0), always NUL-terminated, never cutting a UTF-8 sequence. */
void pcs_copy(char *dst, const char *src, size_t size);

/* Length in bytes of the UTF-8 character at p; 0 at the terminating NUL. */
int pcs_u8len(const char *p);

#ifdef __cplusplus
}
#endif

#endif
