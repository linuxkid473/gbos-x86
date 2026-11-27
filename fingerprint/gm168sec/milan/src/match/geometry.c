/*
 * Goodix 53x5 driver for libfprint - Milan match geometry
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/match/geometry.h"
#include "milan/match/correspondence.h"
#include "milan/preprocess/state.h"
#include "milan/private.h"
#include "milan/relations.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum
{
  MILAN_AFFINE_Q8_ONE = 0x100,
  MILAN_AFFINE_Q8_HALF = 0x80,
  MILAN_AFFINE_Q8_SHIFT = 8,
  MILAN_AFFINE_SOLVE_SHIFT = 10,
  MILAN_AFFINE_SOLVE_SCALE = 1024,
  MILAN_ORIENTATION_HALF_PERIOD = 0x1922,
  MILAN_ORIENTATION_PERIOD = 0x3244,
  MILAN_ORIENTATION_FULL_PERIOD = 0x6488,
  MILAN_TRIANGLE_ORIENTATION_TOLERANCE = 0x400,
  MILAN_TRIANGLE_AFFINE_SYMMETRY_TOLERANCE = 0x32,
  MILAN_TRIANGLE_AFFINE_COEFFICIENT_LIMIT = 300,
  MILAN_TRIANGLE_MINIMUM_EDGE_SQUARED = 0x2ffff,
  MILAN_RECOGNITION_TRIANGLE_LIMIT = 0x3b2,
  MILAN_RECOGNITION_NO_MODEL_RESIDUAL = 0x190000,
  MILAN_RECOGNITION_INLIER_AXIS_LIMIT = 0x281,
  MILAN_RECOGNITION_INLIER_SQUARED_LIMIT = 0x64000,
  MILAN_RECOGNITION_ORIENTATION_LIMIT = 0x506,
  MILAN_RECOGNITION_EARLY_INLIER_COUNT = 20,
  MILAN_RECOGNITION_REFINE_INLIER_COUNT = 3,
  MILAN_RECOGNITION_REFINE_RESIDUAL = 0x4000,
};

static void
milan_affine_from_three_points (const int32_t source[6],
                                const int32_t target[6],
                                int32_t       affine[6])
{
  int64_t determinant;
  int64_t raw_a;
  int64_t raw_b;
  int64_t raw_c;
  int64_t raw_d;

  determinant =
    (int64_t) (source[0] - source[2]) * (source[3] - source[5]) -
    (int64_t) (source[2] - source[4]) * (source[1] - source[3]);
  if (determinant == 0)
    {
      raw_a = INT32_MAX;
      raw_b = INT32_MAX;
    }
  else
    {
      raw_a =
        ((int64_t) (target[0] - target[2]) * MILAN_AFFINE_SOLVE_SCALE *
           (source[3] - source[5]) -
         (int64_t) (target[2] - target[4]) * MILAN_AFFINE_SOLVE_SCALE *
           (source[1] - source[3])) /
        determinant;
      raw_b =
        ((int64_t) (target[0] - target[2]) * MILAN_AFFINE_SOLVE_SCALE *
           (source[2] - source[4]) -
         (int64_t) (target[2] - target[4]) * MILAN_AFFINE_SOLVE_SCALE *
           (source[0] - source[2])) /
        -determinant;
    }
  raw_a = (int32_t) raw_a;
  raw_b = (int32_t) raw_b;
  affine[0] = (int32_t) raw_a >> 2;
  affine[1] = (int32_t) raw_b >> 2;
  affine[2] = (int32_t) ((((int64_t) target[0] << MILAN_AFFINE_SOLVE_SHIFT) -
                           raw_b * source[1] - raw_a * source[0]) >>
                          MILAN_AFFINE_SOLVE_SHIFT);

  determinant =
    (int64_t) (source[2] - source[0]) * (source[5] - source[1]) -
    (int64_t) (source[4] - source[0]) * (source[3] - source[1]);
  if (determinant == 0)
    {
      raw_c = INT32_MAX;
      raw_d = INT32_MAX;
    }
  else
    {
      raw_c =
        ((int64_t) (target[3] - target[1]) * MILAN_AFFINE_SOLVE_SCALE *
           (source[5] - source[1]) -
         (int64_t) (target[5] - target[1]) * MILAN_AFFINE_SOLVE_SCALE *
           (source[3] - source[1])) /
        determinant;
      raw_d =
        ((int64_t) (target[3] - target[1]) * MILAN_AFFINE_SOLVE_SCALE *
           (source[4] - source[0]) -
         (int64_t) (target[5] - target[1]) * MILAN_AFFINE_SOLVE_SCALE *
           (source[2] - source[0])) /
        -determinant;
    }
  raw_c = (int32_t) raw_c;
  raw_d = (int32_t) raw_d;
  affine[3] = (int32_t) raw_c >> 2;
  affine[4] = (int32_t) raw_d >> 2;
  affine[5] = (int32_t) ((((int64_t) target[5] << MILAN_AFFINE_SOLVE_SHIFT) -
                           raw_d * source[5] - raw_c * source[4]) >>
                          MILAN_AFFINE_SOLVE_SHIFT);
}

static int
milan_affine_is_valid (const int32_t affine[6])
{
  int64_t a = affine[0];
  int64_t b = affine[1];
  int64_t c = affine[3];
  int64_t d = affine[4];
  /* Native sign-extends each wrapped dword product before the qword sum. */
  int64_t first = (int64_t) (int32_t) (a * a) + (int32_t) (b * b);
  int64_t cross = (int64_t) (int32_t) (a * c) + (int32_t) (b * d);
  int64_t second = (int64_t) (int32_t) (c * c) + (int32_t) (d * d);
  int64_t trace = first + second;
  int64_t discriminant = (int64_t) (
    (uint64_t) trace * (uint64_t) trace +
    UINT64_C (4) * ((uint64_t) cross * (uint64_t) cross -
                    (uint64_t) first * (uint64_t) second));
  int64_t lower = ((int64_t) 163 << 9) - trace;
  int64_t upper = ((int64_t) 401 << 9) - trace;

  return discriminant >= 0 && trace >= 0 && lower < 1 && upper >= 0 &&
         lower * lower != discriminant && discriminant <= lower * lower &&
         upper * upper != discriminant && discriminant <= upper * upper;
}

