/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Instruction encodings/semantics: Intel 80286 PRM instruction set reference;
 * NEC V20/V30 User Manual common 8086 arithmetic/data movement instructions. */
#include "execute.h"
#include "purge.h"
#include "alu.h"
static uint16_t regread(const Pc98Cpu16State *c,unsigned r,unsigned bits) {
 return (uint16_t)(bits==16?c->r[r]:(c->r[r&3]>>(r<4?0:8))&255);
}
static void regwrite(Pc98Cpu16State *c,unsigned r,unsigned bits,uint16_t v) {
 if(bits==16)c->r[r]=v;
 else { unsigned shift=r<4?0:8,index=r&3;uint16_t mask=(uint16_t)(255u<<shift);
  c->r[index]=(uint16_t)((c->r[index]&~mask)|((v&255u)<<shift)); }
}
static int resolve_memory(Pc98Cpu16State *c,const Pc98ExecuteBus *b,unsigned segment,uint16_t offset,unsigned bits,enum Pc98MemoryAccess16 access,uint32_t *address) {
 if(c->fault.valid)return 0;
 *address=c->base[segment]+offset;
 if(b->resolve) {
  Pc98CpuFault16 fault={0};
  if(!b->resolve(b->resolve_context,segment,offset,bits/8,access,address,&fault)) {
   fault.valid=1;c->fault=fault;return 0;
  }
 }
 return 1;
}
static uint16_t read_memory(Pc98Cpu16State *c,const Pc98ExecuteBus *b,unsigned segment,uint16_t offset,unsigned bits) {
 uint32_t address;if(!resolve_memory(c,b,segment,offset,bits,PC98_ACCESS_READ,&address))return 0;
 return bits==8?b->read8(b->context,address):b->read16(b->context,address);
}
static uint16_t operand_read(Pc98Cpu16State *c,const Pc98ExecuteBus *b,const Pc98Operand16 *o,unsigned bits) {
 if(c->fault.valid)return 0;
 if(o->is_register)return regread(c,o->rm,bits);
 return read_memory(c,b,o->segment,o->offset,bits);
}
static uint16_t operand_rmw(Pc98Cpu16State *c,const Pc98ExecuteBus *b,const Pc98Operand16 *o,unsigned bits) {
 uint32_t ignored;
 if(!o->is_register&&!resolve_memory(c,b,o->segment,o->offset,bits,PC98_ACCESS_WRITE,&ignored))return 0;
 return operand_read(c,b,o,bits);
}
static void operand_write(Pc98Cpu16State *c,const Pc98ExecuteBus *b,const Pc98Operand16 *o,unsigned bits,uint16_t v) {
 if(c->fault.valid)return;
 if(o->is_register)regwrite(c,o->rm,bits,v);
 else {
  uint32_t address;if(!resolve_memory(c,b,o->segment,o->offset,bits,PC98_ACCESS_WRITE,&address))return;
  if(bits==8)b->write8(b->context,address,(uint8_t)v);else b->write16(b->context,address,v);
 }
}
static uint16_t immediate(Pc98Cpu16State *c,const Pc98FetchBus *b,unsigned bits) {
 return bits==8?pc98_cpu_fetch8(c,b):pc98_cpu_fetch16(c,b);
}
/* Values are captured before changing SP, including operands addressed through SS.
 * The V30 PUSH SP source-value adjustment is applied by the caller. With a
 * resolver, stack faults are reported before SP changes or memory access.
 * The default real backend still rejects words crossing FFFFh. */
static int push16(Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t value) {
 if(!b->resolve&&c->r[PC98_SP]==1)return 0;
 uint16_t next=(uint16_t)(c->r[PC98_SP]-2);uint32_t address;
 if(!resolve_memory(c,b,PC98_SS,next,16,PC98_ACCESS_WRITE,&address))return 0;
 c->r[PC98_SP]=next;b->write16(b->context,address,value);return 1;
}
static int pop16(Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t *value) {
 if(!b->resolve&&c->r[PC98_SP]==0xffff)return 0;
 uint32_t address;
 if(!resolve_memory(c,b,PC98_SS,c->r[PC98_SP],16,PC98_ACCESS_READ,&address))return 0;
 *value=b->read16(b->context,address);c->r[PC98_SP]=(uint16_t)(c->r[PC98_SP]+2);return 1;
}
/* Validate the entire frame before its first write. */
static int stack_words(Pc98Cpu16State *c,const Pc98ExecuteBus *b,unsigned count,int push) {
 uint16_t sp=c->r[PC98_SP];
 if(b->resolve) {
  uint32_t ignored;uint16_t start=push?(uint16_t)(sp-2*count):sp;
  return resolve_memory(c,b,PC98_SS,start,count*16,push?PC98_ACCESS_WRITE:PC98_ACCESS_READ,&ignored);
 }
 for(unsigned n=0;n<count;n++) {
  if(push)sp=(uint16_t)(sp-2);
  if(sp==0xffff)return 0;
  if(!push)sp=(uint16_t)(sp+2);
 }
 return 1;
}
/* A taken near transfer checks its destination before committing IP. The
 * resolver validates execute permission/bounds without fetching target data. */
static void near_target(Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t target) {
 uint32_t ignored;
 if(resolve_memory(c,b,PC98_CS,target,8,PC98_ACCESS_FETCH,&ignored))c->ip=target;
}
static int check_push16(Pc98Cpu16State *c,const Pc98ExecuteBus *b) {
 uint32_t ignored;
 return resolve_memory(c,b,PC98_SS,(uint16_t)(c->r[PC98_SP]-2),16,PC98_ACCESS_WRITE,&ignored);
}
static int near_call(Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t target) {
 uint32_t ignored;
 if(b->resolve&&(!check_push16(c,b)||!resolve_memory(c,b,PC98_CS,target,8,PC98_ACCESS_FETCH,&ignored)))return 0;
 if(!push16(c,b,c->ip))return 0;
 c->ip=target;return 1;
}
static void load_segment(Pc98Cpu16State *c,unsigned segment,uint16_t value) {
 c->seg[segment]=value;c->base[segment]=(uint32_t)value<<4;
}
static int load_data_segment(Pc98Cpu16State *c,const Pc98ExecuteBus *b,unsigned segment,uint16_t selector) {
 if(c->fault.valid)return 0;
 if(c->fault.valid)return 0;
 if(!b->resolve) {load_segment(c,segment,selector);return 1;}
 if(!b->load_segment)return 0;
 Pc98CpuFault16 fault={0};
 if(!b->load_segment(b->segment_context,c,b,segment,selector,&fault)) {fault.valid=1;c->fault=fault;return 0;}
 return 1;
}
static int far_transfer(Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint16_t selector,uint16_t offset,int call) {
 if(c->fault.valid)return 0;
 Pc98CpuFault16 fault={0};int result;
 if(call){if(!b->call_far)return 0;result=b->call_far(b->call_context?b->call_context:b->segment_context,c,b,selector,offset,&fault);}
 else{if(!b->jump_far)return 0;result=b->jump_far(b->jump_context?b->jump_context:b->segment_context,c,b,selector,offset,&fault);}
 if(result<0)return 0;
 if(!result){fault.valid=1;c->fault=fault;return 0;}
 return 1;
}
#if !PC98_FULLCPU
const char *pc98_purge_reason;
#endif
/* V30 is native-only here: MD is implicit in the model. Undefined PSW
 * bits 14..12 export as ones for the observed PC-98 compatibility contract.
 * 8080 emulation and native calls from that mode need additional state. */
