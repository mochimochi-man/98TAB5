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
volatile long long g_prof_loop, g_prof_cb, g_prof_snd, g_prof_snd_n, g_prof_draw;
/* Host-CPU cycles spent inside pc98_adapt_exec and the emulated clocks it consumed: the same
 * yardstick for the legacy and the new CPU (shown on the PCS line). HLT idle counts as
 * consumed clocks, so compare busy code, not idle. */
volatile unsigned long long g_prof_cpu_cyc, g_prof_cpu_clk, g_prof_cpu_ins;
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
static inline uint32_t prof_cycles(void) { return (uint32_t)esp_cpu_get_cycle_count(); }
static inline uint32_t prof_instret(void) { uint32_t v; __asm__ volatile("csrr %0, 0xb02" : "=r"(v)); return v; }
#else
static inline uint32_t prof_cycles(void) { return 0; }
static inline uint32_t prof_instret(void) { return 0; }
#endif

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
    /* Static timestamps also remain safe across the CPU's nonlocal exits. */
    static long long start, finish, callbacks;
    g_mstat.drawframe = (UINT8)draw;
    mouse_frame();
    start = esp_timer_get_time();
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
            SINT32 rem0 = g_cpu.slice_left;
            uint32_t c0 = prof_cycles(), i0 = prof_instret();
            if (g_cpu.v30 & 1) pc98_adapt_exec();
            else pc98_adapt_exec();
            g_prof_cpu_cyc += prof_cycles() - c0; g_prof_cpu_ins += prof_instret() - i0;
            g_prof_cpu_clk += (uint32_t)(rem0 - g_cpu.slice_left);
        }
        ev_slice_done();
    }
    finish = esp_timer_get_time();
    g_prof_loop += finish - start;
    artic_callback();
    drv_fd_tick();
    callbacks = esp_timer_get_time();
    g_prof_cb += callbacks - finish;
    snd_publish();
    g_prof_snd += esp_timer_get_time() - callbacks;
    ++g_prof_snd_n;
    fdc_frame();
    if (g_mstat.hardwarereset) {
        g_mstat.hardwarereset = FALSE;
        machine_sync_switches();
        machine_reset();
    }
}

/* ---- Stage 14 diagnostic: CPU micro-benchmark (serial command Z) ---- */
#include <string.h>
extern int ets_printf(const char *fmt, ...);

#ifdef __riscv
static inline unsigned zb_csr_ins(void) { unsigned v; __asm__ volatile("csrr %0, 0xb02" : "=r"(v)); return v; }
#else
static inline unsigned zb_csr_ins(void) { return 0; }
#endif

static void set_state(unsigned cs_ip, unsigned es, unsigned ds)
{
#if defined(PC98_NEWCPU) && PC98_NEWCPU >= 1
    Pc98Adapter *A = &g_cpuad;
    Pc98Cpu16State *c = &A->s.low;
    unsigned s;
    c->r[0] = 0x1234; c->r[1] = 0; c->r[2] = 0; c->r[3] = 0x4000; c->r[4] = 0x7000; c->r[5] = 0x4000; c->r[6] = 0x4100; c->r[7] = 0x4200;
    for (s = 0; s < 4; s++) {
        c->seg[s] = (uint16_t)(s == 0 ? es : s == 3 ? ds : 0); c->base[s] = (uint32_t)c->seg[s] << 4;
        if (A->menu != 0) {
            A->mode.segments.cache[s].base = c->base[s]; A->mode.segments.cache[s].limit = 0xffffu;
            A->mode.segments.cache[s].access = s == 1 ? 0x9bu : 0x93u; A->mode.segments.valid[s] = 1;
        }
    }
    c->ip = (uint16_t)cs_ip; c->flags = 0x0002u; c->halted = 0; c->irq_inhibit = c->nmi_inhibit = c->debug_inhibit = 0;
    A->pending = 0; A->rep_valid = 0; A->stopped = 0;
    pc98_adapt_mirror();
#else
    g_cpu.reg.ax = 0x1234; g_cpu.reg.cx = 0; g_cpu.reg.dx = 0; g_cpu.reg.bx = 0x4000; g_cpu.reg.sp = 0x7000; g_cpu.reg.bp = 0x4000; g_cpu.reg.si = 0x4100; g_cpu.reg.di = 0x4200;
    g_cpu.reg.cs = g_cpu.reg.ss = 0; g_cpu.reg.cs_base = g_cpu.reg.ss_base = 0; g_cpu.reg.es = (UINT16)es; g_cpu.reg.es_base = (UINT32)es << 4; g_cpu.reg.ds = (UINT16)ds; g_cpu.reg.ds_base = (UINT32)ds << 4;
    g_cpu.reg.ip = (UINT16)cs_ip; g_cpu.reg.flags = 2;
#endif
}

