// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
// The browser build (build/emu/felucca.wasm, web/emu/felucca_web.c) in Node, as the worklet runs it. After X0X's
// tests/host/emu_test.mjs (charlesvestal/fm1-x0x, GPL-3.0).
//   node web/emu/emu_test.mjs build/emu/felucca.wasm [NATIVE.f32]
// Checks: it boots (the KOSMO screen; the power-on LED sweep under the splash, over by 1 s), a held key sounds, MIDI
// plays, OCT-/OCT+ shift the octave, the MASTER pot at 0 is silent; the same gestures twice give the same audio
// (deterministic), with NATIVE.f32 (web/emu/native_check.c) the native build's, bit for bit.
// Then the cost: 1 s of a heavy chord (eight voices, the reverb) against real time (BENCH_MAX, default 0.5: fails
// above half of real time on this machine).
import fs from "fs";

const wasmPath = process.argv[2] || "build/emu/felucca.wasm", nativePath = process.argv[3];
const bytes = fs.readFileSync(wasmPath);
const B = { FX: 0, SCL: 1, ENV: 2, LFO: 3, EDIT: 4, GLO: 5, HOME: 6, SAVE: 7, ARP: 8, SEQ: 9, PLAY: 10, REC: 11, OCTDN: 12, OCTUP: 13 };
let fails = 0;
const check = (what, ok) => { console.log(`emu: ${what.padEnd(76)} ${ok ? "ok" : "FAIL"}`); if (!ok) fails++; };

async function device() {
  const { instance } = await WebAssembly.instantiate(bytes, {});
  const ex = instance.exports, mem = ex.memory;
  ex._initialize();
  ex.web_nor_erase();
  ex.web_boot();
  const d = { ex, mem, peak: 0, sig: 0, rec: false, frames: 0, kept: [] };
  d.render = (ms) => {
    const n = Math.round(ms * 44.1);
    for (let k = 0; k < n; k += 128) {
      const m = Math.min(128, n - k);
      ex.web_render(m);
      const l = new Float32Array(mem.buffer, ex.web_out_l(), m), r = new Float32Array(mem.buffer, ex.web_out_r(), m);
      for (let i = 0; i < m; i++) {
        d.peak = Math.max(d.peak, Math.abs(l[i]), Math.abs(r[i]));
        if (d.rec) {
          d.sig = (d.sig * 31 + Math.round(l[i] * 32768) + 7 * Math.round(r[i] * 32768)) % 1000000007;
          d.kept.push(l[i], r[i]);
        }
      }
      d.frames += m;
    }
  };
  d.tap = (b) => { ex.web_buttons(1 << b); d.render(60); ex.web_buttons(0); d.render(60); };
  Object.defineProperty(d, "samples", { get: () => new Float32Array(d.kept) });
  d.screen = () => new Uint16Array(mem.buffer, ex.web_screen(), 240 * 240).slice();
  return d;
}
const colours = (fb) => new Set(fb).size;

