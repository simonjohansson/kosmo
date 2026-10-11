#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Device benchmark for a KOSMO_PERF=1 KOSMO_CDC=1 build: drives notes/knobs over USB-MIDI (CC 102 page, 103 wave,
104 scan, 105 fat, 106 detune, 107 cutoff, 108 preset, 109 ECHO char, 110 ECHO amount, 111 SPACE program, 112 SPACE amount) and reads the "P ..." / "C ..." lines from the CDC port.

    tools/perf_host.py [--port /dev/cu.usbmodemXXXX] [--secs 5]
"""
import argparse, glob, re, statistics as st, threading, time
import mido, serial

DEADLINE = 128 * 24_000_000 // 44117          # TIMER4 ticks of one half buffer
NOTES = [48, 52, 55, 59, 60, 64, 67, 71]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=(glob.glob("/dev/cu.usbmodem*") or [None])[0])
    ap.add_argument("--secs", type=int, default=5)
    ap.add_argument("--space", action="store_true", help="only the ECHO CLEAN + SPACE program sweep (issue #27)")
    ap.add_argument("--midi", default="Felucca")
    a = ap.parse_args()
    ser = serial.Serial(a.port, 115200, timeout=0.2)
    out = mido.open_output(next(n for n in mido.get_output_names() if a.midi.lower() in n.lower()))
    lines, stop = [], False

    def rd():
        buf = b""
        while not stop:
            buf += ser.read(256)
            while b"\n" in buf:
                l, buf = buf.split(b"\n", 1)
                lines.append((time.time(), l.decode("ascii", "replace")))
    threading.Thread(target=rd, daemon=True).start()
    cc = lambda n, v: out.send(mido.Message("control_change", control=n, value=v))
    notes = lambda on: [out.send(mido.Message("note_on" if on else "note_off", note=n, velocity=100 if on else 0)) for n in NOTES]

    def phase(name, setup, during=None):
        setup()
        t0 = time.time()
        i = 0
        while time.time() - t0 < a.secs:
            if during:
                during(i)
            i += 1
            time.sleep(0.3 if during else 0.05)
        ws = [parse(l) for t, l in lines if t0 + 1.5 < t <= time.time() and l.startswith("P ")]
        report(name, ws)

    def parse(l):
        return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", l)}

    def ms(t): return t / 24000

    def report(name, ws):
        if not ws:
            print(f"{name:28s} no data"); return
        n = sum(w["rn"] for w in ws)
        avg = sum(w["rs"] for w in ws) / max(n, 1)
        mx = max(w["rx"] for w in ws)
        amx = max(w["ax"] for w in ws)
        s = f"{name:28s} ISR avg {avg / 24:6.0f}us {100 * avg / DEADLINE:5.1f}%  max {100 * mx / DEADLINE:5.1f}%  all-max {100 * amx / DEADLINE:5.1f}%  late {ws[-1]['late'] - ws[0]['late']} shed {ws[-1]['shed'] - ws[0]['shed']}"
        for k, lab in (("st", "step cpu"), ("sf", "step full"), ("pc", "pic"), ("pg", "page cpu"), ("pf", "page full")):
            c = sum(w[k + "n"] for w in ws)
            if c:
                s += f" | {lab} {ms(sum(w[k + 's'] for w in ws) / c):.1f}ms/{ms(max(w[k + 'x'] for w in ws)):.0f}max n{c}"
        t5 = sum(w["ts"] + w["us"] for w in ws) / len(ws) / 24e6 * 100
        parts = " ".join(f"{k}:{sum(w[k + 's'] for w in ws) / max(sum(w[k + 'n'] for w in ws), 1) / 24:.1f}us*{sum(w[k + 'n'] for w in ws) // len(ws)}/s"
                         for k in ("i", "ub", "ua") if k + "n" in ws[0])
        print(s + f" | TIMER5 {t5:.1f}% of CPU [{parts}]")
        spi = ws[-1]["spi"] - ws[0]["spi"]
        if spi:
            print(f"{'':28s} SPI {spi / (len(ws) - 1)} B/s  bss {ws[-1]['bss']} pool {ws[-1]['pool']}")

    notes(False); cc(123, 0); cc(102, 0); cc(105, 0)
    time.sleep(2)
    if a.space:                                       # 8 notes, ECHO CLEAN (amount 64), SPACE amount 100, each program, FAT 0 / 4
        names = ("BEDROOM", "CASTLE", "PLATE", "CLOUD", "GATED", "WOBBLE")
        cc(108, 0); cc(109, 0); cc(110, 64); cc(112, 100)
        for fat in (0, 4):
            for p in range(6):
                phase(f"FAT{fat} {names[p]}", lambda: (notes(False), cc(111, p), cc(105, fat), time.sleep(1.5), notes(True)))
        notes(False)
        stop = True
        return
    phase("idle HOME FAT0", lambda: None)
    chord = lambda on: [out.send(mido.Message("note_on" if on else "note_off", note=n, velocity=100 if on else 0)) for n in (60, 64, 67)]
    phase("chord(3) FAT0", lambda: (cc(105, 0), chord(True)))
    phase("chord(3) FAT4 (8 voices)", lambda: (chord(False), cc(105, 4), time.sleep(1.5), chord(True)))
    phase("8 notes", lambda: (chord(False), cc(105, 0), time.sleep(1.5), notes(True)))
    phase("8 notes DETUNE steps", lambda: None, lambda i: cc(106, (i * 4) % 128))
    phase("8 notes page cycle", lambda: None, lambda i: cc(102, (i + 1) % 4))
    phase("FAT4 HOME DETUNE steps", lambda: (notes(False), cc(102, 0), cc(105, 4), time.sleep(1.5), chord(True)), lambda i: cc(106, (i * 4) % 128))
    notes(False)
    cs = [parse(l) for t, l in lines if l.startswith("C ")]
    if cs:
        c = cs[-1]
        print("clock regs:", {k: hex(v) for k, v in c.items() if k != "ips"}, "ips ticks/20000 loops min", min(x["ips"] for x in cs))
    stop = True


if __name__ == "__main__":
    main()
