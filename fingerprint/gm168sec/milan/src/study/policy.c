/*
 * Goodix 53x5 driver for libfprint - profile-9/type-12 study policy
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/study/policy.h"

#include <limits.h>
#include <string.h>

static inline int32_t
study_policy_u32_as_s32 (uint32_t value)
{
  if (value <= INT32_MAX)
    return (int32_t) value;
  return -1 - (int32_t) (UINT32_MAX - value);
}

static inline int32_t
study_policy_wrap_add (int32_t left,
                       int32_t right)
{
  return study_policy_u32_as_s32 ((uint32_t) left + (uint32_t) right);
}

static inline int32_t
study_policy_wrap_subtract (int32_t left,
                            int32_t right)
{
  return study_policy_u32_as_s32 ((uint32_t) left - (uint32_t) right);
}

static inline int32_t
study_policy_wrap_multiply (int32_t left,
                            int32_t right)
{
  return study_policy_u32_as_s32 ((uint32_t) left * (uint32_t) right);
}

static inline int32_t
study_policy_wrap_shift_left (int32_t value,
                              unsigned int shift)
{
  return study_policy_u32_as_s32 ((uint32_t) value << shift);
}

static inline int32_t
study_policy_footprint_bound (int32_t dimension)
{
  return study_policy_wrap_subtract (
    study_policy_wrap_shift_left (dimension, 8), 0x100);
}

static inline int32_t
study_policy_transform_coordinate (int32_t first_coefficient,
                                   int32_t first_coordinate,
                                   int32_t second_coefficient,
                                   int32_t second_coordinate,
                                   int32_t offset)
{
  return study_policy_wrap_add (
    study_policy_wrap_add (
      study_policy_wrap_multiply (first_coefficient, first_coordinate),
      study_policy_wrap_multiply (second_coefficient, second_coordinate)),
    offset);
}

static inline int32_t
study_policy_percent_from_area (int32_t area)
{
  return study_policy_wrap_multiply (area, 100) /
         GOODIX_MILAN_STUDY_MASK_SIZE;
}

static inline int
goodix_milan_study_policy_feature_better (
  const GoodixMilanStudyPolicyFeature *candidate,
  int32_t                               best_residual,
  int32_t                               best_coverage,
  int32_t                               best_overlap_count)
{
  return candidate->residual < best_residual ||
         (candidate->residual == best_residual &&
          (candidate->coverage < best_coverage ||
           (candidate->coverage == best_coverage &&
            candidate->overlap_count > best_overlap_count)));
}

int
goodix_milan_study_policy_select (const GoodixMilanStudyPolicyInput *input,
                                  GoodixMilanStudyPolicyResult      *result)
{
  const GoodixMilanStudyPolicyFeature *matched;
  const GoodixMilanStudyPolicyFeature *selected = NULL;
  size_t selected_index = SIZE_MAX;
  int32_t best_residual = 0xfffff;
  int32_t best_coverage = 0xfffff;
  int32_t best_overlap_count = 0;

  if (!input || !result || input->feature_count == 0 ||
      input->feature_count > GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY ||
      input->maximum_features == 0 ||
      input->maximum_features > GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY ||
      input->matched_feature_index >= input->feature_count ||
      input->reference_feature_index >= input->feature_count)
    return -1;
  result->action = GOODIX_MILAN_STUDY_ACTION_NONE;
  result->selected_feature_index = SIZE_MAX;
  result->primary_candidate = 0;

  if (input->action_gate == 0 || input->mode_enabled == 0 ||
      input->probe_quality <= 15 || input->probe_coverage <= 65)
    return 0;
  if (input->feature_count < input->maximum_features)
    {
      result->action = GOODIX_MILAN_STUDY_ACTION_APPEND;
      result->selected_feature_index = input->feature_count;
      return 0;
    }
  if (input->feature_count != input->maximum_features ||
      input->replacement_enabled == 0)
    return 0;

  matched = &input->features[input->matched_feature_index];
  if (matched->quality >= 60 &&
      study_policy_wrap_multiply (input->probe_quality, 10) <=
        study_policy_wrap_multiply (matched->quality, 6))
    return 0;

  if (matched->residual == 0)
    {
      selected = matched;
      selected_index = input->matched_feature_index;
    }
  else
    {
      for (size_t i = 0; i < input->feature_count; i++)
        {
          const GoodixMilanStudyPolicyFeature *candidate = &input->features[i];

          if (candidate->active == 0 || i == input->reference_feature_index)
            continue;
          if (goodix_milan_study_policy_feature_better (
                candidate, best_residual, best_coverage,
                best_overlap_count))
            {
              selected = candidate;
              selected_index = i;
              best_residual = candidate->residual;
              best_coverage = candidate->coverage;
              best_overlap_count = candidate->overlap_count;
            }
        }
      if (selected && input->retained_flag != 0)
        {
          int32_t adjusted = selected->uncovered_probe_residual;

          if (input->template_counter > 700)
            adjusted = study_policy_wrap_add (adjusted, 20);
          if (adjusted < selected->residual)
            {
              selected = NULL;
              selected_index = SIZE_MAX;
            }
        }
      else if (selected)
        {
          size_t zero_count = 0;

          for (size_t i = 0; i < input->feature_count; i++)
            zero_count += input->features[i].active == 1 &&
                          input->features[i].residual == 0;
          if (selected->residual != 0 || zero_count < 3)
            {
              selected = NULL;
              selected_index = SIZE_MAX;
            }
        }
    }

  if (selected)
    result->primary_candidate = 1;
  else if (input->retained_flag == 0)
    {
      int32_t scaled_area = study_policy_wrap_multiply (
        input->primary_transform_area, 100);
      int32_t threshold = study_policy_wrap_multiply (
        study_policy_wrap_multiply (104, 88), 80);

      if (scaled_area <= threshold)
        return 0;
      selected_index = input->matched_feature_index;
    }
  else
    {
      int32_t best_area = INT32_MIN;

      for (size_t i = 0; i < input->feature_count; i++)
        if (input->features[i].active != 0 && input->features[i].state != 5 &&
            input->features[i].geometric_overlap_area > best_area)
          {
            selected = &input->features[i];
            selected_index = i;
            best_area = selected->geometric_overlap_area;
          }
      int32_t best_percent = selected
                               ? study_policy_percent_from_area (
                                   selected->geometric_overlap_area)
                               : 0;
      if (!selected ||
          (best_percent < 80 &&
           (best_percent < 60 || input->template_counter <= 1000)))
        return 0;
      if (input->retained_flag == 1)
        {
          result->action = GOODIX_MILAN_STUDY_ACTION_GEOMETRIC;
          result->selected_feature_index = selected_index;
          return 0;
        }
    }

  if (input->retained_flag != 1)
    {
      if (input->probe_coverage <
          study_policy_wrap_subtract (matched->coverage, 10))
        return 0;
      result->action = GOODIX_MILAN_STUDY_ACTION_REPLACE_NO_RELATION;
    }
  else
    result->action = GOODIX_MILAN_STUDY_ACTION_REPLACE;
  result->selected_feature_index = selected_index;
  return 0;
}

void
goodix_milan_study_policy_expand_mask (
  const uint8_t packed[72],
  uint8_t       expanded[GOODIX_MILAN_STUDY_MASK_SIZE])
{
  for (size_t y = 0; y < 22; y++)
    for (size_t x = 0; x < 26; x++)
      {
        size_t bit = y * 26 + x;
        uint8_t value = (packed[bit / 8] >> (bit & 7)) & 1U;

        expanded[(y * 2) * 52 + x * 2] = value;
        expanded[(y * 2) * 52 + x * 2 + 1] = value;
        expanded[(y * 2 + 1) * 52 + x * 2] = value;
        expanded[(y * 2 + 1) * 52 + x * 2 + 1] = value;
      }
}

int32_t
goodix_milan_study_policy_remove_footprint (
  uint8_t       mask[GOODIX_MILAN_STUDY_MASK_SIZE],
  const int32_t transform[6])
{
  int32_t removed = 0;
  const int32_t max_x = study_policy_footprint_bound (52);
  const int32_t max_y = study_policy_footprint_bound (44);

  for (int32_t y = 0; y < 44; y++)
    for (int32_t x = 0; x < 52; x++)
      {
        size_t index = (size_t) y * 52 + (size_t) x;
        int32_t mapped_x = study_policy_transform_coordinate (
          transform[0], x, transform[1], y, transform[2]);
        int32_t mapped_y = study_policy_transform_coordinate (
          transform[3], x, transform[4], y, transform[5]);

        if (mask[index] != 0 && mapped_x >= 0 && mapped_x <= max_x &&
            mapped_y >= 0 && mapped_y <= max_y)
          {
            mask[index] = 0;
            removed = study_policy_wrap_add (removed, 1);
          }
      }
  return removed;
}

int32_t
goodix_milan_study_policy_footprint_area (const int32_t transform[6])
{
  uint8_t mask[GOODIX_MILAN_STUDY_MASK_SIZE];

  memset (mask, 1, sizeof(mask));
  return goodix_milan_study_policy_remove_footprint (mask, transform);
}

int32_t
goodix_milan_study_policy_full_footprint_area (const int32_t transform[6])
{
  int32_t area = 0;
  const int32_t max_x = study_policy_footprint_bound (104);
  const int32_t max_y = study_policy_footprint_bound (88);

  for (int32_t y = 0; y < 88; y++)
    for (int32_t x = 0; x < 104; x++)
      {
        int32_t mapped_x = study_policy_transform_coordinate (
          transform[0], x, transform[1], y, transform[2]);
        int32_t mapped_y = study_policy_transform_coordinate (
          transform[3], x, transform[4], y, transform[5]);

        area = study_policy_wrap_add (
          area, mapped_x >= 0 && mapped_x <= max_x &&
                  mapped_y >= 0 && mapped_y <= max_y);
      }
  return area;
}
