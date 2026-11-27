/*
 * Goodix 53x5 driver for libfprint - profile-9/type-12 matcher policy
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <stdint.h>

typedef struct
{
  int32_t configuration[20];
} GoodixMilanMatcherPolicy;

typedef struct
{
  int32_t accumulated_high_class;
  int32_t probe_low_class;
  int32_t probe_primary_histogram_class;
} GoodixMilanMatcherLateContext;

enum
{
  GOODIX_MILAN_MATCHER_CONFIGURATION_OFFSET = 6,
  GOODIX_MILAN_MATCHER_TAIL_HAMMING_LIMIT_INDEX = 5,
};

void goodix_milan_matcher_policy_init (GoodixMilanMatcherPolicy *policy,
                                       int32_t                   packed_mode);

void goodix_milan_matcher_policy_evaluate (
  GoodixMilanMatcherPolicy      *policy,
  const int32_t                  metrics[77],
  int32_t                        image_quality,
  int32_t                        image_coverage,
  int32_t                        accumulated_high_class,
  int32_t                       *match_flag,
  int32_t                       *candidate_flag);

void goodix_milan_matcher_policy_apply_final (
  const int32_t metrics[77],
  int32_t       probe_coverage,
  int32_t       accumulated_high_class,
  int32_t       probe_low_class,
  int32_t       support_ratio_q8,
  int32_t      *match_flag,
  int32_t      *candidate_flag);

void goodix_milan_matcher_policy_apply_late_veto (
  const GoodixMilanMatcherPolicy *policy,
  const int32_t                   metrics[77],
  int32_t                        *match_flag,
  int32_t                        *candidate_flag);

void goodix_milan_matcher_late_context_init (
  GoodixMilanMatcherLateContext *context,
  int32_t                        packed_probe_c7,
  int32_t                        probe_primary_histogram_class);

void goodix_milan_matcher_late_context_derive (
  GoodixMilanMatcherLateContext *context,
  int32_t                        packed_feature_c7,
  int32_t                        state[3]);

int goodix_milan_matcher_policy_apply_status (
  const int32_t metrics[77],
  const int32_t transform[6],
  const int32_t state[3],
  int32_t       context_record_count,
  int32_t       image_quality,
  int32_t       image_coverage,
  int32_t      *match_flag,
  int32_t      *status_counter);
