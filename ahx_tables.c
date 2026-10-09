/* SPDX-License-Identifier: MIT AND BSD-3-Clause */
/* AHX-1: the waveform tables, which are the host's half of the voice.
 *
 * This file is the reference's 410,760 byte table and the float arithmetic that builds it,
 * and the two functions that read it: ahx_wave_block() and ahx_period_to_delta(). None of it
 * can be compiled for the FM-1, which has no FPU and nowhere to put the table, so it is
 * host only and the device implements those same two functions instead
 * (ahx_voice_fixed.c, over ahx_fixed.c's generator and step). The voice itself,
 * the state, the panning, the plant and the mix, is ahx_voice.c and is shared.
 *
 * The tables are compared byte for byte against the reference's own arrays by
 * tests/reference/tables_check.c, and the device's per-waveform substitute for them by
 * tests/variant_check and tests/wave_fixed_check.
 */
#include <math.h>
#include <string.h>

#include "ahx_voice.h"

/* The reference computes our playback step in float (replay.h, Period2Freq), so the product
 * loses precision before the division. Kept as it is on purpose: the tables and the render
 * are compared against the oracle byte for byte. The device has no FPU, so it defines this
 * same function over ahx_fixed.c's ahx_fixed_delta instead, and tests/ahx_test.c measures
 * the two against each other rather than assuming they agree.
 *
 * The name carries the _FLOAT because ahx_fixed.c has an integer AHX_PERIOD_SCALE for the
 * same 3546897 - twice two to the sixteen - and the two files are compiled into one translation
 * unit by host/ahx_module_render.c's host-voice build, which needs the player's noise walk out of
 * the one and this out of the other. The values are not interchangeable and the names say so. */
#define AHX_PERIOD_SCALE_FLOAT (3546897.0f * 65536.0f)

uint32_t ahx_period_to_delta(uint16_t period, uint32_t frequency)
{
    double freq;
    uint32_t delta;

    if (period == 0 || frequency == 0) {
        return 1;
    }
    freq = (double)(AHX_PERIOD_SCALE_FLOAT / (float)period);
    delta = (uint32_t)(freq / (double)frequency);
    if (delta > (uint32_t)(AHX_VOICE_POINTS << 16)) {
        delta -= (uint32_t)(AHX_VOICE_POINTS << 16);
    }
    if (delta == 0) {
        delta = 1;
    }
    return delta;
}

/* Where a waveform the filter walks starts in the unfiltered block. The triangles and the
 * sawtooths sit end to end by length, at the same six offsets the player adds for a voice's
 * wave length; each of the thirty-two pulse widths is one 0x80 byte block of the square
 * table, and the noise is one waveform of three voice lengths. */
static uint32_t ahx_variant_offset(uint32_t variant)
{
    static const uint16_t len_off[6] = { 0x00, 0x04, 0x0c, 0x1c, 0x3c, 0x7c };

    if (variant >= AHX_VARIANT_OF_SQUARE(0) && variant < AHX_VARIANT_OF_NOISE) {
        return AHX_WO_SQUARES + (variant - AHX_VARIANT_OF_SQUARE(0)) * 0x80u;
    }
    if (variant == AHX_VARIANT_OF_NOISE) {
        return AHX_WO_WHITENOISE;
    }
    if (variant >= AHX_VARIANT_OF_SAWTOOTH(0)) {
        return AHX_WO_SAWTOOTH_04 + len_off[variant - AHX_VARIANT_OF_SAWTOOTH(0)];
    }
    return AHX_WO_TRIANGLE_04 + len_off[variant];
}

const int8_t *ahx_wave_block(const ahx_waves_t *waves, uint32_t variant, int32_t offset,
                             int filter_pos)
{
    /* A filter position outside 0x01 to 0x3f lands outside the table. The reference gets away
     * with that because its table is a global array: the read before it is inside its own data
     * segment, and the position it happens in is one where nothing is audible, a voice that has
     * had a square command but no instrument yet, whose volume is zero and stays zero. Every
     * byte of the square it builds there is multiplied by that zero. Ours is a heap table, so
     * the same read would fault, and it is clamped rather than read out of bounds. */
    if (filter_pos < 0x01) {
        filter_pos = 0x01;
    } else if (filter_pos > 0x3f) {
        filter_pos = 0x3f;
    }

    /* The unfiltered set is at 0x20 and the low and high passes are whole strides on either
     * side of it, so the position is an offset from the variant's own place. The arithmetic is
     * unsigned, as the reference's is: a position below 0x20 subtracts a stride through the
     * wrap and comes out at the variant's address in the low passes. */
    return &waves->byte[(uint32_t)ahx_variant_offset(variant) +
                        (uint32_t)((filter_pos - 0x20) * AHX_FILTER_SPAN) + (uint32_t)offset];
}

/* Sine shaped panning, 0 hard left to 255 hard right. The reference left table starts a
 * quarter cycle in and the right one starts at zero, so the two cross over.
 *
 * The arithmetic detail that matters: the starting angle is a quarter turn computed in
 * *float* while the step is a quarter turn computed in *double*. sin() of the float one is a
 * hair under 1.0, so left[0] is 254 and not 255. Both are reproduced, because the comparison
 * against the oracle is byte for byte and this is a real value in the render. */
