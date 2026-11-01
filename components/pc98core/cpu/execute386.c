/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Intel 80386 PRM instruction formats and bit/shift instruction pages. */
#include "execute386.h"
#include "purge.h"
#include <string.h>
#include "bits386.h"
#include "decode386.h"
#include "alu.h"
#include "alu386.h"
#include "muldiv386.h"
#include "shift386.h"
/* Intel 386 PRM Jcc table/Appendix D; SETcc uses the same conditions. */
static unsigned condition386(uint32_t flags,unsigned code){
 unsigned cf=(flags&PC98_CF)!=0,pf=(flags&PC98_PF)!=0,zf=(flags&PC98_ZF)!=0,sf=(flags&PC98_SF)!=0,of=(flags&PC98_OF)!=0,result;
 switch(code>>1){case 0:result=of;break;case 1:result=cf;break;case 2:result=zf;break;case 3:result=cf||zf;break;case 4:result=sf;break;case 5:result=pf;break;case 6:result=sf!=of;break;default:result=zf||(sf!=of);break;}
 return code&1u?!result:result;
}
static uint32_t immediate32(Pc98Cpu16State *c,const Pc98FetchBus *f){uint32_t low=pc98_cpu_fetch16(c,f);return low|((uint32_t)pc98_cpu_fetch16(c,f)<<16);}
static uint32_t reg(const Pc98Cpu386State *s,unsigned r,unsigned bits){return s->low.r[r]|(bits==32?(uint32_t)s->high[r]<<16:0);}
static void write_reg(Pc98Cpu386State *s,unsigned r,unsigned bits,uint32_t value){s->low.r[r]=(uint16_t)value;if(bits==32)s->high[r]=(uint16_t)(value>>16);}
static uint32_t read_general386(const Pc98Cpu386State *s,unsigned r,unsigned bits){return bits==8?((uint32_t)s->low.r[r&3u]>>(r<4?0:8))&255u:reg(s,r,bits);}
static void write_general386(Pc98Cpu386State *s,unsigned r,unsigned bits,uint32_t value){
 if(bits==8){unsigned shift=r<4?0:8;uint32_t mask=255u<<shift;s->low.r[r&3u]=(uint16_t)(((uint32_t)s->low.r[r&3u]&~mask)|((value&255u)<<shift));}
 else write_reg(s,r,bits,value);
}
static uint32_t immediate386(Pc98Cpu16State *c,const Pc98FetchBus *f,unsigned bits){return bits==8?pc98_cpu_fetch8(c,f):bits==16?pc98_cpu_fetch16(c,f):immediate32(c,f);}
static uint32_t alu_width386(enum Pc98AluOp operation,unsigned bits,uint32_t left,uint32_t right,uint32_t *flags){
 if(bits==32)return pc98_cpu_alu386(operation,left,right,flags);
 uint16_t low=(uint16_t)*flags,result=pc98_cpu_alu(operation,bits,(uint16_t)left,(uint16_t)right,&low);*flags=(*flags&0xffff0000u)|low;return result;
}
static uint32_t imul_width386(unsigned bits,uint32_t left,uint32_t right,uint32_t *flags){
 if(bits==32){uint32_t low,high;(void)pc98_cpu_muldiv386(5,left,0,right,flags,&low,&high);return low;}
 uint16_t low_flags=(uint16_t)*flags,ax,dx;(void)pc98_cpu_muldiv(5,16,(uint16_t)left,0,(uint16_t)right,&low_flags,&ax,&dx);*flags=(*flags&0xffff0000u)|low_flags;return ax;
}
static Pc98Operand386 operand386(Pc98Cpu386State *s,const Pc98FetchBus *f,unsigned address32,int segment){
 if(address32){uint32_t r[8];for(unsigned i=0;i<8;i++)r[i]=reg(s,i,32);return pc98_cpu_modrm386(&s->low,f,r,segment);}
 Pc98Operand16 o=pc98_cpu_modrm16(&s->low,f,segment<=PC98_DS?segment:-1);
 return (Pc98Operand386){.reg=o.reg,.rm=o.rm,.is_register=o.is_register,.segment=segment>PC98_DS?(unsigned)segment:o.segment,.offset=o.offset};
}
static int segment_ready386(const Pc98Cpu386State *s,unsigned segment){return segment<=PC98_DS||(segment>=PC98_FS386&&segment<=PC98_GS386&&s->extra[segment-PC98_FS386].initialized);}
static int memory_address(Pc98Cpu386State *s,const Pc98ExecuteBus *b,const Pc98Operand386 *o,unsigned size,enum Pc98MemoryAccess16 access,uint32_t *physical){
 Pc98Cpu16State *c=&s->low;Pc98CpuFault16 fault={0};
 if(o->segment>PC98_DS){
  unsigned index=o->segment-PC98_FS386;
  if(index<2&&s->extra[index].initialized){
   const Pc98ExtraSegmentCache386 *cache=&s->extra[index];unsigned ar=cache->access;
   int rights=cache->valid&&(ar&16u);
   if(access==PC98_ACCESS_WRITE)rights=rights&&!(ar&8u)&&(ar&2u);
   else if(access==PC98_ACCESS_READ)rights=rights&&(!(ar&8u)||(ar&2u));
   else rights=0;
   int range=size&&o->offset<=0xffffu&&size<=0x10000u-o->offset;
   if(range)range=(ar&12u)==4u?o->offset>cache->limit:o->offset+size<=(uint32_t)cache->limit+1u;
   if(rights&&range){*physical=(cache->base+o->offset)&0xffffffu;return 1;}
  }
  c->fault=(Pc98CpuFault16){.valid=1,.vector=13,.has_error=(uint8_t)(b->resolve&&!b->real_mode)};return 0;
 }

 if(b->resolve){if(!b->resolve(b->resolve_context,o->segment,o->offset,size,access,physical,&fault)){fault.valid=1;c->fault=fault;return 0;}return 1;}
 if(o->offset>0x10000u-size){c->fault=(Pc98CpuFault16){.valid=1,.vector=13};return 0;}
 *physical=(c->base[o->segment]+o->offset)&0xffffffu;return 1;
}
static uint32_t read_value386(const Pc98ExecuteBus *b,uint32_t physical,unsigned bits){
 if(bits==8)return b->read8(b->context,physical);
 uint32_t value=b->read16(b->context,physical);if(bits==32)value|=(uint32_t)b->read16(b->context,(physical+2u)&0xffffffu)<<16;return value;
}
static void write_value386(const Pc98ExecuteBus *b,uint32_t physical,unsigned bits,uint32_t value){
 if(bits==8)b->write8(b->context,physical,(uint8_t)value);
 else{b->write16(b->context,physical,(uint16_t)value);if(bits==32)b->write16(b->context,(physical+2u)&0xffffffu,(uint16_t)(value>>16));}
}
static uint32_t read_operand32(Pc98Cpu386State *s,const Pc98ExecuteBus *b,const Pc98Operand386 *o,enum Pc98MemoryAccess16 access,uint32_t *physical){
 if(o->is_register)return reg(s,o->rm,32);
 if(!memory_address(s,b,o,4,access,physical))return 0;
 uint32_t value=b->read16(b->context,*physical);return value|((uint32_t)b->read16(b->context,(*physical+2u)&0xffffffu)<<16);
}
/* Plain real-mode execution still checks the whole linear fetch cursor;
 * IP wrapping must not hide an instruction crossing FFFF. */
