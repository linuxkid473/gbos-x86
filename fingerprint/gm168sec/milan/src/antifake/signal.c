/*
 * Goodix 53x5 driver for libfprint - Milan anti-fake signal analysis
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/antifake/antifake.h"
#include "milan/print.h"
#include "milan/transform-private.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

/* Profile-9 anti-fake signal stages recovered from FUN_18003a150,
 * FUN_18003a9b0, FUN_18003aff0, and FUN_180039c10. */
#define ANTIFAKE_RESIDUAL_BASELINE 0x1bb7
#define ANTIFAKE_EDGE_REPLICATE_TYPE_17 0x11
#define ANTIFAKE_NEIGHBORHOOD_SIZE 9
#define ANTIFAKE_NEIGHBORHOOD_MEDIAN 4
#define ANTIFAKE_HISTOGRAM_BINS 4096
#define ANTIFAKE_HISTOGRAM_LIMIT 0x0fff
#define ANTIFAKE_TAIL_COUNT_DIVISOR 50
#define ANTIFAKE_IMPULSE_CUTOFF_NUMERATOR 11
#define ANTIFAKE_IMPULSE_CUTOFF_DENOMINATOR 10
#define ANTIFAKE_ROUGHNESS_MEDIAN_SCALE 5
#define ANTIFAKE_ROUGHNESS_CENTER_SCALE 2
#define ANTIFAKE_TEXTURE_BORDER 4
#define ANTIFAKE_TEXTURE_RADIUS 5
#define ANTIFAKE_HIGHPASS_CENTER 0x2000
#define ANTIFAKE_TEXTURE_SCALE 3
#define ANTIFAKE_VARIATION_Q12_ONE 0x1000

int
goodix_milan_antifake_residual (
  const uint16_t *calibration,
  const uint16_t *raw_frame,
  size_t          rows,
  size_t          columns,
  int32_t         sensor_offset,
  uint16_t        chip_type,
  uint16_t       *residual)
{
  size_t count;

  if (!calibration || !raw_frame || !residual || rows < 2 || columns < 2 ||
      columns > SIZE_MAX / rows)
    return -1;
  count = rows * columns;
  if (count > SIZE_MAX / sizeof (*residual))
    return -1;

  memset (residual, 0, count * sizeof (*residual));
  for (size_t row = 1; row + 1 < rows; row++)
    for (size_t column = 1; column + 1 < columns; column++)
      {
        size_t i = row * columns + column;
        int32_t value = (int32_t) calibration[i] - raw_frame[i] -
                        sensor_offset - ANTIFAKE_RESIDUAL_BASELINE;

        residual[i] = (uint16_t) value;
      }

  if (chip_type == GOODIX_MILAN_PRINT_SENSOR_TYPE ||
      chip_type == ANTIFAKE_EDGE_REPLICATE_TYPE_17)
    {
      memcpy (residual, residual + columns,
              columns * sizeof (*residual));
      memcpy (residual + (rows - 1) * columns,
              residual + (rows - 2) * columns,
              columns * sizeof (*residual));
      for (size_t row = 0; row < rows; row++)
        {
          residual[row * columns] = residual[row * columns + 1];
          residual[row * columns + columns - 1] =
            residual[row * columns + columns - 2];
        }
    }

  return 0;
}

static uint16_t
antifake_median9_u16 (const uint16_t values[ANTIFAKE_NEIGHBORHOOD_SIZE])
{
  uint16_t sorted[ANTIFAKE_NEIGHBORHOOD_SIZE];

  memcpy (sorted, values, sizeof (sorted));
  for (size_t i = 1; i < ANTIFAKE_NEIGHBORHOOD_SIZE; i++)
    {
      uint16_t value = sorted[i];
      size_t j = i;

      while (j > 0 && value < sorted[j - 1])
        {
          sorted[j] = sorted[j - 1];
          j--;
        }
      sorted[j] = value;
    }
  return sorted[ANTIFAKE_NEIGHBORHOOD_MEDIAN];
}

static int32_t
antifake_local_roughness (const uint16_t *residual,
                          size_t          rows,
                          size_t          columns,
                          ptrdiff_t       x,
                          ptrdiff_t       y)
{
  int32_t roughness = 0;

  if (x < 1)
    x = 1;
  if (x > (ptrdiff_t) columns - 2)
    x = (ptrdiff_t) columns - 2;
  if (y < 1)
    y = 1;
  if (y > (ptrdiff_t) rows - 2)
    y = (ptrdiff_t) rows - 2;
  uint16_t center = residual[(size_t) y * columns + (size_t) x];
  for (ptrdiff_t dy = -1; dy <= 1; dy++)
    for (ptrdiff_t dx = -1; dx <= 1; dx++)
      {
        uint16_t neighbor = residual[(size_t) (y + dy) * columns +
                                     (size_t) (x + dx)];
        roughness += center < neighbor ? neighbor - center :
                     center - neighbor;
      }
  return roughness;
}

