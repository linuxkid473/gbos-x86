/*
 * Goodix 53x5 driver for libfprint - Milan anti-fake classifier
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/antifake/antifake.h"

#include <limits.h>
#include <stdint.h>

int
goodix_milan_antifake_model_score (
  const int32_t vector[51],
  int32_t      *score)
{
  static const int32_t coefficients[3][51] = {
    {
      -881719, -58411, -460347, -335962, -47999, 29399, -186137,
      218335, -202819, 442778, 62551, 167207, 168755, 154635,
      799758, 219867, -212815, 94435, 185941, -156653, -86768,
      251010, 5761, -30002, 328469, 214185, -910040, -274539,
      154810, 93271, 484464, -43376, -124520, 301790, 493756,
      193732, 33331, 123407, -96289, -162522, 101447, -16494,
      24321, -60577, 399584, -130566, -103732, -152795, -154894,
      -178784, 444620,
    },
    {
      -866330, -168716, -630017, -594975, -156495, 70847, -54945,
      266503, 178411, 875699, 307277, 176863, 332214, 369364,
      898523, 169352, 3588, 266863, 404322, -535741, 74253, 273206,
      53473, 234738, 208033, 169163, -474512, -232840, 128840,
      116924, 502816, 229632, 237877, 399671, 535274, 440234,
      42991, -117560, -118880, -28531, 120452, 19973, 294438,
      95383, 563415, -49589, 4058, -166184, -177503, -85203, 555291,
    },
    {
      -948340, -250213, -656204, -406007, -131997, -13883, -38493,
      250237, 122625, 662349, 89881, 83237, 161596, 395349, 747880,
      65116, -125278, 164230, 347575, -438777, 104650, 298676,
      107056, 212462, 116398, 65515, -501033, -197080, 130394, 99012,
      337049, 95841, 143597, 27112, 404840, 321412, 77898, -63774,
      -110036, -33350, 115883, 207773, 338726, 191328, 459497,
      -134510, -183482, -195391, -127574, 60763, 544563,
    },
  };
  static const int32_t biases[3] = {
    -66765, -5325, -51610,
  };

  if (!vector || !score)
    return -1;

  int32_t best = INT32_MAX;
  for (size_t model = 0; model < 3; model++)
    {
      uint32_t dot = 0;

      for (size_t i = 0; i < 51; i++)
        dot += (uint32_t) vector[i] * (uint32_t) coefficients[model][i];
      int32_t rounded = (int32_t) (dot + 0x800);
      rounded >>= 12;
      int32_t current = (int32_t) ((uint32_t) biases[model] -
                                   (uint32_t) rounded);

      if (current < best)
        best = current;
    }
  *score = best;
  return 0;
}

static int
antifake_ring_class (const uint16_t *residual,
                     const uint8_t  *mask,
                     size_t          rows,
                     size_t          columns,
                     size_t          row,
                     size_t          column,
                     size_t          radius,
                     size_t          border)
{
  static const int8_t ring_1[8][2] = {
    { 1, 0 }, { 1, 1 }, { 0, 1 }, {-1, 1 },
    {-1, 0 }, {-1,-1 }, { 0,-1 }, { 1,-1 },
  };
  static const int8_t ring_2[16][2] = {
    { 2, 0 }, { 2, 1 }, { 1, 1 }, { 1, 2 },
    { 0, 2 }, {-1, 2 }, {-1, 1 }, {-2, 1 },
    {-2, 0 }, {-2,-1 }, {-1,-1 }, {-1,-2 },
    { 0,-2 }, { 1,-2 }, { 1,-1 }, { 2,-1 },
  };
  static const int8_t ring_3[24][2] = {
    { 3, 0 }, { 3, 1 }, { 3, 2 }, { 2, 2 },
    { 2, 3 }, { 1, 3 }, { 0, 3 }, {-1, 3 },
    {-2, 3 }, {-2, 2 }, {-3, 2 }, {-3, 1 },
    {-3, 0 }, {-3,-1 }, {-3,-2 }, {-2,-2 },
    {-2,-3 }, {-1,-3 }, { 0,-3 }, { 1,-3 },
    { 2,-3 }, { 2,-2 }, { 3,-2 }, { 3,-1 },
  };
  uint8_t bits[24];
  const int8_t (*ring)[2] = radius == 1 ? ring_1
                              : radius == 2 ? ring_2 : ring_3;
  size_t bit_count = radius * 8;

  if (row < radius + border || row + radius >= rows - border ||
      column < radius + border || column + radius >= columns - border)
    return -1;

  uint16_t center = residual[row * columns + column];
  for (size_t i = 0; i < bit_count; i++)
    bits[i] = center <= residual[(size_t) ((ptrdiff_t) row + ring[i][1]) *
                                 columns +
                                 (size_t) ((ptrdiff_t) column + ring[i][0])];

  size_t ones = 0;
  size_t transitions = 0;
  for (size_t i = 0; i < bit_count; i++)
    {
      size_t neighbor =
        (size_t) ((ptrdiff_t) row + ring[i][1]) * columns +
        (size_t) ((ptrdiff_t) column + ring[i][0]);
      if (mask[neighbor] == 0)
        return -1;
      ones += bits[i];
      if (i != 0 && bits[i] != bits[i - 1])
        transitions++;
    }
  transitions += bits[0] != bits[bit_count - 1];
  return transitions > 2 ? (int) bit_count + 1 : (int) ones;
}

int
goodix_milan_antifake_model_vector (
  const uint16_t *residual,
  const uint8_t  *mask,
  size_t          rows,
  size_t          columns,
  size_t          border,
  int32_t         vector[51])
{
  uint32_t counts[3][26] = { { 0 } };
  const size_t bins[3] = { 10, 18, 26 };
  size_t output = 0;

  if (!residual || !mask || !vector || rows <= border * 2 ||
      columns <= border * 2 || columns > SIZE_MAX / rows)
    return -1;

  for (size_t row = border; row < rows - border; row++)
    for (size_t column = border; column < columns - border; column++)
      for (size_t radius = 1; radius <= 3; radius++)
        {
          int classification = antifake_ring_class (
            residual, mask, rows, columns, row, column, radius, border);

          if (classification >= 0)
            counts[radius - 1][classification]++;
        }

  for (size_t radius = 0; radius < 3; radius++)
    {
      uint32_t total = 0;

      for (size_t i = 0; i < bins[radius]; i++)
        total += counts[radius][i];
      for (size_t i = 0; i + 1 < bins[radius]; i++)
        vector[output++] = total == 0
                             ? (int32_t) (counts[radius][i] << 12)
                             : (int32_t) ((counts[radius][i] * 0x1000 +
                                           total / 2) /
                                          total);
    }
  return 0;
}
