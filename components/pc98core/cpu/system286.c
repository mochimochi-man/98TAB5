/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "system286.h"
#include "alu.h"
#include <assert.h>
Pc98Table286 pc98_cpu_table_load286(const uint8_t b[6]) {
 Pc98Table286 table;
 table.limit=(uint16_t)(b[0]|((unsigned)b[1]<<8));
 table.base=b[2]|((uint32_t)b[3]<<8)|((uint32_t)b[4]<<16);return table;
}
void pc98_cpu_table_store286(const Pc98Table286 *t,uint8_t b[6]) {
 b[0]=(uint8_t)t->limit;b[1]=(uint8_t)(t->limit>>8);
 b[2]=(uint8_t)t->base;b[3]=(uint8_t)(t->base>>8);b[4]=(uint8_t)(t->base>>16);b[5]=0xff;
}
uint16_t pc98_cpu_lmsw286(uint16_t current,uint16_t source) {
 return (uint16_t)((current&0xfff1u)|(source&15u));
}
uint16_t pc98_cpu_arpl286(uint16_t destination,uint16_t source,uint16_t *flags) {
 unsigned changed=(destination&3u)<(source&3u);
 *flags=(uint16_t)((*flags&~PC98_ZF)|(changed?PC98_ZF:0));
 return changed?(uint16_t)((destination&0xfffcu)|(source&3u)):destination;
}
int pc98_cpu_verify_data286(const Pc98Descriptor286 *d,uint16_t selector,unsigned cpl,int write) {
 assert(cpl<4);
 if(!(selector&0xfffcu)||!d)return 0;
 unsigned ar=d->access,dpl=(ar>>5)&3u;
 if(!(ar&16))return 0;
 if(write) {if((ar&8)||!(ar&2))return 0;}
 else if((ar&8)&&!(ar&2))return 0;
 if((ar&8)&&(ar&4))return 1;
 return cpl<=dpl&&(selector&3u)<=dpl;
}

/* Intel 80286 PRM 11.3, LAR/LSL: presence is deliberately not tested. */
int pc98_cpu_query_descriptor286(const Pc98Descriptor286 *d,uint16_t selector,unsigned cpl,int limit,uint16_t *result) {
 assert(cpl<4);
 if(!d||!(selector&0xfffcu))return 0;
 unsigned ar=d->access,type=ar&15u,dpl=(ar>>5)&3u;
 if(!(ar&16u)&&(type<1u||type>(limit?3u:7u)))return 0;
 if(!((ar&28u)==28u)&&(cpl>dpl||(selector&3u)>dpl))return 0;
 *result=limit?d->limit:(uint16_t)(ar<<8);return 1;
}
