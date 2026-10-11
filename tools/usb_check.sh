#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The USB host path must work at every optimisation level: build the CDC stats firmware (KOSMO_CDC=1
# KOSMO_PERF=2) at -Os and -O2, boot each in fm1-emulator with the CDC console on stdio, and require the
# "P seq=N" lines, which only get out when USB enumerated and the host opened the port. Ends with a plain
# ./build.sh so build/kosmo.bin is the shipping image again. (The emulator has no USB MIDI.)
#   FM1_EMULATOR  emulator binary (default: ~/src/fm1-emulator/emulator)
set -e
cd "$(dirname "$0")/.."
EMU="${FM1_EMULATOR:-$HOME/src/fm1-emulator/emulator}"
[ -x "$EMU" ] || { echo "usb_check: no emulator at $EMU"; exit 1; }
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
rc=0
for opt in -Os -O2; do
    KOSMO_OPT=$opt KOSMO_CDC=1 KOSMO_PERF=2 ./build.sh >"$D/build" 2>&1 || { cat "$D/build"; exit 1; }
    FM1_POC_STATE_DIR="$D" FM1_POC_MAX_INSTRUCTIONS=10000000000000 FM1_POC_CAPTURE_NS=8000000000 \
    "$EMU" --qemu -M fm1-poc -smp 2 -accel tcg,thread=single -icount shift=3,align=off,sleep=off \
        -display none -serial stdio -monitor none -nodefaults -no-user-config \
        -kernel build/kosmo.bin -append application </dev/null >"$D/out" 2>"$D/err" || true
    if [ "$(rg -c '^P seq=[0-9]+ ' "$D/out" || true)" -ge 3 ] 2>/dev/null; then
        echo "usb-check $opt: PASS"
    else
        echo "usb-check $opt: FAIL, the CDC console is silent"; tail -3 "$D/err"; rc=1
    fi
done
./build.sh >/dev/null
exit $rc
