/*
 * DUSK CRUSHER - Gothic Arcade  (PSP port, C, software renderer)
 * Port of DUSK_CRUSHER_Gothic_Arcade_v10.html
 *
 * Build for PSP : make              (needs PSPSDK / pspdev)
 * Host test     : gcc -O2 -DHOST -o host_test src/main.c -lm && ./host_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#ifndef HOST
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <pspge.h>
#include <psppower.h>
PSP_MODULE_INFO("DUSKCRUSHER", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
#ifdef PSP_HEAP_SIZE_KB
PSP_HEAP_SIZE_KB(-2048);
#endif
#endif

typedef uint32_t u32;
typedef int16_t  s16;
typedef uint8_t  u8;

/* ------------------------------------------------------------------ */
/*  Tunables                                                          */
/* ------------------------------------------------------------------ */
#define SW 480
#define SH 272
#define FBW 512                     /* PSP framebuffer stride          */

#define HEART_MAX        5.0f       /* the "happiness" meter = 5 hearts */
#define MUSIC_START_AT   0.85f      /* music starts at 85% (15% left)   */
#define MUSIC_STOP_AT    0.60f      /* music fades out below 60%        */

#define ASPHALT_TOP 220
#define CURB_TOP    259
#define GROUND_FRONT 256.f
#define GROUND_BACK  240.f
#define MAXANT 100
#define MAXPART 420
#define MAXPOP 48
#define MAXSTRAND 24

#define CLAMP(v,a,b) ((v)<(a)?(a):((v)>(b)?(b):(v)))
#define MINF(a,b) ((a)<(b)?(a):(b))
#define MAXF(a,b) ((a)>(b)?(a):(b))
#define RGBA(r,g,b,a) ((u32)(r)|((u32)(g)<<8)|((u32)(b)<<16)|((u32)(a)<<24))
#define RGB(r,g,b) RGBA(r,g,b,255)
#define CR(c) ((int)((c)&255))
#define CG(c) ((int)(((c)>>8)&255))
#define CB(c) ((int)(((c)>>16)&255))
#define CA(c) ((int)(((c)>>24)&255))
#define DIV255(v) ((((v)+128)*257)>>16)

/* ------------------------------------------------------------------ */
/*  RNG                                                               */
/* ------------------------------------------------------------------ */
static u32 rng_s = 0x1234567u;
static inline u32 rnd(void){ rng_s ^= rng_s<<13; rng_s ^= rng_s>>17; rng_s ^= rng_s<<5; return rng_s; }
static inline float frand(float a,float b){ return a + (rnd()&0xFFFFFF)/16777216.0f*(b-a); }
static inline int irand(int a,int b){ return a + (int)(rnd()%(u32)(b-a+1)); }

/* ------------------------------------------------------------------ */
/*  Surfaces / drawing                                                */
/* ------------------------------------------------------------------ */
typedef struct { int w,h; u32 *p; } Img;
typedef struct { u32 *p; int w,h,stride; } Surf;

static Surf *T;                 /* current target */
static int OX, OY;              /* world draw offset (screen shake) */

static Img img_new(int w,int h){ Img i; i.w=w; i.h=h; i.p=(u32*)calloc((size_t)w*h,4); return i; }

static inline void putpx(int x,int y,u32 c,int a){
    x+=OX; y+=OY;
    if((unsigned)x>=(unsigned)T->w || (unsigned)y>=(unsigned)T->h) return;
    u32 *d=&T->p[y*T->stride+x];
    if(a>=255){ *d=c|0xFF000000u; return; }
    if(a<=0) return;
    u32 o=*d; int ia=255-a;
    int r=DIV255(CR(c)*a+CR(o)*ia), g=DIV255(CG(c)*a+CG(o)*ia), b=DIV255(CB(c)*a+CB(o)*ia);
    *d=RGB(r,g,b);
}
static void fillrect(int x,int y,int w,int h,u32 c,int a){
    for(int j=0;j<h;j++) for(int i=0;i<w;i++) putpx(x+i,y+j,c,a);
}
static void strokerect(int x,int y,int w,int h,u32 c,int a){
    for(int i=0;i<w;i++){ putpx(x+i,y,c,a); putpx(x+i,y+h-1,c,a); }
    for(int j=1;j<h-1;j++){ putpx(x,y+j,c,a); putpx(x+w-1,y+j,c,a); }
}
static void fillellipse(float cx,float cy,float rx,float ry,u32 c,int a){
    if(rx<0.5f||ry<0.5f) return;
    for(int y=(int)floorf(cy-ry); y<=(int)ceilf(cy+ry); y++){
        float t=((float)y+0.5f-cy)/ry; if(t<-1||t>1) continue;
        float hw=rx*sqrtf(1-t*t);
        for(int x=(int)floorf(cx-hw); x<=(int)ceilf(cx+hw); x++){
            float dx=((float)x+0.5f-cx);
            if(fabsf(dx)<=hw) putpx(x,y,c,a);
        }
    }
}
static void line(float x0,float y0,float x1,float y1,u32 c,int a){
    float dx=x1-x0, dy=y1-y0; int n=(int)(MAXF(fabsf(dx),fabsf(dy)))+1;
    for(int i=0;i<=n;i++){ float t=(float)i/n; putpx((int)(x0+dx*t),(int)(y0+dy*t),c,a); }
}

/* area-averaged downscale (premultiplied alpha) */
static Img img_scale(const u32 *src,int sw,int sh,int dw,int dh){
    Img o=img_new(dw,dh);
    float fx=(float)sw/dw, fy=(float)sh/dh;
    for(int y=0;y<dh;y++) for(int x=0;x<dw;x++){
        int x0=(int)floorf(x*fx), x1=(int)ceilf((x+1)*fx); if(x1>sw)x1=sw; if(x1<=x0)x1=x0+1;
        int y0=(int)floorf(y*fy), y1=(int)ceilf((y+1)*fy); if(y1>sh)y1=sh; if(y1<=y0)y1=y0+1;
        long ar=0,ag=0,ab=0,aa=0; int n=0;
        for(int j=y0;j<y1;j++) for(int i=x0;i<x1;i++){
            u32 c=src[j*sw+i]; int a=CA(c);
            ar+=CR(c)*a; ag+=CG(c)*a; ab+=CB(c)*a; aa+=a; n++;
        }
        if(aa==0){ o.p[y*dw+x]=0; continue; }
        o.p[y*dw+x]=RGBA((int)(ar/aa),(int)(ag/aa),(int)(ab/aa),(int)(aa/n));
    }
    return o;
}

/* alpha blit; tint 0xFFFFFFFF = none; ga = global alpha */
static void blit(const Img *s,int dx,int dy,int ga,u32 tint){
    for(int y=0;y<s->h;y++){
        int Y=dy+y+OY; if((unsigned)Y>=(unsigned)T->h) continue;
        for(int x=0;x<s->w;x++){
            int X=dx+x+OX; if((unsigned)X>=(unsigned)T->w) continue;
            u32 c=s->p[y*s->w+x]; int a=CA(c); if(!a) continue;
            a=a*ga/255; if(!a) continue;
            int r=CR(c),g=CG(c),b=CB(c);
            if(tint!=0xFFFFFFFFu){ r=r*CR(tint)/255; g=g*CG(tint)/255; b=b*CB(tint)/255; }
            u32 *d=&T->p[Y*T->stride+X];
            if(a>=250){ *d=RGB(r,g,b); continue; }
            u32 o=*d; int ia=255-a;
            *d=RGB(DIV255(r*a+CR(o)*ia),DIV255(g*a+CG(o)*ia),DIV255(b*a+CB(o)*ia));
        }
    }
}

/* ---- 3x5 bitmap font ---- */
static const char *FONT_CH = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-!x:.";
static const char *FONT_BITS[] = {
 "010101111101101","110101110101110","011100100100011","110101101101110","111100110100111","111100110100100",
 "011100101101011","101101111101101","111010010010111","001001001101010","101101110101101","100100100100111",
 "101111111101101","110101101101101","010101101101010","110101110100100","010101101111011","110101110101101",
 "011100010001110","111010010010010","101101101101111","101101101101010","101101111111101","101101010101101",
 "101101010010010","111001010100111",
 "111101101101111","010110010010111","110001010100111","110001010001110","101101111001001","111100110001110",
 "011100111101111","111001010010010","111101111101111","111101111001110",
 "000010111010000","000000111000000","010010010000010","000101010101000","000010000010000","000000000000010"
};
static void draw_text(int x,int y,const char *s,int sc,u32 col,int a,int shadow){
    for(;*s;s++){
        char ch=*s; if(ch>='a'&&ch<='z') ch-=32;
        const char *p=strchr(FONT_CH,ch);
        if(p && ch){
            const char *bits=FONT_BITS[p-FONT_CH];
            for(int j=0;j<5;j++) for(int i=0;i<3;i++) if(bits[j*3+i]=='1'){
                if(shadow) fillrect(x+i*sc+1,y+j*sc+1,sc,sc,RGB(8,5,14),a);
                fillrect(x+i*sc,y+j*sc,sc,sc,col,a);
            }
        }
        x+=4*sc;
    }
}

/* ------------------------------------------------------------------ */
/*  File I/O                                                          */
/* ------------------------------------------------------------------ */
static char BASE[256] = "";
static void *load_file(const char *name,size_t *sz){
    char path[320]; snprintf(path,sizeof path,"%sassets/%s",BASE,name);
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    void *b=malloc((size_t)n+8); if(!b){ fclose(f); return NULL; }
    size_t rd=fread(b,1,(size_t)n,f); fclose(f);
    if(sz) *sz=rd;
    return b;
}

/* ------------------------------------------------------------------ */
/*  Audio mixer (22.05 kHz mono sources -> 44.1 kHz stereo out)        */
/* ------------------------------------------------------------------ */
typedef struct { const s16 *d; int len; float pos; float rate; float vol; float pan; int end; volatile int on; } Voice;
#define NVOICE 12
static Voice voices[NVOICE];
static s16 *music_d; static int music_len;
static volatile float music_pos, music_vol, music_target;
static volatile int music_playing;
static float master_sfx = 1.0f;
static int sfx_muted = 0;
static volatile int music_started_count = 0;       /* for tests */

static int sfx_play(const s16 *d,int len,float vol,float rate,int start,int maxlen){
    if(!d||sfx_muted) return -1;
    int best=-1; 
    for(int i=0;i<NVOICE;i++) if(!voices[i].on){ best=i; break; }
    if(best<0) best=(int)(rnd()%NVOICE);
    Voice *v=&voices[best];
    v->on=0; v->d=d; v->len=len; v->pos=(float)start; v->rate=rate; v->vol=vol*master_sfx;
    v->end = (maxlen>0 && start+maxlen<len) ? start+maxlen : len;
    v->on=1; return best;
}
static void sfx_stop(int h){ if(h>=0 && h<NVOICE) voices[h].on=0; }

