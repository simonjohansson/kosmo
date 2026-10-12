/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* WAVE: one wavetable oscillator through a Moog ladder filter (LP / BP / HP / NOTCH) or a comb (P_FTYPE).
 * WAVE picks one of 8 tables, SCAN moves through its 64 frames (each table a designed morph), CUT and RES set the filter.
 * Tables: build/gen/kosmo_waves.h (tools/gen_waves.py). CUT and RES are smoothed once per block; SCAN glides (~45 ms) in Q16. */
#include "kosmo_waves.h"

/* the order of TABLES in tools/gen_waves.py; AF_L has no lowercase, so upper case only */
static const char *const WAVE_NAMES[NWAVES] = {"ROBOT TALK", "LASER", "MONSTER", "CHIPTUNE", "ALIEN", "CRYSTAL",
                                               "GHOST", "SPACE ORGAN"};

/* the frame pair of mip level m (0: 256 points; m >= 1: 2^WAVE_BITS[m] points, low-passed to their Nyquist) and the weight
 * (Q8) of B for SCAN scan_q8 (0..127 << 8) */
static inline void wave_frames(uint32_t m, int32_t table, int32_t scan_q8, const int16_t **a, const int16_t **b, int32_t *w)
{
    int32_t fp = scan_q8 * (WAVE_FRAMES - 1) / 127, f = fp >> 8;   /* frame position, Q8: 0..63 << 8 */
    const int16_t *base = WAVE_LVL[m] + (((table = clamp(table, 0, NWAVES - 1)) * WAVE_FRAMES) << WAVE_BITS[m]);
    *a = base + (f << WAVE_BITS[m]);
    *b = base + ((f < WAVE_FRAMES - 1 ? f + 1 : WAVE_FRAMES - 1) << WAVE_BITS[m]);
    *w = fp & 255;
}

/* the level a voice plays: the coarsest whose points still hold the note's Nyquist harmonic within a factor of two (P / 4
 * harmonics, inc >= 2^(32 - bits): level 1 from F5, then one octave each), so no harmonic folds back and at most the top
 * octave is missing. Below F5 the full table aliases only a little, and a level for it would cost 16 KB a table. */
static inline uint32_t wave_level(uint32_t inc)
{
    uint32_t m = 0;
    while (m < WAVE_NLVL - 1u && (inc >> (32u - WAVE_BITS[m + 1u])))
        m++;
    return m;
}

static inline int32_t wave_read(const int16_t *a, const int16_t *b, int32_t w, uint32_t ph)
{
    uint32_t i = ph >> 24, j = (i + 1u) & 255u;
    int32_t fr = (int32_t)((ph >> 9) & 0x7FFFu);
    int32_t sa = a[i] + (((a[j] - a[i]) * fr) >> 15);
    int32_t sb = b[i] + (((b[j] - b[i]) * fr) >> 15);
    return sa + (((sb - sa) * w) >> 8);
}

/* The SCAN pair mixed once per mip level into a 256-point frame (plus the wrap point), shared by the voices while the pair and
 * the weight stay the same (SCAN and WAVE settle within a few blocks): a sample is then one interpolation between two table
 * points instead of two, and the frames' mix. A coarse level's points (2^bits, band-limited) are mixed and then spread to 256
 * with a cubic, so the voices' linear read stays clear of the images a linear read of 64 points would fold back. Level 0
 * differs from wave_read by rounding only (+-2 of 32768). */
static int16_t wb_tab[WAVE_NLVL][257];
typedef struct { const int16_t *a, *b; int32_t w; } wkey_t;
static wkey_t wb_key[WAVE_NLVL];
static void blend_fill(int16_t *tab, const int16_t *a, const int16_t *b, int32_t w, uint32_t bits)
{
    uint32_t j, P = 1u << bits, r = 8u - bits;
    int32_t x[256 + 3];
    for (j = 0; j < P; j++) {
        int32_t sa = a[j];
        x[j + 1u] = sa + (((b[j] - sa) * w) >> 8);
    }
    if (!r) {
        for (j = 0; j < 256u; j++)
            tab[j] = (int16_t)x[j + 1u];
    } else {
        x[0] = x[P];
        x[P + 1u] = x[1];
        x[P + 2u] = x[2];
        for (j = 0; j < 256u; j++) {                       /* Catmull-Rom between x[i + 1] and x[i + 2], t in Q10 */
            const int32_t *q = x + (j >> r), t = (int32_t)(j & ((1u << r) - 1u)) << (10u - r), t2 = (t * t) >> 10, t3 = (t2 * t) >> 10;
            int32_t v = ((q[1] * 2) << 10) + (q[2] - q[0]) * t + (2 * q[0] - 5 * q[1] + 4 * q[2] - q[3]) * t2 +
                        (3 * (q[1] - q[2]) + q[3] - q[0]) * t3;
            tab[j] = (int16_t)clamp(v >> 11, -32767, 32767);
        }
    }
    tab[256] = tab[0];
}
static const int16_t *wave_blend(uint32_t m, const int16_t *a, const int16_t *b, int32_t w)
{
    wkey_t *k = &wb_key[m];
    if (a != k->a || b != k->b || w != k->w) {
        blend_fill(wb_tab[m], a, b, w, WAVE_BITS[m]);
        k->a = a;
        k->b = b;
        k->w = w;
    }
    return wb_tab[m];
}
static inline int32_t blend_read(const int16_t *tab, uint32_t ph)
{
    uint32_t i = ph >> 24;
    int32_t a = tab[i];
    return a + (((tab[i + 1u] - a) * (int32_t)((ph >> 9) & 0x7FFFu)) >> 15);
}

