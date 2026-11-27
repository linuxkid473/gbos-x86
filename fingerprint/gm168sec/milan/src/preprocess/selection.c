/*
 * Goodix 53x5 driver for libfprint - Milan preprocessing selection
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/preprocess/state.h"

#include <limits.h>
#include <string.h>

/* Profile-9 candidate selection recovered from FUN_180069150,
 * FUN_180081d30, and FUN_18006e7f0. */
#define SELECTION_Q8_SHIFT 8
#define SELECTION_Q8_ONE (1 << SELECTION_Q8_SHIFT)
#define SELECTION_EXCLUDED_PIXEL 122
#define SELECTION_FIRST_BOUNDARY 1800
#define SELECTION_SECOND_BOUNDARY 3600
#define SELECTION_FINAL_BOUNDARY 5400
#define SELECTION_LOW_RATIO 205
#define SELECTION_MIDDLE_SLOPE 179
#define SELECTION_HIGH_SLOPE 2
#define SELECTION_HIGH_RATIO 384
#define SELECTION_MAX_CORRELATION 235

static int
milan_selection_metric (const uint8_t *frame,
                        const uint8_t *mask,
                        size_t         rows,
                        size_t         columns)
{
  size_t count;
  uint64_t valid_sum = 0;
  size_t valid_count = 0;
  size_t first_row = 0;
  size_t last_row = 0;
  int found_first = 0;
  int previous_average = 0;
  uint64_t difference_sum = 0;

  count = rows * columns;
  for (size_t i = 0; i < count; i++)
    {
      if (mask[i] != 0)
        {
          valid_sum += frame[i];
          valid_count++;
          if (!found_first)
            {
              first_row = i / columns;
              found_first = 1;
            }
          last_row = i / columns;
        }
    }

  if (!found_first)
    return 0;

  uint32_t fallback = (uint32_t) (valid_sum / (valid_count + 1));
  for (size_t row = first_row; row <= last_row; row++)
    {
      uint32_t row_sum = 0;

      for (size_t column = 0; column < columns; column++)
        {
          size_t index = row * columns + column;

          row_sum += mask[index] != 0 ? frame[index] : fallback;
        }

      int average = (int) ((row_sum << SELECTION_Q8_SHIFT) / columns);
      if (row != first_row)
        difference_sum += average > previous_average ?
                          (uint32_t) (average - previous_average) :
                          (uint32_t) (previous_average - average);
      previous_average = average;
    }

  if (last_row == first_row)
    return (int) difference_sum;
  return (int) ((((rows - 1) * difference_sum) /
                 (last_row - first_row)) >> SELECTION_Q8_SHIFT);
}

static uint32_t
integer_square_root (uint64_t value)
{
  uint32_t root = 0;
  uint32_t bit = UINT32_C (0x80000000);

  if (value < 2)
    return (uint32_t) value;
  for (int shift = 31; bit != 0; shift--, bit >>= 1)
    {
      uint64_t candidate = ((uint64_t) root * 2 + bit) << shift;

      if (candidate <= value)
        {
          root += bit;
          value -= candidate;
        }
    }
  return root;
}

static int
milan_masked_correlation (const uint8_t *first,
                          const uint8_t *second,
                          const uint8_t *mask,
                          size_t         rows,
                          size_t         columns)
{
  size_t count;
  uint64_t first_sum = 0;
  uint64_t second_sum = 0;
  int64_t covariance = 0;
  uint64_t first_variance = 0;
  uint64_t second_variance = 0;

  count = rows * columns;
  for (size_t i = 0; i < count; i++)
    {
      first_sum += mask[i] != 0 ? first[i] & mask[i] :
                   SELECTION_EXCLUDED_PIXEL;
      second_sum += mask[i] != 0 ? second[i] & mask[i] :
                    SELECTION_EXCLUDED_PIXEL;
    }

  int first_mean = (int) (first_sum / count);
  int second_mean = (int) (second_sum / count);
  for (size_t i = 0; i < count; i++)
    {
      int first_value =
        (mask[i] != 0 ? first[i] & mask[i] : SELECTION_EXCLUDED_PIXEL) -
        first_mean;
      int second_value =
        (mask[i] != 0 ? second[i] & mask[i] : SELECTION_EXCLUDED_PIXEL) -
        second_mean;

      covariance += first_value * second_value;
      first_variance += (uint32_t) (first_value * first_value);
      second_variance += (uint32_t) (second_value * second_value);
    }

  uint32_t denominator =
    integer_square_root (first_variance * second_variance);
  if (denominator == 0)
    return 0;
  return (int) ((covariance * SELECTION_Q8_ONE) / denominator);
}

/* Ties at the required metric or correlation ceiling keep primary. */
static int
milan_refined_candidate_wins (int primary_metric,
                              int refined_metric,
                              int correlation,
                              int threshold)
{
  int required_metric;

  if (primary_metric < SELECTION_FIRST_BOUNDARY)
    {
      required_metric = threshold * SELECTION_LOW_RATIO >> SELECTION_Q8_SHIFT;
    }
  else if (primary_metric < SELECTION_SECOND_BOUNDARY)
    {
      required_metric =
        (((primary_metric - SELECTION_FIRST_BOUNDARY) * threshold *
          SELECTION_MIDDLE_SLOPE) /
         (SELECTION_SECOND_BOUNDARY - SELECTION_FIRST_BOUNDARY) +
         threshold * SELECTION_LOW_RATIO) >> SELECTION_Q8_SHIFT;
    }
  else if (primary_metric < SELECTION_FINAL_BOUNDARY)
    {
      required_metric =
        ((primary_metric - SELECTION_SECOND_BOUNDARY) * threshold *
         SELECTION_HIGH_SLOPE) /
        (SELECTION_SECOND_BOUNDARY - SELECTION_FIRST_BOUNDARY) +
        (threshold * SELECTION_HIGH_RATIO >> SELECTION_Q8_SHIFT);
    }
  else
    {
      required_metric = INT_MAX;
    }

  return refined_metric < required_metric &&
         (primary_metric >= SELECTION_FIRST_BOUNDARY ||
          correlation < SELECTION_MAX_CORRELATION);
}

int
goodix_milan_preprocess_select_output (const uint8_t *contrast,
                                       const uint8_t *refined,
                                       const uint8_t *mask,
                                       size_t         rows,
                                       size_t         columns,
                                       int            threshold,
                                       uint8_t       *output,
                                       int           *selected_refined)
{
  size_t count;
  int primary_metric;
  int refined_metric;
  int correlation;

  count = rows * columns;
  primary_metric = milan_selection_metric (contrast, mask, rows, columns);

  memmove (output, contrast, count);
  *selected_refined = 0;
  if (primary_metric < threshold)
    return 0;

  refined_metric = milan_selection_metric (refined, mask, rows, columns);
  correlation = milan_masked_correlation (
    contrast, refined, mask, rows, columns);

  if (!milan_refined_candidate_wins (
        primary_metric, refined_metric, correlation, threshold))
    return 0;

  memmove (output, refined, count);
  *selected_refined = 1;
  return 0;
}
