/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* KOSMO core types: the part, its voices and parameters. One synth part with NVOICE voices. */
#include <stdint.h>

/* ------------------------------------------------------------ sizes --- */
#if KOSMO_PERF
static uint32_t pfx_s[8], pfx_n[8];   /* section timers (perf.c prints them): 0 events 1 mix_part 2 voice render 3 reverb 4 master 5 uac+out */
#define PFX(k, code) do { uint32_t _t = fm1_ticks(); code; pfx_s[k] += fm1_ticks() - _t; pfx_n[k]++; } while (0)
#else
#define PFX(k, code) code
#endif
#define NVOICE 8                 /* voices of the part */
#define NPART 1                  /* synth parts */
#define NTRK NPART
#define HALF_FRAMES 128          /* I2S half buffer: 2.9 ms at 44.1 kHz (a key waits 0..1 half, then plays 1 half later) */
#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------- parameters --- */
enum { F_INT, F_PCT, F_BIPCT, F_TIME, F_CUTOFF, F_DB, F_ENUM };

typedef struct {
    const char *label;
    uint8_t fmt;
    int16_t min, max, def;
    const char *const *names;   /* F_ENUM */
    const char *unit;           /* F_INT / F_ENUM optional unit */
} param_desc_t;

enum {                          /* per-part parameters */
    P_LEVEL,
    P_ATK, P_DEC, P_SUS, P_REL,
    P_ED_FLT, P_ED_PIT, P_ED_SHP,              /* ENV -> filter, pitch, shape amounts */
    P_REV,                                     /* SPACE reverb send (AMOUNT) */
    P_VOICE, P_GLIDE, P_PAN, P_MUTE,
    P_GLMODE, P_PRIO, P_ALLOC, P_DETUNE,
    P_FTYPE,                                   /* filter TYPE: LP / BP / HP / NOTCH / COMB (eng_wave.c) */
    P_L1SH, P_L1RT, P_L1DP, P_L1TW, P_L1TS,   /* LFO 1 (the engine): SHAPE RATE DEPTH, TARGET per engine (WAVE, SEM) (lfo.c); before P_E0: power-on defaults come from TP */
    P_L2SH, P_L2RT, P_L2DP, P_L2T,             /* LFO 2 (an FX): SHAPE RATE DEPTH TARGET */
    P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7,
    P_S0, P_S1, P_S2, P_S3, P_S4, P_S5, P_S6, P_S7,   /* SEM's own parameters (eng_sem.c); CUT / RES stay P_E2 / P_E3 for both engines */
    P_ENG,                                     /* the engine: index into ENGINES (engines.c), ALGORITHM turns it */
    P_COUNT
};

enum {                          /* global parameters */
    G_TUNE,                     /* cents */
    G_RSIZE, G_RTONE, G_RPROG,   /* SPACE reverb: SIZE, TONE, ROOM (program, space.c) */
    G_ETIME, G_EFB, G_ECHAR, G_EAMT,   /* ECHO: TIME, REPEATS, CHARACTER (echo.c), AMOUNT; the order presets.c loads */
    G_COUNT
};

enum { EC_CLEAN, EC_CRUNCH, EC_TOY, EC_BACK, EC_ROBOT };   /* G_ECHAR */

/* ---------------------------------------------------------- LFOs --- */
enum { LS_SINE, LS_TRI, LS_SAW, LS_SQUARE, LS_RND };       /* SHAPE */
enum { LK_SCAN, LK_PITCH, LK_CUT, LK_VOL, LK_PW, LK_TUNE2, LK_MIX,          /* LFO 1 targets (the engines'), then LFO 2's */
       LK_ETIME, LK_EFB, LK_EAMT, LK_RSIZE, LK_RTONE, LK_RAMT, LK_NONE };
typedef struct { uint8_t kind; const char *bar, *full; } ltgt_t;   /* a target: what it moves, its knob label, its name on the picture */
typedef struct { uint32_t ph; int32_t sm, hold, seed; } lfo_t;      /* phase, smoothed output, the S&H value, its noise */

