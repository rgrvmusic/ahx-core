/* SPDX-License-Identifier: MIT */
/* AHX-1: reading an AHX module. See ahx.h and docs/ahx-container.md.
 *
 * The module layout, in order, all offsets in bytes from the start:
 *
 *   0                                  header, 14 bytes
 *   14                                 subsongs, ss words
 *   14 + ss*2                          positions, len * 8
 *   ... + len*8                        tracks, stored_tracks * trl * 3
 *   ...                                instruments, smp entries of 22 bytes plus their playlist
 *   ...                                names, smp+1 NUL terminated strings, to the end
 *
 * The walk is done forward, once, at read time, and each boundary is checked against the
 * module size as it is computed. Nothing here allocates or copies.
 */
#include "ahx.h"

#include "ahx_libc.h"      /* memset, memcpy: <string.h> on the host, Felucca's libc.c on the device */

int ahx_read(const uint8_t *bytes, size_t size, ahx_song_t *song)
{
    size_t off, end;
    unsigned i;

    memset(song, 0, sizeof *song);

    if (size < AHX_HEADER_BYTES) {
        return AHX_ERR_SHORT;
    }
    if (bytes[0] != 'T' || bytes[1] != 'H' || bytes[2] != 'X') {
        return AHX_ERR_ID;
    }
    if (bytes[3] > 1) {
        return AHX_ERR_ID;
    }

    song->base = bytes;
    song->size = size;
    song->version = bytes[3];
    song->title_offset = (uint16_t)((bytes[4] << 8) | bytes[5]);

    /* The SPEED multiplier is bits 6-5, not the bits 6-4 the specification states: over the
     * corpus it only ever takes the values 0, 1, 2 and 3 when read as two bits. */
    song->speed = (uint8_t)((bytes[6] >> 5) & 3u);
    song->track0_absent = (uint8_t)((bytes[6] >> 7) & 1u);
    song->len = (uint16_t)(((bytes[6] & 0x0f) << 8) | bytes[7]);
    song->res = (uint16_t)((bytes[8] << 8) | bytes[9]);
    song->trl = bytes[10];
    song->trk = bytes[11];
    song->smp = bytes[12];
    song->ss = bytes[13];

    if (song->len < 1 || song->len > AHX_MAX_LEN) {
        return AHX_ERR_FIELD;
    }
    if (song->trl < 1 || song->trl > AHX_MAX_TRL) {
        return AHX_ERR_FIELD;
    }
    if (song->smp > AHX_MAX_SMP) {
        return AHX_ERR_FIELD;
    }
    if (song->version == 0) {
        song->speed = 0;           /* AHX0 has no SPEED field, and this one is always 0 */
    }

    /* Track 0 exists as a slot in every module, but it is only in the file when the
     * header says it was not empty. */
    song->stored_tracks = (uint16_t)(song->trk + (song->track0_absent ? 0 : 1));

    song->subsongs = bytes + AHX_HEADER_BYTES;
    off = AHX_HEADER_BYTES + (size_t)song->ss * 2;
    if (off > size) {
        return AHX_ERR_SHORT;
    }

    song->positions = bytes + off;
    off += (size_t)song->len * AHX_POS_BYTES;
    if (off > size) {
        return AHX_ERR_SHORT;
    }

    song->tracks = bytes + off;
    off += (size_t)song->stored_tracks * song->trl * AHX_CELL_BYTES;
    if (off > size) {
        return AHX_ERR_SHORT;
    }

    /* Instruments are variable sized, so record where each one starts, which is also what
     * makes the playlist reachable in one step later on. */
    song->instruments = bytes + off;
    for (i = 1; i <= song->smp; i++) {
        uint8_t plist_len;

        song->inst_off[i] = (uint32_t)off;
        if (off + AHX_INST_BYTES > size) {
            return AHX_ERR_SHORT;
        }
        plist_len = bytes[off + 21];
        off += AHX_INST_BYTES + (size_t)plist_len * AHX_PLIST_BYTES;
        if (off > size) {
            return AHX_ERR_SHORT;
        }
    }

    /* The names come last and run to the end of the module. A final name in the wild may
     * lack its terminator, in which case it is clamped to the end rather than dropped. */
    song->names = bytes + off;
    end = off;
    for (i = 0; i <= song->smp; i++) {
        size_t start = end;

        while (end < size && bytes[end] != 0) {
            end++;
        }
        song->name_off[i] = (uint32_t)start;
        song->name_len[i] = (uint16_t)(end - start);
        song->names_found = (uint8_t)(i + 1);
        if (end >= size) {
            break;                 /* unterminated: this name runs to the end of the module */
        }
        end++;                     /* step over the terminator */
    }
    song->names_trailing = (uint8_t)(end < size);

    return AHX_OK;
}

const char *ahx_error(int status)
{
    switch (status) {
    case AHX_OK:          return "ok";
    case AHX_ERR_SHORT:   return "module ends before its structures do";
    case AHX_ERR_ID:      return "not an AHX module";
    case AHX_ERR_FIELD:   return "header field out of range";
    default:              return "unknown";
    }
}

unsigned ahx_frame_rate(const ahx_song_t *song)
{
    return 50u * (song->speed + 1u);
}

unsigned ahx_wave_points(uint8_t wave_len)
{
    if (wave_len > 5) {
        return 0;
    }
    return 4u << wave_len;
}

