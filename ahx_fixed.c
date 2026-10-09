/* SPDX-License-Identifier: MIT AND BSD-3-Clause */
/* AHX-1: the integer code the device runs. See ahx_fixed.h for why it is integer at all and
 * what it is held to.
 *
 * Four things live here, and they are here rather than in ahx_voice.c or ahx_tables.c
 * for one reason: the FM-1 has no FPU, Felucca's firmware uses no floating point anywhere (the
 * rule is stated in its phys_dsp.c and fm6_core.c), and those two files are where the floating
 * point is. The generators, the length table and the unfiltered waveform are the same bytes
 * either way; ahx_tables.c's table builder now calls ahx_wave_unfiltered() for each
 * variant, so the table and the device's copy of a waveform cannot drift apart;
 * the pitch step is the reference's own float arithmetic done exactly in integer; and the
 * filter is the one place where the device cannot be byte for byte the reference, measured by
 * tests/wave_fixed_check.c. */
#include "ahx_fixed.h"

/* ------------------------------------------------------------ the waveforms --- */

static void gen_sawtooth(int8_t *buf, uint32_t len)
{
    uint32_t i;
    int32_t add = (int32_t)(256 / (len - 1));
    int32_t val = -128;

    for (i = 0; i < len; i++, val += add) {
        *buf++ = (int8_t)val;
    }
}

static void gen_triangle(int8_t *buf, uint32_t len)
{
    uint32_t i;
    int32_t d2 = (int32_t)len;
    int32_t d5 = (int32_t)(len >> 2);
    int32_t d1 = 128 / d5;
    int32_t d4 = -(d2 >> 1);
    int32_t val;
    int8_t *mirror;

    val = 0;
    for (i = 0; i < (uint32_t)d5; i++) {
        *buf++ = (int8_t)val;
        val += d1;
    }
    *buf++ = 0x7f;

    if (d5 != 1) {
        val = 128;
        for (i = 0; i < (uint32_t)(d5 - 1); i++) {
            val -= d1;
            *buf++ = (int8_t)val;
        }
    }

    /* The falling half is the rising half read backwards and negated, one sample behind, so
     * that the peak lands on the negative rail instead of repeating. */
    mirror = buf + d4;
    for (i = 0; i < (uint32_t)(d5 * 2); i++) {
        int8_t c = *mirror++;
        /* -(-128) is -128 again in eight bits, which is what the reference gets. */
        *buf++ = (c == 0x7f) ? (int8_t)0x80 : (int8_t)(-c);
    }
}

/* One pulse width, one 0x80 byte block of the square table: the low part first, then the
 * high one. Thirty-two widths, the first with the shortest high part and the longest low
 * one. A width is generated on its own from `pulse` 0..31 and the table is the thirty-two
 * of them end to end (ahx_tables.c's ahx_waves_generate), so the device's copy of a width
 * cannot drift from the table's. */
static void gen_square_one(int8_t *buf, uint32_t pulse)
{
    uint32_t i = pulse + 1u, j;

    for (j = 0; j < (0x40u - i) * 2u; j++) {
        *buf++ = (int8_t)0x80;
    }
    for (j = 0; j < i * 2u; j++) {
        *buf++ = 0x7f;
    }
}

/* The AYS noise generator, seeded as the reference seeds it. Note the always true test in
 * the middle: `(int32_t)(ays & 0xffff) >= 0` cannot be false, so when bit 8 is set the sample
 * is 0x7f and the 0x80 branch is dead. It is kept because the oracle plays it this way. */
static void gen_white_noise(int8_t *buf, uint32_t len)
{
    uint32_t ays = 0x41595321u;

    do {
        uint16_t ax, bx;
        int8_t s = (int8_t)ays;

        if (ays & 0x100) {
            s = (int8_t)0x80;
            if ((int32_t)(ays & 0xffffu) >= 0) {
                s = 0x7f;
            }
        }
        *buf++ = s;
        len--;

        ays = (ays >> 5) | (ays << 27);
        ays = (ays & 0xffffff00u) | ((ays & 0xffu) ^ 0x9a);
        bx = (uint16_t)ays;
        ays = (ays << 2) | (ays >> 30);
        ax = (uint16_t)ays;
        bx = (uint16_t)(bx + ax);
        ax = (uint16_t)(ax ^ bx);
        ays = (ays & 0xffff0000u) | ax;
        ays = (ays >> 3) | (ays << 29);
    } while (len);
}

/* The length of every waveform the filter walks, minus one, in the filter's own order: the
 * six triangles by length, the six sawtooths, the thirty-two pulse widths, and the white
 * noise, which is three voice lengths long because a voice reads a moving window into it.
 * The reference declares the same table inside hvl_GenFilterWaves; it is here because both
 * the unfiltered generator below and the filter use it, and because the device carries no
 * table of waveforms to read the lengths off. */
