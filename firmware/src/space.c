/* SPDX-License-Identifier: GPL-3.0-only */
/* SPACE (issue #21): an 80s digital studio reverb, written from the published topologies (Schroeder allpass diffusers,
 * Jot's feedback delay network with a Hadamard mix); no code borrowed. Stereo out of a mono send (the voices and ECHO's
 * pair summed into send_r), AMOUNT is the send, the dry mix always passes.
 *
 * Vintage on purpose: the input is halved to a 22 kHz tank clock by a two-sample average (a low pass at ~11 kHz), soft
 * clipped (tanh), then diffused by three allpasses and fed to four lines whose outputs go through one-pole dampers, a
 * 4x4 Hadamard mix and the feedback gain. The lines hold 16 bits; every sum is 32 bits and clamped on the way back.
 * The line reads are linearly interpolated and wobble slowly (a triangle LFO per line): the chorus that keeps the tail
 * from ringing metallic. SIZE scales the line lengths (a glide, like tape) and so the decay; TONE the dampers.
 * Six programs, one engine: lengths, feedback, damping, diffusion and modulation; GATED also closes a gate on the tank
 * and its feedback HOLD after the input falls silent. L and R are different pairs of lines; the output is doubled back
 * to 44.1 kHz by linear interpolation. 4 x 2 KB + 3 x 256 samples = 17 KB of the pool. */
#define SP_N 2048u
#define SP_AN 256u
enum { SP_BEDROOM, SP_CASTLE, SP_PLATE, SP_CLOUD, SP_GATED, SP_WOBBLE, SP_NPROG };
typedef struct {
    uint8_t lo, hi;         /* line length at SIZE 0 / 127, of the longest, /256 */
    uint16_t g;             /* feedback gain, Q15 */
    uint16_t damp;          /* damper coefficient at TONE 64, Q15 (higher: brighter) */
    uint16_t ap;            /* diffuser coefficient, Q15 */
    uint8_t depth, rate;    /* LFO depth in tank samples, rate in units of ~0.1 Hz */
    uint8_t hold;           /* GATED: the gate stays open this long after the input, in 46 ms (0: no gate) */
} sp_prog_t;
static const sp_prog_t SP_PROG[SP_NPROG] = {
    {40, 130, 25000, 27500, 16000, 1, 3, 0},     /* BEDROOM: small, short, bright */
    {110, 250, 30500, 11000, 19000, 2, 3, 0},    /* CASTLE: big, long, dark */
    {70, 180, 29500, 27000, 24000, 2, 4, 0},     /* METAL PLATE: dense, bright */
    {140, 250, 30500, 17000, 20000, 8, 4, 0},    /* CLOUD: long, heavy modulation */
    {70, 150, 29000, 26000, 24000, 1, 3, 6},     /* GATED: dense, cut off by the gate */
    {90, 200, 29500, 21000, 18000, 14, 15, 0},   /* WOBBLE: the tail shimmers */
};
static const uint16_t SP_LEN[4] = {1051, 1327, 1637, 1993};    /* the longest line, tank samples (mutually odd-ish) */
static const uint16_t SP_APL[3] = {113, 181, 241};
static int16_t sp_line[4][SP_N] __attribute__((section(".pool")));
static int16_t sp_ap[3][SP_AN] __attribute__((section(".pool")));
static int32_t sp_in[CTL / 2u];
static struct {
    uint32_t wr, aw, ph[4];
    int32_t dl[4], cur[4], lp[4], el[4], ef[4], pl, pr, gate, hold;   /* el, ef: the remainders of the damper and the feedback product */
} sp;

static void space_clear(void)
{
    uint32_t i, k;
    for (k = 0; k < 4u; k++) {
        for (i = 0; i < SP_N; i++)
            sp_line[k][i] = 0;
        sp.lp[k] = sp.dl[k] = sp.cur[k] = sp.el[k] = sp.ef[k] = 0;
        sp.ph[k] = k * 0x40000000u;
    }
    for (k = 0; k < 3u; k++)
        for (i = 0; i < SP_AN; i++)
            sp_ap[k][i] = 0;
    sp.wr = sp.aw = 0;
    sp.pl = sp.pr = sp.gate = sp.hold = 0;
}

/* a Q15 product rounded toward zero: floor leaves a bias that keeps a limit cycle alive (and DC) in the tank, long after the sound */
static inline int32_t sp_mul(int32_t a, int32_t b)
{
    int32_t p = a * b;
    return (p + ((p >> 31) & 32767)) >> 15;
}

