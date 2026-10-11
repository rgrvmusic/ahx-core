/* SPDX-License-Identifier: MIT */
/* AHX module format and reader interface. */
#ifndef AHX_H
#define AHX_H

#include <stdint.h>
#include <stddef.h>

/* Header magic: "THX" plus a version byte, 0 for AHX0 (AHX 1.00 to 1.27), 1 for AHX1
 * (AHX 2.0 and later). */
#define AHX_ID0 0x54485800u
#define AHX_ID1 0x54485801u

enum {
    AHX_MAX_LEN = 999,           /* position list length */
    AHX_MAX_TRL = 64,            /* rows in a track */
    AHX_MAX_SMP = 63,            /* instruments */
    AHX_HEADER_BYTES = 14,
    AHX_INST_BYTES = 22,         /* fixed part of an instrument, before its playlist */
    AHX_PLIST_BYTES = 4,         /* one playlist step */
    AHX_WAVE_MAX = 128,          /* points in the longest waveform */
    AHX_CELL_BYTES = 3,          /* bytes per track cell */
    AHX_POS_BYTES = 8,           /* bytes per position: 4 pairs of track and transpose */
    AHX_CHANNELS = 4,
    AHX_TRACK_BYTES = 192        /* 64 rows of 3 bytes: the largest a track can be */
};

/* AHX effect commands. Command 0 supplies the hundreds digit for command B. */
enum {
    AHX_CMD_JUMP_HI = 0x0,       /* data 1..9: the hundreds digit of the next B command */
    AHX_CMD_PITCH_UP = 0x1,
    AHX_CMD_PITCH_DOWN = 0x2,
    AHX_CMD_PITCH_PORTA = 0x3,   /* slide to the note in the same cell */
    AHX_CMD_FILTER = 0x4,        /* data 1..3f holds the filter, 41..7f moves it */
    AHX_CMD_VOLUME_PORTA = 0x5,
    AHX_CMD_PANNING = 0x7,
    AHX_CMD_SQUARE = 0x9,
    AHX_CMD_VOLUME_SLIDE = 0xA,
    AHX_CMD_POSITION_JUMP = 0xB,
    AHX_CMD_VOLUME = 0xC,        /* note, then playlist, then track master volume */
    AHX_CMD_ROW_BREAK = 0xD,
    AHX_CMD_EXTENDED = 0xE,      /* C0-CF note cut, D0-DF note delay */
    AHX_CMD_SPEED = 0xF          /* frames per row; data 0 ends the song */
};

/* The waveform a playlist step selects, from the playlist's 3 waveform bits. 0 holds the
 * waveform of the previous step. */
enum {
    AHX_WAVE_HOLD = 0,
    AHX_WAVE_TRIANGLE = 1,
    AHX_WAVE_SAWTOOTH = 2,
    AHX_WAVE_SQUARE = 3,
    AHX_WAVE_NOISE = 4
};

/* One decoded track cell: three bytes, bits 23-0. */
typedef struct {
    uint8_t note;                /* 0 none, else 1..60 (B-5) */
    uint8_t instrument;          /* 0..63 */
    uint8_t command;             /* 0..15 */
    uint8_t data;                /* 0..255 */
} ahx_cell_t;

/* One playlist step, from the instrument's variable-length playlist. */
typedef struct {
    uint8_t fx2;                 /* 0..7 */
    uint8_t fx1;                 /* 0..7 */
    uint8_t waveform;            /* one of AHX_WAVE_x, 0 holds the previous */
    uint8_t fix_note;            /* 1: the step's note does not transpose with the channel */
    uint8_t note;                /* 0 none, else 1..60 */
    uint8_t fx1_data;
    uint8_t fx2_data;
} ahx_step_t;

