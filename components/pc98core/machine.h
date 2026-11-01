/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#pragma once
/* Machine / CPU contract for stage 7.
 * The CPU is cpu/ (adapter cpuadapt.c) and shares its state through cpulink.h.
 * INITIALIZE/DEINITIALIZE own instruction tables; RESET preserves g_cpu.v30 and
 * extended allocation, resets registers/clocks/A20. SHUT is the F0h CPU-only
 * reset: devices and pending machine events must survive it.
 * EXEC / EXECV30 consume REMCLOCK; they may overshoot or leave it negative on
 * HLT. BASECLOCK is the scheduled slice, CLOCK the completed time; only the
 * scheduler commits BASECLOCK and dispatches events. IRQ is offered by pic_irq
 * at each slice boundary (pc98_adapt_interrupt(vector), subject to IF/inhibit).
 * A20EN / ADRSMASK apply to bus accesses; g_cpu.reg.cs_base, CS, IP and CLEARPREFETCH
 * establish the ROM reset vector. SETEXTSIZE takes MB, retains contents at the
 * same size, frees on zero; V30 has none. EMSPTR[4] address the C0000h windows;
 * standard VX has no EMS controller. mem[] / memory dispatch remain stage 7.
 * g_cpu.reset_pending must be cleared before SHUT, without resetting the scheduler.
 * main owns wall-clock pacing; one machine_frame call ends at display completion.
 */
#include <pc98.h>
#ifdef __cplusplus
extern "C" {
#endif
extern int g_pc98_draw_enabled;
extern volatile long long g_prof_loop, g_prof_cb, g_prof_snd;
extern volatile long long g_prof_snd_n, g_prof_draw;
#ifdef __cplusplus
}
#endif
