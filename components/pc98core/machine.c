/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * PC-9801 machine orchestration. Written from the lifecycle / observable
 * ordering specification in MIGRATION_LOG, stage 2c; CPU and bus stay separate.
 */
#include <compiler.h>
#include <stdio.h>
#include <cpulink.h>
#include "machine.h"
#include <hostfile.h>
#include <host.h>
#include <devices.h>
#include <io.h>
#include <io.h>
#include <video.h>
#include <sound.h>
#include "media/drives.h"
#include "util/pcstr.h"
#include <kbd.h>
#include "esp_timer.h"
#include "cpu/cpuadapt.h"

extern int pc98_rom_load(void);
MachineState g_mstat;
UINT8 g_audio_reopen;		/* the audio settings changed: reopen the stream at the next reset */
UINT g_frames_composed;
int g_pc98_draw_enabled = 1;
typedef void (*MachineStep)(void);
static void run_steps(const MachineStep *steps, unsigned count)
{
    unsigned n;
    for (n = 0; n < count; ++n) steps[n]();
}
#define RUN(table) run_steps(table, sizeof(table) / sizeof((table)[0]))

/* the FM and SSG levels are a percentage of the overall level */
static UINT part_of_all(UINT8 percent)
{
    return (UINT)percent * g_settings.volume_all / 100;
}

static void audio_open(void)
{
    /* without a host audio stream the chips still run, at rate 0 they only keep time */
    const UINT rate = snd_open(g_settings.sample_rate, g_settings.audio_block_ms) == 0 ? g_settings.sample_rate : 0;

    spk_open(rate);
    spk_set_volume(g_settings.beep_level);
    fm_open(rate);
    fm_set_volume(part_of_all(g_settings.volume_fm));
    ssg_open(rate);
    ssg_set_volume(part_of_all(g_settings.volume_ssg));
}
static void audio_close(void)
{
    snd_close();
}
static void select_cpu(void)
{
    g_cpu.v30 = (g_settings.dip_switch[2] & 0x80) ? 1 : 0;
}
static void cpu_open(void) { select_cpu(); pc98_adapt_initialize(); }
static void font_open(void)
{
    cgrom_init();
    cgrom_load(g_settings.font_file);
}
void machine_init(void)
{
    static const MachineStep steps[] = {
        cpu_open, font_open, video_init,
        audio_open
    };
    RUN(steps);
}
static void disks_close(void)
{
    unsigned drive;
    for (drive = 0; drive != DRV_FDD_MAX; ++drive) drv_fd_eject((int)drive);
}
static void cpu_close(void) { cpu_link_ext_resize(0); pc98_adapt_deinitialize(); }
void machine_term(void)
{
    static const MachineStep steps[] = {
        audio_close, disks_close, io_free,
        cpu_close
    };
    RUN(steps);
}
void machine_defaults(void) { config_defaults(&g_settings); }
/* a ROM file: biospath / fname */
void rom_path(char *path, const char *fname, int maxlen)
{
    const size_t n = strlen(g_settings.rom_dir);

    snprintf(path, (size_t)maxlen, "%s%s%s", g_settings.rom_dir, (n && g_settings.rom_dir[n - 1] != '/') ? "/" : "", fname);
}
/* the memory switches live in text VRAM attribute space (A3FE2h + 4n) and the guest may rewrite them, as it may
 * the DIP switch copy through the ports; take both back into the settings so the host can keep them */
