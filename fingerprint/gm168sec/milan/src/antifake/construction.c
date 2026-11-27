/*
 * Goodix 53x5 driver for libfprint - Milan anti-fake construction
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/antifake/antifake.h"
#include "milan/print.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int milan_antifake_collect_candidates (
  GoodixMilanAntifakeBlob *antifake,
  size_t                   antifake_size,
  const uint16_t          *source,
  size_t                   rows,
  size_t                   columns,
  int32_t                  x_adjustment);

static int milan_antifake_build_class (
  const uint16_t *calibration,
  const uint16_t *raw_frame,
  const uint8_t  *classification_plane,
  const uint8_t  *feature_mask,
  size_t          feature_mask_size,
  size_t          rows,
  size_t          columns,
  uint16_t        t_code,
  uint16_t        dac_high,
  uint16_t        dac_low,
  uint16_t        chip_type,
  int32_t         calibration_scalar,
  GoodixMilanAntifakeBlob *antifake,
  size_t                   antifake_size);

static int32_t
milan_antifake_sensor_offset (uint16_t t_code,
                              uint16_t dac_high,
                              uint16_t dac_low,
                              uint16_t chip_type)
{
  uint32_t difference = (uint32_t) dac_high - dac_low;
  uint32_t scale = 0x11b;

  if (chip_type == 0x11)
    {
      difference += 0x55;
      scale = 0x146;
    }

  return (int32_t) (difference * t_code * scale) / 1000;
}

int
goodix_milan_antifake_build (
  const uint16_t *calibration,
  const uint16_t *raw_frame,
  const uint8_t  *classification_plane,
  const uint8_t  *feature_mask,
  size_t          feature_mask_size,
  size_t          rows,
  size_t          columns,
  uint16_t        t_code,
  uint16_t        dac_high,
  uint16_t        dac_low,
  uint16_t        chip_type,
  int32_t         calibration_scalar,
  GoodixMilanAntifakeBlob *antifake,
  size_t                   antifake_size)
{
  return milan_antifake_build_class (
    calibration, raw_frame, classification_plane, feature_mask,
    feature_mask_size, rows, columns, t_code, dac_high, dac_low, chip_type,
    calibration_scalar, antifake, antifake_size);
}

static int
milan_antifake_build_class (
  const uint16_t                   *calibration,
  const uint16_t                   *raw_frame,
  const uint8_t                    *classification_plane,
  const uint8_t                    *feature_mask,
  size_t                            feature_mask_size,
  size_t                            rows,
  size_t                            columns,
  uint16_t                          t_code,
  uint16_t                          dac_high,
  uint16_t                          dac_low,
  uint16_t                          chip_type,
  int32_t                           calibration_scalar,
  GoodixMilanAntifakeBlob          *antifake,
  size_t                            antifake_size)
{
  uint16_t *residual = NULL;
  uint8_t *mask = NULL;
  uint8_t *classes = NULL;
  int32_t vector[51];
  int32_t threshold;
  int32_t texture;
  int32_t mean;
  int32_t variation;
  int32_t boundary;
  int32_t model;
  size_t count;
  int result = -1;

  if (!calibration || !raw_frame || !classification_plane || !feature_mask ||
      !antifake || antifake_size < GOODIX_MILAN_ANTIFAKE_SIZE || rows < 2 ||
      columns <= 4 ||
      columns > SIZE_MAX / rows)
    return -1;
  count = rows * columns;
  if (count > SIZE_MAX / sizeof(*residual))
    return -1;
  residual = malloc (count * sizeof(*residual));
  mask = malloc (count);
  classes = malloc (count);
  if (!residual || !mask || !classes)
    goto out;

  memset (goodix_milan_antifake_data (antifake), 0, sizeof(*antifake));
  if (goodix_milan_antifake_residual (
        calibration, raw_frame, rows, columns,
        milan_antifake_sensor_offset (t_code, dac_high, dac_low, chip_type),
        chip_type, residual) != 0 ||
      goodix_milan_antifake_build_mask (
        feature_mask, feature_mask_size, rows, columns, mask,
        goodix_milan_antifake_mask (antifake),
        GOODIX_MILAN_ANTIFAKE_MASK_SIZE) != 0 ||
      goodix_milan_antifake_impulse_filter (
        residual, rows, columns, &threshold) != 0)
    goto out;
  goodix_milan_antifake_set_calibration_scalar (antifake,
                                                 calibration_scalar);
  goodix_milan_antifake_set_threshold (antifake, threshold);

  if (milan_antifake_collect_candidates (
        antifake, antifake_size, residual, rows, columns,
        chip_type == GOODIX_MILAN_PRINT_SENSOR_TYPE ? 0 : -2) != 0)
    goto out;

  if (goodix_milan_antifake_statistics (
        residual, mask, rows, columns, &texture, &mean) != 0 ||
      goodix_milan_antifake_block_variation (
        residual, mask, rows, columns, 12, &variation) != 0 ||
      goodix_milan_antifake_class_map (
        classification_plane, mask, rows, columns, classes) != 0 ||
      goodix_milan_antifake_boundary_score (
        residual, classes, rows, columns, &boundary) != 0 ||
      goodix_milan_antifake_model_vector (
        residual, mask, rows, columns, 2, vector) != 0 ||
      goodix_milan_antifake_model_score (vector, &model) != 0)
    goto out;
  goodix_milan_antifake_set_texture (antifake, texture);
  goodix_milan_antifake_set_mean (antifake, mean);
  goodix_milan_antifake_set_variation (antifake, variation);
  goodix_milan_antifake_set_boundary (antifake, boundary);
  goodix_milan_antifake_set_model (antifake, model);

  if (chip_type == GOODIX_MILAN_PRINT_SENSOR_TYPE)
    {
      if (columns != 0 && rows > SIZE_MAX / columns)
        goto out;
      size_t packed_pixels = rows * columns;
      uint8_t *packed_mask = goodix_milan_antifake_mask (antifake);

      if (packed_pixels > GOODIX_MILAN_ANTIFAKE_MASK_SIZE * 8)
        goto out;

      memset (packed_mask, 0, GOODIX_MILAN_ANTIFAKE_MASK_SIZE);
      for (size_t i = 0; i < packed_pixels; i++)
        if (mask[i] != 0)
          packed_mask[i / 8] |= (uint8_t) (1U << (i & 7));
    }
  if (columns <= 4 ||
      goodix_milan_antifake_feature_update (
        antifake, antifake_size, residual, rows, columns - 4) != 0)
    goto out;
  result = 0;

out:
  free (classes);
  free (mask);
  free (residual);
  return result;
}

int
goodix_milan_antifake_build_with_boundary (
  const uint16_t                   *calibration,
  const uint16_t                   *raw_frame,
  const uint8_t                    *classification_plane,
  const uint8_t                    *feature_mask,
  size_t                            feature_mask_size,
  size_t                            rows,
  size_t                            columns,
  uint16_t                          t_code,
  uint16_t                          dac_high,
  uint16_t                          dac_low,
  uint16_t                          chip_type,
  int32_t                           calibration_scalar,
  GoodixMilanAntifakeBlob          *antifake,
  size_t                            antifake_size,
  GoodixMilanAntifakeBoundaryResult *boundary_result)
{
  GoodixMilanAntifakeBlob *alternate_antifake = NULL;
  uint8_t *alternate_feature_mask = NULL;
  int result;

  if (boundary_result)
    memset (boundary_result, 0, sizeof(*boundary_result));
  result = milan_antifake_build_class (
    calibration, raw_frame, classification_plane, feature_mask,
    feature_mask_size, rows, columns, t_code, dac_high, dac_low, chip_type,
    calibration_scalar, antifake, antifake_size);
  if (result != 0 || chip_type != GOODIX_MILAN_PRINT_SENSOR_TYPE ||
      rows != GOODIX_MILAN_SENSOR_ROWS ||
      columns != GOODIX_MILAN_SENSOR_COLUMNS ||
      feature_mask_size != 52 * 44)
    return result;

  alternate_antifake = malloc (sizeof(*alternate_antifake));
  alternate_feature_mask = malloc (feature_mask_size + 1);
  if (!alternate_antifake || !alternate_feature_mask)
    {
      result = -1;
      goto out;
    }
  memcpy (alternate_feature_mask, feature_mask, feature_mask_size);
  alternate_feature_mask[feature_mask_size] = 1;
  result = milan_antifake_build_class (
    calibration, raw_frame, classification_plane, alternate_feature_mask,
    feature_mask_size + 1, rows, columns, t_code, dac_high, dac_low, chip_type,
    calibration_scalar, alternate_antifake,
    GOODIX_MILAN_ANTIFAKE_DEFINED_MATERIAL_SIZE);
  if (result != 0)
    goto out;

  if (boundary_result)
    {
      memcpy (&boundary_result->zero_projection, antifake, sizeof(*antifake));
      memcpy (&boundary_result->nonzero_projection, alternate_antifake,
              sizeof(*alternate_antifake));
      boundary_result->zero_candidate_count =
        goodix_milan_antifake_candidate_count (antifake);
      boundary_result->nonzero_candidate_count =
        goodix_milan_antifake_candidate_count (alternate_antifake);
    }
  if (memcmp (goodix_milan_antifake_const_data (antifake),
              goodix_milan_antifake_const_data (alternate_antifake),
              sizeof(*antifake)) != 0)
    {
      if (boundary_result)
        boundary_result->classification =
          GOODIX_MILAN_ANTIFAKE_BOUNDARY_AMBIGUOUS;
      result = GOODIX_MILAN_ANTIFAKE_AMBIGUOUS;
      goto out;
    }
  if (boundary_result)
    boundary_result->classification = GOODIX_MILAN_ANTIFAKE_BOUNDARY_STABLE;

out:
  free (alternate_feature_mask);
  free (alternate_antifake);
  return result;
}

static int
milan_antifake_collect_candidates (
  GoodixMilanAntifakeBlob *antifake,
  size_t                   antifake_size,
  const uint16_t          *source,
  size_t                   rows,
  size_t                   columns,
  int32_t                  x_adjustment)
{
  static const int8_t offsets[44][2] = {
    { 1, 0 }, { 1, 1 }, { 0, 1 }, {-1, 1 }, {-1, 0 }, {-1,-1 },
    { 0,-1 }, { 1,-1 }, { 2, 0 }, { 2, 1 }, { 1, 2 }, { 0, 2 },
    {-1, 2 }, {-2, 1 }, {-2, 0 }, {-2,-1 }, {-1,-2 }, { 0,-2 },
    { 1,-2 }, { 2,-1 }, { 3, 0 }, { 3, 1 }, { 3, 2 }, { 2, 2 },
    { 2, 3 }, { 1, 3 }, { 0, 3 }, {-1, 3 }, {-2, 3 }, {-2, 2 },
    {-3, 2 }, {-3, 1 }, {-3, 0 }, {-3,-1 }, {-3,-2 }, {-2,-2 },
    {-2,-3 }, {-1,-3 }, { 0,-3 }, { 1,-3 }, { 2,-3 }, { 2,-2 },
    { 3,-2 }, { 3,-1 },
  };
  typedef struct
  {
    int32_t x;
    int32_t y;
    int32_t metric;
    int invalid;
  } AntifakeCandidate;
  AntifakeCandidate candidates[400];
  size_t candidate_count = 0;
  uint32_t output_count = 0;

  if (!antifake || !source || antifake_size < GOODIX_MILAN_ANTIFAKE_SIZE ||
      rows <= 8 ||
      columns <= 8 || columns > SIZE_MAX / rows)
    return -1;
  const uint8_t *mask = goodix_milan_antifake_const_mask (antifake);
  size_t cropped_columns = columns - 4;
  for (size_t y = 4; y + 4 < rows; y++)
    for (size_t x = 4; x + 4 < columns; x++)
      {
        size_t pixel = y * columns + x;
        uint16_t center = source[pixel];
        int32_t metric = 0;
        size_t mask_bit = y * cropped_columns + x - 2;
        int accepted =
          (mask[mask_bit >> 3] & (1U << (mask_bit & 7))) != 0;

        if (!accepted)
          continue;
        for (size_t i = 0; i < 44; i++)
          {
            uint16_t neighbor = source[(size_t) ((ptrdiff_t) y + offsets[i][1]) *
                                       columns +
                                       (size_t) ((ptrdiff_t) x + offsets[i][0])];

            if (center < neighbor)
              {
                accepted = 0;
                break;
              }
            metric += center - neighbor;
          }
        if (accepted && candidate_count < 400)
          candidates[candidate_count++] = (AntifakeCandidate) {
            (int32_t) x, (int32_t) y, metric, 0,
          };
      }
  for (size_t i = 0; i < candidate_count; i++)
    {
      if (candidates[i].invalid)
        continue;
      for (size_t j = i + 1; j < candidate_count; j++)
        {
          if (candidates[j].invalid)
            continue;
          int32_t delta_x = candidates[i].x - candidates[j].x;
          int32_t delta_y = candidates[i].y - candidates[j].y;

          if (abs (delta_x) <= 3 && abs (delta_y) <= 3 &&
              delta_x * delta_x + delta_y * delta_y < 10)
            {
              if (candidates[j].metric < candidates[i].metric)
                {
                  candidates[i].invalid = 1;
                  break;
                }
              candidates[j].invalid = 1;
            }
        }
    }
  memset (goodix_milan_antifake_data (antifake), 0,
          GOODIX_MILAN_ANTIFAKE_RECORDS_SIZE);
  for (size_t i = 0;
       i < candidate_count &&
       output_count < GOODIX_MILAN_ANTIFAKE_RECORD_CAPACITY;
       i++)
    if (!candidates[i].invalid)
      {
        uint8_t *record = goodix_milan_antifake_record (antifake,
                                                        output_count);

        goodix_milan_antifake_set_record_x (record, candidates[i].x);
        goodix_milan_antifake_set_record_y (record, candidates[i].y);
        memcpy (record + GOODIX_MILAN_ANTIFAKE_RECORD_DATA_24_OFFSET,
                &candidates[i].metric,
                sizeof(candidates[i].metric));
        output_count++;
      }
  for (size_t i = 0; i < output_count; i++)
    {
      int32_t x;
      uint8_t *record = goodix_milan_antifake_record (antifake, i);

      x = goodix_milan_antifake_record_x (record);
      x += x_adjustment;
      goodix_milan_antifake_set_record_x (record, x);
    }
  goodix_milan_antifake_set_candidate_count (antifake, output_count);
  return 0;
}
