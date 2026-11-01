/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * ide.h - PC-98 IDE (ATA) hard disk interface, stage 4-5.
 *
 * The PC-9821 style IDE board: an ATA task file at 640h..64Eh (even ports; the data register at 640h is
 * 16 bits wide and is also visible as 641h), device control / alternate status at 74Ch, drive address
 * at 74Eh, bank (channel) select at 430h / 432h and the status ports 433h / 435h.  One channel with
 * two drives (master = unit 0, slave = unit 1); bank 0 selects it, any other bank value selects an
 * empty channel.  Every command is PIO, 28-bit LBA or CHS, executed without delay (BSY is never seen
 * set outside a soft reset).
 */
#ifndef PC98CORE_IDE_H
#define PC98CORE_IDE_H

#include <stdint.h>
#include "../media/hddmedia.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IDE_DRIVES	2

/* Non-zero when the image can be an IDE drive (512-byte sectors, ATA-sized geometry). */
int	ide_accepts(const hm_disk_t *d);
/* Connect an image to drive 0 (master) or 1 (slave); takes ownership, replaces and closes the old one.
 * NULL detaches.  Returns -1 (and closes the image) when it is not an IDE image. */
int	ide_attach(int drive, hm_disk_t *d);
hm_disk_t *ide_disk(int drive);

/* Raised (when the interrupt is enabled by the device control register) at the end of a command and
 * for every data block; the machine wires it to IRQ 9.  May be NULL. */
extern void (*ide_irq_hook)(void);

/* Machine reset (hardware reset of the drives); the images stay connected. */
void	ide_machine_reset(void);

/* The port handlers (registered by ide_bind(); exported for the host tests).  Data bytes at 640h and
 * 641h are one stream, so a 16-bit access (low byte at 640h, high byte at 641h) moves one word. */
void	ide_out(uint32_t port, uint8_t data);
uint8_t	ide_in(uint32_t port);

#ifdef __cplusplus
}
#endif
#endif
