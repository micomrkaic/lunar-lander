/* physics_test.c — collision, attitude, RNG isolation, edge rendering.
 *
 *  1. hull:  Eagle dropped upside down (and on his side) must crash on
 *            first hull contact, centre still well above the rock —
 *            not sink until his feet catch up.
 *  2. wrap:  an upright landing after any number of full rolls
 *            (angle = +-2pi, +-4pi) is still a landing.
 *  3. rng:   flying, thrusting, crashing and rendering never consume
 *            the gameplay RNG; a different FX seed changes nothing
 *            about the flight.
 *  4. ghost: near a screen edge, Eagle is drawn on both sides.
 */
#define main lander_main_unused
#include "lander.c"
#undef main

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) {                              \
    if (fails < 25) { printf("FAIL "); printf(__VA_ARGS__); puts(""); }   \
    fails++; } } while (0)

static void seed_level(Game *g, int seed, int level)
{
    memset(g, 0, sizeof *g);
    g->rng   = (uint32_t)(seed * 2654435761u) | 1;
    g->level = level;
    new_game(g);
}

/* free-fall from just above (x, ground) at the given attitude */
static GameState drop(Game *g, float x, float angle, float clearance)
{
    g->lander.pos   = (Vec2){ x, terrain_y(g, x) - clearance };
    g->lander.vel   = (Vec2){ 0.0f, 5.0f };
    g->lander.angle = angle;
    g->state        = ST_FLYING;
    for (int i = 0; i < 1200 && g->state == ST_FLYING; i++)
        update(g, 1.0f / 120.0f);
    return g->state;
}

static void test_hull(void)
{
    static Game g;
    int n = 0;
    for (int level = 1; level <= 8; level += 3)
        for (int seed = 1; seed <= 300; seed++) {
            seed_level(&g, seed, level);
            for (int p = 0; p < N_PADS; p++) {
                float x = 0.5f * (g.pads[p].x0 + g.pads[p].x1);
                /* inverted: ears point at the ground, 17 px below centre */
                GameState st = drop(&g, x, 3.14159265f, 30.0f);
                float above = g.pads[p].y - g.lander.pos.y;
                CHECK(st == ST_CRASHED, "L%d seed %d: inverted drop did "
                      "not crash", level, seed);
                CHECK(above > 14.0f, "L%d seed %d: inverted Eagle sank to "
                      "%.1f px above pad (ears are 17 px out)",
                      level, seed, above);
                /* on his back, rolled 120 deg: belly/tail lead */
                st = drop(&g, x, 2.1f, 30.0f);
                above = g.pads[p].y - g.lander.pos.y;
                CHECK(st == ST_CRASHED && above > 8.0f, "L%d seed %d: "
                      "rolled drop %s, centre %.1f px above pad", level,
                      seed, st == ST_CRASHED ? "crashed" : "landed", above);
                n += 2;
            }
        }
    printf("hull: %d bad-attitude drops\n", n);
}

static void test_angle_wrap(void)
{
    static Game g;
    const float TAU = 6.2831853f;
    const float turns[] = { TAU, -TAU, 2 * TAU, -2 * TAU };
    int n = 0;
    for (int seed = 1; seed <= 300; seed++) {
        seed_level(&g, seed, 1 + seed % 8);
        for (int p = 0; p < N_PADS; p++)
            for (int k = 0; k < 4; k++)
                for (int t = -1; t <= 1; t++) {        /* small lean too */
                    float x = 0.5f * (g.pads[p].x0 + g.pads[p].x1);
                    float a = turns[k] + 0.1f * (float)t;
                    GameState st = drop(&g, x, a, LEG_Y + 6.0f);
                    CHECK(st == ST_LANDED, "seed %d: upright landing at "
                          "angle %.3f (%+.1f after full rolls) -> %s",
                          seed, a, 0.1f * t,
                          st == ST_CRASHED ? "CRASHED" : "no contact");
                    n++;
                }
    }
    printf("angle wrap: %d landings after full rolls\n", n);
}