static int real_fetch(void *context,unsigned segment,uint32_t offset,uint32_t size,enum Pc98MemoryAccess16 access,uint32_t *physical,Pc98CpuFault16 *fault){
 (void)access;Pc98Cpu16State *c=context;
 if(offset>0xffffu||size>0x10000u-offset){*fault=(Pc98CpuFault16){.valid=1,.vector=13};return 0;}
 *physical=(c->base[segment]+offset)&0xffffffu;return 1;
}
static int near_target386(Pc98Cpu16State *c,const Pc98ExecuteBus *b,uint32_t target){
 uint32_t physical;Pc98CpuFault16 fault={0};
 int valid=b->resolve?b->resolve(b->resolve_context,PC98_CS,target,1,PC98_ACCESS_FETCH,&physical,&fault):real_fetch(c,PC98_CS,target,1,PC98_ACCESS_FETCH,&physical,&fault);
 if(!valid){fault.valid=1;c->fault=fault;}return valid;
}
static int push386(Pc98Cpu386State *s,const Pc98ExecuteBus *b,uint32_t value){
 Pc98Cpu16State *c=&s->low;
 if((!b->resolve||b->real_mode)&&c->r[PC98_SP]>0&&c->r[PC98_SP]<4)return -1;
 uint16_t next=(uint16_t)(c->r[PC98_SP]-4u);Pc98Operand386 stack={.segment=PC98_SS,.offset=next};uint32_t physical;
 if(!memory_address(s,b,&stack,4,PC98_ACCESS_WRITE,&physical))return 0;
 b->write16(b->context,physical,(uint16_t)value);b->write16(b->context,(physical+2u)&0xffffffu,(uint16_t)(value>>16));c->r[PC98_SP]=next;return 1;
}
/* 16-bit stack-address attribute only. Real stack-underflow shutdown remains
 * unsupported before frame access; the future mode adapter owns shutdown. */
static int near_call386(Pc98Cpu386State *s,const Pc98ExecuteBus *b,uint32_t target){
 Pc98Cpu16State *c=&s->low;
 if((!b->resolve||b->real_mode)&&c->r[PC98_SP]>0&&c->r[PC98_SP]<4)return -1;
 uint16_t next=(uint16_t)(c->r[PC98_SP]-4u);Pc98Operand386 stack={.segment=PC98_SS,.offset=next};uint32_t physical;
 if(!memory_address(s,b,&stack,4,PC98_ACCESS_WRITE,&physical)||!near_target386(c,b,target))return 0;
 b->write16(b->context,physical,c->ip);b->write16(b->context,(physical+2u)&0xffffffu,0);c->r[PC98_SP]=next;c->ip=(uint16_t)target;return 1;
}

/* Real-mode far32 with plain bus or atomic CS-cache reload.
 * Real hidden caches can borrow an atomic jump
 * hook after complete stack preflight. Protected far adapters remain pending. */
static int real_far_available386(const Pc98ExecuteBus *b){return !b->resolve||(b->real_mode&&b->jump_far);}
static int real_far386(Pc98Cpu386State *s,const Pc98ExecuteBus *b,unsigned call,uint16_t selector,uint32_t target){
 Pc98Cpu16State *c=&s->low;
 if(!real_far_available386(b)||PC98_V86(s)||(call&&c->r[PC98_SP]>0&&c->r[PC98_SP]<8))return -1;
 if(target>0xffffu){c->fault=(Pc98CpuFault16){.valid=1,.vector=13};return 0;}
 uint16_t return_cs=c->seg[PC98_CS],return_ip=c->ip,next=(uint16_t)(c->r[PC98_SP]-8u);uint32_t physical=0;
 if(call){Pc98Operand386 stack={.segment=PC98_SS,.offset=next};if(!memory_address(s,b,&stack,8,PC98_ACCESS_WRITE,&physical))return 0;}
 if(b->resolve){
  Pc98CpuFault16 failure={0};int result=b->jump_far(b->jump_context?b->jump_context:b->segment_context,c,b,selector,(uint16_t)target,&failure);
  if(result<0)return -1;
  if(!result){failure.valid=1;c->fault=failure;return 0;}
 }
 /* All fallible validation precedes the atomic CS hook. After success,
  * frame writes and GPR updates cannot fail under the bus contract. This
  * certifies final-state atomicity, not physical cache/bus commit ordering. */
 if(call){write_value386(b,(physical+4u)&0xffffffu,32,return_cs);write_value386(b,physical,32,return_ip);c->r[PC98_SP]=next;}
 if(!b->resolve){c->seg[PC98_CS]=selector;c->base[PC98_CS]=(uint32_t)selector<<4;c->ip=(uint16_t)target;}
 c->cpl=0;c->cpl_valid=1;return 1;
}

static int load_pointer386(Pc98Cpu386State *s,const Pc98ExecuteBus *b,const Pc98Operand386 *o,unsigned segment,unsigned bits){
 Pc98Cpu16State *c=&s->low;unsigned extra=segment>=4;
 if(PC98_V86(s)||(!o->is_register&&!segment_ready386(s,o->segment)))return -1;
 if(b->resolve&&(extra?(!b->real_mode&&!b->load_extra_segment):!b->load_segment))return -1;
 if(o->is_register){c->fault=(Pc98CpuFault16){.valid=1,.vector=6};return 0;}
 uint32_t physical;if(!memory_address(s,b,o,bits/8+2,PC98_ACCESS_READ,&physical))return 0;
 uint32_t value=read_value386(b,physical,bits);uint16_t selector=b->read16(b->context,(physical+bits/8)&0xffffffu);
 if(extra){
  Pc98ExtraSegmentCache386 cache={0};
  if(b->resolve&&!b->real_mode){Pc98CpuFault16 failure={0};int result=b->load_extra_segment(b->extra_segment_context,c,b,selector,&cache,&failure);if(result<0)return -1;if(!result){failure.valid=1;c->fault=failure;return 0;}}
  else cache=(Pc98ExtraSegmentCache386){.base=(uint32_t)selector<<4,.selector=selector,.limit=0xffff,.access=0x93,.initialized=1,.valid=1};
  s->extra[segment-4]=cache;
 }else if(b->resolve){Pc98CpuFault16 failure={0};if(!b->load_segment(b->segment_context,c,b,segment,selector,&failure)){failure.valid=1;c->fault=failure;return 0;}}
 else{c->seg[segment]=selector;c->base[segment]=(uint32_t)selector<<4;}
 write_reg(s,o->reg,bits,value);
 /* Intel1986 PRM3.10.3 excludes LSS from MOV/POP SS's new shadow.
  * The complete selector:offset pair commits before ordinary TF delivery. */
 return 1;
}

void pc98_cpu_extra_segments_reset386(Pc98Cpu386State *s){
 memset(s->extra,0,sizeof(s->extra));
 for(unsigned i=0;i<2;i++){s->extra[i].limit=0xffff;s->extra[i].access=0x93;s->extra[i].initialized=s->extra[i].valid=1;}
}

