/*
 * Goodix 53x5 driver for libfprint - rejection fallback finalization
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
#include <string.h>

#include "milan/capacity.h"

#define GOODIX_MILAN_MATCH_FALLBACK_PAIR_CAPACITY 31

typedef struct
{
  int32_t feature;
  int32_t enabled;
  size_t  pair_count;
  int32_t pairs[GOODIX_MILAN_MATCH_FALLBACK_PAIR_CAPACITY * 2];
} GoodixMilanMatchFallbackWorkspace;

typedef struct
{
  int32_t feature;
  int32_t filtered_count;
  int32_t overlap;
  int32_t raw_detail;
  int32_t coverage;
  int32_t adjusted_detail;
  int32_t transform[6];
} GoodixMilanMatchFallbackWinner;

typedef struct
{
  GoodixMilanMatchFallbackWorkspace
    workspaces[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  size_t workspace_count;
  int32_t geometry_seen;
  int32_t count_sum;
  int32_t best_adjusted_detail;
  GoodixMilanMatchFallbackWinner winner;
  int winner_valid;
} GoodixMilanMatchFallback;

void
goodix_milan_match_fallback_reset (GoodixMilanMatchFallback *fallback);

int
goodix_milan_match_fallback_store (
  GoodixMilanMatchFallback *fallback,
  int32_t                   feature,
  int32_t                   enabled,
  const int32_t             pairs[GOODIX_MILAN_MATCH_FALLBACK_PAIR_CAPACITY * 2],
  size_t                    pair_count);

int
goodix_milan_match_fallback_consider (
  GoodixMilanMatchFallback *fallback,
  int32_t                   feature,
  int32_t                   filtered_count,
  int32_t                   transform_classifier,
  int32_t                   overlap,
  int32_t                   overlap_threshold,
  int32_t                   raw_detail,
  int32_t                   coverage,
  int32_t                   penalty_enabled,
  int32_t                   average_scale,
  int32_t                   absolute_dot_q16,
  const int32_t             transform[6]);

int32_t
goodix_milan_match_fallback_rejection_score (
  const GoodixMilanMatchFallback *fallback);
