/* SPDX-License-Identifier: MIT AND BSD-3-Clause */
/* AHX-1: the player: the frame clock, the effects, and the four voices.
 */
#include "ahx_libc.h"      /* memset: <string.h> on the host, Felucca's libc.c on the device */

#include "ahx_player.h"
#include "ahx_fixed.h"      /* ahx_wn_next: the noise walk, shared with the engine */

/* The reference's vibrato table, transcribed from replay.c. A quarter of a sine in 8.8
 * fixed point, four quadrants of sixteen. Compared against the reference's own array by
 * tests/reference/tables_check.c. */
const int16_t ahx_vib_tab[64] = {
       0,   24,   49,   74,   97,  120,  141,  161,  180,  197,  212,  224,  235,  244,  250,  253,
     255,  253,  250,  244,  235,  224,  212,  197,  180,  161,  141,  120,   97,   74,   49,   24,
       0,  -24,  -49,  -74,  -97, -120, -141, -161, -180, -197, -212, -224, -235, -244, -250, -253,
    -255, -253, -250, -244, -235, -224, -212, -197, -180, -161, -141, -120,  -97,  -74,  -49,  -24
};

/* The five panning settings, from hard left to hard right, and the gain that goes with each:
 * the width costs volume, so the widest setting is mixed loudest. stereo 2 is the middle. */
static const uint8_t ahx_stereopan_left[5] = { 128, 96, 64, 32, 0 };
static const uint8_t ahx_stereopan_right[5] = { 128, 160, 193, 225, 255 };
static const uint8_t ahx_defgain[5] = { 71, 72, 76, 85, 100 };

/* Which side each of the four channels sits on: left, right, right, left by the reference's
 * table, which pans voices 0 and 3 out of one side and 1 and 2 out of the other. */
static const uint8_t ahx_pan_side[AHX_CHANNELS] = { 0, 1, 1, 0 };

/* How many samples are summed before the mix gain is applied and the pair is stored. The sum
 * has to be complete before the gain, so it is kept in an accumulator; this is how long that
 * is. It is a stack frame (two int32 arrays, 512 bytes here) and not a field of the player,
 * because a field would be 2 KB of the device's .bss and AHX-1 has about 7 KB of RAM free.
 *
 * On the device the call is inside the audio ISR (fx.c mix_block(), ahx_module.c) and
 * the frame comes out of the supervisor stack, which is 7,936 bytes with a 256 byte guard band,
 * so 64 is also the size that keeps an ISR frame modest: a 256 sample block would be 2,048
 * bytes of that stack. It is still twice the device's block (CTL 32), which is what the bus
 * asks for in one call.
 *
 * The length is not audible and not a tuning knob: a sample's sum is over the four voices at
 * that sample and does not depend on how the calls are cut, which is what tests/block_check.c
 * measures at eleven block sizes and three rates. */
#define AHX_MIX_BLOCK 64

/* A filtered waveform in the table is reached through ahx_wave_block() in ahx_tables.c,
 * which is where its address arithmetic lives. The frame step here records *which* waveform and
 * *where* in it (a variant, an offset and a filter position) rather than the address, because
 * the device has no table to point into: it generates the one waveform into a shared scratch, so
 * a pointer taken here would be clobbered by the next voice's frame step. ahx_voice_set_audio()
 * resolves the description per voice, and the two builds meet there. See ahx_voice.h. */

/* The reference's white noise state update, which walks the voice a random distance into the
 * noise table every tick, is ahx_fixed.c's ahx_wn_next: it is shared with the engine, which
 * keeps the same frame clock for the same instrument. Its >> 8 is an arithmetic shift on a
 * signed int32, which sign extends before the rotate brings the low byte back on top; both are
 * spelled out there rather than left to an implementation-defined shift. */

/* ---------------------------------------------------------------------------------------
 * The track commands, split into three the way the reference splits them, by what they
 * touch: the tune's transport, the voice's pitch, and the rest. The numbers are its own, so
 * the functions can be read beside hvl_process_stepfx_1/2/3.
 * --------------------------------------------------------------------------------------- */

/* The transport, the volume slide and the panning. The panning read is signed the
 * reference's way: 0 to 127 is right of centre and 128 to 255 is left of it, so the value it
 * stores is the offset from centre plus 128. */
static void ahx_stepfx_1(ahx_player_t *p, ahx_voice_t *v, unsigned fx, unsigned data)
{
    switch (fx) {
    case AHX_CMD_JUMP_HI:
        /* Not a command of its own: its data is the hundreds digit of the B command that
         * follows, which is why B is the only one that takes three digits. */
        if ((data & 0x0f) > 0 && (data & 0x0f) <= 9) {
            p->pos_jump = (uint16_t)(data & 0x0f);
        }
        break;

    case AHX_CMD_VOLUME_PORTA:
    case AHX_CMD_VOLUME_SLIDE:
        v->volume_slide_down = (int16_t)(data & 0x0f);
        v->volume_slide_up = (int16_t)(data >> 4);
        break;

    case AHX_CMD_PANNING:
        v->pan = (uint16_t)((int32_t)data - (data > 127 ? 256 : 0) + 128);
        v->set_pan = v->pan;
        v->pan_mult_left = (int16_t)p->pan->left[v->pan];
        v->pan_mult_right = (int16_t)p->pan->right[v->pan];
        break;

    case AHX_CMD_POSITION_JUMP:
        /* Three digits, so two commands: the digits arrive as one value. The jump is only
         * compared against the current position for the song's end, which is why a jump
         * backwards repeats and a jump forwards is not checked at all. */
        p->pos_jump = (uint16_t)(p->pos_jump * 100 + (data & 0x0f) + (data >> 4) * 10);
        p->pattern_break = 1;
        if (p->pos_jump <= (uint16_t)p->pos_nr) {
            p->song_end_reached = 1;
        }
        break;

    case AHX_CMD_ROW_BREAK:
        p->pos_jump = (uint16_t)(p->pos_nr + 1);
        p->pos_jump_note = (uint16_t)((data & 0x0f) + (data >> 4) * 10);
        p->pattern_break = 1;
        if (p->pos_jump_note > p->song->trl) {
            p->pos_jump_note = 0;
        }
        break;

    case AHX_CMD_EXTENDED:
        if ((data >> 4) == 0xc) {                       /* note cut */
            if ((int)(data & 0x0f) < p->tempo) {
                v->note_cut_wait = (int16_t)(data & 0x0f);
                if (v->note_cut_wait) {
                    v->note_cut_on = 1;
                    v->hard_cut_release = 0;
                }
            }
        }
        break;

    case AHX_CMD_SPEED:
        p->tempo = (int16_t)data;
        if (data == 0) {
            p->song_end_reached = 1;
        }
        break;
    }
}

