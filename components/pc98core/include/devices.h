/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * devices.h - the devices on the I/O bus (dev/, video/, sound/) and what they offer each other. */
#ifndef PC98_DEVICES_H
#define PC98_DEVICES_H

#include "compiler.h"
#include "cpulink.h"
#include "pc98.h"
#include "events.h"
#include "io.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Pc98Settings;

/* ---- pic.c: the two i8259A interrupt controllers ---- */

void	pic_raise(REG8 irq);		/* a request on line irq (0-15): latched in IRR */
void	pic_lower(REG8 irq);		/* the request is taken away */
int	pic_pending(void);		/* an unmasked request is latched */
void	pic_deliver(void);		/* the CPU takes the highest deliverable interrupt (interrupts enabled) */
UINT8	pic_isr(UINT chip);
void	pic_reset(const struct Pc98Settings *pConfig);
void	pic_bind(void);


/* ---- pit.c: the i8253 interval timer ---- */
void	pit_reset(const struct Pc98Settings *pConfig);
void	pit_bind(void);


/* ---- artic.c: free-running counter 5Ch-5Fh ---- */
void	artic_callback(void);
REG16	artic_r16(UINT port);
void	artic_reset(const struct Pc98Settings *pConfig);
void	artic_bind(void);

/* ---- cgwin.c: character generator ports A1h-A9h and the CG window ---- */
#define CGWIN_WRITABLE	0x01		/* a user-defined character is selected: the window can be written */
#define CGWIN_WRITTEN	0x80		/* a user-defined character changed (the text screen redraws) */
typedef struct {
	UINT32	even;			/* fontrom offset shown at even window addresses */
	UINT32	odd;			/* ... at odd addresses */
	UINT8	flags;
} CgWindow;
extern CgWindow g_cgwin;
void	cgwin_reset(const struct Pc98Settings *pConfig);
void	cgwin_bind(void);

/* ---- sysctl.c: NMI mask, CPU reset / A20, NEC ROM bank ---- */
int	sysctl_nmi_open(void);
void	sysctl_reset(const struct Pc98Settings *pConfig);
void	sysctl_bind(void);

/* ---- ppi.c: the system and printer 8255s (serial and printer ports with nothing connected) ---- */
UINT8	ppi_portc(void);		/* system port C (bit 3 = buzzer off) */
void	ppi_reset(const struct Pc98Settings *pConfig);
void	ppi_bind(void);

/* ---- rtc.c: the uPD4990A calendar clock (20h) ---- */
UINT8	rtc_dout(void);
void	rtc_reset(const struct Pc98Settings *pConfig);
void	rtc_bind(void);

/* ---- mouse.c: the bus mouse interface (8255 at 7FD9h-7FDFh, rate BFDBh, IRQ13) ---- */
void	mouse_reset(const struct Pc98Settings *pConfig);
void	mouse_bind(void);
void	mouse_frame(void);		/* once per frame: take the host's motion */
void	mouse_changeclock(void);

/* ---- dma8237.c: the i8237 DMA controller and its page registers ---- */
typedef struct {
	void	(*to_dev)(REG8 data);		/* memory -> device */
	REG8	(*from_dev)(void);		/* device -> memory (also a verify cycle) */
	void	(*tc)(void);			/* terminal count */
} DmaDevice;
extern UINT8 g_dma_active;			/* channels transferring: the CPU loop calls dma_cycle() */
void	dma_connect(UINT ch, const DmaDevice *dev);
void	dma_request(UINT ch, int on);	/* the device's DREQ line */
void	dma_update(void);			/* re-evaluate which channels run */
void	dma_cycle(void);			/* one byte on every running channel */
int	dma_burst_byte(UINT ch);		/* one byte on ch now; 0 = the channel would not take it */
UINT32	dma_address(UINT ch);
UINT16	dma_count(UINT ch);
UINT8	dma_mode(UINT ch);
void	dma_reset(const struct Pc98Settings *pConfig);
void	dma_bind(void);

/* ---- fdc765.c: the uPD765A floppy controller ---- */
extern UINT8 fdc_waiting;		/* a command waits for a track that is still being read */
void	fdc_reset(const struct Pc98Settings *pConfig);
void	fdc_bind(void);
void	fdc_frame(void);		/* once per frame */
void	fdc_resume(void);		/* try the waiting command again */
void	fdc_drive_changed(int drive, int ready);	/* a drive's ready line changed */

/* ---- sasi_bind.c: the SASI and IDE hard disk interfaces ---- */
void	sasi_reset(const struct Pc98Settings *pConfig);
void	sasi_bind(void);
void	ide_reset(const struct Pc98Settings *pConfig);
void	ide_bind(void);


#ifdef __cplusplus
}
#endif

#include "video.h"
#include "sound.h"

#endif
