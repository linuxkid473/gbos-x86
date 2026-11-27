/*
 * Goodix 53x5 driver for libfprint - Milan match overlap
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/match/overlap.h"
#include "milan/preprocess/state.h"
#include "milan/private.h"
#include "milan/transform-private.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static int
milan_warp_inverse (const int32_t transform[6], int32_t inverse[6])
{
  uint64_t numerator[6];
  int32_t determinant = goodix_milan_transform_s32 (
    (uint32_t) transform[0] * (uint32_t) transform[4] -
    (uint32_t) transform[1] * (uint32_t) transform[3]);

  if (determinant == 0)
    {
      static const int32_t singular[6] = { 1, 0, 0, 0, 1, 0 };

      memcpy (inverse, singular, sizeof (singular));
      return 0;
    }
  /* Refinement can publish large affines. Native wraps before signed division,
   * then narrows the Q10 coefficients and every sampling accumulation. */
  numerator[0] = (uint64_t) (int64_t) transform[4] << 18;
  numerator[1] = (UINT64_C (0) - (uint64_t) (int64_t) transform[3]) << 18;
  numerator[2] =
    ((uint64_t) (int64_t) transform[5] * (uint64_t) (int64_t) transform[1] -
     (uint64_t) (int64_t) transform[4] * (uint64_t) (int64_t) transform[2]) << 10;
  numerator[3] = (UINT64_C (0) - (uint64_t) (int64_t) transform[1]) << 18;
  numerator[4] = (uint64_t) (int64_t) transform[0] << 18;
  numerator[5] =
    ((uint64_t) (int64_t) transform[3] * (uint64_t) (int64_t) transform[2] -
     (uint64_t) (int64_t) transform[5] * (uint64_t) (int64_t) transform[0]) << 10;
  for (size_t i = 0; i < 6; i++)
    {
      if (determinant == -1 && numerator[i] == (UINT64_C (1) << 63))
        return -1;
      inverse[i] = goodix_milan_transform_divide (numerator[i], determinant);
    }
  return 0;
}

static int
milan_packed_bitmap_bit (const uint8_t *bitmap,
                          size_t         columns,
                          size_t         x,
                          size_t         y)
{
  size_t index = y * columns + x;

  return (bitmap[index / 8] >> (index % 8)) & 1;
}

static int
milan_warp_sample (const uint8_t *bitmap,
                    size_t         rows,
                    size_t         columns,
                    int32_t        raw_x,
                    int32_t        raw_y)
{
  int32_t x = raw_x >> 10;
  int32_t y = raw_y >> 10;

  if (x + 1 < 0 || y + 1 < 0 || x >= (int32_t) columns ||
      y >= (int32_t) rows)
    return -1;
  int32_t fraction_x = raw_x - x * 0x400;
  int32_t fraction_y = raw_y - y * 0x400;
  if (x >= 0 && y >= 0 && x + 1 < (int32_t) columns &&
      y + 1 < (int32_t) rows)
    {
      int32_t top = milan_packed_bitmap_bit (bitmap, columns, x, y) *
                      (0x400 - fraction_x) +
                    milan_packed_bitmap_bit (bitmap, columns, x + 1, y) *
                      fraction_x;
      int32_t bottom = milan_packed_bitmap_bit (bitmap, columns, x, y + 1) *
                         (0x400 - fraction_x) +
                       milan_packed_bitmap_bit (bitmap, columns, x + 1, y + 1) *
                         fraction_x;

      return (top * (0x400 - fraction_y) + bottom * fraction_y +
              0x80000) >> 20;
    }

  int32_t sum = 0;
  int32_t count = 0;
  if (x >= 0 && y >= 0)
    {
      sum += milan_packed_bitmap_bit (bitmap, columns, (size_t) x,
                                      (size_t) y);
      count++;
    }
  if (x + 1 >= 0 && x + 1 < (int32_t) columns && y >= 0)
    {
      sum += milan_packed_bitmap_bit (bitmap, columns, (size_t) (x + 1),
                                      (size_t) y);
      count++;
    }
  if (y + 1 >= 0 && y + 1 < (int32_t) rows)
    {
      if (x >= 0)
        {
          sum += milan_packed_bitmap_bit (bitmap, columns, (size_t) x,
                                          (size_t) (y + 1));
          count++;
        }
      if (x + 1 >= 0 && x + 1 < (int32_t) columns)
        {
          sum += milan_packed_bitmap_bit (bitmap, columns,
                                          (size_t) (x + 1),
                                          (size_t) (y + 1));
          count++;
        }
    }
  return count > 0 ? sum / count : -1;
}

