/* SPDX-License-Identifier: MIT
 * Copyright (c) mochimochi-man / Uh
 * Small PCM orchestra player. Sample provenance: samples/SOURCES.json (CC0).
 * No disk I/O, allocation or floating-point work in the sample loop.
 */
#include <compiler.h>
#include "chips.h"
#include "samples/orchestra_bank.h"
extern const SINT16 orchestra_bank[] asm("_binary_pc98_orchestra_bank_start");
static UINT output_rate=22050;
static UINT32 scales[ORCH_SAMPLE_ZONES],onset[ORCH_SAMPLE_ZONES];
enum { VIOLINS, VIOLAS, CELLI, BASSES, HARP, BOWED=0, PLUCKED=3 };

void orchestra_sample_open(UINT rate)
{
    output_rate=rate ? rate : 22050;
    for(UINT z=0;z<ORCH_SAMPLE_ZONES;z++)
        scales[z]=(UINT32)(((UINT64)sample_zones[z].level[0].rate<<24)/
                           ((UINT64)sample_zones[z].root_inc*output_rate));
    /* A bowed recording opens at a quarter of its sustain and swells for half
     * a second; even past 85% of the level, some dip again and the section
     * spreads in late, a beat behind the note. A note starts at the loop,
     * where the bow is settled; the voice envelope keeps the 2 ms onset
     * click-free. */
    for(UINT z=0;z<ORCH_SAMPLE_ZONES;z++) {
        const OrchSampleLevel *s=&sample_zones[z].level[0];
        onset[z]=sample_zones[z].family==HARP || !s->loop_end ? 0 : s->loop_start;
    }
}

static UINT select_zone(UINT section,UINT layer,UINT32 pitch)
{
    UINT best=0;UINT64 score=~(UINT64)0;
    for(UINT z=0;z<ORCH_SAMPLE_ZONES;z++) if(sample_zones[z].family==section && sample_zones[z].layer==layer) {
        UINT64 hi=pitch>sample_zones[z].root_inc ? pitch : sample_zones[z].root_inc;
        UINT64 lo=pitch<sample_zones[z].root_inc ? pitch : sample_zones[z].root_inc;
        const UINT64 distance=lo ? (hi<<20)/lo : ~(UINT64)0;
        if(distance<score){score=distance;best=z;}
    }
    return best;
}

/* FM increment per Hz: 2^20 / (3993600 / 72). */
#define HZ(f) ((UINT32)((f)*18.9047))

/* The section that plays a pitch, as an orchestrator would hand out a line.
 * A running line keeps its section while it stays in that section's compass,
 * so a melody does not change instrument at every boundary crossing. */
static UINT choose_section(const OrchestraSampleVoice *v,UINT32 pitch)
{
    static const UINT32 low[4]={HZ(185),HZ(116),HZ(52),0};
    static const UINT32 high[4]={~0u,HZ(700),HZ(440),HZ(147)};
    if(v->ready && v->was_key && v->section<HARP && pitch>=low[v->section] && pitch<=high[v->section])
        return v->section;
    return pitch<HZ(98) ? BASSES : pitch<HZ(165) ? CELLI : pitch<HZ(262) ? VIOLAS : VIOLINS;
}

/* Recording read with linear interpolation; loops wrap, one-shots stop. */
static int fetch(const OrchSampleLevel *s,UINT32 *position,SINT32 *out)
{
    const UINT32 end=s->length<<16,start=s->loop_start<<16;
    if(*position>=end) {
        if(!s->loop_end)return 0;
        *position=start+(*position-start)%(end-start);
    }
    const UINT index=*position>>16,frac=*position&65535;
    const UINT next=index+1<s->length ? index+1 : s->loop_end ? s->loop_start : index;
    const SINT32 a=orchestra_bank[s->offset+index],b=orchestra_bank[s->offset+next];
    *out=a+(SINT32)(((SINT64)(b-a)*frac)/65536);
    return 1;
}

/* Start, retrigger or glide one voice to a new note. Each recording is already
 * a whole section; layering two of them at one pitch makes their independent
 * vibratos beat into a flutter, so one plays at a time. The second recording
 * is the soft dynamic: a quiet note is bowed softly, with a softer tone. */
