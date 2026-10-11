/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Effects: the part's level / pan, the ROOM reverb send, then the master: DC blocker, peak limiter, soft clip.
 * Mono reverb bus, stereo dry mix. */
static const uint16_t REV_COMB[4] = {1116, 1188, 1277, 1356};
static const uint16_t REV_AP[2] = {556, 441};
static int16_t rev_comb[1116 + 1188 + 1277 + 1356] __attribute__((section(".pool")));
static int16_t rev_ap[556 + 441] __attribute__((section(".pool")));
static struct {
    uint16_t comb_i[4], ap_i[2];
    int32_t comb_lp[4];
} fx;

/* master: peak limiter in front of the soft clipper. Fast attack (~0.1 ms),
 * ~150 ms release, threshold where tanh is still nearly linear, so chords
 * get quieter instead of crushed. */
#define LIM_T 18000
static int32_t lim_env = LIM_T;
#define MASTER_FULL 4096                /* main.c: the MASTER knob's top, Q12 */
/* the MASTER pot (ADC 0..1023, smoothed in main.c) to its level, square law: 0 .. 4088 (Q12) */
static inline uint32_t master_of_pot(uint32_t k10) { return (k10 * k10) >> 8; }
static int32_t dc_l, dc_r, dce_l, dce_r;

/* DC blocker (~2 Hz), always on: a leaky integrator of the input (Q6 state) subtracted from it.
 * The >> 12 step keeps its remainder (error feedback, 0..4095) and adds it to the next one, so no
 * part of the step is lost: the state follows the input exactly, down to 0 after the sound stops.
 * (A rounded step of (x - dc) / 4096 would stop moving at |x - dc| < 2048 and leave an offset of up
 * to +-31 at the output after silence.) */
static inline int32_t dc_block(int32_t x, int32_t *dc, int32_t *err)
{
    int32_t e = (x << 6) - *dc + *err, d = e >> 12;
    *err = e - (d << 12);
    *dc += d;
    return x - ((*dc + 32) >> 6);
}

static inline void master_out(int32_t *l, int32_t *r)
{
    int32_t al, ar, a;
    *l = dc_block(*l, &dc_l, &dce_l);
    *r = dc_block(*r, &dc_r, &dce_r);
    al = *l < 0 ? -*l : *l;
    ar = *r < 0 ? -*r : *r;
    a = al > ar ? al : ar;
    if (a > lim_env)
        lim_env += (a - lim_env) >> 2;
    else if (lim_env > LIM_T)
        lim_env -= ((lim_env - LIM_T) >> 12) + 1;
    if (lim_env > LIM_T) {
        int32_t g = (int32_t)(((uint32_t)LIM_T << 15) / (uint32_t)lim_env);   /* < 32768 */
        *l = ((*l >> 4) * g) >> 11;                      /* >> 4 first: |l| may be far above Q15 */
        *r = ((*r >> 4) * g) >> 11;
    }
    *l = softclip(*l);
    *r = softclip(*r);
}

/* ROOM (Freeverb-style: four combs, two allpasses), added to out. One pass over the block per line, so each loop
 * keeps its few values in registers; a pass runs in segments up to the line's wrap, with no wrap test per sample.
 * The same sums in the same order as the sample by sample form, so the output is bit for bit the same. */
static int32_t rv_in[CTL], rv_a[CTL];

