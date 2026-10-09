/* SPDX-License-Identifier: MIT AND BSD-3-Clause */
/* Note-level AHX voice API, without module tracks or transport.
 * Callers provide their own amplitude envelope and mix the returned samples. */
#ifndef AHX_NOTE_H
#define AHX_NOTE_H

#include <stdint.h>

#include "ahx_config.h"
#include "ahx_voice.h"      /* AHX_VOICE_POINTS, AHX_VARIANT_OF_*, ahx_wave_block() */
#include "ahx_fixed.h"      /* ahx_period_tab, ahx_fixed_delta(), ahx_wn_next() */

/* Waveform selection for a standalone note. */
enum { AHX_NOTE_TRIANGLE, AHX_NOTE_SAWTOOTH, AHX_NOTE_SQUARE, AHX_NOTE_NOISE };

/* ahx_period_tab is five octaves of twelve notes with an unused entry 0, so 61 entries. */
#define AHX_NOTE_COUNT 61

/* Initial noise state and window offset. */
#define AHX_NOISE_SEED 0x280
#define AHX_NOISE_WINDOW(r) (((uint32_t)(r) & (2u * AHX_VOICE_POINTS - 1u)) & ~1u)

/* Instrument settings and pitch. Values are clamped by ahx_note_begin(). */
typedef struct {
    int32_t wave;               /* 0..3: AHX_NOTE_TRIANGLE / SAWTOOTH / SQUARE / NOISE */
    int32_t wlen;               /* 0..5: the cycle is 4 << wlen samples */
    int32_t pulse;              /* 0..31: the square's pulse width, meaningless to the rest */
    int32_t filt;               /* 1..63: 0x20 is the unfiltered waveform, below it low pass */
    int32_t vol;                /* 0..64: the instrument's volume */
    int32_t pitch16;            /* the note, in 1/16 semitones, 0 = the bottom of the table */
    int32_t fine;               /* the residual above it, in the same 1/16 semitone unit */
} ahx_note_cfg_t;

/* Resolved note parameters for a block. Initialize with ahx_note_begin(). */
typedef struct {
    const int8_t *src;          /* the filtered waveform the position indexes */
    uint32_t mask, shift;       /* index = ((pos >> 16) << shift) & mask */
    uint32_t pmax;              /* the cycle in 16.16, which the position wraps at */
    int32_t gain;               /* the instrument's volume as a multiplier of an int8 wave */
    int32_t delta;              /* the step per sample, 16.16, signed */
} ahx_note_block_t;

#if defined(__GNUC__) || defined(__clang__)
#define AHX_NOTE_INLINE static inline __attribute__((always_inline))
#else
#define AHX_NOTE_INLINE static inline
#endif

/* Clamp v to the inclusive range [lo, hi]. */
static inline int32_t ahx_note_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Build the caller-owned playback-step table for rate. Call once before rendering notes. */
AHX_NOTE_INLINE void ahx_note_bank_build(uint32_t *tab, uint32_t rate)
{
    uint32_t i;

    for (i = 0; i < (uint32_t)AHX_NOTE_COUNT; i++) {
        int32_t period = ahx_period_tab[i];

        if (period < 0x0071)
            period = 0x0071;
        if (period > 0x0d60)
            period = 0x0d60;
        tab[i] = ahx_fixed_delta((uint16_t)period, rate);
    }
}

/* Reset note position and noise state. */
AHX_NOTE_INLINE void ahx_note_retrigger(int32_t *st)
{
    st[0] = 0;
    st[1] = AHX_NOISE_SEED;
    st[2] = 0;
}

/* Resolve configuration and advance the noise clock for count samples. */
AHX_NOTE_INLINE void ahx_note_begin(int32_t *st, const ahx_note_cfg_t *c, const uint32_t *tab,
                                    uint32_t count, ahx_note_block_t *b)
{
    uint32_t wave = (uint32_t)ahx_note_clamp(c->wave, 0, 3);
    uint32_t wlen = (uint32_t)ahx_note_clamp(c->wlen, 0, 5);
    uint32_t pulse = (uint32_t)ahx_note_clamp(c->pulse, 0, 31);
    int32_t filt = ahx_note_clamp(c->filt, 1, 63);        /* 0x20 is the unfiltered waveform */
    int32_t gain = ahx_note_clamp(c->vol, 0, 64) * 4;     /* up to 256: an int8 wave at full scale */
    int32_t pitch = ahx_note_clamp(c->pitch16, 0, 59 * 16);
    int32_t k;
    uint32_t variant, len, mask, shift;
    int32_t win;

    switch (wave) {
    case AHX_NOTE_SAWTOOTH:
        variant = AHX_VARIANT_OF_SAWTOOTH(wlen);
        break;
    case AHX_NOTE_SQUARE:
        variant = AHX_VARIANT_OF_SQUARE(pulse);
        break;
    case AHX_NOTE_NOISE:
        variant = AHX_VARIANT_OF_NOISE;
        break;
    default:
        variant = AHX_VARIANT_OF_TRIANGLE(wlen);
        break;
    }

    /* Advance the noise state once per frame of audio. */
    if (variant == AHX_VARIANT_OF_NOISE) {
        int32_t left = st[2] + (int32_t)count;

        while (left >= (int32_t)AHX_FRAME_SAMPLES) {
            left -= (int32_t)AHX_FRAME_SAMPLES;
            st[1] = ahx_wn_next(st[1]);
        }
        st[2] = left;
    }
    win = (int32_t)AHX_NOISE_WINDOW(st[1]);

    b->src = ahx_wave_block(NULL, variant, (variant == AHX_VARIANT_OF_NOISE) ? win : 0, (int)filt);

    /* Pitched cycles use 4 << wlen samples; noise uses a 640-sample window. */
    len = (variant == AHX_VARIANT_OF_NOISE) ? (uint32_t)AHX_VOICE_POINTS : ((uint32_t)4u << wlen);
    mask = len - 1u;
    shift = 0;
    if (variant != AHX_VARIANT_OF_NOISE && wave == AHX_NOTE_SQUARE) {
        shift = 5u - wlen;
        mask = 0x7Fu;
    }

    /* Apply the fractional pitch offset to the selected table entry. */
    b->delta = (int32_t)tab[(pitch >> 4) + 1];
    k = (pitch & 15) * 100 * 2367 / 16000 + c->fine;
    if (k > 2047)
        k = 2047;
    else if (k < -2048)
        k = -2048;
    b->delta += (b->delta * k) >> 12;

    b->gain = gain;
    b->mask = mask;
    b->shift = shift;
    b->pmax = len << 16;
}

/* Return one scaled sample and advance the waveform position. No amplitude envelope is applied. */
AHX_NOTE_INLINE int32_t ahx_note_sample(int32_t *st, const ahx_note_block_t *b)
{
    uint32_t pos = (uint32_t)st[0];
    int32_t s;

    if (pos >= b->pmax)
        pos -= b->pmax;
    s = (int32_t)b->src[((pos >> 16) << b->shift) & b->mask] * b->gain;
    pos += (uint32_t)b->delta;
    st[0] = (int32_t)pos;
    return s;
}

/* Render a block of samples into out. */
static inline void ahx_note_render(int32_t *st, const ahx_note_cfg_t *c, const uint32_t *tab,
                                   int32_t *out, uint32_t count)
{
    ahx_note_block_t b;
    uint32_t i;

    ahx_note_begin(st, c, tab, count, &b);
    for (i = 0; i < count; i++)
        out[i] += ahx_note_sample(st, &b);
}

#endif /* AHX_NOTE_H */
