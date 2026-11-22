import re, json, base64, io, math
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as ndi
from sheetlib import *
OUT='/tmp/v3'
W,H=1280,720
ASPHALT_TOP,CURB_TOP=588,690

def hexc(h):
    h=h.lstrip('#'); return tuple(int(h[i:i+2],16) for i in (0,2,4))
img=Image.new('RGB',(W,H),(0,0,0)); d=ImageDraw.Draw(img,'RGBA')
def px(x,y,w,h,c):
    col=hexc(c) if isinstance(c,str) else c
    d.rectangle([round(x),round(y),round(x)+round(w)-1,round(y)+round(h)-1],fill=col)
def vgrad(y0,y1,stops):
    arr=np.array(img).astype(np.float32)
    ys=np.arange(H)
    t=np.clip((ys-y0)/(y1-y0),0,1)
    def interp(t,stops):
        out=np.zeros((len(t),4),np.float32)
        for i,tt in enumerate(t):
            for k in range(len(stops)-1):
                if tt<=stops[k+1][0]:
                    a,b=stops[k],stops[k+1]; f=(tt-a[0])/max(1e-6,(b[0]-a[0]))
                    out[i]=[a[1][j]+(b[1][j]-a[1][j])*f for j in range(4)]; break
            else: out[i]=stops[-1][1]
        return out
    return interp(t,stops)
def fill_grad(y0,y1,stops):
    global img,d
    arr=np.array(img).astype(np.float32)
    cols=vgrad(y0,y1,stops)
    for y in range(max(0,int(y0)),min(H,int(y1))):
        r,g,b,a=cols[y]; arr[y,:,:]=arr[y,:,:]*(1-a)+np.array([r,g,b])*a
    img=Image.fromarray(arr.astype(np.uint8)); d=ImageDraw.Draw(img,'RGBA')
def rgba(h,a=1.0):
    r,g,b=hexc(h); return (r,g,b,a)
fill_grad(0,ASPHALT_TOP,[(0,(*hexc('#C88AB8'),1)),(0.42,(*hexc('#8C5682'),1)),(0.82,(*hexc('#55305B'),1)),(1,(*hexc('#3D2144'),1))])
fill_grad(320,ASPHALT_TOP,[(0,(200,138,184,0)),(0.65,(138,78,134,0.22)),(1,(42,22,52,0.45))])
far=[(-10,120,96,0),(82,185,88,10),(168,92,112,0),(278,205,94,-8),(370,148,125,0),(492,84,104,0),(594,174,115,12),(706,118,132,0),(836,188,98,0),(932,98,138,-10),(1068,152,116,0),(1182,112,110,0)]
for x,y,w,sl in far:
    d.polygon([(x,ASPHALT_TOP),(x,y+max(0,sl)),(x+w,y+max(0,-sl)),(x+w,ASPHALT_TOP)],fill=hexc('#4A284E'))
    px(x+18,y-10,20,10,'#4A284E'); px(x+w-22,y-32,2,32,'#4A284E')
fill_grad(180,ASPHALT_TOP,[(0,(140,86,130,0.16)),(1,(76,43,82,0.36))])
mid=[(0,195,165,'#48274F','#623768','strips'),(165,275,215,'#5A325F','#744377','balcony'),(380,230,190,'#44244A','#5E3464','sparse'),(570,190,225,'#562F5B','#6E4074','strips'),(795,255,180,'#412147','#5B3161','balcony'),(975,175,165,'#4E2A54','#673A6D','sparse'),(1140,215,145,'#3B1E41','#522C59','strips')]
for bx,by,bw,c,e,st in mid:
    px(bx,by,bw,ASPHALT_TOP-by,c); px(bx,by,bw,6,e); px(bx+bw-5,by,5,ASPHALT_TOP-by,'#2C1532'); px(bx+12,by-12,28,12,'#351B3B'); px(bx+48,by-8,18,8,'#2D1633')
    fy=by+28
    while fy<ASPHALT_TOP-72:
        d.rectangle([round(bx+8),round(fy+24),round(bx+8)+bw-16-1,round(fy+24)+2],fill=(26,12,32,115))
        if st=='strips':
            px(bx+16,fy,bw-32,14,'#2D1734')
            if (bx+fy)%96==0: px(bx+22,fy+2,34,10,'#A86D9E')
            elif (bx*3+fy)%144==0: px(bx+bw-64,fy+2,28,10,'#72B8B8')
        elif st=='balcony':
            px(bx+14,fy+16,bw-28,7,'#2A1530')
            if (fy+bx)%3==0: px(bx+28,fy-2,26,16,'#9B608E')
            if (fy*5+bx)%7==0: px(bx+bw-72,fy,20,14,'#C688A8')
        else:
            if (bx+fy)%5==0: px(bx+24,fy,18,22,'#8F5888')
            if (bx+fy*3)%7==0: px(bx+bw-58,fy+4,24,14,'#5E4074')
        fy+=48
    px(bx+8,ASPHALT_TOP-56,bw-16,56,'#2D1834'); px(bx+16,ASPHALT_TOP-46,bw-32,34,'#1F1126')
poles=[58,396,848,1226]
for p in poles:
    px(p,215,7,ASPHALT_TOP-215,'#24142B'); px(p-18,232,43,4,'#24142B'); px(p-12,250,31,3,'#24142B')
