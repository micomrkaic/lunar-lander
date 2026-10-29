/* lander.c — Lunar Lander starring Eagle the Fat Cat.
 * C17 + SDL2, single file, Emscripten-ready.
 *
 * Copyright (C) 2026  Mico Mrkaic
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Native build:  make
 *   (= cc -std=c17 -O2 -Wall -Wextra lander.c -o lander \
 *        $(sdl2-config --cflags --libs) -lm)
 *
 * WASM build:    make wasm      (needs the Emscripten SDK)
 *   (= emcc -std=gnu17 -O2 lander.c -o index.html --shell-file shell.html \
 *        -s USE_SDL=2 -s INITIAL_MEMORY=67108864 -lidbfs.js)
 *   gnu17 because EM_ASM needs GNU statement expressions; fixed heap
 *   because growable memory breaks TextDecoder (see Makefile). High
 *   scores persist in IndexedDB via an IDBFS mount at /data. Audio
 *   starts after the first keypress, which the title screen guarantees.
 *
 * Tests:         make test      (see README)
 *
 * Controls: LEFT/RIGHT (A/D) rotate, UP (W) thrust, P pause,
 *           ESC end flight, R next level / retry,
 *           SPACE start a run, D start today's daily run.
 * Sound is synthesized in the audio callback — no assets, no SDL_mixer.
 */

#include <SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ---------------------------------------------------------------- config */

enum { W = 960, H = 640 };

#define PHYS_HZ        120      /* fixed physics rate; render runs freely  */
#define PHYS_DT        (1.0f / PHYS_HZ)
#define GRAVITY        22.0f    /* px/s^2 at level 1                       */
#define THRUST_ACC     55.0f    /* px/s^2 at full burn with a full tank    */
#define FUEL_MASS      0.4f     /* full tank adds 40% to Eagle's dry mass  */
#define ROT_SPEED      2.6f     /* rad/s                                   */
#define FUEL_MAX       100.0f
#define FUEL_BURN      12.0f    /* units/s                                 */
#define REFUEL_BASE    20.0f    /* landing refuel: BASE + PER_MULT * mult  */
#define REFUEL_PER_MULT 5.0f
#define LIVES_START    3
#define LIVES_MAX      9        /* he is a cat                             */
#define ZOOM_IN_ALT    90.0f    /* px: zoom 2x below this altitude ...     */
#define ZOOM_OUT_ALT   130.0f   /*   ... back to 1x above this one         */
#define WIND_FROM_LEVEL  3
#define PLANK_FROM_LEVEL 5
#define PLANK_MULT     6        /* moving plank pays the most              */
#define PLANK_HALF     30.0f    /* px half-width                           */
#define PLANK_THICK    4.0f
#define MAX_STEPS      (PHYS_HZ * 240)  /* input recording: 4 minutes      */
#define N_GHOST_LEVELS 16       /* daily ghosts kept for levels 1..16      */
#define SAFE_VY        28.0f    /* px/s max descent for safe landing       */
#define SAFE_VX        18.0f    /* px/s max lateral                        */
#define SAFE_ANGLE     0.20f    /* rad from vertical (~11 deg)             */
#define LEG_X          13.0f    /* foot tips at (+-LEG_X, LEG_Y) in body   */
#define LEG_Y          12.0f    /*   frame; shared by renderer + collision */
#define TERRAIN_N      65       /* must be 2^k + 1 for midpoint displ.     */
#define N_PADS         3
#define N_PARTICLES    256
#define N_STARS        120
#define N_HISCORES     8
#define NAME_MAX       10
#ifndef SCORE_FILE               /* tests override this before #include */
#ifdef __EMSCRIPTEN__
#define SCORE_FILE     "/data/lander_hiscores.txt"   /* IDBFS mount */
#else
#define SCORE_FILE     "lander_hiscores.txt"
#endif
#endif
#ifndef GHOST_FILE
#ifdef __EMSCRIPTEN__
#define GHOST_FILE     "/data/lander_ghost.txt"
#else
#define GHOST_FILE     "lander_ghost.txt"
#endif
#endif
#define AUDIO_SR       48000

/* ----------------------------------------------------------------- types */

typedef struct { float x, y; } Vec2;

typedef struct {
    Vec2  pos, vel;
    float angle;          /* 0 = pointing up; positive = clockwise */
    float fuel;
    bool  thrusting;
} Lander;

typedef struct {
    Vec2  pos, vel;
    float life;
} Particle;

typedef enum {
    ST_TITLE, ST_FLYING, ST_PAUSED, ST_LANDED, ST_CRASHED, ST_ENTRY
} GameState;

typedef struct {
    float x0, x1, y;
    int   mult;
} Pad;

/* floating pad that slides side to side: x(t) = cx + amp*sin(omega*t) */
typedef struct {
    bool  on;
    float cx, y, amp, omega;
} Plank;

/* per-step pilot input, recorded for ghosts */
enum { IN_LEFT = 1, IN_RIGHT = 2, IN_THRUST = 4 };

typedef enum { CT_NONE, CT_LAND, CT_CRASH } Contact;

/* a daily-mode ghost: the best landing on one level today */
typedef struct {
    bool    valid;
    float   fuel;          /* fuel at the start of that attempt (exact) */
    int     bonus;         /* landing bonus it earned                   */
    int     n;             /* recorded steps                            */
    uint8_t in[MAX_STEPS];
} Ghost;

typedef struct {
    char name[NAME_MAX + 1];
    int  score;
} HiScore;

/* All synth state lives here; the audio callback runs on SDL's audio
 * thread, so the game triggers events only under SDL_LockAudioDevice. */
typedef struct {
    SDL_AudioDeviceID dev;
    bool     thrust_on;    /* set by game each frame                   */
    float    thrust_env;   /* smoothed gain, avoids clicks             */
    float    lp, elp;      /* one-pole low-pass states                 */
    float    expl_t;       /* time into explosion; < 0 = inactive      */
    float    land_t;       /* time into landing chime; < 0 = inactive  */
    uint32_t rng;
} Audio;

typedef struct {
    SDL_Window   *win;
    SDL_Renderer *ren;
    Lander        lander;
    Vec2          terrain[TERRAIN_N];
    Pad           pads[N_PADS];
    Particle      particles[N_PARTICLES];
    Vec2          stars[N_STARS];
    GameState     state;
    int           level;
    float         gravity;
    int           score;
    int           landing_bonus;
    /* run state */
    uint32_t      run_seed;     /* level seeds derive from this          */
    bool          daily;        /* run_seed = today's date               */
    uint32_t      daily_date;   /* YYYYMMDD of a daily run               */
    int           lives;
    float         level_fuel;   /* fuel at level start; restored on retry */
    float         refuel;       /* last landing's refuel, for the banner */
    bool          extra_life;   /* last landing earned a life            */
    /* level state */
    uint32_t      lseed;        /* this level's seed (terrain, wind)     */
    int           step;         /* physics steps since level start       */
    float         step_acc;     /* real time not yet simulated           */
    Plank         plank;
    bool          on_plank;     /* landed on the plank: ride it          */
    float         plank_off;    /* x offset from plank centre when landed */
    float         zoom;         /* camera: 1 = whole moon, 2 = close-up  */
    Vec2          cam;          /* camera centre                          */
    SDL_Texture  *world_tex;    /* world is drawn here, then zoomed       */
    /* input recording and ghost */
    uint8_t       rec[MAX_STEPS];
    int           rec_n;
    Lander        ghost;
    bool          ghost_on;     /* a ghost exists for this level          */
    bool          ghost_flying;
    bool          ghost_on_plank;
    float         ghost_plank_off;
    int           ghost_bonus;
    float         state_time;
    uint64_t      prev_ticks;
    bool          keys[SDL_NUM_SCANCODES];
    bool          running;
    uint32_t      rng;          /* gameplay only: terrain, stars, spawn  */
    uint32_t      fx_rng;       /* cosmetics only: particles, flame      */
    Audio         audio;
    HiScore       hiscores[N_HISCORES];
    int           n_hiscores;
    char          entry_name[NAME_MAX + 1];
    int           entry_len;
} Game;

/* ------------------------------------------------------------------- rng */

static uint32_t xorshift(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}
static float frand(uint32_t *s)              /* [0,1) */
{
    return (float)(xorshift(s) >> 8) / 16777216.0f;
}
static float frange(uint32_t *s, float a, float b)
{
    return a + (b - a) * frand(s);
}

/* stateless integer hash (lowbias32): for seeds and wind, which must be
 * pure functions of (seed, level, time) so replays reproduce exactly  */
static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* every level of a run has its own seed, so a retry rebuilds the same
 * moon and a daily run gives everyone the same sequence of moons      */
static uint32_t level_seed(uint32_t run_seed, int level)
{
    return hash32(run_seed ^ hash32((uint32_t)level * 0x9E3779B9u)) | 1u;
}

/* ------------------------------------------------------------------ wind */

/* Horizontal wind acceleration (px/s^2) at physics step `step`.
 * From WIND_FROM_LEVEL on: a steady breeze from a per-level direction
 * plus gusts — value noise with a knot every 2 s, cosine-blended.
 * |wind| <= wind_max(level). Depends only on (seed, step): no RNG.
 * Capped at 10: full thrust at the legal landing tilt gives
 * 55*sin(0.2) ~ 11 px/s^2 sideways, so touchdown stays holdable.    */
static float wind_max(int level)
{
    if (level < WIND_FROM_LEVEL) return 0.0f;
    float a = 4.0f + 2.0f * (float)(level - WIND_FROM_LEVEL);
    return a > 10.0f ? 10.0f : a;
}

static float wind_noise(uint32_t seed, int k)        /* [-1, 1] */
{
    return (float)(hash32(seed ^ hash32((uint32_t)k + 0x51ED27u)) >> 8)
           / 8388608.0f - 1.0f;
}