/* a Schroeder allpass on a 256-sample ring: v = x + g d, y = d - g v */
static inline int32_t sp_allpass(int16_t *b, uint32_t w, uint32_t len, int32_t x, int32_t g)
{
    int32_t d = b[(w - len) & (SP_AN - 1u)], v = clamp(x + sp_mul(d, g), -32767, 32767);
    b[w & (SP_AN - 1u)] = (int16_t)v;
    return d - sp_mul(v, g);
}

static __attribute__((noinline)) void space_block(const int32_t *in, int32_t *wl, int32_t *wr, uint32_t n)
{
    const sp_prog_t *p = &SP_PROG[clamp(song.g[G_RPROG], 0, SP_NPROG - 1)];
    int32_t sc = p->lo + (p->hi - p->lo) * song.g[G_RSIZE] / 127;
    int32_t da = clamp(p->damp + (song.g[G_RTONE] - 64) * 250, 1500, 32000), ga = p->ap, i, k;
    int32_t st[4], half = (int32_t)(n >> 1), hold = p->hold << 10;     /* GATED: the hold in 1024 tank samples (46 ms) */
    static const uint16_t RM[4] = {256, 211, 177, 139};
    for (k = 0; k < 4; k++) {          /* once a block: the length glide and the LFO; the read position then moves in a ramp */
        int32_t t, m, e;
        sp.ph[k] += (uint32_t)p->rate * RM[k] * 19480u / 256u * (uint32_t)half;   /* 0.1 Hz = 19480 of 2^32 at 22.05 kHz */
        t = (int32_t)(sp.ph[k] >> 16);
        m = ((t < 32768 ? t : 65535 - t) - 16384) * p->depth << 2;              /* triangle, +-depth samples (Q16) */
        e = (SP_LEN[k] * sc >> 8) << 16;
        sp.dl[k] = sp.dl[k] ? sp.dl[k] + ((e - sp.dl[k]) >> 4) : e;
        e = sp.dl[k] + m;
        st[k] = sp.cur[k] ? (e - sp.cur[k]) / half : 0;
        if (!sp.cur[k])
            sp.cur[k] = e;
    }
    for (i = 0; i < (int32_t)n; i += 2)
        sp_in[i >> 1] = softclip((in[i] + in[i + 1]) >> 1);
    for (i = 0; i < (int32_t)n / 2; i++) {
        int32_t x = sp_in[i], y[4], h[4], a, b, c, d, ol, or_;
        uint32_t w = sp.wr, aw = sp.aw;
        int32_t g = p->g;
        if (hold) {                                    /* GATED: open while the input is, and HOLD after it */
            int32_t ax = x < 0 ? -x : x;
            sp.hold = ax > 120 ? hold : sp.hold > 0 ? sp.hold - 1 : 0;
            sp.gate += ((sp.hold ? 32767 : 0) - sp.gate) >> (sp.hold ? 6 : 9);
            g = 8000 + mulq15(g - 8000, sp.gate);       /* shut: the tank rings out quickly */
        }
        x = sp_allpass(sp_ap[0], aw, SP_APL[0], x >> 2, ga);
        x = sp_allpass(sp_ap[1], aw, SP_APL[1], x, ga);
        x = sp_allpass(sp_ap[2], aw, SP_APL[2], x, ga);
        sp.aw = (aw + 1u) & (SP_AN - 1u);
        for (k = 0; k < 4; k++) {
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
        a = y[0] + y[1];
        b = y[0] - y[1];
        c = y[2] + y[3];
        d = y[2] - y[3];
        h[0] = a + c;
        h[1] = b + d;
        h[2] = a - c;
        h[3] = b - d;
        for (k = 0; k < 4; k++) {
            int32_t m = h[k] * (g >> 1) + sp.ef[k];                    /* (the Hadamard's 1/2 is in the gain) */
            sp.ef[k] = m & 32767;
            sp_line[k][w & (SP_N - 1u)] = (int16_t)clamp((m >> 15) + (k & 1 ? -x : x), -32767, 32767);
        }
        sp.wr = (w + 1u) & (SP_N - 1u);
        ol = (y[0] + y[2]) << 1;        /* (the input went in at 1/4: the lines keep their headroom) */
        or_ = (y[1] + y[3]) << 1;
        if (hold) {
            ol = mulq15(ol, sp.gate);
            or_ = mulq15(or_, sp.gate);
        }
        wl[2 * i] = (sp.pl + ol) >> 1;
        wr[2 * i] = (sp.pr + or_) >> 1;
        wl[2 * i + 1] = sp.pl = ol;
        wr[2 * i + 1] = sp.pr = or_;
    }
}
