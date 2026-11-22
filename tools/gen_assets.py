import re, json, base64, io, sys
import numpy as np
from PIL import Image
from scipy import ndimage as ndi

SRC='/mnt/user-data/uploads/DUSK_CRUSHER_Gothic_Arcade_v10.html'
OUT='/tmp/v2'
K=480/1280.0
SX1=6.28*1.36   # sprite-unit -> canvas px (x), scale 1.0
SY1=6.28
s=open(SRC,encoding='utf8').read()
sheet_b64=re.search(r'DEFAULT_SHEET\s*=\s*"data:image/png;base64,([A-Za-z0-9+/=]+)"',s).group(1)
sheet=Image.open(io.BytesIO(base64.b64decode(sheet_b64))).convert('RGBA')
POSE_CAL=json.loads(re.search(r'POSE_CAL\s*=\s*(\[.*?\]);',s,re.S).group(1))
W,H=sheet.size
print('sheet',W,H,'poses cal',len(POSE_CAL))
SHEET_REF=1254; fx=W/SHEET_REF; fy=H/SHEET_REF
BOOT_COLS=[[8,198],[203,392],[398,590],[600,783]]
BOOT_ROWS=[[3,313],[315,626],[631,945]]
BOOT_CELLS=[[0,0],[0,1],[0,2],[0,3],[1,0],[1,1],[1,2],[1,3],[2,0],[2,1],[2,2]]
UPRIGHT=[0,1,2,5,6,9,10]
PORT_BOXES=[[958,25,1080,151],[1115,25,1237,151],[958,181,1080,308],[1115,181,1237,308],
 [958,337,1080,465],[1115,337,1237,465],[958,494,1080,621],[1115,494,1237,621],
 [958,651,1080,778],[1115,651,1237,778],[958,808,1080,935],[1115,808,1237,935],
 [959,1121,1080,1248],[1115,1121,1237,1248]]

# composite sheet over white like the JS does
white=Image.new('RGBA',(W,H),(255,255,255,255)); white.alpha_composite(sheet)
sg=np.array(white)

def crop(x0,y0,x1,y1):
    X0=max(0,round(x0*fx));X1=min(W,round(x1*fx));Y0=max(0,round(y0*fy));Y1=min(H,round(y1*fy))
    return sg[Y0:Y1,X0:X1].copy()

def erode(a):
    p=np.pad(a,1,constant_values=1)
    return (a & p[1:-1,:-2] & p[1:-1,2:] & p[:-2,1:-1] & p[2:,1:-1])

def matte(rgba,pocket):
    h,w=rgba.shape[:2]
    minc=rgba[:,:,:3].min(axis=2)
    cand=minc>=236
    lab,n=ndi.label(cand)            # 4-connectivity
    border=set(np.unique(np.concatenate([lab[0],lab[-1],lab[:,0],lab[:,-1]])))-{0}
    bg=np.isin(lab,list(border))
    if pocket:
        cand2=(minc>=250)&~bg
        lab2,n2=ndi.label(cand2)
        if n2:
            sizes=ndi.sum(cand2,lab2,range(1,n2+1))
            for i,sz in enumerate(sizes,1):
                if sz>=30: bg|=(lab2==i)
    fg=~bg
    lab,n=ndi.label(fg,structure=np.ones((3,3)))   # 8-connectivity
    if n:
        sizes=np.array(ndi.sum(fg,lab,range(1,n+1)))
        mx=sizes.max()
        keep=np.zeros(n+1,bool); keep[1:]=sizes>=mx*0.04
        fg=fg&keep[lab]
    e1=erode(fg); core=erode(e1)
    idx=ndi.distance_transform_edt(~core,return_distances=False,return_indices=True)
    rgb=rgba[:,:,:3][idx[0],idx[1]]
    a=e1.astype(np.float32)
    for _ in range(2): a=ndi.uniform_filter(a,size=3,mode='nearest')
    v=np.clip((a-0.08)/0.84,0,1)
    out=np.dstack([rgb,np.round(v*255).astype(np.uint8)])
    return out.astype(np.uint8)

raw=[]
for r,c in BOOT_CELLS:
    y0,y1=BOOT_ROWS[r]; x0,x1=BOOT_COLS[c]
    cr=crop(x0,y0,x1,y1)
    rgba=matte(cr,False)
    al=rgba[:,:,3]>128
    ys,xs=np.where(al)
    minX,maxX,minY,maxY=xs.min(),xs.max(),ys.min(),ys.max()
    hTrim=min(cr.shape[0],maxY+1+round(2*fy))
    raw.append(dict(rgba=rgba[:hTrim],w=cr.shape[1],h=hTrim,minX=minX,maxX=maxX,minY=minY,maxY=maxY))
