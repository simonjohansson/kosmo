/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Host-side render of the KOSMO DSP (voices, the ROOM reverb, the master) to a WAV, for debugging sound without
 * hardware. Same sources as the firmware.
 *   tests/run_tests.sh builds it into build/host/;
 *   build/host/hostsim SOUND MONO OUT.wav [REVERB]
 * env: SECS=n renders n s (the 2 s note pattern repeats), CHORD[=k] holds k keys (default 4, up to 8), BENCH=1
 * prints the render time, LEVEL=l, SWEEP=1, NOTE=k, OCT=o, PSET=id:v,... (P_* ids), VSWEEP=1, PSWEEP=id:a:b. */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define __attribute__(x)
#define memset felucca_memset
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "felucca_tables.h"
#include "../firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp
static struct { volatile uint32_t notes, buttons; } fm1_in;
#include "../firmware/src/core.h"
#include "../firmware/src/engines.c"
#include "../firmware/src/params.c"
#include "../firmware/src/presets.c"
#include "../firmware/src/voice.c"
#include "../firmware/src/fx.c"
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#include "../firmware/src/usb.c"
#include "../firmware/src/midi_uart.c"                /* TRS MIDI IN: its parser (um_byte) feeds midi_in_q */
#include "../firmware/src/input.c"
#define inst (trk[0])                   /* the part */

