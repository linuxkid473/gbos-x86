/*
 * Goodix 53x5 driver for libfprint - Milan feature extraction
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/feature/feature.h"
#include "milan/private.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  size_t length;
  uint16_t coefficients[15];
} MilanFeatureKernel;

static const MilanFeatureKernel milan_feature_kernels[2][6] = {
  {
    { 9,  { 84, 957, 5433, 15399, 21790, 15399, 5433, 957, 84 } },
    { 7,  { 139, 2672, 15742, 28430, 15742, 2672, 139 } },
    { 9,  { 58, 791, 5088, 15549, 22564, 15549, 5088, 791, 58 } },
    { 11, { 51, 420, 2169, 7008, 14165, 17910, 14165, 7008, 2169,
             420, 51 } },
    { 13, { 70, 353, 1337, 3761, 7873, 12265, 14218, 12265, 7873,
             3761, 1337, 353, 70 } },
    { 15, { 118, 396, 1102, 2547, 4886, 7783, 10289, 11294, 10289,
             7783, 4886, 2547, 1102, 396, 118 } },
  },
  {
    { 9,  { 54, 757, 5011, 15578, 22736, 15578, 5011, 757, 54 } },
    { 7,  { 90, 2260, 15585, 29666, 15585, 2260, 90 } },
    { 9,  { 36, 612, 4651, 15697, 23544, 15697, 4651, 612, 36 } },
    { 11, { 31, 314, 1876, 6727, 14476, 18688, 14476, 6727, 1876,
             314, 31 } },
    { 13, { 45, 266, 1130, 3486, 7794, 12630, 14834, 12630, 7794,
             3486, 1130, 266, 45 } },
    { 15, { 82, 306, 934, 2327, 4731, 7853, 10645, 11780, 10645,
             7853, 4731, 2327, 934, 306, 82 } },
  },
};

static int32_t
feature_u32_as_s32 (uint32_t value)
{
  if (value <= INT32_MAX)
    return (int32_t) value;
  return -1 - (int32_t) (UINT32_MAX - value);
}

static int
feature_filter_q16 (const uint16_t          *source,
                    size_t                   rows,
                    size_t                   columns,
                    const MilanFeatureKernel *kernel,
                    uint16_t                *output)
{
  size_t count = rows * columns;
  size_t radius = kernel->length / 2;
  uint16_t *horizontal = malloc (count * sizeof(*horizontal));

  if (!horizontal)
    return -1;
  for (size_t column = 0; rows != 0 && column < columns; column++)
    {
      size_t reflected[15];

      for (size_t tap = 0; tap < kernel->length; tap++)
        reflected[tap] = goodix_milan_reflect101_index (
          (ptrdiff_t) column + (ptrdiff_t) tap - (ptrdiff_t) radius,
          columns);
      for (size_t row = 0; row < rows; row++)
        {
          uint64_t sum = 0;

          for (size_t tap = 0; tap < kernel->length; tap++)
            sum += (uint64_t) source[row * columns + reflected[tap]] *
                   kernel->coefficients[tap];
          horizontal[row * columns + column] = (uint16_t) (sum >> 16);
        }
    }
  for (size_t row = 0; columns != 0 && row < rows; row++)
    {
      size_t reflected[15];

      for (size_t tap = 0; tap < kernel->length; tap++)
        reflected[tap] = goodix_milan_reflect101_index (
          (ptrdiff_t) row + (ptrdiff_t) tap - (ptrdiff_t) radius, rows) *
                         columns;
      for (size_t column = 0; column < columns; column++)
        {
          uint64_t sum = 0;

          for (size_t tap = 0; tap < kernel->length; tap++)
            sum += (uint64_t) horizontal[reflected[tap] + column] *
                   kernel->coefficients[tap];
          output[row * columns + column] = (uint16_t) (sum >> 16);
        }
    }
  free (horizontal);
  return 0;
}

uint16_t
feature_cordic (int32_t vertical,
                int32_t *horizontal)
{
  static const uint16_t angles[13] = {
    0x0c91, 0x076b, 0x03eb, 0x01fd, 0x0100, 0x0080, 0x0040,
    0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x0001,
  };
  static const int32_t gains[13] = {
    0xb505, 0xa1e9, 0x9d13, 0x9bdd, 0x9b8f, 0x9b7b, 0x9b77,
    0x9b75, 0x9b75, 0x9b75, 0x9b75, 0x9b75, 0x9b75,
  };
  int32_t original_horizontal = *horizontal;
  int32_t original_vertical = vertical;
  int32_t x = original_horizontal > 0 ? original_horizontal
                                       : -original_horizontal;
  int32_t y = original_vertical > 0 ? original_vertical : -original_vertical;
  uint16_t angle = 0;
  size_t stop = 12;

  if (y == 0)
    {
      *horizontal = x;
      return original_horizontal > 0 ? 0 : 0x3244;
    }
  if (x == 0)
    {
      *horizontal = y;
      return original_vertical > 0 ? 0x1922 : 0xe6de;
    }
  for (size_t iteration = 0; iteration < 13; iteration++)
    {
      int32_t shifted_y = y >> iteration;
      int32_t shifted_x = x >> iteration;

      if (y > 0)
        {
          x += shifted_y;
          y -= shifted_x;
          angle = (uint16_t) (angle + angles[iteration]);
        }
      else
        {
          x -= shifted_y;
          y += shifted_x;
          angle = (uint16_t) (angle - angles[iteration]);
        }
      if (y == 0)
        {
          stop = iteration;
          break;
        }
    }
  if (original_horizontal > 0)
    {
      if (original_vertical < 0)
        angle = (uint16_t) -angle;
    }
  else if (original_vertical > 0)
    angle = (uint16_t) (0x3244 - angle);
  else
    angle = (uint16_t) (angle + 0xcdbc);
  *horizontal = (int32_t) (((int64_t) gains[stop] * x + 0x8000) >> 16);
  return angle;
}

static int
feature_build_scale_space_pass (const uint8_t *blurred,
                                size_t         rows,
                                size_t         columns,
                                unsigned int   pass_marker,
                                uint16_t      *scales)
{
  const MilanFeatureKernel *kernels;
  size_t count;

  if ((rows != 0 && columns > SIZE_MAX / rows) ||
      (pass_marker != 0 && pass_marker != 100))
    return -1;
  kernels = milan_feature_kernels[pass_marker == 100];
  count = rows * columns;
  uint16_t *source = malloc (count * sizeof(*source));
  if (!source)
    return -1;
  for (size_t i = 0; i < count; i++)
    source[i] = (uint16_t) blurred[i] << 8;
  if (feature_filter_q16 (source, rows, columns, &kernels[0], scales) != 0)
    {
      free (source);
      return -1;
    }
  free (source);
  for (size_t scale = 1; scale < 6; scale++)
    if (feature_filter_q16 (scales + (scale - 1) * count, rows, columns,
                             &kernels[scale],
                             scales + scale * count) != 0)
      return -1;
  return 0;
}

static int
feature_build_gradients_pass (const uint8_t *enhanced,
                              size_t         rows,
                              size_t         columns,
                              unsigned int   pass_marker,
                              int32_t       *gradient_input,
                              uint32_t      *magnitude,
                              int16_t       *orientation)
{
  const MilanFeatureKernel *kernels;
  size_t count;
  uint16_t *lifted = NULL;
  uint16_t *first = NULL;
  uint16_t *second = NULL;
  int result = -1;

  if ((rows != 0 && columns > SIZE_MAX / rows) ||
      (pass_marker != 0 && pass_marker != 100))
    return -1;
  kernels = milan_feature_kernels[pass_marker == 100];
  count = rows * columns;
  lifted = malloc (count * sizeof(*lifted));
  first = malloc (count * sizeof(*first));
  second = malloc (count * sizeof(*second));
  if (!lifted || !first || !second)
    goto out;
  for (size_t i = 0; i < count; i++)
    lifted[i] = (uint16_t) enhanced[i] << 8;
  if (feature_filter_q16 (lifted, rows, columns, &kernels[0], first) != 0 ||
      feature_filter_q16 (first, rows, columns, &kernels[1], second) != 0)
    goto out;
  for (size_t i = 0; i < count; i++)
    gradient_input[i] = (int32_t) first[i] * 10 - (int32_t) second[i] * 9;
  memset (magnitude, 0, count * sizeof(*magnitude));
  memset (orientation, 0, count * sizeof(*orientation));
  for (size_t row = 1; row + 1 < rows; row++)
    for (size_t column = 1; column + 1 < columns; column++)
      {
        size_t i = row * columns + column;
        int32_t horizontal = gradient_input[i + 1] - gradient_input[i - 1];
        int32_t vertical = gradient_input[i + columns] -
                           gradient_input[i - columns];

        if (horizontal > 0x7ffff)
          horizontal = 0x7ffff;
        else if (horizontal < -0x80000)
          horizontal = -0x80000;
        if (vertical > 0x7ffff)
          vertical = 0x7ffff;
        else if (vertical < -0x80000)
          vertical = -0x80000;
        horizontal = feature_u32_as_s32 ((uint32_t) horizontal << 12);
        vertical = feature_u32_as_s32 ((uint32_t) vertical << 12);
        orientation[i] = (int16_t) feature_cordic (vertical, &horizontal);
        magnitude[i] = (uint32_t) (horizontal >> 12);
        if (magnitude[i] > 0x3ffff)
          magnitude[i] = 0x3ffff;
      }
  result = 0;

out:
  free (second);
  free (first);
  free (lifted);
  return result;
}

static int
feature_gaussian6_u8 (const uint8_t *source,
                      size_t         rows,
                      size_t         columns,
                      uint8_t       *output)
{
  static const MilanFeatureKernel kernel = {
    3, { 21845, 21845, 21845 },
  };
  size_t count;
  uint16_t *lifted = NULL;
  uint16_t *filtered = NULL;

  if (rows == 0 || columns == 0 || columns > SIZE_MAX / rows ||
      rows * columns > SIZE_MAX / sizeof(*lifted))
    return -1;
  count = rows * columns;
  lifted = calloc (count, sizeof(*lifted));
  filtered = calloc (count, sizeof(*filtered));

  if (!lifted || !filtered)
    {
      free (filtered);
      free (lifted);
      return -1;
    }
  for (size_t i = 0; i < count; i++)
    lifted[i] = (uint16_t) source[i] << 8;
  int result = feature_filter_q16 (lifted, rows, columns, &kernel, filtered);
  if (result == 0)
    for (size_t i = 0; i < count; i++)
      output[i] = (uint8_t) (filtered[i] >> 8);
  free (filtered);
  free (lifted);
  return result;
}

int
goodix_milan_feature_should_retry_scale_space (size_t       materialized_count,
                                                unsigned int pass_marker,
                                                int          configured_retry)
{
  return materialized_count <= 60 && pass_marker == 0 &&
         configured_retry != 0;
}

static int
feature_filter_broken_pixels (GoodixMilanFeatureRecord *records,
                              size_t                   *record_count,
                              const uint8_t            *broken_mask,
                              size_t                    rows,
                              size_t                    columns,
                              uint8_t                  *validity_mask,
                              int                       high_class)
{
  size_t stride = columns + 1;
  size_t radius = high_class < 4 ? 2 : 5;
  int16_t *integral = calloc ((rows + 1) * stride, sizeof(*integral));
  uint16_t *local_zeros = calloc (rows * columns, sizeof(*local_zeros));
  int global_zeros = 0;
  int result = -1;

  if (!integral || !local_zeros)
    goto out;
  for (size_t y = 0; y < rows; y++)
    for (size_t x = 0; x < columns; x++)
      {
        size_t source = y * columns + x;
        size_t destination = (y + 1) * stride + x + 1;
        int zero = broken_mask[source] == 0;

        global_zeros += zero;
        integral[destination] = (int16_t) (
          integral[destination - stride] + integral[destination - 1] -
          integral[destination - stride - 1] + zero);
      }
  if (global_zeros >= 50)
    {
      for (size_t y = 0; y < rows; y++)
        for (size_t x = 0; x < columns; x++)
          {
            size_t left = x > radius ? x - radius : 0;
            size_t top = y > radius ? y - radius : 0;
            size_t right = x + radius + 1 < columns
                             ? x + radius + 1
                             : columns;
            size_t bottom = y + radius + 1 < rows ? y + radius + 1 : rows;

            local_zeros[y * columns + x] = (uint16_t) (
              integral[bottom * stride + right] -
              integral[top * stride + right] -
              integral[bottom * stride + left] +
              integral[top * stride + left]);
          }
      if (high_class >= 4)
        {
          size_t validity_columns = columns / 8 * 8;

          for (size_t y = 0; y < rows; y++)
            for (size_t x = 0; x < validity_columns; x++)
              if (local_zeros[y * columns + x] != 0)
                validity_mask[y * validity_columns + x] = 0;
        }
      for (size_t i = 0; i < *record_count;)
        {
          size_t x = ((uint16_t) records[i].refined_x + 0x80) >> 8;
          size_t y = ((uint16_t) records[i].refined_y + 0x80) >> 8;

          /* Native checks only x before indexing the unshifted 108-wide map. */
          if (x < columns && local_zeros[y * columns + x] > 1)
            {
              (*record_count)--;
              if (i != *record_count)
                {
                  records[i] = records[*record_count];
                  memset (&records[*record_count], 0,
                          sizeof(records[*record_count]));
                }
              continue;
            }
          i++;
        }
    }
  result = 0;

