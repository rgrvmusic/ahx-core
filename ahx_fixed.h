/* SPDX-License-Identifier: MIT */
/* SPDX-License-Identifier: GPL-3.0-only */
/* Fixed-point waveform generation, filtering and playback-step functions. */
#ifndef AHX_FIXED_H
#define AHX_FIXED_H

#include <stdint.h>

#include "ahx_voice.h"

/* Return the 16.16 playback step for a period and output rate. */
uint32_t ahx_fixed_delta(uint16_t period, uint32_t frequency);

/* Advance the white-noise random state. */
int32_t ahx_wn_next(int32_t state);

/* Filter an unfiltered waveform in place. Returns the number of samples written. */
uint32_t ahx_fixed_variant(int8_t *buf, uint32_t variant, int filter_pos, uint32_t count);

#endif /* AHX_FIXED_H */
