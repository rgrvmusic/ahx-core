Regrooved AHX core
==================


AHX is the tracker format used by HivelyTracker. This C library reads AHX modules and renders
their audio. It also provides a small note-playing interface for callers that do not need a
module's tracks or transport.

The core is self-contained and uses the C standard library. It is used by the AHX-1 firmware and
can also be built as a standalone library.


## Build

```sh
make
make check
```

By default, the build uses the fixed-point voice implementation for devices without an FPU.
To build with the reference floating-point waveform table instead:

```sh
make clean
make VOICE=ahx_tables.c
```

The host build of `ahx_tables.c` uses `sin()` and `cos()` and may need `-lm` when linking the
library.


## Note demo

`ahx_note_demo` renders a sequence of notes to a 16-bit mono WAV file:

```sh
./ahx_note_demo -o out.wav c3 e3 g3 c4
./ahx_note_demo -w 3 -l 3 f2:800 g2:400
./ahx_note_demo -r 48000 -v 32 a3
```

Notes may be MIDI numbers or names, with an optional duration in milliseconds. Options select
the waveform (`-w`), wave length (`-l`), pulse width (`-p`), filter (`-f`), volume (`-v`) and
sample rate (`-r`). The core does not apply an amplitude envelope; callers provide their own.


## Implementations

`ahx_tables.c` uses the reference's waveform table and floating-point playback step.
`ahx_voice_fixed.c` generates the required waveform in shared scratch space and uses a fixed-point
step. The rest of the voice and module player is shared.

The module reader is zero-copy: module data remains in the caller's buffer. See
[`docs/ahx-container.md`](docs/ahx-container.md) for format details and
[`docs/ahx-engine.md`](docs/ahx-engine.md) for implementation and comparison notes.


## License

Files are licensed under MIT or MIT AND BSD-3-Clause, as marked by their SPDX identifiers.
The BSD-3-Clause files include code ported from Pete Gordon's HivelyTracker replayer; their
license text is in [`LICENSES/BSD-3-Clause.txt`](LICENSES/BSD-3-Clause.txt).
