/* SPDX-License-Identifier: MIT */
/* AHX-1: play the core's note voice from a command line and write a WAV.
 *
 *   ahx_note_demo [-o out.wav] [-r hz] [-w wave] [-l wlen] [-p pulse] [-f filt] [-v vol]
 *                 [-d ms] [-a ms] [-R ms] note[:ms] ...
 *
 * A note is a MIDI number (36) or a name (c2, f#3, eb4). MIDI 36 is the bottom of the reference's
 * period table at W.LEN 2; the table is five octaves, so 36 to 95 is all of it and anything
 * outside is clamped, as the reference clamps it. W.LEN is a coarse tune as much as a timbre:
 * the cycle is 4 << W.LEN samples, so every step down is an octave up, and the default of 2 is
 * what puts the table's five octaves somewhere a keyboard would look for them. The five
 * instrument fields are the ones an AHX instrument actually has; everything else in the format is
 * the playlist's, and a caller with no playlist has none of it.
 *
 * Output is 16 bit mono at the rate asked for, which defaults to the core's AHX_RATE. The note
 * voice is mono (the module player's panning is the player's, and is what makes a module stereo),
 * so a mono file is what this renders. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ahx_note.h"

/* The instrument's volume is 0..64 and the waveform is int8, so the loudest a sample can be is
 * 64 * 4 * 127 = 32512: inside an int16 already, which is why the conversion below clamps rather
 * than scales. */
#define DEMO_ENV_ONE 4096

/* The table the core asks the caller to own. It is 244 bytes, built once for one rate. */
static uint32_t demo_tab[AHX_NOTE_COUNT];

/* A note as the caller holds it: the instrument is shared by the whole run, the pitch and the
 * length are the note's own. */
typedef struct {
    int pitch16;
    uint32_t samples;
    char name[16];
} demo_note_t;

static void usage(void)
{
    fprintf(stderr,
            "usage: ahx_note_demo [-o out.wav] [-r hz] [-w 0..3] [-l 0..5] [-p 0..31] [-f 1..63]\n"
            "                     [-v 0..64] [-d ms] [-a ms] [-R ms] note[:ms] ...\n"
            "  wave   0 triangle, 1 sawtooth, 2 square, 3 noise\n");
}

/* An option's value, or a message and no value. Everything here is a small integer a shell can
 * write, so strtol with an end check is the whole of the parsing. */
static int number(const char *s, long *out)
{
    char *end;
    long v;

    if (s == NULL || *s == '\0') {
        return 0;
    }
    v = strtol(s, &end, 10);
    if (*end != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

/* A name to a MIDI number: a letter, an optional accidental, an octave. MIDI puts C-1 at 0, so
 * an octave o's C is (o + 1) * 12. Returns -1 on anything else. */
static int note_name(const char *s)
{
    static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };   /* a b c d e f g */
    int letter, semi, octave = 0, i = 0;
    const char *p;

    if (s[0] == '\0') {
        return -1;
    }
    letter = (s[0] >= 'A' && s[0] <= 'G') ? s[0] - 'A' : (s[0] >= 'a' && s[0] <= 'g') ? s[0] - 'a' : -1;
    if (letter < 0) {
        return -1;
    }
    i = 1;
    semi = base[letter];
    if (s[i] == '#') {
        semi++;
        i++;
    } else if (s[i] == 'b') {
        semi--;
        i++;
    }
    p = s + i;
    if (*p == '\0') {
        octave = 4;                 /* no octave written: the middle one, as a tracker would default */
    } else {
        char *end;
        long v = strtol(p, &end, 10);

        if (*end != '\0' || v < -1 || v > 9) {
            return -1;
        }
        octave = (int)v;
    }
    return (octave + 1) * 12 + semi;
}

/* The envelope: attack up, flat, release down, all linear, over the note's own length. It is the
 * caller's and it is per sample, which is what the device's voice_amp() is too; a plugin would
 * put its ADSR here. Returns 0..DEMO_ENV_ONE. */
static int32_t envelope(uint32_t i, uint32_t total, uint32_t attack, uint32_t release)
{
    if (attack > total / 2) {
        attack = total / 2;
    }
    if (release > total - attack) {
        release = total - attack;
    }
    if (attack > 0 && i < attack) {
        return (int32_t)((uint32_t)DEMO_ENV_ONE * i / attack);
    }
    if (release > 0 && i >= total - release) {
        return (int32_t)((uint32_t)DEMO_ENV_ONE * (total - 1 - i) / release);
    }
    return DEMO_ENV_ONE;
}

/* One note, in blocks of AHX_BLOCK, with the envelope applied over the whole of it afterwards.
 *
 * The two loops are the seam and not an accident. ahx_note_begin() takes the block length because
 * the noise's frame clock advances by the samples the block is worth, so a caller that rendered a
 * whole note in one call would drift the noise against the module player's; the core is asked in
 * the same blocks the firmware asks in (Felucca's CTL) and the demo just uses the core's default
 * for that. The envelope is the second loop because it is the caller's, and keeping it out of the
 * sample loop is what lets the firmware fuse its own amp in there instead.
 *
 * out is already zeroed by the caller, which it has to be: ahx_note_render() adds to what is
 * there, so a caller can mix several voices without a scratch buffer. */
static void render_note(int32_t *out, const ahx_note_cfg_t *cfg, int32_t *st,
                        uint32_t total, uint32_t attack, uint32_t release)
{
    uint32_t done = 0, k;

    while (done < total) {
        uint32_t n = total - done;

        if (n > (uint32_t)AHX_BLOCK) {
            n = (uint32_t)AHX_BLOCK;
        }
        ahx_note_render(st, cfg, demo_tab, out + done, n);
        done += n;
    }

    for (k = 0; k < total; k++) {
        out[k] = (out[k] * envelope(k, total, attack, release)) / DEMO_ENV_ONE;
    }
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
    p[2] = (unsigned char)((v >> 16) & 0xffu);
    p[3] = (unsigned char)((v >> 24) & 0xffu);
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xffu);
    p[1] = (unsigned char)((v >> 8) & 0xffu);
}

