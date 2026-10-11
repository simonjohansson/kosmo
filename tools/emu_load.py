#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Guest load in fm1-emulator (issue #17): a C-E-G chord on HOME at FAT 0 and FAT 4, then the firmware's own load
meter (felucca_dbg in the SRAM dump, written by the audio ISR); and the length of a WAVE redraw at FAT 4 in guest
instructions (traced at cv_begin / cv_blit). Fails above the limits below.

    tools/emu_load.py [FIRMWARE]      (FM1_EMULATOR: emulator binary, default ~/src/fm1-emulator/emulator)

Load = ISR time per half buffer (2902 us) on the emulator's 8 ns per instruction; the chip needs ~0.8x of that.
"""
import os, re, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EMU = Path(os.environ.get("FM1_EMULATOR", Path.home() / "src/fm1-emulator/emulator"))
S, MS = 10**9, 10**6
SRAM = 0x01C00000
HALF_US = 128 * 10**6 // 44100
LOAD_MAX = {0: 25, 4: 45}           # % of a half, 93 ms average with the chord held (measured 21.5 / 40.6)
WORST_US = HALF_US * 85 // 100      # above this the firmware sheds voices
REDRAW_MAX = 6_500_000              # instructions, first cv_begin to the last cv_blit of a WAVE redraw at FAT 4 (measured 6.1 M, mostly the SPI DMA wait)
K3, K4 = ("f19", "f20"), ("f21", "f22")   # HOME knobs (tools/emu_check.py)
CHORD = ("z", "m", "slash")


def sym(elf, name):
    return int(re.search(rf"^([0-9a-f]+) . {name}$", subprocess.run(["nm", str(elf)], capture_output=True, text=True, check=True).stdout, re.M).group(1), 16)


def detents(ev, t, knob, n):
    a, b = knob
    for _ in range(n):               # clockwise detent, as emu_check.py; 120 ms apart: the UI misses faster ones while it redraws
        ev += [(t, b, 1), (t + 5 * MS, a, 1), (t + 10 * MS, b, 0), (t + 15 * MS, a, 0)]
        t += 120 * MS
    return t


def run(fw, fat, chord, steps):
    ev = []
    t = detents(ev, 19 * S, K3[::-1], 4)            # FAT is 2 after boot: down to 0 ...
    detents(ev, t, K3, fat)                          # ... then up
    for k in chord:
        ev += [(20 * S, k, 1), (26 * S, k, 0)]
    detents(ev, 20 * S, K4, steps)                   # DETUNE: each step is one WAVE redraw
    elf = fw.with_suffix(".elf")
    pcs = f"{sym(elf, 'cv_begin'):x},{sym(elf, 'cv_blit'):x}" if steps else ""
    d = Path(tempfile.mkdtemp(dir=ROOT / "build"))
    env = {k: v for k, v in os.environ.items() if not k.startswith("FM1_POC_")}
    env.update(FM1_POC_STATE_DIR=str(d), FM1_POC_MAX_INSTRUCTIONS=str(10**13), FM1_POC_CAPTURE_NS=str(25 * S),
               FM1_POC_INPUT=",".join(f"{x}:{c}:{v}" for x, c, v in sorted(ev)))
    if pcs:
        env["FM1_POC_TRACE_PCS"] = pcs
    cmd = [str(EMU), "--qemu", "-M", "fm1-poc", "-smp", "2", "-accel", "tcg,thread=single", "-icount", "shift=3,align=off,sleep=off",
           "-display", "none", "-chardev", "null,id=console", "-serial", "chardev:console", "-monitor", "none", "-nodefaults",
           "-no-user-config", "-kernel", str(fw), "-append", "application"]
    err = subprocess.run(cmd, env=env, capture_output=True, text=True).stderr
    dbg = struct.unpack_from("<11I", (d / "state.sram").read_bytes(), sym(elf, "felucca_dbg") - SRAM)
    shutil.rmtree(d)
    tr = [(int(a), int(b, 16)) for a, b in re.findall(r"TRACE core\d n=(\d+) pc=([0-9a-f]+) .* irq=0", err)] if steps else []
    return dict(halves=dbg[1], max_us=dbg[2], late=dbg[5], load=dbg[9] * 100 / 256, trace=tr)


def redraw(tr, begin, blit):
    """Instructions from a frame's first cv_begin to its second cv_blit: the last complete frame."""
    ev = [(n, "b" if pc == begin else "k") for n, pc in tr if pc in (begin, blit)]
    s = "".join(k for _, k in ev)
    i = s.rfind("bkbk")
    return ev[i + 3][0] - ev[i][0] if i >= 0 else None


def main():
    fw = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/kosmo.bin"
    assert EMU.is_file() and fw.is_file(), f"emu_load: need {EMU} and {fw}"
    elf = fw.with_suffix(".elf")
    bad = []
    for fat in (0, 4):
        r = run(fw, fat, CHORD, 0)
        print(f"chord, FAT {fat}: load {r['load']:.1f}% (max {LOAD_MAX[fat]}), worst half {r['max_us']} us (max {WORST_US}), late {r['late']}")
        if r["load"] > LOAD_MAX[fat]: bad.append(f"FAT {fat}: load {r['load']:.1f}% > {LOAD_MAX[fat]}%")
        if r["max_us"] > WORST_US: bad.append(f"FAT {fat}: worst half {r['max_us']} us > {WORST_US}")
        if r["late"]: bad.append(f"FAT {fat}: {r['late']} late halves (underrun)")
    r = run(fw, 4, (), 4)
    n = redraw(r["trace"], sym(elf, "cv_begin"), sym(elf, "cv_blit"))
    print(f"WAVE redraw at FAT 4: {n} instructions (max {REDRAW_MAX})")
    if n is None or n > REDRAW_MAX: bad.append(f"WAVE redraw {n} instructions > {REDRAW_MAX}")
    print("\n".join("FAIL: " + b for b in bad) or "PASS")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