int
goodix_milan_antifake_impulse_filter (
  uint16_t *residual,
  size_t    rows,
  size_t    columns,
  int32_t  *threshold)
{
  uint32_t histogram[ANTIFAKE_HISTOGRAM_BINS] = { 0 };

  if (!residual || !threshold || rows < 3 || columns < 3 ||
      columns > SIZE_MAX / rows || rows * columns > INT32_MAX)
    return -1;
  for (size_t row = 1; row + 1 < rows; row++)
    for (size_t column = 1; column + 1 < columns; column++)
      if (residual[row * columns + column] < ANTIFAKE_HISTOGRAM_LIMIT)
        histogram[residual[row * columns + column]]++;
  for (int value = ANTIFAKE_HISTOGRAM_BINS - 2; value >= 0; value--)
    histogram[value] += histogram[value + 1];

  int32_t selected = 0;
  int32_t minimum_count =
    (int32_t) (rows * columns) / ANTIFAKE_TAIL_COUNT_DIVISOR;
  for (int32_t value = ANTIFAKE_HISTOGRAM_BINS - 1; value >= 0; value--)
    if ((int32_t) histogram[value] > minimum_count)
      {
        selected = value;
        break;
      }
  *threshold = selected;
  uint16_t cutoff =
    (uint16_t) ((selected * ANTIFAKE_IMPULSE_CUTOFF_NUMERATOR) /
                ANTIFAKE_IMPULSE_CUTOFF_DENOMINATOR);
  for (size_t row = 0; row < rows; row++)
    for (size_t column = 0; column < columns; column++)
      {
        size_t pixel = row * columns + column;

        if (residual[pixel] < cutoff)
          continue;
        int32_t roughness[ANTIFAKE_NEIGHBORHOOD_SIZE];
        size_t roughness_index = 0;
        for (ptrdiff_t dy = -1; dy <= 1; dy++)
          for (ptrdiff_t dx = -1; dx <= 1; dx++)
            roughness[roughness_index++] = antifake_local_roughness (
              residual, rows, columns, (ptrdiff_t) column + dx,
              (ptrdiff_t) row + dy);
        int32_t sorted_roughness[ANTIFAKE_NEIGHBORHOOD_SIZE];
        memcpy (sorted_roughness, roughness, sizeof (sorted_roughness));
        for (size_t i = 1; i < ANTIFAKE_NEIGHBORHOOD_SIZE; i++)
          {
            int32_t value = sorted_roughness[i];
            size_t j = i;

            while (j > 0 && value < sorted_roughness[j - 1])
              {
                sorted_roughness[j] = sorted_roughness[j - 1];
                j--;
              }
            sorted_roughness[j] = value;
          }
        if (sorted_roughness[ANTIFAKE_NEIGHBORHOOD_MEDIAN] *
            ANTIFAKE_ROUGHNESS_MEDIAN_SCALE <=
            roughness[ANTIFAKE_NEIGHBORHOOD_MEDIAN] *
            ANTIFAKE_ROUGHNESS_CENTER_SCALE)
          {
            uint16_t neighborhood[ANTIFAKE_NEIGHBORHOOD_SIZE];
            size_t neighborhood_index = 0;
            size_t count = rows * columns;

            /*
             * Vendor replacement uses flat adjacency, so edge columns intentionally
             * wrap; only allocation-end offsets use the safe coordinate-clamped
             * fallback.
             */
            if (pixel >= columns + 1 && pixel + columns + 1 < count)
              {
                for (ptrdiff_t dy = -1; dy <= 1; dy++)
                  for (ptrdiff_t dx = -1; dx <= 1; dx++)
                    neighborhood[neighborhood_index++] = residual[
                      (size_t) ((ptrdiff_t) pixel +
                                dy * (ptrdiff_t) columns + dx)];
              }
            else
              {
                ptrdiff_t center_x = (ptrdiff_t) column;
                ptrdiff_t center_y = (ptrdiff_t) row;

                if (center_x < 1)
                  center_x = 1;
                if (center_x > (ptrdiff_t) columns - 2)
                  center_x = (ptrdiff_t) columns - 2;
                if (center_y < 1)
                  center_y = 1;
                if (center_y > (ptrdiff_t) rows - 2)
                  center_y = (ptrdiff_t) rows - 2;
                for (ptrdiff_t dy = -1; dy <= 1; dy++)
                  for (ptrdiff_t dx = -1; dx <= 1; dx++)
                    neighborhood[neighborhood_index++] =
                      residual[(size_t) (center_y + dy) * columns +
                               (size_t) (center_x + dx)];
              }
            residual[pixel] = antifake_median9_u16 (neighborhood);
          }
      }
  return 0;
}