static int32_t
milan_reinterpret_uint32_as_int32 (uint32_t value)
{
  int32_t result;

  memcpy (&result, &value, sizeof(result));
  return result;
}

static int32_t
milan_triangle_shift_right_two (uint32_t value)
{
  uint32_t shifted = value >> 2;

  if (value & UINT32_C (0x80000000))
    shifted |= UINT32_C (0xc0000000);
  return milan_reinterpret_uint32_as_int32 (shifted);
}

static int32_t
milan_triangle_distance_shift_then_add (int32_t first_x,
                                         int32_t first_y,
                                         int32_t second_x,
                                         int32_t second_y)
{
  uint32_t dx = (uint32_t) first_x - (uint32_t) second_x;
  uint32_t dy = (uint32_t) first_y - (uint32_t) second_y;
  uint32_t distance =
    (uint32_t) milan_triangle_shift_right_two (dx * dx) +
    (uint32_t) milan_triangle_shift_right_two (dy * dy);

  return milan_reinterpret_uint32_as_int32 (distance);
}

static int32_t
milan_triangle_distance_add_then_shift (int32_t first_x,
                                         int32_t first_y,
                                         int32_t second_x,
                                         int32_t second_y)
{
  uint32_t dx = (uint32_t) first_x - (uint32_t) second_x;
  uint32_t dy = (uint32_t) first_y - (uint32_t) second_y;

  return milan_triangle_shift_right_two (dx * dx + dy * dy);
}

static int
milan_triangle_edge_is_consistent (int32_t source_distance,
                                   int32_t target_distance)
{
  return milan_reinterpret_uint32_as_int32 (
           (uint32_t) source_distance * UINT32_C (5)) <=
           milan_reinterpret_uint32_as_int32 (
             (uint32_t) target_distance * UINT32_C (6)) &&
         milan_reinterpret_uint32_as_int32 (
           (uint32_t) target_distance * UINT32_C (5)) <=
           milan_reinterpret_uint32_as_int32 (
             (uint32_t) source_distance * UINT32_C (6)) &&
          source_distance > MILAN_TRIANGLE_MINIMUM_EDGE_SQUARED &&
          target_distance > MILAN_TRIANGLE_MINIMUM_EDGE_SQUARED;
}

static int32_t
milan_normalize_orientation_difference (int32_t difference,
                                        int32_t half_period,
                                        int32_t period)
{
  if (difference > half_period)
    difference -= period;
  if (difference < -half_period)
    difference += period;
  return difference;
}

static int
milan_triangle_orientations_are_consistent (int32_t differences[3])
{
  const int32_t half_period = MILAN_ORIENTATION_HALF_PERIOD;
  const int32_t period = MILAN_ORIENTATION_PERIOD;
  const int32_t tolerance = MILAN_TRIANGLE_ORIENTATION_TOLERANCE;

  for (size_t retry = 0; retry < 2; retry++)
    {
      int32_t average =
        (differences[0] + differences[1] + differences[2]) / 3;
      int consistent = 1;

      for (size_t i = 0; i < 3; i++)
        consistent &= differences[i] - average <= tolerance &&
                      differences[i] - average >= -tolerance;
      if (consistent)
        return 1;
      for (size_t i = 0; i < 3; i++)
        differences[i] = milan_normalize_orientation_difference (
          differences[i] + half_period, half_period, period);
    }
  return 0;
}

static int
milan_triangle_affine_is_consistent (const int32_t affine[6])
{
  return abs (affine[0] - affine[4]) <
           MILAN_TRIANGLE_AFFINE_SYMMETRY_TOLERANCE &&
         abs (affine[3] + affine[1]) <
           MILAN_TRIANGLE_AFFINE_SYMMETRY_TOLERANCE &&
         abs (affine[0]) < MILAN_TRIANGLE_AFFINE_COEFFICIENT_LIMIT &&
         abs (affine[3]) < MILAN_TRIANGLE_AFFINE_COEFFICIENT_LIMIT &&
         abs (affine[4]) < MILAN_TRIANGLE_AFFINE_COEFFICIENT_LIMIT &&
         abs (affine[1]) < MILAN_TRIANGLE_AFFINE_COEFFICIENT_LIMIT;
}

static int32_t
milan_divide_affine_coefficient (int64_t numerator,
                                 int64_t denominator,
                                 int64_t half_denominator)
{
  uint64_t dividend_bits;
  int64_t dividend;
  int64_t quotient;

  if (denominator == 0)
    return 0;
  /* Native rounds with wrapping qword arithmetic before signed division. The
   * rounded dividend can be negative even with a positive Gram determinant. */
  dividend_bits = numerator < 0 ?
                  (uint64_t) half_denominator - (uint64_t) numerator :
                  (uint64_t) numerator + (uint64_t) half_denominator;
  memcpy (&dividend, &dividend_bits, sizeof (dividend));
  quotient = dividend / denominator;
  return milan_reinterpret_uint32_as_int32 (
    numerator < 0 ? UINT32_C (0) - (uint32_t) quotient :
    (uint32_t) quotient);
}

static int64_t
milan_wrapped_multiply (int64_t first, int64_t second)
{
  uint64_t result = (uint64_t) first * (uint64_t) second;
  int64_t signed_result;

  memcpy (&signed_result, &result, sizeof(signed_result));
  return signed_result;
}

static int64_t
milan_wrapped_add (int64_t first, int64_t second)
{
  uint64_t result = (uint64_t) first + (uint64_t) second;
  int64_t signed_result;

  memcpy (&signed_result, &result, sizeof(signed_result));
  return signed_result;
}

static int64_t
milan_wrapped_subtract (int64_t first, int64_t second)
{
  uint64_t result = (uint64_t) first - (uint64_t) second;
  int64_t signed_result;

  memcpy (&signed_result, &result, sizeof(signed_result));
  return signed_result;
}