/* The period of a note, for the tone portamento's limit, which is the one place a note
 * reaches the table without the frame step's own clamp in front of it. A note is six bits
 * but the table holds sixty-one entries, so a malformed module can name one there is no
 * period for; the reference reads past its table and gets whatever neighbours it, a zero in
 * the binary we compare against. No track cell in the corpus names one, so this guard is
 * never taken there (docs/ahx-engine.md), and the top note is the nearest thing to an
 * answer. The frame step needs no guard here because it clamps the note to 5*12 itself. */
static uint16_t ahx_period_of(unsigned note)
{
    return ahx_period_tab[note > 60 ? 60 : note];
}

/* The two commands that slide towards a note: 3 on its own and 5 with a volume slide as
 * well. The limit is where the slide stops, worked out from the note the cell asks for and
 * the note the voice is on. A limit of zero means the slide has nothing to do, so it goes on
 * without one. Note is cleared on the way out, because a cell that slides to a note does not
 * also jump to it. */
static int32_t ahx_stepfx_2(ahx_player_t *p, ahx_voice_t *v, unsigned fx, unsigned data,
                            int32_t note)
{
    (void)p;

    switch (fx) {
    case AHX_CMD_SQUARE:
        v->square_pos = (int16_t)(data >> (5 - v->wave_length));
        v->plant_square = 1;
        v->ignore_square = 1;
        break;

    case AHX_CMD_VOLUME_PORTA:
    case AHX_CMD_PITCH_PORTA:
        if (data != 0) {
            v->period_slide_speed = (int16_t)data;
        }
        if (note) {
            int32_t diff = (int32_t)ahx_period_of(v->track_period) - ahx_period_of((unsigned)note);
            int32_t target = diff + v->period_slide_period;

            if (target) {
                v->period_slide_limit = (int16_t)-diff;
            }
        }
        v->period_slide_on = 1;
        v->period_slide_with_limit = 1;
        note = 0;
        break;
    }
    return note;
}

/* Everything else: the plain pitch slides, the filter override, the three volume targets and
 * the extended commands, which are the fine slides, the vibrato depth, the two fine volume
 * steps and the transpose pin. */
static void ahx_stepfx_3(ahx_player_t *p, ahx_voice_t *v, unsigned fx, unsigned data)
{
    int32_t d;
    unsigned i;

    switch (fx) {
    case AHX_CMD_PITCH_UP:
        v->period_slide_speed = (int16_t)-(int32_t)data;
        v->period_slide_on = 1;
        v->period_slide_with_limit = 0;
        break;

    case AHX_CMD_PITCH_DOWN:
        v->period_slide_speed = (int16_t)data;
        v->period_slide_on = 1;
        v->period_slide_with_limit = 0;
        break;

    case AHX_CMD_FILTER:
        /* 0x40 is the unfiltered waveform, which the frame picks up on its own, so it and 0
         * are both ignored. Below 0x40 the value holds the filter where it is until the
         * playlist asks for the next position; above it the filter moves there now. */
        if (data == 0 || data == 0x40) {
            break;
        }
        if (data < 0x40) {
            v->ignore_filter = (int16_t)data;
            break;
        }
        if (data > 0x7f) {
            break;
        }
        v->filter_pos = (int16_t)(data - 0x40);
        break;

    case AHX_CMD_VOLUME:
        /* Three ranges, three targets: the note's own volume up to 0x40, every voice's track
         * volume from 0x50, and this voice's track volume from 0xa0. The two gaps between the
         * ranges are unreachable and fall through doing nothing. */
        if ((int32_t)data <= 0x40) {
            v->note_max_volume = (int16_t)data;
            break;
        }
        d = (int32_t)data - 0x50;
        if (d < 0) {
            break;
        }
        if (d <= 0x40) {
            for (i = 0; i < AHX_CHANNELS; i++) {
                p->voice[i].track_master_volume = (int16_t)d;
            }
            break;
        }
        d -= 0xa0 - 0x50;
        if (d < 0) {
            break;
        }
        if (d <= 0x40) {
            v->track_master_volume = (int16_t)d;
        }
        break;

    case AHX_CMD_EXTENDED:
        switch (data >> 4) {
        case 0x1:                                       /* fine slide up */
            v->period_slide_period -= (int16_t)(data & 0x0f);
            v->plant_period = 1;
            break;
        case 0x2:                                       /* fine slide down */
            v->period_slide_period += (int16_t)(data & 0x0f);
            v->plant_period = 1;
            break;
        case 0x4:                                       /* vibrato depth */
            v->vibrato_depth = (int16_t)(data & 0x0f);
            break;
        case 0xa:                                       /* fine volume up */
            v->note_max_volume += (int16_t)(data & 0x0f);
            if (v->note_max_volume > 0x40) {
                v->note_max_volume = 0x40;
            }
            break;
        case 0xb:                                       /* fine volume down */
            v->note_max_volume -= (int16_t)(data & 0x0f);
            if (v->note_max_volume < 0) {
                v->note_max_volume = 0;
            }
            break;
        case 0xf:                                       /* misc flags */
            /* The one flag AHX1 defines: pin the voice to the position's transpose instead
             * of letting a note go on overriding it. There is no such flag in AHX0. */
            if (p->song->version < 1) {
                break;
            }
            if ((data & 0x0f) == 1) {
                v->override_transpose = v->transpose;
            }
            break;
        }
        break;
    }
}