static void music_start(void){
    if(!music_d) return;
    music_pos=0; music_vol=0; music_target=1.0f; music_playing=1; music_started_count++;
}
static void music_fadeout(void){ music_target=0; }

static void mix_block(s16 *out,int frames){
    for(int i=0;i<frames;i++){
        float l=0,r=0;
        for(int k=0;k<NVOICE;k++){
            Voice *v=&voices[k]; if(!v->on) continue;
            int ip=(int)v->pos;
            if(ip>=v->end){ v->on=0; continue; }
            float s=v->d[ip]*(1.0f/32768.0f)*v->vol;
            l+=s; r+=s;
            v->pos+=v->rate*0.5f;
        }
        if(music_playing){
            float mv=music_vol;
            if(mv<music_target) mv=MINF(music_target,mv+1.0f/(44100*0.8f));
            else if(mv>music_target) mv=MAXF(music_target,mv-1.0f/(44100*1.5f));
            music_vol=mv;
            if(mv<=0.0001f && music_target==0){ music_playing=0; }
            else {
                int ip=(int)music_pos;
                if(ip>=music_len){ music_pos=0; ip=0; }
                float s=music_d[ip]*(1.0f/32768.0f)*mv*0.85f;
                l+=s; r+=s;
                music_pos+=0.5f;
            }
        }
        l=CLAMP(l,-1.f,1.f); r=CLAMP(r,-1.f,1.f);
        out[i*2]=(s16)(l*32000); out[i*2+1]=(s16)(r*32000);
    }
}

/* ---- sound bank ---- */
static s16 *snd_thud, *snd_crunch, *snd_bone, *snd_creak, *snd_squelch[3], *snd_rumble, *snd_thudsyn, *snd_scream;
static int  n_thud, n_crunch, n_bone, n_creak, n_squelch[3], n_rumble, n_thudsyn, n_scream;

static s16 *synth_alloc(int n){ return (s16*)calloc((size_t)n,2); }
static void gen_sounds(void){
    const float SR=22050.f;
    /* bone break: noise burst + two clicks + low body */
    n_bone=(int)(0.22f*SR); snd_bone=synth_alloc(n_bone);
    for(int i=0;i<n_bone;i++){ float t=i/SR;
        float nz=frand(-1,1)*expf(-t*55.f);
        float click=(i==(int)(0.045f*SR)||i==(int)(0.09f*SR))?0.9f:0;
        float body=sinf(6.2832f*140.f*t)*expf(-t*28.f)*0.6f;
        snd_bone[i]=(s16)(CLAMP(nz*0.8f+click+body,-1.f,1.f)*28000); }
    /* leather creak */
    n_creak=(int)(0.5f*SR); snd_creak=synth_alloc(n_creak);
    { float lp=0, ph=0; for(int i=0;i<n_creak;i++){ float t=i/SR;
        float f=85.f+45.f*sinf(t*19.f)+t*40.f; ph+=f/SR; ph-=floorf(ph);
        float saw=ph*2-1; lp+=(saw-lp)*0.18f;
        float env=sinf(3.1416f*t/0.5f); snd_creak[i]=(s16)(lp*env*0.55f*30000); } }
    /* wet squelch x3 */
    for(int v=0;v<3;v++){
        n_squelch[v]=(int)((0.22f+0.05f*v)*SR); snd_squelch[v]=synth_alloc(n_squelch[v]);
        float lp=0, lp2=0; 
        for(int i=0;i<n_squelch[v];i++){ float t=i/SR;
            float cut=0.06f+0.30f*expf(-t*(9.f+3*v));
            lp+=(frand(-1,1)-lp)*cut; lp2+=(lp-lp2)*0.5f;
            float bub=sinf(6.2832f*(240.f+520.f*t*(2+v))*t)*expf(-t*18.f)*0.35f;
            float env=expf(-t*(7.f-v)); 
            snd_squelch[v][i]=(s16)(CLAMP((lp2*3.2f+bub)*env,-1.f,1.f)*26000); }
    }
    /* smear-start rumble */
    n_rumble=(int)(0.6f*SR); snd_rumble=synth_alloc(n_rumble);
    { float a=0,b=0; for(int i=0;i<n_rumble;i++){ float t=i/SR;
        a+=(frand(-1,1)-a)*0.03f; b+=(a-b)*0.08f;
        float env=sinf(3.1416f*t/0.6f); snd_rumble[i]=(s16)(CLAMP(b*14.f*env,-1.f,1.f)*30000); } }
    /* synth thud (low sine sweep) */
    n_thudsyn=(int)(0.32f*SR); snd_thudsyn=synth_alloc(n_thudsyn);
    { float ph=0; for(int i=0;i<n_thudsyn;i++){ float t=i/SR; float f=38.f+90.f*expf(-t*18.f);
        ph+=f/SR; snd_thudsyn[i]=(s16)(sinf(6.2832f*ph)*expf(-t*9.f)*30000); } }
    /* tiny scream (squeak) */
    n_scream=(int)(0.6f*SR); snd_scream=synth_alloc(n_scream);
    { float ph=0; for(int i=0;i<n_scream;i++){ float t=i/SR; float f=880.f+160.f*sinf(t*38.f)+t*220.f;
        ph+=f/SR; float env=sinf(3.1416f*t/0.6f);
        snd_scream[i]=(s16)(sinf(6.2832f*ph)*env*0.22f*30000); } }
}
static void load_audio(void){
    size_t sz;
    snd_thud=(s16*)load_file("thud.pcm",&sz);   n_thud=snd_thud?(int)(sz/2):0;
    snd_crunch=(s16*)load_file("crunch.pcm",&sz); n_crunch=snd_crunch?(int)(sz/2):0;
    music_d=(s16*)load_file("music.pcm",&sz);   music_len=music_d?(int)(sz/2):0;
    gen_sounds();
}

/* high level SFX */
static void sx_thud(float inten,int full){
    if(snd_thud) sfx_play(snd_thud,n_thud,CLAMP(0.55f+inten*0.45f,0.3f,1.f),full?0.85f:1.0f,0,0);
    sfx_play(snd_thudsyn,n_thudsyn,full?0.95f:0.6f*inten,1.0f,0,0);
}
static void sx_crunch(float power,int full,int count){
    if(!snd_crunch) { sfx_play(snd_bone,n_bone,0.8f,1.f,0,0); return; }
    static const float rates[4]={0.82f,0.95f,1.10f,1.28f};
    int len=(int)((0.30f+MINF(count,6)*0.07f)*22050);
    int start=irand(0,MAXF(1,n_crunch-len-1));
    sfx_play(snd_crunch,n_crunch,CLAMP(0.45f+power*0.35f,0.3f,1.f),rates[irand(0,3)]*(full?0.9f:1.f),start,len);
    if(count>=3 || full) sfx_play(snd_crunch,n_crunch,0.5f,rates[irand(0,3)]*0.8f,irand(0,MAXF(1,n_crunch-len-1)),len);
    sfx_play(snd_bone,n_bone,0.5f,frand(0.8f,1.25f),0,0);
}
static void sx_bone(int n){ sfx_play(snd_bone,n_bone,CLAMP(0.55f+n*0.05f,0.5f,1.f),frand(0.85f,1.2f),0,0); }
static void sx_creak(void){ sfx_play(snd_creak,n_creak,0.35f,frand(0.9f,1.25f),0,0); }
static void sx_squelch(float v){ int k=irand(0,2); sfx_play(snd_squelch[k],n_squelch[k],CLAMP(v,0.1f,0.9f),frand(0.85f,1.2f),0,0); }
static void sx_rumble(void){ sfx_play(snd_rumble,n_rumble,0.55f,1.f,0,0); }
static int  sx_scream(void){ return sfx_play(snd_scream,n_scream,0.25f,frand(0.8f,1.4f),0,0); }

/* ------------------------------------------------------------------ */
/*  Platform: pad / video                                              */
/* ------------------------------------------------------------------ */
typedef struct { float lx,ly; int cross,circle,square,triangle,l,r,start,select; } Pad;

#ifdef HOST
static Pad host_pad;
static void pad_read(Pad *p){ *p=host_pad; }
#else
static void pad_read(Pad *p){
    SceCtrlData d; sceCtrlPeekBufferPositive(&d,1);
    float x=((int)d.Lx-128)/128.f, y=((int)d.Ly-128)/128.f;
    if(fabsf(x)<0.18f) x=0; if(fabsf(y)<0.18f) y=0;
    if(d.Buttons&PSP_CTRL_LEFT) x=-1; if(d.Buttons&PSP_CTRL_RIGHT) x=1;
    if(d.Buttons&PSP_CTRL_UP) y=-1;   if(d.Buttons&PSP_CTRL_DOWN) y=1;
    p->lx=x; p->ly=y;
    p->cross=!!(d.Buttons&PSP_CTRL_CROSS);   p->circle=!!(d.Buttons&PSP_CTRL_CIRCLE);
    p->square=!!(d.Buttons&PSP_CTRL_SQUARE); p->triangle=!!(d.Buttons&PSP_CTRL_TRIANGLE);
    p->l=!!(d.Buttons&PSP_CTRL_LTRIGGER);    p->r=!!(d.Buttons&PSP_CTRL_RTRIGGER);
    p->start=!!(d.Buttons&PSP_CTRL_START);   p->select=!!(d.Buttons&PSP_CTRL_SELECT);
}
#endif

static u32 back_px[SW*SH];
static Surf backS={back_px,SW,SH,SW};
static u32 scene_px[SW*SH];             /* city + persistent goo */
static Surf sceneS={scene_px,SW,SH,SW};
static u32 city_px[SW*SH];              /* clean city (for goo fading, unused) */

/* ------------------------------------------------------------------ */
/*  Assets                                                            */
/* ------------------------------------------------------------------ */
#define NBOOT 11
#define BOOT_SZ 128
static Img boots[NBOOT];
static Img back_boot[2];
static Img portraits[14];
static int  assets_ok=0;

static void load_images(void){
    size_t sz;
    char nm[32];
    assets_ok=1;
    for(int i=0;i<NBOOT;i++){
        snprintf(nm,sizeof nm,"boot_%02d.rgba",i);
        u32 *d=(u32*)load_file(nm,&sz);
        if(!d||sz<256*256*4){ assets_ok=0; boots[i]=img_new(BOOT_SZ,BOOT_SZ); continue; }
        boots[i]=img_scale(d,256,256,BOOT_SZ,BOOT_SZ);
        if(i==0) back_boot[0]=img_scale(d,256,256,112,112);
        if(i==4) back_boot[1]=img_scale(d,256,256,112,112);
        free(d);
    }
    u32 *pd=(u32*)load_file("portraits.rgba",&sz);
    for(int i=0;i<14;i++){
        portraits[i]=img_new(96,100);
        if(pd && sz>=(size_t)(i+1)*96*100*4) memcpy(portraits[i].p,pd+(size_t)i*96*100,96*100*4);
        else assets_ok=0;
    }
    free(pd);
}

