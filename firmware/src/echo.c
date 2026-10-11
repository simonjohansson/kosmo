/* SPDX-License-Identifier: GPL-3.0-only */
/* ECHO (issue #20): a vintage 12-bit stereo delay between the voices and the SPACE reverb. It adds to mix_l / mix_r (the
 * dry signal stays) and to the reverb send. At AMOUNT 0 it does nothing at all: the output is bit for bit as without it.
 *
 * Memory: two lines (A feeds B, B feeds back to A: the echoes ping-pong L R L R), 16K samples each at a ~22 kHz delay
 * clock (0.74 s), 12 bits packed 3 bytes per 2 samples = 48 KB. The input is L into line A and R into line B, halved to the delay clock by
 * a two-sample average; the taps are doubled back to 44.1 kHz by linear interpolation.
 * Write: TPDF dither (+-1 LSB), the code is clamped to 12 bits; CRUNCH and TOY first run the sample through a sqrt curve
 * (ec_enc LUT) and square it on the way out (a compander: fine steps when quiet, coarse when loud: the grain).
 * Read: a fractional pointer, linear interpolation. TIME moves the delay at a limited rate, so the repeats glide in
 * pitch like tape. Loop: every hop (A -> B, B -> A) is gain * one-pole low pass * a soft clip, so each repeat is
 * darker; the clip bounds REPEATS 127 (> 1.0), which self-oscillates.
 * BACKWARDS: two read heads half a window apart sweep backwards through the last TIME, crossfaded by a triangle.
 * ROBOT: a plain echo whose recirculating repeats are held (zero-order hold, ~5 kHz, no interpolation on the read): every hop is
 * stair-stepped again, so it gets more robotic as it recirculates. */
#define EC_N 16384u                     /* samples per line */
#define EC_MASK (EC_N - 1u)
static uint8_t ec_mem[2][EC_N / 2u * 3u] __attribute__((section(".pool")));
static uint16_t ec_enc[1025] __attribute__((section(".pool")));   /* compander: |x| >> 5 -> sqrt code, Q4 of a 12-bit code */
static int32_t ec_wl[CTL], ec_wr[CTL];
static struct {
    uint32_t wr, rng, ph, rph;
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
    ec.dc = (1100 + song.g[G_ETIME] * 120) << 16;
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
    int32_t xmax = 0x7FFFFFFF / (r | 1), i;
    if (!amt && !ec.g24) {
        ec_stop();                                          /* the next AMOUNT > 0 starts from silence */
        return;
    }
    uint32_t comp = ch == EC_CRUNCH || ch == EC_TOY, w4 = (uint32_t)ec.dc >> 12, inc = (1u << 21) / (w4 >> 4 | 1u);
    int32_t a = ch == EC_TOY ? 11000 : ch == EC_CRUNCH ? 20000 : 24000, tdc = (1100 + song.g[G_ETIME] * 120) << 16;
    if (!ec.on)
        ec_start();
    for (i = 0; i < (int32_t)n; i += 2) {
        int32_t ml = (mix_l[i] + mix_l[i + 1]) >> 1, mr = (mix_r[i] + mix_r[i + 1]) >> 1, ya, yb, ca, cb, step = (tdc - ec.dc) >> 8;
        if (ch == EC_BACK) {
            uint32_t ua = ec.ph, ub = (ua + (1u << 19)) & 0xFFFFF;
            uint32_t da = (((ua >> 7) * w4 >> 13) + 32u) << 12, db = (((ub >> 7) * w4 >> 13) + 32u) << 12;
            int32_t wa = (int32_t)(ua < (1u << 19) ? ua >> 4 : (0x100000u - ua) >> 4);
            int32_t wb = 32768 - wa;
            ya = mulq15(ec_rd(ec_mem[0], ec.wr, da, 0), wa) + mulq15(ec_rd(ec_mem[0], ec.wr, db, 0), wb);
            yb = mulq15(ec_rd(ec_mem[1], ec.wr, da, 0), wa) + mulq15(ec_rd(ec_mem[1], ec.wr, db, 0), wb);
            ec.ph = (ec.ph + inc) & 0xFFFFF;
        } else {
            ya = ch == EC_ROBOT ? ec_dec(ec_get(ec_mem[0], ec.wr - (uint32_t)(ec.dc >> 16) & EC_MASK), 0) : ec_rd(ec_mem[0], ec.wr, (uint32_t)ec.dc, comp);
            yb = ch == EC_ROBOT ? ec_dec(ec_get(ec_mem[1], ec.wr - (uint32_t)(ec.dc >> 16) & EC_MASK), 0) : ec_rd(ec_mem[1], ec.wr, (uint32_t)ec.dc, comp);
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
        ca = ec_quant(clamp(ml + ca, -32767, 32767), comp, (uint32_t)((ml | mr) != 0));   /* L feeds line A, R line B */
        cb = ec_quant(clamp(mr + cb, -32767, 32767), comp, (uint32_t)((ml | mr) != 0));
        ec_put(ec_mem[0], ec.wr, ca);
        ec_put(ec_mem[1], ec.wr, cb);
        ec.wr = (ec.wr + 1u) & EC_MASK;
        step = step > 8192 ? 8192 : step < -8192 ? -8192 : step ? step : tdc > ec.dc ? 1 : tdc < ec.dc ? -1 : 0;
        ec.dc += step;
        ec_wl[i] = (ec.ya + ya) >> 1;
        ec_wl[i + 1] = ya;
        ec_wr[i] = (ec.yb + yb) >> 1;
        ec_wr[i + 1] = yb;
        ec.ya = ya;
        ec.yb = yb;
    }
    for (i = 0; i < (int32_t)n; i++) {
        int32_t gw, wl, wr;
        ec.g24 += (tgt - ec.g24) >> 10;
        gw = ec.g24 >> 9;
        wl = mulq15(ec_wl[i], gw);
        wr = mulq15(ec_wr[i], gw);
        mix_l[i] += wl;
        mix_r[i] += wr;
        if (r) {
            send_l[i] += mulq15(clamp(wl, -xmax, xmax), r);
            send_r[i] += mulq15(clamp(wr, -xmax, xmax), r);
        }
    }
}
