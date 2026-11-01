/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "decode386.h"
#include <assert.h>
static uint32_t fetch32(Pc98Cpu16State *c,const Pc98FetchBus *b){uint32_t low=pc98_cpu_fetch16(c,b);return low|((uint32_t)pc98_cpu_fetch16(c,b)<<16);}
Pc98Operand386 pc98_cpu_modrm386(Pc98Cpu16State *c,const Pc98FetchBus *b,const uint32_t r[8],int override){
 assert(override>=-1&&override<=PC98_GS386);
 unsigned byte=pc98_cpu_fetch8(c,b),mod=byte>>6;
 Pc98Operand386 out={.reg=(byte>>3)&7u,.rm=byte&7u,.is_register=mod==3,.segment=PC98_DS};
 if(c->fault.valid||out.is_register)return out;
 unsigned base=out.rm,index=4,scale=0;
 if(base==4){unsigned sib=pc98_cpu_fetch8(c,b);scale=sib>>6;index=(sib>>3)&7u;base=sib&7u;}
 if(c->fault.valid)return out;
 uint32_t offset=0;
 if(!mod&&base==5)offset=fetch32(c,b);
 else{offset=r[base];out.sp_base=base==PC98_SP;if(base==PC98_SP||base==PC98_BP)out.segment=PC98_SS;}
 if(index!=4)offset+=r[index]<<scale;
 if(mod==1){uint32_t displacement=pc98_cpu_fetch8(c,b);offset+=displacement<128?displacement:displacement|0xffffff00u;}
 else if(mod==2)offset+=fetch32(c,b);
 if(override>=0)out.segment=(unsigned)override;
 out.offset=offset;return out;
}
