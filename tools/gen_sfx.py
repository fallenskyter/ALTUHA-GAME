import re, base64, io, wave, struct, math, random
import numpy as np
from scipy import signal
SR=22050
random.seed(7); np.random.seed(7)
rand=lambda a,b: random.uniform(a,b)
clamp=lambda v,a,b: max(a,min(b,v))
s=open('/mnt/user-data/uploads/DUSK_CRUSHER_Gothic_Arcade_v10.html',encoding='utf8').read()
def b64wav(b):
    w=wave.open(io.BytesIO(base64.b64decode(b)))
    x=np.frombuffer(w.readframes(w.getnframes()),np.int16).astype(np.float32)/32768.0
    return x.reshape(-1,w.getnchannels()).mean(axis=1), w.getframerate()
blobs=re.findall(r'["\'`](UklGR[A-Za-z0-9+/=]{200,})["\'`]',s)
real_thud,r1=b64wav(blobs[0]); real_crunch,r2=b64wav(blobs[1])
print('real thud',len(real_thud)/r1,'crunch',len(real_crunch)/r2,r1,r2)

# ---------- DSP helpers (RBJ biquads, same maths as WebAudio BiquadFilterNode) ----------
def biquad(x,kind,f,Q=0.7071):
    w0=2*math.pi*f/SR; al=math.sin(w0)/(2*Q); c=math.cos(w0)
    if kind=='lp': b=[(1-c)/2,1-c,(1-c)/2]
    elif kind=='hp': b=[(1+c)/2,-(1+c),(1+c)/2]
    else: b=[al,0,-al]          # bandpass, constant 0 dB peak
    a=[1+al,-2*c,1-al]
    return signal.lfilter(np.array(b)/a[0],np.array(a)/a[0],x).astype(np.float32)
def resample_rate(x,rate):       # playbackRate
    n=int(len(x)/rate); idx=np.arange(n)*rate
    return np.interp(idx,np.arange(len(x)),x).astype(np.float32)
def expenv(n,peak,t_attack,dur,floor=0.001):
    t=np.arange(n)/SR; e=np.zeros(n,np.float32)
    att=t<t_attack
    e[att]=0.0001+(peak-0.0001)*(t[att]/max(t_attack,1e-6))
    rest=~att
    tt=np.clip((t[rest]-t_attack)/max(dur-t_attack,1e-6),0,1)
    e[rest]=peak*(floor/peak)**tt
    e[t>dur]=0
    return e
def add(dst,src,at):
    i=int(at*SR); 
    if i>=len(dst): return
    n=min(len(src),len(dst)-i); dst[i:i+n]+=src[:n]
white=(np.random.rand(int(SR*0.25))*2-1).astype(np.float32)
def pink(n):
    w=np.random.randn(n); b=signal.lfilter([0.049922035,-0.095993537,0.050612699,-0.004408786],[1,-2.494956002,2.017265875,-0.522189400],w)
    return (b/np.abs(b).max()).astype(np.float32)
def brown(n):
    w=np.random.randn(n); b=np.cumsum(w); b-=signal.lfilter([1],[1,-0.999],b)*0  # integrate
    b=signal.lfilter([1,-1],[1,-0.995],b)
    return (b/np.abs(b).max()).astype(np.float32)

def bone_snap(freq,power):
    dur=rand(0.020,0.052); n=int((dur+0.03)*SR)
    off=int(rand(0,0.15)*SR); seg=np.tile(white,2)[off:off+int(n*1.4)]
    seg=resample_rate(seg,rand(0.9,1.3))[:n]
    seg=biquad(biquad(seg,'hp',650),'bp',freq,rand(2.2,4.5))
    seg=seg*expenv(len(seg),1.25*power,0.0009,dur,0.0008)
    t=np.arange(int(dur*1.7*SR))/SR
    f=freq*rand(0.28,0.45)*(0.18/0.45/ (rand(0.28,0.45)/0.45))**np.clip(t/(dur*1.4),0,1)
    f=freq*0.35*(0.18/0.35)**np.clip(t/(dur*1.4),0,1)
    ph=2*math.pi*np.cumsum(f)/SR
    tri=(2/math.pi*np.arcsin(np.sin(ph))).astype(np.float32)
    tri*=expenv(len(tri),0.42*power,0.0012,dur*1.5,0.0008)
    out=np.zeros(max(len(seg),len(tri)),np.float32); out[:len(seg)]+=seg; out[:len(tri)]+=tri
    return out

