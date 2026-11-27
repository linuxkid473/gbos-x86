/*
 * Goodix 53x5 driver for libfprint - profile-9/type-12 study relations
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

#include "milan/milan.h"

typedef struct
{
  int32_t values[7];
} GoodixMilanRelationSlot;

typedef struct
{
  GoodixMilanRelationSlot slots[GOODIX_MILAN_TEMPLATE_RELATION_CAPACITY];
  int32_t row_bases[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  size_t feature_count;
  size_t reference_feature_index;
  int graph_established;
} GoodixMilanRelationMatrix;

typedef struct
{
  int32_t count;
  int32_t feature;
  int32_t direct[6];
  int32_t routed[6];
  int valid;
} GoodixMilanActiveRelationWinner;

void
goodix_milan_transform_normalize (int32_t transform[6]);

void
goodix_milan_transform_compose (const int32_t after[6],
                                const int32_t before[6],
                                int32_t       output[6]);

int
goodix_milan_transform_invert (const int32_t transform[6],
                                     int32_t       inverse[6]);

void
goodix_milan_transform_route (const int32_t stored[6],
                                    const int32_t direct[6],
                                    int32_t       feature,
                                    int32_t       reference,
                                    int32_t       routed[6]);

void
goodix_milan_active_relation_winner_reset (
  GoodixMilanActiveRelationWinner *winner);

int
goodix_milan_active_relation_winner_would_update (
  const GoodixMilanActiveRelationWinner *winner,
  int32_t                                count);

int
goodix_milan_active_relation_winner_update (
  GoodixMilanActiveRelationWinner *winner,
  int                              active,
  int32_t                          count,
  int32_t                          feature,
  const int32_t                    direct[6],
  const int32_t                    routed[6]);

int32_t
goodix_milan_relation_matrix_expected_row_base (size_t feature_index);

void
goodix_milan_relation_slot_unset (GoodixMilanRelationSlot *slot);

int
goodix_milan_relation_matrix_slot_index (
  const GoodixMilanRelationMatrix *matrix,
  size_t                                first,
  size_t                                second,
  int32_t                              *slot_index);

GoodixMilanRelationSlot *
goodix_milan_relation_matrix_slot (GoodixMilanRelationMatrix *matrix,
                                  int32_t                         slot_index);

int
goodix_milan_relation_matrix_init (
  GoodixMilanRelationMatrix   *matrix,
  size_t                            feature_count,
  const int32_t                    row_bases[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  uint32_t                          registration_count,
  int32_t                           reference_feature_index,
  int32_t                           graph_established,
  const GoodixMilanTemplateRelation *relations,
  size_t                            relation_count);

int
goodix_milan_relation_matrix_append_row (
  GoodixMilanRelationMatrix *matrix,
  int32_t                         row_base);

int
goodix_milan_relation_matrix_clear_incident (
  GoodixMilanRelationMatrix *matrix,
  size_t                          feature_index);

int
goodix_milan_relation_matrix_store_reference (
  GoodixMilanRelationMatrix *matrix,
  size_t                          feature_index,
  const int32_t                   feature_to_reference[6]);

int
goodix_milan_relation_matrix_refresh (
  GoodixMilanRelationMatrix *matrix,
  size_t                          feature_index,
  const int32_t                   retained_transform[6],
  const int32_t                   evidence_relation_transform[6]);

int
goodix_milan_relation_matrix_replace (
  GoodixMilanRelationMatrix *matrix,
  size_t                          feature_index,
  const int32_t                   new_feature_to_reference[6]);

int
goodix_milan_relation_matrix_close (
  GoodixMilanRelationMatrix *matrix,
  int32_t                         active_markers[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY]);

int
goodix_milan_relation_matrix_project_reference_star (
  const GoodixMilanRelationMatrix *matrix,
  GoodixMilanTemplateRelation          *relations,
  size_t                                relation_capacity,
  size_t                               *relation_count);
