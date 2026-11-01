/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#ifndef PC98_CPU_EXECUTE386_H
#define PC98_CPU_EXECUTE386_H
#include "execute.h"
/* flag_high is EFLAGS31..16 (g_cpu.x386.flags_hi in cpulink.h is a diagnostic copy). */
typedef struct Pc98ExtraSegmentCache386 {
 uint32_t base;uint16_t selector,limit;uint8_t access,initialized,valid;
} Pc98ExtraSegmentCache386;
typedef struct Pc98Cpu386State {Pc98Cpu16State low;uint16_t high[8],flag_high;Pc98ExtraSegmentCache386 extra[2];} Pc98Cpu386State;
/* Explicit FS/GS reset defaults. Leaves low CPU/highGPR/EFLAGS untouched.
 * A zero aggregate has uninitialized extra caches until reset or MOV load. */
void pc98_cpu_extra_segments_reset386(Pc98Cpu386State *);
/* Real MOV FS/GS,r/m16 and MOV r/m16,FS/GS are supported; initialized FS/GS caches
 * provide memory preflight for supported scalar/string/IO forms. Optional protected
 * FS/GS selector hooks support the existing286 descriptor/cache contract;
 * full386 descriptors/tasks remain pending.
 * Operand32 segment-store highword contract remains explicitly unsupported.
 * Partial 386 extension endpoint for 16-bit code: register and DS/ES/CS/SS memory BSF/BSR,
 * BT/BTS/BTR/BTC and SHLD/SHRD, plus 8/16/32-bit register/memory/immediate/moffs MOV, integer ALU8/16/32 and TEST,
 * CWDE/CDQ, register XCHG and memory XCHG8/16/32 with optional atomic bus hook, byte/word/dword MOVS/CMPS/STOS/LODS/SCAS, TEST/NOT/NEG and MUL/IMUL/DIV/IDIV8/16/32, shift/rotate8/16/32, LEA16/32, MOVSX/MOVZX,
 * IN/OUT EAX (real mode or CPL<=IOPL), SETcc, near Jcc, near JMP/CALL/RET32, PUSH32, POP32, PUSHAD/POPAD, PUSHF/POPF16/32 and real IRET16, plus INC/DEC with 66 operand override and a 15-byte
 * fetch limit. Segment/67 prefixes are consumed for register operands.
 * Stack operations use the existing 16-bit stack-address attribute. Memory
 * POP destinations with an explicit ESP base use updated ESP per Intel
 * B1 DesignNote2. POP ESP writes its captured value after the increment.
 * Near CALL/RET32 use the existing 16-bit stack address attribute. Real
 * PUSH/CALL stack-underflow shutdown is unsupported before frame access.
 * IN/OUT require explicit32-bit callbacks. Lower-privilege TSS bitmap IO,
 * V86 IO remain unsupported before device access. INS/OUTS byte/word/dword preflight
 * the complete memory operand before one matching-width callback and follow the same
 * one-iteration REP boundary contract as the other string operations.
 * LFS/LGS16/32 and LES/LDS/LSS32 load GPR pointers and plain-real caches, or
 * borrow an atomic loader for existing286 real/protected hidden caches. LSS
 * adds no MOV/POP SS shadow per Intel1986 PRM3.10.3; cached loaders without an atomic hook and V86 full-pointer loads remain
 * unsupported before source accesses. This is not a full386 descriptor model.
 * Far JMP/CALL/RET32 support plain real bus or an atomic real CS reload
 * hook, with paddedCS/EIP dword frames. Protected/V86 far adapters and real low-SP call
 * shutdown/wrap variants remain unsupported before operand accesses.
 * Memory XCHG requires the paired lock_bus hook around read/write/register
 * commit, after full WRITE preflight. Explicit LOCK prefix remains pending.
 * Group2 masked-zero/full-ring
 * memory uses compatibility RMW; SHLD/SHRD masked-zero reads without write.
 * Both preflight WRITE rights. Physical bus timing is not certified.
 * Bit address-size wrap, full386 descriptors/tasks and other32-bit operations
 * remain unsupported; firmware integration is pending. The existing 24-bit bus/resolver contract
 * is retained; this is not a full 386 protected-mode implementation.
 * REP strings commit one iteration per host step, retaining restart IP and
 * shadows/RF until completion. Multi-iteration TF and F2 noncomparison
 * strings remain unsupported; host IRQ/boundary adapters exist, but timing/DMA
 * and firmware scheduler integration remain pending.
 * PUSHFD/POPFD reject unsupported V86 state before frame access. RF clears
 * after successful instructions except POPFD; fault/unsupported retains RF.
 * POPFD preserves RF/VM per PRM3.8/17-130 (contradictory Chapter12 generic
 * POPF sentence retained in the validation notes).
 * Real IRET16 restores NT/IOPL and clears RF per Intel B1; IRETD32 and
 * protected IRET16 remain unsupported. This never falls through to a 286 step.
 * Unsupported operations restore CPU state; source/CL aliases are captured
 * before writeback. Uses the bus retirement tracing contract. Memory access
 * faults are returned pending, including real-mode bounds without a resolver. */
enum Pc98StepResult pc98_cpu_extension386(Pc98Cpu386State *,const Pc98ExecuteBus *);
#endif