static int
milan_warp_sample_u8 (const uint8_t *image,
                       size_t         rows,
                       size_t         columns,
                       int32_t        raw_x,
                       int32_t        raw_y)
{
  int32_t x = raw_x >> 10;
  int32_t y = raw_y >> 10;

  if (x + 1 < 0 || y + 1 < 0 || x >= (int32_t) columns ||
      y >= (int32_t) rows)
    return -1;
  int32_t fraction_x = raw_x - x * 0x400;
  int32_t fraction_y = raw_y - y * 0x400;
  if (x >= 0 && y >= 0 && x + 1 < (int32_t) columns &&
      y + 1 < (int32_t) rows)
    {
      int32_t top = image[(size_t) y * columns + (size_t) x] *
                      (0x400 - fraction_x) +
                    image[(size_t) y * columns + (size_t) (x + 1)] *
                      fraction_x;
      int32_t bottom = image[(size_t) (y + 1) * columns + (size_t) x] *
                         (0x400 - fraction_x) +
                       image[(size_t) (y + 1) * columns +
                             (size_t) (x + 1)] * fraction_x;

      return (top * (0x400 - fraction_y) + bottom * fraction_y +
              0x80000) >> 20;
    }

  int32_t sum = 0;
  int32_t count = 0;
  for (int32_t dy = 0; dy <= 1; dy++)
    for (int32_t dx = 0; dx <= 1; dx++)
      {
        int32_t sample_x = x + dx;
        int32_t sample_y = y + dy;

        if (sample_x >= 0 && sample_x < (int32_t) columns && sample_y >= 0 &&
            sample_y < (int32_t) rows)
          {
            sum += image[(size_t) sample_y * columns + (size_t) sample_x];
            count++;
          }
      }
  return count > 0 ? sum / count : -1;
}

static int milan_build_overlap_mask (
  const uint8_t source_mask[44 * 52],
  const uint8_t target_mask[44 * 52],
  const int32_t transform[6],
  uint8_t       overlap_mask[44 * 52],
  size_t       *origin_x,
  size_t       *origin_y,
  size_t       *overlap_rows,
  size_t       *overlap_columns,
  int32_t       adjusted_transform[6]);

int
goodix_milan_match_bitmap_classes (
  const uint8_t *target_bitmap,
  const uint8_t *source_bitmap,
  const uint8_t *overlap_mask,
  size_t         bitmap_rows,
  size_t         bitmap_columns,
  size_t         overlap_x,
  size_t         overlap_y,
  size_t         overlap_rows,
  size_t         overlap_columns,
  const int32_t  transform[6],
  int32_t        classes[4],
  int32_t       *valid_count)
{
  int32_t inverse[6];

  if (!target_bitmap || !source_bitmap || !overlap_mask ||
      bitmap_rows == 0 || bitmap_columns == 0 || overlap_rows == 0 ||
      overlap_columns == 0 || overlap_rows > bitmap_rows ||
      overlap_columns > bitmap_columns || overlap_x > bitmap_columns ||
      overlap_y > bitmap_rows || overlap_columns > bitmap_columns - overlap_x ||
      overlap_rows > bitmap_rows - overlap_y || bitmap_rows > INT32_MAX ||
      bitmap_columns > INT32_MAX || !transform || !classes || !valid_count)
    return -1;
  if (milan_warp_inverse (transform, inverse) != 0)
    return -1;

  memset (classes, 0, 4 * sizeof(*classes));
  *valid_count = 0;
  for (size_t y = 0; y < overlap_rows; y++)
    for (size_t x = 0; x < overlap_columns; x++)
      {
        int32_t raw_x = goodix_milan_transform_affine_s32 (
          inverse[0], x, inverse[3], y, inverse[2]);
        int32_t raw_y = goodix_milan_transform_affine_s32 (
          inverse[1], x, inverse[4], y, inverse[5]);
        int source = milan_warp_sample (
          source_bitmap, bitmap_rows, bitmap_columns, raw_x, raw_y);

        if (source < 0 || !overlap_mask[y * overlap_columns + x])
          continue;
        int target = milan_packed_bitmap_bit (
          target_bitmap, bitmap_columns, x + overlap_x, y + overlap_y);

        if (source < 2 && target < 2)
          {
            classes[target + source * 2]++;
            (*valid_count)++;
          }
      }
  return 0;
}