static float wind_at(uint32_t seed, int level, int step)
{
    float a = wind_max(level);
    if (a == 0.0f) return 0.0f;
    const int KNOT = 2 * PHYS_HZ;
    int   k = step / KNOT;
    float t = (float)(step % KNOT) / KNOT;
    float s = 0.5f - 0.5f * cosf(3.14159265f * t);
    float gust = wind_noise(seed, k) * (1 - s) + wind_noise(seed, k + 1) * s;
    float dir  = (hash32(seed) & 1u) ? 1.0f : -1.0f;
    return a * (0.4f * dir + 0.6f * gust);
}

/* ----------------------------------------------------------------- audio */

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    Audio *a = ud;
    float *out = (float *)stream;
    int n = len / (int)sizeof(float);

    for (int i = 0; i < n; i++) {
        float s = 0.0f;

        /* thrust: low-passed white noise behind a smoothed envelope */
        float target = a->thrust_on ? 0.55f : 0.0f;
        a->thrust_env += (target - a->thrust_env) * 0.0018f;
        if (a->thrust_env > 0.002f) {
            float w = frand(&a->rng) * 2.0f - 1.0f;
            a->lp += 0.12f * (w - a->lp);
            s += a->lp * a->thrust_env * 2.2f;
        }

        /* explosion: noise burst through a closing low-pass, exp decay */
        if (a->expl_t >= 0.0f) {
            float t = a->expl_t;
            if (t > 2.2f) {
                a->expl_t = -1.0f;
            } else {
                float e = expf(-2.0f * t);
                float w = frand(&a->rng) * 2.0f - 1.0f;
                a->elp += (0.04f + 0.30f * e) * (w - a->elp);
                s += a->elp * e * 2.0f;
                a->expl_t += 1.0f / AUDIO_SR;
            }
        }

        /* touchdown chime: rising three-note arpeggio */
        if (a->land_t >= 0.0f) {
            float t = a->land_t;
            if (t > 0.45f) {
                a->land_t = -1.0f;
            } else {
                static const float freq[3] = { 440.0f, 554.37f, 659.25f };
                int k = (int)(t / 0.15f);
                if (k > 2) k = 2;
                float seg = t - k * 0.15f;
                s += 0.28f * sinf(6.2831853f * freq[k] * t)
                           * expf(-7.0f * seg);
                a->land_t += 1.0f / AUDIO_SR;
            }
        }

        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        out[i] = s;
    }
}

static void audio_init(Audio *a)
{
    a->expl_t = a->land_t = -1.0f;
    a->rng = 0xC0FFEEu;
    SDL_AudioSpec want = {
        .freq     = AUDIO_SR,
        .format   = AUDIO_F32SYS,
        .channels = 1,
        .samples  = 512,
        .callback = audio_cb,
        .userdata = a,
    };
    a->dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (a->dev) SDL_PauseAudioDevice(a->dev, 0);
    /* if no audio device, dev == 0 and the game just runs silent */
}

static void audio_set_thrust(Audio *a, bool on)
{
    if (!a->dev) return;
    SDL_LockAudioDevice(a->dev);
    a->thrust_on = on;
    SDL_UnlockAudioDevice(a->dev);
}

static void audio_trigger_explosion(Audio *a)
{
    if (!a->dev) return;
    SDL_LockAudioDevice(a->dev);
    a->expl_t = 0.0f;
    SDL_UnlockAudioDevice(a->dev);
}

static void audio_trigger_landing(Audio *a)
{
    if (!a->dev) return;
    SDL_LockAudioDevice(a->dev);
    a->land_t = 0.0f;
    SDL_UnlockAudioDevice(a->dev);
}

/* ------------------------------------------------------------ high scores */

/* file-scope so the Emscripten IDBFS callback below can reach it */
static Game g_game;

static void hiscores_load(Game *g)
{
    g->n_hiscores = 0;
    FILE *f = fopen(SCORE_FILE, "r");
    if (!f) return;
    while (g->n_hiscores < N_HISCORES &&
           fscanf(f, "%10s %d",
                  g->hiscores[g->n_hiscores].name,
                  &g->hiscores[g->n_hiscores].score) == 2)
        g->n_hiscores++;
    fclose(f);
}

#ifdef __EMSCRIPTEN__
/* called from JS once the initial IndexedDB -> MEMFS sync completes */
EMSCRIPTEN_KEEPALIVE void hiscores_ready(void)
{
    hiscores_load(&g_game);
}
#endif

static void hiscores_save(const Game *g)
{
    FILE *f = fopen(SCORE_FILE, "w");
    if (!f) return;
    for (int i = 0; i < g->n_hiscores; i++)
        fprintf(f, "%s %d\n", g->hiscores[i].name, g->hiscores[i].score);
    fclose(f);
#ifdef __EMSCRIPTEN__
    /* push MEMFS state out to IndexedDB (async; fire and forget) */
    EM_ASM({ FS.syncfs(false, function(err) {}); });
#endif
}

static bool hiscores_qualifies(const Game *g, int score)
{
    if (score <= 0) return false;
    if (g->n_hiscores < N_HISCORES) return true;
    return score > g->hiscores[g->n_hiscores - 1].score;
}

static void hiscores_insert(Game *g, const char *name, int score)
{
    int pos = g->n_hiscores;
    while (pos > 0 && g->hiscores[pos - 1].score < score) pos--;
    if (pos >= N_HISCORES) return;
    if (g->n_hiscores < N_HISCORES) g->n_hiscores++;
    for (int i = g->n_hiscores - 1; i > pos; i--)
        g->hiscores[i] = g->hiscores[i - 1];
    snprintf(g->hiscores[pos].name, sizeof g->hiscores[pos].name, "%s",
             name[0] ? name : "PILOT");
    g->hiscores[pos].score = score;
    hiscores_save(g);
}

/* --------------------------------------------------------------- terrain */

/* Midpoint-displacement heightmap (TERRAIN_N = 2^k + 1), slope-limited,
 * clamped into the window, then N_PADS segments flattened.
 * Roughness and allowed steepness grow with g->level.                  */
static void gen_terrain(Game *g)
{
    float h[TERRAIN_N];

    float rough = 0.10f + 0.035f * (float)(g->level - 1);
    if (rough > 0.26f) rough = 0.26f;

    /* The world wraps horizontally, so the ground must too: the last
     * vertex IS the first one, and every pass below treats the
     * heightmap as a ring of TERRAIN_N-1 distinct vertices.           */
    h[0]             = frange(&g->rng, 0.60f, 0.85f);
    h[TERRAIN_N - 1] = h[0];

    for (int step = TERRAIN_N - 1; step >= 2; step /= 2) {
        float amp = rough * (float)step / (TERRAIN_N - 1);
        for (int i = 0; i + step < TERRAIN_N; i += step) {
            int mid = i + step / 2;
            h[mid] = 0.5f * (h[i] + h[i + step]) + frange(&g->rng, -amp, amp);
        }
    }

    /* keep every vertex comfortably inside the window */
    for (int i = 0; i < TERRAIN_N; i++) {
        if (h[i] < 0.42f) h[i] = 0.42f;
        if (h[i] > 0.90f) h[i] = 0.90f;
    }

    /* slope limiter: cap rise/run between adjacent vertices so cliffs
     * stay flyable. Vertex spacing is W/(TERRAIN_N-1) = 15 px; max_d is
     * the allowed height change per vertex as a fraction of H.         */
    float max_d = 0.030f + 0.007f * (float)(g->level - 1);
    if (max_d > 0.060f) max_d = 0.060f;
    enum { M = TERRAIN_N - 1 };                  /* ring size */
    for (int pass = 0; pass < 64; pass++) {      /* sweep to a fixpoint */
        bool changed = false;
        for (int k = 1; k <= M; k++) {           /* k == M: seam edge */
            int i = k % M, j = k - 1;
            float d = h[i] - h[j];
            if (d >  max_d) { h[i] = h[j] + max_d; changed = true; }
            if (d < -max_d) { h[i] = h[j] - max_d; changed = true; }
        }
        for (int k = M - 1; k >= 0; k--) {
            int i = k, j = (k + 1) % M;
            float d = h[i] - h[j];
            if (d >  max_d) { h[i] = h[j] + max_d; changed = true; }
            if (d < -max_d) { h[i] = h[j] - max_d; changed = true; }
        }
        if (!changed) break;
    }
    h[TERRAIN_N - 1] = h[0];

    /* carve flat pads; wider pad -> lower multiplier. Pads narrow at
     * higher levels. A pad must be a run of whole vertices to be flat
     * under terrain_y's interpolation, so widths are counted in 15 px
     * segments: 3/4/5/6 segs = 45/60/75/90 px -> 5X/4X/3X/2X.
     * The narrowest (45 px) still clears Eagle's stance at max legal
     * tilt (~30 px, see landing_test.c) with ~15 px to spare.
     * Levels 1-3 offer 4X/3X/2X; level 4+ offers 5X/4X/3X.             */
    enum { BLEND = 4 };                          /* vertices each side */
    int shrink = g->level >= 4 ? 1 : 0;
    int width[N_PADS], start[N_PADS], order[N_PADS];
    int slack = (TERRAIN_N - 2);                 /* zones live in 1..N-2 */
    for (int p = 0; p < N_PADS; p++) {
        width[p] = (6 - p - shrink) + 1;         /* 6,5,4 or 5,4,3 segs */
        slack   -= width[p] + 2 * BLEND;
        order[p] = p;
    }
    /* Layout: each pad reserves itself plus its blend zones, so no
     * blend can reach another pad. Shuffle the left-to-right order,
     * then split the leftover slack into N_PADS+1 random gaps. This is
     * constructive, so it always terminates (rejection sampling can
     * paint itself into a corner).                                     */
    for (int p = N_PADS - 1; p > 0; p--) {
        int j = (int)(frand(&g->rng) * (float)(p + 1));
        int t = order[p]; order[p] = order[j]; order[j] = t;
    }
    int cut[N_PADS];
    for (int p = 0; p < N_PADS; p++)
        cut[p] = (int)(frand(&g->rng) * (float)(slack + 1));
    for (int a = 1; a < N_PADS; a++)             /* insertion sort */
        for (int b = a; b > 0 && cut[b - 1] > cut[b]; b--) {
            int t = cut[b]; cut[b] = cut[b - 1]; cut[b - 1] = t;
        }
    int x = 1;                                   /* left edge of next zone */
    for (int k = 0; k < N_PADS; k++) {
        x += cut[k] - (k ? cut[k - 1] : 0);      /* gap before this zone */
        int p = order[k];
        start[p] = x + BLEND;
        x += width[p] + 2 * BLEND;
    }

    for (int p = 0; p < N_PADS; p++) {
        int s = start[p], w = width[p];
        float y = 0.5f * (h[s] + h[s + w - 1]);
        for (int i = 0; i < w; i++) h[s + i] = y;

        /* blend outward so carving can't create a wall steeper than
         * max_d. Zones are disjoint, so a blend never touches a pad.  */
        for (int i = s - 1; i >= 0 && i >= s - BLEND; i--) {
            float d = h[i] - h[i + 1];
            if (d >  max_d) h[i] = h[i + 1] + max_d;
            if (d < -max_d) h[i] = h[i + 1] - max_d;
        }
        for (int i = s + w; i < TERRAIN_N && i < s + w + BLEND; i++) {
            float d = h[i] - h[i - 1];
            if (d >  max_d) h[i] = h[i - 1] + max_d;
            if (d < -max_d) h[i] = h[i - 1] - max_d;
        }

        g->pads[p].x0   = (float)s / (TERRAIN_N - 1) * W;
        g->pads[p].x1   = (float)(s + w - 1) / (TERRAIN_N - 1) * W;
        g->pads[p].y    = y * H;
        g->pads[p].mult = 9 - w;                 /* 7 verts->2X ... 4->5X */
    }

    for (int i = 0; i < TERRAIN_N; i++) {
        g->terrain[i].x = (float)i / (TERRAIN_N - 1) * W;
        g->terrain[i].y = h[i] * H;
    }
}

