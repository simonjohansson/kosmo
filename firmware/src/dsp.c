/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Shared DSP building blocks for the Felucca engines (all fixed point).
 * Voice output convention: add sample * amp to out[], where a full-scale
 * oscillator at amp = 1.0 (Q15 32767) contributes VOICE_FS. */
#define VOICE_FS 24000           /* per-voice level: one voice peaks near -6 dBFS before the master */

static inline int32_t mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }
/* a * k / 65536 without 64-bit: a up to 2^31, k Q16 */
static inline int32_t mulq16(int32_t a, uint32_t k)
{
    return (int32_t)(((a >> 16) * (int32_t)k) + (int32_t)(((uint32_t)(a & 0xFFFF) * k) >> 16));
}
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* sine, linearly interpolated between the 1024 table points (plain lookup: THD -55 dB) */
static inline int32_t sine_i(uint32_t ph)
{
    uint32_t i = ph >> 22;
    int32_t a = SINE[i], b = SINE[(i + 1u) & 1023u];
    return a + (((b - a) * (int32_t)((ph >> 7) & 0x7FFFu)) >> 15);
}
static inline int32_t osc_sine(uint32_t ph) { return sine_i(ph); }

/* polyBLEP residual (Q15) around a wrap of a phase accumulator */
static inline int32_t blep(uint32_t ph, uint32_t inc)
{
    uint32_t d = inc >> 15;
    int32_t x;
    if (!d)
        return 0;
    if (ph < inc) {
        x = (int32_t)(ph / d);                       /* 0..32767 */
        return x + x - ((x * x) >> 15) - 32768;
    }
    if (ph > 0xFFFFFFFFu - inc) {
        x = -(int32_t)((0xFFFFFFFFu - ph) / d);       /* -32767..0 */
        return ((x * x) >> 15) + x + x + 32768;
    }
    return 0;
}

static inline int32_t osc_saw(uint32_t ph, uint32_t inc)
{
    return ((int32_t)(ph >> 16) - 32768) - blep(ph, inc);
}

static inline int32_t osc_pulse(uint32_t ph, uint32_t inc, uint32_t pw)
{
    return (osc_saw(ph, inc) - osc_saw(ph + pw, inc)) >> 1;
}

static inline int32_t osc_tri(uint32_t ph)
{
    int32_t s = (int32_t)(ph >> 15);                 /* 0..131071 */
    return s < 65536 ? s - 32768 : 98303 - s;
}

/* tanh(x / 32768) * 32767, linearly interpolated (a plain lookup gives
 * 256-unit steps, audible on quiet signals) */
static inline int32_t softclip(int32_t x)
{
    int32_t a = x < 0 ? -x : x, i = a >> 8, y;
    if (i >= 256)
        y = TANH_Q15[256];                           /* continuous with the table */
    else
        y = TANH_Q15[i] + (((TANH_Q15[i + 1] - TANH_Q15[i]) * (a & 255)) >> 8);
    return x < 0 ? -y : y;
}

/* xorshift32 */
static inline uint32_t noise32(int32_t *st)
{
    uint32_t s = (uint32_t)*st;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *st = (int32_t)s;
    return s;
}

/* g = tan(pi fc / FS), Q12, for a cutoff 0..127 << 8 (between table points: sweeps without 128 steps) */
static inline int32_t tan_g(int32_t cut)
{
    int32_t i, g;
    cut = clamp(cut, 0, 127 << 8);
    i = cut >> 8;
    g = TAN_G[i];
    if (i < 127)
        g += ((TAN_G[i + 1] - g) * (cut & 255)) >> 8;
    return g;
}

/* Moog ladder: four zero-delay (TPT) one-pole stages, resonance k (0 .. 3.9) fed back from the 4th stage through a
 * soft clip. With u the clipped input of stage 1 and y1..y4 the stage outputs, the types are Oberheim-style
 * pole mixes: LP y4; BP 4 (y2 - 2 y3 + y4); HP u - 4 y1 + 6 y2 - 4 y3 + y4; NOTCH u - BP.
 * Its response is the analog ladder's at the pre-warped frequency: tools/gen_tables.py LAD_DB draws it.
 * shortcut: Q15 stages, so below ~100 Hz a quiet signal (< -40 dBFS) falls in the stage's dead band; use 64-bit
 * or error feedback if low bass tails need to be clean. Signals stay within +-31600 (the clip), products in 32 bits. */