const uint16_t ahx_variant_len[AHX_VARIANT_COUNT] = {
    3, 7, 0xf, 0x1f, 0x3f, 0x7f,
    3, 7, 0xf, 0x1f, 0x3f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
    (0x280 * 3) - 1
};

uint32_t ahx_wave_unfiltered(int8_t *out, uint32_t variant)
{
    if (variant >= AHX_VARIANT_COUNT) {
        variant = AHX_VARIANT_OF_NOISE;
    }
    if (variant < AHX_VARIANT_OF_SAWTOOTH(0)) {
        gen_triangle(out, 4u << variant);
    } else if (variant < AHX_VARIANT_OF_SQUARE(0)) {
        gen_sawtooth(out, 4u << (variant - AHX_VARIANT_OF_SAWTOOTH(0)));
    } else if (variant < AHX_VARIANT_OF_NOISE) {
        gen_square_one(out, variant - AHX_VARIANT_OF_SQUARE(0));
    } else {
        /* The noise is one waveform of three voice lengths, generated from the reference's
         * seed, so the same window comes out of it every time it is asked for. */
        gen_white_noise(out, AHX_NOISE_LEN);
    }
    return (uint32_t)ahx_variant_len[variant] + 1u;
}

/* ----------------------------------------------------------------- the pitch --- */

/* Transcribed from the reference, and compared against its own array by
 * tests/reference/tables_check.c. It is here and not in ahx_player.c because the device's
 * step below is what turns a period into a rate, and that is the same transcription either
 * way: one table, so a fix to it cannot reach one side and not the other. */
const uint16_t ahx_period_tab[61] = {
    0x0000, 0x0d60, 0x0ca0, 0x0be8, 0x0b40, 0x0a98, 0x0a00, 0x0970,
    0x08e8, 0x0868, 0x07f0, 0x0780, 0x0714, 0x06b0, 0x0650, 0x05f4,
    0x05a0, 0x054c, 0x0500, 0x04b8, 0x0474, 0x0434, 0x03f8, 0x03c0,
    0x038a, 0x0358, 0x0328, 0x02fa, 0x02d0, 0x02a6, 0x0280, 0x025c,
    0x023a, 0x021a, 0x01fc, 0x01e0, 0x01c5, 0x01ac, 0x0194, 0x017d,
    0x0168, 0x0153, 0x0140, 0x012e, 0x011d, 0x010d, 0x00fe, 0x00f0,
    0x00e2, 0x00d6, 0x00ca, 0x00be, 0x00b4, 0x00aa, 0x00a0, 0x0097,
    0x008f, 0x0087, 0x007f, 0x0078, 0x0071
};

/* The reference's period scale as an integer: 3546897 is the C64's clock, and the step is
 * (scale << 16) / (period * rate). ahx_tables.c holds the same number as a float, which is
 * what ahx_period_to_delta() divides with. */
#define AHX_PERIOD_SCALE 3546897u

uint32_t ahx_fixed_delta(uint16_t period, uint32_t frequency)
{
    uint32_t delta;

    if (period == 0u || frequency == 0u) {
        return 1u;
    }

    /* The reference's scale is 3546897 * 65536, which a float holds exactly: 3546897 is under
     * 2^22, so 24 bits of mantissa carry it without rounding, and it is only the division by
     * the period that the reference rounds to those same 24 bits before dividing again by the
     * rate. Dividing once in 64 bits gives the exact step instead of the reference's rounded
     * one, which is as close as an integer can be to it and not the same number: tests/
     * ahx_test.c measures the two over every period the table holds at every rate the device
     * runs at.
     *
     * The divisor is 32 bit on purpose, and it fits: the largest period is 0xd60 and the
     * highest rate the device runs at is 48000, so period * frequency is under 2^29. The FM-1's
     * core divides two 32 bit registers in one instruction, but it has no 64 bit divide at all
     * and the firmware links no library that would supply one (the build's size pass is
     * freestanding: asking for a 64 / 64 leaves __udivdi3 undefined and the link fails). This
     * runs once per note per block rather than per sample. */
    delta = (uint32_t)(((uint64_t)AHX_PERIOD_SCALE << 16) / (uint32_t)(period * frequency));

    /* No period and no rate that reach here wrap, because the table's own clamp of the period
     * into 0x71..0xd60 bounds delta at 46643; the wrap is the reference's and is kept so that
     * the two forms are the same function of their arguments. */
    if (delta > (uint32_t)(AHX_VOICE_POINTS << 16)) {
        delta -= (uint32_t)(AHX_VOICE_POINTS << 16);
    }
    if (delta == 0u) {
        delta = 1u;
    }
    return delta;
}

