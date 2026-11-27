/*
 * Goodix 53x5 driver for libfprint - Milan feature enhancement
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

/* Orientation extraction recovered from FUN_18007ebe0 and FUN_180074290. */
enum {
  MILAN_CORDIC_ITERATIONS = 13,
  MILAN_ORIENTATION_PERIOD = 0x6488,
  MILAN_ORIENTATION_HALF_PERIOD = 0x3244,
  MILAN_ORIENTATION_QUARTER_PERIOD = 0x1922,
  MILAN_ORIENTATION_DEGREE_SCALE = 0x1ca6,
  MILAN_ORIENTATION_DEGREE_SHIFT = 20,
  MILAN_ORIENTATION_SPLIT_DEGREES = 135,
  MILAN_ORIENTATION_ROTATION_DEGREES = 45,
  MILAN_ORIENTATION_BYTE_OFFSET = 76,
  MILAN_ENHANCEMENT_DIRECTION_COUNT = 12,
  MILAN_ENHANCEMENT_TAP_COUNT = 7,
  MILAN_ENHANCEMENT_RADIUS = 6,
  MILAN_ENHANCEMENT_DIRECTION_OFFSET = 8,
  MILAN_ENHANCEMENT_DIRECTION_WIDTH = 15,
  MILAN_ENHANCEMENT_DIRECTION_LIMIT = 165,
  MILAN_BITMAP_HISTOGRAM_BINS = 256,
  MILAN_BITMAP_SAMPLE_STRIDE = 2,
  MILAN_BITMAP_QUANTILE_Q8 = 205,
  MILAN_Q8_HALF = 128,
  MILAN_Q8_SHIFT = 8,
};

int16_t
feature_atan2 (int32_t y, int32_t x)
{
  static const int16_t angles[MILAN_CORDIC_ITERATIONS] = {
    3217, 1899, 1003, 509, 256, 128, 64, 32, 16, 8, 4, 2, 1,
  };
  int32_t original_x = x;
  int32_t original_y = y;
  uint16_t angle = 0;

  x = x > 0 ? x : -x;
  y = y > 0 ? y : -y;
  if (x == 0)
    return original_y > 0 ? 0 : MILAN_ORIENTATION_HALF_PERIOD;
  if (y == 0)
    return original_x > 0 ? MILAN_ORIENTATION_QUARTER_PERIOD :
           -MILAN_ORIENTATION_QUARTER_PERIOD;

  for (size_t i = 0; i < MILAN_CORDIC_ITERATIONS; i++)
    {
      int32_t shifted_x = x >> i;
      int32_t shifted_y = y >> i;

      if (x <= 0)
        {
          x += shifted_y;
          y -= shifted_x;
          angle = (uint16_t) (angle - angles[i]);
        }
      else
        {
          x -= shifted_y;
          y += shifted_x;
          angle = (uint16_t) (angle + angles[i]);
        }
      if (x == 0)
        break;
    }

  if (original_y <= 0)
    return original_x > 0 ?
           (int16_t) (MILAN_ORIENTATION_HALF_PERIOD - angle) :
           (int16_t) (angle - MILAN_ORIENTATION_HALF_PERIOD);
  if (original_x < 0)
    return (int16_t) -angle;
  return (int16_t) angle;
}

static uint32_t
feature_integral_sum (const uint32_t *integral,
                      size_t          columns,
                      size_t          row,
                      size_t          column,
                      size_t          rows,
                      size_t          radius)
{
  size_t first_column = column > radius ? column - radius : 1;
  size_t last_column = column + radius < columns ? column + radius :
                       columns - 1;
  size_t first_row = row > radius ? row - radius : 1;
  size_t last_row = row + radius < rows ? row + radius : rows - 1;

  if (first_column == 0)
    first_column = 1;
  if (first_row == 0)
    first_row = 1;
  return integral[last_row * columns + last_column] -
         integral[(first_row - 1) * columns + last_column] -
         integral[last_row * columns + first_column - 1] +
         integral[(first_row - 1) * columns + first_column - 1];
}