ratios=sorted((raw[i]['maxX']+1-raw[i]['minX'])/POSE_CAL[i]['ow'] for i in UPRIGHT)
S=ratios[len(ratios)//2]
print('S=',S)
poses=[]
for i,p in enumerate(raw):
    cal=POSE_CAL[i]
    nxu0=p['minX']/S; nxu1=(p['maxX']+1)/S
    maxBot=(p['maxY']+1)/S
    botY=maxBot+cal['bo']
    soleMin=nxu0+cal['sm']; soleMax=nxu1+cal['sx']
    anchor=(nxu0+nxu1)/2+cal['an']
    al=p['rgba'][:,:,3]>128
    soleYs=[]
    for t in range(24):
        xc=round((soleMin+(soleMax-soleMin)*t/23)*S)
        xc=min(p['w']-1,max(0,xc))
        yb=-1
        for y in range(p['h']-1,-1,-1):
            if al[y,max(0,xc-1):min(p['w']-1,xc+1)+1].any(): yb=y;break
        soleYs.append(round(((yb+1 if yb>=0 else p['maxY']+1)/S)-botY,2))
    def centerAt(r):
        y=min(p['h']-1,round(r*S))
        xs=np.where(al[y])[0]
        return None if len(xs)==0 else (xs[0]+xs[-1]+1)/2/S
    c1=centerAt(4); c2=centerAt(30)
    slope=((c2-c1)/26) if (c1 is not None and c2 is not None) else 0
    wu=p['w']/S; hu=p['h']/S
    poses.append(dict(wu=wu,hu=hu,botY=botY,anchor=anchor,soleMin=soleMin,soleMax=soleMax,soleYs=soleYs,slope=slope))
    # final PSP-size image
    wpx=max(2,round(wu*SX1*K)); hpx=min(256,max(2,round(hu*SY1*K)))
    im=Image.fromarray(p['rgba'],'RGBA')
    # premultiply to avoid dark halos while resizing
    arr=np.array(im).astype(np.float32); a=arr[:,:,3:4]/255.0
    pm=np.dstack([arr[:,:,:3]*a,arr[:,:,3:4]]).astype(np.uint8)
    pmim=Image.fromarray(pm,'RGBA').resize((wpx,hpx),Image.LANCZOS)
    q=np.array(pmim).astype(np.float32); qa=q[:,:,3:4]/255.0
    rgb=np.where(qa>0.003,q[:,:,:3]/np.maximum(qa,0.003),0)
    fin=np.dstack([np.clip(rgb,0,255),q[:,:,3:4]]).astype(np.uint8)
    # pad to power-of-two texture
    tw=1
    while tw<wpx: tw*=2
    th=1
    while th<hpx: th*=2
    tex=np.zeros((th,tw,4),np.uint8); tex[:hpx,:wpx]=fin
    # make sure the last row/col stay opaque-ish for leg stretching: nothing needed
    open(f'{OUT}/assets/boot_{i:02d}.rgba','wb').write(tex.tobytes())
    poses[-1].update(wpx=wpx,hpx=hpx,tw=tw,th=th)
    print(i,'units',round(wu,1),round(hu,1),'px',wpx,hpx,'tex',tw,th,'botY',round(botY,1),'anchor',round(anchor,1),'slope',round(slope,3))

# header
with open(f'{OUT}/poses.h','w') as f:
    f.write('/* generated by tools/gen_assets.py */\n#pragma once\n#define NPOSE 11\ntypedef struct { float wu,hu,botY,anchor,soleMin,soleMax,slope; int wpx,hpx,tw,th; float soleYs[24]; } PoseInfo;\nstatic const PoseInfo POSES[NPOSE] = {\n')
    for p in poses:
        f.write('  {%.3ff,%.3ff,%.3ff,%.3ff,%.3ff,%.3ff,%.4ff,%d,%d,%d,%d,{%s}},\n'%(p['wu'],p['hu'],p['botY'],p['anchor'],p['soleMin'],p['soleMax'],p['slope'],p['wpx'],p['hpx'],p['tw'],p['th'],','.join('%.2ff'%v for v in p['soleYs'])))
    f.write('};\n')
print('header ok')