static void
milan_refine_affine_least_squares (
  const GoodixMilanFeatureRecord *enrolled_records,
  const GoodixMilanFeatureRecord *probe_records,
  const int32_t                  *pairs,
  size_t                          pair_count,
  const uint8_t                  *inliers,
  int32_t                         previous_residual,
  int32_t                         affine[6])
{
  int64_t matrix[3][3] = { { 0 } };
  int64_t right[3][2] = { { 0 } };
  size_t inlier_count = 0;
  int32_t refined[6];

  for (size_t i = 0; i < pair_count; i++)
    {
      if (!inliers[i])
        continue;
      int32_t enrolled_index = pairs[i * 2];
      int32_t probe_index = pairs[i * 2 + 1];
      int64_t source[3] = {
        (uint16_t) probe_records[probe_index].refined_x,
        (uint16_t) probe_records[probe_index].refined_y,
        MILAN_AFFINE_Q8_ONE,
      };
      int64_t target[2] = {
        (uint16_t) enrolled_records[enrolled_index].refined_x,
        (uint16_t) enrolled_records[enrolled_index].refined_y,
      };

      for (size_t row = 0; row < 3; row++)
        {
          for (size_t column = 0; column < 3; column++)
            matrix[row][column] += source[row] * source[column];
          for (size_t column = 0; column < 2; column++)
            right[row][column] += source[row] * target[column];
        }
      inlier_count++;
    }
  if (inlier_count < 3)
    return;
  for (size_t row = 0; row < 3; row++)
    {
      for (size_t column = 0; column < 3; column++)
        matrix[row][column] =
          (matrix[row][column] + MILAN_AFFINE_Q8_HALF) >>
          MILAN_AFFINE_Q8_SHIFT;
      for (size_t column = 0; column < 2; column++)
        right[row][column] =
          (right[row][column] + MILAN_AFFINE_Q8_HALF) >>
          MILAN_AFFINE_Q8_SHIFT;
    }

  int64_t cofactors[3][3] = {
    {
      milan_wrapped_subtract (
        milan_wrapped_multiply (matrix[1][1], matrix[2][2]),
        milan_wrapped_multiply (matrix[1][2], matrix[2][1])),
      milan_wrapped_subtract (
        milan_wrapped_multiply (matrix[0][2], matrix[2][1]),
        milan_wrapped_multiply (matrix[0][1], matrix[2][2])),
      milan_wrapped_subtract (
        milan_wrapped_multiply (matrix[0][1], matrix[1][2]),
        milan_wrapped_multiply (matrix[0][2], matrix[1][1])),
    },
    {
      milan_wrapped_subtract (
        milan_wrapped_multiply (matrix[2][0], matrix[1][2]),
        milan_wrapped_multiply (matrix[1][0], matrix[2][2])),
      milan_wrapped_subtract (
        milan_wrapped_multiply (matrix[0][0], matrix[2][2]),
        milan_wrapped_multiply (matrix[2][0], matrix[0][2])),
      milan_wrapped_subtract (
        milan_wrapped_multiply (matrix[1][0], matrix[0][2]),
        milan_wrapped_multiply (matrix[0][0], matrix[1][2])),
    },
    {
      milan_wrapped_subtract (
        milan_wrapped_multiply (matrix[1][0], matrix[2][1]),
        milan_wrapped_multiply (matrix[2][0], matrix[1][1])),
      milan_wrapped_subtract (
        milan_wrapped_multiply (matrix[2][0], matrix[0][1]),
        milan_wrapped_multiply (matrix[0][0], matrix[2][1])),
      milan_wrapped_subtract (
        milan_wrapped_multiply (matrix[0][0], matrix[1][1]),
        milan_wrapped_multiply (matrix[1][0], matrix[0][1])),
    },
  };
  int64_t determinant = milan_wrapped_add (
    milan_wrapped_add (
      milan_wrapped_multiply (matrix[0][0], cofactors[0][0]),
      milan_wrapped_multiply (matrix[0][1], cofactors[0][1])),
    milan_wrapped_multiply (matrix[0][2], cofactors[0][2]));
  int64_t rounded_determinant = milan_wrapped_add (
    determinant, MILAN_AFFINE_Q8_HALF);
  int64_t denominator = rounded_determinant >> MILAN_AFFINE_Q8_SHIFT;
  int64_t half_denominator = rounded_determinant >> 9;

  if (denominator == 0)
    return;
  for (size_t output = 0; output < 2; output++)
    {
      int64_t numerator[3];

      for (size_t row = 0; row < 3; row++)
        numerator[row] = milan_wrapped_add (
          milan_wrapped_add (
            milan_wrapped_multiply (right[0][output], cofactors[row][0]),
            milan_wrapped_multiply (right[1][output], cofactors[row][1])),
          milan_wrapped_multiply (right[2][output], cofactors[row][2]));

      refined[output * 3] = milan_divide_affine_coefficient (
        numerator[0], denominator, half_denominator);
      refined[output * 3 + 1] = milan_divide_affine_coefficient (
        numerator[1], denominator, half_denominator);
      refined[output * 3 + 2] = milan_divide_affine_coefficient (
        numerator[2], denominator, half_denominator);
    }

  size_t refined_inlier_count = 0;
  uint64_t refined_residual_sum = 0;

  for (size_t i = 0; i < pair_count; i++)
    {
      int32_t enrolled_index = pairs[i * 2];
      int32_t probe_index = pairs[i * 2 + 1];
      int32_t x = (uint16_t) probe_records[probe_index].refined_x;
      int32_t y = (uint16_t) probe_records[probe_index].refined_y;
      int64_t transformed_x =
        (((int64_t) refined[0] * x + (int64_t) refined[1] * y +
          MILAN_AFFINE_Q8_HALF) >> MILAN_AFFINE_Q8_SHIFT) +
        refined[2];
      int64_t transformed_y =
        (((int64_t) refined[3] * x + (int64_t) refined[4] * y +
          MILAN_AFFINE_Q8_HALF) >> MILAN_AFFINE_Q8_SHIFT) +
        refined[5];
      /* Refinement can publish large coefficients after orientation filtering.
       * Native narrows the residual to a signed dword before squaring it. */
      int64_t dx = milan_reinterpret_uint32_as_int32 (
        (uint32_t) transformed_x -
        (uint16_t) enrolled_records[enrolled_index].refined_x);
      int64_t dy = milan_reinterpret_uint32_as_int32 (
        (uint32_t) transformed_y -
        (uint16_t) enrolled_records[enrolled_index].refined_y);
      uint64_t squared = (uint64_t) (dx * dx) + (uint64_t) (dy * dy);

      if (squared < MILAN_RECOGNITION_INLIER_SQUARED_LIMIT)
        {
          refined_inlier_count++;
          refined_residual_sum += squared;
        }
    }
  int32_t refined_residual = refined_inlier_count == 0
                                ? MILAN_RECOGNITION_NO_MODEL_RESIDUAL
                               : (int32_t) ((refined_residual_sum +
                                             (refined_inlier_count >> 1)) /
                                            refined_inlier_count);

  if (refined_residual < previous_residual &&
      refined_inlier_count >= inlier_count)
    memcpy (affine, refined, sizeof(refined));
}