# ---------- procedural stomp buffers (port of the JS sample loops) ----------
def make_stomp():
    nS=int(SR*0.76); out=np.zeros(nS,np.float32)
    impact=int(SR*0.28); fBase=rand(125,148)
    t=np.arange(impact)/SR
    f=fBase*np.exp(-t*10.5)+34
    ph1=2*math.pi*np.cumsum(f)/SR; ph2=2*math.pi*np.cumsum(f*1.98)/SR; ph3=2*math.pi*np.cumsum(f*2.95)/SR
    env=np.minimum(1,np.arange(impact)/(SR*0.004))*np.exp(-t*8.2)
    body=(np.sin(ph1)*0.62+np.sin(ph2)*0.30+np.sin(ph3)*0.14)*env
    wn=np.random.rand(impact)*2-1
    lp=signal.lfilter([0.28],[1,-0.72],wn)
    slap=lp*np.exp(-t*42)*1.35
    out[:impact]+=body*1.15+slap
    tt=np.arange(nS)/SR; wn=np.random.rand(nS)*2-1
    r1=r2=hp=prev=0.0; rum=np.zeros(nS)
    for i in range(nS):
        cut=0.045+0.095*math.exp(-tt[i]*3.8)
        r1+=cut*(wn[i]-r1); r2+=cut*(r1-r2)
        hp=0.985*(hp+r2-prev); prev=r2; rum[i]=hp
    out+=rum*np.minimum(1,tt/0.025)*np.exp(-tt*3.6)*3.6
    dry=out[:int(SR*0.22)].copy()
    for ms,g in [(0.038,0.46),(0.078,0.34),(0.135,0.24),(0.210,0.15)]:
        off=int(ms*SR); damp=0
        for i in range(len(dry)):
            if off+i>=nS: break
            damp=damp*0.65+dry[i]*0.35; out[off+i]+=damp*g
    for _ in range(16):
        st=int(rand(0.018,0.24)*SR); gl=min(nS-st,int(SR*rand(0.004,0.012))); ga=rand(0.10,0.26)
        out[st:st+gl]+=(np.random.rand(gl)*2-1)*ga*np.exp(-np.arange(gl)/(gl*0.35))
    out=np.tanh(out*1.55); out/=max(1e-3,np.abs(out).max())*1.0/0.98
    return out.astype(np.float32)
stomps=[make_stomp() for _ in range(4)]

def norm(x,peak):
    m=np.abs(x).max()
    return (x/m*peak).astype(np.float32) if m>1e-6 else x
out={}

# ---------- playThud ----------
def make_thud(intensity,fp,heavy_force=None):
    power=clamp(0.75+intensity*0.65,0.82,1.70); heavy=fp or intensity>=0.68; TRIM=0.36
    L=int(SR*0.8); m=np.zeros(L,np.float32)
    s1=resample_rate(random.choice(stomps),rand(0.80,0.92) if heavy else rand(0.92,1.04)); add(m,s1*power*1.58*TRIM,0)
    s2=resample_rate(random.choice(stomps),rand(0.72,0.84) if heavy else rand(0.84,0.96)); add(m,s2*power*0.95*TRIM,0.015)
    add(m,resample_rate(real_thud,rand(0.74,0.86) if heavy else rand(0.86,1.0))*power*1.22*TRIM,0)
    n=int(0.44*SR); t=np.arange(n)/SR
    f0,T=(114,0.38) if heavy else (102,0.26)
    f=f0*(23/f0)**np.clip(t/T,0,1); ph=2*math.pi*np.cumsum(f)/SR
    g=power*(0.92 if heavy else 0.62)*TRIM*(0.001/1.0)**np.clip(t/(0.42 if heavy else 0.29),0,1)
    add(m,(np.sin(ph)*g).astype(np.float32),0)
    return norm(m,0.95 if heavy else 0.80)
out['thud_n0']=make_thud(0.6,False); out['thud_n1']=make_thud(0.5,False)
out['thud_h0']=make_thud(1.0,True); out['thud_h1']=make_thud(1.3,True)

# ---------- playCrunch / playBoneBreak ----------
def crunch_slice(power,fp,rate=None):
    sliceDur=rand(0.16,0.26)*(1.25 if fp else 1.0)
    off=rand(0,max(0,len(real_crunch)/r2-sliceDur-0.1))
    seg=real_crunch[int(off*r2):int((off+sliceDur+0.03)*r2)]
    seg=resample_rate(seg,rate or rand(1.30,1.62)); seg=biquad(seg,'hp',1100)
    return seg*expenv(len(seg),0.62*power,0.004,sliceDur,0.001)
def make_crunch(intensity,fp,kills):
    power=clamp(intensity*0.80,0.70,1.55); n=clamp(3+round(kills*0.8)+(3 if fp else 0),3,10)
    m=np.zeros(int(SR*0.9),np.float32); t=0.0
    for k in range(n):
        lead=(k==0)
        add(m,bone_snap(rand(1500,2300) if lead else rand(900,3200),power*(1.0 if lead else rand(0.40,0.85))),t)
        t+=rand(0.010,0.048)*(1+k*0.10)
    add(m,crunch_slice(power,fp),0)
    nT=int(0.09*SR); tt=np.arange(nT)/SR; f=(190 if fp else 220)*(70/(190 if fp else 220))**np.clip(tt/0.07,0,1)
    th=np.sin(2*math.pi*np.cumsum(f)/SR)*expenv(nT,0.38*power,0.003,0.085)
    add(m,th.astype(np.float32),0)
    last=np.nonzero(np.abs(m)>0.01)[0]; m=m[:(last[-1]+int(0.02*SR)) if len(last) else len(m)]
    return norm(m,0.85)
