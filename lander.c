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
 * Native build:
 *   cc -std=c17 -O2 -Wall -Wextra lander.c -o lander $(sdl2-config --cflags --libs) -lm
 *
 * WASM build:
 *   emcc -std=c17 -O2 lander.c -o lander.html -s USE_SDL=2 -s ALLOW_MEMORY_GROWTH=1
 *   (High scores use MEMFS in the browser: per-session only, unless you
 *    mount IDBFS. Audio starts after the first keypress, which the title
 *    screen guarantees.)
 *
 * Controls: LEFT/RIGHT (A/D) rotate, UP (W) thrust,
 *           P pause, ESC end flight, R continue, SPACE start.
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

#define GRAVITY        22.0f    /* px/s^2 at level 1                       */
#define THRUST_ACC     55.0f    /* px/s^2 at full burn                     */
#define ROT_SPEED      2.6f     /* rad/s                                   */
#define FUEL_MAX       100.0f
#define FUEL_BURN      12.0f    /* units/s                                 */
#define SAFE_VY        28.0f    /* px/s max descent for safe landing       */
#define SAFE_VX        18.0f    /* px/s max lateral                        */
#define SAFE_ANGLE     0.20f    /* rad from vertical (~11 deg)             */
#define TERRAIN_N      65       /* must be 2^k + 1 for midpoint displ.     */
#define N_PADS         3
#define N_PARTICLES    256
#define N_STARS        120
#define N_HISCORES     8
#define NAME_MAX       10
#ifdef __EMSCRIPTEN__
#define SCORE_FILE     "/data/lander_hiscores.txt"   /* IDBFS mount */
#else
#define SCORE_FILE     "lander_hiscores.txt"
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
    float         state_time;
    uint64_t      prev_ticks;
    bool          keys[SDL_NUM_SCANCODES];
    bool          running;
    uint32_t      rng;
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

    h[0]             = frange(&g->rng, 0.60f, 0.85f);
    h[TERRAIN_N - 1] = frange(&g->rng, 0.60f, 0.85f);

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
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 1; i < TERRAIN_N; i++) {
            float d = h[i] - h[i - 1];
            if (d >  max_d) h[i] = h[i - 1] + max_d;
            if (d < -max_d) h[i] = h[i - 1] - max_d;
        }
        for (int i = TERRAIN_N - 2; i >= 0; i--) {
            float d = h[i] - h[i + 1];
            if (d >  max_d) h[i] = h[i + 1] + max_d;
            if (d < -max_d) h[i] = h[i + 1] - max_d;
        }
    }

    /* carve flat pads; wider pad -> lower multiplier. Pads narrow at
     * higher levels.                                                    */
    int shrink = g->level >= 4 ? 1 : 0;
    int used[N_PADS];
    for (int p = 0; p < N_PADS; p++) {
        int width = 3 + p - shrink;              /* vertices, >= 2 */
        if (width < 2) width = 2;
        int start;
        bool clash;
        do {
            clash = false;
            start = 2 + (int)(frand(&g->rng) * (TERRAIN_N - width - 4));
            for (int q = 0; q < p; q++)
                if (abs(start - used[q]) < 9) clash = true;
        } while (clash);
        used[p] = start;

        float y = 0.5f * (h[start] + h[start + width - 1]);
        for (int i = 0; i < width; i++) h[start + i] = y;

        /* blend outward so carving can't create a wall steeper than
         * max_d. Pads are >= 9 vertices apart, so a 4-vertex blend can
         * never touch another pad.                                     */
        for (int i = start - 1; i >= 0 && i >= start - 4; i--) {
            float d = h[i] - h[i + 1];
            if (d >  max_d) h[i] = h[i + 1] + max_d;
            if (d < -max_d) h[i] = h[i + 1] - max_d;
        }
        for (int i = start + width; i < TERRAIN_N && i <= start + width + 3; i++) {
            float d = h[i] - h[i - 1];
            if (d >  max_d) h[i] = h[i - 1] + max_d;
            if (d < -max_d) h[i] = h[i - 1] - max_d;
        }

        g->pads[p].x0   = (float)start / (TERRAIN_N - 1) * W;
        g->pads[p].x1   = (float)(start + width - 1) / (TERRAIN_N - 1) * W;
        g->pads[p].y    = y * H;
        g->pads[p].mult = 7 - width;             /* w5->2x ... w2->5x */
    }

    for (int i = 0; i < TERRAIN_N; i++) {
        g->terrain[i].x = (float)i / (TERRAIN_N - 1) * W;
        g->terrain[i].y = h[i] * H;
    }
}