/* map any x into [0, W): the world is a horizontal ring */
static float wrap_x(float x)
{
    x = fmodf(x, (float)W);
    if (x < 0.0f) x += (float)W;
    return x;
}

static float terrain_y(const Game *g, float x)
{
    float fx = wrap_x(x) / W * (TERRAIN_N - 1);
    int   i  = (int)fx;
    if (i > TERRAIN_N - 2) i = TERRAIN_N - 2;    /* float edge case */
    float t  = fx - (float)i;
    return g->terrain[i].y * (1 - t) + g->terrain[i + 1].y * t;
}

static const Pad *pad_under(const Game *g, float x)
{
    x = wrap_x(x);
    for (int p = 0; p < N_PADS; p++)
        if (x >= g->pads[p].x0 && x <= g->pads[p].x1) return &g->pads[p];
    return NULL;
}

/* ----------------------------------------------------------------- plank */

static float plank_x(const Plank *pk, int step)
{
    return pk->cx + pk->amp * sinf(pk->omega * (float)step * PHYS_DT);
}

static float plank_vx(const Plank *pk, int step)
{
    return pk->amp * pk->omega * cosf(pk->omega * (float)step * PHYS_DT);
}

/* From PLANK_FROM_LEVEL on, a floating pad swings side to side. It
 * hovers >= 90 px above the highest ground anywhere under its travel,
 * and is skipped on the rare moon too tall to fit it below spawn.     */
static void gen_plank(Game *g)
{
    Plank *pk = &g->plank;
    memset(pk, 0, sizeof *pk);
    if (g->level < PLANK_FROM_LEVEL) return;
    float amp = 50.0f + 5.0f * (float)(g->level - PLANK_FROM_LEVEL);
    if (amp > 80.0f) amp = 80.0f;
    float reach = amp + PLANK_HALF;
    float cx  = frange(&g->rng, reach + 60.0f, W - reach - 60.0f);
    /* highest ground under the swing (+20 px margin): the ground is
     * piecewise linear, so the extreme is at an end or a vertex      */
    float x0 = cx - reach - 20.0f, x1 = cx + reach + 20.0f;
    float top = fminf(terrain_y(g, x0), terrain_y(g, x1));
    for (int i = 0; i < TERRAIN_N; i++)
        if (g->terrain[i].x > x0 && g->terrain[i].x < x1 && g->terrain[i].y < top)
            top = g->terrain[i].y;
    float y = top - 90.0f;
    if (y < 170.0f) return;
    pk->on    = true;
    pk->cx    = cx;
    pk->y     = y;
    pk->amp   = amp;
    pk->omega = 6.2831853f / 9.0f;               /* one swing every 9 s */
}

/* ------------------------------------------------------------------ game */

static void reset_lander(Game *g)
{
    g->lander.pos    = (Vec2){ frange(&g->rng, W * 0.2f, W * 0.8f), H * 0.12f };
    g->lander.vel    = (Vec2){ frange(&g->rng, -25.0f, 25.0f), 0.0f };
    g->lander.angle  = 0.0f;
    g->lander.fuel   = FUEL_MAX;
    g->lander.thrusting = false;
    g->state         = ST_FLYING;
    g->state_time    = 0.0f;
}

/* Build a level from g->rng as the caller seeded it (start_level seeds
 * it from the run; tests seed it directly).                          */
static void new_game(Game *g)
{
    if (g->level < 1) g->level = 1;
    if (!g->fx_rng) g->fx_rng = 0x9E3779B9u;     /* xorshift must not be 0 */
    g->lseed   = g->rng;
    g->gravity = GRAVITY * (1.0f + 0.07f * (float)(g->level - 1));
    gen_terrain(g);
    gen_plank(g);
    for (int i = 0; i < N_STARS; i++)
        g->stars[i] = (Vec2){ frand(&g->rng) * W, frand(&g->rng) * H * 0.7f };
    memset(g->particles, 0, sizeof g->particles);
    reset_lander(g);
    g->step        = 0;
    g->on_plank    = false;
    g->rec_n       = 0;
    g->ghost_on    = g->ghost_flying = g->ghost_on_plank = false;
    g->zoom        = 1.0f;
    g->cam         = (Vec2){ W / 2.0f, H / 2.0f };
}

/* ----------------------------------------------------------------- ghosts */

/* Daily mode keeps, per level, the input stream of today's best landing
 * and replays it as a translucent Eagle. Physics is a pure function of
 * (level seed, step, input), so the replay retraces the flight exactly. */
static Ghost    g_ghosts[N_GHOST_LEVELS + 1];    /* index = level */
static uint32_t g_ghost_date;

static uint32_t today_yyyymmdd(void)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    return (uint32_t)((tm->tm_year + 1900) * 10000 +
                      (tm->tm_mon + 1) * 100 + tm->tm_mday);
}

/* File format:  "LANDERGHOST 1 <yyyymmdd>", then per level
 *   "L <level> <start fuel, %a hex float> <bonus> <steps> <runs>"
 *   followed by <runs> lines "<input>:<count>" (run-length encoded).
 * The fuel is stored exactly because mass, hence the trajectory,
 * depends on it.                                                     */
static bool ghosts_save(void)
{
    FILE *f = fopen(GHOST_FILE, "w");
    if (!f) return false;
    fprintf(f, "LANDERGHOST 1 %u\n", (unsigned)g_ghost_date);
    for (int lv = 1; lv <= N_GHOST_LEVELS; lv++) {
        const Ghost *gh = &g_ghosts[lv];
        if (!gh->valid) continue;
        int runs = 0;
        for (int i = 0; i < gh->n; i++)
            if (i == 0 || gh->in[i] != gh->in[i - 1]) runs++;
        fprintf(f, "L %d %a %d %d %d\n", lv, (double)gh->fuel,
                gh->bonus, gh->n, runs);
        for (int i = 0, k; i < gh->n; i = k) {
            for (k = i; k < gh->n && gh->in[k] == gh->in[i]; k++) {}
            fprintf(f, "%u:%d\n", (unsigned)gh->in[i], k - i);
        }
    }
    fclose(f);
#ifdef __EMSCRIPTEN__
    EM_ASM({ FS.syncfs(false, function(err) {}); });
#endif
    return true;
}

/* Load the ghosts recorded on `date`; a file from another day (or a
 * damaged one) leaves the bank empty or partly filled, never invalid. */
