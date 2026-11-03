#!/usr/bin/env python3
"""Compile real FM renderers; compare unchanged modes, render synthetic audition WAVs."""
from pathlib import Path
import subprocess,tempfile,re,sys,wave,struct,math,json
root=Path(__file__).resolve().parents[1];core=root/'components/pc98core'
output=Path(sys.argv[1]) if len(sys.argv)>1 else None
src=(core/'sound/fm.c').read_text()
# Build the previous signal path from the exact unaffected implementation.
reference=src.replace('\n\torchestra_open(rate);','').replace('!orchestra_enabled() && !f->orchestra_mix && ','')
start=reference.index('        if (orchestra_enabled() || f->orchestra_mix) {')
end=reference.index('\n\t}\n}',start)
reference=reference[:start]+'        lr[0] += v;\n        lr[1] += v;'+reference[end:]
harness=r'''
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <stddef.h>
#include <time.h>
#include <sound.h>
#include "chips.h"
void ref_fm_open(UINT);void ref_fm_set_volume(UINT);void ref_fm_reset(FmChip*);
void ref_fm_write(FmChip*,UINT,REG8);void ref_fm_key(FmChip*,REG8);void ref_fm_render(void*,SINT32*,UINT);
static void patch(FmChip *f,int family,int channel) {
 static const int algorithm[]={0,0,7,4};
 fm_write(f,0xb0+channel,algorithm[family]|((family==1?5:family==0?1:0)<<3));
 for(int op=0;op<4;op++) {
  int off=op*4+channel;
  fm_write(f,0x30+off,1);fm_write(f,0x40+off,op==3||family==2?0:family==1?20:60);
  fm_write(f,0x50+off,31);fm_write(f,0x60+off,family==3?24:0);
  fm_write(f,0x70+off,family==3?12:0);fm_write(f,0x80+off,0x0a);
 }
 fm_write(f,0xa4+channel,0x22);fm_write(f,0xa0+channel,0x69+channel*60);
 fm_key(f,0xf0|channel);
}
int main(int argc,char **argv) {
 assert(argc==2);fm_open(22050);ref_fm_open(22050);fm_set_volume(70);ref_fm_set_volume(70);
 FmChip f,ref;fm_reset(&f);patch(&f,0,0);patch(&f,1,1);patch(&f,2,2);ref=f;
 for(int block=0;block<200;block++) {
  SINT32 a[512]={0},b[512]={0};
  fm_orchestra_enable(0);fm_render(&f,a,256);ref_fm_render(&ref,b,256);
  assert(!memcmp(a,b,sizeof a));
  assert(!memcmp((char*)&f+offsetof(FmChip,ch3mode),(char*)&ref+offsetof(FmChip,ch3mode),sizeof f - offsetof(FmChip,ch3mode)));
 }
 puts("PASS: original FM PCM and device state bit-identical (200 blocks / all three parts)");
 /* Equal native attenuation must stay equal with inverted SSG-EG, including release. */
 for(int release=0;release<2;release++) {
  FmChip normal,inverted;fm_reset(&normal);fm_reset(&inverted);
  normal.ch[0].alg=inverted.ch[0].alg=0;
  FmOp *a=&normal.ch[0].op[3],*b=&inverted.ch[0].op[3];
  a->state=b->state=release?4:3;a->key=b->key=!release;
  a->inc=b->inc=2000;a->mul=b->mul=1;a->tl=b->tl=0;
  a->att=release?128:0;b->att=release?128:512;b->ssg=12;b->ssg_inv=1;
  normal.orchestra[0][3].was_key=inverted.orchestra[0][3].was_key=!release;
  fm_orchestra_enable(1);
  for(int n=0;n<22050;n++) {SINT32 l,r;orchestra_frame(&normal,&l,&r);orchestra_frame(&inverted,&l,&r);}
  assert(normal.orchestra[0][3].envelope>0);
  assert(normal.orchestra[0][3].envelope==inverted.orchestra[0][3].envelope);
 }
 puts("PASS: inverted SSG-EG carrier loudness and release match native attenuation");
 /* Additive/special-mode voices: distinct carrier pitches must all survive.
  * Check the actual oscillators by spectral projection, not state fields alone. */
 for(int variant=1;variant<=2;variant++) {
 for(int rate=22050;rate<=44100;rate*=2) {
  fm_open(rate);FmChip chord;fm_reset(&chord);chord.ch[2].alg=7;chord.ch3mode=1;
  const unsigned inc[]={4000,6500,9000,11500};
  for(int o=0;o<4;o++){FmOp *p=&chord.ch[2].op[o];p->state=3;p->key=1;p->att=0;p->inc=inc[o];p->tl=8;}
  double re[4]={0},im[4]={0};fm_orchestra_enable(variant);
  for(int n=0;n<rate;n++) {
   SINT32 l,r;orchestra_frame(&chord,&l,&r);
   if(n<rate/4)continue;
   for(int o=0;o<4;o++){double phase=6.283185307179586*n*inc[o]*(3993600.0/72.0)/(1048576.0*rate);re[o]+=l*cos(phase);im[o]+=l*sin(phase);}
  }
  for(int o=0;o<4;o++){double level=hypot(re[o],im[o])/(rate*3/4);fprintf(stderr,"carrier rate=%d op=%d level=%f\n",rate,o,level);assert(level>(variant==1?300:50));}
 }
 }
 puts("PASS: all four independent carrier pitches audible at 22050/44100 Hz");
 fm_open(22050);
 /* Large modulator multipliers alone do not turn a sustained lead into percussion. */
 fm_reset(&f);patch(&f,1,0);fm_write(&f,0x30,10);
 {SINT32 lr[2]={0};fm_orchestra_enable(1);fm_render(&f,lr,1);assert(f.orchestra[0][3].family==1);}
 puts("PASS: sustained high-harmonic FM lead stays brass, not percussion");
 fm_reset(&f);patch(&f,2,0);fm_write(&f,0xb0,6|16);fm_write(&f,0x30,5);fm_write(&f,0x40,28);
 {SINT32 lr[2]={0};fm_render(&f,lr,1);assert(f.orchestra[0][3].family==0);}
 puts("PASS: sustained additive lead with rich modulation uses bowed ensemble");

 /* With constant pitch/level, an onset has more high partial energy than sustain. */
 FmChip air;fm_reset(&air);air.ch[0].alg=7;
 FmOp *wind=&air.ch[0].op[3];wind->state=3;wind->key=1;wind->att=0;wind->tl=0;wind->inc=2000;
 for(int n=0;n<22050;n++){SINT32 l,r;orchestra_frame(&air,&l,&r);}
 double colour[2];
 for(int onset=0;onset<2;onset++) {
  if(onset){SINT32 l,r;wind->key=0;orchestra_frame(&air,&l,&r);wind->key=1;}
  double re[2]={0},im[2]={0};
  for(int n=0;n<4410;n++) {
   SINT32 l,r;orchestra_frame(&air,&l,&r);
   for(int h=0;h<2;h++){double phase=6.283185307179586*n*2000*(3993600.0/72.0)*(h?5:1)/(1048576.0*22050);re[h]+=l*cos(phase);im[h]+=l*sin(phase);}
  }
  colour[onset]=hypot(re[1],im[1])/hypot(re[0],im[0]);
 }
 assert(colour[1]>colour[0]*1.05);
 puts("PASS: acoustic onset is spectrally brighter than sustain at constant pitch/level");
 /* No periodic complete cancellation in the mono sum of non-string voices. */
 for(int c=0;c<3;c++) {
  FmChip solo;fm_reset(&solo);solo.ch[c].alg=7;
  FmOp *p=&solo.ch[c].op[3];p->state=3;p->key=1;p->att=0;p->tl=0;p->inc=6000;
  for(int block=0;block<300;block++) {
   long long energy=0;
   for(int n=0;n<441;n++){SINT32 l,r;orchestra_frame(&solo,&l,&r);energy+=(long long)(l+r)*(l+r);}
   if(block>10)assert(energy/441>1000000);
  }
 }
 puts("PASS: sustained non-string melody has no detune cancellation dropouts");

 for(int family=0;family<4;family++) {
  fm_reset(&f);fm_orchestra_enable(1);for(int warm=0;warm<512;warm++){SINT32 z[2]={0};fm_render(&f,z,1);}patch(&f,family,0);
  char name[1024];snprintf(name,sizeof name,"%s/family%d.pcm",argv[1],family);FILE *out=fopen(name,"wb");assert(out);
  long long energy=0;int peak=0;
  for(int frame=0;frame<22050*4;frame++) {
   SINT32 lr[2]={0};if(frame==22050)fm_key(&f,0);
   fm_render(&f,lr,1);
   for(int c=0;c<2;c++) {assert(lr[c]>-65536&&lr[c]<65536);if(abs(lr[c])>peak)peak=abs(lr[c]);if(frame<22050)energy+=(long long)lr[c]*lr[c];}
   if(frame>22050*3)assert(abs(lr[0])<=2&&abs(lr[1])<=2);
   short samples[2]={(short)(lr[0]>32767?32767:lr[0]<-32768?-32768:lr[0]),(short)(lr[1]>32767?32767:lr[1]<-32768?-32768:lr[1])};fwrite(samples,2,2,out);
  }
  fclose(out);assert(energy>1000000);assert(f.orchestra[0][3].family==family);printf("PASS: family %d, peak %d, finite output, release to silence\n",family,peak);
 }
 fm_reset(&f);patch(&f,0,0);patch(&f,1,1);patch(&f,2,2);ref=f;fm_orchestra_enable(1);
 for(int block=0;block<100;block++) {
  SINT32 a[256]={0},b[256]={0};fm_render(&f,a,128);ref_fm_render(&ref,b,128);
  assert(!memcmp((char*)&f+offsetof(FmChip,ch3mode),(char*)&ref+offsetof(FmChip,ch3mode),sizeof f - offsetof(FmChip,ch3mode)));
 }
 fm_orchestra_enable(0);
 for(int block=0;block<10;block++){SINT32 a[256]={0},b[256]={0};fm_render(&f,a,128);ref_fm_render(&ref,b,128);if(block>=4)assert(!memcmp(a,b,sizeof a));}
 puts("PASS: replacement leaves FM state intact and switching off restores exact output");
 for(int rate=22050;rate<=44100;rate*=2) {
  fm_open(rate);fm_reset(&f);patch(&f,0,0);fm_orchestra_enable(1);
  for(int n=0;n<rate;n++){SINT32 lr[2]={0};fm_render(&f,lr,1);assert(abs(lr[0])<32768&&abs(lr[1])<32768);}
 }
 puts("PASS: 22050/44100 Hz sample rates");
 for(int family=0;family<4;family++) {
  fm_open(22050);fm_reset(&f);patch(&f,family,0);ref=f;fm_orchestra_enable(2);
  char name[1024];snprintf(name,sizeof name,"%s/sample%d.pcm",argv[1],family);FILE *out=fopen(name,"wb");assert(out);
  long long energy=0,tail=0;int max=0;
  for(int block=0;block<400;block++) {
   SINT32 lr[512]={0},native[512]={0};if(block==200){fm_key(&f,0);fm_key(&ref,0);}
   fm_render(&f,lr,256);ref_fm_render(&ref,native,256);
   assert(!memcmp((char*)&f+offsetof(FmChip,ch3mode),(char*)&ref+offsetof(FmChip,ch3mode),sizeof f - offsetof(FmChip,ch3mode)));
   for(int i=0;i<512;i++){if(abs(lr[i])>max)max=abs(lr[i]);assert(abs(lr[i])<65536);if(block<200)energy+=(long long)lr[i]*lr[i];if(block>380)tail+=abs(lr[i]);short x=(short)(lr[i]>32767?32767:lr[i]<-32768?-32768:lr[i]);fwrite(&x,2,1,out);}
  }
  fclose(out);assert(energy>10000000);assert(tail<100);
  printf("PASS: sampled family %d audible, loop/release stable, peak %d, native state preserved\n",family,max);
 }
 for(int rate=22050;rate<=44100;rate*=2) {
  fm_open(rate);fm_reset(&f);patch(&f,1,0);
  for(int block=0;block<600;block++) {
   fm_orchestra_enable((block/100)%3);SINT32 lr[512]={0};fm_render(&f,lr,256);
   for(int i=0;i<512;i++)assert(abs(lr[i])<32768);
  }
 }
 puts("PASS: Original/Synthesizer/Orchestra switching and sample resampling at both rates");
 /* Key-off/on register writes can both occur between output samples. A new
  * note must restart a finished one-shot even when the sampled key bit stays 1. */
 fm_open(22050);fm_reset(&f);fm_orchestra_enable(2);patch(&f,3,0);
 for(int o=0;o<4;o++){f.ch[0].op[o].att=0;f.ch[0].op[o].state=3;}
 for(int n=0;n<22050*2;n++){SINT32 l,r;orchestra_frame(&f,&l,&r);}
 fm_key(&f,0);fm_key(&f,0xf0);
 long long retrigger_energy=0;
 for(int n=0;n<1024;n++){SINT32 l,r;orchestra_frame(&f,&l,&r);retrigger_energy+=(long long)l*l;}
 fprintf(stderr,"one-shot retrigger energy: %lld\n",retrigger_energy);
 assert(retrigger_energy>1000000);
 puts("PASS: back-to-back key-off/on restarts a finished acoustic one-shot");
 /* Nearly-unison FM carrier detune must not turn identical recordings into
  * a deep periodic amplitude beat. Distinct musical notes remain independent. */
 fm_open(22050);fm_reset(&f);fm_orchestra_enable(2);f.ch[0].alg=4;
 for(int o=2;o<4;o++){FmOp *p=&f.ch[0].op[o];p->state=3;p->key=1;p->att=0;p->tl=16;p->inc=6000+(o-2)*10;}
 long long low=0x7fffffffffffffffLL,high=0;
 for(int block=0;block<100;block++) {
  long long energy=0;
  for(int n=0;n<1764;n++){SINT32 l,r;orchestra_frame(&f,&l,&r);energy+=(long long)l*l;}
  if(block>10){if(energy<low)low=energy;if(energy>high)high=energy;}
 }
 fprintf(stderr,"sampled near-unison energy ripple: %.3f dB\n",10*log10((double)high/low));
 assert(low>0 && high<low*2);
 puts("PASS: sampled near-unison carriers have no deep periodic volume beating");
 /* Layered melody parts can also detune the same recording across FM parts. */
 fm_open(22050);fm_reset(&f);fm_orchestra_enable(2);
 for(int c=0;c<2;c++) {
  f.ch[c].alg=1;f.ch[c].fb=7;f.ch[c].op[0].tl=27;
  FmOp *o=&f.ch[c].op[3];o->key=1;o->state=3;o->att=14;o->tl=19+c*8;o->inc=6000+c*10;
 }
 low=0x7fffffffffffffffLL;high=0;
 for(int block=0;block<100;block++) {
  long long energy=0;
  for(int n=0;n<1764;n++){SINT32 l,r;orchestra_frame(&f,&l,&r);energy+=(long long)l*l;}
  if(block>10){if(energy<low)low=energy;if(energy>high)high=energy;}
 }
 fprintf(stderr,"cross-part sampled near-unison energy ripple: %.3f dB\n",10*log10((double)high/low));
 assert(low>0 && high<low*2);
 puts("PASS: layered near-unison melody parts do not cancel the same recording");
 /* A software vibrato crosses the near-unison boundary repeatedly. That must
  * not alternate between one ensemble and two interfering recordings. */
 fm_open(22050);fm_reset(&f);fm_orchestra_enable(2);
 for(int c=0;c<2;c++) {
  f.ch[c].alg=1;f.ch[c].fb=7;f.ch[c].op[0].tl=27;
  FmOp *o=&f.ch[c].op[3];o->key=1;o->state=3;o->att=14;o->tl=19+c*8;o->inc=6000;
 }
 low=0x7fffffffffffffffLL;high=0;
 for(int block=0;block<160;block++) {
  long long energy=0;
  for(int n=0;n<1102;n++) {
   double t=(double)(block*1102+n)/22050;
   f.ch[0].op[3].inc=6000+(int)(80*sin(t*6.283185307179586*4));
   SINT32 l,r;orchestra_frame(&f,&l,&r);energy+=(long long)l*l;
  }
  if(block>20){if(energy<low)low=energy;if(energy>high)high=energy;}
 }
 fprintf(stderr,"vibrato boundary energy ripple: %.3f dB\n",10*log10((double)high/low));
 assert(low>0 && high<low*2);
 puts("PASS: ordinary pitch vibrato does not repeatedly split/merge ensemble voices");
 /* Fast sustained notes must not repeatedly replay a slow acoustic attack. */
 fm_open(22050);fm_reset(&f);fm_orchestra_enable(2);patch(&f,1,0);
 for(int o=0;o<4;o++){f.ch[0].op[o].att=0;f.ch[0].op[o].state=3;}
 low=0x7fffffffffffffffLL;high=0;
 for(int block=0;block<300;block++) {
  long long energy=0;
  if(block%5==0){fm_key(&f,0);fm_key(&f,0xf0);}
  for(int n=0;n<441;n++){SINT32 l,r;orchestra_frame(&f,&l,&r);energy+=(long long)l*l;}
  if(block>50){if(energy<low)low=energy;if(energy>high)high=energy;}
 }
 fprintf(stderr,"repeated sustained-note energy ripple: %.3f dB\n",10*log10((double)high/low));
 assert(low>0 && high<low*2);
 puts("PASS: fast sustained notes retain their body instead of pumping the recording attack");
 /* Unrelated parts must retain exactly the sum of their individual outputs.
  * (Same-pitch parts are one string section in Orchestra and may merge.) */
 for(int different_family=0;different_family<3;different_family+=2) {
  FmChip a,b,both;fm_reset(&a);fm_reset(&b);fm_reset(&both);
  a.ch[0].alg=1;a.ch[0].fb=7;a.ch[0].op[0].tl=27;
  b.ch[1].alg=different_family==1?7:1;b.ch[1].fb=different_family==1?0:7;b.ch[1].op[0].tl=27;
  FmOp *oa=&a.ch[0].op[3],*ob=&b.ch[1].op[3];
  oa->key=ob->key=1;oa->state=ob->state=3;oa->att=ob->att=0;oa->tl=ob->tl=16;
  oa->inc=6000;ob->inc=different_family==1?6000:different_family==2?6090:9000;
  both.ch[0]=a.ch[0];both.ch[1]=b.ch[1];
  for(int n=0;n<4096;n++) {
   SINT32 al,ar,bl,br,l,r;orchestra_sample_frame(&a,&al,&ar);orchestra_sample_frame(&b,&bl,&br);orchestra_sample_frame(&both,&l,&r);
   assert(l==al+bl && r==ar+br);
  }
 }
 puts("PASS: distinct pitches remain independent across parts");

 /* Timbre replacement must retain the FM/SSG level reference. Use one audible
  * carrier so native FM is a sine; no game register sequences are involved. */
 for(int mode=1;mode<=2;mode++)for(int family=0;family<3;family++) {
  fm_open(22050);fm_reset(&f);patch(&f,family,0);
  for(int i=0;i<4;i++){f.ch[0].op[i].tl=i==3?19:127;f.ch[0].op[i].att=14;f.ch[0].op[i].state=3;}
  ref=f;fm_orchestra_enable(mode);double replacement=0,native=0;
  for(int n=0;n<33075;n++) {
   SINT32 a[2]={0},b[2]={0};fm_render(&f,a,1);ref_fm_render(&ref,b,1);
   if(n>=11025){replacement+=(double)a[0]*a[0];native+=(double)b[0]*b[0];}
  }
  double ratio=sqrt(replacement/native);
  fprintf(stderr,"FM loudness reference mode=%d family=%d ratio=%.3f\n",mode,family,ratio);
  assert(ratio>.60 && ratio<1.40);
 }
 puts("PASS: fixed timbre calibration preserves the native FM loudness reference against unchanged SSG");
 /* Per-part loudness matching: each replaced part sounds as loud as its own
  * native FM channel (pre-emphasised power, repeated notes), for every family
  * and level, so no game needs its own balance. */
 for(int mode=1;mode<=2;mode++) for(int family=0;family<4;family++) for(int tl=0;tl<=16;tl+=16) {
  double e[2];
  for(int side=0;side<2;side++) {
   fm_open(22050);fm_set_volume(70);fm_reset(&f);fm_orchestra_enable(side?mode:0);patch(&f,family,0);
   if(tl){for(int op=0;op<4;op++){if(f.ch[0].op[op].tl<tl)fm_write(&f,0x40+op*4,tl);}}
   double sum=0;SINT32 previous=0;
   for(int block=0;block<200;block++) {
    SINT32 lr[512]={0};if(block%8==0){fm_key(&f,0);fm_key(&f,0xf0);}
    fm_render(&f,lr,256);
    if(block>=80)for(int i=0;i<512;i+=2){const SINT32 m=(lr[i]+lr[i+1])/2;const double w=m-.7*previous;previous=m;sum+=w*w;}
   }
   e[side]=10*log10(sum+1);
  }
  /* A part may be held below native loudness only by the headroom ceiling
   * (e.g. four in-phase full-scale carriers), never by the matcher itself. */
  const int capped=(long long)f.orchestra_gain[0]*f.orchestra_rpeak[0]>=22000LL*1024*85/100; /* within the decaying peak's reach of HEADROOM */
  fprintf(stderr,"match mode=%d family=%d tl=%d %+.2f dB%s\n",mode,family,tl,e[1]-e[0],capped?" (headroom ceiling)":"");
  assert(fabs(e[1]-e[0])<4.0 || (capped && e[1]<e[0]));
 }
 puts("PASS: every replaced part follows its native FM part loudness within 4 dB unless held by the headroom ceiling (all families, two levels, both modes)");





}
'''
with tempfile.TemporaryDirectory(prefix='pc98-orchestra-') as tmp:
 p=Path(tmp);(p/'reference.c').write_text(reference);(p/'test.c').write_text(harness)
 flags=['gcc','-std=gnu99','-O2','-g','-Wall','-Wextra','-Werror','-Wno-unknown-pragmas','-I'+str(core/'include'),'-I'+str(core/'sound')]
 rename=['-D'+name+'=ref_'+name for name in ['fm_open','fm_set_volume','fm_reset','fm_write','fm_key','fm_csm','fm_render']]
 subprocess.run(flags+rename+['-c',str(p/'reference.c'),'-o',str(p/'reference.o')],check=True)
 (p/'bank.S').write_text('.section .rodata\n.balign 4\n.global _binary_pc98_orchestra_bank_start\n_binary_pc98_orchestra_bank_start:\n.incbin \"'+str(core/'sound/samples/orchestra_bank.bin')+'\"\n.section .note.GNU-stack,\"\",@progbits\n')
 subprocess.run(flags+['-fsanitize=undefined,address',str(p/'test.c'),str(core/'sound/fm.c'),str(core/'sound/orchestra.c'),str(core/'sound/orchestra_sampler.c'),str(p/'bank.S'),str(p/'reference.o'),'-lm','-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test'),str(p)],check=True)
 if output:
  output.mkdir(parents=True,exist_ok=True)
  for family,name in enumerate(['strings','brass','woodwind','percussion']):
   pcm=(p/f'family{family}.pcm').read_bytes()
   with wave.open(str(output/(name+'.wav')),'wb') as w:w.setnchannels(2);w.setsampwidth(2);w.setframerate(22050);w.writeframes(pcm)
  for family,name in enumerate(['strings','brass','woodwind','percussion']):
   with wave.open(str(output/('sampled_'+name+'.wav')),'wb') as w:w.setnchannels(2);w.setsampwidth(2);w.setframerate(22050);w.writeframes((p/f'sample{family}.pcm').read_bytes())
  print('Saved synthetic and CC0 sampled instrument notes (no game music):',output)
