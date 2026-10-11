/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* WAVE: one wavetable oscillator through a Moog ladder filter (LP / BP / HP / NOTCH) or a comb (P_FTYPE).
 * WAVE picks one of 12 tables, SCAN moves through its 64 frames (each table a designed morph), CUT and RES set the filter.
 * Tables: build/gen/kosmo_waves.h (tools/gen_waves.py). CUT and RES are smoothed once per block; SCAN glides (~45 ms) in Q16,
 * and ENV -> SCAN (P_ED_SHP) adds the note's envelope to it. */
#include "kosmo_waves.h"

/* the order of TABLES in tools/gen_waves.py; AF_L has no lowercase, so upper case only */
static const char *const WAVE_NAMES[NWAVES] = {"ROBOT TALK", "LASER", "MONSTER", "ZAP", "CHIPTUNE", "ALIEN", "BUBBLES",
                                               "CRYSTAL", "DRAGON", "GHOST", "SPACE ORGAN", "WILD"};

/* the frame pair and the weight (Q8) of B for SCAN scan_q8 (0..127 << 8) */
static inline void wave_frames(int32_t table, int32_t scan_q8, const int16_t **a, const int16_t **b, int32_t *w)
{
    int32_t fp = scan_q8 * (WAVE_FRAMES - 1) / 127, f = fp >> 8;   /* frame position, Q8: 0..63 << 8 */
    table = clamp(table, 0, NWAVES - 1);
    *a = WAVES[table][f];
    *b = WAVES[table][f < WAVE_FRAMES - 1 ? f + 1 : WAVE_FRAMES - 1];
    *w = fp & 255;
}

/* shortcut: no band-limiting, so high notes alias; add mip-mapped (band-limited) tables when that is audible */
static inline int32_t wave_read(const int16_t *a, const int16_t *b, int32_t w, uint32_t ph)
{
    uint32_t i = ph >> 24, j = (i + 1u) & 255u;
    int32_t fr = (int32_t)((ph >> 9) & 0x7FFFu);
    int32_t sa = a[i] + (((a[j] - a[i]) * fr) >> 15);
    int32_t sb = b[i] + (((b[j] - b[i]) * fr) >> 15);
    return sa + (((sb - sa) * w) >> 8);
}

/* The SCAN pair mixed once into a 256-point frame (plus the wrap point), shared by the voices while the pair and the
 * weight stay the same (SCAN and WAVE settle within a few blocks): a sample is then one interpolation between two
 * table points instead of two, and the frames' mix. Differs from wave_read by rounding only (+-2 of 32768). */
static int16_t wb_tab[257];
static const int16_t *wb_a, *wb_b;
static int32_t wb_w = -1;
static void blend_fill(int16_t *tab, const int16_t *a, const int16_t *b, int32_t w)
{
    uint32_t j;
    for (j = 0; j < 256u; j++) {
        int32_t sa = a[j];
        tab[j] = (int16_t)(sa + (((b[j] - sa) * w) >> 8));
    }
    tab[256] = tab[0];
}
static void wave_blend(const int16_t *a, const int16_t *b, int32_t w)
{
    if (a == wb_a && b == wb_b && w == wb_w)
        return;
    blend_fill(wb_tab, a, b, w);
    wb_a = a;
    wb_b = b;
    wb_w = w;
}
static inline int32_t blend_read(const int16_t *tab, uint32_t ph)
{
    uint32_t i = ph >> 24;
    int32_t a = tab[i];
    return a + (((tab[i + 1u] - a) * (int32_t)((ph >> 9) & 0x7FFFu)) >> 15);
}

/* ENV -> SCAN (P_ED_SHP): each voice has its own pair and weight, and a shared frame would be rebuilt for every voice every
 * block (+18 points of load with 8 voices). A voice whose position moved since its last block reads the pair per sample
 * (about 5 instructions a sample more than the frame); one that stood still reads a frame of its own, rebuilt only
 * when its position changes, so a held note costs nothing. */
typedef struct { const int16_t *a, *b; int32_t w; } wkey_t;
static wkey_t sh_last[NVOICE], sh_built[NVOICE];
static int16_t sh_tab[NVOICE][257];

/* sample i (0..255) of the frame SCAN (0..127) of table, for the UI to draw */
int16_t wave_peek(int table, int scan, int i)
{
    const int16_t *a, *b;
    int32_t w;
    wave_frames(table, clamp(scan, 0, 127) << 8, &a, &b, &w);
    return (int16_t)wave_read(a, b, w, (uint32_t)(i & 255) << 24);
}

static int16_t comb_buf[NVOICE][COMB_N];             /* one comb line per voice; s[7] says it is cleared */

/* slots: s[0] s[1] s[4] s[5] ladder stages (kept on retrigger), s[2] s[3] smoothed CUT RES (Q8), s[6] SCAN (Q16), s[7] comb valid */
static void wave_note_on(track_t *t, voice_t *v)
{
    v->s[0] = v->s[1] = v->s[4] = v->s[5] = v->s[7] = 0;
    v->s[2] = t->p[P_E2] << 8;
    v->s[3] = t->p[P_E3] << 8;
    v->s[6] = t->p[P_E1] << 16;
}

