/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Regression suite of the KOSMO DSP on the Mac (same sources as the firmware, through hostsim.c).
 *   build/host/regress [GOLDEN_FILE CPU_FILE]      (run_tests.sh builds and runs it)
 *
 * 1. golden renders: every test sound (hostsim.c HOST_SOUNDS: the WAVE sounds), each WAVE, the filter TYPEs
 *    (LP / BP / HP / NOTCH), the voice modes (POLY / MONO / LEGATO / UNISON), the ROOM reverb (send, SIZE, DAMP), the keys with
 *    OCT-/OCT+, and MIDI (notes, bend, sustain). Each render plays a fixed phrase (notes, an overlap, a chord, note-offs,
 *    the release tail) and is reduced to a 64-bit FNV-1a hash of its output samples, compared with GOLDEN_FILE
 *    (tests/golden.txt). Every render runs in its own fork()ed child of a process that never touched the DSP state, so all
 *    state starts as at boot: the noise / unison-phase seeds, the reverb buffers, the limiter.
 * 2. health checks on every render: no sample beyond 16 bits, none near full scale, DC offset, the peak level, every
 *    voice free after the release, silence at the end. Fails with the numbers.
 * 3. CPU budget: per test sound (8 notes held) and a few mixes, the instructions per sample counted by the kernel
 *    (proc_pid_rusage ri_instructions: the same on every run within ~1 %, unlike wall time), compared with CPU_FILE
 *    (tests/cpu_baseline.txt) at +-25 %; ns per sample printed for information. A count over the budget fails, one far
 *    under it only warns.
 * 4. voices: the budget of 8, MONO / LEGATO / UNISON changes keep a lead and reclaim the rest, key-ups release after a
 *    live voice mode change, and no hanging note after random MIDI (notes, sustain, bend) and keys.
 * env: GOLDEN_UPDATE=1 rewrites GOLDEN_FILE, BUDGET_UPDATE=1 rewrites CPU_FILE (on purpose: review the
 * diff), VERBOSE=1 prints every render's numbers, JOBS=n children at once (default 8). */
#define main hostsim_main
#include "hostsim.c"
#undef main
#ifdef __APPLE__
#include <libproc.h>
#include <sys/resource.h>
#endif

/* ------------------------------------------------------------- limits --- */
#define LIM_NEAR 30000            /* |sample| at or above: "near full scale" (the limiter holds ~18000) */
#define LIM_DC 400                /* |mean| of the sounding part, 16-bit units (1.2 %; the master blocks DC) */
#define LIM_PEAK_MIN 250          /* quieter than this (-42 dBFS): something is missing */
#define LIM_TAIL 6                /* AC peak of the last 0.25 s, after the voices are free and the FX rang out */
#define LIM_TAIL_DC 2             /* its constant part: the master DC blocker settles to 0 (error feedback; it
                                   * used to leave up to +-31), what is left is the midpoint of the AC tail */
#define FREE_CAP_S 12             /* the voices must be free this long after the last note-off */
#ifndef TAIL_S
#define TAIL_S 3                  /* FX tail rendered after the voices are free */
#endif
#define CPU_TOL 0.25              /* +-25 %, and at least CPU_SLACK instructions (light presets) */
#define CPU_SLACK 20

/* ------------------------------------------------------------ results --- */
typedef struct {
    uint64_t hash;
    uint32_t frames, over, near, vmax, ok;     /* ok: a scenario's own checks passed */
    int32_t peak, tail_peak, tail_dc, tmin, tmax;
    double dc, free_s;                         /* free_s: s from the last note-off until all voices free, -1 never */
    double ipc, ns;                            /* CPU: instructions and ns per sample */
    char msg[512];
} res_t;

static res_t R;
static uint32_t fpos, rel_at, dc_end, tail_from;
static int64_t dcs[2];

static int parts_free(void)
{
    uint32_t p, i;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++)
            if (trk[p].v[i].active)
                return 0;
    return 1;
}

/* one block of the mix into the hash and the numbers */
static int32_t last_out[2 * CTL];
static void blk(void)
{
    int32_t *o = last_out;
    uint32_t i, k;
    mix_block(o, CTL);
    for (i = 0; i < 2u * CTL; i++) {
        int32_t x = o[i], a = x < 0 ? -x : x;
        uint32_t u = (uint32_t)x;
        for (k = 0; k < 4u; k++) {
            R.hash ^= (u >> (8u * k)) & 0xFFu;
            R.hash *= 0x100000001B3ull;
        }
        if (a > R.peak)
            R.peak = a;
        R.over += a > 32767;
        R.near += a >= LIM_NEAR;
        if (fpos < dc_end)
            dcs[i & 1u] += x;
        if (tail_from && fpos >= tail_from) {
            R.tmin = x < R.tmin ? x : R.tmin;
            R.tmax = x > R.tmax ? x : R.tmax;
        }
    }
    k = voices_busy();
    if (k > R.vmax)
        R.vmax = k;
    fpos += CTL;
}

static uint32_t at(double s) { return (uint32_t)(s * FS) / CTL * CTL; }
static void run_to(uint32_t f) { while (fpos < f) blk(); }

