/* SPDX-License-Identifier: GPL-3.0-only */
/* SPACE (issue #21, 8 lines and a per-program tank rate: issue #27): an 80s digital studio reverb, written from the published
 * topologies (Schroeder allpass diffusers, Jot's feedback delay network with a Hadamard mix); no code borrowed. Stereo in and out:
 * the send is a pair (send_l / send_r), L goes into the even lines (the left output taps) and R into the odd (issue #26), AMOUNT is the send, the dry mix always passes.
 *
 * Vintage on purpose: the input is averaged over two samples (a low pass), soft clipped (tanh) and taken at the program's
 * tank clock (22 kHz dark / vintage, up to 32 kHz for the clean programs), diffused by three allpasses and fed to eight
 * lines whose outputs go through one-pole dampers, an 8x8 fast Hadamard mix and the feedback gain. The lines hold 16 bits;
 * every sum is 32 bits and clamped on the way back. The line reads are linearly interpolated and wobble slowly (a triangle
 * LFO per line): the chorus that keeps the tail from ringing metallic. SIZE scales the line lengths (a glide, like tape)
 * and so the decay; TONE the dampers. Delay times are written in samples at 22 kHz and scale with the tank clock.
 * Six programs, one engine: lengths, clock, feedback, damping, diffusion and modulation; GATED also closes a gate on the
 * tank and its feedback HOLD after the input falls silent. L and R are different quartets of lines; the output is a linear
 * interpolation back to 44.1 kHz. 8 x 8 KB + 3 x 1 KB = 67 KB of the pool. */
#define SP_N 4096u
#define SP_AN 512u
#define SP_L 8
enum { SP_BEDROOM, SP_CASTLE, SP_PLATE, SP_CLOUD, SP_GATED, SP_WOBBLE, SP_NPROG };
typedef struct {
    uint8_t lo, hi;         /* line length at SIZE 0 / 127, of the longest, /256 */
    uint16_t g;             /* feedback gain, Q15 */
    uint16_t damp;          /* damper coefficient at TONE 64, Q15 (higher: brighter) */
    uint16_t ap;            /* diffuser coefficient, Q15 */
    uint8_t depth, rate;    /* LFO depth in tank samples at 22 kHz, rate in units of ~0.1 Hz */
    uint8_t hold;           /* GATED: the gate stays open this long after the input, in 46 ms (0: no gate) */
    uint16_t clk;           /* tank clock / 44.1 kHz, Q16 */
    uint16_t out;           /* output tap gain, Q14: the wet level of the 4-line version */
} sp_prog_t;
static const sp_prog_t SP_PROG[SP_NPROG] = {
    {40, 130, 25000, 27500, 16000, 1, 3, 0, 47563, 21000},     /* BEDROOM: small, short, bright (32 kHz) */
    {110, 250, 30500, 11000, 19000, 2, 3, 0, 32768, 24300},    /* CASTLE: big, long, dark (22 kHz, dark: no use for more) */
    {70, 180, 29500, 27000, 24000, 2, 4, 0, 47563, 17750},     /* METAL PLATE: dense, bright (32 kHz) */
    {140, 250, 30500, 17000, 20000, 8, 4, 0, 32768, 22600},    /* CLOUD: long, heavy modulation (22 kHz) */
    {70, 150, 29000, 26000, 24000, 1, 3, 6, 47563, 18760},     /* GATED: dense, cut off by the gate (32 kHz) */
    {90, 200, 29500, 21000, 18000, 14, 15, 0, 32768, 22050},   /* WOBBLE: the tail shimmers (22 kHz) */
};
static const uint16_t SP_LEN[SP_L] = {739, 743, 881, 941, 1129, 1319, 1657, 1997};   /* the longest line, samples at 22 kHz (mutually odd-ish) */
static const uint16_t SP_APL[3] = {113, 181, 241};
static int16_t sp_line[SP_L][SP_N] __attribute__((section(".pool")));
static int16_t sp_ap[2][3][SP_AN] __attribute__((section(".pool")));
static struct {
    uint32_t wr, aw, acc, ph[SP_L];
    int32_t dl[SP_L], cur[SP_L], lp[SP_L], el[SP_L], ef[SP_L], pl, pr, cl, cr, xp, xq, gate, hold;   /* el, ef: the remainders of the damper and the feedback product; pl.. the last two tank outputs */
} sp;

