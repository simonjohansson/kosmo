/* SPDX-License-Identifier: GPL-3.0-only */
/* Two LFOs, ticked once per control block (CTL samples), free-running 0.05 .. 20 Hz, no tempo sync.
 * LFO 1 moves a target of the engine playing (the engine's lt table: eng_wave.c, eng_sem.c); one shared LFO, or with LFO RETRIG
 * (SETTINGS) one per voice that restarts at its note-on (voice.c). LFO 2 moves one FX parameter, always one shared LFO.
 * DEPTH 0 is off and leaves every value as it was. The output is smoothed (a one-pole of a few blocks), so a square or a
 * sample & hold steps over ~3 ms instead of zippering. A target moves around its knob value, +-its range at full DEPTH, clamped. */
static uint8_t lfo_retrig;                      /* SETTINGS LFO RETRIG (ui.c syncs it) */
static lfo_t lfo1g = {0, 0, 0, 0x1234567}, lfo2g = {0, 0, 0, 0x2345679};   /* the shared ones */

/* the shape at phase ph (Q32), Q15: starts at 0 and rises. RND holds one value per cycle (hold) */
static inline int32_t lfo_wave(uint32_t shape, uint32_t ph, int32_t hold)
{
    switch (shape) {
    case LS_SINE: return sine_i(ph);
    case LS_TRI: return osc_tri(ph + 0x40000000u);
    case LS_SAW: return (int32_t)ph >> 16;
    case LS_SQUARE: return ph < 0x80000000u ? 32767 : -32767;
    default: return hold;
    }
}

/* the phase step per block for RATE 0..127: 0.05 Hz * 400^(RATE / 127), as a pitch (125 = 12.8 Hz = 0.05 Hz * 256) */
static inline uint32_t lfo_inc(int32_t rate) { return pitch_inc(125u + (uint32_t)clamp(rate, 0, 127) * 835u / 64u) >> (8 - CTL_LOG2); }

static int32_t lfo_tick(lfo_t *l, uint32_t shape, uint32_t inc)
{
    uint32_t ph = l->ph + inc;
    if (ph < inc)
        l->hold = (int32_t)(noise32(&l->seed) >> 16) - 32768;
    l->ph = ph;
    l->sm += (lfo_wave(shape, ph, l->hold) - l->sm) >> 2;
    return l->sm;
}

static void lfo_start(lfo_t *l, uint32_t n)      /* phase 0 */
{
    l->ph = 0;
    l->sm = 0;
    l->seed = (int32_t)(n * 2654435761u | 1u);
    l->hold = (int32_t)(noise32(&l->seed) >> 16) - 32768;
}

static inline int32_t lfo_depth(int32_t v, int32_t depth) { return (v * depth) >> 7; }       /* DEPTH 0..127 */
static inline int32_t lfo_delta(int32_t lm, int32_t span) { return (lm * span) >> 15; }      /* +-span at full DEPTH */

/* ct cents as whole 1/16 semitones and the rest in 1/4096 of the increment (as track_render's TUNE) */
static int32_t cents_split(int32_t ct, int32_t *fine)
{
    int32_t c16 = ct >= 0 ? ct * 16 / 100 : -((-ct * 16 + 99) / 100);
    *fine = (ct * 16 - c16 * 100) * 2367 / 16000;
    return c16;
}

/* LFO 2: the FX parameter (song.g, or the SPACE amount) is replaced by its modulated value for the block and put back after it
 * (both in the audio ISR: the UI never sees the modulated one) */
static const ltgt_t LT_FX[6] = {{LK_ETIME, "E Time", "ECHO TIME"}, {LK_EFB, "E Reps", "ECHO REPEATS"}, {LK_EAMT, "E Amt", "ECHO AMOUNT"},
                                {LK_RSIZE, "S Size", "SPACE SIZE"}, {LK_RTONE, "S Tone", "SPACE TONE"}, {LK_RAMT, "S Amt", "SPACE AMOUNT"}};
static int16_t *lfo2_ptr, lfo2_sv;
static int16_t *lfo2_target(uint32_t i)
{
    switch (LT_FX[i > 5u ? 0u : i].kind) {
    case LK_EFB: return &song.g[G_EFB];
    case LK_EAMT: return &song.g[G_EAMT];
    case LK_RSIZE: return &song.g[G_RSIZE];
    case LK_RTONE: return &song.g[G_RTONE];
    case LK_RAMT: return &trk[0].p[P_REV];
    default: return &song.g[G_ETIME];
    }
}
static void lfo2_begin(void)
{
    const int16_t *p = trk[0].p;
    int32_t lm = lfo_depth(lfo_tick(&lfo2g, (uint32_t)p[P_L2SH], lfo_inc(p[P_L2RT])), p[P_L2DP]);
    int16_t *t;
    if (!p[P_L2DP])
        return;
    t = lfo2_target((uint32_t)p[P_L2T]);
    if (t == &song.g[G_EAMT] && !*t)
        return;                                 /* ECHO off stays off (it would start and clear its lines every cycle) */
    lfo2_ptr = t;
    lfo2_sv = *t;
    *t = (int16_t)clamp(*t + lfo_delta(lm, 127), 0, 127);
}
static void lfo2_end(void)
{
    if (lfo2_ptr)
        *lfo2_ptr = lfo2_sv;
    lfo2_ptr = 0;
}
