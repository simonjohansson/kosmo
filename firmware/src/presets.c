/* SPDX-License-Identifier: GPL-3.0-only */
/* Factory presets (toys): the PRESETS encoder steps through them (ui.c). No user saving. Each one sets every
 * parameter the UI pages show, plus the filter's ENV amount and the voice mode, so none leaks into the next. */
typedef struct {
    const char *name;            /* AF_L, up to 11 characters fit the banner */
    uint8_t e[4];                /* WAVE SCAN CUT RES */
    uint8_t ftype;
    uint8_t env[4];              /* ATK DEC SUS REL */
    uint8_t rev, rsize, rprog, voice;   /* rprog: the SPACE program */
    int8_t fenv;
    uint8_t fat, detune;         /* FAT copies, DETUNE */
    uint8_t echo[4];             /* ECHO TIME REPEATS CHARACTER AMOUNT; AMOUNT 0: off, the others at their power-on values */
} preset_t;

static const preset_t PRESETS[] = {
    {"SPACE WHALE", {5, 38, 60, 40}, 0, {90, 90, 110, 100}, 100, 127, 3, V_POLY, 10, 2, 60, {0}},
    {"LASER CAT", {1, 90, 100, 90}, 1, {0, 50, 20, 30}, 20, 40, 2, V_LEGATO, 48, 0, 0, {36, 64, EC_CLEAN, 50}},
    {"ROBOT DOG", {0, 40, 90, 30}, 0, {4, 70, 100, 50}, 40, 30, 4, V_LEGATO, 20, 0, 0, {70, 0, EC_ROBOT, 70}},
    {"MAGIC BELLS", {7, 90, 70, 20}, 2, {0, 88, 0, 60}, 90, 100, 1, V_POLY, 30, 0, 0, {0}},
    {"TOY ORGAN", {10, 60, 127, 0}, 0, {2, 80, 100, 40}, 30, 60, 0, V_POLY, 0},
    {"BUZZY BEE", {2, 80, 70, 70}, 4, {0, 60, 40, 30}, 10, 20, 0, V_LEGATO, 30},
    {"GLASS GHOST", {9, 50, 80, 50}, 3, {60, 80, 90, 80}, 100, 110, 5, V_POLY, 0},
    {"PIXEL PUP", {4, 20, 100, 20}, 0, {0, 40, 60, 20}, 15, 30, 0, V_LEGATO, 0, 0, 0, {24, 56, EC_TOY, 60}},
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
    trk[0].p[P_E4] = s->fat;
    trk[0].p[P_DETUNE] = s->detune;
    trk[0].p[P_FTYPE] = s->ftype;
    trk[0].p[P_REV] = s->rev;
    trk[0].p[P_VOICE] = s->voice;
    trk[0].p[P_ED_FLT] = s->fenv;
    song.g[G_RSIZE] = s->rsize;
    song.g[G_RPROG] = s->rprog;
    song.g[G_RTONE] = GP[G_RTONE].def;
    for (k = 0; k < 4u; k++)
        song.g[G_ETIME + k] = s->echo[3] ? s->echo[k] : GP[G_ETIME + k].def;
}