/* ---------------------------------------------------------------------------------------
 * The instrument, which is where most of a step goes.
 * --------------------------------------------------------------------------------------- */

/* Load an instrument into a voice: the envelope, the waveform, the volumes, the square and
 * filter modulation and the playlist that drives them. The reference resets a long list of
 * fields here rather than in the voice, which is why the voice's own reset is so short: this
 * is the reset that matters, and it happens on every note that names an instrument.
 *
 * The envelope lengths are in ticks and the step is 1/256 of a volume unit per tick, so a
 * length of zero has nothing to divide by. The reference divides anyway and dies on it: two
 * modules of the 1335 in our corpus make it crash with a SIGFPE (Curt Cool's rubber spine
 * and Shinobi's beach buggy, see docs/ahx-engine.md). A render that stops is worse than a
 * voice that holds its volume, so a zero length steps by zero instead and the envelope's
 * own terms never run. */
static void ahx_load_instrument(ahx_player_t *p, ahx_voice_t *v, uint8_t index)
{
    ahx_inst_t ins = ahx_instrument(p->song, index);
    int32_t a = ins.attack_len, d = ins.decay_len, r = ins.release_len;
    int32_t sq_lo, sq_hi, f_lo, f_hi;

    v->pan = v->set_pan;
    v->pan_mult_left = (int16_t)p->pan->left[v->pan];
    v->pan_mult_right = (int16_t)p->pan->right[v->pan];

    v->period_slide_speed = 0;
    v->period_slide_period = 0;
    v->period_slide_limit = 0;

    v->perf_sub_volume = 0x40;
    v->adsr_volume = 0;
    v->inst = ins;
    v->perf_on = 1;
    v->sample_pos = 0;

    v->adsr_a_frames = ins.attack_len;
    v->adsr_a_volume = a ? (int16_t)((int32_t)ins.attack_vol * 256 / a) : 0;
    v->adsr_d_frames = ins.decay_len;
    v->adsr_d_volume = d ? (int16_t)(((int32_t)ins.decay_vol - ins.attack_vol) * 256 / d) : 0;
    v->adsr_s_frames = ins.sustain_len;
    v->adsr_r_frames = ins.release_len;
    v->adsr_r_volume = r ? (int16_t)(((int32_t)ins.release_vol - ins.decay_vol) * 256 / r) : 0;

    /* A wave length is 0..5 for the six cycle lengths, but the field holds three bits. The
     * reference shifts by 5 minus the length, which is undefined for 6 and 7; no module in the
     * corpus has one, and this is the one place a clamped length would matter, so it is
     * clamped here rather than at every shift. */
    v->wave_length = ins.wave_len > 5 ? 5 : ins.wave_len;
    v->note_max_volume = ins.volume;

    v->vibrato_current = 0;
    v->vibrato_delay = ins.vib_delay;
    v->vibrato_depth = ins.vib_depth;
    v->vibrato_speed = ins.vib_speed;
    v->vibrato_period = 0;

    v->hard_cut_release = ins.relcut;
    v->hard_cut = ins.hardcut;

    v->ignore_square = 0;
    v->square_sliding_in = 0;
    v->square_wait = 0;
    v->square_on = 0;

    /* The square limits are positions in the square table, so they scale with the wave
     * length, and the reference takes them in either order rather than rejecting a pair that
     * is the wrong way round. */
    sq_lo = (int16_t)(ins.sqr_lo >> (5 - v->wave_length));
    sq_hi = (int16_t)(ins.sqr_hi >> (5 - v->wave_length));
    if (sq_hi < sq_lo) {
        int16_t t = (int16_t)sq_hi;
        sq_hi = sq_lo;
        sq_lo = t;
    }
    v->square_upper_limit = (int16_t)sq_hi;
    v->square_lower_limit = (int16_t)sq_lo;

    v->ignore_filter = 0;
    v->filter_wait = 0;
    v->filter_on = 0;
    v->filter_sliding_in = 0;

    /* The reference folds the top bits of the two filter limits back into the speed field and
     * then masks them off, which cannot do anything: its loader has already moved those two
     * bits into the speed (replay.c, hvl_load_ahx) and masked the limits, and ahx_read.c
     * reads the same three fields the same way. What is left is the sort. */
    f_lo = ins.filt_lo;
    f_hi = ins.filt_hi;
    v->filter_speed = ins.filt_speed;
    if (f_lo > f_hi) {
        int32_t t = f_lo;
        f_lo = f_hi;
        f_hi = t;
    }
    v->filter_upper_limit = (int16_t)f_hi;
    v->filter_lower_limit = (int16_t)f_lo;
    v->filter_pos = 32;

    v->perf_current = 0;
    v->perf_wait = 0;
    v->perf_speed = ins.plist_speed;
}

/* ---------------------------------------------------------------------------------------
 * A step: one row of one channel.
 * --------------------------------------------------------------------------------------- */

/* Run one row of a channel: the cell's note, instrument and command. The two things that can
 * hold the row off are the note delay, which is what EDx does, and a note that is already
 * delayed and now comes due. */
