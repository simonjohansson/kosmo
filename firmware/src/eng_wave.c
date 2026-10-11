/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* WAVE: one wavetable oscillator through a state-variable filter (LP / BP / HP / NOTCH, P_FTYPE).
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

static void wave_note_on(track_t *t, voice_t *v)
{
    v->s[0] = v->s[1] = 0;                            /* filter */
    v->s[2] = t->p[P_E2] << 8;                        /* smoothed CUT, RES, SCAN (Q8) */
    v->s[3] = t->p[P_E3] << 8;
    v->s[4] = t->p[P_E1] << 8;
}

static void wave_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p, *fa, *fb;
    int32_t sc = v->s[2], sr = v->s[3], ss = v->s[4], w, ic1 = v->s[0], ic2 = v->s[1];
    uint32_t ph = v->ph[0], inc = m->inc, ft = (uint32_t)p[P_FTYPE] & 3u, i;
    tsvf_t flt;
    int32_t k;
    sc += ((p[P_E2] << 8) - sc) >> 2;
    sr += ((p[P_E3] << 8) - sr) >> 2;
    ss += ((p[P_E1] << 8) - ss) >> 2;
    wave_frames(p[P_E0], ss, &fa, &fb, &w);
    k = 8192 - (sr >> 8) * 7600 / 127;                /* tsvf_coef's damping, Q12 */
    tsvf_coef_k(&flt, sc + m->cutoff, k);
    for (i = 0; i < n; i++) {
        int32_t s = wave_read(fa, fb, w, ph);
        ph += inc;
        s = soft_knee(tsvf_tap(&flt, k, s >> 1, &ic1, &ic2, ft), 16000) << 1;
        out[i] += voice_amp(s, m, i) << 1;
    }
    v->ph[0] = ph;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = sc;
    v->s[3] = sr;
    v->s[4] = ss;
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
    .keep = 0x03,                /* the filter */
};