/* after the last note-off (rel_at): until the voices are free, then the FX tail */
static void finish(void)
{
    uint32_t cap = rel_at + FREE_CAP_S * FS;
    dc_end = dc_end ? dc_end : fpos;
    while (!parts_free() && fpos < cap)
        blk();
    R.free_s = parts_free() ? (double)(fpos - rel_at) / FS : -1;
    tail_from = fpos + TAIL_S * FS - FS / 4u;
    R.tmin = 0x7FFFFFFF;
    R.tmax = -0x7FFFFFFF;
    run_to(fpos + TAIL_S * FS);
    R.tail_peak = (R.tmax - R.tmin + 1) / 2;
    R.tail_dc = (R.tmax + R.tmin) / 2;
    R.frames = fpos;
    R.dc = (double)(llabs(dcs[0]) > llabs(dcs[1]) ? dcs[0] : dcs[1]) / (double)(dc_end ? dc_end : 1);
}

/* --------------------------------------------------------- the phrase --- */
/* notes (with an overlap: legato / steal), a 5-note chord, note-offs, the tail. Through trk_note_on / trk_note_off
 * (what the keys and MIDI end up calling). */
static void phrase(track_t *t, uint32_t base)
{
    static const uint8_t CH[5] = {0, 7, 12, 16, 19};
    uint32_t i;
    trk_note_on(t, base, 100);
    run_to(at(0.30));
    trk_note_off(t, base);
    run_to(at(0.35));
    trk_note_on(t, base + 4u, 80);
    run_to(at(0.60));
    trk_note_on(t, base + 7u, 120);            /* overlaps: MONO / LEGATO hand over, POLY two voices */
    run_to(at(0.70));
    trk_note_off(t, base + 4u);
    run_to(at(0.95));
    trk_note_off(t, base + 7u);
    run_to(at(1.00));
    for (i = 0; i < 5u; i++)
        trk_note_on(t, base - 12u + CH[i], 90 + 5 * i);
    run_to(at(1.80));
    for (i = 0; i < 5u; i++)
        trk_note_off(t, base - 12u + CH[i]);
    rel_at = fpos;
    dc_end = fpos + FS / 2u;
    finish();
}

static void midi_pkt(uint32_t st, uint32_t d1, uint32_t d2)   /* as usb.c: the queue holds MQ packets */
{
    if (mi_w - mi_r >= MQ)
        blk();
    midi_in_q[mi_w++ % MQ] = (st >> 4) | st << 8 | d1 << 16 | d2 << 24;
}

/* ------------------------------------------------------------- jobs --- */
enum { J_SOUND, J_WAVE, J_FILTER, J_MODE, J_REVERB, J_KEYS, J_MIDI, J_CPU, J_CHECK };
typedef struct {
    char name[64];
    uint8_t kind, pi, arg, cpu_notes, own;     /* own: J_CPU is a sound (its cost less the idle mix's), not a mix */
    int (*check)(char *msg, uint32_t n);       /* J_CHECK */
    res_t r;
    int crashed;
} job_t;

static void job_sound(const job_t *j)
{
    host_tracks_init();
    host_sound(&trk[0], j->pi);
    phrase(&trk[0], 60);
}

static void job_wave(const job_t *j)            /* arg: WAVE 0..7 on the default sound */
{
    host_tracks_init();
    trk[0].p[P_E0] = j->arg;
    phrase(&trk[0], 60);
}

static void job_filter(const job_t *j)          /* arg: the filter TYPE, LP BP HP NOTCH, on the test sound pi */
{
    host_tracks_init();
    host_sound(&trk[0], j->pi);
    trk[0].p[P_FTYPE] = j->arg;
    phrase(&trk[0], 60);
}

static void job_mode(const job_t *j)            /* arg: the voice mode, glide on for MONO / LEGATO */
{
    track_t *t = &trk[0];
    host_tracks_init();
    host_sound(t, j->pi);
    t->p[P_VOICE] = j->arg;
    t->p[P_GLIDE] = j->arg == V_MONO || j->arg == V_LEGATO ? 30 : 0;
    t->p[P_DETUNE] = 40;
    phrase(t, 60);
}

static void job_reverb(const job_t *j)          /* arg: the send, SIZE and DAMP */
{
    static const int16_t S[5][3] = {{0, 90, 60}, {40, 90, 60}, {127, 90, 60}, {80, 95, 20}, {80, 0, 127}};
    host_tracks_init();
    host_sound(&trk[0], j->pi);
    trk[0].p[P_REV] = S[j->arg][0];
    song.g[G_RSIZE] = S[j->arg][1];
    song.g[G_RDAMP] = S[j->arg][2];
    phrase(&trk[0], 60);
}

/* the keys (input.c): a phrase on the 27 keys, the octave buttons changing the octave under held keys */
static void job_keys(const job_t *j)
{
    uint32_t k;
    (void)j;
    host_tracks_init();
    host_sound(&trk[0], 0);
    for (k = 0; k < 8u; k++) {
        fm1_in.notes = 1u << (7u + 2u * k % 12u);
        if (k == 4u)
            song.octave = 1;
        run_to(at(0.15 * (k + 1)));
        fm1_in.notes = 0;
        run_to(at(0.15 * (k + 1) + 0.05));
    }
    fm1_in.notes = 1u << 7 | 1u << 11 | 1u << 14;     /* a chord, then the octave down while it is held */
    run_to(fpos + FS / 2u);
    song.octave = -1;
    fm1_in.notes = 1u << 7 | 1u << 11 | 1u << 14 | 1u << 3;
    run_to(fpos + FS / 4u);
    fm1_in.notes = 0;
    rel_at = fpos;
    dc_end = fpos;
    finish();
}