static uint64_t milan_match_integer_sqrt_u64 (uint64_t value);

static int
milan_filter_affine_orientation (
  const GoodixMilanFeatureRecord *enrolled_records,
  const GoodixMilanFeatureRecord *probe_records,
  const int32_t                  *pairs,
  size_t                          pair_count,
  const int32_t                   affine[6],
  uint8_t                         inliers[MILAN_MATCH_MAX_PAIRS])
{
  uint32_t first_length = (uint32_t) milan_match_integer_sqrt_u64 (
    (uint64_t) ((int64_t) affine[0] * affine[0]) +
    (uint64_t) ((int64_t) affine[3] * affine[3]));
  uint32_t second_length = (uint32_t) milan_match_integer_sqrt_u64 (
    (uint64_t) ((int64_t) affine[1] * affine[1]) +
    (uint64_t) ((int64_t) affine[4] * affine[4]));
  int32_t average_length = (int32_t) ((first_length + second_length) / 2);
  int retained = 0;

  if (average_length == 0)
    return 0;

  int32_t cosine = ((affine[4] + affine[0]) / 2 * MILAN_AFFINE_Q8_ONE) /
                    average_length;
  int32_t sine = ((affine[3] - affine[1]) / 2 * MILAN_AFFINE_Q8_ONE) /
                  average_length;
  int32_t affine_angle = feature_atan2 (cosine, sine);

  if (affine_angle < 0)
    affine_angle += MILAN_ORIENTATION_FULL_PERIOD;
  for (size_t i = 0; i < pair_count; i++)
    {
      if (inliers[i])
        {
          int32_t enrolled_index = pairs[i * 2];
          int32_t probe_index = pairs[i * 2 + 1];
          int32_t difference =
            enrolled_records[enrolled_index].orientation -
            probe_records[probe_index].orientation + affine_angle;
          int32_t direct = difference;
          int32_t opposite = difference + MILAN_ORIENTATION_PERIOD;

          if (direct < 0)
            direct += MILAN_ORIENTATION_FULL_PERIOD;
          if (direct > MILAN_ORIENTATION_FULL_PERIOD)
            direct -= MILAN_ORIENTATION_FULL_PERIOD;
          if (MILAN_ORIENTATION_FULL_PERIOD - direct < direct)
            direct = MILAN_ORIENTATION_FULL_PERIOD - direct;
          if (opposite < 0)
            opposite += MILAN_ORIENTATION_FULL_PERIOD;
          if (opposite > MILAN_ORIENTATION_FULL_PERIOD)
            opposite -= MILAN_ORIENTATION_FULL_PERIOD;
          if (MILAN_ORIENTATION_FULL_PERIOD - opposite < opposite)
            opposite = MILAN_ORIENTATION_FULL_PERIOD - opposite;
          if (opposite < direct)
            direct = opposite;
          if (direct > MILAN_RECOGNITION_ORIENTATION_LIMIT)
            inliers[i] = 0;
        }
      retained += inliers[i] != 0;
    }
  return retained;
}

