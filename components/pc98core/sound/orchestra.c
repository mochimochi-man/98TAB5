/* SPDX-License-Identifier: MIT
 * Optional orchestral timbre synthesis for the three YM2203 FM parts.
 * Reads pitch, carrier envelopes and patch structure; never rewrites the chip.
 * Synthesizer uses procedural approximations; Orchestra uses the PCM sampler.
 */
#include <compiler.h>
#include <sound.h>
#include <math.h>
#include "chips.h"

enum { STRINGS, BRASS, WOODWIND, PERCUSSION, FAMILIES };
enum { EG_RELEASE = 4 }; /* FmOp.state, as in the native FM renderer. */
#define WAVE_BITS 9
#define WAVE_LEN (1u << WAVE_BITS)
/* Four harmonic bands keep the added partials below Nyquist. 16 KiB total. */
static SINT16 waves[FAMILIES][4][WAVE_LEN];
static UINT32 wave_gain[FAMILIES][4]; /* fixed RMS calibration, Q12 */
static UINT rate_hz = 22050;
static UINT16 attenuation_gain[1024];
static UINT32 native_step;
static int enabled;
static UINT attack[FAMILIES], release_time[FAMILIES];
static UINT32 vibrato_step, texture_decay;

void fm_orchestra_enable(int on) { __atomic_store_n(&enabled, on>=0 && on<=2 ? on : 0, __ATOMIC_RELAXED); }
int orchestra_enabled(void) { return __atomic_load_n(&enabled, __ATOMIC_RELAXED); }

void orchestra_open(UINT rate)
{
    static const double partial[FAMILIES][8] = {
        {1,.48,.31,.22,.16,.11,.08,.06}, /* bowed ensemble */
        {1,.72,.48,.32,.21,.14,.09,.06}, /* brass */
        {1,.06,.29,.03,.11,.02,.04,.01}, /* reed / woodwind */
        {1,.38,.15,.07,.03,.01,0,0}     /* tuned percussion */
    };
    static const unsigned attack_us[] = {7000,3500,4500,500};
    static const unsigned release_us[] = {35000,24000,20000,8000};
    UINT f,b,i,h;
    rate_hz = rate ? rate : 22050;
    orchestra_sample_open(rate_hz);
    native_step = (UINT32)((3993600.0 / 72.0) * 65536.0 / rate_hz + .5);
    vibrato_step = (UINT32)(4294967296.0 * 4.7 / rate_hz);
    texture_decay = 65536u / (rate_hz / 12u + 1u);
    for(i=0;i<1024;i++) attenuation_gain[i]=(UINT16)(8191.0 * pow(2.0, -(double)i/64.0));
    for(f=0;f<FAMILIES;f++) {
        attack[f] = (UINT)((UINT64)rate_hz * attack_us[f] / 1000000u) + 1;
        release_time[f] = (UINT)((UINT64)rate_hz * release_us[f] / 1000000u) + 1;
        for(b=0;b<4;b++) {
            const UINT count = 8u >> b;
            double norm=0;
            for(h=0;h<count;h++) norm += partial[f][h];
            for(i=0;i<WAVE_LEN;i++) {
                double v=0;
                for(h=0;h<count;h++)
                    v += partial[f][h] * sin(6.283185307179586 * (h+1) * i / WAVE_LEN);
                waves[f][b][i]=(SINT16)(v * 32700.0 / norm);
            }
            double square=0;
            for(i=0;i<WAVE_LEN;i++) square+=(double)waves[f][b][i]*waves[f][b][i];
            /* Native carrier is a full-scale sine. Harmonic peak normalization
             * otherwise attenuates replacements against the unchanged SSG. */
            wave_gain[f][b]=(UINT32)(4096.0*23170.0/sqrt(square/WAVE_LEN)+.5);
        }
    }
}

static UINT carriers(UINT algorithm)
{
    if(algorithm<4) return 8;
    if(algorithm==4) return 12;
    return algorithm==7 ? 15 : 14;
}

/* FM patches have no instrument IDs. This is a structural heuristic, not a
 * claim that a game's patch is identified unambiguously. Classify on key-on
 * so volume / envelope register writes cannot change family mid-note. */
UINT orchestra_family(const FmCh *ch)
{
    const UINT mask=carriers(ch->alg);
    UINT i,fast=0,carriers_n=0,rich_modulator=0;
    for(i=0;i<4;i++) {
        if(mask & (1u<<i)) {
            const FmOp *o=&ch->op[i];
            carriers_n++;
            if(o->dr>=16 && (o->sr>=8 || o->rr>=10)) fast++;
        } else if(ch->op[i].mul>=3 && ch->op[i].tl<48) rich_modulator=1;
    }
    if(fast && fast==carriers_n) return PERCUSSION;
    if(ch->alg<4 && ch->fb>=3 && ch->op[0].tl<48) return BRASS;
    if(ch->alg>=4 && ch->fb<=2) return rich_modulator ? STRINGS : WOODWIND;
    return STRINGS;
}