out:
  free (local_zeros);
  free (integral);
  return result;
}

static int
feature_extract_records (
  const uint8_t            *frame,
  size_t                    rows,
  size_t                    columns,
  GoodixMilanFeatureRecord *records,
  size_t                    capacity,
  size_t                   *record_count,
  size_t                   *zero_flag_count,
  int                       expand_records,
  int                       configured_retry,
  const uint8_t            *broken_mask,
  uint8_t                  *validity_mask,
  int                       high_class)
{
  GoodixMilanFeatureRecord
    materialized[MILAN_FEATURE_MATERIALIZED_LIMIT];
  GoodixMilanFeatureRank ranks[MILAN_FEATURE_MATERIALIZED_LIMIT];
  GoodixMilanFeatureAux auxiliary[MILAN_FEATURE_MATERIALIZED_LIMIT];
  size_t cropped_columns;
  size_t count;
  size_t materialized_count;
  size_t offset;
  unsigned int pass_marker = 0;
  uint8_t *cropped = NULL;
  uint8_t *dense_orientation = NULL;
  uint8_t *enhanced = NULL;
  uint8_t *blurred = NULL;
  uint16_t *scales = NULL;
  int32_t *gradient_input = NULL;
  uint32_t *magnitude = NULL;
  int16_t *gradient_orientation = NULL;
  int result = -1;

  if (!frame || !records || !record_count || !zero_flag_count || rows < 2 ||
      columns < 8 || columns > SIZE_MAX / rows)
    return -1;
  cropped_columns = columns / 8 * 8;
  count = rows * cropped_columns;
  offset = (columns - cropped_columns) / 2;
  cropped = malloc (count);
  dense_orientation = malloc (count);
  enhanced = malloc (count);
  blurred = malloc (count);
  scales = malloc (6 * count * sizeof(*scales));
  gradient_input = malloc (count * sizeof(*gradient_input));
  magnitude = malloc (count * sizeof(*magnitude));
  gradient_orientation = malloc (count * sizeof(*gradient_orientation));
  if (!cropped || !dense_orientation || !enhanced || !blurred ||
      !scales || !gradient_input || !magnitude ||
      !gradient_orientation)
    goto out;
  for (size_t row = 0; row < rows; row++)
    memcpy (cropped + row * cropped_columns,
            frame + row * columns + offset, cropped_columns);
  if (goodix_milan_feature_enhance (
        cropped, rows, cropped_columns, dense_orientation, enhanced) != 0 ||
      feature_gaussian6_u8 (
        cropped, rows, cropped_columns, blurred) != 0 ||
      feature_build_gradients_pass (
        enhanced, rows, cropped_columns, pass_marker, gradient_input,
        magnitude, gradient_orientation) != 0 ||
      feature_build_scale_space_pass (
        blurred, rows, cropped_columns, pass_marker, scales) != 0)
    goto out;

  materialized_count = goodix_milan_feature_collect_materialized (
    blurred, scales, magnitude, gradient_orientation, rows, cropped_columns,
    materialized, ranks, auxiliary, MILAN_FEATURE_MATERIALIZED_LIMIT);
  if (goodix_milan_feature_should_retry_scale_space (
        materialized_count, pass_marker, configured_retry))
    {
      pass_marker = 100;
      if (feature_build_scale_space_pass (
            blurred, rows, cropped_columns, pass_marker, scales) != 0)
        goto out;
      if (feature_build_gradients_pass (
            enhanced, rows, cropped_columns, pass_marker, gradient_input,
            magnitude, gradient_orientation) != 0)
        goto out;
      materialized_count = goodix_milan_feature_collect_materialized (
        blurred, scales, magnitude, gradient_orientation, rows,
        cropped_columns, materialized, ranks, auxiliary,
        MILAN_FEATURE_MATERIALIZED_LIMIT);
    }
  size_t limit = capacity < 150 ? capacity : 150;
  *record_count = feature_finish_pretransform_records (
    magnitude, gradient_orientation, rows, cropped_columns, records, limit,
    materialized, ranks, auxiliary, materialized_count);
  if (broken_mask && *record_count != 0 &&
      feature_filter_broken_pixels (
        records, record_count, broken_mask, rows, columns, validity_mask,
        high_class) != 0)
    goto out;
  for (size_t i = 0; i < *record_count; i++)
    goodix_milan_feature_transform_record ((uint8_t *) &records[i],
                                            expand_records);
  *zero_flag_count = goodix_milan_feature_partition_records (
    (uint8_t *) records, *record_count);
  result = 0;

out:
  free (gradient_orientation);
  free (magnitude);
  free (gradient_input);
  free (scales);
  free (blurred);
  free (enhanced);
  free (dense_orientation);
  free (cropped);
  return result;
}

int
goodix_milan_feature_extract_records_mode_configured (
  const uint8_t            *frame,
  size_t                    rows,
  size_t                    columns,
  GoodixMilanFeatureRecord *records,
  size_t                    capacity,
  size_t                   *record_count,
  size_t                   *zero_flag_count,
  int                       expand_records,
  int                       configured_retry)
{
  return feature_extract_records (
    frame, rows, columns, records, capacity, record_count, zero_flag_count,
    expand_records, configured_retry, NULL, NULL, 0);
}

int
goodix_milan_feature_extract_records_mode_masked (
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
  int                       high_class)
{
  return feature_extract_records (
    frame, rows, columns, records, capacity, record_count, zero_flag_count,
    expand_records, 1, broken_mask, validity_mask, high_class);
}

int
goodix_milan_feature_extract_records_mode (
  const uint8_t            *frame,
  size_t                    rows,
  size_t                    columns,
  GoodixMilanFeatureRecord *records,
  size_t                    capacity,
  size_t                   *record_count,
  size_t                   *zero_flag_count,
  int                       expand_records)
{
  return goodix_milan_feature_extract_records_mode_configured (
    frame, rows, columns, records, capacity, record_count, zero_flag_count,
    expand_records, 1);
}
