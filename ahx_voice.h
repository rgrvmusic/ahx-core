/* SPDX-License-Identifier: MIT AND BSD-3-Clause */
/* Voice state and waveform interface shared by the host and fixed-point builds. */
#ifndef AHX_VOICE_H
#define AHX_VOICE_H

#include <stdint.h>

#include "ahx.h"

/* Samples in a voice buffer. */
#define AHX_VOICE_POINTS 0x280

/* Size of the generated noise waveform. */
#define AHX_NOISE_LEN (0x280 * 3)

/* Bytes per filter position in the waveform table. */
#define AHX_FILTER_SPAN (0xfc + 0xfc + 0x80 * 0x1f + 0x80 + 3 * 0x280)

/* Filter positions per side of the unfiltered waveform. */
#define AHX_FILTER_STEPS 31

/* Waveform variants in table order. */
#define AHX_VARIANT_COUNT 45u
#define AHX_VARIANT_OF_TRIANGLE(wave_length) (0u + (wave_length))    /* 0..5: 4 to 128 samples */
#define AHX_VARIANT_OF_SAWTOOTH(wave_length) (6u + (wave_length))
#define AHX_VARIANT_OF_SQUARE(pulse) (12u + (pulse))                 /* 0..31 pulse widths */
#define AHX_VARIANT_OF_NOISE 44u                                     /* 0x280 * 3 samples */

/* Offsets of the waveform table regions. */
enum {
    AHX_WO_LOWPASSES = 0,
    AHX_WO_TRIANGLE_04 = AHX_WO_LOWPASSES + AHX_FILTER_SPAN * AHX_FILTER_STEPS,
    AHX_WO_TRIANGLE_08 = AHX_WO_TRIANGLE_04 + 0x04,
    AHX_WO_TRIANGLE_10 = AHX_WO_TRIANGLE_08 + 0x08,
    AHX_WO_TRIANGLE_20 = AHX_WO_TRIANGLE_10 + 0x10,
    AHX_WO_TRIANGLE_40 = AHX_WO_TRIANGLE_20 + 0x20,
    AHX_WO_TRIANGLE_80 = AHX_WO_TRIANGLE_40 + 0x40,
    AHX_WO_SAWTOOTH_04 = AHX_WO_TRIANGLE_80 + 0x80,
    AHX_WO_SAWTOOTH_08 = AHX_WO_SAWTOOTH_04 + 0x04,
    AHX_WO_SAWTOOTH_10 = AHX_WO_SAWTOOTH_08 + 0x08,
    AHX_WO_SAWTOOTH_20 = AHX_WO_SAWTOOTH_10 + 0x10,
    AHX_WO_SAWTOOTH_40 = AHX_WO_SAWTOOTH_20 + 0x20,
    AHX_WO_SAWTOOTH_80 = AHX_WO_SAWTOOTH_40 + 0x40,
    AHX_WO_SQUARES = AHX_WO_SAWTOOTH_80 + 0x80,
    AHX_WO_WHITENOISE = AHX_WO_SQUARES + (0x80 * 0x20),
    AHX_WO_HIGHPASSES = AHX_WO_WHITENOISE + AHX_NOISE_LEN,
    AHX_WAVES_SIZE = AHX_WO_HIGHPASSES + AHX_FILTER_SPAN * AHX_FILTER_STEPS
};

/* Waveform table in reference order. */
typedef struct {
    int8_t byte[AHX_WAVES_SIZE];
} ahx_waves_t;

/* Panning gains for 256 positions from left to right. */
typedef struct {
    uint16_t left[256];
    uint16_t right[256];
} ahx_pan_t;

