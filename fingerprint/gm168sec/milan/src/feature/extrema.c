/*
 * Goodix 53x5 driver for libfprint - Milan feature extrema refinement
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/feature/feature.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Profile-9 extrema discovery and refinement recovered from FUN_180047c50,
 * FUN_180048c60, FUN_18004bed0, FUN_18004c5a0, and FUN_18004c100. */
enum {
  AXIS_X,
  AXIS_Y,
  AXIS_SCALE,
  AXIS_COUNT,
};

enum {
  HESSIAN_XX,
  HESSIAN_YY,
  HESSIAN_SCALE_SCALE,
  HESSIAN_XY,
  HESSIAN_Y_SCALE,
  HESSIAN_X_SCALE,
  HESSIAN_COUNT,
};

#define EXTREMA_SCALE_LIMIT 4
#define EXTREMA_RESPONSE_THRESHOLD 0x148
#define DERIVATIVE_LIMIT 0x8000
#define MIXED_DERIVATIVE_LIMIT 0x2000
#define REFINEMENT_ATTEMPT_LIMIT 5
#define REFINEMENT_Q12_SHIFT 12
#define REFINEMENT_Q12_ONE (1 << REFINEMENT_Q12_SHIFT)
#define REFINEMENT_Q12_HALF (REFINEMENT_Q12_ONE >> 1)
#define REFINEMENT_Q12_TO_Q8_SHIFT 4
#define REFINEMENT_OFFSET_SATURATION 0x00ffffff
#define REFINEMENT_CONTRAST_SCALE 0x4000
#define REFINEMENT_CONTRAST_MINIMUM 0x1478000
#define REFINEMENT_EDGE_RATIO 40
#define REFINEMENT_EDGE_RATIO_PLUS_ONE 41
#define REFINEMENT_CURVATURE_SCALE 0x400
#define REFINEMENT_SCALE_EXPONENT_FACTOR 0x10
#define REFINEMENT_SCALE_EXPONENT_DIVISOR 3
#define REFINEMENT_SCALE_FACTOR 0x13333
#define REFINEMENT_Q16_SHIFT 16

int
feature_next_extremum (
  const uint16_t             *scales,
  size_t                      rows,
  size_t                      columns,
  size_t                      cursor[3],
  GoodixMilanFeatureExtremum *result)
{
  size_t count;

  count = rows * columns;
  for (size_t scale = cursor[0];
       scale < EXTREMA_SCALE_LIMIT;
       scale++, cursor[1] = EXTREMA_SCAN_BORDER)
    for (size_t row = cursor[1];
         row + EXTREMA_SCAN_BORDER < rows;
         row++, cursor[2] = EXTREMA_SCAN_BORDER)
      for (size_t column = cursor[2];
           column + EXTREMA_SCAN_BORDER < columns;
           column++)
        {
          size_t pixel = row * columns + column;
          int32_t response = (int16_t) (
            scales[(scale + 1) * count + pixel] -
            scales[scale * count + pixel]);
          int32_t absolute = response < 0 ? -response : response;
          int extremum = absolute > EXTREMA_RESPONSE_THRESHOLD;

          for (ptrdiff_t adjacent = -1; extremum && adjacent <= 1; adjacent++)
            for (ptrdiff_t delta_row = -1;
                 extremum && delta_row <= 1;
                 delta_row++)
              for (ptrdiff_t delta_column = -1; delta_column <= 1;
                   delta_column++)
                {
                  if (adjacent == 0 && delta_row == 0 && delta_column == 0)
                    continue;
                  size_t neighbor =
                    (size_t) ((ptrdiff_t) row + delta_row) * columns +
                    (size_t) ((ptrdiff_t) column + delta_column);
                  size_t neighbor_scale =
                    (size_t) ((ptrdiff_t) scale + adjacent);
                  int32_t neighbor_response =
                    (int32_t) scales[(neighbor_scale + 1) * count + neighbor] -
                    (int32_t) scales[neighbor_scale * count + neighbor];

                  if ((response > 0 && neighbor_response > response) ||
                      (response < 0 && neighbor_response < response))
                    extremum = 0;
                }
          if (!extremum)
            continue;
          *result = (GoodixMilanFeatureExtremum){
            (int32_t) column, (int32_t) row, (int32_t) scale, response,
          };
          cursor[0] = scale;
          cursor[1] = row;
          cursor[2] = column + 1;
          return 1;
        }
  cursor[0] = EXTREMA_SCALE_LIMIT;
  return 0;
}

