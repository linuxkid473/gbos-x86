/*
 * Goodix 53x5 driver for libfprint - Milan template study ordering
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

typedef struct
{
  int32_t active;
  int32_t generation;
  int32_t ordinal;
} GoodixMilanStudyOrderKey;

int
goodix_milan_study_order_key_greater (const GoodixMilanStudyOrderKey *left,
                                      const GoodixMilanStudyOrderKey *right);

void
goodix_milan_study_order_sort (uint32_t                       *order,
                               size_t                          feature_count,
                               const GoodixMilanStudyOrderKey *keys);
