/* SPDX-License-Identifier: GPL-3.0-only */
/* ECHO (issue #20, #28): a vintage 12-bit stereo delay between the voices and the SPACE reverb. It adds to mix_l / mix_r (the
 * dry signal stays) and to the reverb send. At AMOUNT 0 it does nothing at all: the output is bit for bit as without it.
 *
 * Memory: two lines (A feeds B, B feeds back to A: the echoes ping-pong L R L R), 32K samples each at a ~22 kHz delay
 * clock (1.48 s), 12 bits packed 3 bytes per 2 samples = 96 KB. The input (ec_stereo 0: the mono sum into line A only, the ping-pong; 1: L into A and R into B) is
 * halved to the delay clock and the taps are doubled back to 44.1 kHz: CLEAN with 15-tap half-band filters (a clean 9 kHz does not image
 * to 13 kHz, a 14 kHz does not alias to 8 kHz), the others by a two-sample average / linear interpolation (their dirt).
 * Write: TPDF dither (+-1 LSB), the code is clamped to 12 bits; CRUNCH and TOY first run the sample through a sqrt curve
 * (ec_enc LUT) and square it on the way out (a compander: fine steps when quiet, coarse when loud: the grain).
 * Read: a fractional pointer, cubic (Hermite) interpolation. TIME moves the delay at a limited rate, so the repeats glide in
 * pitch like tape. CLEAN also wanders a little: wow (0.5 Hz) and flutter (6 Hz) on the read position. Loop: every hop (A -> B, B -> A) is gain * one-pole low pass * a soft clip, so each repeat is
 * darker; the clip bounds REPEATS 127 (> 1.0), which self-oscillates.
 * BACKWARDS: two read heads half a window apart sweep backwards through the last TIME, crossfaded by a triangle (linear reads).
 * ROBOT: a plain echo whose recirculating repeats are held (zero-order hold, ~5 kHz, no interpolation on the read): every hop is
 * stair-stepped again, so it gets more robotic as it recirculates. */
#define EC_N 32768u                     /* samples per line */
#define EC_MASK (EC_N - 1u)
#define EC_HIST 14                      /* CLEAN's input half-band: 15 taps, the 13 inputs before a block */
#define EC_RHIST 8                      /* CLEAN's output half-band: the 7 delay-clock samples before a block */
static const int16_t EC_HB[4] = {10381, -2905, 1197, -481};  /* half-band taps h1 h3 h5 h7, Q15 (h0 = 1/2; equiripple, +-0.9 % to 8 kHz, -21 dB at 13 kHz re 9 kHz) */
/* TIME 0..127 -> delay clock samples: 120 a step up to 64 (0.4 s), then 258 up to the 32594 of the line (1.48 s) */
#define EC_TIME(t) (1100 + (t) * 120 + ((t) > 64 ? ((t) - 64) * 258 : 0))
static uint8_t ec_mem[2][EC_N / 2u * 3u] __attribute__((section(".pool")));
static uint16_t ec_enc[1025] __attribute__((section(".pool")));   /* compander: |x| >> 5 -> sqrt code, Q4 of a 12-bit code */
static int32_t ec_wl[CTL], ec_wr[CTL];
static int32_t ec_xb[2][EC_HIST + CTL], ec_ra[2][EC_RHIST + CTL / 2];   /* CLEAN's filters: the inputs, the taps */
typedef struct { int32_t s[4]; uint32_t i; } ec_win_t;
static uint8_t ec_wow = 1;                      /* CLEAN wow and flutter (the host tests switch it off) */
static uint8_t ec_stereo;                       /* 0 PING-PONG (the sum into line A), 1 STEREO (L into A, R into B); ui.c syncs it from the settings */
static struct {
    uint32_t wr, rng, ph, rph, wph1, wph2, wc;
    ec_win_t win[2];                    /* the read windows of lines A and B; wc: the character they were decoded for + 1 */
    int32_t dc;                         /* the delay, Q16 samples of the delay clock */
    int32_t lpa, lpb, ya, yb;           /* loop one-poles; the last taps */
    int32_t g24;                        /* AMOUNT smoothed, Q24 */
    int32_t hold_l, hold_r;
    uint8_t on, dirty;                  /* on: the lines hold audio; dirty: they are being wiped (ec.clr: next word) */
    uint32_t clr;
} ec = {.rng = 1};