enum Pc98StepResult pc98_cpu_extension386(Pc98Cpu386State *s,const Pc98ExecuteBus *b){
 if(b->instruction_completed)*b->instruction_completed=0;
 Pc98Cpu16State *c=&s->low;if(c->fault.valid)return PC98_STEP_FAULT;if(c->halted)return PC98_STEP_HALTED;
 Pc98Cpu386State before=*s;unsigned consumed=0;uint32_t cursor=c->ip;
 Pc98FetchBus fetch={.context=b->fetch8?b->fetch_context:b->context,.read8=b->fetch8?b->fetch8:b->read8,.resolve=b->resolve?b->resolve:real_fetch,.resolve_context=b->resolve?b->resolve_context:c,.cursor=&cursor,.consumed=&consumed,.maximum=15,.length_has_error=(uint8_t)(b->resolve&&!b->real_mode)};
 unsigned opcode,bits=16,address32=0,repeat=0;int segment=-1;
 for(;;){
  opcode=pc98_cpu_fetch8(c,&fetch);if(c->fault.valid)goto failure;
  if(opcode==0xf2||opcode==0xf3){repeat=opcode;continue;}
  if(opcode==0x66){bits=32;continue;}
  if(opcode==0x67){address32=1;continue;}
  if(opcode==0x26||opcode==0x2e||opcode==0x36||opcode==0x3e){segment=(int)((opcode>>3)&3u);continue;}
  if(opcode==0x64||opcode==0x65){segment=opcode==0x64?PC98_FS386:PC98_GS386;continue;}
  break;
 }
 unsigned complete=1;
 uint32_t flags=c->flags|((uint32_t)s->flag_high<<16);
 unsigned io_string_opcode=opcode>=0x6c&&opcode<=0x6f;
 unsigned string_opcode=(opcode>=0xa4&&opcode<=0xa7)||(opcode>=0xaa&&opcode<=0xaf);
 unsigned basic_alu=(opcode<0x40&&(opcode&7u)<=5u)||opcode==0x80||opcode==0x81||opcode==0x83||opcode==0x84||opcode==0x85||opcode==0xa8||opcode==0xa9;
 unsigned unary_alu=(opcode>=0x40&&opcode<=0x4f)||opcode==0xfe||(opcode==0xff&&bits==16);
 unsigned group3=opcode==0xf6||opcode==0xf7,flags_opcode=opcode==0x9c||opcode==0x9d||opcode==0xcf;
 unsigned group2=opcode==0xc0||opcode==0xc1||(opcode>=0xd0&&opcode<=0xd3),lea=opcode==0x8d,imul_literal=opcode==0x69||opcode==0x6b;
 unsigned segment_move=opcode==0x8c||opcode==0x8e,xchg=opcode==0x86||opcode==0x87;
 unsigned basic_move=(opcode>=0xb0&&opcode<=0xbf)||(opcode>=0x88&&opcode<=0x8b)||opcode==0xc6||opcode==0xc7,moffs_move=opcode>=0xa0&&opcode<=0xa3;
 /* A REP/REPNE prefix on a non-string instruction is ignored (the manuals leave it undefined; the processors
  * execute the instruction once). */
 if(repeat&&!io_string_opcode&&!string_opcode)repeat=0;
 if(opcode!=0x0f){
  if(bits!=32&&!string_opcode&&!io_string_opcode&&!basic_move&&!moffs_move&&!basic_alu&&!unary_alu&&!group3&&!group2&&!lea&&!imul_literal&&!flags_opcode&&!segment_move&&!xchg)goto unsupported;
  if(xchg){
   unsigned width=opcode==0x86?8u:bits;Pc98Operand386 o=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;
   uint32_t source=read_general386(s,o.reg,width),value,physical=0;
   if(o.is_register){value=read_general386(s,o.rm,width);write_general386(s,o.reg,width,value);write_general386(s,o.rm,width,source);}
   else{
    /* Intel386 XCHG asserts LOCK even without an explicit prefix. The
     * callback is a caller-owned atomic/DMA exclusion window, not a
     * claim of physical bus timing. No faultable operation follows it. */
    if(!b->lock_bus||PC98_V86(s)||!segment_ready386(s,o.segment))goto unsupported;
    if(!memory_address(s,b,&o,width/8,PC98_ACCESS_WRITE,&physical))goto failure;
    b->lock_bus(b->lock_context,1);value=read_value386(b,physical,width);
    write_value386(b,physical,width,source);write_general386(s,o.reg,width,value);
    b->lock_bus(b->lock_context,0);
   }
   goto retired;
  }
  if(segment_move){
   Pc98Operand386 o=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;
   if(o.reg<4)goto unsupported;
   if(o.reg>5){c->fault=(Pc98CpuFault16){.valid=1,.vector=6};goto failure;}
   if(PC98_V86(s))goto unsupported;
   unsigned extra=o.reg-4u;
   if(opcode==0x8c&&(bits!=16||!s->extra[extra].initialized))goto unsupported;
   if(opcode==0x8e&&b->resolve&&!b->real_mode&&!b->load_extra_segment)goto unsupported;
   if(!o.is_register&&!segment_ready386(s,o.segment))goto unsupported;
   uint32_t physical=0;if(!o.is_register&&!memory_address(s,b,&o,2,opcode==0x8e?PC98_ACCESS_READ:PC98_ACCESS_WRITE,&physical))goto failure;
   if(opcode==0x8e){
    uint16_t selector=o.is_register?c->r[o.rm]:(uint16_t)read_value386(b,physical,16);
    Pc98ExtraSegmentCache386 cache={0};
    if(b->resolve&&!b->real_mode){
     Pc98CpuFault16 failure={0};int result=b->load_extra_segment(b->extra_segment_context,c,b,selector,&cache,&failure);
     if(result<0)goto unsupported;
     if(!result){failure.valid=1;c->fault=failure;goto failure;}
    }else{cache.selector=selector;cache.base=(uint32_t)selector<<4;cache.limit=0xffff;cache.access=0x93;cache.initialized=cache.valid=1;}
    s->extra[extra]=cache;
   }else if(o.is_register)write_reg(s,o.rm,16,s->extra[extra].selector);
   else write_value386(b,physical,16,s->extra[extra].selector);
   goto retired;
  }
  if(lea){
   Pc98Operand386 o=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;
   if(o.is_register){c->fault=(Pc98CpuFault16){.valid=1,.vector=6};goto failure;}write_reg(s,o.reg,bits,o.offset);goto retired;
  }
  if(group2){
   unsigned width=opcode&1u?bits:8u;Pc98Operand386 o=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;if(o.reg==6)goto unsupported;
   unsigned count=opcode==0xd0||opcode==0xd1?1u:opcode==0xd2||opcode==0xd3?c->r[PC98_CX]&255u:pc98_cpu_fetch8(c,&fetch),amount=count&31u;if(c->fault.valid)goto failure;
   /* A masked count of 0 changes nothing (PRM pseudo-code: the destination is assigned only inside the count loop):
    * the operand is read, after the WRITE rights check, but not written back. */
   if(!o.is_register&&!segment_ready386(s,o.segment))goto unsupported;
   uint32_t physical=0,value;if(o.is_register)value=read_general386(s,o.rm,width);else{if(!memory_address(s,b,&o,width/8,PC98_ACCESS_WRITE,&physical))goto failure;value=read_value386(b,physical,width);}
   if(width==32)value=pc98_cpu_shift386(o.reg,value,amount,&flags);else{uint16_t low=(uint16_t)flags;value=pc98_cpu_shift(o.reg,width,(uint16_t)value,amount,&low);flags=(flags&0xffff0000u)|low;}
   if(amount){if(o.is_register)write_general386(s,o.rm,width,value);else write_value386(b,physical,width,value);}goto retired;
  }
  if(group3){
   unsigned width=opcode==0xf6?8u:bits;Pc98Operand386 o=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;if(o.reg==1)goto unsupported;
   uint32_t immediate=o.reg==0?immediate386(c,&fetch,width):0;if(c->fault.valid)goto failure;
   if(!o.is_register&&!segment_ready386(s,o.segment))goto unsupported;
   unsigned modify=o.reg==2||o.reg==3;uint32_t physical=0,value;
   if(o.is_register)value=read_general386(s,o.rm,width);else{if(!memory_address(s,b,&o,width/8,modify?PC98_ACCESS_WRITE:PC98_ACCESS_READ,&physical))goto failure;value=read_value386(b,physical,width);}
   if(o.reg>=4){
    if(width==32){uint32_t low,high;if(!pc98_cpu_muldiv386(o.reg,reg(s,PC98_AX,32),reg(s,PC98_DX,32),value,&flags,&low,&high)){c->fault=(Pc98CpuFault16){.valid=1,.vector=0};goto failure;}write_reg(s,PC98_AX,32,low);write_reg(s,PC98_DX,32,high);}
    else{uint16_t low_flags=(uint16_t)flags,ax,dx;if(!pc98_cpu_muldiv(o.reg,width,c->r[PC98_AX],c->r[PC98_DX],(uint16_t)value,&low_flags,&ax,&dx)){c->fault=(Pc98CpuFault16){.valid=1,.vector=0};goto failure;}flags=(flags&0xffff0000u)|low_flags;c->r[PC98_AX]=ax;c->r[PC98_DX]=dx;}
   }else{
    uint32_t result=o.reg==2?~value:alu_width386(o.reg==3?PC98_NEG:PC98_AND,width,value,immediate,&flags);
    if(modify){if(o.is_register)write_general386(s,o.rm,width,result);else write_value386(b,physical,width,result);}
   }
   goto retired;
  }
  if(unary_alu){
   unsigned width=opcode==0xfe?8u:bits;Pc98Operand386 o={.is_register=1,.rm=opcode&7u,.reg=opcode>=0x48};
   if(opcode==0xfe||opcode==0xff){o=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;if(o.reg>1)goto unsupported;}
   if(!o.is_register&&!segment_ready386(s,o.segment))goto unsupported;
   uint32_t physical=0,value;
   if(o.is_register)value=read_general386(s,o.rm,width);else{if(!memory_address(s,b,&o,width/8,PC98_ACCESS_WRITE,&physical))goto failure;value=read_value386(b,physical,width);}
   value=alu_width386(o.reg?PC98_DEC:PC98_INC,width,value,0,&flags);
   if(o.is_register)write_general386(s,o.rm,width,value);else write_value386(b,physical,width,value);goto retired;
  }
  if(basic_alu){
   unsigned width=opcode<0x40?(opcode&1u?bits:8u):(opcode==0x80||opcode==0x84||opcode==0xa8?8u:bits),operation=0,write=1;
   if((opcode<0x40&&(opcode&7u)>=4u)||opcode==0xa8||opcode==0xa9){
    operation=opcode<0x40?(opcode>>3)&7u:PC98_AND;uint32_t right=immediate386(c,&fetch,width);if(c->fault.valid)goto failure;
    uint32_t value=alu_width386((enum Pc98AluOp)operation,width,read_general386(s,PC98_AX,width),right,&flags);
    if(opcode<0x40&&operation!=PC98_CMP){write_general386(s,PC98_AX,width,value);}goto retired;
   }
   Pc98Operand386 o=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;
   unsigned group=opcode==0x80||opcode==0x81||opcode==0x83,to_reg=opcode<0x40&&(opcode&2u)!=0,test=opcode==0x84||opcode==0x85;
   uint32_t left=0,right=0,physical=0;
   operation=group?o.reg:test?PC98_AND:(opcode>>3)&7u;write=!test&&operation!=PC98_CMP;
   if(group){if(opcode==0x83){right=pc98_cpu_fetch8(c,&fetch);if(right&128u)right|=0xffffff00u;}else right=immediate386(c,&fetch,width);if(c->fault.valid)goto failure;}
   if(!o.is_register&&!segment_ready386(s,o.segment))goto unsupported;
   uint32_t operand;if(o.is_register)operand=read_general386(s,o.rm,width);else{if(!memory_address(s,b,&o,width/8,!to_reg&&write?PC98_ACCESS_WRITE:PC98_ACCESS_READ,&physical))goto failure;operand=read_value386(b,physical,width);}
   if(to_reg){left=read_general386(s,o.reg,width);right=operand;}else{left=operand;if(!group)right=read_general386(s,o.reg,width);}
   uint32_t value=alu_width386((enum Pc98AluOp)operation,width,left,right,&flags);
   if(write){if(to_reg)write_general386(s,o.reg,width,value);else if(o.is_register)write_general386(s,o.rm,width,value);else write_value386(b,physical,width,value);}goto retired;
  }
  if(basic_move){
   unsigned width=opcode>=0xb0&&opcode<=0xbf?(opcode<0xb8?8u:bits):(opcode&1u?bits:8u);
   if(opcode>=0xb0&&opcode<=0xbf){uint32_t value=immediate386(c,&fetch,width);if(c->fault.valid)goto failure;write_general386(s,opcode&7u,width,value);goto retired;}
   Pc98Operand386 o=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;
   unsigned immediate=opcode==0xc6||opcode==0xc7,load=opcode==0x8a||opcode==0x8b;
   if(immediate&&o.reg)goto unsupported;
   uint32_t value=immediate?immediate386(c,&fetch,width):read_general386(s,o.reg,width),physical=0;if(c->fault.valid)goto failure;
   if(!o.is_register&&!segment_ready386(s,o.segment))goto unsupported;
   if(load){
    if(o.is_register)value=read_general386(s,o.rm,width);
    else{if(!memory_address(s,b,&o,width/8,PC98_ACCESS_READ,&physical))goto failure;value=read_value386(b,physical,width);}
    write_general386(s,o.reg,width,value);
   }else{
    if(o.is_register)write_general386(s,o.rm,width,value);
    else{if(!memory_address(s,b,&o,width/8,PC98_ACCESS_WRITE,&physical))goto failure;write_value386(b,physical,width,value);}
   }
   goto retired;
  }
  if(opcode==0xc4||opcode==0xc5){
   Pc98Operand386 o=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;
   int result=load_pointer386(s,b,&o,opcode==0xc4?PC98_ES:PC98_DS,32);if(result<0)goto unsupported;if(!result)goto failure;goto retired;
  }
  if(opcode==0x9a||opcode==0xea){
   uint32_t target=immediate32(c,&fetch);uint16_t selector=pc98_cpu_fetch16(c,&fetch);if(c->fault.valid)goto failure;
   int result=real_far386(s,b,opcode==0x9a,selector,target);if(result<0)goto unsupported;if(!result)goto failure;goto retired;
  }
  if(opcode==0xca||opcode==0xcb){
   uint16_t release=opcode==0xca?pc98_cpu_fetch16(c,&fetch):0;if(c->fault.valid)goto failure;
   if(!real_far_available386(b)||PC98_V86(s))goto unsupported;
   Pc98Operand386 stack={.segment=PC98_SS,.offset=c->r[PC98_SP]};uint32_t physical;
   if(!memory_address(s,b,&stack,8,PC98_ACCESS_READ,&physical))goto failure;
   uint32_t target=read_value386(b,physical,32),selector=read_value386(b,(physical+4u)&0xffffffu,32);
   int result=real_far386(s,b,0,(uint16_t)selector,target);if(result<0)goto unsupported;if(!result)goto failure;
   c->r[PC98_SP]=(uint16_t)(c->r[PC98_SP]+8u+release);goto retired;
  }
  if(io_string_opcode){
   unsigned output=(opcode&2u)!=0,io_bits=opcode&1u?bits:8u,size=io_bits/8u,index=output?PC98_SI:PC98_DI,index_bits=address32?32u:16u;uint32_t count=reg(s,PC98_CX,index_bits);
   if(repeat==0xf2)goto unsupported;
   if(repeat&&!count)goto retired;
   if(repeat&&count>1&&b->single_step&&(c->flags&PC98_TF))goto unsupported;
   if(PC98_V86(s)||(b->resolve&&!b->real_mode&&pc98_cpu_cpl16(c)>((c->flags>>12)&3u)))goto unsupported;
   unsigned missing=output?(io_bits==8?!b->out8:io_bits==16?!b->out16:!b->out32):(io_bits==8?!b->in8:io_bits==16?!b->in16:!b->in32);if(missing)goto unsupported;
   if(output&&segment>=0&&!segment_ready386(s,(unsigned)segment))goto unsupported;
   Pc98Operand386 memory={.segment=output?(segment<0?PC98_DS:(unsigned)segment):PC98_ES,.offset=reg(s,index,index_bits)};uint32_t physical;
   if(!memory_address(s,b,&memory,size,output?PC98_ACCESS_READ:PC98_ACCESS_WRITE,&physical))goto failure;
   uint16_t port=c->r[PC98_DX];
   if(output){uint32_t value=read_value386(b,physical,io_bits);if(io_bits==8)b->out8(b->context,port,(uint8_t)value);else if(io_bits==16)b->out16(b->context,port,(uint16_t)value);else b->out32(b->context,port,value);}
   else{uint32_t value=io_bits==8?b->in8(b->context,port):io_bits==16?b->in16(b->context,port):b->in32(b->context,port);write_value386(b,physical,io_bits,value);}
   uint32_t delta=flags&PC98_DF?0u-size:size;write_reg(s,index,index_bits,reg(s,index,index_bits)+delta);
   if(repeat){count--;write_reg(s,PC98_CX,index_bits,count);if(count){c->ip=before.low.ip;complete=0;}}
   goto retired;
  }
  if(opcode==0xe5||opcode==0xed||opcode==0xe7||opcode==0xef){
   uint16_t port=(opcode==0xe5||opcode==0xe7)?pc98_cpu_fetch8(c,&fetch):c->r[PC98_DX];if(c->fault.valid)goto failure;
   /* Original386 lower-privilege IO needs the386 TSS permission bitmap.
    * Do not substitute286's unconditional#GP or invoke a device first. */
   if(PC98_V86(s)||(b->resolve&&!b->real_mode&&pc98_cpu_cpl16(c)>((c->flags>>12)&3u)))goto unsupported;
   if(opcode==0xe5||opcode==0xed){if(!b->in32)goto unsupported;write_reg(s,PC98_AX,32,b->in32(b->context,port));}
   else{if(!b->out32)goto unsupported;b->out32(b->context,port,reg(s,PC98_AX,32));}
   goto retired;
  }
  if(string_opcode){
   unsigned base=opcode&~1u,string_bits=opcode&1u?bits:8u,size=string_bits/8u;
   unsigned source=base==0xa4||base==0xa6||base==0xac,destination=base!=0xac,store=base==0xa4||base==0xaa,compare=base==0xa6||base==0xae,index_bits=address32?32u:16u;
   uint32_t count=reg(s,PC98_CX,index_bits);
   /* Only documented REP/REPE/REPNE combinations; original B1 REP MOVS
    * single-step has stepping-specific odd-iteration behavior. */
   if(repeat==0xf2&&!compare)goto unsupported;
   if(repeat&&!count)goto retired;
   if(repeat&&count>1&&b->single_step&&(c->flags&PC98_TF))goto unsupported;
   if(source&&segment>=0&&!segment_ready386(s,(unsigned)segment))goto unsupported;
   Pc98Operand386 from={.segment=segment<0?PC98_DS:(unsigned)segment,.offset=reg(s,PC98_SI,index_bits)},to={.segment=PC98_ES,.offset=reg(s,PC98_DI,index_bits)};
   uint32_t aphysical=0,dphysical=0;
   /* Both complete operands preflight before endpoint reads or stores. */
   if(source&&!memory_address(s,b,&from,size,PC98_ACCESS_READ,&aphysical))goto failure;
   if(destination&&!memory_address(s,b,&to,size,store?PC98_ACCESS_WRITE:PC98_ACCESS_READ,&dphysical))goto failure;
   uint32_t a=source?read_value386(b,aphysical,string_bits):reg(s,PC98_AX,32);
   if(store)write_value386(b,dphysical,string_bits,a);
   else if(base==0xac){if(string_bits==8)c->r[PC98_AX]=(uint16_t)((c->r[PC98_AX]&0xff00u)|(a&255u));else write_reg(s,PC98_AX,string_bits,a);}
   else{
    uint32_t d=read_value386(b,dphysical,string_bits);
    if(string_bits==32)(void)pc98_cpu_alu386(PC98_CMP,a,d,&flags);
    else{uint16_t low=(uint16_t)flags;(void)pc98_cpu_alu(PC98_CMP,string_bits,(uint16_t)a,(uint16_t)d,&low);flags=(flags&0xffff0000u)|low;}
   }
   uint32_t delta=flags&PC98_DF?0u-size:size;
   if(source)write_reg(s,PC98_SI,index_bits,reg(s,PC98_SI,index_bits)+delta);
   if(destination)write_reg(s,PC98_DI,index_bits,reg(s,PC98_DI,index_bits)+delta);
   if(repeat){
    count--;write_reg(s,PC98_CX,index_bits,count);
    unsigned again=count!=0;if(compare)again=again&&((repeat==0xf3)==((flags&PC98_ZF)!=0));
    if(again){c->ip=before.low.ip;complete=0;}
   }
   goto retired;
  }
  if(opcode==0x98||opcode==0x99){
   if(opcode==0x98)s->high[PC98_AX]=(uint16_t)(c->r[PC98_AX]&0x8000u?0xffffu:0u);
   else write_reg(s,PC98_DX,32,s->high[PC98_AX]&0x8000u?UINT32_MAX:0u);
   goto retired;
  }
  if(opcode>=0x90&&opcode<=0x97){
   unsigned first=PC98_AX,second=opcode&7u;
   uint32_t a=reg(s,first,32),d=reg(s,second,32);write_reg(s,first,32,d);write_reg(s,second,32,a);goto retired;
  }
  if(moffs_move){
   unsigned width=opcode&1u?bits:8u,output=(opcode&2u)!=0;
   uint32_t offset=address32?immediate32(c,&fetch):pc98_cpu_fetch16(c,&fetch);if(c->fault.valid)goto failure;
   if(segment>=0&&!segment_ready386(s,(unsigned)segment))goto unsupported;
   Pc98Operand386 o={.segment=segment<0?PC98_DS:(unsigned)segment,.offset=offset};uint32_t physical;
   if(!memory_address(s,b,&o,width/8,output?PC98_ACCESS_WRITE:PC98_ACCESS_READ,&physical))goto failure;
   if(output)write_value386(b,physical,width,read_general386(s,PC98_AX,width));else write_general386(s,PC98_AX,width,read_value386(b,physical,width));
   goto retired;
  }
  if(opcode==0xcf){
   if(!real_far_available386(b)||PC98_V86(s))goto unsupported;
   /* IRET (16-bit frame) and IRETD (66 CF, 12-byte frame, real mode only). EIP above FFFF raises #GP(0)
    * in real_far386. IRETD loads the low 16 EFLAGS bits like IRET; the stack's VM/RF bits are not taken
    * (VM stays as it is, RF clears at retirement). */
   unsigned frame=bits==32?12u:6u,unit=bits/8u;
   Pc98Operand386 stack={.segment=PC98_SS,.offset=c->r[PC98_SP]};uint32_t physical;
   if(!memory_address(s,b,&stack,frame,PC98_ACCESS_READ,&physical))goto failure;
   uint32_t target=read_value386(b,physical,bits),selector=b->read16(b->context,(physical+unit)&0xffffffu),image=read_value386(b,(physical+2u*unit)&0xffffffu,bits);
   int result=real_far386(s,b,0,(uint16_t)selector,target);if(result<0)goto unsupported;if(!result)goto failure;
   c->r[PC98_SP]=(uint16_t)(c->r[PC98_SP]+frame);c->nmi_active=0;
   /* Original386 real IRET restores NT/IOPL. B1 specification change
    * explicitly clears RF for16-bit IRET; ordinary retirement below does it. */
   flags=(flags&0xffff0000u)|(image&0x7fd5u)|2u;goto retired;
  }
  if(bits==32&&(opcode==0xc8||opcode==0xc9)){
   /* ENTER / LEAVE with a 32-bit operand size on the 16-bit stack-address attribute (PRM ENTER/LEAVE
    * pseudo-code): dword frame slots, EBP receives the zero-extended 16-bit frame pointer, with the same
    * wrap/underflow rejections as the 16-bit core's ENTER/LEAVE. */
   if(opcode==0xc9){
    uint16_t bp=c->r[PC98_BP];
    Pc98Operand386 stack={.segment=PC98_SS,.offset=bp};uint32_t physical;
    if(!memory_address(s,b,&stack,4,PC98_ACCESS_READ,&physical))goto failure;
    uint32_t value=read_value386(b,physical,32);
    c->r[PC98_SP]=(uint16_t)(bp+4u);write_reg(s,PC98_BP,32,value);goto retired;
   }
   uint16_t allocation=pc98_cpu_fetch16(c,&fetch);unsigned level=pc98_cpu_fetch8(c,&fetch)&31u;if(c->fault.valid)goto failure;
   unsigned words=level?level+1u:1u;uint16_t sp=c->r[PC98_SP],source=c->r[PC98_BP];
   if((!b->resolve||b->real_mode)&&sp>0&&sp<4u*words)goto unsupported;
   uint16_t end=(uint16_t)(sp-4u*words);
   if(b->resolve){
    uint16_t lowest=(uint16_t)(end-allocation);Pc98Operand386 region={.segment=PC98_SS,.offset=lowest};uint32_t ignored;
    if(!memory_address(s,b,&region,4u*words+allocation,PC98_ACCESS_WRITE,&ignored))goto failure;
    for(unsigned n=1;n<level;n++){source=(uint16_t)(source-4u);Pc98Operand386 from={.segment=PC98_SS,.offset=source};if(!memory_address(s,b,&from,4,PC98_ACCESS_READ,&ignored))goto failure;}
   }else{
    if(allocation>end)goto unsupported;
    for(unsigned n=1;n<level;n++){source=(uint16_t)(source-4u);if(source>0xfffcu)goto unsupported;}
   }
   int result=push386(s,b,reg(s,PC98_BP,32));if(result<0)goto unsupported;if(!result)goto failure;
   uint16_t frame=c->r[PC98_SP];source=c->r[PC98_BP];
   for(unsigned n=1;n<level;n++){
    source=(uint16_t)(source-4u);Pc98Operand386 from={.segment=PC98_SS,.offset=source};uint32_t physical;
    if(!memory_address(s,b,&from,4,PC98_ACCESS_READ,&physical))goto failure;
    result=push386(s,b,read_value386(b,physical,32));if(result<0)goto unsupported;if(!result)goto failure;
   }
   if(level){result=push386(s,b,frame);if(result<0)goto unsupported;if(!result)goto failure;}
   write_reg(s,PC98_BP,32,frame);c->r[PC98_SP]=(uint16_t)(c->r[PC98_SP]-allocation);goto retired;
  }
  if(opcode==0x9c||opcode==0x9d){
   /* V86 mode needs its own mode/IOPL adapter; no stack accesses here. */
   if(PC98_V86(s))goto unsupported;
   if(opcode==0x9c){
    /* PRM3.8: PUSHFD does not transfer RF/VM; reserved bits normalized. */
    if(bits==32){int result=push386(s,b,(flags&0x7fd5u)|2u);if(result<0)goto unsupported;if(!result)goto failure;}
    else{
     if((!b->resolve||b->real_mode)&&c->r[PC98_SP]==1)goto unsupported;
     uint16_t next=(uint16_t)(c->r[PC98_SP]-2u);Pc98Operand386 stack={.segment=PC98_SS,.offset=next};uint32_t physical;
     if(!memory_address(s,b,&stack,2,PC98_ACCESS_WRITE,&physical))goto failure;
     b->write16(b->context,physical,(uint16_t)((flags&0x7fd5u)|2u));c->r[PC98_SP]=next;
    }
   }else{
    Pc98Operand386 stack={.segment=PC98_SS,.offset=c->r[PC98_SP]};uint32_t physical;
    if(!memory_address(s,b,&stack,bits/8,PC98_ACCESS_READ,&physical))goto failure;
    uint32_t value=read_value386(b,physical,bits);
    unsigned protected_mode=b->resolve&&!b->real_mode,cpl=protected_mode?pc98_cpu_cpl16(c):0,iopl=(c->flags>>12)&3u;
    uint32_t writable=0x4dd5u;if(!cpl)writable|=0x3000u;if(!protected_mode||cpl<=iopl)writable|=PC98_IF;
    /* PRM3.8 and17-130: POPFD leaves RF/VM unchanged. Chapter12.3.1.1
     * has a contradictory generic POPF sentence; no later CPU assumed. */
    flags=(flags&0xffff0000u)|((flags&0x7fd5u&~writable)|(value&writable)|2u);
    c->r[PC98_SP]=(uint16_t)(c->r[PC98_SP]+bits/8);
   }
   goto retired;
  }
  if(opcode==0x60||opcode==0x61){
   unsigned pop=opcode==0x61;uint16_t old=c->r[PC98_SP],next=(uint16_t)(old+(pop?32u:0xffe0u));
   /* Original386 real low-SP shutdown/wrap variants need mode integration.
    * Do not approximate them by a generic frame fault or partial writes. */
   if(!pop&&(!b->resolve||b->real_mode)&&old>0&&old<32)goto unsupported;
   Pc98Operand386 stack={.segment=PC98_SS,.offset=pop?old:next};uint32_t physical;
   if(!memory_address(s,b,&stack,32,pop?PC98_ACCESS_READ:PC98_ACCESS_WRITE,&physical))goto failure;
   uint32_t values[8];
   if(pop){
    /* 1986 PRM models the discarded ESP slot as a Pop, too. Capture the
     * complete frame before changing registers; microtiming not modelled. */
    for(unsigned i=0;i<8;i++){uint32_t a=(physical+4u*i)&0xffffffu;values[i]=b->read16(b->context,a)|((uint32_t)b->read16(b->context,(a+2u)&0xffffffu)<<16);}
    for(unsigned i=0;i<8;i++)if(7u-i!=PC98_SP)write_reg(s,7u-i,32,values[i]);
   }else{
    for(unsigned i=0;i<8;i++)values[i]=reg(s,7u-i,32);
    for(unsigned i=0;i<8;i++){uint32_t a=(physical+4u*i)&0xffffffu;b->write16(b->context,a,(uint16_t)values[i]);b->write16(b->context,(a+2u)&0xffffffu,(uint16_t)(values[i]>>16));}
   }
   c->r[PC98_SP]=next;goto retired;
  }
  if((opcode>=0x50&&opcode<=0x57)||opcode==0x68||opcode==0x6a){
   uint32_t value;if(opcode==0x68)value=immediate32(c,&fetch);else if(opcode==0x6a){value=pc98_cpu_fetch8(c,&fetch);if(value&128u)value|=0xffffff00u;}else value=reg(s,opcode&7u,32);
   if(c->fault.valid)goto failure;
   int result=push386(s,b,value);if(result<0)goto unsupported;if(!result)goto failure;goto retired;
  }
  if((opcode>=0x58&&opcode<=0x5f)||opcode==0x8f){
   Pc98Operand386 destination={.is_register=1,.rm=opcode&7u};
   if(opcode==0x8f){destination=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;if(destination.reg)goto unsupported;}
   if(!destination.is_register&&!segment_ready386(s,destination.segment))goto unsupported;
   uint16_t next=(uint16_t)(c->r[PC98_SP]+4u);Pc98Operand386 stack={.segment=PC98_SS,.offset=c->r[PC98_SP]};uint32_t source_physical,destination_physical=0;
   if(!memory_address(s,b,&stack,4,PC98_ACCESS_READ,&source_physical))goto failure;
   if(!destination.is_register){
    /* Intel80386 B1 DesignNote2: explicit ESP base sees the incremented
     * value. For a 16-bit stack, preserve the upper ESP word on low wrap. */
    if(destination.sp_base)destination.offset+=(uint32_t)next-(uint32_t)c->r[PC98_SP];
    if(!memory_address(s,b,&destination,4,PC98_ACCESS_WRITE,&destination_physical))goto failure;
   }
   uint32_t value=b->read16(b->context,source_physical);value|=(uint32_t)b->read16(b->context,(source_physical+2u)&0xffffffu)<<16;
   if(destination.is_register){c->r[PC98_SP]=next;write_reg(s,destination.rm,32,value);}
   else{b->write16(b->context,destination_physical,(uint16_t)value);b->write16(b->context,(destination_physical+2u)&0xffffffu,(uint16_t)(value>>16));c->r[PC98_SP]=next;}
   goto retired;
  }

  if(opcode==0xe8||opcode==0xe9){
   uint32_t displacement=immediate32(c,&fetch);if(c->fault.valid)goto failure;uint32_t target=(uint32_t)c->ip+displacement;
   if(opcode==0xe8){int result=near_call386(s,b,target);if(result<0)goto unsupported;if(!result)goto failure;}
   else{if(!near_target386(c,b,target))goto failure;c->ip=(uint16_t)target;}
   goto retired;
  }
  if(opcode==0xc2||opcode==0xc3){
   uint16_t release=opcode==0xc2?pc98_cpu_fetch16(c,&fetch):0;if(c->fault.valid)goto failure;
   Pc98Operand386 stack={.segment=PC98_SS,.offset=c->r[PC98_SP]};uint32_t physical,target=read_operand32(s,b,&stack,PC98_ACCESS_READ,&physical);if(c->fault.valid||!near_target386(c,b,target))goto failure;
   c->r[PC98_SP]=(uint16_t)(c->r[PC98_SP]+4u+release);c->ip=(uint16_t)target;goto retired;
  }

  if(imul_literal){
   Pc98Operand386 o=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;if(!o.is_register&&!segment_ready386(s,o.segment))goto unsupported;
   uint32_t immediate;if(opcode==0x69)immediate=immediate386(c,&fetch,bits);else{immediate=pc98_cpu_fetch8(c,&fetch);if(immediate&128u)immediate|=0xffffff00u;}if(c->fault.valid)goto failure;
   uint32_t physical=0,value;if(o.is_register)value=reg(s,o.rm,bits);else{if(!memory_address(s,b,&o,bits/8,PC98_ACCESS_READ,&physical))goto failure;value=read_value386(b,physical,bits);}
   write_reg(s,o.reg,bits,imul_width386(bits,value,immediate,&flags));goto retired;
  }
  if(opcode==0xff){
   Pc98Operand386 o=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;if(!o.is_register&&!segment_ready386(s,o.segment))goto unsupported;
   unsigned source=o.reg;uint32_t physical=0;
   if(source==3||source==5){
    if(!real_far_available386(b)||PC98_V86(s)||(source==3&&c->r[PC98_SP]>0&&c->r[PC98_SP]<8))goto unsupported;
    if(o.is_register){c->fault=(Pc98CpuFault16){.valid=1,.vector=6};goto failure;}
    if(!memory_address(s,b,&o,6,PC98_ACCESS_READ,&physical))goto failure;
    uint32_t target=read_value386(b,physical,32);uint16_t selector=b->read16(b->context,(physical+4u)&0xffffffu);
    int result=real_far386(s,b,source==3,selector,target);if(result<0)goto unsupported;if(!result)goto failure;goto retired;
   }
   if(source<2){uint32_t value=read_operand32(s,b,&o,PC98_ACCESS_WRITE,&physical);if(c->fault.valid)goto failure;value=pc98_cpu_alu386(source==0?PC98_INC:PC98_DEC,value,0,&flags);if(o.is_register)write_reg(s,o.rm,32,value);else write_value386(b,physical,32,value);goto retired;}
   if(source!=2&&source!=4&&source!=6)goto unsupported;
   if((source==2||source==6)&&(!b->resolve||b->real_mode)&&c->r[PC98_SP]>0&&c->r[PC98_SP]<4)goto unsupported;
   uint32_t target=read_operand32(s,b,&o,PC98_ACCESS_READ,&physical);if(c->fault.valid)goto failure;
   if(source==6){int result=push386(s,b,target);if(result<0)goto unsupported;if(!result)goto failure;goto retired;}
   if(source==2){int result=near_call386(s,b,target);if(result<0)goto unsupported;if(!result)goto failure;}else{if(!near_target386(c,b,target))goto failure;c->ip=(uint16_t)target;}goto retired;
  }
  goto unsupported;
 }
 unsigned second=pc98_cpu_fetch8(c,&fetch);if(c->fault.valid)goto failure;
 if(second>=0x80&&second<=0x8f){
  uint32_t displacement=bits==32?immediate32(c,&fetch):pc98_cpu_fetch16(c,&fetch);if(c->fault.valid)goto failure;
  if(condition386(flags,second&15u)){
   uint32_t target=(uint32_t)c->ip+displacement;if(bits==16)target&=0xffffu;
   if(!near_target386(c,b,target))goto failure;
   c->ip=(uint16_t)target;
  }
  goto retired;
 }