for i in range(3):
    out[f'crunch_a{i}']=make_crunch(1.05,False,1)
    out[f'crunch_b{i}']=make_crunch(1.25,False,5)
    out[f'crunch_c{i}']=make_crunch(1.55,True,8)
def make_bone(count):
    n=clamp(2+count*2,3,9); m=np.zeros(int(SR*0.5),np.float32); t=0.0
    for k in range(n):
        add(m,bone_snap(rand(1700,2500) if k==0 else rand(900,3200),1.35 if k==0 else rand(0.6,1.1)),t); t+=rand(0.008,0.035)
    last=np.nonzero(np.abs(m)>0.01)[0]; m=m[:last[-1]+int(0.02*SR)]
    return norm(m,0.85)
for i,c in enumerate([1,2,4]): out[f'bone{i}']=make_bone(c)

# ---------- smear start rumble, rub crunch, gritty crackle ----------
for i in range(2):
    st=resample_rate(random.choice(stomps),rand(0.76,0.90)); st=biquad(st[int(0.03*SR):],'lp',340)
    st=st[:int(0.58*SR)]; st=st*expenv(len(st),1.28,0.020,0.55,0.001); out[f'rumble{i}']=norm(st,0.8)
for i in range(3):
    speed=[0.2,0.5,0.9][i]; rate=clamp(0.84+speed*0.22,0.78,1.12); sd=rand(0.62,1.02)
    off=rand(0,max(0,len(real_crunch)/r2-sd-0.1)); seg=real_crunch[int(off*r2):int((off+sd*rate+0.05)*r2)]
    seg=resample_rate(seg,rate)[:int(sd*SR)]; seg=biquad(seg,'lp',6800+speed*3400)
    e=np.ones(len(seg),np.float32); a=int(0.045*SR); e[:a]=np.linspace(0.001,1,a); tail=int(sd*0.28*SR); e[-tail:]=np.geomspace(0.94,0.001,tail)
    out[f'rub{i}']=norm(seg*e,0.8)
for i in range(2):
    rate=rand(0.88,0.96); seg=resample_rate(real_crunch,rate)[:int(1.6*SR)]
    e=np.ones(len(seg),np.float32); a=int(0.006*SR); e[:a]=np.linspace(0.001,1,a); tail=int(0.5*SR); e[-tail:]=np.geomspace(0.95,0.001,tail)
    m=np.zeros(len(seg),np.float32); add(m,seg*e,0)
    for k in range(3): add(m,bone_snap(rand(1000,2800),0.55*rand(0.6,1.0))*0.8,k*rand(0.03,0.09))
    out[f'grit{i}']=norm(m,0.8)

# ---------- loops (2 s, cross-faded) ----------
def loopify(x,fade=int(0.15*SR)):
    n=len(x)-fade; y=x[:n].copy(); w=np.linspace(0,1,fade)
    y[:fade]=x[:fade]*w+x[n:n+fade]*(1-w); return y
N=int(SR*2.15)
b=brown(N); lp=biquad(b,'lp',260,1.6)
am=0.72+0.24*np.sin(2*math.pi*11*np.arange(N)/SR); out['loop_smear']=norm(loopify(lp*am),0.9)
pk=pink(N); out['loop_grit']=norm(loopify(biquad(pk,'bp',1180,2.8)),0.8)
out['loop_wet']=norm(loopify(biquad(pink(N),'bp',460,2.6)),0.8)
out['loop_desc']=norm(loopify(biquad(brown(N),'bp',120,1.4)),0.9)

names=list(out.keys())
with open('/tmp/v3/assets/sfx.bin','wb') as f:
    f.write(struct.pack('<4sI',b'SFX1',len(names)))
    off=0
    for nm in names:
        f.write(struct.pack('<16sII',nm.encode(),off,len(out[nm]))); off+=len(out[nm])
    for nm in names: f.write((np.clip(out[nm],-1,1)*32767).astype('<i2').tobytes())
with open('/tmp/v3/sfx_ids.h','w') as h:
    h.write('/* generated by tools/gen_sfx.py */\n#pragma once\nstatic const char *SFX_NAMES[]={'+','.join('"%s"'%n for n in names)+'};\n#define SFX_COUNT %d\n'%len(names))
print('sfx',len(names),'samples',sum(len(v) for v in out.values()),'KB',sum(len(v) for v in out.values())*2//1024)
