/*
 * Goodix 53x5 driver for libfprint - aggregate match rescue
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

#define GOODIX_MILAN_MATCH_RESCUE_METRICS       77
#define GOODIX_MILAN_MATCH_RESCUE_MASK_WIDTH    52
#define GOODIX_MILAN_MATCH_RESCUE_MASK_HEIGHT   44
#define GOODIX_MILAN_MATCH_RESCUE_MASK_STRIDE   7
#define GOODIX_MILAN_MATCH_RESCUE_MASK_SIZE     308

typedef struct
{
  uint32_t       type;
  int32_t        width;
  int32_t        height;
  int32_t        half_resolution;
  const uint8_t *source_mask;
  size_t         source_mask_size;
  size_t         source_stride;
  const int32_t *records;
  size_t         record_count;
  const int32_t *ordered_features;
  size_t         ordered_count;
  int32_t        incoming_score;
} GoodixMilanMatchRescueInput;

typedef struct
{
  int32_t eligible_count;
  int32_t total_area;
  int32_t weighted_metric5;
  int32_t average_count;
  int32_t weighted_metric8;
  int32_t large_increment_count;
  int32_t large_increment_threshold;
  int32_t score_denominator;
  int32_t best_count;
  int32_t best_feature;
  int32_t marginal_areas[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  size_t  marginal_area_count;
  int32_t score;
  int32_t selected_feature;
  int32_t selected_transform[6];
  int     set_acceptance;
  int     set_rejection;
  int     retain_transform;
} GoodixMilanMatchRescueResult;

int goodix_milan_match_rescue_caller_eligible (
  int32_t aggregate_candidate_count,
  int32_t disqualifying_count,
  int32_t rejection_evidence,
  uint32_t type);

int goodix_milan_match_rescue_common_gate (
  int32_t eligible_count,
  int32_t total_area,
  int32_t weighted_metric5,
  int32_t average_count,
  int32_t weighted_metric8,
  int32_t large_increment_count);

int goodix_milan_match_rescue_retain_transform (
  int32_t incoming_score,
  int32_t large_increment_count,
  uint32_t type,
  int32_t weighted_metric5,
  int32_t weighted_metric8);

int goodix_milan_match_rescue_evaluate (
  const GoodixMilanMatchRescueInput *input,
  GoodixMilanMatchRescueResult      *result);