void machine_sync_switches(void)
{
    unsigned n;

    for (n = 0; n < sizeof(g_settings.mem_switch); n++) g_settings.mem_switch[n] = mem[0xa3fe2 + n * 4];
    memcpy(g_settings.dip_switch, g_mach.dip_switch, sizeof(g_settings.dip_switch));
}
static void reset_audio_prepare(void)
{
    if (g_audio_reopen) {
        g_audio_reopen = 0;
        audio_close();
        audio_open();
    }
}
static void reset_memory(void)
{
    unsigned n;
    memset(mem, 0, 0xc0000);			/* main RAM and VRAM bank 0 */
    memset(mem + 0xc0000, 0xff, 0xe8000 - 0xc0000);	/* the expansion area reads as nothing fitted */
    /* E0000h-E7FFFh is graphics plane E, not an empty expansion slot. */
    memset(mem + GVRAM_E, 0, 0x8000);
    memset(mem + 0xe8000, 0, 0x110000 - 0xe8000);	/* the ROM area (rom.c fills it) and the HMA */
    memset(mem + GVRAM1_B, 0, 0x18000);		/* graphics VRAM bank 1 */
    memset(mem + GVRAM1_E, 0, 0x8000);
    memset(mem + CGROM_AT, 0, 0x8000);
    for (n = 0; n != 8; ++n) mem[0xa3fe2 + 4 * n] = g_settings.mem_switch[n];
}
static void reset_config(void)
{
    config_derive(&g_mach, &g_settings);
    select_cpu();
}
static void reset_cpu(void)
{
    pc98_adapt_reset();
    cpu_link_ext_resize((UINT32)(g_mach.ext_mb) << 20);
    cgrom_semigraphics();
}
/* the devices on the I/O bus, in the order they are reset and attached */
static void (*const s_dev_reset[])(const Pc98Settings *) = {
    sysctl_reset, cgwin_reset, crtc_reset, dma_reset, gdc_reset, fdc_reset, kbd_reset, pic_reset,
    ppi_reset, rtc_reset, pit_reset, mouse_reset,
    sasi_reset, ide_reset, artic_reset, egc_reset
};
static void (*const s_dev_bind[])(void) = {
    sysctl_bind, cgwin_bind, crtc_bind, dma_bind, gdc_bind, fdc_bind, kbd_bind, pic_bind,
    ppi_bind, rtc_bind, pit_bind, mouse_bind,
    sasi_bind, ide_bind, artic_bind, egc_bind
};
static void reset_io(void)
{
    unsigned n;
    for (n = 0; n < sizeof(s_dev_reset) / sizeof(s_dev_reset[0]); n++) s_dev_reset[n](&g_settings);
    board_reset(&g_settings, g_mach.sound);
}
static void bind_io(void)
{
    bus_set_map(0);
    unsigned n;
    io_clear();
    io_set_waitclock(g_mach.clock_mul);
    for (n = 0; n < sizeof(s_dev_bind) / sizeof(s_dev_bind[0]); n++) s_dev_bind[n]();
    board_bind();
    spk_bind();
}
static void reset_vector(void)
{
    /* Keep the existing ROM loader's failure handling (caller logs it). */
    pc98_rom_load();
    g_cpu.reg.cs_base = 0xf0000;
    g_cpu.reg.cs = 0xf000;
    g_cpu.reg.ip = 0xfff0;
}
void machine_reset(void)
{
    /* PIC must reset before audio IRQs; memory architecture before port bind;
     * VRAM reset follows bind, and ROM follows memory / device initialisation. */
    static const MachineStep steps[] = {
        reset_audio_prepare, reset_memory, reset_config, ev_reset_all,
        reset_cpu, snd_clock_changed,
        snd_reset, spk_reset,
        reset_io, bind_io,
        vram_reset, video_reset, reset_vector,
        keyunit_init
    };
    snd_lock(1);    /* the sound task is kept out while the sound side is rebuilt */
    RUN(steps);
    snd_lock(0);
}
void pccore_postevent(UINT32 event) { (void)event; }
void machine_frame(BOOL draw)
{
    g_mstat.drawframe = (UINT8)draw;
    mouse_frame();
    g_mstat.in_display = 1;
    g_memwait_tram = g_settings.vram_wait[0];
    g_memwait_vram = g_settings.vram_wait[2];
    g_memwait_grcg = g_settings.vram_wait[4];
    gdc_frame_start();
    keyunit_tick();
    while (g_mstat.in_display) {
        pic_deliver();
        if (g_cpu.reset_pending) {
            g_cpu.reset_pending = 0;
            pc98_adapt_shut();
        }
        if (g_cpu.slice_left > 0) {
            if (g_cpu.v30 & 1) pc98_adapt_exec();
            else pc98_adapt_exec();
        }
        ev_slice_done();
    }
    artic_callback();
    drv_fd_tick();
    snd_publish();
    fdc_frame();
    if (g_mstat.hardwarereset) {
        g_mstat.hardwarereset = FALSE;
        machine_sync_switches();
        machine_reset();
    }
}
