/*
 * Goodix 53x5 driver for libfprint - profile-9/type-12 contribution state
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/match/selection.h"
#include "milan/print.h"
#include "milan/relations.h"

#include <limits.h>
#include <string.h>

static int32_t
selection_u32_as_s32 (uint32_t value)
{
  if (value <= INT32_MAX)
    return (int32_t) value;
  return -1 - (int32_t) (UINT32_MAX - value);
}

static int32_t
selection_wrap_add (int32_t left,
                    int32_t right)
{
  return selection_u32_as_s32 ((uint32_t) left + (uint32_t) right);
}

static int32_t
selection_wrap_multiply (int32_t left,
                         int32_t right)
{
  return selection_u32_as_s32 ((uint32_t) left * (uint32_t) right);
}

static int32_t
selection_arithmetic_shift_8 (int32_t value)
{
  if (value >= 0)
    return value / 256;
  return -1 - (int32_t) ((uint32_t) (-(value + 1)) / 256);
}

static int
selection_signed_divide (int32_t  numerator,
                         int32_t  denominator,
                         int32_t *quotient)
{
  if (denominator == 0 || (numerator == INT32_MIN && denominator == -1))
    return -1;
  *quotient = numerator / denominator;
  return 0;
}

static int
selection_rank_better (const int32_t candidate[77],
                       const int32_t winner[77])
{
  static const int rank_fields[] = { 5, 1, 9 };

  for (size_t i = 0; i < sizeof(rank_fields) / sizeof(rank_fields[0]); i++)
    if (candidate[rank_fields[i]] != winner[rank_fields[i]])
      return candidate[rank_fields[i]] > winner[rank_fields[i]];
  return 0;
}

static int
selection_type12_blocked (int32_t texture_delta,
                          int32_t shape_delta,
                          int32_t boundary_delta,
                          int32_t boundary_score,
                          int32_t candidate_coverage,
                          int32_t pair_count,
                          int32_t pair_coverage,
                          int32_t support_ratio,
                          int32_t adjusted_detail)
{
  return (shape_delta > 415 && boundary_delta > 318) ||
         (shape_delta >= 358 && boundary_delta >= 581) ||
         (texture_delta >= 27 && shape_delta > 236 && boundary_delta > 315 &&
          boundary_score >= 95 && pair_count >= 6) ||
         (texture_delta >= 15 && shape_delta >= 295 && boundary_delta >= 320 &&
          boundary_score >= 93 && candidate_coverage >= 95 &&
          support_ratio >= 3200) ||
         (texture_delta >= 27 && shape_delta >= 350 && pair_count >= 29) ||
         (texture_delta >= 25 && shape_delta >= 370 && boundary_delta >= 450 &&
          boundary_score >= 88 && pair_count >= 10 && pair_coverage <= 1900) ||
         (texture_delta >= 90 && shape_delta >= 95 && boundary_delta >= 250 &&
          boundary_score >= 85 && pair_count >= 20 && adjusted_detail <= 77) ||
         (texture_delta >= 88 && shape_delta >= 130 && boundary_delta >= 280 &&
          candidate_coverage >= 80 && pair_count >= 3 && adjusted_detail <= 78 &&
          pair_coverage <= 1700 && support_ratio >= 2700) ||
         (texture_delta >= 33 && shape_delta >= 220 && boundary_score >= 90 &&
          candidate_coverage >= 71 && pair_count >= 2 && adjusted_detail <= 78 &&
          pair_coverage <= 1070 && support_ratio >= 3399) ||
         (texture_delta >= 80 && shape_delta >= 50 && boundary_delta >= 120 &&
          boundary_score >= 93 && candidate_coverage >= 100 && pair_count <= 12 &&
          adjusted_detail <= 75 && pair_coverage <= 920 && support_ratio >= 3990) ||
         (texture_delta >= 20 && shape_delta >= 290 && boundary_score >= 90 &&
          pair_count >= 7 && pair_coverage <= 1024) ||
         (texture_delta >= 70 && shape_delta >= 340 && boundary_delta >= 400 &&
          boundary_score >= 95) ||
         (texture_delta >= 54 && shape_delta >= 145 && boundary_delta >= 210 &&
          boundary_score <= 88 && candidate_coverage >= 150 && pair_count >= 21) ||
         (texture_delta >= 24 && boundary_delta >= 245 && boundary_score <= 88 &&
          pair_count >= 9 && support_ratio <= 930) ||
         (texture_delta >= 57 && shape_delta >= 147 && boundary_delta >= 229 &&
          boundary_score <= 88 && candidate_coverage >= 105 && pair_count >= 9 &&
          support_ratio >= 3750) ||
         (texture_delta >= 33 && shape_delta >= 100 && boundary_delta >= 230 &&
          boundary_score >= 89 && adjusted_detail >= 70 && pair_count >= 9 &&
          pair_coverage <= 1470 && support_ratio >= 4600) ||
         (texture_delta >= 40 && shape_delta <= -240 && boundary_delta <= -220 &&
          boundary_score <= 91 && adjusted_detail >= 70 && pair_count >= 10 &&
          pair_coverage <= 1402 && support_ratio <= 3670) ||
         (texture_delta >= 23 &&
          ((shape_delta >= 271 && boundary_delta >= 288 && boundary_score >= 92 &&
            candidate_coverage >= 163) ||
           (shape_delta >= 280 && boundary_delta >= 287 && boundary_score >= 90 &&
            candidate_coverage <= 161 && adjusted_detail >= 69 &&
            support_ratio >= 2800))) ||
         (boundary_delta >= 420 && pair_coverage <= 670 && support_ratio >= 1000) ||
         (shape_delta >= 380 && boundary_delta >= 480 && support_ratio <= 1000) ||
         (texture_delta >= 56 && shape_delta >= 220 && boundary_delta >= 289 &&
          candidate_coverage >= 141) ||
         (texture_delta >= 19 && shape_delta >= 280 && boundary_delta >= 350 &&
          boundary_score <= 95 && candidate_coverage >= 170) ||
         (texture_delta >= 40 && boundary_delta >= 480 && pair_count <= 0) ||
         (texture_delta >= 120 && shape_delta >= 100 && boundary_delta >= 270 &&
          boundary_score >= 87 && pair_count >= 8 && adjusted_detail <= 76) ||
         (texture_delta >= 30 && boundary_delta >= 420 && boundary_score >= 90 &&
          pair_coverage <= 1700 && support_ratio >= 2200) ||
         (texture_delta >= 45 && shape_delta >= 170 && boundary_delta >= 290 &&
          boundary_score >= 95 && pair_count >= 10) ||
         (texture_delta >= 20 && shape_delta >= 180 && boundary_delta >= 260 &&
          boundary_score >= 85 && pair_coverage <= 1000 &&
          support_ratio >= 3500);
}

static int
selection_q8_score (const GoodixMilanMatchSelection *selection,
                    int32_t                         *score)
{
  int32_t product;
  int32_t quotient;

  product = selection_wrap_multiply (selection->q8_sum, 100);
  if (selection_signed_divide (
        product, selection->q8_contributor_count, &quotient) != 0)
    return -1;
  *score = selection_arithmetic_shift_8 (quotient);
  return 0;
}

static int32_t
selection_q8_term (int32_t retained_count)
{
  uint32_t shifted = (uint32_t) retained_count << 8;
  int32_t numerator = selection_u32_as_s32 (shifted + (42 >> 1));

  return numerator / 42;
}

void
goodix_milan_active_relation_winner_reset (
  GoodixMilanActiveRelationWinner *winner)
{
  memset (winner, 0, sizeof(*winner));
  winner->feature = -1;
}

int
goodix_milan_active_relation_winner_would_update (
  const GoodixMilanActiveRelationWinner *winner,
  int32_t                                count)
{
  return winner && count > winner->count;
}

int
goodix_milan_active_relation_winner_update (
  GoodixMilanActiveRelationWinner *winner,
  int                              active,
  int32_t                          count,
  int32_t                          feature,
  const int32_t                    direct[6],
  const int32_t                    routed[6])
{
  if (!winner || !active || !direct || !routed ||
      !goodix_milan_active_relation_winner_would_update (winner, count))
    return 0;
  winner->count = count;
  winner->feature = feature;
  memcpy (winner->direct, direct, sizeof(winner->direct));
  memcpy (winner->routed, routed, sizeof(winner->routed));
  winner->valid = 1;
  return 1;
}

void
goodix_milan_match_selection_reset (GoodixMilanMatchSelection *selection)
{
  memset (selection, 0, sizeof(*selection));
  selection->winner_feature = -1;
  selection->selected_feature = -1;
}

int
goodix_milan_match_selection_contribute (
  GoodixMilanMatchSelection       *selection,
  const int32_t                    metrics[GOODIX_MILAN_MATCH_SELECTION_METRICS],
  const int32_t                    transform[6],
  size_t                           physical_feature,
  int32_t                          configuration_word_0,
  int32_t                          match_flag,
  int32_t                          candidate_flag,
  GoodixMilanMatchContributionEvent *event)
{
  int32_t threshold;
  int better;
  int replace;

  if (!selection || !metrics || !transform ||
      physical_feature >= GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT)
    return -1;
  if (event)
    memset (event, 0, sizeof(*event));

  threshold = selection_wrap_add (configuration_word_0, 207);
  if (candidate_flag != 1 &&
      (metrics[4] <= threshold || metrics[5] <= 195))
    return 0;

  selection->contributor_slots[physical_feature] = 1;
  int32_t term = selection_q8_term (metrics[1]);

  selection->q8_sum = selection_wrap_add (selection->q8_sum, term);
  selection->q8_contributor_count =
    selection_wrap_add (selection->q8_contributor_count, 1);

  better = !selection->winner_valid ||
           selection_rank_better (metrics, selection->winner_metrics);
  if (selection->rejection_evidence == 0 &&
      selection->acceptance_evidence == 0)
    replace = match_flag != 0 || candidate_flag != 0 || better;
  else if (selection->rejection_evidence == 0)
    replace = match_flag != 0 || (candidate_flag != 0 && better);
  else
    replace = match_flag != 0 && better;

  if (replace)
    {
      memcpy (selection->winner_metrics, metrics,
              sizeof(selection->winner_metrics));
      memcpy (selection->winner_transform, transform,
              sizeof(selection->winner_transform));
      selection->winner_feature = (int32_t) physical_feature;
      selection->winner_valid = 1;
    }
  if (event)
    {
      event->contributed = 1;
      event->winner_replaced = replace;
      event->q8_term = term;
      event->q8_prefix = selection->q8_sum;
      event->contributor_count = selection->q8_contributor_count;
    }
  return 1;
}

int
goodix_milan_match_selection_admit (
  GoodixMilanMatchSelection       *selection,
  const int32_t                    metrics[GOODIX_MILAN_MATCH_SELECTION_METRICS],
  const int32_t                    transform[6],
  size_t                           physical_feature,
  int32_t                          feature_active,
  int32_t                          match_flag,
  int32_t                          candidate_flag,
  GoodixMilanMatchContributionEvent *event)
{
  int32_t score;

  if (!selection || !metrics || !transform ||
      physical_feature >= GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT)
    return -1;
  if (event)
    {
      event->retained = 0;
      event->selected_replaced = 0;
      event->direct_published = 0;
      event->published_score = 0;
    }
  if (match_flag != 0)
    selection->rejection_evidence = 1;
  if (!((metrics[0] > 6 && metrics[5] > 208) || candidate_flag != 0))
    return 0;

  if (candidate_flag != 0)
    selection->acceptance_evidence = 1;
  if (feature_active != 0 && candidate_flag != 0)
    selection->retained_active_evidence = 1;
  else if (feature_active == 0)
    {
      size_t retained = selection->retained_evidence_count;

      if (retained >= GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT)
        return -1;
      selection->retained_evidence_feature_indices[retained] =
        (int32_t) physical_feature;
      memcpy (selection->retained_evidence_transforms[retained], transform,
              sizeof(selection->retained_evidence_transforms[retained]));
      selection->retained_evidence_count++;
    }
  if (event)
    event->retained = 1;

  if (candidate_flag == 0)
    return 1;

  if (metrics[1] > selection->selected_numerator)
    {
      selection->selected_feature = (int32_t) physical_feature;
      selection->selected_numerator = metrics[1];
      memcpy (selection->selected_transform, transform,
              sizeof(selection->selected_transform));
      if (event)
        event->selected_replaced = 1;
    }
  selection->direct_slots[physical_feature] = 1;
  selection->lifecycle_update_mask |= UINT64_C (1) << physical_feature;
  if (event)
    event->direct_published = 1;
  if (selection_q8_score (selection, &score) != 0)
    return -2;
  selection->latched_score = score;
  selection->score_latched = 1;
  if (event)
    event->published_score = score;
  return 1;
}

int
goodix_milan_match_selection_block_candidate (
  GoodixMilanMatchSelection       *selection,
  uint32_t                         type,
  const int32_t                    comparison_metrics[5],
  int32_t                          feature_texture_delta,
  int32_t                          feature_shape_delta,
  int32_t                          feature_boundary_delta,
  int32_t                          feature_boundary_score,
  int32_t                          candidate_coverage,
  GoodixMilanMatchContributionEvent *event)
{
  int32_t adjustment = -20;
  int32_t adjusted_detail;

  if (!selection || !comparison_metrics)
    return -1;
  if (event)
    {
      event->blocking_recorded = 0;
      event->blocking_metric = 0;
    }
  if (type != GOODIX_MILAN_PRINT_SENSOR_TYPE)
    return 0;

  if (feature_boundary_score != -1)
    {
      if (feature_boundary_score < 70)
        adjustment = -10;
      if (feature_boundary_score < 60 ||
          (feature_boundary_delta > 100 && feature_boundary_score < 75))
        adjustment = 0;
    }
  adjusted_detail = selection_wrap_add (comparison_metrics[4], adjustment);
  if (!selection_type12_blocked (
        feature_texture_delta, feature_shape_delta, feature_boundary_delta,
        feature_boundary_score, candidate_coverage, comparison_metrics[1],
        comparison_metrics[2], comparison_metrics[3], comparison_metrics[4]))
    return 0;

  selection->postloop_blocking_count =
    selection_wrap_add (selection->postloop_blocking_count, 1);
  selection->postloop_blocking_sum =
    selection_wrap_add (selection->postloop_blocking_sum, adjusted_detail);
  if (event)
    {
      event->blocking_recorded = 1;
      event->blocking_metric = adjusted_detail;
    }
  return 1;
}

int
goodix_milan_match_selection_blocking_override (
  const GoodixMilanMatchSelection *selection)
{
  int32_t threshold;

  if (!selection)
    return 0;
  if (selection->postloop_blocking_count > 5)
    {
      threshold = selection_wrap_multiply (
        selection->postloop_blocking_count, 60);
      if (selection->postloop_blocking_sum > threshold)
        return 1;
    }
  if (selection->postloop_blocking_count > 10)
    {
      threshold = selection_wrap_multiply (
        selection->postloop_blocking_count, 50);
      if (selection->postloop_blocking_sum > threshold)
        return 1;
    }
  return 0;
}

int
goodix_milan_match_selection_finalize (
  GoodixMilanMatchSelection       *selection,
  int32_t                          packed_mode,
  int32_t                          blocking_state,
  GoodixMilanMatchWinnerValidator validator,
  void                            *validator_data)
{
  int32_t validator_score = 0;
  int32_t score;

  if (!selection)
    return -1;
  if (selection->score_latched)
    return selection->latched_score > 0;
  if (!selection->winner_valid || selection->q8_contributor_count == 0 ||
      packed_mode != 0 || blocking_state != 0 || !validator ||
      validator (selection->winner_metrics, validator_data,
                 &validator_score) != 0 || validator_score <= 0)
    return 0;

  selection->lifecycle_update_mask =
    goodix_milan_match_selection_contributor_mask (selection);
  if (selection_q8_score (selection, &score) != 0)
    return -1;
  selection->latched_score = score;
  selection->score_latched = 1;
  return selection->latched_score > 0;
}

int
goodix_milan_match_selection_publish_fallback (
  GoodixMilanMatchSelection *selection,
  int32_t                    fallback_score,
  int32_t                    blocking_state,
  int32_t                   *published_score)
{
  if (!selection || !published_score)
    return -1;
  *published_score = fallback_score;
  if (fallback_score > 0)
    {
      selection->acceptance_evidence = 1;
      return 1;
    }
  if (blocking_state != 0)
    *published_score = -65536;
  return 0;
}

uint64_t
goodix_milan_match_selection_contributor_mask (
  const GoodixMilanMatchSelection *selection)
{
  uint64_t mask = 0;

  if (!selection)
    return 0;
  for (size_t i = 0; i < GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT; i++)
    if (selection->contributor_slots[i])
      mask |= UINT64_C(1) << i;
  return mask;
}

uint64_t
goodix_milan_match_selection_direct_mask (
  const GoodixMilanMatchSelection *selection)
{
  uint64_t mask = 0;

  if (!selection)
    return 0;
  for (size_t i = 0; i < GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT; i++)
    if (selection->direct_slots[i])
      mask |= UINT64_C(1) << i;
  return mask;
}

uint64_t
goodix_milan_match_selection_lifecycle_mask (
  const GoodixMilanMatchSelection *selection)
{
  return selection ? selection->lifecycle_update_mask : 0;
}
