/* Use a private scores file: this test deletes and rewrites it, and
 * must never touch the player's real lander_hiscores.txt.            */
#define SCORE_FILE "hiscore_test_scores.txt"
#define main lander_main_unused
#include "lander.c"
#undef main
#include <unistd.h>
int main(void) {
    if (strcmp(SCORE_FILE, "hiscore_test_scores.txt") != 0) {
        puts("FAIL test is pointed at the real scores file"); return 1;
    }
    remove(SCORE_FILE);
    Game g; memset(&g, 0, sizeof g);
    /* fill beyond capacity, unsorted */
    int vals[] = {120, 900, 340, 55, 780, 780, 10, 660, 205, 999, 1};
    char nm[16];
    for (int i = 0; i < 11; i++) {
        snprintf(nm, sizeof nm, "P%d", i);
        if (hiscores_qualifies(&g, vals[i])) hiscores_insert(&g, nm, vals[i]);
    }
    /* must be sorted desc, capped at N_HISCORES */
    if (g.n_hiscores != N_HISCORES) { puts("FAIL count"); return 1; }
    for (int i = 1; i < g.n_hiscores; i++)
        if (g.hiscores[i].score > g.hiscores[i-1].score) { puts("FAIL order"); return 1; }
    if (g.hiscores[0].score != 999) { puts("FAIL top"); return 1; }
    /* score of 1 and 10 must not have displaced anything */
    if (g.hiscores[N_HISCORES-1].score < 55) { puts("FAIL tail"); return 1; }
    /* zero score never qualifies */
    if (hiscores_qualifies(&g, 0)) { puts("FAIL zero"); return 1; }
    /* empty name becomes PILOT */
    Game g2; memset(&g2, 0, sizeof g2);
    hiscores_insert(&g2, "", 42);
    if (strcmp(g2.hiscores[0].name, "PILOT")) { puts("FAIL default name"); return 1; }
    /* round-trip through the file */
    Game g3; memset(&g3, 0, sizeof g3);
    hiscores_save(&g);      /* overwrite with g's table */
    hiscores_load(&g3);
    if (g3.n_hiscores != g.n_hiscores) { puts("FAIL reload count"); return 1; }
    for (int i = 0; i < g.n_hiscores; i++)
        if (g3.hiscores[i].score != g.hiscores[i].score ||
            strcmp(g3.hiscores[i].name, g.hiscores[i].name))
            { puts("FAIL roundtrip"); return 1; }
    remove(SCORE_FILE);

    /* XDG save location (GHOST_FILE is not pinned here, so ghost_path()
     * is the real resolver). Sandboxed under the current directory.   */
    char cwd[512], dir[600], want[700];
    struct stat st;
    if (!getcwd(cwd, sizeof cwd)) { puts("FAIL getcwd"); return 1; }
    snprintf(dir, sizeof dir, "%s/xdg_test_tmp", cwd);
    setenv("XDG_DATA_HOME", dir, 1);
    snprintf(want, sizeof want, "%s/lander/ghost.txt", dir);
    if (strcmp(ghost_path(), want)) { printf("FAIL xdg path %s\n", ghost_path()); return 1; }
    snprintf(want, sizeof want, "%s/lander", dir);
    if (stat(want, &st) || !S_ISDIR(st.st_mode)) { puts("FAIL xdg dir not created"); return 1; }
    rmdir(want); rmdir(dir);

    setenv("XDG_DATA_HOME", "relative/ignored", 1);   /* spec: ignore */
    snprintf(dir, sizeof dir, "%s/home_test_tmp", cwd);
    setenv("HOME", dir, 1);
    snprintf(want, sizeof want, "%s/.local/share/lander/ghost.txt", dir);
    if (strcmp(ghost_path(), want)) { printf("FAIL home path %s\n", ghost_path()); return 1; }
    const char *chain[] = { "/.local/share/lander", "/.local/share", "/.local", "" };
    for (int i = 0; i < 4; i++) {
        snprintf(want, sizeof want, "%s%s", dir, chain[i]);
        if (i == 0 && (stat(want, &st) || !S_ISDIR(st.st_mode))) {
            puts("FAIL home dir not created"); return 1;
        }
        rmdir(want);
    }
    puts("hiscore tests: ALL PASS");
    return 0;
}