/* MIDI: notes on two channels, the bend up and down, the sustain pedal holding released notes, ALL NOTES OFF */
static void job_midi(const job_t *j)
{
    uint32_t b;
    (void)j;
    host_tracks_init();
    host_sound(&trk[0], 1);
    midi_pkt(0x90, 60, 100);
    midi_pkt(0x91, 64, 90);
    run_to(at(0.3));
    for (b = 0; b < 16u; b++) {                        /* up to the top and back */
        uint32_t v = b < 8u ? 8192u + b * 1023u : 16383u - (b - 8u) * 1023u;
        midi_pkt(0xE0, v & 127u, v >> 7);
        run_to(fpos + at(0.05));
    }
    midi_pkt(0xE0, 0, 64);
    midi_pkt(0xB0, 64, 127);                          /* sustain down */
    midi_pkt(0x80, 60, 0);
    midi_pkt(0x80, 64, 0);
    midi_pkt(0x90, 67, 100);
    run_to(fpos + at(0.5));
    midi_pkt(0xB0, 64, 0);                            /* up: 60 and 64 go, 67 is held */
    run_to(fpos + at(0.3));
    midi_pkt(0xB0, 123, 0);
    rel_at = fpos;
    dc_end = fpos;
    finish();
}

/* CPU: the test sound pi, 8 notes held (POLY, SUS 127); 0.5 s to settle, then 1 s counted. cpu_notes 0: the idle mix */
static uint64_t instr_now(void)
{
#ifdef __APPLE__
    struct rusage_info_v4 ri;
    if (!proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri))
        return ri.ri_instructions;
#endif
    return 0;
}
static void job_cpu(const job_t *j)
{
    static const uint8_t NOTES[8] = {48, 52, 55, 59, 60, 64, 67, 71};
    uint32_t i, k, nb = FS / CTL;
    uint64_t i0 = 0, t0 = 0;
    host_tracks_init();
    host_sound(&trk[0], j->pi);
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_SUS] = 127;
    for (i = 0; i < j->cpu_notes; i++)
        trk_note_on(&trk[0], NOTES[i], 100);
    for (k = 0; k < nb / 2u + nb; k++) {
        if (k == nb / 2u) {
            i0 = instr_now();
            t0 = now_ns();
        }
        mix_block(last_out, CTL);
    }
    R.ns = (double)(now_ns() - t0) / (nb * CTL);
    R.ipc = i0 ? (double)(instr_now() - i0) / (nb * CTL) : 0;
}

static int run_job_body(job_t *j)
{
    memset(&R, 0, sizeof R);
    switch (j->kind) {
    case J_SOUND:
        job_sound(j);
        break;
    case J_WAVE:
        job_wave(j);
        break;
    case J_FILTER:
        job_filter(j);
        break;
    case J_MODE:
        job_mode(j);
        break;
    case J_REVERB:
        job_reverb(j);
        break;
    case J_KEYS:
        job_keys(j);
        break;
    case J_MIDI:
        job_midi(j);
        break;
    case J_CPU:
        job_cpu(j);
        break;
    case J_CHECK:
        R.ok = (uint32_t)j->check(R.msg, sizeof R.msg);
        break;
    }
    return 0;
}

/* run jobs [0, n) in children, up to nj at once; results come back through pipes */
static void run_jobs(job_t *jobs, uint32_t n, uint32_t nj)
{
    uint32_t i, k;
    for (i = 0; i < n; i += nj) {
        int fd[64];
        pid_t pid[64];
        uint32_t m = n - i < nj ? n - i : nj;
        for (k = 0; k < m; k++) {
            int p[2];
            if (pipe(p)) {
                perror("pipe");
                exit(2);
            }
            fflush(stdout);
            if (!(pid[k] = fork())) {
                close(p[0]);
                run_job_body(&jobs[i + k]);
                if (write(p[1], &R, sizeof R) != sizeof R)
                    _exit(1);
                _exit(0);
            }
            close(p[1]);
            fd[k] = p[0];
        }
        for (k = 0; k < m; k++) {
            size_t got = 0;
            ssize_t r;
            int st = 0;
            while (got < sizeof(res_t) && (r = read(fd[k], (char *)&jobs[i + k].r + got, sizeof(res_t) - got)) > 0)
                got += (size_t)r;
            close(fd[k]);
            waitpid(pid[k], &st, 0);
            jobs[i + k].crashed = got != sizeof(res_t) || !WIFEXITED(st) || WEXITSTATUS(st);
        }
    }
}

/* ------------------------------------------------- voice / routing checks --- */
static uint32_t lcg = 12345u;
static uint32_t rnd(uint32_t n)
{
    lcg = lcg * 1103515245u + 12345u;
    return (lcg >> 8) % n;
}

/* the budget: random notes on and off for 6 s, up to 8 held and more asked; after every block at most 8 voices are
 * active and none is still fading; then all off: every voice free */