 if(second==0xa0||second==0xa1||second==0xa8||second==0xa9){
  /* PUSH/POP FS/GS, 16- and 32-bit operand size. PUSH stores the selector zero-extended; POP takes the low
   * word of the slot, loads it through the same FS/GS loader as MOV FS,r/m16 and adds the operand size to SP
   * only after the load succeeded. */
  unsigned extra=second>=0xa8u,pop=(second&1u)!=0,unit=bits/8u;
  if(PC98_V86(s)||!s->extra[extra].initialized)goto unsupported;
  if(!pop){
   if((!b->resolve||b->real_mode)&&c->r[PC98_SP]>0&&c->r[PC98_SP]<unit)goto unsupported;
   uint16_t next=(uint16_t)(c->r[PC98_SP]-unit);Pc98Operand386 stack={.segment=PC98_SS,.offset=next};uint32_t physical;
   if(!memory_address(s,b,&stack,unit,PC98_ACCESS_WRITE,&physical))goto failure;
   write_value386(b,physical,bits,s->extra[extra].selector);c->r[PC98_SP]=next;goto retired;
  }
  if(b->resolve&&!b->real_mode&&!b->load_extra_segment)goto unsupported;
  Pc98Operand386 stack={.segment=PC98_SS,.offset=c->r[PC98_SP]};uint32_t physical;
  if(!memory_address(s,b,&stack,unit,PC98_ACCESS_READ,&physical))goto failure;
  uint16_t selector=b->read16(b->context,physical);
  Pc98ExtraSegmentCache386 cache={0};
  if(b->resolve&&!b->real_mode){
   Pc98CpuFault16 failure={0};int result=b->load_extra_segment(b->extra_segment_context,c,b,selector,&cache,&failure);
   if(result<0)goto unsupported;
   if(!result){failure.valid=1;c->fault=failure;goto failure;}
  }else{cache.selector=selector;cache.base=(uint32_t)selector<<4;cache.limit=0xffff;cache.access=0x93;cache.initialized=cache.valid=1;}
  s->extra[extra]=cache;c->r[PC98_SP]=(uint16_t)(c->r[PC98_SP]+unit);goto retired;
 }
 if(second==0x20||second==0x22){
  /* MOV r32,CRn / MOV CRn,r32: always register form (the mod bits are ignored, no displacement), 32-bit. */
  unsigned modrm=pc98_cpu_fetch8(c,&fetch);if(c->fault.valid)goto failure;
  unsigned number=(modrm>>3)&7u,gpr=modrm&7u,write=second==0x22;
  if(!b->control_register386)goto unsupported;
  uint32_t value=write?reg(s,gpr,32):0;Pc98CpuFault16 refusal={0};
  int result=b->control_register386(b->control_context,c,write,number,&value,&refusal);
  if(result<0)goto unsupported;
  if(!result){refusal.valid=1;c->fault=refusal;goto failure;}
  if(!write)write_reg(s,gpr,32,value);
  goto retired;
 }
 if(second==0xb2||second==0xb4||second==0xb5){
  Pc98Operand386 o=operand386(s,&fetch,address32,segment);if(c->fault.valid)goto failure;
  unsigned target=second==0xb2?PC98_SS:second==0xb4?4u:5u;
  int result=load_pointer386(s,b,&o,target,bits);if(result<0)goto unsupported;if(!result)goto failure;goto retired;
 }
 if(!(second>=0x90&&second<=0x9f)&&second!=0xb6&&second!=0xb7&&second!=0xbe&&second!=0xbf&&second!=0xaf&&second!=0xbc&&second!=0xbd&&second!=0xa3&&second!=0xab&&second!=0xb3&&second!=0xbb&&second!=0xba&&second!=0xa4&&second!=0xa5&&second!=0xac&&second!=0xad)goto unsupported;
 Pc98Operand386 operand=operand386(s,&fetch,address32,segment);
 unsigned source=operand.reg,destination=operand.rm,is_register=operand.is_register,memory_segment=operand.segment;
 if(c->fault.valid)goto failure;
 if(second>=0x90&&second<=0x9f){
  unsigned value=condition386(flags,second&15u);
  if(is_register){unsigned r=destination&3u,shift=destination<4?0:8;uint32_t mask=255u<<shift;c->r[r]=(uint16_t)(((uint32_t)c->r[r]&~mask)|((uint32_t)value<<shift));}
  else{if(!segment_ready386(s,memory_segment))goto unsupported;uint32_t physical;if(!memory_address(s,b,&operand,1,PC98_ACCESS_WRITE,&physical))goto failure;b->write8(b->context,physical,(uint8_t)value);}
  goto retired;
 }
 if(second==0xb6||second==0xb7||second==0xbe||second==0xbf){
  unsigned source_bits=(second&1u)?16:8;if(source_bits==16&&bits==16)goto unsupported;
  uint32_t value;
  if(is_register)value=source_bits==16?c->r[destination]:((uint32_t)c->r[destination&3u]>>(destination<4?0:8))&255u;
  else{if(!segment_ready386(s,memory_segment))goto unsupported;uint32_t physical;if(!memory_address(s,b,&operand,source_bits/8,PC98_ACCESS_READ,&physical))goto failure;value=source_bits==8?b->read8(b->context,physical):b->read16(b->context,physical);}
  if((second&8u)&&(value&(UINT32_C(1)<<(source_bits-1))))value|=source_bits==8?0xffffff00u:0xffff0000u;
  write_reg(s,source,bits,value);goto retired;
 }
 if(second==0xa4||second==0xa5||second==0xac||second==0xad){
  unsigned count=(second&1u)?c->r[PC98_CX]&255u:pc98_cpu_fetch8(c,&fetch),amount=count&31u;
  if(c->fault.valid)goto failure;
  /* Undefined16-bit large counts remain unsupported. A zero count reads the operand but does not write it
   * (PRM 17-156/158: nothing is assigned); WRITE rights are still checked before that read. */
  if(amount>=bits)goto unsupported;
  uint32_t other=reg(s,source,bits),value,result,physical=0;
  if(is_register)value=reg(s,destination,bits);
  else{
   if(!segment_ready386(s,memory_segment))goto unsupported;
   if(!memory_address(s,b,&operand,bits/8,PC98_ACCESS_WRITE,&physical))goto failure;
   value=b->read16(b->context,physical);if(bits==32)value|=(uint32_t)b->read16(b->context,(physical+2u)&0xffffffu)<<16;
  }
  (void)pc98_cpu_double_shift386(second>=0xac,bits,value,other,count,&flags,&result);
  if(amount){if(is_register)write_reg(s,destination,bits,result);else{b->write16(b->context,physical,(uint16_t)result);if(bits==32)b->write16(b->context,(physical+2u)&0xffffffu,(uint16_t)(result>>16));}}
  goto retired;
 }
 uint32_t value=0,other=reg(s,source,bits),physical=0;unsigned index=(unsigned)other;
 unsigned bit_family=second==0xa3||second==0xab||second==0xb3||second==0xbb||second==0xba;
 enum Pc98Bit386 bit_op=PC98_BIT_TEST386;
 if(bit_family){
  if(second==0xba){if(source<4)goto unsupported;bit_op=source==4?PC98_BIT_TEST386:source==5?PC98_BIT_SET386:source==6?PC98_BIT_RESET386:PC98_BIT_COMPLEMENT386;index=pc98_cpu_fetch8(c,&fetch);if(c->fault.valid)goto failure;}
  else bit_op=second==0xa3?PC98_BIT_TEST386:second==0xab?PC98_BIT_SET386:second==0xb3?PC98_BIT_RESET386:PC98_BIT_COMPLEMENT386;
 }

