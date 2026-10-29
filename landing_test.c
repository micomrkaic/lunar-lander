/* landing_test.c — every pad must be landable through the real
 * collision code.
 *
 * For each of 16,000 terrains (2,000 seeds x 8 levels) and each pad,
 * drop Eagle gently onto the pad centre — upright and at just under
 * the legal tilt either way — run update() until contact, and require
 * ST_LANDED. Also asserts each pad leaves a usable horizontal margin
 * beyond Eagle's stance at maximum legal tilt.
 */
#define main lander_main_unused
#include "lander.c"
#undef main

/* horizontal reach of the outermost foot from the body centre at tilt a */
static float foot_reach(float a)
{
    return LEG_X * cosf(a) + LEG_Y * sinf(fabsf(a));
}

static GameState drop_onto(Game *g, const Pad *pd, float angle)
{
    float cx = 0.5f * (pd->x0 + pd->x1);
    g->lander.pos   = (Vec2){ cx, pd->y - LEG_Y - LEG_X * fabsf(sinf(angle)) - 6.0f };
    g->lander.vel   = (Vec2){ 0.0f, 5.0f };
    g->lander.angle = angle;
    g->lander.fuel  = FUEL_MAX;
    g->state        = ST_FLYING;
    for (int step = 0; step < 600 && g->state == ST_FLYING; step++)
        update(g, 1.0f / 120.0f);
    return g->state;
}

int main(void)
{
    static Game g;
    int fails = 0, drops = 0;
    float min_margin = 1e9f;
    const float tilt = SAFE_ANGLE - 0.01f;
    const float need = 2.0f * foot_reach(tilt);   /* stance at max tilt */

    for (int level = 1; level <= 8; level++) {
        for (int seed = 1; seed <= 2000; seed++) {
            memset(&g, 0, sizeof g);
            g.rng   = (uint32_t)(seed * 2654435761u) | 1;
            g.level = level;
            new_game(&g);

            /* pad set per level: 4X/3X/2X on 1-3, 5X/4X/3X from 4 */
            int lo_mult = level >= 4 ? 3 : 2, seen = 0;
            for (int p = 0; p < N_PADS; p++) {
                int m = g.pads[p].mult;
                if (m >= lo_mult && m < lo_mult + N_PADS)
                    seen |= 1 << (m - lo_mult);
            }
            if (seen != (1 << N_PADS) - 1) {
                if (fails < 20)
                    printf("FAIL L%d seed %d: wrong pad multipliers "
                           "%dX %dX %dX\n", level, seed, g.pads[0].mult,
                           g.pads[1].mult, g.pads[2].mult);
                fails++;
            }

            for (int p = 0; p < N_PADS; p++) {
                Pad pd = g.pads[p];
                /* width is fixed by multiplier: 5X=45 ... 2X=90 px */
                float want = (float)W / (TERRAIN_N - 1) * (float)(8 - pd.mult);
                if (fabsf((pd.x1 - pd.x0) - want) > 0.01f) {
                    if (fails < 20)
                        printf("FAIL L%d seed %d: %dX pad is %.0f px, "
                               "expected %.0f\n", level, seed, pd.mult,
                               pd.x1 - pd.x0, want);
                    fails++;
                }
                float margin = (pd.x1 - pd.x0) - need;
                if (margin < min_margin) min_margin = margin;
                if (margin < 8.0f) {
                    if (fails < 20)
                        printf("FAIL L%d seed %d: %dX pad %.0f px wide, "
                               "stance at max tilt needs %.1f (+8 slack)\n",
                               level, seed, pd.mult, pd.x1 - pd.x0, need);
                    fails++;
                }
                const float angles[3] = { 0.0f, -tilt, tilt };
                for (int k = 0; k < 3; k++) {
                    drops++;
                    GameState st = drop_onto(&g, &pd, angles[k]);
                    if (st != ST_LANDED) {
                        if (fails < 20)
                            printf("FAIL L%d seed %d: %dX pad (%.0f px) "
                                   "angle %+.2f -> %s\n",
                                   level, seed, pd.mult, pd.x1 - pd.x0,
                                   angles[k],
                                   st == ST_CRASHED ? "CRASHED" : "no contact");
                        fails++;
                    }
                }
            }
        }
    }
    printf("%d gentle drops onto pads. fails=%d\n", drops, fails);
    printf("narrowest pad margin beyond max-tilt stance: %.1f px\n",
           min_margin);
    return fails ? 1 : 0;
}
