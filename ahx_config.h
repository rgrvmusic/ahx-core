/* SPDX-License-Identifier: MIT */
/* Compile-time audio settings. Override these when building for a different target. */
#ifndef AHX_CONFIG_H
#define AHX_CONFIG_H

/* Output sample rate in Hz. */
#ifndef AHX_RATE
#define AHX_RATE 44100u
#endif

/* Maximum number of samples processed per module-mix chunk. */
#ifndef AHX_BLOCK
#define AHX_BLOCK 32u
#endif

/* Samples per AHX frame at 50 Hz. */
#define AHX_FRAME_SAMPLES (AHX_RATE / 50u)

#endif /* AHX_CONFIG_H */