static void ghosts_load(uint32_t date)
{
    memset(g_ghosts, 0, sizeof g_ghosts);
    g_ghost_date = date;
    FILE *f = fopen(GHOST_FILE, "r");
    if (!f) return;
    int ver; unsigned fdate;
    if (fscanf(f, "LANDERGHOST %d %u", &ver, &fdate) != 2 ||
        ver != 1 || fdate != date) {
        fclose(f);
        return;
    }
    int lv, bonus, n, runs; double fuel;
    while (fscanf(f, " L %d %la %d %d %d", &lv, &fuel, &bonus, &n, &runs) == 5) {
        Ghost *gh = (lv >= 1 && lv <= N_GHOST_LEVELS) ? &g_ghosts[lv] : NULL;
        bool ok = gh && n > 0 && n <= MAX_STEPS && runs > 0;
        int pos = 0;
        for (int r = 0; r < runs; r++) {
            unsigned in; int cnt;
            if (fscanf(f, " %u:%d", &in, &cnt) != 2) {
                if (gh) gh->valid = false;
                fclose(f);
                return;
            }
            if (ok && (cnt <= 0 || pos + cnt > n)) ok = false;
            if (ok) memset(gh->in + pos, (int)(in & 7u), (size_t)cnt);
            pos += cnt;
        }
        if (gh) {
            gh->valid = ok && pos == n;
            gh->fuel  = (float)fuel;
            gh->bonus = bonus;
            gh->n     = n;
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------------- physics */

/* One fixed step of PHYS_DT for any lander, the player's or a ghost's.
 * Pure in (level, step, input): no RNG, audio or particles, so a
 * recorded input stream replays bit for bit. On touchdown the lander
 * is snapped onto the surface; on a crash it is left where it hit.  */
static Contact step_lander(const Game *g, Lander *l, uint8_t in, int step,
                           const Pad **pad_out, bool *plank_out)
{
    const float dt = PHYS_DT;
    *pad_out = NULL;
    *plank_out = false;

    if (in & IN_LEFT)  l->angle -= ROT_SPEED * dt;
    if (in & IN_RIGHT) l->angle += ROT_SPEED * dt;
    /* keep angle in [-pi, pi] so tilt is just |angle| however many
     * full rolls the pilot has done                                   */
    l->angle = remainderf(l->angle, 6.2831853f);

    /* semi-implicit Euler. Thrust force is fixed while mass falls as
     * fuel burns: THRUST_ACC is the full-tank acceleration, and an
     * empty Eagle accelerates (1 + FUEL_MASS) times harder.           */
    l->thrusting = (in & IN_THRUST) && l->fuel > 0.0f;
    float ax = wind_at(g->lseed, g->level, step), ay = g->gravity;
    if (l->thrusting) {
        float mass = (1.0f + FUEL_MASS * l->fuel / FUEL_MAX) / (1.0f + FUEL_MASS);
        float acc  = THRUST_ACC / mass;
        ax += sinf(l->angle) * acc;              /* +angle tips right */
        ay -= cosf(l->angle) * acc;
        l->fuel -= FUEL_BURN * dt;
        if (l->fuel < 0) l->fuel = 0;
    }
    l->vel.x += ax * dt;
    l->vel.y += ay * dt;
    l->pos.x += l->vel.x * dt;
    l->pos.y += l->vel.y * dt;
    l->pos.x  = wrap_x(l->pos.x);

    /* contact points. body->world: (bx,by) -> (bx*c - by*s, bx*s + by*c).
     * Feet at (+-LEG_X, LEG_Y). Hull points sit on Eagle's outline;
     * the belly's lowest point (0, 11) is left out on purpose: it is
     * 1 px above the feet, so a legal touchdown could graze it.       */
    static const Vec2 hull[] = {
        { -6.5f, -17.0f }, { 6.5f, -17.0f }, { 0.0f, -13.5f }, /* ears, crown */
        { -6.0f,  -8.0f }, { 6.0f,  -8.0f },                   /* cheeks      */
        {-10.5f,   3.0f }, {10.5f,   3.0f },                   /* belly sides */
        { -7.4f,   8.7f }, { 7.4f,   8.7f },                   /* lower belly */
        { 15.5f,  -2.0f }, {14.5f,   3.0f },                   /* tail        */
    };
    float s = sinf(l->angle), c = cosf(l->angle);
    Vec2 fl = { l->pos.x + (-LEG_X * c - LEG_Y * s),
                l->pos.y + (-LEG_X * s + LEG_Y * c) };
    Vec2 fr = { l->pos.x + ( LEG_X * c - LEG_Y * s),
                l->pos.y + ( LEG_X * s + LEG_Y * c) };

    const Plank *pk = &g->plank;
    float px = pk->on ? plank_x(pk, step) : 0.0f;
#define ON_PLANK(P) (pk->on && (P).y >= pk->y && (P).y <= pk->y + PLANK_THICK \
                     && fabsf((P).x - px) <= PLANK_HALF)

    bool hull_hit = false;
    for (size_t k = 0; k < sizeof hull / sizeof hull[0] && !hull_hit; k++) {
        Vec2 h = { l->pos.x + hull[k].x * c - hull[k].y * s,
                   l->pos.y + hull[k].x * s + hull[k].y * c };
        hull_hit = h.y >= terrain_y(g, h.x) || ON_PLANK(h);
    }
    bool gL = fl.y >= terrain_y(g, fl.x), gR = fr.y >= terrain_y(g, fr.x);
    bool pL = ON_PLANK(fl),               pR = ON_PLANK(fr);
#undef ON_PLANK
    if (!(hull_hit || gL || gR || pL || pR)) return CT_NONE;
    if (hull_hit) return CT_CRASH;

    bool upright = fabsf(l->angle) <= SAFE_ANGLE;   /* angle is wrapped */
    if (pL || pR) {
        float rvx = l->vel.x - plank_vx(pk, step);  /* match its speed */
        if (pL && pR && upright && fabsf(rvx) <= SAFE_VX &&
            fabsf(l->vel.y) <= SAFE_VY) {
            *plank_out = true;
            l->vel   = (Vec2){ 0, 0 };
            l->angle = 0;
            l->pos.y = pk->y - LEG_Y;
            return CT_LAND;
        }
        return CT_CRASH;
    }

    const Pad *pad = pad_under(g, l->pos.x);
    bool on_pad = pad && pad_under(g, fl.x) && pad_under(g, fr.x);
    if (on_pad && upright && fabsf(l->vel.x) <= SAFE_VX &&
        fabsf(l->vel.y) <= SAFE_VY) {
        *pad_out = pad;
        l->vel   = (Vec2){ 0, 0 };
        l->angle = 0;
        l->pos.y = pad->y - LEG_Y;
        return CT_LAND;
    }
    return CT_CRASH;
}

/* height of Eagle's feet above whatever is directly below him */
static float altitude(const Game *g)
{
    const Lander *l = &g->lander;
    float surf = terrain_y(g, l->pos.x);
    if (g->plank.on) {
        float px = plank_x(&g->plank, g->step);
        if (fabsf(l->pos.x - px) <= PLANK_HALF &&
            g->plank.y >= l->pos.y && g->plank.y < surf)
            surf = g->plank.y;
    }
    return surf - (l->pos.y + LEG_Y);
}

/* ------------------------------------------------------------ run / level */

/* start, or retry, level g->level of the current run with `fuel` */
static void start_level(Game *g, float fuel)
{
    g->rng = level_seed(g->run_seed, g->level);
    new_game(g);
    g->lander.fuel = g->level_fuel = fuel;
    if (g->daily && g->level <= N_GHOST_LEVELS && g_ghosts[g->level].valid) {
        g->ghost       = g->lander;               /* same spawn as us */
        g->ghost.fuel  = g_ghosts[g->level].fuel;
        g->ghost_bonus = g_ghosts[g->level].bonus;
        g->ghost_on    = g->ghost_flying = true;
    }
}

static void start_run(Game *g, bool daily)
{
    static uint32_t runs;
    g->daily = daily;
    if (daily) {
        g->daily_date = today_yyyymmdd();
        g->run_seed   = hash32(g->daily_date ^ 0xDA11u);
        ghosts_load(g->daily_date);
    } else {
        g->run_seed = hash32((uint32_t)time(NULL) ^ hash32(++runs));
    }
    g->level = 1;
    g->score = 0;
    g->lives = LIVES_START;
    start_level(g, FUEL_MAX);
}

/* run is over (out of lives or aborted): go to name entry or title */
static void end_run(Game *g)
{
    audio_set_thrust(&g->audio, false);
    if (hiscores_qualifies(g, g->score)) {
        g->entry_name[0] = '\0';
        g->entry_len = 0;
        g->state = ST_ENTRY;
    } else {
        g->state = ST_TITLE;
    }
    g->state_time = 0.0f;
}

static void spawn_exhaust(Game *g, float dt)
{
    int n = (int)(dt * 400.0f) + 1;
    float s = sinf(g->lander.angle), c = cosf(g->lander.angle);
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < N_PARTICLES; i++) {
            Particle *p = &g->particles[i];
            if (p->life > 0) continue;
            float spread = frange(&g->fx_rng, -0.45f, 0.45f);
            float speed  = frange(&g->fx_rng, 70.0f, 140.0f);
            p->pos = (Vec2){ g->lander.pos.x + s * 10.0f,
                             g->lander.pos.y + c * 10.0f };
            p->vel = (Vec2){
                g->lander.vel.x + (s + spread * c) * speed,
                g->lander.vel.y + (c - spread * s) * speed
            };
            p->life = frange(&g->fx_rng, 0.25f, 0.6f);
            break;
        }
    }
}

static void spawn_explosion(Game *g)
{
    for (int i = 0; i < N_PARTICLES; i++) {
        Particle *p = &g->particles[i];
        float a = frand(&g->fx_rng) * 6.2831853f;
        float v = frange(&g->fx_rng, 30.0f, 220.0f);
        p->pos  = g->lander.pos;
        p->vel  = (Vec2){ cosf(a) * v, sinf(a) * v * 0.8f - 40.0f };
        p->life = frange(&g->fx_rng, 0.5f, 1.6f);
    }
}

static void update_particles(Game *g, float dt)
{
    float wind = wind_at(g->lseed, g->level, g->step);
    for (int i = 0; i < N_PARTICLES; i++) {
        Particle *p = &g->particles[i];
        if (p->life <= 0) continue;
        p->life  -= dt;
        p->vel.x += wind * 2.0f * dt;            /* smoke drifts visibly */
        p->vel.y += g->gravity * 0.5f * dt;
        p->pos.x += p->vel.x * dt;
        p->pos.y += p->vel.y * dt;
        if (p->pos.y > terrain_y(g, p->pos.x)) {
            p->pos.y = terrain_y(g, p->pos.x);
            p->vel.y *= -0.3f;
            p->vel.x *=  0.6f;
        }
    }
}

/* camera: zoom 2x near the surface (with hysteresis), centred on Eagle
 * and clamped so the view never leaves the moon                       */
