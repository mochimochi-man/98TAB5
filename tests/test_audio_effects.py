#!/usr/bin/env python3
"""Exercise the actual codec-side DSP without ESP-IDF hardware."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'main/audio_codec.cpp').read_text()
a=s.index('// ---- sound effects');b=s.index('// ---- core 0: drain',a)
code='''#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>
#define MALLOC_CAP_SPIRAM 0
static void *heap_caps_malloc(size_t n,int) {return malloc(n);}
extern "C" void fm_orchestra_enable(int) {}
#define RING_FRAME 8
'''+s[a:b]+r'''
int main() {
 assert(fx_init(22050));
 int16_t in[4*512]={0},out[2*512],hall[2*512];
 for(int m=0;m<=4;m++){audio_set_fx(m);assert(audio_get_fx()==(m==1?0:m));}
 audio_set_fx(99);assert(audio_get_fx()==0);
 // Beep bypasses both hall variants with the same direct gain.
 for(int i=0;i<512;i++){in[4*i+2]=1000;in[4*i+3]=-1000;}
 fx_reset();fx_process(in,hall,512,2);fx_reset();fx_process(in,out,512,3);
 assert(!memcmp(hall,out,sizeof hall));
 // Full-scale mix remains bounded; impulse produces a decaying, finite tail.
 for(int mode=1;mode<=4;mode++) {
  fx_reset();long long early=0,late=0;
  memset(in,0,sizeof in);in[0]=in[1]=32767;
  for(int block=0;block<220;block++) {
   fx_process(in,out,512,mode);memset(in,0,sizeof in);
   for(int i=0;i<1024;i++) {if(block<30)early+=abs(out[i]);if(block>200)late+=abs(out[i]);}
  }
  assert(early>0);assert(late<early/100+1);
  for(int i=0;i<2048;i++)in[i]=(i&1)?32767:-32768;
  fx_process(in,out,512,mode);
 }

 // Orchestra keeps the direct lead band while removing the inherited bass
 // boost; this is a fixed filter choice, not an audio-dependent compressor.
 double energy[2][2]={{0}};
 for(int mode=3;mode<=4;mode++)for(int band=0;band<2;band++) {
  fx_reset();double hz=band?880:110;
  for(int block=0;block<100;block++) {
   memset(in,0,sizeof in);
   for(int i=0;i<512;i++)in[4*i]=in[4*i+1]=(int16_t)(1000*sin((block*512+i)*6.283185307179586*hz/22050));
   fx_process(in,out,512,mode);
   if(block>=50)for(int i=0;i<1024;i++)energy[mode-3][band]+=(double)out[i]*out[i];
  }
 }
 double bass=sqrt(energy[1][0]/energy[0][0]),lead=sqrt(energy[1][1]/energy[0][1]);
 fprintf(stderr,"Orchestra EQ vs Synthesizer: bass %.3f, lead %.3f\n",bass,lead);
 assert(bass>.5 && bass<.8 && lead>.9 && lead<1.1);
 puts("PASS: mode validation, beep bypass, full-scale processing, stable decaying DSP tails");
}
'''
# Free delay storage after the checks so leak checking also stays meaningful.
code=code.replace(' puts("PASS:', ''' for(int ch=0;ch<2;ch++){for(int k=0;k<FX_COMBS;k++)free(s_comb[ch][k].buf);for(int k=0;k<FX_ALLPS;k++)free(s_allp[ch][k].buf);}free(s_wide);
 puts("PASS:''')
with tempfile.TemporaryDirectory(prefix='pc98-dsp-') as tmp:
 p=Path(tmp);(p/'test.cpp').write_text(code)
 subprocess.run(['g++','-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unused-function','-fsanitize=address,undefined',str(p/'test.cpp'),'-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