/* scripted flight: roll left, burn, roll right, burn, then fall */
static void fly_script(Game *g, SDL_Renderer *ren, int frames)
{
    for (int f = 0; f < frames && g->state == ST_FLYING; f++) {
        memset(g->keys, 0, sizeof g->keys);
        int ph = (f / 40) % 4;
        g->keys[SDL_SCANCODE_UP]    = ph != 3;
        g->keys[SDL_SCANCODE_LEFT]  = ph == 0;
        g->keys[SDL_SCANCODE_RIGHT] = ph == 2;
        update(g, 1.0f / 60.0f);
        if (ren) { g->ren = ren; render(g); }
    }
    memset(g->keys, 0, sizeof g->keys);
    for (int f = 0; f < 2000 && g->state == ST_FLYING; f++) {
        update(g, 1.0f / 60.0f);
        if (ren) render(g);
    }
    for (int f = 0; f < 60; f++) {             /* let explosion/landing play */
        update(g, 1.0f / 60.0f);
        if (ren) render(g);
    }
}

static void test_rng_isolation(SDL_Renderer *ren)
{
    static Game a, b;
    int n = 0;
    for (int seed = 1; seed <= 200; seed++) {
        seed_level(&a, seed, 1 + seed % 8);
        seed_level(&b, seed, 1 + seed % 8);
        a.fx_rng = 0x12345678u;
        b.fx_rng = 0xDEADBEEFu;
        uint32_t before = a.rng;
        fly_script(&a, ren, 240);
        fly_script(&b, NULL, 240);
        CHECK(a.rng == before, "seed %d: gameplay RNG consumed during "
              "flight/render", seed);
        CHECK(a.state == b.state && a.lander.pos.x == b.lander.pos.x &&
              a.lander.pos.y == b.lander.pos.y &&
              a.lander.fuel == b.lander.fuel,
              "seed %d: FX seed changed the flight (%.3f,%.3f) vs "
              "(%.3f,%.3f)", seed, a.lander.pos.x, a.lander.pos.y,
              b.lander.pos.x, b.lander.pos.y);
        n++;
    }
    printf("rng isolation: %d scripted flights, rendered\n", n);
}

/* count Eagle-orange pixels in columns [x0, x1) */
static int orange_in(SDL_Surface *s, int x0, int x1)
{
    int n = 0;
    for (int y = 0; y < H; y++)
        for (int x = x0; x < x1; x++) {
            uint32_t px = ((uint32_t *)((uint8_t *)s->pixels + y * s->pitch))[x];
            uint8_t r, gg, bb;
            SDL_GetRGB(px, s->format, &r, &gg, &bb);
            if (r == 255 && gg == 170 && bb == 80) n++;
        }
    return n;
}

static void test_ghost(SDL_Surface *surf, SDL_Renderer *ren)
{
    static Game g;
    seed_level(&g, 7, 1);
    g.ren = ren;
    const float xs[] = { 4.0f, W - 4.0f };
    for (int k = 0; k < 2; k++) {
        g.lander.pos = (Vec2){ xs[k], H * 0.3f };
        render(&g);
        int left  = orange_in(surf, 0, 30);
        int right = orange_in(surf, W - 30, W);
        CHECK(left > 20 && right > 20, "Eagle at x=%.0f: orange px "
              "left edge %d, right edge %d (need both)", xs[k], left, right);
    }
    g.lander.pos = (Vec2){ W / 2.0f, H * 0.3f };
    render(&g);
    CHECK(orange_in(surf, 0, 30) == 0 && orange_in(surf, W - 30, W) == 0,
          "ghost drawn while Eagle is mid-screen");
    puts("ghost: edge rendering checked");
}

int main(void)
{
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32,
                                                       SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer *ren = surf ? SDL_CreateSoftwareRenderer(surf) : NULL;
    if (!ren) { printf("FAIL software renderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

    test_hull();
    test_angle_wrap();
    test_rng_isolation(ren);
    test_ghost(surf, ren);

    printf("physics tests: %s (fails=%d)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
