#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Smoothness of the wavetables for SCAN: per table the largest and mean step of the spectral centroid (in harmonics)
between neighbouring frames, and the worst dip in level of a half-way crossfade (cancellation).
  tools/wave_smooth.py"""
import math
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import gen_waves as gw


def centroid(s):                                              # spectral centroid in harmonics
    m = [abs(sum(s[i] * complex(math.cos(gw.TAU * h * i / gw.N), -math.sin(gw.TAU * h * i / gw.N)) for i in range(gw.N)))
         for h in range(1, 65)]
    return sum((h + 1) * v for h, v in enumerate(m)) / sum(m)


rms = lambda s: math.sqrt(sum(v * v for v in s) / len(s))


def sweep(fr, k=47):                                          # largest centroid jump of the linear blend, k steps a frame
    sp = [gw.spec(s, 64) for s in fr]
    best, prev = 0.0, None
    for a, b in zip(sp, sp[1:]):
        for j in range(k):
            m = [abs(x + (y - x) * j / k) for x, y in zip(a, b)]
            c = sum((h + 1) * v for h, v in enumerate(m)) / sum(m)
            best = max(best, abs(c - prev)) if prev is not None else best
            prev = c
    return best


def stats(fr):
    c = [centroid(s) for s in fr]
    d = [abs(b - a) for a, b in zip(c, c[1:])]
    dip = max(1 - rms([(x + y) / 2 for x, y in zip(a, b)]) / ((rms(a) + rms(b)) / 2) for a, b in zip(fr, fr[1:]))
    return max(d), sum(d) / len(d), dip, sweep(fr)


if __name__ == "__main__":
    for name, fn in gw.TABLES:
        mx, mean, dip, sw = stats(gw.frames(fn, name in gw.WARPED))
        print(f"{name:12s} max step {mx:5.2f}  mean step {mean:5.2f}  worst crossfade dip {dip * 100:4.0f} %  blend jump {sw:5.3f}/block")
