/* ALTUHA GAME - native PSP port (plain C), stage 2
 * Game logic runs in the original 1280x720 canvas units and is scaled by
 * K = 480/1280 only when vertices are emitted. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspaudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "poses.h"
#include "poses_b.h"
#include "poses_c.h"
#include "chars.h"
#include "sfx_ids.h"

PSP_MODULE_INFO("ALTUHAGAME", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define CW 1280.0f
#define CH 720.0f
#define K  0.375f
#define ASPHALT_TOP 588.0f
#define CURB_TOP 690.0f
#define GROUND_Y_FRONT 682.0f
#define GROUND_Y_BACK 640.0f
#define WORLD_ZOOM 1.12f
#define MAXV_A 40000
#define MAXV_B 24000

typedef struct { unsigned int c; float x, y, z; } VC;
typedef struct { float u, v; unsigned int c; float x, y, z; } VT;

static unsigned int __attribute__((aligned(16))) list[65536];
static VC __attribute__((aligned(16))) vbA[MAXV_A];
static VC __attribute__((aligned(16))) vbB[MAXV_B];
static VC __attribute__((aligned(16))) vbC[MAXV_B];
static VC __attribute__((aligned(16))) vbD[MAXV_B];
static VC __attribute__((aligned(16))) vbE[MAXV_B];
static VT __attribute__((aligned(16))) tvLeg[4][6];
static VT __attribute__((aligned(16))) tvBody[4][6];
static VT __attribute__((aligned(16))) tvCity[6];
static VT __attribute__((aligned(16))) tvPort[6];
#define NSLOT 7   /* poses the foot code really uses: 0,3,4,5,6,7,8 */
static const int poseSlot[NPOSE] = {0, -1, -1, 1, 2, 3, 4, 5, 6, -1, -1};
static unsigned int __attribute__((aligned(16))) bootTex[NCHAR][NSLOT][256 * 256];
static unsigned int __attribute__((aligned(16))) portTex[NCHAR][14][64 * 64];
static int bootOk[NCHAR][NSLOT], portOk[NCHAR][14];
static int cityOk = 0;
static int curChar = 0;                    /* 0 = boots, 1 = barefoot, 2 = striped sweater */
static const PoseInfo *charPoses[NCHAR] = { POSES_A, POSES_B, POSES_C };
static const PoseInfo *curPoses = POSES_A;
#define POSES curPoses
/* VRAM: draw 0x0, disp 0x88000, depth 0x110000; city textures live behind them (read from VRAM = fast) */
#define VRAM_CITY_TOP 0x154000
#define VRAM_CITY_BOT 0x194000
#ifdef SIM_BUILD
static unsigned char simVram[0x200000];
#define VRAM_CPU(off) ((void *)(simVram + (off)))
#define VRAM_GE(off) ((void *)(simVram + (off)))
#else
#define VRAM_CPU(off) ((void *)(0x44000000 + (off)))
#define VRAM_GE(off) ((void *)(0x04000000 + (off)))
#endif

static char base[256] = "";
static float shakeX = 0, shakeY = 0;
static int useZoom = 1;
static float gPsycho = 0;   /* 0..1, ramps up while the music (psycho mode) is on */
/* ---------- helpers ---------- */
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float rnd(float a, float b) { return a + (float)rand() / (float)RAND_MAX * (b - a); }
static int rndi(int a, int b) { return a + rand() % (b - a + 1); }
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }
static unsigned int col(int r, int g, int b, int a) {
  return ((unsigned)a << 24) | ((unsigned)b << 16) | ((unsigned)g << 8) | (unsigned)r;
}
#define RGB(r, g, b) col(r, g, b, 255)

static void logf_(const char *msg) {
  FILE *f = fopen("ms0:/DUSKLOG.TXT", "ab");
  if (f) { fputs(msg, f); fputc('\n', f); fclose(f); }
}
static void say(const char *msg) { pspDebugScreenPrintf("%s\n", msg); logf_(msg); }
static const char *ap(const char *rel) {
  static char b[4][320];
  static int i = 0;
  i = (i + 1) & 3;
  snprintf(b[i], sizeof(b[i]), "%s%s", base, rel);
  return b[i];
}

/* world (canvas) -> PSP pixel */
static float wX(float x) { if (useZoom) x = CW * 0.5f + (x - CW * 0.5f) * WORLD_ZOOM; return (x + shakeX) * K; }
static float wY(float y) { if (useZoom) y = CH + (y - CH) * WORLD_ZOOM; return (y + shakeY) * K; }
static float wS(float s) { return s * (useZoom ? WORLD_ZOOM : 1.0f) * K; }

/* ---------- batched, order-preserving primitives ---------- */
#define MAXSEG 300
typedef struct { VC *buf; int cap, n, nseg; int type[MAXSEG], start[MAXSEG]; } Batch;
static Batch bA = { vbA, MAXV_A }, bB = { vbB, MAXV_B }, bC = { vbC, MAXV_B }, bD = { vbD, MAXV_B }, bE = { vbE, MAXV_B };
static Batch *CB = &bA;

static int bPrep(int type, int need) {
  if (CB->n + need > CB->cap) return 0;
  if (CB->nseg == 0 || CB->type[CB->nseg - 1] != type) {
    if (CB->nseg >= MAXSEG) return 0;
    CB->type[CB->nseg] = type; CB->start[CB->nseg] = CB->n; CB->nseg++;
  }
  return 1;
}
static void rectP(float x, float y, float w, float h, unsigned int c) {
  VC *v;
  if (!bPrep(GU_SPRITES, 2)) return;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  x = floorf(x); y = floorf(y); w = floorf(w + 0.5f); h = floorf(h + 0.5f);
  v = CB->buf + CB->n;
  v[0].c = c; v[0].x = x;     v[0].y = y;     v[0].z = 0;
  v[1].c = c; v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
  CB->n += 2;
}
static void triP(float x0, float y0, float x1, float y1, float x2, float y2, unsigned int c) {
  VC *v;
  if (!bPrep(GU_TRIANGLES, 3)) return;
  v = CB->buf + CB->n;
  v[0].c = c; v[0].x = x0; v[0].y = y0; v[0].z = 0;
  v[1].c = c; v[1].x = x1; v[1].y = y1; v[1].z = 0;
  v[2].c = c; v[2].x = x2; v[2].y = y2; v[2].z = 0;
  CB->n += 3;
}
static void quadP(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, unsigned int c) {
  triP(x0, y0, x1, y1, x2, y2, c); triP(x0, y0, x2, y2, x3, y3, c);
}
static float ecos[13], esin[13]; static int etabInit = 0;
static void ellipseP(float cx, float cy, float rx, float ry, float rot, unsigned int c) {
  int segs, step, i, n; float cr = 1, sr = 0, px = 0, py = 0;
  if (!etabInit) { for (i = 0; i <= 12; i++) { ecos[i] = cosf(6.2831853f * i / 12); esin[i] = sinf(6.2831853f * i / 12); } etabInit = 1; }
  if (rx < 0.4f && ry < 0.4f) return;
  if (cx + rx < -4 || cx - rx > 484 || cy + ry < -4 || cy - ry > 276) return;
  if (rx > 5 || ry > 5) { segs = 12; step = 1; } else { segs = 6; step = 2; }
  if (rot != 0) { cr = cosf(rot); sr = sinf(rot); }
  for (i = 0, n = 0; n <= segs; n++, i += step) {
    float ex = ecos[i] * rx, ey = esin[i] * ry;
    float x = cx + ex * cr - ey * sr, y = cy + ex * sr + ey * cr;
    if (n > 0) triP(cx, cy, px, py, x, y, c);
    px = x; py = y;
  }
}
/* world-space helpers (canvas units, zoomed) */
static void rectW(float x, float y, float w, float h, unsigned int c) {
  float x0 = wX(x), y0 = wY(y), x1 = wX(x + w), y1 = wY(y + h);
  rectP(x0, y0, x1 - x0, y1 - y0, c);
}
static void ellipseW(float cx, float cy, float rx, float ry, float rot, unsigned int c) {
  ellipseP(wX(cx), wY(cy), wS(rx), wS(ry), rot, c);
}
static void lineW(float x1, float y1, float x2, float y2, float w, unsigned int c) {
  float ax = wX(x1), ay = wY(y1), bx = wX(x2), by = wY(y2), dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy), hw = wS(w) * 0.5f, nx, ny;
  if (hw < 0.5f) hw = 0.5f;
  if (len < 0.001f) { ellipseP(ax, ay, hw, hw, 0, c); return; }
  nx = -dy / len * hw; ny = dx / len * hw;
  quadP(ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny, c);
  if (hw > 3.2f) { ellipseP(ax, ay, hw, hw, 0, c); ellipseP(bx, by, hw, hw, 0, c); }
}
static void flushBatch(Batch *b) {
  int i;
  if (b->n > 0) {
    sceKernelDcacheWritebackRange(b->buf, b->n * sizeof(VC));
    sceGuDisable(GU_TEXTURE_2D);
    for (i = 0; i < b->nseg; i++) {
      int s = b->start[i], e = (i + 1 < b->nseg) ? b->start[i + 1] : b->n;
      if (e - s <= 0) continue;
      sceGuDrawArray(b->type[i], GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, e - s, 0, b->buf + s);
    }
  }
  b->n = 0; b->nseg = 0;
}
static void useBatch(Batch *b) { CB = b; b->n = 0; b->nseg = 0; }