int
goodix_milan_antifake_statistics (
  const uint16_t *residual,
  const uint8_t  *mask,
  size_t          rows,
  size_t          columns,
  int32_t        *texture,
  int32_t        *mean)
{
  uint64_t masked_sum = 0;
  uint64_t texture_sum = 0;
  size_t masked_count = 0;
  size_t texture_count = 0;

  if (!residual || !mask || !texture || !mean || rows == 0 || columns == 0 ||
      columns > SIZE_MAX / rows)
    return -1;

  for (size_t i = 0; i < rows * columns; i++)
    if (mask[i] != 0)
      {
        masked_sum += residual[i];
        masked_count++;
      }
  *mean = masked_count == 0 ?
          0 :
          (int32_t) ((masked_sum + masked_count / 2) / masked_count);

  if (rows > 2 * ANTIFAKE_TEXTURE_BORDER &&
      columns > 2 * ANTIFAKE_TEXTURE_BORDER)
    {
      for (size_t row = ANTIFAKE_TEXTURE_BORDER;
           row + ANTIFAKE_TEXTURE_BORDER < rows;
           row++)
        for (size_t column = ANTIFAKE_TEXTURE_BORDER;
             column + ANTIFAKE_TEXTURE_BORDER < columns;
             column++)
          {
            size_t i = row * columns + column;
            uint32_t local_sum = 0;
            size_t top = row > ANTIFAKE_TEXTURE_RADIUS ?
                         row - ANTIFAKE_TEXTURE_RADIUS : 0;
            size_t bottom = row + ANTIFAKE_TEXTURE_RADIUS < rows ?
                            row + ANTIFAKE_TEXTURE_RADIUS : rows - 1;
            size_t left = column > ANTIFAKE_TEXTURE_RADIUS ?
                          column - ANTIFAKE_TEXTURE_RADIUS : 0;
            size_t right = column + ANTIFAKE_TEXTURE_RADIUS < columns ?
                           column + ANTIFAKE_TEXTURE_RADIUS : columns - 1;
            size_t area = (bottom - top + 1) * (right - left + 1);

            if (mask[i] == 0)
              continue;
            for (size_t y = top; y <= bottom; y++)
              for (size_t x = left; x <= right; x++)
                local_sum += residual[y * columns + x];
            uint16_t local_mean = (uint16_t) ((local_sum + area / 2) / area);
            int16_t highpass = (int16_t) ((int16_t) residual[i] -
                                          (int16_t) local_mean +
                                          ANTIFAKE_HIGHPASS_CENTER);
            uint16_t encoded = (uint16_t) highpass;

            texture_sum += encoded > ANTIFAKE_HIGHPASS_CENTER ?
                           encoded - ANTIFAKE_HIGHPASS_CENTER :
                           ANTIFAKE_HIGHPASS_CENTER - encoded;
            texture_count++;
          }
    }
  *texture = texture_count == 0 ?
             0 :
             (int32_t) (((texture_sum + texture_count / 2) *
                         ANTIFAKE_TEXTURE_SCALE) /
                        texture_count);
  return 0;
}

static uint32_t
antifake_integer_sqrt (uint32_t value)
{
  uint32_t root = 0;

  if (value < 2)
    return value;
  for (uint32_t bit = 0x8000, shift = 15; bit != 0; bit >>= 1, shift--)
    {
      uint32_t candidate = (bit + root * 2) << shift;

      if (candidate <= value)
        {
          root += bit;
          value -= candidate;
        }
    }
  return root;
}

int
goodix_milan_antifake_block_variation (
  const uint16_t *residual,
  const uint8_t  *mask,
  size_t          rows,
  size_t          columns,
  size_t          block_size,
  int32_t        *variation)
{
  uint32_t sum = 0;
  uint32_t square_sum = 0;
  uint32_t valid_count = 0;

  if (!residual || !mask || !variation || block_size == 0 ||
      rows / block_size < 1 || columns / block_size < 1 ||
      columns > SIZE_MAX / rows)
    return -1;

  size_t block_rows = rows / block_size - 1;
  size_t block_columns = columns / block_size - 1;
  size_t start_row = (rows - block_rows * block_size) / 2;
  size_t start_column = (columns - block_columns * block_size) / 2;

  for (size_t block_row = 0; block_row < block_rows; block_row++)
    for (size_t block_column = 0; block_column < block_columns; block_column++)
      {
        size_t row0 = start_row + block_row * block_size;
        size_t column0 = start_column + block_column * block_size;
        uint16_t maximum = 0;
        int valid = 1;

        for (size_t y = 0; y < block_size; y++)
          for (size_t x = 0; x < block_size; x++)
            {
              size_t i = (row0 + y) * columns + column0 + x;

              if (maximum <= residual[i])
                maximum = residual[i];
              if (mask[i] == 0)
                valid = 0;
            }
        if (valid)
          {
            sum += maximum;
            square_sum += (uint32_t) maximum * maximum;
            valid_count++;
          }
      }

  uint32_t mean = 0;
  int32_t mean_square = 0;
  if (valid_count != 0)
    {
      uint32_t rounded_square_sum = square_sum + valid_count / 2;

      mean = (sum + valid_count / 2) / valid_count;
      mean_square = goodix_milan_transform_s32 (rounded_square_sum) /
                    (int32_t) valid_count;
    }
  uint32_t variance = (uint32_t) mean_square - mean * mean;
  if ((int32_t) variance < 0)
    variance = 0;
  uint32_t scaled =
    antifake_integer_sqrt (variance) * ANTIFAKE_VARIATION_Q12_ONE;

  *variation = mean == 0 ? (int32_t) scaled :
               (int32_t) ((scaled + mean / 2) / mean);
  return 0;
}
