/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * sasi.h - SASI hard disk interface of the PC-9801 (ports 80h / 82h), stage 4-4.
 */
#ifndef PC98CORE_SASI_H
#define PC98CORE_SASI_H

#include <stdint.h>
#include "../media/hddmedia.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SASI_UNITS	2

/* Non-zero when the image can be presented as a SASI drive (256-byte x 33-sector, 4/6/8 heads). Images that
 * fail this test but pass ide_accepts() are IDE drives (dev/ide.c, stage 4-5). */
int	sasi_accepts(const hm_disk_t *d);

/* Connect an image to unit 0 or 1 (takes ownership; replaces and closes the old one).  NULL detaches. */
/* Returns -1 (and closes the image) when its geometry cannot be presented as a SASI drive (see sasi.c). */
int	sasi_attach(int unit, hm_disk_t *d);
hm_disk_t *sasi_disk(int unit);
/* Media switch value (0..6; 7 = no drive) reported for a unit through the DIP switch read of 82h. */
int	sasi_media_type(int unit);
/* Called (when INTE is set) at the end of a command; the machine raises IRQ 9 there. May be NULL. */
extern void (*sasi_irq_hook)(void);

/* Machine reset: bus free, control register and sense cleared; the images stay connected. */
void	sasi_machine_reset(void);
/* Bus reset (RST pulse): back to the bus-free phase, interrupt latch cleared.  The sense data survive: the BIOS
 * pulses RST before every command and asks for the sense afterwards. */
void	sasi_bus_reset(void);

/* The port handlers (registered by sasi_bind(); exported for the host tests). */
void	sasi_out80(uint32_t port, uint8_t data);
void	sasi_out82(uint32_t port, uint8_t data);
uint8_t	sasi_in80(uint32_t port);
uint8_t	sasi_in82(uint32_t port);

/* Debug: current phase (SASI_PH_*) and the control register as last written. */
int	sasi_phase(void);

#ifdef __cplusplus
}
#endif
#endif