int
goodix_milan_feature_mask_forward_overlap (
  const GoodixMilanFeatureView *first_feature,
  const GoodixMilanFeatureView *second_feature,
  const int32_t                 transform[6],
  int32_t                       half_resolution,
  int32_t                      *overlap,
  int32_t                      *overlap_count)
{
  uint8_t first_mask[GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS];
  uint8_t second_mask[GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS];
  size_t scale;
  size_t rows;
  size_t columns;
  int32_t translation_x;
  int32_t translation_y;
  int32_t count = 0;

  if (!first_feature || !second_feature || !first_feature->inline_mask ||
      !second_feature->inline_mask || !transform || !overlap ||
      !overlap_count || (half_resolution != 0 && half_resolution != 1))
    return -1;
  scale = half_resolution ? 2 : 4;
  rows = 22 * scale;
  columns = 26 * scale;
  for (size_t packed_y = 0; packed_y < 22; packed_y++)
    for (size_t packed_x = 0; packed_x < 26; packed_x++)
      {
        size_t bit = packed_y * 26 + packed_x;
        uint8_t first = (first_feature->inline_mask[bit / 8] >>
                         (bit & 7)) & 1;
        uint8_t second = (second_feature->inline_mask[bit / 8] >>
                          (bit & 7)) & 1;

        for (size_t y = 0; y < scale; y++)
          for (size_t x = 0; x < scale; x++)
            {
              size_t output_y = packed_y * scale + y;
              size_t output_x = packed_x * scale + x;

              first_mask[output_y * columns + output_x] = first;
              second_mask[output_y * columns + output_x] = second;
            }
      }
  translation_x = transform[2];
  translation_y = transform[5];
  if (half_resolution)
    {
      translation_x =
        goodix_milan_transform_s32 ((uint32_t) translation_x + 1) / 2;
      translation_y =
        goodix_milan_transform_s32 ((uint32_t) translation_y + 1) / 2;
    }
  for (size_t y = 0; y < rows; y++)
    for (size_t x = 0; x < columns; x++)
      {
        int32_t mapped_x;
        int32_t mapped_y;

        if (!first_mask[y * columns + x])
          continue;
        mapped_x = goodix_milan_transform_affine_s32 (
          transform[0], x, transform[1], y, (uint32_t) translation_x + 0x80) >> 8;
        mapped_y = goodix_milan_transform_affine_s32 (
          transform[3], x, transform[4], y, (uint32_t) translation_y + 0x80) >> 8;
        if (mapped_x >= 0 && mapped_x < (int32_t) columns &&
            mapped_y >= 0 && mapped_y < (int32_t) rows &&
            second_mask[(size_t) mapped_y * columns + (size_t) mapped_x])
          count++;
      }
  *overlap_count = half_resolution ? count * 4 : count;
  *overlap = (count * 0x100 + (int32_t) (rows * columns / 2)) /
             (int32_t) (rows * columns);
  return 0;
}