int
goodix_milan_filter_recognition_pairs_internal (
  const GoodixMilanFeatureRecord *enrolled_records,
  const GoodixMilanFeatureRecord *probe_records,
  const int32_t                  *pairs,
  size_t                          match_count,
  int32_t                         best_affine[6],
  int                            *best_residual,
  uint8_t                        *output_mask,
  int                            *model_valid)
{
  int best_inliers = 0;
  uint8_t best_mask[MILAN_MATCH_MAX_PAIRS] = { 0 };
  int triangle_count = 0;

  if (model_valid)
    *model_valid = 0;
  *best_residual = MILAN_RECOGNITION_NO_MODEL_RESIDUAL;
  memset (best_affine, 0, 6 * sizeof(*best_affine));
  for (size_t first = 0; first + 2 < match_count; first++)
    {
      if (triangle_count >= MILAN_RECOGNITION_TRIANGLE_LIMIT)
        break;
      for (size_t second = first + 1; second + 1 < match_count; second++)
        {
          const size_t edge[2] = { first, second };
          int32_t source[6];
          int32_t target[6];
          int32_t edge_orientations[2];

          for (size_t i = 0; i < 2; i++)
            {
              int32_t enrolled_index = pairs[edge[i] * 2];
              int32_t probe_index = pairs[edge[i] * 2 + 1];

              source[i * 2] =
                (uint16_t) probe_records[probe_index].refined_x;
              source[i * 2 + 1] =
                (uint16_t) probe_records[probe_index].refined_y;
              target[i * 2] =
                (uint16_t) enrolled_records[enrolled_index].refined_x;
              target[i * 2 + 1] =
                (uint16_t) enrolled_records[enrolled_index].refined_y;
              edge_orientations[i] = milan_normalize_orientation_difference (
                enrolled_records[enrolled_index].orientation -
                probe_records[probe_index].orientation,
                MILAN_ORIENTATION_HALF_PERIOD, MILAN_ORIENTATION_PERIOD);
            }
          if (!milan_triangle_edge_is_consistent (
                milan_triangle_distance_shift_then_add (
                  source[0], source[1], source[2], source[3]),
                milan_triangle_distance_shift_then_add (
                  target[0], target[1], target[2], target[3])))
            continue;
          for (size_t third = second + 1; third < match_count; third++)
            {
              /* The orientation retry mutates its per-triangle scratch. */
              int32_t orientation_differences[3] = {
                edge_orientations[0], edge_orientations[1], 0
              };
              int32_t affine[6];
              int inliers = 0;
              int64_t residual_sum = 0;
              uint8_t mask[MILAN_MATCH_MAX_PAIRS] = { 0 };

              {
                int32_t enrolled_index = pairs[third * 2];
                int32_t probe_index = pairs[third * 2 + 1];

                source[4] = (uint16_t) probe_records[probe_index].refined_x;
                source[5] = (uint16_t) probe_records[probe_index].refined_y;
                target[4] = (uint16_t) enrolled_records[enrolled_index].refined_x;
                target[5] = (uint16_t) enrolled_records[enrolled_index].refined_y;
                orientation_differences[2] =
                  milan_normalize_orientation_difference (
                    enrolled_records[enrolled_index].orientation -
                      probe_records[probe_index].orientation,
                    MILAN_ORIENTATION_HALF_PERIOD, MILAN_ORIENTATION_PERIOD);
              }
              if (!milan_triangle_edge_is_consistent (
                    milan_triangle_distance_shift_then_add (
                      source[0], source[1], source[4], source[5]),
                    milan_triangle_distance_shift_then_add (
                      target[0], target[1], target[4], target[5])) ||
                  !milan_triangle_edge_is_consistent (
                    milan_triangle_distance_add_then_shift (
                      source[2], source[3], source[4], source[5]),
                    milan_triangle_distance_add_then_shift (
                      target[2], target[3], target[4], target[5])) ||
                  !milan_triangle_orientations_are_consistent (
                    orientation_differences))
                continue;
              triangle_count++;
              milan_affine_from_three_points (source, target, affine);
              if (!milan_triangle_affine_is_consistent (affine))
                continue;
              for (size_t i = 0; i < match_count; i++)
                {
                  int32_t enrolled_index = pairs[i * 2];
                  int32_t probe_index = pairs[i * 2 + 1];
                  int32_t x = (uint16_t) probe_records[probe_index].refined_x;
                  int32_t y = (uint16_t) probe_records[probe_index].refined_y;
                  int64_t transformed_x =
                    (((int64_t) affine[0] * x +
                      (int64_t) affine[1] * y + 0x80) >> 8) + affine[2];
                  int64_t transformed_y =
                    (((int64_t) affine[3] * x +
                      (int64_t) affine[4] * y + 0x80) >> 8) + affine[5];
                  int64_t dx = transformed_x -
                    (uint16_t) enrolled_records[enrolled_index].refined_x;
                  int64_t dy = transformed_y -
                    (uint16_t) enrolled_records[enrolled_index].refined_y;
                  int64_t squared = dx * dx + dy * dy;

                  if (llabs (dx) < MILAN_RECOGNITION_INLIER_AXIS_LIMIT &&
                      llabs (dy) < MILAN_RECOGNITION_INLIER_AXIS_LIMIT &&
                      squared < MILAN_RECOGNITION_INLIER_SQUARED_LIMIT)
                    {
                      inliers++;
                      residual_sum += squared;
                      mask[i] = 1;
                    }
                }
              int residual = inliers == 0
                               ? MILAN_RECOGNITION_NO_MODEL_RESIDUAL
                               : (int) ((residual_sum + (inliers >> 1)) /
                                        inliers);
              if ((inliers > best_inliers ||
                   (inliers == best_inliers && residual < *best_residual)) &&
                  milan_affine_is_valid (affine))
                {
                  best_inliers = inliers;
                  *best_residual = residual;
                  memcpy (best_affine, affine, 6 * sizeof(*best_affine));
                  memcpy (best_mask, mask, sizeof(best_mask));
                }
              if (best_inliers > MILAN_RECOGNITION_EARLY_INLIER_COUNT)
                goto done;
            }
        }
    }
done:
  if (model_valid)
    *model_valid = best_inliers > 0;
  best_inliers = milan_filter_affine_orientation (
    enrolled_records, probe_records, pairs, match_count, best_affine,
    best_mask);
  if (output_mask)
    memcpy (output_mask, best_mask, sizeof(best_mask));
  if (best_inliers > MILAN_RECOGNITION_REFINE_INLIER_COUNT &&
      *best_residual > MILAN_RECOGNITION_REFINE_RESIDUAL)
    milan_refine_affine_least_squares (
      enrolled_records, probe_records, pairs, match_count, best_mask,
      *best_residual, best_affine);
  return best_inliers;
}

