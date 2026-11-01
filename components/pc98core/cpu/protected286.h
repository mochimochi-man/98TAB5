/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Host-only protected segment-cache foundation; optional scalar/branch resolver. */
#ifndef PC98_CPU_PROTECTED286_H
#define PC98_CPU_PROTECTED286_H
#include "descriptor286.h"
#include "execute.h"
typedef struct {
 Pc98DescriptorTables286 tables;
 Pc98Descriptor286 cache[4];
 uint8_t valid[4];
} Pc98ProtectedSegments286;
enum Pc98SegmentAccess286 {PC98_SEG_READ,PC98_SEG_WRITE,PC98_SEG_EXECUTE};
/* DS/ES/SS only, 286 only. Return 0 or #NP/#SS/#GP with selector error.
 * Faults leave CPU/cache/memory unchanged. Success marks descriptor accessed.
 * The instruction caller, not this cache helper, installs the SS shadow. */
unsigned pc98_cpu_load_segment286(Pc98Cpu16State *,Pc98ProtectedSegments286 *,
 const Pc98ExecuteBus *,unsigned segment,uint16_t selector,uint16_t *error);
enum {PC98_PROTECTED_SYSTEM_PENDING=256};
/* Direct code-segment JMP only. System descriptors return SYSTEM_PENDING
 * without changes; gate/task routing remains a future instruction-core task. */
unsigned pc98_cpu_jump_code286(Pc98Cpu16State *,Pc98ProtectedSegments286 *,
 const Pc98ExecuteBus *,uint16_t selector,uint16_t offset,uint16_t *error);
/* Check cached validity/type/bounds and form a 24-bit physical address.
 * Returns 0 or #GP/#SS (error code 0); physical is unchanged on failure. */
unsigned pc98_cpu_translate286(const Pc98ProtectedSegments286 *,unsigned segment,
 uint16_t offset,uint32_t size,enum Pc98SegmentAccess286,uint32_t *physical);
/* Bridge for the scalar executor's optional resolver. Context is a pointer
 * to these caches. This reports protected faults; it does not deliver IDT
 * gates, switch PE, or make other instruction families protected-capable. */
int pc98_cpu_resolve_segment286(void *,unsigned segment,uint32_t offset,uint32_t size,
 enum Pc98MemoryAccess16,uint32_t *physical,Pc98CpuFault16 *fault);
/* Executor segment-loader bridge. Context is the mutable cache set.
 * It validates all rights before committing descriptor accessed/CPU/cache. */
int pc98_cpu_load_segment_bridge286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,
 unsigned segment,uint16_t selector,Pc98CpuFault16 *);

int pc98_cpu_jump_far_bridge286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,
 uint16_t selector,uint16_t offset,Pc98CpuFault16 *);

/* Direct code CALL only, same CPL/conforming target. Gates/tasks pending.
 * Checks code rights/presence, full return frame, then target IP before writes. */
unsigned pc98_cpu_call_code286(Pc98Cpu16State *,Pc98ProtectedSegments286 *,
 const Pc98ExecuteBus *,uint16_t selector,uint16_t offset,uint16_t *error);
int pc98_cpu_call_far_bridge286(void *,Pc98Cpu16State *,const Pc98ExecuteBus *,
 uint16_t selector,uint16_t offset,Pc98CpuFault16 *);

#endif
