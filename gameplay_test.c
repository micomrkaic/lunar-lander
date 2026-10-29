/* gameplay_test.c — fixed timestep, fuel/mass, lives, daily seeds,
 * wind, moving plank, zoom, and ghost record/replay.
 *
 * The centrepiece is an autopilot that flies real levels (wind,
 * plank and all) through update(), the same path keyboard input
 * takes. Every landing it makes is saved as a daily ghost, and the
 * ghost must then retrace the flight to the exact same touchdown,
 * both from memory and after a round trip through the ghost file.
 */
#define SCORE_FILE "gameplay_test_scores.txt"
#define GHOST_FILE "gameplay_test_ghost.txt"
#define main lander_main_unused
#include "lander.c"
#undef main

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) {                              \
    if (fails < 30) { printf("FAIL "); printf(__VA_ARGS__); puts(""); }   \
    fails++; } } while (0)

static void seed_level(Game *g, int seed, int level)
{
    memset(g, 0, sizeof *g);
    g->rng   = (uint32_t)(seed * 2654435761u) | 1;
    g->level = level;
    new_game(g);
}

static void set_keys(Game *g, uint8_t in)
{
    g->keys[SDL_SCANCODE_LEFT]  = in & IN_LEFT;
    g->keys[SDL_SCANCODE_RIGHT] = in & IN_RIGHT;
    g->keys[SDL_SCANCODE_UP]    = in & IN_THRUST;
}

/* ------------------------------------------------------- fixed timestep */

static void test_timestep(void)
{
    static Game g;
    seed_level(&g, 1, 1);
    g.lives = 3;
    g.step_acc = 0;
    int n = advance(&g, 0.1005f);
    CHECK(n == 12, "advance(0.1 s) ran %d steps, want 12", n);
    g.step_acc = 0; n = 0;
    for (int i = 0; i < 9; i++) n += advance(&g, 1.0f / 480.0f);
    CHECK(n == 2, "9 x (1/480 s) ran %d steps, want 2 (remainder carried)", n);
    g.step_acc = 0;
    n = advance(&g, 5.0f);
    CHECK(n >= 29 && n <= 30, "a 5 s stall ran %d steps, want the 0.25 s cap", n);
    /* same inputs, different frame pacing -> identical flight */
    static Game a, b;
    seed_level(&a, 9, 6); seed_level(&b, 9, 6);
    a.lives = b.lives = 3;
    set_keys(&a, IN_THRUST | IN_RIGHT); set_keys(&b, IN_THRUST | IN_RIGHT);
    for (int i = 0; i < 90; i++) advance(&a, 1.0f / 30.0f);   /* 30 fps  */
    for (int i = 0; i < 432; i++) advance(&b, 1.0f / 144.0f); /* 144 fps */
    while (a.step < b.step) update(&a, PHYS_DT);   /* align to the same step */
    while (b.step < a.step) update(&b, PHYS_DT);
    CHECK(a.step == b.step && a.lander.pos.x == b.lander.pos.x &&
          a.lander.pos.y == b.lander.pos.y,
          "30 fps vs 144 fps diverged: step %d/%d pos (%.4f,%.4f) vs (%.4f,%.4f)",
          a.step, b.step, a.lander.pos.x, a.lander.pos.y,
          b.lander.pos.x, b.lander.pos.y);
    puts("timestep: fixed 120 Hz, remainder carried, stall capped, "
         "frame-rate independent");
}

/* ---------------------------------------------------------- fuel / mass */