/* 5x7 font: space, 0-9, A-Z */
static const unsigned char glyph[42][7] = {
  {0,0,0,0,0,0,0}, {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
  {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30}, {6,8,16,30,17,17,14},
  {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
  {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14}, {28,18,17,17,17,18,28},
  {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16}, {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
  {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
  {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
  {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17}, {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
  {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
  {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
  {0,4,4,31,4,4,0}, {0,12,12,0,12,12,0}, {0,0,0,31,0,0,0}, {0,0,0,0,0,12,12}, {4,4,4,4,4,0,4}
};
static void textP(float x, float y, const char *s, unsigned int c, int sc) {
  for (; *s; s++, x += 6 * sc) {
    int id = 0, yy, xx;
    char ch = *s;
    if (ch >= '0' && ch <= '9') id = 1 + ch - '0';
    else if (ch >= 'A' && ch <= 'Z') id = 11 + ch - 'A';
    else if (ch >= 'a' && ch <= 'z') id = 11 + ch - 'a';
    else if (ch == '+') id = 37; else if (ch == ':') id = 38; else if (ch == '-') id = 39; else if (ch == '.') id = 40; else if (ch == '!') id = 41;
    if (id == 0) continue;
    for (yy = 0; yy < 7; yy++) {
      xx = 0;
      while (xx < 5) {
        if (glyph[id][yy] & (1 << (4 - xx))) {
          int run = 1;
          while (xx + run < 5 && (glyph[id][yy] & (1 << (4 - xx - run)))) run++;
          rectP(x + xx * sc, y + yy * sc, run * sc, sc, c);
          xx += run;
        } else xx++;
      }
    }
  }
}

/* tiny 3x5 font (digits, A-Z, + : - .) for the small HUD text */
static const unsigned char tiny[41][5] = {
  {0,0,0,0,0},
  {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},{7,5,7,5,7},{7,5,7,1,7},
  {2,5,7,5,5},{6,5,6,5,6},{3,4,4,4,3},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4},{3,4,5,5,3},{5,5,7,5,5},{7,2,2,2,7},{1,1,1,5,2},
  {5,5,6,5,5},{4,4,4,4,7},{5,7,7,5,5},{6,5,5,5,5},{2,5,5,5,2},{6,5,6,4,4},{2,5,5,6,3},{6,5,6,5,5},{3,4,2,1,6},{7,2,2,2,2},
  {5,5,5,5,7},{5,5,5,5,2},{5,5,7,7,5},{5,5,2,5,5},{5,5,2,2,2},{7,1,2,4,7},
  {0,2,7,2,0},{0,2,0,2,0},{0,0,7,0,0},{0,0,0,0,2}
};
static void textS(float x, float y, const char *s, unsigned int c) {
  for (; *s; s++, x += 4) {
    int id = 0, yy, xx; char ch = *s;
    if (ch >= '0' && ch <= '9') id = 1 + ch - '0';
    else if (ch >= 'A' && ch <= 'Z') id = 11 + ch - 'A';
    else if (ch >= 'a' && ch <= 'z') id = 11 + ch - 'a';
    else if (ch == '+') id = 37; else if (ch == ':') id = 38; else if (ch == '-') id = 39; else if (ch == '.') id = 40;
    if (id == 0) continue;
    for (yy = 0; yy < 5; yy++) {
      xx = 0;
      while (xx < 3) {
        if (tiny[id][yy] & (1 << (2 - xx))) {
          int run = 1;
          while (xx + run < 3 && (tiny[id][yy] & (1 << (2 - xx - run)))) run++;
          rectP(x + xx, y + yy, run, 1, c);
          xx += run;
        } else xx++;
      }
    }
  }
}
/* ---------- audio: sfx pool + loop voices + streamed music, mixed in a thread ---------- */
#define POOL_MAX 500000
#define NV 16
#define LOOP_SMEAR 12
#define LOOP_GRIT 13
#define LOOP_WET 14
#define LOOP_DESC 15
#define RING 32768
#define MUSIC_VOL 0.55f
typedef struct { const short *p; int len; volatile float pos; float rate, gain, target, tc; int loop, tag; volatile int active; } Voice;
static short __attribute__((aligned(64))) sfxPool[POOL_MAX];
static int sfxOff[SFX_COUNT], sfxLen[SFX_COUNT], sfxLoaded = 0;
static Voice vox[NV];
static volatile int audioRun = 0, musicRun = 0;
static int audioCh = -1, audioThread = -1, musicThread = -1, muted = 0;
static short __attribute__((aligned(64))) musicRing[RING * 2];
static volatile unsigned mwr = 0;
static unsigned mrdI = 0; static float mrdF = 0;
static volatile int musicPrimed = 0, musicRestartReq = 0, musicWant = 0, musicPaused = 0, musicAvail = 0;
static float musicGain = 0, musicTarget = 0;

static int sfxId(const char *name) {
  int i;
  for (i = 0; i < SFX_COUNT; i++) if (!strcmp(SFX_NAMES[i], name)) return i;
  return -1;
}
static int sfxVar(const char *prefix, int n) { char nm[24]; snprintf(nm, sizeof(nm), "%s%d", prefix, rand() % n); return sfxId(nm); }
static int sfxPlay(int id, float gain, float rate, int tag) {
  int i, best = 0;
  if (muted || !sfxLoaded || id < 0 || audioCh < 0) return -1;
  for (i = 0; i < 12; i++) { if (!vox[i].active) { best = i; goto found; } if (vox[i].pos > vox[best].pos) best = i; }
found:
  vox[best].active = 0;
  vox[best].p = sfxPool + sfxOff[id]; vox[best].len = sfxLen[id]; vox[best].pos = 0; vox[best].rate = rate;
  vox[best].gain = gain; vox[best].target = gain; vox[best].tc = 0.02f; vox[best].loop = 0; vox[best].tag = tag;
  vox[best].active = 1;
  return best;
}
static void sfxFadeTag(int tag, float tc) {
  int i;
  for (i = 0; i < 12; i++) if (vox[i].active && vox[i].tag == tag) { vox[i].target = 0; vox[i].tc = tc; }
}
static void loopSet(int slot, float target, float rate) { vox[slot].target = muted ? 0 : target; vox[slot].rate = rate; }

static int audioMain(SceSize args, void *argp) {
  static short __attribute__((aligned(64))) buf[1024 * 2];
  static float mix[1024 * 2];
  const float blockDt = 1024.0f / 44100.0f;
  (void)args; (void)argp;
  while (audioRun) {
    int v, i;
    memset(mix, 0, sizeof(mix));
    for (v = 0; v < NV; v++) {
      Voice *vo = &vox[v];
      float pos, rate, gain, k;
      int len;
      if (!vo->active || !vo->p) continue;
      if (vo->loop && vo->gain < 0.0008f && vo->target < 0.0008f) { vo->gain = 0; continue; }
      k = 1.0f - expf(-blockDt / vo->tc);
      vo->gain += (vo->target - vo->gain) * k;
      gain = vo->gain / 32768.0f; pos = vo->pos; rate = vo->rate * 0.5f; len = vo->len;
      for (i = 0; i < 1024; i++) {
        int ip = (int)pos; float fr, s0, s1;
        if (ip + 1 >= len) { if (vo->loop) { pos -= (float)len; if (pos < 0) pos = 0; ip = (int)pos; } else { vo->active = 0; break; } }
        fr = pos - ip; s0 = vo->p[ip]; s1 = vo->p[ip + 1 < len ? ip + 1 : ip];
        { float s = (s0 + (s1 - s0) * fr) * gain; mix[i * 2] += s; mix[i * 2 + 1] += s; }
        pos += rate;
      }
      vo->pos = pos;
      if (!vo->loop && vo->target <= 0.0005f && vo->gain < 0.002f) vo->active = 0;
    }
    /* music stream (32 kHz stereo -> 44.1 kHz) */
    musicGain += (musicTarget - musicGain) * (1.0f - expf(-blockDt / 0.6f));
    if (musicGain > 0.003f && musicWant && !musicPaused && musicPrimed && !musicRestartReq) {
      const float step = 32000.0f / 44100.0f;
      for (i = 0; i < 1024; i++) {
        unsigned avail = mwr - mrdI;
        if (avail < 3) break;
        { unsigned a = mrdI % RING, b2 = (mrdI + 1) % RING; float f = mrdF;
          float l = musicRing[a * 2] + (musicRing[b2 * 2] - musicRing[a * 2]) * f;
          float r = musicRing[a * 2 + 1] + (musicRing[b2 * 2 + 1] - musicRing[a * 2 + 1]) * f;
          mix[i * 2] += l / 32768.0f * musicGain; mix[i * 2 + 1] += r / 32768.0f * musicGain; }
        mrdF += step; if (mrdF >= 1.0f) { int adv = (int)mrdF; mrdI += adv; mrdF -= adv; }
      }
    }
    for (i = 0; i < 1024 * 2; i++) {
      float x = mix[i] * 0.8f, ax = x < 0 ? -x : x; int xi;
      if (ax > 0.6f) { float e = (ax - 0.6f) / 0.4f; ax = 0.6f + 0.4f * (e / (1.0f + e)); x = x < 0 ? -ax : ax; }   /* soft limiter */
      xi = (int)(x * 31000.0f); buf[i] = (short)xi;
    }
    sceAudioOutputPannedBlocking(audioCh, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, buf);
  }
  return 0;
}
static int musicMain(SceSize args, void *argp) {
  static short tmp[4096 * 2];
  FILE *f = fopen(ap("assets/music.pcm"), "rb");
  (void)args; (void)argp;
  if (!f) { musicAvail = 0; return 0; }
  musicAvail = 1;
  while (musicRun) {
    unsigned used;
    if (musicRestartReq) { fseek(f, 0, SEEK_SET); mwr = 0; mrdI = 0; mrdF = 0; musicPrimed = 0; musicRestartReq = 0; }
    used = mwr - mrdI;
    if (RING - used >= 4096) {
      int n = (int)fread(tmp, 4, 4096, f), i;
      if (n <= 0) { fseek(f, 0, SEEK_SET); continue; }
      for (i = 0; i < n; i++) { unsigned p = (mwr + i) % RING; musicRing[p * 2] = tmp[i * 2]; musicRing[p * 2 + 1] = tmp[i * 2 + 1]; }
      mwr += n;
      if (mwr >= 8192) musicPrimed = 1;
    } else sceKernelDelayThread(8000);
  }
  fclose(f);
  return 0;
}
static int loadSfx(void) {
  FILE *f = fopen(ap("assets/sfx.bin"), "rb");
  struct { char name[16]; unsigned off, len; } ent[64];
  char magic[4]; unsigned count, total = 0, i; int n;
  if (!f) return -1;
  if (fread(magic, 1, 4, f) != 4 || fread(&count, 4, 1, f) != 1 || count > 64) { fclose(f); return -2; }
  for (i = 0; i < count; i++) if (fread(&ent[i], 24, 1, f) != 1) { fclose(f); return -3; }
  for (i = 0; i < count; i++) total += ent[i].len;
  if (total > POOL_MAX) { fclose(f); return -4; }
  n = (int)fread(sfxPool, 2, total, f); fclose(f);
  if ((unsigned)n != total) return -5;
  for (i = 0; i < SFX_COUNT; i++) { sfxOff[i] = 0; sfxLen[i] = 0; }
  for (i = 0; i < count; i++) { int id = sfxId(ent[i].name); if (id >= 0) { sfxOff[id] = ent[i].off; sfxLen[id] = ent[i].len; } }
  sfxLoaded = 1;
  return (int)count;
}
static void startLoops(void) {
  static const char *nm[4] = { "loop_smear", "loop_grit", "loop_wet", "loop_desc" };
  int i;
  for (i = 0; i < 4; i++) {
    Voice *v = &vox[LOOP_SMEAR + i]; int id = sfxId(nm[i]);
    if (id < 0 || sfxLen[id] <= 0) continue;
    v->p = sfxPool + sfxOff[id]; v->len = sfxLen[id]; v->pos = 0; v->rate = 1; v->gain = 0; v->target = 0; v->tc = 0.03f; v->loop = 1; v->tag = 99; v->active = 1;
  }
}

/* --- game-level sound hooks (ports of VisceralAudio.play*) --- */
static float gTime = 0, lastCrunchAt = -10, lastGrindTickAt = -10, lastCrackleAt = -10;
static void sfxThud(float intensity, int fp) {
  int heavy = fp || intensity >= 0.68f;
  sfxPlay(sfxVar(heavy ? "thud_h" : "thud_n", 2), 1.0f, 1.0f, 1);
}
static void sfxCrunch(float intensity, int fp, int kills) {
  float power = clampf(intensity * 0.80f, 0.70f, 1.55f);
  if (gTime - lastCrunchAt < 0.14f) return;
  lastCrunchAt = gTime;
  sfxPlay(sfxVar(fp ? "crunch_c" : ((kills >= 3 || power > 1.2f) ? "crunch_b" : "crunch_a"), 3), 0.9f, 1.0f, 2);
}
static void sfxBoneBreak(int count) { sfxPlay(sfxVar("bone", count >= 4 ? 3 : (count >= 2 ? 2 : 1)), 0.9f, 1.0f, 2); }
static void sfxSmearRumble(void) { sfxPlay(sfxVar("rumble", 2), 0.9f, 1.0f, 3); }
static void audioSmearUpdate(float absVx, float gooAmount, int isGrounded, int grindingPinned, float heartPower, int onBlackGoo) {
  if (!sfxLoaded) return;
  if (!muted && isGrounded && absVx > 10) {
    float sn = clampf((absVx - 10) / 360.0f, 0, 1), hpBoost = 1 + heartPower * 0.75f, wetEff, wetNorm, grindGap;
    loopSet(LOOP_SMEAR, 0.55f * (0.36f + sn * 0.48f + (grindingPinned ? 0.22f : 0)) * (1 + heartPower * 0.40f), (200 + sn * 270 + heartPower * 70) / 260.0f);
    loopSet(LOOP_GRIT, 0.50f * (0.04f + sn * 0.14f + (grindingPinned ? 0.09f : 0)) * hpBoost * 2.2f, (920 + sinf(gTime * 17) * 220 + sn * 650) / 1180.0f);
    wetEff = maxf(gooAmount, grindingPinned ? (0.62f + heartPower * 0.38f) : heartPower * 0.35f);
    if (wetEff > 0.12f) {
      wetNorm = clampf((wetEff - 0.12f) / 0.88f, 0, 1);
      loopSet(LOOP_WET, 0.5f * wetNorm * (0.15f + sn * 0.30f) * hpBoost * 2.0f, (320 + sinf(gTime * 14) * 190 + sn * 380 + heartPower * 160) / 460.0f);
    } else loopSet(LOOP_WET, 0, 1);
    grindGap = grindingPinned ? lerpf(0.24f, 0.14f, heartPower) : lerpf(0.26f, 0.15f, heartPower);
    if ((grindingPinned || onBlackGoo || gooAmount > 0.04f) && gTime - lastGrindTickAt > rnd(grindGap * 0.82f, grindGap * 1.18f)) {
      lastGrindTickAt = gTime;
      if (grindingPinned) { if (gTime - lastCrackleAt >= 0.30f) { lastCrackleAt = gTime; sfxPlay(sfxVar("grit", 2), 0.8f, 1.0f, 4); } }
      else sfxPlay(sfxVar("rub", 3), 0.75f, 1.0f, 4);
    }
  } else {
    loopSet(LOOP_SMEAR, 0, 1); loopSet(LOOP_GRIT, 0, 1); loopSet(LOOP_WET, 0, 1);
    if (!isGrounded) sfxFadeTag(4, 0.02f); else sfxFadeTag(4, 0.04f);
  }
}
static void audioDescent(float vy, int grounded) {
  if (!sfxLoaded) return;
  if (!muted && !grounded && vy > 75) { float n = clampf((vy - 75) / 680.0f, 0, 1); loopSet(LOOP_DESC, 0.5f * (0.08f + n * 0.42f), (95 + n * 175) / 120.0f); }
  else loopSet(LOOP_DESC, 0, 1);
}
static void audioStopCrunches(void) { sfxFadeTag(2, 0.02f); sfxFadeTag(4, 0.02f); }
static void musicSet(int want) {
  if (want && !musicWant) { musicRestartReq = 1; }
  musicWant = want; musicTarget = (want && !muted) ? MUSIC_VOL : 0.0f;
}
/* ---------- HUD logic (exact) ---------- */
typedef struct {
  float elapsed; int totalKills, comboKills; float comboTimer; int highestCombo, steps;
  int seriesScore, displayScore; float seriesFade, crushHoldAcc, hypeLevel;
  char bLabel[8][32]; int bCount[8]; float bFlash[8]; int nb; float feedAlpha, feedHold;
} Hud;
static Hud hud;
static const float COMBO_DUR = 2.75f;
typedef struct { int count; int mode; int isDry, isSmearing, screamsCut, diedAlone, wipeout, trashCrushed, pinnedNow, grindingNow; } KillRes;
enum { MODE_PIN, MODE_SOFT, MODE_SLOW_SMEAR, MODE_FULL };

static void hudAddBonus(const char *label, int amount) {
  int i;
  if (amount <= 0) return;
  for (i = 0; i < hud.nb; i++) if (!strcmp(hud.bLabel[i], label)) { hud.bCount[i] += amount; hud.bFlash[i] = 1; goto done; }
  if (hud.nb < 8) { strncpy(hud.bLabel[hud.nb], label, 31); hud.bCount[hud.nb] = amount; hud.bFlash[hud.nb] = 1; hud.nb++; }
done:
  hud.feedAlpha = 1; hud.feedHold = 2.6f;
}
static void hudSetBonus(const char *label, int value) {
  int i;
  if (value <= 0) return;
  for (i = 0; i < hud.nb; i++) if (!strcmp(hud.bLabel[i], label)) { hud.bCount[i] = value; hud.bFlash[i] = 1; goto done; }
  if (hud.nb < 8) { strncpy(hud.bLabel[hud.nb], label, 31); hud.bCount[hud.nb] = value; hud.bFlash[hud.nb] = 1; hud.nb++; }
done:
  hud.feedAlpha = 1; hud.feedHold = 2.6f;
}
static int hudMult(void) {
  if (hud.comboKills >= 420) return 4;
  if (hud.comboKills >= 260) return 3;
  if (hud.comboKills >= 140) return 2;
  if (hud.comboKills >= 55) return 1;
  return 0;
}
static void hudRegister(const KillRes *r) {
  int mult, base;
  if (r->trashCrushed > 0) { hudAddBonus("TRASH TAKEN OUT", r->trashCrushed); hud.seriesScore += r->trashCrushed * 12; hud.seriesFade = 2.8f; }
  if (r->count <= 0) return;
  hud.comboKills += r->count; hud.comboTimer = COMBO_DUR;
  hud.hypeLevel = minf(5, hud.hypeLevel + r->count * (r->isSmearing ? 0.014f : 0.010f) + (r->wipeout ? 0.06f : 0));
  hud.feedHold = 2.6f; hud.feedAlpha = 1; hud.seriesFade = 3.0f;
  mult = hudMult();
  base = (r->mode == MODE_FULL) ? 11 : 7;
  hud.seriesScore += (int)floorf(r->count * (base + mult * 2.2f) + (r->wipeout ? 45 : 0) + r->screamsCut * 8 + 0.5f);
  if (r->wipeout) hudAddBonus("WIPEOUTS", 1);
  if (r->screamsCut > 0) hudAddBonus("SCREAMS CUT SHORT", r->screamsCut);
  if (r->mode == MODE_FULL) hudAddBonus("FULL PRESSURE KILLS", r->count);
  if (r->mode == MODE_SOFT) hudAddBonus("SOFT KILLS", r->count);
  if (r->isDry) hudAddBonus("DRY KILLS", r->count);
  if (r->diedAlone > 0) hudAddBonus("DIED ALONE", r->diedAlone);
  if (hud.comboKills > hud.highestCombo && hud.comboKills >= 5) { hud.highestCombo = hud.comboKills; hudSetBonus("HIGHEST COMBO", hud.comboKills); }
}
static void hudCrushTime(float dt, int smearing) {
  if (smearing) hud.hypeLevel = minf(5, hud.hypeLevel + dt * 0.048f);
  hud.crushHoldAcc += dt;
  if (hud.crushHoldAcc >= 1) { int s = (int)hud.crushHoldAcc; hud.crushHoldAcc -= s; hudAddBonus("SECONDS CRUSHING", s); }
}
static void hudUpdate(float dt) {
  int i;
  hud.elapsed += dt;
  if (hud.comboKills > 0) { hud.comboTimer -= dt; if (hud.comboTimer <= 0) { hud.totalKills += hud.comboKills; hud.comboKills = 0; hud.comboTimer = 0; } }
  if (hud.displayScore < hud.seriesScore) { int d = hud.seriesScore - hud.displayScore; int st = (int)ceilf(d * minf(1, dt * 16)); hud.displayScore += st < 1 ? 1 : st; }
  if (hud.comboKills == 0 && hud.hypeLevel > 0) hud.hypeLevel = maxf(0, hud.hypeLevel - dt * 0.055f);
  if (hud.comboKills == 0 && hud.seriesFade > 0) { hud.seriesFade -= dt; if (hud.seriesFade <= 0) { hud.seriesScore = 0; hud.displayScore = 0; } }
  for (i = 0; i < hud.nb; i++) hud.bFlash[i] = maxf(0, hud.bFlash[i] - dt * 4.5f);
  if (hud.comboKills == 0) {
    if (hud.feedHold > 0) hud.feedHold -= dt;
    else if (hud.feedAlpha > 0) { hud.feedAlpha = maxf(0, hud.feedAlpha - dt * 1.1f); if (hud.feedAlpha == 0) hud.nb = 0; }
  }
}

static void gooSplash(float x, float y, int n, float power, float vxBias);
struct Foot_;
static void gooLiftStrands(struct Foot_ *foot, int heavy);
/* ---------- giant foot (exact port) ---------- */
typedef struct Foot_ {
  int isBack;
  float x, y, tx, ty, vx, vy, scale, groundY, spriteScaleX, spriteScaleY, width;
  float supportRise; int prevPoseIdx; float poseBlend, poseCooldown; int poseIdx;
  float animTimer, angle, angleVel, manualTilt, smearShear, squashX, squashY;
  float landingRecoil, landingRecoilVel, microTremor;
  int isGrounded, wasGrounded, fullPressure; float pressureSink, dryness, adhesion;
  int isSlamming, peelStrandsSpawned; float peelSfxCooldown;
  float soleBlobs[24], soleDrips[24];
} Foot;
typedef struct { float toeX, toeY, heelX, heelY; } Sole;

static void footInit(Foot *f, float sx, float sy, float scale, float groundY, int isBack) {
  memset(f, 0, sizeof(*f));
  f->isBack = isBack; f->x = f->tx = sx; f->y = f->ty = sy; f->scale = scale; f->groundY = groundY;
  f->spriteScaleY = 6.28f * scale; f->spriteScaleX = 6.28f * 1.36f * scale; f->width = 480 * scale;
  f->poseIdx = f->prevPoseIdx = (sy < groundY - 30) ? 4 : 0; f->poseBlend = 1;
  f->squashX = f->squashY = 1; f->dryness = 1;
  f->isGrounded = f->wasGrounded = (sy >= groundY - 2);
}
static Sole footSole(const Foot *f) {
  float halfW = f->width * 0.48f, c = cosf(f->angle), s = sinf(f->angle);
  float cy = f->y + f->pressureSink + f->landingRecoil;
  Sole r; r.toeX = f->x - halfW * c; r.toeY = cy - halfW * s; r.heelX = f->x + halfW * c; r.heelY = cy + halfW * s;
  return r;
}
static void samplePoseSole(const Foot *f, int pi, float u, float *ox, float *oy) {
  const PoseInfo *p = &POSES[pi];
  float lx = (lerpf(p->soleMin, p->soleMax, u) - p->anchor) * f->spriteScaleX;
  float fl = clampf(u, 0, 1) * 23.0f;
  int i0 = (int)floorf(fl), i1 = i0 + 1 > 23 ? 23 : i0 + 1;
  float yU = lerpf(p->soleYs[i0], p->soleYs[i1], fl - i0);
  *ox = lx; *oy = yU * f->spriteScaleY - 4 * f->scale;
}
static void footLocalSole(const Foot *f, float u, float *ox, float *oy) {
  float x1, y1;
  samplePoseSole(f, f->poseIdx, u, &x1, &y1);
  if (f->poseBlend < 0.99f && f->prevPoseIdx != f->poseIdx) {
    float x0, y0; samplePoseSole(f, f->prevPoseIdx, u, &x0, &y0);
    x1 = lerpf(x0, x1, f->poseBlend); y1 = lerpf(y0, y1, f->poseBlend);
  }
  *ox = x1; *oy = y1;
}
static void footSolePointAt(const Foot *f, float u, float *ox, float *oy) {
  float lx, ly, c = cosf(f->angle), s = sinf(f->angle), cx, cy, ax, ay;
  footLocalSole(f, u, &ax, &ay);
  lx = ax * f->squashX + ay * f->smearShear; ly = ay * f->squashY;
  cx = f->x + f->microTremor; cy = f->y + f->pressureSink + f->landingRecoil;
  *ox = cx + lx * c - ly * s; *oy = cy + lx * s + ly * c;
}
static void footAddGoo(Foot *f, float worldX, float amount) {
  Sole s = footSole(f);
  float u = clampf((worldX - s.toeX) / maxf(1, s.heelX - s.toeX), 0, 1);
  int idx = (int)clampf(floorf(u * 24), 0, 23);
  f->dryness = maxf(0, f->dryness - amount);
  f->adhesion = minf(1, f->adhesion + amount * 2.2f);
  f->soleBlobs[idx] = minf(1, f->soleBlobs[idx] + 0.42f);
  if (idx > 0) f->soleBlobs[idx - 1] = minf(1, f->soleBlobs[idx - 1] + 0.18f);
  if (idx < 23) f->soleBlobs[idx + 1] = minf(1, f->soleBlobs[idx + 1] + 0.18f);
}
static void footSetPose(Foot *f, int next, float dt) {
  f->poseCooldown = maxf(0, f->poseCooldown - dt);
  if (next != f->poseIdx && f->poseCooldown <= 0) { f->prevPoseIdx = f->poseIdx; f->poseIdx = next; f->poseBlend = 1; f->poseCooldown = 0.075f; }
}
static void footLandingRecoil(Foot *f, float sp) {
  float s = clampf(sp, 0.3f, 1.4f);
  f->landingRecoilVel = 68 * s; f->squashX = 1 + 0.058f * s; f->squashY = 1 - 0.052f * s;
}
static void footBackSupport(Foot *f, float dt, float riseTarget) {
  int next;
  f->wasGrounded = f->isGrounded; f->animTimer += dt;
  f->supportRise = lerpf(f->supportRise, clampf(riseTarget, 0, 1), minf(1, 9.5f * dt));
  f->x = f->tx; f->y = f->groundY - f->supportRise * 44; f->vx = f->vy = 0;
  f->isGrounded = f->supportRise < 0.72f; f->fullPressure = 0;
  next = f->poseIdx;
  if (f->supportRise > 0.58f || (f->poseIdx == 7 && f->supportRise > 0.44f)) {
    next = 7; f->angle = lerpf(f->angle, -0.05f * (f->supportRise - 0.44f), minf(1, 10 * dt));
  } else if (f->supportRise > 0.18f || (f->poseIdx == 3 && f->supportRise > 0.10f)) {
    next = 3; f->angle = lerpf(f->angle, -0.018f * f->supportRise, minf(1, 10 * dt));
  } else { next = 0; f->angle = lerpf(f->angle, 0, minf(1, 9 * dt)); }
  footSetPose(f, next, dt);
  f->squashY = lerpf(f->squashY, 1 + f->supportRise * 0.028f, minf(1, 9 * dt));
  f->squashX = lerpf(f->squashX, 1 - f->supportRise * 0.020f, minf(1, 9 * dt));
  f->pressureSink = (1 - f->supportRise) * 2.5f; f->microTremor = 0;
}
static void footUpdate(Foot *f, float dt, int activeDrag, float time) {
  float stiffX = activeDrag ? 18.0f : 11.0f, stiffY, prevX = f->x, prevY = f->y, curH, stickyStr, hover, effStX, heightAbove, targetAngle, angAcc, targetShear, recAcc, targetSink, frictionDry;
  int pullingUp, peeling = 0, next, i;
  f->wasGrounded = f->isGrounded; f->animTimer += dt;
  stiffY = f->isSlamming ? 42.0f : (activeDrag ? 13.5f : 10.5f);
  f->tx = clampf(f->tx, 185, CW - 185); f->ty = clampf(f->ty, 150, f->groundY);
  curH = maxf(0, f->groundY - f->y);
  pullingUp = f->ty < f->y - 8;
  f->peelSfxCooldown = maxf(0, f->peelSfxCooldown - dt);
  stickyStr = maxf(f->adhesion, (1 - f->dryness) * 0.95f);
  if (pullingUp && stickyStr > 0.12f && curH < 165) {
    float snap = clampf(curH / 165, 0, 1);
    peeling = 1;
    stiffY = lerpf(1.25f, 4.4f, snap) * lerpf(1.0f, 0.48f, stickyStr);
    if (!f->peelStrandsSpawned && curH > 2.5f) { f->peelStrandsSpawned = 1; f->peelSfxCooldown = 0.34f; gooLiftStrands(f, stickyStr > 0.55f); }
    f->adhesion = maxf(0, f->adhesion - dt * (0.28f + snap * 0.55f));
  } else if (curH >= 165) f->peelStrandsSpawned = 0;
  else if (f->isGrounded) f->peelStrandsSpawned = 0;
  hover = (!f->isGrounded && !activeDrag && !peeling) ? sinf(time * 2.4f + (f->isBack ? 1.6f : 0)) * 5 : 0;
  effStX = peeling ? stiffX * 0.42f : stiffX;
  f->x += (f->tx - f->x) * minf(1, effStX * dt * (1 + 0.65f * gPsycho));
  f->y += (f->ty + hover - f->y) * minf(1, stiffY * dt * (1 + 0.65f * gPsycho));
  f->vx = (f->x - prevX) / maxf(0.001f, dt); f->vy = (f->y - prevY) / maxf(0.001f, dt);
  f->isGrounded = f->y >= f->groundY - 5;
  if (f->isGrounded) f->isSlamming = 0;
  heightAbove = maxf(0, f->groundY - f->y);

  targetAngle = f->manualTilt * 0.45f; next = f->poseIdx;
  if (!f->isGrounded) {
    if (f->vy < -220) { f->squashY = lerpf(f->squashY, 1.028f, minf(1, 10 * dt)); f->squashX = lerpf(f->squashX, 0.978f, minf(1, 10 * dt)); }
    if (f->manualTilt < -0.14f) { next = f->manualTilt < -0.24f ? 7 : 3; targetAngle += clampf(f->vx / 2800, -0.06f, 0.06f); }
    else if (heightAbove > 118 || (f->poseIdx == 4 && heightAbove > 92)) { next = 4; targetAngle += clampf(f->vx / 2200, -0.08f, 0.10f) + clampf(-f->vy / 2200, -0.06f, 0.06f); }
    else if (heightAbove > 22 || (f->poseIdx == 8 && heightAbove > 12)) { next = 8; targetAngle += clampf(f->vx / 2400, -0.07f, 0.08f); }
    else { next = 0; targetAngle += clampf(f->vx / 3200, -0.03f, 0.03f); }
  } else {
    float avx = fabsf(f->vx);
    if (f->manualTilt < -0.14f) next = f->manualTilt < -0.24f ? 7 : 3;
    else if (f->manualTilt > 0.16f) next = 8;
    else if (f->fullPressure) { next = 6; targetAngle = 0; }
    else if (avx > 18) { next = 5; targetAngle = clampf(f->vx / 2800, -0.042f, 0.042f); }
    else { next = 0; targetAngle = 0; }
  }
  footSetPose(f, next, dt);
  angAcc = (targetAngle - f->angle) * 58 - f->angleVel * 15.2f;
  f->angleVel += angAcc * dt; f->angle += f->angleVel * dt;
  targetShear = f->isGrounded ? clampf(f->vx / 620, -0.048f, 0.048f) : 0;
  f->smearShear = lerpf(f->smearShear, targetShear, minf(1, 11 * dt));
  f->squashX = lerpf(f->squashX, 1, minf(1, 10 * dt)); f->squashY = lerpf(f->squashY, 1, minf(1, 10 * dt));
  recAcc = -f->landingRecoil * 105 - f->landingRecoilVel * 19.5f;
  f->landingRecoilVel += recAcc * dt; f->landingRecoil += f->landingRecoilVel * dt;
  targetSink = (f->isGrounded && f->fullPressure) ? 10.0f : (f->isGrounded ? 1.5f : 0);
  f->pressureSink = lerpf(f->pressureSink, targetSink, minf(1, 13 * dt));
  f->microTremor = peeling ? sinf(time * 18) * 0.45f * stickyStr : 0;
  frictionDry = f->isGrounded ? fabsf(f->vx) * 0.00007f : 0.007f;
  f->dryness = minf(1, f->dryness + (0.0035f + frictionDry) * dt);
  for (i = 0; i < 24; i++) {
    f->soleBlobs[i] = maxf(0, f->soleBlobs[i] - (0.008f + frictionDry * 0.45f) * dt);
    if (!f->isGrounded && f->soleBlobs[i] > 0.22f) {
      f->soleDrips[i] += dt * (8 + f->soleBlobs[i] * 18);
      if (f->soleDrips[i] > 40 + f->soleBlobs[i] * 36) {
        float px, py; footSolePointAt(f, i / 23.0f, &px, &py);
        gooSplash(px, py + f->soleDrips[i] * 0.72f, 1, 0.14f, 0);
        f->soleDrips[i] *= 0.36f;
      }
    } else f->soleDrips[i] = lerpf(f->soleDrips[i], 0, minf(1, 10 * dt));
  }
}


/* ---------- textured quads ---------- */
static void quadVT(VT *v, const float *xs, const float *ys, float u0, float v0, float u1, float v1, unsigned int c) {
  static const int idx[6] = {0, 1, 2, 0, 2, 3};
  float us[4], vs[4]; int i;
  us[0] = u0; vs[0] = v0; us[1] = u1; vs[1] = v0; us[2] = u1; vs[2] = v1; us[3] = u0; vs[3] = v1;
  for (i = 0; i < 6; i++) { int k = idx[i]; v[i].u = us[k]; v[i].v = vs[k]; v[i].c = c; v[i].x = xs[k]; v[i].y = ys[k]; v[i].z = 0; }
}
static void texBind(const void *tex, int tw, int th, int linear, int psm, int swz) {
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexMode(psm, 0, 0, swz);
  sceGuTexImage(0, tw, th, tw, tex);
  sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
  sceGuTexFilter(linear ? GU_LINEAR : GU_NEAREST, linear ? GU_LINEAR : GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
}
static void texDraw(VT *v) {
  sceKernelDcacheWritebackRange(v, 6 * sizeof(VT));
  sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 6, 0, v);
}
static void footDrawEx(const Foot *f, int slot, int ch, float alpha) {
  const PoseInfo *p = &charPoses[ch][f->poseIdx];
  float sx = f->spriteScaleX, sy = f->spriteScaleY;
  float pW = p->wu * sx, pH = p->hu * sy, pLeft = -p->anchor * sx, pTop = -p->botY * sy;
  float extY = pTop + 3.0f * sy, extH = 620.0f, sh = p->slope * (sx / sy);
  float cx = f->x + f->microTremor, cy = f->y + f->pressureSink + f->landingRecoil;
  float c = cosf(f->angle), s = sinf(f->angle);
  float lx[4], ly[4], xs[4], ys[4];
  float kt = (float)p->hpx / p->hu;
  float rowV = 3.0f * kt + 0.5f;
  unsigned int g = f->isBack ? 194u : 255u, tint = ((unsigned int)(clampf(alpha, 0, 1) * 255.0f) << 24) | (g << 16) | (g << 8) | g;
  int i, sl = poseSlot[f->poseIdx];
  if (sl < 0 || !bootOk[ch][sl] || alpha <= 0.01f) return;
  texBind(bootTex[ch][sl], p->tw, p->th, 1, GU_PSM_8888, 1);
#define XFORM(LX, LY, OX, OY) do { float a_ = (LX) * f->squashX, b_ = (LY) * f->squashY; a_ += b_ * f->smearShear; \
    float xr_ = a_ * c - b_ * s, yr_ = a_ * s + b_ * c; (OX) = wX(cx + xr_); (OY) = wY(cy + yr_); } while (0)
  lx[0] = pLeft - extH * sh;      ly[0] = extY - extH;
  lx[1] = pLeft + pW - extH * sh; ly[1] = extY - extH;
  lx[2] = pLeft + pW + 5 * sh;    ly[2] = extY + 5;
  lx[3] = pLeft + 5 * sh;         ly[3] = extY + 5;
  for (i = 0; i < 4; i++) XFORM(lx[i], ly[i], xs[i], ys[i]);
  quadVT(tvLeg[slot], xs, ys, 0, rowV, (float)p->wpx, rowV, tint); texDraw(tvLeg[slot]);
  lx[0] = pLeft;      ly[0] = pTop;
  lx[1] = pLeft + pW; ly[1] = pTop;
  lx[2] = pLeft + pW; ly[2] = pTop + pH;
  lx[3] = pLeft;      ly[3] = pTop + pH;
  for (i = 0; i < 4; i++) XFORM(lx[i], ly[i], xs[i], ys[i]);
  quadVT(tvBody[slot], xs, ys, 0, 0, (float)p->wpx, (float)p->hpx, tint); texDraw(tvBody[slot]);
  sceGuDisable(GU_TEXTURE_2D);
#undef XFORM
}
static void footDraw(const Foot *f, int slot) { footDrawEx(f, slot, curChar, 1.0f); }
static void footShadow(const Foot *f) {
  float h = maxf(0, f->groundY - f->y);
  float alpha = clampf((f->isBack ? 0.42f : 0.60f) - h / 680.0f, 0.12f, 0.60f);
  float spread = 1 + h / 860.0f;
  ellipseW(f->x, f->groundY + 4, f->width * 0.50f * spread, 16 * f->scale, 0, col(8, 5, 14, (int)(alpha * 255)));
}
static void footGooRim(const Foot *f) {
  int i; float liftReveal, sxs, sys;
  if (f->dryness >= 0.99f) return;
  liftReveal = clampf((f->groundY - f->y) / 72.0f, 0, 1);
  sxs = lerpf(0.45f, 0.92f, liftReveal); sys = lerpf(0.32f, 0.88f, liftReveal);
  for (i = 0; i < 24; i++) {
    float amt = f->soleBlobs[i], bx, by, rx, ry, drip;
    if (amt <= 0.04f) continue;
    footSolePointAt(f, i / 23.0f, &bx, &by);
    if (!f->isGrounded && (f->poseIdx == 4 || f->poseIdx == 8)) by += -14 * f->scale * amt * 0.45f;
    rx = (8 + amt * 18) * f->scale * sxs; ry = (5 + amt * 11) * f->scale * sys;
    ellipseW(bx, by, rx, ry, 0, RGB(5, 5, 6));
    if (i < 23 && f->soleBlobs[i + 1] > 0.08f) {
      float bx2, by2; footSolePointAt(f, (i + 1) / 23.0f, &bx2, &by2);
      lineW(bx, by, bx2, by2, (7 + (amt + f->soleBlobs[i + 1]) * 9) * f->scale * sys, RGB(5, 5, 6));
    }
    drip = f->soleDrips[i];
    if (drip > 3) {
      float dw = (3.2f + amt * 5.2f) * f->scale;
      lineW(bx, by, bx, by + drip, dw * 1.7f, RGB(5, 5, 6));
      ellipseW(bx, by + drip, dw * 0.8f, dw * 0.8f, 0, RGB(5, 5, 6));
      ellipseW(bx - dw * 0.16f, by + drip - dw * 0.15f, dw * 0.22f, dw * 0.22f, 0, col(48, 48, 50, 90));
    }
    if (amt > 0.25f) ellipseW(bx - rx * 0.22f, by - ry * 0.26f, rx * 0.36f, ry * 0.24f, -0.15f, col(44, 44, 46, 90));
  }
}
/* ---------- goo system (port of GooSystem) ---------- */
typedef struct { float ox, oy, sx, sy; } Lobe;
typedef struct { float x, y, rx, ry, spread, alpha, age; Lobe lobe[3]; } Puddle;
typedef struct { float x1, y1, x2, y2, w, alpha, age; } Smear;
typedef struct { float x, y, vx, vy, r, life; } GPart;
typedef struct { Foot *foot; float u, gx, gy, thickness, maxLen, sag; } Strand;
typedef struct { float x, y, r, maxR, alpha; } Shock;
#define MAX_PUDDLE 70
#define MAX_SMEAR 70
#define MAX_GPART 130
#define MAX_STRAND 36
#define MAX_SHOCK 8
static Puddle puddles[MAX_PUDDLE]; static int puddleN = 0;
static Smear smears[MAX_SMEAR]; static int smearN = 0;
static GPart gparts[MAX_GPART]; static int gpartN = 0;
static Strand strands[MAX_STRAND]; static int strandN = 0;
static Shock shocks[MAX_SHOCK]; static int shockN = 0;

static void gooShockwave(float x, float y, float maxR) {
  if (shockN >= MAX_SHOCK) { memmove(&shocks[0], &shocks[1], sizeof(Shock) * (MAX_SHOCK - 1)); shockN--; }
  shocks[shockN].x = x; shocks[shockN].y = y; shocks[shockN].r = 14; shocks[shockN].maxR = maxR; shocks[shockN].alpha = 0.75f; shockN++;
}
static int gooHasUnder(const Foot *f) {
  Sole s = footSole(f); int i;
  float minX = minf(s.toeX, s.heelX) - 10, maxX = maxf(s.toeX, s.heelX) + 10;
  for (i = puddleN - 1; i >= 0; i--) if (puddles[i].x >= minX && puddles[i].x <= maxX) return 1;
  for (i = smearN - 1; i >= 0; i--) if ((smears[i].x1 >= minX && smears[i].x1 <= maxX) || (smears[i].x2 >= minX && smears[i].x2 <= maxX)) return 1;
  return 0;
}
static void gooPuddle(float x, float y, float rx, float ry) {
  Puddle *p;
  if (puddleN >= MAX_PUDDLE) { memmove(&puddles[0], &puddles[1], sizeof(Puddle) * (MAX_PUDDLE - 1)); puddleN--; }
  p = &puddles[puddleN++];
  p->x = x; p->y = clampf(y, ASPHALT_TOP + 6, CURB_TOP - 4); p->rx = rx; p->ry = ry; p->spread = 0.55f; p->alpha = 0.96f; p->age = 0;
  p->lobe[0].ox = 0; p->lobe[0].oy = 0; p->lobe[0].sx = 1; p->lobe[0].sy = 1;
  p->lobe[1].ox = rnd(-rx * 0.55f, rx * 0.55f); p->lobe[1].oy = rnd(-ry * 0.3f, ry * 0.3f); p->lobe[1].sx = rnd(0.55f, 0.82f); p->lobe[1].sy = rnd(0.6f, 0.85f);
  p->lobe[2].ox = rnd(-rx * 0.65f, rx * 0.65f); p->lobe[2].oy = rnd(-ry * 0.25f, ry * 0.25f); p->lobe[2].sx = rnd(0.35f, 0.62f); p->lobe[2].sy = rnd(0.45f, 0.72f);
}
static void gooSmear(float x, float y, float vx, float thickness) {
  Smear *m; float dir = vx > 0 ? 1.0f : (vx < 0 ? -1.0f : ((rand() % 2) ? 1.0f : -1.0f));
  float len = clampf(fabsf(vx) * 0.28f + rnd(18, 42), 20, 175);
  if (smearN >= MAX_SMEAR) { memmove(&smears[0], &smears[1], sizeof(Smear) * (MAX_SMEAR - 1)); smearN--; }
  m = &smears[smearN++];
  m->x1 = x; m->y1 = clampf(y, ASPHALT_TOP + 8, CURB_TOP - 5);
  m->x2 = clampf(x + dir * len, 10, CW - 10); m->y2 = clampf(y + rnd(-2.5f, 2.5f), ASPHALT_TOP + 8, CURB_TOP - 5);
  m->w = clampf(thickness + rnd(-2, 4), 5, 19); m->alpha = 0.94f; m->age = 0;
}
static void gooSplash(float x, float y, int count, float power, float vxBias) {
  int i;
  for (i = 0; i < count; i++) {
    float ang = rnd(-3.14159f * 0.95f, -3.14159f * 0.05f), spd = rnd(65, 245) * power; GPart *p;
    if (gpartN >= MAX_GPART) { memmove(&gparts[0], &gparts[1], sizeof(GPart) * (MAX_GPART - 1)); gpartN--; }
    p = &gparts[gpartN++];
    p->x = x + rnd(-18, 18); p->y = y + rnd(-6, 4);
    p->vx = cosf(ang) * spd + vxBias * 0.30f + rnd(-42, 42); p->vy = sinf(ang) * spd - rnd(16, 75);
    p->r = rnd(3.2f, 8.2f); p->life = rnd(0.45f, 1.05f);
  }
}
static void gooLiftStrands(Foot *foot, int heavy) {
  float wet = maxf(1 - foot->dryness, foot->adhesion); int count, i;
  if (wet < 0.10f) return;
  count = (int)clampf(floorf((heavy ? 13 : 8) + wet * 14 + 0.5f), 9, 25);
  for (i = 0; i < count; i++) {
    float u = (i + rnd(0.06f, 0.94f)) / count, px, py; Strand *s;
    if (strandN >= MAX_STRAND) { memmove(&strands[0], &strands[1], sizeof(Strand) * (MAX_STRAND - 1)); strandN--; }
    footSolePointAt(foot, u, &px, &py);
    s = &strands[strandN++];
    s->foot = foot; s->u = u; s->gx = px + rnd(-15, 15); s->gy = foot->groundY + rnd(-5, 6);
    s->thickness = rnd(6.2f, 15.2f) * (0.72f + wet * 0.75f); s->maxLen = rnd(240, 445) * (0.82f + wet * 0.58f); s->sag = rnd(34, 78);
  }
}
static void gooUpdate(float dt) {
  int i, j;
  for (i = 0, j = 0; i < shockN; i++) {
    Shock s = shocks[i];
    s.r += (s.maxR - s.r) * minf(1, dt * 12) + 80 * dt; s.alpha -= dt * 2.8f;
    if (s.alpha > 0.01f) shocks[j++] = s;
  }
  shockN = j;
  for (i = 0, j = 0; i < puddleN; i++) {
    Puddle p = puddles[i];
    p.age += dt; if (p.spread < 1) p.spread = minf(1, p.spread + dt * 1.05f);
    if (p.age > 28) p.alpha -= dt * 0.08f;
    if (p.alpha > 0.02f) puddles[j++] = p;
  }
  puddleN = j;
  for (i = 0, j = 0; i < smearN; i++) {
    Smear m = smears[i]; m.age += dt; if (m.age > 24) m.alpha -= dt * 0.07f;
    if (m.alpha > 0.02f) smears[j++] = m;
  }
  smearN = j;
  for (i = 0, j = 0; i < gpartN; i++) {
    GPart p = gparts[i]; int dead = 0;
    p.vy += 980 * dt; p.vx *= 0.985f; p.x += p.vx * dt; p.y += p.vy * dt; p.life -= dt;
    if (p.y >= GROUND_Y_FRONT + rnd(-18, 12) && p.vy > 0) { if (p.r > 3.6f && rnd(0, 1) < 0.38f) gooPuddle(p.x, p.y, p.r * 2.2f, p.r * 0.65f); dead = 1; }
    else if (p.life <= 0) dead = 1;
    if (!dead) gparts[j++] = p;
  }
  gpartN = j;
  for (i = 0, j = 0; i < strandN; i++) {
    Strand s = strands[i]; float px, py, dist; int dead = 0;
    footSolePointAt(s.foot, s.u, &px, &py);
    dist = hypotf(px - s.gx, py - s.gy);
    if (dist > s.maxLen || (s.foot->isGrounded && dist < 8 && rnd(0, 1) < 0.04f)) {
      if (dist > s.maxLen) gooSplash((px + s.gx) * 0.5f, (py + s.gy) * 0.5f, 3, 0.45f, 0);
      dead = 1;
    }
    if (!dead) strands[j++] = s;
  }
  strandN = j;
}
static unsigned int ca(int r, int g, int b, float a) { return col(r, g, b, (int)(clampf(a, 0, 1) * 255)); }
static void gooDrawGround(void) {
  int i;
  for (i = 0; i < smearN; i++) {
    Smear *m = &smears[i]; float a = m->alpha;
    lineW(m->x1, m->y1, m->x2, m->y2, m->w, ca(5, 5, 6, a));
    lineW(m->x1, m->y1 - m->w * 0.18f, m->x2, m->y2 - m->w * 0.18f, maxf(1.4f, m->w * 0.24f), ca(30, 30, 34, 0.34f * a));
  }
  for (i = 0; i < puddleN; i++) {
    Puddle *p = &puddles[i]; float sp = p->spread;
    ellipseW(p->x, p->y, p->rx * sp, p->ry * sp, 0, ca(5, 5, 6, p->alpha));
    ellipseW(p->x + p->lobe[1].ox * sp, p->y + p->lobe[1].oy * sp, p->rx * p->lobe[1].sx * sp, p->ry * p->lobe[1].sy * sp, 0, ca(5, 5, 6, p->alpha));
    ellipseW(p->x - p->rx * 0.22f * sp, p->y - p->ry * 0.24f * sp, p->rx * 0.40f * sp, p->ry * 0.24f * sp, 0, ca(38, 38, 40, 0.30f * p->alpha));
  }
  for (i = 0; i < shockN; i++) {
    Shock *s = &shocks[i];
    ellipseW(s->x, s->y, s->r, s->r * 0.16f, 0, ca(155, 184, 255, s->alpha * 0.22f));
    ellipseW(s->x, s->y, s->r * 0.93f, s->r * 0.14f, 0, ca(8, 5, 14, s->alpha * 0.18f));
  }
}
static void gooDrawStrands(const Foot *foot) {
  int i;
  for (i = 0; i < strandN; i++) {
    Strand *s = &strands[i]; float px, py, dist, stretch, midX, midY, w, rootR, bt, omt, bx, by, qx = 0, qy = 0; int k;
    if (s->foot != foot) continue;
    footSolePointAt(foot, s->u, &px, &py);
    dist = hypotf(px - s->gx, py - s->gy); stretch = clampf(dist / s->maxLen, 0, 1);
    midX = (px + s->gx) * 0.5f + sinf(s->u * 14) * 5; midY = (py + s->gy) * 0.5f + s->sag * (1 - stretch * 0.65f);
    w = maxf(1.2f, s->thickness * (1 - stretch * 0.70f)); rootR = w * 1.45f;
    ellipseW(px, py, rootR * 1.15f, rootR * 0.65f, 0, RGB(5, 5, 6));
    ellipseW(s->gx, s->gy, rootR * 1.35f, rootR * 0.55f, 0, RGB(5, 5, 6));
    for (k = 0; k <= 5; k++) {
      float t = k / 5.0f, o = 1 - t, x = o * o * px + 2 * o * t * midX + t * t * s->gx, y = o * o * py + 2 * o * t * midY + t * t * s->gy;
      if (k > 0) lineW(qx, qy, x, y, w, RGB(5, 5, 6));
      qx = x; qy = y;
    }
    bt = 0.25f + stretch * 0.55f; omt = 1 - bt;
    bx = omt * omt * px + 2 * omt * bt * midX + bt * bt * s->gx; by = omt * omt * py + 2 * omt * bt * midY + bt * bt * s->gy;
    ellipseW(bx, by, w * 0.85f, w * 1.35f, 0, RGB(5, 5, 6));
    if (w > 2.0f) lineW(px - 1, py, midX - 1, midY, maxf(0.9f, w * 0.28f), col(46, 46, 48, 95));
  }
}
static void gooDrawParticles(void) {
  int i;
  for (i = 0; i < gpartN; i++) {
    GPart *p = &gparts[i]; float spd = hypotf(p->vx, p->vy), ang = atan2f(p->vy, p->vx), stretch = clampf(1 + spd / 420.0f, 1, 2.35f);
    ellipseW(p->x, p->y, p->r * stretch, p->r * 0.82f, ang, RGB(5, 5, 6));
    if (p->r > 5.5f) ellipseW(p->x + p->r * 0.2f, p->y - p->r * 0.22f, p->r * 0.42f * stretch, p->r * 0.24f, ang, col(52, 52, 55, 92));
  }
}
/* ---------- enemies (exact port) ---------- */
typedef struct {
  float x, y, vx, baseSpeed, dir, h, w, phase, crushHealth, screamTimer, screamCooldown;
  int state, isScreaming, alive; Foot *pinnedBy;
} Ant;
enum { ST_WALK, ST_PANIC, ST_PINNED };
typedef struct { float x, y; int type; float w, h; } Trash;
typedef struct { float x, y, vy; int value; float life, maxLife; } Popup;
#define MAX_ANTS 125
#define MAX_TRASH 10
#define MAX_POPUP 90
static Ant ants[MAX_ANTS]; static int antN = 0;
static Trash trash[MAX_TRASH]; static int trashN = 0;
static Popup popups[MAX_POPUP]; static int popupN = 0;
static float spawnTimer = 0, trashTimer = 2.5f;

static void spawnAnt(float xo, int useX) {
  Ant *a; int fromLeft = rand() % 2; float spd;
  if (antN >= MAX_ANTS) return;
  a = &ants[antN++]; memset(a, 0, sizeof(*a));
  a->x = useX ? xo : (fromLeft ? -16.0f : CW + 16.0f);
  a->y = rnd(ASPHALT_TOP + 14, CURB_TOP - 8);
  a->dir = useX ? ((rand() % 2) ? 1.0f : -1.0f) : (fromLeft ? 1.0f : -1.0f);
  spd = rnd(88, 144); a->vx = a->dir * spd; a->baseSpeed = spd;
  a->h = rnd(68, 86); a->w = rnd(38, 48); a->phase = rnd(0, 6.2832f);
  a->state = ST_WALK; a->crushHealth = 1; a->screamCooldown = rnd(0.6f, 3.2f); a->alive = 1;
}
static void spawnTrash(float xo, int useX) {
  Trash *t;
  if (trashN >= MAX_TRASH) return;
  t = &trash[trashN++];
  t->x = useX ? xo : rnd(80, CW - 80); t->y = rnd(ASPHALT_TOP + 12, CURB_TOP - 10);
  t->type = rndi(0, 2); t->w = rnd(16, 24); t->h = rnd(14, 22);
}
static void addPopup(float x, float y, int v) {
  Popup *p;
  if (popupN >= MAX_POPUP) { memmove(&popups[0], &popups[1], sizeof(Popup) * (MAX_POPUP - 1)); popupN = MAX_POPUP - 1; }
  p = &popups[popupN++]; p->x = x + rnd(-8, 8); p->y = y - 10; p->vy = -60 - rnd(0, 28); p->value = v; p->life = p->maxLife = 0.95f;
}
static void enemiesInit(void) {
  int i;
  antN = trashN = popupN = 0; spawnTimer = 0; trashTimer = 2.5f;
  for (i = 0; i < 58; i++) spawnAnt(rnd(40, CW - 40), 1);
  for (i = 0; i < 4; i++) spawnTrash(rnd(120, CW - 120), 1);
}
static void removeDeadAnts(void) {
  int i, j = 0;
  for (i = 0; i < antN; i++) if (ants[i].alive) ants[j++] = ants[i];
  antN = j;
}
static void enemiesUpdate(float dt, Foot *front, Foot *back, float speedMult) {
  int i, j; Foot *feet[2];
  feet[0] = front; feet[1] = back;
  spawnTimer -= dt;
  if (spawnTimer <= 0) {
    int deficit = (int)(82 + 40 * gPsycho) - antN; int batch, k; float side; int wave;
    if (deficit < 0) deficit = 0;
    batch = deficit > 35 ? rndi(6, 12) : rndi(3, 6); batch = (int)(batch * (1 + 0.8f * gPsycho));
    side = (rand() % 2) ? -16.0f : CW + 16.0f; wave = (rand() % 100) < 78;
    for (k = 0; k < batch; k++) spawnAnt(wave ? side + (side < 0 ? -k * 12.0f : k * 12.0f) : 0, wave);
    spawnTimer = (deficit > 30 ? rnd(0.18f, 0.36f) : rnd(0.36f, 0.72f)) * (1 - 0.5f * gPsycho);
  }
  trashTimer -= dt;
  if (trashTimer <= 0) { spawnTrash(0, 0); trashTimer = rnd(4.5f, 8.5f); }

  for (i = 0; i < antN; i++) {
    Ant *a = &ants[i];
    float threatX = 0, closest = 195; int hasThreat = 0, k;
    if (!a->alive) continue;
    if (a->isScreaming) { a->screamTimer -= dt; if (a->screamTimer <= 0) a->isScreaming = 0; }
    else a->screamCooldown -= dt;

    if (a->state == ST_PINNED && a->pinnedBy) {
      Foot *f = a->pinnedBy; Sole s = footSole(f);
      float minX = minf(s.toeX, s.heelX) - 8, maxX = maxf(s.toeX, s.heelX) + 8;
      if (!f->isGrounded) { a->state = ST_PANIC; a->pinnedBy = NULL; }
      else {
        a->x = clampf(a->x + f->vx * dt * 0.82f + sinf(a->phase) * 16 * dt, minX + 16, maxX - 16);
        a->phase += dt * (28 + fabsf(f->vx) * 0.12f);
        continue;
      }
    }
    for (k = 0; k < 2; k++) {
      float absDx = fabsf(a->x - feet[k]->x), above = feet[k]->groundY - feet[k]->y;
      if (absDx < closest && above < 400) { closest = absDx; threatX = feet[k]->x; hasThreat = 1; }
    }
    if (hasThreat) {
      float flee = a->x >= threatX ? 1.0f : -1.0f;
      a->state = ST_PANIC; a->dir = flee;
      a->vx = lerpf(a->vx, flee * a->baseSpeed * 2.35f * speedMult, minf(1, 9 * dt));
      a->phase += dt * 22 * sqrtf(speedMult);
    } else {
      a->state = ST_WALK;
      a->vx = lerpf(a->vx, a->dir * a->baseSpeed * speedMult, minf(1, 4 * dt));
      a->phase += dt * 11 * sqrtf(speedMult);
    }
    a->x += a->vx * dt;
    if (a->x < -60 || a->x > CW + 60) a->alive = 0;
  }
  removeDeadAnts();
  for (i = 0, j = 0; i < popupN; i++) {
    Popup p = popups[i];
    p.y += p.vy * dt; p.vy *= 0.94f; p.life -= dt;
    if (p.life > 0) popups[j++] = p;
  }
  popupN = j;
}
static void killAnt(Ant *a, KillRes *res) {
  int i, neighbor = 0;
  for (i = 0; i < antN; i++) {
    Ant *o = &ants[i];
    if (o != a && o->alive && hypotf(o->x - a->x, o->y - a->y) < 90) { neighbor = 1; break; }
  }
  if (!neighbor) res->diedAlone++;
  if (a->isScreaming) { res->screamsCut++; a->isScreaming = 0; }
  a->alive = 0;
}
static void evaluateContact(Foot *foot, int mode, int comboMult, float dt, float heartPower, void (*onResult)(const KillRes *)) {
  Sole s = footSole(foot);
  float reach = mode == MODE_PIN ? 16.0f : 6.0f;
  float minX = minf(s.toeX, s.heelX) - reach, maxX = maxf(s.toeX, s.heelX) + reach;
  float yMin = ASPHALT_TOP - 4, yMax = CURB_TOP + 8;
  KillRes res; int i, nKilled = 0; float cx = 0, cy = 0;
  float smearStrength = 1 + heartPower * 0.95f;
  float grindRate = (0.48f + fabsf(foot->vx) * 0.026f + fabsf(foot->angleVel) * 1.8f) * smearStrength * dt * (1 + 0.9f * gPsycho);
  static float kx[MAX_ANTS], ky[MAX_ANTS];
  memset(&res, 0, sizeof(res));

  for (i = trashN - 1; i >= 0; i--) {
    Trash *t = &trash[i];
    if (t->x >= minX && t->x <= maxX && t->y >= yMin - 12 && t->y <= yMax + 12) {
      res.trashCrushed++; gooSplash(t->x, t->y, 4, 0.55f, foot->vx);
      trash[i] = trash[--trashN];
    }
  }
  for (i = 0; i < antN; i++) {
    Ant *a = &ants[i];
    if (!a->alive) continue;
    if (a->x >= minX && a->x <= maxX && a->y >= yMin && a->y <= yMax) {
      if (mode == MODE_PIN) { killAnt(a, &res); kx[nKilled] = a->x; ky[nKilled++] = a->y; }
      else if (mode == MODE_SLOW_SMEAR) {
        if (a->state != ST_PINNED) { a->state = ST_PINNED; a->pinnedBy = foot; }
        res.pinnedNow++; res.grindingNow++;
        a->crushHealth -= maxf(0.01f, grindRate * rnd(0.75f, 1.35f));
        if (rnd(0, 1) < 0.36f + heartPower * 0.38f) {
          float thick = 1 + heartPower * 0.85f;
          gooSmear(a->x, a->y + 3, foot->vx * (1.25f + heartPower * 0.45f), rnd(7, 14) * thick);
          footAddGoo(foot, a->x, 0.016f * thick);
          if (heartPower > 0.35f && rnd(0, 1) < heartPower * 0.45f) gooSplash(a->x, a->y, (int)floorf(2 + heartPower * 3 + 0.5f), 0.65f + heartPower * 0.45f, foot->vx);
        }
        if (a->crushHealth <= 0) { killAnt(a, &res); kx[nKilled] = a->x; ky[nKilled++] = a->y; }
      } else { killAnt(a, &res); kx[nKilled] = a->x; ky[nKilled++] = a->y; }
    }
  }
  if (nKilled > 0) {
    int isDry = foot->dryness > 0.7f, isSmearing = (mode == MODE_SLOW_SMEAR) || fabsf(foot->vx) > 35;
    for (i = 0; i < nKilled; i++) {
      float hpS = 1 + heartPower * 0.85f; int spCnt, pts, basePts;
      cx += kx[i]; cy += ky[i];
      footAddGoo(foot, kx[i], 0.035f);
      if (isSmearing) gooSmear(kx[i], ky[i] + 3, (foot->vx != 0 ? foot->vx : foot->angleVel * 180) * (1 + heartPower * 0.35f), rnd(10, 17) * hpS);
      else gooPuddle(kx[i], ky[i] + 4, rnd(14, 26) * (1 + heartPower * 0.35f), rnd(4.5f, 8.5f) * (1 + heartPower * 0.35f));
      spCnt = (int)floorf((mode == MODE_FULL ? 8 : 5) * (1 + heartPower * 0.65f) + 0.5f);
      gooSplash(kx[i], ky[i], spCnt, (mode == MODE_FULL ? 1.18f : 0.85f) * (1 + heartPower * 0.35f), foot->vx);
      basePts = mode == MODE_FULL ? 11 : 6;
      pts = (int)floorf(basePts + comboMult * 2.2f + (isDry ? 2 : 0) + 0.5f);
      if (pts < 6) pts = 6; if (pts > 15) pts = 15;
      addPopup(kx[i], ky[i] - 6, pts);
    }
    cx /= nKilled; cy /= nKilled;
    res.wipeout = 0;
    if (nKilled >= 5) {
      int near = 0;
      for (i = 0; i < antN; i++) if (ants[i].alive && hypotf(ants[i].x - cx, ants[i].y - cy) < 80) { near = 1; break; }
      if (!near) res.wipeout = 1;
    }
    res.count = nKilled; res.isDry = isDry; res.isSmearing = isSmearing;
    res.mode = (mode == MODE_SLOW_SMEAR || mode == MODE_PIN) ? MODE_SOFT : mode;
    onResult(&res);
  } else if (res.trashCrushed > 0 || res.pinnedNow > 0 || res.grindingNow > 0) {
    res.count = 0; res.mode = mode; res.isDry = foot->dryness > 0.7f; res.isSmearing = res.grindingNow > 0;
    onResult(&res);
  }
  removeDeadAnts();
}

static void drawAnts(void) {
  int i;
  /* simple insertion sort by y (stable enough for drawing) */
  for (i = 1; i < antN; i++) { Ant t = ants[i]; int j = i - 1; while (j >= 0 && ants[j].y > t.y) { ants[j + 1] = ants[j]; j--; } ants[j + 1] = t; }
  for (i = 0; i < antN; i++) { Ant *a = &ants[i]; if (a->alive) ellipseW(a->x, a->y, a->w * (a->state == ST_PINNED ? (1.15f + (1 - a->crushHealth) * 0.55f) : 1.0f) * 0.55f, 4.5f, 0, col(0, 0, 0, 122)); }
  for (i = 0; i < antN; i++) {
    Ant *a = &ants[i];
    float x = a->x, y = a->y, squash, h, w, bob, legH, bodyH, r, top, legSwing, armY, eyeOff, ey;
    unsigned int blk = RGB(5, 5, 6);
    if (!a->alive) continue;
    squash = a->state == ST_PINNED ? lerpf(0.28f, 0.58f, a->crushHealth) : 1.0f;
    h = a->h * squash; w = a->w * (a->state == ST_PINNED ? (1.15f + (1 - a->crushHealth) * 0.55f) : 1.0f);
    bob = a->state == ST_PINNED ? 0 : fabsf(sinf(a->phase)) * (a->state == ST_PANIC ? 3.5f : 1.8f);
    legH = a->state == ST_PINNED ? 0 : floorf(h * 0.17f);
    bodyH = maxf(8, h - legH);
    r = minf(14, w * 0.36f);
    top = y - h - bob;
    rectW(x - w * 0.48f + r * 0.55f, top, w * 0.96f - r * 1.1f, r, blk);          /* rounded top */
    rectW(x - w * 0.48f, top + r * 0.5f, w * 0.96f, bodyH - r * 0.5f, blk);
    if (a->state != ST_PINNED) {
      legSwing = sinf(a->phase) * 9;
      rectW(x - w * 0.28f + legSwing * 0.5f, y - legH - 2, w * 0.18f, legH + 2, blk);
      rectW(x + w * 0.10f - legSwing * 0.5f, y - legH - 2, w * 0.18f, legH + 2, blk);
    }
    if (a->state == ST_PANIC || a->state == ST_PINNED) {
      armY = top + minf(16, bodyH * 0.32f) + sinf(a->phase * 1.5f) * 9;
      rectW(x - w * 0.48f - 9, armY, 9, 4.5f, blk); rectW(x + w * 0.48f, armY, 9, 4.5f, blk);
    }
    eyeOff = a->dir > 0 ? 5.0f : -5.0f; ey = floorf(top + minf(16, bodyH * 0.25f));
    rectW(x - 10 + eyeOff, ey, 5.8f, 5.8f, RGB(255, 255, 255));
    rectW(x + 4 + eyeOff, ey, 5.8f, 5.8f, RGB(255, 255, 255));
    if (a->isScreaming) rectW(x - 2.5f + eyeOff, ey + 9, 5, 5.5f, RGB(255, 255, 255));
  }
}
static void drawTrash(void) {
  int i;
  for (i = 0; i < trashN; i++) {
    Trash *t = &trash[i]; float x = floorf(t->x - t->w * 0.5f), y = floorf(t->y - t->h);
    if (t->type == 0) { rectW(x, y + 4, t->w, t->h - 4, RGB(24, 24, 34)); rectW(x + 3, y + 6, 5, 5, RGB(56, 56, 74)); rectW(x + 5, y, t->w - 10, 5, RGB(16, 16, 24)); }
    else if (t->type == 1) { rectW(x - 1, y + 1, t->w + 2, t->h, RGB(28, 20, 20)); rectW(x, y + 2, t->w, t->h - 2, RGB(92, 68, 56)); rectW(x + 2, y + 4, t->w - 4, 4, RGB(122, 91, 75)); }
    else { rectW(x + 4, y + 6, 8, 11, RGB(88, 106, 128)); rectW(x + 5, y + 8, 3, 7, RGB(155, 180, 208)); }
  }
}

/* ---------- city, popups, portrait, HUD (screen space) ---------- */
typedef struct { float x, y, vx, vy, r, phase; } Mote;
static Mote motes[36];
static void motesInit(void) {
  int i;
  for (i = 0; i < 36; i++) { motes[i].x = rnd(0, CW); motes[i].y = rnd(40, ASPHALT_TOP - 20); motes[i].vx = rnd(-14, -4); motes[i].vy = rnd(-6, 6); motes[i].r = rnd(1.0f, 2.4f); motes[i].phase = rnd(0, 6.2832f); }
}
static void motesUpdate(float dt) {
  int i;
  for (i = 0; i < 36; i++) {
    Mote *m = &motes[i];
    m->x += m->vx * dt; m->y += (m->vy + sinf(m->phase) * 5) * dt; m->phase += dt * 1.6f;
    if (m->x < -10) m->x = CW + 10;
    if (m->y < 30) m->y = ASPHALT_TOP - 30;
    if (m->y > ASPHALT_TOP - 15) m->y = 40;
  }
}
/* textured backdrop: dithered RGB565 copy of the HTML city, kept in VRAM in two pieces (512x256 + 512x64) */
static void drawCityTex(void) {
  float x0 = -16 + shakeX * K, y0 = -8 + shakeY * K, xs[4], ys[4];
  if (!cityOk) return;
  xs[0] = x0; ys[0] = y0; xs[1] = x0 + 512; ys[1] = y0; xs[2] = x0 + 512; ys[2] = y0 + 256; xs[3] = x0; ys[3] = y0 + 256;
  texBind(VRAM_GE(VRAM_CITY_TOP), 512, 256, 0, GU_PSM_5650, 1);
  quadVT(tvCity, xs, ys, 0, 0, 512, 256, 0xffffffffu); texDraw(tvCity);
  ys[0] = y0 + 256; ys[1] = y0 + 256; ys[2] = y0 + 288; ys[3] = y0 + 288;
  texBind(VRAM_GE(VRAM_CITY_BOT), 512, 64, 0, GU_PSM_5650, 1);
  quadVT(tvCity, xs, ys, 0, 0, 512, 32, 0xffffffffu); texDraw(tvCity);
  sceGuDisable(GU_TEXTURE_2D);
}
static void glowW(float cx, float cy, float r, int R, int G, int B, float a) {
  int i;
  for (i = 0; i < 2; i++) ellipseW(cx, cy, r * (1 - i * 0.45f), r * (1 - i * 0.45f), 0, ca(R, G, B, a * 0.55f));
}
static void drawCityAnim(float time) {
  float flicker = (sinf(time * 23) > 0.96f) ? 0.03f : 1.0f, p = (0.09f + sinf(time * 2.8f) * 0.035f) * flicker;
  int i;
  glowW(308, 380, 170, 40, 200, 170, p);
  glowW(32, 280, 125, 255, 110, 175, p * 1.08f);
  glowW(95, 546, 72, 255, 220, 90, 0.11f + sinf(time * 4.2f) * 0.04f);
  for (i = 0; i < 36; i += 2) ellipseW(motes[i].x, motes[i].y, motes[i].r, motes[i].r, 0, ca(235, 195, 225, 0.18f + sinf(motes[i].phase) * 0.10f));
}

/* ---------- portrait renderer (exact port) ---------- */
typedef struct { float happyTimer, hitBounce, frameTimer, blushLevel, joyLevel; } Portrait;
static Portrait portrait;
static void portraitTrigger(float strength) {
  portrait.happyTimer = maxf(portrait.happyTimer, 1.45f);
  portrait.hitBounce = minf(1.0f, portrait.hitBounce + 0.55f * strength);
}
static void portraitUpdate(float dt, int comboActive, float hype, int isSmearing, int dots) {
  float dotsNorm, heartsNorm, mood, tb, tj;
  portrait.frameTimer += dt;
  if (portrait.happyTimer > 0) portrait.happyTimer = maxf(0, portrait.happyTimer - dt);
  portrait.hitBounce = maxf(0, portrait.hitBounce - dt * 5.5f);
  if (comboActive) portrait.happyTimer = maxf(portrait.happyTimer, 0.35f);
  dotsNorm = clampf(dots / 4.0f, 0, 1); heartsNorm = clampf(hype / 5.0f, 0, 1);
  mood = clampf(maxf(dotsNorm * 0.85f, heartsNorm * 0.90f) + dotsNorm * 0.20f, 0, 1);
  tb = isSmearing ? clampf(0.18f + mood * 0.82f, 0, 1) : (portrait.happyTimer > 0 ? mood * 0.55f : mood * 0.22f);
  portrait.blushLevel = lerpf(portrait.blushLevel, tb, minf(1, (isSmearing ? 2.4f : 1.8f) * dt));
  tj = isSmearing ? clampf(0.25f + mood * 0.75f, 0, 1) : (portrait.happyTimer > 0 ? clampf(0.40f + mood * 0.50f, 0, 1) : 0);
  portrait.joyLevel = lerpf(portrait.joyLevel, tj, minf(1, 2.8f * dt));
}
#define PX 986.0f
#define PY 18.0f
#define PW 274.0f
#define PH 186.0f
typedef struct { int fever, happy, fIdx; float dx, dy, tw, th; } PortraitView;
static PortraitView pview;
static void portraitCompute(float time, int combo) {
  static const int idle[7] = {0, 2, 4, 6, 8, 10, 12}, hap[7] = {1, 3, 5, 7, 9, 11, 13};
  PortraitView *v = &pview; float breath, bob, bounceY, swayX;
  v->fever = combo >= 30 || portrait.joyLevel > 0.72f;
  v->happy = portrait.happyTimer > 0 || combo >= 3 || portrait.joyLevel > 0.32f;
  { (void)idle; (void)hap;
    v->fIdx = v->happy ? PORT_HAP[curChar][(int)(portrait.frameTimer * (v->fever ? 5.2f : 3.6f)) % PORT_NHAP[curChar]]
                       : PORT_IDLE[curChar][(int)(portrait.frameTimer * 1.4f) % PORT_NIDLE[curChar]]; }
  breath = sinf(time * 2.4f) * 2.0f;
  bob = v->happy ? fabsf(sinf(time * (v->fever ? 11 : 6.5f))) * (v->fever ? 7.0f : 3.8f) : 0;
  bounceY = breath - bob - portrait.hitBounce * 5.5f;
  swayX = v->happy ? sinf(time * (v->fever ? 10 : 5.5f)) * (v->fever ? 3.5f : 1.5f) : 0;
  v->th = PH * 0.96f; v->tw = ((float)PORT_WF[curChar][v->fIdx] / 64.0f) * v->th;
  v->dx = PX + (PW - v->tw) * 0.5f + swayX; v->dy = PY + (PH - v->th) + 8 + bounceY;
}
static void portraitBuildBg(void) {
  int i; unsigned int ctr = curChar == 0 ? (pview.fever ? RGB(60, 32, 63) : RGB(44, 35, 52)) : (curChar == 1 ? (pview.fever ? RGB(48, 46, 92) : RGB(32, 44, 70)) : (pview.fever ? RGB(66, 44, 66) : RGB(40, 40, 46)));
  float bx = (PX + 5) * K, by = (PY + 5) * K, bw = (PW - 10) * K, bh = (PH - 10) * K;
  useZoom = 0;
  rectP(PX * K, PY * K, PW * K, PH * K, RGB(26, 22, 31));
  for (i = 0; i < 6; i++) {            /* radial gradient as nested rectangles, always inside the frame */
    float t = i / 5.0f, ins = t * 0.34f; unsigned int c = RGB((int)lerpf(20, (float)(ctr & 255), t), (int)lerpf(16, (float)((ctr >> 8) & 255), t), (int)lerpf(25, (float)((ctr >> 16) & 255), t));
    rectP(bx + bw * ins, by + bh * ins * 0.9f, bw * (1 - 2 * ins), bh * (1 - 2 * ins * 0.9f), c);
  }
}
static void portraitDrawTex(void) {
  float xs[4], ys[4], x0, y0, x1, y1;
  if (!portOk[curChar][pview.fIdx]) return;
  sceGuScissor((int)((PX + 5) * K), (int)((PY + 5) * K), (int)ceilf((PX + PW - 5) * K), (int)ceilf((PY + PH - 5) * K));
  x0 = pview.dx * K; y0 = pview.dy * K; x1 = (pview.dx + pview.tw) * K; y1 = (pview.dy + pview.th) * K;
  xs[0] = x0; ys[0] = y0; xs[1] = x1; ys[1] = y0; xs[2] = x1; ys[2] = y1; xs[3] = x0; ys[3] = y1;
  texBind(portTex[curChar][pview.fIdx], 64, 64, 1, GU_PSM_8888, 1);
  quadVT(tvPort, xs, ys, 0, 0, (float)PORT_WF[curChar][pview.fIdx], 64, 0xffffffffu);
  texDraw(tvPort);
  sceGuDisable(GU_TEXTURE_2D);
  sceGuScissor(0, 0, 480, 272);
}
static void portraitBuildFg(void) {
  float b = clampf(portrait.blushLevel, 0, 1); int side, k; unsigned int edge;
  useZoom = 0;
  if (b > 0.03f) {
    float faceCx = pview.dx + pview.tw * 0.50f, cheekY = pview.dy + pview.th * 0.57f, spread = pview.tw * 0.145f, cr = pview.tw * (0.115f + b * 0.04f);
    for (side = -1; side <= 1; side += 2) {
      float cx = faceCx + side * spread;
      ellipseP(cx * K, cheekY * K, cr * 1.15f * K, cr * 0.72f * K, 0, ca(255, 82, 130, (0.08f + b * 0.28f) * 0.55f));
      ellipseP(cx * K, cheekY * K, cr * 0.80f * K, cr * 0.50f * K, 0, ca(255, 70, 118, (0.18f + b * 0.56f) * 0.55f));
      ellipseP(cx * K, cheekY * K, cr * 0.42f * K, cr * 0.26f * K, 0, ca(255, 58, 108, (0.18f + b * 0.56f) * 0.45f));
      if (b > 0.38f) for (k = -1; k <= 1; k++) {
        float lx = cx + k * 4.2f;
        quadP((lx - 2) * K, (cheekY + 3) * K, (lx + 2) * K, (cheekY - 3) * K, (lx + 2) * K + 1, (cheekY - 3) * K, (lx - 2) * K + 1, (cheekY + 3) * K, ca(255, 48, 96, (b - 0.38f) * 0.68f));
      }
    }
    if (b > 0.45f) rectP((PX + 5) * K, (PY + 5) * K, (PW - 10) * K, (PH - 10) * K, ca(255, 70, 125, (b - 0.45f) * 0.16f));
  }
  edge = pview.fever ? RGB(230, 126, 192) : RGB(150, 130, 155);
  { float x = PX * K, y = PY * K, w = PW * K, h = PH * K, t = 1.1f;
    rectP(x, y, w, t, edge); rectP(x, y + h - t, w, t, edge); rectP(x, y, t, h, edge); rectP(x + w - t, y, t, h, edge);
    x = (PX + 5) * K; y = (PY + 5) * K; w = (PW - 10) * K; h = (PH - 10) * K; t = 1.0f;
    rectP(x, y, w, t, RGB(78, 64, 88)); rectP(x, y + h - t, w, t, RGB(78, 64, 88)); rectP(x, y, t, h, RGB(78, 64, 88)); rectP(x + w - t, y, t, h, RGB(78, 64, 88)); }
}

/* ---------- HUD ---------- */
static void textShadow(float xc, float yc, const char *s, unsigned int c, float alpha) {
  float x = xc * K, y = yc * K - 3.5f; int a = (int)(clampf(alpha, 0, 1) * 255);
  textP(x + 1, y + 1, s, col(9, 6, 14, a), 1);
  textP(x, y, s, (c & 0x00ffffffu) | ((unsigned)a << 24), 1);
}
static float textWidthC(const char *s) { return (float)strlen(s) * 6.0f / K; }
static void textShadowS(float xc, float yc, const char *s, unsigned int c, float alpha) {
  float x = xc * K, y = yc * K - 2.5f; int a = (int)(clampf(alpha, 0, 1) * 255);
  textS(x + 1, y + 1, s, col(9, 6, 14, a));
  textS(x, y, s, (c & 0x00ffffffu) | ((unsigned)a << 24));
}
static float textWidthS(const char *s) { return (float)strlen(s) * 4.0f / K; }
static void drawHeartIcon(float xc, float yc, float fill) {
  static const char *shape[6] = { ".XX.XX.", "XXXXXXX", "XXXXXXX", ".XXXXX.", "..XXX..", "...X..." };
  int r, c; float x0 = xc * K - 3.5f, y0 = yc * K - 3.0f; int full = fill >= 0.99f;
  unsigned int outline = full ? RGB(255, 255, 255) : (fill > 0.05f ? RGB(255, 176, 212) : RGB(156, 136, 162));
  for (r = 0; r < 6; r++) for (c = 0; c < 7; c++) {
    int on, edge; if (shape[r][c] != 'X') continue;
    on = 1; edge = (r == 0) || (r == 5) || (c == 0) || (c == 6) || shape[r - 1][c] != 'X' || shape[r + 1 < 6 ? r + 1 : r][c] != 'X' || (c > 0 && shape[r][c - 1] != 'X') || (c < 6 && shape[r][c + 1] != 'X');
    if (edge) rectP(x0 + c, y0 + r, 1, 1, outline);
    else {
      int rowsFilled = (int)ceilf(fill * 6.0f - 0.01f); int filled = (5 - r) < rowsFilled;
      rectP(x0 + c, y0 + r, 1, 1, filled ? (full ? RGB(255, 228, 242) : RGB(255, 130, 184)) : RGB(27, 21, 34));
    }
    (void)on;
  }
  if (full) ellipseP(xc * K, yc * K, 6, 5, 0, col(255, 85, 153, 38));
}
static void drawHudScreen(float time, int hasInter, float hintT) {
  char num[64]; int i, secs; float y = 28, nextX, bootX, gx, gy, pulse;
  useZoom = 0;
  /* hourglass + time */
  { float hx = 28 * K, hy = y * K;
    rectP(hx - 3, hy - 4, 6, 1, RGB(230, 220, 235)); rectP(hx - 3, hy + 3, 6, 1, RGB(230, 220, 235));
    triP(hx - 2.5f, hy - 3, hx + 2.5f, hy - 3, hx, hy, RGB(230, 220, 235)); triP(hx - 2.5f, hy + 3, hx + 2.5f, hy + 3, hx, hy, RGB(230, 220, 235)); }
  secs = (int)hud.elapsed; snprintf(num, sizeof(num), "%02d:%02d:%02d", secs / 3600, (secs % 3600) / 60, secs % 60);
  textShadowS(48, y, num, RGB(245, 238, 248), 1);
  /* pile icon + kills */
  pulse = hud.comboKills > 0 ? 1 + sinf(time * 14) * 0.14f : 1;
  { float px_ = 160 * K, py_ = y * K; unsigned int pc = hud.comboKills > 0 ? RGB(255, 46, 77) : RGB(138, 114, 150);
    ellipseP(px_ - 5.5f * K * pulse, py_ + 3.5f * K * pulse, 5 * K * pulse, 5 * K * pulse, 0, pc);
    ellipseP(px_ + 5.5f * K * pulse, py_ + 3.5f * K * pulse, 5 * K * pulse, 5 * K * pulse, 0, pc);
    ellipseP(px_, py_ - 3.5f * K * pulse, 5.5f * K * pulse, 5.5f * K * pulse, 0, pc); }
  snprintf(num, sizeof(num), "%d", hud.totalKills); textShadowS(182, y, num, RGB(255, 255, 255), 1);
  nextX = 182 + textWidthS(num);
  if (hud.comboKills > 0) { snprintf(num, sizeof(num), "+%d", hud.comboKills); textShadowS(nextX + 6, y, num, RGB(255, 71, 120), 1); nextX += 6 + textWidthS(num); }
  bootX = maxf(262, nextX + 26);
  { float bx = bootX * K, by = y * K;
    rectP(bx - 2.5f, by - 2.5f, 7, 5.5f, RGB(9, 6, 14));
    rectP(bx + 1, by - 3, 3, 5.5f, RGB(255, 255, 255)); rectP(bx - 3, by, 7, 2.5f, RGB(255, 255, 255)); }
  snprintf(num, sizeof(num), "%d", hud.steps); textShadowS(bootX + 22, y, num, RGB(255, 255, 255), 1);
  /* bonus feed */
  if (hud.feedAlpha > 0.01f && hud.nb > 0) {
    float fy = 62;
    for (i = 0; i < hud.nb; i++) {
      snprintf(num, sizeof(num), "+%d %s", hud.bCount[i], hud.bLabel[i]);
      textShadowS(24 + (hud.bFlash[i] > 0.1f ? hud.bFlash[i] * 4 : 0), fy, num, hud.bFlash[i] > 0.2f ? RGB(255, 214, 234) : RGB(255, 255, 255), hud.feedAlpha);
      fy += 17;
    }
  }
  /* hearts + dots + rating under the portrait */
  gx = PX + 2; gy = PY + PH + 18;
  for (i = 0; i < 5; i++) drawHeartIcon(gx + 11 + i * 24, gy, clampf(hud.hypeLevel - i, 0, 1));
  for (i = 0; i < 4; i++) {
    float cx = (gx + 140 + i * 18) * K, cy = gy * K; int on = i < hudMult();
    ellipseP(cx, cy, 2.7f, 2.7f, 0, on ? RGB(255, 240, 248) : RGB(150, 130, 155));
    ellipseP(cx, cy, 1.9f, 1.9f, 0, on ? RGB(255, 126, 184) : RGB(30, 24, 38));
  }
  { int rating = hud.comboKills * 5 + hud.seriesScore / 18; float rx = PX + PW - 50; if (rating > 999) rating = 999;
    rectP(rx * K - 0.5f, (gy - 10) * K - 0.5f, 48 * K + 1, 20 * K + 1, RGB(166, 146, 171));
    rectP(rx * K, (gy - 10) * K, 48 * K, 20 * K, RGB(25, 20, 33));
    snprintf(num, sizeof(num), "%03d", rating);
    textP(rx * K + (48 * K - 18) * 0.5f, gy * K - 3.5f, num, rating > 0 ? RGB(255, 230, 242) : RGB(166, 146, 171), 1); }
  if (hud.displayScore > 0) {
    snprintf(num, sizeof(num), "+%d", hud.displayScore);
    textShadow(gx + 4, gy + 29, num, RGB(255, 255, 255), hud.comboKills > 0 ? 1 : clampf(hud.seriesFade, 0, 1));
  }
  if (!hasInter && hintT > 0)
  { int ha = (int)(clampf(hintT, 0, 1) * 255);
    textP(46, 226, "CROSS HOLD LOWER   TAP IN AIR OR CIRCLE  SLAM", col(242, 232, 247, ha), 1);
    textP(46, 238, "SELECT  CHANGE CHARACTER  TAKE OFF BOOTS", col(255, 160, 205, ha), 1); }
}
static void drawPopups(void) {
  int i; char num[16]; useZoom = 1;
  for (i = 0; i < popupN; i++) {
    Popup *p = &popups[i]; float alpha = clampf(p->life / (p->maxLife * 0.45f), 0, 1); float px = wX(p->x), py = wY(p->y); int a = (int)(alpha * 255);
    snprintf(num, sizeof(num), "+%d", p->value);
    textP(px - 6 + 1, py + 1, num, col(6, 8, 18, a), 1);
    textP(px - 6, py, num, p->value >= 11 ? col(82, 178, 255, a) : col(58, 139, 255, a), 1);
  }
}

static void drawPsychoOverlay(float time) {            /* world layer: tint flashing over the scene */
  float t = time * 3.2f, a = (0.045f + 0.035f * sinf(t * 2.1f)) * gPsycho;
  if (gPsycho < 0.02f) return;
  useZoom = 0;
  rectP(0, 0, 480, 272, ca(sinf(t) > 0 ? 255 : 150, sinf(t * 0.7f) > 0.2f ? 40 : 120, sinf(t * 1.3f) > 0 ? 190 : 255, a));
  rectP(0, 0, 480, 3, ca(255, 40, 140, 0.7f * gPsycho)); rectP(0, 269, 480, 3, ca(60, 200, 255, 0.7f * gPsycho));
}
static void drawPsychoBanner(float time) {
  if (gPsycho > 0.3f && sinf(time * 9) > -0.35f) {
    static const char *msg = "PSYCHO MODE";
    unsigned int c = sinf(time * 6) > 0 ? RGB(255, 60, 150) : RGB(90, 230, 255);
    textP(176 + 1, 5 + 1, msg, RGB(10, 5, 18), 2);
    textP(176, 5, msg, c, 2);
  }
}

static void drawSwitchFx(float flashA) {               /* white flash while the characters swap */
  if (flashA <= 0.01f) return;
  useZoom = 0;
  rectP(0, 0, 480, 272, ca(255, 255, 255, flashA));
}
static void drawSwitchBanner(float t, int ch) {
  static const char *names[NCHAR] = { "BOOTS ON!", "BOOTS OFF!", "STRIPES!" };
  const char *msg = names[ch];
  float a = clampf(t / 0.5f, 0, 1), w = (float)strlen(msg) * 12.0f;
  unsigned int c = ch == 0 ? col(160, 220, 255, (int)(a * 255)) : (ch == 1 ? col(255, 190, 120, (int)(a * 255)) : col(235, 235, 235, (int)(a * 255)));
  if (t <= 0) return;
  textP((480 - w) * 0.5f + 1, 41, msg, col(10, 5, 18, (int)(a * 255)), 2);
  textP((480 - w) * 0.5f, 40, msg, c, 2);
}
/* ---------- engine state ---------- */
typedef struct { float x, y, sx, sy, prevY, vy, downAt; int lmb, rmb; } Mouse;
static Mouse mouse;
static Foot frontFoot, backFoot;
static float time_ = 0, shakeMag = 0, pressHoldTime = 0, smearRumbleCooldown = 0;
static int hasInteracted = 0, spaceDown = 0, wasSmearingFront = 0, started = 0, paused = 0;
static float hintTimer = 3.2f;
static Foot *activeFoot;

static void addShake(float a) { shakeMag = minf(24, maxf(shakeMag, a)); }
static void handleKill(const KillRes *r) {
  float hp = clampf(hud.hypeLevel / 5.0f, 0, 1);
  if (r->count > 0) {
    int isFP = r->mode == MODE_FULL;
    float intensity = (isFP ? 1.28f : 1.08f) * ((r->isSmearing ? 1.22f : 1.05f) + hp * 0.42f);
    hudRegister(r);
    sfxCrunch(intensity, isFP, r->count);
    if (!r->isSmearing) sfxBoneBreak(r->count);
    portraitTrigger(minf(1.6f, 0.6f + r->count * 0.18f));
    addShake(minf(22, (4.0f + r->count * 0.95f + (isFP ? 5.5f : 0)) * (1 + hp * 0.35f)));
  } else if (r->trashCrushed > 0) { hudRegister(r); sfxCrunch(1.0f, 0, 1); }
}
static void resetGame(void) {
  memset(&hud, 0, sizeof(hud)); memset(&portrait, 0, sizeof(portrait));
  puddleN = smearN = gpartN = strandN = shockN = 0;
  footInit(&frontFoot, 430, 500, 1.0f, GROUND_Y_FRONT, 0);
  footInit(&backFoot, 770, GROUND_Y_BACK, 0.88f, GROUND_Y_BACK, 1);
  mouse.x = 430; mouse.y = 500; mouse.sx = 430; mouse.sy = 500; mouse.prevY = 500; mouse.vy = 0; mouse.lmb = mouse.rmb = 0;
  activeFoot = &frontFoot; pressHoldTime = 0; wasSmearingFront = 0; spaceDown = 0; hasInteracted = 0;
  time_ = 0; hintTimer = 3.2f; shakeMag = 0;
  enemiesInit(); motesInit();
  started = 1; paused = 0;
}

/* ---------- character switch ("taking off the boots") ---------- */
#define GH_T 0.55f
static Foot ghostFront, ghostBack;
static float ghostT = 0, bannerT = 0;
static int ghostChar = 0;
static void switchCharacter(void) {
  ghostFront = frontFoot; ghostBack = backFoot; ghostChar = curChar;
  curChar = (curChar + 1) % NCHAR; curPoses = charPoses[curChar];
  ghostT = GH_T; bannerT = 1.3f;
  sfxThud(1.2f, 1); addShake(10.0f); gooShockwave(frontFoot.x, frontFoot.groundY + 2, 190);
  frontFoot.landingRecoilVel = 55; frontFoot.squashX = 1.05f; frontFoot.squashY = 0.95f;
}

static void engineUpdate(float dt, const SceCtrlData *pad, unsigned int pressed, unsigned int released) {
  float frontAvx, supportRiseTarget = 0, heartPower, antSpeedMult, gooAmount;
  int activeDots, isSmearingFront, prevFP, i, grindingPinned = 0;
  time_ += dt; gTime = time_;
  gPsycho += ((musicWant ? 1.0f : 0.0f) - gPsycho) * minf(1, dt * 1.6f);
  if (gPsycho > 0.5f) shakeMag = maxf(shakeMag, 1.4f * gPsycho);
  hintTimer = maxf(0, hintTimer - dt);
  ghostT = maxf(0, ghostT - dt); bannerT = maxf(0, bannerT - dt);
  shakeMag = maxf(0, shakeMag - dt * 36);

  {
    float ax = 0, ay = 0;
    if (pad->Lx < 90 || pad->Lx > 166) ax = clampf(((float)pad->Lx - 128) / 100.0f, -1, 1);
    if (pad->Ly < 90 || pad->Ly > 166) ay = clampf(((float)pad->Ly - 128) / 100.0f, -1, 1);
    if (pad->Buttons & PSP_CTRL_LEFT) ax = -1;
    if (pad->Buttons & PSP_CTRL_RIGHT) ax = 1;
    if (pad->Buttons & PSP_CTRL_UP) ay = -1;
    if (pad->Buttons & PSP_CTRL_DOWN) ay = 1;
    if (ax != 0 || ay != 0) hasInteracted = 1;
    mouse.sx = clampf(mouse.sx + ax * 1150 * dt * (1 + 0.35f * gPsycho), 0, CW);
    mouse.sy = clampf(mouse.sy + ay * 900 * dt * (1 + 0.35f * gPsycho), 0, CH);
    mouse.x = clampf(CW * 0.5f + (mouse.sx - CW * 0.5f) / WORLD_ZOOM, 0, CW);
    mouse.y = clampf(CH + (mouse.sy - CH) / WORLD_ZOOM, 0, CH);
  }
  if (pressed & PSP_CTRL_CROSS) { mouse.lmb = 1; mouse.downAt = time_; activeFoot = &frontFoot; hasInteracted = 1; hintTimer = 0; }
  if (released & PSP_CTRL_CROSS) {
    if (time_ - mouse.downAt < 0.17f && !frontFoot.isGrounded) { frontFoot.isSlamming = 1; frontFoot.ty = frontFoot.groundY; }
    mouse.lmb = 0;
  }
  if (pressed & PSP_CTRL_CIRCLE) { mouse.rmb = 1; hasInteracted = 1; hintTimer = 0; if (!frontFoot.isGrounded) frontFoot.isSlamming = 1; }
  if (released & PSP_CTRL_CIRCLE) mouse.rmb = 0;
  if (pressed & PSP_CTRL_RTRIGGER) { spaceDown = 1; hasInteracted = 1; hintTimer = 0; if (!frontFoot.isGrounded) frontFoot.isSlamming = 1; }
  if (released & PSP_CTRL_RTRIGGER) spaceDown = 0;
  if (pad->Buttons & PSP_CTRL_LTRIGGER) activeFoot->manualTilt = clampf(activeFoot->manualTilt - dt * 1.2f, -0.32f, 0.32f);
  if (pad->Buttons & PSP_CTRL_TRIANGLE) activeFoot->manualTilt = clampf(activeFoot->manualTilt + dt * 1.2f, -0.32f, 0.32f);
  if (pad->Buttons & PSP_CTRL_SQUARE) activeFoot->manualTilt = lerpf(activeFoot->manualTilt, 0, minf(1, 5 * dt));

  mouse.vy = (mouse.y - mouse.prevY) / maxf(0.001f, dt); mouse.prevY = mouse.y;
  if (frontFoot.isSlamming) { frontFoot.tx = mouse.x; frontFoot.ty = frontFoot.groundY; }
  else if (mouse.lmb) {
    frontFoot.tx = mouse.x;
    if (!frontFoot.isGrounded && mouse.vy > 560) { frontFoot.isSlamming = 1; frontFoot.ty = frontFoot.groundY; }
    else if (frontFoot.isGrounded) frontFoot.ty = mouse.y >= frontFoot.groundY - 145 ? frontFoot.groundY : mouse.y;
    else frontFoot.ty = clampf(mouse.y, 150, frontFoot.groundY);
  } else { frontFoot.tx = mouse.x; frontFoot.ty = clampf(mouse.y, 150, frontFoot.groundY - 22); }
  if (spaceDown || mouse.rmb) { if (!frontFoot.isGrounded) frontFoot.isSlamming = 1; frontFoot.ty = frontFoot.groundY; }

  prevFP = frontFoot.fullPressure;
  footUpdate(&frontFoot, dt, 1, time_);
  frontAvx = fabsf(frontFoot.vx);
  smearRumbleCooldown = maxf(0, smearRumbleCooldown - dt);
  isSmearingFront = hasInteracted && frontFoot.isGrounded && (frontAvx > 14 || fabsf(frontFoot.angleVel) > 0.30f);
  if (isSmearingFront && !wasSmearingFront && smearRumbleCooldown <= 0) { sfxSmearRumble(); smearRumbleCooldown = 0.45f; }
  wasSmearingFront = isSmearingFront;
  if (hasInteracted && frontFoot.isGrounded && (mouse.lmb || spaceDown || mouse.rmb)) pressHoldTime += dt;
  else pressHoldTime = maxf(0, pressHoldTime - dt * 3.5f);

  if (hasInteracted && frontFoot.isGrounded) {
    if (spaceDown || mouse.rmb) supportRiseTarget = 1.0f;
    else if (isSmearingFront) supportRiseTarget = clampf(0.42f + frontAvx / 420, 0.42f, 0.78f);
    else if (mouse.lmb && pressHoldTime > 0.35f) supportRiseTarget = clampf((pressHoldTime - 0.35f) * 0.9f, 0, 0.55f);
  }
  footBackSupport(&backFoot, dt, supportRiseTarget);
  frontFoot.fullPressure = hasInteracted && frontFoot.isGrounded && (backFoot.supportRise > 0.80f || spaceDown || mouse.rmb);
  backFoot.fullPressure = 0;

  activeDots = hudMult();
  heartPower = clampf(hud.hypeLevel / 5.0f, 0, 1);
  {
    Foot *foot = &frontFoot; int mult = hudMult();
    if (foot->isGrounded && !foot->wasGrounded) {
      float impact = clampf(foot->vy / 680.0f, 0.35f, 1.55f);
      hud.steps++;
      footLandingRecoil(foot, impact); sfxThud(impact, foot->fullPressure);
      addShake(impact * (foot->fullPressure ? 11.5f : 6.0f));
      if (foot->fullPressure || impact > 0.75f) gooShockwave(foot->x, foot->groundY + 2, foot->fullPressure ? 170 : 120);
      if (foot->fullPressure) evaluateContact(foot, MODE_FULL, mult, dt, heartPower, handleKill);
      else if (foot->vy > 580) evaluateContact(foot, MODE_SOFT, mult, dt, heartPower, handleKill);
      else evaluateContact(foot, MODE_PIN, mult, dt, heartPower, handleKill);
    }
    if (foot->fullPressure && !prevFP && foot->wasGrounded) {
      footLandingRecoil(foot, 0.95f); sfxThud(0.95f, 1); gooShockwave(foot->x, foot->groundY + 2, 185); addShake(12.5f);
      evaluateContact(foot, MODE_FULL, mult, dt, heartPower, handleKill);
    }
    if (hasInteracted && !foot->isGrounded && foot->y >= foot->groundY - 55) evaluateContact(foot, MODE_PIN, mult, dt, heartPower, handleKill);
    if (hasInteracted && foot->isGrounded) {
      float avx = fabsf(foot->vx);
      if (foot->fullPressure) evaluateContact(foot, MODE_FULL, mult, dt, heartPower, handleKill);
      else if (avx > 14 || fabsf(foot->angleVel) > 0.32f) {
        evaluateContact(foot, MODE_SLOW_SMEAR, mult, dt, heartPower, handleKill);
        if (avx > 55 && foot->dryness < 0.85f && rnd(0, 1) < 0.35f) { float px, py; footSolePointAt(foot, rnd(0.12f, 0.88f), &px, &py); gooSmear(px, py, foot->vx * 0.75f, rnd(5, 11)); }
      } else evaluateContact(foot, MODE_PIN, mult, dt, heartPower, handleKill);
    }
  }
  for (i = 0; i < antN; i++) if (ants[i].alive && ants[i].state == ST_PINNED) grindingPinned++;
  if (grindingPinned > 0 || (hud.comboKills > 0 && frontFoot.fullPressure)) hudCrushTime(dt, isSmearingFront);

  /* smear / descent audio */
  gooAmount = maxf(1 - frontFoot.dryness, frontFoot.adhesion);
  audioSmearUpdate(frontAvx, gooAmount, frontFoot.isGrounded, grindingPinned > 0, heartPower, gooHasUnder(&frontFoot));
  audioDescent(frontFoot.vy, frontFoot.isGrounded);
  if (!frontFoot.isGrounded) audioStopCrunches();

  antSpeedMult = (0.58f + activeDots * 0.355f) * (1 + 0.22f * gPsycho);
  enemiesUpdate(dt, &frontFoot, &backFoot, antSpeedMult);
  gooUpdate(dt); motesUpdate(dt);
  hudUpdate(dt);
  portraitUpdate(dt, hud.comboKills > 0, hud.hypeLevel, isSmearingFront, activeDots);
#ifdef SIM_FORCE_HYPE
  if (time_ > 6.0f && time_ < 6.1f) hud.hypeLevel = 4.6f;
#endif
  /* music: starts when only 15% of the happiness meter is left, stops once it falls well below */
  if (hud.hypeLevel >= 5.0f * 0.85f) musicSet(1);
  else if (hud.hypeLevel < 3.0f) musicSet(0);
}

/* ---------- frame drawing ---------- */
static void buildFrame(void) {
  int k;
  shakeX = shakeMag > 0.2f ? rnd(-shakeMag, shakeMag) : 0; shakeY = shakeMag > 0.2f ? rnd(-shakeMag, shakeMag) : 0;
  useZoom = 1;
  useBatch(&bA); drawCityAnim(time_); gooDrawGround(); drawTrash(); footShadow(&backFoot);
  useBatch(&bB); footGooRim(&backFoot); gooDrawStrands(&backFoot); drawAnts(); footShadow(&frontFoot);
  useBatch(&bC); footGooRim(&frontFoot); gooDrawStrands(&frontFoot); gooDrawParticles(); drawPopups(); drawPsychoOverlay(time_); drawSwitchFx(0.6f * (ghostT / GH_T) * (ghostT / GH_T));
  { float sx_ = shakeX, sy_ = shakeY; shakeX = shakeY = 0;
    portraitCompute(time_, hud.comboKills);
    useBatch(&bD); portraitBuildBg();
    useBatch(&bE); portraitBuildFg(); drawHudScreen(time_, hasInteracted, hintTimer); drawPsychoBanner(time_); drawSwitchBanner(bannerT, curChar);
    /* cursor + overlays */
    { float cx = mouse.sx * K, cy = mouse.sy * K; unsigned int cc = (mouse.lmb || mouse.rmb) ? RGB(255, 126, 184) : RGB(255, 255, 255);
      if (started) { rectP(cx - 6, cy - 1, 4, 2, cc); rectP(cx + 3, cy - 1, 4, 2, cc); rectP(cx - 1, cy - 6, 2, 4, cc); rectP(cx - 1, cy + 3, 2, 4, cc); rectP(cx - 1, cy - 1, 2, 2, cc); } }
    if (muted) textP(440, 262, "MUTE", RGB(165, 145, 174), 1);
    if (paused) { rectP(120, 92, 240, 84, col(17, 11, 25, 225)); textP(206, 100, "PAUSED", RGB(238, 226, 242), 1); textP(150, 120, "START   RESUME", RGB(255, 105, 165), 1); textP(150, 134, "SELECT  CHANGE CHARACTER", RGB(255, 190, 120), 1); textP(150, 148, "SQUARE  SOUND ON OFF", RGB(160, 220, 255), 1); }
    shakeX = sx_; shakeY = sy_; }
  (void)k;
}
static void submitFrame(void) {
  sceGuStart(GU_DIRECT, list);
  sceGuClearColor(0xff0e0508);
  sceGuClear(GU_COLOR_BUFFER_BIT);
  useZoom = 1;
  drawCityTex();
  flushBatch(&bA);
  { float gk = ghostT / GH_T, real = 1.0f - 0.85f * gk;
    footDrawEx(&backFoot, 0, curChar, real);
    if (ghostT > 0) footDrawEx(&ghostBack, 2, ghostChar, gk);
    flushBatch(&bB);
    footDrawEx(&frontFoot, 1, curChar, real);
    if (ghostT > 0) footDrawEx(&ghostFront, 3, ghostChar, gk); }
  flushBatch(&bC);
  sceGuScissor((int)(PX * K), (int)(PY * K), (int)ceilf((PX + PW) * K), (int)ceilf((PY + PH) * K));
  flushBatch(&bD);
  sceGuScissor(0, 0, 480, 272);
  portraitDrawTex();
  flushBatch(&bE);
  sceGuFinish(); sceGuSync(0, 0); sceDisplayWaitVblankStart(); sceGuSwapBuffers();
}

/* ---------- system ---------- */
static int exit_cb(int a, int b, void *c) { (void)a; (void)b; (void)c; sceKernelExitGame(); return 0; }
static int cb_thread(SceSize args, void *argp) {
  int id; (void)args; (void)argp;
  id = sceKernelCreateCallback("exit", exit_cb, NULL);
  sceKernelRegisterExitCallback(id);
  sceKernelSleepThreadCB();
  return 0;
}
static void setup_cb(void) {
  int t = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, NULL);
  if (t >= 0) sceKernelStartThread(t, 0, NULL);
}
static int loadFile(const char *path, void *dst, int bytes) {
  FILE *f = fopen(path, "rb"); int n;
  if (!f) return -1;
  n = (int)fread(dst, 1, bytes, f); fclose(f); return n;
}
static void gu_init(void) {
  sceGuInit();
  sceGuStart(GU_DIRECT, list);
  sceGuDrawBuffer(GU_PSM_8888, (void *)0, 512);
  sceGuDispBuffer(480, 272, (void *)0x88000, 512);
  sceGuDepthBuffer((void *)0x110000, 512);
  sceGuOffset(2048 - 240, 2048 - 136);
  sceGuViewport(2048, 2048, 480, 272);
  sceGuDepthRange(65535, 0);
  sceGuScissor(0, 0, 480, 272);
  sceGuEnable(GU_SCISSOR_TEST);
  sceGuEnable(GU_BLEND);
  sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
  sceGuDisable(GU_DEPTH_TEST);
  sceGuFinish();
  sceGuSync(0, 0);
  sceDisplayWaitVblankStart();
  sceGuDisplay(GU_TRUE);
}

int main(int argc, char **argv) {
  char m[400]; int i, loaded = 0, ploaded = 0, sc;
  SceCtrlData pad; unsigned int prevBtn = 0; unsigned long long last; unsigned int frameNo = 0; float accBuild = 0, accSubmit = 0;
  FILE *lf = fopen("ms0:/DUSKLOG.TXT", "wb"); if (lf) fclose(lf);
  setup_cb();
  pspDebugScreenInit();
  scePowerSetClockFrequency(333, 333, 166);
  say("ALTUHA GAME (C build, stage 2b, 333MHz)");
  if (argc > 0 && argv && argv[0]) {
    char *sl; strncpy(base, argv[0], sizeof(base) - 1); sl = strrchr(base, '/'); if (sl) sl[1] = 0; else base[0] = 0;
  }
  snprintf(m, sizeof(m), "base: %s", base); say(m);
  for (i = 0; i < NPOSE; i++) {
    int ch, sl = poseSlot[i];
    if (sl < 0) continue;
    for (ch = 0; ch < NCHAR; ch++) {
      static const char *fp[NCHAR] = { "boot", "bare", "strp" };
      char p[64]; snprintf(p, sizeof(p), "assets/%s_%02d.rgba", fp[ch], i);
      bootOk[ch][sl] = (loadFile(ap(p), bootTex[ch][sl], 256 * 256 * 4) == 256 * 256 * 4);
      if (bootOk[ch][sl]) { sceKernelDcacheWritebackRange(bootTex[ch][sl], 256 * 256 * 4); loaded++; }
    }
  }
  { int ch; static const char *pp[NCHAR] = { "portrait", "bport", "sport" };
    for (ch = 0; ch < NCHAR; ch++) for (i = 0; i < PORT_COUNT[ch]; i++) {
      char p[64]; snprintf(p, sizeof(p), "assets/%s_%02d.rgba", pp[ch], i);
      portOk[ch][i] = (loadFile(ap(p), portTex[ch][i], 64 * 64 * 4) == 64 * 64 * 4);
      if (portOk[ch][i]) { sceKernelDcacheWritebackRange(portTex[ch][i], 64 * 64 * 4); ploaded++; }
    } }
  { static unsigned char cbuf[65536]; int ok1 = 0, ok2 = 0, n, tot;
    FILE *fc = fopen(ap("assets/city_top.565"), "rb");
    if (fc) { tot = 0; while ((n = (int)fread(cbuf, 1, sizeof(cbuf), fc)) > 0 && tot + n <= 0x40000) { memcpy((unsigned char *)VRAM_CPU(VRAM_CITY_TOP) + tot, cbuf, n); tot += n; } fclose(fc); ok1 = (tot == 0x40000); }
    fc = fopen(ap("assets/city_bot.565"), "rb");
    if (fc) { tot = 0; while ((n = (int)fread(cbuf, 1, sizeof(cbuf), fc)) > 0 && tot + n <= 0x10000) { memcpy((unsigned char *)VRAM_CPU(VRAM_CITY_BOT) + tot, cbuf, n); tot += n; } fclose(fc); ok2 = (tot == 0x10000); }
    cityOk = ok1 && ok2; }
  snprintf(m, sizeof(m), "feet %d/21  portraits %d/38  city %d", loaded, ploaded, cityOk); say(m);
  sc = loadSfx(); snprintf(m, sizeof(m), "sfx pack: %d sounds", sc); say(m);
  sceKernelDelayThread(1200000);
  say("starting graphics..."); sceKernelDelayThread(400000);
  gu_init(); logf_("GU ready");
  audioCh = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 1024, PSP_AUDIO_FORMAT_STEREO);
  if (audioCh >= 0) {
    startLoops();
    audioRun = 1; audioThread = sceKernelCreateThread("audio", audioMain, 0x12, 0x10000, 0, NULL);
    if (audioThread >= 0) sceKernelStartThread(audioThread, 0, NULL); else audioRun = 0;
    musicRun = 1; musicThread = sceKernelCreateThread("music", musicMain, 0x22, 0x8000, 0, NULL);
    if (musicThread >= 0) sceKernelStartThread(musicThread, 0, NULL); else musicRun = 0;
  }
  srand((unsigned)sceKernelGetSystemTimeWide());
  sceCtrlSetSamplingCycle(0); sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
  memset(&pad, 0, sizeof(pad)); pad.Lx = 128; pad.Ly = 128;
  resetGame();
  last = sceKernelGetSystemTimeWide(); logf_("main loop");

  for (;;) {
    unsigned long long now; float dt; unsigned int pressed, released;
    sceCtrlReadBufferPositive(&pad, 1);
    now = sceKernelGetSystemTimeWide(); dt = (float)(now - last) / 1000000.0f; last = now;
    if (dt < 0.008f) dt = 0.008f;
    if (dt > 0.05f) dt = 0.05f;
    pressed = pad.Buttons & ~prevBtn; released = ~pad.Buttons & prevBtn; prevBtn = pad.Buttons;
    if (pressed & PSP_CTRL_START) paused = !paused;
    if (pressed & PSP_CTRL_SELECT) { if (!paused) switchCharacter(); }
    if (paused && (pressed & PSP_CTRL_SQUARE)) muted = !muted;
    musicPaused = paused; musicTarget = (musicWant && !muted) ? MUSIC_VOL : 0.0f;
    if (!paused) engineUpdate(dt, &pad, pressed, released);
    else { audioSmearUpdate(0, 0, 0, 0, 0, 0); audioDescent(0, 1); }
    { unsigned long long t0 = sceKernelGetSystemTimeWide(), t1, t2;
      buildFrame(); t1 = sceKernelGetSystemTimeWide();
      submitFrame(); t2 = sceKernelGetSystemTimeWide();
      accBuild += (float)(t1 - t0); accSubmit += (float)(t2 - t1); }
    frameNo++;
    if (frameNo % 300 == 0) { snprintf(m, sizeof(m), "frame %u: avg build %.1f ms, submit(+vsync) %.1f ms, ants %d goo p%d s%d", frameNo, accBuild / 300000.0f, accSubmit / 300000.0f, antN, puddleN, smearN); logf_(m); accBuild = accSubmit = 0; }
    if (frameNo == 1 || frameNo == 60 || frameNo == 600) {
      snprintf(m, sizeof(m), "frame %u ok (A=%d B=%d C=%d D=%d E=%d)", frameNo, bA.n, bB.n, bC.n, bD.n, bE.n); logf_(m);
    }
  }
  return 0;
}