typedef struct { const char *name; const uint8_t *pat; unsigned len; unsigned es, ds, grcg; } Bt;
#define P(n, ...) static const uint8_t n[] = { __VA_ARGS__ }
P(p_movrr, 0x89, 0xd8);
P(p_movrm, 0x8b, 0x07);
P(p_movmr, 0x89, 0x07);
P(p_movbp, 0x8b, 0x46, 0x04);
P(p_movab, 0xa1, 0x10, 0x40);
P(p_addrr, 0x01, 0xd8);
P(p_addrm, 0x03, 0x07);
P(p_addmr, 0x01, 0x07);
P(p_cmprr, 0x39, 0xd8);
P(p_inc, 0x40);
P(p_pushpop, 0x50, 0x58);
P(p_movbm, 0x8a, 0x07);
P(p_addi, 0x05, 0x34, 0x12);
P(p_addi8, 0x83, 0xc0, 0x01);
P(p_shl1, 0xd1, 0xe0);
P(p_jmp0, 0xeb, 0x00);
P(p_je0, 0x39, 0xc0, 0x74, 0x00);
P(p_movri, 0xb8, 0x34, 0x12);
P(p_dec, 0x48);
P(p_mix, 0x8b, 0x07, 0x40, 0x50, 0x89, 0xd8, 0x83, 0xc0, 0x02, 0x8b, 0x46, 0x04, 0x58, 0x3b, 0x07, 0x74, 0x00, 0x89, 0x46, 0x06, 0x01, 0xd8, 0x8a, 0x07, 0x43, 0x4b);
P(p_rstos, 0xb9, 0x00, 0x02, 0xbf, 0x00, 0x40, 0xf3, 0xab);
P(p_rmovs, 0xb9, 0x00, 0x02, 0xbe, 0x00, 0x50, 0xbf, 0x00, 0x40, 0xf3, 0xa5);
P(p_vstos, 0xb9, 0x00, 0x02, 0x31, 0xff, 0xf3, 0xab);
P(p_vstosb, 0xb9, 0x00, 0x04, 0x31, 0xff, 0xf3, 0xaa);
P(p_vmovs, 0xb9, 0x00, 0x02, 0xbe, 0x00, 0x50, 0x31, 0xff, 0xf3, 0xa5);
P(p_vrd, 0xb9, 0x00, 0x02, 0x31, 0xf6, 0xbf, 0x00, 0x40, 0xf3, 0xa5);
static const Bt tests[] = {
    { "mov ax,bx", p_movrr, sizeof(p_movrr) }, { "mov ax,[bx]", p_movrm, sizeof(p_movrm) }, { "mov [bx],ax", p_movmr, sizeof(p_movmr) },
    { "mov ax,[bp+4]", p_movbp, sizeof(p_movbp) }, { "mov ax,[abs]", p_movab, sizeof(p_movab) }, { "add ax,bx", p_addrr, sizeof(p_addrr) },
    { "add ax,[bx]", p_addrm, sizeof(p_addrm) }, { "add [bx],ax", p_addmr, sizeof(p_addmr) }, { "cmp ax,bx", p_cmprr, sizeof(p_cmprr) },
    { "inc ax", p_inc, sizeof(p_inc) }, { "push/pop ax", p_pushpop, sizeof(p_pushpop) }, { "mov al,[bx]", p_movbm, sizeof(p_movbm) },
    { "add ax,imm16", p_addi, sizeof(p_addi) }, { "add ax,imm8(83)", p_addi8, sizeof(p_addi8) }, { "shl ax,1", p_shl1, sizeof(p_shl1) },
    { "jmp short +0", p_jmp0, sizeof(p_jmp0) }, { "cmp;je +0", p_je0, sizeof(p_je0) }, { "mov ax,imm16", p_movri, sizeof(p_movri) },
    { "dec ax", p_dec, sizeof(p_dec) },
    { "mix", p_mix, sizeof(p_mix) },
    { "rep stosw ram", p_rstos, sizeof(p_rstos), 0, 0, 0 }, { "rep movsw ram", p_rmovs, sizeof(p_rmovs), 0, 0, 0 },
    { "rep stosw vram", p_vstos, sizeof(p_vstos), 0xa800, 0, 0 }, { "rep stosb vram", p_vstosb, sizeof(p_vstosb), 0xa800, 0, 0 },
    { "rep stosw grcg tdw", p_vstos, sizeof(p_vstos), 0xa800, 0, 0x80 }, { "rep movsw ram>vram", p_vmovs, sizeof(p_vmovs), 0xa800, 0, 0 },
    { "rep movsw ram>grcg rmw", p_vmovs, sizeof(p_vmovs), 0xa800, 0, 0xc0 }, { "rep movsw vram>ram", p_vrd, sizeof(p_vrd), 0, 0xa800, 0 },
};

