/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "menu_cpu.h"
#include "purge.h"
#include "alu.h"
/* Instruction-boundary external delivery, not an instruction retirement.
 * Intel386 PRM17-69 real entry pushes FLAGS and clears IF/TF only. Upper
 * GPRs, FS/GS and EFLAGS high bits remain untouched. PE/V86/task entry
 * requires a separate386 contract and must not borrow286 task semantics. */
enum Pc98InterruptResult pc98_cpu_menu_interrupt(unsigned menu,Pc98Mode286 *m,Pc98Cpu386State *s,const Pc98ExecuteBus *io,unsigned vector,enum Pc98InterruptKind kind){
 Pc98Cpu16State *c=&s->low;
 if(menu>2||c->model!=(menu?PC98_CPU_286:PC98_CPU_V30)||kind<PC98_INT_IRQ||kind>PC98_INT_NMI||vector>255u||(kind==PC98_INT_NMI&&vector!=2u))return PC98_INT_UNSUPPORTED;
 if(menu==0)return pc98_cpu_interrupt16(c,io,vector,kind);
 if(menu==1)return pc98_cpu_mode_interrupt286(m,c,io,vector,kind);
 if((m->system.msw&1u)||PC98_V86(s))return PC98_INT_UNSUPPORTED;
 if(m->shutdown||c->fault.valid)return PC98_INT_BLOCKED;
 if(kind==PC98_INT_IRQ&&(!(c->flags&PC98_IF)||c->irq_inhibit))return PC98_INT_BLOCKED;
 if(kind==PC98_INT_NMI&&(c->nmi_inhibit||c->nmi_active))return PC98_INT_BLOCKED;
 /* Original386 real low-SP shutdown/wrap is not implemented. Do not
  * approximate it with a286 stack fault or permit partial frame writes. */
 if(c->r[PC98_SP]>0&&c->r[PC98_SP]<6)return PC98_INT_UNSUPPORTED;
 return pc98_cpu_mode_interrupt286(m,c,io,vector,kind);
}