/* One decoded instrument. The playlist is not copied: plist points into the module. */
typedef struct {
    uint8_t volume;              /* 0..64 */
    uint8_t wave_len;            /* 0..5: the cycle is 4, 8, 16, 32, 64 or 128 points */
    uint8_t filt_speed;          /* 0..127, assembled from bytes 1, 12 and 19; 0 is off */
    uint8_t filt_lo;             /* 1..63 when the filter is used, else 0 */
    uint8_t filt_hi;             /* 1..63 when the filter is used, else 0 */
    uint8_t sqr_lo;              /* square modulation limits */
    uint8_t sqr_hi;
    uint8_t sqr_speed;
    uint8_t attack_len, attack_vol;
    uint8_t decay_len, decay_vol;
    uint8_t sustain_len;
    uint8_t release_len, release_vol;
    uint8_t vib_delay, vib_depth, vib_speed;
    uint8_t hardcut;             /* 0..7: frames after which the note is cut */
    uint8_t relcut;              /* 1: the hardcut also applies on release */
    uint8_t plist_speed;         /* frames per playlist step, 1..255 */
    uint8_t plist_len;           /* 0: an empty instrument, and it plays nothing */
    const uint8_t *plist;        /* plist_len steps of AHX_PLIST_BYTES, or NULL */
} ahx_inst_t;

/* Parsed module. Pointers refer into the input buffer, which must remain valid. */
typedef struct {
    const uint8_t *base;
    size_t size;

    uint8_t version;             /* 0 or 1 */
    uint16_t title_offset;       /* stored offset of the names block */
    uint8_t speed;               /* SPEED: frames multiplier 0..3, so 50, 100, 150, 200 Hz */
    uint16_t len;                /* positions, 1..999 */
    uint16_t res;                /* restart position */
    uint8_t trl;                 /* rows per track, 1..64 */
    uint8_t trk;                 /* highest track number */
    uint8_t smp;                 /* instruments */
    uint8_t ss;                  /* subsongs */

    /* Set when track 0 is empty and omitted from the file. */
    uint8_t track0_absent;
    uint16_t stored_tracks;      /* trk, plus one when track 0 is in the file at all */

    const uint8_t *subsongs;     /* ss * 2 bytes, big endian */
    const uint8_t *positions;    /* len * 8 bytes */
    const uint8_t *tracks;       /* stored_tracks * trl * 3 bytes */
    const uint8_t *instruments;  /* the start of the variable-sized instrument block */
    const uint8_t *names;        /* the name block, which runs to the end of the module */

    uint32_t inst_off[AHX_MAX_SMP + 1];  /* byte offset per instrument, index 1..smp */
    uint32_t name_off[AHX_MAX_SMP + 2];  /* name 0 is the title, then 1..smp */
    uint16_t name_len[AHX_MAX_SMP + 2];  /* readable bytes at name_off, without the NUL */
    uint8_t names_found;         /* names present and terminated inside the module */

    /* Set when bytes follow the last name. */
    uint8_t names_trailing;
} ahx_song_t;

enum {
    AHX_OK = 0,
    AHX_ERR_SHORT,               /* a structure runs past the end of the module */
    AHX_ERR_ID,                  /* not "THX" plus 0 or 1 */
    AHX_ERR_FIELD,               /* a header field is outside its valid range */
    AHX_ERR_ROOM                 /* a write would not fit the caller's buffer */
};

/* Parse a module. On error, the song struct is unusable. */
int ahx_read(const uint8_t *bytes, size_t size, ahx_song_t *song);

const char *ahx_error(int status);

/* The frame rate in Hz that SPEED asks for: 50, 100, 150 or 200. */
unsigned ahx_frame_rate(const ahx_song_t *song);

/* The waveform cycle length in points for a wave_len field: 4, 8, 16, 32, 64 or 128. */
unsigned ahx_wave_points(uint8_t wave_len);

/* Read one channel's track and transpose from a position. Returns 0 if out of range. */
int ahx_position(const ahx_song_t *song, uint16_t position, unsigned channel,
                 uint8_t *track, int8_t *transpose);

/* Decode a track cell. Invalid or missing tracks and rows return an empty cell. */
ahx_cell_t ahx_cell(const ahx_song_t *song, uint8_t track, uint8_t row);

/* The subsong's start position, or 0 when there is no subsong list. */
uint16_t ahx_subsong(const ahx_song_t *song, uint8_t index);

/* Return a title (index 0) or instrument name and its readable length. May not be NUL-terminated. */
const char *ahx_name(const ahx_song_t *song, uint8_t index, size_t *length);

/* Decode one instrument, 1..smp. An out-of-range index reads as an empty instrument. */
ahx_inst_t ahx_instrument(const ahx_song_t *song, uint8_t index);

/* Decode one step of an instrument's playlist. A step past the playlist reads as empty. */
ahx_step_t ahx_step(const ahx_inst_t *instrument, uint8_t step);

#endif /* AHX_H */
