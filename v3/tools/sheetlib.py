import re, json, base64, io, sys
import numpy as np
from PIL import Image
from scipy import ndimage as ndi

SRC='/mnt/user-data/uploads/DUSK_CRUSHER_Gothic_Arcade_v10.html'

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