static void ahx_step_voice(ahx_player_t *p, ahx_voice_t *v)
{
    ahx_cell_t cell;
    int32_t note, instr;

    if (!v->track_on) {
        return;
    }

    v->volume_slide_up = 0;
    v->volume_slide_down = 0;

    cell = ahx_cell(p->song, (uint8_t)v->track, (uint8_t)p->note_nr);
    note = cell.note;
    instr = cell.instrument;

    /* The note delay. AHX has one command per cell, so the reference's second check of its
     * twin command field can never fire and is not reproduced. */
    if (cell.command == AHX_CMD_EXTENDED && (cell.data & 0xf0) == 0xd0) {
        if (v->note_delay_on) {
            v->note_delay_on = 0;
        } else if ((int)(cell.data & 0x0f) < p->tempo) {
            v->note_delay_wait = (int16_t)(cell.data & 0x0f);
            if (v->note_delay_wait) {
                v->note_delay_on = 1;
                return;
            }
        }
    }

    /* A note in the cell lets the transpose follow the position again, undoing a pin. */
    if (note) {
        v->override_transpose = 1000;
    }

    ahx_stepfx_1(p, v, cell.command, cell.data);

    if (instr && instr <= p->song->smp) {
        ahx_load_instrument(p, v, (uint8_t)instr);
    }

    /* Cleared here rather than per cell, so that a slide started last row and a slide started
     * this row are told apart by the command, not by the field. */
    v->period_slide_on = 0;

    note = ahx_stepfx_2(p, v, cell.command, cell.data, note);

    if (note) {
        v->track_period = (uint16_t)note;
        v->plant_period = 1;
    }

    ahx_stepfx_3(p, v, cell.command, cell.data);
}

/* ---------------------------------------------------------------------------------------
 * The playlist's commands, a different set from the tracks': the instrument's own effects.
 * --------------------------------------------------------------------------------------- */

/* The playlist's commands. The reference renumbers two of them into its HivelyTracker
 * numbering while loading (6 is the volume and 7 the speed, the same commands the tracks
 * have as C and F), and drops the filter half of a toggle in AHX0, where there was no
 * filter. Both are done here, where the step is decoded.
 *
 * Two of the reference's cases are not reproduced because the three bit field cannot reach
 * them: 7 and 8 are the ring modulation, which the renumbering has moved out of the way, and
 * 9 is the playlist's own panning, which only HivelyTracker modules use. */
static void ahx_plist_command(ahx_player_t *p, ahx_voice_t *v, unsigned fx, unsigned data)
{
    if (fx == 6) {
        fx = 12;
    } else if (fx == 7) {
        fx = 15;
    }
    if (p->song->version == 0 && fx == 4 && (data & 0xf0) != 0) {
        data &= 0x0f;
    }

    switch (fx) {
    case 0:                                             /* set filter */
        if (data > 0 && data < 0x40) {
            if (v->ignore_filter) {
                v->filter_pos = v->ignore_filter;
                v->ignore_filter = 0;
            } else {
                v->filter_pos = (int16_t)data;
            }
            v->new_waveform = 1;
        }
        break;

    case 1:                                             /* slide the pitch up */
        v->period_perf_slide_speed = (int16_t)data;
        v->period_perf_slide_on = 1;
        break;

    case 2:                                             /* slide the pitch down */
        v->period_perf_slide_speed = (int16_t)-(int32_t)data;
        v->period_perf_slide_on = 1;
        break;

    case 3:                                             /* set the square offset */
        if (v->ignore_square == 0) {
            v->square_pos = (int16_t)(data >> (5 - v->wave_length));
        } else {
            v->ignore_square = 0;
        }
        break;

    case 4:                                             /* toggle the modulation */
        if (data == 0) {
            v->square_init = (v->square_on ^= 1);
            v->square_sign = 1;
        } else {
            if (data & 0x0f) {
                v->square_init = (v->square_on ^= 1);
                v->square_sign = ((data & 0x0f) == 0x0f) ? -1 : 1;
            }
            if (data & 0xf0) {
                v->filter_init = (v->filter_on ^= 1);
                v->filter_sign = ((data & 0xf0) == 0xf0) ? -1 : 1;
            }
        }
        break;

    case 5:                                             /* jump in the playlist */
        v->perf_current = (int16_t)data;
        break;

    case 12:                                            /* volume */
        if ((int32_t)data <= 0x40) {
            v->note_max_volume = (int16_t)data;
            break;
        }
        if ((int32_t)data - 0x50 < 0) {
            break;
        }
        if ((int32_t)data - 0x50 <= 0x40) {
            v->perf_sub_volume = (int16_t)((int32_t)data - 0x50);
            break;
        }
        if ((int32_t)data - 0xa0 < 0) {
            break;
        }
        if ((int32_t)data - 0xa0 <= 0x40) {
            v->track_master_volume = (int16_t)((int32_t)data - 0xa0);
        }
        break;

    case 15:                                            /* speed */
        v->perf_speed = (int16_t)data;
        v->perf_wait = (int16_t)data;
        break;
    }
}

/* ---------------------------------------------------------------------------------------
 * A tick of one voice: the envelopes, the slides, the playlist and the waveform.
 * --------------------------------------------------------------------------------------- */

