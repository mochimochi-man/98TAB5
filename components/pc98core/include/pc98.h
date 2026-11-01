/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 *
 * pc98.h - the machine as a whole: the configuration the host sets (g_settings, config.c), the description a reset derives
 * from it (g_mach), the run state, and the lifecycle (machine.c). */
#ifndef PC98_PC98_H
#define PC98_PC98_H

#include "compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the base clock of the timers: 2.4576 MHz (5 / 10 MHz machines) or 1.9968 MHz (8 MHz machines) */
#define CLK_BASE5	2457600
#define CLK_BASE8	1996800

#define MODEL_VM	0
#define MODEL_VX	1

typedef enum {
	SOUND_NONE	= 0,
	SOUND_26K	= 1		/* PC-9801-26K */
} SoundBoard;

#define CFG_PATH_MAX	256

/* what the host chooses (main/ sets it after machine_defaults()) */
typedef struct Pc98Settings {
	/* display */
	UINT8	draw_at_retrace;	/* render at the start of the vertical retrace (else at its end) */
	UINT8	has_16color;		/* the 16-colour board is fitted */
	UINT8	gfx_chip;		/* 0 none, 1 GRCG, 2 GRCG with CG window, 3 EGC */
	/* keyboard */
	UINT8	key_repeat;
	UINT16	key_repeat_delay_ms;
	UINT16	key_repeat_rate_ms;
	/* machine */
	char	model[8];		/* "VX" or "VM" */
	UINT	base_clock;		/* CLK_BASE5 or CLK_BASE8 */
	UINT	clock_mul;		/* CPU clock = base clock x clock_mul */
	UINT8	dip_switch[3];
	UINT8	mem_switch[8];		/* copied to A3FE2h + 4n at reset */
	UINT8	ext_mb;			/* extended memory (at most 13, none on a V30) */
	UINT8	vram_wait[6];		/* CPU clocks per access of text VRAM / graphics VRAM / GRCG-EGC, in display and in retrace */
	/* sound */
	UINT32	sample_rate;
	UINT16	audio_block_ms;		/* length of the host's audio block */
	UINT8	sound_board;		/* SoundBoard */
	UINT8	board26k_jumpers;
	UINT8	beep_level;		/* 0..3 */
	UINT8	volume_all;		/* % */
	UINT8	volume_fm;		/* % of volume_all */
	UINT8	volume_ssg;
	/* drives and files */
	UINT8	fdd_fitted;		/* bit per drive */
	UINT8	fdd_144;		/* the 1.44 MB interface */
	char	fdd_image[4][CFG_PATH_MAX];
	char	hdd_image[2][CFG_PATH_MAX];
	char	font_file[CFG_PATH_MAX];
	char	rom_dir[CFG_PATH_MAX];
} Pc98Settings;

/* what a reset makes of the settings */
typedef struct {
	UINT32	base_clock;
	UINT	clock_mul;
	UINT32	cpu_clock;		/* base_clock x clock_mul */
	UINT8	clk8mhz;		/* the 8 MHz machine (1.9968 MHz base clock) */
	UINT8	model;
	UINT8	ext_mb;
	UINT8	dip_switch[3];		/* the DIP switches; the ports may change this copy */
	UINT8	rom_bios;		/* a BIOS ROM image was loaded (rom.c) */
	SoundBoard	sound;
} MachineDesc;

/* what the emulation loop and the renderer share */
typedef struct {
	UINT8	in_display;		/* 1 while the current frame's display period and retrace run */
	UINT8	drawframe;		/* this frame is rendered */
	UINT8	hardwarereset;		/* a reset was requested */
} MachineState;

extern Pc98Settings g_settings;
extern MachineDesc g_mach;
extern MachineState g_mstat;
extern int g_pc98_draw_enabled;		/* main/: render frames (off while booting) */
extern UINT g_frames_composed;			/* frames composited (diagnostics) */

void	config_defaults(Pc98Settings *cfg);
void	config_derive(MachineDesc *m, const Pc98Settings *cfg);

void	machine_init(void);
void	machine_term(void);
void	machine_defaults(void);
void	machine_reset(void);
void	machine_sync_switches(void);	/* the guest changed DIP / memory switches: copy them back to g_settings */
void	machine_frame(BOOL draw);	/* run one frame */
void	rom_path(char *path, const char *fname, int maxlen);

/* host hooks (main/platform_stubs.cpp) */
void	host_fdd_access(UINT8 drv);
void	host_hdd_access(UINT8 drv);

#ifdef __cplusplus
}
#endif

#endif