static SINT32 wave(UINT family,UINT band,UINT32 phase)
{
    const UINT i=phase>>(32-WAVE_BITS);
    const UINT frac=(phase>>(32-WAVE_BITS-12))&4095;
    const SINT32 a=waves[family][band][i];
    const SINT32 b=waves[family][band][(i+1)&(WAVE_LEN-1)];
    return (SINT32)((SINT64)(a+(b-a)*(SINT32)frac/4096)*wave_gain[family][band]/4096);
}

static void synthesizer_frame(FmChip *chip,SINT32 lc[3],SINT32 rc[3])
{
    UINT c;
    for(c=0;c<3;c++) {
        SINT32 l=0,r=0;
        const FmCh *ch=&chip->ch[c];
        const UINT mask=carriers(ch->alg);
        for(UINT i=0;i<4;i++) {
            const FmOp *o=&ch->op[i];
            OrchestraVoice *v=&chip->orchestra[c][i];
            const UINT carrier=mask&(1u<<i);
            const UINT key=carrier && o->key;
            const UINT32 pitch=o->inc;
            UINT att=o->att;
            /* Match native FM's SSG-EG inversion, including the release exception. */
            if((o->ssg&8) && o->ssg_inv && o->state!=EG_RELEASE) att=(0x200u-att)&0x3ffu;
            att+=(UINT)o->tl<<3;
            const SINT32 target=carrier && o->state && att<1023 ? attenuation_gain[att] : 0;
            if(key && !v->was_key) {
                v->family=(UINT8)orchestra_family(ch);
                v->transient=4096;
                if(!v->noise) v->noise=0x9e3779b9u ^ (c*17u+i*137u);
                /* Start each family with different section phases, not a copied
                 * FM waveform or a full second set of operators. */
                if(!v->envelope) {v->phase[0]=0;v->phase[1]=0x34000000u+c*0x10000000u;}
            }
            v->was_key=(UINT8)!!key;
            const UINT family=v->family;
            const SINT32 desired=target*4096;
            const SINT32 diff=desired-v->envelope;
            const UINT smoothing=diff>0 ? attack[family] : release_time[family];
            v->envelope+=diff/(SINT32)smoothing;
            if(!target && v->envelope<(SINT32)smoothing) v->envelope=0;
            if(!pitch || !v->envelope) continue;
            /* native phase uses 20 bits; output oscillators use all 32. */
            const UINT64 wide_step=((UINT64)pitch*native_step)>>4;
            if(wide_step>=0x7f000000u) continue;
            const UINT32 step=(UINT32)wide_step;
            if(!v->noise) v->noise=0x9e3779b9u ^ (c*17u+i*137u);
            v->noise^=v->noise<<13;v->noise^=v->noise>>17;v->noise^=v->noise<<5;
            const SINT32 air=(SINT32)(v->noise&65535u)-32768;
            v->air+=(air-v->air)/16;
            /* Breath/bow colour changes quickly at onset, then settles. */
            if(v->transient) {
                UINT decay=(v->transient*texture_decay>>16)+1;
                v->transient=v->transient>decay ? v->transient-decay : 0;
            }
            UINT band=0;
            while(band<3 && (UINT64)step*(8u>>band)*1004u>=((UINT64)1<<31)*1000u) band++;
            v->vibrato+=vibrato_step;
            const SINT32 tri=(SINT32)((v->vibrato>>16)&65535);
            const SINT32 vibrato=((tri<32768 ? tri : 65535-tri)-16384);
            /* No added pitch motion at all: an LFO vibrato, noise jitter or a
             * detuned second oscillator beats into wah/flutter against the
             * game's own pitch. The game's own vibrato still comes through the
             * FM pitch. The second oscillator keeps a fixed phase offset. */
            (void)vibrato;
            v->phase[0]+=step;
            v->phase[1]+=step;
            const SINT32 amp=v->envelope/4096;
            /* Interpolate bright/dark spectra instead of a static synthesizer tone.
             * Both tables obey the same Nyquist bound; no new wave storage. */
            static const UINT body_brightness[FAMILIES]={2900,2800,2600,2400};
            UINT bright=body_brightness[family]+v->transient/4+(UINT)amp/12;
            if(bright>4096) bright=4096;
            const UINT darkband=band<3 ? band+1 : band;
            SINT32 a=wave(family,darkband,v->phase[0]);
            a+=(wave(family,band,v->phase[0])-a)*(SINT32)bright/4096;
            SINT32 b=a;
            if(family==STRINGS) {
                b=wave(family,darkband,v->phase[1]);
                b+=(wave(family,band,v->phase[1])-b)*(SINT32)bright/4096;
            }
            static const UINT texture[FAMILIES]={48,16,24,8};
            const SINT32 breath=(air-v->air)*(SINT32)(texture[family]+v->transient/64)/4096;
            a+=breath;b+=breath;
            /* Stable stereo seats; no polarity-inverted widening. */
            if(family==STRINGS) {const SINT32 olda=a;a=(3*a+b)/4;b=(olda+3*b)/4;}
            else {b=a;}
            a=(SINT32)((SINT64)a*amp/32768);
            b=(SINT32)((SINT64)b*amp/32768);
            if(family==BRASS) {l+=a*3/4;r+=b;}
            else if(family==STRINGS) {l+=a;r+=b*7/8;}
            else {l+=a*7/8;r+=b*7/8;}
        }
        lc[c]=l;rc[c]=r;
    }
}

