/* SPDX-License-Identifier: GPL-3.0-only
 * End-to-end MIDI input tests using the real queues, the UART parser, the key path and the voice renderer:
 * notes, pitch bend, the sustain pedal, the panic controllers, ownership between keys and MIDI, ring recovery. */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int bad;
static int32_t dummy_out[2 * CTL];
static int check(const char *what, int ok)
{
    printf("midi: %-76s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
    return ok ? 0 : 1;
}
static void reset(void)
{
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    host_tracks_init();
    host_sound(&trk[0], 0);
    trk[0].p[P_VOICE] = V_POLY;
    memset(midi_on, 0, sizeof midi_on);
    memset(midi_sus, 0, sizeof midi_sus);
    memset(kb_note, KB_SILENT, sizeof kb_note);
    memset(midi_bend_q8, 0, sizeof midi_bend_q8);
    memset(midi_bend_target, 0, sizeof midi_bend_target);
    memset(&um, 0, sizeof um);
    midi_pedal = 0;
    mi_r = mi_w = 0;
    midi_in_overflow = 0;
    fm1_in.notes = kb_prev = 0;
    events_block(CTL);
}
static void queued(uint32_t st, uint32_t d1, uint32_t d2, uint32_t source)
{
    midi_enqueue((st >= 0xF8u ? 0xFu : st >> 4) | st << 8 | d1 << 16 | d2 << 24, source);
    events_block(CTL);
}
static int gate_note(uint32_t note)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (trk[0].v[i].active && trk[0].v[i].gate && trk[0].v[i].note == note)
            return 1;
    return 0;
}
static uint32_t gates(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[0].v[i].active && trk[0].v[i].gate;
    return n;
}

static void notes_and_bend(void)
{
    uint32_t before;
    reset();
    queued(0x90, 60, 100, 1);
    check("a USB note-on plays", gate_note(60));
    queued(0x80, 60, 0, 1);
    check("its note-off releases it", !gate_note(60));
    queued(0x91, 62, 100, 2);
    queued(0x9F, 64, 100, 1);
    check("every channel plays the part (TRS and USB, channels 2 and 16)", gate_note(62) && gate_note(64));
    queued(0x92, 62, 0, 2);
    check("a note-on with velocity 0 is a note-off", !gate_note(62) && gate_note(64));
    queued(0xE0, 127, 127, 1);
    check("USB maximum bend is exactly +2 semitones", midi_bend_target[0] == 512);
    queued(0xE0, 0, 0, 1);
    check("USB minimum bend is exactly -2 semitones", midi_bend_target[0] == -512);
    queued(0xE0, 0, 64, 1);
    check("bend centre is zero", midi_bend_target[0] == 0);
    queued(0xE0, 127, 127, 1);
    before = (uint32_t)midi_bend_q8[0];
    mix_block(dummy_out, CTL);
    check("bend is smoothed toward the target in the real voice renderer",
          (uint32_t)midi_bend_q8[0] > before && midi_bend_q8[0] < midi_bend_target[0]);
    {
        uint32_t i;
        for (i = 0; i < 40u; i++)
            mix_block(dummy_out, CTL);
    }
    check("smoothing reaches the exact target and the note keeps its number", midi_bend_q8[0] == 512 && gate_note(64));
    queued(0xB0, 121, 0, 1);
    check("Reset All Controllers centres the bend and keeps the sounding note", !midi_bend_target[0] && gate_note(64));
    queued(0xB0, 7, 20, 1);
    queued(0xB0, 1, 99, 1);
    queued(0xD0, 50, 0, 1);
    check("controllers the part does not use are ignored", gate_note(64) && gates() == 1u);
    queued(0xF8, 0, 0, 1);
    queued(0xFA, 0, 0, 1);
    check("realtime (clock, start) is dropped", gate_note(64) && gates() == 1u);
}

static void sustain(void)
{
    reset();
    queued(0xB0, 64, 127, 1);
    queued(0x90, 60, 100, 1);
    queued(0x80, 60, 0, 1);
    check("the pedal holds a released note", gate_note(60) && MBIT(midi_sus, 60));
    queued(0x90, 60, 100, 1);
    queued(0x80, 60, 0, 1);
    check("a repeated note under the pedal is one note", gates() == 1u);
    queued(0xB0, 64, 0, 1);
    check("the pedal up releases it", !gate_note(60) && !MBIT(midi_sus, 60) && !midi_pedal);
    queued(0xB0, 64, 127, 2);
    queued(0x90, 62, 100, 2);
    queued(0xB0, 123, 0, 2);
    check("All Notes Off ends held notes and the pedal's notes", !gate_note(62));
    queued(0x90, 64, 100, 1);
    queued(0xB0, 64, 127, 1);
    queued(0x80, 64, 0, 1);
    queued(0xB0, 121, 0, 1);
    check("Reset All Controllers releases pedal-held notes", !gate_note(64) && !midi_pedal);
    queued(0x90, 67, 100, 1);
    queued(0xB0, 120, 0, 1);
    check("All Sound Off silences at once", !gate_note(67) && gates() == 0u);
}

