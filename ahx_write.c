/* SPDX-License-Identifier: MIT */
/* AHX-1: writing an AHX module. See ahx_write.h and docs/ahx-container.md.
 *
 * Every expression here is ahx_read.c's, inverted and put back at the offset the reader took it
 * from, which is why the two files are read together. The offsets come from the parse rather than
 * from arithmetic of this file's own: the reader walked the block chain once and recorded where
 * each instrument is (song->inst_off) and what the tracks' base is, and those are the places to
 * write. Where a field is split across bytes, the reader's shift and mask decide both halves.
 */
#include "ahx_write.h"

#include "ahx_libc.h"      /* memset, memcpy: <string.h> on the host, Felucca's libc.c on the device */

/* The reader's pointers, with the writability the caller promised. */
static uint8_t *wr(const uint8_t *p)
{
    return (uint8_t *)p;
}

/* Copy with the source and the destination allowed to overlap, which is the case a playlist
 * resize makes. The direction follows which end the two share. */
static void copy_over(uint8_t *dst, const uint8_t *src, size_t n)
{
    size_t i;

    if (dst <= src) {
        for (i = 0; i < n; i++) {
            dst[i] = src[i];
        }
    } else {
        for (i = n; i > 0; i--) {
            dst[i - 1] = src[i - 1];
        }
    }
}

int ahx_write_cell(uint8_t *bytes, const ahx_song_t *song, uint8_t track, uint8_t row,
                   ahx_cell_t cell)
{
    uint8_t *entry;
    unsigned block;

    if (row >= song->trl || track > song->trk) {
        return AHX_ERR_FIELD;
    }
    if (song->track0_absent) {
        if (track == 0) {
            return AHX_ERR_FIELD;  /* track 0 was empty, so the file does not hold it */
        }
        block = (unsigned)(track - 1);
    } else {
        block = track;
    }
    if (block >= song->stored_tracks) {
        return AHX_ERR_FIELD;
    }

    entry = wr(song->tracks) + ((size_t)block * song->trl + row) * AHX_CELL_BYTES;
    if ((const uint8_t *)entry + AHX_CELL_BYTES > bytes + song->size) {
        return AHX_ERR_SHORT;
    }

    entry[0] = (uint8_t)((cell.note << 2) | ((cell.instrument >> 4) & 0x03));
    entry[1] = (uint8_t)(((cell.instrument & 0x0f) << 4) | (cell.command & 0x0f));
    entry[2] = cell.data;
    return AHX_OK;
}

int ahx_write_position(uint8_t *bytes, const ahx_song_t *song, uint16_t position, unsigned channel,
                       uint8_t track, int8_t transpose)
{
    uint8_t *entry;

    if (position >= song->len || channel >= AHX_CHANNELS) {
        return AHX_ERR_FIELD;
    }
    entry = wr(song->positions) + (size_t)position * AHX_POS_BYTES + channel * 2;
    if ((const uint8_t *)entry + 2 > bytes + song->size) {
        return AHX_ERR_SHORT;
    }
    entry[0] = track;
    entry[1] = (uint8_t)transpose;
    return AHX_OK;
}

int ahx_write_subsong(uint8_t *bytes, const ahx_song_t *song, uint8_t index, uint16_t position)
{
    uint8_t *entry;

    if (index >= song->ss) {
        return AHX_ERR_FIELD;
    }
    entry = wr(song->subsongs) + (size_t)index * 2;
    if ((const uint8_t *)entry + 2 > bytes + song->size) {
        return AHX_ERR_SHORT;
    }
    entry[0] = (uint8_t)(position >> 8);
    entry[1] = (uint8_t)(position & 0xff);
    return AHX_OK;
}

int ahx_write_instrument(uint8_t *bytes, const ahx_song_t *song, uint8_t index,
                         const ahx_inst_t *inst)
{
    uint8_t *b;

    if (index == 0 || index > song->smp) {
        return AHX_ERR_FIELD;
    }
    b = wr(song->base) + song->inst_off[index];
    if ((const uint8_t *)b + AHX_INST_BYTES > bytes + song->size) {
        return AHX_ERR_SHORT;
    }

    b[0] = inst->volume;
    /* wave_len in bits 2-0, and the filter speed's low five bits above it. */
    b[1] = (uint8_t)((inst->wave_len & 0x07) | ((inst->filt_speed & 0x1f) << 3));
    b[2] = inst->attack_len;
    b[3] = inst->attack_vol;
    b[4] = inst->decay_len;
    b[5] = inst->decay_vol;
    b[6] = inst->sustain_len;
    b[7] = inst->release_len;
    b[8] = inst->release_vol;
    /* 9..11 are unused by the reader, so they stay as they are. */
    b[12] = (uint8_t)((inst->filt_lo & 0x7f) | ((inst->filt_speed & 0x20) << 2));
    b[13] = inst->vib_delay;
    b[14] = (uint8_t)((inst->relcut ? 0x80u : 0u) | ((inst->hardcut & 0x07) << 4) |
                      (inst->vib_depth & 0x0f));
    b[15] = inst->vib_speed;
    b[16] = inst->sqr_lo;
    b[17] = inst->sqr_hi;
    b[18] = inst->sqr_speed;
    /* The upper filter limit keeps its six bits; the byte's top two are not read. */
    b[19] = (uint8_t)((b[19] & 0xc0) | (inst->filt_hi & 0x3f));
    b[20] = inst->plist_speed;
    /* 21 is the playlist's length: ahx_write_plist_len() owns it. */
    return AHX_OK;
}

