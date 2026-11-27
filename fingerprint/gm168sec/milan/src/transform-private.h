/*
 * Goodix 53x5 driver for libfprint - Milan fixed-width transform primitives
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <stdint.h>
#include <string.h>

static inline int32_t
goodix_milan_transform_s32 (uint32_t value)
{
  int32_t result;

  memcpy (&result, &value, sizeof(result));
  return result;
}

static inline int32_t
goodix_milan_transform_sar32 (uint32_t value, unsigned int shift)
{
  uint32_t shifted = value >> shift;

  if ((value & UINT32_C (0x80000000)) != 0)
    shifted |= UINT32_MAX << (32 - shift);
  return goodix_milan_transform_s32 (shifted);
}

static inline int32_t
goodix_milan_transform_affine_s32 (int32_t  first,
                                   uint32_t first_coordinate,
                                   int32_t  second,
                                   uint32_t second_coordinate,
                                   uint32_t translation)
{
  uint32_t value = (uint32_t) first * first_coordinate;

  value += (uint32_t) second * second_coordinate;
  value += translation;
  return goodix_milan_transform_s32 (value);
}

static inline int32_t
goodix_milan_transform_divide (uint64_t numerator, int32_t denominator)
{
  uint64_t magnitude;
  uint64_t quotient;
  uint32_t denominator_magnitude;
  int negative = (numerator >> 63) != 0;

  magnitude = negative ? (~numerator + 1) : numerator;
  denominator_magnitude = denominator < 0
                            ? (uint32_t) (-(int64_t) denominator)
                            : (uint32_t) denominator;
  quotient = magnitude / denominator_magnitude;
  if (negative != (denominator < 0))
    quotient = ~quotient + 1;
  return goodix_milan_transform_s32 ((uint32_t) quotient);
}

static inline int
goodix_milan_transform_invert_s32_checked (const int32_t transform[6],
                                           int32_t       inverse[6])
{
  uint64_t numerator[6];
  int32_t determinant = goodix_milan_transform_s32 (
    (uint32_t) transform[0] * (uint32_t) transform[4] -
    (uint32_t) transform[1] * (uint32_t) transform[3]);

  if (determinant == 0)
    {
      memcpy (inverse, transform, 6 * sizeof(*inverse));
      return 0;
    }
  numerator[0] = (uint64_t) (int64_t) transform[4] << 16;
  numerator[1] = (UINT64_C (0) -
                  (uint64_t) (int64_t) transform[1]) << 16;
  numerator[2] =
    ((uint64_t) (int64_t) transform[5] *
       (uint64_t) (int64_t) transform[1] -
     (uint64_t) (int64_t) transform[4] *
       (uint64_t) (int64_t) transform[2]) << 8;
  numerator[3] = (UINT64_C (0) -
                  (uint64_t) (int64_t) transform[3]) << 16;
  numerator[4] = (uint64_t) (int64_t) transform[0] << 16;
  numerator[5] =
    ((uint64_t) (int64_t) transform[3] *
       (uint64_t) (int64_t) transform[2] -
     (uint64_t) (int64_t) transform[5] *
       (uint64_t) (int64_t) transform[0]) << 8;
  for (unsigned int i = 0; i < 6; i++)
    {
      if (determinant == -1 && numerator[i] == (UINT64_C (1) << 63))
        return -1;
      inverse[i] = goodix_milan_transform_divide (
        numerator[i], determinant);
    }
  return 0;
}