static void ahx_frame_voice(ahx_player_t *p, ahx_voice_t *v)
{
    if (!v->track_on) {
        return;
    }

    if (v->note_delay_on) {
        if (v->note_delay_wait <= 0) {
            ahx_step_voice(p, v);
        } else {
            v->note_delay_wait--;
        }
    }

    /* Hard cut: when the next row's cell names an instrument, this note stops after the
     * number of ticks the instrument asks for instead of running to the end of the row. The
     * lookahead runs off the end of the track into the next position's first row, which is
     * why the voice keeps the next position's track as well as this one's. */
    if (v->hard_cut) {
        int32_t nextinst;

        if (p->note_nr + 1 < (int32_t)p->song->trl) {
            nextinst = ahx_cell(p->song, (uint8_t)v->track, (uint8_t)(p->note_nr + 1)).instrument;
        } else {
            nextinst = ahx_cell(p->song, (uint8_t)v->next_track, 0).instrument;
        }

        if (nextinst) {
            int32_t wait = p->tempo - v->hard_cut;

            if (wait < 0) {
                wait = 0;
            }
            if (!v->note_cut_on) {
                v->note_cut_on = 1;
                v->note_cut_wait = (int16_t)wait;
                v->hard_cut_release_frames = (int16_t)-(wait - p->tempo);
            } else {
                v->hard_cut = 0;
            }
        }
    }

    if (v->note_cut_on) {
        if (v->note_cut_wait <= 0) {
            v->note_cut_on = 0;

            if (v->hard_cut_release) {
                /* A release from where the envelope is now, over the frames the cut left.
                 * The frame count is the note cut's own distance back from the tempo, which
                 * the hard cut only sets when the instrument asks for a cut at all, so it is
                 * never zero here; a tempo of zero would make it zero, and that is a song
                 * that has already ended. */
                if (v->hard_cut_release_frames != 0) {
                    int32_t from = v->adsr_volume - ((int32_t)v->inst.release_vol << 8);

                    v->adsr_r_volume = (int16_t)(-from / v->hard_cut_release_frames);
                    v->adsr_r_frames = v->hard_cut_release_frames;
                    v->adsr_a_frames = 0;
                    v->adsr_d_frames = 0;
                    v->adsr_s_frames = 0;
                }
            } else {
                v->note_max_volume = 0;
            }
        } else {
            v->note_cut_wait--;
        }
    }

    /* The amplitude envelope: attack, decay, sustain, release, one step a tick, each of the
     * stepping stages ending by snapping to its target rather than stepping onto it. */
    if (v->adsr_a_frames) {
        v->adsr_volume += v->adsr_a_volume;
        if (--v->adsr_a_frames <= 0) {
            v->adsr_volume = (int32_t)v->inst.attack_vol << 8;
        }
    } else if (v->adsr_d_frames) {
        v->adsr_volume += v->adsr_d_volume;
        if (--v->adsr_d_frames <= 0) {
            v->adsr_volume = (int32_t)v->inst.decay_vol << 8;
        }
    } else if (v->adsr_s_frames) {
        v->adsr_s_frames--;
    } else if (v->adsr_r_frames) {
        v->adsr_volume += v->adsr_r_volume;
        if (--v->adsr_r_frames <= 0) {
            v->adsr_volume = (int32_t)v->inst.release_vol << 8;
        }
    }

    v->note_max_volume = (int16_t)(v->note_max_volume + v->volume_slide_up -
                                   v->volume_slide_down);
    if (v->note_max_volume < 0) {
        v->note_max_volume = 0;
    } else if (v->note_max_volume > 0x40) {
        v->note_max_volume = 0x40;
    }

    /* The slide to a note. The limit is signed, so a slide away from it is not slowed down,
     * it is reversed; the XOR is the reference's way of asking whether the step went past the
     * limit without the subtraction being able to overflow, which it takes the sign of. */
    if (v->period_slide_on) {
        if (v->period_slide_with_limit) {
            int32_t d0 = v->period_slide_period - v->period_slide_limit;
            int32_t d2 = v->period_slide_speed;

            if (d0 > 0) {
                d2 = -d2;
            }
            if (d0) {
                int32_t d3 = (d0 + d2) ^ d0;

                if (d3 >= 0) {
                    d0 = v->period_slide_period + d2;
                } else {
                    d0 = v->period_slide_limit;
                }
                v->period_slide_period = (int16_t)d0;
                v->plant_period = 1;
            }
        } else {
            v->period_slide_period = (int16_t)(v->period_slide_period + v->period_slide_speed);
            v->plant_period = 1;
        }
    }

    if (v->vibrato_depth) {
        if (v->vibrato_delay <= 0) {
            v->vibrato_period =
                (int16_t)(((int32_t)ahx_vib_tab[v->vibrato_current] * v->vibrato_depth) >> 7);
            v->plant_period = 1;
            v->vibrato_current = (int16_t)((v->vibrato_current + v->vibrato_speed) & 0x3f);
        } else {
            v->vibrato_delay--;
        }
    }

    /* The instrument's playlist, one step every perf_speed ticks. The step can change the
     * waveform, run two effects and move the note, and it is the only thing that gives an
     * AHX instrument a shape over time. */
    if (v->perf_on) {
        if (v->perf_current < (int16_t)v->inst.plist_len) {
            if (--v->perf_wait <= 0) {
                int32_t cur = v->perf_current++;
                ahx_step_t step;

                v->perf_wait = v->perf_speed;
                step = ahx_step(&v->inst, (uint8_t)cur);

                if (step.waveform) {
                    v->waveform = (uint8_t)(step.waveform - 1);
                    v->new_waveform = 1;
                    v->period_perf_slide_speed = 0;
                    v->period_perf_slide_period = 0;
                }

                /* A step that does not set the waveform stops any playlist slide. */
                v->period_perf_slide_on = 0;

                ahx_plist_command(p, v, step.fx1, step.fx1_data);
                ahx_plist_command(p, v, step.fx2, step.fx2_data);

                if (step.note) {
                    v->instr_period = step.note;
                    v->plant_period = 1;
                    v->fixed_note = step.fix_note;
                }
            }
        } else {
            /* Past the end of the playlist the speed decays to nothing instead of the slide
             * running on for ever. */
            if (v->perf_wait) {
                v->perf_wait--;
            } else {
                v->period_perf_slide_speed = 0;
            }
        }
    }

    if (v->period_perf_slide_on) {
        v->period_perf_slide_period =
            (int16_t)(v->period_perf_slide_period - v->period_perf_slide_speed);
        if (v->period_perf_slide_period) {
            v->plant_period = 1;
        }
    }

    /* Square modulation: the pulse width walks between the two limits and turns round at
     * each, one step every square_wait ticks. The square is the waveform whose shape the
     * instrument can change while it plays. */
    if (v->waveform == AHX_WAVE_SQUARE - 1 && v->square_on) {
        if (--v->square_wait <= 0) {
            int32_t d1 = v->square_lower_limit;
            int32_t d2 = v->square_upper_limit;
            int32_t d3 = v->square_pos;

            if (v->square_init) {
                v->square_init = 0;
                if (d3 <= d1) {
                    v->square_sliding_in = 1;
                    v->square_sign = 1;
                } else if (d3 >= d2) {
                    v->square_sliding_in = 1;
                    v->square_sign = -1;
                }
            }

            if (d1 == d3 || d2 == d3) {
                if (v->square_sliding_in) {
                    v->square_sliding_in = 0;
                } else {
                    v->square_sign = (int16_t)-v->square_sign;
                }
            }

            d3 += v->square_sign;
            v->square_pos = (int16_t)d3;
            v->plant_square = 1;
            v->square_wait = v->inst.sqr_speed;
        }
    }

    /* Filter modulation, the same walk in the filter table. Below a speed of three the filter
     * moves more than one position a tick, which is how the reference reaches its shortest
     * filter sweeps. */
    if (v->filter_on && --v->filter_wait <= 0) {
        int32_t d1 = v->filter_lower_limit;
        int32_t d2 = v->filter_upper_limit;
        int32_t d3 = v->filter_pos;
        uint32_t fmax = (v->filter_speed < 3) ? (uint32_t)(5 - v->filter_speed) : 1u;
        uint32_t i;

        if (v->filter_init) {
            v->filter_init = 0;
            if (d3 <= d1) {
                v->filter_sliding_in = 1;
                v->filter_sign = 1;
            } else if (d3 >= d2) {
                v->filter_sliding_in = 1;
                v->filter_sign = -1;
            }
        }

        for (i = 0; i < fmax; i++) {
            if (d1 == d3 || d2 == d3) {
                if (v->filter_sliding_in) {
                    v->filter_sliding_in = 0;
                } else {
                    v->filter_sign = (int16_t)-v->filter_sign;
                }
            }
            d3 += v->filter_sign;
        }
        if (d3 < 1) {
            d3 = 1;
        }
        if (d3 > 63) {
            d3 = 63;
        }
        v->filter_pos = (int16_t)d3;
        v->new_waveform = 1;
        v->filter_wait = (int16_t)(v->filter_speed - 3);
        if (v->filter_wait < 1) {
            v->filter_wait = 1;
        }
    }

    /* The square is built by hand, because its pulse width is a place in the waveform rather
     * than a shape of its own: the wave length gives the stride and the square position gives
     * the start, and the two halves of the cycle read the pulse block in opposite directions.
     * The reference writes this into the voice's own buffer and points its shared square slot
     * at it; the slot is read back in the same voice's step, so the per voice buffer is the same
     * thing without the aliasing.
     *
     * The pulse width is asked for as its own variant rather than as an advance into a block of
     * all thirty-two of them, which is what the reference's table makes possible and what the
     * device's scratch cannot: a waveform there is generated one at a time, so reading across
     * them would mean building the 4,096 byte region for every filter position. The variant is
     * the same bytes either way, because the squares are laid out one 0x80 byte block each in
     * the same order (ahx_tables.c ahx_variant_offset), and the width is clamped into 1..32
     * by the reflection below before it is used: x is square_pos << (5 - W.LEN), and the
     * reference's own fold at 0x20 makes 0x40 - x of anything above it. */
    if (v->waveform == AHX_WAVE_SQUARE - 1 || v->plant_square) {
        int32_t x = (int32_t)v->square_pos << (5 - v->wave_length);
        const int8_t *sq;
        uint32_t count = ((uint32_t)1 << v->wave_length) * 4u;
        uint32_t delta = 32u >> v->wave_length;
        uint32_t i;

        if (x > 0x20) {
            x = 0x40 - x;
            v->square_reverse = 1;
        }
        if (x < 1) {
            x = 1;
        }

        sq = ahx_wave_block(p->waves, AHX_VARIANT_OF_SQUARE((uint32_t)(x - 1)), 0, v->filter_pos);

        for (i = 0; i < count; i++) {
            v->square_temp[i] = *sq;
            sq += delta;
        }

        v->new_waveform = 1;
        v->waveform = AHX_WAVE_SQUARE - 1;
        v->plant_square = 0;
    }

    /* The noise is a fresh waveform every tick, because it reads a moving window into its
     * table. */
    if (v->waveform == AHX_WAVE_NOISE - 1) {
        v->new_waveform = 1;
    }

    if (v->new_waveform) {
        /* The filter position is the address, as it is for the square: 0x20 is the
         * unfiltered waveform and everything on either side is a whole number of strides
         * away, into the low passes or into the high passes. The square is the exception:
         * the pass above has already built the filtered copy into the voice's own buffer.
         *
         * What is left here is the description ahx_voice_set_audio() resolves: the variant
         * names the waveform, and the offset is how far into it this voice reads. The wave
         * length is not an offset of its own any more, because it is what names the variant
         * for the two waveforms that have six of them. */
        v->wave_own = 0;
        v->wave_offset = 0;
        switch (v->waveform) {
        case AHX_WAVE_TRIANGLE - 1:
            v->wave_variant = AHX_VARIANT_OF_TRIANGLE(v->wave_length);
            break;
        case AHX_WAVE_SAWTOOTH - 1:
            v->wave_variant = AHX_VARIANT_OF_SAWTOOTH(v->wave_length);
            break;
        case AHX_WAVE_SQUARE - 1:
            v->wave_variant = AHX_VARIANT_OF_SQUARE(0);
            v->wave_own = 1;
            break;
        default:
            /* The noise is the one waveform whose offset moves: its window is a whole voice
             * length of the three it has, and the frame clock walks it. The window is even,
             * because the reference's table holds the noise as 16 bit samples and this walks
             * pairs of bytes. */
            v->wave_variant = AHX_VARIANT_OF_NOISE;
            v->wave_offset = (int32_t)(((uint32_t)v->wn_random & (2u * 0x280u - 1u)) & ~1u);
            v->wn_random = ahx_wn_next(v->wn_random);
            break;
        }
    }

    /* The period: the note the instrument is on, moved by the position's transpose and the
     * channel's, then clamped into the table and taken through it. A note the playlist fixed
     * does not transpose, which is what keeps a drum on its own pitch. */
    v->audio_period = (int16_t)v->instr_period;
    if (!v->fixed_note) {
        if (v->override_transpose != 1000) {
            v->audio_period += v->override_transpose + (int16_t)v->track_period - 1;
        } else {
            v->audio_period += v->transpose + (int16_t)v->track_period - 1;
        }
    }
    if (v->audio_period > 5 * 12) {
        v->audio_period = 5 * 12;
    }
    if (v->audio_period < 0) {
        v->audio_period = 0;
    }
    v->audio_period = (int16_t)ahx_period_tab[v->audio_period];

    if (!v->fixed_note) {
        v->audio_period += v->period_slide_period;
    }
    v->audio_period += v->period_perf_slide_period + v->vibrato_period;

    if (v->audio_period > 0x0d60) {
        v->audio_period = 0x0d60;
    }
    if (v->audio_period < 0x0071) {
        v->audio_period = 0x0071;
    }

    /* The volume chain: the envelope, then the note's volume, then the playlist's, then the
     * track's, each a shift of six so four of them come to the one volume the mixer uses. */
    v->audio_volume = (int16_t)(((((((v->adsr_volume >> 8) * v->note_max_volume) >> 6) *
                                   v->perf_sub_volume) >> 6) * v->track_master_volume) >> 6);
}

