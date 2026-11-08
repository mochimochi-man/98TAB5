/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * atapi.h - an ATAPI CD-ROM drive, the master of the second IDE channel (bank 1 at 432h), as the
 * PC-9821 internal CD-ROM drives are connected.  Same task file ports as the hard disks (640h..64Eh,
 * 74Ch); every command is PIO and finishes without delay.
 */
#ifndef PC98CORE_ATAPI_H
#define PC98CORE_ATAPI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The drive is fitted (the menu chose to have one); the disc can come and go. */
void	atapi_fit(int fitted);
int	atapi_fitted(void);
/* Put a disc in (an image path) or take it out (NULL / ""); the drive reports the change. */
int	atapi_insert(const char *path);
const char *atapi_disc(void);

void	atapi_reset(void);
extern void (*atapi_irq_hook)(void);
void	atapi_out(uint32_t port, uint8_t v);
uint8_t	atapi_in(uint32_t port);

#ifdef __cplusplus
}
#endif
#endif
