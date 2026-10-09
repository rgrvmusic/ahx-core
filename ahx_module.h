/* SPDX-License-Identifier: MIT */
/* Interface for opening, stopping and mixing one module. */
#ifndef AHX_MODULE_H
#define AHX_MODULE_H

#include <stdint.h>

/* Parse and start a module. Keep the input buffer valid until close or the next open.
 * Returns AHX_OK or a parser error. */
int ahx_module_open(const uint8_t *bytes, uint32_t size);

/* Stop playback. */
void ahx_module_close(void);

/* Return nonzero when a module is loaded and playing. */
int ahx_module_playing(void);

/* Add n stereo samples to the caller's mix buffers. Larger blocks are processed in AHX_BLOCK
 * chunks. */
void ahx_module_mix(int32_t *mix_l, int32_t *mix_r, uint32_t n);

#endif /* AHX_MODULE_H */
