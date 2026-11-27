/*
 * Goodix 53x5 driver for libfprint - profile-9/type-12 study policy
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "milan/capacity.h"

#define GOODIX_MILAN_STUDY_MASK_SIZE           (52 * 44)

typedef enum
{
  GOODIX_MILAN_STUDY_ACTION_NONE = 0,
  GOODIX_MILAN_STUDY_ACTION_APPEND = 1,
  GOODIX_MILAN_STUDY_ACTION_REPLACE_NO_RELATION = 2,
  GOODIX_MILAN_STUDY_ACTION_GEOMETRIC = 3,
  GOODIX_MILAN_STUDY_ACTION_REPLACE = 4,
} GoodixMilanStudyActionCode;

typedef struct
{
  int32_t active;
  int32_t quality;
  int32_t coverage;
  int32_t state;
  int32_t residual;
  int32_t overlap_count;
  int32_t uncovered_probe_residual;
  int32_t geometric_overlap_area;
  int32_t geometric_overlap_percent;
} GoodixMilanStudyPolicyFeature;

typedef struct
{
  int32_t action_gate;
  int32_t mode_enabled;
  int32_t replacement_enabled;
  int32_t probe_quality;
  int32_t probe_coverage;
  size_t feature_count;
  size_t maximum_features;
  size_t matched_feature_index;
  size_t reference_feature_index;
  int32_t retained_flag;
  int32_t template_counter;
  int32_t primary_transform_area;
  GoodixMilanStudyPolicyFeature features[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
} GoodixMilanStudyPolicyInput;

typedef struct
{
  GoodixMilanStudyActionCode action;
  size_t selected_feature_index;
  int primary_candidate;
} GoodixMilanStudyPolicyResult;

int
goodix_milan_study_policy_select (const GoodixMilanStudyPolicyInput *input,
                                  GoodixMilanStudyPolicyResult      *result);

void
goodix_milan_study_policy_expand_mask (
  const uint8_t packed[72],
  uint8_t       expanded[GOODIX_MILAN_STUDY_MASK_SIZE]);

int32_t
goodix_milan_study_policy_remove_footprint (
  uint8_t       mask[GOODIX_MILAN_STUDY_MASK_SIZE],
  const int32_t transform[6]);

int32_t
goodix_milan_study_policy_footprint_area (const int32_t transform[6]);

int32_t
goodix_milan_study_policy_full_footprint_area (const int32_t transform[6]);
