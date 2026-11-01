/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_EXECUTE_H
#define PC98_CPU_EXECUTE_H
#include "decode.h"
/* Native16 backend may consume386 prefixes only for this explicit opcode
 * whitelist. Address override never changes implicit SS stack addressing
 * (Intel386 PRM17.1.3). No LOOP/string/ModRM address forms are included. */
struct Pc98ExtraSegmentCache386;
enum Pc98IgnoredPrefix386 {PC98_IGNORE_OPERAND386=1,PC98_IGNORE_ADDRESS386=2,PC98_IGNORE_SEGMENT386=4};
static inline unsigned pc98_cpu_native_prefix_mask386(unsigned op){
 switch(op){
 case 0x90:case 0x9b:case 0x9e:case 0x9f:case 0xe4:case 0xe6:case 0xec:case 0xee:
 case 0xf4:case 0xf5:case 0xf8:case 0xf9:case 0xfa:case 0xfb:case 0xfc:case 0xfd:return 7;
 default:break;
 }
 if((op>=0x50&&op<=0x5f)||(op>=0x70&&op<=0x7f))return 6;
 switch(op){
 case 0x06:case 0x07:case 0x0e:case 0x16:case 0x17:case 0x1e:case 0x1f:
 case 0x27:case 0x2f:case 0x37:case 0x3f:case 0x60:case 0x61:case 0x68:case 0x6a:
 case 0x98:case 0x99:case 0x9a:case 0xc2:case 0xc3:case 0xc8:case 0xc9:case 0xca:case 0xcb:
 case 0xcc:case 0xcd:case 0xce:case 0xd4:case 0xd5:case 0xe5:case 0xe7:case 0xe8:case 0xe9:
 case 0xea:case 0xeb:case 0xed:case 0xef:return 6;
 default:return 0;
 }
}

