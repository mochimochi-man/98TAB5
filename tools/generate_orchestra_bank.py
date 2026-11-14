# SPDX-License-Identifier: MIT
# Copyright (c) mochimochi-man / Uh
"""Regenerate the embedded CC0 orchestra bank (normal builds use committed data).

Four bowed sections (violins, violas, celli, contrabass) at roughly five-semitone
steps, each note in two independent recordings (two dynamic layers of the same
section), plus harp one-shots. Two different recordings layered never comb-filter
into a wah the way delayed copies of one recording do.

Then the rest of the orchestra, one recording a note: horn, trumpet and trombone,
flute, oboe, clarinet and bassoon sustains with their own attacks, and pizzicato
basses and celli and glockenspiel one-shots."""
from pathlib import Path
import json,numpy as np,hashlib,math,re
import argparse,urllib.request,urllib.parse,subprocess
COMMIT='440300901dfe9275fd84e0b7763af1f8443ae62e'
RAW='https://raw.githubusercontent.com/sgossner/VSCO-2-CE/'+COMMIT+'/'
# family, directory, file pattern (NOTE / LAYER), notes, layer tags (primary, second)
SECTIONS=[
 (0,'Strings/Violin Section/susVib','VlnEns_susVib_{n}_{l}.wav',['A2','B2','D3','F#3','A3','C4','E4','G4','B4','D5'],['v2','v1']),
 (1,'Strings/Viola Section/susvib','ViolaEns_susvib_{n}_{l}_1.wav',['C2','E2','G2','B2','D3','F3','A3','C4'],['v2','v1']),
 (2,'Strings/Cello Section/susvib','susvib_{n}_{l}_1.wav',['C1','E1','G1','B1','D2','F2','A2','C3','E3'],['v3','v1']),
 (3,'Strings/Solo Contrabass/SusVib','BKCtbss_SusVib_{n}_{l}_rr1.wav',['F#0','A#0','D1','F#1','A1','C#2'],['v3','v1']),
]
HARP=(4,'Strings/Harp',['E1_f','B1_mf','F2_mf','C3_mf','G3_mf','D4_mf','A4_mf','E5_mf','B5_mf','F6_mf'])
# family, directory, file pattern (NOTE), notes, sustain length, loop crossfade
WINDS=[
 (5,'Brass/F Horn/sus','MOHorn_sus_{n}_1.wav',['D#1_v2','A#1_v2','F2_v2','C3_v2','D4_v1'],26460,512),
 (6,'Brass/Trumpet/sus','Sum_SHTrumpet_sus_{n}_v3_rr1.wav',['F2','C3','G3','D4','A4'],26460,512),
 (7,'Brass/Tenor Trombone/sus','tenortbn_sus_{n}_v2_1.wav',['A#0','D#1','A#1','F2','C3','F3'],26460,512),
 (8,'Woodwinds/Flute/susvib','LDFlute_susvib_{n}_v1_1.wav',['C3','A3','E4','C5','E5'],33075,1024),
 (9,'Woodwinds/Oboe/Vib','Oboe_Vib_{n}_v3_Main.wav',['A#2','F3','D4','A#4','F5'],33075,1024),
 (10,'Woodwinds/Clarinet/susLong','DCClar_susLong_{n}_v2_rr1_sum.wav',['D2','A#2','F3','D4','A#4','F#5'],26460,512),
 (11,'Woodwinds/Bassoon/sus','PSBassoon_{n}_v2_1.wav',['A#0','F1','C2','G2','C3','G#3'],26460,512),
]
# family, directory, file pattern (NOTE), notes, length
SHORTS=[
 (12,'Strings/Solo Contrabass/Pizz','BKCtbss_Pizz_{n}_v1_rr1.wav',['F#0','A#0','E1','A1','E2'],22050),
 (13,'Strings/Cello Section/pizzT','pizzT_{n}_v2_RR1.wav',['C1','G1','D2','A2','E3','B3'],22050),
 (14,'Percussion/Glock','glock_medium_{n}.wav',['G5','C6','G6','C7'],26460),
]
RATE=22050;SUSTAIN=26460;PLUCK=33075;LOOP_START=5500;CROSS=512
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cache',type=Path,required=True)
parser.add_argument('--ffmpeg',default='ffmpeg')
parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'components/pc98core/sound/samples')
args=parser.parse_args();src=args.cache;src.mkdir(parents=True,exist_ok=True);out=args.output;out.mkdir(parents=True,exist_ok=True)

