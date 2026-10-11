/* SPDX-License-Identifier: MIT */
/* AHX-1: the module player, which is the module image, the transport, the four channels and the
 * bus call.
 */

#include "ahx_module.h"
#include "ahx_config.h"          /* AHX_RATE, AHX_BLOCK */
#include "ahx_player.h"          /* the transport and the four channels it holds */

/* The rate and the block size come from ahx_config.h, which on the device is pinned to Felucca's
 * own FS and CTL before any of this is compiled (firmware/ahx_seam.c) and on a host is whatever
 * the build chose. The guard is here because a build that forgot to include the config would
 * otherwise compile against whatever AHX_RATE it found, which is a rate nobody chose. */
#ifndef AHX_CONFIG_H
#error "ahx_module.c: include ahx-core's ahx_config.h first (the seam defines AHX_RATE/AHX_BLOCK from FS/CTL)"
#endif

/* The panning curve the four channels are panned with. It is data (a sine, and no FPU), so it is
 * generated on the host and included here; ahx_module.c is its only user. */
#include "ahx_pan_table.c"

/* The reference's stereo setting the module plays with: 2 of 0 to 4, which is the host
 * renderer's own default (host/ahx_render.c -v 2) and therefore what tests/module_check.sh
 * compares the device against. It is one line because the module carries no stereo field of its
 * own - the reference's separation is a player setting - and where a setting belongs is the UI's,
 * in step 5. */
#define AHX_MODULE_STEREO 2

/* How many samples one call is cut into at most: the bus's own block, so on the device this is
 * one pass of the loop below. It is a bound rather than a choice, and it is AHX_BLOCK (Felucca's
 * CTL on the device, ahx_config.h's default elsewhere) and not a literal 32, so that a caller
 * built for another block size is still cut to that one. */
#define AHX_MODULE_CHUNK AHX_BLOCK

/* The waveform table the player is handed, and the one part of this file that is not the same on
 * both builds. On the device there is none and there cannot be: the host's player would address
 * 410,760 bytes of it, and the device's ahx_wave_block() (ahx_voice_fixed.c) generates
 * the one waveform a voice is about to read instead and ignores the pointer. The panning curve,
 * which is the one thing the player really reads out of a table, is passed for real on both.
 *
 * A build may define this to a table of its own. host/ahx_module_render.c does, for the second
 * tool it makes: the same module player over the host's half of the voice (ahx_tables.c),
 * which is what separates "the player differs" from "the waveform differs" when the two are
 * compared - with the host's tables the difference has to be nothing at all. */
#ifndef AHX_MODULE_WAVES
#define AHX_MODULE_WAVES 0
#endif

/* The loaded module. The bytes are the caller's and must outlive the player (ahx.h), which is
 * why the song holds offsets into them rather than a copy of anything. */
static ahx_song_t ahx_mod_song;
static ahx_player_t ahx_mod_player;
static uint8_t ahx_mod_loaded;

int ahx_module_open(const uint8_t *bytes, uint32_t size)
{
    int status;

    /* Whatever was loaded goes first, so a failed parse cannot leave half of the old module
     * playing against half of the new one. Nothing to discard: neither structure holds an
     * allocation, and the transport is rebuilt from the top by ahx_player_init(). */
    ahx_module_close();

    status = ahx_read(bytes, (size_t)size, &ahx_mod_song);
    if (status != AHX_OK) {
        return status;
    }

    /* ahx_player_init() takes the subsong as 0 - the start of the song - and the module's own
     * SPEED, and it resets every voice. */
    ahx_player_init(&ahx_mod_player, &ahx_mod_song, AHX_MODULE_WAVES, &ahx_pan_table,
                    (uint32_t)AHX_RATE, AHX_MODULE_STEREO);
    ahx_mod_loaded = 1;
    return AHX_OK;
}

void ahx_module_close(void)
{
    ahx_mod_loaded = 0;
}

int ahx_module_playing(void)
{
    return ahx_mod_loaded;
}

int ahx_module_subsong(unsigned index)
{
    if (!ahx_mod_loaded) {
        return AHX_ERR_FIELD;
    }
    return ahx_player_subsong(&ahx_mod_player, index);
}

/* The two below are for a caller that shows what is playing - the device's AHXBox screen reads the
 * title out of the song and the position out of the transport. Nothing writes through them, and
 * both go NULL at close, so a caller must re-read them rather than keep them. */
const ahx_song_t *ahx_module_song(void)
{
    return ahx_mod_loaded ? &ahx_mod_song : 0;
}

const ahx_player_t *ahx_module_transport(void)
{
    return ahx_mod_loaded ? &ahx_mod_player : 0;
}

void ahx_module_loop(int on)
{
    if (ahx_mod_loaded) {
        ahx_player_loop(&ahx_mod_player, on);
    }
}

void ahx_module_seek(unsigned pos)
{
    if (ahx_mod_loaded) {
        ahx_player_seek(&ahx_mod_player, (uint16_t)pos);
    }
}

void ahx_module_voices(unsigned mask)
{
    if (ahx_mod_loaded) {
        ahx_player_voices(&ahx_mod_player, mask);
    }
}

void ahx_module_mix(int32_t *mix_l, int32_t *mix_r, uint32_t n)
{
    int16_t block[2 * AHX_MODULE_CHUNK];
    uint32_t done = 0;

    if (!ahx_mod_loaded) {
        return;                  /* nothing loaded: the bus is what the parts made of it */
    }

    while (done < n) {
        uint32_t chunk = n - done;
        uint32_t i;

        if (chunk > AHX_MODULE_CHUNK) {
            chunk = AHX_MODULE_CHUNK;
        }

        /* The transport ticks where a tick boundary falls inside these samples. The block is
         * the player's own int16 stereo pair per sample, mixed the way the reference mixes it
         * (its sum truncated into an int16, so a loud passage wraps exactly as the oracle's
         * does, which is what the byte comparison needs). */
        ahx_player_block(&ahx_mod_player, block, chunk);

        /* Added, not written: the parts are already in the bus and the FX buses are about to
         * read it. int16 into int32 is exact, and the sum is what the limiter downstream is
         * there for. */
        for (i = 0; i < chunk; i++) {
            mix_l[done + i] += block[2 * i];
            mix_r[done + i] += block[2 * i + 1];
        }
        done += chunk;
    }
}
