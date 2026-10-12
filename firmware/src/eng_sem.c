/* SPDX-License-Identifier: GPL-3.0-only */
/* SEM: the oscillator section of an Oberheim SEM. Two band-limited (polyBLEP) oscillators per voice, each a SAW or a PULSE with
 * its own width (5 .. 95 %, 50 % = square); VCO2 is tuned against VCO1 (TUNE in semitones, FINE in cents) and can be hard-synced
 * to it (the reset is band-limited with a BLEP too); MIX crossfades them. Then the same filter, envelope and chain as WAVE
 * (FILTER_BLOCK, dsp.c). No FAT / DETUNE, no modulation (issue #33).
 * Parameters P_S0..P_S7: VCO1 wave, VCO1 PW, VCO2 wave, VCO2 PW, TUNE (24 = 0, +-24 semitones), FINE (50 = 0, +-50 cents), SYNC, MIX.
 * Slots: ph[0] VCO1, ph[2] VCO2, s[6] s[8] s[9] the oscillators' last samples and a pending BLEP term, as WAVE for the rest. */
#define SEM_PW_MAX 126
static const char *const SEM_WAVE_NAMES[2] = {"SAW", "PULSE"};

/* PW 0 .. 126 as the phase (Q32) the pulse stays high for: 5 % + v * 90 % / 126 (63 = 50 %) */
static inline uint32_t sem_pw(int32_t v) { return 214748365u + (uint32_t)clamp(v, 0, SEM_PW_MAX) * 30678338u; }
/* the smoothed PWs live in ph[1], Q8, 16 bits each: PW1 high; a note starts on the knobs' values */
static inline uint32_t sem_pw_start(const int16_t *p) { return (uint32_t)clamp(p[P_S1], 0, SEM_PW_MAX) << 24 | (uint32_t)clamp(p[P_S3], 0, SEM_PW_MAX) << 8; }
/* the width from the smoothed PW (Q8) */
static inline uint32_t sem_pwq(uint32_t q)
{
    if (q > SEM_PW_MAX * 256u)
        q = SEM_PW_MAX * 256u;
    return 214748365u + (q >> 8) * 30678338u + (q & 255u) * 119837u;
}

/* the naive shape at phase ph (+-32768): SAW, or the zero-mean PULSE (osc_pulse without the BLEPs); the UI draws it */
static inline int32_t sem_naive(int32_t pulse, uint32_t ph, uint32_t pw)
{
    return pulse ? ((int32_t)(ph >> 16) - (int32_t)((ph + pw) >> 16)) >> 1 : (int32_t)(ph >> 16) - 32768;
}

/* (x / 3 and x / 24 as multiplications: the compiler keeps a divide at -Os.) The BLEP of a step J (+-65536 = a saw's wrap) that happened f (Q15) of a sample ago, weight w (0..128) in the mix: the integral of a cubic
 * B-spline over four samples (steeper than dsp.c's two-sample polyBLEP, whose aliasing at C6 is about WAVE's worst; this one is ~8 dB
 * lower near Nyquist). It corrects the sample now (x), the one before (p1), two before (p2) and the next (c1). */
#define SEM_BLS(J, c, w) ((((((J) >> 2) * (c)) >> 13) * (w)) >> 7)
static inline void sem_bl(int32_t J, int32_t f, int32_t w, int32_t *x, int32_t *p1, int32_t *p2, int32_t *c1)
{
    int32_t g = 32768 - f, f2 = (f * f) >> 15, f3 = (f2 * f) >> 15, f4 = (f3 * f) >> 15, g2 = (g * g) >> 15, g3 = (g2 * g) >> 15,
            g4 = (g3 * g) >> 15;
    *x += SEM_BLS(J, -16384 + ((2 * f * 10923) >> 15) - ((f3 * 10923) >> 15) + (f4 >> 3), w);
    *c1 += SEM_BLS(J, -((g4 * 1365) >> 15), w);
    *p1 += SEM_BLS(J, 16384 - ((2 * g * 10923) >> 15) + ((g3 * 10923) >> 15) - (g4 >> 3), w);
    *p2 += SEM_BLS(J, (f4 * 1365) >> 15, w);
}
static inline int32_t sem_frac(uint32_t ph, uint32_t d) { uint32_t f = ph / d; return (int32_t)(f > 32767u ? 32767u : f); }

static void sem_note_on(track_t *t, voice_t *v)
{
    v->s[0] = v->s[1] = v->s[4] = v->s[5] = v->s[6] = v->s[7] = v->s[8] = v->s[9] = 0;
    v->s[2] = t->p[P_E2] << 8;
    v->s[3] = t->p[P_E3] << 8;
    v->ph[1] = sem_pw_start(t->p);
}

/* the oscillators' block, mixed, into xbuf (the filter's input), two samples late (the BLEP reaches back two); state ph[0], ph[2],
 * s[6] s[8] the last two samples, s[9] the correction waiting for the next. W1, W2 (PULSE) and SYNC are constants where the wave and
 * SYNC are known: sem_osc picks the loop, so a SAW does not pay for the PULSE's second edge */
