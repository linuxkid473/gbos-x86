/*
 * Goodix 53x5 driver for libfprint - Milan match correspondence internals
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "milan/milan.h"

#define MILAN_MATCH_MAX_PAIRS 42
#define MILAN_MATCH_DESCRIPTOR_DISTANCE_LIMIT 23

typedef struct
{
  int32_t prior_index;
  int32_t current_index;
  int32_t best_distance;
  int32_t second_distance;
} MilanFeatureMatch;

typedef struct
{
  int32_t best_distance;
  int32_t second_distance;
  int32_t best_index;
} MilanMatchDistanceReduction;

size_t goodix_milan_match_feature_records (
  const GoodixMilanFeatureRecord *prior,
  size_t                          prior_count,
  const GoodixMilanFeatureRecord *current,
  size_t                          current_count,
  MilanFeatureMatch              matches[31]);

int goodix_milan_match_correspondences_partitioned (const GoodixMilanFeatureRecord    *enrolled_records,
                                                    size_t                             enrolled_record_count,
                                                    size_t                             enrolled_partition_count,
                                                    const GoodixMilanFeatureRecord    *probe_records,
                                                    size_t                             probe_record_count,
                                                    size_t                             probe_partition_count,
                                                    int32_t                           *pairs,
                                                    size_t                             pair_capacity,
                                                    size_t                            *pair_count,
                                                    const MilanMatchDistanceReduction *primary_reductions);

size_t goodix_milan_match_relaxed_correspondences (const GoodixMilanFeatureRecord *enrolled_records,
                                                   size_t                          enrolled_record_count,
                                                   size_t                          enrolled_partition_count,
                                                   const GoodixMilanFeatureRecord *probe_records,
                                                   size_t                          probe_record_count,
                                                   size_t                          probe_partition_count,
                                                   int32_t                         pairs[62],
                                                   MilanMatchDistanceReduction    *primary_reductions);

int goodix_milan_match_alternate_correspondences_internal (
  const GoodixMilanFeatureRecord *enrolled_records,
  size_t                          enrolled_record_count,
  size_t                          enrolled_partition,
  const GoodixMilanFeatureRecord *probe_records,
  size_t                          probe_record_count,
  size_t                          probe_partition,
  const int32_t                   primary_transform[6],
  int32_t                        *pairs,
  size_t                          pair_capacity,
  size_t                         *pair_count);

size_t goodix_milan_match_cross_class_correspondences (
  const GoodixMilanFeatureRecord *enrolled_records,
  size_t                          enrolled_record_count,
  size_t                          enrolled_partition_count,
  const GoodixMilanFeatureRecord *probe_records,
  size_t                          probe_record_count,
  size_t                          probe_partition_count,
  uint8_t                         enrolled_class,
  int32_t                         tail_hamming_limit,
  int32_t                        *pairs,
  size_t                          pair_capacity);

size_t goodix_milan_match_cross_class_alternate_correspondences (
  const GoodixMilanFeatureRecord *enrolled_records,
  size_t                          enrolled_record_count,
  size_t                          enrolled_partition_count,
  const GoodixMilanFeatureRecord *probe_records,
  size_t                          probe_record_count,
  size_t                          probe_partition_count,
  const int32_t                   primary_transform[6],
  int32_t                         sibling_tail_hamming_limit,
  int32_t                        *pairs,
  size_t                          pair_capacity);
