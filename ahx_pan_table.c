/* SPDX-License-Identifier: MIT AND BSD-3-Clause */
/* AHX-1: the panning curve, generated. Do not edit: make pan-table rewrites it from
 * ahx-core/ahx_tables.c's ahx_pan_generate(), which is the reference's own, and
 * tests/pan_check.sh fails when this file and that generator disagree.
 *
 * The device cannot compute it: the curve is a sine, and the FM-1 has no FPU and no
 * <math.h>. It is 1,024 bytes of const, so it is in XIP with the rest of the read-only
 * data and costs the firmware no RAM. left[i] falls from 254 at position 0 to 0 at
 * 255 and right[i] rises from 0 to 254 the other way, a quarter turn apart. The 254
 * and not 255 is the generator's own: sin(pi / 2) * 255 in float is a hair below 255
 * and the cast truncates. It is what every host render has been mixed with, so the
 * device carries it too and not the number it was meant to be. left[255] and
 * right[0] are pinned to 0 by the generator, so a voice at one end is silent on that
 * side and not a rounding away from it.
 *
 * A voice picks two entries: the module player's stereo setting gives each of its four
 * channels one pan position (ahx-core/ahx_player.c, the reference's ahx_stereopan_left /
 * _right), and stereo 2 - the reference's own default, and what the module player uses
 * - reaches only left[64] and right[193]. The whole curve is here because the player
 * takes an ahx_pan_t and a pan position is any of the 256. */
#include "ahx_voice.h"

const ahx_pan_t ahx_pan_table = {
    {
         254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254,
         254, 254, 254, 253, 253, 253, 253, 253, 253, 252, 252, 252,
         252, 252, 251, 251, 251, 250, 250, 250, 250, 249, 249, 249,
         248, 248, 248, 247, 247, 246, 246, 246, 245, 245, 244, 244,
         244, 243, 243, 242, 242, 241, 241, 240, 240, 239, 239, 238,
         237, 237, 236, 236, 235, 234, 234, 233, 233, 232, 231, 231,
         230, 229, 229, 228, 227, 227, 226, 225, 224, 224, 223, 222,
         221, 221, 220, 219, 218, 217, 217, 216, 215, 214, 213, 212,
         212, 211, 210, 209, 208, 207, 206, 205, 204, 203, 202, 201,
         201, 200, 199, 198, 197, 196, 195, 194, 193, 192, 191, 189,
         188, 187, 186, 185, 184, 183, 182, 181, 180, 179, 178, 176,
         175, 174, 173, 172, 171, 170, 168, 167, 166, 165, 164, 162,
         161, 160, 159, 158, 156, 155, 154, 153, 151, 150, 149, 148,
         146, 145, 144, 142, 141, 140, 139, 137, 136, 135, 133, 132,
         131, 129, 128, 127, 125, 124, 122, 121, 120, 118, 117, 116,
         114, 113, 111, 110, 109, 107, 106, 104, 103, 101, 100,  99,
          97,  96,  94,  93,  91,  90,  88,  87,  85,  84,  82,  81,
          79,  78,  77,  75,  74,  72,  71,  69,  68,  66,  64,  63,
          61,  60,  58,  57,  55,  54,  52,  51,  49,  48,  46,  45,
          43,  42,  40,  38,  37,  35,  34,  32,  31,  29,  28,  26,
          24,  23,  21,  20,  18,  17,  15,  14,  12,  10,   9,   7,
           6,   4,   3,   0,
    },
    {
           0,   1,   3,   4,   6,   7,   9,  10,  12,  14,  15,  17,
          18,  20,  21,  23,  24,  26,  28,  29,  31,  32,  34,  35,
          37,  38,  40,  42,  43,  45,  46,  48,  49,  51,  52,  54,
          55,  57,  58,  60,  61,  63,  64,  66,  68,  69,  71,  72,
          74,  75,  77,  78,  79,  81,  82,  84,  85,  87,  88,  90,
          91,  93,  94,  96,  97,  99, 100, 101, 103, 104, 106, 107,
         109, 110, 111, 113, 114, 116, 117, 118, 120, 121, 122, 124,
         125, 127, 128, 129, 131, 132, 133, 135, 136, 137, 139, 140,
         141, 142, 144, 145, 146, 148, 149, 150, 151, 153, 154, 155,
         156, 158, 159, 160, 161, 162, 164, 165, 166, 167, 168, 170,
         171, 172, 173, 174, 175, 176, 178, 179, 180, 181, 182, 183,
         184, 185, 186, 187, 188, 189, 191, 192, 193, 194, 195, 196,
         197, 198, 199, 200, 201, 201, 202, 203, 204, 205, 206, 207,
         208, 209, 210, 211, 212, 212, 213, 214, 215, 216, 217, 217,
         218, 219, 220, 221, 221, 222, 223, 224, 224, 225, 226, 227,
         227, 228, 229, 229, 230, 231, 231, 232, 233, 233, 234, 234,
         235, 236, 236, 237, 237, 238, 239, 239, 240, 240, 241, 241,
         242, 242, 243, 243, 244, 244, 244, 245, 245, 246, 246, 246,
         247, 247, 248, 248, 248, 249, 249, 249, 250, 250, 250, 250,
         251, 251, 251, 252, 252, 252, 252, 252, 253, 253, 253, 253,
         253, 253, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254,
         254, 254, 254, 254,
    },
};
