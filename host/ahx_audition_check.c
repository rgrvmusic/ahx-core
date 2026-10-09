/* SPDX-License-Identifier: MIT */
/* AHX-1: an auditioned note against the same note played from a pattern row.
 *
 * ahx_player_audition() sounds an instrument with no pattern data, for a key or a row the cursor
 * landed on, and does it through the transport's own voice. This renders one note both ways and
 * compares the samples. The fixture is a one-row module whose instrument carries the playlist,
 * the ADSR, the vibrato and the square modulation, with an empty row after the note.
 */
#include <stdio.h>
#include <string.h>

#include "ahx.h"
#include "ahx_config.h"
#include "ahx_player.h"
#include "ahx_voice.h"

/* 0.2 s, which is inside the fixture's first position: the note is one row of six ticks at
 * 50 Hz, and position 1 is where the transport would wrap and retrigger it. */
#define FIX_FRAMES 10u
#define FIX_NOTE 25
#define FIX_INSTRUMENT 1

static uint8_t fixture[128];
static size_t fixture_len;

static void put_cell(size_t at, uint8_t note, uint8_t instrument, uint8_t command, uint8_t data)
{
    fixture[at + 0] = (uint8_t)((note << 2) | (instrument >> 4));
    fixture[at + 1] = (uint8_t)(((instrument & 15u) << 4) | command);
    fixture[at + 2] = data;
}

/* A module of two rows on one track: row 0 is the note, row 1 is empty. */
static void build_fixture(void)
{
    static const char title[] = "audition fixture";
    static const char name[] = "i1";
    static const uint8_t steps[5][4] = {
        { 0x01, 0x80, 0, 0 },      /* waveform 3: square, so the square modulation runs */
        { 0x01, 0x00, 0, 0 },      /* waveform 2: sawtooth */
        { 0x02, 0x00, 0, 0 },      /* waveform 4: noise */
        { 0x00, 0x80, 0, 0 },      /* waveform 1: triangle */
        { 0x18, 0x00, 40, 0 },     /* fx 6, the playlist's volume, 40; the waveform holds */
    };
    size_t off;
    unsigned i;

    memset(fixture, 0, sizeof fixture);

    fixture[0] = 'T';
    fixture[1] = 'H';
    fixture[2] = 'X';
    fixture[3] = 1;                                    /* version 1 */
    fixture[6] = (uint8_t)(0x80u | (2u >> 8));         /* track 0 absent, SPEED 1 */
    fixture[7] = 2;                                    /* len */
    fixture[10] = 2;                                   /* trl */
    fixture[11] = 1;                                   /* trk */
    fixture[12] = 1;                                   /* smp */
    /* ss stays 0, so there is no subsong list and position 0 is the song's start. */
    off = AHX_HEADER_BYTES;

    fixture[off + 0] = 1;                              /* channel 0 plays track 1 */
    fixture[off + 1] = 0;                              /* transpose */
    off += AHX_POS_BYTES * 2;                          /* position 1 is empty */

    put_cell(off, FIX_NOTE, FIX_INSTRUMENT, 0, 0);
    off += AHX_CELL_BYTES * 2;                         /* row 1 is empty */

    fixture[off + 0] = 64;                             /* volume */
    fixture[off + 1] = 2;                              /* wave length 2, filter speed 0 (off) */
    fixture[off + 2] = 2;                              /* attack length, in ticks */
    fixture[off + 3] = 64;                             /* attack volume */
    fixture[off + 4] = 2;                              /* decay length */
    fixture[off + 5] = 48;                             /* decay volume */
    fixture[off + 6] = 4;                              /* sustain length */
    fixture[off + 7] = 3;                              /* release length */
    fixture[off + 8] = 0;                              /* release volume */
    /* 9..11 are unused by the reader. */
    fixture[off + 12] = 32;                            /* filter low limit: 32 is unfiltered */
    fixture[off + 13] = 0;                             /* vibrato delay */
    fixture[off + 14] = (uint8_t)((3u << 4) | 0x80u | 3u);   /* hard cut 3, relcut, depth 3 */
    fixture[off + 15] = 2;                             /* vibrato speed */
    fixture[off + 16] = 0;                             /* square low limit */
    fixture[off + 17] = 0x7f;                          /* square high limit */
    fixture[off + 18] = 3;                             /* square speed */
    fixture[off + 19] = 32;                            /* filter high limit */
    fixture[off + 20] = 1;                             /* playlist speed: a step a tick */
    fixture[off + 21] = 5;                             /* playlist length */
    off += AHX_INST_BYTES;

    for (i = 0; i < 5; i++) {
        memcpy(fixture + off, steps[i], AHX_PLIST_BYTES);
        off += AHX_PLIST_BYTES;
    }

    memcpy(fixture + off, title, sizeof title);
    off += sizeof title;
    memcpy(fixture + off, name, sizeof name);
    off += sizeof name;

    fixture_len = off;
}