static __attribute__((noinline)) void rev_room(const int32_t *rev_in, int32_t *out, uint32_t n)
{
    uint32_t i, k, j, seg;
    int32_t size = 25000 + song.g[G_RSIZE] * 50, damp = 32767 - song.g[G_RDAMP] * 200, a;
    int16_t *c = rev_comb;
    for (i = 0; i < n; i++)
        rv_in[i] = mulq15(rev_in[i], 2580);           /* 1/8 at -4 dB: level as before the allpass fix */
    for (k = 0; k < 4u; k++) {
        uint32_t ci = fx.comb_i[k];
        int32_t lp = fx.comb_lp[k], g = 32767 - damp;
        for (i = 0; i < n; i += seg) {
            int16_t *q = c + ci;
            seg = REV_COMB[k] - ci < n - i ? REV_COMB[k] - ci : n - i;
            for (j = 0; j < seg; j++) {
                int32_t o = q[j];
                lp = o + mulq15(lp - o, g);
                q[j] = (int16_t)clamp(rv_in[i + j] + mulq15(lp, size), -32768, 32767);
                rv_a[i + j] = k ? rv_a[i + j] + o : o;
            }
            ci += seg;
            if (ci == REV_COMB[k])
                ci = 0;
        }
        fx.comb_i[k] = (uint16_t)ci;
        fx.comb_lp[k] = lp;
        c += REV_COMB[k];
    }
    c = rev_ap;
    for (k = 0; k < 2u; k++) {
        uint32_t ci = fx.ap_i[k];
        for (i = 0; i < n; i += seg) {
            int16_t *q = c + ci;
            seg = REV_AP[k] - ci < n - i ? REV_AP[k] - ci : n - i;
            for (j = 0; j < seg; j++) {
                int32_t o = q[j];
                a = rv_a[i + j];
                q[j] = (int16_t)clamp(a + (o >> 1), -32768, 32767);
                rv_a[i + j] = o - a;                   /* Freeverb: out = buf - in (o - v is a notch comb) */
            }
            ci += seg;
            if (ci == REV_AP[k])
                ci = 0;
        }
        fx.ap_i[k] = (uint16_t)ci;
        c += REV_AP[k];
    }
    for (i = 0; i < n; i++)
        out[i] += rv_a[i];
}


static void rev_clear(void)
{
    uint32_t i;
    for (i = 0; i < sizeof rev_comb / 2u; i++)
        rev_comb[i] = 0;
    for (i = 0; i < sizeof rev_ap / 2u; i++)
        rev_ap[i] = 0;
    for (i = 0; i < 4u; i++)
        fx.comb_lp[i] = 0;
}

static void events_block(uint32_t n);                    /* input.c */
static int32_t part_buf[CTL];                            /* the part's block (mix_part) */
static int32_t send_r[CTL], wet[CTL], mix_l[CTL], mix_r[CTL];

/* the part into the dry mix and the reverb send; with no voice sounding it costs a cleared buffer only */
static void mix_part(track_t *t, uint32_t n)
{
    int32_t *b = part_buf;
    uint32_t i;
    int32_t lvl, pan, gl, gr, r, xmax;
    if (!track_render(t, b, n))
        return;
    lvl = LEVEL_Q12[t->p[P_LEVEL] & 127];
    pan = t->p[P_PAN];
    gl = 4096 - (pan > 0 ? pan * 64 : 0);
    gr = 4096 + (pan < 0 ? pan * 64 : 0);
    r = t->p[P_REV] * 258;
    xmax = 0x7FFFFFFF / (r | 1);                        /* sends: loud chords at a high LEVEL */
    for (i = 0; i < n; i++) {
        int32_t x = ((b[i] >> 2) * lvl) >> 10;          /* pre-shift: 8 loud voices */
        if (r)
            send_r[i] += mulq15(clamp(x, -xmax, xmax), r);   /* (mulq15 would overflow unclamped) */
        mix_l[i] += (x * gl) >> 12;
        mix_r[i] += (x * gr) >> 12;
    }
}

static void mix_block(int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t mg = (int32_t)song.master_q12;
    for (i = 0; i < n; i++) {
        send_r[i] = mix_l[i] = mix_r[i] = 0;
        wet[i] = 0;
    }
    PFX(0, events_block(n));
    PFX(1, mix_part(&trk[0], n));
    PFX(3, rev_room(send_r, wet, n));
#if KOSMO_PERF
    uint32_t _t4 = fm1_ticks();
#endif
    for (i = 0; i < n; i++) {
        int32_t l = (((mix_l[i] + wet[i]) >> 2) * mg) >> 10;
        int32_t r = (((mix_r[i] + wet[i]) >> 2) * mg) >> 10;
        master_out(&l, &r);
        out[2u * i] = l;
        out[2u * i + 1u] = r;
    }
#if KOSMO_PERF
    pfx_s[4] += fm1_ticks() - _t4;
    pfx_n[4]++;
#endif
}