/* ------------------------------------------------- voices, engines --- */
enum { V_POLY, V_MONO, V_LEGATO, V_UNISON };   /* P_VOICE */
typedef struct {
    uint8_t note, vel, gate, active;
    uint8_t stage;               /* env: 0 off, 1 attack, 2 decay/sustain, 3 release, 4 given up (fades out) */
    int32_t env;                 /* Q24 */
    int32_t env_out;             /* last control-rate amplitude, Q15 */
    int32_t pitch16, pitch_cur;  /* 1/16 semitone, with glide */
    int32_t gstep;               /* glide TIME mode: 1/16 st per control tick, 0 = RATE mode */
    int32_t fk;                  /* unison copy's spread, in DETUNE units (fine = fk * DETUNE * 56 / 889) */
    int32_t fine;                /* unison detune: phase increment * (1 + fine / 4096) */
    uint32_t ph[3];
    int32_t s[10];               /* engine state (filters, envs) */
    uint32_t age;
    lfo_t lfo;                   /* LFO 1 of this voice (LFO RETRIG ON) */
} voice_t;

typedef struct {                 /* per-voice control-rate modulation, computed in voice.c */
    uint32_t inc;                /* phase increment of the base pitch */
    int32_t pitch16;
    int32_t amp0, amp1;          /* Q15 ramp over the block */
    int32_t cutoff;              /* 0..127 << 8 */
    int32_t envq15;              /* env value (for engines that use it as a mod source) */
    int32_t fk;                  /* unison copy's spread, in DETUNE units (fine = fk * DETUNE * 56 / 889) */
    int32_t fine;                /* the residual below 1/16 semitone in inc: unison detune, TUNE, bend (1/4096) */
    int32_t lgain, lpit, lfine, lcut;   /* LFO 1 (lfo_voice): the amplitude factor (Q15), pitch (1/16 semitone, residual) and cutoff it adds */
    int32_t lscan, lpw, ltune, lmix;    /* and the engine's own targets: SCAN (Q8 frames), PW (Q8), VCO2 TUNE (cents), MIX */
} vmod_t;

struct track;
typedef struct {                 /* the engine (engines.c ENGINE; eng_wave.c) */
    const char *name;
    param_desc_t edit[8];        /* the engine's own parameters: P_E0..P_E7, P_S0..P_S7 for SEM (pbase) */
    uint8_t pbase;               /* P_E0 / P_S0 */
    const ltgt_t *lt;            /* LFO 1's targets in this engine, nlt of them; P_L1TW / P_L1TS remembers the choice (ltp) */
    uint8_t nlt, ltp;
    uint16_t keep;               /* bit k: s[k] is kept when a sounding voice is retriggered (filters) */
    void (*note_on)(struct track *t, voice_t *v);
    void (*render)(struct track *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m);
} engine_t;

/* ------------------------------------------------------------- part --- */
typedef struct track {
    int16_t p[P_COUNT];
    voice_t v[NVOICE];
    /* mono */
    uint8_t mono_stack[8];
    uint8_t nmono;
    uint8_t eng;                 /* the engine the voices play (P_ENG, taken over at a block start: track_render) */
    uint8_t fat_seen;            /* P_E4 the held notes' copies were last matched to (#22) */
    uint8_t mono_note;           /* note the MONO / LEGATO / UNISON voice(s) play, 0 = none */
    uint8_t rr;                  /* POLY ROTATE: next voice to try */
    uint8_t rsv_sz[9];          /* (sizeof(track_t) a multiple of 16: below) */
} track_t;
/* the audio ISR indexes trk[] with a multiply by a small immediate and a shift while the size is a multiple of 16 */
_Static_assert(sizeof(track_t) % 16u == 0, "track_t: a multiple of 16 bytes (a cheap trk[] index on the target)");

typedef struct {
    int16_t g[G_COUNT];
    int8_t octave;               /* OCT- / OCT+ */
    uint32_t cpu_q8;             /* audio ISR load, 1/256 */
    uint32_t master_q12;
    int32_t batt_raw;            /* smoothed ADC ch3 (battery divider), 0 = not read yet */
} song_t;

static track_t trk[NTRK];        /* the instrument */
static song_t song;
#define TSEL (&trk[0])

/* ----------------------------------------------------------- system --- */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")   /* slot store before the index update */
static volatile uint32_t fm1_ms;  /* milliseconds since boot (TIMER4-based, TIMER5 ISR in main.c) */
/* boot-loop guard (main.c): two boots in a row that die in the first 30 s -> UBOOT */
#define BOOTGUARD_MAGIC 0x42475244u
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));