size_t
goodix_milan_feature_collect_extrema (
  const uint16_t             *scales,
  size_t                      rows,
  size_t                      columns,
  GoodixMilanFeatureExtremum *extrema,
  size_t                      capacity)
{
  size_t cursor[3] = FEATURE_EXTREMA_CURSOR_INIT;
  GoodixMilanFeatureExtremum extremum;
  size_t result_count = 0;

  while (feature_next_extremum (scales, rows, columns, cursor, &extremum))
    {
      if (result_count < capacity && extrema)
        extrema[result_count] = extremum;
      result_count++;
    }
  return result_count;
}

static int
feature_build_refinement_derivatives (const uint16_t *scales,
                                      size_t          rows,
                                      size_t          columns,
                                      int32_t         x,
                                      int32_t         y,
                                      int32_t         scale,
                                      int16_t         hessian[HESSIAN_COUNT],
                                      int16_t         gradient[AXIS_COUNT],
                                      int16_t        *center)
{
  size_t count = rows * columns;
  size_t pixel = (size_t) y * columns + (size_t) x;
  const uint16_t *previous = scales + (size_t) (scale - 1) * count;
  const uint16_t *current = scales + (size_t) scale * count;
  const uint16_t *next = scales + (size_t) (scale + 1) * count;
  const uint16_t *next2 = scales + (size_t) (scale + 2) * count;
  int32_t value;

  *center = (int16_t) (2 * ((int32_t) next[pixel] - current[pixel]));
  value = 2 * (((int32_t) current[pixel - 1] - next[pixel - 1]) -
               current[pixel + 1] + next[pixel + 1]);
  if (value <= -DERIVATIVE_LIMIT || value >= DERIVATIVE_LIMIT)
    return 0;
  gradient[AXIS_X] = (int16_t) value;
  value = 2 * (((int32_t) current[pixel - columns] -
                next[pixel - columns]) -
               current[pixel + columns] + next[pixel + columns]);
  if (value <= -DERIVATIVE_LIMIT || value >= DERIVATIVE_LIMIT)
    return 0;
  gradient[AXIS_Y] = (int16_t) value;
  value = 2 * (((int32_t) previous[pixel] - current[pixel]) - next[pixel] +
               next2[pixel]);
  if (value <= -DERIVATIVE_LIMIT || value >= DERIVATIVE_LIMIT)
    return 0;
  gradient[AXIS_SCALE] = (int16_t) value;

  value = 4 * (((int32_t) next[pixel - 1] - current[pixel - 1]) -
               current[pixel + 1] - *center + next[pixel + 1]);
  if (value <= -DERIVATIVE_LIMIT || value >= DERIVATIVE_LIMIT)
    return 0;
  hessian[HESSIAN_XX] = (int16_t) value;
  value = 4 * (((int32_t) next[pixel - columns] -
                current[pixel - columns]) -
               *center - current[pixel + columns] + next[pixel + columns]);
  if (value <= -DERIVATIVE_LIMIT || value >= DERIVATIVE_LIMIT)
    return 0;
  hessian[HESSIAN_YY] = (int16_t) value;
  value = 4 * (((int32_t) current[pixel] - previous[pixel]) - next[pixel] -
               *center + next2[pixel]);
  if (value <= -DERIVATIVE_LIMIT || value >= DERIVATIVE_LIMIT)
    return 0;
  hessian[HESSIAN_SCALE_SCALE] = (int16_t) value;

  value = ((int32_t) current[pixel + columns - 1] -
           next[pixel + columns - 1]) -
          next[pixel - columns + 1] - current[pixel + columns + 1] -
          current[pixel - columns - 1] + current[pixel - columns + 1] +
          next[pixel + columns + 1] + next[pixel - columns - 1];
  if (value <= -MIXED_DERIVATIVE_LIMIT || value >= MIXED_DERIVATIVE_LIMIT)
    return 0;
  hessian[HESSIAN_XY] = (int16_t) value;
  value = ((int32_t) previous[pixel + columns] -
           previous[pixel - columns]) -
          next2[pixel - columns] + next2[pixel + columns] -
          next[pixel + columns] - current[pixel + columns] +
          current[pixel - columns] + next[pixel - columns];
  if (value <= -MIXED_DERIVATIVE_LIMIT || value >= MIXED_DERIVATIVE_LIMIT)
    return 0;
  hessian[HESSIAN_Y_SCALE] = (int16_t) value;
  value = ((int32_t) previous[pixel + 1] - previous[pixel - 1]) -
          next2[pixel - 1] + next2[pixel + 1] - current[pixel + 1] -
          next[pixel + 1] + next[pixel - 1] + current[pixel - 1];
  if (value <= -MIXED_DERIVATIVE_LIMIT || value >= MIXED_DERIVATIVE_LIMIT)
    return 0;
  hessian[HESSIAN_X_SCALE] = (int16_t) value;
  return 1;
}