/* The canonical 44 byte header, 16 bit PCM, one channel. Written by hand rather than through a
 * library for the same reason the core has no dependencies: this file is meant to be built by a
 * compiler and nothing else. */
static int write_wav(const char *path, const int32_t *mix, uint32_t total, uint32_t rate)
{
    unsigned char head[44];
    unsigned char *pcm;
    FILE *f;
    uint32_t i;
    int ok;

    pcm = malloc(total == 0 ? 1u : (size_t)total * 2u);
    if (pcm == NULL) {
        fprintf(stderr, "ahx_note_demo: out of memory for %u samples\n", (unsigned)total);
        return 0;
    }
    for (i = 0; i < total; i++) {
        int32_t v = mix[i];

        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        put16(pcm + (size_t)i * 2u, (unsigned)(v & 0xffff));
    }

    memcpy(head, "RIFF", 4);
    put32(head + 4, 36u + (unsigned long)total * 2u);
    memcpy(head + 8, "WAVE", 4);
    memcpy(head + 12, "fmt ", 4);
    put32(head + 16, 16);
    put16(head + 20, 1);                       /* PCM */
    put16(head + 22, 1);                       /* mono */
    put32(head + 24, rate);
    put32(head + 28, rate * 2u);               /* byte rate */
    put16(head + 32, 2);                       /* block align */
    put16(head + 34, 16);                      /* bits */
    memcpy(head + 36, "data", 4);
    put32(head + 40, (unsigned long)total * 2u);

    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "ahx_note_demo: cannot write %s\n", path);
        free(pcm);
        return 0;
    }
    ok = fwrite(head, 1, sizeof head, f) == sizeof head &&
         (total == 0 || fwrite(pcm, 2, total, f) == total);
    if (fclose(f) != 0) {
        ok = 0;
    }
    free(pcm);
    if (!ok) {
        fprintf(stderr, "ahx_note_demo: %s was not written in full\n", path);
    }
    return ok;
}

