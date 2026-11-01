/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Host-only 16-bit fetch and effective-address foundation. Intel 80286 PRM,
 * instruction format and 16-bit addressing tables; NEC V20/V30 User Manual. */
#ifndef PC98_CPU_DECODE_H
#define PC98_CPU_DECODE_H
#include <stdint.h>
enum Pc98Gpr { PC98_AX,PC98_CX,PC98_DX,PC98_BX,PC98_SP,PC98_BP,PC98_SI,PC98_DI };
enum Pc98Seg { PC98_ES,PC98_CS,PC98_SS,PC98_DS };
/* Execution models are not the persisted menu CPU-mode numbers. */
enum Pc98CpuModel { PC98_CPU_286, PC98_CPU_V30 };
enum Pc98MemoryAccess16 {PC98_ACCESS_READ,PC98_ACCESS_WRITE,PC98_ACCESS_FETCH};
/* committed marks a task-switch fault in the incoming architectural context. */
typedef struct {
 uint16_t error,restart_ip;
 uint8_t vector,valid,has_error,committed,external;
} Pc98CpuFault16;
typedef int (*Pc98Resolve16)(void *,unsigned segment,uint32_t offset,uint32_t size,
                            enum Pc98MemoryAccess16,uint32_t *physical,Pc98CpuFault16 *fault);
typedef struct {
 uint16_t r[8],seg[4],ip,flags;
 uint32_t base[4];
 enum Pc98CpuModel model;
 /* Explicit CPL preserves a real-mode CS selector across PE enable.
  * Standalone fixtures without explicit CPL derive it from CS.RPL. */
 uint8_t cpl,cpl_valid;
 /* Instruction-boundary IRQ/NMI inhibition and real-mode delivery state. */
 uint8_t irq_inhibit,nmi_inhibit,nmi_active,halted,debug_inhibit;
 Pc98CpuFault16 fault;
} Pc98Cpu16State;
static inline unsigned pc98_cpu_cpl16(const Pc98Cpu16State *c) {return c->cpl_valid?c->cpl:(c->seg[PC98_CS]&3u);}
static inline void pc98_cpu_set_cpl16(Pc98Cpu16State *c,unsigned level) {if(c->cpl_valid)c->cpl=(uint8_t)level;}

typedef struct {
 void *context;uint8_t (*read8)(void *,uint32_t);
 Pc98Resolve16 resolve;void *resolve_context;
 /* Optional linear instruction cursor prevents protected fetch wrapping
  * past FFFF within one instruction. Plain real-mode fetch still wraps IP. */
 uint32_t *cursor;
 /* Optional execution byte budget. Standalone decoding is unlimited. */
 unsigned *consumed;unsigned maximum;uint8_t length_has_error;
} Pc98FetchBus;
typedef struct {
 unsigned reg,rm,is_register,segment;
 uint16_t offset;
 uint32_t address;
} Pc98Operand16;
uint8_t pc98_cpu_fetch8(Pc98Cpu16State *,const Pc98FetchBus *);
uint16_t pc98_cpu_fetch16(Pc98Cpu16State *,const Pc98FetchBus *);
/* Consumes ModR/M plus displacement. override_segment is -1 or Pc98Seg.
 * Register operands have no memory address. Segment limits/access checks
 * belong to the eventual instruction executor, before memory side effects. */
Pc98Operand16 pc98_cpu_modrm16(Pc98Cpu16State *,const Pc98FetchBus *,int override_segment);
#endif
