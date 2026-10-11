/* SPDX-License-Identifier: GPL-3.0-only */
/* Factory presets (toys): the PRESETS encoder steps through them (ui.c). No user saving. Each one sets every
 * parameter the UI pages show, plus the filter's ENV amount and the voice mode, so none leaks into the next. */
typedef struct {
    const char *name;            /* AF_L, up to 11 characters fit the banner */
    uint8_t e[4];                /* WAVE SCAN CUT RES */
    uint8_t ftype;
    uint8_t env[4];              /* ATK DEC SUS REL */
    uint8_t rev, rsize, voice;
    int8_t fenv;
} preset_t;

static const preset_t PRESETS[] = {
    {"SPACE WHALE", {1, 30, 60, 40}, 0, {90, 90, 110, 100}, 110, 127, V_POLY, 10},
    {"LASER CAT", {7, 127, 100, 90}, 1, {0, 50, 20, 30}, 20, 40, V_LEGATO, 48},
    {"ROBOT DOG", {0, 40, 90, 30}, 0, {4, 70, 100, 50}, 20, 30, V_LEGATO, 20},
    {"MAGIC BELLS", {2, 90, 110, 20}, 2, {0, 88, 0, 60}, 90, 100, V_POLY, 30},
    {"TOY ORGAN", {4, 60, 127, 0}, 0, {2, 80, 100, 40}, 30, 60, V_POLY, 0},
    {"BUZZY BEE", {3, 20, 70, 70}, 1, {0, 60, 40, 30}, 10, 20, V_LEGATO, 30},
    {"GLASS GHOST", {5, 50, 80, 50}, 3, {60, 80, 90, 80}, 100, 110, V_POLY, 0},
    {"PIXEL PUP", {6, 64, 100, 20}, 0, {0, 40, 60, 20}, 15, 30, V_LEGATO, 0},
};
#define NPRESETS ((uint32_t)NELEM(PRESETS))

static void preset_load(uint32_t i)
{
    const preset_t *s = &PRESETS[i % NPRESETS];
    uint32_t k;
    for (k = 0; k < 4u; k++) {
        trk[0].p[P_E0 + k] = s->e[k];
        trk[0].p[P_ATK + k] = s->env[k];
    }
    trk[0].p[P_FTYPE] = s->ftype;
    trk[0].p[P_REV] = s->rev;
    trk[0].p[P_VOICE] = s->voice;
    trk[0].p[P_ED_FLT] = s->fenv;
    song.g[G_RSIZE] = s->rsize;
}
