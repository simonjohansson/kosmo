#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Guest load in fm1-emulator (issue #17): a C-E-G chord on HOME at FAT 0 and FAT 4, then the firmware's own load
meter (felucca_dbg in the SRAM dump, written by the audio ISR); and the length of a WAVE redraw at FAT 4 in guest
instructions (traced at cv_begin / cv_blit). Also the UI of one WAVE knob step (issue #17): bytes sent over the SPI, the time to
the bar and to the picture, and that a burst of detents 25 ms apart (faster than a redraw) lands on the right value.
Fails above the limits below.

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
LOAD_MAX = {0: 32, 4: 48}           # % (issue #26, stereo end to end: measured 30.9 / 46.5, was 28.1 / 43.4); of a half, 93 ms average with the chord held (measured 28.1 with #25 + #27, 26.6 / 41.4 before; 8-line SPACE, issue #27: it runs always, preset 0 is CLOUD)
ECHO_MAX = {"CLEAN": 59, "BACKWARDS": 56}   # #28: CLEAN measured 57.0 (half-band filters, cubic reads, wow; was 52.3), BACKWARDS 54.7; #26: measured 52.3 / 53.9; FAT 4 with ECHO on (issue #20: AMOUNT 64, REPEATS as at boot; measured 46.9 / 48.8 with the 8-line SPACE CLOUD; 39.8 / 41.4 with the 4-line one)
WORST_US = HALF_US * 85 // 100      # above this the firmware sheds voices
STEP_BYTES_MAX = 66_000             # SPI bytes of one DETUNE step on HOME: the bar (58 x 60) and the picture (224 x 122), measured 61.6 K (96 K before)
BAR_MS_MAX = 15                     # last contact of the detent to the bar on the panel (measured 5.8; 74 ms to the whole redraw before)
PIC_MS_MAX = 41                     # (#26: 40.1 under the stereo ISR's longer blocks) from the bar on the panel to the picture on it (measured 35.5: its DMA at 12 MHz)
BAR_BYTES = 58 * 60 * 2
BURST = 12                          # detents 25 ms apart: DETUNE must move BURST * 4
SPI_BPS = 12_000_000 // 8
SPACE_MAX = {"CLOUD": 59, "GATED": 66}  # #28: measured 57.0 / 63.7 with the better CLEAN; #26: measured 52.3 / 59.0; GATED 54.7 with #25 + #27; FAT 4 + ECHO CLEAN + SPACE on its 2/2 page, AMOUNT up (issue #27: 8 lines, measured 46.9 at the 22 kHz tank, 52.7 at 32 kHz; the 4-line one 39.8; the worst half stays at 1.5 ms of 2.1)
K1, K2, K3, K4 = ("f17", "f18"), ("d", "f"), ("f19", "f20"), ("f21", "f22")   # knobs (tools/emu_check.py)
CHORD = ("z", "m", "slash")


def sym(elf, name):
    return int(re.search(rf"^([0-9a-f]+) . {name}$", subprocess.run(["nm", str(elf)], capture_output=True, text=True, check=True).stdout, re.M).group(1), 16)


def detents(ev, t, knob, n, gap=120):
    a, b = knob
    for _ in range(n):               # clockwise detent, as emu_check.py; 120 ms apart: the UI misses faster ones while it redraws
        ev += [(t, b, 1), (t + 5 * MS, a, 1), (t + 10 * MS, b, 0), (t + 15 * MS, a, 0)]
        t += gap * MS
    return t


def run(fw, fat, chord, steps, gap=120, echo=None, space=None):
    ev = []
    t = detents(ev, 19 * S, K3[::-1], 4)            # FAT is 2 after boot: down to 0 ...
    detents(ev, t, K3, fat)                          # ... then up
    if echo is not None:                             # ECHO: the FX button, AMOUNT 0 -> 64, CHARACTER echo steps (CLEAN 0, BACKWARDS 3)
        ev += [(19950 * MS, "f1", 1), (20000 * MS, "f1", 0)]
        t = detents(ev, 20100 * MS, K4, 16)
        t = detents(ev, t, K3, echo)
        if space is not None:                                    # the FX button again: SPACE; AMOUNT +32, SIZE +16, ROOM +space steps
            ev += [(t + 100 * MS, "f1", 1), (t + 150 * MS, "f1", 0)]
            t = detents(ev, t + 300 * MS, K1, 8)
            t = detents(ev, t, K2, 4)
            detents(ev, t, K3, space)
    for k in chord:
        ev += [(20 * S, k, 1), (26 * S, k, 0)]
    detents(ev, 20 * S, K4, steps, gap)                   # DETUNE: each step is one WAVE redraw
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
    ram = (d / "state.sram").read_bytes()
    dbg = struct.unpack_from("<11I", ram, sym(elf, "felucca_dbg") - SRAM)
    shutil.rmtree(d)
    tr = [(int(a), int(b, 16)) for a, b in re.findall(r"TRACE core\d n=(\d+) pc=([0-9a-f]+) .* irq=0", err)] if steps else []
    return dict(halves=dbg[1], max_us=dbg[2], late=dbg[5], load=dbg[9] * 100 / 256, trace=tr,
                detune=struct.unpack_from("<h", ram, sym(elf, "ui_sig") - SRAM + 8)[0],
                bytes=struct.unpack_from("<I", ram, sym(elf, "lcd_bytes") - SRAM)[0])


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
    for name, ch in (("CLEAN", 0), ("BACKWARDS", 3)):
        r = run(fw, 4, CHORD, 0, echo=ch)
        print(f"chord, FAT 4, ECHO {name}: load {r['load']:.1f}% (max {ECHO_MAX[name]}), worst half {r['max_us']} us (max {WORST_US}), late {r['late']}")
        if r["load"] > ECHO_MAX[name]: bad.append(f"FAT 4 + ECHO {name}: load {r['load']:.1f}% > {ECHO_MAX[name]}%")
        if r["max_us"] > WORST_US: bad.append(f"FAT 4 + ECHO {name}: worst half {r['max_us']} us > {WORST_US}")
        if r["late"]: bad.append(f"FAT 4 + ECHO {name}: {r['late']} late halves (underrun)")
    for name, rm in (("CLOUD", 0), ("GATED", 1)):      # the preset 0 program (a 22 kHz tank) and one step up (GATED: a 32 kHz one, the dearest) (issue #27)
        r = run(fw, 4, CHORD, 0, echo=0, space=rm)
        print(f"chord, FAT 4, ECHO CLEAN + SPACE {name}: load {r['load']:.1f}% (max {SPACE_MAX[name]}), worst half {r['max_us']} us (max {WORST_US}), late {r['late']}")
        if r["load"] > SPACE_MAX[name]: bad.append(f"FAT 4 + ECHO + SPACE {name}: load {r['load']:.1f}% > {SPACE_MAX[name]}%")
        if r["max_us"] > WORST_US: bad.append(f"FAT 4 + ECHO + SPACE {name}: worst half {r['max_us']} us > {WORST_US}")
        if r["late"]: bad.append(f"FAT 4 + ECHO + SPACE {name}: {r['late']} late halves (underrun)")
    base = run(fw, 4, CHORD, 0)                 # (the chord changes nothing on HOME's screen)
    r = run(fw, 4, (), 4)
    per = (r["bytes"] - base["bytes"]) // 4
    ev = [(n * 8, "b" if pc == sym(elf, "cv_begin") else "k") for n, pc in r["trace"]]
    s = "".join(k for _, k in ev)
    i = s.rfind("bkbkbk")
    t = [x for x, _ in ev[i:i + 6]] if i >= 0 else [0] * 6          # bar begin / blit, then two picture strips
    last = 20 * S + 3 * 120 * MS + 15 * MS                             # the 4th detent's last contact
    half = (per - BAR_BYTES) // 2
    bar = (t[1] + BAR_BYTES * S // SPI_BPS - last) / MS
    pic = (max(t[5], t[3] + half * S // SPI_BPS) + half * S // SPI_BPS - t[1] - BAR_BYTES * S // SPI_BPS) / MS
    print(f"WAVE knob step at FAT 4: {per} SPI bytes (max {STEP_BYTES_MAX}), bar after {bar:.1f} ms (max {BAR_MS_MAX}), picture after the bar {pic:.1f} ms (max {PIC_MS_MAX})")
    if per > STEP_BYTES_MAX: bad.append(f"knob step {per} SPI bytes > {STEP_BYTES_MAX}")
    if not 0 < bar <= BAR_MS_MAX: bad.append(f"bar {bar:.1f} ms > {BAR_MS_MAX}")
    if not 0 < pic <= PIC_MS_MAX: bad.append(f"picture {pic:.1f} ms > {PIC_MS_MAX}")
    b = run(fw, 4, CHORD, BURST, 25)               # chord held, so the audio runs too; the UI redraws while the detents come
    want = min(127, base["detune"] + 4 * BURST)
    print(f"{BURST} detents 25 ms apart: DETUNE {base['detune']} -> {b['detune']} (want {want})")
    if b["detune"] != want: bad.append(f"detent burst: DETUNE {b['detune']}, want {want}")
    print("\n".join("FAIL: " + b for b in bad) or "PASS")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
