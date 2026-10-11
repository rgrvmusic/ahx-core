/* SPDX-License-Identifier: MIT */
/* AHX-1: writing a module back to the bytes it was read from.
 *
 * The writer is the reader inverted, so the test is the round trip: decode every structure of a
 * fixture and write it straight back, and the module must not move a byte. That covers the fields
 * whose bits are split across bytes (the filter speed, the waveform) and the bytes the reader does
 * not read at all, which the fixture sets. Each patcher is then given a change, and the module must
 * differ in exactly the bytes that field lives in and nowhere else. The playlist resize is the one
 * edit that moves things, and there the test is stronger: grow it and shrink it back and the module
 * has to come out byte for byte as it went in, with what follows it intact either way.
 *
 * The fixture is built twice, once with track 0 in the file and once with it omitted, because the
 * block a track lives in is found differently in each and the writer has to follow.
 */
#include <stdio.h>
#include <string.h>

#include "ahx.h"
#include "ahx_write.h"

#define FIX_CAP 512
#define FIX_INST1 1
#define FIX_INST2 2
#define FIX_PLIST1 3
#define FIX_PLIST2 2
#define FIX_ROWS 2

static uint8_t mod[FIX_CAP];         /* the working copy: what a caller edits */
static uint8_t golden[FIX_CAP];      /* the fixture as built, for the byte diffs */
static size_t mod_len;
static int failures;

static void fail(const char *what)
{
    printf("ahx_write_check: %s\n", what);
    failures++;
}

static unsigned diff_bytes(const uint8_t *a, const uint8_t *b, size_t n)
{
    unsigned d = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            d++;
        }
    }
    return d;
}

/* One instrument's fixed block and its playlist. Bytes 9..11 and 19's top two bits, which the
 * reader does not read, are set on purpose: a write that rebuilt the block rather than patched it
 * would clear them, and the round trip would not notice. */
static size_t put_inst(size_t at, uint8_t volume, uint8_t plist_len, uint8_t fill)
{
    size_t i;

    memset(mod + at, 0, AHX_INST_BYTES + (size_t)plist_len * AHX_PLIST_BYTES);
    mod[at + 0] = volume;
    mod[at + 1] = 0x2au;                          /* wave length 2, filter speed bits 4-0 = 5 */
    mod[at + 2] = 2;                              /* attack length and volume */
    mod[at + 3] = 64;
    mod[at + 4] = 2;                              /* decay */
    mod[at + 5] = 48;
    mod[at + 6] = 4;                              /* sustain length */
    mod[at + 7] = 3;                              /* release length and volume */
    mod[at + 8] = 0;
    mod[at + 9] = 0xa5;                           /* unused by the reader */
    mod[at + 10] = 0x5a;
    mod[at + 11] = 0xff;
    mod[at + 12] = 0xb8u;                         /* filter speed bit 5, low limit 0x38 */
    mod[at + 13] = 1;                             /* vibrato delay */
    mod[at + 14] = 0xb5u;                         /* relcut, hard cut 3, vibrato depth 5 */
    mod[at + 15] = 2;                             /* vibrato speed */
    mod[at + 16] = 0x10;                          /* square limits and speed */
    mod[at + 17] = 0x3f;
    mod[at + 18] = 3;
    mod[at + 19] = 0xe0u;                         /* top two bits unread, high limit 0x20 */
    mod[at + 20] = 1;                             /* playlist speed */
    mod[at + 21] = plist_len;

    for (i = 0; i < plist_len; i++) {
        size_t e = at + AHX_INST_BYTES + i * AHX_PLIST_BYTES;

        mod[e + 0] = (uint8_t)(((i + 1u) << 5) | (i << 2) | 0x02u);
        mod[e + 1] = (uint8_t)(0xc0u | (10u + i));    /* fix note, and a note of its own */
        mod[e + 2] = (uint8_t)(0x20u + i);
        mod[e + 3] = (uint8_t)(0x40u + fill + i);
    }
    return at + AHX_INST_BYTES + (size_t)plist_len * AHX_PLIST_BYTES;
}

static size_t put_cell(size_t at, uint8_t note, uint8_t instrument, uint8_t command, uint8_t data)
{
    mod[at + 0] = (uint8_t)((note << 2) | (instrument >> 4));
    mod[at + 1] = (uint8_t)(((instrument & 15u) << 4) | command);
    mod[at + 2] = data;
    return at + AHX_CELL_BYTES;
}

