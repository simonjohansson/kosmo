# Kosmo

[![License: GPL-3.0-only](https://img.shields.io/badge/license-GPL--3.0--only-blue.svg)](LICENSE)

Kosmo is a synth firmware for kids, for the M-VAVE FM-1. It is a fork of
[Felucca](https://github.com/hugelton/Felucca) cut down to one engine (WAVE, a wavetable oscillator with its filter and ADSR envelope),
the ROOM reverb, the 27 keys, USB-MIDI and TRS MIDI in, the MASTER knob, and the update path. A new wavetable
oscillator, filter types, reverb and a kids' UI are to come on top.

Right now the screen says KOSMO, the keys play (OCT- and OCT+ shift them an octave), MIDI in plays, and the
MASTER knob sets the volume.

## Build and test

Needs [mise](https://mise.jdx.dev) and, on macOS, Docker (the JieLi toolchain is Linux only). See
[BUILDING.md](BUILDING.md).

```
mise run build    # build/kosmo.bin, the update loader and the installable package
mise run smoke    # boots build/kosmo.bin in fm1-emulator, plays a key, prints PASS
mise run test     # host tests (the DSP, MIDI, storage, USB, the update path, the browser emulator)
```

`mise run web` builds the browser emulator (`build/emu`).

## Licence and credits

Kosmo is derived from [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita (@kurogedelic), Hügelton
Instruments, and is free software under [GPL-3.0-only](LICENSE), as Felucca is. The bundled fonts keep their own
licences ([LICENSES/](LICENSES/)); details in [LICENSING.md](LICENSING.md). The browser emulator follows
[X0X](https://github.com/charlesvestal/fm1-x0x) by charlesvestal (GPL-3.0).

M-VAVE and FM-1 are trademarks of their respective owners. Kosmo is not affiliated with or endorsed by them.