typedef struct { int32_t G, c0, c1, c2, c3, xr, kr; } lad_t;      /* xr, kr: input and feedback gain times 1 / (1 + k G^4), Q12 */
#define LAD_K 126                                      /* k (Q12) per RES step */
static inline void lad_coef(lad_t *c, int32_t cut, int32_t res, uint32_t ft)   /* cut, res: 0..127 << 8 */
{
    int32_t g = tan_g(cut), G = (g << 15) / (4096 + g), b = 32768 - G, G2 = mulq15(G, G), k = (res * LAD_K) >> 8, r;
    c->G = G;
    c->c0 = b;
    c->c1 = mulq15(G, b);
    c->c2 = mulq15(G2, b);
    c->c3 = mulq15(G2, c->c1);
    r = (1 << 24) / (4096 + ((k * mulq15(G2, G2)) >> 15));       /* 1 / (1 + k G^4): the loop through the stages */
    c->xr = ((ft == 0 ? 4096 + (k >> 2) : 4096) * r) >> 12;      /* the LP loses 1 / (1 + k) at DC: give a part back */
    c->kr = (k * r) >> 12;
}

/* ft is a constant at every call (the mix folds away) */
__attribute__((always_inline)) static inline int32_t lad_tap(const lad_t *c, int32_t *s, int32_t x, uint32_t ft)
{
    int32_t S = (c->c3 * s[0] + c->c2 * s[1] + c->c1 * s[2] + c->c0 * s[3]) >> 15, u, y1, y2, y3, y4, v;
    u = softclip((x * c->xr - S * c->kr) >> 12);
    v = (c->G * (u - s[0])) >> 15;
    y1 = s[0] + v;
    s[0] = y1 + v;
    v = (c->G * (y1 - s[1])) >> 15;
    y2 = s[1] + v;
    s[1] = y2 + v;
    v = (c->G * (y2 - s[2])) >> 15;
    y3 = s[2] + v;
    s[2] = y3 + v;
    v = (c->G * (y3 - s[3])) >> 15;
    y4 = s[3] + v;
    s[3] = y4 + v;
    return ft == 0 ? y4 : ft == 1 ? 4 * (y2 - 2 * y3 + y4) : ft == 2 ? u - 4 * (y1 + y3) + 6 * y2 + y4 :
           u - 4 * (y2 + y4) + 8 * y3;}

/* Feedback comb y[n] = (1 - g) x[n] + g y[n - D], 0 dB at its teeth; D (Q6 samples, 8 .. 400) from COMB_D, g Q15
 * (< 0.96). The ring buffer is COMB_N long, wp its write index; the delay is read linearly interpolated. */
#define COMB_N 512
static inline int32_t comb_tap(int16_t *buf, uint32_t *wp, int32_t d, int32_t g, int32_t x)
{
    uint32_t i = *wp - ((uint32_t)d >> 6), f = (uint32_t)d & 63u;
    int32_t a = buf[i & (COMB_N - 1)], b = buf[(i - 1u) & (COMB_N - 1)];
    int32_t y = ((32768 - g) * x + g * (a + (((b - a) * (int32_t)f) >> 6)) + 16384) >> 15;
    buf[*wp & (COMB_N - 1)] = (int16_t)y;
    (*wp)++;
    return y;
}

/* amplitude ramp over the block. Blocks are always CTL long, so x / CTL is a
 * shift (rounded towards zero, as a division); the compiler at -Os keeps
 * even constant divisions as a hardware divide, once per sample and voice */
#define CTL_LOG2 5
#if (1 << CTL_LOG2) != CTL
#error "CTL_LOG2 does not match CTL"
#endif
static inline int32_t amp_at(const vmod_t *m, uint32_t i)
{
    int32_t x = (m->amp1 - m->amp0) * (int32_t)i;
    return m->amp0 + ((x + ((x >> 31) & (CTL - 1))) >> CTL_LOG2);
}

/* a voice's sample s (Q15) at the amplitude ramp's sample i, scaled to the voice level (VOICE_FS);
 * engines whose signal peaks near half scale add it << 1 */
static inline int32_t voice_amp(int32_t s, const vmod_t *m, uint32_t i)
{
    return mulq15(mulq15(s, amp_at(m, i)), VOICE_FS);
}

/* linear up to k, then a soft knee: only the peaks above k saturate (|y| - k must stay < 2^30) */
static inline int32_t soft_knee(int32_t y, int32_t k)
{
    int32_t a = y < 0 ? -y : y;
    if (a <= k)
        return y;
    a = k + (softclip((a - k) * 2) >> 1);
    return y < 0 ? -a : a;
}

/* phase increment of pitch16 (1/16 semitone) plus ct cents: whole 1/16 semitones from the table,
 * the rest (and fine, in 1/4096) as a factor (1 ct = 2.367 / 4096) */
static inline uint32_t cents_inc(int32_t pitch16, int32_t ct, int32_t fine)
{
    int32_t d16 = ct * 16 / 100, rem = ct * 16 - d16 * 100;          /* rem: 1/1600 semitone */
    uint32_t inc = pitch_inc(clamp(pitch16 + d16, 0, 2047));
    return inc + (uint32_t)((int32_t)(inc >> 12) * (rem * 2367 / 16000 + fine));
}