 if(is_register)value=reg(s,destination,bits);
 else{
  if(!segment_ready386(s,memory_segment))goto unsupported;
  if(bit_family){
   unsigned selected;if(!pc98_cpu_bit_address386(bits,address32?32:16,operand.offset,index,second==0xba,&operand.offset,&selected))goto unsupported;index=selected;
   if(!memory_address(s,b,&operand,bits/8,bit_op==PC98_BIT_TEST386?PC98_ACCESS_READ:PC98_ACCESS_WRITE,&physical))goto failure;
  }else{
   if(second!=0xbc&&second!=0xbd&&second!=0xaf)goto unsupported;
   if(!memory_address(s,b,&operand,bits/8,PC98_ACCESS_READ,&physical))goto failure;
  }
  value=b->read16(b->context,physical);
  if(bits==32)value|=(uint32_t)b->read16(b->context,(physical+2u)&0xffffffu)<<16;
 }
 uint32_t result=value;
 if(second==0xaf){result=imul_width386(bits,other,value,&flags);write_reg(s,source,bits,result);}
 else if(second==0xbc||second==0xbd){
  result=reg(s,source,bits);int found=pc98_cpu_scan386(second==0xbd,bits,value,&result,&flags);
  if(found)write_reg(s,source,bits,result);
 }else{
  result=pc98_cpu_bit386(bit_op,bits,value,index,&flags);
  if(bit_op!=PC98_BIT_TEST386){if(is_register)write_reg(s,destination,bits,result);else{b->write16(b->context,physical,(uint16_t)result);if(bits==32)b->write16(b->context,(physical+2u)&0xffffffu,(uint16_t)(result>>16));}}
 }
retired:
 /* PRM12.3.1.1: RF clears only after successful retirement, except POPF.
  * Unsupported/fault exits retain the incoming RF through the snapshot. */
 if(complete&&opcode!=0x9d)flags&=~UINT32_C(0x10000);
 c->flags=(uint16_t)flags;s->flag_high=(uint16_t)(flags>>16);
 if(!complete)return PC98_STEP_OK;
 if(b->instruction_completed)*b->instruction_completed=1;
 if(c->irq_inhibit)c->irq_inhibit--;
 if(c->nmi_inhibit)c->nmi_inhibit--;
 if(c->debug_inhibit)c->debug_inhibit--;
 if(b->single_step&&(before.low.flags&PC98_TF)&&before.low.debug_inhibit<2){c->fault=(Pc98CpuFault16){.valid=1,.vector=1,.external=1,.restart_ip=c->ip};return PC98_STEP_TRAP;}
 return PC98_STEP_OK;
unsupported:
 *s=before;return PC98_STEP_UNSUPPORTED;
failure:{Pc98CpuFault16 fault=c->fault;fault.restart_ip=before.low.ip;*s=before;s->low.fault=fault;return PC98_STEP_FAULT;}
}