/* Shared carrier loudness follows native FM, including inverted SSG-EG. */
SINT32 orchestra_level(const FmOp *o)
{
    if(!o->state) return 0;
    UINT att=o->att;
    if((o->ssg&8) && o->ssg_inv && o->state!=EG_RELEASE) att=(0x200u-att)&0x3ffu;
    att+=(UINT)o->tl<<3;
    return att<1023 ? attenuation_gain[att] : 0;
}

/* Gain Q10 bounds: -12..+18 dB around the fixed calibration. Matching is per part,
 * never across the mix, so a quiet accompaniment stays quiet under the lead. */
enum { MATCH_MIN=256, MATCH_MAX=8192, MATCH_FLOOR=1024, MATCH_EVERY=16, HEADROOM=22000 };

/* The same follower for a single part without cross-part merging (SSG). */
void orchestra_match_feed(OrchestraMatch *m,SINT32 native,SINT32 replaced,SINT32 peak)
{
    const SINT64 x=native-(SINT64)m->prev[0]*7/10,y=replaced-(SINT64)m->prev[1]*7/10;
    m->prev[0]=native;m->prev[1]=replaced;
    m->npow=(UINT64)((SINT64)m->npow+((x*x-(SINT64)m->npow)>>10));
    m->rpow=(UINT64)((SINT64)m->rpow+((y*y-(SINT64)m->rpow)>>10));
    const UINT32 a=(UINT32)peak; /* the louder side: a seated section is not centred */
    m->rpeak-=m->rpeak>>13;
    if(a>m->rpeak)m->rpeak=a;
    if(!m->gain)m->gain=1024;
}

void orchestra_match_adjust(OrchestraMatch *m,UINT64 native_power)
{
    if(native_power<MATCH_FLOOR || m->rpow<MATCH_FLOOR)return;
    const float ratio=sqrtf((float)native_power/(float)m->rpow)*1024.0f;
    const float ceiling=m->rpeak ? (float)HEADROOM*1024.0f/(float)m->rpeak : (float)MATCH_MAX;
    float want=ratio<ceiling ? ratio : ceiling;
    if(want<1024.0f && ratio>=1024.0f)want=1024.0f;
    const UINT target=want<MATCH_MIN ? MATCH_MIN : want>MATCH_MAX ? MATCH_MAX : (UINT)want;
    m->gain=(UINT16)(m->gain+((SINT32)target-(SINT32)m->gain)/192);
    /* headroom acts at once; only lifting glides */
    if((UINT64)m->gain*m->rpeak>(UINT64)HEADROOM*1024u)m->gain=(UINT16)((UINT64)HEADROOM*1024u/m->rpeak);
}