static int
milan_build_overlap_mask (const uint8_t source_mask[44 * 52],
                           const uint8_t target_mask[44 * 52],
                           const int32_t transform[6],
                           uint8_t       overlap_mask[44 * 52],
                           size_t       *origin_x,
                           size_t       *origin_y,
                           size_t       *overlap_rows,
                           size_t       *overlap_columns,
                           int32_t       adjusted_transform[6])
{
  static const int32_t corners[4][2] = {
    { 0, 0 }, { 51, 0 }, { 51, 43 }, { 0, 43 },
  };
  int32_t minimum_x = INT32_MAX;
  int32_t minimum_y = INT32_MAX;
  int32_t maximum_x = INT32_MIN;
  int32_t maximum_y = INT32_MIN;
  int32_t inverse[6];

  for (size_t i = 0; i < 4; i++)
    {
      int32_t x = goodix_milan_transform_affine_s32 (
        transform[0], corners[i][0], transform[1], corners[i][1],
        (uint32_t) transform[2] + 0x80) >> 8;
      int32_t y = goodix_milan_transform_affine_s32 (
        transform[3], corners[i][0], transform[4], corners[i][1],
        (uint32_t) transform[5] + 0x80) >> 8;

      if (x < minimum_x)
        minimum_x = x;
      if (x > maximum_x)
        maximum_x = x;
      if (y < minimum_y)
        minimum_y = y;
      if (y > maximum_y)
        maximum_y = y;
    }
  if (minimum_x < 0)
    minimum_x = 0;
  if (minimum_y < 0)
    minimum_y = 0;
  if (maximum_x > 51)
    maximum_x = 51;
  if (maximum_y > 43)
    maximum_y = 43;
  if (maximum_x < minimum_x || maximum_y < minimum_y)
    return -1;
  *origin_x = (size_t) minimum_x;
  *origin_y = (size_t) minimum_y;
  *overlap_columns = (size_t) (maximum_x - minimum_x + 1);
  *overlap_rows = (size_t) (maximum_y - minimum_y + 1);
  memcpy (adjusted_transform, transform, 6 * sizeof(*transform));
  adjusted_transform[2] = goodix_milan_transform_s32 (
    (uint32_t) transform[2] - (uint32_t) minimum_x * 0x100);
  adjusted_transform[5] = goodix_milan_transform_s32 (
    (uint32_t) transform[5] - (uint32_t) minimum_y * 0x100);

  if (milan_warp_inverse (adjusted_transform, inverse) != 0)
    return -1;

  for (size_t y = 0; y < *overlap_rows; y++)
    for (size_t x = 0; x < *overlap_columns; x++)
      {
        int32_t raw_x = goodix_milan_transform_affine_s32 (
          inverse[0], x, inverse[3], y, inverse[2]);
        int32_t raw_y = goodix_milan_transform_affine_s32 (
          inverse[1], x, inverse[4], y, inverse[5]);
        size_t target_x = x + *origin_x;
        size_t target_y = y + *origin_y;
        int source = milan_warp_sample_u8 (
          source_mask, 44, 52, raw_x, raw_y);

        overlap_mask[y * *overlap_columns + x] =
          source > 0 && target_mask[target_y * 52 + target_x] ? 1 : 0;
      }
  return 0;
}

