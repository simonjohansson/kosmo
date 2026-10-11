#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Review montage: every wavetable at SCAN 0 12 25 37 50 62 75 87 100 %, glow lines like the WAVE page.
  tools/wave_montage.py build/wave_montage.png"""
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter
sys.path.insert(0, str(Path(__file__).parent))
import gen_waves as gw

W, H, PAD = 140, 80, 5
COLS = (0, 12, 25, 37, 50, 62, 75, 87, 100)
img = Image.new("RGB", (len(COLS) * W + 130, len(gw.TABLES) * H), (8, 10, 16))
d = ImageDraw.Draw(img)
for r, (name, fn) in enumerate(gw.TABLES):
    fr = gw.frames(fn)
    d.text((6, r * H + H // 2 - 5), name, fill=(255, 255, 255))
    for c, pct in enumerate(COLS):
        s = fr[round(pct / 100 * (gw.FRAMES - 1))] + [fr[round(pct / 100 * (gw.FRAMES - 1))][0]]
        pts = [(130 + c * W + PAD + i * (W - 2 * PAD) / gw.N, r * H + H / 2 - v / 32000 * (H / 2 - 6)) for i, v in enumerate(s)]
        glow = Image.new("RGB", img.size)
        ImageDraw.Draw(glow).line(pts, fill=(0, 120, 255), width=5)
        glow = glow.filter(ImageFilter.GaussianBlur(3))
        img.paste(glow, mask=glow.convert("L").point(lambda v: min(255, v * 3)))
        d = ImageDraw.Draw(img)
        d.line(pts, fill=(160, 220, 255), width=2)
        d.rectangle((130 + c * W, r * H, 130 + (c + 1) * W - 1, (r + 1) * H - 1), outline=(30, 36, 50))
img.save(sys.argv[1])
