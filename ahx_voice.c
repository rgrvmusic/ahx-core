/* SPDX-License-Identifier: MIT AND BSD-3-Clause */
/* AHX-1: one voice of the player, and the half of the voice state the device compiles.
 */
#include "ahx_libc.h"      /* memcpy, memset: <string.h> on the host, Felucca's libc.c on the device */

#include "ahx_voice.h"

void ahx_voice_init(ahx_voice_t *voice)
{
    memset(voice, 0, sizeof *voice);
    voice->delta = 1;
    voice->override_transpose = 1000;
    voice->wn_random = 0x280;
    voice->track_master_volume = 0x40;
    voice->track_on = 1;
    voice->mix_source = voice->voice_buffer;
}

void ahx_voice_set_pan(ahx_voice_t *voice, const ahx_pan_t *pan, uint16_t position)
{
    voice->pan = position;
    voice->set_pan = position;
    voice->pan_mult_left = (int16_t)pan->left[position & 0xff];
    voice->pan_mult_right = (int16_t)pan->right[position & 0xff];
}

void ahx_voice_set_audio(ahx_voice_t *voice, const ahx_waves_t *waves, uint32_t frequency)
{
    if (!voice->track_on) {
        voice->voice_volume = 0;
        return;
    }

    voice->voice_volume = voice->audio_volume;

    if (voice->plant_period) {
        voice->plant_period = 0;
        voice->voice_period = voice->audio_period;
        voice->delta = ahx_period_to_delta(voice->audio_period, frequency);
    }

    if (voice->new_waveform) {
        const int8_t *src;
        uint32_t point = (uint32_t)(1 << voice->wave_length);

        /* The one place the two builds differ: the host addresses the table, the device
         * generates the waveform it is about to read. Resolved here and not in the frame step
         * because the device's copy is a shared scratch.
         *
         * Note what is not here: clearing the flag. The reference sets vc_NewWaveform and
         * never clears it (replay.c: the flag is written at 1234, 1523, 1638, 1675 and 1681
         * and read at 1696 and 1815, and nothing anywhere sets it back to 0), so once a voice
         * has a waveform it re-reads the address and rebuilds this buffer on every tick for
         * the rest of its life. That is the reference's own behaviour and it is reproduced,
         * because it is not only a cost: the address it re-reads is the *current* filter
         * position, and a filter that has moved without a waveform change is heard through
         * this path. tests/compare.sh over the corpus is what says the two agree. */
        if (voice->wave_own) {
            src = voice->square_temp;
        } else {
            src = ahx_wave_block(waves, voice->wave_variant, voice->wave_offset,
                                 voice->filter_pos);
        }

        if (voice->waveform == 3) {
            /* White noise: one voice length straight out of the waveform, from the moving
             * window the frame step chose. */
            memcpy(voice->voice_buffer, src, AHX_VOICE_POINTS);
        } else {
            /* The other waveforms are tiled up to one voice length: five repeats of the
             * longest, and more of the shorter ones, so the buffer holds a whole number of
             * cycles whatever the wave length is. The square's source is the frame step's own
             * buffer, already strided, so this is the same tiling for a shorter cycle. */
            uint32_t loops = (1u << (5 - voice->wave_length)) * 5u;
            uint32_t i;

            for (i = 0; i < loops; i++) {
                memcpy(&voice->voice_buffer[i * 4 * point], src, 4 * point);
            }
        }
        /* One sample past the end reads the start, so a position that wraps mid frame still
         * reads a real sample. */
        voice->voice_buffer[AHX_VOICE_POINTS] = voice->voice_buffer[0];
        voice->mix_source = voice->voice_buffer;
    }
}

void ahx_voice_mix(ahx_voice_t *voice, int32_t *acc_left, int32_t *acc_right, uint32_t samples)
{
    const int8_t *src = voice->mix_source;
    uint32_t pos = voice->sample_pos;
    uint32_t delta = voice->delta;
    int32_t vol = voice->voice_volume;
    int32_t pan_left = voice->pan_mult_left;
    int32_t pan_right = voice->pan_mult_right;
    uint32_t i;

    if (src == NULL) {
        return;
    }

    /* The position advances even at zero volume, as it does in the reference, so a note that
     * is muted mid flight resumes where the clock says it should. */
    for (i = 0; i < samples; i++) {
        int32_t sample;

        if (pos >= (uint32_t)(AHX_VOICE_POINTS << 16)) {
            pos -= (uint32_t)(AHX_VOICE_POINTS << 16);
        }
        sample = src[pos >> 16] * vol;
        acc_left[i] += (sample * pan_left) >> 7;
        acc_right[i] += (sample * pan_right) >> 7;
        pos += delta;
    }

    voice->sample_pos = pos;
}
