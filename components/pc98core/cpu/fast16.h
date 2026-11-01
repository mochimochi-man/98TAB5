/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Real-mode fast path (stage 7c-1b, DESIGN_CPU.md "fast16") for the V30 (menu 0)
 * and 80286 (menu 1) models. The reference interpreter (execute.c) stays the
 * semantic oracle; this module executes the common one-byte opcode set with
 * bit-identical results (registers, all 16 FLAGS bits, memory writes, port
 * traffic, clocks) and hands everything else back untouched. */
#ifndef PC98_FAST16_H
#define PC98_FAST16_H
#include <stdint.h>
#include "cpuadapt.h"

/* Nonzero when the fast path may run on the adapter's current state: menu 0/1,
 * active, not stopped/halted/faulting, TF=0, A->ram set and, for the 286, real
 * mode (PE=0, no shutdown) with plain 64K hidden segment caches that agree with
 * the CPU bases and a conventional IDT. Evaluate after any reference-step or
 * external state change, and whenever TF may have become set. */
int pc98_fast16_eligible(const Pc98Adapter *A);

/* Execute one instruction (a REP string instruction may run several iterations
 * while the accumulated clocks stay below budget; INS/OUTS run one). Returns 1
 * with *clocks set (same value clocks_of() would give, rep_valid/rep_ip updated
 * the same way), or 0 with no state, memory, port or rep_* change when the
 * instruction or situation is not handled. Caller guarantees eligibility. */
int pc98_fast16_step(Pc98Adapter *A, int32_t budget, unsigned *clocks);

/* Batch form used by the adapter: runs instructions back to back until budget clocks are
 * consumed, max_insn is reached, an instruction touched anything outside plain RAM/ROM
 * (port, device memory), TF got set, an IRQ is latched (A->pending), a REP stopped part-way,
 * or an instruction is refused. Returns 1 if at least one instruction ran (*clocks = total).
 * When A->slice_left_p is set, the consumed clocks are subtracted from it incrementally (before
 * every bus access, so devices see the exact g_cpu.slice_left) and the caller must not subtract. */
int pc98_fast16_run(Pc98Adapter *A, int32_t budget, unsigned max_insn, unsigned *clocks);
#endif
