/* SPDX-License-Identifier: GPL-3.0-only
 * Adapted from MIDI control contribution by ChanceTheMaker (2026).
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Minimal MIDI control, included by input.c: notes, pitch bend and the sustain pedal on every channel (omni), and the
 * panic controllers. USB and TRS share this state. Bend range is +-2 semitones (no RPN). */
static uint32_t midi_on[4];                 /* bit n: note n is held by a MIDI note-on */
static uint32_t midi_sus[4];                /* bit n: note n was let go under the pedal */
static uint8_t midi_pedal;
#define MBIT(a, n) (((a)[(n) >> 5] >> ((n) & 31u)) & 1u)
#define MSET(a, n) ((a)[(n) >> 5] |= 1u << ((n) & 31u))
#define MCLR(a, n) ((a)[(n) >> 5] &= ~(1u << ((n) & 31u)))

#if KOSMO_PERF
static void perf_cc(uint32_t cc, uint32_t v);        /* ui.c: CC 102.. set page, wave, scan, fat, detune, cutoff, preset, ECHO char / amount, SPACE program / amount (host-driven bench) */
#endif
static int key_holds(uint32_t note);        /* input.c: a key held plays this note */
static int midi_holds(uint32_t note) { return MBIT(midi_on, note) || MBIT(midi_sus, note); }

static void midi_release(uint32_t note)
{
    MCLR(midi_on, note);
    MCLR(midi_sus, note);
    if (!key_holds(note))
        trk_note_off(&trk[0], note);
}

static void midi_pedal_up(void)
{
    uint32_t n;
    midi_pedal = 0;
    for (n = 0; n < 128u; n++)
        if (MBIT(midi_sus, n))
            midi_release(n);
}

static void midi_all_notes_off(int sound_off)
{
    uint32_t n;
    for (n = 0; n < 128u; n++)
        if (MBIT(midi_on, n) || MBIT(midi_sus, n))
            midi_release(n);
    if (sound_off) {
        uint32_t i;
        trk_all_off(&trk[0]);
        for (i = 0; i < NVOICE; i++)
            if (trk[0].v[i].active)
                voice_kill(&trk[0].v[i]);          /* one-block fade, regardless of RELEASE */
    }
}

static void midi_note_event(uint32_t note, uint32_t vel)
{
    if (vel) {
        if (MBIT(midi_on, note) || MBIT(midi_sus, note))
            midi_release(note);                   /* a repeated note replaces the previous press */
        MSET(midi_on, note);
        if (!key_holds(note))
            trk_note_on(&trk[0], note, vel);
    } else if (MBIT(midi_on, note)) {
        MCLR(midi_on, note);
        if (midi_pedal)
            MSET(midi_sus, note);
        else
            midi_release(note);
    }
}

/* Channel voice messages only (realtime is dropped in events_block, SysEx never reaches here) */
static void __attribute__((noinline)) midi_event(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (st == 0x90u || st == 0x80u)
        midi_note_event(d1, st == 0x90u ? d2 : 0);
    else if (st == 0xE0u) {
        int32_t b = (int32_t)(d1 | (d2 << 7)) - 8192;
        midi_bend_target[0] = b * 512 / (b < 0 ? 8192 : 8191);          /* Q8 semitones, +-2 */
    }
    else if (st == 0xB0u) {
#if KOSMO_PERF
        if (d1 >= 102u && d1 <= 112u)
            perf_cc(d1, d2);
        else
#endif
        if (d1 == 64u) {
            if (d2 >= 64u)
                midi_pedal = 1;
            else
                midi_pedal_up();
        } else if (d1 == 120u || d1 == 123u)       /* All Sound Off / All Notes Off */
            midi_all_notes_off(d1 == 120u);
        else if (d1 == 121u) {                     /* Reset All Controllers */
            midi_bend_target[0] = 0;
            midi_pedal_up();
        }
    }
}
