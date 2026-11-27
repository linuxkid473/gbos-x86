/*
 * Goodix 53x5 driver for libfprint - Milan feature extraction
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

int goodix_milan_feature_base_maps (const uint8_t *frame,
                                    size_t         rows,
                                    size_t         columns,
                                    uint8_t       *high_bitmap,
                                    uint8_t       *low_bitmap,
                                    uint8_t       *feature_mask,
                                    uint8_t       *inline_mask);

int goodix_milan_feature_enhance (const uint8_t *frame,
                                  size_t         rows,
                                  size_t         columns,
                                  uint8_t       *orientation,
                                  uint8_t       *output);

int goodix_milan_feature_enhanced_bitmap (const uint8_t *enhanced,
                                          const uint8_t *feature_mask,
                                          size_t         rows,
                                          size_t         columns,
                                          uint8_t       *bitmap,
                                          uint8_t       *threshold);

void goodix_milan_feature_transform_record (uint8_t *record,
                                            int      reverse_bits);

size_t goodix_milan_feature_partition_records (uint8_t *records,
                                               size_t   record_count);

typedef struct
{
  int32_t x;
  int32_t y;
  int32_t scale;
  int32_t response;
} GoodixMilanFeatureExtremum;

#define EXTREMA_SCAN_BORDER 6
#define EXTREMA_FIRST_SCALE 1
#define FEATURE_EXTREMA_CURSOR_INIT \
  { EXTREMA_FIRST_SCALE, EXTREMA_SCAN_BORDER, EXTREMA_SCAN_BORDER }

/* Cursor order is scale, row, column. Keep the planes and dimensions unchanged
 * between calls; a zero return leaves the cursor exhausted. */
int feature_next_extremum (const uint16_t             *scales,
                           size_t                      rows,
                           size_t                      columns,
                           size_t                      cursor[3],
                           GoodixMilanFeatureExtremum *extremum);

size_t goodix_milan_feature_collect_extrema (const uint16_t             *scales,
                                             size_t                      rows,
                                             size_t                      columns,
                                             GoodixMilanFeatureExtremum *extrema,
                                             size_t                      capacity);

typedef struct
{
  int32_t x;
  int32_t y;
  int32_t scale;
  int32_t strength;
  int16_t refined_x;
  int16_t refined_y;
  int32_t scale_value;
} GoodixMilanFeatureCandidate;

int goodix_milan_feature_refine_extremum (const uint16_t              *scales,
                                          size_t                       rows,
                                          size_t                       columns,
                                          GoodixMilanFeatureCandidate *candidate,
                                          uint32_t                    *curvature);

typedef struct _GoodixMilanFeatureRecord GoodixMilanFeatureRecord;

struct _GoodixMilanFeatureRecord
{
  uint16_t foreground;
  int16_t  refined_x;
  int16_t  refined_y;
  int16_t  orientation;
  int32_t  strength;
  uint8_t  payload[44];
};

typedef struct
{
  int32_t strength;
  int32_t index;
} GoodixMilanFeatureRank;

typedef struct
{
  int32_t  x;
  int32_t  y;
  int32_t  scale_value;
  uint32_t peak;
  uint32_t selected_peak;
  uint16_t secondary_orientation;
  uint16_t reserved;
} GoodixMilanFeatureAux;

size_t goodix_milan_feature_collect_materialized (const uint8_t            *feature_source,
                                                  const uint16_t           *scales,
                                                  const uint32_t           *magnitude,
                                                  const int16_t            *orientation,
                                                  size_t                    rows,
                                                  size_t                    columns,
                                                  GoodixMilanFeatureRecord *records,
                                                  GoodixMilanFeatureRank   *ranks,
                                                  GoodixMilanFeatureAux    *auxiliary,
                                                  size_t                    capacity);

int goodix_milan_feature_should_retry_scale_space (size_t       materialized_count,
                                                   unsigned int pass_marker,
                                                   int          configured_retry);

void goodix_milan_feature_build_descriptor_samples (int32_t         center_x,
                                                    int32_t         center_y,
                                                    int32_t         descriptor_scale,
                                                    int16_t         feature_orientation,
                                                    const uint32_t *magnitude,
                                                    const int16_t  *orientation,
                                                    size_t          rows,
                                                    size_t          columns,
                                                    int32_t         samples[128]);

int goodix_milan_feature_extract_records_mode (const uint8_t            *frame,
                                               size_t                    rows,
                                               size_t                    columns,
                                               GoodixMilanFeatureRecord *records,
                                               size_t                    capacity,
                                               size_t                   *record_count,
                                               size_t                   *zero_flag_count,
                                               int                       expand_records);

int goodix_milan_feature_extract_records_mode_configured (const uint8_t            *frame,
                                                          size_t                    rows,
                                                          size_t                    columns,
                                                          GoodixMilanFeatureRecord *records,
                                                          size_t                    capacity,
                                                          size_t                   *record_count,
                                                          size_t                   *zero_flag_count,
                                                          int                       expand_records,
                                                          int                       configured_retry);