void
goodix_milan_match_record_metrics_internal (
  const GoodixMilanFeatureRecord *enrolled_records,
  size_t                          enrolled_record_count,
  const GoodixMilanFeatureRecord *probe_records,
  size_t                          probe_record_count,
  const int32_t                   transform[6],
  int                             filtered_count,
  int32_t                        *topology_percent,
  int32_t                        *geometric_percent,
  int32_t                        *topology_bonus,
  int32_t                        *topology_distance_output,
  int32_t                        *valid_count_output,
  int32_t                        *matched_count_output)
{
  int valid_topology_records = 0;
  int matched_topology_records = 0;
  int topology_distance_sum = 0;

  for (size_t probe_index = 0; probe_index < probe_record_count; probe_index++)
    {
      int64_t raw_x =
        (int64_t) transform[0] * (uint16_t) probe_records[probe_index].refined_x +
        (int64_t) transform[1] * (uint16_t) probe_records[probe_index].refined_y +
        (int64_t) transform[2] * 0x100;
      int64_t raw_y =
        (int64_t) transform[3] * (uint16_t) probe_records[probe_index].refined_x +
        (int64_t) transform[4] * (uint16_t) probe_records[probe_index].refined_y +
        (int64_t) transform[5] * 0x100;
      int32_t transformed_x = milan_reinterpret_uint32_as_int32 (
        raw_x < 1 ? UINT32_C (0) - (uint32_t) ((0x80 - raw_x) >> 8) :
                    (uint32_t) ((raw_x + 0x80) >> 8));
      int32_t transformed_y = milan_reinterpret_uint32_as_int32 (
        raw_y < 1 ? UINT32_C (0) - (uint32_t) ((0x80 - raw_y) >> 8) :
                    (uint32_t) ((raw_y + 0x80) >> 8));

      if (transformed_x < 0x600 ||
          transformed_x >=
          (GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS - 7) * 0x100 ||
          transformed_y < 0x600 ||
          transformed_y >=
          (GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS - 7) * 0x100)
        continue;
      valid_topology_records++;

      int best_index = -1;
      int best_squared = INT32_MAX;
      for (size_t enrolled_index = 0;
           enrolled_index < enrolled_record_count; enrolled_index++)
        {
          if ((enrolled_records[enrolled_index].foreground & 3) !=
              (probe_records[probe_index].foreground & 3))
            continue;
          int32_t dx = transformed_x -
                       (uint16_t) enrolled_records[enrolled_index].refined_x;
          int32_t dy = transformed_y -
                       (uint16_t) enrolled_records[enrolled_index].refined_y;
          if (abs (dx) > 0x200 || abs (dy) > 0x200)
            continue;
          int squared = dx * dx + dy * dy;
          if (squared < best_squared)
            {
              best_squared = squared;
              best_index = (int) enrolled_index;
            }
        }
      if (best_index >= 0 && best_squared < 0x40000)
        {
          const uint8_t *enrolled =
            (const uint8_t *) &enrolled_records[best_index];
          const uint8_t *probe = (const uint8_t *) &probe_records[probe_index];
          int first_distance = 0;
          int second_distance = 0;

          for (size_t i = 0; i < 8; i++)
            {
              first_distance += __builtin_popcount (
                (unsigned) (enrolled[40 + i] ^ probe[40 + i]));
              second_distance += __builtin_popcount (
                (unsigned) (enrolled[40 + i] ^ probe[48 + i]));
            }
          matched_topology_records++;
          topology_distance_sum += first_distance < second_distance
                                     ? first_distance
                                     : second_distance;
        }
    }

  int topology_distance = matched_topology_records == 0
                            ? 0
                            : topology_distance_sum /
                              matched_topology_records;

  *topology_percent = valid_topology_records == 0
                        ? 0
                        : matched_topology_records * 100 /
                          valid_topology_records;
  *geometric_percent = valid_topology_records == 0
                         ? 0
                         : filtered_count * 100 / valid_topology_records;
  *topology_bonus = *topology_percent > 30 && topology_distance < 10 &&
                    filtered_count < 31;
  if (topology_distance_output)
    *topology_distance_output = topology_distance;
  if (valid_count_output)
    *valid_count_output = valid_topology_records;
  if (matched_count_output)
    *matched_count_output = matched_topology_records;
}

static uint64_t
milan_match_integer_sqrt_u64 (uint64_t value)
{
  uint64_t root = 0;
  uint64_t bit = UINT64_C (1) << 62;

  while (bit > value)
    bit >>= 2;
  while (bit != 0)
    {
      if (value >= root + bit)
        {
          value -= root + bit;
          root = (root >> 1) + bit;
        }
      else
        root >>= 1;
      bit >>= 2;
    }
  return root;
}

static uint32_t
milan_match_affine_product_root (uint64_t value)
{
  uint32_t root = 0;
  uint64_t bit = UINT64_C (0x80000000);

  /* Native doubles the root in a dword before widening the trial. */
  for (int shift = 31; shift >= 0; shift--, bit >>= 1)
    {
      uint64_t trial = ((uint32_t) (root * UINT32_C (2)) + bit) << shift;

      if (value >= trial)
        {
          root += (uint32_t) bit;
          value -= trial;
        }
    }
  return root;
}

void
goodix_milan_match_affine_penalties (const int32_t transform[6],
                               int32_t      *scale_penalty,
                               int32_t      *orthogonality_penalty,
                               int32_t      *strong_orthogonality_penalty)
{
  int32_t average_scale;
  int32_t orthogonality;

  goodix_milan_match_affine_details (transform, &average_scale, &orthogonality);
  *scale_penalty = (uint32_t) (average_scale - 234) > 47;
  *orthogonality_penalty = orthogonality >= 0x147b;
  if (strong_orthogonality_penalty)
    *strong_orthogonality_penalty = orthogonality >= 0x28f6;
}

void
goodix_milan_match_affine_details (const int32_t transform[6],
                            int32_t      *average_scale,
                            int32_t      *absolute_dot_q16)
{
  uint32_t a = (uint32_t) transform[0];
  uint32_t b = (uint32_t) transform[1];
  uint32_t c = (uint32_t) transform[3];
  uint32_t d = (uint32_t) transform[4];
  uint32_t first_squared = a * a + c * c;
  uint32_t second_squared = b * b + d * d;
  uint32_t first_length = (uint32_t) milan_match_integer_sqrt_u64 (first_squared);
  uint32_t second_length = (uint32_t) milan_match_integer_sqrt_u64 (second_squared);
  int64_t product =
    (int64_t) milan_reinterpret_uint32_as_int32 (first_squared) *
    milan_reinterpret_uint32_as_int32 (second_squared);
  uint32_t product_length = milan_match_affine_product_root ((uint64_t) product);
  int64_t dot = milan_reinterpret_uint32_as_int32 (a * b + c * d);

  *average_scale = (int32_t) ((first_length + second_length) >> 1);
  *absolute_dot_q16 = 0;
  if (first_length != 0 && product_length != 0)
    *absolute_dot_q16 = milan_reinterpret_uint32_as_int32 (
      (uint32_t) ((dot * 0x10000) / product_length));
  if (*absolute_dot_q16 < 0)
    *absolute_dot_q16 = milan_reinterpret_uint32_as_int32 (
      UINT32_C (0) - (uint32_t) *absolute_dot_q16);
}

void
goodix_milan_match_fit_affine_state (
  const GoodixMilanFeatureRecord *enrolled_records,
  const GoodixMilanFeatureRecord *probe_records,
  const int32_t                  *pairs,
  size_t                          pair_count,
  int                             attempted,
  MilanMatchAffineState          *state)
{
  memset (state, 0, sizeof(*state));
  state->selected_pair_count = pair_count;
  state->attempted = attempted && pair_count >= 3;
  if (!state->attempted)
    return;

  state->filtered_count = goodix_milan_filter_recognition_pairs_internal (
    enrolled_records, probe_records, pairs, pair_count, state->affine,
    &state->residual, NULL, &state->valid);
}