static int chk_budget(char *msg, uint32_t n)
{
    uint8_t held[128] = {0};
    uint32_t k, i, worst = 0, fading = 0, nheld = 0;
    track_t *t = &trk[0];
    host_tracks_init();
    host_sound(t, 1);
    t->p[P_VOICE] = V_POLY;
    t->p[P_SUS] = 100;
    for (k = 0; k < 6u * FS / CTL; k++) {
        uint32_t a = 0;
        if (k % 4u == 0u) {
            uint32_t note = 36u + rnd(48);
            if (held[note]) {
                trk_note_off(t, note);
                held[note] = 0;
            } else {
                trk_note_on(t, note, 60u + rnd(60));
                held[note] = 1;
            }
            nheld = 0;
            for (i = 0; i < 128u; i++)
                nheld += held[i];
        }
        blk();
        for (i = 0; i < NVOICE; i++) {
            a += t->v[i].active;
            fading += t->v[i].active && t->v[i].stage == 4u;
        }
        worst = a > worst ? a : worst;
    }
    for (k = 0; k < 128u; k++)
        if (held[k])
            trk_note_off(t, k);
    rel_at = fpos;
    finish();
    snprintf(msg, n, "POLY, random notes: at most %u voices active (budget %u), %u still fading after their block, all "
             "free %.2f s after the note-offs", worst, NVOICE, fading, R.free_s);
    return worst <= NVOICE && !fading && R.free_s >= 0;
}

/* ---- WAVE / filter / reverb checks (issues #3 #4 #5) ---- */
/* blocks of the mix: the largest sample-to-sample step and the sum of the steps of the left channel */
static int32_t prev_l;
static double lvl_sum;          /* sum of |sample|: the frames differ in level or in edges, not always in both */
static void steps(uint32_t blocks, int32_t *maxd, double *sum)
{
    uint32_t b, i;
    for (b = 0; b < blocks; b++) {
        blk();
        for (i = 0; i < CTL; i++) {
            int32_t d = last_out[2u * i] - prev_l;
            d = d < 0 ? -d : d;
            prev_l = last_out[2u * i];
            *maxd = d > *maxd ? d : *maxd;
            *sum += d;
            lvl_sum += llabs(last_out[2u * i]);
        }
    }
}

/* SCAN swept 0..127 in one second on a held note, every table: no click (bounded step), and the frames differ */
static int chk_scan(char *msg, uint32_t n)
{
    uint32_t tb, k, bad = 0, nb = FS / CTL;
    int32_t worst = 0;
    track_t *t = &trk[0];
    for (tb = 0; tb < 8u; tb++) {
        int32_t md = 0, md2 = 0, ref;
        double s0 = 0, s1 = 0, l0, l1;
        host_tracks_init();
        t->p[P_E0] = (int16_t)tb;
        t->p[P_E2] = 127;
        t->p[P_E3] = 0;
        t->p[P_SUS] = 127;
        t->p[P_REV] = 0;
        trk_note_on(t, 48, 100);
        run_to(fpos + FS / 4u);
        lvl_sum = 0;
        steps(FS / 10u / CTL, &md, &s0);
        l0 = lvl_sum;
        ref = md;
        md = 0;
        for (k = 0; k < nb; k++) {
            t->p[P_E1] = (int16_t)(k * 127u / (nb - 1u));
            steps(1, &md, &s1);
        }
        s1 = 0;
        lvl_sum = 0;
        steps(FS / 10u / CTL, &md2, &s1);
        l1 = lvl_sum;
        ref = md2 > ref ? md2 : ref;
        worst = md > worst ? md : worst;
        if (md > ref * 2 || (s1 < s0 * 1.05 && s0 < s1 * 1.05 && l1 < l0 * 1.05 && l0 < l1 * 1.05))
            fprintf(stderr, "scan table %u: sweep step %d, steady steps %d, steps %.0f -> %.0f, level %.0f -> %.0f\n", tb, md, ref, s0, s1, l0, l1);
        bad += md > ref * 2 || (s1 < s0 * 1.05 && s0 < s1 * 1.05 && l1 < l0 * 1.05 && l0 < l1 * 1.05);
        trk_note_off(t, 48);
        run_to(fpos + FS / 2u);
    }
    snprintf(msg, n, "SCAN sweep, 8 tables: largest step %d (limit 2x the steady frames' own), %u tables with a click or without a change",
             worst, bad);
    return !bad;
}



/* Voices held before a mode change must remain reclaimable except for the lead. */
static int chk_mode_budget(char *msg, uint32_t n)
{
    uint32_t mode, initial, i, bad = 0;
    for (initial = 0; initial < 2u; initial++)
        for (mode = V_MONO; mode <= V_UNISON; mode++) {
            track_t *t = &trk[0];
            host_tracks_init();
            memset(t->v, 0, sizeof t->v);
            host_sound(t, 0);
            t->p[P_VOICE] = initial ? V_UNISON : V_POLY;
            if (initial)
                trk_note_on(t, 48, 100);
            else
                for (i = 0; i < NVOICE; i++) trk_note_on(t, 48u + 2u * i, 100);
            bad += voices_busy() != NVOICE;
            t->p[P_VOICE] = (int16_t)mode;
            trk_note_on(t, 72, 100);
            bad += voices_busy() > NVOICE || !t->v[0].gate || t->v[0].note != 72u;
            blk();
            bad += voices_busy() > NVOICE;
            trk_all_off(t);
        }
    snprintf(msg, n, "POLY / UNISON chord -> MONO / LEGATO / UNISON: the new note leads, budget %u kept", NVOICE);
    return !bad;
}

