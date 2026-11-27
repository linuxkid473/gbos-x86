/*
 * Goodix 53x5 driver for libfprint - Milan anti-fake mask
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/antifake/antifake.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void
antifake_mask_dilate (const uint8_t *source,
                      uint8_t       *output,
                      size_t         rows,
                      size_t         columns)
{
  for (size_t y = 0; y < rows; y++)
    for (size_t x = 0; x < columns; x++)
      {
        size_t i = y * columns + x;
        int active = source[i] != 0 ||
                     (x >= 1 && source[i - 1] != 0) ||
                     (x + 1 < columns && source[i + 1] != 0) ||
                     (y >= 1 && source[i - columns] != 0) ||
                     (y + 1 < rows && source[i + columns] != 0) ||
                     (x >= 2 && source[i - 2] != 0) ||
                     (x + 2 < columns && source[i + 2] != 0) ||
                     (y >= 2 && source[i - columns * 2] != 0) ||
                     (y + 2 < rows && source[i + columns * 2] != 0);

        output[i] = active ? UINT8_MAX : 0;
      }
}

static void
antifake_mask_erode (const uint8_t *source,
                     uint8_t       *output,
                     size_t         rows,
                     size_t         columns)
{
  for (size_t y = 0; y < rows; y++)
    for (size_t x = 0; x < columns; x++)
      {
        size_t i = y * columns + x;
        int inactive = source[i] == 0 ||
                       (x >= 1 && source[i - 1] == 0) ||
                       (x + 1 < columns && source[i + 1] == 0) ||
                       (y >= 1 && source[i - columns] == 0) ||
                       (y + 1 < rows && source[i + columns] == 0) ||
                       (x >= 2 && source[i - 2] == 0) ||
                       (x + 2 < columns && source[i + 2] == 0) ||
                       (y >= 2 && source[i - columns * 2] == 0) ||
                       (y + 2 < rows && source[i + columns * 2] == 0);

        output[i] = inactive ? 0 : UINT8_MAX;
      }
}

int
goodix_milan_antifake_build_mask (
  const uint8_t *feature_mask,
  size_t         feature_mask_size,
  size_t         rows,
  size_t         columns,
  uint8_t       *mask,
  uint8_t       *packed,
  size_t         packed_size)
{
  size_t count;
  size_t cropped_columns;
  size_t last_feature_row;
  size_t last_feature_column;
  size_t maximum_feature_index;
  uint8_t *temporary = NULL;

  if (!feature_mask || !mask || !packed || rows < 2 || columns <= 4 ||
      columns > SIZE_MAX / rows || feature_mask_size == 0)
    return -1;
  count = rows * columns;
  cropped_columns = columns - 4;
  last_feature_row = (rows - 1) / 2;
  last_feature_column = (columns - 3) / 2;
  if (last_feature_row > (SIZE_MAX - last_feature_column) / 52)
    return -1;
  maximum_feature_index = last_feature_row * 52 + last_feature_column;
  if (rows > SIZE_MAX / cropped_columns ||
      feature_mask_size < maximum_feature_index ||
      packed_size < (rows * cropped_columns + 7) / 8)
    return -1;
  temporary = malloc (count);
  if (!temporary)
    return -1;
  memset (mask, 0, count);
  for (size_t y = 0; y < rows; y++)
    for (size_t x = 2; x + 2 < columns; x++)
      {
        size_t feature_index = (y / 2) * 52 + x / 2;

        /* Windows reads data[2288] for the lower-right 2x2 of this exact
         * profile-9 matrix. canonical-zero-v1 defines that one UB source as
         * inactive; every in-range byte remains authoritative. */
        if (feature_index < feature_mask_size
              ? feature_mask[feature_index] != 0
              : 0)
          mask[y * columns + x] = 1;
      }
  antifake_mask_dilate (mask, temporary, rows, columns);
  antifake_mask_erode (temporary, mask, rows, columns);
  antifake_mask_erode (mask, temporary, rows, columns);
  antifake_mask_erode (temporary, mask, rows, columns);
  antifake_mask_erode (mask, temporary, rows, columns);
  memcpy (mask, temporary, count);

  memset (packed, 0, packed_size);
  for (size_t y = 0; y < rows; y++)
    for (size_t x = 2; x + 2 < columns; x++)
      {
        size_t bit = y * cropped_columns + x - 2;

        if (mask[y * columns + x] != 0)
          packed[bit >> 3] |= (uint8_t) (1U << (bit & 7));
      }
  free (temporary);
  return 0;
}
