/*
 * Goodix 53x5 driver for libfprint - Milan transform routing
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/milan.h"
#include "milan/private.h"
#include "milan/relations.h"
#include "milan/transform-private.h"

/* Q8 affine primitives recovered from FUN_1800687c0, FUN_180068860, and
 * FUN_1800689a0. */
enum {
  AFFINE_XX,
  AFFINE_XY,
  AFFINE_X_OFFSET,
  AFFINE_YX,
  AFFINE_YY,
  AFFINE_Y_OFFSET,
  AFFINE_WORDS,
};

#define AFFINE_Q8_SHIFT 8
#define AFFINE_Q16_SHIFT (2 * AFFINE_Q8_SHIFT)
#define AFFINE_Q8_ONE (1 << AFFINE_Q8_SHIFT)

static inline int32_t
goodix_milan_transform_sar64_narrow (uint64_t     value,
                                     unsigned int shift)
{
  return goodix_milan_transform_s32 ((uint32_t) (value >> shift));
}

static inline int32_t
goodix_milan_transform_dot_q8 (int32_t left_first,
                               int32_t right_first,
                               int32_t left_second,
                               int32_t right_second)
{
  return goodix_milan_transform_sar64_narrow (
    (uint64_t) (int64_t) left_first * (uint64_t) (int64_t) right_first +
    (uint64_t) (int64_t) left_second * (uint64_t) (int64_t) right_second,
    AFFINE_Q8_SHIFT);
}

static inline uint32_t
goodix_milan_transform_integer_sqrt (uint32_t value)
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

void
goodix_milan_transform_normalize (int32_t transform[6])
{
  int32_t average = goodix_milan_transform_sar32 (
    (uint32_t) transform[AFFINE_XX] + (uint32_t) transform[AFFINE_YY], 1);
  int32_t rotation = goodix_milan_transform_sar32 (
    (uint32_t) transform[AFFINE_YX] - (uint32_t) transform[AFFINE_XY], 1);
  uint32_t norm = goodix_milan_transform_integer_sqrt (
    (uint32_t) rotation * (uint32_t) rotation +
    (uint32_t) average * (uint32_t) average);

  if (norm == 0)
    {
      transform[AFFINE_XX] = AFFINE_Q8_ONE;
      transform[AFFINE_XY] = 0;
      transform[AFFINE_YX] = 0;
      transform[AFFINE_YY] = AFFINE_Q8_ONE;
      return;
    }
  transform[AFFINE_XX] = goodix_milan_transform_divide (
    (uint64_t) (int64_t) goodix_milan_transform_s32 (
      (uint32_t) (norm >> 1) +
      ((uint32_t) average << AFFINE_Q8_SHIFT)),
    (int32_t) norm);
  transform[AFFINE_YY] = transform[AFFINE_XX];
  transform[AFFINE_XY] = goodix_milan_transform_divide (
    (uint64_t) (int64_t) goodix_milan_transform_s32 (
      (uint32_t) (norm >> 1) -
      ((uint32_t) rotation << AFFINE_Q8_SHIFT)),
    (int32_t) norm);
  transform[AFFINE_YX] = goodix_milan_transform_s32 (
    UINT32_C (0) - (uint32_t) transform[AFFINE_XY]);
}

/* Compose Q8 matrices as output = after * before. Each array encodes
 * [xx xy x_offset; yx yy y_offset; 0 0 256]. Inputs are copied because output
 * may alias either one, and native normalizes the linear part after the product. */