int ahx_write_step(uint8_t *bytes, const ahx_song_t *song, uint8_t index, uint8_t step,
                   ahx_step_t s)
{
    uint8_t *e;

    if (index == 0 || index > song->smp) {
        return AHX_ERR_FIELD;
    }
    if (step >= bytes[song->inst_off[index] + 21]) {
        return AHX_ERR_FIELD;          /* past the playlist, whose length is byte 21 */
    }
    e = wr(song->base) + song->inst_off[index] + AHX_INST_BYTES + (size_t)step * AHX_PLIST_BYTES;
    if ((const uint8_t *)e + AHX_PLIST_BYTES > bytes + song->size) {
        return AHX_ERR_SHORT;
    }

    /* The waveform's three bits are split: the top two ride in the first byte below the two
     * effects, and the bottom one is the second byte's top bit, above the fix-note flag. */
    e[0] = (uint8_t)(((s.fx2 & 0x07) << 5) | ((s.fx1 & 0x07) << 2) | ((s.waveform >> 1) & 0x03));
    e[1] = (uint8_t)((s.fix_note ? 0x40u : 0u) | ((s.waveform & 0x01) << 7) | (s.note & 0x3f));
    e[2] = s.fx1_data;
    e[3] = s.fx2_data;
    return AHX_OK;
}

int ahx_write_header(uint8_t *bytes, const ahx_song_t *song, uint8_t speed, uint16_t res)
{
    if (song->size < AHX_HEADER_BYTES) {
        return AHX_ERR_SHORT;
    }
    /* SPEED is bits 6-5 of byte 6, and the byte also holds whether track 0 was omitted, the
     * length's high nibble and one bit the reader does not read. */
    bytes[6] = (uint8_t)((bytes[6] & 0x9f) | ((speed & 0x03) << 5));
    bytes[8] = (uint8_t)(res >> 8);
    bytes[9] = (uint8_t)(res & 0xff);
    return AHX_OK;
}

int ahx_write_plist_len(uint8_t *bytes, size_t cap, const ahx_song_t *song, uint8_t index,
                        uint8_t len, size_t *size)
{
    size_t at, old_end, new_end, tail, new_size;
    uint16_t title;
    int32_t shift;

    if (index == 0 || index > song->smp) {
        return AHX_ERR_FIELD;
    }
    at = song->inst_off[index] + AHX_INST_BYTES;
    old_end = at + (size_t)bytes[song->inst_off[index] + 21] * AHX_PLIST_BYTES;
    if (old_end > song->size) {
        return AHX_ERR_SHORT;
    }
    new_end = at + (size_t)len * AHX_PLIST_BYTES;
    new_size = new_end + (song->size - old_end);
    if (new_size > cap) {
        return AHX_ERR_ROOM;
    }

    /* Everything after this playlist moves: the instruments below it, the names, and any bytes
     * after the last name, which are the module's to keep. */
    tail = song->size - old_end;
    copy_over(bytes + new_end, bytes + old_end, tail);
    if (new_end > old_end) {
        memset(bytes + old_end, 0, new_end - old_end);   /* a new step is an empty one */
    }
    bytes[song->inst_off[index] + 21] = len;

    /* The title offset is the module's only absolute offset. It is stored, not derived, and the
     * corpus holds a few modules whose stored value does not agree with the walk; one that does
     * not point into the block that moved is left as it is. */
    shift = (int32_t)(new_end - old_end);
    title = (uint16_t)((bytes[4] << 8) | bytes[5]);
    if (shift != 0 && title >= old_end && title <= song->size) {
        title = (uint16_t)((int32_t)title + shift);
        bytes[4] = (uint8_t)(title >> 8);
        bytes[5] = (uint8_t)(title & 0xff);
    }

    *size = new_size;
    return AHX_OK;
}
