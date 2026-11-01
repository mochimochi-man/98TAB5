/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "decode.h"
#include <assert.h>
uint8_t pc98_cpu_fetch8(Pc98Cpu16State *cpu,const Pc98FetchBus *bus) {
 if(cpu->fault.valid)return 0;
 if(bus->maximum&&bus->consumed&&*bus->consumed>=bus->maximum){cpu->fault=(Pc98CpuFault16){.valid=1,.vector=13,.has_error=bus->length_has_error};return 0;}
 uint32_t address=cpu->base[PC98_CS]+cpu->ip;
 if(bus->resolve) {
  Pc98CpuFault16 fault={0};uint32_t offset=bus->cursor?*bus->cursor:cpu->ip;
  if(!bus->resolve(bus->resolve_context,PC98_CS,offset,1,PC98_ACCESS_FETCH,&address,&fault)) {
   fault.valid=1;cpu->fault=fault;return 0;
  }
 }
 uint8_t v=bus->read8(bus->context,address);
 if(bus->consumed)(*bus->consumed)++;
 cpu->ip=(uint16_t)(cpu->ip+1);if(bus->cursor)(*bus->cursor)++;return v;
}
uint16_t pc98_cpu_fetch16(Pc98Cpu16State *cpu,const Pc98FetchBus *bus) {
 uint16_t lo=pc98_cpu_fetch8(cpu,bus);
 return (uint16_t)(lo|((uint16_t)pc98_cpu_fetch8(cpu,bus)<<8));
}
Pc98Operand16 pc98_cpu_modrm16(Pc98Cpu16State *cpu,const Pc98FetchBus *bus,int override_segment) {
 assert(override_segment>=-1 && override_segment<=PC98_DS);
 uint8_t byte=pc98_cpu_fetch8(cpu,bus);unsigned mod=byte>>6;
 Pc98Operand16 out={.reg=(byte>>3)&7,.rm=byte&7,.is_register=mod==3,.segment=PC98_DS,.offset=0,.address=0};
 if(out.is_register)return out;
 uint32_t off;
 switch(out.rm) {
 case 0:off=cpu->r[PC98_BX]+cpu->r[PC98_SI];break;
 case 1:off=cpu->r[PC98_BX]+cpu->r[PC98_DI];break;
 case 2:off=cpu->r[PC98_BP]+cpu->r[PC98_SI];out.segment=PC98_SS;break;
 case 3:off=cpu->r[PC98_BP]+cpu->r[PC98_DI];out.segment=PC98_SS;break;
 case 4:off=cpu->r[PC98_SI];break;
 case 5:off=cpu->r[PC98_DI];break;
 case 6:
  if(!mod)off=pc98_cpu_fetch16(cpu,bus);
  else { off=cpu->r[PC98_BP];out.segment=PC98_SS; }
  break;
 default:off=cpu->r[PC98_BX];break;
 }
 if(mod==1) { uint8_t d=pc98_cpu_fetch8(cpu,bus);off+=d<128?d:(int)d-256; }
 else if(mod==2)off+=pc98_cpu_fetch16(cpu,bus);
 if(override_segment>=0)out.segment=(unsigned)override_segment;
 out.offset=(uint16_t)off;out.address=cpu->base[out.segment]+out.offset;
 return out;
}