void
goodix_milan_transform_compose (const int32_t after[6],
                                const int32_t before[6],
                                int32_t       output[6])
{
  int32_t after_copy[AFFINE_WORDS];
  int32_t before_copy[AFFINE_WORDS];
  int32_t result[AFFINE_WORDS];

  if (!after || !before || !output)
    return;
  memcpy (after_copy, after, sizeof (after_copy));
  memcpy (before_copy, before, sizeof (before_copy));
  result[AFFINE_XX] = goodix_milan_transform_dot_q8 (
    before_copy[AFFINE_XX], after_copy[AFFINE_XX],
    before_copy[AFFINE_YX], after_copy[AFFINE_XY]);
  result[AFFINE_XY] = goodix_milan_transform_dot_q8 (
    after_copy[AFFINE_XX], before_copy[AFFINE_XY],
    before_copy[AFFINE_YY], after_copy[AFFINE_XY]);
  result[AFFINE_X_OFFSET] = goodix_milan_transform_s32 (
    (uint32_t) goodix_milan_transform_dot_q8 (
      before_copy[AFFINE_X_OFFSET], after_copy[AFFINE_XX],
      before_copy[AFFINE_Y_OFFSET], after_copy[AFFINE_XY]) +
    (uint32_t) after_copy[AFFINE_X_OFFSET]);
  result[AFFINE_YX] = goodix_milan_transform_dot_q8 (
    before_copy[AFFINE_XX], after_copy[AFFINE_YX],
    before_copy[AFFINE_YX], after_copy[AFFINE_YY]);
  result[AFFINE_YY] = goodix_milan_transform_dot_q8 (
    after_copy[AFFINE_YX], before_copy[AFFINE_XY],
    after_copy[AFFINE_YY], before_copy[AFFINE_YY]);
  result[AFFINE_Y_OFFSET] = goodix_milan_transform_s32 (
    (uint32_t) goodix_milan_transform_dot_q8 (
      after_copy[AFFINE_YX], before_copy[AFFINE_X_OFFSET],
      after_copy[AFFINE_YY], before_copy[AFFINE_Y_OFFSET]) +
    (uint32_t) after_copy[AFFINE_Y_OFFSET]);
  goodix_milan_transform_normalize (result);
  memcpy (output, result, sizeof (result));
}

/* Native wraps the determinant at 32 bits and copies wrapped-singular input.
 * Snapshotting all words also permits inversion in place. */
int
goodix_milan_transform_invert (const int32_t transform[6],
                               int32_t       inverse[6])
{
  int32_t input[AFFINE_WORDS];
  int32_t result[AFFINE_WORDS];
  int32_t determinant;
  uint64_t numerator;

  if (!transform || !inverse)
    return -1;
  memcpy (input, transform, sizeof (input));
  determinant = goodix_milan_transform_s32 (
    (uint32_t) input[AFFINE_XX] * (uint32_t) input[AFFINE_YY] -
    (uint32_t) input[AFFINE_XY] * (uint32_t) input[AFFINE_YX]);
  if (determinant == 0)
    {
      memcpy (inverse, input, sizeof (input));
      return 0;
    }
  result[AFFINE_XX] = goodix_milan_transform_divide (
    (uint64_t) (int64_t) input[AFFINE_YY] << AFFINE_Q16_SHIFT,
      determinant);
  result[AFFINE_XY] = goodix_milan_transform_divide (
    (UINT64_C (0) - (uint64_t) (int64_t) input[AFFINE_XY]) <<
      AFFINE_Q16_SHIFT, determinant);
  numerator = (uint64_t) (int64_t) input[AFFINE_Y_OFFSET] *
              (uint64_t) (int64_t) input[AFFINE_XY] -
              (uint64_t) (int64_t) input[AFFINE_YY] *
              (uint64_t) (int64_t) input[AFFINE_X_OFFSET];
  result[AFFINE_X_OFFSET] = goodix_milan_transform_divide (
    numerator << AFFINE_Q8_SHIFT, determinant);
  result[AFFINE_YX] = goodix_milan_transform_divide (
    (UINT64_C (0) - (uint64_t) (int64_t) input[AFFINE_YX]) <<
      AFFINE_Q16_SHIFT, determinant);
  result[AFFINE_YY] = goodix_milan_transform_divide (
    (uint64_t) (int64_t) input[AFFINE_XX] << AFFINE_Q16_SHIFT,
      determinant);
  numerator = (uint64_t) (int64_t) input[AFFINE_YX] *
              (uint64_t) (int64_t) input[AFFINE_X_OFFSET] -
              (uint64_t) (int64_t) input[AFFINE_Y_OFFSET] *
              (uint64_t) (int64_t) input[AFFINE_XX];
  result[AFFINE_Y_OFFSET] = goodix_milan_transform_divide (
    numerator << AFFINE_Q8_SHIFT, determinant);
  memcpy (inverse, result, sizeof (result));
  return 0;
}

void
goodix_milan_transform_route (const int32_t stored[6],
                              const int32_t direct[6],
                              int32_t       feature,
                              int32_t       reference,
                              int32_t       routed[6])
{
  int32_t inverse[6];

  if (feature > reference)
    {
      goodix_milan_transform_compose (stored, direct, routed);
    }
  else if (feature < reference)
    {
      goodix_milan_transform_invert (stored, inverse);
      goodix_milan_transform_compose (inverse, direct, routed);
    }
  else
    {
      memmove (routed, direct, AFFINE_WORDS * sizeof (*routed));
    }
}
