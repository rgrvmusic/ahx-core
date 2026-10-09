/* SPDX-License-Identifier: MIT */
/* AHX-1: the device's half of the voice, which is the two functions the two halves of the
 * engine meet.
 *
 * ahx_voice.c is the half both builds compile: the voice state, the plant and the mix, all
 * integer. What it cannot carry is the table the waveform comes out of, because the reference's
 * table is 410,760 bytes and the FM-1 has neither the room nor an FPU to build it with. So the
 * device answers the two calls that touch it itself:
 *
 *   - ahx_wave_block(), the one filtered waveform a voice is about to read, generated on the
 *     spot into a shared scratch (ahx_fixed.c's ahx_wave_unfiltered, then ahx_fixed_variant
 *     filtering it in place). ahx_tables.c returns an address in the table instead;
 *   - ahx_period_to_delta(), the reference's float playback step, which on the device is the
 *     exact integer form (ahx_fixed.c's ahx_fixed_delta).
 *
 * Everything about this file that is a claim rather than an implementation is measured:
 * tests/wave_fixed_check says the generated waveform is the table's bytes, and tests/ahx_test.c
 * says the engine built on it renders what the reference renders.
 *
 * The scratch is deliberately shared by every voice of every part, and with the engine
 * (firmware/eng_ahx.c), because it is keyed: the variant and the filter position a waveform was
 * made for are remembered, so a block that keeps its instrument builds it once and not once per
 * voice. That matters more than it looks. The reference replants the noise every tick (there is
 * no clearing of vc_NewWaveform in replay.c), so a noise voice asks for its waveform 50 times a
 * second; the window it reads moves, the waveform under it does not, and the key is what makes
 * the second one free. The cost of sharing is that two voices on different waveforms make each
 * other rebuild, which is a filter run of at most 1,920 samples and only when they differ.
 *
 * The three waveforms of a pitched voice are at most 128 samples, the square's pulse block is
 * 128 of the four thousand it is laid out with in the table (the frame step asks for the width
 * it wants as its own variant, see the square branch in ahx_player.c), and the noise is
 * 1,920: it is three voice lengths long because the reference's window into it is one voice
 * length and walks. So one waveform's worth of scratch is 1,920 bytes and never more. */

#include "ahx_fixed.h"

/* The waveform the last call made, and what it was made for. The key is the variant and the
 * filter position, which is everything the bytes depend on: an offset into the waveform is not
 * part of it, because that is what the caller does with the result. It is the same key
 * firmware/eng_ahx.c used while it kept its own copy of the scratch. */
static int8_t ahx_block_buf[AHX_NOISE_LEN];
static int32_t ahx_block_key = -1;

const int8_t *ahx_wave_block(const ahx_waves_t *waves, uint32_t variant, int32_t offset,
                             int filter_pos)
{
    uint32_t key;

    /* There is no table to read on this build, so there is nothing to pass in. The parameter is
     * part of the interface ahx_voice.c is compiled against, and the host's implementation
     * is where it is used. */
    (void)waves;

    /* The filter position is the reference's own range, and its clamp here is the one the host
     * does in its own ahx_wave_block(): a position outside 1..63 lands outside the table, and
     * the position it happens in is one where nothing is audible (a voice with a square command
     * and no instrument yet), so clamping rather than reading out of bounds cannot be heard. */
    if (filter_pos < 1) {
        filter_pos = 1;
    } else if (filter_pos > 63) {
        filter_pos = 63;
    }

    key = variant * 64u + (uint32_t)filter_pos;
    if (ahx_block_key != (int32_t)key) {
        ahx_wave_unfiltered(ahx_block_buf, variant);
        ahx_fixed_variant(ahx_block_buf, variant, filter_pos, 0);
        ahx_block_key = (int32_t)key;
    }

    /* The offset is the caller's: the noise reads a window the frame clock walks, which is one
     * voice length from anywhere in the 1,920 (the reference masks it to even samples of two
     * voice lengths, so it always fits), and everything else reads from the start. */
    return &ahx_block_buf[offset];
}

uint32_t ahx_period_to_delta(uint16_t period, uint32_t frequency)
{
    /* The reference's own formula, done exactly rather than in float: the device has no FPU and
     * the integer form is the exact one, so this is the reference's number and not an
     * approximation of it (tests/ahx_test.c measures the two against each other). */
    return ahx_fixed_delta(period, frequency);
}