static void space_clear(void)
{
    uint32_t i, k;
    for (k = 0; k < SP_L; k++) {
        for (i = 0; i < SP_N; i++)
            sp_line[k][i] = 0;
        sp.lp[k] = sp.dl[k] = sp.cur[k] = sp.el[k] = sp.ef[k] = 0;
        sp.ph[k] = k * 0x20000000u;
    }
    for (k = 0; k < 6u; k++)
        for (i = 0; i < SP_AN; i++)
            sp_ap[k / 3u][k % 3u][i] = 0;
    sp.wr = sp.aw = sp.acc = 0;
    sp.pl = sp.pr = sp.cl = sp.cr = sp.xp = sp.xq = sp.gate = sp.hold = 0;
}

/* a Q15 product rounded toward zero: floor leaves a bias that keeps a limit cycle alive (and DC) in the tank, long after the sound */
static inline int32_t sp_mul(int32_t a, int32_t b)
{
    int32_t p = a * b;
    return (p + ((p >> 31) & 32767)) >> 15;
}

/* a Schroeder allpass on a 512-sample ring: v = x + g d, y = d - g v */
static inline int32_t sp_allpass(int16_t *b, uint32_t w, uint32_t len, int32_t x, int32_t g)
{
    int32_t d = b[(w - len) & (SP_AN - 1u)], v = clamp(x + sp_mul(d, g), -32767, 32767);
    b[w & (SP_AN - 1u)] = (int16_t)v;
    return d - sp_mul(v, g);
}