static void update_camera(Game *g, float dt)
{
    float target = g->zoom > 1.5f ? 2.0f : 1.0f;
    if (g->state == ST_FLYING) {
        float alt = altitude(g);
        if (alt < ZOOM_IN_ALT)  target = 2.0f;
        if (alt > ZOOM_OUT_ALT) target = 1.0f;
    }
    g->zoom += (target - g->zoom) * (1.0f - expf(-5.0f * dt));
    float hw = W / (2.0f * g->zoom), hh = H / (2.0f * g->zoom);
    float cx = g->lander.pos.x, cy = g->lander.pos.y;
    if (cx < hw)     cx = hw;
    if (cx > W - hw) cx = W - hw;
    if (cy < hh)     cy = hh;
    if (cy > H - hh) cy = H - hh;
    g->cam = (Vec2){ cx, cy };
}

static uint8_t read_input(const Game *g)
{
    uint8_t in = 0;
    if (g->keys[SDL_SCANCODE_LEFT]  || g->keys[SDL_SCANCODE_A]) in |= IN_LEFT;
    if (g->keys[SDL_SCANCODE_RIGHT] || g->keys[SDL_SCANCODE_D]) in |= IN_RIGHT;
    if (g->keys[SDL_SCANCODE_UP]    || g->keys[SDL_SCANCODE_W]) in |= IN_THRUST;
    return in;
}

static void on_landing(Game *g, const Pad *pad, bool plank)
{
    Lander *l = &g->lander;
    int mult = plank ? PLANK_MULT : pad->mult;
    g->state         = ST_LANDED;
    g->on_plank      = plank;
    g->plank_off     = plank ? l->pos.x - plank_x(&g->plank, g->step) : 0.0f;
    g->landing_bonus = 50 * mult + (int)l->fuel;
    g->score        += g->landing_bonus;

    g->extra_life = mult >= 4 && g->lives < LIVES_MAX;
    if (g->extra_life) g->lives++;
    g->refuel = REFUEL_BASE + REFUEL_PER_MULT * (float)mult;
    if (g->refuel > FUEL_MAX - l->fuel) g->refuel = FUEL_MAX - l->fuel;
    l->fuel += g->refuel;

    /* daily mode: keep the best landing per level as its ghost */
    if (g->daily && g->level <= N_GHOST_LEVELS && g->rec_n <= MAX_STEPS) {
        Ghost *gh = &g_ghosts[g->level];
        if (!gh->valid || g->landing_bonus > gh->bonus) {
            gh->valid = true;
            gh->fuel  = g->level_fuel;
            gh->bonus = g->landing_bonus;
            gh->n     = g->rec_n;
            memcpy(gh->in, g->rec, (size_t)g->rec_n);
            g_ghost_date = g->daily_date;
            ghosts_save();
        }
    }
    audio_trigger_landing(&g->audio);
}

/* one fixed physics step (dt is PHYS_DT in the game; tests may pass
 * other values, which only affects cosmetics)                        */
static void update(Game *g, float dt)
{
    g->state_time += dt;

    if (g->state == ST_PAUSED || g->state == ST_TITLE ||
        g->state == ST_ENTRY) {
        audio_set_thrust(&g->audio, false);
        return;
    }

    update_particles(g, dt);
    update_camera(g, dt);
    int s = g->step;
    const Pad *pad;
    bool plank;

    /* the ghost flies on regardless of what the player is doing */
    if (g->ghost_on && g->ghost_flying) {
        const Ghost *gh = &g_ghosts[g->level];
        uint8_t gin = s < gh->n ? gh->in[s] : 0;
        if (step_lander(g, &g->ghost, gin, s, &pad, &plank) != CT_NONE) {
            g->ghost_flying   = false;
            g->ghost_on_plank = plank;
            g->ghost_plank_off = g->ghost.pos.x - plank_x(&g->plank, s);
            if (plank)                      /* ride from this step on */
                g->ghost.pos.x = plank_x(&g->plank, s + 1) + g->ghost_plank_off;
        }
    } else if (g->ghost_on && g->ghost_on_plank) {
        g->ghost.pos.x = plank_x(&g->plank, s + 1) + g->ghost_plank_off;
    }

    if (g->state == ST_FLYING) {
        uint8_t in = read_input(g);
        if (s < MAX_STEPS) { g->rec[s] = in; g->rec_n = s + 1; }
        else               g->rec_n = MAX_STEPS + 1;   /* too long: no ghost */

        Contact ct = step_lander(g, &g->lander, in, s, &pad, &plank);
        audio_set_thrust(&g->audio, g->lander.thrusting && ct == CT_NONE);
        if (g->lander.thrusting) spawn_exhaust(g, dt);

        if (ct == CT_LAND) {
            on_landing(g, pad, plank);
            if (plank)                      /* ride from this step on */
                g->lander.pos.x = plank_x(&g->plank, s + 1) + g->plank_off;
            g->state_time = 0.0f;
        } else if (ct == CT_CRASH) {
            g->state = ST_CRASHED;
            g->lives--;
            spawn_explosion(g);
            audio_trigger_explosion(&g->audio);
            g->state_time = 0.0f;
        }
    } else if (g->state == ST_LANDED && g->on_plank) {
        g->lander.pos.x = plank_x(&g->plank, s + 1) + g->plank_off;   /* ride */
    }
    g->step++;
}

/* Fixed-timestep driver: simulate as many PHYS_DT steps as real time
 * allows, carrying the remainder. Capped so a stalled tab can't make
 * the game try to catch up seconds at once. Returns steps taken.    */
static int advance(Game *g, float real_dt)
{
    if (real_dt > 0.25f) real_dt = 0.25f;
    if (real_dt < 0.0f)  real_dt = 0.0f;
    g->step_acc += real_dt;
    int n = 0;
    while (g->step_acc >= PHYS_DT) {
        update(g, PHYS_DT);
        g->step_acc -= PHYS_DT;
        n++;
    }
    return n;
}

/* -------------------------------------------------------------- rendering */

static void draw_lines(SDL_Renderer *r, const Vec2 *pts, int n,
                       float x, float y, float angle, float scale)
{
    float s = sinf(angle), c = cosf(angle);
    SDL_FPoint out[32];
    for (int i = 0; i < n && i < 32; i++) {
        float px = pts[i].x * scale, py = pts[i].y * scale;
        out[i].x = x + px * c - py * s;
        out[i].y = y + px * s + py * c;
    }
    SDL_RenderDrawLinesF(r, out, n);
}

/* stroke font, full A-Z 0-9 plus a little punctuation */
static void draw_char(SDL_Renderer *r, char ch, float x, float y, float sz)
{
    #define STROKE(...) do { Vec2 p[] = {__VA_ARGS__};                       \
        draw_lines(r, p, (int)(sizeof p / sizeof p[0]), x, y, 0, sz); } while (0)
    switch (ch) {
    case '0': STROKE({0,0},{2,0},{2,3},{0,3},{0,0}); break;
    case '1': STROKE({1,0},{1,3}); break;
    case '2': STROKE({0,0},{2,0},{2,1.5f},{0,1.5f},{0,3},{2,3}); break;
    case '3': STROKE({0,0},{2,0},{2,3},{0,3}); STROKE({0,1.5f},{2,1.5f}); break;
    case '4': STROKE({0,0},{0,1.5f},{2,1.5f}); STROKE({2,0},{2,3}); break;
    case '5': STROKE({2,0},{0,0},{0,1.5f},{2,1.5f},{2,3},{0,3}); break;
    case '6': STROKE({2,0},{0,0},{0,3},{2,3},{2,1.5f},{0,1.5f}); break;
    case '7': STROKE({0,0},{2,0},{2,3}); break;
    case '8': STROKE({0,0},{2,0},{2,3},{0,3},{0,0}); STROKE({0,1.5f},{2,1.5f}); break;
    case '9': STROKE({0,1.5f},{2,1.5f}); STROKE({2,0},{0,0},{0,1.4f}); STROKE({2,0},{2,3},{0,3}); break;
    case 'A': STROKE({0,3},{0,1},{1,0},{2,1},{2,3}); STROKE({0,1.7f},{2,1.7f}); break;
    case 'B': STROKE({0,3},{0,0},{1.7f,0},{1.7f,1.5f},{0,1.5f}); STROKE({1.7f,1.5f},{2,1.5f},{2,3},{0,3}); break;
    case 'C': STROKE({2,0},{0,0},{0,3},{2,3}); break;
    case 'D': STROKE({0,0},{1.4f,0},{2,0.7f},{2,2.3f},{1.4f,3},{0,3},{0,0}); break;
    case 'E': STROKE({2,0},{0,0},{0,3},{2,3}); STROKE({0,1.5f},{1.6f,1.5f}); break;
    case 'F': STROKE({2,0},{0,0},{0,3}); STROKE({0,1.5f},{1.6f,1.5f}); break;
    case 'G': STROKE({2,0},{0,0},{0,3},{2,3},{2,1.7f},{1.2f,1.7f}); break;
    case 'H': STROKE({0,0},{0,3}); STROKE({2,0},{2,3}); STROKE({0,1.5f},{2,1.5f}); break;
    case 'I': STROKE({1,0},{1,3}); break;
    case 'J': STROKE({2,0},{2,2.4f},{1.3f,3},{0.5f,3},{0,2.4f}); break;
    case 'K': STROKE({0,0},{0,3}); STROKE({2,0},{0,1.5f},{2,3}); break;
    case 'L': STROKE({0,0},{0,3},{2,3}); break;
    case 'M': STROKE({0,3},{0,0},{1,1.2f},{2,0},{2,3}); break;
    case 'N': STROKE({0,3},{0,0},{2,3},{2,0}); break;
    case 'O': STROKE({0,0},{2,0},{2,3},{0,3},{0,0}); break;
    case 'P': STROKE({0,3},{0,0},{2,0},{2,1.5f},{0,1.5f}); break;
    case 'Q': STROKE({0,0},{2,0},{2,3},{0,3},{0,0}); STROKE({1.2f,2.2f},{2.3f,3.3f}); break;
    case 'R': STROKE({0,3},{0,0},{2,0},{2,1.5f},{0,1.5f}); STROKE({0.7f,1.5f},{2,3}); break;
    case 'S': STROKE({2,0},{0,0},{0,1.5f},{2,1.5f},{2,3},{0,3}); break;
    case 'T': STROKE({0,0},{2,0}); STROKE({1,0},{1,3}); break;
    case 'U': STROKE({0,0},{0,3},{2,3},{2,0}); break;
    case 'V': STROKE({0,0},{1,3},{2,0}); break;
    case 'W': STROKE({0,0},{0.5f,3},{1,1.5f},{1.5f,3},{2,0}); break;
    case 'X': STROKE({0,0},{2,3}); STROKE({2,0},{0,3}); break;
    case 'Y': STROKE({0,0},{1,1.4f},{2,0}); STROKE({1,1.4f},{1,3}); break;
    case 'Z': STROKE({0,0},{2,0},{0,3},{2,3}); break;
    case '-': STROKE({0.3f,1.5f},{1.7f,1.5f}); break;
    case '+': STROKE({0.3f,1.5f},{1.7f,1.5f}); STROKE({1,0.8f},{1,2.2f}); break;
    case '.': STROKE({0.9f,2.8f},{1.1f,2.8f},{1.1f,3},{0.9f,3},{0.9f,2.8f}); break;
    case '_': STROKE({0,3},{2,3}); break;
    default: break;
    }
    #undef STROKE
}