/* sample i (0..255) of the frame SCAN (0..127) of table, for the UI to draw */
int16_t wave_peek(int table, int scan, int i)
{
    const int16_t *a, *b;
    int32_t w;
    wave_frames(0, table, clamp(scan, 0, 127) << 8, &a, &b, &w);
    return (int16_t)wave_read(a, b, w, (uint32_t)(i & 255) << 24);
}

static int32_t xbuf[HALF_FRAMES];                  /* the block of a level change: old level cross-faded to the new */
static int16_t comb_buf[NVOICE][COMB_N];             /* one comb line per voice; s[7] says it is cleared */

/* slots: s[0] s[1] s[4] s[5] ladder stages (kept on retrigger), s[2] s[3] smoothed CUT RES (Q8), s[6] SCAN (Q16), s[7] comb valid */
static void wave_note_on(track_t *t, voice_t *v)
{
    v->s[0] = v->s[1] = v->s[4] = v->s[5] = v->s[7] = 0;
    v->s[2] = t->p[P_E2] << 8;
    v->s[3] = t->p[P_E3] << 8;
    v->s[6] = t->p[P_E1] << 16;
}

static void wave_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
#if KOSMO_PERF
    uint32_t _tw = fm1_ticks();
#endif
    const int16_t *p = t->p, *fa, *fb;
    int32_t sc = v->s[2], sr = v->s[3], ss = v->s[6], w, st[4] = {v->s[0], v->s[1], v->s[4], v->s[5]}, s, bq, bstep;
    uint32_t ph = v->ph[0], inc = m->inc, ft = (uint32_t)p[P_FTYPE], i, lv = wave_level(inc), pl = v->ph[2], xf = 0;
    sc += ((p[P_E2] << 8) - sc) >> 2;
    sr += ((p[P_E3] << 8) - sr) >> 2;
    ss += ((p[P_E1] << 16) - ss) >> 6;                /* a one-pole of 64 blocks (tau 43 ms): a detent is a sweep */
    int32_t scan = clamp(ss >> 8, 0, 127 << 8);
    const int16_t *tab;
    wave_frames(lv, p[P_E0], scan, &fa, &fb, &w);
    tab = wave_blend(lv, fa, fb, w);
    if (pl - 1u < WAVE_NLVL && pl - 1u != lv && n <= HALF_FRAMES) {   /* the level changed (a bend or glide): cross-fade the block */
        const int16_t *oa, *ob, *ot;
        int32_t ow, x = 0, dx = 32768 / (int32_t)n;
        wave_frames(pl - 1u, p[P_E0], scan, &oa, &ob, &ow);
        ot = wave_blend(pl - 1u, oa, ob, ow);
        for (i = 0; i < n; i++, x += dx) {
            int32_t o = blend_read(ot, ph + i * inc), c = blend_read(tab, ph + i * inc);
            xbuf[i] = o + (((c - o) * x) >> 15);
        }
        xf = 1;
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
            s = (xf ? xbuf[i] : blend_read(tab, ph)) >> 1;
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
        s = xf ? xbuf[i] : blend_read(tab, ph);                                            \
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
    v->ph[2] = lv + 1u;
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
        {"WAVE", F_ENUM, 0, NWAVES - 1, 0, WAVE_NAMES, 0},
        {"SCAN", F_PCT, 0, 127, 0, 0, 0},
        {"CUT", F_CUTOFF, 0, 127, 90, 0, 0},
        {"RES", F_PCT, 0, 127, 30, 0, 0},
    },
    .note_on = wave_note_on,
    .render = wave_render,
    .keep = 0xB3,                /* the filter: s[0] s[1] s[4] s[5], the comb flag s[7] */
};