static int
milan_match_low_bitmap_compute (
  const uint8_t enrolled_bitmap[286],
  const uint8_t enrolled_inline_mask[72],
  const uint8_t probe_bitmap[286],
  const uint8_t probe_inline_mask[72],
  const int32_t transform[6],
  int32_t       geometry[4],
  int32_t       classes[5],
  int32_t       matrix_sizes[4],
  uint8_t       matrices[4][2288])
{
  uint8_t source_mask[44 * 52];
  uint8_t target_mask[44 * 52];
  uint8_t overlap_mask[44 * 52];
  int32_t half_transform[6];
  int32_t adjusted_transform[6];
  int32_t inverse[6];
  size_t origin_x;
  size_t origin_y;
  size_t overlap_rows;
  size_t overlap_columns;

  if (!enrolled_bitmap || !enrolled_inline_mask || !probe_bitmap ||
      !probe_inline_mask || !transform || !classes ||
      ((geometry != NULL) != (matrix_sizes != NULL)) ||
      ((geometry != NULL) != (matrices != NULL)))
    return -1;
  if (matrices)
    memset (matrices, 0, 4 * 2288);
  memcpy (half_transform, transform, sizeof(half_transform));
  half_transform[2] =
    goodix_milan_transform_s32 ((uint32_t) half_transform[2] + 1) / 2;
  half_transform[5] =
    goodix_milan_transform_s32 ((uint32_t) half_transform[5] + 1) / 2;
  goodix_milan_feature_mask_expand (probe_inline_mask, source_mask);
  goodix_milan_feature_mask_expand (enrolled_inline_mask, target_mask);
  if (milan_build_overlap_mask (
        source_mask, target_mask, half_transform, overlap_mask, &origin_x,
        &origin_y, &overlap_rows, &overlap_columns, adjusted_transform) != 0)
    return -1;
  if (milan_warp_inverse (adjusted_transform, inverse) != 0)
    return -1;

  if (matrices)
    for (size_t i = 0; i < 44 * 52; i++)
      {
        matrices[0][i] = (uint8_t) milan_packed_bitmap_bit (
          enrolled_bitmap, 52, i % 52, i / 52);
        matrices[2][i] = target_mask[i];
      }
  memset (classes, 0, 5 * sizeof(*classes));
  for (size_t y = 0; y < overlap_rows; y++)
    for (size_t x = 0; x < overlap_columns; x++)
      {
        size_t index = y * overlap_columns + x;
        int32_t raw_x = goodix_milan_transform_affine_s32 (
          inverse[0], x, inverse[3], y, inverse[2]);
        int32_t raw_y = goodix_milan_transform_affine_s32 (
          inverse[1], x, inverse[4], y, inverse[5]);
        int source = milan_warp_sample (
          probe_bitmap, 44, 52, raw_x, raw_y);
        int source_valid = milan_warp_sample_u8 (
          source_mask, 44, 52, raw_x, raw_y);
        int target = milan_packed_bitmap_bit (
          enrolled_bitmap, 52, x + origin_x, y + origin_y);

        if (matrices)
          {
            matrices[1][index] = source < 0 ? UINT8_MAX : (uint8_t) source;
            matrices[3][index] =
              source_valid < 0 ? UINT8_MAX : (uint8_t) source_valid;
          }
        if (source >= 0 && source < 2 && target >= 0 && target < 2 &&
            source_valid > 0 &&
            target_mask[(y + origin_y) * 52 + x + origin_x] != 0)
          {
            classes[target + source * 2]++;
            classes[4]++;
          }
      }
  if (geometry)
    {
      geometry[0] = (int32_t) origin_x;
      geometry[1] = (int32_t) origin_y;
      geometry[2] = (int32_t) overlap_columns;
      geometry[3] = (int32_t) overlap_rows;
      matrix_sizes[0] = 44 * 52;
      matrix_sizes[1] = (int32_t) (overlap_rows * overlap_columns);
      matrix_sizes[2] = 44 * 52;
      matrix_sizes[3] = (int32_t) (overlap_rows * overlap_columns);
    }
  return 0;
}

int
goodix_milan_match_queue_duplicate_score (const GoodixMilanFeatureView *newest,
                                          const GoodixMilanFeatureView *incoming,
                                          int32_t                      *score)
{
  static const int32_t identity[6] = { 0x100, 0, 0, 0, 0x100, 0 };
  static const int32_t weights[3] = { 3, 2, 3 };
  int32_t confidence = -1;

  if (!newest || !incoming || !score)
    return -1;
  for (size_t pass = 0; pass < 2; pass++)
    {
      int32_t classes[5];
      int32_t pass_score;
      int32_t detail;

      if (milan_match_low_bitmap_compute (
            pass ? newest->enhanced_bitmap : newest->high_bitmap,
            newest->inline_mask,
            pass ? incoming->enhanced_bitmap : incoming->high_bitmap,
            incoming->inline_mask, identity, NULL, classes, NULL, NULL) != 0 ||
          goodix_milan_match_overlap_result (
            classes, classes[4], 52 * 44, 0, weights, 10, &confidence,
            &pass_score, NULL, &detail) != 0)
        return -1;
      if (pass == 0)
        *score = pass_score;
      else
        *score = goodix_milan_match_secondary_result (*score, pass_score, detail);
    }
  return 0;
}

