#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Boot build/kosmo.bin headless in fm1-emulator, press a piano key, snapshot after it.
# Passes if the LCD is visible and audio_nonzero_words > 0. See fm1-emulator tools/panel_smoke.py.
#   FM1_EMULATOR  emulator binary (default: ~/src/fm1-emulator/emulator)
set -e
cd "$(dirname "$0")/.."
EMU="${FM1_EMULATOR:-$HOME/src/fm1-emulator/emulator}"
FW="${1:-build/kosmo.bin}"
[ -x "$EMU" ] || { echo "emu_smoke: no emulator at $EMU"; exit 1; }
[ -f "$FW" ] || { echo "emu_smoke: no firmware at $FW (run ./build.sh)"; exit 1; }
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
# Times in guest ns: HOME is reached by ~17 s; key "z" is piano C (fm1-controls.h).
FM1_POC_STATE_DIR="$D" FM1_POC_MAX_INSTRUCTIONS=10000000000000 \
FM1_POC_INPUT="20000000000:z:1,20300000000:z:0" \
FM1_POC_SNAPSHOT_NS="20750000000" FM1_POC_CAPTURE_NS="21000000000" \
"$EMU" --qemu -M fm1-poc -smp 2 -accel tcg,thread=single -icount shift=3,align=off,sleep=off \
    -display none -chardev null,id=console -serial chardev:console -monitor none -nodefaults \
    -no-user-config -kernel "$FW" -append application 2>"$D/stderr" || true
S="$D/snapshots.jsonl"
[ -s "$S" ] || { echo "FAIL: no snapshot taken"; cat "$D/stderr"; exit 1; }
cat "$S"
rg -q '"visible":true' "$S" || { echo "FAIL: LCD not visible"; exit 1; }
rg -q '"audio_nonzero_words":[1-9]' "$S" || { echo "FAIL: no audio after key press"; exit 1; }
echo "PASS"