static void build_fixture(int track0_absent)
{
    static const char title[] = "write fixture";
    static const char name1[] = "i1";
    static const char name2[] = "i2";
    size_t off;

    memset(mod, 0, sizeof mod);

    mod[0] = 'T';
    mod[1] = 'H';
    mod[2] = 'X';
    mod[3] = 1;                                   /* version 1 */
    mod[6] = (uint8_t)((track0_absent ? 0x80u : 0u) | (1u << 5) | 0x10u);   /* SPEED 1 */
    mod[7] = FIX_ROWS;                            /* len */
    mod[8] = 0;
    mod[9] = 0;                                   /* res */
    mod[10] = FIX_ROWS;                           /* trl */
    mod[11] = 2;                                  /* trk */
    mod[12] = 2;                                  /* smp */
    mod[13] = 1;                                  /* ss */
    off = AHX_HEADER_BYTES;

    mod[off + 0] = 0;
    mod[off + 1] = 1;                             /* subsong 0 starts at position 1 */
    off += 2;

    /* Position 0: two channels playing tracks 1 and 2. Position 1: track 1, transposed down. */
    mod[off + 0] = 1;
    mod[off + 1] = 0;
    mod[off + 2] = 2;
    mod[off + 3] = 0;
    mod[off + 8] = 1;
    mod[off + 9] = (uint8_t)-12;
    off += AHX_POS_BYTES * FIX_ROWS;

    /* Track 0 first when the file holds it, so every block after it shifts when it does not. */
    if (!track0_absent) {
        off = put_cell(off, 0, 0, 0, 0);
        off = put_cell(off, 0, 0, 0, 0);
    }
    off = put_cell(off, 25, FIX_INST1, 0, 0);      /* track 1, row 0 */
    off = put_cell(off, 0, 0, 0, 0);
    off = put_cell(off, 0, 0, 0, 0);
    off = put_cell(off, 13, FIX_INST2, 0xa, 0x40); /* track 2, row 1 */

    off = put_inst(off, 64, FIX_PLIST1, 1);
    off = put_inst(off, 32, FIX_PLIST2, 2);

    mod[4] = (uint8_t)(off >> 8);                 /* the stored title offset: the name block */
    mod[5] = (uint8_t)(off & 0xff);
    memcpy(mod + off, title, sizeof title);
    off += sizeof title;
    memcpy(mod + off, name1, sizeof name1);
    off += sizeof name1;
    memcpy(mod + off, name2, sizeof name2);
    off += sizeof name2;

    mod_len = off;
    memcpy(golden, mod, mod_len);
}

static int reparse(ahx_song_t *song, size_t size)
{
    int status = ahx_read(mod, size, song);

    if (status != AHX_OK) {
        printf("ahx_write_check: the module does not parse at %u bytes (error %d)\n",
               (unsigned)size, status);
        failures++;
    }
    return status;
}

/* Every structure, decoded and written straight back. */
static void round_trip(const ahx_song_t *song)
{
    ahx_cell_t cell;
    ahx_inst_t inst;
    ahx_step_t s;
    unsigned track, row, ch, i, k;

    for (track = song->track0_absent ? 1u : 0u; track <= song->trk; track++) {
        for (row = 0; row < song->trl; row++) {
            cell = ahx_cell(song, (uint8_t)track, (uint8_t)row);
            if (ahx_write_cell(mod, song, (uint8_t)track, (uint8_t)row, cell) != AHX_OK) {
                fail("a cell the reader read was refused by the writer");
                return;
            }
        }
    }
    for (i = 0; i < song->len; i++) {
        for (ch = 0; ch < AHX_CHANNELS; ch++) {
            uint8_t t;
            int8_t tr;

            if (!ahx_position(song, (uint16_t)i, ch, &t, &tr)) {
                continue;
            }
            if (ahx_write_position(mod, song, (uint16_t)i, ch, t, tr) != AHX_OK) {
                fail("a position the reader read was refused by the writer");
                return;
            }
        }
    }
    for (i = 0; i < song->ss; i++) {
        if (ahx_write_subsong(mod, song, (uint8_t)i, ahx_subsong(song, (uint8_t)i)) != AHX_OK) {
            fail("a subsong the reader read was refused by the writer");
            return;
        }
    }
    for (i = 1; i <= song->smp; i++) {
        inst = ahx_instrument(song, (uint8_t)i);
        if (ahx_write_instrument(mod, song, (uint8_t)i, &inst) != AHX_OK) {
            fail("an instrument the reader read was refused by the writer");
            return;
        }
        for (k = 0; k < inst.plist_len; k++) {
            s = ahx_step(&inst, (uint8_t)k);
            if (ahx_write_step(mod, song, (uint8_t)i, (uint8_t)k, s) != AHX_OK) {
                fail("a playlist step the reader read was refused by the writer");
                return;
            }
        }
    }
    if (memcmp(mod, golden, mod_len) != 0) {
        printf("ahx_write_check: writing the module back moved %u of %u bytes\n",
               diff_bytes(mod, golden, mod_len), (unsigned)mod_len);
        failures++;
    }
}