static void match_update(FmChip *chip,const SINT32 out[3],const SINT32 peak[3],const UINT8 owner[3])
{
    UINT64 native[3]={0,0,0};
    for(UINT c=0;c<3;c++) {
        /* Loudness, not raw power: first-order pre-emphasis (-10 dB in the bass,
         * rising through the presence range) on both sides, so a darker
         * recording is matched to how loud the bright FM part sounded. */
        const SINT64 x=chip->orchestra_native[c]-(SINT64)chip->orchestra_prev[0][c]*7/10;
        const SINT64 y=out[c]-(SINT64)chip->orchestra_prev[1][c]*7/10;
        chip->orchestra_prev[0][c]=chip->orchestra_native[c];
        chip->orchestra_prev[1][c]=out[c];
        /* ~46 ms power followers at 22.05 kHz, the same window on both sides. */
        chip->orchestra_npow[c]=(UINT64)((SINT64)chip->orchestra_npow[c]+((x*x-(SINT64)chip->orchestra_npow[c])>>10));
        chip->orchestra_rpow[c]=(UINT64)((SINT64)chip->orchestra_rpow[c]+((y*y-(SINT64)chip->orchestra_rpow[c])>>10));
        const UINT32 a=(UINT32)peak[c]; /* the louder side, not the mono sum */
        chip->orchestra_rpeak[c]-=chip->orchestra_rpeak[c]>>13; /* ~0.4 s */
        if(a>chip->orchestra_rpeak[c])chip->orchestra_rpeak[c]=a;
        if(!chip->orchestra_gain[c])chip->orchestra_gain[c]=1024;
    }
    if(++chip->orchestra_tick<MATCH_EVERY)return;
    chip->orchestra_tick=0;
    /* A recording merged across parts answers for all of them. */
    for(UINT c=0;c<3;c++)native[owner[c]]+=chip->orchestra_npow[c];
    for(UINT c=0;c<3;c++) {
        const UINT64 rp=chip->orchestra_rpow[c];
        /* Hold the gain through silence and release tails; no hunting on noise. */
        if(native[c]<MATCH_FLOOR || rp<MATCH_FLOOR)continue;
        /* Followers measure the pre-gain output, so the ratio is the gain itself. */
        const float ratio=sqrtf((float)native[c]/(float)rp)*1024.0f;
        /* Never lift a part beyond the full scale one native channel can reach;
         * recordings carry more crest than an FM sine of the same power. */
        const UINT32 pk=chip->orchestra_rpeak[c];
        const float ceiling=pk ? (float)HEADROOM*1024.0f/(float)pk : (float)MATCH_MAX;
        float want=ratio<ceiling ? ratio : ceiling;
        if(want<1024.0f && ratio>=1024.0f)want=1024.0f; /* headroom never cuts below calibration */
        const UINT target=want<MATCH_MIN ? MATCH_MIN : want>MATCH_MAX ? MATCH_MAX : (UINT)want;
        SINT32 g=chip->orchestra_gain[c];
        g+=((SINT32)target-g)/192; /* ~140 ms glide: follows phrases; never fast enough to wah */
        if(pk && (UINT64)g*pk>(UINT64)HEADROOM*1024u)g=(SINT32)((UINT64)HEADROOM*1024u/pk); /* headroom at once */
        chip->orchestra_gain[c]=(UINT16)g;
    }
}

void orchestra_frame(FmChip *chip,SINT32 *left,SINT32 *right)
{
    const int mode=orchestra_enabled(); /* 0 native, 1 Synthesizer, 2 Orchestra */
    if(mode && mode!=chip->orchestra_last_mode) {
        if(mode==2) memset(chip->samples,0,sizeof chip->samples);
        else if(chip->orchestra_sample_mix) memset(chip->orchestra,0,sizeof chip->orchestra);
    }
    chip->orchestra_last_mode=(UINT8)mode;
    if(mode) {if(chip->orchestra_mix<1024) chip->orchestra_mix+=2;}
    else if(chip->orchestra_mix) chip->orchestra_mix-=2;
    if(mode==2) {if(chip->orchestra_sample_mix<1024) chip->orchestra_sample_mix+=2;}
    else if(mode==1 && chip->orchestra_sample_mix) chip->orchestra_sample_mix-=2;
    SINT32 sl[3]={0,0,0},sr[3]={0,0,0},al[3]={0,0,0},ar[3]={0,0,0},mono[3],peak[3];
    UINT8 owner[3]={0,1,2};
    if(chip->orchestra_sample_mix<1024) synthesizer_frame(chip,sl,sr);
    if(chip->orchestra_sample_mix) orchestra_sample_channels(chip,al,ar,owner);
    const UINT mix=chip->orchestra_sample_mix;
    SINT64 l=0,r=0;
    for(UINT c=0;c<3;c++) {
        const SINT64 cl=((SINT64)sl[c]*(1024-mix)+(SINT64)al[c]*mix)/1024;
        const SINT64 cr=((SINT64)sr[c]*(1024-mix)+(SINT64)ar[c]*mix)/1024;
        mono[c]=(SINT32)((cl+cr)/2);
        {const SINT64 pl=cl<0?-cl:cl,pr=cr<0?-cr:cr;peak[c]=(SINT32)(pl>pr?pl:pr);}
        const SINT64 g=chip->orchestra_gain[c] ? chip->orchestra_gain[c] : 1024;
        l+=cl*g/1024;r+=cr*g/1024;
    }
    match_update(chip,mono,peak,owner);
    *left=(SINT32)l;*right=(SINT32)r;
}
