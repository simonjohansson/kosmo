#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Wavetables for Kosmo's WAVE engine: 12 tables x 64 frames x 256 int16 samples. Each table is a designed morph
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


def zap(t):                                                   # CZ resonance: a saw-windowed sine, 1 -> 18 cycles
    k = 1 + 17 * t ** 1.5
    return [(1 - i / N) ** 1.3 * math.sin(TAU * k * i / N) + 0.3 * (1 - i / N) ** 3 * math.sin(TAU * k * 1.5 * i / N)
            for i in range(N)]


def chiptune(t):
    f = round(t * 63)
    if f < 24:                                                # pulse widths in 1/16 steps, 8 of them, 3 frames each
        w = 8 - f // 3
        return [1.0 if (i * 16 // N) < w else -1.0 for i in range(N)]
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


def bubbles(t):                                               # harmonics grow in one by one, then odd only, then bells
    f = round(t * 63)
    if f < 30:
        n = 1 + 27 * f / 29
        return add({h: (clip01(n - h + 1) / h ** .8, 0) for h in range(1, 29)})
    if f < 46:
        u = smooth((f - 30) / 15)
        return add({h: ((1 if h % 2 else 1 - u) / h ** .8, 0) for h in range(1, 29)})
    u = smooth((f - 46) / 17)
    bell = {1: 1, 3: .7, 4: .6, 6: .5, 9: .45, 10: .4, 14: .35, 17: .3}
    return add({h: (lerp((1 / h ** .8) if h % 2 else 0, bell.get(h, 0), u), 0) for h in range(1, 29)})


def crystal(t):                                               # sparse high partials that light up one after another
    P = (4, 7, 9, 12, 15, 18, 22, 26, 31)
    return add({h: (G(t, k / 8, .11) / h ** .2 + (.15 if h == 4 else 0), (k * .21) % 1) for k, h in enumerate(P)})


def dragon(t):                                                # more and more saws, driven harder, pulled off-centre
    g = 1.0 * 5 ** t
    saws = (frac(i / N + .5) * 2 - 1 for i in range(N))
    return [math.tanh(g * (x + sum(clip01(t * 6 - j + 1) * (frac((j + 1) * i / N + .13 * j) * 2 - 1) for j in range(1, 6))
                           + 0.8 * smooth(t))) for i, x in enumerate(saws)]


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


def wild(t):                                                  # a new random spectrum every two frames
    f = round(t * 63)
    def key(n):
        rnd = random.Random(1000 + n)
        return {rnd.randint(1, 30): rnd.uniform(.3, 1) for _ in range(rnd.randint(2, 6))}
    k, u = divmod(f, 2)
    a, b = key(k), key(k + 1)
    ph = random.Random(5)
    return add({h: (lerp(a.get(h, 0), b.get(h, 0), u / 2), ph.random()) for h in range(1, 31)})


TABLES = [("ROBOT TALK", robot_talk), ("LASER", laser), ("MONSTER", monster), ("ZAP", zap), ("CHIPTUNE", chiptune),
          ("ALIEN", alien), ("BUBBLES", bubbles), ("CRYSTAL", crystal), ("DRAGON", dragon), ("GHOST", ghost),
          ("SPACE ORGAN", space_organ), ("WILD", wild)]


def frames(fn):                                               # 64 frames, DC removed, peak-normalised, as ints
    out = []
    for f in range(FRAMES):
        s = fn(f / (FRAMES - 1))
        m = sum(s) / N
        s = [v - m for v in s]
        pk = max(abs(v) for v in s)
        out.append([round(v / pk * PEAK) for v in s])
    return out


def jump(a, b):                                               # rms of the difference, relative to the louder frame's rms
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)) / max(sum(x * x for x in a), sum(x * x for x in b)))


JUMP_MAX = 1.2                                                # two unrelated frames differ by about 1.4


def main(path):
    out = ["/* generated by tools/gen_waves.py */", "#pragma once", "#include <stdint.h>", f"#define NWAVES {len(TABLES)}",
           f"#define WAVE_FRAMES {FRAMES}", f"static const int16_t WAVES[NWAVES][WAVE_FRAMES][{N}] = {{"]
    for name, fn in TABLES:
        fr = frames(fn)
        for f, s in enumerate(fr):
            assert max(map(abs, s)) >= PEAK - 1 and abs(sum(s)) <= N, f"{name} frame {f}: not normalised / DC-free"
            if f:
                assert jump(fr[f - 1], s) <= JUMP_MAX, f"{name} frame {f}: jump {jump(fr[f - 1], s):.2f}"
        out.append(f" {{ /* {name} */")
        out += ["  {" + ",".join(map(str, s)) + "}," for s in fr]
        out.append(" },")
    out.append("};")
    with open(path, "w") as fh:
        fh.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main(sys.argv[1])
