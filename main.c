/* DUSK CRUSHER - native PSP port in plain C (PSPSDK)
 * Rendering: all coloured rectangles are collected into static vertex
 * buffers and drawn with a few sceGuDrawArray calls per frame. */
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

PSP_MODULE_INFO("DUSKCRUSHER", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define SW 480
#define SH 272
#define MAX_ANTS 70
#define MAX_PART 48
#define MAX_SPLAT 60
#define MAX_RING 8
#define MAXV 12000            /* vertices per batch buffer (2 per rect) */
#define GROUND 157.0f
#define ANT_Y1 206.0f
#define ANT_Y2 246.0f

typedef struct { unsigned int c; float x, y, z; } VC;
typedef struct { float u, v; unsigned int c; float x, y, z; } VT;

static unsigned int __attribute__((aligned(16))) list[65536];
static VC __attribute__((aligned(16))) vbA[MAXV];
static VC __attribute__((aligned(16))) vbB[MAXV];
static VT __attribute__((aligned(16))) tv[2];
static unsigned int __attribute__((aligned(16))) bootTex[11][64 * 128];
static int bootOk[11];
#define PCM_MAX 30000
static short __attribute__((aligned(64))) thudPCM[PCM_MAX * 2];
static short __attribute__((aligned(64))) crunchPCM[PCM_MAX * 2];
static int thudN = 0, crunchN = 0;

static char base[256] = "";
static VC *cur = vbA;
static int *curN;
static int nA = 0, nB = 0;
static float ox = 0, oy = 0;

/* ---------- helpers ---------- */
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float lerpf(float a, float b, float t) { return a + (b - a) * clampf(t, 0, 1); }
static float rnd(float a, float b) { return a + (float)rand() / (float)RAND_MAX * (b - a); }
static int rndi(int a, int b) { return a + rand() % (b - a + 1); }
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

/* ---------- batched drawing ---------- */
static void rect(float x, float y, float w, float h, unsigned int c) {
  VC *v;
  if (*curN > MAXV - 2) return;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  x = floorf(x + ox); y = floorf(y + oy); w = floorf(w); h = floorf(h);
  v = cur + *curN;
  v[0].c = c; v[0].x = x;     v[0].y = y;     v[0].z = 0;
  v[1].c = c; v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
  *curN += 2;
}
static void flush(VC *buf, int n) {
  if (n <= 0) return;
  sceKernelDcacheWritebackRange(buf, n * sizeof(VC));
  sceGuDisable(GU_TEXTURE_2D);
  sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, n, 0, buf);
}
static void sprite(unsigned int *tex, float x, float y, int tw, int th) {
  tv[0].u = 0; tv[0].v = 0; tv[0].c = 0xffffffff; tv[0].x = floorf(x + ox); tv[0].y = floorf(y + oy); tv[0].z = 0;
  tv[1].u = (float)tw; tv[1].v = (float)th; tv[1].c = 0xffffffff;
  tv[1].x = floorf(x + ox) + tw; tv[1].y = floorf(y + oy) + th; tv[1].z = 0;
  sceKernelDcacheWritebackRange(tv, sizeof(tv));
  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexMode(GU_PSM_8888, 0, 0, 0);
  sceGuTexImage(0, tw, th, tw, tex);
  sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
  sceGuTexFilter(GU_NEAREST, GU_NEAREST);
  sceGuTexWrap(GU_CLAMP, GU_CLAMP);
  sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, tv);
  sceGuDisable(GU_TEXTURE_2D);
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
static void text(float x, float y, const char *s, unsigned int c, int sc) {
  for (; *s; s++, x += 6 * sc) {
    int id = 0, yy, xx;
    char ch = *s;
    if (ch >= '0' && ch <= '9') id = 1 + ch - '0';
    else if (ch >= 'A' && ch <= 'Z') id = 11 + ch - 'A';
    else if (ch >= 'a' && ch <= 'z') id = 11 + ch - 'a';
    if (id == 0) continue;
    for (yy = 0; yy < 7; yy++)
      for (xx = 0; xx < 5; xx++)
        if (glyph[id][yy] & (1 << (4 - xx))) rect(x + xx * sc, y + yy * sc, sc, sc, c);
  }
}

