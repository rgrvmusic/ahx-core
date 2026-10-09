AHX engine
==========


`ahx_voice.c` implements one channel's waveform, filter position, envelopes, panning and mix.
`ahx_player.c` handles the frame clock, tracks, effects and four voices. `ahx_module.c` connects
the module reader and player to a caller. The engine is based on Pete Gordon's HivelyTracker
replayer (`replay.c`, BSD-3-Clause); the applicable license is recorded per file.


## Waveform implementations

The voice interface has two build-time implementations:

- `ahx_tables.c` builds the reference waveform table and uses floating-point playback steps.
- `ahx_voice_fixed.c` generates one required waveform at a time and uses fixed-point playback
  steps. This implementation avoids the 410,760-byte table and floating-point math.

The host/reference tables are compared byte-for-byte with the upstream tables. The fixed-point
waveforms are compared across waveform variants and filter positions; their filter output can
differ from floating point by at most one int8 unit.


## Rendering

`ahx_player_frame()` renders one frame. `ahx_player_block()` renders arbitrary block sizes while
preserving tick and frame timing across calls. The block path is compared with frame rendering at
multiple rates and block sizes.

The host comparison tests render modules with both this engine and the reference replayer. In the
1,335-module corpus measured on 2026-10-09, 1,333 renders were identical; the reference exited
with a division-by-zero error on the other two. The device build uses the fixed-point filter and
may differ by the measured rounding described above.


## Reference behavior

The player preserves behavior that affects output, including the reference's waveform tables,
noise sequence, playback step, effects and panning. It guards cases that would otherwise read
outside the table or divide by a zero-length envelope. Ring modulation and wave lengths above five
are not implemented; AHX modules do not use those features.

