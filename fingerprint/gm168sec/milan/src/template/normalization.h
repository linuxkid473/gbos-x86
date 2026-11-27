/*
 * Goodix 53x5 driver for libfprint - Milan learned-feature normalization
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
#include <limits.h>

/* The divisor is positive; C division otherwise truncates toward zero. */
static inline int64_t
milan_normalization_floor_divide (int64_t numerator, int64_t denominator)
{
  return numerator / denominator - (numerator % denominator < 0);
}

/* Intersect an inclusive row interval with 0 <= base + step*x <= maximum. */
static inline void
milan_normalization_clip_interval (int64_t  base,
                                   int64_t  step,
                                   int64_t  maximum,
                                   int64_t *first,
                                   int64_t *last)
{
  int64_t lower, upper;

  if (step == 0)
    {
      if (base < 0 || base > maximum)
        *last = -1;
      return;
    }
  if (step > 0)
    {
      lower = -milan_normalization_floor_divide (base, step);
      upper = milan_normalization_floor_divide (maximum - base, step);
    }
  else
    {
      lower = -milan_normalization_floor_divide (maximum - base, -step);
      upper = milan_normalization_floor_divide (base, -step);
    }
  if (lower > *first)
    *first = lower;
  if (upper < *last)
    *last = upper;
}

/* An affine coordinate reaches its extrema at the rectangle's corners. Only
 * use intervals when its mathematical result fits the native signed word
 * everywhere; wrapped coordinates retain the per-cell implementation. */
static inline int
milan_normalization_can_use_intervals (int32_t       destination_rows,
                                       int32_t       destination_columns,
                                       int32_t       source_rows,
                                       int32_t       source_columns,
                                       const int32_t transform[6])
{
  if ((int64_t) (source_columns - 1) * 256 > INT32_MAX ||
      (int64_t) (source_rows - 1) * 256 > INT32_MAX)
    return 0;
  for (int axis = 0; axis < 2; axis++)
    for (int corner = 0; corner < 4; corner++)
      {
        int64_t x = (corner & 1) ? destination_columns - 1 : 0;
        int64_t y = (corner & 2) ? destination_rows - 1 : 0;
        int64_t value = transform[axis * 3] * x +
                        transform[axis * 3 + 1] * y + transform[axis * 3 + 2];

        if (value < INT32_MIN || value > INT32_MAX)
          return 0;
      }
  return 1;
}

static inline int32_t
goodix_milan_template_normalization_from_bits (uint32_t bits)
{
  int32_t value;

  memcpy (&value, &bits, sizeof (value));
  return value;
}

static inline uint32_t
goodix_milan_template_normalization_to_bits (int32_t value)
{
  uint32_t bits;

  memcpy (&bits, &value, sizeof (bits));
  return bits;
}

static inline int32_t
goodix_milan_template_normalization_add (int32_t left,
                                         int32_t right)
{
  return goodix_milan_template_normalization_from_bits (
    goodix_milan_template_normalization_to_bits (left) +
    goodix_milan_template_normalization_to_bits (right));
}

static inline int32_t
goodix_milan_template_normalization_multiply (int32_t left,
                                              int32_t right)
{
  return goodix_milan_template_normalization_from_bits (
    goodix_milan_template_normalization_to_bits (left) *
    goodix_milan_template_normalization_to_bits (right));
}

static inline int32_t
goodix_milan_template_normalization_sar1 (int32_t value)
{
  uint32_t bits = goodix_milan_template_normalization_to_bits (value);

  return goodix_milan_template_normalization_from_bits (
    (bits >> 1) | (bits & UINT32_C (0x80000000)));
}

static inline int32_t
goodix_milan_template_normalization_domain_area (int32_t rows,
                                                 int32_t columns,
                                                 int32_t half_resolution)
{
  if (half_resolution)
    {
      rows = goodix_milan_template_normalization_sar1 (rows);
      columns = goodix_milan_template_normalization_sar1 (columns);
    }
  return goodix_milan_template_normalization_multiply (rows, columns);
}

static inline int
goodix_milan_template_normalization_overlap_qualifies (int32_t area,
                                                       int32_t rows,
                                                       int32_t columns,
                                                       int32_t half_resolution)
{
  int32_t scaled_area = half_resolution ?
                        goodix_milan_template_normalization_multiply (area, 4) :
                        area;
  int32_t left = goodix_milan_template_normalization_multiply (scaled_area, 100);
  int32_t right = goodix_milan_template_normalization_multiply (
    goodix_milan_template_normalization_multiply (rows, columns), 40);

  return left > right;
}

static inline int32_t
goodix_milan_template_normalization_remove_footprint (
  uint8_t      *residual,
  int32_t       destination_rows,
  int32_t       destination_columns,
  int32_t       source_rows,
  int32_t       source_columns,
  const int32_t transform[6])
{
  int32_t maximum_x;
  int32_t maximum_y;
  int32_t area = 0;

  if (!transform || destination_rows <= 0 || destination_columns <= 0 ||
      source_rows <= 0 || source_columns <= 0)
    return 0;
  maximum_x = goodix_milan_template_normalization_multiply (source_columns - 1, 256);
  maximum_y = goodix_milan_template_normalization_multiply (source_rows - 1, 256);
  if (milan_normalization_can_use_intervals (
        destination_rows, destination_columns, source_rows, source_columns,
        transform))
    {
      for (int32_t y = 0; y < destination_rows; y++)
        {
          int64_t first = 0, last = destination_columns - 1;

          milan_normalization_clip_interval (
            (int64_t) transform[1] * y + transform[2], transform[0],
            maximum_x, &first, &last);
          milan_normalization_clip_interval (
            (int64_t) transform[4] * y + transform[5], transform[3],
            maximum_y, &first, &last);
          if (first > last)
            continue;
          area = goodix_milan_template_normalization_add (
            area, (int32_t) (last - first + 1));
          if (residual)
            memset (residual + (size_t) y * destination_columns + first,
                    0, (size_t) (last - first + 1));
        }
      return area;
    }
  for (int32_t y = 0; y < destination_rows; y++)
    for (int32_t x = 0; x < destination_columns; x++)
      {
        int32_t mapped_x = goodix_milan_template_normalization_add (
          goodix_milan_template_normalization_add (
            goodix_milan_template_normalization_multiply (transform[0], x),
            goodix_milan_template_normalization_multiply (transform[1], y)),
          transform[2]);
        int32_t mapped_y = goodix_milan_template_normalization_add (
          goodix_milan_template_normalization_add (
            goodix_milan_template_normalization_multiply (transform[3], x),
            goodix_milan_template_normalization_multiply (transform[4], y)),
          transform[5]);

        if (mapped_x < 0 || mapped_x > maximum_x ||
            mapped_y < 0 || mapped_y > maximum_y)
          continue;
        area = goodix_milan_template_normalization_add (area, 1);
        if (residual)
          residual[(size_t) y * (size_t) destination_columns + (size_t) x] = 0;
      }
  return area;
}

static inline int32_t
goodix_milan_template_normalization_residual (const uint8_t *residual,
                                              int32_t        rows,
                                              int32_t        columns)
{
  int32_t count = 0;

  if (!residual || rows <= 0 || columns <= 0)
    return 0;
  for (int32_t y = 0; y < rows; y++)
    for (int32_t x = 0; x < columns; x++)
      count = goodix_milan_template_normalization_add (
        count, residual[(size_t) y * (size_t) columns + (size_t) x] != 0);
  return count < 20 ? 0 : count;
}