static void test_mass(void)
{
    static Game g;
    seed_level(&g, 3, 1);                     /* level 1: no wind */
    const Pad *pd; bool pl;
    float acc[2];
    const float fuels[2] = { FUEL_MAX, 0.5f };
    for (int k = 0; k < 2; k++) {
        Lander l = { .pos = { W / 2.0f, 100 }, .fuel = fuels[k] };
        step_lander(&g, &l, IN_THRUST, 0, &pd, &pl);
        acc[k] = g.gravity - l.vel.y / PHYS_DT;      /* upward thrust acc */
    }
    CHECK(fabsf(acc[0] - THRUST_ACC) < 0.05f,
          "full-tank thrust %.2f px/s^2, want %.2f", acc[0], THRUST_ACC);
    float want = THRUST_ACC * (1 + FUEL_MASS) / (1 + FUEL_MASS * 0.005f);
    CHECK(fabsf(acc[1] - want) < 0.1f,
          "near-empty thrust %.2f px/s^2, want %.2f", acc[1], want);
    CHECK(acc[1] > 1.3f * acc[0], "burning fuel barely lightens Eagle: "
          "%.1f vs %.1f px/s^2", acc[1], acc[0]);
    printf("mass: thrust %.1f px/s^2 full, %.1f near empty\n", acc[0], acc[1]);
}

/* ------------------------------------------------- lives / fuel economy */

static void test_lives_and_fuel(void)
{
    static Game g;
    memset(&g, 0, sizeof g);
    g.run_seed = 4242; g.level = 4; g.lives = 3;
    start_level(&g, 60.0f);
    Vec2 spawn = g.lander.pos;
    float t3 = g.terrain[3].y;

    /* land on the 5X pad with 40 fuel: +1 life, refuel 20+5*5 = 45 */
    const Pad *p5 = NULL;
    for (int p = 0; p < N_PADS; p++) if (g.pads[p].mult == 5) p5 = &g.pads[p];
    CHECK(p5 != NULL, "level 4 has no 5X pad");
    if (!p5) return;
    g.lander.fuel = 40.0f;
    on_landing(&g, p5, false);
    CHECK(g.lives == 4 && g.extra_life, "5X landing: lives %d, want 4", g.lives);
    CHECK(fabsf(g.lander.fuel - 85.0f) < 1e-4f && fabsf(g.refuel - 45.0f) < 1e-4f,
          "5X refuel: fuel %.1f (+%.1f), want 85 (+45)", g.lander.fuel, g.refuel);
    CHECK(g.landing_bonus == 50 * 5 + 40, "bonus %d, want 290", g.landing_bonus);

    /* refuel never overfills */
    g.lander.fuel = 90.0f;
    on_landing(&g, p5, false);
    CHECK(g.lander.fuel == FUEL_MAX, "refuel overfilled to %.1f", g.lander.fuel);

    /* lives cap at nine */
    g.lives = LIVES_MAX;
    on_landing(&g, p5, false);
    CHECK(g.lives == LIVES_MAX && !g.extra_life, "lives went past %d", LIVES_MAX);

    /* a 2X/3X landing earns no life */
    g.lives = 2;
    const Pad *p3 = NULL;
    for (int p = 0; p < N_PADS; p++) if (g.pads[p].mult == 3) p3 = &g.pads[p];
    if (p3) {
        on_landing(&g, p3, false);
        CHECK(g.lives == 2, "3X landing changed lives to %d", g.lives);
    }

    /* crash -> lose a life; R retries the same moon with level-start fuel */
    g.lives = 2;
    start_level(&g, 60.0f);
    g.lander.fuel = 3.0f;
    g.lander.pos.y = g.terrain[10].y + 50;        /* bury him: instant crash */
    g.lander.pos.x = g.terrain[10].x;
    update(&g, PHYS_DT);
    CHECK(g.state == ST_CRASHED && g.lives == 1, "crash: state %d lives %d",
          g.state, g.lives);
    handle_key(&g, SDL_SCANCODE_R);
    CHECK(g.state == ST_FLYING && g.lander.fuel == 60.0f &&
          g.lander.pos.x == spawn.x && g.lander.pos.y == spawn.y &&
          g.terrain[3].y == t3,
          "retry: state %d fuel %.1f, same moon %d, same spawn %d", g.state,
          g.lander.fuel, g.terrain[3].y == t3,
          g.lander.pos.x == spawn.x && g.lander.pos.y == spawn.y);

    /* last life gone -> R ends the run */
    g.lander.pos.y = g.terrain[10].y + 50;
    g.lander.pos.x = g.terrain[10].x;
    update(&g, PHYS_DT);
    CHECK(g.lives == 0, "lives %d after last crash", g.lives);
    handle_key(&g, SDL_SCANCODE_R);
    CHECK(g.state == ST_TITLE || g.state == ST_ENTRY,
          "out of lives: R led to state %d, not title/entry", g.state);

    /* next level carries fuel over */
    g.lives = 3; g.score = 0;
    start_level(&g, 70.0f);
    g.state = ST_LANDED;
    g.lander.fuel = 33.0f;
    int lv = g.level;
    handle_key(&g, SDL_SCANCODE_R);
    CHECK(g.level == lv + 1 && g.lander.fuel == 33.0f && g.level_fuel == 33.0f,
          "next level: level %d fuel %.1f", g.level, g.lander.fuel);
    puts("lives/fuel: refuel, extra lives (cap 9), retry same moon, carry-over");
}