int
goodix_milan_filter_recognition_pairs (
  const GoodixMilanFeatureRecord *enrolled_records,
  const GoodixMilanFeatureRecord *probe_records,
  const int32_t                  *pairs,
  size_t                          pair_count,
  int32_t                         transform[6],
  size_t                         *filtered_count,
  int32_t                        *residual)
{
  int32_t best_residual = 0x190000;
  int count;

  if (!enrolled_records || !probe_records || !pairs ||
      pair_count > MILAN_MATCH_MAX_PAIRS ||
      !transform || !filtered_count || !residual)
    return -1;
  count = goodix_milan_filter_recognition_pairs_internal (
    enrolled_records, probe_records, pairs, pair_count, transform,
    &best_residual, NULL, NULL);
  *filtered_count = (size_t) count;
  *residual = best_residual;
  return count > 0 ? 0 : -1;
}

int
goodix_milan_estimate_relation (
  const GoodixMilanFeatureRecord *prior_records,
  size_t                          prior_record_count,
  const GoodixMilanFeatureRecord *current_records,
  size_t                          current_record_count,
  int32_t                         relation_index,
  GoodixMilanTemplateRelation    *relation)
{
  MilanFeatureMatch matches[31];
  int32_t best_affine[6] = { 0 };
  int best_inliers = 0;
  int best_residual = 0x190000;
  size_t match_count;

  if (!prior_records || !current_records || !relation ||
      prior_record_count == 0 || current_record_count == 0 ||
      prior_record_count > 150 || current_record_count > 150)
    return -1;
  match_count = goodix_milan_match_feature_records (
    prior_records, prior_record_count, current_records, current_record_count,
    matches);
  if (match_count < 5)
    return -1;

  for (size_t first = 0; first + 2 < match_count; first++)
    for (size_t second = first + 1; second + 1 < match_count; second++)
      for (size_t third = second + 1; third < match_count; third++)
        {
          const size_t selected[3] = { first, second, third };
          int32_t source[6];
          int32_t target[6];
          int32_t affine[6];
          int inliers = 0;
          int64_t residual_sum = 0;

          for (size_t i = 0; i < 3; i++)
            {
              const MilanFeatureMatch *match = &matches[selected[i]];

              source[i * 2] = (uint16_t)
                current_records[match->current_index].refined_x;
              source[i * 2 + 1] = (uint16_t)
                current_records[match->current_index].refined_y;
              target[i * 2] = (uint16_t)
                prior_records[match->prior_index].refined_x;
              target[i * 2 + 1] = (uint16_t)
                prior_records[match->prior_index].refined_y;
            }
          milan_affine_from_three_points (source, target, affine);
          for (size_t i = 0; i < match_count; i++)
            {
              int32_t x = (uint16_t)
                current_records[matches[i].current_index].refined_x;
              int32_t y = (uint16_t)
                current_records[matches[i].current_index].refined_y;
              int64_t transformed_x =
                (((int64_t) affine[0] * x +
                  (int64_t) affine[1] * y) >> 8) +
                affine[2];
              int64_t transformed_y =
                (((int64_t) affine[3] * x +
                  (int64_t) affine[4] * y) >> 8) +
                affine[5];
              /* Native rejects mapped coordinates before subtraction, then
               * bounds each residual before evaluating its square. */
              if (transformed_x <= -INT64_C(0x100000000) ||
                  transformed_x >= INT64_C(0x100000000) ||
                  transformed_y <= -INT64_C(0x100000000) ||
                  transformed_y >= INT64_C(0x100000000))
                continue;
              int64_t dx = transformed_x -
                           (uint16_t) prior_records[matches[i].prior_index].refined_x;
              int64_t dy = transformed_y -
                           (uint16_t) prior_records[matches[i].prior_index].refined_y;
              if (dx < -640 || dx > 640 || dy < -640 || dy > 640)
                continue;
              int64_t squared = dx * dx + dy * dy;

              if (squared < 0x64000)
                {
                  inliers++;
                  residual_sum += squared;
                }
            }
          int residual = inliers == 0
                           ? 0x190000
                           : (int) ((residual_sum + (inliers >> 1)) / inliers);
          if ((inliers > best_inliers ||
               (inliers == best_inliers && residual < best_residual)) &&
              milan_affine_is_valid (affine))
            {
              best_inliers = inliers;
              best_residual = residual;
              memcpy (best_affine, affine, sizeof(best_affine));
            }
          if (best_inliers > 20)
            goto done;
        }

done:
  if (best_inliers == 0)
    return -1;
  relation->index = relation_index;
  relation->values[0] = best_inliers;
  memcpy (relation->values + 1, best_affine, sizeof(best_affine));
  return 0;
}

static void
milan_fill_record_index_map (const GoodixMilanFeatureRecord *records,
                             size_t                          begin,
                             size_t                          end,
                             int32_t                         radius,
                             int16_t                         map[GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS])
{
  memset (map, 0xff,
          GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS * sizeof (*map));
  for (size_t index = begin; index < end; index++)
    {
      int32_t x = ((uint16_t) records[index].refined_x + 0x80) >> 8;
      int32_t y = ((uint16_t) records[index].refined_y + 0x80) >> 8;
      int32_t left = x > radius ? x - radius : 0;
      int32_t right = x + radius < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS ?
                      x + radius :
                      GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS - 1;
      int32_t top = y > radius ? y - radius : 0;
      int32_t bottom = y + radius < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS ?
                       y + radius :
                       GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS - 1;

      for (int32_t row = top; row <= bottom; row++)
        for (int32_t column = left; column <= right; column++)
          map[row * GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS + column] =
            (int16_t) index;
    }
}