/* ---------- audio (mixer thread, never blocks the game) ---------- */
typedef struct { short *p; volatile int pos, len; } Voice;
static Voice voices[8];
static volatile int audioRun = 0;
static int audioCh = -1, audioThread = -1;
static int muted = 0;

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

/* ---------- game state ---------- */
typedef struct { float x, y, spd, dir, phase, hp, w, h; int pinned, dead, panic; } Ant;
typedef struct { float x, y, vx, vy, life, r; } Part;
typedef struct { float x, y, r, life; } Splat;
typedef struct { float x, y, r, life; int big; } Ring;
static Ant ants[MAX_ANTS]; static int antN = 0;
static Part parts[MAX_PART]; static int partN = 0;
static Splat splats[MAX_SPLAT]; static int splatN = 0;
static Ring rings[MAX_RING]; static int ringN = 0;
static int score = 0, display = 0, totalKills = 0, combo = 0;
static float comboT = 0, hype = 0, seriesFade = 0, spawnT = 0, tNow = 0, lastCrunch = -1, shake = 0;
static float footX = 235, footY = 70, footV = 0, footVx = 0, ty = 70, recoil = 0, squash = 0, poseT = 0;
static int pose = 1, wasGrounded = 0, fullPress = 0, started = 0, paused = 0;

static int multiplier(void) {
  if (combo >= 420) return 4;
  if (combo >= 260) return 3;
  if (combo >= 140) return 2;
  if (combo >= 55) return 1;
  return 0;
}
static void spawnAnt(float x, int useX) {
  Ant *a;
  int fromLeft = rand() % 2;
  if (antN >= MAX_ANTS) return;
  a = &ants[antN++];
  memset(a, 0, sizeof(*a));
  a->dir = useX ? ((rand() % 2) ? 1.0f : -1.0f) : (fromLeft ? 1.0f : -1.0f);
  a->x = useX ? x : (fromLeft ? -10.0f : (float)SW + 10.0f);
  a->y = rnd(ANT_Y1 + 6, ANT_Y2 - 4);
  a->spd = rnd(34, 54);
  a->phase = rnd(0, 6.28f);
  a->hp = 1; a->w = rnd(13, 17); a->h = rnd(22, 29);
}
static void puff(float x, float y, int n, float power) {
  int i;
  for (i = 0; i < n && partN < MAX_PART; i++) {
    float a = rnd(-3.14f, 0), v = rnd(20, 100) * power;
    Part *p = &parts[partN++];
    p->x = x; p->y = y; p->vx = cosf(a) * v + rnd(-20, 20); p->vy = sinf(a) * v - rnd(10, 55);
    p->life = rnd(0.22f, 0.6f); p->r = rnd(1, 3);
  }
}
static void addSplat(float x, float y, int n) {
  int i;
  for (i = 0; i < n; i++) {
    Splat *s;
    if (splatN >= MAX_SPLAT) { memmove(&splats[0], &splats[1], sizeof(Splat) * (MAX_SPLAT - 1)); splatN = MAX_SPLAT - 1; }
    s = &splats[splatN++];
    s->x = x + rnd(-12, 12); s->y = y + rnd(-2, 5); s->r = rnd(1, 4); s->life = rnd(3, 6);
  }
}
static void addRing(float x, float y, int big) {
  if (ringN < MAX_RING) { rings[ringN].x = x; rings[ringN].y = y; rings[ringN].r = 4; rings[ringN].life = 0.3f; rings[ringN].big = big; ringN++; }
}
static void resetGame(void) {
  int i;
  antN = partN = splatN = ringN = 0;
  score = display = totalKills = combo = 0;
  comboT = hype = seriesFade = 0;
  footX = 235; footY = 70; footV = footVx = 0; ty = 70; pose = 1; spawnT = 0.3f;
  recoil = squash = shake = 0; wasGrounded = 0;
  for (i = 0; i < 36; i++) spawnAnt(rnd(20, 460), 1);
  started = 1; paused = 0;
}
static void killAnt(Ant *a, int full, int smearing) {
  int pts;
  a->dead = 1; totalKills++; combo++; comboT = 2.75f;
  pts = (int)((full ? 11 : 6) + multiplier() * 2.2f + 0.5f);
  if (pts < 6) pts = 6;
  if (pts > 15) pts = 15;
  score += pts; seriesFade = 3;
  hype = fminf(5, hype + (smearing ? 0.014f : 0.010f));
  addSplat(a->x, a->y + 2, full ? 4 : 3);
  puff(a->x, a->y, full ? 6 : 3, full ? 1.3f : 0.8f);
  if (tNow - lastCrunch > 0.11f) { lastCrunch = tNow; playPCM(crunchPCM, crunchN); }
  shake = fmaxf(shake, full ? 4.0f : 2.0f);
}
enum { M_PIN, M_SOFT, M_FULL, M_SMEAR };
static void contact(int mode, float dt) {
  int i;
  float reach = (mode == M_PIN) ? 6.0f : 3.0f;
  float minX = footX - 26 - reach, maxX = footX + 26 + reach;
  float grind = (0.48f + fabsf(footVx) * 0.012f) * (1 + (hype / 5) * 0.95f) * dt;
  for (i = 0; i < antN; i++) {
    Ant *a = &ants[i];
    if (a->dead || a->x < minX || a->x > maxX || a->y < ANT_Y1 - 2 || a->y > ANT_Y2 + 4) continue;
    if (mode == M_SMEAR) {
      float g = grind * rnd(0.75f, 1.35f);
      a->pinned = 1;
      a->hp -= (g > 0.01f ? g : 0.01f);
      if (rand() % 100 < 30) addSplat(a->x, a->y + 3, 1);
      if (a->hp <= 0) killAnt(a, 0, 1);
    } else {
      killAnt(a, mode == M_FULL, 0);
    }
  }
}
static void sweepDead(void) {
  int i, j = 0;
  for (i = 0; i < antN; i++) if (!ants[i].dead) ants[j++] = ants[i];
  antN = j;
}

