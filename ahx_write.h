/* SPDX-License-Identifier: MIT */
/* AHX module editor: the inverse of ahx_read.c. */
#ifndef AHX_WRITE_H
#define AHX_WRITE_H

#include "ahx.h"

/* Every function here patches a module in place, at the offset ahx_read() derived it from, so
 * `bytes` is the writable copy of `song->base` and `song` is the parse of that same buffer. The
 * bytes a caller is not writing are left alone, including the ones the reader does not read, which
 * is what makes a decoded structure write back to the bytes it came from unchanged.
 *
 * Only ahx_write_plist_len() moves anything, and after it the song struct is stale: re-read the
 * module before using it again.
 */

/* One track cell. Returns AHX_ERR_FIELD for a track or row the module does not hold. */
int ahx_write_cell(uint8_t *bytes, const ahx_song_t *song, uint8_t track, uint8_t row,
                   ahx_cell_t cell);

/* One channel's track and transpose within a position. */
int ahx_write_position(uint8_t *bytes, const ahx_song_t *song, uint16_t position, unsigned channel,
                       uint8_t track, int8_t transpose);

/* One subsong's start position. */
int ahx_write_subsong(uint8_t *bytes, const ahx_song_t *song, uint8_t index, uint16_t position);

/* An instrument's fixed 22 bytes. The playlist's length is byte 21 and belongs to
 * ahx_write_plist_len(), so it is not written here; filt_speed's bit 6 is not written either,
 * because the reader does not read it (byte 19's bit 7) and neither does the reference replayer. */
int ahx_write_instrument(uint8_t *bytes, const ahx_song_t *song, uint8_t index,
                         const ahx_inst_t *inst);

/* One playlist step. */
int ahx_write_step(uint8_t *bytes, const ahx_song_t *song, uint8_t index, uint8_t step,
                   ahx_step_t s);

/* The two header fields that move nothing: SPEED and the restart position. */
int ahx_write_header(uint8_t *bytes, const ahx_song_t *song, uint8_t speed, uint16_t res);

/* Resize an instrument's playlist, moving what follows it: the instruments after this one, the
 * name block, and the stored title offset, which is the module's only absolute offset. `cap` is
 * what the buffer holds, `size` is the module's length on the way in and out, and new steps are
 * empty. Returns AHX_ERR_ROOM when it would not fit. */
int ahx_write_plist_len(uint8_t *bytes, size_t cap, const ahx_song_t *song, uint8_t index,
                        uint8_t len, size_t *size);

#endif /* AHX_WRITE_H */