static inline __attribute__((always_inline)) void wave_render_x(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m,
                                                                const int shp)
{
#if KOSMO_PERF
    uint32_t _tw = fm1_ticks();
#endif
    const int16_t *p = t->p, *fa, *fb;
    int32_t sc = v->s[2], sr = v->s[3], ss = v->s[6], w, st[4] = {v->s[0], v->s[1], v->s[4], v->s[5]}, s, bq, bstep;
    uint32_t ph = v->ph[0], inc = m->inc, ft = (uint32_t)p[P_FTYPE], i;
    sc += ((p[P_E2] << 8) - sc) >> 2;
    sr += ((p[P_E3] << 8) - sr) >> 2;
    ss += ((p[P_E1] << 16) - ss) >> 6;                /* a one-pole of 64 blocks (tau 43 ms): a detent is a sweep */
    wave_frames(p[P_E0], clamp((ss >> 8) + m->shape - (64 << 8), 0, 127 << 8), &fa, &fb, &w);
    const int16_t *tab = wb_tab;
    uint32_t mv = 0;
    if (!shp) {
        wave_blend(fa, fb, w);
    } else {
        uint32_t vi = (uint32_t)(v - t->v);
        wkey_t k = {fa, fb, w};
        mv = fa != sh_last[vi].a || fb != sh_last[vi].b || w != sh_last[vi].w;
        sh_last[vi] = k;
        if (!mv) {
            if (fa != sh_built[vi].a || fb != sh_built[vi].b || w != sh_built[vi].w) {
                blend_fill(sh_tab[vi], fa, fb, w);
                sh_built[vi] = k;
            }
            tab = sh_tab[vi];
        }
    }
    bq = (m->amp0 * VOICE_FS) >> 7;                   /* the amplitude ramp times VOICE_FS, Q8 */
    bstep = (((m->amp1 * VOICE_FS) >> 7) - bq) >> CTL_LOG2;
    if (ft > 4u)
        ft = 4;
    if (ft == 4u) {                                   /* COMB: CUT the delay, RES the feedback */
        int16_t *buf = comb_buf[v - t->v];
        int32_t c = clamp(sc + m->cutoff, 0, 127 << 8), d = COMB_D[c >> 8], g = (sr >> 8) * 248;
        if (c < 127 << 8)
            d += ((COMB_D[(c >> 8) + 1] - d) * (c & 255)) >> 8;
        if (!v->s[7]) {
            memset(buf, 0, sizeof comb_buf[0]);
            v->s[7] = 1;
        }
        for (i = 0; i < n; i++) {
            s = (mv ? wave_read(fa, fb, w, ph) : blend_read(tab, ph)) >> 1;
            ph += inc;
            s = soft_knee(comb_tap(buf, &v->ph[1], d, g, s), 16000);
            out[i] += (s * (bq >> 8)) >> 13;
            bq += bstep;
        }
    } else {
        lad_t flt;
        lad_coef(&flt, sc + m->cutoff, sr, ft);
#if KOSMO_PERF
        pfx_s[5] += fm1_ticks() - _tw; pfx_n[5]++; _tw = fm1_ticks();
#endif
#define LAD_LOOP(FT)                                                                           \
    for (i = 0; i < n; i++) {                                                                  \
        s = mv ? wave_read(fa, fb, w, ph) : blend_read(tab, ph);                               \
        ph += inc;                                                                             \
        s = soft_knee(lad_tap(&flt, st, s >> 1, FT), 16000);                                   \
        out[i] += (s * (bq >> 8)) >> 13;                                                       \
        bq += bstep;                                                                           \
    }
        switch (ft) {                                 /* one loop per type: its mix folds into the code */
        case 0: LAD_LOOP(0) break;
        case 1: LAD_LOOP(1) break;
        case 2: LAD_LOOP(2) break;
        default: LAD_LOOP(3) break;
        }
    }
#if KOSMO_PERF
    pfx_s[6] += fm1_ticks() - _tw; pfx_n[6]++;
#endif
    v->ph[0] = ph;
    v->s[0] = st[0];
    v->s[1] = st[1];
    v->s[4] = st[2];
    v->s[5] = st[3];
    v->s[2] = sc;
    v->s[3] = sr;
    v->s[6] = ss;
}

static __attribute__((noinline)) void wave_render_shp(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    wave_render_x(t, v, out, n, m, 1);
}

static void wave_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    if (t->p[P_ED_SHP])
        wave_render_shp(t, v, out, n, m);
    else
        wave_render_x(t, v, out, n, m, 0);
}

static const engine_t ENG_WAVE = {
    .name = "WAVE",
    .edit = {
        {"WAVE", F_ENUM, 0, NWAVES - 1, 0, WAVE_NAMES, 0},
        {"SCAN", F_PCT, 0, 127, 0, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 90, 0, 0},
        {"RES", F_PCT, 0, 127, 30, 0, 0},
    },
    .note_on = wave_note_on,
    .render = wave_render,
    .keep = 0xB3,                /* the filter: s[0] s[1] s[4] s[5], the comb flag s[7] */
};
