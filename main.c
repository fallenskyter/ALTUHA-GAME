/* DUSK CRUSHER - native PSP port (plain C), stage 1
 * Game logic runs in the original 1280x720 canvas units and is scaled by
 * K = 480/1280 only when vertices are emitted. */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "poses.h"

PSP_MODULE_INFO("DUSKCRUSHER", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define CW 1280.0f
#define CH 720.0f
#define K  0.375f
#define ASPHALT_TOP 588.0f
#define CURB_TOP 690.0f
#define GROUND_Y_FRONT 682.0f
#define GROUND_Y_BACK 640.0f
#define WORLD_ZOOM 1.12f
#define MAXV 14000

typedef struct { unsigned int c; float x, y, z; } VC;
typedef struct { float u, v; unsigned int c; float x, y, z; } VT;

static unsigned int __attribute__((aligned(16))) list[65536];
static VC __attribute__((aligned(16))) vbA[MAXV];
static VC __attribute__((aligned(16))) vbB[MAXV];
static VC __attribute__((aligned(16))) vbC[MAXV];
static int nC = 0;
static VT __attribute__((aligned(16))) tvLeg[2][6];
static VT __attribute__((aligned(16))) tvBody[2][6];
static unsigned int __attribute__((aligned(16))) bootTex[NPOSE][256 * 256];
static int bootOk[NPOSE];
#define PCM_MAX 30000
static short __attribute__((aligned(64))) thudPCM[PCM_MAX * 2];
static short __attribute__((aligned(64))) crunchPCM[PCM_MAX * 2];
static int thudN = 0, crunchN = 0;

static char base[256] = "";
static VC *cur = vbA;
static int *curN;
static int nA = 0, nB = 0;
static float shakeX = 0, shakeY = 0;
static int useZoom = 1;

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

/* ---------- batched rectangles ---------- */
static void rectP(float x, float y, float w, float h, unsigned int c) {
  VC *v;
  if (*curN > MAXV - 2) return;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  x = floorf(x); y = floorf(y); w = floorf(w + 0.5f); h = floorf(h + 0.5f);
  v = cur + *curN;
  v[0].c = c; v[0].x = x;     v[0].y = y;     v[0].z = 0;
  v[1].c = c; v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
  *curN += 2;
}
/* world-space rect (canvas units, zoomed) */
static void rectW(float x, float y, float w, float h, unsigned int c) {
  float x0 = wX(x), y0 = wY(y), x1 = wX(x + w), y1 = wY(y + h);
  rectP(x0, y0, x1 - x0, y1 - y0, c);
}
static void flush(VC *buf, int n) {
  if (n <= 0) return;
  sceKernelDcacheWritebackRange(buf, n * sizeof(VC));
  sceGuDisable(GU_TEXTURE_2D);
  sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, n, 0, buf);
}