static int
feature_signed_bit_length (int64_t value)
{
  uint64_t magnitude = value < 0 ? (uint64_t) -value : (uint64_t) value;
  int length = 1;

  while (magnitude != 0)
    {
      length++;
      magnitude >>= 1;
    }
  return length;
}

static void
feature_solve_refinement_offset (const int16_t hessian[HESSIAN_COUNT],
                                 const int16_t gradient[AXIS_COUNT],
                                 int32_t       offset[AXIS_COUNT])
{
  int64_t h00 = hessian[HESSIAN_XX];
  int64_t h11 = hessian[HESSIAN_YY];
  int64_t h22 = hessian[HESSIAN_SCALE_SCALE];
  int64_t h01 = hessian[HESSIAN_XY];
  int64_t h12 = hessian[HESSIAN_Y_SCALE];
  int64_t h02 = hessian[HESSIAN_X_SCALE];
  int64_t cofactor0 = h11 * h22 - h12 * h12;
  int64_t cofactor1 = h02 * h12 - h01 * h22;
  int64_t cofactor2 = h01 * h12 - h02 * h11;
  int64_t determinant = h00 * cofactor0 + h01 * cofactor1 +
                        h02 * cofactor2;
  int64_t numerator[AXIS_COUNT];
  int determinant_bits = feature_signed_bit_length (determinant);

  if (determinant == 0)
    {
      memset (offset, 0, AXIS_COUNT * sizeof (*offset));
      return;
    }
  numerator[AXIS_X] = -((int64_t) gradient[AXIS_X] * cofactor0 +
                        (int64_t) gradient[AXIS_Y] * cofactor1 +
                        (int64_t) gradient[AXIS_SCALE] * cofactor2);
  numerator[AXIS_Y] =
    -((h00 * h22 - h02 * h02) * gradient[AXIS_Y] +
      (h01 * h02 - h00 * h12) * gradient[AXIS_SCALE] +
      (int64_t) gradient[AXIS_X] * cofactor1);
  numerator[AXIS_SCALE] =
    -((h00 * h11 - h01 * h01) * gradient[AXIS_SCALE] +
      (h01 * h02 - h00 * h12) * gradient[AXIS_Y] +
      (int64_t) gradient[AXIS_X] * cofactor2);

  for (size_t axis = 0; axis < AXIS_COUNT; axis++)
    {
      int numerator_bits = feature_signed_bit_length (numerator[axis]);
      int64_t scaled_numerator = numerator[axis];
      int64_t scaled_determinant = determinant;

      if (numerator_bits - determinant_bits >= 9)
        {
          offset[axis] = REFINEMENT_OFFSET_SATURATION;
          continue;
        }
      int bits = numerator_bits > determinant_bits ? numerator_bits :
                 determinant_bits;
      if (bits > 32)
        {
          scaled_numerator >>= bits - 32;
          scaled_determinant >>= bits - 32;
        }
      offset[axis] = (int32_t) ((scaled_numerator * REFINEMENT_Q12_ONE) /
                                scaled_determinant);
    }
}

static int32_t
feature_round_q12_move (int32_t value)
{
  if (value < 0)
    {
      uint32_t magnitude = (uint32_t) -value;
      return -(int32_t) ((magnitude >> REFINEMENT_Q12_SHIFT) +
                         ((magnitude & REFINEMENT_Q12_HALF) != 0));
    }
  return (value >> REFINEMENT_Q12_SHIFT) +
         (((uint32_t) value & REFINEMENT_Q12_HALF) != 0);
}

static uint32_t
feature_exp2_q16 (int32_t value)
{
  static const uint32_t thresholds[16] = {
    0x95c0, 0x526a, 0x2b80, 0x1664, 0x0b5d, 0x05ba, 0x02e0, 0x0171,
    0x00b8, 0x005c, 0x002e, 0x0017, 0x000c, 0x0006, 0x0003, 0x0001,
  };
  uint32_t magnitude = value < 0 ? (uint32_t) -value : (uint32_t) value;
  uint32_t fraction = magnitude & ((UINT32_C (1) << REFINEMENT_Q16_SHIFT) - 1);
  uint64_t result = UINT64_C (1) << REFINEMENT_Q16_SHIFT;

  if ((magnitude >> REFINEMENT_Q16_SHIFT) > 0)
    result <<= magnitude >> REFINEMENT_Q16_SHIFT;
  for (size_t bit = 0; bit < 16; bit++)
    if (fraction >= thresholds[bit])
      {
        fraction -= thresholds[bit];
        result += result >> (bit + 1);
      }
  return value > 0 ? (uint32_t) result :
         (uint32_t) ((UINT64_C (1) << 32) / result);
}