int
goodix_milan_refine_record_similarity (
  const GoodixMilanFeatureRecord *enrolled_records,
  size_t                          enrolled_record_count,
  size_t                          enrolled_partition,
  const GoodixMilanFeatureRecord *probe_records,
  size_t                          probe_record_count,
  size_t                          probe_partition,
  const int32_t                   transform[6],
  int32_t                         search_radius,
  int32_t                         refined[6])
{
  int32_t inverse[6];
  int32_t pairs[300];
  int16_t index_map[GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS];
  size_t pair_count = 0;
  uint64_t source_x_sum = 0;
  uint64_t source_y_sum = 0;
  uint64_t target_x_sum = 0;
  uint64_t target_y_sum = 0;
  uint64_t source_square_sum = 0;
  int64_t dot_sum = 0;
  int64_t cross_sum = 0;

  if (enrolled_partition > enrolled_record_count ||
      probe_partition > probe_record_count || search_radius < 0 ||
      goodix_milan_transform_invert (transform, inverse) != 0)
    return -1;
  for (size_t partition = 0; partition < 2; partition++)
    {
      size_t enrolled_begin = partition == 0 ? 0 : enrolled_partition;
      size_t enrolled_end = partition == 0 ? enrolled_partition
                                            : enrolled_record_count;
      size_t probe_begin = partition == 0 ? 0 : probe_partition;
      size_t probe_end = partition == 0 ? probe_partition : probe_record_count;

      milan_fill_record_index_map (
        probe_records, probe_begin, probe_end, search_radius, index_map);
      for (size_t index = enrolled_begin; index < enrolled_end; index++)
        {
          int32_t x = (uint16_t) enrolled_records[index].refined_x;
          int32_t y = (uint16_t) enrolled_records[index].refined_y;
          int64_t mapped_x =
            (((int64_t) inverse[0] * x + (int64_t) inverse[1] * y + 0x80) >> 8) +
            inverse[2];
          int64_t mapped_y =
            (((int64_t) inverse[3] * x + (int64_t) inverse[4] * y + 0x80) >> 8) +
            inverse[5];
          /* Native rounds the translated coordinate as a wrapping dword. */
          int32_t pixel_x = milan_reinterpret_uint32_as_int32 (
            (uint32_t) mapped_x + UINT32_C (0x80)) >> 8;
          int32_t pixel_y = milan_reinterpret_uint32_as_int32 (
            (uint32_t) mapped_y + UINT32_C (0x80)) >> 8;

          if (pixel_x < 0 ||
              pixel_x >= GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS ||
              pixel_y < 0 ||
              pixel_y >= GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS)
            continue;
          int32_t probe_index =
            index_map[pixel_y * GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS +
                      pixel_x];
          if (probe_index < 0 || pair_count == 150)
            continue;
          pairs[pair_count * 2] = (int32_t) index;
          pairs[pair_count * 2 + 1] = probe_index;
          pair_count++;
        }
    }
  if (pair_count < 3)
    return -1;
  for (size_t pair = 0; pair < pair_count; pair++)
    {
      int32_t enrolled_index = pairs[pair * 2];
      int32_t probe_index = pairs[pair * 2 + 1];
      uint32_t source_x = (uint16_t) probe_records[probe_index].refined_x;
      uint32_t source_y = (uint16_t) probe_records[probe_index].refined_y;
      uint32_t target_x =
        (uint16_t) enrolled_records[enrolled_index].refined_x;
      uint32_t target_y =
        (uint16_t) enrolled_records[enrolled_index].refined_y;

      source_x_sum += source_x;
      source_y_sum += source_y;
      target_x_sum += target_x;
      target_y_sum += target_y;
      source_square_sum +=
        (uint64_t) source_x * source_x + (uint64_t) source_y * source_y;
      dot_sum += (int64_t) target_x * source_x +
                 (int64_t) target_y * source_y;
      cross_sum += (int64_t) target_x * -(int64_t) source_y +
                   (int64_t) target_y * source_x;
    }

  int64_t count = (int64_t) pair_count;
  int64_t source_x = (int64_t) (source_x_sum + 0x80) >> 8;
  int64_t source_y = (int64_t) (source_y_sum + 0x80) >> 8;
  int64_t target_x = (int64_t) (target_x_sum + 0x80) >> 8;
  int64_t target_y = (int64_t) (target_y_sum + 0x80) >> 8;
  int64_t squares = (int64_t) (source_square_sum + 0x8000) >> 16;
  int64_t dot = (dot_sum + 0x8000) >> 16;
  int64_t cross = (cross_sum + 0x8000) >> 16;
  int64_t denominator =
    squares * count - source_x * source_x - source_y * source_y;

  if (denominator == 0)
    return -1;
  int64_t reciprocal = ((denominator >> 1) + INT64_C (0x800000000)) /
                       denominator;
  /* At most 150 unsigned-word pairs bound the sums and denominator, and
   * reciprocal * source_x/y fit signed64. Later fit products wrap natively. */
  int64_t center = milan_wrapped_add (
    milan_wrapped_multiply (reciprocal,
                           source_x * source_x + source_y * source_y),
    INT64_C (0x800000000)) / count;
  int64_t negative_source_x = -(reciprocal * source_x);
  int64_t negative_source_y = -(reciprocal * source_y);

  refined[0] = milan_reinterpret_uint32_as_int32 ((uint32_t) (
    milan_wrapped_multiply (count * dot - target_y * source_y -
                           target_x * source_x, reciprocal) >> 27));
  refined[4] = refined[0];
  refined[3] = milan_reinterpret_uint32_as_int32 ((uint32_t) (
    milan_wrapped_multiply (count * cross - target_y * source_x +
                           target_x * source_y, reciprocal) >> 27));
  refined[1] = milan_reinterpret_uint32_as_int32 (
    UINT32_C (0) - (uint32_t) refined[3]);
  refined[5] = milan_reinterpret_uint32_as_int32 ((uint32_t) (
    milan_wrapped_add (
      milan_wrapped_add (milan_wrapped_multiply (negative_source_x, cross),
                        milan_wrapped_multiply (negative_source_y, dot)),
      milan_wrapped_multiply (center, target_y)) >> 27));
  refined[2] = milan_reinterpret_uint32_as_int32 ((uint32_t) (
    milan_wrapped_add (
      milan_wrapped_subtract (milan_wrapped_multiply (negative_source_x, dot),
                             milan_wrapped_multiply (negative_source_y, cross)),
      milan_wrapped_multiply (center, target_x)) >> 27));
  return 0;
}

int
goodix_milan_match_post_admission_replaces (int32_t current_score,
                                      int32_t current_detail,
                                      int32_t alternate_score,
                                      int32_t alternate_detail)
{
  return alternate_score != 0x80 &&
         (current_score == 0x80 || alternate_detail > current_detail);
}
