/*
 * Goodix 53x5 driver for libfprint - aggregate match rescue
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/match/rescue.h"
#include "milan/print.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int32_t
rescue_arithmetic_half (int32_t value)
{
  if (value >= 0)
    return value / 2;
  return (int32_t) -((-(int64_t) value + 1) / 2);
}

static int32_t
rescue_mask_area (const uint8_t *mask,
                  size_t         size)
{
  int32_t area = 0;

  for (size_t i = 0; i < size; i++)
    {
      uint8_t value = mask[i];

      while (value != 0)
        {
          area += value & 1U;
          value >>= 1;
        }
    }
  return area < 20 ? 0 : area;
}

static void
rescue_remove_transform (uint8_t       *mask,
                         size_t         stride,
                         int32_t        width,
                         int32_t        height,
                         const int32_t  input_transform[6],
                         int32_t        half_resolution)
{
  int32_t transform[6];
  int64_t maximum_x = (int64_t) (width - 1) * 0x100;
  int64_t maximum_y = (int64_t) (height - 1) * 0x100;

  memcpy (transform, input_transform, sizeof(transform));
  if (half_resolution)
    {
      transform[2] = rescue_arithmetic_half (transform[2]);
      transform[5] = rescue_arithmetic_half (transform[5]);
    }

  for (int32_t y = 0; y < height; y++)
    for (int32_t x = 0; x < width; x++)
      {
        int64_t source_x = (int64_t) x * transform[0] +
                           (int64_t) y * transform[1] + transform[2];
        int64_t source_y = (int64_t) x * transform[3] +
                           (int64_t) y * transform[4] + transform[5];

        if (source_x >= 0 && source_x <= maximum_x &&
            source_y >= 0 && source_y <= maximum_y)
          mask[(size_t) y * stride + (size_t) x / 8] &=
            (uint8_t) ~(1U << (x & 7));
      }
}

static int32_t
rescue_score_denominator (uint32_t type)
{
  return type == GOODIX_MILAN_PRINT_SENSOR_TYPE || type == 13 ||
         type == 16 || type == 22 ? 42 : 31;
}

int
goodix_milan_match_rescue_caller_eligible (int32_t  aggregate_candidate_count,
                                           int32_t  disqualifying_count,
                                           int32_t  rejection_evidence,
                                           uint32_t type)
{
  return aggregate_candidate_count > 1 && rejection_evidence == 0 &&
         type != 11 && type != 21 && disqualifying_count == 0;
}

int
goodix_milan_match_rescue_common_gate (int32_t eligible_count,
                                       int32_t total_area,
                                       int32_t weighted_metric5,
                                       int32_t average_count,
                                       int32_t weighted_metric8,
                                       int32_t large_increment_count)
{
  return eligible_count > 1 && total_area != 0 && weighted_metric5 > 208 &&
         average_count > 6 && weighted_metric8 > 183 &&
         large_increment_count > 1;
}

int
goodix_milan_match_rescue_retain_transform (int32_t  incoming_score,
                                            int32_t  large_increment_count,
                                            uint32_t type,
                                            int32_t  weighted_metric5,
                                            int32_t  weighted_metric8)
{
  return incoming_score > 40 || large_increment_count > 2 ||
         ((type == 11 || type == 21) && weighted_metric5 > 216 &&
          weighted_metric8 > 202);
}

int
goodix_milan_match_rescue_evaluate (const GoodixMilanMatchRescueInput *input,
                                    GoodixMilanMatchRescueResult      *result)
{
  uint8_t *working = NULL;
  int32_t width;
  int32_t height;
  int32_t previous_area;
  int64_t weighted_metric5 = 0;
  int64_t weighted_metric8 = 0;
  int64_t total_area = 0;
  int64_t sum_counts = 0;
  int32_t best_count = 0;
  int32_t best_feature = -1;
  int32_t threshold;
  int scale;
  int status = -1;

  if (!input || !result || !input->source_mask || !input->records ||
      !input->ordered_features || input->width <= 0 || input->height <= 0 ||
      (input->half_resolution != 0 && input->half_resolution != 1) ||
      input->ordered_count > GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT)
    return -1;
  memset (result, 0, sizeof(*result));
  result->best_feature = -1;
  result->selected_feature = -1;
  width = input->half_resolution ? input->width / 2 : input->width;
  height = input->half_resolution ? input->height / 2 : input->height;
  if (width <= 0 || height <= 0 ||
      input->source_stride < ((size_t) width + 7) / 8 ||
      (size_t) height > SIZE_MAX / input->source_stride ||
      input->source_mask_size < (size_t) height * input->source_stride)
    return -1;

  working = malloc ((size_t) height * input->source_stride);
  if (!working)
    return -1;
  memcpy (working, input->source_mask,
          (size_t) height * input->source_stride);
  previous_area = rescue_mask_area (
    working, (size_t) height * input->source_stride);
  threshold = input->type == 11 || input->type == 21 ? 1100 : 1400;
  scale = input->half_resolution ? 4 : 1;

  for (size_t order = 0; order < input->ordered_count; order++)
    {
      int32_t feature = input->ordered_features[order];
      const int32_t *record;
      int32_t current_area;
      int32_t marginal_area;

      if (feature < 0 || (size_t) feature >= input->record_count)
        goto out;
      record = input->records +
               (size_t) feature * GOODIX_MILAN_MATCH_RESCUE_METRICS;
      if (record[1] <= 4 || record[5] <= 189)
        continue;
      if (record[1] > best_count)
        {
          best_count = record[1];
          best_feature = feature;
        }
      rescue_remove_transform (working, input->source_stride, width, height,
                               record + 15, input->half_resolution);
      current_area = rescue_mask_area (
        working, (size_t) height * input->source_stride);
      marginal_area = (previous_area - current_area) * scale;
      previous_area = current_area;
      if (result->marginal_area_count >=
          GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT)
        goto out;
      result->marginal_areas[result->marginal_area_count++] = marginal_area;
      result->eligible_count++;
      total_area += marginal_area;
      weighted_metric5 += (int64_t) marginal_area * record[5];
      weighted_metric8 += (int64_t) marginal_area * record[8];
      sum_counts += record[1];
      result->large_increment_count += marginal_area > threshold;
    }

  if (total_area < INT32_MIN || total_area > INT32_MAX ||
      weighted_metric5 < INT32_MIN || weighted_metric5 > INT32_MAX ||
      weighted_metric8 < INT32_MIN || weighted_metric8 > INT32_MAX ||
      sum_counts < INT32_MIN || sum_counts > INT32_MAX)
    goto out;
  result->total_area = (int32_t) total_area;
  result->weighted_metric5 = total_area != 0
                               ? (int32_t) (weighted_metric5 / total_area) : 0;
  result->average_count = result->eligible_count != 0
                            ? (int32_t) (sum_counts / result->eligible_count) : 0;
  result->weighted_metric8 = total_area != 0
                               ? (int32_t) (weighted_metric8 / total_area) : 0;
  result->large_increment_threshold = threshold;
  result->score_denominator = rescue_score_denominator (input->type);
  result->best_count = best_count;
  result->best_feature = best_feature;
  if (goodix_milan_match_rescue_common_gate (
        result->eligible_count, result->total_area, result->weighted_metric5,
        result->average_count, result->weighted_metric8,
        result->large_increment_count))
    {
      const int32_t *best_record;

      if (best_feature < 0 || (size_t) best_feature >= input->record_count)
        goto out;
      best_record = input->records +
                    (size_t) best_feature * GOODIX_MILAN_MATCH_RESCUE_METRICS;
      result->set_acceptance = 1;
      result->selected_feature = best_feature;
      result->score = best_count * 100 / result->score_denominator;
      if (goodix_milan_match_rescue_retain_transform (
            input->incoming_score, result->large_increment_count, input->type,
            result->weighted_metric5, result->weighted_metric8))
        {
          result->set_rejection = 1;
          result->retain_transform = 1;
          memcpy (result->selected_transform, best_record + 15,
                  sizeof(result->selected_transform));
        }
    }
  status = 0;

out:
  free (working);
  return status;
}