static void wav_hdr(FILE *f, uint32_t frames)
{
    uint32_t v;
    fwrite("RIFF", 1, 4, f); v = 36 + frames * 4; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    uint16_t a = 1, ch = 2, ba = 4, bits = 16; uint32_t sr = FS, br = FS * 4;
    fwrite(&a, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); v = frames * 4; fwrite(&v, 4, 1, f);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* the sounds of the tests: the engine's E values (WAVE SCAN CUT RES), ATK DEC SUS REL, the ENV -> FILTER amount,
 * MONO, the reverb send */
typedef struct {
    const char *name;
    int8_t e[8];
    uint8_t env[4];
    int8_t fenv;
    uint8_t mono;
    uint8_t rev;
} host_sound_t;
static const host_sound_t HOST_SOUNDS[] = {
    {"ROBOT LEAD", {0, 40, 90, 30}, {4, 70, 100, 50}, 20, 1, 30},
    {"CHOIR PAD", {1, 30, 60, 10}, {80, 90, 110, 95}, 10, 0, 70},
    {"BELLS", {2, 90, 110, 20}, {0, 88, 0, 60}, 30, 0, 50},
    {"BUZZ BASS", {3, 20, 50, 70}, {0, 60, 40, 30}, 30, 1, 10},
    {"ORGAN", {4, 60, 127, 0}, {2, 80, 100, 40}, 0, 0, 30},
    {"ZAP", {7, 127, 70, 100}, {0, 55, 20, 30}, 48, 1, 15},
};
#define NHOST_SOUNDS ((uint32_t)NELEM(HOST_SOUNDS))

static void host_tracks_init(void)                /* as kosmo_init (main.c): the power-on values */
{
    uint32_t i;
    for (i = 0; i < G_COUNT; i++)
        song.g[i] = GP[i].def;
    for (i = 0; i < P_E0; i++)
        trk[0].p[i] = TP[i].def;
    for (i = 0; i < 8u; i++)
        trk[0].p[P_E0 + i] = ENGINE->edit[i].def;
    ec_init();
    ec.dirty = 1;                                     /* (the lines hold the last test's audio) */
    ec.clr = 0;
    song.master_q12 = 4096;
}

/* sound pi (HOST_SOUNDS) onto the part: its sound and reverb send, its voice mode */
static void host_sound(track_t *t, uint32_t pi)
{
    const host_sound_t *s = &HOST_SOUNDS[pi % NHOST_SOUNDS];
    uint32_t i;
    for (i = 0; i < 8u; i++)
        t->p[P_E0 + i] = s->e[i];
    for (i = 0; i < 4u; i++)
        t->p[P_ATK + i] = s->env[i];
    t->p[P_ED_FLT] = s->fenv;
    t->p[P_VOICE] = s->mono ? V_LEGATO : V_POLY;
    t->p[P_REV] = s->rev;
}

int main(int argc, char **argv)
{
    int sound = argc > 1 ? atoi(argv[1]) : 4;
    int mono = argc > 2 ? atoi(argv[2]) : 1;
    const char *out = argc > 3 ? argv[3] : "out.wav";
    uint32_t i, frames = FS * (getenv("SWEEP") ? 4 : 2), f, nk = 0;
    clock_t c0;
    FILE *w = fopen(out, "wb");
    if (getenv("SECS"))
        frames = FS * (uint32_t)atoi(getenv("SECS"));
    if (getenv("CHORD"))
        nk = atoi(getenv("CHORD")) > 4 ? (uint32_t)atoi(getenv("CHORD")) : 4u;
    host_tracks_init();
    host_sound(&inst, (uint32_t)sound);
    inst.p[P_VOICE] = (int16_t)mono;
    if (argc > 4)
        inst.p[P_REV] = (int16_t)atoi(argv[4]);
    if (getenv("OCT"))                                   /* OCT=o: keyboard octave shift (key 7 = C4 + 12 o) */
        song.octave = (int8_t)atoi(getenv("OCT"));
    if (getenv("PSET")) {                               /* PSET=id:value,... (part parameter ids, P_E0 = 18) */
        const char *s = getenv("PSET");
        int id, val, k;
        while (sscanf(s, "%d:%d%n", &id, &val, &k) == 2) {
            if (id >= 0 && id < P_COUNT)
                inst.p[id] = (int16_t)val;
            s += k;
            if (*s == ',')
                s++;
        }
    }
    wav_hdr(w, frames);
    c0 = clock();
    for (f = 0; f < frames; f += CTL) {
        int32_t o[2 * CTL];
        uint32_t fp = f % (2u * FS);    /* position in the repeating 2 s pattern */
        {   /* C4 0.1-0.5 s, E4 0.45-0.8 s (overlap: legato/steal), G4 0.9-1.2 s */
            uint32_t n = 0, f = fp;
            if (f > FS / 10 && f < FS / 2) n |= 1u << 7;
            if (f > FS * 45 / 100 && f < FS * 8 / 10) n |= 1u << 11;
            if (f > FS * 9 / 10 && f < FS * 12 / 10) n |= 1u << 14;
            fm1_in.notes = n;
        }
        if (nk) {                       /* keys held 0.1-1.5 s (C E G B, then D F A C) */
            static const uint8_t K[8] = {7, 11, 14, 18, 9, 12, 16, 19};
            uint32_t n = 0, k;
            for (k = 0; k < nk && k < 8u; k++)
                n |= 1u << K[k];
            fm1_in.notes = (fp > FS / 10 && fp < FS * 3 / 2) ? n : 0;
        }
        if (getenv("LEVEL"))
            inst.p[P_LEVEL] = (int16_t)atoi(getenv("LEVEL"));
        if (getenv("SWEEP")) {          /* cutoff and resonance sweeps while a note is held */
            uint32_t t = f * 1000 / FS;  /* ms */
            inst.p[P_E2] = (int16_t)((t / 4) % 256 < 128 ? (t / 4) % 128 : 127 - (t / 4) % 128);
            inst.p[P_E3] = (int16_t)((t / 7) % 256 < 128 ? (t / 7) % 128 : 127 - (t / 7) % 128);
            fm1_in.notes = (f > FS / 20) ? (1u << 7) : 0;
        }
        if (getenv("NOTE"))             /* NOTE=k: one key (7 = C4, see OCT) held to 0.5 s before the end */
            fm1_in.notes = (f > FS / 20 && f + FS / 2 < frames) ? 1u << atoi(getenv("NOTE")) : 0;
        if (getenv("VSWEEP"))           /* VSWEEP=1: SCAN (P_E1) from 0 to 127 over the render */
            inst.p[P_E1] = (int16_t)((uint64_t)f * 128u / frames);
        if (getenv("PSWEEP")) {         /* PSWEEP=id:from:to: a part parameter from .. to over the render */
            int id, a, b;
            if (sscanf(getenv("PSWEEP"), "%d:%d:%d", &id, &a, &b) == 3 && id >= 0 && id < P_COUNT)
                inst.p[id] = (int16_t)(a + (int64_t)(b - a) * f / frames);
        }
        mix_block(o, CTL);
        for (i = 0; i < 2 * CTL; i++) {
            int16_t s = (int16_t)(o[i] > 32767 ? 32767 : o[i] < -32768 ? -32768 : o[i]);
            fwrite(&s, 2, 1, w);
        }
    }
    if (getenv("BENCH"))
        fprintf(stderr, "%.1f s audio in %.1f ms (%.2f %% of real time)\n", (double)frames / FS,
                (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC,
                (double)(clock() - c0) * 100.0 / CLOCKS_PER_SEC / ((double)frames / FS));
    fclose(w);
    return 0;
}
