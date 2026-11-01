/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh */
#include "loadall286.h"
static uint16_t word(const uint8_t *b){return (uint16_t)(b[0]|((unsigned)b[1]<<8));}
static Pc98Descriptor286 cache(const uint8_t *b){return (Pc98Descriptor286){.base=b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16),.access=b[3],.limit=word(b+4)};}
Pc98Loadall286 pc98_cpu_parse_loadall286(const uint8_t b[102]){
 Pc98Loadall286 s={.msw=word(b+6),.tr=word(b+0x16),.flags=word(b+0x18),.ip=word(b+0x1a),.ldtr=word(b+0x1c)};
 for(unsigned i=0;i<4;i++){s.seg[i]=word(b+0x24-2*i);s.cache[i]=cache(b+0x36+6*i);}
 for(unsigned i=0;i<8;i++)s.r[i]=word(b+0x34-2*i);
 Pc98Descriptor286 g=cache(b+0x4e),t=cache(b+0x5a);s.gdt=(Pc98Table286){g.base,g.limit};s.idt=(Pc98Table286){t.base,t.limit};s.ldt=cache(b+0x54);s.tss=cache(b+0x60);return s;
}
