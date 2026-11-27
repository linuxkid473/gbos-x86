/*
 * Goodix 53x5 driver for libfprint - Milan anti-fake pair scoring
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/antifake/antifake.h"
#include "milan/preprocess/state.h"
#include "milan/transform-private.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int
antifake_descriptor_distance (const uint8_t *prior,
                              const uint8_t *current)
{
  int head0 = 0;
  int head1 = 0;
  int tail24 = 0;
  int tail32 = 0;
  int tail40 = 0;
  int tail44 = 0;

  for (size_t i = 0; i < 8; i++)
    {
      head0 += __builtin_popcount (
        (unsigned) (prior[GOODIX_MILAN_ANTIFAKE_RECORD_DATA_8_OFFSET + i] ^
                    current[GOODIX_MILAN_ANTIFAKE_RECORD_DATA_8_OFFSET + i]));
      head1 += __builtin_popcount (
        (unsigned) (prior[GOODIX_MILAN_ANTIFAKE_RECORD_DATA_16_OFFSET + i] ^
                    current[GOODIX_MILAN_ANTIFAKE_RECORD_DATA_8_OFFSET + i]));
      tail24 += __builtin_popcount (
        (unsigned) (prior[GOODIX_MILAN_ANTIFAKE_RECORD_DATA_24_OFFSET + i] ^
                    current[GOODIX_MILAN_ANTIFAKE_RECORD_DATA_24_OFFSET + i]));
      tail32 += __builtin_popcount (
        (unsigned) (prior[GOODIX_MILAN_ANTIFAKE_RECORD_DATA_32_OFFSET + i] ^
                    current[GOODIX_MILAN_ANTIFAKE_RECORD_DATA_32_OFFSET + i]));
    }
  for (size_t i = 0; i < 4; i++)
    {
      tail40 += __builtin_popcount (
        (unsigned) (prior[GOODIX_MILAN_ANTIFAKE_RECORD_DATA_40_OFFSET + i] ^
                    current[GOODIX_MILAN_ANTIFAKE_RECORD_DATA_40_OFFSET + i]));
      tail44 += __builtin_popcount (
        (unsigned) (prior[GOODIX_MILAN_ANTIFAKE_RECORD_DATA_40_OFFSET + i] ^
                    current[GOODIX_MILAN_ANTIFAKE_RECORD_DATA_44_OFFSET + i]));
    }

  int head = head0 < head1 ? head0 : head1;
  int normal_tail = tail24 + tail32 + tail40;
  int shifted_tail = tail24 + 64 + tail44 - tail32;

  return head + (normal_tail < shifted_tail ? normal_tail : shifted_tail);
}

static int
compare_int32 (const void *first,
               const void *second)
{
  int32_t a = *(const int32_t *) first;
  int32_t b = *(const int32_t *) second;

  return (a > b) - (a < b);
}

static uint32_t
antifake_coordinate_distance (int32_t prior_x,
                              int32_t prior_y,
                              int32_t transformed_x,
                              int32_t transformed_y)
{
  uint32_t dx = (uint32_t) prior_x * UINT32_C (0x100) -
                (uint32_t) transformed_x;
  uint32_t dy = (uint32_t) prior_y * UINT32_C (0x100) -
                (uint32_t) transformed_y;
  uint32_t distance = (uint32_t) goodix_milan_transform_sar32 (dx * dx, 8);

  distance += (uint32_t) goodix_milan_transform_sar32 (dy * dy, 8);
  return distance;
}

int
goodix_milan_antifake_pair_metrics (
  const GoodixMilanAntifakeBlob *prior,
  size_t                         prior_size,
  const GoodixMilanAntifakeBlob *current,
  size_t                         current_size,
  const int32_t                  current_to_prior[6],
  int32_t                        metrics[5])
{
  enum {
    rows = GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS,
    columns = GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS,
  };
  int32_t inverse[6];
  uint8_t overlap[rows * columns] = { 0 };
  int32_t transformed_x[GOODIX_MILAN_ANTIFAKE_RECORD_CAPACITY];
  int32_t transformed_y[GOODIX_MILAN_ANTIFAKE_RECORD_CAPACITY];
  int16_t current_to_prior_index[GOODIX_MILAN_ANTIFAKE_RECORD_CAPACITY];
  int16_t prior_to_current_index[GOODIX_MILAN_ANTIFAKE_RECORD_CAPACITY];
  uint8_t current_valid[GOODIX_MILAN_ANTIFAKE_RECORD_CAPACITY] = { 0 };
  int32_t distances[GOODIX_MILAN_ANTIFAKE_RECORD_CAPACITY];
  uint32_t prior_count;
  uint32_t current_count;
  size_t distance_count = 0;
  int32_t current_valid_count = 0;
  int32_t prior_valid_count = 0;

  if (!prior || prior_size < GOODIX_MILAN_ANTIFAKE_SIZE || !current ||
      current_size < GOODIX_MILAN_ANTIFAKE_SIZE || !current_to_prior ||
      !metrics)
    return -1;
  memset (metrics, 0, 5 * sizeof(*metrics));
  prior_count = goodix_milan_antifake_candidate_count (prior);
  current_count = goodix_milan_antifake_candidate_count (current);
  if (prior_count > GOODIX_MILAN_ANTIFAKE_RECORD_CAPACITY ||
      current_count > GOODIX_MILAN_ANTIFAKE_RECORD_CAPACITY)
    return -1;
  memset (current_to_prior_index, 0xff, sizeof(current_to_prior_index));
  memset (prior_to_current_index, 0xff, sizeof(prior_to_current_index));
  if (goodix_milan_transform_invert_s32_checked (
        current_to_prior, inverse) != 0)
    return -1;

  for (int32_t y = 0; y < rows; y++)
    for (int32_t x = 0; x < columns; x++)
      {
        size_t pixel = (size_t) y * columns + (size_t) x;
        int32_t source_x;
        int32_t source_y;

        if ((goodix_milan_antifake_const_mask (prior)[pixel / 8] &
             (1U << (pixel & 7))) == 0)
          continue;
        source_x = goodix_milan_transform_sar32 (
          (uint32_t) goodix_milan_transform_affine_s32 (
            inverse[0], (uint32_t) x, inverse[1], (uint32_t) y,
            (uint32_t) inverse[2]) + UINT32_C (0x80),
          8);
        source_y = goodix_milan_transform_sar32 (
          (uint32_t) goodix_milan_transform_affine_s32 (
            inverse[3], (uint32_t) x, inverse[4], (uint32_t) y,
            (uint32_t) inverse[5]) + UINT32_C (0x80),
          8);
        if (source_x >= 0 && source_x < columns && source_y >= 0 &&
            source_y < rows)
          {
            size_t source_pixel =
              (size_t) source_y * columns + (size_t) source_x;

            overlap[pixel] =
              (goodix_milan_antifake_const_mask (current)[source_pixel / 8] &
               (1U << (source_pixel & 7))) != 0;
          }
      }

  for (uint32_t current_index = 0; current_index < current_count;
       current_index++)
    {
      const uint8_t *current_record = goodix_milan_antifake_const_record (
        current, current_index);
      int32_t x = goodix_milan_antifake_record_x (current_record);
      int32_t y = goodix_milan_antifake_record_y (current_record);
      int32_t rounded_x;
      int32_t rounded_y;
      uint32_t best_distance = UINT32_MAX;
      int16_t best_index = -1;

      transformed_x[current_index] = goodix_milan_transform_affine_s32 (
        current_to_prior[0], (uint32_t) x, current_to_prior[1],
        (uint32_t) y, (uint32_t) current_to_prior[2]);
      transformed_y[current_index] = goodix_milan_transform_affine_s32 (
        current_to_prior[3], (uint32_t) x, current_to_prior[4],
        (uint32_t) y, (uint32_t) current_to_prior[5]);
      rounded_x = goodix_milan_transform_sar32 (
        (uint32_t) transformed_x[current_index] + UINT32_C (0x80), 8);
      rounded_y = goodix_milan_transform_sar32 (
        (uint32_t) transformed_y[current_index] + UINT32_C (0x80), 8);
      if (rounded_x < 0 || rounded_x >= columns || rounded_y < 0 ||
          rounded_y >= rows || !overlap[rounded_y * columns + rounded_x])
        continue;
      current_valid[current_index] = 1;
      current_valid_count++;
      for (uint32_t prior_index = 0; prior_index < prior_count; prior_index++)
        {
          const uint8_t *prior_record = goodix_milan_antifake_const_record (
            prior, prior_index);
          int32_t prior_x = goodix_milan_antifake_record_x (prior_record);
          int32_t prior_y = goodix_milan_antifake_record_y (prior_record);
          uint32_t candidate = antifake_coordinate_distance (
            prior_x, prior_y, transformed_x[current_index],
            transformed_y[current_index]);

          if (candidate < best_distance)
            {
              best_distance = candidate;
              best_index = (int16_t) prior_index;
            }
        }
      if (best_distance <= 0x400)
        current_to_prior_index[current_index] = best_index;
    }

  for (uint32_t prior_index = 0; prior_index < prior_count; prior_index++)
    {
      const uint8_t *prior_record = goodix_milan_antifake_const_record (
        prior, prior_index);
      int32_t prior_x = goodix_milan_antifake_record_x (prior_record);
      int32_t prior_y = goodix_milan_antifake_record_y (prior_record);
      uint32_t best_distance = UINT32_MAX;
      int16_t best_index = -1;

      if (prior_x < 0 || prior_x >= columns || prior_y < 0 || prior_y >= rows ||
          !overlap[prior_y * columns + prior_x])
        continue;
      prior_valid_count++;
      for (uint32_t current_index = 0; current_index < current_count;
           current_index++)
        {
          uint32_t candidate;

          if (!current_valid[current_index])
            continue;
          candidate = antifake_coordinate_distance (
            prior_x, prior_y, transformed_x[current_index],
            transformed_y[current_index]);
          if (candidate < best_distance)
            {
              best_distance = candidate;
              best_index = (int16_t) current_index;
            }
        }
      if (best_distance <= 0x400)
        prior_to_current_index[prior_index] = best_index;
    }

  for (uint32_t prior_index = 0; prior_index < prior_count; prior_index++)
    {
      int16_t current_index = prior_to_current_index[prior_index];

      if (current_index >= 0 &&
          current_to_prior_index[current_index] == (int16_t) prior_index)
        distances[distance_count++] = antifake_descriptor_distance (
          goodix_milan_antifake_const_record (prior, prior_index),
          goodix_milan_antifake_const_record (current,
                                               (size_t) current_index));
    }
  metrics[1] = (int32_t) distance_count;
  if (distance_count == 0)
    return 0;

  int32_t valid_total = prior_valid_count + current_valid_count;

  metrics[2] = valid_total == 0
                 ? (int32_t) distance_count << 13
                 : (valid_total / 2 + (int32_t) distance_count * 0x2000) /
                     valid_total;
  metrics[3] = prior_valid_count == 0
                 ? current_valid_count * 0x1000
                 : (prior_valid_count / 2 + current_valid_count * 0x1000) /
                     prior_valid_count;
  qsort (distances, distance_count, sizeof (distances[0]), compare_int32);
  metrics[4] = distances[(distance_count - 1) / 2];
  return 0;
}

int
goodix_milan_antifake_score_pair (
  const GoodixMilanAntifakeBlob *prior,
  size_t                         prior_size,
  const GoodixMilanAntifakeBlob *current,
  size_t                         current_size,
  const int32_t                  current_to_prior[6],
  int32_t                       *score)
{
  int32_t metrics[5];

  if (!score || goodix_milan_antifake_pair_metrics (
        prior, prior_size, current, current_size, current_to_prior,
        metrics) != 0)
    return -1;
  *score = metrics[1] == 0 ? -1 : metrics[4];
  return 0;
}