int
feature_build_dense_orientation (const uint8_t *frame,
                                 size_t         rows,
                                 size_t         columns,
                                 size_t         radius,
                                 uint8_t       *orientation)
{
  size_t count;
  int32_t *first = NULL;
  int32_t *second = NULL;
  uint32_t *first_integral = NULL;
  uint32_t *second_integral = NULL;
  int result = -1;

  if (!frame || !orientation || rows < 2 || columns < 2 ||
      columns > SIZE_MAX / rows)
    return -1;
  count = rows * columns;
  if (count > SIZE_MAX / sizeof (*first))
    return -1;
  first = calloc (count, sizeof (*first));
  second = calloc (count, sizeof (*second));
  first_integral = calloc (count, sizeof (*first_integral));
  second_integral = calloc (count, sizeof (*second_integral));
  if (!first || !second || !first_integral || !second_integral)
    goto out;

  for (size_t row = 1; row + 1 < rows; row++)
    for (size_t column = 1; column + 1 < columns; column++)
      {
        size_t i = row * columns + column;
        int32_t horizontal =
          ((int32_t) frame[i + 1] - frame[i - 1]) * 2 -
          frame[i - columns - 1] - frame[i + columns - 1] +
          frame[i - columns + 1] + frame[i + columns + 1];
        int32_t vertical =
          ((int32_t) frame[i + columns] - frame[i - columns]) * 2 -
          frame[i - columns - 1] - frame[i - columns + 1] +
          frame[i + columns - 1] + frame[i + columns + 1];

        first[i] = horizontal * vertical * 2;
        second[i] = horizontal * horizontal - vertical * vertical;
      }
  for (size_t row = 1; row < rows; row++)
    for (size_t column = 1; column < columns; column++)
      {
        size_t i = row * columns + column;

        first_integral[i] = (uint32_t) first[i] -
                            first_integral[i - columns - 1] +
                            first_integral[i - 1] +
                            first_integral[i - columns];
        second_integral[i] = (uint32_t) second[i] -
                             second_integral[i - columns - 1] +
                             second_integral[i - 1] +
                             second_integral[i - columns];
      }
  for (size_t row = 0; row < rows; row++)
    for (size_t column = 0; column < columns; column++)
      {
        int32_t y = (int32_t) feature_integral_sum (
          first_integral, columns, row, column, rows, radius);
        int32_t x = (int32_t) feature_integral_sum (
          second_integral, columns, row, column, rows, radius);
        int32_t angle = feature_atan2 (y, x);
        int32_t degrees;

        if (angle < 0)
          angle += MILAN_ORIENTATION_PERIOD;
        degrees = angle * MILAN_ORIENTATION_DEGREE_SCALE >>
                  MILAN_ORIENTATION_DEGREE_SHIFT;
        orientation[row * columns + column] =
          (uint8_t) (-MILAN_ORIENTATION_BYTE_OFFSET -
                     (degrees <= MILAN_ORIENTATION_SPLIT_DEGREES ?
                      degrees + MILAN_ORIENTATION_ROTATION_DEGREES :
                      degrees - MILAN_ORIENTATION_SPLIT_DEGREES));
      }
  result = 0;

out:
  free (second_integral);
  free (first_integral);
  free (second);
  free (first);
  return result;
}

/* Smooth each pixel along the seven-tap line selected by its orientation;
 * edge taps outside the image are omitted rather than reflected. */
