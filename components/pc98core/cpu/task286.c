/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "task286.h"
#include "purge.h"
#include <assert.h>
static uint16_t word(const uint8_t *p){return (uint16_t)(p[0]|((unsigned)p[1]<<8));}
#if PC98_FULLCPU
static void put(uint8_t *p,uint16_t value){p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);}
Pc98Tss286 pc98_cpu_parse_tss286(const uint8_t bytes[44]) {
 Pc98Tss286 t={.backlink=word(bytes),.ip=word(bytes+14),.flags=word(bytes+16),.ldt=word(bytes+42)};
 for(unsigned level=0;level<3;level++){t.sp[level]=word(bytes+2+4*level);t.ss[level]=word(bytes+4+4*level);}
 for(unsigned r=0;r<8;r++)t.r[r]=word(bytes+18+2*r);
 for(unsigned s=0;s<4;s++)t.seg[s]=word(bytes+34+2*s);
 return t;
}
void pc98_cpu_save_tss286(uint8_t bytes[44],const Pc98Cpu16State *c) {
 assert(c->model==PC98_CPU_286);put(bytes+14,c->ip);put(bytes+16,(uint16_t)((c->flags&0x7fd5u)|2u));
 for(unsigned r=0;r<8;r++)put(bytes+18+2*r,c->r[r]);
 for(unsigned s=0;s<4;s++)put(bytes+34+2*s,c->seg[s]);
}
#endif
unsigned pc98_cpu_lldt286(Pc98TaskRegisters286 *t,Pc98ProtectedSegments286 *p,const Pc98ExecuteBus *b,uint16_t selector,uint16_t *error) {
 *error=(uint16_t)(selector&0xfffcu);
 if(!(selector&0xfffcu)){t->ldtr=selector;p->tables.ldt=(Pc98Table286){0};p->tables.ldt_valid=0;*error=0;return 0;}
 if(selector&4u)return 13;
 Pc98FetchBus fetch={.context=b->context,.read8=b->read8};Pc98Descriptor286 descriptor;
 unsigned fault=pc98_cpu_lookup286(&fetch,&p->tables,selector,&descriptor,error);if(fault)return fault;
 *error=(uint16_t)(selector&0xfffcu);
 if((descriptor.access&31u)!=2)return 13;
 if(!(descriptor.access&128u))return 11;
 t->ldtr=selector;p->tables.ldt=(Pc98Table286){descriptor.base,descriptor.limit};p->tables.ldt_valid=1;*error=0;return 0;
}
unsigned pc98_cpu_ltr286(Pc98TaskRegisters286 *t,const Pc98ProtectedSegments286 *p,const Pc98ExecuteBus *b,uint16_t selector,uint16_t *error) {
 *error=(uint16_t)(selector&0xfffcu);
 if(!(selector&0xfffcu)||(selector&4u))return 13;
 Pc98FetchBus fetch={.context=b->context,.read8=b->read8};Pc98Descriptor286 descriptor;
 unsigned fault=pc98_cpu_lookup286(&fetch,&p->tables,selector,&descriptor,error);if(fault)return fault;
 *error=(uint16_t)(selector&0xfffcu);
 if((descriptor.access&31u)!=1)return 13;
 if(!(descriptor.access&128u))return 11;
 descriptor.access|=2;b->write8(b->context,(p->tables.gdt.base+(selector&0xfff8u)+5u)&0xffffffu,descriptor.access);
 t->tr=selector;t->tss=descriptor;t->tr_valid=1;*error=0;return 0;
}
unsigned pc98_cpu_tss_stack286(const Pc98TaskRegisters286 *t,const Pc98FetchBus *b,unsigned privilege,uint16_t *sp,uint16_t *ss,uint16_t *error) {
 assert(privilege<3);unsigned offset=2+4*privilege;*error=(uint16_t)(t->tr&0xfffcu);
 if(!t->tr_valid||offset+3>t->tss.limit)return 10;
 uint8_t bytes[4];for(unsigned i=0;i<4;i++)bytes[i]=b->read8(b->context,(t->tss.base+offset+i)&0xffffffu);
 *sp=word(bytes);*ss=word(bytes+2);*error=0;return 0;
}