int main(int argc, char **argv)
{
    const char *out = "ahx_note.wav";
    long rate = (long)AHX_RATE, wave = 1, wlen = 2, pulse = 16, filt = 32, vol = 48;
    long dur_ms = 250, attack_ms = 4, release_ms = 120;
    demo_note_t notes[256];
    int nnotes = 0, i, rc = 1;
    ahx_note_cfg_t cfg;
    int32_t st[3];
    int32_t *mix = NULL;
    uint32_t total = 0, off = 0, attack, release;
    long v;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        long *slot = NULL;

        if (a[0] == '-' && a[1] != '\0' && a[2] == '\0') {
            switch (a[1]) {
            case 'o': slot = NULL; break;
            case 'r': slot = &rate; break;
            case 'w': slot = &wave; break;
            case 'l': slot = &wlen; break;
            case 'p': slot = &pulse; break;
            case 'f': slot = &filt; break;
            case 'v': slot = &vol; break;
            case 'd': slot = &dur_ms; break;
            case 'a': slot = &attack_ms; break;
            case 'R': slot = &release_ms; break;
            default:
                fprintf(stderr, "ahx_note_demo: unknown option %s\n", a);
                return 2;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "ahx_note_demo: %s wants a value\n", a);
                return 2;
            }
            if (a[1] == 'o') {
                out = argv[++i];
            } else {
                if (!number(argv[++i], &v)) {
                    fprintf(stderr, "ahx_note_demo: %s %s is not a number\n", a, argv[i]);
                    return 2;
                }
                *slot = v;
            }
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "ahx_note_demo: unknown option %s\n", a);
            return 2;
        }

        /* A note, and optionally its own length after a colon: "c3:400". The colon is that note's
         * own and leaves -d's default where it was, so a run can be uniform or written out note by
         * note. */
        {
            char buf[16];
            const char *colon = strchr(a, ':');
            size_t len = colon == NULL ? strlen(a) : (size_t)(colon - a);
            long ms = dur_ms;
            int midi;

            if (len == 0 || len >= sizeof buf) {
                fprintf(stderr, "ahx_note_demo: %s is not a note\n", a);
                return 2;
            }
            memcpy(buf, a, len);
            buf[len] = '\0';
            if (buf[0] >= '0' && buf[0] <= '9') {
                if (!number(buf, &v) || v < 0 || v > 127) {
                    fprintf(stderr, "ahx_note_demo: %s is not a MIDI note (0..127)\n", buf);
                    return 2;
                }
                midi = (int)v;
            } else {
                midi = note_name(buf);
                if (midi < 0) {
                    fprintf(stderr, "ahx_note_demo: %s is not a note name\n", buf);
                    return 2;
                }
            }
            if (nnotes == (int)(sizeof notes / sizeof notes[0])) {
                fprintf(stderr, "ahx_note_demo: at most %d notes at a time\n", nnotes);
                return 2;
            }
            notes[nnotes].pitch16 = midi - 36;
            if (notes[nnotes].pitch16 < 0) {
                notes[nnotes].pitch16 = 0;
            } else {
                notes[nnotes].pitch16 *= 16;
            }
            snprintf(notes[nnotes].name, sizeof notes[nnotes].name, "%s", buf);

            if (colon != NULL) {
                if (!number(colon + 1, &ms) || ms <= 0) {
                    fprintf(stderr, "ahx_note_demo: %s wants a positive length in ms\n", a);
                    return 2;
                }
            }
            if (ms <= 0 || ms > 60000) {
                fprintf(stderr, "ahx_note_demo: a note is 1 to 60000 ms\n");
                return 2;
            }
            notes[nnotes].samples = (uint32_t)(rate * ms / 1000);
            if (notes[nnotes].samples == 0) {
                notes[nnotes].samples = 1;
            }
            /* The whole run is one allocation whose length is the sum of the notes, so the sum is
             * the one place a size can go wrong. A quarter of a gigasample is over an hour at
             * 44.1 kHz, which is past any sequence a command line is going to hold. */
            if (notes[nnotes].samples > (1u << 28) - total) {
                fprintf(stderr, "ahx_note_demo: the sequence is too long (over %u samples)\n",
                        1u << 28);
                return 2;
            }
            total += notes[nnotes].samples;
            nnotes++;
        }
    }

    if (nnotes == 0) {
        usage();
        return 2;
    }
    if (rate < 1000 || rate > 192000) {
        fprintf(stderr, "ahx_note_demo: -r takes 1000 to 192000 Hz\n");
        return 2;
    }

    attack = (uint32_t)(rate * attack_ms / 1000);
    release = (uint32_t)(rate * release_ms / 1000);
    if (release == 0) {
        release = 1;
    }

    mix = calloc(total, sizeof *mix);
    if (mix == NULL) {
        fprintf(stderr, "ahx_note_demo: out of memory for %u samples\n", (unsigned)total);
        return 1;
    }

    /* Once per rate, before the first note: the core keeps no global state, so the table and the
     * voice state are the caller's, and this is the whole of what a caller has to set up. */
    ahx_note_bank_build(demo_tab, (uint32_t)rate);
    ahx_note_retrigger(st);

    cfg.wave = (int32_t)wave;
    cfg.wlen = (int32_t)wlen;
    cfg.pulse = (int32_t)pulse;
    cfg.filt = (int32_t)filt;
    cfg.vol = (int32_t)vol;
    cfg.fine = 0;

    printf("ahx_note_demo: %ld Hz, wave %ld, wlen %ld (cycle %u), filt %ld, vol %ld\n",
           rate, wave, wlen, 4u << (unsigned)wlen, filt, vol);
    for (i = 0; i < nnotes; i++) {
        cfg.pitch16 = notes[i].pitch16;
        /* Every note starts its waveform again, which is what a key press does. A caller that
         * wanted a repeated note to carry on inside the waveform instead would keep st[0] across
         * this call - which is exactly what the device's engine_t does with its keep mask. */
        ahx_note_retrigger(st);
        render_note(mix + off, &cfg, st, notes[i].samples, attack, release);
        printf("  %-4s pitch16 %4d  %u samples\n", notes[i].name, notes[i].pitch16,
               (unsigned)notes[i].samples);
        off += notes[i].samples;
    }

    rc = write_wav(out, mix, total, (uint32_t)rate) ? 0 : 1;
    if (rc == 0) {
        /* The peak is printed so that `make check` can tell an engine that rendered a note from
         * one that rendered nothing, without decoding the file it just wrote. It is the loudest
         * sample before the clamp, in int16 units: the instrument's volume at full scale through
         * an int8 waveform is 32512 and the envelope only takes from that. */
        int32_t peak = 0;

        for (off = 0; off < total; off++) {
            int32_t v2 = mix[off] < 0 ? -mix[off] : mix[off];

            if (v2 > peak) {
                peak = v2;
            }
        }
        printf("%s: %u samples, %.2f s, 16 bit mono, peak=%d\n", out, (unsigned)total,
               (double)total / (double)rate, (int)peak);
    }
    free(mix);
    return rc;
}