/* -------------------------------------------------------------- seeds */

static void test_seeds(void)
{
    static Game a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    start_run(&a, true); start_run(&b, true);
    CHECK(a.run_seed == b.run_seed && a.daily_date == today_yyyymmdd(),
          "two daily runs today got different seeds");
    for (int lv = 1; lv <= 6; lv++) {
        a.level = b.level = lv;
        start_level(&a, FUEL_MAX); start_level(&b, FUEL_MAX);
        CHECK(!memcmp(a.terrain, b.terrain, sizeof a.terrain) &&
              a.lander.pos.x == b.lander.pos.x && a.plank.cx == b.plank.cx,
              "daily level %d differs between runs", lv);
    }
    int same = 0;
    for (int i = 0; i < 20; i++) {
        start_run(&a, false); start_run(&b, false);
        same += a.run_seed == b.run_seed;
    }
    CHECK(same == 0, "%d of 20 normal runs repeated a seed", same);
    CHECK(level_seed(7, 1) != level_seed(7, 2) && level_seed(7, 1) != level_seed(8, 1),
          "level seeds collide");
    puts("seeds: daily runs identical, normal runs differ");
}

/* --------------------------------------------------------------- wind */

static void test_wind(void)
{
    int levels_seen_both_signs = 0;
    for (int lv = 1; lv <= 12; lv++) {
        float mx = wind_max(lv), lo = 1e9f, hi = -1e9f;
        for (uint32_t seed = 1; seed < 60; seed++) {
            uint32_t s = level_seed(seed, lv);
            for (int st = 0; st < PHYS_HZ * 60; st += 7) {
                float w = wind_at(s, lv, st);
                if (w < lo) lo = w;
                if (w > hi) hi = w;
                CHECK(w == wind_at(s, lv, st), "wind not deterministic");
            }
        }
        if (lv < WIND_FROM_LEVEL)
            CHECK(lo == 0 && hi == 0, "wind on level %d", lv);
        else {
            CHECK(hi <= mx + 1e-4f && lo >= -mx - 1e-4f,
                  "level %d wind range [%.2f, %.2f] exceeds %.2f", lv, lo, hi, mx);
            CHECK(mx <= 10.0f, "level %d wind cap %.1f > 10", lv, mx);
            levels_seen_both_signs += lo < 0 && hi > 0;
        }
    }
    CHECK(levels_seen_both_signs == 12 - WIND_FROM_LEVEL + 1,
          "wind never blows both ways on some level");
    /* gusts are smooth: no step-to-step jump */
    float worst = 0;
    uint32_t s = level_seed(5, 8);
    for (int st = 1; st < PHYS_HZ * 120; st++) {
        float d = fabsf(wind_at(s, 8, st) - wind_at(s, 8, st - 1));
        if (d > worst) worst = d;
    }
    CHECK(worst < 0.1f, "wind jumps %.3f px/s^2 in one step", worst);
    puts("wind: off before level 3, bounded, smooth, deterministic");
}

/* -------------------------------------------------------------- plank */

