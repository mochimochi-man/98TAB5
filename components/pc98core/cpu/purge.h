/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Stage 13-2: PC98_FULLCPU switch for the CPU purge (set by components/pc98core/CMakeLists.txt). */
#ifndef PC98_CPU_PURGE_H
#define PC98_CPU_PURGE_H
#ifndef PC98_FULLCPU
#define PC98_FULLCPU 1	/* the firmware and the machine builds always set it; a bare unit test gets the full CPU */
#endif
#if PC98_FULLCPU
#define PURGE_STOP(kind) ((void)0)
#define PC98_V86(s) ((s)->flag_high&2u)
#else
extern const char *pc98_purge_reason;
#define PURGE_STOP(kind) (pc98_purge_reason=(kind))
#define PC98_V86(s) 0u /* V86 mode cannot be entered: the VM bit is never set */
#endif
#endif