static void ahx_pan_generate(ahx_pan_t *pan)
{
    uint32_t i;
    double left = (3.14159265f * 2.0f) / 4.0f;
    double right = 0.0;
    const double step = (3.14159265 * 2.0f / 4.0f) / 256.0f;

    for (i = 0; i < 256; i++) {
        pan->left[i] = (uint16_t)(sin(left) * 255.0f);
        pan->right[i] = (uint16_t)(sin(right) * 255.0f);
        left += step;
        right += step;
    }
    pan->left[255] = 0;
    pan->right[0] = 0;
}

static double clip_wave(double x)
{
    if (x > 127.0) {
        return 127.0;
    }
    if (x < -128.0) {
        return -128.0;
    }
    return x;
}

/* The filter of AHX, and the reason for the reference's 410,760 byte table. It is a two pole
 * state variable filter run over each waveform at thirty-one frequencies, producing a low
 * pass and a high pass variant of every one. The player then only has to pick a position; it
 * never filters anything per sample.
 *
 * This builds all of them into the table, which is what the host needs to be comparable with
 * the oracle and what the reference does. The device builds one at a time instead, with
 * ahx_wave_variant() below, because it has nowhere to put 410,760 bytes; that is a saving in
 * memory and not a different filter, and the test that says so is tests/variant_check. */
static void gen_filter_waves(int8_t *buf, int8_t *lowbuf, int8_t *highbuf)
{
    double freq;
    uint32_t step;

    for (step = 0, freq = 8.0; step < AHX_FILTER_STEPS; step++, freq += 3.0) {
        uint32_t wv;
        const int8_t *src = buf;

        /* Six triangles, six sawtooths, thirty-two squares and the noise: the waveforms are
         * read straight out of the table in its own order, which is why the layout matters. */
        for (wv = 0; wv < 6 + 6 + 0x20 + 1; wv++) {
            double fre, high, mid, low;
            uint32_t i;

            mid = 0.0;
            low = 0.0;
            fre = freq * 1.25 / 100.0;

            /* One silent pass to settle the filter, then the pass that is kept. */
            for (i = 0; i <= ahx_variant_len[wv]; i++) {
                high = clip_wave(src[i] - mid - low);
                mid = clip_wave(mid + high * fre);
                low = clip_wave(low + mid * fre);
            }
            for (i = 0; i <= ahx_variant_len[wv]; i++) {
                high = clip_wave(src[i] - mid - low);
                mid = clip_wave(mid + high * fre);
                low = clip_wave(low + mid * fre);
                *lowbuf++ = (int8_t)low;
                *highbuf++ = (int8_t)high;
            }
            src += ahx_variant_len[wv] + 1;
        }
    }
}

uint32_t ahx_wave_variant(int8_t *out, const ahx_waves_t *waves, uint32_t variant,
                          int filter_pos, uint32_t count)
{
    uint32_t len, i, step;
    const int8_t *src;
    double fre, high, mid, low;

    if (variant >= AHX_VARIANT_COUNT) {
        variant = AHX_VARIANT_OF_NOISE;
    }
    len = (uint32_t)ahx_variant_len[variant] + 1u;
    src = &waves->byte[ahx_variant_offset(variant)];
    if (count == 0u || count > len) {
        count = len;
    }

    /* Position 0x20 is the unfiltered block, which is a copy and not a filter. */
    if (filter_pos == 0x20) {
        memcpy(out, src, count);
        return count;
    }
    if (filter_pos < 0x01) {
        filter_pos = 0x01;
    } else if (filter_pos > 0x3f) {
        filter_pos = 0x3f;
    }

    /* The position is a side and a frequency: 1 to 31 step up the low passes, 33 to 63 step
     * up the high passes through the same thirty-one frequencies, and 32 is between them.
     * The reference computes both outputs at every position and lets the player's address
     * choose; only the chosen one is written here, which cannot change the other. */
    step = (uint32_t)(filter_pos < 32 ? filter_pos - 1 : filter_pos - 33);
    fre = (8.0 + 3.0 * (double)step) * 1.25 / 100.0;

    mid = 0.0;
    low = 0.0;
    for (i = 0; i < len; i++) {
        high = clip_wave((double)src[i] - mid - low);
        mid = clip_wave(mid + high * fre);
        low = clip_wave(low + mid * fre);
    }
    for (i = 0; i < count; i++) {
        high = clip_wave((double)src[i] - mid - low);
        mid = clip_wave(mid + high * fre);
        low = clip_wave(low + mid * fre);
        out[i] = (int8_t)(filter_pos < 32 ? low : high);
    }
    return count;
}

void ahx_waves_generate(ahx_waves_t *waves, ahx_pan_t *pan)
{
    int8_t *w = waves->byte;
    uint32_t v;

    ahx_pan_generate(pan);

    /* The unfiltered block is the device's own generators laid end to end, in the order the
     * AHX_VARIANT_OF_* numbering names them, so what the table holds at a variant's offset is
     * the bytes ahx_wave_unfiltered() makes for that variant. Building it this way is what
     * makes the device's copy of a waveform unable to drift from the table's, and it is
     * check 1 of tests/wave_fixed_check. */
    for (v = 0; v < AHX_VARIANT_COUNT; v++) {
        ahx_wave_unfiltered(&w[ahx_variant_offset(v)], v);
    }
    gen_filter_waves(&w[AHX_WO_TRIANGLE_04], &w[AHX_WO_LOWPASSES], &w[AHX_WO_HIGHPASSES]);
}
