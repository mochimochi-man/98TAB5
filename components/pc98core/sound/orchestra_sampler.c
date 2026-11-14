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
static UINT32 scales[ORCH_SAMPLE_ZONES];
/* The players, as the bank numbers them. The pizzicato and the glockenspiel are in the bank, but no desk below has
 * them play. */
enum { VIOLINS, VIOLAS, CELLI, BASSES, HARP, HORN, TRUMPET, TROMBONE, FLUTE, OBOE, CLARINET, BASSOON,
       PIZZ_BASSES, PIZZ_CELLI, GLOCK, PLAYERS };
#define STRING_SECTION(p) ((p)<=BASSES)
#define ONE_SHOT(p) ((p)==HARP || (p)>=PIZZ_BASSES)

void orchestra_sample_open(UINT rate)
{
    output_rate=rate ? rate : 22050;
    for(UINT z=0;z<ORCH_SAMPLE_ZONES;z++)
        scales[z]=(UINT32)(((UINT64)sample_zones[z].level[0].rate<<24)/
                           ((UINT64)sample_zones[z].root_inc*output_rate));
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
/* An operator's multiplier in halves, so that MUL 0 (x0.5) fits; and the note it sounds when the voice's
 * multipliers have `common` (also in halves) as their common factor. */
#define HALF_MUL(o) ((o)->mul ? 2u*(o)->mul : 1u)
#define NOTE_INC(o,common) ((UINT32)((UINT64)(o)->inc*(common)/HALF_MUL(o)))

/* Who plays a part at a pitch, as an orchestrator would hand out a line: the
 * desks of each kind of part from the bottom up. A new line goes to the highest
 * desk whose `from` it reaches; a running line keeps its player while it stays
 * within low..high, that player's compass, so a melody does not change
 * instrument at every boundary crossing.
 *   bowed   basses, celli, violas, violins
 *   brass   the bass line stays with the string basses (a tuba there is a
 *           brass band), then trombones, horns, trumpets
 *   reed    bassoon, clarinet, oboe, flute
 *   plucked the harp at every pitch, every note new. The FM drums and an FM
 *           bass line are this kind of patch too, but those the chip plays
 *           itself (orchestra_frame) */
typedef struct { UINT8 player; UINT32 from,low,high; } Desk;
static const Desk desks[ORCH_PARTS][4]={
    {{BASSES,0,0,HZ(147)},{CELLI,HZ(98),HZ(52),HZ(440)},{VIOLAS,HZ(165),HZ(116),HZ(700)},{VIOLINS,HZ(262),HZ(185),~0u}},
    {{BASSES,0,0,HZ(110)},{TROMBONE,HZ(87),HZ(58),HZ(392)},{HORN,HZ(147),HZ(82),HZ(587)},{TRUMPET,HZ(349),HZ(196),~0u}},
    {{BASSOON,0,0,HZ(330)},{CLARINET,HZ(175),HZ(147),HZ(880)},{OBOE,HZ(277),HZ(233),HZ(1175)},{FLUTE,HZ(740),HZ(262),~0u}},
    {{HARP,0,0,~0u},{HARP,0,0,~0u},{HARP,0,0,~0u},{HARP,0,0,~0u}},
};

static UINT choose_player(const OrchestraSampleVoice *v,UINT part,UINT32 pitch)
{
    const Desk *d=desks[part<ORCH_PARTS ? part : ORCH_BOWED];
    UINT pick=d[0].player;
    if(v->ready && v->was_key && v->family==part)
        for(UINT i=0;i<4;i++) if(d[i].player==v->section && pitch>=d[i].low && pitch<=d[i].high)return v->section;
    for(UINT i=1;i<4;i++) if(pitch>=d[i].from)pick=d[i].player;
    return pick;
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
 * of the strings is the soft dynamic: a quiet note is bowed softly, with a
 * softer tone.
 * A note that follows the last one without a gap is slurred, on a wind as on a
 * string; one that comes after the key was let go is tongued.
 * Every note starts at the loop. A bowed recording opens at a quarter of its
 * sustain and swells for half a second, and the section spreads in a beat
 * behind the note; a wind's recorded attack is bright and the loop after it is
 * not, so a held note changed its tone a quarter of a second in. At the loop the
 * bow is settled and the tone is the held one; the voice envelope keeps the
 * 2 ms onset click-free. (A one-shot has no loop: its loop_start is 0, the
 * pluck itself.) */
static void voice_note(OrchestraSampleVoice *v,UINT part,UINT32 pitch,SINT32 level)
{
    const UINT section=choose_player(v,part,pitch);
    const UINT sounding=v->ready && v->envelope;
    const UINT legato=sounding && v->was_key && v->section==section && !ONE_SHOT(section);
    /* a running phrase keeps its dynamic layer: no timbre switch mid-line */
    const UINT layer=!STRING_SECTION(section) ? 0u : legato ? v->layer : level<2048 ? 1u : 0u;
    const UINT zone=select_zone(section,layer,pitch);
    if(legato && zone==v->zone[0]) {
        /* Keep the bow moving through fast notes; replaying the recorded
         * attack every note artificially pumps the level. */
    } else {
        /* The recording changes under a sounding voice: the old one is faded
         * out under the new one over a hundredth of a second, not cut. */
        if(sounding && v->zone[0]<ORCH_SAMPLE_ZONES){v->zone[1]=v->zone[0];v->pos[1]=v->pos[0];v->fade=255;}
        else {v->zone[1]=0xffffu;v->fade=0;}
        v->pos[0]=sample_zones[zone].level[0].loop_start<<16;
    }
    v->zone[0]=(UINT16)zone;
    v->family=(UINT8)part;v->section=(UINT8)section;v->layer=(UINT8)layer;v->ready=1;
}

/* Seats, left/right in 64ths: violins and harp left, violas centre, celli and
 * basses right, the woodwinds in the middle with the horns to their left and
 * the trumpets and trombones to their right, as an orchestra sits, but
 * narrowly: a wide stage sounded spread out. One recording per voice, so no
 * comb filtering and no wah. */
static const UINT8 seat[PLAYERS][2]={{53,41},{49,45},{45,49},{41,53},{51,43},
    {51,43},{44,50},{43,51},{49,45},{48,46},{46,48},{45,49},{41,53},{45,49},{50,44}};

/* How long a voice takes to fall silent once its part has: a bow lifts in a
 * moment, a wind note a little later, and a plucked string or a struck bar is
 * left to ring - the FM patch behind it has died away long before it has. */
static UINT release_smooth(const OrchestraSampleVoice *v)
{
    const UINT p=v->section;
    return (ONE_SHOT(p) ? output_rate/6 : STRING_SECTION(p) ? output_rate/35 : output_rate/25)+1;
}

/* The voice's envelope toward its part's level: up in 2 ms, down as release_smooth() says. Every voice, every
 * sample, so kept inline. */
static inline __attribute__((always_inline)) void voice_follow(OrchestraSampleVoice *v,SINT32 level)
{
    const SINT32 desired=level*4096;
    const UINT smooth=desired>v->envelope ? output_rate/500+1 : release_smooth(v);
    v->envelope+=(desired-v->envelope)/(SINT32)smooth;
    if(!level && v->envelope<(SINT32)smooth)v->envelope=0;
}

static void voice_render(OrchestraSampleVoice *v,UINT32 pitch,SINT32 *l,SINT32 *r)
{
    if(!v->ready || !v->envelope || !pitch || v->zone[0]>=ORCH_SAMPLE_ZONES)return;
    if((UINT64)pitch*3993600u >= (UINT64)output_rate*72u*524288u)return;
    const UINT zone=v->zone[0];
    const OrchSampleLevel *s=&sample_zones[zone].level[0];
    SINT32 x=0,a=0;
    if(fetch(s,&v->pos[0],&x)) {
        a=(SINT32)(((SINT64)x*s->gain_q12)>>12);
        v->pos[0]=(UINT32)((UINT64)v->pos[0]+(((UINT64)pitch*scales[zone])>>8));
    } else if(!v->fade)return;
    if(v->fade) {
        SINT32 b=0;
        if(v->zone[1]<ORCH_SAMPLE_ZONES) {
            const OrchSampleLevel *t=&sample_zones[v->zone[1]].level[0];
            if(fetch(t,&v->pos[1],&x)) {
                b=(SINT32)(((SINT64)x*t->gain_q12)>>12);
                v->pos[1]=(UINT32)((UINT64)v->pos[1]+(((UINT64)pitch*scales[v->zone[1]])>>8));
            }
        }
        a=(a*(255-v->fade)+b*v->fade)/255;
        v->fade--;
    }
    a=(SINT32)(((SINT64)a*(v->envelope/4096))/32768);
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
        const UINT mask=orchestra_carriers(ch->alg);
        /* The note an FM voice sounds is the channel fundamental times the
         * common factor of all operator multipliers, not each carrier's own
         * frequency: a MUL 3 carrier is a timbre, not a twelfth above. In
         * halves, so that MUL 0 (x0.5) fits. */
        UINT common=0;
        for(UINT i=0;i<4;i++) {
            UINT a=HALF_MUL(&ch->op[i]),b=common;
            while(b){const UINT t=a%b;a=b;b=t;}
            common=a;
        }
        common_mul[c]=common;
        /* The patch says what kind of part this is - bowed, brass, reed or
         * plucked - and the pitch then says which desk plays it. */
        const UINT patch_family=orchestra_family(ch);
        for(UINT i=0;i<4;i++) if(mask&(1u<<i)) {
            const UINT slot=c*4+i;
            const FmOp *o=&ch->op[i];OrchestraSampleVoice *v=&chip->samples[c][i];
            const SINT32 level=patch_family==ORCH_PLUCKED ? 0 : orchestra_level(o); /* the chip plays those itself (orchestra_frame) */
            const UINT32 note_inc=NOTE_INC(o,common);
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
        const UINT32 pitch=weight[slot] ? pitch_base[slot] : NOTE_INC(&chip->ch[c].op[i],common_mul[c]);
        const UINT key=keys[slot];
        /* Approximate RMS addition, keeping section level steady. */
        const SINT32 level=peak[slot]+(total[slot]-peak[slot])*3/8;
        voice_follow(v,level);
        if((retrigger[slot] || (key && !v->was_key) || (!v->ready && level)) && pitch)
            voice_note(v,family[slot],pitch,level);
        v->was_key=(UINT8)!!key;
        voice_render(v,pitch,&lc[c],&rc[c]);
    }
}

/* One SSG tone channel as bowed strings. note: a new note began. Not a wind: the SSG also carries the game's sound
 * effects, and a sweep handed to a wind was a new tongued note at every step. On strings it is one bow. */
void orchestra_ssg_voice(OrchestraSampleVoice *v,UINT32 pitch,SINT32 level,int note,SINT32 *l,SINT32 *r)
{
    voice_follow(v,level);
    if(note && pitch)voice_note(v,ORCH_BOWED,pitch,level);
    v->was_key=(UINT8)(level>0);
    voice_render(v,pitch,l,r);
}

void orchestra_sample_frame(FmChip *chip,SINT32 *left,SINT32 *right)
{
    SINT32 l[3],r[3];UINT8 owner[3];
    orchestra_sample_channels(chip,l,r,owner);
    *left=l[0]+l[1]+l[2];*right=r[0]+r[1]+r[2];
}