/* One playback channel. */
typedef struct {
    /* Current and next track assignment. */
    int16_t track;
    int16_t next_track;
    int16_t transpose;
    int16_t next_transpose;

    /* 1000 means use the position's transpose. */
    int16_t override_transpose;

    /* Current instrument and envelope state. */
    ahx_inst_t inst;
    uint8_t perf_on;
    int32_t adsr_volume;
    int16_t adsr_a_frames, adsr_a_volume;
    int16_t adsr_d_frames, adsr_d_volume;
    int16_t adsr_s_frames;
    int16_t adsr_r_frames, adsr_r_volume;

    /* Playback: a 16.16 position into the voice buffer, and the step per output sample. */
    uint32_t sample_pos;
    uint32_t delta;
    uint16_t voice_period;

    /* Note periods and vibrato offset. */
    uint16_t instr_period;
    uint16_t track_period;
    int16_t vibrato_period;

    /* Note, playlist and track volume. */
    int16_t note_max_volume;
    int16_t perf_sub_volume;
    int16_t track_master_volume;

    /* Waveform selection. */
    uint16_t wave_length;
    uint8_t waveform;
    uint8_t new_waveform;
    uint8_t plant_period;
    uint8_t plant_square;
    uint8_t ignore_square;
    uint8_t fixed_note;
    uint8_t track_on;

    int16_t volume_slide_up;
    int16_t volume_slide_down;

    /* Hard-cut state. */
    int16_t hard_cut;
    uint8_t hard_cut_release;
    int16_t hard_cut_release_frames;

    /* Track and playlist period-slide state. */
    uint8_t period_slide_on;
    int16_t period_slide_speed;
    int16_t period_slide_period;
    int16_t period_slide_limit;
    int16_t period_slide_with_limit;
    int16_t period_perf_slide_speed;
    int16_t period_perf_slide_period;
    uint8_t period_perf_slide_on;

    /* Vibrato. */
    int16_t vibrato_delay;
    int16_t vibrato_speed;
    int16_t vibrato_current;
    int16_t vibrato_depth;

    /* Square-wave modulation state. */
    int16_t square_on;
    int16_t square_init;
    int16_t square_wait;
    int16_t square_lower_limit;
    int16_t square_upper_limit;
    int16_t square_pos;
    int16_t square_sign;
    int16_t square_sliding_in;
    int16_t square_reverse;

    /* Filter modulation. */
    uint8_t filter_on;
    uint8_t filter_init;
    int16_t filter_wait;
    int16_t filter_speed;
    int16_t filter_upper_limit;
    int16_t filter_lower_limit;
    int16_t filter_pos;
    int16_t filter_sign;
    int16_t filter_sliding_in;
    int16_t ignore_filter;

    /* Instrument playlist state. */
    int16_t perf_current;
    int16_t perf_speed;
    int16_t perf_wait;

    /* Note delay and note cut, in frames. */
    uint8_t note_delay_on;
    uint8_t note_cut_on;
    int16_t note_delay_wait;
    int16_t note_cut_wait;

    /* What the voice is playing this frame. */
    int16_t audio_period;
    int16_t audio_volume;
    int16_t voice_volume;

    /* Waveform selection and offset, resolved by ahx_voice_set_audio(). */
    uint32_t wave_variant;
    int32_t wave_offset;
    uint8_t wave_own;            /* the square: the frame step already built it into square_temp */
    const int8_t *mix_source;

    /* Panning and mix gain. */
    uint16_t pan;
    uint16_t set_pan;
    int16_t pan_mult_left;
    int16_t pan_mult_right;

    /* Per-voice noise state. */
    int32_t wn_random;

    /* Mix buffer and square-wave scratch. */
    int8_t voice_buffer[AHX_VOICE_POINTS + 2];
    int8_t square_temp[0x80];
} ahx_voice_t;

/* Build the waveform and panning tables. Host build only. */
void ahx_waves_generate(ahx_waves_t *waves, ahx_pan_t *pan);

/* AHX note periods in C64 clock units. Entry 0 is unused. */
extern const uint16_t ahx_period_tab[61];

/* Waveform lengths minus one, indexed by AHX_VARIANT_OF_* value. */
extern const uint16_t ahx_variant_len[AHX_VARIANT_COUNT];

/* Generate one unfiltered waveform into out and return its length. */
uint32_t ahx_wave_unfiltered(int8_t *out, uint32_t variant);

/* Generate one filtered waveform. Host build only; returns the number of samples written. */
uint32_t ahx_wave_variant(int8_t *out, const ahx_waves_t *waves, uint32_t variant,
                          int filter_pos, uint32_t count);

/* Playback step per output sample. Implemented by the selected voice backend. */
uint32_t ahx_period_to_delta(uint16_t period, uint32_t frequency);

/* Return a waveform at variant, offset and filter_pos. The result is temporary in the
 * fixed-point build and must be consumed before the next call. */
const int8_t *ahx_wave_block(const ahx_waves_t *waves, uint32_t variant, int32_t offset,
                             int filter_pos);

/* Reset a voice. */
void ahx_voice_init(ahx_voice_t *voice);

/* Set a voice's panning table and position. */
void ahx_voice_set_pan(ahx_voice_t *voice, const ahx_pan_t *pan, uint16_t position);

/* Resolve the waveform and playback step for the current frame. */
void ahx_voice_set_audio(ahx_voice_t *voice, const ahx_waves_t *waves, uint32_t frequency);

/* Mix samples from this voice into the left and right accumulators. */
void ahx_voice_mix(ahx_voice_t *voice, int32_t *acc_left, int32_t *acc_right, uint32_t samples);

#endif /* AHX_VOICE_H */