static void update(float dt, const SceCtrlData *pad) {
  int i, j, slam, slow, full, lift, grounded, nxt;
  float sx = 0, sy = 0, oldX, oldY, k, acc, speedMult;
  tNow += dt;
  shake = fmaxf(0, shake - dt * 14);
  if (combo > 0) { comboT -= dt; if (comboT <= 0) combo = 0; }
  if (combo == 0 && hype > 0) hype = fmaxf(0, hype - dt * 0.055f);
  if (combo == 0 && seriesFade > 0) { seriesFade -= dt; if (seriesFade <= 0) { score = 0; display = 0; } }
  if (display < score) { int d = score - display; int step = (int)ceilf(d * fminf(1, dt * 16)); display += step < 1 ? 1 : step; }

  /* input */
  if (pad->Lx < 90) sx = clampf(((float)pad->Lx - 128) / 100.0f, -1, 1);
  else if (pad->Lx > 166) sx = clampf(((float)pad->Lx - 128) / 100.0f, -1, 1);
  if (pad->Ly < 90) sy = clampf(((float)pad->Ly - 128) / 100.0f, -1, 1);
  else if (pad->Ly > 166) sy = clampf(((float)pad->Ly - 128) / 100.0f, -1, 1);
  if (pad->Buttons & PSP_CTRL_LEFT) sx = -1;
  if (pad->Buttons & PSP_CTRL_RIGHT) sx = 1;
  if (pad->Buttons & PSP_CTRL_UP) sy = -1;
  if (pad->Buttons & PSP_CTRL_DOWN) sy = 1;
  slam = (pad->Buttons & PSP_CTRL_CROSS) != 0;
  slow = (pad->Buttons & PSP_CTRL_SQUARE) != 0;
  full = (pad->Buttons & (PSP_CTRL_RTRIGGER | PSP_CTRL_CIRCLE)) != 0;
  lift = (pad->Buttons & (PSP_CTRL_TRIANGLE | PSP_CTRL_LTRIGGER)) != 0;

  oldX = footX;
  footX = clampf(footX + sx * 200 * dt, 34, 446);
  footVx = (footX - oldX) / fmaxf(dt, 0.001f);

  if (lift) ty = 48;
  else if (slam || full || slow) ty = GROUND;
  else ty = clampf(ty + sy * 130 * dt, 48, GROUND - 4);

  k = (slam || full) ? 40.0f : (slow ? 14.0f : 10.0f);
  oldY = footY; (void)oldY;
  acc = (ty - footY) * k - footV * 2 * sqrtf(k);
  footV += acc * dt;
  footY = clampf(footY + footV * dt, 45, GROUND + 2);
  grounded = footY >= GROUND - 4;
  fullPress = grounded && full;

  if (grounded && !wasGrounded) {
    float impact = clampf(footV / 280.0f, 0.35f, 1.5f);
    recoil = 5; squash = 0.9f;
    puff(footX, 244, 12, 1.2f); addRing(footX, 246, fullPress);
    shake = fmaxf(shake, impact * (fullPress ? 7.0f : 4.0f));
    playPCM(thudPCM, thudN);
    if (fullPress) contact(M_FULL, dt);
    else if (footV > 230) contact(M_SOFT, dt);
    else contact(M_PIN, dt);
  }
  wasGrounded = grounded;
  if (grounded) {
    if (fullPress) contact(M_FULL, dt);
    else if (slow || fabsf(footVx) > 14) contact(M_SMEAR, dt);
    else contact(M_PIN, dt);
  } else if (footY >= GROUND - 38) {
    contact(M_PIN, dt);
  }

  /* pose */
  nxt = 1;
  if (!grounded) { float h = GROUND - footY; nxt = h > 80 ? 5 : (h > 25 ? 4 : 1); }
  else if (full) nxt = 7;
  else if (slam) nxt = 8;
  else if (slow) nxt = (sinf(tNow * 8) > 0) ? 10 : 11;
  else if (fabsf(footV) > 35) nxt = 8;
  else { float q = sinf(tNow * 2.4f); nxt = q > 0.72f ? 2 : (q < -0.72f ? 3 : 1); }
  if (nxt != pose) { poseT += dt; if (poseT > 0.06f) { pose = nxt; poseT = 0; } } else poseT = 0;
  recoil = lerpf(recoil, 0, 9 * dt);
  squash = fmaxf(0, squash - dt * 3.5f);

  /* ants */
  speedMult = 0.58f + multiplier() * 0.355f;
  spawnT -= dt;
  if (spawnT <= 0 && antN < MAX_ANTS - 8) {
    float side = (rand() % 2) ? -10.0f : 490.0f;
    int wave = rand() % 100 < 78, n = rndi(2, 5);
    for (i = 1; i <= n; i++) {
      if (wave) spawnAnt(side + (side < 0 ? -i * 8.0f : i * 8.0f), 1); else spawnAnt(0, 0);
    }
    spawnT = (antN < 40) ? rnd(0.3f, 0.55f) : rnd(0.6f, 1.1f);
  }
  for (i = 0; i < antN; i++) {
    Ant *a = &ants[i];
    if (a->dead) continue;
    if (a->pinned) { a->phase += dt * 30; a->pinned = 0; }
    else {
      int panic = fabsf(a->x - footX) < 70 && footY > 70;
      float d = a->dir;
      if (panic) d = (a->x >= footX) ? 1.0f : -1.0f;
      a->x += d * a->spd * (panic ? 2.1f : 1.0f) * speedMult * dt;
      a->phase += dt * (panic ? 22 : 11);
      a->panic = panic;
    }
    if (a->x < -26 || a->x > SW + 26) a->dead = 1;
  }
  sweepDead();

  for (i = 0, j = 0; i < partN; i++) {
    Part p = parts[i];
    p.life -= dt; p.vy += 115 * dt; p.x += p.vx * dt; p.y += p.vy * dt;
    if (p.life > 0) parts[j++] = p;
  }
  partN = j;
  for (i = 0, j = 0; i < splatN; i++) { Splat s = splats[i]; s.life -= dt; if (s.life > 0) splats[j++] = s; }
  splatN = j;
  for (i = 0, j = 0; i < ringN; i++) { Ring r = rings[i]; r.life -= dt; r.r += dt * 155; if (r.life > 0) rings[j++] = r; }
  ringN = j;
}