static inline int32_t ec_get(const uint8_t *m, uint32_t n)
{
    const uint8_t *p = m + (n >> 1) * 3u;
    int32_t c = n & 1u ? (p[1] >> 4 | p[2] << 4) : (p[0] | (p[1] & 15) << 8);
    return (c ^ 0x800) - 0x800;
}

static inline void ec_put(uint8_t *m, uint32_t n, int32_t c)
{
    uint8_t *p = m + (n >> 1) * 3u;
    c &= 0xFFF;
    if (n & 1u) {
        p[1] = (uint8_t)((p[1] & 15) | (c & 15) << 4);
        p[2] = (uint8_t)(c >> 4);
    } else {
        p[0] = (uint8_t)c;
        p[1] = (uint8_t)((p[1] & 0xF0) | c >> 8);
    }
}

static inline int32_t ec_dec(int32_t c, uint32_t comp)      /* 12-bit code -> Q15 */
{
    return comp ? c * (c < 0 ? -c : c) >> 7 : c << 4;
}

static inline int32_t ec_quant(int32_t x, uint32_t comp, uint32_t dith)   /* Q15 -> 12-bit code, dithered while there is input */
{
    int32_t a = x < 0 ? -x : x, v, d;
    uint32_t r = ec.rng;
    d = 0;
    if (dith) {                                             /* (no dither once the input stops: the tail dies away, no hiss) */
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        ec.rng = r;
        d = (int32_t)(r & 15u) - (int32_t)(r >> 8 & 15u);   /* TPDF, +-15 of 16 per code step */
    }
    if (a > 32767)
        a = 32767;
    if (comp) {
        int32_t i = a >> 5, e0 = ec_enc[i], e1 = ec_enc[i + 1];
        a = e0 + (((e1 - e0) * (a & 31)) >> 5);
    }
    v = (x < 0 ? -a : a) + d;
    return clamp((v + 8) >> 4, -2047, 2047);
}

/* the line read d (Q16) samples behind the write position, linearly interpolated */
static inline int32_t ec_rd(const uint8_t *m, uint32_t wr, uint32_t d, uint32_t comp)
{
    uint32_t p = (wr << 16) - d, i = p >> 16 & EC_MASK;
    int32_t a = ec_dec(ec_get(m, i), comp), b = ec_dec(ec_get(m, (i + 1u) & EC_MASK), comp);
    return a + (((b - a) * (int32_t)(p >> 1 & 0x7FFF)) >> 15);
}

/* the same, cubic (Catmull-Rom Hermite, 4 points); f in Q11 keeps the Horner steps inside 32 bits. The 4 decoded points of a line
 * slide along with the read (one new point a step; a jump or a new character reloads them) */
static inline int32_t ec_rd3(const uint8_t *m, ec_win_t *w, uint32_t wr, uint32_t d, uint32_t comp)
{
    uint32_t p = (wr << 16) - d, i = p >> 16 & EC_MASK, k = (i - w->i) & EC_MASK;
    int32_t f = (int32_t)(p >> 5 & 0x7FFu), t;
    if (k == 1u && w->i <= EC_MASK) {
        w->s[0] = w->s[1];
        w->s[1] = w->s[2];
        w->s[2] = w->s[3];
        w->s[3] = ec_dec(ec_get(m, (i + 2u) & EC_MASK), comp);
    } else if (k || w->i > EC_MASK) {
        w->s[0] = ec_dec(ec_get(m, (i - 1u) & EC_MASK), comp);
        w->s[1] = ec_dec(ec_get(m, i), comp);
        w->s[2] = ec_dec(ec_get(m, (i + 1u) & EC_MASK), comp);
        w->s[3] = ec_dec(ec_get(m, (i + 2u) & EC_MASK), comp);
    }
    w->i = i;
    t = ((w->s[3] - w->s[0]) + 3 * (w->s[1] - w->s[2])) * f >> 11;
    t = (t + 2 * w->s[0] - 5 * w->s[1] + 4 * w->s[2] - w->s[3]) * f >> 11;
    return (((t + w->s[2] - w->s[0]) * f >> 11) + w->s[1] * 2) >> 1;
}