void pc98_cpubench(void)
{
    unsigned t;
    ets_printf("ZBENCH start type=%s\n",
#if defined(PC98_NEWCPU) && PC98_NEWCPU >= 1
        "new"
#else
        "legacy"
#endif
    );
    for (t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
        const Bt *b = &tests[t];
        unsigned pos = 0x8000u, rep;
        unsigned best_cyc = ~0u, best_ins = 0, best_clk = 1;
        for (rep = 0; rep < 3; rep++) {
            unsigned c0, i0, c1, i1;
            int32_t clk0;
            pos = 0x8000u;
            while (pos + b->len + 3u < 0x8000u + 1200u) { memcpy(&mem[pos], b->pat, b->len); pos += b->len; }
            mem[pos] = 0xe9; mem[pos + 1] = (uint8_t)((0x8000u - (pos + 3u)) & 0xffu); mem[pos + 2] = (uint8_t)(((0x8000u - (pos + 3u)) >> 8) & 0xffu);
            if (b->grcg) { io_out8(0x7c, b->grcg); io_out8(0x7e, 0x5a); io_out8(0x7e, 0xa5); io_out8(0x7e, 0xff); io_out8(0x7e, 0x0f); }
            set_state(0x8000u, b->es, b->ds);
            g_cpu.slice_left = 400000;
            clk0 = g_cpu.slice_left;
            c0 = prof_cycles(); i0 = prof_instret();
#if defined(PC98_NEWCPU) && PC98_NEWCPU >= 1
            pc98_adapt_exec();
#else
            if (g_cpu.v30 & 1) pc98_adapt_exec(); else pc98_adapt_exec();
#endif
            c1 = prof_cycles(); i1 = prof_instret();
            if (c1 - c0 < best_cyc) { best_cyc = c1 - c0; best_ins = i1 - i0; best_clk = (unsigned)(clk0 - g_cpu.slice_left); }
        }
        ets_printf("ZB %-18s cpc=%u.%02u hipc=%u.%02u clk=%u\n", b->name, best_cyc / best_clk, best_cyc * 100 / best_clk % 100,
                   best_ins / best_clk, best_ins * 100 / best_clk % 100, best_clk);
    }
    ets_printf("ZBENCH done\n");
}
