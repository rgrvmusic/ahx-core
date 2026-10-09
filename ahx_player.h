/* SPDX-License-Identifier: MIT AND BSD-3-Clause */
/* AHX module transport and rendering interface.
 *
 * A tick advances the transport; a frame is 1/50 second of audio.
 */
#ifndef AHX_PLAYER_H
#define AHX_PLAYER_H

#include <stdint.h>

#include "ahx.h"
#include "ahx_voice.h"

/* Player state for one module. */
typedef struct {
    const ahx_song_t *song;
    const ahx_waves_t *waves;
    const ahx_pan_t *pan;

    ahx_voice_t voice[AHX_CHANNELS];

    /* Current transport position and pending effects. */
    int16_t pos_nr;
    int16_t note_nr;
    int16_t tempo;                   /* ticks per row, from the module and its F command */
    int16_t step_wait_frames;
    uint16_t pos_jump;
    uint16_t pos_jump_note;
    uint8_t get_new_position;
    uint8_t pattern_break;
    uint8_t song_end_reached;
    uint32_t playing_time;

    /* Output settings. stereo ranges from 0 to 4. */
    uint32_t frequency;              /* the rate the module is rendered at */
    uint32_t frame_samples;          /* one frame at that rate: frequency / 50 */
    uint8_t speed_multiplier;        /* 1 to 4, from the module's SPEED */
    uint8_t stereo;

    /* Progress within the current frame for block rendering. */
    uint32_t tick_samples;           /* one tick at this rate: frame_samples / speed_multiplier */
    uint32_t tick_pos;               /* samples of the current tick already mixed */
    uint32_t tick_nr;                /* ticks done in the current frame, 0 .. speed_multiplier */
    uint32_t tail_pos;               /* samples of the frame's unmixed tail already emitted */

    uint8_t defpan_left;
    uint8_t defpan_right;
    int32_t mixgain;

    uint16_t restart;                /* RES, clamped the way the reference's loader does */
} ahx_player_t;

/* Initialize a player for a parsed module. Keep the module and tables valid while it plays.
 * frequency must be a multiple of 50; stereo ranges from 0 to 4. */
void ahx_player_init(ahx_player_t *player, const ahx_song_t *song, const ahx_waves_t *waves,
                     const ahx_pan_t *pan, uint32_t frequency, unsigned stereo);

/* Start a subsong. Index 0 starts at the beginning; other indices are 1-based. */
int ahx_player_subsong(ahx_player_t *player, unsigned index);

/* How many stereo pairs one frame of audio holds at this rate. */
uint32_t ahx_player_frame_samples(const ahx_player_t *player);

/* Render one frame to out, which must hold ahx_player_frame_samples() stereo pairs. */
void ahx_player_frame(ahx_player_t *player, int16_t *out);

/* Render any number of stereo pairs while preserving tick and frame timing. */
void ahx_player_block(ahx_player_t *player, int16_t *out, uint32_t samples);

/* Render up to frames frames and return the number rendered. */
uint32_t ahx_player_render(ahx_player_t *player, int16_t *out, uint32_t frames);

/* Sound one note, for a caller that has no pattern row: an audition on a key, a preview on a
 * row the cursor landed on. It goes through the load and the frame step a cell goes through, so
 * the envelope, the playlist, the square and filter modulation and the vibrato sound here as
 * they will in the module; and the instrument is the caller's, so one being edited can be heard
 * before it is written anywhere.
 *
 * Use a player of its own, initialized for the module the instrument came from: the song,
 * waves, pan and frequency are read, and of its voices only voice 0 is touched (note is an
 * AHX note, 1..60). Any note already sounding is replaced, and the note starts at the top of
 * a frame. The amplitude ramp in and out is the caller's: a release in the middle of a
 * waveform is a click. */
void ahx_player_audition(ahx_player_t *player, const ahx_inst_t *inst, uint8_t note);

/* Render samples stereo pairs of the note ahx_player_audition() started, on the same tick and
 * frame boundaries ahx_player_block() uses, so the two cannot drift apart. */
void ahx_player_audition_block(ahx_player_t *player, int16_t *out, uint32_t samples);

/* Vibrato lookup table. */
extern const int16_t ahx_vib_tab[64];

#endif /* AHX_PLAYER_H */