static void test_plank(void)
{
    static Game g;
    int present = 0, total = 0;
    for (int lv = 1; lv <= 10; lv++)
        for (int seed = 1; seed <= 400; seed++) {
            seed_level(&g, seed, lv);
            const Plank *pk = &g.plank;
            if (lv < PLANK_FROM_LEVEL) {
                CHECK(!pk->on, "plank on level %d", lv);
                continue;
            }
            total++;
            if (!pk->on) continue;
            present++;
            CHECK(pk->y >= 170.0f, "L%d seed %d: plank at y=%.0f, too high",
                  lv, seed, pk->y);
            for (float x = pk->cx - pk->amp - PLANK_HALF;
                 x <= pk->cx + pk->amp + PLANK_HALF; x += 2.0f)
                if (terrain_y(&g, x) - pk->y < 90.0f - 0.01f) {
                    CHECK(0, "L%d seed %d: plank %.0f px above ground at x=%.0f",
                          lv, seed, terrain_y(&g, x) - pk->y, x);
                    break;
                }
            CHECK(pk->cx - pk->amp - PLANK_HALF > 0 &&
                  pk->cx + pk->amp + PLANK_HALF < W,
                  "L%d seed %d: plank swings off screen", lv, seed);
        }
    CHECK(present >= total * 9 / 10, "plank present on only %d of %d moons",
          present, total);

    /* landing on it: match its speed, settle on top, ride along */
    int landed = 0, tries = 0;
    for (int seed = 1; seed <= 200; seed++) {
        seed_level(&g, seed, 6);
        g.lives = 3;
        if (!g.plank.on) continue;
        tries++;
        g.step = (seed * 37) % (PHYS_HZ * 9);           /* any phase */
        float px = plank_x(&g.plank, g.step);
        g.lander.pos   = (Vec2){ px + 5.0f, g.plank.y - LEG_Y - 3.0f };
        g.lander.vel   = (Vec2){ plank_vx(&g.plank, g.step), 8.0f };
        g.lander.angle = 0.0f;
        for (int i = 0; i < 240 && g.state == ST_FLYING; i++) update(&g, PHYS_DT);
        if (g.state != ST_LANDED || !g.on_plank) {
            CHECK(0, "seed %d: matched-speed plank landing -> state %d",
                  seed, g.state);
            continue;
        }
        CHECK(g.landing_bonus >= 50 * PLANK_MULT,
              "seed %d: plank bonus %d", seed, g.landing_bonus);
        float off = g.lander.pos.x - plank_x(&g.plank, g.step);
        for (int i = 0; i < 300; i++) update(&g, PHYS_DT);
        float off2 = g.lander.pos.x - plank_x(&g.plank, g.step);
        CHECK(fabsf(off - off2) < 0.01f && g.lander.pos.y == g.plank.y - LEG_Y,
              "seed %d: Eagle slid off the moving plank (%.2f -> %.2f)",
              seed, off, off2);
        landed++;

        /* same approach but 40 px/s off its speed: crash */
        seed_level(&g, seed, 6);
        g.lives = 3;
        g.step = (seed * 37) % (PHYS_HZ * 9);
        px = plank_x(&g.plank, g.step);
        g.lander.pos = (Vec2){ px, g.plank.y - LEG_Y - 3.0f };
        g.lander.vel = (Vec2){ plank_vx(&g.plank, g.step) + 40.0f, 8.0f };
        for (int i = 0; i < 240 && g.state == ST_FLYING; i++) update(&g, PHYS_DT);
        CHECK(g.state == ST_CRASHED, "seed %d: 40 px/s mismatch still landed", seed);

        /* flying up into its underside: crash */
        seed_level(&g, seed, 6);
        g.lives = 3;
        px = plank_x(&g.plank, 0);
        g.lander.pos = (Vec2){ px, g.plank.y + PLANK_THICK + 20.0f };
        g.lander.vel = (Vec2){ plank_vx(&g.plank, 0), -60.0f };
        g.gravity = 0;
        for (int i = 0; i < 240 && g.state == ST_FLYING; i++) update(&g, PHYS_DT);
        CHECK(g.state == ST_CRASHED && g.lander.pos.y > g.plank.y,
              "seed %d: head-first into the plank's underside -> state %d",
              seed, g.state);
    }
    printf("plank: present on %d/%d moons, %d/%d matched landings ride it\n",
           present, total, landed, tries);
}

/* -------------------------------------------------------- zoom / alt */