/* ------------------------------------------------------------------ */
/*  City background (static layer + animated motes / neon / windows)   */
/* ------------------------------------------------------------------ */
static u32 lerpc(u32 a,u32 b,float t){
    return RGB((int)(CR(a)+(CR(b)-CR(a))*t),(int)(CG(a)+(CG(b)-CG(a))*t),(int)(CB(a)+(CB(b)-CB(a))*t));
}
typedef struct { float x,y,vx,vy,r,ph; } Mote;
static Mote motes[28];
typedef struct { int x,y,w,h; u32 col; float ph; } Win;
static Win wins[64]; static int nwins;

static void px(int x,int y,int w,int h,u32 c){ fillrect(x,y,w,h,c,255); }

static void render_city(void){
    T=&sceneS; OX=OY=0;
    const float K=0.375f;
    /* sky */
    for(int y=0;y<ASPHALT_TOP;y++){
        float t=(float)y/ASPHALT_TOP; u32 c;
        if(t<0.42f) c=lerpc(RGB(0xC8,0x8A,0xB8),RGB(0x8C,0x56,0x82),t/0.42f);
        else if(t<0.82f) c=lerpc(RGB(0x8C,0x56,0x82),RGB(0x55,0x30,0x5B),(t-0.42f)/0.40f);
        else c=lerpc(RGB(0x55,0x30,0x5B),RGB(0x3D,0x21,0x44),(t-0.82f)/0.18f);
        for(int x=0;x<SW;x++) scene_px[y*SW+x]=c;
    }
    /* far skyline */
    static const int far_[12][4]={{-10,120,96,0},{82,185,88,10},{168,92,112,0},{278,205,94,-8},{370,148,125,0},{492,84,104,0},
        {594,174,115,12},{706,118,132,0},{836,188,98,0},{932,98,138,-10},{1068,152,116,0},{1182,112,110,0}};
    for(int i=0;i<12;i++){
        int bx=(int)(far_[i][0]*K), by=(int)(far_[i][1]*K), bw=(int)(far_[i][2]*K), sl=(int)(far_[i][3]*K);
        for(int x=0;x<bw;x++){
            float t=(float)x/bw; int top=by+(int)(MAXF(0,sl)*(1-t)+MAXF(0,-sl)*t);
            px(bx+x,top,1,ASPHALT_TOP-top,RGB(0x4A,0x28,0x4E));
        }
        px(bx+7,by-4,8,4,RGB(0x4A,0x28,0x4E)); px(bx+bw-8,by-12,1,12,RGB(0x4A,0x28,0x4E));
    }
    /* mist */
    for(int y=68;y<ASPHALT_TOP;y++){ int a=(int)(18+30*(float)(y-68)/(ASPHALT_TOP-68));
        fillrect(0,y,SW,1,RGB(0x4C,0x2B,0x52),a); }
    /* mid buildings */
    static const int mid[7][3]={{0,195,165},{165,275,215},{380,230,190},{570,190,225},{795,255,180},{975,175,165},{1140,215,145}};
    static const u32 mcol[7]={RGB(0x48,0x27,0x4F),RGB(0x5A,0x32,0x5F),RGB(0x44,0x24,0x4A),RGB(0x56,0x2F,0x5B),RGB(0x41,0x21,0x47),RGB(0x4E,0x2A,0x54),RGB(0x3B,0x1E,0x41)};
    static const u32 ecol[7]={RGB(0x62,0x37,0x68),RGB(0x74,0x43,0x77),RGB(0x5E,0x34,0x64),RGB(0x6E,0x40,0x74),RGB(0x5B,0x31,0x61),RGB(0x67,0x3A,0x6D),RGB(0x52,0x2C,0x59)};
    nwins=0;
    for(int i=0;i<7;i++){
        int bx=(int)(mid[i][0]*K), by=(int)(mid[i][1]*K), bw=(int)(mid[i][2]*K)+1;
        px(bx,by,bw,ASPHALT_TOP-by,mcol[i]); px(bx,by,bw,2,ecol[i]);
        px(bx+bw-2,by,2,ASPHALT_TOP-by,RGB(0x2C,0x15,0x32));
        px(bx+4,by-5,10,5,RGB(0x35,0x1B,0x3B)); px(bx+18,by-3,7,3,RGB(0x2D,0x16,0x33));
        for(int fy=by+10; fy<ASPHALT_TOP-27; fy+=18){
            fillrect(bx+3,fy+9,bw-6,1,RGB(0x1A,0x0C,0x20),115);
            if(i%3==0) px(bx+6,fy,bw-12,5,RGB(0x2D,0x17,0x34)); else px(bx+5,fy+6,bw-10,3,RGB(0x2A,0x15,0x30));
            int h=(bx*7+fy*13+i*5);
            if(h%5==0 && nwins<60){ Win *w=&wins[nwins++]; w->x=bx+8+(h%7); w->y=fy+1; w->w=10; w->h=5;
                w->col=(h%2)?RGB(0xA8,0x6D,0x9E):RGB(0x72,0xB8,0xB8); w->ph=(float)(h%17); }
            else if(h%7==0 && nwins<60){ Win *w=&wins[nwins++]; w->x=bx+bw-24; w->y=fy+1; w->w=8; w->h=6;
                w->col=RGB(0xC6,0x88,0xA8); w->ph=(float)(h%11); }
        }
        px(bx+3,ASPHALT_TOP-21,bw-6,21,RGB(0x2D,0x18,0x34)); px(bx+6,ASPHALT_TOP-17,bw-12,13,RGB(0x1F,0x11,0x26));
    }
    /* poles + cables */
    static const int poles[4]={58,396,848,1226};
    for(int i=0;i<4;i++){ int p=(int)(poles[i]*K);
        px(p,81,3,ASPHALT_TOP-81,RGB(0x24,0x14,0x2B)); px(p-7,87,16,2,RGB(0x24,0x14,0x2B)); px(p-4,94,12,1,RGB(0x24,0x14,0x2B)); }
    for(int i=0;i<3;i++){ float x1=poles[i]*K+1, x2=poles[i+1]*K+1;
        for(int w=0;w<3;w++){ float yy=87+w*3.4f;
            float px_=x1,py_=yy;
            for(int s=1;s<=24;s++){ float t=s/24.f; float nx=x1+(x2-x1)*t, ny=yy+(4*t*(1-t))*(10+w*5);
                line(px_,py_,nx,ny,RGB(0x1C,0x0E,0x22),210); px_=nx; py_=ny; } } }
    /* asphalt */
    for(int y=ASPHALT_TOP;y<CURB_TOP;y++){
        float t=(float)(y-ASPHALT_TOP)/(CURB_TOP-ASPHALT_TOP);
        u32 c=lerpc(RGB(0x3A,0x2F,0x42),RGB(0x2A,0x20,0x32),t);
        for(int x=0;x<SW;x++){ int n=(int)(rnd()&7); scene_px[y*SW+x]=RGB(CR(c)+n-3,CG(c)+n-3,CB(c)+n-3); }
    }
    px(0,ASPHALT_TOP-1,SW,1,RGB(0x1A,0x0F,0x20));
    for(int x=6;x<SW;x+=46) px(x,238,24,1,RGB(0x6C,0x5A,0x70));
    /* curb */
    px(0,CURB_TOP,SW,SH-CURB_TOP,RGB(0x74,0x66,0x7A)); px(0,CURB_TOP,SW,2,RGB(0x9A,0x8A,0x9E)); px(0,CURB_TOP+2,SW,1,RGB(0x2A,0x20,0x32));
    for(int x=0;x<SW;x+=60) px(x,CURB_TOP+6,1,SH-CURB_TOP-6,RGB(0x58,0x4C,0x5E));
    /* neon sign (static body) */
    px(6,77,11,56,RGB(0x23,0x11,0x2B)); px(7,78,9,54,RGB(0xFF,0x5E,0xA0)); px(8,79,7,52,RGB(0xFF,0xE1,0x68));
    for(int sy=83;sy<128;sy+=8){ px(9,sy,4,4,RGB(0xD8,0x27,0x78)); px(10,sy+1,3,2,RGB(0xFF,0xF0,0x9E)); }
    memcpy(city_px,scene_px,sizeof city_px);
    for(int i=0;i<28;i++){ motes[i].x=frand(0,SW); motes[i].y=frand(15,ASPHALT_TOP-8); motes[i].vx=frand(-5,-1.5f);
        motes[i].vy=frand(-2,2); motes[i].r=frand(0.5f,1.1f); motes[i].ph=frand(0,6.28f); }
}
static void city_update(float dt){
    for(int i=0;i<28;i++){ Mote *m=&motes[i]; m->x+=m->vx*dt; m->y+=m->vy*dt+sinf(m->ph)*0.1f; m->ph+=dt*1.3f;
        if(m->x<-4){ m->x=SW+4; }
        if(m->y<10){ m->y=ASPHALT_TOP-10; }
        if(m->y>ASPHALT_TOP-6){ m->y=14; } }
}
static void city_draw_anim(float time){
    for(int i=0;i<nwins;i++){ Win *w=&wins[i];
        float f=0.5f+0.5f*sinf(time*(0.7f+(i%5)*0.25f)+w->ph);
        if(((int)(time*0.5f+w->ph))%9==0) f*=0.35f;       /* occasional flicker-off */
        fillrect(w->x,w->y,w->w,w->h,w->col,(int)(70+150*f)); }
    float neon=0.75f+0.25f*sinf(time*6.f)+((rnd()&63)==0?-0.5f:0);
    fillrect(5,76,13,58,RGB(0xFF,0x5E,0xA0),(int)(CLAMP(neon,0.f,1.f)*40));
    for(int i=0;i<28;i++){ Mote *m=&motes[i];
        int a=(int)(60+60*sinf(time*2+m->ph)); fillrect((int)m->x,(int)m->y,m->r>0.8f?2:1,m->r>0.8f?2:1,RGB(0xE8,0xB8,0xE0),CLAMP(a,10,140)); }
}

/* ------------------------------------------------------------------ */
/*  Goo / particles / shockwaves                                       */
/* ------------------------------------------------------------------ */
typedef struct { float x,y,z,vx,vy,vz,life; int size; u32 col; int on; } Part;
static Part parts[MAXPART];
typedef struct { float x1,x2,gy,life,maxl,sag; int on; } Strand;
static Strand strands[MAXSTRAND];
typedef struct { float x,y,r,maxr,life; int on; } Wave;
static Wave waves[6];