static void voice_note(OrchestraSampleVoice *v,UINT cls,UINT32 pitch,SINT32 level)
{
    const UINT section=cls==PLUCKED ? HARP : choose_section(v,pitch);
    const UINT legato=v->ready && v->was_key && v->envelope && section!=HARP && v->section==section;
    /* a running phrase keeps its dynamic layer: no timbre switch mid-line */
    const UINT layer=section==HARP ? 0u : legato ? v->layer : level<2048 ? 1u : 0u;
    const UINT zone=select_zone(section,layer,pitch);
    if(legato && zone==v->zone[0]) {
        /* Keep the bow moving through fast notes; replaying the recorded
         * attack every note artificially pumps the level. */
    } else if(legato) {
        v->pos[0]=sample_zones[zone].level[0].loop_start<<16; /* next recording, mid-bow */
    } else v->pos[0]=onset[zone]<<16; /* fresh bow stroke at full level, or pluck */
    v->zone[0]=(UINT16)zone;v->zone[1]=0xffffu;
    v->family=(UINT8)cls;v->section=(UINT8)section;v->layer=(UINT8)layer;v->ready=1;
}

/* Seats, left/right in 64ths: violins left, violas centre, celli and basses
 * right, as an orchestra sits, but narrowly: a wide stage sounded spread out.
 * One recording per voice, so no comb filtering
 * and no wah. */
static const UINT8 seat[5][2]={{53,41},{49,45},{45,49},{41,53},{47,47}};

static void voice_render(OrchestraSampleVoice *v,UINT32 pitch,SINT32 *l,SINT32 *r)
{
    if(!v->ready || !v->envelope || !pitch)return;
    if((UINT64)pitch*3993600u >= (UINT64)output_rate*72u*524288u)return;
    const UINT zone=v->zone[0];
    const OrchSampleLevel *s=&sample_zones[zone].level[0];
    SINT32 x;
    if(!fetch(s,&v->pos[0],&x))return;
    const SINT32 a=(SINT32)(((SINT64)x*(v->envelope/4096)*s->gain_q12)/(32768LL*4096));
    v->pos[0]=(UINT32)((UINT64)v->pos[0]+(((UINT64)pitch*scales[zone])>>8));
    *l+=a*seat[v->section][0]/64;*r+=a*seat[v->section][1]/64;
}

