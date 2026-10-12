/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Effects: the part's level / pan, ECHO (echo.c), the SPACE reverb send (space.c), then the master: DC blocker, peak limiter, soft clip.
 * Stereo end to end: voices (copies spread L / R), ECHO, SPACE sends, dry mix and reverb return. */
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

static void events_block(uint32_t n);                    /* input.c */
static int32_t part_buf[3 * CTL];                        /* the part's block (mix_part): centre, left copies, right copies */
static int32_t send_l[CTL], send_r[CTL], wet_l[CTL], wet_r[CTL], mix_l[CTL], mix_r[CTL];
#include "echo.c"
#include "space.c"

/* the part into the dry mix and the reverb send; with no voice sounding it costs a cleared buffer only */
static void mix_part(track_t *t, uint32_t n)
{
    int32_t *b = part_buf;
    uint32_t i;
    int32_t lvl, pan, gl, gr, r, xmax, w;
    if (!track_render(t, b, n))
        return;
    lvl = LEVEL_Q12[t->p[P_LEVEL] & 127];
    pan = t->p[P_PAN];
    gl = 4096 - (pan > 0 ? pan * 64 : 0);
    gr = 4096 + (pan < 0 ? pan * 64 : 0);
    r = t->p[P_REV] * 258;
    xmax = 0x7FFFFFFF / (r | 1);                        /* sends: loud chords at a high LEVEL */
    w = t->p[P_DETUNE] * 32;                            /* the copies' width grows with DETUNE; the L+R sum stays */
    for (i = 0; i < n; i++) {
        int32_t x = ((b[i] >> 2) * lvl) >> 10;          /* pre-shift: 8 loud voices */
        int32_t xl = x, xr = x;
        if (tr_side) {                                  /* x: all voices, d: left copies minus right copies */
            int32_t bl = b[n + i], br = b[2u * n + i], d = (((bl - br) >> 2) * lvl >> 10) * w >> 12;
            x = (((b[i] + bl + br) >> 2) * lvl) >> 10;
            xl = x + d;
            xr = x - d;
        }
        xl = (xl * gl) >> 12;
        xr = (xr * gr) >> 12;
        if (r) {
            send_l[i] += mulq15(clamp(xl, -xmax, xmax), r);   /* (mulq15 would overflow unclamped) */
            send_r[i] += mulq15(clamp(xr, -xmax, xmax), r);
        }
        mix_l[i] += xl;
        mix_r[i] += xr;
    }
}

static void mix_block(int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t mg = (int32_t)song.master_q12;
    for (i = 0; i < n; i++) {
        send_l[i] = send_r[i] = mix_l[i] = mix_r[i] = 0;
    }
    PFX(0, events_block(n));
    lfo2_begin();                                       /* LFO 2: its FX parameter for this block (lfo.c) */
    PFX(1, mix_part(&trk[0], n));
    PFX(6, echo_block(n));                              /* before SPACE: the send hears the echoes */
    PFX(3, space_block(send_l, send_r, wet_l, wet_r, n));
    lfo2_end();
#if KOSMO_PERF
    uint32_t _t4 = fm1_ticks();
#endif
    for (i = 0; i < n; i++) {
        int32_t l = (((mix_l[i] + wet_l[i]) >> 2) * mg) >> 10;
        int32_t r = (((mix_r[i] + wet_r[i]) >> 2) * mg) >> 10;
        master_out(&l, &r);
        out[2u * i] = l;
        out[2u * i + 1u] = r;
    }
#if KOSMO_PERF
    pfx_s[4] += fm1_ticks() - _t4;
    pfx_n[4]++;
#endif
}