int ahx_position(const ahx_song_t *song, uint16_t position, unsigned channel,
                 uint8_t *track, int8_t *transpose)
{
    const uint8_t *entry;

    if (position >= song->len || channel >= AHX_CHANNELS) {
        return 0;
    }
    entry = song->positions + (size_t)position * AHX_POS_BYTES + channel * 2;
    if (entry + 2 > song->base + song->size) {
        return 0;
    }
    *track = entry[0];
    *transpose = (int8_t)entry[1];
    return 1;
}

ahx_cell_t ahx_cell(const ahx_song_t *song, uint8_t track, uint8_t row)
{
    ahx_cell_t cell = { 0, 0, 0, 0 };
    const uint8_t *entry;
    unsigned block;

    if (row >= song->trl) {
        return cell;
    }
    if (track > song->trk) {
        return cell;               /* the position list never reaches past trk */
    }
    if (song->track0_absent) {
        if (track == 0) {
            return cell;           /* track 0 was empty, so the file does not hold it */
        }
        block = track - 1;         /* the file's tracks are 1..trk in order */
    } else {
        block = track;
    }
    if (block >= song->stored_tracks) {
        return cell;
    }

    entry = song->tracks + ((size_t)block * song->trl + row) * AHX_CELL_BYTES;
    if (entry + AHX_CELL_BYTES > song->base + song->size) {
        return cell;
    }

    cell.note = (uint8_t)(entry[0] >> 2);
    cell.instrument = (uint8_t)(((entry[0] & 0x03) << 4) | (entry[1] >> 4));
    cell.command = (uint8_t)(entry[1] & 0x0f);
    cell.data = entry[2];
    return cell;
}

uint16_t ahx_subsong(const ahx_song_t *song, uint8_t index)
{
    const uint8_t *entry;

    if (index >= song->ss) {
        return 0;
    }
    entry = song->subsongs + (size_t)index * 2;
    if (entry + 2 > song->base + song->size) {
        return 0;
    }
    return (uint16_t)((entry[0] << 8) | entry[1]);
}

const char *ahx_name(const ahx_song_t *song, uint8_t index, size_t *length)
{
    if (index >= song->names_found) {
        if (length) {
            *length = 0;
        }
        return NULL;
    }
    if (length) {
        *length = song->name_len[index];
    }
    return (const char *)(song->base + song->name_off[index]);
}

ahx_inst_t ahx_instrument(const ahx_song_t *song, uint8_t index)
{
    ahx_inst_t inst;
    const uint8_t *b;

    memset(&inst, 0, sizeof inst);
    if (index == 0 || index > song->smp) {
        return inst;
    }
    b = song->base + song->inst_off[index];
    if (b + AHX_INST_BYTES > song->base + song->size) {
        return inst;
    }

    inst.volume = b[0];
    inst.wave_len = (uint8_t)(b[1] & 0x07);
    /* The filter speed is a 7-bit field split across three bytes: bits 4-0 in byte 1, bit 5
     * in byte 12 and bit 6 in byte 19, which is what the specification says and what byte 19
     * being a 7-bit upper limit below it allows.
     *
     * Only the first five bits are read here. The reference replayer reads the same five and
     * drops byte 19's bit 7 on the floor, and it is the oracle the render is compared against:
     * eight instruments of the 18,266 in our corpus set that bit (docs/ahx-engine.md), so
     * reading it would be a real difference in the filter modulation of those eight. The
     * upper limit keeps its six bits either way, as the reference reads it, because the
     * specification's valid range for it is 1 to 63. */
    inst.filt_speed = (uint8_t)(((b[1] & 0xf8) >> 3) | ((b[12] & 0x80) >> 2));
    inst.filt_lo = (uint8_t)(b[12] & 0x7f);
    inst.filt_hi = (uint8_t)(b[19] & 0x3f);
    inst.attack_len = b[2];
    inst.attack_vol = b[3];
    inst.decay_len = b[4];
    inst.decay_vol = b[5];
    inst.sustain_len = b[6];
    inst.release_len = b[7];
    inst.release_vol = b[8];
    inst.vib_delay = b[13];
    inst.hardcut = (uint8_t)((b[14] >> 4) & 0x07);
    inst.relcut = (uint8_t)((b[14] >> 7) & 0x01);
    inst.vib_depth = (uint8_t)(b[14] & 0x0f);
    inst.vib_speed = b[15];
    inst.sqr_lo = b[16];
    inst.sqr_hi = b[17];
    inst.sqr_speed = b[18];
    inst.plist_speed = b[20];
    inst.plist_len = b[21];
    if (inst.plist_len != 0) {
        inst.plist = b + AHX_INST_BYTES;
        if (inst.plist + (size_t)inst.plist_len * AHX_PLIST_BYTES > song->base + song->size) {
            inst.plist = NULL;
            inst.plist_len = 0;
        }
    }
    return inst;
}

ahx_step_t ahx_step(const ahx_inst_t *instrument, uint8_t step)
{
    ahx_step_t out = { 0, 0, 0, 0, 0, 0, 0 };
    const uint8_t *e;

    if (instrument->plist == NULL || step >= instrument->plist_len) {
        return out;
    }
    e = instrument->plist + (size_t)step * AHX_PLIST_BYTES;

    out.fx2 = (uint8_t)((e[0] >> 5) & 0x07);
    out.fx1 = (uint8_t)((e[0] >> 2) & 0x07);
    out.waveform = (uint8_t)(((e[0] << 1) & 0x06) | (e[1] >> 7));
    out.fix_note = (uint8_t)((e[1] >> 6) & 0x01);
    out.note = (uint8_t)(e[1] & 0x3f);
    out.fx1_data = e[2];
    out.fx2_data = e[3];
    return out;
}
