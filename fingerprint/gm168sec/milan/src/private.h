/*
 * Goodix 53x5 driver for libfprint - Milan algorithm internals
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "milan/feature/feature.h"
#include "milan/milan.h"
#include "milan/match/correspondence.h"

enum { MILAN_FEATURE_MATERIALIZED_LIMIT = 300 };

static inline size_t
goodix_milan_reflect101_index (ptrdiff_t coordinate,
                               size_t    length)
{
  while (coordinate < 0 || (size_t) coordinate >= length)
    {
      if (coordinate < 0)
        coordinate = -coordinate;
      else
        {
          ptrdiff_t last = (ptrdiff_t) length - 1;

          coordinate = last - (coordinate - last);
        }
    }

  return (size_t) coordinate;
}
uint16_t feature_cordic (int32_t vertical,
                         int32_t *horizontal);
int16_t feature_atan2 (int32_t y,
                       int32_t x);
int feature_build_dense_orientation (const uint8_t *frame,
                                     size_t         rows,
                                     size_t         columns,
                                     size_t         radius,
                                     uint8_t       *orientation);
void goodix_milan_feature_mask_expand (const uint8_t packed[72],
                                uint8_t       mask[44 * 52]);
int goodix_milan_feature_base_maps_with_validity (
  const uint8_t *frame,
  size_t         rows,
  size_t         columns,
  uint8_t       *high_bitmap,
  uint8_t       *low_bitmap,
  uint8_t       *feature_mask,
  uint8_t       *inline_mask,
  uint8_t       *validity_mask);
void goodix_milan_feature_pack_inline_mask (const uint8_t *validity_mask,
                                            size_t         rows,
                                            size_t         columns,
                                            uint8_t       *inline_mask);
int goodix_milan_feature_extract_records_mode_masked (
  const uint8_t            *frame,
  size_t                    rows,
  size_t                    columns,
  GoodixMilanFeatureRecord *records,
  size_t                    capacity,
  size_t                   *record_count,
  size_t                   *zero_flag_count,
  int                       expand_records,
  const uint8_t            *broken_mask,
  uint8_t                  *validity_mask,
  int                       high_class);
int goodix_milan_template_patch_feature_scalar (uint8_t *feature_element,
                                size_t   feature_element_size,
                                uint8_t  tag,
                                int32_t  value);
int goodix_milan_template_read_feature_scalar (const uint8_t *feature_element,
                               size_t         feature_element_size,
                               uint8_t        tag,
                               int32_t       *value);
int goodix_milan_template_sort_type12_order (GoodixMilanUnpackedTemplate *unpacked);
int goodix_milan_template_reference_transform (
  const GoodixMilanUnpackedTemplate *unpacked,
  size_t                              feature_index,
  int                                 reverse_above_reference,
  int32_t                             transform[6]);
int goodix_milan_template_normalize_unpacked (
  GoodixMilanUnpackedTemplate *unpacked,
  uint8_t *feature_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  int32_t overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY]);
void feature_build_descriptor (
  const uint32_t              *magnitude,
  const int16_t               *orientation,
  size_t                       rows,
  size_t                       columns,
  int32_t                      descriptor_scale,
  const GoodixMilanFeatureAux *auxiliary,
  GoodixMilanFeatureRecord    *record);
size_t feature_finish_pretransform_records (
  const uint32_t              *magnitude,
  const int16_t               *orientation,
  size_t                       rows,
  size_t                       columns,
  GoodixMilanFeatureRecord    *records,
  size_t                       capacity,
  GoodixMilanFeatureRecord    *materialized,
  GoodixMilanFeatureRank      *ranks,
  const GoodixMilanFeatureAux *auxiliary,
  size_t                       count);
