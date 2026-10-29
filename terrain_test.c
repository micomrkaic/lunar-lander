/* terrain_test.c — statistical check of gen_terrain.
 * Includes lander.c with main() renamed away, then hammers the
 * generator: every vertex must be inside the window, slopes bounded,
 * pads inside the window and genuinely flat.
 */
#define main lander_main_unused
#include "lander.c"
#undef main

int main(void)
{
    Game g;
    memset(&g, 0, sizeof g);
    int fails = 0;
    float worst_slope = 0, ymin = 1e9f, ymax = -1e9f;

    for (int level = 1; level <= 8; level++) {
        for (int seed = 1; seed <= 2000; seed++) {
            g.rng = (uint32_t)(seed * 2654435761u) | 1;
            g.level = level;
            gen_terrain(&g);

            /* the world wraps, so the ground must join up at the seam */
            if (g.terrain[0].y != g.terrain[TERRAIN_N - 1].y) {
                printf("FAIL L%d seed %d: seam step %.1f px\n", level, seed,
                       g.terrain[TERRAIN_N - 1].y - g.terrain[0].y);
                fails++;
            }
            for (float x = 0.5f; x < 40.0f; x += 1.5f)
                if (fabsf(terrain_y(&g, x) - terrain_y(&g, x + W)) > 0.01f ||
                    fabsf(terrain_y(&g, -x) - terrain_y(&g, W - x)) > 0.01f) {
                    printf("FAIL L%d seed %d: terrain_y not periodic at "
                           "x=%.1f\n", level, seed, x);
                    fails++; break;
                }
            /* every edge, the seam's neighbours included, obeys this
             * level's slope cap (mirrors max_d in gen_terrain)         */
            float cap_d = 0.030f + 0.007f * (float)(level - 1);
            if (cap_d > 0.060f) cap_d = 0.060f;
            float cap = cap_d * H / ((float)W / (TERRAIN_N - 1)) + 1e-3f;

            for (int i = 0; i < TERRAIN_N; i++) {
                float y = g.terrain[i].y;
                if (y < ymin) ymin = y;
                if (y > ymax) ymax = y;
                if (y < 0 || y > H) {
                    printf("FAIL L%d seed %d: vertex %d y=%.1f off-screen\n",
                           level, seed, i, y);
                    fails++;
                }
                if (i > 0) {
                    float dy = fabsf(g.terrain[i].y - g.terrain[i-1].y);
                    float dx = g.terrain[i].x - g.terrain[i-1].x;
                    float slope = dy / dx;
                    if (slope > worst_slope) worst_slope = slope;
                    if (slope > cap) {
                        printf("FAIL L%d seed %d: slope %.2f > cap %.2f "
                               "at vertex %d\n", level, seed, slope, cap, i);
                        fails++;
                    }
                }
            }
            for (int p = 0; p < N_PADS; p++) {
                const Pad *pd = &g.pads[p];
                if (pd->y < 0 || pd->y > H ||
                    pd->x0 < 0 || pd->x1 > W || pd->x1 <= pd->x0) {
                    printf("FAIL L%d seed %d: pad %d bad geometry\n",
                           level, seed, p);
                    fails++;
                }
                /* pad flatness under interpolation */
                for (float x = pd->x0; x <= pd->x1; x += 2.0f)
                    if (fabsf(terrain_y(&g, x) - pd->y) > 0.01f) {
                        printf("FAIL L%d seed %d: pad %d not flat at x=%.0f\n",
                               level, seed, p, x);
                        fails++; break;
                    }
            }
        }
    }
    printf("16000 terrains checked. fails=%d\n", fails);
    printf("y range: [%.0f, %.0f] of window height %d\n", ymin, ymax, H);
    printf("worst slope: %.2f (%.0f deg); 15px-run max wall: %.0f px\n",
           worst_slope, atanf(worst_slope) * 57.2958f, worst_slope * 15.0f);
    return fails ? 1 : 0;
}