static uint16_t export_flags(const Pc98Cpu16State *c) {
 return (uint16_t)((c->flags&0xfd5u)|2u|(c->model==PC98_CPU_V30?0xf000u:c->flags&0x7000u));
}
static uint16_t import_flags(const Pc98Cpu16State *c,uint16_t value,int iret) {
 if(c->model==PC98_CPU_V30)return (uint16_t)((value&0xfd5u)|2u);
 return (uint16_t)((value&0xfd5u)|2u|(iret?0u:c->flags&0x7000u));
}
static int interrupt_frame(Pc98Cpu16State *c,const Pc98ExecuteBus *b,unsigned vector) {
 if(vector>255 || !stack_words(c,b,3,1))return 0;
 (void)push16(c,b,export_flags(c));
 (void)push16(c,b,c->seg[PC98_CS]);(void)push16(c,b,c->ip);
 c->flags&=(uint16_t)~(PC98_IF|PC98_TF);
 uint16_t target=b->read16(b->context,vector*4),selector=b->read16(b->context,vector*4+2);
 load_segment(c,PC98_CS,selector);c->ip=target;
 c->irq_inhibit=c->nmi_inhibit=c->halted=0;return 1;
}
/* Synchronous 286 faults restart at the first prefix. Protected delivery
 * belongs to the caller; the real-mode prototype constructs its vector frame. */
static int fault286(Pc98Cpu16State *c,const Pc98ExecuteBus *b,unsigned vector,uint16_t restart) {
 if(c->fault.valid)return 1;
 if(b->resolve){c->fault=(Pc98CpuFault16){.vector=(uint8_t)vector,.valid=1,.has_error=(uint8_t)(!b->real_mode&&(vector==8||(vector>=10&&vector<=13)))};return 1;}
 c->ip=restart;return interrupt_frame(c,b,vector);
}
static int32_t signed_word(uint16_t value) {
 return (int32_t)value-((value&0x8000u)?65536:0);
}
enum Pc98InterruptResult pc98_cpu_interrupt16(Pc98Cpu16State *c,const Pc98ExecuteBus *b,unsigned vector,enum Pc98InterruptKind kind) {
 if(kind<PC98_INT_SOFTWARE || kind>PC98_INT_NMI || vector>255 || c->model>PC98_CPU_V30)return PC98_INT_UNSUPPORTED;
 if(kind==PC98_INT_NMI && vector!=2)return PC98_INT_UNSUPPORTED;
 if(kind==PC98_INT_IRQ && (!(c->flags&PC98_IF)||c->irq_inhibit))return PC98_INT_BLOCKED;
 if(kind==PC98_INT_NMI && (c->nmi_inhibit||c->nmi_active))return PC98_INT_BLOCKED;
 if(c->fault.valid)return PC98_INT_BLOCKED;
 if(b->resolve) {
  if(c->model!=PC98_CPU_286||!b->interrupt_gate)return PC98_INT_UNSUPPORTED;
  Pc98CpuFault16 fault={0};int result=b->interrupt_gate(b->interrupt_context,c,b,vector,kind==PC98_INT_SOFTWARE,kind!=PC98_INT_SOFTWARE,0,0,&fault);
  if(result<0)return PC98_INT_UNSUPPORTED;
  if(!result){fault.valid=1;fault.restart_ip=c->ip;fault.external=(uint8_t)(kind!=PC98_INT_SOFTWARE);c->fault=fault;return PC98_INT_FAULT;}
  if(kind==PC98_INT_NMI)c->nmi_active=1;
  return PC98_INT_DELIVERED;
 }
 if(!interrupt_frame(c,b,vector))return PC98_INT_UNSUPPORTED;
 if(kind==PC98_INT_NMI)c->nmi_active=1;
 return PC98_INT_DELIVERED;
}
static uint16_t relative8(Pc98Cpu16State *c,const Pc98FetchBus *f) {
 unsigned v=pc98_cpu_fetch8(c,f);
 return (uint16_t)(v<128?v:v+0xff00);
}
static int condition(unsigned code,uint16_t flags) {
 unsigned of=(flags&PC98_OF)!=0,cf=(flags&PC98_CF)!=0;
 unsigned zf=(flags&PC98_ZF)!=0,sf=(flags&PC98_SF)!=0;
 unsigned pf=(flags&PC98_PF)!=0,result;
 switch(code>>1) {
 case 0:result=of;break;case 1:result=cf;break;
 case 2:result=zf;break;case 3:result=cf|zf;break;
 case 4:result=sf;break;case 5:result=pf;break;
 case 6:result=sf^of;break;default:result=(sf^of)|zf;break;
 }
 return (int)(result^(code&1));
}
/* Only instructions with complete scalar preflight enter the resolver path.
 * Complex instruction families remain explicit unsupported, not half executed. */