static void draw_text(SDL_Renderer *r, const char *s, float x, float y, float sz)
{
    for (; *s; s++, x += sz * 3.0f)
        if (*s != ' ') draw_char(r, *s, x, y, sz);
}

/* centered helper: glyph advance is 3*sz */
static void draw_text_c(SDL_Renderer *r, const char *s, float y, float sz)
{
    float w = (float)strlen(s) * 3.0f * sz - sz;   /* last advance unused */
    draw_text(r, s, (W - w) / 2.0f, y, sz);
}

/* ellipse outline in body coordinates, rotated into the world */
static void draw_ellipse(SDL_Renderer *r, float cx, float cy,
                         float rx, float ry,
                         float x, float y, float angle)
{
    enum { SEG = 20 };
    Vec2 pts[SEG + 1];
    for (int i = 0; i <= SEG; i++) {
        float a = 6.2831853f * (float)i / SEG;
        pts[i] = (Vec2){ cx + rx * cosf(a), cy + ry * sinf(a) };
    }
    draw_lines(r, pts, SEG + 1, x, y, angle, 1.0f);
}

/* small filled dot at a body-frame point, rotated into the world */
static void draw_dot(SDL_Renderer *r, float bx, float by, float rad,
                     float x, float y, float angle)
{
    float s = sinf(angle), c = cosf(angle);
    SDL_FRect q = { x + bx * c - by * s - rad,
                    y + bx * s + by * c - rad,
                    2.0f * rad, 2.0f * rad };
    SDL_RenderFillRectF(r, &q);
}

enum { EAGLE_R = 24 };   /* px: Eagle's reach from centre incl. flame */

/* Eagle draws in his own colours, or all in translucent blue-grey when
 * he is the daily ghost.                                              */
static void eagle_col(SDL_Renderer *r, bool ghost, uint8_t cr, uint8_t cg, uint8_t cb)
{
    if (ghost) SDL_SetRenderDrawColor(r, 150, 175, 255, 110);
    else       SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
}

/* Eagle the fat cat at screen x = lx. Body frame: up is -y; the legs
 * drawn below end at the collision feet (+-LEG_X, LEG_Y).             */
static void draw_eagle(Game *g, const Lander *l, float lx, bool ghost, bool flying)
{
    SDL_Renderer *r = g->ren;
    float ly = l->pos.y, la = l->angle;

    eagle_col(r, ghost, 255, 170, 80);                       /* orange */
    draw_ellipse(r, 0.0f,  3.0f, 10.5f, 8.0f, lx, ly, la);   /* body   */
    draw_ellipse(r, 0.0f, -8.0f,  6.0f, 5.5f, lx, ly, la);   /* head   */
    {
        static const Vec2 earL[] = { {-5.0f,-11.5f},{-6.5f,-17.0f},{-1.5f,-13.0f} };
        static const Vec2 earR[] = { { 5.0f,-11.5f},{ 6.5f,-17.0f},{ 1.5f,-13.0f} };
        static const Vec2 tail[] = { {10.0f,6.0f},{14.5f,3.0f},{15.5f,-2.0f},{12.5f,-4.5f} };
        static const Vec2 legL[] = { {-6.0f,9.5f},{-10.0f,LEG_Y},{-LEG_X,LEG_Y} };
        static const Vec2 legR[] = { { 6.0f,9.5f},{ 10.0f,LEG_Y},{ LEG_X,LEG_Y} };
        draw_lines(r, earL, 3, lx, ly, la, 1.0f);
        draw_lines(r, earR, 3, lx, ly, la, 1.0f);
        draw_lines(r, tail, 4, lx, ly, la, 1.0f);
        draw_lines(r, legL, 3, lx, ly, la, 1.0f);
        draw_lines(r, legR, 3, lx, ly, la, 1.0f);
    }

    /* whiskers — prominently visible, three per side */
    eagle_col(r, ghost, 240, 240, 250);
    {
        static const Vec2 wsk[6][2] = {
            { {-4.5f,-8.5f}, {-13.0f,-10.0f} },
            { {-5.0f,-7.5f}, {-13.5f, -7.5f} },
            { {-4.5f,-6.5f}, {-13.0f, -5.0f} },
            { { 4.5f,-8.5f}, { 13.0f,-10.0f} },
            { { 5.0f,-7.5f}, { 13.5f, -7.5f} },
            { { 4.5f,-6.5f}, { 13.0f, -5.0f} },
        };
        for (int i = 0; i < 6; i++)
            draw_lines(r, wsk[i], 2, lx, ly, la, 1.0f);
    }

    eagle_col(r, ghost, 70, 150, 255);                       /* blue eyes */
    draw_dot(r, -2.5f, -9.0f, 1.6f, lx, ly, la);
    draw_dot(r,  2.5f, -9.0f, 1.6f, lx, ly, la);
    eagle_col(r, ghost, 255, 120, 150);                      /* pink nose */
    draw_dot(r, 0.0f, -6.3f, 1.0f, lx, ly, la);

    if (l->thrusting && flying) {
        eagle_col(r, ghost, 255, 190, 60);
        float flick = 6.0f + frand(&g->fx_rng) * 6.0f;
        Vec2 flame[] = { {-4,11},{0,11 + flick},{4,11} };
        draw_lines(r, flame, 3, lx, ly, la, 1.0f);
    }
}

/* the world wraps: while Eagle straddles an edge, draw a copy on the
 * far side too so he never half-vanishes                              */
static void draw_eagle_wrapped(Game *g, const Lander *l, bool ghost, bool flying)
{
    float lx = l->pos.x;
    draw_eagle(g, l, lx, ghost, flying);
    if (lx < EAGLE_R)     draw_eagle(g, l, lx + W, ghost, flying);
    if (lx > W - EAGLE_R) draw_eagle(g, l, lx - W, ghost, flying);
}

static void draw_plank(Game *g)
{
    const Plank *pk = &g->plank;
    if (!pk->on) return;
    float x  = plank_x(pk, g->step);
    float x0 = x - PLANK_HALF, x1 = x + PLANK_HALF, y = pk->y;
    SDL_SetRenderDrawColor(g->ren, 80, 255, 120, 255);
    for (float t = 0; t < PLANK_THICK; t += 1.0f)
        SDL_RenderDrawLineF(g->ren, x0, y + t, x1, y + t);
    /* two little hover jets */
    SDL_SetRenderDrawColor(g->ren, 255, 190, 60, 200);
    float f = 4.0f + frand(&g->fx_rng) * 4.0f;
    SDL_RenderDrawLineF(g->ren, x0 + 6, y + PLANK_THICK, x0 + 6, y + PLANK_THICK + f);
    SDL_RenderDrawLineF(g->ren, x1 - 6, y + PLANK_THICK, x1 - 6, y + PLANK_THICK + f);
    SDL_SetRenderDrawColor(g->ren, 80, 255, 120, 255);
    char lbl[8];
    snprintf(lbl, sizeof lbl, "%dX", PLANK_MULT);
    draw_text(g->ren, lbl, x - 8, y + PLANK_THICK + 12, 3.0f);
}

static void render_world(Game *g)
{
    SDL_SetRenderDrawColor(g->ren, 140, 140, 170, 255);
    for (int i = 0; i < N_STARS; i++)
        SDL_RenderDrawPointF(g->ren, g->stars[i].x, g->stars[i].y);

    SDL_SetRenderDrawColor(g->ren, 190, 190, 200, 255);
    SDL_FPoint tp[TERRAIN_N];
    for (int i = 0; i < TERRAIN_N; i++)
        tp[i] = (SDL_FPoint){ g->terrain[i].x, g->terrain[i].y };
    SDL_RenderDrawLinesF(g->ren, tp, TERRAIN_N);

    for (int p = 0; p < N_PADS; p++) {
        const Pad *pd = &g->pads[p];
        SDL_SetRenderDrawColor(g->ren, 80, 255, 120, 255);
        SDL_RenderDrawLineF(g->ren, pd->x0, pd->y, pd->x1, pd->y);
        SDL_RenderDrawLineF(g->ren, pd->x0, pd->y + 1, pd->x1, pd->y + 1);
        char lbl[8];
        snprintf(lbl, sizeof lbl, "%dX", pd->mult);
        draw_text(g->ren, lbl, (pd->x0 + pd->x1) / 2 - 8, pd->y + 8, 3.0f);
    }
    draw_plank(g);

    for (int i = 0; i < N_PARTICLES; i++) {
        Particle *p = &g->particles[i];
        if (p->life <= 0) continue;
        int a = (int)(p->life * 400);
        if (a > 255) a = 255;
        SDL_SetRenderDrawColor(g->ren, 255,
                               (uint8_t)(160 + frand(&g->fx_rng) * 60),
                               60, (uint8_t)a);
        SDL_RenderDrawPointF(g->ren, p->pos.x, p->pos.y);
        SDL_RenderDrawPointF(g->ren, p->pos.x + 1, p->pos.y);
    }

    if (g->ghost_on)
        draw_eagle_wrapped(g, &g->ghost, true, g->ghost_flying);
    if (g->state != ST_CRASHED)
        draw_eagle_wrapped(g, &g->lander, false, g->state == ST_FLYING);
}