static const u32 GOO_DARK=RGB(0x2A,0x08,0x12), GOO_MID=RGB(0x55,0x0E,0x22), GOO_WET=RGB(0x8E,0x1D,0x38);

static void stamp_puddle(float x,float y,float rx,float ry,int a){
    Surf *old=T; int ox=OX, oy=OY; T=&sceneS; OX=OY=0;
    fillellipse(x,y,rx,ry,GOO_DARK,a);
    fillellipse(x-rx*0.15f,y-ry*0.2f,rx*0.6f,ry*0.5f,GOO_MID,a/3);
    T=old; OX=ox; OY=oy;
}
static void stamp_smear(float x,float y,float vx,float wid){
    Surf *old=T; int ox=OX, oy=OY; T=&sceneS; OX=OY=0;
    float len=CLAMP(fabsf(vx)*0.10f,2.f,26.f), dir=vx<0?-1.f:1.f;
    for(int i=0;i<(int)len;i++){
        float t=(float)i/len; float xx=x-dir*i*1.0f;
        fillellipse(xx,y,MAXF(1.2f,wid*0.5f*(1-t*0.55f)),MAXF(1.f,wid*0.18f*(1-t*0.4f)),GOO_DARK,150);
    }
    T=old; OX=ox; OY=oy;
}
static void spawn_splash(float x,float y,int n,float power,float footvx){
    for(int i=0;i<n;i++){
        Part *p=NULL; for(int k=0;k<MAXPART;k++) if(!parts[k].on){ p=&parts[k]; break; }
        if(!p) return;
        float ang=frand(0,6.2832f), sp=frand(15,85)*power;
        p->on=1; p->x=x; p->y=y; p->z=frand(2,8); p->vx=cosf(ang)*sp+footvx*0.12f; p->vy=sinf(ang)*sp*0.28f;
        p->vz=frand(40,150)*power; p->life=1.4f; p->size=irand(1,3); p->col=(rnd()&3)?GOO_WET:GOO_MID;
    }
}
static void add_strand(float x1,float x2,float gy){
    for(int i=0;i<MAXSTRAND;i++) if(!strands[i].on){ strands[i]=(Strand){x1,x2,gy,0.35f,0.35f,frand(4,10),1}; return; }
}
static void add_wave(float x,float y,float r){
    for(int i=0;i<6;i++) if(!waves[i].on){ waves[i]=(Wave){x,y,4,r,0.28f,1}; return; }
}
static void goo_update(float dt){
    for(int i=0;i<MAXPART;i++){ Part *p=&parts[i]; if(!p->on) continue;
        p->vz-=420*dt; p->x+=p->vx*dt; p->y+=p->vy*dt; p->z+=p->vz*dt; p->life-=dt;
        if(p->z<=0||p->life<=0){ 
            if(p->y>ASPHALT_TOP-2 && p->y<SH-1 && p->x>0 && p->x<SW)
                stamp_puddle(p->x,p->y,(float)p->size*frand(0.8f,1.6f),(float)p->size*0.55f,200);
            p->on=0; } }
    for(int i=0;i<MAXSTRAND;i++){ if(strands[i].on){ strands[i].life-=dt; if(strands[i].life<=0) strands[i].on=0; } }
    for(int i=0;i<6;i++){ Wave *w=&waves[i]; if(!w->on) continue; w->life-=dt; w->r+=(w->maxr-w->r)*MINF(1,dt*10); if(w->life<=0) w->on=0; }
}
static void goo_draw_back(void){   /* shockwaves */
    for(int i=0;i<6;i++){ Wave *w=&waves[i]; if(!w->on) continue;
        int a=(int)(120*w->life/0.28f);
        for(int s=0;s<48;s++){ float t=s/48.f*6.2832f; putpx((int)(w->x+cosf(t)*w->r),(int)(w->y+sinf(t)*w->r*0.22f),RGB(0xE6,0xB0,0xD8),a); } }
}
static void goo_draw_front(void){
    for(int i=0;i<MAXPART;i++){ Part *p=&parts[i]; if(!p->on) continue;
        fillrect((int)p->x,(int)(p->y-p->z),p->size,p->size,p->col,230); }
}
static void strands_draw(void){
    for(int i=0;i<MAXSTRAND;i++){ Strand *s=&strands[i]; if(!s->on) continue;
        float k=s->life/s->maxl; float tx=(s->x1+s->x2)*0.5f;
        int a=(int)(220*k);
        float h=(1-k)*s->sag;
        line(s->x1,s->gy-14*k,tx,s->gy-14*k+h,GOO_MID,a);
        line(tx,s->gy-14*k+h,s->x2,s->gy,GOO_DARK,a); }
}

/* ------------------------------------------------------------------ */
/*  Foot                                                               */
/* ------------------------------------------------------------------ */
typedef struct {
    float x,y,vx,vy,tx,ty,gy;
    int grounded,wasGrounded,slamming,fp;
    float tilt,angle,angVel,dry,goo,recoil,anim;
} Foot;
static Foot foot, bfoot;
static float support_rise=0;

static void foot_init(Foot *f,float x,float y,float gy){
    memset(f,0,sizeof *f); f->x=f->tx=x; f->y=f->ty=y; f->gy=gy; f->dry=0.3f;
}
static void foot_update(Foot *f,float dt){
    f->wasGrounded=f->grounded;
    float ox=f->x, oy=f->y;
    f->x+=(f->tx-f->x)*MINF(1.f,13.f*dt);
    if(f->slamming){
        f->vy+=3000.f*dt; if(f->vy>950.f) f->vy=950.f;
        f->y+=f->vy*dt;
        if(f->y>=f->gy){ f->y=f->gy; f->slamming=0; }
    } else {
        float dy=f->ty-f->y;
        float rate=(dy>0)?5.f:11.f;
        float mv=dy*MINF(1.f,rate*dt);
        if(dy>0 && mv>230.f*dt) mv=230.f*dt;
        f->y+=mv; if(f->y>f->gy) f->y=f->gy;
        f->vy=(f->y-oy)/MAXF(dt,0.001f);
    }
    f->grounded=(f->y>=f->gy-0.6f);
    float nvx=(f->x-ox)/MAXF(dt,0.001f);
    f->vx+=(nvx-f->vx)*MINF(1.f,18.f*dt);
    float tgt=f->tilt+CLAMP(f->vx/700.f,-0.3f,0.3f)*0.2f;
    float na=f->angle+(tgt-f->angle)*MINF(1.f,10.f*dt);
    f->angVel=(na-f->angle)/MAXF(dt,0.001f); f->angle=na;
    if(f->recoil>0) f->recoil=MAXF(0,f->recoil-dt*5.f);
    if(f->grounded && fabsf(f->vx)>14) f->dry=MINF(1.f,f->dry+dt*0.03f);
    f->anim+=dt*MINF(14.f,fabsf(f->vx)*0.06f);
}
static void foot_sole(const Foot *f,float *toe,float *heel){
    float hw=36.f*(1-fabsf(f->angle)*0.3f);
    *toe=f->x+hw+f->angle*10; *heel=f->x-hw+f->angle*10;
}
static void foot_add_goo(Foot *f,float amt){ f->goo=MINF(1.f,f->goo+amt*3.f); f->dry=MAXF(0.f,f->dry-amt*8.f); }

static int foot_pose(const Foot *f){
    if(f->fp) return 8;
    if(f->slamming) return 3;
    if(!f->grounded) return (f->vy<-40)?4:(f->vy>60?3:1);
    if(fabsf(f->angVel)>0.32f || fabsf(f->vx)>14){
        int k=((int)f->anim)%3; return k==0?0:(k==1?1:2); }
    return 0;
}
static void foot_draw(const Foot *f,int pose,float shadowK){
    /* shadow */
    float air=MAXF(0,f->gy-f->y);
    fillellipse(f->x,f->gy+2,36-air*0.06f,6,RGB(0,0,0),(int)(110*shadowK*CLAMP(1-air/160.f,0.2f,1.f)));
    const Img *im=&boots[pose];
    int bw=BOOT_SZ; float yo=f->recoil*3.f;
    int dx=(int)(f->x-0.41f*bw), dy=(int)(f->y+yo-bw+2);
    blit(im,dx,dy,255,0xFFFFFFFFu);
    /* extend the leg upward to the screen top (repeat top row) */
    if(dy>-OY){
        for(int x=0;x<bw;x++){ u32 c=im->p[x]; if(CA(c)>200)
            for(int y=-OY; y<dy; y++) putpx(dx+x,y,c,255); }
    }
    /* goo on the sole */
    if(f->goo>0.05f){ int a=(int)(f->goo*180); fillrect((int)(f->x-30),(int)(f->y+yo-1),60,2,GOO_DARK,a); }
}

/* ------------------------------------------------------------------ */
/*  Ants / popups / HUD                                                */
/* ------------------------------------------------------------------ */
typedef struct { float x,y,vx,base,w,h,phase,crush,scream_t; int alive,pinned,screaming,voice; u32 coat,skin,pants; } Ant;
static Ant ants[MAXANT];
typedef struct { float x,y,vy,life; int val,on; } Popup;
static Popup pops[MAXPOP];

static const u32 COATS[6]={RGB(0x6A,0x3A,0x5E),RGB(0x3A,0x4E,0x6E),RGB(0x5E,0x5E,0x3A),RGB(0x7A,0x2E,0x3A),RGB(0x3A,0x6A,0x5E),RGB(0x66,0x66,0x70)};
static const u32 SKINS[3]={RGB(0xE0,0xB8,0x98),RGB(0xC8,0x98,0x78),RGB(0x90,0x66,0x4E)};

