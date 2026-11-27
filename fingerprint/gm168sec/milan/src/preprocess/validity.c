/*
 * Goodix 53x5 driver for libfprint - Milan preprocessing validity
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/preprocess/state.h"

#include <stdint.h>

/* Profile-9 validity masks recovered from FUN_1800446f0 and FUN_180073ff0;
 * propagated-mask coverage is consumed by FUN_180044530. */
#define VALIDITY_Q16_ONE UINT32_C (0x10000)
#define VALIDITY_INVALID_SAMPLE UINT8_MAX
#define VALIDITY_MASK_SELECTED UINT8_MAX

int
goodix_milan_preprocess_quality_mask_coverage (const uint8_t *mask,
                                               size_t         count)
{
  size_t propagated_valid = 0;

  for (size_t i = 0; i < count; i++)
    propagated_valid += mask[i] != 0;
  return (int) ((propagated_valid * VALIDITY_Q16_ONE) / count);
}

int
goodix_milan_preprocess_quality_valid_mask (const uint8_t *frame,
                                            size_t         rows,
                                            size_t         columns,
                                            uint8_t       *mask,
                                            int           *valid_score)
{
  size_t count;
  size_t original_valid = 0;

  count = rows * columns;
  for (size_t row = 0; row < rows; row++)
    {
      for (size_t column = 0; column < columns; column++)
        {
          size_t index = row * columns + column;
          int invalid = frame[index] == VALIDITY_INVALID_SAMPLE;

          original_valid += !invalid;
          /* A byte remains excluded only when the center and every in-bounds
           * axial neighbor at distance one or two are invalid. */
          if (invalid &&
              (column < 1 || frame[index - 1] == VALIDITY_INVALID_SAMPLE) &&
              (column + 1 >= columns ||
               frame[index + 1] == VALIDITY_INVALID_SAMPLE) &&
              (row < 1 || frame[index - columns] == VALIDITY_INVALID_SAMPLE) &&
              (row + 1 >= rows ||
               frame[index + columns] == VALIDITY_INVALID_SAMPLE) &&
              (column < 2 || frame[index - 2] == VALIDITY_INVALID_SAMPLE) &&
              (column + 2 >= columns ||
               frame[index + 2] == VALIDITY_INVALID_SAMPLE) &&
              (row < 2 ||
               frame[index - columns * 2] == VALIDITY_INVALID_SAMPLE) &&
              (row + 2 >= rows ||
               frame[index + columns * 2] == VALIDITY_INVALID_SAMPLE))
            mask[index] = 0;
          else
            mask[index] = VALIDITY_MASK_SELECTED;
        }
    }

  *valid_score = (int) ((original_valid * VALIDITY_Q16_ONE) / count);
  return 0;
}