/* Back to the fixture, and parsed again. */
static int restore(ahx_song_t *song)
{
    memcpy(mod, golden, mod_len);
    return reparse(song, mod_len);
}

static void check(int track0_absent)
{
    ahx_song_t song;
    ahx_cell_t cell;
    ahx_inst_t inst;
    ahx_step_t s;
    size_t size;
    uint32_t inst2_at, names_at;
    unsigned i;

    build_fixture(track0_absent);
    if (reparse(&song, mod_len) != AHX_OK) {
        return;
    }
    printf("ahx_write_check: %s: %u bytes, %u tracks stored of %u, %u instruments, "
           "playlists %u and %u\n",
           track0_absent ? "track 0 omitted" : "track 0 present", (unsigned)mod_len,
           song.stored_tracks, song.trk, song.smp, ahx_instrument(&song, 1).plist_len,
           ahx_instrument(&song, 2).plist_len);

    /* 1. the round trip. */
    round_trip(&song);
    if (song.title_offset != song.name_off[0]) {
        fail("the fixture's stored title offset does not agree with its name block");
    }

    /* 2. a cell, cleared and then set: its three bytes and no others. */
    if (restore(&song) != AHX_OK) {
        return;
    }
    cell = ahx_cell(&song, 2, 1);
    if (cell.note == 0) {
        fail("the fixture's second track has no note to clear");
    }
    cell.note = 0;
    cell.instrument = 0;
    cell.command = 0;
    cell.data = 0;
    if (ahx_write_cell(mod, &song, 2, 1, cell) != AHX_OK) {
        fail("clearing a cell was refused");
    }
    i = diff_bytes(mod, golden, mod_len);
    if (i != AHX_CELL_BYTES) {
        printf("ahx_write_check: clearing a cell moved %u bytes, not %u\n", i, AHX_CELL_BYTES);
        failures++;
    }
    if (restore(&song) != AHX_OK) {
        return;
    }
    cell.note = 37;
    cell.instrument = FIX_INST2;
    cell.command = AHX_CMD_SPEED;
    cell.data = 6;
    if (ahx_write_cell(mod, &song, 2, 1, cell) != AHX_OK) {
        fail("setting a cell was refused");
    }
    if (diff_bytes(mod, golden, mod_len) != AHX_CELL_BYTES) {
        fail("setting a cell moved more than its three bytes");
    }
    (void)reparse(&song, mod_len);
    cell = ahx_cell(&song, 2, 1);
    if (cell.note != 37 || cell.instrument != FIX_INST2 || cell.command != AHX_CMD_SPEED ||
        cell.data != 6) {
        fail("the cell that was written does not read back");
    }

    /* 3. a position: two bytes, and a channel given a track it did not play. */
    if (restore(&song) != AHX_OK) {
        return;
    }
    if (ahx_write_position(mod, &song, 1, 1, 2, 7) != AHX_OK) {
        fail("writing a position was refused");
    }
    if (diff_bytes(mod, golden, mod_len) != 2) {
        fail("writing a position moved more than its two bytes");
    }
    (void)reparse(&song, mod_len);
    {
        uint8_t t = 0;
        int8_t tr = 0;

        ahx_position(&song, 1, 1, &t, &tr);
        if (t != 2 || tr != 7) {
            fail("the position that was written does not read back");
        }
    }

    /* 4. the header's two free fields, and the fields around them that are not free. */
    if (restore(&song) != AHX_OK) {
        return;
    }
    if (ahx_write_header(mod, &song, 3, 1) != AHX_OK) {
        fail("writing the header was refused");
    }
    (void)reparse(&song, mod_len);
    if (song.speed != 3 || song.res != 1) {
        fail("SPEED and the restart position do not read back");
    }
    if (song.len != FIX_ROWS || song.trl != FIX_ROWS || song.trk != 2 || song.smp != 2 ||
        song.ss != 1 || song.track0_absent != (uint8_t)track0_absent) {
        fail("writing SPEED moved another header field");
    }
    if ((mod[6] & 0x10u) != 0x10u) {
        fail("writing SPEED cleared the header bit the reader does not read");
    }

    /* 5. an instrument's fields, and the bytes of its block the reader does not read. */
    if (restore(&song) != AHX_OK) {
        return;
    }
    inst = ahx_instrument(&song, FIX_INST1);
    inst.volume = 40;
    inst.filt_speed = 0x2a;                       /* bits 4-0 and bit 5, so both bytes move */
    inst.filt_lo = 0x11;
    inst.filt_hi = 0x22;
    inst.vib_depth = 9;
    inst.hardcut = 6;
    inst.relcut = 0;
    inst.plist_speed = 4;
    if (ahx_write_instrument(mod, &song, FIX_INST1, &inst) != AHX_OK) {
        fail("writing an instrument was refused");
    }
    (void)reparse(&song, mod_len);
    {
        ahx_inst_t back = ahx_instrument(&song, FIX_INST1);

        if (back.volume != 40 || back.filt_speed != 0x2a || back.filt_lo != 0x11 ||
            back.filt_hi != 0x22 || back.vib_depth != 9 || back.hardcut != 6 || back.relcut != 0 ||
            back.plist_speed != 4) {
            printf("ahx_write_check: an instrument field does not read back (speed %u depth %u)\n",
                   back.filt_speed, back.vib_depth);
            failures++;
        }
        if (back.plist_len != FIX_PLIST1) {
            fail("writing an instrument moved its playlist's length");
        }
    }
    if (mod[song.inst_off[FIX_INST1] + 9] != 0xa5 || mod[song.inst_off[FIX_INST1] + 10] != 0x5a ||
        mod[song.inst_off[FIX_INST1] + 11] != 0xff ||
        (mod[song.inst_off[FIX_INST1] + 19] & 0xc0u) != 0xc0u) {
        fail("writing an instrument cleared bytes of its block the reader does not read");
    }

    /* 6. a playlist step: its four bytes, and no others. */
    if (restore(&song) != AHX_OK) {
        return;
    }
    inst = ahx_instrument(&song, FIX_INST1);
    s = ahx_step(&inst, 1);
    s.waveform = AHX_WAVE_SQUARE;
    s.note = 42;
    s.fix_note = 0;
    s.fx2 = 7;
    s.fx1 = 0;
    s.fx1_data = 0x33;
    s.fx2_data = 0x77;
    if (ahx_write_step(mod, &song, FIX_INST1, 1, s) != AHX_OK) {
        fail("writing a playlist step was refused");
    }
    if (diff_bytes(mod, golden, mod_len) != AHX_PLIST_BYTES) {
        fail("writing a playlist step moved more than its four bytes");
    }
    (void)reparse(&song, mod_len);
    {
        ahx_step_t back;

        inst = ahx_instrument(&song, FIX_INST1);
        back = ahx_step(&inst, 1);
        if (back.waveform != AHX_WAVE_SQUARE || back.note != 42 || back.fix_note != 0 ||
            back.fx1 != 0 || back.fx2 != 7 || back.fx1_data != 0x33 || back.fx2_data != 0x77) {
            printf("ahx_write_check: a playlist step does not read back (wave %u note %u)\n",
                   back.waveform, back.note);
            failures++;
        }
    }

    /* 7. the resize. Instrument 1's playlist moves instrument 2 and the names with it. */
    if (restore(&song) != AHX_OK) {
        return;
    }
    inst2_at = song.inst_off[FIX_INST2];
    names_at = song.name_off[0];
    if (ahx_write_plist_len(mod, FIX_CAP, &song, FIX_INST1, FIX_PLIST1 + 3, &size) != AHX_OK) {
        fail("growing a playlist was refused");
        return;
    }
    if (size != mod_len + 3u * AHX_PLIST_BYTES) {
        printf("ahx_write_check: growing a playlist by 3 steps made the module %u bytes, not %u\n",
               (unsigned)size, (unsigned)(mod_len + 3u * AHX_PLIST_BYTES));
        failures++;
    }
    if (reparse(&song, size) != AHX_OK) {
        return;
    }
    if (ahx_instrument(&song, FIX_INST1).plist_len != FIX_PLIST1 + 3) {
        fail("the grown playlist's length does not read back");
    }
    if (song.inst_off[FIX_INST2] != inst2_at + 3u * AHX_PLIST_BYTES) {
        fail("the instrument after the playlist did not move with it");
    }
    if (memcmp(mod + song.inst_off[FIX_INST2], golden + inst2_at, AHX_INST_BYTES) != 0) {
        fail("the instrument after the playlist moved but changed");
    }
    if (song.name_off[0] != names_at + 3u * AHX_PLIST_BYTES) {
        fail("the name block did not move with the playlist");
    }
    if (memcmp(mod + song.name_off[0], golden + names_at, mod_len - names_at) != 0) {
        fail("the name block moved but changed");
    }
    if (song.title_offset != song.name_off[0]) {
        fail("the stored title offset did not follow the name block");
    }
    inst = ahx_instrument(&song, FIX_INST1);
    for (i = FIX_PLIST1; i < FIX_PLIST1 + 3; i++) {
        s = ahx_step(&inst, (uint8_t)i);
        if (s.note != 0 || s.waveform != AHX_WAVE_HOLD || s.fx1 != 0 || s.fx2 != 0 ||
            s.fx1_data != 0 || s.fx2_data != 0) {
            fail("a step a grow added is not empty");
            break;
        }
    }
    if (ahx_write_plist_len(mod, FIX_CAP, &song, FIX_INST1, FIX_PLIST1, &size) != AHX_OK) {
        fail("shrinking a playlist was refused");
        return;
    }
    if (size != mod_len || memcmp(mod, golden, mod_len) != 0) {
        printf("ahx_write_check: grow and shrink left %u of %u bytes different\n",
               diff_bytes(mod, golden, mod_len), (unsigned)mod_len);
        failures++;
    }

    /* 8. what the writer refuses, with the module left alone. */
    if (restore(&song) != AHX_OK) {
        return;
    }
    {
        ahx_cell_t c = { 0, 0, 0, 0 };
        ahx_inst_t in = ahx_instrument(&song, FIX_INST1);

        if (ahx_write_cell(mod, &song, (uint8_t)(song.trk + 1u), 0, c) != AHX_ERR_FIELD) {
            fail("a track past the header's was accepted");
        }
        if (ahx_write_cell(mod, &song, 1, song.trl, c) != AHX_ERR_FIELD) {
            fail("a row past the header's was accepted");
        }
        if (song.track0_absent && ahx_write_cell(mod, &song, 0, 0, c) != AHX_ERR_FIELD) {
            fail("track 0 was accepted in a module that omits it");
        }
        if (ahx_write_instrument(mod, &song, 0, &in) != AHX_ERR_FIELD ||
            ahx_write_instrument(mod, &song, (uint8_t)(song.smp + 1u), &in) != AHX_ERR_FIELD) {
            fail("an instrument past the header's was accepted");
        }
        if (ahx_write_step(mod, &song, FIX_INST1, FIX_PLIST1, ahx_step(&in, 0)) != AHX_ERR_FIELD) {
            fail("a playlist step past the playlist's length was accepted");
        }
        if (ahx_write_plist_len(mod, FIX_CAP, &song, 0, 1, &size) != AHX_ERR_FIELD) {
            fail("instrument 0's playlist was accepted");
        }
        if (ahx_write_plist_len(mod, mod_len + 2u, &song, FIX_INST1, 255, &size) != AHX_ERR_ROOM) {
            fail("a playlist that does not fit the buffer was accepted");
        }
        if (diff_bytes(mod, golden, mod_len) != 0) {
            fail("a refused write moved bytes anyway");
        }
    }
}

int main(void)
{
    check(0);
    check(1);

    if (failures) {
        printf("ahx_write_check: %d failure(s)\n", failures);
        return 1;
    }
    printf("ahx_write_check: ok\n");
    return 0;
}