static void ant_spawn(float xo,int hasx){
    Ant *a=NULL; for(int i=0;i<MAXANT;i++) if(!ants[i].alive){ a=&ants[i]; break; }
    if(!a) return;
    int fromLeft=(rnd()&1);
    float x=hasx?xo:(fromLeft?-8.f:SW+8.f);
    int dir=hasx?((rnd()&1)?1:-1):(fromLeft?1:-1);
    float spd=frand(33,54);
    memset(a,0,sizeof *a);
    a->x=x; a->y=frand(ASPHALT_TOP+6,CURB_TOP-3); a->vx=dir*spd; a->base=spd;
    a->h=frand(25,32); a->w=frand(13,17); a->phase=frand(0,6.28f); a->crush=1; a->alive=1; a->voice=-1;
    a->scream_t=frand(0.6f,3.2f);
    a->coat=COATS[irand(0,5)]; a->skin=SKINS[irand(0,2)]; a->pants=RGB(0x25,0x1C,0x2E);
}
static void popup_add(float x,float y,int v){
    for(int i=0;i<MAXPOP;i++) if(!pops[i].on){ pops[i]=(Popup){x+frand(-4,4),y-4,-26-frand(0,10),0.95f,v,1}; return; }
}
static void draw_ant(const Ant *a,float time){
    int dir=a->vx>=0?1:-1;
    float sq=a->pinned?(0.25f+0.75f*CLAMP(a->crush,0.f,1.f)):1.f;
    int h=(int)(a->h*sq), w=(int)(a->w*(a->pinned?1.35f:1.f)), x=(int)a->x, y=(int)a->y;
    if(a->pinned){ fillellipse(x,y,w*0.7f,3,GOO_DARK,150); }
    int step=a->pinned?0:(int)(sinf(time*9+a->phase)*2.2f);
    int legh=MAXF(2,(int)(h*0.38f));
    /* legs */
    fillrect(x-w/4-1,y-legh+ (step>0?0:-1),3,legh,a->pants,255);
    fillrect(x+w/4-1,y-legh+ (step>0?-1:0),3,legh,a->pants,255);
    /* torso */
    int th=MAXF(2,(int)(h*0.42f));
    fillrect(x-w/2,y-legh-th,w,th,a->coat,255);
    fillrect(x-w/2,y-legh-th,w,1,RGB(255,255,255),40);
    /* arms */
    if(!a->pinned){ int sw=(int)(sinf(time*9+a->phase+1.57f)*2); fillrect(x-w/2-1,y-legh-th+1+sw,2,th-1,a->coat,255); fillrect(x+w/2-1,y-legh-th+1-sw,2,th-1,a->coat,255); }
    /* head */
    int hs=MAXF(3,(int)(h*0.22f));
    fillrect(x-hs/2,y-legh-th-hs,hs,hs,a->skin,255);
    fillrect(x-hs/2+(dir>0?hs/2:0),y-legh-th-hs,hs/2+1,2,RGB(0x2A,0x1C,0x20),255);
    if(a->screaming) fillrect(x-1+dir,y-legh-th-hs/2,2,2,RGB(0x30,0x0A,0x10),255);
}

typedef struct { const char *label; int count; float flash; int on; } Bonus;
static Bonus bonuses[10]; static int nbon=0;

typedef struct {
    float elapsed; int totalKills, comboKills; float comboTimer; int highest;
    float seriesScore, dispScore, seriesFade;
    float crushAcc, hype;
    float feedAlpha, feedHold;
} HUD;
static HUD hud;

static void hud_set_bonus(const char *label,int v,int additive){
    for(int i=0;i<nbon;i++) if(strcmp(bonuses[i].label,label)==0){ bonuses[i].count=additive?bonuses[i].count+v:v; bonuses[i].flash=1; goto done; }
    if(nbon<10){ bonuses[nbon]=(Bonus){label,v,1,1}; nbon++; }
done: hud.feedAlpha=1; hud.feedHold=2.6f;
}
static int hud_mult(void){ int c=hud.comboKills; return c>=420?4:c>=260?3:c>=140?2:c>=55?1:0; }

typedef struct { int count; int mode; int isDry,isSmearing,screamsCut,diedAlone,wipeout,trash,pinnedNow,grindingNow; } Result; /* mode 0=SOFT 1=FULL */

static void hud_register(const Result *r){
    if(r->trash>0){ hud_set_bonus("TRASH TAKEN OUT",r->trash,1); hud.seriesScore+=r->trash*12; hud.seriesFade=2.8f; }
    if(r->count<=0) return;
    hud.comboKills+=r->count; hud.comboTimer=2.75f;
    float gain=r->count*(r->isSmearing?0.014f:0.010f)+(r->wipeout?0.06f:0);
    hud.hype=MINF(HEART_MAX,hud.hype+gain);
    hud.feedHold=2.6f; hud.feedAlpha=1; hud.seriesFade=3.0f;
    int mult=hud_mult(); int base=(r->mode==1)?11:7;
    hud.seriesScore+=(int)(r->count*(base+mult*2.2f)+(r->wipeout?45:0)+r->screamsCut*8);
    if(r->wipeout) hud_set_bonus("WIPEOUTS",1,1);
    if(r->screamsCut>0) hud_set_bonus("SCREAMS CUT SHORT",r->screamsCut,1);
    if(r->mode==1) hud_set_bonus("FULL PRESSURE KILLS",r->count,1); else hud_set_bonus("SOFT KILLS",r->count,1);
    if(r->isDry) hud_set_bonus("DRY KILLS",r->count,1);
    if(r->diedAlone>0) hud_set_bonus("DIED ALONE",r->diedAlone,1);
    if(hud.comboKills>hud.highest && hud.comboKills>=5){ hud.highest=hud.comboKills; hud_set_bonus("HIGHEST COMBO",hud.comboKills,0); }
    else if(hud.comboKills>=5) for(int i=0;i<nbon;i++) if(!strcmp(bonuses[i].label,"HIGHEST COMBO")){ bonuses[i].count=hud.comboKills; bonuses[i].flash=1; }
}
static void hud_crush_time(float dt,int smearing){
    if(smearing) hud.hype=MINF(HEART_MAX,hud.hype+dt*0.048f);
    hud.crushAcc+=dt; if(hud.crushAcc>=1.f){ int s=(int)hud.crushAcc; hud.crushAcc-=s; hud_set_bonus("SECONDS CRUSHING",s,1); }
}
static void hud_update(float dt){
    hud.elapsed+=dt;
    if(hud.comboKills>0){ hud.comboTimer-=dt; if(hud.comboTimer<=0){ hud.totalKills+=hud.comboKills; hud.comboKills=0; hud.comboTimer=0; } }
    if(hud.dispScore<hud.seriesScore){ float d=hud.seriesScore-hud.dispScore; hud.dispScore+=MAXF(1.f,ceilf(d*MINF(1.f,dt*16))); if(hud.dispScore>hud.seriesScore) hud.dispScore=hud.seriesScore; }
    if(hud.comboKills==0 && hud.hype>0) hud.hype=MAXF(0,hud.hype-dt*0.055f);
    if(hud.comboKills==0 && hud.seriesFade>0){ hud.seriesFade-=dt; if(hud.seriesFade<=0){ hud.seriesScore=0; hud.dispScore=0; } }
    for(int i=0;i<nbon;i++) bonuses[i].flash=MAXF(0,bonuses[i].flash-dt*4.5f);
    if(hud.comboKills==0){
        if(hud.feedHold>0) hud.feedHold-=dt;
        else if(hud.feedAlpha>0){ hud.feedAlpha=MAXF(0,hud.feedAlpha-dt*1.1f); if(hud.feedAlpha==0) nbon=0; }
    }
}

/* ------------------------------------------------------------------ */
/*  Portrait (14 frames) + rapture mode                                */
/* ------------------------------------------------------------------ */
typedef struct {
    float x,y,w,h, happyTimer,hitBounce,frameTimer,blush,joy,rapture;
    float heartT; 
} Portrait;
static Portrait portrait;
typedef struct { float x,y,vy,life,sway; int on; } FloatHeart;
static FloatHeart fh[10];

static void portrait_init(void){
    memset(&portrait,0,sizeof portrait);
    portrait.x=364; portrait.y=4; portrait.w=112; portrait.h=96;
}
static void portrait_trigger(float s){
    portrait.happyTimer=MAXF(portrait.happyTimer,1.45f); portrait.hitBounce=MINF(1.f,portrait.hitBounce+0.55f*s);
}
static void portrait_update(float dt,int comboActive,float hype,int smearing,int dots,int ecstasy){
    Portrait *p=&portrait;
    p->frameTimer+=dt;
    if(p->happyTimer>0) p->happyTimer=MAXF(0,p->happyTimer-dt);
    p->hitBounce=MAXF(0,p->hitBounce-dt*5.5f);
    if(comboActive) p->happyTimer=MAXF(p->happyTimer,0.35f);
    float dn=CLAMP(dots/4.f,0.f,1.f), hn=CLAMP(hype/HEART_MAX,0.f,1.f);
    float mood=CLAMP(MAXF(dn*0.85f,hn*0.90f)+dn*0.20f,0.f,1.f);
    float tb=smearing?CLAMP(0.18f+mood*0.82f,0.f,1.f):(p->happyTimer>0?mood*0.55f:mood*0.22f);
    p->blush+=(tb-p->blush)*MINF(1.f,(smearing?2.4f:1.8f)*dt);
    float tj=smearing?CLAMP(0.25f+mood*0.75f,0.f,1.f):(p->happyTimer>0?CLAMP(0.40f+mood*0.50f,0.f,1.f):0.f);
    p->joy+=(tj-p->joy)*MINF(1.f,2.8f*dt);
    /* rapture: she is really enjoying it (meter >= 85%) */
    float tr=ecstasy?1.f:0.f;
    p->rapture+=(tr-p->rapture)*MINF(1.f,(ecstasy?1.6f:0.9f)*dt);
    if(ecstasy){
        p->blush=MAXF(p->blush,0.55f+0.45f*p->rapture);
        p->heartT-=dt;
        if(p->heartT<=0){ p->heartT=frand(0.18f,0.4f);
            for(int i=0;i<10;i++) if(!fh[i].on){ fh[i]=(FloatHeart){p->x+frand(14,p->w-14),p->y+p->h-14,-frand(16,30),1.4f,frand(0,6.28f),1}; break; } }
    }
    for(int i=0;i<10;i++) if(fh[i].on){ fh[i].life-=dt; fh[i].y+=fh[i].vy*dt; fh[i].sway+=dt*4; if(fh[i].life<=0) fh[i].on=0; }
}
static const char *HEART9[8]={"..XX.XX..",".XXXXXXX.","XXXXXXXXX","XXXXXXXXX",".XXXXXXX.","..XXXXX..","...XXX...","....X...."};
static void draw_heart_small(int x,int y,u32 c,int a){
    for(int j=0;j<8;j++) for(int i=0;i<9;i++) if(HEART9[j][i]=='X') putpx(x+i,y+j,c,a);
}
static void portrait_draw(float time,int combo){
    Portrait *p=&portrait;
    int x=(int)p->x,y=(int)p->y,w=(int)p->w,h=(int)p->h;
    int fever=(combo>=30||p->joy>0.72f||p->rapture>0.3f);
    int happy=(p->happyTimer>0||combo>=3||p->joy>0.32f);
    fillrect(x,y,w,h,RGB(0x1A,0x16,0x1F),255);
    /* radial-ish background */
    for(int j=5;j<h-5;j++) for(int i=5;i<w-5;i++){
        float dx=(i-w*0.5f)/(w*0.72f), dy=(j-h*0.55f)/(w*0.72f); float d=sqrtf(dx*dx+dy*dy);
        u32 c0=fever?RGB(0x3C,0x20,0x3F):RGB(0x2C,0x23,0x34); u32 c1=RGB(0x14,0x10,0x19);
        putpx(x+i,y+j,lerpc(c0,c1,CLAMP(d,0.f,1.f)),255);
    }
    if(p->rapture>0.05f){      /* pink glow pulse */
        float pul=0.5f+0.5f*sinf(time*7.f);
        fillrect(x+5,y+5,w-10,h-10,RGB(255,70,140),(int)(p->rapture*(18+30*pul)));
    }
    static const int idleF[7]={0,2,4,6,8,10,12}, happyF[7]={1,3,5,7,9,11,13}, rapF[4]={9,11,13,7};
    int fi;
    if(p->rapture>0.45f) fi=rapF[(int)(p->frameTimer*(4.0f+3.0f*p->rapture))%4];
    else if(happy) fi=happyF[(int)(p->frameTimer*(fever?5.2f:3.6f))%7];
    else fi=idleF[(int)(p->frameTimer*1.4f)%7];
    const Img *im=&portraits[fi];
    float breath=sinf(time*2.4f)*1.5f;
    float bobAmp=happy?(fever?5.f:2.5f):0.f; if(p->rapture>0.3f) bobAmp=3.5f+3.5f*p->rapture;
    float bob=happy||p->rapture>0.3f? fabsf(sinf(time*(p->rapture>0.3f?13.f:(fever?11.f:6.5f))))*bobAmp:0.f;
    float swayX=(happy||p->rapture>0.3f)?sinf(time*(p->rapture>0.3f?12.f:(fever?10.f:5.5f)))*(p->rapture>0.3f?3.f:1.5f):0.f;
    int dx=x+(w-im->w)/2+(int)swayX;
    int dy=y+3+(int)(breath-bob-p->hitBounce*4.f);
    /* clipped draw with blush */
    float b=CLAMP(p->blush,0.f,1.f);
    float cheekY=dy+im->h*0.57f, cx0=dx+im->w*0.5f, sp=im->w*0.17f, rr=im->w*(0.12f+b*0.04f);
    for(int j=0;j<im->h;j++){
        int Y=dy+j; if(Y<y+5||Y>=y+h-5) continue;
        for(int i=0;i<im->w;i++){
            int X=dx+i; if(X<x+5||X>=x+w-5) continue;
            u32 c=im->p[j*im->w+i]; if(CA(c)<128) continue;
            if(b>0.03f){
                int lum=(CR(c)+CG(c)+CB(c))/3;
                if(lum>105){
                    for(int s=-1;s<=1;s+=2){
                        float ex=(X-(cx0+s*sp))/(rr*1.15f), ey=(Y-cheekY)/(rr*0.72f); float d=ex*ex+ey*ey;
                        if(d<1.f){ float k=(1.f-d)*(0.20f+b*0.60f);
                            c=RGB((int)(CR(c)+(255-CR(c))*k),(int)(CG(c)*(1-k)+60*k),(int)(CB(c)*(1-k)+110*k)); }
                    }
                }
            }
            putpx(X,Y,c,255);
        }
    }
    if(b>0.38f){ for(int s=-1;s<=1;s+=2) for(int k=-1;k<=1;k++){ int lx=(int)(cx0+s*sp+k*3); int ly=(int)cheekY; line(lx-1,ly+2,lx+1,ly-2,RGB(255,48,96),(int)((b-0.38f)*170)); } }
    /* floating hearts */
    for(int i=0;i<10;i++) if(fh[i].on){ int a=(int)(255*CLAMP(fh[i].life/0.5f,0.f,1.f));
        draw_heart_small((int)(fh[i].x+sinf(fh[i].sway)*3),(int)fh[i].y,RGB(255,90,160),a); }
    /* frame */
    u32 fc=fever?RGB(0xE6,0x7E,0xC0):RGB(0x96,0x82,0x9B);
    if(p->rapture>0.3f && ((int)(time*6))%2==0) fc=RGB(255,170,220);
    strokerect(x,y,w,h,fc,255); strokerect(x+1,y+1,w-2,h-2,fc,255);
    strokerect(x+4,y+4,w-8,h-8,RGB(0x4E,0x40,0x58),255);
}