int
goodix_milan_feature_enhance (const uint8_t *frame,
                              size_t         rows,
                              size_t         columns,
                              uint8_t       *orientation,
                              uint8_t       *output)
{
  static const int8_t
    offsets[MILAN_ENHANCEMENT_DIRECTION_COUNT][MILAN_ENHANCEMENT_TAP_COUNT][2] = {
    { {-3, 0}, {-2, 0}, {-1, 0}, {0, 0}, {1, 0}, {2, 0}, {3, 0} },
    { {-3, -1}, {-2, -1}, {-1, 0}, {0, 0}, {1, 0}, {2, 1}, {3, 1} },
    { {-3, -2}, {-2, -1}, {-1, -1}, {0, 0}, {1, 1}, {2, 1}, {3, 2} },
    { {-3, -3}, {-2, -2}, {-1, -1}, {0, 0}, {1, 1}, {2, 2}, {3, 3} },
    { {-2, -3}, {-1, -2}, {-1, -1}, {0, 0}, {1, 1}, {1, 2}, {2, 3} },
    { {-1, -3}, {-1, -2}, { 0, -1}, {0, 0}, {0, 1}, {1, 2}, {1, 3} },
    { { 0, -3}, { 0, -2}, { 0, -1}, {0, 0}, {0, 1}, {0, 2}, {0, 3} },
    { {-1, 3}, {-1, 2}, { 0, 1}, {0, 0}, {0, -1}, {1, -2}, {1, -3} },
    { {-2, 3}, {-1, 2}, {-1, 1}, {0, 0}, {1, -1}, {1, -2}, {2, -3} },
    { {-3, 3}, {-2, 2}, {-1, 1}, {0, 0}, {1, -1}, {2, -2}, {3, -3} },
    { {-3, 2}, {-2, 1}, {-1, 1}, {0, 0}, {1, -1}, {2, -1}, {3, -2} },
    { {-3, 1}, {-2, 1}, {-1, 0}, {0, 0}, {1, 0}, {2, -1}, {3, -1} },
  };
  static const uint8_t weights[MILAN_ENHANCEMENT_TAP_COUNT] = {
    1, 2, 4, 8, 4, 2, 1,
  };
  size_t count;
  int32_t *first = NULL;
  int32_t *second = NULL;
  uint32_t *first_integral = NULL;
  uint32_t *second_integral = NULL;
  int result = -1;

  if (!frame || !orientation || !output || rows < 2 || columns < 2 ||
      columns > SIZE_MAX / rows)
    return -1;
  count = rows * columns;
  if (count > SIZE_MAX / sizeof (*first))
    return -1;

  first = calloc (count, sizeof (*first));
  second = calloc (count, sizeof (*second));
  first_integral = calloc (count, sizeof (*first_integral));
  second_integral = calloc (count, sizeof (*second_integral));
  if (!first || !second || !first_integral || !second_integral)
    goto out;

  for (size_t row = 1; row + 1 < rows; row++)
    {
      for (size_t column = 1; column + 1 < columns; column++)
        {
          size_t i = row * columns + column;
          int32_t horizontal =
            ((int32_t) frame[i + 1] - frame[i - 1]) * 2 -
            frame[i - columns - 1] - frame[i + columns - 1] +
            frame[i - columns + 1] + frame[i + columns + 1];
          int32_t vertical =
            ((int32_t) frame[i + columns] - frame[i - columns]) * 2 -
            frame[i - columns - 1] - frame[i - columns + 1] +
            frame[i + columns - 1] + frame[i + columns + 1];

          first[i] = horizontal * vertical * 2;
          second[i] = horizontal * horizontal - vertical * vertical;
        }
    }

  for (size_t row = 1; row < rows; row++)
    {
      for (size_t column = 1; column < columns; column++)
        {
          size_t i = row * columns + column;

          first_integral[i] = (uint32_t) first[i] -
                              first_integral[i - columns - 1] +
                              first_integral[i - 1] +
                              first_integral[i - columns];
          second_integral[i] = (uint32_t) second[i] -
                               second_integral[i - columns - 1] +
                               second_integral[i - 1] +
                               second_integral[i - columns];
        }
    }

  for (size_t row = 0; row < rows; row++)
    {
      for (size_t column = 0; column < columns; column++)
        {
          int32_t y = (int32_t) feature_integral_sum (
            first_integral, columns, row, column, rows,
            MILAN_ENHANCEMENT_RADIUS);
          int32_t x = (int32_t) feature_integral_sum (
            second_integral, columns, row, column, rows,
            MILAN_ENHANCEMENT_RADIUS);
          int32_t angle = feature_atan2 (y, x);
          int32_t degrees;

          if (angle < 0)
            angle += MILAN_ORIENTATION_PERIOD;
          degrees = angle * MILAN_ORIENTATION_DEGREE_SCALE >>
                    MILAN_ORIENTATION_DEGREE_SHIFT;
          orientation[row * columns + column] =
            (uint8_t) (-MILAN_ORIENTATION_BYTE_OFFSET -
                       (degrees <= MILAN_ORIENTATION_SPLIT_DEGREES ?
                        degrees + MILAN_ORIENTATION_ROTATION_DEGREES :
                        degrees - MILAN_ORIENTATION_SPLIT_DEGREES));
        }
    }

  for (size_t row = 0; row < rows; row++)
    {
      for (size_t column = 0; column < columns; column++)
        {
          size_t i = row * columns + column;
          uint8_t shifted =
            (uint8_t) (orientation[i] - MILAN_ENHANCEMENT_DIRECTION_OFFSET);
          size_t direction = shifted < MILAN_ENHANCEMENT_DIRECTION_LIMIT ?
                             shifted / MILAN_ENHANCEMENT_DIRECTION_WIDTH + 1 :
                             0;
          uint32_t sum = 0;
          uint32_t weight_sum = 0;

          for (size_t tap = 0; tap < MILAN_ENHANCEMENT_TAP_COUNT; tap++)
            {
              ptrdiff_t x = (ptrdiff_t) column + offsets[direction][tap][0];
              ptrdiff_t y = (ptrdiff_t) row + offsets[direction][tap][1];

              if (x >= 0 && x < (ptrdiff_t) columns &&
                  y >= 0 && y < (ptrdiff_t) rows)
                {
                  sum += frame[(size_t) y * columns + (size_t) x] * weights[tap];
                  weight_sum += weights[tap];
                }
            }
          output[i] = weight_sum != 0 ? (uint8_t) (sum / weight_sum) : UINT8_MAX;
        }
    }
  result = 0;

out:
  free (second_integral);
  free (first_integral);
  free (second);
  free (first);
  return result;
}