static int orange_px(SDL_Surface *s)
{
    int n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t px = ((uint32_t *)((uint8_t *)s->pixels + y * s->pitch))[x];
            uint8_t r, gg, bb;
            SDL_GetRGB(px, s->format, &r, &gg, &bb);
            if (r == 255 && gg == 170 && bb == 80) n++;
        }
    return n;
}

static void test_zoom(void)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32,
                                                       SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer *ren = SDL_CreateSoftwareRenderer(surf);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_TARGET, W, H);
    CHECK(tex != NULL, "no target texture: %s", SDL_GetError());
    if (!tex) return;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);

    static Game g;
    seed_level(&g, 11, 1);
    g.lives = 3; g.ren = ren; g.world_tex = tex;
    const Pad *pd = &g.pads[0];
    float cx = 0.5f * (pd->x0 + pd->x1);

    /* altitude readout is feet-to-surface */
    g.lander.pos = (Vec2){ cx, pd->y - LEG_Y - 50.0f };
    CHECK(fabsf(altitude(&g) - 50.0f) < 0.01f, "altitude %.2f, want 50",
          altitude(&g));

    /* high up: stays 1x */
    g.lander.pos = (Vec2){ cx, pd->y - LEG_Y - 300.0f };
    g.gravity = 0; g.lander.vel = (Vec2){ 0, 0 };
    for (int i = 0; i < 120; i++) update(&g, PHYS_DT);
    CHECK(fabsf(g.zoom - 1.0f) < 0.01f, "zoomed to %.2f at 300 px", g.zoom);
    g.lives = 0;            /* HUD cat-head icons are orange too: hide them */
    render(&g);
    int far_px = orange_px(surf);

    /* low: eases to 2x within a second, and Eagle draws ~4x the pixels */
    g.lander.pos = (Vec2){ cx, pd->y - LEG_Y - 60.0f };
    for (int i = 0; i < 120; i++) update(&g, PHYS_DT);
    CHECK(g.zoom > 1.95f, "zoom only %.2f after 1 s at 60 px", g.zoom);
    render(&g);
    int near_px = orange_px(surf);
    CHECK(near_px > 3 * far_px, "zoomed Eagle %d px vs %d unzoomed", near_px, far_px);

    /* hysteresis: at 110 px (between thresholds) it holds 2x */
    g.lander.pos.y = pd->y - LEG_Y - 110.0f;
    for (int i = 0; i < 120; i++) update(&g, PHYS_DT);
    CHECK(g.zoom > 1.95f, "zoom dropped to %.2f inside hysteresis band", g.zoom);
    g.lander.pos.y = pd->y - LEG_Y - 200.0f;
    for (int i = 0; i < 120; i++) update(&g, PHYS_DT);
    CHECK(g.zoom < 1.05f, "zoom %.2f after climbing to 200 px", g.zoom);

    /* zoomed view never leaves the moon: Eagle near a corner */
    g.lander.pos = (Vec2){ 5.0f, terrain_y(&g, 5.0f) - LEG_Y - 40.0f };
    for (int i = 0; i < 120; i++) update(&g, PHYS_DT);
    float hw = W / (2 * g.zoom), hh = H / (2 * g.zoom);
    CHECK(g.cam.x >= hw - 0.01f && g.cam.y <= H - hh + 0.01f,
          "camera (%.0f,%.0f) shows off-moon at zoom %.2f", g.cam.x, g.cam.y, g.zoom);

    printf("zoom: 1x high, 2x below %.0f px (%d -> %d Eagle px), hysteresis ok\n",
           ZOOM_IN_ALT, far_px, near_px);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_FreeSurface(surf);
}

/* ------------------------------------------- autopilot + ghost replay */

/* A plain, fuel-aware pilot that reads only what a player can see and
 * acts only through the keys: head for the nearest pad that the plank
 * doesn't swing over, glide down en route while clearing the ground
 * ahead, cancel drift over the pad, then descend gently.            */