static void hud_draw(float time){
    OX=OY=0;
    Portrait *p=&portrait;
    /* hearts + dots under the portrait */
    int gx=(int)p->x+2, gy=(int)(p->y+p->h)+6;
    for(int i=0;i<5;i++){
        float fill=CLAMP(hud.hype-i,0.f,1.f);
        draw_heart_small(gx+i*11,gy,RGB(0x62,0x4C,0x70),255);
        if(fill>0.02f){
            int top=(int)(8*(1-fill));
            for(int j=top;j<8;j++) for(int k=0;k<9;k++) if(HEART9[j][k]=='X')
                putpx(gx+i*11+k,gy+j,fill>=0.999f?RGB(255,110,170):RGB(0xFF,0x4F,0x94),255);
            if(fill>=0.999f) putpx(gx+i*11+2,gy+2,RGB(255,255,255),255);
        }
    }
    int mult=hud_mult();
    for(int i=0;i<4;i++){
        int cx=gx+60+i*8+4, cy=gy+4;
        fillellipse((float)cx,(float)cy,3.2f,3.2f,i<mult?RGB(0xFF,0x6B,0xA9):RGB(0x3A,0x2A,0x44),255);
        if(i<mult) putpx(cx-1,cy-1,RGB(255,255,255),255);
    }
    /* rating box "000" */
    int rating=CLAMP((int)(hud.comboKills*5+(int)(hud.seriesScore/18)),0,999);
    char buf[16]; snprintf(buf,sizeof buf,"%03d",rating);
    int rx=(int)p->x+(int)p->w-31, ry=gy+12;
    fillrect(rx,ry,30,12,RGB(0x14,0x10,0x19),255); strokerect(rx,ry,30,12,RGB(0x96,0x82,0x9B),255);
    draw_text(rx+4,ry+1,buf,2,RGB(255,255,255),255,0);
    /* score */
    if(hud.dispScore>0){
        float al=CLAMP(hud.comboKills>0?1.f:hud.seriesFade,0.f,1.f);
        snprintf(buf,sizeof buf,"+%d",(int)hud.dispScore);
        draw_text(gx+2,ry+1,buf,2,RGB(255,255,255),(int)(al*255),1);
    }
    /* combo */
    if(hud.comboKills>0){
        snprintf(buf,sizeof buf,"+%d",hud.comboKills);
        draw_text(8,8,buf,3,RGB(0xFF,0x6B,0xA9),255,1);
    }
    /* bonus feed */
    if(nbon>0 && hud.feedAlpha>0){
        int yy=hud.comboKills>0?30:8;
        for(int i=0;i<nbon;i++){
            char line_[48]; snprintf(line_,sizeof line_,"%s +%d",bonuses[i].label,bonuses[i].count);
            int a=(int)(255*CLAMP(hud.feedAlpha,0.f,1.f));
            u32 col=bonuses[i].flash>0.2f?RGB(255,255,255):RGB(0xE2,0xD4,0xEA);
            draw_text(8,yy,line_,1,col,a,1); yy+=8;
        }
    }
    (void)time;
}

/* ------------------------------------------------------------------ */
/*  Game state                                                         */
/* ------------------------------------------------------------------ */
static float G_time, shake, hint_timer=5.f; static int interacted;
static float cur_x=140, cur_y=170, cur_vy, cur_prev_y=170;
static float press_hold, smear_rumble_cd, squelch_cd, cross_down_at, creak_cd;
static int was_smearing;
static int prev_cross, prev_circle, prev_square, prev_start, prev_triangle;
static int music_on;                     /* music state driven by the heart meter */
static int muted_music;
static int heart_flash;

static void add_shake(float a){ shake=MAXF(shake,MINF(24.f,a)); }

static void game_init(void){
    memset(ants,0,sizeof ants); memset(parts,0,sizeof parts);
    memset(&hud,0,sizeof hud); nbon=0;
    foot_init(&foot,160,120,GROUND_FRONT); foot_init(&bfoot,289,GROUND_BACK-60,GROUND_BACK); bfoot.grounded=1; bfoot.y=GROUND_BACK;
    portrait_init();
    for(int i=0;i<40;i++) ant_spawn(frand(20,SW-20),1);
}

static void evaluate_contact(Foot *f,int mode /*0 PIN 1 SLOW_SMEAR 2 SOFT 3 FULL*/,float dt,float hp,Result *out){
    memset(out,0,sizeof *out);
    float toe,heel; foot_sole(f,&toe,&heel);
    float reach=(mode==0)?6.f:2.f;
    float minX=MINF(toe,heel)-reach, maxX=MAXF(toe,heel)+reach;
    float yMin=ASPHALT_TOP-2, yMax=CURB_TOP+4;
    int killed[MAXANT]; int nk=0;
    float smearStr=1+hp*0.95f;
    float grind=(0.48f+fabsf(f->vx)*0.026f*2.6f+fabsf(f->angVel)*1.8f)*smearStr*dt;
    for(int i=0;i<MAXANT;i++){ Ant *a=&ants[i]; if(!a->alive) continue;
        if(a->x>=minX && a->x<=maxX && a->y>=yMin && a->y<=yMax){
            if(mode==0){ killed[nk++]=i; }
            else if(mode==1){
                if(!a->pinned) a->pinned=1;
                out->pinnedNow++; out->grindingNow++;
                a->crush-=MAXF(0.01f,grind*frand(0.75f,1.35f));
                if(frand(0,1)<0.36f+hp*0.38f){
                    float tb=1+hp*0.85f;
                    stamp_smear(a->x,a->y+2,f->vx*(1.25f+hp*0.45f),frand(3,6)*tb);
                    foot_add_goo(f,0.016f*tb);
                    if(hp>0.35f && frand(0,1)<hp*0.45f) spawn_splash(a->x,a->y,(int)(2+hp*3),0.65f+hp*0.45f,f->vx);
                }
                if(a->crush<=0) killed[nk++]=i;
            } else killed[nk++]=i;
        }
    }
    if(nk>0){
        out->isDry=f->dry>0.7f; out->isSmearing=(mode==1)||fabsf(f->vx)>35.f;
        float cx=0,cy=0;
        for(int k=0;k<nk;k++){ Ant *a=&ants[killed[k]];
            int alone=1;
            for(int j=0;j<MAXANT;j++){ Ant *o=&ants[j]; if(j==killed[k]||!o->alive) continue;
                if(hypotf(o->x-a->x,o->y-a->y)<34.f){ alone=0; break; } }
            if(alone) out->diedAlone++;
            if(a->screaming){ out->screamsCut++; sfx_stop(a->voice); a->screaming=0; }
            a->alive=0; cx+=a->x; cy+=a->y;
            foot_add_goo(f,0.035f);
            float hs=1+hp*0.85f;
            if(out->isSmearing) stamp_smear(a->x,a->y+2,(f->vx!=0?f->vx:f->angVel*180)*(1+hp*0.35f),frand(5,8)*hs);
            else stamp_puddle(a->x,a->y+2,frand(6,11)*(1+hp*0.35f),frand(2.2f,4.f)*(1+hp*0.35f),230);
            int sc=(int)((mode==3?8:5)*(1+hp*0.65f));
            spawn_splash(a->x,a->y,sc,(mode==3?1.18f:0.85f)*(1+hp*0.35f),f->vx);
            int base=(mode==3)?11:6; int pts=CLAMP((int)(base+hud_mult()*2.2f+(out->isDry?2:0)),6,15);
            popup_add(a->x,a->y-10,pts);
        }
        cx/=nk; cy/=nk;
        if(nk>=5){ int near_=0; for(int j=0;j<MAXANT;j++) if(ants[j].alive && hypotf(ants[j].x-cx,ants[j].y-cy)<30.f){ near_=1; break; } if(!near_) out->wipeout=1; }
        out->count=nk; out->mode=(mode==3)?1:0;
    }
}

