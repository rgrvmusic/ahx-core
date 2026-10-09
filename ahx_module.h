/* SPDX-License-Identifier: MIT */
/* Interface for opening, stopping and mixing one module. */
#ifndef AHX_MODULE_H
#define AHX_MODULE_H

#include <stdint.h>

#include "ahx.h"
#include "ahx_player.h"

/* Parse and start a module. Keep the input buffer valid until close or the next open.
 * Returns AHX_OK or a parser error. */
int ahx_module_open(const uint8_t *bytes, uint32_t size);

/* Stop playback. */
void ahx_module_close(void);

/* Return nonzero when a module is loaded and playing. */
int ahx_module_playing(void);

/* Start one of the loaded module's subsongs. Index 0 is the song's own start and the rest are
 * 1-based (ahx_player_subsong), so the index a caller shows is this one plus one. Returns the
 * player's own status, and AHX_ERR_FIELD when nothing is loaded. */
int ahx_module_subsong(unsigned index);

/* The loaded module and its transport, for a caller that shows either, or NULL when nothing is
 * loaded. Both are the player's own structures: read them, do not write them, and do not hold
 * the pointer across a close or a later open. A screen wants ahx_name() of the song and the
 * player's pos_nr / note_nr / song_end_reached, which is the whole of what this pair is for. */
const ahx_song_t *ahx_module_song(void);
const ahx_player_t *ahx_module_transport(void);

/* Add n stereo samples to the caller's mix buffers. Larger blocks are processed in AHX_BLOCK
 * chunks. */
void ahx_module_mix(int32_t *mix_l, int32_t *mix_r, uint32_t n);

#endif /* AHX_MODULE_H */