/* ---------------------------------------------------------------------------------------
 * The transport.
 * --------------------------------------------------------------------------------------- */

/* One tick: step the rows when the wait is up, run every voice's frame, advance the row and
 * position when its ticks are done, then hand each voice its period and waveform. */
static void ahx_play_irq(ahx_player_t *p)
{
    unsigned i;

    if (p->step_wait_frames <= 0) {
        if (p->get_new_position) {
            int16_t nextpos = (p->pos_nr + 1 == (int16_t)p->song->len)
                                  ? 0 : (int16_t)(p->pos_nr + 1);

            for (i = 0; i < AHX_CHANNELS; i++) {
                uint8_t track = 0;
                int8_t transpose = 0;

                /* A position the module does not have reads as track 0, transpose 0. The
                 * reference reads whatever is next in its tune struct there; no module in the
                 * corpus has a position list that goes out of range (docs/ahx-engine.md), so
                 * this only ever fires on a module the reference would be reading rubbish
                 * from anyway. */
                ahx_position(p->song, (uint16_t)p->pos_nr, i, &track, &transpose);
                p->voice[i].track = track;
                p->voice[i].transpose = transpose;

                track = 0;
                transpose = 0;
                ahx_position(p->song, (uint16_t)nextpos, i, &track, &transpose);
                p->voice[i].next_track = track;
                p->voice[i].next_transpose = transpose;
            }
            p->get_new_position = 0;
        }

        for (i = 0; i < AHX_CHANNELS; i++) {
            ahx_step_voice(p, &p->voice[i]);
        }
        p->step_wait_frames = p->tempo;
    }

    for (i = 0; i < AHX_CHANNELS; i++) {
        ahx_frame_voice(p, &p->voice[i]);
    }

    p->playing_time++;

    /* The row ends when its ticks are up. A tempo of zero never reaches here, which is the
     * position an F command with no data leaves the song in. */
    if (p->tempo > 0 && --p->step_wait_frames <= 0) {
        if (!p->pattern_break) {
            p->note_nr++;
            if (p->note_nr >= (int16_t)p->song->trl) {
                p->pos_jump = (uint16_t)(p->pos_nr + 1);
                p->pos_jump_note = 0;
                p->pattern_break = 1;
            }
        }

        if (p->pattern_break) {
            p->pattern_break = 0;
            p->pos_nr = (int16_t)p->pos_jump;
            p->note_nr = (int16_t)p->pos_jump_note;

            /* Off the end of the position list is the song's end, and the transport rewinds
             * to the restart position so that a caller that keeps going hears the loop. */
            if (p->pos_nr == (int16_t)p->song->len) {
                p->song_end_reached = 1;
                p->pos_nr = (int16_t)p->restart;
            }

            p->pos_jump_note = 0;
            p->pos_jump = 0;
            p->get_new_position = 1;
        }
    }

    for (i = 0; i < AHX_CHANNELS; i++) {
        ahx_voice_set_audio(&p->voice[i], p->waves, p->frequency);
    }
}