/* the 15-tap half-band on the input: x[-6] is the centre of the newest 15 */
static inline int32_t ec_dec2(const int32_t *x)
{
    return (x[-6] >> 1) + ((EC_HB[0] * (x[-7] + x[-5]) + EC_HB[1] * (x[-9] + x[-3]) + EC_HB[2] * (x[-11] + x[-1]) + EC_HB[3] * (x[-13] + x[1])) >> 15);
}

/* power on (and the host tests): the compander table, no echo state */
static void ec_init(void)
{
    uint32_t j, r = 0;
    for (j = 0; j <= 1024u; j++) {                          /* isqrt(|x| << 15), |x| = j << 5 */
        uint32_t t = j << 20;
        while ((r + 1u) * (r + 1u) <= t)
            r++;
        ec_enc[j] = (uint16_t)(r > 32752u ? 32752u : r);
    }
    ec.on = ec.dirty = 0;
    ec.g24 = 0;
}

/* off: the lines are wiped a block at a time (a spike of 0.9 ms in one block otherwise); a restart finishes it */
static void ec_wipe(uint32_t words)
{
    uint32_t *m = (uint32_t *)ec_mem, e = ec.clr + words;
    if (!ec.dirty)
        return;
    if (e >= sizeof ec_mem / 4u) {
        e = sizeof ec_mem / 4u;
        ec.dirty = 0;
    }
    for (; ec.clr < e; ec.clr++)
        m[ec.clr] = 0;
}

static void ec_stop(void)
{
    if (ec.on) {
        ec.on = 0;
        ec.dirty = 1;
        ec.clr = 0;
    }
    ec_wipe(256);
}

static void ec_start(void)
{
    ec_wipe(~0u >> 1);
    ec.lpa = ec.lpb = ec.ya = ec.yb = 0;
    ec.wr = ec.ph = ec.rph = 0;
    ec.hold_l = ec.hold_r = 0;
    ec.dc = EC_TIME(song.g[G_ETIME]) << 16;
    ec.wph1 = ec.wph2 = ec.wc = 0;
    ec.on = 1;
}

/* one hop of the loop: the tap through a low pass, REPEATS and the character's soft clip (small-signal gain 1) */
static inline int32_t ec_hop(int32_t y, int32_t *lp, int32_t a, int32_t g, uint32_t ch)
{
    int32_t x;
    *lp += mulq15(y - *lp, a);
    x = mulq15(*lp, g);
    if (ch == EC_CRUNCH)
        return mulq15(softclip(x * 3), 10923);
    if (ch == EC_TOY)
        return x > 0 ? softclip(x * 2) >> 1 : softclip(x);  /* asymmetric */
    return softclip(x);
}