int
goodix_milan_match_low_bitmap_metrics (
  const uint8_t enrolled_bitmap[286],
  const uint8_t enrolled_inline_mask[72],
  const uint8_t probe_bitmap[286],
  const uint8_t probe_inline_mask[72],
  const int32_t transform[6],
  int32_t       metrics[3])
{
  int32_t classes[5];

  if (!metrics || milan_match_low_bitmap_compute (
        enrolled_bitmap, enrolled_inline_mask, probe_bitmap, probe_inline_mask,
        transform, NULL, classes, NULL, NULL) != 0)
    return -1;

  metrics[0] = classes[4] > 0 ?
               (classes[4] / 2 + (classes[0] + classes[3]) * 0x100) /
               classes[4] :
               0;
  int32_t zero_total = classes[0] + classes[1] + classes[2];
  int32_t one_total = classes[1] + classes[2] + classes[3];
  metrics[1] = zero_total > 0
                 ? (zero_total / 2 + classes[0] * 0x100) / zero_total
                 : 0;
  metrics[2] = one_total > 0
                 ? (one_total / 2 + classes[3] * 0x100) / one_total
                 : 0;
  return 0;
}

int
goodix_milan_match_masked_classes (
  const uint8_t probe_inline_mask[72],
  const uint8_t classification[52 * 44],
  const int32_t transform[6],
  int32_t       counts[3])
{
  uint8_t source_valid[52 * 44];
  uint8_t direct_valid[52 * 44];
  uint8_t overlap_mask[52 * 44];
  uint8_t binary[286] = { 0 };
  int32_t half_transform[6];
  int32_t adjusted_transform[6];
  int32_t classes[4];
  int32_t valid_count;
  size_t origin_x, origin_y, rows, columns;

  if (!probe_inline_mask || !classification || !transform || !counts)
    return -1;
  goodix_milan_feature_mask_expand (probe_inline_mask, source_valid);
  memcpy (direct_valid, source_valid, sizeof (direct_valid));
  for (size_t i = 0; i < 52 * 44; i++)
    {
      if (classification[i] == 2)
        binary[i / 8] |= 1u << (i % 8);
      else if (classification[i] != 1)
        direct_valid[i] = 0;
    }
  /* Native leaves invalid-class temporary pixels unwritten. Initialize those
   * unspecified values to zero, but do not clear the earlier warped validity.
   * Both binary images and the direct validity belong to the probe. */
  memcpy (half_transform, transform, sizeof (half_transform));
  half_transform[2] =
    goodix_milan_transform_s32 ((uint32_t) half_transform[2] + 1) / 2;
  half_transform[5] =
    goodix_milan_transform_s32 ((uint32_t) half_transform[5] + 1) / 2;
  if (milan_build_overlap_mask (
        source_valid, direct_valid, half_transform, overlap_mask, &origin_x,
        &origin_y, &rows, &columns, adjusted_transform) != 0 ||
      goodix_milan_match_bitmap_classes (
        binary, binary, overlap_mask, 44, 52, origin_x, origin_y, rows,
        columns, adjusted_transform, classes, &valid_count) != 0)
    return -1;
  counts[0] = classes[0];
  counts[1] = classes[1] + classes[2];
  counts[2] = classes[3];
  return 0;
}