static int scalar_resolver_opcode(unsigned op) {
 return (op<0x40&&(op&7)<=5)||(op>=0x40&&op<=0x5f)||op==0x07||op==0x17||op==0x1f||op==0x06||op==0x0e||op==0x16||op==0x1e||op==0x68||op==0x6a||op==0x0f||op==0x60||op==0x61||op==0x62||op==0x63||op==0x69||op==0x6b||(op>=0x6c&&op<=0x6f)||
  (op>=0x70&&op<=0x8d)||op==0x8e||op==0x8f||(op>=0x90&&op<=0x99)||op==0x9a||op==0x9b||op==0x9c||op==0x9d||op==0x9e||op==0x9f||
  (op>=0xa0&&op<=0xaf)||(op>=0xb0&&op<=0xbf)||
  op==0xc0||op==0xc1||op==0xc2||op==0xc3||op==0xc4||op==0xc5||op==0xc6||op==0xc7||op==0xc8||op==0xc9||op==0xca||op==0xcb||op==0xcc||op==0xcd||op==0xce||op==0xcf||(op>=0xd0&&op<=0xd5)||op==0xd7||(op>=0xd8&&op<=0xdf)||
  op==0x27||op==0x2f||op==0x37||op==0x3f||op==0xf4||op==0xf5||op==0xf6||op==0xf7||
  (op>=0xe0&&op<=0xe7)||(op>=0xec&&op<=0xef)||op==0xe8||op==0xe9||op==0xea||op==0xeb||op==0xf8||op==0xf9||op==0xfa||op==0xfb||op==0xfc||op==0xfd||op==0xfe||op==0xff;
}
static enum Pc98StepResult rollback16(Pc98Cpu16State *c,const Pc98Cpu16State *before,enum Pc98StepResult result) {
 if(c->fault.valid) {
  Pc98CpuFault16 fault=c->fault;
  if(fault.committed){fault.restart_ip=c->ip;c->fault=fault;return PC98_STEP_FAULT;}
  fault.restart_ip=before->ip;*c=*before;c->fault=fault;return PC98_STEP_FAULT;
 }
 *c=*before;return result;
}
static enum Pc98StepResult step16(Pc98Cpu16State *c,const Pc98ExecuteBus *b,int *completed) {
 Pc98Cpu16State before=*c;uint32_t cursor=c->ip;unsigned fetched=0;
 const Pc98FetchBus f={.context=b->fetch8?b->fetch_context:b->context,.read8=b->fetch8?b->fetch8:b->read8,.resolve=b->resolve,.resolve_context=b->resolve_context,.cursor=b->resolve?&cursor:0,.consumed=c->model==PC98_CPU_286?&fetched:0,.maximum=b->instruction_maximum?b->instruction_maximum:10,.length_has_error=(uint8_t)(b->resolve&&!b->real_mode)};int segment=-1;
 unsigned opcode,repeat=0,prefixes=0,suppress_debug=0,ignored=0;
 for(;;) {
  opcode=pc98_cpu_fetch8(c,&f);
  switch(opcode) {
  case 0x26:segment=PC98_ES;break;case 0x2e:segment=PC98_CS;break;
  case 0x36:segment=PC98_SS;break;case 0x3e:segment=PC98_DS;break;
  case 0x64:case 0x65:
   if(c->model==PC98_CPU_V30)repeat=opcode;
   else if(b->ignored_prefixes386&PC98_IGNORE_SEGMENT386)ignored|=PC98_IGNORE_SEGMENT386;
   else goto decoded;
   break;
  case 0x66:case 0x67:{
   unsigned bit=opcode==0x66?PC98_IGNORE_OPERAND386:PC98_IGNORE_ADDRESS386;
   if(c->model!=PC98_CPU_286||!(b->ignored_prefixes386&bit))goto decoded;
   ignored|=bit;break;
  }
  case 0xf2:case 0xf3:repeat=opcode;break;
  default:goto decoded;
  }
  if(++prefixes>=15)goto unsupported;
 }
decoded:
 if(c->fault.valid)goto unsupported;
 if(ignored&~pc98_cpu_native_prefix_mask386(opcode))goto unsupported;
 if(b->resolve&&(c->model!=PC98_CPU_286||!scalar_resolver_opcode(opcode)))goto unsupported;
 if(b->resolve&&((opcode==0xea&&!b->jump_far)||(opcode==0x9a&&!b->call_far)||((opcode==0xca||opcode==0xcb)&&!b->return_far)))goto unsupported;
 if(b->resolve&&!b->load_segment&&(opcode==0x8e||opcode==0x07||opcode==0x17||opcode==0x1f||opcode==0xc4||opcode==0xc5))goto unsupported;
 /* NEC 12-14: carry repeat is defined for CMPBK/CMPM (CMPS/SCAS).
  * The incoming CF is ignored; the first comparison runs whenever CX != 0. */
 if((repeat==0x64||repeat==0x65)&&opcode!=0xa6&&opcode!=0xa7&&opcode!=0xae&&opcode!=0xaf)goto unsupported;
 /* Multi-iteration REP tracing needs generation-specific certification. */
 if(b->single_step&&(before.flags&PC98_TF)&&repeat&&c->r[PC98_CX]>1u)goto unsupported;
 if(repeat && !((opcode>=0x6c&&opcode<=0x6f)||(opcode>=0xa4&&opcode<=0xa7)||(opcode>=0xaa&&opcode<=0xaf)))goto unsupported;
 unsigned bits=(opcode&1)?16:8;
 /* NEC V20/V30 User Manual 12-81..12-99: TEST1/CLR1/SET1/NOT1. */
 if(opcode==0x0f&&c->model==PC98_CPU_V30){
  unsigned second=pc98_cpu_fetch8(c,&f);
  /* NEC 12-21..24: INS/EXT bit fields use word-aligned bit cursors.
   * Register upper nibbles must be zero for documented operation. */
  if(second==0x31||second==0x33||second==0x39||second==0x3b){
   unsigned spec=pc98_cpu_fetch8(c,&f),immediate=second&8u,extract=second&2u;
   if((spec&0xc0u)!=0xc0u||(immediate&&(spec&0x38u)))goto unsupported;
   unsigned index_register=spec&7u,start=regread(c,index_register,8);
   unsigned encoded=immediate?pc98_cpu_fetch8(c,&f):regread(c,(spec>>3)&7u,8);
   if(start>15u||encoded>15u||segment>=0)goto unsupported;
   unsigned length=encoded+1u,total=start+length,index=extract?PC98_SI:PC98_DI;
   uint16_t offset=c->r[index];unsigned crossing=total>16u;
   if(offset>(crossing?0xfffcu:0xfffeu))goto unsupported;
   uint32_t address=c->base[extract?PC98_DS:PC98_ES]+offset;
   uint32_t value=b->read16(b->context,address);
   if(crossing)value|=(uint32_t)b->read16(b->context,address+2u)<<16;
   uint32_t mask=(1u<<length)-1u;
   if(extract)c->r[PC98_AX]=(uint16_t)((value>>start)&mask);
   else {
    value=(value&~(mask<<start))|(((uint32_t)c->r[PC98_AX]&mask)<<start);
    b->write16(b->context,address,(uint16_t)value);
    if(crossing)b->write16(b->context,address+2u,(uint16_t)(value>>16));
   }
   c->r[index]=(uint16_t)(offset+(total>=16u?2u:0u));
   regwrite(c,index_register,8,(uint16_t)(total&15u));goto completed;
  }
  /* NEC 12-42..44: CL counts digits; SI/DI/CL and direction stay unchanged.
   * Odd counts operate through the last byte, including its padding nibble;
   * its high result nibble is undefined. Undefined arithmetic flags persist.
   * Undocumented lengths and segment wrap remain explicit unsupported. */
  if(second==0x20||second==0x22||second==0x26){
   unsigned digits=c->r[PC98_CX]&255u;
   if(!digits||digits==255)goto unsupported;
   unsigned length=(digits+1u)/2u;
   if((uint32_t)c->r[PC98_SI]+length>0x10000u||(uint32_t)c->r[PC98_DI]+length>0x10000u)goto unsupported;
   unsigned source_segment=segment<0?PC98_DS:(unsigned)segment,carry=0,nonzero=0;
   uint32_t source=c->base[source_segment]+c->r[PC98_SI],destination=c->base[PC98_ES]+c->r[PC98_DI];
   for(unsigned i=0;i<length;i++){
    unsigned from=b->read8(b->context,source+i),to=b->read8(b->context,destination+i);
    unsigned x=(from>>4)*10u+(from&15u),y=(to>>4)*10u+(to&15u),result;
    if(second==0x20){unsigned sum=x+y+carry;carry=sum>=100u;result=sum%100u;}
    else {unsigned sub=x+carry;carry=y<sub;result=(y+200u-sub)%100u;}
    unsigned packed=((result/10u)<<4)|(result%10u);nonzero|=packed;
    if(second!=0x26)b->write8(b->context,destination+i,(uint8_t)packed);
   }
   c->flags=(uint16_t)((c->flags&~(PC98_CF|PC98_ZF))|(carry?PC98_CF:0)|(nonzero?0:PC98_ZF));goto completed;
  }
  /* NEC 12-45/46: the high AL nibble is undefined. Preserve its current
   * value deterministically; all inputs are captured before either write. */
  if(second==0x28||second==0x2a){
   Pc98Operand16 operand=pc98_cpu_modrm16(c,&f,segment);
   if(operand.reg)goto unsupported;
   uint16_t value=operand_read(c,b,&operand,8),low=c->r[PC98_AX]&15u;
   uint16_t result=(uint16_t)(second==0x28?(((unsigned)value<<4)|low)&255u:((unsigned)low<<4)|(value>>4));
   uint16_t next=(uint16_t)(second==0x28?value>>4:value&15u);
   operand_write(c,b,&operand,8,result);
   c->r[PC98_AX]=(uint16_t)((c->r[PC98_AX]&0xfff0u)|next);goto completed;
  }
#if !PC98_FULLCPU
  if(second==0xff||second==0xfd||second==0xed){PURGE_STOP("V30 8080 emulation (BRKEM/RETEM/CALLN)");goto unsupported;}
#endif
  if(second<0x10||second>0x1f)goto unsupported;
  Pc98Operand16 operand=pc98_cpu_modrm16(c,&f,segment);
  if(operand.reg)goto unsupported;
  unsigned width=(second&1u)?16u:8u,operation=(second>>1)&3u;
  unsigned index=second&8u?pc98_cpu_fetch8(c,&f):c->r[PC98_CX]&255u;
  if(!operand.is_register&&width==16&&operand.offset==0xffff)goto unsupported;
  uint16_t value=operand_read(c,b,&operand,width),mask=(uint16_t)(1u<<(index&(width-1u)));
  if(!operation)c->flags=(uint16_t)((c->flags&~(PC98_ZF|PC98_CF|PC98_OF))|((value&mask)?0:PC98_ZF));
  else {
   if(operation==1)value&=(uint16_t)~mask;
   else if(operation==2)value|=mask;
   else value^=mask;
   operand_write(c,b,&operand,width,value);
  }
  goto completed;
 }
 /* V30 without an 8087: ESC computes its address only, WAIT passes (POLL inactive). */
 if(c->model==PC98_CPU_V30&&b->npx_absent&&(opcode==0x9b||(opcode>=0xd8&&opcode<=0xdf))) {
  if(opcode!=0x9b)(void)pc98_cpu_modrm16(c,&f,segment);
  if(c->fault.valid)goto unsupported;
  goto completed;
 }
 if(opcode==0x0f||opcode==0x63||opcode==0x9b||(opcode>=0xd8&&opcode<=0xdf)) {
  if(c->model!=PC98_CPU_286||!b->system_instruction)goto unsupported;
  unsigned system_opcode=opcode;Pc98Operand16 operand={0};const Pc98Operand16 *argument=&operand;
  if(opcode==0x0f) {
   unsigned second=pc98_cpu_fetch8(c,&f);
   if(second>3&&second!=5&&second!=6)goto unsupported;
   system_opcode=0xf00u|second;
   if(second==5||second==6)argument=0;
  }
  if(opcode==0x9b)argument=0;
  if(argument)operand=pc98_cpu_modrm16(c,&f,segment);
  if(c->fault.valid)goto unsupported;
  Pc98CpuFault16 fault={0};int result=b->system_instruction(b->system_context,c,b,system_opcode,argument,&fault);
  if(result<0)goto unsupported;
  if(result==2)return rollback16(c,&before,PC98_STEP_WAITING);
  if(!result){fault.valid=1;c->fault=fault;}
  goto completed;
 }
 if(opcode==0x27 || opcode==0x2f || opcode==0x37 || opcode==0x3f || opcode==0xd4 || opcode==0xd5) {
  if(opcode>=0xd4) {
   uint8_t base=pc98_cpu_fetch8(c,&f);if(c->fault.valid)goto unsupported;
   if(c->model==PC98_CPU_V30) {(void)base;/* V30 ignores the immediate: base 10 */}
   else {
    uint16_t value;
    if(!pc98_cpu_adjust_base286(opcode,c->r[PC98_AX],base,&c->flags,&value)) {
     if(!fault286(c,b,0,before.ip))goto unsupported;
    } else c->r[PC98_AX]=value;
    goto completed;
   }
  }
  c->r[PC98_AX]=c->model==PC98_CPU_V30?pc98_cpu_adjust_v30(opcode,c->r[PC98_AX],&c->flags):pc98_cpu_adjust286(opcode,c->r[PC98_AX],&c->flags);
  goto completed;
 }
 if(opcode==0x62) {
  if(c->model!=PC98_CPU_286)goto unsupported;
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);uint32_t address;
  if(c->fault.valid)goto unsupported;
  if(o.is_register) {if(!fault286(c,b,6,before.ip))goto unsupported;goto completed;}
  if(!b->resolve&&o.offset>0xfffc) {if(!fault286(c,b,13,before.ip))goto unsupported;goto completed;}
  if(!resolve_memory(c,b,o.segment,o.offset,32,PC98_ACCESS_READ,&address))goto unsupported;
  int32_t lower=signed_word(b->read16(b->context,address));
  uint32_t upper_address=b->resolve?(address+2)&0xffffffu:address+2;
  int32_t upper=signed_word(b->read16(b->context,upper_address)),index=signed_word(c->r[o.reg]);
  if((index<lower||index>upper)&&!fault286(c,b,5,before.ip))goto unsupported;
  goto completed;
 }
 if(opcode==0x69 || opcode==0x6b) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);
  uint16_t factor=opcode==0x69?pc98_cpu_fetch16(c,&f):relative8(c,&f),source=operand_read(c,b,&o,16),low,high;
  (void)pc98_cpu_muldiv(5,16,source,0,factor,&c->flags,&low,&high);
  c->r[o.reg]=low;goto completed;
 }
 if(opcode==0xd7) {
  uint16_t offset=(uint16_t)(c->r[PC98_BX]+(c->r[PC98_AX]&255u));
  unsigned seg=segment<0?PC98_DS:(unsigned)segment;
  regwrite(c,PC98_AX,8,read_memory(c,b,seg,offset,8));goto completed;
 }
 if(opcode>=0x6c&&opcode<=0x6f) {
  if(repeat&&!c->r[PC98_CX])goto completed;
  unsigned output=(opcode&2)!=0,index=output?PC98_SI:PC98_DI;
  if(b->resolve&&!b->real_mode&&pc98_cpu_cpl16(c)>((c->flags>>12)&3u)){c->fault=(Pc98CpuFault16){.vector=13,.valid=1,.has_error=1};goto completed;}
  if(!b->resolve&&bits==16&&c->r[index]==0xffff)goto unsupported;
  if(output?(bits==8?!b->out8:!b->out16):(bits==8?!b->in8:!b->in16))goto unsupported;
  uint16_t port=c->r[PC98_DX];uint32_t address;
  unsigned memory_segment=output?(segment<0?PC98_DS:(unsigned)segment):PC98_ES;
  if(!resolve_memory(c,b,memory_segment,c->r[index],bits,output?PC98_ACCESS_READ:PC98_ACCESS_WRITE,&address))goto unsupported;
  if(output) {
   if(bits==8) {if(!b->out8)goto unsupported;b->out8(b->context,port,b->read8(b->context,address));}
   else {if(!b->out16)goto unsupported;b->out16(b->context,port,b->read16(b->context,address));}
  } else {
   if(bits==8) {if(!b->in8)goto unsupported;b->write8(b->context,address,b->in8(b->context,port));}
   else {if(!b->in16)goto unsupported;b->write16(b->context,address,b->in16(b->context,port));}
  }
  unsigned width=bits/8;
  c->r[index]=(uint16_t)(c->r[index]+((c->flags&PC98_DF)?0u-width:width));
  if(repeat) {
   c->r[PC98_CX]=(uint16_t)(c->r[PC98_CX]-1);
   if(c->r[PC98_CX]){c->ip=before.ip;*completed=0;}
  }
  goto completed;
 }
 if((opcode>=0xe4&&opcode<=0xe7)||(opcode>=0xec&&opcode<=0xef)) {
  uint16_t port=opcode<0xec?pc98_cpu_fetch8(c,&f):c->r[PC98_DX];
  if(c->fault.valid)goto unsupported;
  if(b->resolve&&!b->real_mode&&pc98_cpu_cpl16(c)>((c->flags>>12)&3u)){c->fault=(Pc98CpuFault16){.vector=13,.valid=1,.has_error=1};goto completed;}
  unsigned output=(opcode&2)!=0;
  if(output) {
   if(bits==8) {if(!b->out8)goto unsupported;b->out8(b->context,port,(uint8_t)c->r[PC98_AX]);}
   else {if(!b->out16)goto unsupported;b->out16(b->context,port,c->r[PC98_AX]);}
  } else {
   uint16_t value;
   if(bits==8) {if(!b->in8)goto unsupported;value=b->in8(b->context,port);}
   else {if(!b->in16)goto unsupported;value=b->in16(b->context,port);}
   regwrite(c,PC98_AX,bits,value);
  }
  goto completed;
 }
 if((opcode>=0xa4&&opcode<=0xa7)||(opcode>=0xaa&&opcode<=0xaf)) {
  if(repeat&&!c->r[PC98_CX])goto completed;
  unsigned source=opcode<=0xa7||opcode==0xac||opcode==0xad;
  unsigned destination=opcode<=0xab||opcode>=0xae;
  if(!b->resolve&&bits==16&&((source&&c->r[PC98_SI]==0xffff)||(destination&&c->r[PC98_DI]==0xffff)))goto unsupported;
  uint32_t from=0,to=0;unsigned source_segment=segment<0?PC98_DS:(unsigned)segment;
  unsigned store=opcode==0xa4||opcode==0xa5||opcode==0xaa||opcode==0xab;
  /* Both accesses must be valid before reading either source (MMIO may
   * have read side effects). REP commits one complete iteration at a time. */
  if(source&&!resolve_memory(c,b,source_segment,c->r[PC98_SI],bits,PC98_ACCESS_READ,&from))goto unsupported;
  if(destination&&!resolve_memory(c,b,PC98_ES,c->r[PC98_DI],bits,store?PC98_ACCESS_WRITE:PC98_ACCESS_READ,&to))goto unsupported;
  uint16_t a=source?(bits==8?b->read8(b->context,from):b->read16(b->context,from)):regread(c,PC98_AX,bits);
  if(opcode==0xa4||opcode==0xa5||opcode==0xaa||opcode==0xab) {
   if(bits==8)b->write8(b->context,to,(uint8_t)a);else b->write16(b->context,to,a);
  } else if(opcode==0xac||opcode==0xad)regwrite(c,PC98_AX,bits,a);
  else {
   uint16_t value=bits==8?b->read8(b->context,to):b->read16(b->context,to);
   (void)pc98_cpu_alu(PC98_CMP,bits,a,value,&c->flags);
  }
  unsigned distance=bits/8;uint16_t delta=(uint16_t)((c->flags&PC98_DF)?0u-distance:distance);
  if(source)c->r[PC98_SI]=(uint16_t)(c->r[PC98_SI]+delta);
  if(destination)c->r[PC98_DI]=(uint16_t)(c->r[PC98_DI]+delta);
  if(repeat) {
   c->r[PC98_CX]=(uint16_t)(c->r[PC98_CX]-1);
   unsigned again=c->r[PC98_CX]!=0;
   if(opcode==0xa6||opcode==0xa7||opcode==0xae||opcode==0xaf)
    again=again&&((repeat==0x64||repeat==0x65)?((repeat==0x65)==((c->flags&PC98_CF)!=0)):((repeat==0xf3)==((c->flags&PC98_ZF)!=0)));
   /* One iteration per host step gives the future IRQ/timing adapter a
    * restartable instruction boundary. Prefixes are fetched again. */
   if(again) {c->ip=before.ip;*completed=0;}
  }
  goto completed;
 }
 if(opcode==0xc0 || opcode==0xc1 || (opcode>=0xd0 && opcode<=0xd3)) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);if(o.reg==6)goto unsupported;
  unsigned count=opcode<0xd0?pc98_cpu_fetch8(c,&f):opcode<0xd2?1:c->r[PC98_CX]&255u;
  if(c->model==PC98_CPU_286)count&=31;
  if(b->resolve&&!count&&!o.is_register)goto unsupported;
  if(count)operand_write(c,b,&o,bits,pc98_cpu_shift(o.reg,bits,operand_rmw(c,b,&o,bits),count,&c->flags));
  goto completed;
 }
 if(opcode==0x60 || opcode==0x61) {
  if(!stack_words(c,b,8,opcode==0x60))goto unsupported;
  if(opcode==0x60) {
   uint16_t original_sp=c->r[PC98_SP];
   for(unsigned n=0;n<8;n++)(void)push16(c,b,n==PC98_SP?original_sp:c->r[n]);
  } else {
   for(unsigned n=8;n>0;n--) {
    uint16_t value;if(!pop16(c,b,&value))goto unsupported;
    if(n-1!=PC98_SP)c->r[n-1]=value;
   }
  }
  goto completed;
 }
 if(opcode==0xc8) {
  uint16_t allocation=pc98_cpu_fetch16(c,&f);unsigned level=pc98_cpu_fetch8(c,&f)&31u;
  unsigned words=level?level+1:1;uint16_t source=c->r[PC98_BP];
  if(!stack_words(c,b,words,1))goto unsupported;
  uint16_t end=(uint16_t)(c->r[PC98_SP]-2*words);
  if(b->resolve) {
   uint32_t ignored;
   /* Include the reserved locals as well as every pushed frame word.
    * Reject wrap of the whole frame before the first read or write. */
   uint16_t lowest=(uint16_t)(end-allocation);
   if(!resolve_memory(c,b,PC98_SS,lowest,(2*words+allocation)*8,PC98_ACCESS_WRITE,&ignored))goto unsupported;
   for(unsigned n=1;n<level;n++) {
    source=(uint16_t)(source-2);
    if(!resolve_memory(c,b,PC98_SS,source,16,PC98_ACCESS_READ,&ignored))goto unsupported;
   }
  } else {
   if(allocation>end)goto unsupported;
   for(unsigned n=1;n<level;n++) {
    source=(uint16_t)(source-2);if(source==0xffff)goto unsupported;
   }
  }
  (void)push16(c,b,c->r[PC98_BP]);uint16_t frame=c->r[PC98_SP];source=c->r[PC98_BP];
  for(unsigned n=1;n<level;n++) {
   source=(uint16_t)(source-2);
   (void)push16(c,b,read_memory(c,b,PC98_SS,source,16));
  }
  if(level)(void)push16(c,b,frame);
  c->r[PC98_BP]=frame;c->r[PC98_SP]=(uint16_t)(c->r[PC98_SP]-allocation);
  goto completed;
 }
 if(opcode==0x8d) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);if(o.is_register)goto invalid_operand;
  c->r[o.reg]=o.offset;goto completed;
 }
 if(opcode==0x98 || opcode==0x99) {
  if(opcode==0x98)c->r[PC98_AX]=(uint16_t)((c->r[PC98_AX]&255u)|((c->r[PC98_AX]&128u)?0xff00u:0));
  else c->r[PC98_DX]=(c->r[PC98_AX]&0x8000u)?0xffff:0;
  goto completed;
 }
 if(opcode==0xc9) {
  if(!b->resolve&&c->r[PC98_BP]==0xffff)goto unsupported;
  c->r[PC98_SP]=c->r[PC98_BP];
  if(!pop16(c,b,&c->r[PC98_BP]))goto unsupported;
  goto completed;
 }
 if(opcode==0xea || opcode==0x9a) {
  uint16_t target=pc98_cpu_fetch16(c,&f),selector=pc98_cpu_fetch16(c,&f);
  if(b->resolve) {if(!far_transfer(c,b,selector,target,opcode==0x9a))goto unsupported;goto completed;}
  if(opcode==0x9a) {
   if(!stack_words(c,b,2,1))goto unsupported;
   (void)push16(c,b,c->seg[PC98_CS]);(void)push16(c,b,c->ip);
  }
  load_segment(c,PC98_CS,selector);c->ip=target;goto completed;
 }
 if(opcode==0xcb || opcode==0xca) {
  uint16_t release=opcode==0xca?pc98_cpu_fetch16(c,&f):0,target,selector;
  if(c->fault.valid)goto unsupported;
  if(b->resolve){
   Pc98CpuFault16 fault={0};int result=b->return_far(b->segment_context,c,b,release,&fault);
   if(result<0)goto unsupported;
   if(!result){fault.valid=1;c->fault=fault;}
   goto completed;
  }
  if(!stack_words(c,b,2,0))goto unsupported;
  (void)pop16(c,b,&target);(void)pop16(c,b,&selector);
  load_segment(c,PC98_CS,selector);c->ip=target;
  c->r[PC98_SP]=(uint16_t)(c->r[PC98_SP]+release);goto completed;
 }
 if(opcode==0xc4 || opcode==0xc5) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);
  if(o.is_register)goto invalid_operand;
  uint32_t address;
  if(!b->resolve&&o.offset>0xfffc)goto unsupported;
  if(!resolve_memory(c,b,o.segment,o.offset,32,PC98_ACCESS_READ,&address))goto unsupported;
  uint16_t value=b->read16(b->context,address);
  uint32_t second=b->resolve?(address+2)&0xffffffu:address+2;
  uint16_t selector=b->read16(b->context,second);
  if(!load_data_segment(c,b,opcode==0xc4?PC98_ES:PC98_DS,selector))goto unsupported;
  c->r[o.reg]=value;
  goto completed;
 }
 if(opcode==0x8c || opcode==0x8e) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);
  if(o.reg>3 || (opcode==0x8e && o.reg==PC98_CS))goto invalid_operand;
  if(opcode==0x8c)operand_write(c,b,&o,16,c->seg[o.reg]);
  else {
   uint16_t selector=operand_read(c,b,&o,16);
   if(!load_data_segment(c,b,o.reg,selector))goto unsupported;
   if(o.reg==PC98_SS){c->irq_inhibit=c->nmi_inhibit=2;c->debug_inhibit=2;}
  }
  goto completed;
 }
 if(opcode==0x07 || opcode==0x17 || opcode==0x1f) {
  uint16_t selector;if(!pop16(c,b,&selector))goto unsupported;
  if(!load_data_segment(c,b,opcode>>3,selector))goto unsupported;
  if(opcode==0x17){c->irq_inhibit=c->nmi_inhibit=2;c->debug_inhibit=2;}
  goto completed;
 }
 if(opcode==0xf4) {
  if(b->resolve&&!b->real_mode&&pc98_cpu_cpl16(c)) {c->fault=(Pc98CpuFault16){.vector=13,.valid=1,.has_error=1};goto completed;}
  c->halted=1;goto completed;
 }
 if(opcode==0xcc || opcode==0xcd || opcode==0xce) {
  unsigned vector=opcode==0xcd?pc98_cpu_fetch8(c,&f):opcode==0xcc?3:4;
  if(c->fault.valid)goto unsupported;
  if(opcode==0xce&&!(c->flags&PC98_OF))goto completed;
  suppress_debug=1;
  if(b->resolve) {
   if(!b->interrupt_gate)goto unsupported;
   Pc98CpuFault16 fault={0};int result=b->interrupt_gate(b->interrupt_context,c,b,vector,1,0,0,0,&fault);
   if(result<0)goto unsupported;
   if(!result){fault.valid=1;c->fault=fault;}
  } else if(!interrupt_frame(c,b,vector))goto unsupported;
  goto completed;
 }
 if(opcode==0xcf) {
  if(b->resolve) {
   if(!b->return_gate)goto unsupported;
   Pc98CpuFault16 fault={0};int result=b->return_gate(b->interrupt_context,c,b,&fault);
   if(result<0)goto unsupported;
   if(!result){fault.valid=1;c->fault=fault;}
   goto completed;
  }
  if(!stack_words(c,b,3,0))goto unsupported;
  uint16_t target,selector,flags;
  (void)pop16(c,b,&target);(void)pop16(c,b,&selector);(void)pop16(c,b,&flags);
  load_segment(c,PC98_CS,selector);c->ip=target;c->flags=import_flags(c,flags,1);
  c->nmi_active=0;goto completed;
 }
 if(opcode==0xfa || opcode==0xfb) {
  if(b->resolve&&!b->real_mode&&pc98_cpu_cpl16(c)>((c->flags>>12)&3u)) {c->fault=(Pc98CpuFault16){.vector=13,.valid=1,.has_error=1};goto completed;}
  if(opcode==0xfa)c->flags&=(uint16_t)~PC98_IF;
  else {c->flags|=PC98_IF;c->irq_inhibit=2;}
  goto completed;
 }
 if(opcode==0x9c || opcode==0x9d) {
  if(opcode==0x9c) {
   if(!push16(c,b,export_flags(c)))goto unsupported;
  } else {
   uint16_t value;if(!pop16(c,b,&value))goto unsupported;
   if(b->resolve&&!b->real_mode) {
    unsigned cpl=pc98_cpu_cpl16(c),iopl=(c->flags>>12)&3u;
    uint16_t writable=0x4dd5; /* status/TF/DF/OF/NT; IF and IOPL conditional */
    if(!cpl)writable|=0x3000;
    if(cpl<=iopl)writable|=PC98_IF;
    c->flags=(uint16_t)(((c->flags&0x7fd5u)&~writable)|(value&writable)|2u);
   } else {
    /* Intel 286 real mode cannot change NT/IOPL through POPF. */
    c->flags=import_flags(c,value,0);
   }
  }
  goto completed;
 }
 if(opcode==0x9e || opcode==0x9f) {
  if(opcode==0x9e)c->flags=(uint16_t)((c->flags&~0xd5u)|((c->r[PC98_AX]>>8)&0xd5u));
  else regwrite(c,4,8,(uint16_t)((c->flags&0xd5u)|2));
  goto completed;
 }
 if(opcode==0xf5 || opcode==0xf8 || opcode==0xf9 || opcode==0xfc || opcode==0xfd) {
  if(opcode==0xf5)c->flags^=1;
  else { unsigned mask=opcode<0xfc?1:0x400;
   c->flags=(uint16_t)((c->flags&~mask)|((opcode&1)?mask:0)); }
  goto completed;
 }
 if(opcode>=0x70 && opcode<=0x7f) {
  uint16_t delta=relative8(c,&f);
  if(condition(opcode&15,c->flags))near_target(c,b,(uint16_t)(c->ip+delta));
  goto completed;
 }
 if(opcode>=0xe0 && opcode<=0xe3) {
  uint16_t delta=relative8(c,&f);unsigned take;
  if(opcode==0xe3)take=c->r[PC98_CX]==0;
  else {
   c->r[PC98_CX]=(uint16_t)(c->r[PC98_CX]-1);take=c->r[PC98_CX]!=0;
   if(opcode==0xe0)take=take && !(c->flags&PC98_ZF);
   if(opcode==0xe1)take=take && (c->flags&PC98_ZF);
  }
  if(take)near_target(c,b,(uint16_t)(c->ip+delta));
  goto completed;
 }
 if(opcode==0xeb || opcode==0xe9 || opcode==0xe8) {
  uint16_t delta=opcode==0xeb?relative8(c,&f):pc98_cpu_fetch16(c,&f);
  uint16_t target=(uint16_t)(c->ip+delta);
  if(opcode==0xe8) {if(!near_call(c,b,target))goto unsupported;}
  else near_target(c,b,target);
  goto completed;
 }
 if(opcode>=0x50 && opcode<=0x57) {
  uint16_t value=c->r[opcode&7];
  /* The native V30 short form differs from its generic FF /6 form. */
  if((opcode&7)==PC98_SP && c->model==PC98_CPU_V30)value=(uint16_t)(value-2);
  if(!push16(c,b,value))goto unsupported;
  goto completed;
 }
 if(opcode>=0x58 && opcode<=0x5f) {
  uint16_t value;if(!pop16(c,b,&value))goto unsupported;
  /* Preserve the native short-form update order; 8F /0 is separate. */
  if((opcode&7)==PC98_SP && c->model==PC98_CPU_V30)value=(uint16_t)(value+2);
  c->r[opcode&7]=value;goto completed;
 }
 if(opcode==0x68 || opcode==0x6a) {
  uint16_t value=opcode==0x68?pc98_cpu_fetch16(c,&f):relative8(c,&f);
  if(!push16(c,b,value))goto unsupported;
  goto completed;
 }
 if(opcode==0x06 || opcode==0x0e || opcode==0x16 || opcode==0x1e) {
  if(!push16(c,b,c->seg[opcode>>3]))goto unsupported;
  goto completed;
 }
 if(opcode==0x8f) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);uint16_t value;
  if(o.reg)goto invalid_stack_encoding;
  uint32_t ignored;
  if(b->resolve&&!o.is_register&&!resolve_memory(c,b,o.segment,o.offset,16,PC98_ACCESS_WRITE,&ignored))goto unsupported;
  if(!pop16(c,b,&value))goto unsupported;
  operand_write(c,b,&o,16,value);goto completed;
 }
 if(opcode==0xc3 || opcode==0xc2) {
  uint16_t release=opcode==0xc2?pc98_cpu_fetch16(c,&f):0,target;
  if(!pop16(c,b,&target))goto unsupported;
  near_target(c,b,target);c->r[PC98_SP]=(uint16_t)(c->r[PC98_SP]+release);
  goto completed;
 }
 if(opcode>=0xb0 && opcode<=0xbf) {
  bits=opcode<0xb8?8:16;regwrite(c,opcode&7,bits,immediate(c,&f,bits));goto completed;
 }
 if(opcode>=0x88 && opcode<=0x8b) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);
  if(opcode&2)regwrite(c,o.reg,bits,operand_read(c,b,&o,bits));
  else operand_write(c,b,&o,bits,regread(c,o.reg,bits));
  goto completed;
 }
 if(opcode==0xc6 || opcode==0xc7) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);if(o.reg)goto invalid_operand;
  uint16_t value=immediate(c,&f,bits);operand_write(c,b,&o,bits,value);goto completed;
 }
 if(opcode>=0xa0 && opcode<=0xa3) {
  Pc98Operand16 o={.address=0};unsigned seg=segment<0?PC98_DS:(unsigned)segment;
  o.offset=pc98_cpu_fetch16(c,&f);o.segment=seg;o.address=c->base[seg]+o.offset;
  if(opcode&2)operand_write(c,b,&o,bits,regread(c,PC98_AX,bits));
  else regwrite(c,PC98_AX,bits,operand_read(c,b,&o,bits));
  goto completed;
 }
 if(opcode<0x40 && (opcode&7)<=5) {
  enum Pc98AluOp op=(enum Pc98AluOp)(opcode>>3);unsigned form=opcode&7;
  uint16_t a,value,result;Pc98Operand16 o={0};
  if(form>=4) { a=regread(c,PC98_AX,bits);value=immediate(c,&f,bits); }
  else { o=pc98_cpu_modrm16(c,&f,segment);
   uint16_t rm=(!(form&2)&&op!=PC98_CMP)?operand_rmw(c,b,&o,bits):operand_read(c,b,&o,bits),reg=regread(c,o.reg,bits);
   a=(form&2)?reg:rm;value=(form&2)?rm:reg; }
  result=pc98_cpu_alu(op,bits,a,value,&c->flags);
  if(op!=PC98_CMP) {
   if(form>=4)regwrite(c,PC98_AX,bits,result);
   else if(form&2)regwrite(c,o.reg,bits,result);
   else operand_write(c,b,&o,bits,result);
  }
  goto completed;
 }
 if(opcode>=0x80 && opcode<=0x83) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);uint16_t value;
  if(opcode==0x83) { unsigned v=pc98_cpu_fetch8(c,&f);value=(uint16_t)(v<128?v:v+0xff00); }
  else value=immediate(c,&f,bits);
  uint16_t result=pc98_cpu_alu((enum Pc98AluOp)o.reg,bits,o.reg==PC98_CMP?operand_read(c,b,&o,bits):operand_rmw(c,b,&o,bits),value,&c->flags);
  if(o.reg!=PC98_CMP)operand_write(c,b,&o,bits,result);
  goto completed;
 }
 if(opcode>=0x40 && opcode<=0x4f) {
  unsigned r=opcode&7;uint16_t value=pc98_cpu_alu(opcode<0x48?PC98_INC:PC98_DEC,16,c->r[r],0,&c->flags);
  c->r[r]=value;goto completed;
 }
 if(opcode==0xfe || opcode==0xff) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);
  if(opcode==0xfe&&o.reg>1)goto invalid_operand;
  if(opcode==0xff&&o.reg==7)goto invalid_stack_encoding;
  if(b->resolve&&opcode==0xff&&(o.reg==3||o.reg==5)) {
   uint32_t address;
   if(o.is_register)goto invalid_operand;
   if(o.reg==3?!b->call_far:!b->jump_far)goto unsupported;
   if(!resolve_memory(c,b,o.segment,o.offset,32,PC98_ACCESS_READ,&address))goto unsupported;
   uint16_t target=b->read16(b->context,address),selector=b->read16(b->context,(address+2)&0xffffffu);
   if(!far_transfer(c,b,selector,target,o.reg==3))goto unsupported;
   goto completed;
  }
  if(opcode==0xff && (o.reg==3 || o.reg==5)) {
   if(o.is_register)goto invalid_operand;
   if(o.offset>0xfffc)goto unsupported;
   uint16_t target=b->read16(b->context,o.address),selector=b->read16(b->context,o.address+2);
   if(o.reg==3) {
    if(!stack_words(c,b,2,1))goto unsupported;
    (void)push16(c,b,c->seg[PC98_CS]);(void)push16(c,b,c->ip);
   }
   load_segment(c,PC98_CS,selector);c->ip=target;goto completed;
  }
  if(opcode==0xff && (o.reg==2 || o.reg==4 || o.reg==6)) {
   uint32_t ignored;
   if(b->resolve) {
    if(!o.is_register&&!resolve_memory(c,b,o.segment,o.offset,16,PC98_ACCESS_READ,&ignored))goto unsupported;
    if(o.reg!=4&&!check_push16(c,b))goto unsupported;
   }
   uint16_t value=operand_read(c,b,&o,16);
   if(c->fault.valid)goto unsupported;
   if(o.reg==2) {if(!near_call(c,b,value))goto unsupported;}
   else if(o.reg==6) {if(!push16(c,b,value))goto unsupported;}
   else near_target(c,b,value);
   goto completed;
  }
  if(o.reg>1)goto unsupported;
  uint16_t result=pc98_cpu_alu(o.reg?PC98_DEC:PC98_INC,bits,operand_rmw(c,b,&o,bits),0,&c->flags);
  operand_write(c,b,&o,bits,result);goto completed;
 }
 if(opcode==0x84 || opcode==0x85) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);
  (void)pc98_cpu_alu(PC98_AND,bits,operand_read(c,b,&o,bits),regread(c,o.reg,bits),&c->flags);goto completed;
 }
 if(opcode==0xa8 || opcode==0xa9) {
  uint16_t value=immediate(c,&f,bits);(void)pc98_cpu_alu(PC98_AND,bits,regread(c,PC98_AX,bits),value,&c->flags);goto completed;
 }
 if(opcode==0xf6 || opcode==0xf7) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);if(o.reg==1)goto unsupported;
  uint16_t test_immediate=o.reg==0?immediate(c,&f,bits):0;
  uint16_t value=(o.reg==2||o.reg==3)?operand_rmw(c,b,&o,bits):operand_read(c,b,&o,bits);
  if(c->fault.valid)goto unsupported;
  if(o.reg>=4) {
   uint16_t ax,dx;
   if(!pc98_cpu_muldiv(o.reg,bits,c->r[PC98_AX],c->r[PC98_DX],value,&c->flags,&ax,&dx)) {
    /* 286 divide errors restart at the faulting instruction, including its
     * prefixes. V30 exception return-IP semantics are a separate contract. */
    if(c->model!=PC98_CPU_286)goto unsupported;
    if(!fault286(c,b,0,before.ip))goto unsupported;
    goto completed;
   }
   c->r[PC98_AX]=ax;c->r[PC98_DX]=dx;goto completed;
  }
  if(o.reg==0) { (void)pc98_cpu_alu(PC98_AND,bits,value,test_immediate,&c->flags); }
  else { uint16_t result=o.reg==2?(uint16_t)~value:pc98_cpu_alu(PC98_NEG,bits,value,0,&c->flags);operand_write(c,b,&o,bits,result); }
  goto completed;
 }
 if(opcode>=0x90 && opcode<=0x97) {
  unsigned r=opcode&7;uint16_t value=c->r[r];c->r[r]=c->r[PC98_AX];c->r[PC98_AX]=value;goto completed;
 }
 if(opcode==0x86 || opcode==0x87) {
  Pc98Operand16 o=pc98_cpu_modrm16(c,&f,segment);uint16_t value=operand_rmw(c,b,&o,bits),reg=regread(c,o.reg,bits);
  operand_write(c,b,&o,bits,reg);regwrite(c,o.reg,bits,value);goto completed;
 }
 goto unsupported;
