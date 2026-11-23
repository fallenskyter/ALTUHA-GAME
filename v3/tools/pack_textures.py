"""Post-process the generated textures for fast PSP rendering:
   - boots / portraits: swizzle (16-byte x 8-row blocks) so the GE texture cache works
   - city: dithered RGB565, split into 512x256 (top) + 512x64 (bottom), swizzled, loaded into VRAM at runtime"""
import numpy as np, os, sys
A=sys.argv[1] if len(sys.argv)>1 else '/tmp/v3/assets'
def swizzle(arr,bpp):
    h,w=arr.shape[0],arr.shape[1]
    rowbytes=w*bpp
    raw=arr.reshape(h,rowbytes)
    return raw.reshape(h//8,8,rowbytes//16,16).transpose(0,2,1,3).copy().tobytes()
for n in os.listdir(A):
    if (n.startswith('boot_') or n.startswith('portrait_')) and n.endswith('.rgba'):
        p=os.path.join(A,n); d=np.frombuffer(open(p,'rb').read(),np.uint8)
        side=256 if n.startswith('boot_') else 64
        if d.size!=side*side*4: print('skip',n,d.size); continue
        open(p,'wb').write(swizzle(d.reshape(side,side,4),4))
city=os.path.join(A,'city.rgba')
if os.path.exists(city):
    c=np.frombuffer(open(city,'rb').read(),np.uint8).reshape(512,512,4)[:288,:,:3].astype(np.float32)
    bayer=np.array([[0,8,2,10],[12,4,14,6],[3,11,1,9],[15,7,13,5]],np.float32)/16.0-0.5
    ty,tx=np.mgrid[0:288,0:512]
    d=bayer[ty%4,tx%4][:,:,None]
    r=np.clip(np.floor(c[:,:,0]/255*31+0.5+d[:,:,0]*0.9),0,31).astype(np.uint16)
    g=np.clip(np.floor(c[:,:,1]/255*63+0.5+d[:,:,0]*0.9),0,63).astype(np.uint16)
    b=np.clip(np.floor(c[:,:,2]/255*31+0.5+d[:,:,0]*0.9),0,31).astype(np.uint16)
    v=(r|(g<<5)|(b<<11)).astype('<u2')
    top=v[:256]; bot=np.zeros((64,512),'<u2'); bot[:32]=v[256:288]; bot[32:]=v[287:288]
    open(os.path.join(A,'city_top.565'),'wb').write(swizzle(top.view(np.uint8).reshape(256,1024),1))
    open(os.path.join(A,'city_bot.565'),'wb').write(swizzle(bot.view(np.uint8).reshape(64,1024),1))
    os.remove(city)
    print('city packed')
print('done')