int main(void)
{
    static ahx_waves_t waves;
    static ahx_pan_t pan;
    static ahx_song_t song;
    static ahx_player_t play, aud;
    static int16_t want[AHX_FRAME_SAMPLES * 2];
    static int16_t got[AHX_FRAME_SAMPLES * 2];
    ahx_inst_t inst;
    ahx_cell_t cell;
    int status, failed = 0;
    uint32_t frame, i;
    unsigned differing = 0, octave_differing = 0;
    int32_t peak = 0, worst = 0;

    build_fixture();
    status = ahx_read(fixture, fixture_len, &song);
    if (status != AHX_OK) {
        printf("ahx_audition_check: the fixture does not parse (error %d)\n", status);
        return 1;
    }

    /* The fixture is the test's other half: a cell that does not hold what it was meant to
     * would fail the comparison below and read as a fault in the audition. */
    cell = ahx_cell(&song, 1, 0);
    printf("ahx_audition_check: fixture %u bytes, cell note %u instrument %u, %u rows\n",
           (unsigned)fixture_len, cell.note, cell.instrument, song.trl);
    if (cell.note != FIX_NOTE || cell.instrument != FIX_INSTRUMENT) {
        printf("ahx_audition_check: the fixture's first cell is not note %u instrument %u\n",
               FIX_NOTE, FIX_INSTRUMENT);
        return 1;
    }

    ahx_waves_generate(&waves, &pan);

    ahx_player_init(&play, &song, &waves, &pan, AHX_RATE, 4);
    ahx_player_init(&aud, &song, &waves, &pan, AHX_RATE, 4);

    inst = ahx_instrument(&song, FIX_INSTRUMENT);
    if (inst.plist_len == 0 || inst.volume == 0) {
        printf("ahx_audition_check: the fixture's instrument did not load\n");
        return 1;
    }
    ahx_player_audition(&aud, &inst, FIX_NOTE);

    for (frame = 0; frame < FIX_FRAMES; frame++) {
        ahx_player_frame(&play, want);
        ahx_player_audition_block(&aud, got, ahx_player_frame_samples(&play));

        for (i = 0; i < AHX_FRAME_SAMPLES * 2u; i++) {
            int32_t d = (int32_t)want[i] - (int32_t)got[i];

            if (want[i] > peak) {
                peak = want[i];
            } else if (-(int32_t)want[i] > peak) {
                peak = -(int32_t)want[i];
            }
            if (d != 0) {
                if (d < 0) {
                    d = -d;
                }
                if (d > worst) {
                    worst = d;
                }
                differing++;
            }
        }
    }

    printf("ahx_audition_check: %u frames, %u samples, transport peak %ld, differing %u, worst %ld\n",
           FIX_FRAMES, FIX_FRAMES * AHX_FRAME_SAMPLES * 2u, (long)peak, differing, (long)worst);

    if (peak == 0) {
        printf("ahx_audition_check: the fixture rendered silence, so it proves nothing\n");
        failed = 1;
    }
    if (differing != 0) {
        printf("ahx_audition_check: the audition and the transport disagree\n");
        failed = 1;
    }

    /* And the note is the pitch: no step of the fixture's playlist sets a note of its own, so an
     * octave up has to render something else. */
    ahx_player_audition(&aud, &inst, FIX_NOTE + 12);
    for (frame = 0; frame < FIX_FRAMES; frame++) {
        ahx_player_audition_block(&aud, want, ahx_player_frame_samples(&play));
        for (i = 0; i < AHX_FRAME_SAMPLES * 2u; i++) {
            if (want[i] != got[i]) {
                octave_differing++;
            }
        }
    }
    printf("ahx_audition_check: note %u against note %u, %u samples differ\n",
           FIX_NOTE, FIX_NOTE + 12, octave_differing);
    if (octave_differing == 0) {
        printf("ahx_audition_check: the audition ignores the note it was given\n");
        failed = 1;
    }

    return failed;
}
