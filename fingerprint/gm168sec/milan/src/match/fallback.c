/*
 * Goodix 53x5 driver for libfprint - rejection fallback finalization
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/match/fallback.h"

void
goodix_milan_match_fallback_reset (GoodixMilanMatchFallback *fallback)
{
  memset (fallback, 0, sizeof(*fallback));
  fallback->winner.feature = -1;
}

int
goodix_milan_match_fallback_store (
  GoodixMilanMatchFallback *fallback,
  int32_t                   feature,
  int32_t                   enabled,
  const int32_t             pairs[GOODIX_MILAN_MATCH_FALLBACK_PAIR_CAPACITY * 2],
  size_t                    pair_count)
{
  GoodixMilanMatchFallbackWorkspace *workspace;

  if (!fallback || !pairs ||
      fallback->workspace_count == GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT ||
      pair_count > GOODIX_MILAN_MATCH_FALLBACK_PAIR_CAPACITY)
    return -1;
  workspace = &fallback->workspaces[fallback->workspace_count++];
  workspace->feature = feature;
  workspace->enabled = enabled;
  workspace->pair_count = pair_count;
  memcpy (workspace->pairs, pairs, sizeof(workspace->pairs));
  return 0;
}

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
  const int32_t             transform[6])
{
  GoodixMilanMatchFallbackWinner current;

  if (!fallback || !transform)
    return -1;
  if (filtered_count <= 4 || transform_classifier != 0)
    return 0;

  fallback->geometry_seen = 1;
  if (overlap <= overlap_threshold || raw_detail <= 195)
    return 0;

  current = (GoodixMilanMatchFallbackWinner) {
    .feature = feature,
    .filtered_count = filtered_count,
    .overlap = overlap,
    .raw_detail = raw_detail,
    .coverage = coverage,
    .adjusted_detail = raw_detail,
  };
  memcpy (current.transform, transform, sizeof(current.transform));
  if (penalty_enabled)
    {
      current.adjusted_detail -=
        4 * ((uint32_t) (average_scale - 234) > 47);
      current.adjusted_detail -= 4 * (absolute_dot_q16 >= 0x147b);
    }

  fallback->count_sum += filtered_count;
  if (current.adjusted_detail > fallback->best_adjusted_detail)
    fallback->best_adjusted_detail = current.adjusted_detail;
  if (!fallback->winner_valid ||
      current.adjusted_detail > fallback->winner.adjusted_detail ||
      (current.adjusted_detail == fallback->winner.adjusted_detail &&
       current.filtered_count > fallback->winner.filtered_count) ||
      (current.adjusted_detail == fallback->winner.adjusted_detail &&
       current.filtered_count == fallback->winner.filtered_count &&
       current.coverage > fallback->winner.coverage))
    {
      fallback->winner = current;
      fallback->winner_valid = 1;
    }
  return 1;
}

int32_t
goodix_milan_match_fallback_rejection_score (
  const GoodixMilanMatchFallback *fallback)
{
  int32_t reason = (fallback->count_sum < 6) << 2;

  if (fallback->geometry_seen && fallback->best_adjusted_detail < 208)
    reason |= 2;
  if (fallback->geometry_seen && fallback->winner.coverage < 128)
    reason |= 1;
  return -reason;
}