static void hud_col(SDL_Renderer *r, bool alarm)
{
    if (alarm) SDL_SetRenderDrawColor(r, 255, 80, 80, 255);
    else       SDL_SetRenderDrawColor(r, 200, 220, 255, 255);
}

/* small cat head for the lives counter */
static void draw_life(SDL_Renderer *r, float x, float y)
{
    static const Vec2 earL[] = { {-5.0f,-3.5f},{-6.5f,-9.0f},{-1.5f,-5.0f} };
    static const Vec2 earR[] = { { 5.0f,-3.5f},{ 6.5f,-9.0f},{ 1.5f,-5.0f} };
    SDL_SetRenderDrawColor(r, 255, 170, 80, 255);
    draw_ellipse(r, 0.0f, 0.0f, 6.0f, 5.5f, x, y, 0.0f);
    draw_lines(r, earL, 3, x, y, 0.0f, 1.0f);
    draw_lines(r, earR, 3, x, y, 0.0f, 1.0f);
    SDL_SetRenderDrawColor(r, 70, 150, 255, 255);
    draw_dot(r, -2.5f, -1.0f, 1.2f, x, y, 0.0f);
    draw_dot(r,  2.5f, -1.0f, 1.2f, x, y, 0.0f);
}

static void render_hud(Game *g)
{
    SDL_Renderer *r = g->ren;
    const Lander *l = &g->lander;
    char buf[64];

    /* left: fuel, velocities (red when unsafe), altitude */
    hud_col(r, l->fuel < 15.0f);
    snprintf(buf, sizeof buf, "F %3.0f", l->fuel);
    draw_text(r, buf, 14, 14, 4.0f);
    hud_col(r, fabsf(l->vel.x) > SAFE_VX);
    snprintf(buf, sizeof buf, "VH %4.0f", l->vel.x);
    draw_text(r, buf, 14, 34, 4.0f);
    hud_col(r, l->vel.y > SAFE_VY);
    snprintf(buf, sizeof buf, "VV %4.0f", l->vel.y);
    draw_text(r, buf, 14, 54, 4.0f);
    hud_col(r, false);
    if (g->state == ST_FLYING) {
        float alt = altitude(g);
        snprintf(buf, sizeof buf, "ALT %4.0f", alt < 0 ? 0.0f : alt);
        draw_text(r, buf, 14, 74, 4.0f);
    }

    SDL_SetRenderDrawColor(r, 90, 200, 255, 255);
    SDL_FRect bar = { 110, 14, 1.2f * l->fuel, 10 };
    SDL_RenderFillRectF(r, &bar);
    hud_col(r, false);
    SDL_FRect frame = { 110, 14, 120, 10 };
    SDL_RenderDrawRectF(r, &frame);

    /* tilt gauge: needle shows Eagle's axis, ticks mark the legal limit */
    {
        const float cx = 170, cy = 72, R = 22;
        SDL_SetRenderDrawColor(r, 110, 120, 150, 255);
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            float a = sgn * SAFE_ANGLE;
            SDL_RenderDrawLineF(r, cx + sinf(a) * (R - 5), cy - cosf(a) * (R - 5),
                                   cx + sinf(a) * (R + 3), cy - cosf(a) * (R + 3));
        }
        SDL_RenderDrawLineF(r, cx - R, cy, cx + R, cy);
        if (fabsf(l->angle) <= SAFE_ANGLE) SDL_SetRenderDrawColor(r, 80, 255, 120, 255);
        else                               SDL_SetRenderDrawColor(r, 255, 80, 80, 255);
        SDL_RenderDrawLineF(r, cx, cy, cx + sinf(l->angle) * R, cy - cosf(l->angle) * R);
        SDL_RenderDrawLineF(r, cx + 1, cy, cx + 1 + sinf(l->angle) * R, cy - cosf(l->angle) * R);
    }

    /* right: score, level, lives */
    hud_col(r, false);
    snprintf(buf, sizeof buf, "SCORE %d", g->score);
    draw_text(r, buf, W - 190, 14, 4.0f);
    snprintf(buf, sizeof buf, "LEVEL %d", g->level);
    draw_text(r, buf, W - 190, 34, 4.0f);
    for (int i = 0; i < g->lives; i++)
        draw_life(r, W - 182 + 18.0f * i, 66);

    /* top centre: daily tag, ghost to beat, wind arrow */
    if (g->daily) {
        SDL_SetRenderDrawColor(r, 255, 210, 90, 255);
        snprintf(buf, sizeof buf, "DAILY %u", (unsigned)g->daily_date);
        draw_text_c(r, buf, 14, 3.0f);
        if (g->ghost_on) {
            SDL_SetRenderDrawColor(r, 150, 175, 255, 255);
            snprintf(buf, sizeof buf, "GHOST %d", g->ghost_bonus);
            draw_text_c(r, buf, 30, 3.0f);
        }
    }
    if (wind_max(g->level) > 0) {
        float w = wind_at(g->lseed, g->level, g->step);
        float cx = W / 2.0f, cy = 58, len = w * 5.0f;      /* 14 -> 70 px */
        float d = len >= 0 ? 1.0f : -1.0f;
        hud_col(r, false);
        draw_text(r, "WIND", cx - 22, 66, 3.0f);
        SDL_SetRenderDrawColor(r, 160, 200, 255, 255);
        SDL_RenderDrawLineF(r, cx, cy, cx + len, cy);
        if (fabsf(len) > 4) {
            SDL_RenderDrawLineF(r, cx + len, cy, cx + len - d * 6, cy - 4);
            SDL_RenderDrawLineF(r, cx + len, cy, cx + len - d * 6, cy + 4);
        }
    }
}

static void render_title(Game *g)
{
    char buf[64];
    SDL_SetRenderDrawColor(g->ren, 230, 230, 240, 255);
    draw_text_c(g->ren, "LUNAR LANDER", 50, 9.0f);
    SDL_SetRenderDrawColor(g->ren, 255, 170, 80, 255);
    draw_text_c(g->ren, "STARRING EAGLE THE FAT CAT", 95, 4.0f);

    SDL_SetRenderDrawColor(g->ren, 200, 220, 255, 255);
    draw_text_c(g->ren, "LEFT RIGHT OR A D TO ROTATE  UP OR W TO THRUST", 130, 4.0f);
    draw_text_c(g->ren, "P TO PAUSE  ESC TO END FLIGHT", 153, 4.0f);

    SDL_SetRenderDrawColor(g->ren, 80, 255, 120, 255);
    draw_text_c(g->ren, "LAND UPRIGHT ON A GREEN PAD", 190, 4.0f);
    snprintf(buf, sizeof buf, "DOWN SPEED UNDER %.0f  DRIFT UNDER %.0f",
             (double)SAFE_VY, (double)SAFE_VX);
    draw_text_c(g->ren, buf, 213, 4.0f);
    draw_text_c(g->ren, "SCORE IS 50 X PAD BONUS PLUS FUEL LEFT", 236, 4.0f);
    draw_text_c(g->ren, "FUEL CARRIES OVER  EVERY LANDING REFUELS", 259, 4.0f);
    draw_text_c(g->ren, "THREE LIVES  A 4X OR BETTER LANDING ADDS ONE", 282, 4.0f);
    snprintf(buf, sizeof buf, "WIND FROM LEVEL %d  MOVING PAD FROM LEVEL %d",
             WIND_FROM_LEVEL, PLANK_FROM_LEVEL);
    draw_text_c(g->ren, buf, 305, 4.0f);

    SDL_SetRenderDrawColor(g->ren, 255, 210, 90, 255);
    draw_text_c(g->ren, "HIGH SCORES", 350, 5.0f);
    SDL_SetRenderDrawColor(g->ren, 200, 220, 255, 255);
    if (g->n_hiscores == 0)
        draw_text_c(g->ren, "NO FLIGHTS RECORDED", 382, 4.0f);
    for (int i = 0; i < g->n_hiscores; i++) {
        snprintf(buf, sizeof buf, "%d  %-10s %6d",
                 i + 1, g->hiscores[i].name, g->hiscores[i].score);
        draw_text_c(g->ren, buf, 382.0f + 22.0f * i, 4.0f);
    }

    if (fmodf(g->state_time, 1.0f) < 0.65f) {
        SDL_SetRenderDrawColor(g->ren, 230, 230, 240, 255);
        draw_text_c(g->ren, "SPACE TO FLY  D FOR TODAYS DAILY MOON", H - 50, 5.0f);
    }
}

static void render_entry(Game *g)
{
    char buf[64];
    SDL_SetRenderDrawColor(g->ren, 255, 210, 90, 255);
    draw_text_c(g->ren, "RUN OVER", 160, 8.0f);
    SDL_SetRenderDrawColor(g->ren, 200, 220, 255, 255);
    snprintf(buf, sizeof buf, "SCORE %d", g->score);
    draw_text_c(g->ren, buf, 230, 6.0f);
    draw_text_c(g->ren, "A NEW HIGH SCORE", 280, 4.0f);
    draw_text_c(g->ren, "ENTER YOUR NAME", 320, 4.0f);

    snprintf(buf, sizeof buf, "%s%s", g->entry_name,
             fmodf(g->state_time, 0.8f) < 0.5f ? "_" : " ");
    SDL_SetRenderDrawColor(g->ren, 80, 255, 120, 255);
    draw_text_c(g->ren, buf, 370, 6.0f);

    SDL_SetRenderDrawColor(g->ren, 200, 220, 255, 255);
    draw_text_c(g->ren, "RETURN TO CONFIRM", 440, 4.0f);
}

