/*
 * Goodix 53x5 driver for libfprint - Milan anti-fake feature records
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/antifake/antifake.h"
#include "milan/feature/feature.h"
#include "milan/private.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int
goodix_milan_antifake_feature_maps (
  const uint16_t *source,
  size_t          rows,
  size_t          columns,
  uint8_t        *dense_orientation,
  uint32_t       *magnitude,
  int16_t        *gradient_orientation)
{
  size_t count;
  uint8_t *frame = NULL;
  int result = -1;

  if (!source || !dense_orientation || !magnitude || !gradient_orientation ||
      rows < 2 || columns < 2 || columns > SIZE_MAX / rows)
    return -1;
  count = rows * columns;
  frame = malloc (count);
  if (!frame)
    goto out;
  for (size_t i = 0; i < count; i++)
    {
      uint32_t value = source[i] / 12;

      frame[i] = (uint8_t) (value > UINT8_MAX ? UINT8_MAX : value);
    }
  if (feature_build_dense_orientation (
        frame, rows, columns, 7, dense_orientation) != 0)
    goto out;
  memset (magnitude, 0, count * sizeof(*magnitude));
  memset (gradient_orientation, 0,
          count * sizeof(*gradient_orientation));
  for (size_t row = 1; row + 1 < rows; row++)
    for (size_t column = 1; column + 1 < columns; column++)
      {
        size_t i = row * columns + column;
        int32_t horizontal =
          ((int32_t) frame[i + 1] - frame[i - 1]) * 0x100;
        int32_t vertical =
          ((int32_t) frame[i + columns] - frame[i - columns]) * 0x100;

        horizontal *= 0x1000;
        vertical *= 0x1000;
        gradient_orientation[i] =
          (int16_t) feature_cordic (vertical, &horizontal);
        magnitude[i] = (uint32_t) (horizontal >> 12);
        if (magnitude[i] > 0x3ffff)
          magnitude[i] = 0x3ffff;
      }
  result = 0;

out:
  free (frame);
  return result;
}

int
goodix_milan_antifake_feature_record (
  const uint8_t            *dense_orientation,
  const uint32_t           *magnitude,
  const int16_t            *gradient_orientation,
  size_t                    rows,
  size_t                    columns,
  int32_t                   x,
  int32_t                   y,
  GoodixMilanFeatureRecord *record)
{
  GoodixMilanFeatureAux auxiliary = { 0 };

  if (!dense_orientation || !magnitude || !gradient_orientation || !record ||
      x < 0 || y < 0 || (size_t) x >= columns || (size_t) y >= rows)
    return -1;
  int32_t degrees = (270 - dense_orientation[(size_t) y * columns +
                                              (size_t) x]) % 180;
  memset (record, 0, sizeof(*record));
  record->refined_x = (int16_t) (x << 8);
  record->refined_y = (int16_t) (y << 8);
  record->orientation = (int16_t) ((degrees * 0x6488) / 360);
  auxiliary.x = x;
  auxiliary.y = y;
  feature_build_descriptor (magnitude, gradient_orientation, rows, columns,
                            0xc117, &auxiliary, record);
  return 0;
}

int
goodix_milan_antifake_feature_update (
  GoodixMilanAntifakeBlob *antifake,
  size_t                   antifake_size,
  const uint16_t          *source,
  size_t                   rows,
  size_t                   columns)
{
  size_t count;
  uint8_t *dense_orientation = NULL;
  uint32_t *magnitude = NULL;
  int16_t *gradient_orientation = NULL;
  uint32_t record_count;
  int result = -1;

  if (!antifake || !source ||
      antifake_size < GOODIX_MILAN_ANTIFAKE_SIZE || rows < 2 || columns < 2 ||
      columns > SIZE_MAX / rows)
    return -1;
  count = rows * columns;
  record_count = goodix_milan_antifake_candidate_count (antifake);
  if (record_count > GOODIX_MILAN_ANTIFAKE_RECORD_CAPACITY ||
      record_count * GOODIX_MILAN_ANTIFAKE_RECORD_SIZE >
        GOODIX_MILAN_ANTIFAKE_RECORDS_SIZE)
    return -1;

  dense_orientation = malloc (count);
  magnitude = malloc (count * sizeof(*magnitude));
  gradient_orientation = malloc (count * sizeof(*gradient_orientation));
  if (!dense_orientation || !magnitude || !gradient_orientation)
    goto out;
  if (goodix_milan_antifake_feature_maps (
        source, rows, columns, dense_orientation, magnitude,
        gradient_orientation) != 0)
    goto out;

  for (size_t i = 0; i < record_count; i++)
    {
      uint8_t *target = goodix_milan_antifake_record (antifake, i);
      int32_t x;
      int32_t y;
      GoodixMilanFeatureRecord record = { 0 };

      x = goodix_milan_antifake_record_x (target);
      y = goodix_milan_antifake_record_y (target);
      if (goodix_milan_antifake_feature_record (
            dense_orientation, magnitude, gradient_orientation, rows, columns,
            x, y, &record) != 0)
        goto out;
      goodix_milan_feature_transform_record ((uint8_t *) &record, 1);
      memcpy (target + GOODIX_MILAN_ANTIFAKE_RECORD_DATA_8_OFFSET,
              (uint8_t *) &record + 40, 16);
      memcpy (target + GOODIX_MILAN_ANTIFAKE_RECORD_DATA_24_OFFSET,
              (uint8_t *) &record + 16, 16);
      memcpy (target + GOODIX_MILAN_ANTIFAKE_RECORD_DATA_40_OFFSET,
              (uint8_t *) &record + 32, 8);
    }
  goodix_milan_antifake_set_pair_score (antifake, -1);
  result = 0;

out:
  free (gradient_orientation);
  free (magnitude);
  free (dense_orientation);
  return result;
}