/* ------------------------------------------------------------ the noise walk --- */

/* The white noise's random walk, as the reference has it (replay.c ahx_wn_next): a state that
 * only ever moves forward, so a voice replanting its waveform every frame reads a window that
 * does not repeat.
 *
 * It is here rather than in ahx_player.c because the device needs it too: the engine
 * (firmware/eng_ahx.c) keeps the reference's frame-clock replant for its noise without the
 * player, and the two must walk the same way or the same instrument would sound different as
 * an engine and in a module. The player passes its voice's wn_random; the engine passes the
 * one int32 it keeps in the voice state. */
int32_t ahx_wn_next(int32_t state)
{
    uint32_t raw = (uint32_t)(state + 2239384);
    uint32_t sign = (raw & 0x80000000u) ? 0xff000000u : 0u;
    uint32_t rot = ((raw >> 8) | sign) | (raw << 24);

    rot = ((rot + 782323u) ^ 75u) - 6735u;
    return (int32_t)rot;
}

/* ----------------------------------------------------------------- the filter --- */

/* The state and the input are Q16.16 in an int32. */
#define AHX_Q 16
#define AHX_QONE (1 << AHX_Q)

/* clip_wave() in ahx_tables.c clamps to 127.0 and -128.0 exactly, not to the ends of a byte,
 * so the fixed point clamp is those two values and not 127.999. */
#define AHX_QMAX (127 * AHX_QONE)
#define AHX_QMIN (-128 * AHX_QONE)

static int32_t clipq(int32_t x)
{
    if (x > AHX_QMAX) {
        return AHX_QMAX;
    }
    if (x < AHX_QMIN) {
        return AHX_QMIN;
    }
    return x;
}

/* x * n / 80, rounded to nearest and away from zero.
 *
 * 80 is not a choice: fre is (8 + 3k) * 1.25 / 100 = (8 + 3k) / 80, so with n = 8 + 3k the
 * coefficient is exact and the division is the only rounding in the step. Rounding to
 * nearest rather than truncating keeps the error inside half a unit of 2^-16 instead of a
 * whole one, and every value that reaches this function is a clipped state times n, so the
 * doubling below cannot leave an int32. */
static int32_t mul_fre(int32_t x, int32_t n)
{
    int32_t p = x * n;

    return p >= 0 ? (2 * p + n) / 160 : -((-2 * p + n) / 160);
}

/* Q16.16 to int8, truncating towards zero, which is what the reference's cast of a double
 * does. An arithmetic shift right would round towards minus infinity, and every sample of
 * -0.5 would then come out one lower than the reference's. */
static int trunc8(int32_t x)
{
    return x >= 0 ? (x >> AHX_Q) : -((-x) >> AHX_Q);
}

uint32_t ahx_fixed_variant(int8_t *buf, uint32_t variant, int filter_pos, uint32_t count)
{
    uint32_t len, i, step;
    int32_t n, mid = 0, low = 0;

    if (variant >= AHX_VARIANT_COUNT) {
        variant = AHX_VARIANT_OF_NOISE;
    }
    len = (uint32_t)ahx_variant_len[variant] + 1u;
    if (count == 0u || count > len) {
        count = len;
    }

    /* Position 0x20 is the unfiltered block, which is the waveform itself. In place that is
     * already true, so it is nothing to do rather than a copy to make. */
    if (filter_pos == 0x20) {
        return count;
    }
    if (filter_pos < 0x01) {
        filter_pos = 0x01;
    } else if (filter_pos > 0x3f) {
        filter_pos = 0x3f;
    }
    step = (uint32_t)(filter_pos < 32 ? filter_pos - 1 : filter_pos - 33);
    n = 8 + 3 * (int32_t)step;

    /* One pass to settle the filter, the reference's own second pass being the one kept. The
     * noise never gets to be short here: it is one waveform of three voice lengths and a
     * voice reads the first window of it, so the settling pass runs over all of it. */
    for (i = 0; i < len; i++) {
        int32_t high = clipq(((int32_t)buf[i] << AHX_Q) - mid - low);
        mid = clipq(mid + mul_fre(high, n));
        low = clipq(low + mul_fre(mid, n));
    }

    for (i = 0; i < count; i++) {
        int32_t high = clipq(((int32_t)buf[i] << AHX_Q) - mid - low);
        mid = clipq(mid + mul_fre(high, n));
        low = clipq(low + mul_fre(mid, n));
        buf[i] = (int8_t)trunc8(filter_pos < 32 ? low : high);
    }
    return count;
}