int
goodix_milan_match_overlap_metrics_with_context (const GoodixMilanFeatureView *enrolled_feature,
                              const GoodixMilanFeatureView *probe_feature,
                              const int32_t                 transform[6],
                              int32_t                      *overlap_score,
                              int32_t                      *overlap_coverage,
                              int32_t                      *overlap_detail,
                              int32_t                       low_metrics[3],
                              int32_t                       context_count)
{
  static const int32_t overlap_weights[3] = { 3, 2, 3 };
  uint8_t source_mask[44 * 52];
  uint8_t target_mask[44 * 52];
  uint8_t overlap_mask[44 * 52];
  int32_t half_transform[6];
  int32_t adjusted_transform[6];
  size_t origin_x;
  size_t origin_y;
  size_t overlap_rows;
  size_t overlap_columns;
  int32_t classes[4];
  int32_t valid_count;
  int32_t confidence = -1;

  if (!enrolled_feature || !probe_feature || !transform || !overlap_score ||
      !overlap_coverage || !overlap_detail || !low_metrics)
    return -1;
  memcpy (half_transform, transform, sizeof(half_transform));
  half_transform[2] =
    goodix_milan_transform_s32 ((uint32_t) half_transform[2] + 1) / 2;
  half_transform[5] =
    goodix_milan_transform_s32 ((uint32_t) half_transform[5] + 1) / 2;
  goodix_milan_feature_mask_expand (probe_feature->inline_mask, source_mask);
  goodix_milan_feature_mask_expand (enrolled_feature->inline_mask, target_mask);
  if (milan_build_overlap_mask (
        source_mask, target_mask, half_transform, overlap_mask, &origin_x,
        &origin_y, &overlap_rows, &overlap_columns, adjusted_transform) != 0 ||
      goodix_milan_match_bitmap_classes (
        enrolled_feature->high_bitmap, probe_feature->high_bitmap, overlap_mask,
        44, 52, origin_x, origin_y, overlap_rows, overlap_columns,
        adjusted_transform, classes, &valid_count) != 0 ||
      goodix_milan_match_overlap_result (
        classes, valid_count, 44 * 52, 1, overlap_weights, context_count,
        &confidence, overlap_score, overlap_coverage, overlap_detail) != 0 ||
      goodix_milan_match_low_bitmap_metrics (
        enrolled_feature->low_bitmap, enrolled_feature->inline_mask,
        probe_feature->low_bitmap, probe_feature->inline_mask, transform,
        low_metrics) != 0)
    return -1;
  if (enrolled_feature->enhanced_bitmap && probe_feature->enhanced_bitmap)
    {
      int32_t secondary_classes[4];
      int32_t secondary_valid_count;
      int32_t secondary_score;
      int32_t secondary_detail;

      if (goodix_milan_match_bitmap_classes (
            enrolled_feature->enhanced_bitmap,
            probe_feature->enhanced_bitmap, overlap_mask, 44, 52, origin_x,
            origin_y, overlap_rows, overlap_columns, adjusted_transform,
            secondary_classes, &secondary_valid_count) != 0 ||
          goodix_milan_match_overlap_result (
            secondary_classes, secondary_valid_count, 44 * 52, 1,
            overlap_weights, context_count, &confidence, &secondary_score,
            NULL, &secondary_detail) != 0)
        return -1;
      if (*overlap_score > 220 && secondary_detail > *overlap_detail)
        *overlap_detail = secondary_detail;
      *overlap_score = goodix_milan_match_secondary_result (
        *overlap_score, secondary_score, secondary_detail);
    }
  return 0;
}

int
goodix_milan_match_overlap_metrics (
  const GoodixMilanFeatureView *enrolled_feature,
  const GoodixMilanFeatureView *probe_feature,
  const int32_t                 transform[6],
  int32_t                      *overlap_score,
  int32_t                      *overlap_coverage,
  int32_t                      *overlap_detail,
  int32_t                       low_metrics[3])
{
  return goodix_milan_match_overlap_metrics_with_context (
    enrolled_feature, probe_feature, transform, overlap_score,
    overlap_coverage, overlap_detail, low_metrics, 0);
}

int
goodix_milan_registration_gate_metrics (
  const GoodixMilanFeatureView *prior,
  const GoodixMilanFeatureView *current,
  const int32_t                 transform[6],
  int32_t                      *registration_detail,
  int32_t                      *registration_coverage)
{
  GoodixMilanFeatureView primary_prior;
  int32_t score;
  int32_t coverage;
  int32_t detail;
  int32_t low_metrics[3];

  if (!prior || !registration_detail || !registration_coverage)
    return -1;
  /* Enrollment consumes primary detail: its native mode disables enhanced
  * detail relatching, and the selected score is not used by this gate. */
  primary_prior = *prior;
  primary_prior.enhanced_bitmap = NULL;
  if (goodix_milan_match_overlap_metrics_with_context (
        &primary_prior, current, transform, &score, &coverage, &detail,
        low_metrics, 0) != 0)
    return -1;
  *registration_detail = detail;
  *registration_coverage = coverage;
  return 0;
}