int
goodix_milan_feature_refine_extremum (
  const uint16_t              *scales,
  size_t                       rows,
  size_t                       columns,
  GoodixMilanFeatureCandidate *candidate,
  uint32_t                    *curvature)
{
  int16_t hessian[HESSIAN_COUNT];
  int16_t gradient[AXIS_COUNT];
  int16_t center;
  int32_t offset[AXIS_COUNT];
  int32_t x, y, scale;

  x = candidate->x;
  y = candidate->y;
  scale = candidate->scale;
  for (size_t iteration = 0; iteration < REFINEMENT_ATTEMPT_LIMIT; iteration++)
    {
      if (!feature_build_refinement_derivatives (
            scales, rows, columns, x, y, scale, hessian, gradient, &center))
        return 0;
      feature_solve_refinement_offset (hessian, gradient, offset);
      if (abs (offset[AXIS_X]) < REFINEMENT_Q12_HALF &&
          abs (offset[AXIS_Y]) < REFINEMENT_Q12_HALF &&
          abs (offset[AXIS_SCALE]) < REFINEMENT_Q12_HALF)
        {
          int64_t contrast = (int64_t) center * REFINEMENT_CONTRAST_SCALE +
                             (int64_t) gradient[AXIS_X] * offset[AXIS_X] +
                             (int64_t) gradient[AXIS_Y] * offset[AXIS_Y] +
                             (int64_t) gradient[AXIS_SCALE] * offset[AXIS_SCALE];
          if (contrast <= 0)
            contrast = -contrast;
          if (contrast < REFINEMENT_CONTRAST_MINIMUM)
            return 0;
          candidate->strength =
            (int32_t) (contrast >> REFINEMENT_Q12_SHIFT);
          int32_t trace = (int32_t) hessian[HESSIAN_XX] + hessian[HESSIAN_YY];
          int32_t determinant =
            (int32_t) hessian[HESSIAN_XX] * hessian[HESSIAN_YY] -
            (int32_t) hessian[HESSIAN_XY] * hessian[HESSIAN_XY];
          if (determinant <= 0 ||
              (int64_t) determinant * REFINEMENT_EDGE_RATIO_PLUS_ONE *
              REFINEMENT_EDGE_RATIO_PLUS_ONE <=
              (int64_t) REFINEMENT_EDGE_RATIO * trace * trace)
            return 0;
          candidate->refined_x =
            (int16_t) ((x * REFINEMENT_Q12_ONE + offset[AXIS_X]) >>
                       REFINEMENT_Q12_TO_Q8_SHIFT);
          candidate->refined_y =
            (int16_t) ((y * REFINEMENT_Q12_ONE + offset[AXIS_Y]) >>
                       REFINEMENT_Q12_TO_Q8_SHIFT);
          *curvature =
            (uint32_t) (((int64_t) trace * trace *
                         REFINEMENT_CURVATURE_SCALE) / determinant);
          int32_t exponent =
            ((scale * REFINEMENT_Q12_ONE + offset[AXIS_SCALE]) *
             REFINEMENT_SCALE_EXPONENT_FACTOR) /
            REFINEMENT_SCALE_EXPONENT_DIVISOR;
          candidate->scale_value =
            (int32_t) (((uint64_t) feature_exp2_q16 (exponent) *
                        REFINEMENT_SCALE_FACTOR) >> REFINEMENT_Q16_SHIFT);
          return 1;
        }

      x += feature_round_q12_move (offset[AXIS_X]);
      y += feature_round_q12_move (offset[AXIS_Y]);
      scale += feature_round_q12_move (offset[AXIS_SCALE]);
      candidate->x = x;
      candidate->y = y;
      candidate->scale = scale;
      if (scale < EXTREMA_FIRST_SCALE || scale >= EXTREMA_SCALE_LIMIT || x < 1 ||
          x >= (int32_t) columns - 1 || y < 1 || y >= (int32_t) rows - 1)
        return 0;
    }
  return 0;
}