for i in range(len(poles)-1):
    x1=poles[i]+3; x2=poles[i+1]+3
    for w in range(3):
        pts=[]
        for t in np.linspace(0,1,24):
            y=(1-t)**2*(234+w*9)+2*(1-t)*t*(266+w*14)+t*t*(234+w*9); x=x1+(x2-x1)*t
            pts.append((x,y))
        d.line(pts,fill=(28,14,34,209),width=2)
for args in [(16,206,30,150,'#23112B'),(19,209,24,144,'#FF5EA0'),(22,212,18,138,'#FFE168')]: px(*args)
for sy in range(222,340,22): px(25,sy,12,11,'#D82778'); px(27,sy+3,8,5,'#FFF09E')
for args in [(114,376,62,104,'#180E20'),(117,379,56,98,'#291836'),(117,379,56,16,'#B84A88'),(137,406,16,16,'#0B0810'),(131,422,28,44,'#0B0810'),(140,413,3,3,'#FF80C0'),(147,413,3,3,'#FF80C0'),
  (204,336,208,88,'#17212B'),(208,340,200,80,'#1D7469'),(212,344,192,72,'#28B69A'),(224,356,52,46,'#154B47'),(234,366,32,26,'#8BF5DA'),(290,354,98,15,'#123D3A'),(294,358,90,7,'#D7FFF4'),(290,377,74,11,'#FFE066'),(290,395,92,9,'#154B47'),
  (78,524,34,44,'#24192B'),(81,527,28,38,'#F5C431'),(85,531,20,15,'#FFF199'),(88,551,14,9,'#D48A1C'),(91,568,8,22,'#24182C'),
  (735,340,116,46,'#21142E'),(739,344,108,38,'#4E317A'),(747,352,92,9,'#8AF2E8'),(747,367,62,8,'#FF80C2')]: px(*args)
fill_grad(ASPHALT_TOP-42,ASPHALT_TOP,[(0,(54,30,62,0)),(1,(36,36,46,0.72))])
px(0,ASPHALT_TOP,W,CURB_TOP-ASPHALT_TOP,'#24242E'); px(0,ASPHALT_TOP,W,4,'#181721')
for i in range(480):
    gx=(i*97)%W; gy=ASPHALT_TOP+6+((i*53)%(CURB_TOP-ASPHALT_TOP-10)); px(gx,gy,4,2,'#2C2C38' if i%2==0 else '#1E1E27')
px(0,CURB_TOP,W,H-CURB_TOP,'#585D70'); px(0,CURB_TOP,W,3,'#16161F')
sw=28
for x in range(0,W,sw*2):
    px(x,CURB_TOP+3,sw,H-CURB_TOP-3,'#D6DAE8'); px(x,CURB_TOP+3,sw,5,'#F0F3FA'); px(x+sw,CURB_TOP+3,sw,5,'#72788F'); px(x,CURB_TOP+3,2,H-CURB_TOP-3,'#3B3E4D'); px(x+sw,CURB_TOP+3,2,H-CURB_TOP-3,'#3B3E4D')
px(0,H-5,W,5,'#282A36')
img.save('/tmp/v3/city_world.png')

# ---- final view texture: zoom 1.12 anchored bottom-centre, scaled by K, with shake margins ----
K=480/1280.0; Z=1.12; S=Z*K
rs=img.resize((round(W*S),round(H*S)),Image.LANCZOS)   # 537x302
offX=240+(0-640)*S; offY=272+(0-720)*S                  # PSP position of world (0,0)
TX,TY=512,288; MX,MY=16,8                              # texture content covers PSP x in [-16,496], y in [-8,280]
canvas=Image.new('RGB',(TX,TY),(88,93,112))
canvas.paste(rs,(round(offX)+MX,round(offY)+MY))
# extend bottom rows (below world bottom) with the last row
arr=np.array(canvas)
last=round(offY)+MY+rs.size[1]
if last<TY: arr[last:,:,:]=arr[last-1:last,:,:]
tex=np.zeros((512,512,4),np.uint8); tex[:TY,:TX,:3]=arr; tex[:TY,:TX,3]=255
open('/tmp/v3/assets/city.rgba','wb').write(tex.tobytes())
Image.fromarray(arr).save('/tmp/v3/city_view.png')
print('city ok', rs.size, offX, offY)

# ---- portraits (14 frames, matte with white-pocket removal) ----
PW=64
for i,(x0,y0,x1,y1) in enumerate(PORT_BOXES):
    pad=3
    cr=crop(x0-pad,y0-pad,x1+pad,y1+pad)
    rgba_=matte(cr,True)
    h_,w_=rgba_.shape[:2]
    th=64; tw=round(w_*th/h_)
    arr=rgba_.astype(np.float32); a=arr[:,:,3:4]/255.0
    pm=np.dstack([arr[:,:,:3]*a,arr[:,:,3:4]]).astype(np.uint8)
    pmim=Image.fromarray(pm,'RGBA').resize((tw,th),Image.LANCZOS)
    q=np.array(pmim).astype(np.float32); qa=q[:,:,3:4]/255.0
    rgb=np.where(qa>0.003,q[:,:,:3]/np.maximum(qa,0.003),0)
    fin=np.dstack([np.clip(rgb,0,255),q[:,:,3:4]]).astype(np.uint8)
    tex=np.zeros((64,64,4),np.uint8); tex[:th,:min(tw,64)]=fin[:,:64]
    open(f'/tmp/v3/assets/portrait_{i:02d}.rgba','wb').write(tex.tobytes())
    if i==0: print('portrait content', tw, th, 'aspect', w_/h_)
print('portraits ok')