void orchestra_sample_channels(FmChip *chip,SINT32 lc[3],SINT32 rc[3],UINT8 owner[3])
{
    for(UINT c=0;c<3;c++){lc[c]=rc[c]=0;owner[c]=(UINT8)c;}
    /* A recording already contains a section. Collapse near-unison copies of
     * that recording across parts as well as carriers: two FM melody parts can
     * deliberately detune each other. Distinct pitches/families stay separate. */
    UINT32 pitch_base[12]={0},weight[12]={0};
    SINT32 total[12]={0},peak[12]={0};
    UINT8 keys[12]={0},retrigger[12]={0},family[12]={0};
    UINT common_mul[3];
    for(UINT c=0;c<3;c++) {
        const FmCh *ch=&chip->ch[c];
        const UINT mask=ch->alg<4 ? 8 : ch->alg==4 ? 12 : ch->alg==7 ? 15 : 14;
        /* The note an FM voice sounds is the channel fundamental times the
         * common factor of all operator multipliers, not each carrier's own
         * frequency: a MUL 3 carrier is a timbre, not a twelfth above. In
         * halves, so that MUL 0 (x0.5) fits. */
        UINT common=0;
        for(UINT i=0;i<4;i++){UINT a=ch->op[i].mul ? 2u*ch->op[i].mul : 1u,b=common;while(b){const UINT t=a%b;a=b;b=t;}common=a;}
        common_mul[c]=common;
        /* Orchestra is a string orchestra: every sustained patch is bowed,
         * clearly percussive patches are plucked (harp). */
        const UINT patch_family=orchestra_family(ch)==3 ? PLUCKED : BOWED;
        for(UINT i=0;i<4;i++) if(mask&(1u<<i)) {
            const UINT slot=c*4+i;
            const FmOp *o=&ch->op[i];OrchestraSampleVoice *v=&chip->samples[c][i];
            const SINT32 level=orchestra_level(o);
            const UINT32 note_inc=(UINT32)((UINT64)o->inc*common/(o->mul ? 2u*o->mul : 1u));
            const UINT trigger=v->key_serial!=chip->orchestra_key_serial[c][i];
            family[slot]=(UINT8)(v->ready && !trigger ? v->family : patch_family);
            if(!level && !o->key){v->merge_group=0;continue;}
            UINT group=slot;
            for(UINT j=0;j<slot;j++) if(weight[j] && family[j]==family[slot] && pitch_base[j] && note_inc) {
                const UINT32 d=pitch_base[j]>note_inc ? pitch_base[j]-note_inc : note_inc-pitch_base[j];
                /* Retain an established unison through ordinary vibrato.
                 * A new note still has to meet the narrower entry threshold. */
                const UINT limit=(!trigger && v->merge_group==j+1) ? 2u : 1u;
                if((UINT64)d*100u<=(UINT64)pitch_base[j]*limit){group=j;break;}
            }
            v->merge_group=(UINT8)(group+1);
            if(level && group/4!=c)owner[c]=(UINT8)(group/4);
            if(!weight[group])pitch_base[group]=note_inc;
            weight[group]+=level ? (UINT)level : 1;
            total[group]+=level;if(level>peak[group])peak[group]=level;
            keys[group]|=o->key;retrigger[group]|=(UINT8)trigger;
            if(trigger)v->family=family[slot];
        }
    }
    for(UINT c=0;c<3;c++)for(UINT i=0;i<4;i++)
        chip->samples[c][i].key_serial=chip->orchestra_key_serial[c][i];
    for(UINT c=0;c<3;c++)for(UINT i=0;i<4;i++) {
            const UINT slot=c*4+i;
            OrchestraSampleVoice *v=&chip->samples[c][i];
            const FmOp *po=&chip->ch[c].op[i];
            const UINT32 pitch=weight[slot] ? pitch_base[slot] :
                (UINT32)((UINT64)po->inc*common_mul[c]/(po->mul ? 2u*po->mul : 1u));
            const UINT key=keys[slot];
            /* Approximate RMS addition, keeping section level steady. */
            const SINT32 level=peak[slot]+(total[slot]-peak[slot])*3/8;
            const SINT32 desired=level*4096;
            const UINT smooth=desired>v->envelope ? output_rate/500+1 : output_rate/35+1;
            v->envelope+=(desired-v->envelope)/(SINT32)smooth;
            if(!level && v->envelope<(SINT32)smooth)v->envelope=0;
            if((retrigger[slot] || (key && !v->was_key) || (!v->ready && level)) && pitch)
                voice_note(v,family[slot],pitch,level);
            v->was_key=(UINT8)!!key;
            voice_render(v,pitch,&lc[c],&rc[c]);
    }
}

/* One SSG tone channel as bowed strings. note: a new note began. */
void orchestra_ssg_voice(OrchestraSampleVoice *v,UINT32 pitch,SINT32 level,int note,SINT32 *l,SINT32 *r)
{
    const SINT32 desired=level*4096;
    const UINT smooth=desired>v->envelope ? output_rate/500+1 : output_rate/35+1;
    v->envelope+=(desired-v->envelope)/(SINT32)smooth;
    if(!level && v->envelope<(SINT32)smooth)v->envelope=0;
    if(note && pitch)voice_note(v,BOWED,pitch,level);
    v->was_key=(UINT8)(level>0);
    voice_render(v,pitch,l,r);
}

void orchestra_sample_frame(FmChip *chip,SINT32 *left,SINT32 *right)
{
    SINT32 l[3],r[3];UINT8 owner[3];
    orchestra_sample_channels(chip,l,r,owner);
    *left=l[0]+l[1]+l[2];*right=r[0]+r[1]+r[2];
}