static void handle_result(const Result *r){
    float hp=CLAMP(hud.hype/HEART_MAX,0.f,1.f);
    if(r->count>0){
        hud_register(r);
        int full=(r->mode==1);
        float boost=(r->isSmearing?1.22f:1.05f)+hp*0.42f;
        sx_crunch((full?1.28f:1.08f)*boost,full,r->count);
        portrait_trigger((full?1.25f:0.88f)*(1+hp*0.25f));
        add_shake((4.f+r->count*0.95f+(full?5.5f:0.f))*(1+hp*0.35f)*0.6f);
    }
    if(r->pinnedNow>0 && !r->isSmearing && creak_cd<=0){ sx_creak(); creak_cd=0.6f; }
}

static void music_logic(void){
    float lim_on=HEART_MAX*MUSIC_START_AT, lim_off=HEART_MAX*MUSIC_STOP_AT;
    if(!music_on && hud.hype>=lim_on){ music_on=1; if(!muted_music) music_start(); }
    else if(music_on && hud.hype<lim_off){ music_on=0; music_fadeout(); }
}

static void game_update(float dt,const Pad *pd){
    G_time+=dt; if(hint_timer>0) hint_timer=MAXF(0,hint_timer-dt);
    shake=MAXF(0,shake-dt*36.f); creak_cd=MAXF(0,creak_cd-dt); squelch_cd=MAXF(0,squelch_cd-dt);
    if(pd->cross||pd->circle||pd->square||fabsf(pd->lx)>0.2f||fabsf(pd->ly)>0.2f){ if(!interacted){ interacted=1; hint_timer=0; } }

    /* edges */
    int cross_edge=pd->cross&&!prev_cross, cross_rel=!pd->cross&&prev_cross;
    int slam_btn=pd->circle||pd->square, slam_edge=(pd->circle&&!prev_circle)||(pd->square&&!prev_square);
    if(pd->start&&!prev_start){ sfx_muted=!sfx_muted; }
    if(pd->triangle&&!prev_triangle){ muted_music=!muted_music; if(muted_music) music_fadeout(); else if(music_on) music_start(); }
    prev_cross=pd->cross; prev_circle=pd->circle; prev_square=pd->square; prev_start=pd->start; prev_triangle=pd->triangle;

    /* cursor from stick */
    cur_x=CLAMP(cur_x+pd->lx*dt*260.f,10.f,SW-10.f);
    cur_y=CLAMP(cur_y+pd->ly*dt*210.f,56.f,GROUND_FRONT);
    cur_vy=(cur_y-cur_prev_y)/MAXF(dt,0.001f); cur_prev_y=cur_y;
    if(pd->l) foot.tilt=CLAMP(foot.tilt-dt*1.2f,-0.32f,0.32f);
    if(pd->r) foot.tilt=CLAMP(foot.tilt+dt*1.2f,-0.32f,0.32f);

    if(cross_edge) cross_down_at=G_time;
    if(cross_rel && G_time-cross_down_at<0.17f && !foot.grounded){ foot.slamming=1; foot.ty=foot.gy; }
    if(slam_edge && !foot.grounded) foot.slamming=1;

    if(foot.slamming){ foot.tx=cur_x; foot.ty=foot.gy; }
    else if(pd->cross){
        foot.tx=cur_x;
        if(!foot.grounded && cur_vy>210.f){ foot.slamming=1; foot.ty=foot.gy; }
        else if(foot.grounded) foot.ty=(cur_y>=foot.gy-54.f)?foot.gy:cur_y;
        else foot.ty=CLAMP(cur_y,56.f,foot.gy);
    } else { foot.tx=cur_x; foot.ty=CLAMP(cur_y,56.f,foot.gy-8.f); }
    if(slam_btn){ if(!foot.grounded) foot.slamming=1; foot.ty=foot.gy; }

    int prevFP=foot.fp;
    foot_update(&foot,dt);

    float avx=fabsf(foot.vx);
    smear_rumble_cd=MAXF(0,smear_rumble_cd-dt);
    int smearing=interacted && foot.grounded && (avx>14.f||fabsf(foot.angVel)>0.30f);
    if(smearing && !was_smearing && smear_rumble_cd<=0){ sx_rumble(); smear_rumble_cd=0.55f; }
    was_smearing=smearing;

    if(interacted && foot.grounded && (pd->cross||slam_btn)) press_hold+=dt; else press_hold=MAXF(0,press_hold-dt*3.5f);

    float rise_t=0;
    if(interacted && foot.grounded){
        if(slam_btn) rise_t=1.f;
        else if(smearing) rise_t=CLAMP(0.42f+avx/160.f,0.42f,0.78f);
        else if(pd->cross && press_hold>0.35f) rise_t=CLAMP((press_hold-0.35f)*0.9f,0.f,0.55f);
    }
    support_rise+=(rise_t-support_rise)*MINF(1.f,6.f*dt);
    foot.fp=interacted && foot.grounded && (support_rise>0.80f||slam_btn);

    float maxSmear=0, smearGoo=0; int anyGround=0, grinding=0;
    int dots=hud_mult();
    float hp=CLAMP(hud.hype/HEART_MAX,0.f,1.f);
    Result r;

    if(foot.grounded && !foot.wasGrounded){
        float imp=CLAMP(foot.vy/255.f,0.35f,1.55f);
        foot.recoil=imp; sx_thud(imp,foot.fp); add_shake(imp*(foot.fp?9.f:4.5f));
        if(foot.fp||imp>0.75f) add_wave(foot.x,foot.gy+2,foot.fp?60.f:42.f);
        if(foot.fp){ evaluate_contact(&foot,3,dt,hp,&r); handle_result(&r); }
        else if(foot.vy>218.f){ evaluate_contact(&foot,2,dt,hp,&r); handle_result(&r); }
        else { sx_creak(); evaluate_contact(&foot,0,dt,hp,&r); handle_result(&r); }
    }
    if(!foot.grounded && foot.wasGrounded){
        float toe,heel; foot_sole(&foot,&toe,&heel);
        if(foot.goo>0.12f) { add_strand(heel+6,toe-6,foot.gy); add_strand((heel+toe)*0.5f-8,(heel+toe)*0.5f+8,foot.gy); }
        foot.goo*=0.6f;
    }
    if(foot.fp && !prevFP && foot.wasGrounded){
        foot.recoil=0.95f; sx_thud(1.25f,1); sx_creak(); add_wave(foot.x,foot.gy+2,66.f); add_shake(9.f);
        evaluate_contact(&foot,3,dt,hp,&r); handle_result(&r);
    }
    if(interacted && !foot.grounded && foot.y>=foot.gy-20.f){
        evaluate_contact(&foot,0,dt,hp,&r); if(r.count>0) sx_bone(r.count); handle_result(&r);
    }
    if(interacted && foot.grounded){
        anyGround=1; if(avx>maxSmear){ maxSmear=avx; smearGoo=1-foot.dry; }
        if(foot.fp){ evaluate_contact(&foot,3,dt,hp,&r); handle_result(&r); }
        else if(avx>14.f||fabsf(foot.angVel)>0.32f){
            evaluate_contact(&foot,1,dt,hp,&r);
            if(r.grindingNow>0){ grinding=1; if(hp>0.2f) add_shake(0.5f+hp*1.6f); }
            handle_result(&r);
            if(avx>55.f && foot.dry<0.85f && frand(0,1)<0.35f){
                float toe,heel; foot_sole(&foot,&toe,&heel);
                stamp_smear(heel+(toe-heel)*frand(0.12f,0.88f),foot.gy+1,foot.vx*0.75f,frand(2.5f,4.5f)); }
        } else { evaluate_contact(&foot,0,dt,hp,&r); if(r.count>0) sx_bone(r.count); handle_result(&r); }
    }
    /* wet squelch while smearing, juicier with hearts */
    if(anyGround && maxSmear>30.f && (smearGoo>0.06f||grinding) && squelch_cd<=0){
        sx_squelch(0.25f+hp*0.55f+MINF(maxSmear/400.f,0.25f)); squelch_cd=frand(0.10f,0.22f)-hp*0.05f; }

    int pinned=0; for(int i=0;i<MAXANT;i++) if(ants[i].alive&&ants[i].pinned) pinned++;
    if(pinned>0 || (hud.comboKills>0 && foot.fp)) hud_crush_time(dt,smearing);

    /* back foot (decor) */
    bfoot.x=289; bfoot.y=bfoot.gy-support_rise*10.f;

    /* ants */
    float speedMult=0.58f+dots*0.355f;
    static float spawn_t=0; spawn_t-=dt;
    if(spawn_t<=0){ int alive=0; for(int i=0;i<MAXANT;i++) if(ants[i].alive) alive++;
        int deficit=MAXF(0,70-alive); int batch=deficit>30?irand(5,9):irand(2,4);
        for(int i=0;i<batch;i++) ant_spawn(0,0);
        spawn_t=frand(0.7f,1.8f); }
    float toe,heel; foot_sole(&foot,&toe,&heel); float fminx=MINF(toe,heel)-4, fmaxx=MAXF(toe,heel)+4;
    for(int i=0;i<MAXANT;i++){ Ant *a=&ants[i]; if(!a->alive) continue;
        if(a->pinned){
            if(!(foot.grounded && a->x>=fminx && a->x<=fmaxx)){ a->pinned=0; a->crush=MINF(1.f,a->crush+0.2f); }
            else { a->scream_t-=dt; if(!a->screaming && a->scream_t<=0 && frand(0,1)<dt*3){ a->screaming=1; a->voice=sx_scream(); } }
            continue; }
        a->x+=a->vx*dt*speedMult;
        if(a->x<-30||a->x>SW+30){ a->alive=0; continue; }
        /* occasional panicked scream */
        a->scream_t-=dt;
        if(a->scream_t<=0){ a->scream_t=frand(2.5f,6.f); if(frand(0,1)<0.05f && !a->screaming){ a->screaming=1; a->voice=sx_scream(); } }
        if(a->screaming && a->voice>=0 && !voices[a->voice].on){ a->screaming=0; }
    }
    for(int i=0;i<MAXPOP;i++) if(pops[i].on){ pops[i].y+=pops[i].vy*dt; pops[i].life-=dt; if(pops[i].life<=0) pops[i].on=0; }

    city_update(dt);
    goo_update(dt);
    hud_update(dt);
    music_logic();
    portrait_update(dt,hud.comboKills>0,hud.hype,smearing||grinding,dots,music_on);
    (void)heart_flash;
}