static void ownership(void)
{
    uint32_t i;
    reset();
    fm1_in.notes = 1u << 7;                           /* key 7 = C4 (60) */
    events_block(CTL);
    check("a key plays its note", gate_note(60));
    queued(0x90, 60, 100, 1);
    check("MIDI on a note a key holds starts nothing new", gates() == 1u);
    queued(0x80, 60, 0, 1);
    check("a MIDI release cannot cut a still-held key", gate_note(60));
    fm1_in.notes = 0;
    events_block(CTL);
    check("the key's release ends the note", !gate_note(60));
    queued(0x90, 60, 100, 1);
    fm1_in.notes = 1u << 7;
    events_block(CTL);
    fm1_in.notes = 0;
    events_block(CTL);
    check("a key release cannot cut a note MIDI still holds", gate_note(60));
    queued(0x80, 60, 0, 1);
    check("the last owner ends it", !gate_note(60));
    song.octave = 1;
    fm1_in.notes = 1u << 7;
    events_block(CTL);
    check("OCT+ shifts the keys by an octave", gate_note(72));
    song.octave = -2;
    fm1_in.notes = 0;
    events_block(CTL);
    check("the release follows the note the key started, not the octave now", !gate_note(72) && gates() == 0u);
    song.octave = 0;
    for (i = 0; i < 8u; i++)                          /* ring overflow: a lost note-off must not leave a note */
        queued(0x90, 40u + i, 100, 1);
    queued(0xE0, 127, 127, 1);
    midi_in_overflow = 1;
    events_block(CTL);
    check("ring overflow recovers: the ring is emptied, held notes released, no pedal",
          !midi_in_overflow && mi_r == mi_w && gates() == 0u && !midi_pedal);
    queued(0x90, 50, 100, 1);
    check("... and MIDI plays again", gate_note(50));
}

static void burst(void)
{
    uint8_t pkt[64];
    uint32_t e, k, taken = 0;
    reset();
    queued(0x90, 48, 100, 1);
    for (e = 0; e < 16; e++) {                       /* CC7 = 100 on 16 channels */
        pkt[4 * e] = 0x0B; pkt[4 * e + 1] = (uint8_t)(0xB0 | e); pkt[4 * e + 2] = 7; pkt[4 * e + 3] = 100;
    }
    for (k = 0; k < 5; k++)                          /* 5 polls at 2 kHz before the next half drains */
        taken += (uint32_t)ep1_take(pkt, sizeof pkt);
    check("a USB burst fills the MIDI ring without overflowing it", taken == 3u && !midi_in_overflow && mi_w - mi_r == 48u);
    check("the packet that does not fit is refused (left for the next poll)", !ep1_take(pkt, sizeof pkt) && mi_w - mi_r == 48u);
    for (e = 0; e < 8; e++)                          /* TRS MIDI meanwhile: its 8 slots are free */
        midi_in_event(0x0Bu | 0xB1u << 8 | 1u << 16 | e << 24);
    check("TRS MIDI into a ring full of USB still fits (no overflow)", !midi_in_overflow && mi_w - mi_r == 56u);
    events_block(CTL);
    check("the ring drains with no panic: the held note keeps sounding", gate_note(48) && mi_r == mi_w);
    check("the refused packet is taken once the ring has room", ep1_take(pkt, sizeof pkt) && mi_w - mi_r == 16u);
    events_block(CTL);
    check("the held note survives all 88 events", gate_note(48) && !midi_in_overflow);
}

static void uart(void)
{
    reset();
    um_byte(0x90); um_byte(60); um_byte(100);
    events_block(CTL);
    check("TRS: a note-on", gate_note(60));
    um_byte(0xF8);                                    /* realtime between data bytes: the message survives */
    um_byte(62); um_byte(0x7F);
    events_block(CTL);
    check("TRS: running status, with a clock byte inside the message", gate_note(62) && gate_note(60));
    um_byte(0x80); um_byte(60); um_byte(0);
    um_byte(62); um_byte(0);
    events_block(CTL);
    check("TRS: note-offs, running status", gates() == 0u);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    notes_and_bend();
    sustain();
    ownership();
    burst();
    uart();
    printf("%s\n", bad ? "MIDI CONTROL TEST FAILED" : "MIDI control integration tests passed");
    return bad != 0;
}
