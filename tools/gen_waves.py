#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Wavetables for Kosmo's WAVE engine: 8 tables x 64 frames x 256 int16 samples. Each table is a designed morph
(vowels, hard sync, wavefolding, FM, drawbars ...), not a crossfade of two shapes. Every frame is DC-free and
peak-normalised to 32000; the script fails if a frame is not, or if two neighbouring frames differ too much (a
click when SCAN moves). Writes a C header and the table names.
  tools/gen_waves.py build/gen/kosmo_waves.h"""
import math
import random
import sys

N, FRAMES, PEAK = 256, 64, 32000
TAU = 2 * math.pi
SIN = [math.sin(TAU * k / N) for k in range(N)]
smooth = lambda u: u * u * (3 - 2 * u)                        # 0..1 -> 0..1, flat ends
clip01 = lambda u: min(1.0, max(0.0, u))
lerp = lambda a, b, u: a + (b - a) * u
frac = lambda x: x - math.floor(x)
tri = lambda x: abs(4 * frac(x - 0.25) - 2) - 1               # triangle wave, period 1
G = lambda h, c, w: math.exp(-((h - c) / w) ** 2)


def add(spec):                                                # {harmonic: (amp, phase in cycles)} -> 256 samples
    s = [0.0] * N
    for h, (a, p) in spec.items():
        if a:
            o = round(p * N)
            for i in range(N):
                s[i] += a * SIN[(h * i + o) % N]
    return s


def seg(t, pts):                                              # piecewise-smooth value through (t, v) points
    for (t0, v0), (t1, v1) in zip(pts, pts[1:]):
        if t <= t1:
            return lerp(v0, v1, smooth(clip01((t - t0) / (t1 - t0))))
    return pts[-1][1]


def robot_talk(t):                                            # a e i o u, formants as harmonics of 100 Hz
    V = [(7.3, 10.9, 24), (5.3, 18.4, 25), (2.7, 22.9, 30), (5.7, 8.4, 25), (3.0, 8.7, 24), (7.3, 10.9, 24)]
    k = min(int(t * 5), 4)
    u = smooth(clip01(t * 5 - k - 0.15) / 0.85)
    f1, f2, f3 = (lerp(V[k][j], V[k + 1][j], u) for j in range(3))
    return add({h: ((G(h, f1, 0.9) + 0.9 * G(h, f2, 1.4) + 0.5 * G(h, f3, 2.2)) / h ** (1 - .9 * t), 0) for h in range(1, 41)})   # and the buzz sharpens


def laser(t):                                                 # hard sync, slave ratio 1 -> 8, slave a bent triangle
    r = 8 ** t
    y = [tri(r * i / N + 0.25) + 0.5 * math.sin(TAU * 2 * r * i / N) for i in range(N + 1)]
    return [y[i] - (y[N] - y[0]) * i / N for i in range(N)]   # a tilt closes the sync jump, so scanning never clicks


def monster(t):                                               # wavefolder, gain 0.6 -> 10 on a lopsided sine
    g = 0.6 * (10 / 0.6) ** t
    return [tri(0.25 * g * (math.sin(TAU * i / N) + 0.45 * math.sin(TAU * 2 * i / N + 1.0)) + 0.0) for i in range(N)]


def chiptune(t):
    f = round(t * 63)
    if f < 24:                                                # the pulse width shrinks 8/16 -> 1/16 a frame at a time (edge sample partial)
        w = (8 - 7 * f / 23) * N / 16
        return [clip01(w - i) * 2 - 1 for i in range(N)]
    if f < 40:                                                # NES triangle bit-crushed 32 -> 3 levels
        q = round(32 * (3 / 32) ** ((f - 24) / 15))
        return [round(tri(i / N) * q / 2) for i in range(N)]
    if f < 52:                                                # two-pulse arps: a pulse and a pulse a fifth up
        m = (f - 40) / 11
        return [(1 if (i * 16 // N) < 4 else -1) + (1 if (i * 24 // N) % 2 else -1) * (0.3 + 1.2 * m) for i in range(N)]
    rnd = random.Random(3)                                    # LFSR-ish noise: 32 cells that flip one by one
    c = [(rnd.choice((-1, 1)), rnd.choice((-1, 1)), rnd.random()) for _ in range(32)]
    r = [y if u < (f - 52) / 11 else x for x, y, u in c]
    b = 8
    a = 2.0 * max(0.0, 1 - (f - 52) / 4)                      # the arp fades out under the first noise frames
    return [r[i // b] + a * ((1 if (i * 16 // N) < 4 else -1) + (1 if (i * 24 // N) % 2 else -1) * 1.5) for i in range(N)]


def alien(t):                                                 # FM: four breaths of the index, each with another modulator ratio
    f = round(t * 63)
    g, j = divmod(f, 16)
    m = (2, 5, 3, 7)[g]
    idx = 5 * math.sin(math.pi * (j + 0.5) / (32 if g == 3 else 16)) ** 2   # the last breath ends at full index
    return [math.sin(TAU * i / N + idx * math.sin(TAU * m * i / N)) for i in range(N)]


def crystal(t):                                               # sparse high partials that light up one after another
    P = (4, 7, 9, 12, 15, 18, 22, 26, 31)
    return add({h: (G(t, k / 8, .11) / h ** .2 + (.15 if h == 4 else 0), (k * .21) % 1) for k, h in enumerate(P)})


def ghost(t):                                                 # narrow band of random-phase partials: breathy -> whistly
    rnd = random.Random(7)
    ph = [rnd.random() for _ in range(41)]
    c, w = 6 + 18 * t, 8 * (1.0 / 8) ** smooth(t)
    return add({h: (G(h, c, w) + (0.12 if h == 1 else 0), ph[h]) for h in range(1, 41)})


def space_organ(t):                                           # drawbars on 1 2 3 4 5 6 8 10 12 (8' 4' 2.67' ...) per preset
    D = (1, 2, 3, 4, 5, 6, 8, 10, 12)
    R = ("888000000", "008000000", "888888888", "806000008", "004848800", "880008000", "865432100", "000008888")
    k = min(int(t * 8), 6)
    u = smooth(clip01((t * 8 - k - 0.45) / 0.55))
    return add({h: (lerp(int(R[k][j]), int(R[k + 1][j]), u) / 8, 0.05 * j) for j, h in enumerate(D)})


TABLES = [("ROBOT TALK", robot_talk), ("LASER", laser), ("MONSTER", monster), ("CHIPTUNE", chiptune),
          ("ALIEN", alien), ("CRYSTAL", crystal), ("GHOST", ghost), ("SPACE ORGAN", space_organ)]
WARPED = {"LASER", "MONSTER", "SPACE ORGAN"}                  # smooth in t, so the frames can be re-spaced; the others step on f by design


def prep(s):                                                  # DC removed, peak-normalised
    m = sum(s) / N
    s = [v - m for v in s]
    pk = max(abs(v) for v in s)
    return [v / pk * PEAK for v in s]


def spec(s, hmax=24):                                         # complex harmonics 1..hmax
    return [complex(sum(s[i] * SIN[(h * i + N // 4) % N] for i in range(N)), -sum(s[i] * SIN[h * i % N] for i in range(N)))
            for h in range(1, hmax + 1)]


def sdist(a, b):                                              # spectral distance of magnitudes, relative to the louder
    return math.sqrt(sum((abs(x) - abs(y)) ** 2 for x, y in zip(a, b)) / max(sum(abs(x) ** 2 for x in a), sum(abs(y) ** 2 for y in b)))


def warp(fn):                                                 # t for each frame: half uniform, half equal spectral steps
    G = 3 * FRAMES
    sp = [spec(prep(fn(g / (G - 1))), 16) for g in range(G)]
    c = [0.0]
    for a, b in zip(sp, sp[1:]):
        c.append(c[-1] + sdist(a, b))
    u = [0.5 * g / (G - 1) + 0.5 * c[g] / c[-1] for g in range(G)]
    ts, g = [], 0
    for f in range(FRAMES):
        while g < G - 2 and u[g + 1] < f / (FRAMES - 1):
            g += 1
        w = clip01((f / (FRAMES - 1) - u[g]) / (u[g + 1] - u[g])) if u[g + 1] > u[g] else 0
        ts.append((g + w) / (G - 1))
    return ts


def cjump(A, B, k=8):                                         # largest step of the spectral centroid along the blend A -> B
    best, prev = 0.0, None
    for j in range(k + 1):
        m = [abs(x + (y - x) * j / k) for x, y in zip(A, B)]
        c = sum((h + 1) * v for h, v in enumerate(m)) / sum(m)
        best = max(best, abs(c - prev)) if prev is not None else best
        prev = c
    return best


def align(p, A, s):                                           # rotate s to line its low harmonics up with those of A (the spectrum of p, the
    B = spec(s, 32)                                           # previous frame), unless the blend's centroid is smoother as is (or p is far off)
    x = [a.conjugate() * b for a, b in zip(A, B)]             # the shift k that maximises sum Re(x_h e^{i h k tau / N})
    k = max(range(N), key=lambda k: sum((v * complex(math.cos(TAU * (h + 1) * k / N), math.sin(TAU * (h + 1) * k / N))).real
                                        for h, v in enumerate(x)))
    R = [b * complex(math.cos(TAU * (h + 1) * k / N), math.sin(TAU * (h + 1) * k / N)) for h, b in enumerate(B)]
    r = s[k:] + s[:k]
    return (r, R) if cjump(A, R) < cjump(A, B) or jump(p, s) > 1.0 > jump(p, r) else (s, B)


def frames(fn, warped=False):                                 # 64 frames: phase-aligned to the one before, equal RMS where the peak allows
    ts = warp(fn) if warped else [f / (FRAMES - 1) for f in range(FRAMES)]
    out, sp = [], None
    for t in ts:
        s = prep(fn(t))
        s, sp = align(out[-1], sp, s) if sp else (s, spec(s, 32))
        out.append(s)
    rms = lambda s: math.sqrt(sum(v * v for v in s) / N)
    target = sorted(map(rms, out))[FRAMES // 2]               # the median frame's level
    return [[round(v * min(target / rms(s), PEAK / max(map(abs, s)))) for v in s] for s in out]


def jump(a, b):                                               # rms of the difference, relative to the louder frame's rms
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)) / max(sum(x * x for x in a), sum(x * x for x in b)))


JUMP_MAX = 1.2                                                # two unrelated frames differ by about 1.4


BITS = (8, 6, 5, 4)                                     # log2 points per frame of each mip level: 256, 64, 32, 16; a level holds 1/4 of its points as harmonics


def taper(h, top):                                            # 1 up to half the top harmonic, then a cos^2 fade to 0 above it
    return 1.0 if 4 * h <= top else math.cos(math.pi / 2 * (h - top / 4) / (3 * top / 4 + 1)) ** 2


def mip(fr, bits):                                            # a frame low-passed to P/4 harmonics, resampled to P = 2^bits points (linear
    P, K = 1 << bits, (1 << bits) // 4 + 1                    # reading images the harmonics above P/4 into the band, so half of P is headroom)
    out = []
    for s in fr:
        c = [(sum(s[i] * math.cos(TAU * h * i / N) for i in range(N)) * 2 / N,
              sum(s[i] * math.sin(TAU * h * i / N) for i in range(N)) * 2 / N) for h in range(1, K)]
        out.append([max(-32767, min(32767, round(sum(taper(h, K - 1) * (a * math.cos(TAU * h * i / P) + b * math.sin(TAU * h * i / P))
                                                     for h, (a, b) in enumerate(c, 1))))) for i in range(P)])
    return out


def main(path):
    out = ["/* generated by tools/gen_waves.py */", "#pragma once", "#include <stdint.h>", f"#define NWAVES {len(TABLES)}",
           f"#define WAVE_FRAMES {FRAMES}", f"#define WAVE_NLVL {len(BITS)}", f"static const int16_t WAVES[NWAVES][WAVE_FRAMES][{N}] = {{"]
    mips = [[] for _ in BITS]
    for name, fn in TABLES:
        fr = frames(fn, name in WARPED)
        for m in range(1, len(BITS)):
            mips[m].append(mip(fr, BITS[m]))
        for f, s in enumerate(fr):
            assert max(map(abs, s)) <= PEAK and abs(sum(s)) <= N, f"{name} frame {f}: too loud / not DC-free"
            if f:
                assert jump(fr[f - 1], s) <= JUMP_MAX, f"{name} frame {f}: jump {jump(fr[f - 1], s):.2f}"
        out.append(f" {{ /* {name} */")
        out += ["  {" + ",".join(map(str, s)) + "}," for s in fr]
        out.append(" },")
    out.append("};")
    for m in range(1, len(BITS)):                             # level m: 2^BITS points a frame
        out.append(f"static const int16_t WAVEM{m}[NWAVES][WAVE_FRAMES][{1 << BITS[m]}] = {{")
        out += [" {" + ",".join("{" + ",".join(map(str, s)) + "}" for s in t) + "}," for t in mips[m]]
        out.append("};")
    out.append("static const int16_t *const WAVE_LVL[WAVE_NLVL] = {&WAVES[0][0][0], " +
               ", ".join(f"&WAVEM{m}[0][0][0]" for m in range(1, len(BITS))) + "};")
    out.append("static const uint8_t WAVE_BITS[WAVE_NLVL] = {" + ",".join(map(str, BITS)) + "};")
    with open(path, "w") as fh:
        fh.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main(sys.argv[1])
