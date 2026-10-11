/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Everything that happens between two rendered blocks: the 27 keys play notes (chromatic from F3, OCT-/OCT+ shift
 * them), USB-MIDI and TRS MIDI IN play the part too (midi_control.c). The audio ISR runs this (fx.c mix_block). */
#define KB_SILENT 255u
static uint32_t kb_prev;
static volatile uint8_t kb_boot_hold;    /* #137: set from power-on to the end of the splash (main.c): the keys sound nothing,
                                          * and one held across its end stays silent until it is let go and pressed again */
static uint8_t kb_note[27];              /* per key: the note it started, KB_SILENT none */

#ifdef FM1_INPUT_LAT
/* the press latency stats (hal/fm1_input.h fm1_in_stat): from the first scan that saw key k closed to its note-on
 * here, and to the note's first sample leaving the DMA (kb_out_tick: audio.c) */
static uint32_t kb_out_tick;
static __attribute__((noinline)) void kb_lat(uint32_t k)
{
    uint32_t t0 = fm1_in.note_t0[k], d = fm1_ticks() - t0, o = kb_out_tick - t0;
    fm1_in_stat.kb_n++;
    fm1_in_stat.kb_sum += d;
    fm1_in_stat.dac_sum += o;
    if (d > fm1_in_stat.kb_max)
        fm1_in_stat.kb_max = d;
    if (o > fm1_in_stat.dac_max)
        fm1_in_stat.dac_max = o;
}
#endif

/* key k (0 = F3) with the octave buttons: a chromatic keyboard */
static uint32_t kb_pitch(uint32_t k) { return (uint32_t)clamp(53 + (int32_t)k + 12 * song.octave, 0, 127); }

static int key_holds(uint32_t note)
{
    uint32_t k;
    for (k = 0; k < 27u; k++)
        if (((fm1_in.notes >> k) & 1u) && kb_note[k] == note)
            return 1;
    return 0;
}

#include "midi_control.c"

/* key k down / up. A note another key or MIDI holds already sounds: it is not started (nor ended) again */
static void key_on(uint32_t k)
{
    uint32_t n = kb_pitch(k);
    int dup = key_holds(n) || midi_holds(n);
    kb_note[k] = (uint8_t)n;
    if (!dup)
        trk_note_on(&trk[0], n, 100);
}

static void key_off(uint32_t k)
{
    uint32_t n = kb_note[k];
    kb_note[k] = KB_SILENT;
    if (!key_holds(n) && !midi_holds(n))
        trk_note_off(&trk[0], n);
}

static void keyboard_block(void)
{
    uint32_t cur = fm1_in.notes, ch, k;
    ch = cur ^ kb_prev;
    if (!ch)
        return;
    if (kb_boot_hold) {                           /* #137: under the splash; their release is silent too */
        for (k = 0; k < 27u; k++)
            if ((ch & cur) >> k & 1u)
                kb_note[k] = KB_SILENT;
        kb_prev = cur;
        return;
    }
    for (k = 0; k < 27u; k++) {
        if (!((ch >> k) & 1u))
            continue;
        if ((cur >> k) & 1u) {
            key_on(k);
#ifdef FM1_INPUT_LAT
            kb_lat(k);
#endif
        } else if (kb_note[k] != KB_SILENT)
            key_off(k);
    }
    kb_prev = cur;
}

/* everything that happens between two rendered blocks */
static void events_block(uint32_t n)
{
    (void)n;
    if (midi_in_overflow) {                            /* a lost note-off must never leave a held note */
        mi_r = mi_w;
        memset(midi_on, 0, sizeof midi_on);
        memset(midi_sus, 0, sizeof midi_sus);
        midi_pedal = 0;
        trk_all_off(&trk[0]);
        RING_PUBLISH();
        midi_in_overflow = 0;
    }
    keyboard_block();
    while (mi_r != mi_w) {                            /* USB-MIDI (and TRS) in */
        uint32_t pkt = midi_in_q[mi_r % MQ], status = (pkt >> 8) & 0xFFu;
        mi_r++;
        if (status < 0xF8u)                           /* (realtime: no clock here) */
            midi_event(status & 0xF0u, (pkt >> 16) & 0x7Fu, (pkt >> 24) & 0x7Fu);
    }
}