static const Pad *ap_target(const Game *g)
{
    const Pad *best = NULL;
    float bestd = 1e9f;
    for (int p = 0; p < N_PADS; p++) {
        const Pad *pd = &g->pads[p];
        float cx = 0.5f * (pd->x0 + pd->x1);
        if (g->plank.on && fabsf(cx - g->plank.cx) <
            g->plank.amp + PLANK_HALF + (pd->x1 - pd->x0) / 2 + 30) continue;
        float d = fabsf(remainderf(cx - g->lander.pos.x, (float)W));
        if (d < bestd) { bestd = d; best = pd; }
    }
    return best;
}

/* highest obstacle (smallest y) between x and x+dx, plank included */
static float ap_ceiling(const Game *g, float x, float dx)
{
    float top = (float)H, sgn = dx < 0 ? -1.0f : 1.0f;
    for (float t = 0; t <= fabsf(dx) + 20.0f; t += 5.0f) {
        float xx = wrap_x(x + sgn * t);
        float y = terrain_y(g, xx);
        if (g->plank.on && fabsf(xx - g->plank.cx) < g->plank.amp + PLANK_HALF + 25)
            y = fminf(y, g->plank.y);
        if (y < top) top = y;
    }
    return top;
}

/* Pilot: work out the acceleration wanted — sideways from the distance
 * to the pad and the time left to fall, vertically from a braking
 * profile (fall freely, then slow so we arrive at a walking pace) —
 * point Eagle along it, and pulse the engine to match its size. Keys
 * only; no access to anything a player can't see.                  */
static float ap_duty;                       /* engine pulse accumulator */

static uint8_t autopilot(const Game *g, const Pad *pd)
{
    const Lander *l = &g->lander;
    float gr   = g->gravity;
    float tx   = 0.5f * (pd->x0 + pd->x1);
    float dx   = remainderf(tx - l->pos.x, (float)W);
    float feet = l->pos.y + LEG_Y;
    float alt  = pd->y - feet;
    float mass = (1.0f + FUEL_MASS * l->fuel / FUEL_MAX) / (1.0f + FUEL_MASS);
    float thr  = THRUST_ACC / mass;
    float vy   = l->vel.y;

    /* vertical: target descent speed for this height */
    float brake = 0.45f * (thr - gr);
    float v_t   = sqrtf(2.0f * brake * fmaxf(alt - 8.0f, 0.0f)) + 6.0f;
    if (fabsf(dx) > 8.0f && alt < 40.0f) v_t = 0.0f;     /* not over it yet */
    float look  = l->vel.x * 1.5f;
    float ceil_y = ap_ceiling(g, l->pos.x, fabsf(look) > 1 ? look : (dx < 0 ? -1.0f : 1.0f));
    if (fabsf(dx) > 8.0f && feet + 30.0f > ceil_y) v_t = -15.0f;   /* climb */
    float ay_up = fmaxf(0.0f, gr + 3.0f * (vy - v_t));

    /* horizontal: cover dx by the time we get down there */
    float t_go = fmaxf(alt / fmaxf(v_t, 20.0f), 1.0f);
    float vx_des = fabsf(dx) < 4.0f ? 0.0f
                 : fmaxf(-100.0f, fminf(100.0f, dx / t_go));
    float ax = 2.0f * (vx_des - l->vel.x) - wind_at(g->lseed, g->level, g->step);

    float lim   = alt < 40.0f ? 0.15f : 0.9f;
    float a_des = atan2f(ax, fmaxf(ay_up, 1.0f));
    a_des = fmaxf(-lim, fminf(lim, a_des));

    uint8_t in = 0;
    if (l->angle < a_des - 0.01f)      in |= IN_RIGHT;
    else if (l->angle > a_des + 0.01f) in |= IN_LEFT;

    float want = sqrtf(ax * ax + ay_up * ay_up) / thr;
    if (fabsf(l->angle - a_des) > 0.3f) want = ay_up / thr;  /* turning: lift only */
    ap_duty += fminf(want, 1.0f);
    if (ap_duty >= 1.0f) { ap_duty -= 1.0f; in |= IN_THRUST; }
    return in;
}

typedef struct { Vec2 pos; float fuel; int step; } Touchdown;