_Static_assert(CTL <= HALF_FRAMES, "sem_osc: the block fits xbuf");
__attribute__((always_inline)) static inline void sem_loop(const int16_t *p, voice_t *v, uint32_t n, const vmod_t *m, uint32_t pw1, uint32_t pw2,
                                                          const int W1, const int W2, const int SYNC)
{
    int32_t mx = clamp(p[P_S7], 0, 128), wa = 128 - mx, p1 = v->s[6], p2 = v->s[8], c1 = v->s[9];
    uint32_t i, ph1 = v->ph[0], ph2 = v->ph[2], inc1 = m->inc, q;
    uint32_t inc2 = cents_inc(m->pitch16, (p[P_S4] - 24) * 100 + p[P_S5] - 50, m->fine), d1 = (inc1 >> 15) ? inc1 >> 15 : 1u;
    uint32_t d2 = (inc2 >> 15) ? inc2 >> 15 : 1u;
    const int32_t jw1 = W1 ? 32768 : 65536, jw2 = W2 ? 32768 : 65536;   /* a PULSE's edges are half a saw's step */
    for (i = 0; i < n; i++) {
        int32_t x = c1, f1 = 0;
        c1 = 0;
        ph1 += inc1;
        ph2 += inc2;
        if (ph1 < inc1) {                              /* VCO1 wrapped (a SAW's step, a PULSE's fall) */
            f1 = sem_frac(ph1, d1);
            sem_bl(-jw1, f1, wa, &x, &p1, &p2, &c1);
            if (SYNC) {                                /* SYNC: VCO2 restarts, its phase the time since the wrap on */
                uint32_t rst = (inc2 >> 15) * (uint32_t)f1;
                int32_t fe;
                if (ph2 < inc2 && (fe = sem_frac(ph2, d2)) > f1)      /* its own steps before the reset only: later ones never happen */
                    sem_bl(-jw2, fe, mx, &x, &p1, &p2, &c1);
                if (W2 && (q = ph2 + pw2) < inc2 && (fe = sem_frac(q, d2)) > f1)
                    sem_bl(32768, fe, mx, &x, &p1, &p2, &c1);
                sem_bl(sem_naive(W2, 0, pw2) - sem_naive(W2, ph2 - rst, pw2), f1, mx, &x, &p1, &p2, &c1);
                ph2 = rst;
            }
        }
        if (W1 && (q = ph1 + pw1) < inc1)               /* its rise */
            sem_bl(32768, sem_frac(q, d1), wa, &x, &p1, &p2, &c1);
        if (!SYNC || ph1 >= inc1) {
            if (ph2 < inc2)
                sem_bl(-jw2, sem_frac(ph2, d2), mx, &x, &p1, &p2, &c1);
            if (W2 && (q = ph2 + pw2) < inc2)
                sem_bl(32768, sem_frac(q, d2), mx, &x, &p1, &p2, &c1);
        }
        xbuf[i] = p2;
        p2 = p1;
        p1 = x + ((sem_naive(W1, ph1, pw1) * wa + sem_naive(W2, ph2, pw2) * mx) >> 7);
    }
    v->ph[0] = ph1;
    v->ph[2] = ph2;
    v->s[6] = p1;
    v->s[8] = p2;
    v->s[9] = c1;
}
static void sem_osc(const int16_t *p, voice_t *v, uint32_t n, const vmod_t *m)
{
    uint32_t pq1 = v->ph[1] >> 16, pq2 = v->ph[1] & 0xFFFFu;
    int32_t g1 = (clamp(p[P_S1], 0, SEM_PW_MAX) << 8) - (int32_t)pq1, g2 = (clamp(p[P_S3], 0, SEM_PW_MAX) << 8) - (int32_t)pq2;
    pq1 += (uint32_t)(g1 >> 2);                          /* PW glides like CUT: a knob step does not tick */
    pq2 += (uint32_t)(g2 >> 2);
    v->ph[1] = pq1 << 16 | pq2;
    uint32_t pw1 = sem_pwq(pq1), pw2 = sem_pwq(pq2);
    if (p[P_S0] || p[P_S2] || p[P_S6])
        sem_loop(p, v, n, m, pw1, pw2, p[P_S0] != 0, p[P_S2] != 0, p[P_S6] != 0);   /* PULSE, SYNC: the waves and SYNC tested as it runs */
    else
        sem_loop(p, v, n, m, pw1, pw2, 0, 0, 0);                  /* two SAWs, the common one, without the rest */
}

static void sem_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
#if KOSMO_PERF
    uint32_t _tw = fm1_ticks();
#endif
    const int16_t *p = t->p;
    int32_t sc = v->s[2], sr = v->s[3], st[4] = {v->s[0], v->s[1], v->s[4], v->s[5]}, s;
    uint32_t i;
    sc += ((p[P_E2] << 8) - sc) >> 2;
    sr += ((p[P_E3] << 8) - sr) >> 2;
    sem_osc(p, v, n, m);
#define SEM_GET s = xbuf[i];
    FILTER_BLOCK(SEM_GET);
#if KOSMO_PERF
    pfx_s[6] += fm1_ticks() - _tw; pfx_n[6]++;
#endif
    v->s[0] = st[0];
    v->s[1] = st[1];
    v->s[4] = st[2];
    v->s[5] = st[3];
    v->s[2] = sc;
    v->s[3] = sr;
}

static const engine_t ENG_SEM = {
    .name = "SEM",
    .pbase = P_S0,
    .edit = {
        {"WAVE1", F_ENUM, 0, 1, 0, SEM_WAVE_NAMES, 0},
        {"PW1", F_INT, 0, SEM_PW_MAX, 63, 0, 0},
        {"WAVE2", F_ENUM, 0, 1, 0, SEM_WAVE_NAMES, 0},
        {"PW2", F_INT, 0, SEM_PW_MAX, 63, 0, 0},
        {"TUNE", F_INT, 0, 48, 24, 0, 0},
        {"FINE", F_INT, 0, 100, 50, 0, 0},
        {"SYNC", F_ENUM, 0, 1, 0, 0, 0},
        {"MIX", F_INT, 0, 128, 64, 0, 0},
    },
    .note_on = sem_note_on,
    .render = sem_render,
    .keep = 0x3F3,               /* the filter and the comb flag as WAVE's, s[6] s[8] s[9] the oscillators' pipeline */
};