/* Intel286 PRM Appendix C item6: reserved POP/PUSH memory extensions #GP. */
invalid_stack_encoding:
 if(c->model!=PC98_CPU_286)goto unsupported;
 if(!fault286(c,b,13,before.ip))goto unsupported;
 goto completed;
invalid_operand:
 if(c->model!=PC98_CPU_286)goto unsupported;
 if(!fault286(c,b,6,before.ip))goto unsupported;
 goto completed;
completed:
 if(c->fault.valid)return rollback16(c,&before,PC98_STEP_FAULT);
 if(b->single_step&&(before.flags&PC98_TF)&&!suppress_debug&&c->debug_inhibit<2u&&(!b->task_committed||!*b->task_committed)){
  c->fault=(Pc98CpuFault16){.valid=1,.vector=1,.external=1,.restart_ip=c->ip};return PC98_STEP_TRAP;
 }
 return PC98_STEP_OK;
unsupported:
 return rollback16(c,&before,PC98_STEP_UNSUPPORTED);
}

enum Pc98StepResult pc98_cpu_step16(Pc98Cpu16State *c,const Pc98ExecuteBus *b) {
 if(b->instruction_completed)*b->instruction_completed=0;
 if(c->fault.valid)return PC98_STEP_FAULT;
 if(c->halted)return PC98_STEP_HALTED;
 int completed=1;
 enum Pc98StepResult result=step16(c,b,&completed);
 /* Do not expire an instruction shadow in the middle of a REP sequence. */
 if((result==PC98_STEP_OK||result==PC98_STEP_TRAP) && completed) {
  if(b->instruction_completed)*b->instruction_completed=1;
  if(c->irq_inhibit)c->irq_inhibit--;
  if(c->nmi_inhibit)c->nmi_inhibit--;
  if(c->debug_inhibit)c->debug_inhibit--;
 }
 return result;
}