static void mode_release_reset(uint32_t mode)
{
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    host_tracks_init();
    host_sound(&trk[0], 0);
    trk[0].p[P_VOICE] = (int16_t)mode;
    trk[0].p[P_REL] = 0;
    trk[0].p[P_SUS] = 127;
}

static uint32_t mode_release_gates(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++) n += trk[0].v[i].gate != 0;
    return n;
}

/* A key-up must release its voices even after a live VCE change. */
static int chk_mode_release(char *msg, uint32_t n)
{
    uint32_t from, to, order, i, mode, priority, bad = 0, cases = 0;
    for (from = V_POLY; from <= V_UNISON; from++)
        for (to = V_POLY; to <= V_UNISON; to++)
            for (order = 0; order < 2u; order++) {
                mode_release_reset(from);
                trk_note_on(&trk[0], 60, 100);
                trk_note_on(&trk[0], 64, 100);
                for (i = 0; i < 8u; i++) blk();
                trk[0].p[P_VOICE] = (int16_t)to;
                trk_note_off(&trk[0], order ? 60u : 64u);
                trk_note_off(&trk[0], order ? 64u : 60u);
                bad += mode_release_gates() != 0 || trk[0].nmono != 0 || voices_busy() > NVOICE;
                for (i = 0; i < FS / CTL && !parts_free(); i++) blk();
                bad += !parts_free();
                cases++;
            }
    for (mode = V_MONO; mode <= V_UNISON; mode++) {
        mode_release_reset(mode);
        trk_note_on(&trk[0], 60, 100);
        trk_note_on(&trk[0], 64, 100);
        trk[0].p[P_VOICE] = V_POLY;
        trk_note_off(&trk[0], 64);
        trk[0].p[P_VOICE] = (int16_t)mode;
        trk_note_off(&trk[0], 60);
        trk_note_on(&trk[0], 72, 100);
        trk_note_off(&trk[0], 72);
        bad += mode_release_gates() != 0 || trk[0].nmono != 0;
        cases++;
        for (priority = 0; priority < 3u; priority++) {
            uint32_t lead = priority == 1u ? 72u : 80u, other = priority == 1u ? 80u : 72u;
            uint32_t want = mode == V_UNISON ? NVOICE : 1u;
            mode_release_reset(V_POLY);
            trk_note_on(&trk[0], 48, 100);
            trk_note_on(&trk[0], 64, 100);
            trk[0].p[P_VOICE] = (int16_t)mode;
            trk[0].p[P_PRIO] = (int16_t)priority;
            trk_note_on(&trk[0], 72, 100);
            trk_note_on(&trk[0], 76, 100);
            trk_note_on(&trk[0], 80, 100);
            trk_note_off(&trk[0], 48);
            trk_note_off(&trk[0], 64);
            trk_note_off(&trk[0], other);
            bad += mode_release_gates() != want || trk[0].v[0].note != lead || trk[0].mono_note != lead;
            trk_note_off(&trk[0], lead);
            bad += mode_release_gates() != want || trk[0].v[0].note != 76u || trk[0].mono_note != 76u;
            trk_note_off(&trk[0], 76);
            bad += mode_release_gates() != 0 || trk[0].nmono != 0 || voices_busy() > NVOICE;
            cases++;
        }
    }
    snprintf(msg, n, "%u cases: all 16 VCE pairs and both key-up orders; no stale fallback, MONO / LEGATO / UNISON "
             "priority leads and fallback preserved", cases);
    return !bad;
}

/* no hanging notes: 6 s of random MIDI (note-ons / offs on every channel, the sustain pedal, bend, ALL NOTES OFF) and
 * keys with the octave changing; then every held note off and the pedal up: no gate may stay on, every voice must be
 * free */
static int chk_hang(char *msg, uint32_t n)
{
    uint8_t on[16][128] = {{0}};
    uint32_t keys = 0, k, c, ev = 0, round;
    char who[96] = "";
    int bad = 0;
    for (round = 0; round < 2u && !bad; round++) {
        host_tracks_init();
        for (k = 0; k < 6u * FS / CTL; k++) {
            uint32_t r = rnd(20);
            if (r < 6u) {                          /* MIDI */
                uint32_t ch = rnd(16), note = 36u + rnd(36);
                if (on[ch][note]) {
                    midi_pkt(rnd(2) ? 0x80u | ch : 0x90u | ch, note, 0);
                    on[ch][note] = 0;
                } else {
                    midi_pkt(0x90u | ch, note, 1u + rnd(127));
                    on[ch][note] = 1;
                }
                ev++;
            } else if (r < 9u) {                   /* a key down or up */
                keys ^= 1u << rnd(27);
                fm1_in.notes = keys;
                ev++;
            } else if (r == 9u) {
                midi_pkt(0xB0, 64, rnd(2) ? 127u : 0u);     /* the pedal */
            } else if (r == 10u) {
                song.octave = (int8_t)((int32_t)rnd(7) - 3);  /* OCT- / OCT+ with keys held */
            } else if (r == 11u) {
                midi_pkt(0xE0, rnd(128), rnd(128));
            } else if (r == 12u && round && !rnd(20)) {
                midi_pkt(0xB0, 123, 0);                     /* ALL NOTES OFF */
                memset(on, 0, sizeof on);
            }
            blk();
        }
        fm1_in.notes = keys = 0;                   /* everything off */
        for (c = 0; c < 16u; c++)
            for (k = 0; k < 128u; k++)
                if (on[c][k]) {
                    midi_pkt(0x80u | c, k, 64);
                    on[c][k] = 0;
                }
        midi_pkt(0xB0, 64, 0);                     /* pedal up */
        run_to(fpos + FS / 2u);
        for (k = 0; k < NVOICE; k++)
            if (trk[0].v[k].active && trk[0].v[k].gate) {
                bad = 1;
                snprintf(who, sizeof who, "voice %u note %u gate still on", k, trk[0].v[k].note);
                break;
            }
        rel_at = fpos;
        {
            uint32_t cap = fpos + FREE_CAP_S * FS;
            while (!parts_free() && fpos < cap)
                blk();
            for (c = 0; c < NVOICE && !parts_free() && !bad; c++) {
                const voice_t *v = &trk[0].v[c];
                if (v->active) {
                    bad = 2;
                    snprintf(who, sizeof who, "voice %u note %u still active %d s after the note-offs (gate %u "
                             "stage %u)", c, v->note, FREE_CAP_S, v->gate, v->stage);
                }
            }
        }
    }
    snprintf(msg, n, "%u random MIDI / key events (every channel, pedal, bend, ALL NOTES OFF, octave changes), "
             "2 rounds: %s", ev, bad ? who : "no gate left on, every voice free");
    return !bad;
}