/* Sum the four voices and store them. The gain is applied to the sum the way the reference
 * applies it, and the store is a truncation rather than a clip: a loud passage wraps, which
 * is what the oracle does and therefore what a byte comparison needs. */
static void ahx_mix(ahx_player_t *p, int16_t *out, uint32_t samples)
{
    uint32_t done = 0;

    while (done < samples) {
        int32_t acc_left[AHX_MIX_BLOCK], acc_right[AHX_MIX_BLOCK];
        uint32_t n = samples - done;
        uint32_t i;

        if (n > AHX_MIX_BLOCK) {
            n = AHX_MIX_BLOCK;
        }
        memset(acc_left, 0, n * sizeof *acc_left);
        memset(acc_right, 0, n * sizeof *acc_right);

        for (i = 0; i < AHX_CHANNELS; i++) {
            ahx_voice_mix(&p->voice[i], acc_left, acc_right, n);
        }

        for (i = 0; i < n; i++) {
            out[(done + i) * 2] = (int16_t)(uint16_t)((acc_left[i] * p->mixgain) >> 8);
            out[(done + i) * 2 + 1] = (int16_t)(uint16_t)((acc_right[i] * p->mixgain) >> 8);
        }
        done += n;
    }
}

/* ---------------------------------------------------------------------------------------
 * The player.
 * --------------------------------------------------------------------------------------- */