/* ---------- drawing ---------- */
static void background(void) {
  static const int b[8][4] = {{0,80,55,125},{54,110,62,95},{115,58,48,147},{162,95,67,110},
                              {229,72,61,133},{290,112,58,93},{348,65,67,140},{415,90,65,115}};
  int i, x, y;
  rect(-8, -8, SW + 16, 213, RGB(26, 13, 34));
  rect(0, 150, SW, 55, RGB(48, 24, 52));
  rect(0, 175, SW, 30, RGB(70, 32, 66));
  for (i = 0; i < 8; i++) {
    rect(b[i][0], b[i][1], b[i][2], b[i][3], RGB(48, 25, 55));
    rect(b[i][0], b[i][1], b[i][2], 3, RGB(111, 65, 125));
    for (y = b[i][1] + 13; y < 195; y += 20)
      for (x = b[i][0] + 7; x < b[i][0] + b[i][2] - 8; x += 15)
        if ((x + y + i * 7) % 4)
          rect(x, y, 5, 5, ((x + y) % 3 == 0) ? RGB(48, 190, 164) : RGB(255, 105, 165));
  }
  rect(-8, 202, SW + 16, 56, RGB(35, 35, 47));
  rect(-8, 202, SW + 16, 2, RGB(19, 16, 28));
  for (i = 1; i <= 52; i++) rect((i * 71) % SW, 207 + (i * 19) % 39, 2, 1, RGB(43, 42, 56));
  rect(-8, 251, SW + 16, 30, RGB(105, 111, 132));
  for (x = 0; x < SW; x += 24) { rect(x, 254, 12, 18, RGB(213, 215, 226)); rect(x + 12, 254, 12, 18, RGB(65, 68, 84)); }
}
static void drawAnt(const Ant *a) {
  float x = a->x, y = a->y, w = a->w, h = a->h, bob, top, sw;
  unsigned int cAnt = RGB(17, 11, 20), cLeg = RGB(74, 46, 68), cEye = RGB(255, 105, 165);
  if (a->pinned || a->hp < 1) {
    float sq = lerpf(0.3f, 0.9f, a->hp), ww = w * (1.15f + (1 - a->hp) * 0.55f), hh = h * sq * 0.5f;
    rect(x - ww / 2, y - hh, ww, hh, cAnt);
    rect(x - ww / 2 - 2, y - hh + 1, 3, 2, cLeg);
    rect(x + ww / 2 - 1, y - hh + 1, 3, 2, cLeg);
    rect(x - 1, y - hh + 1, 2, 1, cEye);
    return;
  }
  bob = fabsf(sinf(a->phase)) * (a->panic ? 2.5f : 1.2f);
  top = y - h - bob;
  sw = sinf(a->phase) * 2;
  rect(x - w * 0.3f + sw, y - h * 0.2f - bob, 3, h * 0.2f, cAnt);
  rect(x + w * 0.3f - 3 - sw, y - h * 0.2f - bob, 3, h * 0.2f, cAnt);
  rect(x - w / 2, top + h * 0.28f, w, h * 0.52f, cAnt);
  rect(x - w * 0.34f, top, w * 0.68f, h * 0.3f, cLeg);
  rect(x - w * 0.14f, top + h * 0.1f, 2, 2, cEye);
  if (a->panic) { rect(x - w / 2 - 2, top + h * 0.3f + sw, 2, 5, cLeg); rect(x + w / 2, top + h * 0.3f - sw, 2, 5, cLeg); }
}
static void drawScene(void) {
  int i;
  char num[48];
  unsigned int hudbg = col(17, 11, 25, 215);
  ox = shake > 0.3f ? rnd(-shake, shake) : 0;
  oy = shake > 0.3f ? rnd(-shake, shake) : 0;

  /* layer A: world */
  cur = vbA; curN = &nA; nA = 0;
  background();
  for (i = 0; i < splatN; i++) rect(splats[i].x, splats[i].y, splats[i].r * 2, splats[i].r, RGB(20, 9, 22));
  for (i = 0; i < ringN; i++) rect(rings[i].x - rings[i].r, rings[i].y - rings[i].r * 0.12f, rings[i].r * 2, 2, rings[i].big ? RGB(255, 105, 165) : RGB(153, 126, 158));
  for (i = 0; i < partN; i++) rect(parts[i].x, parts[i].y, parts[i].r, parts[i].r, RGB(153, 126, 158));
  for (i = 0; i < antN; i++) if (!ants[i].dead) drawAnt(&ants[i]);
  rect(footX - 34, 244, 68, 2, RGB(12, 7, 18));

  /* layer B: HUD (drawn after the boot sprite) */
  cur = vbB; curN = &nB; nB = 0;
  rect(6, 6, 160, 34, hudbg); rect(6, 6, 160, 2, RGB(255, 105, 165));
  text(12, 12, "DUSK CRUSHER", RGB(238, 226, 242), 1);
  snprintf(num, sizeof(num), "SCORE %d", display); text(12, 25, num, RGB(255, 215, 115), 1);
  rect(300, 6, 118, 34, hudbg); rect(300, 6, 118, 2, RGB(48, 190, 164));
  snprintf(num, sizeof(num), "KILLS %d", totalKills); text(306, 12, num, RGB(238, 226, 242), 1);
  for (i = 1; i <= 4; i++) rect(306 + (i - 1) * 12, 28, 8, 8, i <= multiplier() ? RGB(255, 105, 165) : RGB(48, 32, 56));
  for (i = 0; i < 5; i++) {
    float f = clampf(hype - i, 0, 1);
    rect(10 + i * 14, 46, 10, 9, RGB(48, 32, 56));
    if (f > 0) rect(10 + i * 14, 46 + 9 * (1 - f), 10, 9 * f, RGB(255, 48, 105));
  }
  if (combo > 0) {
    rect(10, 58, 100 * clampf(comboT / 2.75f, 0, 1), 4, RGB(255, 48, 105));
    snprintf(num, sizeof(num), "CHAIN +%d", combo); text(10, 66, num, RGB(255, 215, 115), 1);
  }
  if (muted) text(SW - 40, SH - 12, "MUTE", RGB(165, 145, 174), 1);
  if (paused) {
    rect(150, 105, 180, 55, hudbg);
    text(206, 119, "PAUSED", RGB(238, 226, 242), 1);
    text(168, 139, "START TO RESUME", RGB(255, 105, 165), 1);
  }
  if (!started) {
    rect(48, 62, 384, 148, hudbg); rect(48, 62, 384, 3, RGB(255, 105, 165));
    text(91, 76, "DUSK CRUSHER", RGB(238, 226, 242), 2);
    text(150, 98, "GOTHIC ARCADE", RGB(255, 105, 165), 1);
    text(76, 120, "START  BEGIN OR PAUSE", RGB(255, 215, 115), 1);
    text(76, 134, "STICK DPAD  MOVE RAISE LOWER", RGB(238, 226, 242), 1);
    text(76, 146, "CROSS SLAM   SQUARE GRIND", RGB(238, 226, 242), 1);
    text(76, 158, "R OR CIRCLE  FULL PRESSURE", RGB(238, 226, 242), 1);
    text(76, 170, "TRIANGLE OR L  LIFT FOOT", RGB(238, 226, 242), 1);
    text(76, 184, "SELECT  SOUND ON OFF", RGB(165, 145, 174), 1);
  }
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
  FILE *f = fopen(path, "rb");
  int n;
  if (!f) return -1;
  n = (int)fread(dst, 1, bytes, f);
  fclose(f);
  return n;
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
  char m[400];
  int i, loaded = 0;
  SceCtrlData pad;
  unsigned int prevBtn = 0;
  unsigned long long last;
  unsigned int frameNo = 0;
  FILE *lf = fopen("ms0:/DUSKLOG.TXT", "wb"); if (lf) fclose(lf);

  setup_cb();
  pspDebugScreenInit();
  say("DUSK CRUSHER (C build)");
  if (argc > 0 && argv && argv[0]) {
    char *sl;
    strncpy(base, argv[0], sizeof(base) - 1);
    sl = strrchr(base, '/');
    if (sl) sl[1] = 0; else base[0] = 0;
  }
  snprintf(m, sizeof(m), "base: %s", base); say(m);

  for (i = 0; i < 11; i++) {
    char p[64];
    snprintf(p, sizeof(p), "assets/boot_%02d.rgba", i + 1);
    bootOk[i] = (loadFile(ap(p), bootTex[i], 64 * 128 * 4) == 64 * 128 * 4);
    if (bootOk[i]) { sceKernelDcacheWritebackRange(bootTex[i], 64 * 128 * 4); loaded++; }
  }
  snprintf(m, sizeof(m), "boot sprites: %d/11", loaded); say(m);
  thudN = loadFile(ap("assets/thud.pcm"), thudPCM, PCM_MAX * 4); thudN = thudN > 0 ? thudN / 4 : 0;
  crunchN = loadFile(ap("assets/crunch.pcm"), crunchPCM, PCM_MAX * 4); crunchN = crunchN > 0 ? crunchN / 4 : 0;
  snprintf(m, sizeof(m), "sounds: thud=%d crunch=%d samples", thudN, crunchN); say(m);
  sceKernelDelayThread(1200000);

  say("starting graphics...");
  sceKernelDelayThread(500000);
  gu_init();
  logf_("GU ready");

  audioCh = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, 1024, PSP_AUDIO_FORMAT_STEREO);
  snprintf(m, sizeof(m), "audio channel %d", audioCh); logf_(m);
  if (audioCh >= 0) {
    audioRun = 1;
    audioThread = sceKernelCreateThread("audio", audioMain, 0x12, 0x10000, 0, NULL);
    if (audioThread >= 0) sceKernelStartThread(audioThread, 0, NULL); else audioRun = 0;
  }

  srand((unsigned)sceKernelGetSystemTimeWide());
  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
  memset(&pad, 0, sizeof(pad));
  pad.Lx = 128; pad.Ly = 128;
  last = sceKernelGetSystemTimeWide();
  logf_("main loop");

  for (;;) {
    unsigned long long now;
    float dt;
    unsigned int pressed;
    sceCtrlReadBufferPositive(&pad, 1);
    now = sceKernelGetSystemTimeWide();
    dt = (float)(now - last) / 1000000.0f;
    last = now;
    if (dt < 0.008f) dt = 0.008f;
    if (dt > 0.05f) dt = 0.05f;
    pressed = pad.Buttons & ~prevBtn;
    prevBtn = pad.Buttons;
    if (pressed & PSP_CTRL_START) { if (!started) resetGame(); else paused = !paused; }
    if (pressed & PSP_CTRL_SELECT) muted = !muted;
    if (started && !paused) update(dt, &pad);

    drawScene();

    sceGuStart(GU_DIRECT, list);
    sceGuClearColor(0xff0e0508);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    flush(vbA, nA);
    if (bootOk[pose - 1]) sprite(bootTex[pose - 1], footX - 32, footY - recoil + squash * 2, 64, 128);
    flush(vbB, nB);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();

    frameNo++;
    if (frameNo == 1 || frameNo == 60 || frameNo == 600) { snprintf(m, sizeof(m), "frame %u ok (rectsA=%d rectsB=%d)", frameNo, nA / 2, nB / 2); logf_(m); }
  }
  return 0;
}
