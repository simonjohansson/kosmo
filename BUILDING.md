# Building Kosmo

The build makes three files in `build/`:

| File | What |
| --- | --- |
| `kosmo.bin` | the firmware app |
| `loader/ota.bin` | the update loader |
| `kosmo.fwsc` | the installable package (app + loader) |

## Prerequisites

Install [mise](https://mise.jdx.dev) and run:

```
mise install     # Python, uv, Node, clang, fribidi
mise run setup   # Pillow, fontTools, the JieLi toolchain (~/.jieli) and the AC79 SDK (build/deps)
mise run build   # ./build.sh
```

Tasks: `build:docker` (forces the `linux/amd64` container, checks Docker first), `test`, `smoke`,
`web` (browser emulator with Emscripten, task-scoped). External, not managed by mise:

- Docker (Docker Desktop, OrbStack, ...): required on macOS, since the JieLi toolchain is Linux x86-64
  only; the build runs each tool in a `debian:bookworm-slim` container. Keep the tree under `/Users`.
- The fm1-emulator binary for `smoke` (`FM1_EMULATOR`).

On Linux x86-64 the toolchain runs natively and Docker is not needed.

## Build

```
./build.sh
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations
(`~/.jieli/toolchain`, `~/fw-AC79_AIoT_SDK`).

`./build.sh --release 1.0` makes a release build: the package identity becomes `FM-1_910`
and the version string `v1.0`; the package is `build/kosmo-1.0.fwsc`, and
`build/release-1.0/` holds what a release ships: the package, the app
(`kosmo-1.0-app.bin`), `SHA256SUMS`, `LICENSE`, `LICENSING.md` and
`LICENSES/` (the package contains Apache-2.0 SDK files, so the licence texts travel with it).

Build options (environment, `0` or `1`; defaults in `firmware/src/kosmo.c`):

| Flag | Default | |
| --- | --- | --- |
| `KOSMO_FLASH` | 1 | the settings in flash |
| `KOSMO_OTA` | 1 | update entry (needs `KOSMO_FLASH`) |
| `KOSMO_CDC` | 0 | USB serial function (the console is gone; the descriptors stay) |
| `KOSMO_CDC_DEFAULT` | 1 | with `KOSMO_CDC=1`: `0` leaves the serial function out of USB from boot |
| `KOSMO_UAC` | 1 | USB audio input (the master output, stereo, 44.1 or 48 kHz) |
| `KOSMO_UAC_48K` | 1 | `0`: the USB audio input at 44.1 kHz only, as up to 1.0.5.2 (`firmware/src/usb.c`) |
| `KOSMO_UAC_AS_FIRST` | 1 | the audio control header lists the audio streaming interface before the MIDI one, so the USB audio driver of older macOS (up to 15) finds it; `0`: 1.0 to 1.5's order, byte for byte (`firmware/src/usb.c`) |
| `KOSMO_UART` | 1 | TRS MIDI IN |

`KOSMO_USB_LAYOUT` (`0` to `3`, default `0`) picks other USB descriptor layouts for testing; see
`firmware/src/usb.c`.

## Browser emulator

```
./build.sh
web/emu/build.sh
python3 -m http.server -d build/emu 8790     # open http://localhost:8790/
```

`web/emu/kosmo_web.c` builds the firmware's sources as the host tests do, against a simulated FM-1, and
Emscripten compiles it to WebAssembly (`build/emu/kosmo.wasm`, with `index.html`, `worklet.js` and the font in `fonts/`). The page
keeps the flash's storage sectors (the settings) in the browser. Not emulated: USB (audio, MIDI out, updates) and
TRS MIDI.

## Tests

```
tests/run_tests.sh
```

Runs the host tests and, with Node.js and Emscripten, the browser emulator's. Run it after `./build.sh`
(it uses `build/` and needs `AC79_SDK` set as for the build). The suites cover flash storage, the settings, the keys
and knobs, MIDI (USB, TRS, bend, sustain, panic), USB audio, the update entry and loader, the command-line
installer, the fonts and palettes, and the DSP (the voices, the WAVE engine and its filter, the ROOM reverb, the
master).

The regression suite (`tests/regress.c`) renders the test sounds, waves, filter types, voice modes, reverb settings,
the keys and MIDI, and compares a hash of each render with `tests/golden.txt`; it also checks levels, voices and the CPU
cost (`tests/cpu_baseline.txt`, `tests/target_budget.txt`). After an intended change of
the sound, `GOLDEN_UPDATE=1 sh tests/run_tests.sh` rewrites the hashes; `BUDGET_UPDATE=1`
does the same for the cost files.

## Install

From the command line (needs `pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py build/kosmo.fwsc
python3 tools/fm1_install.py --info          # identity of the connected FM-1
```

Installing firmware is at your own risk. If an install fails and the FM-1 no longer
starts, recovery needs [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
