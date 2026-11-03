#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <sound.h>
#include "chips.h"
/* Build: gcc -std=gnu99 -O2 -Icomponents/pc98core/include -Icomponents/pc98core/sound tests/test_ssg_orchestra.c
 *   components/pc98core/sound/{ssg,fm,orchestra,orchestra_sampler}.c ref_ssg.o bank.S -lm
 * where ref_ssg.o is the unmodified ssg.c compiled with -Dssg_*=ref_ssg_* and bank.S .incbin-s orchestra_bank.bin. */
void ref_ssg_open(UINT);void ref_ssg_set_volume(UINT);void ref_ssg_reset(SsgChip*);void ref_ssg_write(SsgChip*,UINT,REG8);void ref_ssg_render(void*,SINT32*,UINT);
#define NATIVE offsetof(SsgChip,x1) /* chip registers, generators, envelope: not the output coupling filter */
static void tune(SsgChip *s,void(*w)(SsgChip*,UINT,REG8),int ch,int tp,int vol,int mix){w(s,ch*2,tp&255);w(s,ch*2+1,tp>>8);w(s,8+ch,vol);w(s,7,mix);}
static double weighted(const SINT32 *lr,int n,SINT32 *prev){double e=0;for(int i=0;i<n;i+=2){SINT32 m=(lr[i]+lr[i+1])/2;double w=m-.7**prev;*prev=m;e+=w*w;}return e;}
int main(void){
 ssg_open(22050);ref_ssg_open(22050);ssg_set_volume(70);ref_ssg_set_volume(70);fm_open(22050);
 struct {int mix,vol,replaced;const char*name;} cases[]={{0x3e,15,1,"tone only"},{0x37,15,0,"noise only"},{0x36,15,0,"tone+noise"},{0x3e,0x10,0,"envelope"}};
 for(int k=0;k<4;k++)for(int mode=0;mode<=2;mode+=2){
  SsgChip a,b;ssg_reset(&a);ref_ssg_reset(&b);fm_orchestra_enable(mode);
  tune(&a,ssg_write,0,285,cases[k].vol,cases[k].mix);tune(&b,ref_ssg_write,0,285,cases[k].vol,cases[k].mix);
  ssg_write(&a,11,0);ssg_write(&a,12,8);ssg_write(&a,13,14);ref_ssg_write(&b,11,0);ref_ssg_write(&b,12,8);ref_ssg_write(&b,13,14);
  double ea=0,eb=0;SINT32 pa=0,pb=0;int same=1;
  for(int blk=0;blk<200;blk++){
   SINT32 x[512]={0},y[512]={0};
   if(blk%10==0&&cases[k].replaced){int tp=blk%20?285:240;tune(&a,ssg_write,0,tp,15,cases[k].mix);tune(&b,ref_ssg_write,0,tp,15,cases[k].mix);}
   ssg_render(&a,x,256);ref_ssg_render(&b,y,256);
   if(memcmp(x,y,sizeof x))same=0;
   if(memcmp(&a,&b,NATIVE)){for(size_t o=0;o<NATIVE;o++)if(((unsigned char*)&a)[o]!=((unsigned char*)&b)[o]){printf("case %d mode %d blk %d offset %u (x1 %u y1 %u ecnt %u tone_hi %u)\n",k,mode,blk,(unsigned)o,(unsigned)offsetof(SsgChip,x1),(unsigned)offsetof(SsgChip,y1),(unsigned)offsetof(SsgChip,ecnt),(unsigned)offsetof(SsgChip,tone_hi));break;}return 1;}
   for(int i=0;i<512;i++)assert(x[i]>-32768&&x[i]<32768);
   if(blk>=60){ea+=weighted(x,512,&pa);eb+=weighted(y,512,&pb);}
  }
  const double d=10*log10((ea+1)/(eb+1));
  printf("%-10s mode %d: output %s native, weighted loudness %+.2f dB\n",cases[k].name,mode,same?"identical to":"differs from",d);
  if(mode==0||!cases[k].replaced)assert(same);
  else {assert(!same);assert(fabs(d)<4.0);}
 }
 puts("PASS: SSG native state untouched; Original and noise/envelope channels bit-identical; Orchestra tone channels are strings at native loudness");
 return 0;
}
