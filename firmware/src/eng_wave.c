/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* WAVE: one wavetable oscillator through a Moog ladder filter (LP / BP / HP / NOTCH) or a comb (P_FTYPE).
 * WAVE picks one of 8 tables, SCAN moves through its 8 frames (simple to bright), CUT and RES set the filter.
 * Tables: build/gen/kosmo_waves.h (tools/gen_waves.py). SCAN, CUT and RES are smoothed once per block. */
#include "kosmo_waves.h"

static const char *const WAVE_NAMES[8] = {"ROBOT", "CHOIR", "BELLS", "BUZZ", "ORGAN", "GLASS", "PULSE", "ZAP"};

/* the frame pair and the weight (Q8) of B for SCAN scan_q8 (0..127 << 8) */
static inline void wave_frames(int32_t table, int32_t scan_q8, const int16_t **a, const int16_t **b, int32_t *w)
{
    int32_t fp = scan_q8 * 7 / 127, f = fp >> 8;       /* frame position, Q8: 0..7 << 8 */
    *a = WAVES[table & 7][f];
    *b = WAVES[table & 7][f < 7 ? f + 1 : 7];
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

/* sample i (0..255) of the frame SCAN (0..127) of table, for the UI to draw */
int16_t wave_peek(int table, int scan, int i)
{
    const int16_t *a, *b;
    int32_t w;
    wave_frames(table, clamp(scan, 0, 127) << 8, &a, &b, &w);
    return (int16_t)wave_read(a, b, w, (uint32_t)(i & 255) << 24);
}

static int16_t comb_buf[NVOICE][COMB_N];             /* one comb line per voice; s[7] says it is cleared */

/* slots: s[0] s[1] s[4] s[5] ladder stages (kept on retrigger), s[2] s[3] s[6] smoothed CUT RES SCAN, s[7] comb valid */
static void wave_note_on(track_t *t, voice_t *v)
{
    v->s[0] = v->s[1] = v->s[4] = v->s[5] = v->s[7] = 0;
    v->s[2] = t->p[P_E2] << 8;
    v->s[3] = t->p[P_E3] << 8;
    v->s[6] = t->p[P_E1] << 8;
}

static void wave_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
#if KOSMO_PERF
    uint32_t _tw = fm1_ticks();
#endif
    const int16_t *p = t->p, *fa, *fb;
    int32_t sc = v->s[2], sr = v->s[3], ss = v->s[6], w, st[4] = {v->s[0], v->s[1], v->s[4], v->s[5]}, s;
    uint32_t ph = v->ph[0], inc = m->inc, ft = (uint32_t)p[P_FTYPE], i;
    sc += ((p[P_E2] << 8) - sc) >> 2;
    sr += ((p[P_E3] << 8) - sr) >> 2;
    ss += ((p[P_E1] << 8) - ss) >> 2;
    wave_frames(p[P_E0], ss, &fa, &fb, &w);
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
            s = wave_read(fa, fb, w, ph) >> 1;
            ph += inc;
            s = soft_knee(comb_tap(buf, &v->ph[1], d, g, s), 16000) << 1;
            out[i] += voice_amp(s, m, i) << 1;
        }
    } else {
        lad_t flt;
        lad_coef(&flt, sc + m->cutoff, sr, ft);
#if KOSMO_PERF
        pfx_s[5] += fm1_ticks() - _tw; pfx_n[5]++; _tw = fm1_ticks();
#endif
#define LAD_LOOP(FT)                                                                           \
    for (i = 0; i < n; i++) {                                                                  \
        s = wave_read(fa, fb, w, ph);                                                          \
        ph += inc;                                                                             \
        s = soft_knee(lad_tap(&flt, st, s >> 1, FT), 16000) << 1;                              \
        out[i] += voice_amp(s, m, i) << 1;                                                     \
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

static const engine_t ENG_WAVE = {
    .name = "WAVE",
    .edit = {
        {"WAVE", F_ENUM, 0, 7, 0, WAVE_NAMES, 0},
        {"SCAN", F_PCT, 0, 127, 0, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 90, 0, 0},
        {"RES", F_PCT, 0, 127, 30, 0, 0},
    },
    .note_on = wave_note_on,
    .render = wave_render,
    .keep = 0xB3,                /* the filter: s[0] s[1] s[4] s[5], the comb flag s[7] */
};