static float terrain_y(const Game *g, float x)
{
    if (x <= 0)     return g->terrain[0].y;
    if (x >= W - 1) return g->terrain[TERRAIN_N - 1].y;
    float fx = x / W * (TERRAIN_N - 1);
    int   i  = (int)fx;
    float t  = fx - i;
    return g->terrain[i].y * (1 - t) + g->terrain[i + 1].y * t;
}

static const Pad *pad_under(const Game *g, float x)
{
    for (int p = 0; p < N_PADS; p++)
        if (x >= g->pads[p].x0 && x <= g->pads[p].x1) return &g->pads[p];
    return NULL;
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

static void new_game(Game *g)
{
    if (g->level < 1) g->level = 1;
    g->gravity = GRAVITY * (1.0f + 0.07f * (float)(g->level - 1));
    gen_terrain(g);
    for (int i = 0; i < N_STARS; i++)
        g->stars[i] = (Vec2){ frand(&g->rng) * W, frand(&g->rng) * H * 0.7f };
    memset(g->particles, 0, sizeof g->particles);
    reset_lander(g);
}

/* run is over (crash or aborted flight): go to name entry or title */
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
            float spread = frange(&g->rng, -0.45f, 0.45f);
            float speed  = frange(&g->rng, 70.0f, 140.0f);
            p->pos = (Vec2){ g->lander.pos.x + s * 10.0f,
                             g->lander.pos.y + c * 10.0f };
            p->vel = (Vec2){
                g->lander.vel.x + (s + spread * c) * speed,
                g->lander.vel.y + (c - spread * s) * speed
            };
            p->life = frange(&g->rng, 0.25f, 0.6f);
            break;
        }
    }
}

static void spawn_explosion(Game *g)
{
    for (int i = 0; i < N_PARTICLES; i++) {
        Particle *p = &g->particles[i];
        float a = frand(&g->rng) * 6.2831853f;
        float v = frange(&g->rng, 30.0f, 220.0f);
        p->pos  = g->lander.pos;
        p->vel  = (Vec2){ cosf(a) * v, sinf(a) * v * 0.8f - 40.0f };
        p->life = frange(&g->rng, 0.5f, 1.6f);
    }
}

