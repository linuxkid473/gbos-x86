/*
 * Goodix 53x5 driver for libfprint - Milan feature masks
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/feature/feature.h"
#include "milan/private.h"

#include <stdint.h>
#include <string.h>

void
goodix_milan_feature_mask_expand (const uint8_t packed[72], uint8_t mask[44 * 52])
{
  for (size_t y = 0; y < 22; y++)
    for (size_t x = 0; x < 26; x++)
      {
        size_t bit_index = y * 26 + x;
        uint8_t value = (packed[bit_index / 8] >> (bit_index & 7)) & 1;

        mask[(y * 2) * 52 + x * 2] = value;
        mask[(y * 2) * 52 + x * 2 + 1] = value;
        mask[(y * 2 + 1) * 52 + x * 2] = value;
        mask[(y * 2 + 1) * 52 + x * 2 + 1] = value;
    }
}
