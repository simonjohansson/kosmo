/* SPDX-License-Identifier: GPL-3.0-only */
/* The filter's magnitude response for the FILTER page, in dB * 16 at column x (0..199) of a log frequency axis
 * (pitch 272 + x * 1760 / 199 in 1/16 semitone: 22 Hz .. 12.5 kHz). The ladder types read the table made by
 * tools/gen_tables.py from the same ladder equations (LAD_DB, shift-invariant in log frequency: shifted by the
 * cutoff's pre-warped frequency, interpolated between its 16 RES steps); the comb evaluates its own transfer function
 * (delay and feedback as comb_tap runs them, linear interpolation of the delay included). */

/* log2(v) * 256, v > 0 (within 0.004 of exact) */
static int32_t ilog2_q8(uint32_t v)
{
    int32_t n = 31, m;
    while (!(v >> n))
        n--;
    m = (int32_t)((n >= 8 ? v >> (n - 8) : v << (8 - n)) & 255u);
    return (n << 8) + m + ((m * (256 - m) * 89) >> 16);
}

static int32_t fcurve_db16(uint32_t ft, int32_t cut, int32_t res, int32_t x)   /* cut, res: 0..127 */
{
    if (ft < 4u) {
        int32_t o = LAD_X[x] - ilog2_q8((uint32_t)tan_g(cut << 8)) + 12 * 256 + 1024;   /* 1/256 octave from -4 */
        int32_t oc = clamp(o, 0, 127 * 16), i = oc >> 4 < 126 ? oc >> 4 : 126, fr = oc - i * 16, rp = res * 15 * 256 / 127;
        int32_t j = rp >> 8 < 14 ? rp >> 8 : 14, fj = rp - j * 256, v0, v1;
        const uint8_t *a = LAD_DB + (ft * 16 + (uint32_t)j) * 128 + i, *b = a + 128;
        v0 = ((a[0] * (256 - fj) + b[0] * fj) >> 6) - 48 * 16;       /* dB * 16 at points i and i + 1 */
        v1 = ((a[1] * (256 - fj) + b[1] * fj) >> 6) - 48 * 16;
        if (o != oc)                                                /* past the table: the skirt's slope, dB / octave */
            return o < oc ? v0 + (int32_t[]){0, 12, 24, 0}[ft] * (o - oc) / 16 : v1 + (int32_t[]){-24, -12, 0, 0}[ft] * (o - oc) / 16;
        return (v0 * (16 - fr) + v1 * fr) >> 4;
    } else {
        int32_t c = cut << 8, d = COMB_D[cut], g = res * 248, di, f;
        uint32_t inc = pitch_inc((uint32_t)(272 + x * 1760 / 199)), ph;
        int32_t cs, sn, fr_, fi_, tr, ti, dr, di_;
        if (cut < 127)
            d += ((COMB_D[cut + 1] - d) * (c & 255)) >> 8;
        di = d >> 6;
        f = d & 63;
        ph = inc * (uint32_t)di;
        cs = sine_i(inc + 0x40000000u) >> 3;                       /* e^-jw, Q12 */
        sn = -(sine_i(inc) >> 3);
        fr_ = (64 - f) * 64 + f * cs / 64;                         /* (1 - f') + f' e^-jw */
        fi_ = f * sn / 64;
        cs = sine_i(ph + 0x40000000u) >> 3;                        /* e^-jw di */
        sn = -(sine_i(ph) >> 3);
        tr = (fr_ * cs - fi_ * sn) >> 12;                          /* T = both, Q12 */
        ti = (fr_ * sn + fi_ * cs) >> 12;
        dr = 4096 - ((g >> 3) * tr >> 12);                         /* den = 1 - g T */
        di_ = -((g >> 3) * ti >> 12);
        /* 20 log10 ((1 - g) / |den|) = 3.01 * (log2 (1 - g)^2 - log2 |den|^2), * 16 for dB * 16 */
        return (3083 * (2 * (ilog2_q8((uint32_t)(32768 - g)) - 15 * 256) -
                        (ilog2_q8((uint32_t)(dr * dr + di_ * di_ + 1)) - 24 * 256))) >> 14;
    }
}