static void game_draw(void){
    /* world with shake */
    T=&backS;
    int sx=0,sy=0;
    if(shake>0.2f){ sx=(int)frand(-shake,shake)/2; sy=(int)frand(-shake,shake)/2; }
    for(int y=0;y<SH;y++){
        int ys=y-sy; u32 *d=&back_px[y*SW];
        if(ys<0||ys>=SH){ for(int x=0;x<SW;x++) d[x]=RGB(8,5,12); continue; }
        const u32 *s=&scene_px[ys*SW];
        if(sx==0) memcpy(d,s,SW*4);
        else for(int x=0;x<SW;x++){ int xs=x-sx; d[x]=(xs<0||xs>=SW)?s[xs<0?0:SW-1]:s[xs]; }
    }
    OX=sx; OY=sy;
    city_draw_anim(G_time);
    goo_draw_back();
    /* back foot (planted, rising on the tiptoe) */
    { Foot *b=&bfoot; fillellipse(b->x,b->gy+2,30,5,RGB(0,0,0),80);
      const Img *im=&back_boot[support_rise>0.5f?1:0]; int dx=(int)(b->x-0.41f*112), dy=(int)(b->y-112+2);
      blit(im,dx,dy,255,RGB(190,175,200));
      for(int x=0;x<112;x++){ u32 c=im->p[x]; if(CA(c)>200) for(int y=-OY;y<dy;y++) putpx(dx+x,y,RGB(CR(c)*190/255,CG(c)*175/255,CB(c)*200/255),255); } }
    /* ants sorted by y (cheap insertion by buckets) */
    for(int pass=0;pass<4;pass++){
        float lo=ASPHALT_TOP+pass*10.f, hi=lo+10.f;
        for(int i=0;i<MAXANT;i++) if(ants[i].alive && ants[i].y>=lo && (ants[i].y<hi||pass==3)) draw_ant(&ants[i],G_time);
    }
    strands_draw();
    foot_draw(&foot,foot_pose(&foot),1.f);
    goo_draw_front();
    for(int i=0;i<MAXPOP;i++) if(pops[i].on){ char b[8]; snprintf(b,sizeof b,"+%d",pops[i].val);
        draw_text((int)pops[i].x,(int)pops[i].y,b,1,RGB(255,255,255),(int)(255*CLAMP(pops[i].life/0.5f,0.f,1.f)),1); }
    /* ground target marker */
    if(!foot.grounded){ int tx=(int)foot.x; fillrect(tx-1,(int)foot.gy+4,3,1,RGB(255,140,190),140); }
    OX=OY=0;
    portrait_draw(G_time,hud.comboKills);
    hud_draw(G_time);
    if(!interacted && hint_timer>0){
        int a=(int)(220*CLAMP(hint_timer/0.8f,0.f,1.f));
        fillrect(40,118,280,30,RGB(14,10,22),a*3/4);
        draw_text(46,122,"STICK MOVE   X PRESS OR HOLD TO SMEAR",1,RGB(255,255,255),a,0);
        draw_text(46,130,"O OR SQUARE SLAM   L R TILT   START SFX",1,RGB(255,255,255),a,0);
        draw_text(46,138,"TRIANGLE MUSIC",1,RGB(255,255,255),a,0);
    }
    if(!assets_ok){ draw_text(8,SH-10,"ASSETS MISSING",1,RGB(255,80,80),255,1); }
}

/* ------------------------------------------------------------------ */
/*  Entry points                                                       */
/* ------------------------------------------------------------------ */
#ifdef HOST
/* ---- headless host test -------------------------------------------------- */
static void dump_ppm(const char *name){
    FILE *f=fopen(name,"wb"); fprintf(f,"P6\n%d %d\n255\n",SW,SH);
    for(int i=0;i<SW*SH;i++){ u32 c=back_px[i]; fputc(CR(c),f); fputc(CG(c),f); fputc(CB(c),f); }
    fclose(f);
}
static void step(int frames,float dt){
    static s16 mb[1470*2]; 
    for(int i=0;i<frames;i++){ Pad p; pad_read(&p); game_update(dt,&p); game_draw(); mix_block(mb,735); }
}
int main(int argc,char **argv){
    (void)argc; strncpy(BASE,argc>1?argv[1]:"./",sizeof BASE-1);
    load_images(); load_audio(); render_city(); game_init();
    printf("assets_ok=%d music_len=%d (%.1fs) crunch=%d thud=%d\n",assets_ok,music_len,music_len/22050.0,n_crunch,n_thud);
    float dt=1.f/60.f;
    /* 1) idle */
    step(60,dt); dump_ppm("/tmp/f_idle.ppm");
    /* 2) hold cross, sweep stick left/right: natural gameplay */
    int firstMusicFrame=-1; float hypeAtStart=0;
    int music_count_before=music_started_count;
    for(int fr=0; fr<60*400 && firstMusicFrame<0; fr++){
        host_pad.cross=1; host_pad.ly=(fr<30)?1.f:0.f; host_pad.lx=sinf(fr*0.045f)>0?1.f:-1.f;
        step(1,dt);
        if(music_on && firstMusicFrame<0){ firstMusicFrame=fr; hypeAtStart=hud.hype; }
        if(fr==600) dump_ppm("/tmp/f_mid.ppm");
    }
    printf("natural play: music_on at frame %d (%.1fs) hype=%.3f (threshold %.3f) started_count=%d->%d\n",
        firstMusicFrame,firstMusicFrame/60.0,hypeAtStart,HEART_MAX*MUSIC_START_AT,music_count_before,music_started_count);
    step(90,dt);
    printf("music_playing=%d vol=%.2f rapture=%.2f\n",(int)music_playing,music_vol,portrait.rapture);
    dump_ppm("/tmp/f_rapture.ppm");
    /* 3) forced threshold test: below -> no music, at 85% -> music, hysteresis, restart */
    host_pad.cross=0; host_pad.lx=host_pad.ly=0;
    music_on=0; music_playing=0; music_started_count=0; hud.hype=4.20f; hud.comboKills=0;
    for(int i=0;i<5;i++) music_logic();
    printf("hype 4.20 (84%%): music_on=%d started=%d\n",music_on,music_started_count);
    hud.hype=4.26f; music_logic();
    printf("hype 4.26 (85.2%%): music_on=%d started=%d\n",music_on,music_started_count);
    hud.hype=3.5f; music_logic(); printf("hype 3.5 (70%%, hysteresis): music_on=%d\n",music_on);
    hud.hype=2.9f; music_logic(); printf("hype 2.9 (58%%): music_on=%d target=%.1f\n",music_on,music_target);
    hud.hype=4.5f; music_logic(); printf("hype 4.5 again: music_on=%d started=%d\n",music_on,music_started_count);
    /* audio actually produces signal? */
    { static s16 mb[2048]; double e=0; music_pos=0; music_vol=1; music_target=1; music_playing=1;
      for(int k=0;k<20;k++){ mix_block(mb,1024); for(int i=0;i<2048;i++) e+=fabsf((float)mb[i]); }
      printf("mixer music energy/sample = %.1f\n",e/(20*2048)); }
    hud.hype=4.6f; hud.comboKills=0;
    for(int i=0;i<30;i++){ step(1,dt); hud.hype=4.6f; }
    dump_ppm("/tmp/f_rapture2.ppm");
    hud.hype=0.4f; portrait.rapture=0; portrait.joy=0; portrait.blush=0; music_on=0;
    step(60,dt); dump_ppm("/tmp/f_calm.ppm");
    return 0;
}
#else
static volatile int running=1;
static int exit_cb(int a1,int a2,void *c){ (void)a1;(void)a2;(void)c; running=0; return 0; }
static int cb_thread(SceSize args,void *argp){ (void)args;(void)argp;
    int cbid=sceKernelCreateCallback("Exit",exit_cb,NULL); sceKernelRegisterExitCallback(cbid); sceKernelSleepThreadCB(); return 0; }
static int audio_thread(SceSize args,void *argp){ (void)args;(void)argp;
    static s16 buf[1024*2] __attribute__((aligned(64)));
    int ch=sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL,1024,PSP_AUDIO_FORMAT_STEREO);
    if(ch<0) return 0;
    while(running){ mix_block(buf,1024); sceAudioOutputBlocking(ch,PSP_AUDIO_VOLUME_MAX,buf); }
    sceAudioChRelease(ch); return 0; }

int main(int argc,char **argv){
    scePowerSetClockFrequency(333,333,166);
    int th=sceKernelCreateThread("cb",cb_thread,0x11,0xFA0,0,NULL); if(th>=0) sceKernelStartThread(th,0,NULL);
    if(argc>0 && argv[0]){ strncpy(BASE,argv[0],sizeof BASE-1); char *s=strrchr(BASE,'/'); if(s) s[1]=0; else BASE[0]=0; }
    load_images(); load_audio(); render_city(); game_init();
    sceCtrlSetSamplingCycle(0); sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    void *vram0=(void*)(0x40000000|(u32)sceGeEdramGetAddr());
    void *vram[2]={vram0,(u8*)vram0+FBW*SH*4};
    int draw=0;
    sceDisplaySetMode(0,SW,SH);
    sceDisplaySetFrameBuf(vram[1],FBW,PSP_DISPLAY_PIXEL_FORMAT_8888,PSP_DISPLAY_SETBUF_IMMEDIATE);
    int ath=sceKernelCreateThread("audio",audio_thread,0x12,0x10000,0,NULL); if(ath>=0) sceKernelStartThread(ath,0,NULL);
    u64 last=sceKernelGetSystemTimeWide();
    while(running){
        u64 now=sceKernelGetSystemTimeWide(); float dt=(float)(now-last)/1e6f; last=now; dt=CLAMP(dt,0.001f,0.05f);
        Pad pd; pad_read(&pd);
        game_update(dt,&pd); game_draw();
        u32 *dst=(u32*)vram[draw];
        for(int y=0;y<SH;y++) memcpy(dst+y*FBW,back_px+y*SW,SW*4);
        sceDisplayWaitVblankStart();
        sceDisplaySetFrameBuf(vram[draw],FBW,PSP_DISPLAY_PIXEL_FORMAT_8888,PSP_DISPLAY_SETBUF_NEXTFRAME);
        draw^=1;
    }
    sceKernelExitGame(); return 0;
}
#endif