/* Select the masked even/even quantile, then pack every sample strictly above
 * it; the feature mask affects threshold selection but does not gate bits. */
int
goodix_milan_feature_enhanced_bitmap (
  const uint8_t *enhanced,
  const uint8_t *feature_mask,
  size_t         rows,
  size_t         columns,
  uint8_t       *bitmap,
  uint8_t       *threshold)
{
  uint32_t histogram[MILAN_BITMAP_HISTOGRAM_BINS] = { 0 };
  size_t map_rows;
  size_t map_columns;
  size_t map_count;
  uint32_t active_count = 0;
  uint32_t target;
  uint8_t selected = 0;

  if (!enhanced || !feature_mask || !bitmap || !threshold || rows < 2 ||
      columns < 2 || columns > SIZE_MAX / rows)
    return -1;
  map_rows = rows / MILAN_BITMAP_SAMPLE_STRIDE;
  map_columns = columns / MILAN_BITMAP_SAMPLE_STRIDE;
  if (map_columns > SIZE_MAX / map_rows)
    return -1;
  map_count = map_rows * map_columns;

  for (size_t row = 0; row < map_rows; row++)
    for (size_t column = 0; column < map_columns; column++)
      {
        size_t map_index = row * map_columns + column;

        if (feature_mask[map_index] != 0)
          {
            histogram[enhanced[row * MILAN_BITMAP_SAMPLE_STRIDE * columns +
                               column * MILAN_BITMAP_SAMPLE_STRIDE]]++;
            active_count++;
          }
      }

  target = (MILAN_BITMAP_QUANTILE_Q8 * active_count + MILAN_Q8_HALF) >>
           MILAN_Q8_SHIFT;
  for (size_t value = 1; value < MILAN_BITMAP_HISTOGRAM_BINS; value++)
    {
      histogram[value] += histogram[value - 1];
      if (target <= histogram[value])
        {
          /* Bin zero can already exceed target; native distances are signed. */
          selected = (int32_t) (histogram[value] - target) <=
                     (int32_t) (target - histogram[value - 1]) ?
                     (uint8_t) value :
                     (uint8_t) (value - 1);
          break;
        }
    }
  *threshold = selected;

  memset (bitmap, 0, (map_count + 7) / 8);
  for (size_t row = 0; row < map_rows; row++)
    for (size_t column = 0; column < map_columns; column++)
      {
        size_t map_index = row * map_columns + column;

        if (enhanced[row * MILAN_BITMAP_SAMPLE_STRIDE * columns +
                     column * MILAN_BITMAP_SAMPLE_STRIDE] > selected)
          bitmap[map_index >> 3] |= (uint8_t) (1U << (map_index & 7));
      }
  return 0;
}