static bool fly_autopilot(Game *g, Touchdown *td)
{
    const Pad *pd = ap_target(g);
    if (!pd) return false;
    ap_duty = 0;
    for (int i = 0; i < MAX_STEPS && g->state == ST_FLYING; i++) {
        set_keys(g, autopilot(g, pd));
        update(g, PHYS_DT);
    }
    set_keys(g, 0);
    td->pos  = g->lander.pos;
    td->fuel = g->lander.fuel - g->refuel;      /* before the landing refuel */
    td->step = g->step;
    return g->state == ST_LANDED;
}

static void test_autopilot_and_ghosts(void)
{
    static Game g;
    remove(GHOST_FILE);
    int landed[9] = {0}, flown[9] = {0}, replays = 0;
    for (int lv = 1; lv <= 8; lv++)
        for (int seed = 1; seed <= 60; seed++) {
            memset(&g, 0, sizeof g);
            g.daily      = true;                 /* so landings save ghosts */
            g.daily_date = 20260101u;
            g.run_seed   = hash32((uint32_t)seed * 7919u);
            g.level      = lv;
            g.lives      = 3;
            memset(g_ghosts, 0, sizeof g_ghosts);
            g_ghost_date = g.daily_date;
            start_level(&g, FUEL_MAX);
            flown[lv]++;

            Touchdown td;
            if (!fly_autopilot(&g, &td)) continue;
            landed[lv]++;
            CHECK(g_ghosts[lv].valid && g_ghosts[lv].n == td.step,
                  "L%d seed %d: landing not saved as ghost (n=%d, steps %d)",
                  lv, seed, g_ghosts[lv].n, td.step);

            /* replay twice: from memory, then from the file */
            for (int pass = 0; pass < 2; pass++) {
                if (pass == 1) ghosts_load(20260101u);
                start_level(&g, 7.0f);          /* player fuel irrelevant */
                CHECK(g.ghost_on, "L%d seed %d: no ghost on retry", lv, seed);
                for (int i = 0; i < MAX_STEPS && g.ghost_flying; i++)
                    update(&g, PHYS_DT);        /* player idles meanwhile */
                CHECK(g.ghost.pos.x == td.pos.x && g.ghost.pos.y == td.pos.y &&
                      fabsf(g.ghost.fuel - td.fuel) < 1e-3f,
                      "L%d seed %d pass %d: ghost touched down at (%.4f,%.4f) "
                      "fuel %.4f, flight was (%.4f,%.4f) fuel %.4f", lv, seed,
                      pass, g.ghost.pos.x, g.ghost.pos.y, g.ghost.fuel,
                      td.pos.x, td.pos.y, td.fuel);
                replays++;
            }
        }
    int tot_l = 0, tot_f = 0;
    printf("autopilot landings by level:");
    for (int lv = 1; lv <= 8; lv++) {
        printf(" L%d %d/%d", lv, landed[lv], flown[lv]);
        tot_l += landed[lv]; tot_f += flown[lv];
        /* every level, wind and plank included, must be flyable */
        CHECK(landed[lv] * 4 >= flown[lv] * 3,
              "autopilot landed only %d/%d on level %d", landed[lv], flown[lv], lv);
    }
    printf("\nghost replay: %d replays matched to the bit\n", replays);

    /* a ghost file from another day is ignored; a damaged one is safe */
    ghosts_load(20260102u);
    int any = 0;
    for (int lv = 1; lv <= N_GHOST_LEVELS; lv++) any += g_ghosts[lv].valid;
    CHECK(any == 0, "yesterday's ghosts loaded today");
    FILE *f = fopen(GHOST_FILE, "w");
    fputs("LANDERGHOST 1 20260101\nL 2 0x1.9p+6 300 500 3\n4:100\n0:999\n", f);
    fclose(f);
    ghosts_load(20260101u);
    CHECK(!g_ghosts[2].valid, "accepted a ghost whose runs overflow its length");
    remove(GHOST_FILE);
}

int main(void)
{
    test_timestep();
    test_mass();
    test_lives_and_fuel();
    test_seeds();
    test_wind();
    test_plank();
    test_zoom();
    test_autopilot_and_ghosts();
    remove(SCORE_FILE);
    printf("gameplay tests: %s (fails=%d)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