typedef struct Pc98ExecuteBus {
 void *context;
 uint8_t (*read8)(void *,uint32_t);
 uint16_t (*read16)(void *,uint32_t);
 void (*write8)(void *,uint32_t,uint8_t);
 void (*write16)(void *,uint32_t,uint16_t);
 /* Optional host I/O endpoints. Missing required endpoint is unsupported. */
 uint8_t (*in8)(void *,uint16_t);
 uint16_t (*in16)(void *,uint16_t);
 void (*out8)(void *,uint16_t,uint8_t);
 void (*out16)(void *,uint16_t,uint16_t);
 /* Optional protected preflight for scalar/string memory, stack frames,
  * near control, segment loads, direct far JMP, FLAGS/CLI/STI/HLT and
  * same-CPL INT/IRET via optional atomic hooks. Other far transfers, I/O,
  * privilege transitions and task switching remain unsupported. */
 Pc98Resolve16 resolve;void *resolve_context;
 /* Optional atomic protected segment loader: 1 success, 0 fault.
  * Failure must leave external caches/descriptor memory unchanged.
  * The executor installs the SS inhibition shadow after success. */
 int (*load_segment)(void *,Pc98Cpu16State *,const struct Pc98ExecuteBus *,
                     unsigned segment,uint16_t selector,Pc98CpuFault16 *);
 void *segment_context;
 /* Direct protected far JMP: 1 success, 0 fault, -1 unsupported gate/task.
  * Shares segment_context; failed/pending operations must not commit caches. */
 int (*jump_far)(void *,Pc98Cpu16State *,const struct Pc98ExecuteBus *,
                 uint16_t selector,uint16_t offset,Pc98CpuFault16 *);
 int (*call_far)(void *,Pc98Cpu16State *,const struct Pc98ExecuteBus *,
                 uint16_t selector,uint16_t offset,Pc98CpuFault16 *);
 int (*return_far)(void *,Pc98Cpu16State *,const struct Pc98ExecuteBus *,
                   uint16_t release,Pc98CpuFault16 *);
 void *call_context; /* null uses segment_context for direct-code-only calls */
 void *jump_context; /* null uses segment_context for direct-code-only jumps */
 /* Atomic protected interrupt/IRET hooks: 1 committed, 0 fault,
  * -1 unsupported task/privilege transition. Share interrupt_context. */
 int (*interrupt_gate)(void *,Pc98Cpu16State *,const struct Pc98ExecuteBus *,
                       unsigned vector,int software,int external,int has_error,
                       uint16_t error,Pc98CpuFault16 *);
 int (*return_gate)(void *,Pc98Cpu16State *,const struct Pc98ExecuteBus *,Pc98CpuFault16 *);
 void *interrupt_context;
 /* Optional 286 system instruction endpoint, same atomic 1/0/-1 contract; 2 means WAIT busy, no retirement.
  * opcode is 0F00..03/0F05/0F06 or 63/9B/D8..DF; operand is null for LOADALL/CLTS/WAIT. */
 int (*system_instruction)(void *,Pc98Cpu16State *,const struct Pc98ExecuteBus *,
                           unsigned opcode,const Pc98Operand16 *,Pc98CpuFault16 *);
 void *system_context;
 /* A resolver can also enforce real-mode hidden caches after LOADALL. */
 uint8_t real_mode;
 /* Optional retirement tracing. A task commit suppresses the outgoing TF. */
 uint8_t single_step;unsigned *task_committed;
 /* Optional full-width386 port transactions. Never approximate one dword
  * transfer with repeated16-bit callbacks; device/adapter defines the cycle. */
 uint32_t (*in32)(void *,uint16_t);
 void (*out32)(void *,uint16_t,uint32_t);
 /* Optional instruction-only endpoint for mode dispatch/replay. Resolver
  * preflight still precedes every fetch. Data reads always use context/read8;
  * a null fetch8 preserves the original unified bus contract. */
 void *fetch_context;
 uint8_t (*fetch8)(void *,uint32_t);
 /* Backend dispatch contract: zero maximum keeps native generation limit.
  * Optional completion output is cleared on every entry, including blocked
  * calls, and set only after a whole instruction retires (including TRAP).
  * REP partial iterations, faults, WAIT busy and unsupported return zero. */
 uint8_t instruction_maximum;
 unsigned *instruction_completed;
 /* Opt-in native backend consumption of only operand/address-unused386
  * prefixes. The backend validates actual prefixes against opcode whitelist.
  * Zero preserves strict286/V30 generation behavior. */
 uint8_t ignored_prefixes386;
 /* Optional atomic protected FS/GS selector loader, same1/0/-1 success/
  * fault/unsupported contract. Complete output cache only on success;
  * failure leaves output, descriptors and external caches unchanged.
  * The core captures the whole selector source before calling this hook. */
 int (*load_extra_segment)(void *,Pc98Cpu16State *,const struct Pc98ExecuteBus *,
                           uint16_t,struct Pc98ExtraSegmentCache386 *,Pc98CpuFault16 *);
 void *extra_segment_context;
 /* Optional non-failing atomic bus window:1 acquire,0 release. Memory
  * XCHG requires this endpoint. Whole WRITE preflight precedes acquire;
  * all operand reads/writes and register commit precede release. Fetch,
  * faults and unsupported forms never acquire. Context is independent
  * of memory/fetch hooks. Caller defers DMA while acquired. */
 void (*lock_bus)(void *,unsigned);void *lock_context;
 /* Machine without a numeric coprocessor (PC-9801 with an empty 8087/287
  * socket): ESC decodes its ModR/M without operand access and WAIT never
  * waits (the PC-98 leaves the coprocessor's BUSY input inactive). The 286 MSW EM/TS #NM checks still come
  * first. Zero keeps ESC unsupported (coprocessor required). */
 uint8_t npx_absent;
 /* Optional 386 control-register endpoint for MOV r32,CRn / MOV CRn,r32 (0F 20/22):
  * write=0 reads *value, write=1 consumes it. 1 done, 0 fault (failure filled), -1 unsupported
  * (CR2..CR4, paging, PE clear ...). Without it the forms stay unsupported. */
 int (*control_register386)(void *,Pc98Cpu16State *,unsigned write,unsigned number,uint32_t *value,Pc98CpuFault16 *);
 void *control_context;
} Pc98ExecuteBus;
enum Pc98StepResult { PC98_STEP_OK,PC98_STEP_UNSUPPORTED,PC98_STEP_HALTED,PC98_STEP_FAULT,PC98_STEP_SHUTDOWN,PC98_STEP_TRAP,PC98_STEP_WAITING };
/* Host-only 16-bit prototype with optional partial protected hooks. Includes basic data/ALU, branches and
 * stack frames, shifts, multiply/divide, strings and port I/O. REP executes
 * one iteration per call and restarts at its prefixes while more remain.
 * Real-mode INT/IRET, explicit IRQ/NMI and 286 divide faults are supported;
 * full exception priority/double faults, TF traps, timing and protected transitions remain incomplete.
 * Boundary cases requiring faults return UNSUPPORTED before writes.
 * 286 instruction length is limited to 10 bytes; V30 accepts up to 14
 * segment/REP prefix bytes pending its generation-specific length model.
 * Not linked into the firmware. */
/* TRAP means the instruction committed before queuing a debug event.
 * FAULT means a restartable fault, except explicitly committed task faults. */
enum Pc98StepResult pc98_cpu_step16(Pc98Cpu16State *,const Pc98ExecuteBus *);
enum Pc98InterruptKind { PC98_INT_SOFTWARE,PC98_INT_IRQ,PC98_INT_NMI };
enum Pc98InterruptResult { PC98_INT_DELIVERED,PC98_INT_BLOCKED,PC98_INT_UNSUPPORTED,PC98_INT_FAULT };
/* Explicit 286/V30-native real-mode delivery at a caller-selected instruction boundary.
 * A blocked request is not latched; the future scheduler retains it. */
enum Pc98InterruptResult pc98_cpu_interrupt16(Pc98Cpu16State *,const Pc98ExecuteBus *,
                                            unsigned vector,enum Pc98InterruptKind);
#endif