// ---- boot, keys, MIDI, octave, MASTER, LEDs
const a = await device();
a.render(200);
const splash = colours(a.screen());
{ // the power-on LED sweep (hal/fm1_led_anim.h) under the splash: at 200 ms its head on the middle of the keyboard, the
  // keys it passed glowing (DIM HI), the ones ahead dark, the buttons not yet; the UI's LEDs untouched until it ends
  const p = a.ex.web_anim_levels(), lv = p ? Array.from(new Uint8Array(a.mem.buffer, p, 41)) : [];
  const keys = lv.slice(14), head = keys.indexOf(Math.max(...keys.map((q) => q & 127)) | 128);
  check(`power-on LED sweep: at 200 ms the head on key ${head}, glow behind, dark ahead, buttons dark`,
        lv.length === 41 && head >= 8 && head <= 18 && keys.slice(0, head - 6).every((q) => q === 128) &&
        keys.slice(head + 3).every((q) => q === 0) && lv.slice(0, 14).every((q) => q === 0) &&
        a.ex.web_lit_keys() === 0 && a.ex.web_lit_buttons() === 0);
}
a.render(800);
check("  over by 1 s (~0.70 s)", a.ex.web_anim_levels() === 0);
const home = a.screen();
check(`boots: the KOSMO screen (${colours(home)} colours)`, splash > 1 && colours(home) > 1);
check("silent at rest", a.peak === 0);
a.ex.web_keys(1 << 7);                                        // C4
a.render(300);
a.ex.web_keys(0);
a.render(400);
check(`a key sounds (peak ${a.peak.toFixed(3)})`, a.peak > 0.01);
a.render(1500);
a.peak = 0;
a.ex.web_midi(0x90, 64, 100);                                  // Web MIDI: E4 on channel 1
a.render(300);
a.ex.web_midi(0x80, 64, 0);
a.render(300);
check(`a MIDI note in (USB-MIDI's queue) sounds (peak ${a.peak.toFixed(3)})`, a.peak > 0.01);
check("octave 0 at power-on", a.ex.web_test_octave() === 8);
a.tap(B.OCTUP);
a.tap(B.OCTUP);
check("OCT+ twice: +2", a.ex.web_test_octave() === 10);
a.ex.web_buttons((1 << B.OCTDN) | (1 << B.OCTUP));
a.render(60);
a.ex.web_buttons(0);
a.render(60);
check("OCT- and OCT+ together: back to 0", a.ex.web_test_octave() === 8);
a.ex.web_master(0);
a.render(1500);                                                 // (the pot is followed 1/8 a frame)
a.peak = 0;
a.ex.web_keys(1 << 7);
a.render(300);
a.ex.web_keys(0);
a.render(200);
check(`MASTER at 0 is silent (peak ${a.peak.toFixed(4)})`, a.peak < 0.0005);
a.ex.web_master(700);
{ // the page's breathing peaks against the firmware's (hal/fm1_input.h FM1_LED_BREATH_PK / _LO, /256 of lit): the
  // colour at 50 % between the dark and the lit one by the LED's share, gamma-encoded (sRGB), within 0.06
  const html = fs.readFileSync(new URL("./index.html", import.meta.url), "utf8");
  const hal = fs.readFileSync(new URL("../../firmware/hal/fm1_input.h", import.meta.url), "utf8");
  const pk = (n) => +hal.match(new RegExp(`#define ${n} (\\d+)u`))[1] / 256;
  const srgb = (x) => x <= 0.0031308 ? 12.92 * x : 1.055 * x ** (1 / 2.4) - 0.055;
  const red = (c) => parseInt(c.slice(1, 3), 16);
  const share = (name, lit) => {                        // (the red channel: the peak's share of dark .. lit)
    const m = html.match(new RegExp(`@keyframes ${name} \\{ 0%, 100% \\{ fill: (#[0-9a-f]{6}); \\} 50% \\{ fill: (#[0-9a-f]{6})`));
    return (red(m[2]) - red(m[1])) / (lit - red(m[1]));
  };
  const hi = srgb(pk("FM1_LED_BREATH_PK")), lo = srgb(pk("FM1_LED_BREATH_PK_LO")), litBtn = 0xff, litSlit = 0xec;
  const got = [share("breath-btn", litBtn), share("breath-slit", litSlit), share("breath-btn-lo", litBtn), share("breath-slit-lo", litSlit)];
  check(`the page's breath peaks ${got.map((x) => x.toFixed(2)).join(" ")} ~ ${hi.toFixed(2)} (HI) ${lo.toFixed(2)} (LO)`,
        Math.abs(got[0] - hi) <= 0.06 && Math.abs(got[1] - hi) <= 0.06 && Math.abs(got[2] - lo) <= 0.06 &&
        Math.abs(got[3] - lo) <= 0.06);
}

// ---- deterministic: the same gestures, the same samples
async function song() {
  const d = await device();
  d.render(500);
  d.ex.web_test_heavy();
  d.rec = true;
  d.render(2000);
  return d;
}
const s1 = await song(), s2 = await song();
check(`the heavy chord plays (peak ${s1.peak.toFixed(3)}), twice the same samples`, s1.peak > 0.05 && s1.sig === s2.sig);
if (nativePath) {                                             // web/emu/native_check.c: the same song, built with cc
  const nat = new Float32Array(new Uint8Array(fs.readFileSync(nativePath)).buffer);
  const web = s1.samples;
  let diff = nat.length === web.length ? 0 : Infinity;
  for (let i = 0; i < web.length && diff === 0; i++) if (nat[i] !== web[i]) diff = i + 1;
  check(`the same chord from the native build (cc): ${web.length / 2} frames, ${diff === 0 ? "bit for bit" : "differs at " + diff}`, diff === 0);
}

// ---- the cost: 1 s of the heavy chord against real time (the best of 3, after 2 s to settle)
let best = Infinity;
s1.rec = false;
for (let i = 0; i < 3; i++) {
  const t0 = performance.now();
  s1.render(1000);
  best = Math.min(best, performance.now() - t0);
}
const ratio = best / 1000, max = +(process.env.BENCH_MAX || 0.5);
check(`1 s of the heavy chord (8 voices + reverb) in ${best.toFixed(1)} ms: ${(ratio * 100).toFixed(1)} % of real time`, ratio < max);
console.log(fails ? `emu: ${fails} FAILED` : "emu: all ok");
process.exit(fails ? 1 : 0);
