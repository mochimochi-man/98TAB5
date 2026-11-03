#!/usr/bin/env python3
"""Run the production reset_memory() against all eight graphics planes."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
core=root/'components/pc98core'
s=(core/'machine.c').read_text()
body=re.search(r'static void reset_memory\(void\)\n\{.*?\n\}',s,re.S).group()
code=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <memmap.h>
uint8_t mem[0x200000];
static struct { uint8_t mem_switch[8]; } g_settings;
'''+body+r'''
static void check(void) {
 const unsigned planes[]={GVRAM_B,GVRAM_R,GVRAM_G,GVRAM_E,GVRAM1_B,GVRAM_R+GVRAM_PAGE1,GVRAM_G+GVRAM_PAGE1,GVRAM1_E};
 for(unsigned p=0;p<8;p++) for(unsigned i=0;i<0x8000;i++) assert(mem[planes[p]+i]==0);
 for(unsigned i=0xc0000;i<0xe0000;i++) assert(mem[i]==0xff);
 for(unsigned i=0;i<8;i++) assert(mem[0xa3fe2+4*i]==g_settings.mem_switch[i]);
}
int main(void) {
 for(unsigned i=0;i<8;i++)g_settings.mem_switch[i]=i+1;
 memset(mem,0x5a,sizeof mem);reset_memory();check();
 memset(mem+GVRAM_E,0xff,0x8000);reset_memory();check();
 puts("PASS: all graphics planes clear on reset; expansion holes and memory switches preserved");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(code)
 subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-I'+str(core/'include'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