static __attribute__((noinline)) void echo_block(uint32_t n)
{
    int32_t amt = song.g[G_EAMT], ch = song.g[G_ECHAR], tgt = amt * 258 << 9, g = song.g[G_EFB] * 260, r = trk[0].p[P_REV] * 258;
    int32_t xmax = 0x7FFFFFFF / (r | 1), i, wm = 0, wi = 0;
    if (!amt && !ec.g24) {
        ec_stop();                                          /* the next AMOUNT > 0 starts from silence */
        return;
    }
    uint32_t comp = ch == EC_CRUNCH || ch == EC_TOY, w4 = (uint32_t)ec.dc >> 12, inc = (1u << 21) / (w4 >> 4 | 1u);
    int32_t a = ch == EC_TOY ? 11000 : ch == EC_CRUNCH ? 20000 : 24000, tdc = EC_TIME(song.g[G_ETIME]) << 16;
    if (!ec.on)
        ec_start();
    if (ec.wc != (uint32_t)ch + 1u) {            /* a new character: the read windows and CLEAN's filter taps start again */
        ec.wc = (uint32_t)ch + 1u;
        ec.win[0].i = ec.win[1].i = ~0u;
        memset(ec_xb, 0, sizeof ec_xb);
        memset(ec_ra, 0, sizeof ec_ra);
    }
    if (ch == EC_CLEAN)
        for (i = 0; i < (int32_t)n; i++)                    /* CLEAN's input history (PING-PONG: the sum halved, one line) */
            ec_xb[0][EC_HIST + i] = ec_stereo ? mix_l[i] : (mix_l[i] + mix_r[i]) >> 1;
    if (ch == EC_CLEAN && ec_stereo)
        for (i = 0; i < (int32_t)n; i++)
            ec_xb[1][EC_HIST + i] = mix_r[i];
    if (ch == EC_CLEAN && ec_wow) {                         /* wow 0.5 Hz (+-10.5 samples, +-0.15 % pitch) and flutter 6 Hz (+-0.6, +-0.1 %) on the read, Q16: the ramp over this block */
        wm = sine_i(ec.wph1) * 21 + (sine_i(ec.wph2) * 37 >> 5);
        ec.wph1 += 97392u * (n >> 1);
        ec.wph2 += 1168709u * (n >> 1);
        wi = (sine_i(ec.wph1) * 21 + (sine_i(ec.wph2) * 37 >> 5) - wm) / (int32_t)(n >> 1);
    }
    for (i = 0; i < (int32_t)n; i += 2) {
        int32_t ml, mr, ya, yb, ca, cb, step = (tdc - ec.dc) >> 8;
        if (ch == EC_CLEAN) {                               /* the half-band decimator */
            ml = ec_dec2(ec_xb[0] + EC_HIST + i);
            mr = ec_stereo ? ec_dec2(ec_xb[1] + EC_HIST + i) : 0;
        } else if (ec_stereo) {
            ml = (mix_l[i] + mix_l[i + 1]) >> 1;
            mr = (mix_r[i] + mix_r[i + 1]) >> 1;
        } else {
            ml = (mix_l[i] + mix_l[i + 1] + mix_r[i] + mix_r[i + 1]) >> 2;
            mr = 0;
        }
        if (ch == EC_BACK) {
            uint32_t ua = ec.ph, ub = (ua + (1u << 19)) & 0xFFFFF;
            uint32_t da = (((ua >> 7) * w4 >> 13) + 32u) << 12, db = (((ub >> 7) * w4 >> 13) + 32u) << 12;
            int32_t wa = (int32_t)(ua < (1u << 19) ? ua >> 4 : (0x100000u - ua) >> 4);
            int32_t wb = 32768 - wa;
            ya = mulq15(ec_rd(ec_mem[0], ec.wr, da, 0), wa) + mulq15(ec_rd(ec_mem[0], ec.wr, db, 0), wb);
            yb = mulq15(ec_rd(ec_mem[1], ec.wr, da, 0), wa) + mulq15(ec_rd(ec_mem[1], ec.wr, db, 0), wb);
            ec.ph = (ec.ph + inc) & 0xFFFFF;
        } else {
            uint32_t d = (uint32_t)(ec.dc + wm);
            wm += wi;
            if (ch == EC_ROBOT) {
                ya = ec_dec(ec_get(ec_mem[0], ec.wr - (uint32_t)(ec.dc >> 16) & EC_MASK), 0);
                yb = ec_dec(ec_get(ec_mem[1], ec.wr - (uint32_t)(ec.dc >> 16) & EC_MASK), 0);
            } else {
                ya = ec_rd3(ec_mem[0], &ec.win[0], ec.wr, d, comp);
                yb = ec_rd3(ec_mem[1], &ec.win[1], ec.wr, d, comp);
            }
        }
        ca = ec_hop(yb, &ec.lpb, a, g, (uint32_t)ch);
        cb = ec_hop(ya, &ec.lpa, a, g, (uint32_t)ch);
        if (ch == EC_ROBOT) {                               /* the repeats (not the input): zero-order hold at ~5 kHz */
            ec.rph += 14860u;                               /* 14860 / 65536 of the delay clock */
            if (ec.rph >> 16) {
                ec.rph &= 0xFFFF;
                ec.hold_l = ca;
                ec.hold_r = cb;
            }
            ca = ec.hold_l;
            cb = ec.hold_r;
        }
        ca = ec_quant(clamp(ml + ca, -32767, 32767), comp, (uint32_t)((ml | mr) != 0));   /* PING-PONG: the sum into A only; STEREO: R into B */
        cb = ec_quant(clamp(mr + cb, -32767, 32767), comp, (uint32_t)((ml | mr) != 0));
        ec_put(ec_mem[0], ec.wr, ca);
        ec_put(ec_mem[1], ec.wr, cb);
        ec.wr = (ec.wr + 1u) & EC_MASK;
        step = step > 8192 ? 8192 : step < -8192 ? -8192 : step ? step : tdc > ec.dc ? 1 : tdc < ec.dc ? -1 : 0;
        ec.dc += step;
        if (ch == EC_CLEAN) {
            ec_ra[0][EC_RHIST + (i >> 1)] = ya;
            ec_ra[1][EC_RHIST + (i >> 1)] = yb;
        }
        ec_wl[i] = (ec.ya + ya) >> 1;
        ec_wl[i + 1] = ya;
        ec_wr[i] = (ec.yb + yb) >> 1;
        ec_wr[i + 1] = yb;
        ec.ya = ya;
        ec.yb = yb;
    }
    if (ch == EC_CLEAN)                                     /* the half-band interpolator: the tap itself, then the point between */
        for (i = 0; i < (int32_t)n; i += 2) {
            const int32_t *a = ec_ra[0] + EC_RHIST + (i >> 1), *b = ec_ra[1] + EC_RHIST + (i >> 1);
            ec_wl[i] = a[-4];
            ec_wr[i] = b[-4];
            ec_wl[i + 1] = (EC_HB[0] * (a[-4] + a[-3]) + EC_HB[1] * (a[-5] + a[-2]) + EC_HB[2] * (a[-6] + a[-1]) + EC_HB[3] * (a[-7] + a[0])) >> 14;
            ec_wr[i + 1] = (EC_HB[0] * (b[-4] + b[-3]) + EC_HB[1] * (b[-5] + b[-2]) + EC_HB[2] * (b[-6] + b[-1]) + EC_HB[3] * (b[-7] + b[0])) >> 14;
        }
    if (ch == EC_CLEAN) {
        memcpy(ec_ra[0], ec_ra[0] + (n >> 1), EC_RHIST * 4u);
        memcpy(ec_ra[1], ec_ra[1] + (n >> 1), EC_RHIST * 4u);
        memcpy(ec_xb[0], ec_xb[0] + n, EC_HIST * 4u);
        memcpy(ec_xb[1], ec_xb[1] + n, EC_HIST * 4u);
    }
    for (i = 0; i < (int32_t)n; i++) {
        int32_t gw, wl, wr;
        ec.g24 += (tgt - ec.g24) >> 10;
        gw = ec.g24 >> 9;
        wl = mulq15(ec_wl[i], gw);
        wr = mulq15(ec_wr[i], gw);
        mix_l[i] += wl;
        mix_r[i] += wr;
        if (r) {                                            /* PING-PONG: the pair summed, as before #26 */
            int32_t sl = ec_stereo ? wl : (wl + wr) >> 1, sr = ec_stereo ? wr : sl;
            send_l[i] += mulq15(clamp(sl, -xmax, xmax), r);
            send_r[i] += mulq15(clamp(sr, -xmax, xmax), r);
        }
    }
}