static void update_particles(Game *g, float dt)
{
    for (int i = 0; i < N_PARTICLES; i++) {
        Particle *p = &g->particles[i];
        if (p->life <= 0) continue;
        p->life  -= dt;
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

static void update(Game *g, float dt)
{
    g->state_time += dt;

    if (g->state == ST_PAUSED || g->state == ST_TITLE ||
        g->state == ST_ENTRY) {
        audio_set_thrust(&g->audio, false);
        return;
    }

    update_particles(g, dt);
    if (g->state != ST_FLYING) return;   /* landed/crashed: freeze ship */

    Lander *l = &g->lander;

    if (g->keys[SDL_SCANCODE_LEFT]  || g->keys[SDL_SCANCODE_A])
        l->angle -= ROT_SPEED * dt;
    if (g->keys[SDL_SCANCODE_RIGHT] || g->keys[SDL_SCANCODE_D])
        l->angle += ROT_SPEED * dt;

    /* thrust — semi-implicit Euler (symplectic; fine at these dt's) */
    l->thrusting = (g->keys[SDL_SCANCODE_UP] || g->keys[SDL_SCANCODE_W])
                   && l->fuel > 0.0f;
    audio_set_thrust(&g->audio, l->thrusting);

    float ax = 0.0f, ay = g->gravity;
    if (l->thrusting) {
        ax += sinf(l->angle) * THRUST_ACC;   /* +angle tips right */
        ay -= cosf(l->angle) * THRUST_ACC;
        l->fuel -= FUEL_BURN * dt;
        if (l->fuel < 0) l->fuel = 0;
        spawn_exhaust(g, dt);
    }
    l->vel.x += ax * dt;
    l->vel.y += ay * dt;
    l->pos.x += l->vel.x * dt;
    l->pos.y += l->vel.y * dt;

    if (l->pos.x < 0) l->pos.x += W;
    if (l->pos.x > W) l->pos.x -= W;

    /* ground contact: leg tips at (+-13, +12) in body frame.
     * body->world: (bx,by) -> (bx*c - by*s, bx*s + by*c)             */
    float s = sinf(l->angle), c = cosf(l->angle);
    Vec2 fl = { l->pos.x + (-13.0f * c - 12.0f * s),
                l->pos.y + (-13.0f * s + 12.0f * c) };
    Vec2 fr = { l->pos.x + ( 13.0f * c - 12.0f * s),
                l->pos.y + ( 13.0f * s + 12.0f * c) };

    bool hitL = fl.y >= terrain_y(g, fl.x);
    bool hitR = fr.y >= terrain_y(g, fr.x);
    if (!(hitL || hitR)) return;

    const Pad *pad   = pad_under(g, l->pos.x);
    bool on_pad      = pad && pad_under(g, fl.x) && pad_under(g, fr.x);
    float tilt       = fabsf(fmodf(l->angle + 3.14159265f, 6.2831853f)
                             - 3.14159265f);
    bool soft        = fabsf(l->vel.y) <= SAFE_VY &&
                       fabsf(l->vel.x) <= SAFE_VX &&
                       tilt <= SAFE_ANGLE;

    audio_set_thrust(&g->audio, false);
    if (on_pad && soft) {
        g->state = ST_LANDED;
        g->landing_bonus = 50 * pad->mult + (int)(l->fuel);
        g->score += g->landing_bonus;
        l->vel = (Vec2){ 0, 0 };
        l->angle = 0;
        l->pos.y = pad->y - 12.0f;
        audio_trigger_landing(&g->audio);
    } else {
        g->state = ST_CRASHED;
        spawn_explosion(g);
        audio_trigger_explosion(&g->audio);
    }
    g->state_time = 0.0f;
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

    for (int i = 0; i < N_PARTICLES; i++) {
        Particle *p = &g->particles[i];
        if (p->life <= 0) continue;
        int a = (int)(p->life * 400);
        if (a > 255) a = 255;
        SDL_SetRenderDrawColor(g->ren, 255,
                               (uint8_t)(160 + frand(&g->rng) * 60),
                               60, (uint8_t)a);
        SDL_RenderDrawPointF(g->ren, p->pos.x, p->pos.y);
        SDL_RenderDrawPointF(g->ren, p->pos.x + 1, p->pos.y);
    }

    if (g->state != ST_CRASHED) {
        Lander *l = &g->lander;
        float lx = l->pos.x, ly = l->pos.y, la = l->angle;

        /* Eagle the fat cat. Body frame: up is -y; collision feet
         * remain at (+-13, +12), matching the legs drawn below.      */
        SDL_SetRenderDrawColor(g->ren, 255, 170, 80, 255);   /* orange */
        draw_ellipse(g->ren, 0.0f,  3.0f, 10.5f, 8.0f, lx, ly, la); /* body */
        draw_ellipse(g->ren, 0.0f, -8.0f,  6.0f, 5.5f, lx, ly, la); /* head */
        {
            static const Vec2 earL[] = { {-5.0f,-11.5f},{-6.5f,-17.0f},{-1.5f,-13.0f} };
            static const Vec2 earR[] = { { 5.0f,-11.5f},{ 6.5f,-17.0f},{ 1.5f,-13.0f} };
            static const Vec2 tail[] = { {10.0f,6.0f},{14.5f,3.0f},{15.5f,-2.0f},{12.5f,-4.5f} };
            static const Vec2 legL[] = { {-6.0f,9.5f},{-10.0f,12.0f},{-13.0f,12.0f} };
            static const Vec2 legR[] = { { 6.0f,9.5f},{ 10.0f,12.0f},{ 13.0f,12.0f} };
            draw_lines(g->ren, earL, 3, lx, ly, la, 1.0f);
            draw_lines(g->ren, earR, 3, lx, ly, la, 1.0f);
            draw_lines(g->ren, tail, 4, lx, ly, la, 1.0f);
            draw_lines(g->ren, legL, 3, lx, ly, la, 1.0f);
            draw_lines(g->ren, legR, 3, lx, ly, la, 1.0f);
        }

        /* whiskers — prominently visible, three per side */
        SDL_SetRenderDrawColor(g->ren, 240, 240, 250, 255);
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
                draw_lines(g->ren, wsk[i], 2, lx, ly, la, 1.0f);
        }

        /* blue eyes */
        SDL_SetRenderDrawColor(g->ren, 70, 150, 255, 255);
        draw_dot(g->ren, -2.5f, -9.0f, 1.6f, lx, ly, la);
        draw_dot(g->ren,  2.5f, -9.0f, 1.6f, lx, ly, la);

        /* pink nose */
        SDL_SetRenderDrawColor(g->ren, 255, 120, 150, 255);
        draw_dot(g->ren, 0.0f, -6.3f, 1.0f, lx, ly, la);

        if (l->thrusting && g->state == ST_FLYING) {
            SDL_SetRenderDrawColor(g->ren, 255, 190, 60, 255);
            float flick = 6.0f + frand(&g->rng) * 6.0f;
            Vec2 flame[] = { {-4,11},{0,11 + flick},{4,11} };
            draw_lines(g->ren, flame, 3, lx, ly, la, 1.0f);
        }
    }
}

static void render_hud(Game *g)
{
    SDL_SetRenderDrawColor(g->ren, 200, 220, 255, 255);
    char buf[64];
    snprintf(buf, sizeof buf, "F %3.0f", g->lander.fuel);
    draw_text(g->ren, buf, 14, 14, 4.0f);
    snprintf(buf, sizeof buf, "VH %4.0f", g->lander.vel.x);
    draw_text(g->ren, buf, 14, 34, 4.0f);
    float vy = g->lander.vel.y;
    if (vy > SAFE_VY) SDL_SetRenderDrawColor(g->ren, 255, 80, 80, 255);
    snprintf(buf, sizeof buf, "VV %4.0f", vy);
    draw_text(g->ren, buf, 14, 54, 4.0f);
    SDL_SetRenderDrawColor(g->ren, 200, 220, 255, 255);
    snprintf(buf, sizeof buf, "SCORE %d", g->score);
    draw_text(g->ren, buf, W - 190, 14, 4.0f);
    snprintf(buf, sizeof buf, "LEVEL %d", g->level);
    draw_text(g->ren, buf, W - 190, 34, 4.0f);

    SDL_SetRenderDrawColor(g->ren, 90, 200, 255, 255);
    SDL_FRect bar = { 110, 14, 1.2f * g->lander.fuel, 10 };
    SDL_RenderFillRectF(g->ren, &bar);
    SDL_SetRenderDrawColor(g->ren, 200, 220, 255, 255);
    SDL_FRect frame = { 110, 14, 120, 10 };
    SDL_RenderDrawRectF(g->ren, &frame);
}

static void render_title(Game *g)
{
    char buf[64];
    SDL_SetRenderDrawColor(g->ren, 230, 230, 240, 255);
    draw_text_c(g->ren, "LUNAR LANDER", 60, 9.0f);
    SDL_SetRenderDrawColor(g->ren, 255, 170, 80, 255);
    draw_text_c(g->ren, "STARRING EAGLE THE FAT CAT", 105, 4.0f);

    SDL_SetRenderDrawColor(g->ren, 200, 220, 255, 255);
    draw_text_c(g->ren, "LEFT RIGHT OR A D TO ROTATE", 140, 4.0f);
    draw_text_c(g->ren, "UP OR W TO THRUST", 165, 4.0f);
    draw_text_c(g->ren, "P TO PAUSE  ESC TO END FLIGHT", 190, 4.0f);

    SDL_SetRenderDrawColor(g->ren, 80, 255, 120, 255);
    draw_text_c(g->ren, "LAND UPRIGHT ON A GREEN PAD", 235, 4.0f);
    snprintf(buf, sizeof buf, "DOWN SPEED UNDER %.0f  DRIFT UNDER %.0f",
             (double)SAFE_VY, (double)SAFE_VX);
    draw_text_c(g->ren, buf, 260, 4.0f);
    draw_text_c(g->ren, "SCORE IS 50 X PAD BONUS PLUS FUEL LEFT", 285, 4.0f);
    draw_text_c(g->ren, "EACH LEVEL IS ROUGHER AND HEAVIER", 310, 4.0f);
    draw_text_c(g->ren, "A CRASH ENDS THE RUN", 335, 4.0f);

    SDL_SetRenderDrawColor(g->ren, 255, 210, 90, 255);
    draw_text_c(g->ren, "HIGH SCORES", 380, 5.0f);
    SDL_SetRenderDrawColor(g->ren, 200, 220, 255, 255);
    if (g->n_hiscores == 0)
        draw_text_c(g->ren, "NO FLIGHTS RECORDED", 412, 4.0f);
    for (int i = 0; i < g->n_hiscores; i++) {
        snprintf(buf, sizeof buf, "%d  %-10s %6d",
                 i + 1, g->hiscores[i].name, g->hiscores[i].score);
        draw_text_c(g->ren, buf, 412.0f + 22.0f * i, 4.0f);
    }

    if (fmodf(g->state_time, 1.0f) < 0.65f) {
        SDL_SetRenderDrawColor(g->ren, 230, 230, 240, 255);
        draw_text_c(g->ren, "PRESS SPACE TO FLY", H - 40, 5.0f);
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

    render_world(g);
    render_hud(g);

    char buf[64];
    if (g->state == ST_PAUSED) {
        SDL_SetRenderDrawColor(g->ren, 255, 210, 90, 255);
        draw_text_c(g->ren, "PAUSED", H / 2 - 40, 8.0f);
        draw_text_c(g->ren, "P TO RESUME", H / 2 + 20, 4.0f);
    } else if (g->state == ST_LANDED) {
        SDL_SetRenderDrawColor(g->ren, 80, 255, 120, 255);
        draw_text_c(g->ren, "THE EAGLE HAS LANDED", H / 2 - 60, 7.0f);
        snprintf(buf, sizeof buf, "%d PTS", g->landing_bonus);
        draw_text_c(g->ren, buf, H / 2 - 20, 5.0f);
        draw_text_c(g->ren, "R FOR NEXT LEVEL", H / 2 + 20, 4.0f);
    } else if (g->state == ST_CRASHED) {
        SDL_SetRenderDrawColor(g->ren, 255, 90, 90, 255);
        draw_text_c(g->ren, "CRASHED", H / 2 - 60, 7.0f);
        draw_text_c(g->ren, "R TO CONTINUE", H / 2 - 15, 4.0f);
    }

    SDL_RenderPresent(g->ren);
}

/* --------------------------------------------------------------- input   */

static void handle_key(Game *g, SDL_Scancode sc)
{
    switch (g->state) {
    case ST_TITLE:
        if (sc == SDL_SCANCODE_SPACE || sc == SDL_SCANCODE_RETURN) {
            g->level = 1;
            g->score = 0;
            new_game(g);
        } else if (sc == SDL_SCANCODE_ESCAPE) {
            g->running = false;
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
        if (sc == SDL_SCANCODE_R)      { g->level++; new_game(g); }
        else if (sc == SDL_SCANCODE_ESCAPE) end_run(g);
        break;

    case ST_CRASHED:
        if (sc == SDL_SCANCODE_R || sc == SDL_SCANCODE_ESCAPE)
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
    if (dt > 0.05f) dt = 0.05f;

    update(g, dt);
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
    g->rng = (uint32_t)time(NULL) | 1;
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
    SDL_DestroyRenderer(g->ren);
    SDL_DestroyWindow(g->win);
    SDL_Quit();
    return 0;
}