/* ------------------------------------------------------------ files --- */
typedef struct { char name[64]; char val[64]; } kv_t;
static uint32_t load_kv(const char *path, kv_t *kv, uint32_t max)
{
    FILE *f = fopen(path, "r");
    char line[256];
    uint32_t n = 0;
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f) && n < max) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%63s %63s", kv[n].name, kv[n].val) == 2)
            n++;
    }
    fclose(f);
    return n;
}
static const char *kv_get(const kv_t *kv, uint32_t n, const char *name)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (!strcmp(kv[i].name, name))
            return kv[i].val;
    return 0;
}

static void slug(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; *s && i + 1 < n; s++)
        d[i++] = (*s == ' ' || *s == '/') ? '_' : *s;
    d[i] = 0;
}

/* ------------------------------------------------------------- main --- */
#define MAXJ 128
static job_t J[MAXJ];
static uint32_t nj;
static job_t *add(uint32_t kind, const char *name)
{
    job_t *j = &J[nj++];
    memset(j, 0, sizeof *j);
    j->kind = (uint8_t)kind;
    snprintf(j->name, sizeof j->name, "%s", name);
    return j;
}

int main(int argc, char **argv)
{
    const char *gpath = argc > 1 ? argv[1] : "tests/golden.txt";
    const char *cpath = argc > 2 ? argv[2] : "tests/cpu_baseline.txt";
    int gupd = getenv("GOLDEN_UPDATE") != 0, cupd = getenv("BUDGET_UPDATE") != 0, verbose = getenv("VERBOSE") != 0;
    uint32_t jobs_at_once = getenv("JOBS") ? (uint32_t)atoi(getenv("JOBS")) : 8u;
    static const char *const MN[4] = {"POLY", "MONO", "LEGATO", "UNISON"};
    static const char *const FN[4] = {"LP", "BP", "HP", "NOTCH"};
    static const char *const RN[5] = {"dry", "send40", "send127", "big_bright", "small_dark"};
    static const uint8_t MODE_S[3] = {0, 1, 3}, REV_S[2] = {1, 2}, FILT_S[3] = {0, 3, 5};
    static kv_t gold[MAXJ], cpu[MAXJ];
    uint32_t ng, nc, pi, i, g0, g1, c0, c1, k0, heavy_i = 0;
    uint32_t g_changed = 0, g_new = 0, g_gone = 0, h_fail = 0, c_fail = 0, c_warn = 0, k_fail = 0, crash = 0;
    double heavy = 0, idle_now, idle_base;
    uint64_t t_start = now_ns();
    char name[64], s[64];
    if (jobs_at_once < 1u || jobs_at_once > 64u)
        jobs_at_once = 8;

    /* 1 + 2: the golden renders */
    g0 = nj;
    for (pi = 0; pi < NHOST_SOUNDS; pi++) {
        job_t *j;
        slug(s, HOST_SOUNDS[pi].name, sizeof s);
        snprintf(name, sizeof name, "sound/%02u_%s", pi, s);
        j = add(J_SOUND, name);
        j->pi = (uint8_t)pi;
    }
    for (i = 0; i < 8u; i++) {
        snprintf(name, sizeof name, "wave/%s", WAVE_NAMES[i]);
        add(J_WAVE, name)->arg = (uint8_t)i;
    }
    for (i = 0; i < 3u; i++)
        for (k0 = 0; k0 < 4u; k0++) {
            job_t *j;
            slug(s, HOST_SOUNDS[FILT_S[i]].name, sizeof s);
            snprintf(name, sizeof name, "filter/%s/%s", s, FN[k0]);
            j = add(J_FILTER, name);
            j->pi = FILT_S[i];
            j->arg = (uint8_t)k0;
        }
    for (i = 0; i < 3u; i++)
        for (k0 = 0; k0 < 4u; k0++) {
            job_t *j;
            slug(s, HOST_SOUNDS[MODE_S[i]].name, sizeof s);
            snprintf(name, sizeof name, "mode/%s/%s", s, MN[k0]);
            j = add(J_MODE, name);
            j->pi = MODE_S[i];
            j->arg = (uint8_t)k0;
        }
    for (i = 0; i < 2u; i++)
        for (k0 = 0; k0 < 5u; k0++) {
            job_t *j;
            slug(s, HOST_SOUNDS[REV_S[i]].name, sizeof s);
            snprintf(name, sizeof name, "reverb/%s/%s", s, RN[k0]);
            j = add(J_REVERB, name);
            j->pi = REV_S[i];
            j->arg = (uint8_t)k0;
        }
    add(J_KEYS, "keys/octave");
    add(J_MIDI, "midi/notes_bend_sustain");
    g1 = nj;
    {   /* determinism: the first sound's render once more */
        job_t *j = add(J_SOUND, "repeat");
        *j = J[g0];
        snprintf(j->name, sizeof j->name, "repeat");
    }

    /* 4: the voice checks */
    k0 = nj;
    add(J_CHECK, "voices: budget of 8")->check = chk_budget;
    add(J_CHECK, "voices: budget after a live mode change")->check = chk_mode_budget;
    add(J_CHECK, "voices: note-offs after live voice mode changes")->check = chk_mode_release;
    add(J_CHECK, "routing: no hanging notes")->check = chk_hang;
    add(J_CHECK, "wave: SCAN sweep without clicks")->check = chk_scan;
    run_jobs(J, nj, jobs_at_once);

    /* 3: CPU, one child at a time (the ns are for information; the instruction counts do not care) */
    c0 = nj;
    for (pi = 0; pi < NHOST_SOUNDS; pi++) {
        job_t *j;
        slug(s, HOST_SOUNDS[pi].name, sizeof s);
        snprintf(name, sizeof name, "cpu/%02u_%s", pi, s);
        j = add(J_CPU, name);
        j->pi = (uint8_t)pi;
        j->cpu_notes = 8;
        j->own = 1;
    }
    add(J_CPU, "cpu/mix/idle");                        /* no notes: the reverb and the master (subtracted from the sounds' counts) */
    c1 = nj;
    run_jobs(J + c0, c1 - c0, 1);

    /* ---- report: goldens and health ---- */
    ng = load_kv(gpath, gold, MAXJ);
    for (i = g0; i < g1; i++) {
        job_t *j = &J[i];
        const res_t *r = &j->r;
        const char *want = kv_get(gold, ng, j->name);
        char hs[24], why[256] = "";
        snprintf(hs, sizeof hs, "%016llx", (unsigned long long)r->hash);
        if (j->crashed) {
            printf("regress: CRASH  %s\n", j->name);
            crash++;
            continue;
        }
        if (!want)
            g_new++;
        else if (strcmp(want, hs)) {
            if (!gupd)
                printf("regress: GOLDEN CHANGED  %s  (%s -> %s)\n", j->name, want, hs);
            g_changed++;
        }
        if (r->over)
            snprintf(why + strlen(why), sizeof why - strlen(why), " %u samples beyond 16 bits;", r->over);
        if (r->near)
            snprintf(why + strlen(why), sizeof why - strlen(why), " %u samples >= %d;", r->near, LIM_NEAR);
        if (fabs(r->dc) > LIM_DC)
            snprintf(why + strlen(why), sizeof why - strlen(why), " DC %.1f (limit %d);", r->dc, LIM_DC);
        if (r->peak < LIM_PEAK_MIN)
            snprintf(why + strlen(why), sizeof why - strlen(why), " peak %d (min %d);", r->peak, LIM_PEAK_MIN);
        if (r->free_s < 0)
            snprintf(why + strlen(why), sizeof why - strlen(why), " voices not free %d s after the note-offs;", FREE_CAP_S);
        if (abs(r->tail_dc) > LIM_TAIL_DC)
            snprintf(why + strlen(why), sizeof why - strlen(why), " DC %d left at the end (limit %d);", r->tail_dc,
                     LIM_TAIL_DC);
        if (r->tail_peak > LIM_TAIL)
            snprintf(why + strlen(why), sizeof why - strlen(why), " not silent at the end: peak %d (limit %d);",
                     r->tail_peak, LIM_TAIL);
        if (r->vmax > NVOICE)
            snprintf(why + strlen(why), sizeof why - strlen(why), " %u voices (budget %u);", r->vmax, NVOICE);
        if (why[0]) {
            printf("regress: HEALTH FAIL  %s:%s\n", j->name, why);
            h_fail++;
        }
        if (verbose)
            printf("  %-44s %s  %5.1f s  peak %5d  dc %6.1f  free %5.2f s  tail %3d dc %3d  voices %u\n", j->name,
                   hs, (double)r->frames / FS, r->peak, r->dc, r->free_s, r->tail_peak, r->tail_dc, r->vmax);
    }
    for (i = 0; i < ng; i++) {                      /* in the file, not rendered any more */
        uint32_t k, found = 0;
        for (k = g0; k < g1 && !found; k++)
            found = !strcmp(J[k].name, gold[i].name);
        if (!found) {
            if (!gupd)
                printf("regress: GOLDEN GONE  %s (no such render now)\n", gold[i].name);
            g_gone++;
        }
    }
    if (J[g1].r.hash != J[g0].r.hash || J[g1].crashed) {
        printf("regress: NOT DETERMINISTIC  %s rendered twice: %016llx, %016llx\n", J[g0].name,
               (unsigned long long)J[g0].r.hash, (unsigned long long)J[g1].r.hash);
        h_fail++;
    }
    if (gupd) {
        FILE *f = fopen(gpath, "w");
        if (!f) {
            perror(gpath);
            return 2;
        }
        fprintf(f, "# KOSMO golden renders (tests/regress.c): name, FNV-1a 64 of the output samples.\n"
                   "# Rewritten by GOLDEN_UPDATE=1 tests/run_tests.sh -- only for an intended change of the sound.\n");
        for (i = g0; i < g1; i++)
            fprintf(f, "%s %016llx\n", J[i].name, (unsigned long long)J[i].r.hash);
        fclose(f);
        printf("regress: golden file %s rewritten (%u renders; %u had changed, %u new, %u gone)\n", gpath, g1 - g0,
               g_changed, g_new, g_gone);
    } else if (g_new) {
        printf("regress: %u renders not in %s (new: GOLDEN_UPDATE=1 adds them)\n", g_new, gpath);
    }

    /* ---- voices ---- */
    for (i = k0; i < c0; i++) {
        int ok = !J[i].crashed && J[i].r.ok;
        printf("regress: %s: %s %s\n", J[i].name, J[i].crashed ? "crashed" : J[i].r.msg, ok ? "ok" : "FAIL");
        k_fail += !ok;
    }

    /* ---- CPU ---- */
    /* a preset's own cost: its count less the idle mix's (the mix alone is half of a light preset's),
     * so +25 % means 25 % more engine work; the mixes as they are */
    nc = load_kv(cpath, cpu, MAXJ);
    idle_now = J[c1 - 1u].r.ipc;
    idle_base = kv_get(cpu, nc, J[c1 - 1u].name) ? atof(kv_get(cpu, nc, J[c1 - 1u].name)) : 0;
    for (i = c0; i < c1; i++) {
        const job_t *j = &J[i];
        const char *want = kv_get(cpu, nc, j->name);
        int own = j->own && idle_base > 0;
        double b = want ? atof(want) - (own ? idle_base : 0) : 0, ipc = j->r.ipc - (own ? idle_now : 0);
        if (j->crashed) {
            printf("regress: CRASH  %s\n", j->name);
            crash++;
            continue;
        }
        if (j->own && j->r.ipc > heavy) {
            heavy = j->r.ipc;
            heavy_i = i;
        }
        if (!j->r.ipc || cupd)
            continue;
        if (!want)
            printf("regress: CPU %s: %.0f instructions / sample, no baseline (BUDGET_UPDATE=1 adds it)\n", j->name, ipc);
        else if (ipc > b * (1 + CPU_TOL) && ipc > b + CPU_SLACK) {
            printf("regress: CPU OVER BUDGET  %s: %.0f instructions / sample%s, baseline %.0f (+%.0f %%, limit +%.0f %%)\n",
                   j->name, ipc, own ? " over the idle mix" : "", b, (ipc / b - 1) * 100, CPU_TOL * 100);
            c_fail++;
        } else if (ipc < b * (1 - CPU_TOL) && ipc < b - CPU_SLACK) {
            printf("regress: CPU note  %s: %.0f instructions / sample, baseline %.0f (%.0f %%): faster? "
                   "BUDGET_UPDATE=1 to keep it\n", j->name, ipc, b, (ipc / b - 1) * 100);
            c_warn++;
        }
    }
    if (!J[c0].r.ipc)
        printf("regress: CPU: no instruction counter on this host (proc_pid_rusage); the budget is not checked\n");
    else if (cupd) {
        FILE *f = fopen(cpath, "w");
        if (!f) {
            perror(cpath);
            return 2;
        }
        fprintf(f, "# KOSMO host CPU baseline (tests/regress.c): instructions per 44.1 kHz sample, cc -O2 on\n"
                   "# the Mac (kernel-counted, ~1 %% run to run). The check allows +%.0f %%. Rewritten by BUDGET_UPDATE=1.\n",
                CPU_TOL * 100);
        for (i = c0; i < c1; i++)
            fprintf(f, "%s %.0f\n", J[i].name, J[i].r.ipc);
        fclose(f);
        printf("regress: CPU baseline %s rewritten (%u entries)\n", cpath, c1 - c0);
    }
    printf("regress: CPU, 8 notes held, heaviest sound (instructions / ns per sample):\n");
    printf("regress:   %-23s %6.0f instr  %6.1f ns\n", J[heavy_i].name + 4, heavy, J[heavy_i].r.ns);
    printf("regress:   %-23s %6.0f instr  %6.1f ns\n", J[c1 - 1u].name + 4, J[c1 - 1u].r.ipc, J[c1 - 1u].r.ns);

    printf("regress: %u golden renders (%u changed, %u gone), %u health failures, %u voice / routing checks failed, "
           "%u CPU entries over budget (%u notes), %u crashes; %.1f s\n", g1 - g0, gupd ? 0 : g_changed,
           gupd ? 0 : g_gone, h_fail, k_fail, cupd ? 0 : c_fail, c_warn, crash, (double)(now_ns() - t_start) / 1e9);
    if (!gupd && (g_changed || g_gone))
        printf("regress: the sound changed. If that is intended: GOLDEN_UPDATE=1 sh tests/run_tests.sh, "
               "then review and commit tests/golden.txt\n");
    return (!gupd && (g_changed || g_gone)) || h_fail || k_fail || (!cupd && c_fail) || crash;
}
