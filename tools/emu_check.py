#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""One headless fm1-emulator run of build/kosmo.bin: boot, visit each page, turn its knobs, turn PRESETS,
play a note, turn ALGORITHM to SEM and visit its two HOME pages, snapshot the LCD after each step and compare with tests/emu_ref/*.png.

    tools/emu_check.py [--update] [FIRMWARE]     (FM1_EMULATOR: emulator binary, default ~/src/fm1-emulator/emulator)

--update rewrites the references. A mismatch writes build/emu_check/<step>_diff.png (actual, mismatches red).
"""
import json, os, subprocess, sys, tempfile
from pathlib import Path
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parent.parent
REF, OUT = ROOT / "tests/emu_ref", ROOT / "build/emu_check"
EMU = Path(os.environ.get("FM1_EMULATOR", Path.home() / "src/fm1-emulator/emulator"))
TOL = 0          # per-channel difference a pixel may have: 0 was stable over 3+ runs (snapshots are bit-identical); raise if the 2-core interleaving ever flakes
S, MS = 10**9, 10**6
# qcodes from fm1-emulator src/include/ui/fm1-controls.h: pages, K1..K4 (A, B phases), PRESETS, piano C
PAGE = [("HOME", "h"), ("EDIT", "f5"), ("ENV", "p"), ("ECHO", "f1"), ("SPACE", "f1")]   # FX pressed twice: ECHO, then SPACE (ui.c)
KNOB = [("f17", "f18"), ("d", "f"), ("f19", "f20"), ("f21", "f22")]
PRESETS, ALGO, NOTE = ("f13", "f14"), ("f15", "f16"), "z"
# page -> how many knobs it uses (ui.c PG[])
USES = {"HOME": 4, "EDIT": 3, "ENV": 4, "ECHO": 4, "SPACE": 4}


def script():
    """Returns (events, [(step name, snapshot ns)]). Each step is followed by a 400 ms settle."""
    ev, steps, t = [], [("00_boot", 19 * S)], 20 * S

    def snap(name):
        nonlocal t
        steps.append((name, t + 400 * MS))
        t += 800 * MS

    def press(code, hold=50 * MS):
        nonlocal t
        ev.extend([(t, code, 1), (t + hold, code, 0)])
        t += hold + 50 * MS

    def turn(enc, detents=3):    # clockwise detent: B closes first, A second, then released in that order
        nonlocal t
        a, b = enc
        for _ in range(detents):
            ev.extend([(t, b, 1), (t + 5 * MS, a, 1), (t + 10 * MS, b, 0), (t + 15 * MS, a, 0)])
            t += 25 * MS

    for i, (page, key) in enumerate(PAGE, 1):
        if page != "HOME":
            press(key)
            snap(f"{i:02d}_{page.lower()}_page")
        for k in range(USES[page]):
            turn(KNOB[k])
        snap(f"{i:02d}_{page.lower()}_knobs")
    for name in ("gated", "cloud", "plate", "castle", "bedroom"):
        turn(KNOB[2][::-1], 1)                              # the SPACE page again: ROOM one program down at a time (WOBBLE was shown above: 3 fast detents)
        snap(f"05_space_{name}")
    turn(PRESETS, 1)
    snap("06_preset")
    t += 1200 * MS                                          # the name banner is gone
    snap("07_before_note")
    ev.append((t, NOTE, 1))
    steps.append(("08_note", t + 600 * MS))
    ev.append((t + 700 * MS, NOTE, 0))
    t += 1500 * MS
    press("f6")                                             # GLO: the SETTINGS page (ECHO PING-PONG), then K1 to STEREO, K2 to SEM UNISON ON
    snap("09_settings_page")
    turn(KNOB[0], 1)
    snap("09_settings_knobs")
    turn(KNOB[1], 1)                                        # K2: SEM UNISON ON
    snap("09_settings_unison")
    turn(ALGO, 1)                                           # ALGORITHM: the engine banner (SEM), then HOME is SEM's 1/2 and 2/2
    snap("10_sem_banner")
    t += 1200 * MS
    press("h")
    snap("10_sem_page1")
    for k in range(4):
        turn(KNOB[k])
    snap("10_sem_page1_knobs")
    press("h")
    snap("10_sem_page2")
    for k in range(4):
        turn(KNOB[k])
    snap("10_sem_page2_knobs")
    return ev, steps


def run(fw):
    ev, steps = script()
    d = Path(tempfile.mkdtemp())
    env = {k: v for k, v in os.environ.items() if not k.startswith("FM1_POC_")}
    env.update(FM1_POC_STATE_DIR=str(d), FM1_POC_MAX_INSTRUCTIONS=str(10**13),
               FM1_POC_INPUT=",".join(f"{t}:{c}:{v}" for t, c, v in sorted(ev)),
               FM1_POC_SNAPSHOT_NS=",".join(str(t) for _, t in steps),
               FM1_POC_CAPTURE_NS=str(steps[-1][1] + S))
    cmd = [str(EMU), "--qemu", "-M", "fm1-poc", "-smp", "2", "-accel", "tcg,thread=single",
           "-icount", "shift=3,align=off,sleep=off", "-display", "none", "-chardev", "null,id=console",
           "-serial", "chardev:console", "-monitor", "none", "-nodefaults", "-no-user-config",
           "-kernel", str(fw), "-append", "application"]
    subprocess.run(cmd, env=env, capture_output=True, text=True)
    log = [json.loads(l) for l in (d / "snapshots.jsonl").read_text().splitlines()] if (d / "snapshots.jsonl").exists() else []
    assert len(log) == len(steps), f"emu_check: {len(log)} of {len(steps)} snapshots taken"
    return steps, log, d


def main():
    args = [a for a in sys.argv[1:] if a != "--update"]
    update = "--update" in sys.argv
    fw = Path(args[0]) if args else ROOT / "build/kosmo.bin"
    assert EMU.is_file() and fw.is_file(), f"emu_check: need {EMU} and {fw}"
    steps, log, d = run(fw)
    REF.mkdir(exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    bad = []
    for (name, _), s in zip(steps, log):
        if not s["visible"]:
            bad.append(f"{name}: LCD not visible")
            continue
        img = Image.open(d / f"snapshot-{s['index']}.ppm").convert("RGB")
        ref = REF / f"{name}.png"
        if update:
            img.save(ref)
            continue
        if not ref.exists():
            bad.append(f"{name}: no reference (run with --update)")
            continue
        r = Image.open(ref).convert("RGB")
        if r.size != img.size:
            bad.append(f"{name}: size {img.size} != {r.size}")
            continue
        mask = ImageChops.difference(img, r).convert("L").point(lambda v: 255 if v > TOL else 0)
        n = mask.histogram()[255]
        if n:
            diff = img.copy()
            diff.paste((255, 0, 0), mask=mask)
            p = OUT / f"{name}_diff.png"
            diff.save(p)
            bad.append(f"{name}: {n} pixels differ by more than {TOL}: {p}")
    names = [n for n, _ in steps]
    pre, post = log[names.index("07_before_note")], log[names.index("08_note")]
    if not update and not (post["audio_nonzero_words"] > pre["audio_nonzero_words"] and post["audio_completions"] > pre["audio_completions"]):
        bad.append(f"audio counters did not increase after the note: {pre['audio_nonzero_words']} -> {post['audio_nonzero_words']}")
    print("\n".join(("FAIL: " + b) for b in bad) or ("UPDATED" if update else "PASS") + f" ({len(steps)} snapshots)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
