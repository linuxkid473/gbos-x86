/*
 * Goodix 53x5 driver for libfprint - Milan match geometry internals
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "milan/milan.h"

typedef struct
{
  int32_t affine[6];
  int32_t residual;
  int32_t filtered_count;
  size_t  selected_pair_count;
  int     attempted;
  int     valid;
} MilanMatchAffineState;

int goodix_milan_filter_recognition_pairs_internal (
  const GoodixMilanFeatureRecord *enrolled_records,
  const GoodixMilanFeatureRecord *probe_records,
  const int32_t                  *pairs,
  size_t                          match_count,
  int32_t                         best_affine[6],
  int                            *best_residual,
  uint8_t                        *output_mask,
  int                            *model_valid);

void goodix_milan_match_record_metrics_internal (
  const GoodixMilanFeatureRecord *enrolled_records,
  size_t                          enrolled_record_count,
  const GoodixMilanFeatureRecord *probe_records,
  size_t                          probe_record_count,
  const int32_t                   transform[6],
  int                             filtered_count,
  int32_t                        *topology_percent,
  int32_t                        *geometric_percent,
  int32_t                        *topology_bonus,
  int32_t                        *topology_distance_output,
  int32_t                        *valid_count_output,
  int32_t                        *matched_count_output);

void goodix_milan_match_affine_penalties (const int32_t transform[6],
                                   int32_t      *scale_penalty,
                                   int32_t      *orthogonality_penalty,
                                   int32_t      *strong_orthogonality_penalty);
void goodix_milan_match_affine_details (const int32_t transform[6],
                                 int32_t      *average_scale,
                                 int32_t      *absolute_dot_q16);

void goodix_milan_match_fit_affine_state (
  const GoodixMilanFeatureRecord *enrolled_records,
  const GoodixMilanFeatureRecord *probe_records,
  const int32_t                  *pairs,
  size_t                          pair_count,
  int                             attempted,
  MilanMatchAffineState          *state);

int goodix_milan_refine_record_similarity (
  const GoodixMilanFeatureRecord *enrolled_records,
  size_t                          enrolled_record_count,
  size_t                          enrolled_partition,
  const GoodixMilanFeatureRecord *probe_records,
  size_t                          probe_record_count,
  size_t                          probe_partition,
  const int32_t                   transform[6],
  int32_t                         search_radius,
  int32_t                         refined[6]);

int goodix_milan_match_post_admission_replaces (int32_t current_score,
                                          int32_t current_detail,
                                          int32_t alternate_score,
                                          int32_t alternate_detail);
