/*
 * Goodix 53x5 driver for libfprint - profile-9/type-12 contribution state
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

#define GOODIX_MILAN_MATCH_SELECTION_METRICS     77

typedef struct
{
  int contributed;
  int retained;
  int winner_replaced;
  int blocking_recorded;
  int selected_replaced;
  int direct_published;
  int32_t q8_term;
  int32_t q8_prefix;
  int32_t contributor_count;
  int32_t blocking_metric;
  int32_t published_score;
} GoodixMilanMatchContributionEvent;

typedef struct
{
  int32_t winner_metrics[GOODIX_MILAN_MATCH_SELECTION_METRICS];
  int32_t winner_transform[6];
  int32_t winner_feature;
  int32_t q8_sum;
  int32_t q8_contributor_count;
  uint8_t contributor_slots[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  uint8_t direct_slots[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  uint64_t lifecycle_update_mask;
  int32_t acceptance_evidence;
  int32_t rejection_evidence;
  int32_t retained_active_evidence;
  size_t retained_evidence_count;
  int32_t retained_evidence_feature_indices[
    GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  int32_t retained_evidence_transforms[
    GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT][6];
  int32_t postloop_blocking_count;
  int32_t postloop_blocking_sum;
  int32_t selected_feature;
  int32_t selected_numerator;
  int32_t selected_transform[6];
  int32_t latched_score;
  int winner_valid;
  int score_latched;
} GoodixMilanMatchSelection;

typedef int (*GoodixMilanMatchWinnerValidator) (
  const int32_t metrics[GOODIX_MILAN_MATCH_SELECTION_METRICS],
  void         *user_data,
  int32_t      *score);

void goodix_milan_match_selection_reset (
  GoodixMilanMatchSelection *selection);

int goodix_milan_match_selection_contribute (
  GoodixMilanMatchSelection       *selection,
  const int32_t                    metrics[GOODIX_MILAN_MATCH_SELECTION_METRICS],
  const int32_t                    transform[6],
  size_t                           physical_feature,
  int32_t                          configuration_word_0,
  int32_t                          match_flag,
  int32_t                          candidate_flag,
  GoodixMilanMatchContributionEvent *event);

int goodix_milan_match_selection_admit (
  GoodixMilanMatchSelection       *selection,
  const int32_t                    metrics[GOODIX_MILAN_MATCH_SELECTION_METRICS],
  const int32_t                    transform[6],
  size_t                           physical_feature,
  int32_t                          feature_active,
  int32_t                          match_flag,
  int32_t                          candidate_flag,
  GoodixMilanMatchContributionEvent *event);

int goodix_milan_match_selection_block_candidate (
  GoodixMilanMatchSelection       *selection,
  uint32_t                         type,
  const int32_t                    comparison_metrics[5],
  int32_t                          feature_texture_delta,
  int32_t                          feature_shape_delta,
  int32_t                          feature_boundary_delta,
  int32_t                          feature_boundary_score,
  int32_t                          candidate_coverage,
  GoodixMilanMatchContributionEvent *event);

int goodix_milan_match_selection_blocking_override (
  const GoodixMilanMatchSelection *selection);

int goodix_milan_match_selection_finalize (
  GoodixMilanMatchSelection       *selection,
  int32_t                          packed_mode,
  int32_t                          blocking_state,
  GoodixMilanMatchWinnerValidator validator,
  void                            *validator_data);

int goodix_milan_match_selection_publish_fallback (
  GoodixMilanMatchSelection *selection,
  int32_t                    fallback_score,
  int32_t                    blocking_state,
  int32_t                   *published_score);

uint64_t goodix_milan_match_selection_contributor_mask (
  const GoodixMilanMatchSelection *selection);

uint64_t goodix_milan_match_selection_direct_mask (
  const GoodixMilanMatchSelection *selection);

uint64_t goodix_milan_match_selection_lifecycle_mask (
  const GoodixMilanMatchSelection *selection);