void ahx_player_init(ahx_player_t *p, const ahx_song_t *song, const ahx_waves_t *waves,
                     const ahx_pan_t *pan, uint32_t frequency, unsigned stereo)
{
    unsigned i;

    memset(p, 0, sizeof *p);
    p->song = song;
    p->waves = waves;
    p->pan = pan;

    if (stereo > 4) {
        stereo = 4;
    }
    p->stereo = (uint8_t)stereo;
    p->frequency = frequency;
    p->frame_samples = frequency / 50u;
    p->speed_multiplier = (uint8_t)(song->speed + 1);
    p->tick_samples = p->frame_samples / p->speed_multiplier;
    p->mixgain = (int32_t)ahx_defgain[p->stereo] * 256 / 100;
    p->defpan_left = ahx_stereopan_left[p->stereo];
    p->defpan_right = ahx_stereopan_right[p->stereo];

    /* The restart position is clamped here because the module's is not: the reference's
     * loader turns a restart past the end of the position list into the last position. */
    p->restart = (song->res < song->len) ? song->res : (uint16_t)(song->len - 1);

    for (i = 0; i < AHX_CHANNELS; i++) {
        ahx_voice_init(&p->voice[i]);
    }

    ahx_player_subsong(p, 0);
}

int ahx_player_subsong(ahx_player_t *p, unsigned index)
{
    uint16_t pos = 0;
    unsigned i;

    /* The reference's bound is one past the count, because it numbers the subsongs from one
     * and index 0 is the start of the song rather than the first subsong. Reproduced, quirks
     * and all: the oracle is the definition of right, and a player that offered the subsongs
     * should offer the same ones. */
    if (index > p->song->ss) {
        return 0;
    }
    if (index > 0) {
        pos = ahx_subsong(p->song, (uint8_t)(index - 1));
        if (pos >= p->song->len) {
            pos = 0;
        }
    }

    p->pos_nr = (int16_t)pos;
    p->pos_jump = 0;
    p->pattern_break = 0;
    p->note_nr = 0;
    p->pos_jump_note = 0;
    p->tempo = 6;
    p->step_wait_frames = 0;
    p->get_new_position = 1;
    p->song_end_reached = 0;
    p->playing_time = 0;

    /* A subsong starts at the top of a frame, so the block path's place inside the tick and
     * the frame goes back to the start with it. */
    p->tick_pos = 0;
    p->tick_nr = 0;
    p->tail_pos = 0;

    for (i = 0; i < AHX_CHANNELS; i++) {
        ahx_voice_t *v = &p->voice[i];

        ahx_voice_init(v);
        v->track_on = 1;
        ahx_voice_set_pan(v, p->pan, ahx_pan_side[i] ? p->defpan_right : p->defpan_left);
    }

    return 1;
}

uint32_t ahx_player_frame_samples(const ahx_player_t *p)
{
    return p->frame_samples;
}

void ahx_player_frame(ahx_player_t *p, int16_t *out)
{
    /* One frame is what the block path calls a whole frame's worth of samples, so this is not
     * a second implementation that could drift from it. */
    ahx_player_block(p, out, p->frame_samples);
}

void ahx_player_block(ahx_player_t *p, int16_t *out, uint32_t samples)
{
    uint32_t tail = p->frame_samples - p->speed_multiplier * p->tick_samples;

    while (samples > 0) {
        uint32_t chunk;

        if (p->tick_nr == p->speed_multiplier) {
            /* The frame's tail: the tick length does not always divide the frame, and the
             * reference never mixes the remainder. What it leaves is zero, because the oracle
             * zeroes its frame before every call, so zero is what is written here. When the
             * tick length does divide the frame this is a no-op that only wraps the frame. */
            chunk = tail - p->tail_pos;
            if (chunk > samples) {
                chunk = samples;
            }
            memset(out, 0, (size_t)chunk * 2 * sizeof *out);
            p->tail_pos += chunk;
            if (p->tail_pos == tail) {
                p->tick_nr = 0;
                p->tail_pos = 0;
            }
        } else {
            /* A tick's irq runs once, at its first sample, however the calls are cut. */
            if (p->tick_pos == 0) {
                ahx_play_irq(p);
            }
            chunk = p->tick_samples - p->tick_pos;
            if (chunk > samples) {
                chunk = samples;
            }
            ahx_mix(p, out, chunk);
            p->tick_pos += chunk;
            if (p->tick_pos == p->tick_samples) {
                p->tick_pos = 0;
                p->tick_nr++;
            }
        }

        out += (size_t)chunk * 2;
        samples -= chunk;
    }
}

uint32_t ahx_player_render(ahx_player_t *p, int16_t *out, uint32_t frames)
{
    uint32_t done;

    for (done = 0; done < frames; done++) {
        if (p->song_end_reached) {
            break;
        }
        ahx_player_frame(p, out);
        out += (size_t)p->frame_samples * 2;
    }
    return done;
}