/* The world is drawn into world_tex at 1:1, then the camera's window
 * of it is scaled up to the screen (nearest-neighbour: the zoom keeps
 * the chunky vector look). HUD and banners go on top, unscaled.
 * Without a target texture (e.g. tests) the world is drawn directly. */
static void render(Game *g)
{
    SDL_SetRenderDrawColor(g->ren, 8, 8, 16, 255);
    SDL_RenderClear(g->ren);

    if (g->state == ST_TITLE) {
        SDL_SetRenderDrawColor(g->ren, 140, 140, 170, 255);
        for (int i = 0; i < N_STARS; i++)
            SDL_RenderDrawPointF(g->ren, g->stars[i].x, g->stars[i].y);
        render_title(g);
        SDL_RenderPresent(g->ren);
        return;
    }
    if (g->state == ST_ENTRY) {
        render_entry(g);
        SDL_RenderPresent(g->ren);
        return;
    }

    if (g->world_tex && SDL_SetRenderTarget(g->ren, g->world_tex) == 0) {
        SDL_SetRenderDrawColor(g->ren, 8, 8, 16, 255);
        SDL_RenderClear(g->ren);
        render_world(g);
        SDL_SetRenderTarget(g->ren, NULL);
        float z = g->zoom < 1.0f ? 1.0f : g->zoom;
        int vw = (int)lroundf(W / z), vh = (int)lroundf(H / z);
        int vx = (int)lroundf(g->cam.x - vw / 2.0f);
        int vy = (int)lroundf(g->cam.y - vh / 2.0f);
        if (vx < 0) vx = 0;
        if (vy < 0) vy = 0;
        if (vx > W - vw) vx = W - vw;
        if (vy > H - vh) vy = H - vh;
        SDL_Rect src = { vx, vy, vw, vh };
        SDL_RenderCopy(g->ren, g->world_tex, &src, NULL);
    } else {
        render_world(g);
    }
    render_hud(g);

    char buf[64];
    if (g->state == ST_PAUSED) {
        SDL_SetRenderDrawColor(g->ren, 255, 210, 90, 255);
        draw_text_c(g->ren, "PAUSED", H / 2 - 40, 8.0f);
        draw_text_c(g->ren, "P TO RESUME", H / 2 + 20, 4.0f);
    } else if (g->state == ST_LANDED) {
        SDL_SetRenderDrawColor(g->ren, 80, 255, 120, 255);
        draw_text_c(g->ren, "THE EAGLE HAS LANDED", H / 2 - 90, 7.0f);
        snprintf(buf, sizeof buf, "%d PTS", g->landing_bonus);
        draw_text_c(g->ren, buf, H / 2 - 50, 5.0f);
        snprintf(buf, sizeof buf, "+%.0f FUEL%s", (double)g->refuel,
                 g->extra_life ? "  +1 LIFE" : "");
        draw_text_c(g->ren, buf, H / 2 - 20, 4.0f);
        draw_text_c(g->ren, "R FOR NEXT LEVEL", H / 2 + 10, 4.0f);
    } else if (g->state == ST_CRASHED) {
        SDL_SetRenderDrawColor(g->ren, 255, 90, 90, 255);
        draw_text_c(g->ren, "CRASHED", H / 2 - 90, 7.0f);
        if (g->lives > 0) {
            snprintf(buf, sizeof buf, "%d %s LEFT", g->lives,
                     g->lives == 1 ? "LIFE" : "LIVES");
            draw_text_c(g->ren, buf, H / 2 - 45, 4.0f);
            draw_text_c(g->ren, "R TO TRY THIS MOON AGAIN", H / 2 - 20, 4.0f);
        } else {
            draw_text_c(g->ren, "OUT OF LIVES", H / 2 - 45, 4.0f);
            draw_text_c(g->ren, "R TO CONTINUE", H / 2 - 20, 4.0f);
        }
    }

    SDL_RenderPresent(g->ren);
}

/* --------------------------------------------------------------- input   */

static void handle_key(Game *g, SDL_Scancode sc)
{
    switch (g->state) {
    case ST_TITLE:
        if (sc == SDL_SCANCODE_SPACE || sc == SDL_SCANCODE_RETURN) {
            start_run(g, false);
        } else if (sc == SDL_SCANCODE_D) {
            start_run(g, true);
        } else if (sc == SDL_SCANCODE_ESCAPE) {
#ifndef __EMSCRIPTEN__
            /* in a browser tab there is nothing to quit to: stopping
             * the main loop would just leave a dead canvas            */
            g->running = false;
#endif
        }
        break;

    case ST_FLYING:
        if (sc == SDL_SCANCODE_P)      { g->state = ST_PAUSED; }
        else if (sc == SDL_SCANCODE_ESCAPE) end_run(g);
        break;

    case ST_PAUSED:
        if (sc == SDL_SCANCODE_P)      { g->state = ST_FLYING; }
        else if (sc == SDL_SCANCODE_ESCAPE) end_run(g);
        break;

    case ST_LANDED:
        if (sc == SDL_SCANCODE_R) {          /* fuel carries over */
            g->level++;
            start_level(g, g->lander.fuel);
        }
        else if (sc == SDL_SCANCODE_ESCAPE) end_run(g);
        break;

    case ST_CRASHED:
        if (sc == SDL_SCANCODE_R && g->lives > 0)
            start_level(g, g->level_fuel);       /* same moon, same fuel */
        else if (sc == SDL_SCANCODE_R || sc == SDL_SCANCODE_ESCAPE)
            end_run(g);
        break;

    case ST_ENTRY:
        if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
            if (g->entry_len < NAME_MAX) {
                g->entry_name[g->entry_len++] =
                    (char)('A' + (sc - SDL_SCANCODE_A));
                g->entry_name[g->entry_len] = '\0';
            }
        } else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) {
            /* scancodes run 1..9 then 0 */
            if (g->entry_len < NAME_MAX) {
                char d = sc == SDL_SCANCODE_0
                       ? '0' : (char)('1' + (sc - SDL_SCANCODE_1));
                g->entry_name[g->entry_len++] = d;
                g->entry_name[g->entry_len] = '\0';
            }
        } else if (sc == SDL_SCANCODE_BACKSPACE && g->entry_len > 0) {
            g->entry_name[--g->entry_len] = '\0';
        } else if (sc == SDL_SCANCODE_RETURN ||
                   sc == SDL_SCANCODE_KP_ENTER) {
            hiscores_insert(g, g->entry_name, g->score);
            g->score = 0;
            g->state = ST_TITLE;
            g->state_time = 0.0f;
        }
        break;
    }
}

/* -------------------------------------------------------------- main loop */

static void frame(void *arg)
{
    Game *g = arg;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: g->running = false; break;
        case SDL_KEYDOWN:
            if (!e.key.repeat) handle_key(g, e.key.keysym.scancode);
            g->keys[e.key.keysym.scancode] = true;
            break;
        case SDL_KEYUP:
            g->keys[e.key.keysym.scancode] = false;
            break;
        }
    }

    uint64_t now = SDL_GetPerformanceCounter();
    float dt = (float)(now - g->prev_ticks) / SDL_GetPerformanceFrequency();
    g->prev_ticks = now;

    advance(g, dt);                     /* fixed PHYS_DT steps inside */
    render(g);

#ifdef __EMSCRIPTEN__
    if (!g->running) emscripten_cancel_main_loop();
#endif
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    Game *g = &g_game;
    g->rng    = (uint32_t)time(NULL) | 1;
    g->fx_rng = (uint32_t)time(NULL) * 2654435761u | 1;
    g->running = true;
    g->level = 1;
    g->state = ST_TITLE;

    g->win = SDL_CreateWindow("Lander",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              W, H, 0);
    g->ren = SDL_CreateRenderer(g->win, -1,
                                SDL_RENDERER_ACCELERATED |
                                SDL_RENDERER_PRESENTVSYNC);
    if (!g->win || !g->ren) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderDrawBlendMode(g->ren, SDL_BLENDMODE_BLEND);

    /* off-screen world for the low-altitude zoom; NULL just means the
     * game runs without zoom                                          */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    g->world_tex = SDL_CreateTexture(g->ren, SDL_PIXELFORMAT_RGBA8888,
                                     SDL_TEXTUREACCESS_TARGET, W, H);
    if (g->world_tex)
        SDL_SetTextureBlendMode(g->world_tex, SDL_BLENDMODE_NONE);
    g->zoom = 1.0f;

    audio_init(&g->audio);

#ifdef __EMSCRIPTEN__
    /* mount IndexedDB-backed FS at /data, then pull persisted scores in;
     * the load happens in hiscores_ready() once the async sync is done.
     * Until then the table just shows empty, which is harmless.        */
    EM_ASM({
        FS.mkdir('/data');
        FS.mount(IDBFS, {}, '/data');
        FS.syncfs(true, function(err) {
            if (!err) Module._hiscores_ready();
        });
    });
#else
    hiscores_load(g);
#endif

    /* stars for the title backdrop */
    for (int i = 0; i < N_STARS; i++)
        g->stars[i] = (Vec2){ frand(&g->rng) * W, frand(&g->rng) * H * 0.7f };

    g->prev_ticks = SDL_GetPerformanceCounter();

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(frame, g, 0, 1);
#else
    while (g->running) frame(g);
#endif

    if (g->audio.dev) SDL_CloseAudioDevice(g->audio.dev);
    if (g->world_tex) SDL_DestroyTexture(g->world_tex);
    SDL_DestroyRenderer(g->ren);
    SDL_DestroyWindow(g->win);
    SDL_Quit();
    return 0;
}