def fetch(path):
    local=src/path.replace('/','__')
    if not local.exists():local.write_bytes(urllib.request.urlopen(RAW+urllib.parse.quote(path)).read())
    pcm=local.with_suffix('.pcm')
    subprocess.run([args.ffmpeg,'-v','error','-y','-i',str(local),'-ac','1','-ar',str(RATE),'-af','lowpass=f=9500','-f','s16le',str(pcm)],check=True)
    return local,np.fromfile(pcm,dtype='<i2').astype(float)

def label_hz(note,octave_base=2):
    m=re.match(r'([A-G])(#?)(\d)',note)
    semi={'C':0,'D':2,'E':4,'F':5,'G':7,'A':9,'B':11}[m[1]]+(1 if m[2] else 0)
    # VSCO section/solo string labels sit one octave below scientific; harp labels are scientific
    return 440*2**(((int(m[3])+octave_base)*12+semi-69)/12)

def measure(v,expected):
    """Fundamental from the sustain's autocorrelation. The label convention is
    checked an octave either side and the shortest strong period wins."""
    section=v[4000:16000] if len(v)>16000 else v[len(v)//4:]
    N=1<<16;ac=np.fft.irfft(abs(np.fft.rfft(section-section.mean(),n=N))**2,n=N);ac/=ac[0]
    best=None
    # The label first; the strong second harmonic of a bowed note must not
    # pull the estimate an octave up.
    for f in (expected,expected/2,expected*2):
        lo=max(2,int(RATE/f*.97));hi=int(RATE/f*1.03)+2
        if hi>=len(ac)-1:continue
        lag=max(range(lo,hi),key=lambda x:ac[x])
        if ac[lag]>.4:best=lag;break
    if best is None:raise SystemExit('pitch not found near %.1f Hz'%expected)
    d=.5*(ac[best-1]-ac[best+1])/(ac[best-1]-2*ac[best]+ac[best+1])
    return RATE/(best+d)

def trim(v,length):
    rms=np.sqrt(np.convolve(v*v,np.ones(110)/110,mode='same'))
    start=max(0,int(np.flatnonzero(rms>float(np.percentile(rms,75))*.3)[0])-66)
    v=v[start:start+length];v=v-np.mean(v)
    if len(v)<length:v=np.pad(v,(0,length-len(v)))
    v[:44]*=np.linspace(0,1,44);return v,start

def envelope(x,w,circular):
    k=np.ones(w)/w
    if circular:
        y=np.concatenate([x[-w:],x,x[:w]])**2;return np.sqrt(np.convolve(y,k,'same')[w:-w])
    return np.sqrt(np.convolve(x*x,k,'same'))

def steady(v,ls,frequency):
    """Hold every harmonic of the loop at its own level. A section's players drift against each other, and each
    harmonic of the recording swells and fades with that (10 to 20 dB); in a loop of under a second the same swells
    come round every time, and a held note waves. The loop is taken apart into its harmonics (it is circular, so one
    transform of the whole loop does it exactly), each is levelled, and what leads into the loop is brought to the
    same levels over its last tenth of a second."""
    loop=v[ls:];n=len(loop);win=int(RATE*.03)
    def bands(x):
        X=np.fft.fft(x);f=np.fft.fftfreq(len(x),1/RATE);h=np.rint(np.abs(f)/frequency).astype(int)
        top=min(48,int((RATE/2-frequency)/frequency))
        out=[]
        for k in range(1,top+1):
            Z=np.where((h==k)&(f>0),2*X,0);out.append(np.fft.ifft(Z))
        rest=x-sum(z.real for z in out) if out else x
        return out,rest
    lb,lrest=bands(loop);pb,prest=bands(v)
    ramp=min(ls,int(RATE*.1));t=np.zeros(ls);t[ls-ramp:]=np.linspace(0,1,ramp) if ramp else 0
    new=lrest.copy();pre=prest[:ls].copy()
    for z,zp in zip(lb,pb):
        e=np.abs(z);pad=np.concatenate((e[-win:],e,e[:win]));c=np.cumsum(np.concatenate(([0.],pad)))
        e=(c[2*win+1:2*win+1+n]-c[:n])/(2*win+1)
        g=np.clip(float(np.mean(e))/np.maximum(e,1e-6),.25,4.)
        new+=z.real*g;pre+=zp.real[:ls]*(1+t*(g[0]-1))
    v=v.copy();v[ls:]=new;v[:ls]=pre
    return v

def sustain(v,frequency,cross=CROSS,span=0):
    """Loop the body, then flatten its slow level swell: a short loop repeats
    any bow/vibrato swell as a periodic wah.
    cross: the loop's crossfade. span: how far back the loop's end may be looked
    for; a solo player's vibrato has to meet itself in step as well as the
    waveform, so those look over a whole turn of it."""
    head=v[LOOP_START:LOOP_START+cross];hn=np.linalg.norm(head);period=round(RATE/frequency)
    def coherence(end):
        tail=v[end-cross:end];return np.dot(head,tail)/(hn*np.linalg.norm(tail)+1e-12)
    end=max(range(len(v)-max(span,min(2*period,1024)),len(v)+1),key=coherence)
    v=v[:end].copy();w=np.linspace(1/cross,1,cross)
    v[-cross:]=v[-cross:]*(1-w)+v[LOOP_START:LOOP_START+cross]*w
    ls=LOOP_START+cross;win=int(RATE*.04)
    loop=v[ls:];target=math.sqrt(float(np.mean(loop**2)))
    before=envelope(loop,win,True);gain=np.clip(target/np.maximum(before,1),.5,2.)
    v[ls:]=loop*gain
    ramp=min(ls,int(RATE*.1));pre=envelope(v[:ls],win,False)[ls-ramp:]
    t=np.linspace(0,1,ramp);v[ls-ramp:ls]*=(1-t)+t*np.clip(target/np.maximum(pre,1),.5,2.)
    v=steady(v,ls,frequency)
    peak=abs(v).max()
    if peak>32000:v*=32000/peak
    after=envelope(v[ls:],win,True)
    swell=(20*math.log10(before.max()/max(before.min(),1)),20*math.log10(after.max()/max(after.min(),1)))
    return v,ls,swell

bank=bytearray();zones=[];info=[]
def add(family,layer,path,note,looping,octave_base=2,length=0,cross=CROSS):
    local,v=fetch(path)
    v,start=trim(v,length or (SUSTAIN if looping else PLUCK))
    frequency=measure(v,label_hz(note,octave_base))
    sustained=math.sqrt(float(np.mean(v[4000:16000]**2)))
    v*=min(12000/max(sustained,1),30000/max(abs(v).max(),1))
    swell=None
    # the solo winds with vibrato (the long crossfade) look for their loop's end over 0.3 s
    if looping:v,ls,swell=sustain(v,frequency,cross,int(RATE*.3) if cross>CROSS else 0)
    else:
        fade=441;v[-fade:]*=np.linspace(1,0,fade);ls=0
    pcm=np.clip(np.rint(v),-32767,32767).astype('<i2').tobytes();offset=len(bank)//2;bank.extend(pcm)
    body=np.frombuffer(pcm,dtype='<i2').astype(float);body=body[ls:] if looping else body[:2205]
    gain_q12=round(min(4.,23170/max(1,math.sqrt(float(np.mean(body**2)))))*4096)
    root_inc=round(frequency*1048576/(3993600/72))
    level=(offset,len(v),ls,len(v) if looping else 0,RATE,gain_q12)
    zones.append((family,root_inc,layer,level))
    info.append(dict(path=path,url=RAW+urllib.parse.quote(path),source_sha256=hashlib.sha256(local.read_bytes()).hexdigest(),
                     family=family,layer=layer,measured_hz=round(frequency,4),trim_start=start,
                     loop_swell_db=None if swell is None else [round(swell[0],2),round(swell[1],2)]))
    print(family,layer,path.split('/')[-1],'%.2f Hz'%frequency,'' if swell is None else 'swell %.1f -> %.1f dB'%swell)

for family,directory,pattern,notes,layers in SECTIONS:
    for note in notes:
        for layer,tag in enumerate(layers):add(family,layer,directory+'/'+pattern.format(n=note,l=tag),note,True)
for name in HARP[2]:add(HARP[0],0,HARP[1]+'/KSHarp_'+name+'.wav',name.split('_')[0],False,1)
for family,directory,pattern,notes,length,cross in WINDS:
    for note in notes:add(family,0,directory+'/'+pattern.format(n=note),note.split('_')[0],True,2,length,cross)
for family,directory,pattern,notes,length in SHORTS:
    for note in notes:add(family,0,directory+'/'+pattern.format(n=note),note,False,2,length)

(out/'orchestra_bank.bin').write_bytes(bank)
h=['/* Generated from CC0 VSCO 2 CE; see samples/SOURCES.json and LICENSE. */',
   '/* family: 0 violins, 1 violas, 2 celli, 3 contrabass, 4 harp, 5 horn, 6 trumpet, 7 trombone, 8 flute, 9 oboe, 10 clarinet,',
   ' * 11 bassoon, 12 pizzicato basses, 13 pizzicato celli, 14 glockenspiel. layer: independent recording 0 / 1. */',
   'typedef struct { UINT32 offset,length,loop_start,loop_end,rate,gain_q12; } OrchSampleLevel;',
   'typedef struct { UINT family,root_inc,layer; OrchSampleLevel level[5]; } OrchSampleZone;',
   'static const OrchSampleZone sample_zones[] = {']
for family,root,layer,level in zones:
    one='{'+','.join(map(str,level))+'}'
    h.append('{%d,%d,%d,{%s}},'%(family,root,layer,','.join([one]*5)))  # dense zones: no octave mips needed
h+=['};','#define ORCH_SAMPLE_ZONES (sizeof(sample_zones)/sizeof(sample_zones[0]))']
(out/'orchestra_bank.h').write_text('\n'.join(h)+'\n')
assert (out/'LICENSE').exists()
(out/'SOURCES.json').write_text(json.dumps({'library':'VSCO 2 Community Edition','license':'CC0-1.0','upstream':'https://github.com/sgossner/VSCO-2-CE','commit':COMMIT,
    'bank_sha256':hashlib.sha256(bank).hexdigest(),'bytes':len(bank),
    'format':'signed little-endian mono PCM16 at 22050 Hz; one level per zone (zones at about five-semitone steps)','sources':info},indent=2)+'\n')
(out/'README.md').write_text('''# Compact orchestra sample bank

Derived from VSCO 2 Community Edition (CC0-1.0).
Recorded by Sam Gossner & Simon Dalzell; original sample cutting by Elan Hickler / Soundemote.
Upstream: https://github.com/sgossner/VSCO-2-CE
Publisher: https://versilian-studios.com/vsco-community/

Violin, viola and cello section and solo contrabass sustains with vibrato at about five-semitone steps, each note in two
independent recordings (two dynamic layers), and harp one-shots; horn, trumpet, trombone, flute, oboe, clarinet
and bassoon sustains, pizzicato basses and celli and glockenspiel one-shots, one recording a note. Adaptations: mono
conversion, 22050 Hz resampling, attack trimming, DC removal, level normalization, crossfaded sustain loops whose slow
level swell is flattened and whose harmonics are each held at their own level (so a short loop does not repeat the
players' drifting as a wave), and one-shot fade-outs. No source game audio is included.

SOURCES.json records the exact upstream revision, file hashes, measured tuning and the loop swell before / after.
LICENSE contains the upstream CC0 dedication. The normal build is offline and uses the committed bank directly.
Regeneration requires Python, NumPy and FFmpeg; see tools/generate_orchestra_bank.py.
''')
print('BANK BYTES',len(bank),'zones',len(zones));assert len(bank)<9*1048576
