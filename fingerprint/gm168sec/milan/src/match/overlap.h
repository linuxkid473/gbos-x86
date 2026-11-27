/*
 * Goodix 53x5 driver for libfprint - Milan match overlap internals
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "milan/milan.h"

int goodix_milan_match_queue_duplicate_score (const GoodixMilanFeatureView *newest,
                                              const GoodixMilanFeatureView *incoming,
                                              int32_t                      *score);

int goodix_milan_match_masked_classes (const uint8_t probe_inline_mask[72],
                                       const uint8_t classification[52 * 44],
                                       const int32_t transform[6],
                                       int32_t       counts[3]);

int goodix_milan_match_overlap_metrics_with_context (
  const GoodixMilanFeatureView *enrolled_feature,
  const GoodixMilanFeatureView *probe_feature,
  const int32_t                 transform[6],
  int32_t                      *overlap_score,
  int32_t                      *overlap_coverage,
  int32_t                      *overlap_detail,
  int32_t                       low_metrics[3],
  int32_t                       context_count);