static __attribute__((noinline)) void space_block(const int32_t *inl, const int32_t *inr, int32_t *wl, int32_t *wr, uint32_t n)
{
    const sp_prog_t *p = &SP_PROG[clamp(song.g[G_RPROG], 0, SP_NPROG - 1)];
    int32_t sc = p->lo + (p->hi - p->lo) * song.g[G_RSIZE] / 127;
    int32_t da = clamp(p->damp + (song.g[G_RTONE] - 64) * 250, 1500, 32000), ga = p->ap, i, k;
    int32_t st[SP_L], clk = p->clk, steps = clamp((int32_t)n * clk >> 16, 1, 1 << 12), hold = (p->hold << 10) * clk >> 15;   /* GATED: the hold in 1024 tank samples at 22 kHz (46 ms) */
    uint32_t lk = 19480u * 32768u / (uint32_t)clk, apl[3];                  /* 0.1 Hz as a phase step per tank sample */
    static const uint16_t RM[SP_L] = {256, 211, 177, 139, 233, 193, 157, 121};
    for (k = 0; k < 3; k++)
        apl[k] = SP_APL[k] * clk >> 15;
    for (k = 0; k < SP_L; k++) {       /* once a block: the length glide and the LFO; the read position then moves in a ramp */
        int32_t t, m, e;
        sp.ph[k] += (uint32_t)p->rate * RM[k] * lk / 256u * (uint32_t)steps;
        t = (int32_t)(sp.ph[k] >> 16);
        m = ((t < 32768 ? t : 65535 - t) - 16384) * p->depth << 2;              /* triangle, +-depth samples (Q16) */
        m = m * clk >> 15;
        e = ((SP_LEN[k] * sc >> 8) * clk >> 15) << 16;
        sp.dl[k] = sp.dl[k] ? sp.dl[k] + ((e - sp.dl[k]) >> 4) : e;
        e = sp.dl[k] + m;
        st[k] = sp.cur[k] ? (e - sp.cur[k]) / steps : 0;
        if (!sp.cur[k])
            sp.cur[k] = e;
    }
    for (i = 0; i < (int32_t)n; i++) {
        int32_t xl = softclip((inl[i] + sp.xp) >> 1), xr = softclip((inr[i] + sp.xq) >> 1);
        sp.xp = inl[i];
        sp.xq = inr[i];
        sp.acc += (uint32_t)clk;
        if (sp.acc >= 65536u) {                        /* a tank step */
            int32_t y[SP_L], h[SP_L], b[SP_L], ol, or_, g = p->g, x;
            uint32_t w = sp.wr, aw = sp.aw;
            sp.acc -= 65536u;
            if (hold) {                                    /* GATED: open while the input is, and HOLD after it */
                int32_t ax = xl < 0 ? -xl : xl, bx = xr < 0 ? -xr : xr;
                ax = ax > bx ? ax : bx;
                sp.hold = ax > 120 ? hold : sp.hold > 0 ? sp.hold - 1 : 0;
                sp.gate += ((sp.hold ? 32767 : 0) - sp.gate) >> (sp.hold ? 6 : 9);
                g = 8000 + mulq15(g - 8000, sp.gate);       /* shut: the tank rings out quickly */
            }
            xl = sp_allpass(sp_ap[0][0], aw, apl[0], xl >> 2, ga);
            xl = sp_allpass(sp_ap[0][1], aw, apl[1], xl, ga);
            xl = sp_allpass(sp_ap[0][2], aw, apl[2], xl, ga);
            xr = sp_allpass(sp_ap[1][0], aw, apl[0], xr >> 2, ga);
            xr = sp_allpass(sp_ap[1][1], aw, apl[1], xr, ga);
            xr = sp_allpass(sp_ap[1][2], aw, apl[2], xr, ga);
            sp.aw = (aw + 1u) & (SP_AN - 1u);
            for (k = 0; k < SP_L; k++) {
                int32_t m, o, f, q;
                uint32_t pos = (w << 16) - (uint32_t)sp.cur[k];
                sp.cur[k] += st[k];
                o = sp_line[k][pos >> 16 & (SP_N - 1u)];
                f = sp_line[k][(pos >> 16) + 1u & (SP_N - 1u)] - o;
                q = o + sp_mul(f, (int32_t)(pos >> 1 & 0x7FFF));
                m = (q - sp.lp[k]) * da + sp.el[k];                        /* error feedback: no bias, no dead band, the tail dies away smoothly */
                sp.el[k] = m & 32767;
                sp.lp[k] += m >> 15;
                y[k] = sp.lp[k];
            }
            for (k = 0; k < SP_L; k += 2) {                /* 8x8 fast Hadamard: three butterfly stages */
                h[k] = y[k] + y[k + 1];
                h[k + 1] = y[k] - y[k + 1];
            }
            for (k = 0; k < SP_L; k += 4) {
                b[k] = h[k] + h[k + 2];
                b[k + 1] = h[k + 1] + h[k + 3];
                b[k + 2] = h[k] - h[k + 2];
                b[k + 3] = h[k + 1] - h[k + 3];
            }
            for (k = 0; k < 4; k++) {
                h[k] = b[k] + b[k + 4];
                h[k + 4] = b[k] - b[k + 4];
            }
            g = (g * 11585 >> 15) >> 1;                    /* 1/sqrt 8 of the Hadamard, and a Q14 product below so the sums keep 32 bits */
            for (k = 0; k < SP_L; k++) {
                int32_t m = h[k] * g + sp.ef[k];
                x = k & 1 ? xr : xl;                      /* L into the lines of the left taps (even), R into the right (odd) */
                sp.ef[k] = m & 16383;
                sp_line[k][w & (SP_N - 1u)] = (int16_t)clamp((m >> 14) + (k & 1 ? -x : x), -32767, 32767);
            }
            sp.wr = (w + 1u) & (SP_N - 1u);
            ol = (y[0] + y[2] + y[4] + y[6]) * p->out >> 14;   /* (the input went in at 1/4: the lines keep their headroom) */
            or_ = (y[1] + y[3] + y[5] + y[7]) * p->out >> 14;
            if (hold) {
                ol = mulq15(ol, sp.gate);
                or_ = mulq15(or_, sp.gate);
            }
            sp.pl = sp.cl;
            sp.pr = sp.cr;
            sp.cl = ol;
            sp.cr = or_;
        }
        wl[i] = sp.pl + ((sp.cl - sp.pl) * (int32_t)(sp.acc >> 2) >> 14);
        wr[i] = sp.pr + ((sp.cr - sp.pr) * (int32_t)(sp.acc >> 2) >> 14);
    }
}