/* 5x7 font: space, 0-9, A-Z */
static const unsigned char glyph[37][7] = {
  {0,0,0,0,0,0,0}, {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
  {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30}, {6,8,16,30,17,17,14},
  {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
  {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14}, {28,18,17,17,17,18,28},
  {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16}, {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
  {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
  {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
  {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17}, {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
  {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4}, {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
  {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
};
static void textP(float x, float y, const char *s, unsigned int c, int sc) {
  for (; *s; s++, x += 6 * sc) {
    int id = 0, yy, xx;
    char ch = *s;
    if (ch >= '0' && ch <= '9') id = 1 + ch - '0';
    else if (ch >= 'A' && ch <= 'Z') id = 11 + ch - 'A';
    else if (ch >= 'a' && ch <= 'z') id = 11 + ch - 'a';
    if (id == 0) continue;
    for (yy = 0; yy < 7; yy++)
      for (xx = 0; xx < 5; xx++)
        if (glyph[id][yy] & (1 << (4 - xx))) rectP(x + xx * sc, y + yy * sc, sc, sc, c);
  }
}

/* ---------- audio (mixer thread, never blocks the game) ---------- */
typedef struct { short *p; volatile int pos, len; } Voice;
static Voice voices[8];
static volatile int audioRun = 0;
static int audioCh = -1, audioThread = -1, muted = 0;

static void playPCM(short *p, int n) {
  int i, best = 0;
  if (muted || !p || n <= 0 || audioCh < 0) return;
  for (i = 0; i < 8; i++) {
    if (voices[i].pos >= voices[i].len) { best = i; break; }
    if (voices[i].pos > voices[best].pos) best = i;
  }
  voices[best].pos = 0; voices[best].p = p; voices[best].len = n;
}
static int audioMain(SceSize args, void *argp) {
  static short __attribute__((aligned(64))) buf[1024 * 2];
  static int mix[1024 * 2];
  (void)args; (void)argp;
  while (audioRun) {
    int v, i;
    memset(mix, 0, sizeof(mix));
    for (v = 0; v < 8; v++) {
      int pos = voices[v].pos, len = voices[v].len, n;
      const short *src;
      if (!voices[v].p || pos >= len) continue;
      n = len - pos; if (n > 1024) n = 1024;
      src = voices[v].p + pos * 2;
      for (i = 0; i < n * 2; i++) mix[i] += src[i] / 2;
      voices[v].pos = pos + n;
    }
    for (i = 0; i < 1024 * 2; i++) {
      int x = mix[i];
      if (x > 32767) x = 32767;
      if (x < -32768) x = -32768;
      buf[i] = (short)x;
    }
    sceAudioOutputPannedBlocking(audioCh, PSP_AUDIO_VOLUME_MAX, PSP_AUDIO_VOLUME_MAX, buf);
  }
  return 0;
}
static float lastCrunch = -1, gTime = 0;
static void sfxThud(void) { playPCM(thudPCM, thudN); }
static void sfxCrunch(void) { if (gTime - lastCrunch > 0.09f) { lastCrunch = gTime; playPCM(crunchPCM, crunchN); } }

/* ---------- goo / particles (stage-1 version) ---------- */
typedef struct { float x, y, vx, vy, life, maxLife, r; } Part;
typedef struct { float x, y, w, h, life; } Stain;
typedef struct { float x, y, r, maxR, life; } Wave;
#define MAX_PART 90
#define MAX_STAIN 120
#define MAX_WAVE 6
static Part parts[MAX_PART]; static int partN = 0;
static Stain stains[MAX_STAIN]; static int stainN = 0;
static Wave waves[MAX_WAVE]; static int waveN = 0;

static void gooSplash(float x, float y, int n, float power, float vxBias) {
  int i;
  for (i = 0; i < n && partN < MAX_PART; i++) {
    float a = rnd(-3.0f, -0.15f), v = rnd(120, 420) * power;
    Part *p = &parts[partN++];
    p->x = x; p->y = y - 4; p->vx = cosf(a) * v + vxBias * 0.25f; p->vy = sinf(a) * v;
    p->maxLife = p->life = rnd(0.4f, 0.9f); p->r = rnd(3, 7) * (0.7f + power * 0.3f);
  }
}
static void addStain(float x, float y, float w, float h) {
  Stain *s;
  if (stainN >= MAX_STAIN) { memmove(&stains[0], &stains[1], sizeof(Stain) * (MAX_STAIN - 1)); stainN = MAX_STAIN - 1; }
  s = &stains[stainN++]; s->x = x; s->y = y; s->w = w; s->h = h; s->life = rnd(7, 12);
}
static void gooSmear(float x, float y, float vx, float len) { addStain(x - fabsf(vx) * 0.04f, y, len + fabsf(vx) * 0.08f, rnd(5, 9)); }
static void gooPuddle(float x, float y, float w, float h) { addStain(x - w * 0.5f, y, w, h); }
static void addWave(float x, float y, float maxR) {
  if (waveN < MAX_WAVE) { waves[waveN].x = x; waves[waveN].y = y; waves[waveN].r = 10; waves[waveN].maxR = maxR; waves[waveN].life = 0.45f; waveN++; }
}
static int hasGooUnder(float fx) {
  int i;
  for (i = 0; i < stainN; i++) if (fabsf(stains[i].x + stains[i].w * 0.5f - fx) < 90) return 1;
  return 0;
}
static void gooUpdate(float dt) {
  int i, j;
  for (i = 0, j = 0; i < partN; i++) {
    Part p = parts[i];
    p.life -= dt; p.vy += 1500 * dt; p.x += p.vx * dt; p.y += p.vy * dt;
    if (p.y > CURB_TOP - 6 && p.vy > 0) { if (stainN < MAX_STAIN - 1 && p.r > 3.5f) addStain(p.x - p.r, p.y, p.r * 2.4f, p.r * 0.8f); p.life = 0; }
    if (p.life > 0) parts[j++] = p;
  }
  partN = j;
  for (i = 0, j = 0; i < stainN; i++) { Stain s = stains[i]; s.life -= dt; if (s.life > 0) stains[j++] = s; }
  stainN = j;
  for (i = 0, j = 0; i < waveN; i++) { Wave w = waves[i]; w.life -= dt; w.r += (w.maxR - w.r) * minf(1, 9 * dt); if (w.life > 0) waves[j++] = w; }
  waveN = j;
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

/* ---------- giant foot (exact port) ---------- */
typedef struct {
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
    if (!f->peelStrandsSpawned && curH > 2.5f) { f->peelStrandsSpawned = 1; f->peelSfxCooldown = 0.34f; }
    f->adhesion = maxf(0, f->adhesion - dt * (0.28f + snap * 0.55f));
  } else if (curH >= 165) f->peelStrandsSpawned = 0;
  else if (f->isGrounded) f->peelStrandsSpawned = 0;
  hover = (!f->isGrounded && !activeDrag && !peeling) ? sinf(time * 2.4f + (f->isBack ? 1.6f : 0)) * 5 : 0;
  effStX = peeling ? stiffX * 0.42f : stiffX;
  f->x += (f->tx - f->x) * minf(1, effStX * dt);
  f->y += (f->ty + hover - f->y) * minf(1, stiffY * dt);
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
  /* corners: 0 TL, 1 TR, 2 BR, 3 BL (xs/ys already in PSP pixels) */
  static const int idx[6] = {0, 1, 2, 0, 2, 3};
  float us[4], vs[4]; int i;
  us[0] = u0; vs[0] = v0; us[1] = u1; vs[1] = v0; us[2] = u1; vs[2] = v1; us[3] = u0; vs[3] = v1;
  for (i = 0; i < 6; i++) { int k = idx[i]; v[i].u = us[k]; v[i].v = vs[k]; v[i].c = c; v[i].x = xs[k]; v[i].y = ys[k]; v[i].z = 0; }
}
static void footDraw(const Foot *f, int slot) {
  const PoseInfo *p = &POSES[f->poseIdx];
  float sx = f->spriteScaleX, sy = f->spriteScaleY;
  float pW = p->wu * sx, pH = p->hu * sy, pLeft = -p->anchor * sx, pTop = -p->botY * sy;
  float extY = pTop + 3.0f * sy, extH = 620.0f, sh = p->slope * (sx / sy);
  float cx = f->x + f->microTremor, cy = f->y + f->pressureSink + f->landingRecoil;
  float c = cosf(f->angle), s = sinf(f->angle);
  float lx[4], ly[4], xs[4], ys[4];
  float kt = (float)p->hpx / p->hu;
  float rowV = 3.0f * kt + 0.5f;
  unsigned int tint = f->isBack ? 0xffc2c2c2u : 0xffffffffu;
  int i;
  if (!bootOk[f->poseIdx]) return;
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexMode(GU_PSM_8888, 0, 0, 0);
  sceGuTexImage(0, p->tw, p->th, p->tw, bootTex[f->poseIdx]);
  sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
  sceGuTexFilter(GU_LINEAR, GU_LINEAR);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
#define XFORM(LX, LY, OX, OY) do { float a_ = (LX) * f->squashX, b_ = (LY) * f->squashY; a_ += b_ * f->smearShear; \
    float xr_ = a_ * c - b_ * s, yr_ = a_ * s + b_ * c; (OX) = wX(cx + xr_); (OY) = wY(cy + yr_); } while (0)
  /* leg extension (stretched top band) */
  lx[0] = pLeft - extH * sh;      ly[0] = extY - extH;
  lx[1] = pLeft + pW - extH * sh; ly[1] = extY - extH;
  lx[2] = pLeft + pW + 5 * sh;    ly[2] = extY + 5;
  lx[3] = pLeft + 5 * sh;         ly[3] = extY + 5;
  for (i = 0; i < 4; i++) XFORM(lx[i], ly[i], xs[i], ys[i]);
  quadVT(tvLeg[slot], xs, ys, 0, rowV, (float)p->wpx, rowV, tint);
  sceKernelDcacheWritebackRange(tvLeg[slot], sizeof(tvLeg[slot]));
  sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 6, 0, tvLeg[slot]);
  /* boot body */
  lx[0] = pLeft;      ly[0] = pTop;
  lx[1] = pLeft + pW; ly[1] = pTop;
  lx[2] = pLeft + pW; ly[2] = pTop + pH;
  lx[3] = pLeft;      ly[3] = pTop + pH;
  for (i = 0; i < 4; i++) XFORM(lx[i], ly[i], xs[i], ys[i]);
  quadVT(tvBody[slot], xs, ys, 0, 0, (float)p->wpx, (float)p->hpx, tint);
  sceKernelDcacheWritebackRange(tvBody[slot], sizeof(tvBody[slot]));
  sceGuDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 6, 0, tvBody[slot]);
  sceGuDisable(GU_TEXTURE_2D);
#undef XFORM
}
static void footShadow(const Foot *f) {
  float h = maxf(0, f->groundY - f->y);
  float alpha = clampf((f->isBack ? 0.42f : 0.60f) - h / 680.0f, 0.12f, 0.60f);
  float spread = 1 + h / 860.0f, rx = f->width * 0.50f * spread, ry = 16 * f->scale;
  int i;
  for (i = 0; i < 4; i++) {   /* ellipse as stacked bars */
    float t = (i + 0.5f) / 4.0f, hw = rx * sqrtf(1 - t * t), hy = ry * t;
    rectW(f->x - hw, f->groundY + 4 - hy, hw * 2, hy * 2 * 0.5f + 1, col(8, 5, 14, (int)(alpha * 255 * (0.55f + 0.45f * (1 - t)))));
  }
}
static void footGooRim(const Foot *f) {
  /* simplified sole-goo rim: dark blobs along the sole (full goo port comes in stage 2) */
  int i;
  float liftReveal;
  if (f->dryness >= 0.99f) return;
  liftReveal = clampf((f->groundY - f->y) / 72.0f, 0, 1);
  for (i = 0; i < 24; i++) {
    float amt = f->soleBlobs[i], px, py, rx, ry, drip;
    if (amt <= 0.04f) continue;
    footSolePointAt(f, i / 23.0f, &px, &py);
    rx = (8 + amt * 18) * f->scale * lerpf(0.45f, 0.92f, liftReveal);
    ry = (5 + amt * 11) * f->scale * lerpf(0.32f, 0.88f, liftReveal);
    rectW(px - rx, py - ry, rx * 2, ry * 2, RGB(5, 5, 6));
    drip = f->soleDrips[i];
    if (drip > 3) rectW(px - (3.2f + amt * 5.2f) * f->scale, py, (3.2f + amt * 5.2f) * 2 * f->scale, drip, RGB(5, 5, 6));
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
    int deficit = 82 - antN; int batch, k; float side; int wave;
    if (deficit < 0) deficit = 0;
    batch = deficit > 35 ? rndi(6, 12) : rndi(3, 6);
    side = (rand() % 2) ? -16.0f : CW + 16.0f; wave = (rand() % 100) < 78;
    for (k = 0; k < batch; k++) spawnAnt(wave ? side + (side < 0 ? -k * 12.0f : k * 12.0f) : 0, wave);
    spawnTimer = deficit > 30 ? rnd(0.18f, 0.36f) : rnd(0.36f, 0.72f);
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
        if (!a->isScreaming && a->screamCooldown <= 0 && (rand() % 1000) < 100) {
          float dur = rnd(0.45f, 0.72f); a->isScreaming = 1; a->screamTimer = dur; a->screamCooldown = rnd(1.4f, 2.8f);
        }
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
      if (!a->isScreaming && a->screamCooldown <= 0 && closest < 165 && (rand() % 1000) < 32) {
        float dur = rnd(0.42f, 0.68f); a->isScreaming = 1; a->screamTimer = dur; a->screamCooldown = rnd(2.2f, 4.2f);
      }
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
  float grindRate = (0.48f + fabsf(foot->vx) * 0.026f + fabsf(foot->angleVel) * 1.8f) * smearStrength * dt;
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
  int i, a2;
  /* simple insertion sort by y (stable enough for drawing) */
  for (i = 1; i < antN; i++) { Ant t = ants[i]; int j = i - 1; while (j >= 0 && ants[j].y > t.y) { ants[j + 1] = ants[j]; j--; } ants[j + 1] = t; }
  for (i = 0; i < antN; i++) {
    Ant *a = &ants[i];
    float x = a->x, y = a->y, squash, h, w, bob, legH, bodyH, r, top, legSwing, armY, eyeOff, ey;
    unsigned int blk = RGB(5, 5, 6);
    if (!a->alive) continue;
    squash = a->state == ST_PINNED ? lerpf(0.28f, 0.58f, a->crushHealth) : 1.0f;
    h = a->h * squash; w = a->w * (a->state == ST_PINNED ? (1.15f + (1 - a->crushHealth) * 0.55f) : 1.0f);
    bob = a->state == ST_PINNED ? 0 : fabsf(sinf(a->phase)) * (a->state == ST_PANIC ? 3.5f : 1.8f);
    rectW(x - w * 0.55f, y - 3, w * 1.1f, 6, col(0, 0, 0, 120));
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
  (void)a2;
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

/* ---------- engine state ---------- */
typedef struct { float x, y, sx, sy, prevY, vy, downAt; int lmb, rmb; } Mouse;
static Mouse mouse;
static Foot frontFoot, backFoot;
static float time_ = 0, shakeMag = 0, pressHoldTime = 0, smearRumbleCooldown = 0;
static int hasInteracted = 0, spaceDown = 0, wasSmearingFront = 0, started = 0, paused = 0;
static float hintTimer = 3.2f;
static Foot *activeFoot;

static void addShake(float a) { shakeMag = minf(24, maxf(shakeMag, a)); }
static float gHeartPower = 0;
static void handleKill(const KillRes *r) {
  float hp = clampf(hud.hypeLevel / 5.0f, 0, 1);
  if (r->count > 0) {
    int isFP = r->mode == MODE_FULL;
    hudRegister(r); sfxCrunch();
    addShake(minf(22, (4.0f + r->count * 0.95f + (isFP ? 5.5f : 0)) * (1 + hp * 0.35f)));
  } else if (r->trashCrushed > 0) { hudRegister(r); sfxCrunch(); }
}
static void resetGame(void) {
  memset(&hud, 0, sizeof(hud));
  partN = stainN = waveN = 0;
  footInit(&frontFoot, 430, 500, 1.0f, GROUND_Y_FRONT, 0);
  footInit(&backFoot, 770, GROUND_Y_BACK, 0.88f, GROUND_Y_BACK, 1);
  mouse.x = 430; mouse.y = 500; mouse.sx = 430; mouse.sy = 500; mouse.prevY = 500; mouse.vy = 0; mouse.lmb = mouse.rmb = 0;
  activeFoot = &frontFoot; pressHoldTime = 0; wasSmearingFront = 0; spaceDown = 0; hasInteracted = 0;
  time_ = 0; hintTimer = 3.2f; shakeMag = 0;
  enemiesInit();
  started = 1; paused = 0;
}

static void engineUpdate(float dt, const SceCtrlData *pad, unsigned int pressed, unsigned int released) {
  float frontAvx, supportRiseTarget = 0, maxSmearVx = 0;
  int activeDots, isSmearingFront, prevFP, i;
  float heartPower, antSpeedMult;
  time_ += dt; gTime = time_;
  hintTimer = maxf(0, hintTimer - dt);
  shakeMag = maxf(0, shakeMag - dt * 36);

  /* ---- input -> virtual mouse ---- */
  {
    float ax = 0, ay = 0;
    if (pad->Lx < 90 || pad->Lx > 166) ax = clampf(((float)pad->Lx - 128) / 100.0f, -1, 1);
    if (pad->Ly < 90 || pad->Ly > 166) ay = clampf(((float)pad->Ly - 128) / 100.0f, -1, 1);
    if (pad->Buttons & PSP_CTRL_LEFT) ax = -1;
    if (pad->Buttons & PSP_CTRL_RIGHT) ax = 1;
    if (pad->Buttons & PSP_CTRL_UP) ay = -1;
    if (pad->Buttons & PSP_CTRL_DOWN) ay = 1;
    if (ax != 0 || ay != 0) hasInteracted = 1;
    mouse.sx = clampf(mouse.sx + ax * 1150 * dt, 0, CW);
    mouse.sy = clampf(mouse.sy + ay * 900 * dt, 0, CH);
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
  gHeartPower = heartPower;
  {
    Foot *foot = &frontFoot; int mult = hudMult();
    if (foot->isGrounded && !foot->wasGrounded) {
      float impact = clampf(foot->vy / 680.0f, 0.35f, 1.55f);
      hud.steps++;
      footLandingRecoil(foot, impact); sfxThud();
      addShake(impact * (foot->fullPressure ? 11.5f : 6.0f));
      if (foot->fullPressure || impact > 0.75f) addWave(foot->x, foot->groundY + 2, foot->fullPressure ? 170 : 120);
      if (foot->fullPressure) evaluateContact(foot, MODE_FULL, mult, dt, heartPower, handleKill);
      else if (foot->vy > 580) evaluateContact(foot, MODE_SOFT, mult, dt, heartPower, handleKill);
      else evaluateContact(foot, MODE_PIN, mult, dt, heartPower, handleKill);
    }
    if (foot->fullPressure && !prevFP && foot->wasGrounded) {
      footLandingRecoil(foot, 0.95f); sfxThud(); addWave(foot->x, foot->groundY + 2, 185); addShake(12.5f);
      evaluateContact(foot, MODE_FULL, mult, dt, heartPower, handleKill);
    }
    if (hasInteracted && !foot->isGrounded && foot->y >= foot->groundY - 55) evaluateContact(foot, MODE_PIN, mult, dt, heartPower, handleKill);
    if (hasInteracted && foot->isGrounded) {
      float avx = fabsf(foot->vx);
      if (avx > maxSmearVx) maxSmearVx = avx;
      if (foot->fullPressure) evaluateContact(foot, MODE_FULL, mult, dt, heartPower, handleKill);
      else if (avx > 14 || fabsf(foot->angleVel) > 0.32f) {
        evaluateContact(foot, MODE_SLOW_SMEAR, mult, dt, heartPower, handleKill);
        if (avx > 55 && foot->dryness < 0.85f && rnd(0, 1) < 0.35f) { float px, py; footSolePointAt(foot, rnd(0.12f, 0.88f), &px, &py); gooSmear(px, py, foot->vx * 0.75f, rnd(5, 11)); }
      } else evaluateContact(foot, MODE_PIN, mult, dt, heartPower, handleKill);
    }
  }
  {
    int pinned = 0;
    for (i = 0; i < antN; i++) if (ants[i].alive && ants[i].state == ST_PINNED) pinned++;
    if (pinned > 0 || (hud.comboKills > 0 && frontFoot.fullPressure)) hudCrushTime(dt, isSmearingFront);
  }
  antSpeedMult = 0.58f + activeDots * 0.355f;
  enemiesUpdate(dt, &frontFoot, &backFoot, antSpeedMult);
  gooUpdate(dt);
  hudUpdate(dt);
}

/* ---------- drawing ---------- */
static void background(void) {
  /* PSP-pixel city backdrop (stage-2 port will replace this with the animated city) */
  static const int b[8][4] = {{0,95,55,126},{54,125,62,96},{115,73,48,148},{162,110,67,111},
                              {229,87,61,134},{290,127,58,94},{348,80,67,141},{415,105,65,116}};
  int i, x, y, top = (int)(wY(ASPHALT_TOP) / 1.0f);
  (void)top;
  rectP(-8, -8, 496, 232, RGB(26, 13, 34));
  rectP(0, 160, 480, 62, RGB(48, 24, 52));
  rectP(0, 190, 480, 32, RGB(70, 32, 66));
  for (i = 0; i < 8; i++) {
    rectP(b[i][0], b[i][1], b[i][2], b[i][3], RGB(48, 25, 55));
    rectP(b[i][0], b[i][1], b[i][2], 3, RGB(111, 65, 125));
    for (y = b[i][1] + 13; y < 212; y += 20)
      for (x = b[i][0] + 7; x < b[i][0] + b[i][2] - 8; x += 15)
        if ((x + y + i * 7) % 4) rectP(x, y, 5, 5, ((x + y) % 3 == 0) ? RGB(48, 190, 164) : RGB(255, 105, 165));
  }
  rectP(-8, 220, 496, 40, RGB(35, 35, 47));
  rectP(-8, 220, 496, 2, RGB(19, 16, 28));
  for (i = 1; i <= 52; i++) rectP((i * 71) % 480, 224 + (i * 19) % 33, 2, 1, RGB(43, 42, 56));
  rectP(-8, 259, 496, 30, RGB(105, 111, 132));
  for (x = 0; x < 480; x += 24) { rectP(x, 262, 12, 18, RGB(213, 215, 226)); rectP(x + 12, 262, 12, 18, RGB(65, 68, 84)); }
}
static void drawHudAndOverlays(void) {
  int i; char num[64]; unsigned int hudbg = col(17, 11, 25, 215);
  float heart = hud.hypeLevel;
  /* popups (world space) */
  for (i = 0; i < popupN; i++) {
    Popup *p = &popups[i]; float alpha = clampf(p->life / (p->maxLife * 0.45f), 0, 1);
    float px = wX(p->x), py = wY(p->y); int a = (int)(alpha * 255);
    snprintf(num, sizeof(num), "+%d", p->value);
    textP(px - 6 + 1, py + 1, num, col(6, 8, 18, a), 1);
    textP(px - 6, py, num, p->value >= 11 ? col(82, 178, 255, a) : col(58, 139, 255, a), 1);
  }
  rectP(6, 6, 160, 34, hudbg); rectP(6, 6, 160, 2, RGB(255, 105, 165));
  textP(12, 12, "DUSK CRUSHER", RGB(238, 226, 242), 1);
  snprintf(num, sizeof(num), "SCORE %d", hud.displayScore); textP(12, 25, num, RGB(255, 215, 115), 1);
  rectP(300, 6, 118, 34, hudbg); rectP(300, 6, 118, 2, RGB(48, 190, 164));
  snprintf(num, sizeof(num), "KILLS %d", hud.totalKills + hud.comboKills); textP(306, 12, num, RGB(238, 226, 242), 1);
  for (i = 1; i <= 4; i++) rectP(306 + (i - 1) * 12, 28, 8, 8, i <= hudMult() ? RGB(255, 105, 165) : RGB(48, 32, 56));
  for (i = 0; i < 5; i++) {
    float fl = clampf(heart - i, 0, 1);
    rectP(10 + i * 14, 46, 10, 9, RGB(48, 32, 56));
    if (fl > 0) rectP(10 + i * 14, 46 + 9 * (1 - fl), 10, 9 * fl, RGB(255, 48, 105));
  }
  if (hud.comboKills > 0) {
    rectP(10, 58, 100 * clampf(hud.comboTimer / COMBO_DUR, 0, 1), 4, RGB(255, 48, 105));
    snprintf(num, sizeof(num), "COMBO %d", hud.comboKills); textP(10, 66, num, RGB(255, 215, 115), 1);
  }
  if (hud.nb > 0 && hud.feedAlpha > 0.02f) {
    int a = (int)(clampf(hud.feedAlpha, 0, 1) * 255);
    for (i = 0; i < hud.nb; i++) {
      snprintf(num, sizeof(num), "%d %s", hud.bCount[i], hud.bLabel[i]);
      textP(300, 48 + i * 10, num, col(238, 226, 242, a), 1);
    }
  }
  if (hasInteracted == 0 && hintTimer > 0 && started) {
    textP(60, 36 + 24, "CROSS HOLD LOWER   CIRCLE R SLAM", RGB(242, 232, 247), 1);
  }
  /* cursor */
  {
    float cx = mouse.sx * K, cy = mouse.sy * K; unsigned int cc = (mouse.lmb || mouse.rmb) ? RGB(255, 126, 184) : RGB(255, 255, 255);
    rectP(cx - 6, cy - 1, 4, 2, cc); rectP(cx + 3, cy - 1, 4, 2, cc); rectP(cx - 1, cy - 6, 2, 4, cc); rectP(cx - 1, cy + 3, 2, 4, cc); rectP(cx - 1, cy - 1, 2, 2, cc);
  }
  if (muted) textP(440, 262, "MUTE", RGB(165, 145, 174), 1);
  if (paused) {
    rectP(150, 105, 180, 55, hudbg);
    textP(206, 119, "PAUSED", RGB(238, 226, 242), 1);
    textP(168, 139, "START TO RESUME", RGB(255, 105, 165), 1);
  }
  if (!started) {
    rectP(36, 52, 408, 170, hudbg); rectP(36, 52, 408, 3, RGB(255, 105, 165));
    textP(91, 64, "DUSK CRUSHER", RGB(238, 226, 242), 2);
    textP(150, 86, "GOTHIC ARCADE", RGB(255, 105, 165), 1);
    textP(56, 108, "START  BEGIN OR PAUSE", RGB(255, 215, 115), 1);
    textP(56, 122, "STICK DPAD  MOVE THE FOOT AIM", RGB(238, 226, 242), 1);
    textP(56, 134, "CROSS HOLD  LOWER AND SMEAR", RGB(238, 226, 242), 1);
    textP(56, 146, "CROSS TAP IN AIR  SLAM", RGB(238, 226, 242), 1);
    textP(56, 158, "CIRCLE OR R  FULL PRESSURE", RGB(238, 226, 242), 1);
    textP(56, 170, "L AND TRIANGLE  TILT   SQUARE  LEVEL", RGB(238, 226, 242), 1);
    textP(56, 184, "SELECT  SOUND ON OFF", RGB(165, 145, 174), 1);
  }
}
static void drawWorldA(void) {
  int i;
  useZoom = 0; background(); useZoom = 1;
  for (i = 0; i < stainN; i++) { Stain *s = &stains[i]; int a = (int)(clampf(s->life / 2.5f, 0, 1) * 235); rectW(s->x, s->y, s->w, s->h, col(5, 5, 8, a)); }
  for (i = 0; i < waveN; i++) { Wave *w = &waves[i]; int a = (int)(clampf(w->life / 0.45f, 0, 1) * 170); rectW(w->x - w->r, w->y - 3, w->r * 2, 5, col(150, 130, 160, a)); }
  drawTrash();
  footShadow(&backFoot);
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
  char m[400]; int i, loaded = 0;
  SceCtrlData pad; unsigned int prevBtn = 0; unsigned long long last; unsigned int frameNo = 0;
  FILE *lf = fopen("ms0:/DUSKLOG.TXT", "wb"); if (lf) fclose(lf);
  setup_cb();
  pspDebugScreenInit();
  say("DUSK CRUSHER (C build, stage 1)");
  if (argc > 0 && argv && argv[0]) {
    char *sl; strncpy(base, argv[0], sizeof(base) - 1); sl = strrchr(base, '/'); if (sl) sl[1] = 0; else base[0] = 0;
  }
  snprintf(m, sizeof(m), "base: %s", base); say(m);
  for (i = 0; i < NPOSE; i++) {
    char p[64]; snprintf(p, sizeof(p), "assets/boot_%02d.rgba", i);
    bootOk[i] = (loadFile(ap(p), bootTex[i], 256 * 256 * 4) == 256 * 256 * 4);
    if (bootOk[i]) { sceKernelDcacheWritebackRange(bootTex[i], 256 * 256 * 4); loaded++; }
  }
  snprintf(m, sizeof(m), "boot sprites: %d/%d", loaded, NPOSE); say(m);
  thudN = loadFile(ap("assets/thud.pcm"), thudPCM, PCM_MAX * 4); thudN = thudN > 0 ? thudN / 4 : 0;
  crunchN = loadFile(ap("assets/crunch.pcm"), crunchPCM, PCM_MAX * 4); crunchN = crunchN > 0 ? crunchN / 4 : 0;
  snprintf(m, sizeof(m), "sounds: thud=%d crunch=%d", thudN, crunchN); say(m);
  sceKernelDelayThread(1200000);
  say("starting graphics..."); sceKernelDelayThread(400000);
  gu_init(); logf_("GU ready");
  audioCh = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 1024, PSP_AUDIO_FORMAT_STEREO);
  if (audioCh >= 0) {
    audioRun = 1; audioThread = sceKernelCreateThread("audio", audioMain, 0x12, 0x10000, 0, NULL);
    if (audioThread >= 0) sceKernelStartThread(audioThread, 0, NULL); else audioRun = 0;
  }
  srand((unsigned)sceKernelGetSystemTimeWide());
  sceCtrlSetSamplingCycle(0); sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
  memset(&pad, 0, sizeof(pad)); pad.Lx = 128; pad.Ly = 128;
  resetGame(); started = 0;
  last = sceKernelGetSystemTimeWide(); logf_("main loop");

  for (;;) {
    unsigned long long now; float dt; unsigned int pressed, released;
    sceCtrlReadBufferPositive(&pad, 1);
    now = sceKernelGetSystemTimeWide(); dt = (float)(now - last) / 1000000.0f; last = now;
    if (dt < 0.008f) dt = 0.008f; if (dt > 0.05f) dt = 0.05f;
    pressed = pad.Buttons & ~prevBtn; released = ~pad.Buttons & prevBtn; prevBtn = pad.Buttons;
    if (pressed & PSP_CTRL_START) { if (!started) { resetGame(); } else paused = !paused; }
    if (pressed & PSP_CTRL_SELECT) muted = !muted;
    if (started && !paused) engineUpdate(dt, &pad, pressed, released);
    else if (!started) { time_ += dt; gTime = time_; enemiesUpdate(dt, &frontFoot, &backFoot, 0.8f); footUpdate(&frontFoot, dt, 0, time_); footBackSupport(&backFoot, dt, 0); }

    shakeX = shakeMag > 0.2f ? rnd(-shakeMag, shakeMag) : 0; shakeY = shakeMag > 0.2f ? rnd(-shakeMag, shakeMag) : 0;
    /* pass A: backdrop, stains, trash, back-foot shadow */
    cur = vbA; curN = &nA; nA = 0; drawWorldA();
    /* pass B: back-foot goo rim, ants, front-foot shadow */
    cur = vbB; curN = &nB; nB = 0;
    footGooRim(&backFoot);
    drawAnts();
    footShadow(&frontFoot);
    /* pass C: front-foot goo rim, particles, popups, HUD */
    cur = vbC; curN = &nC; nC = 0;
    footGooRim(&frontFoot);
    { int k; for (k = 0; k < partN; k++) rectW(parts[k].x - parts[k].r * 0.5f, parts[k].y - parts[k].r * 0.5f, parts[k].r, parts[k].r, col(5, 5, 8, (int)(clampf(parts[k].life / (parts[k].maxLife * 0.4f), 0, 1) * 255))); }
    useZoom = 1; drawHudAndOverlays();

    sceGuStart(GU_DIRECT, list);
    sceGuClearColor(0xff0e0508);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    flush(vbA, nA);
    footDraw(&backFoot, 0);
    flush(vbB, nB);
    footDraw(&frontFoot, 1);
    flush(vbC, nC);
    sceGuFinish(); sceGuSync(0, 0); sceDisplayWaitVblankStart(); sceGuSwapBuffers();
    frameNo++;
    if (frameNo == 1 || frameNo == 60 || frameNo == 600) { snprintf(m, sizeof(m), "frame %u ok (A=%d B=%d C=%d)", frameNo, nA / 2, nB / 2, nC / 2); logf_(m); }
  }
  (void)gHeartPower;
  return 0;
}
