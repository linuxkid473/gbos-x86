/*
 * Goodix 53x5 driver for libfprint - Milan feature records
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/feature/feature.h"

#include <string.h>

/* In-place 56-byte descriptor transform recovered from FUN_180047ae0. */
enum {
  MILAN_FEATURE_RECORD_SIZE = 56,
  MILAN_FEATURE_PAIR_COUNT = 8,
  MILAN_FEATURE_PAIR_OFFSET = 16,
  MILAN_FEATURE_SOURCE_WORD_OFFSET = 32,
  MILAN_FEATURE_REVERSED_WORD_OFFSET = 36,
  MILAN_FEATURE_TAIL_OFFSET = 40,
  MILAN_FEATURE_EXPANDED_TAIL_OFFSET = 48,
  MILAN_FEATURE_PARTITION_MASK = 3,
  MILAN_FEATURE_LEFT_CLASS = 0,
  MILAN_FEATURE_RIGHT_CLASS = 1,
};

static void
milan_feature_expand_record (uint8_t *record)
{
  uint32_t bits;
  uint32_t reversed = 0;

  memcpy (&bits, record + MILAN_FEATURE_SOURCE_WORD_OFFSET, sizeof (bits));
  for (size_t i = 0; i < 32; i++)
    reversed |= ((bits >> i) & 1U) << (31 - i);
  memcpy (record + MILAN_FEATURE_REVERSED_WORD_OFFSET,
          &reversed, sizeof (reversed));
  record[MILAN_FEATURE_EXPANDED_TAIL_OFFSET] =
    record[MILAN_FEATURE_TAIL_OFFSET];
  record[MILAN_FEATURE_EXPANDED_TAIL_OFFSET + 1] =
    (uint8_t) ~record[MILAN_FEATURE_TAIL_OFFSET + 1];
  record[MILAN_FEATURE_EXPANDED_TAIL_OFFSET + 2] =
    (uint8_t) ~record[MILAN_FEATURE_TAIL_OFFSET + 2];
  record[MILAN_FEATURE_EXPANDED_TAIL_OFFSET + 3] =
    record[MILAN_FEATURE_TAIL_OFFSET + 3];
  for (size_t i = 0; i < 4; i++)
    record[MILAN_FEATURE_EXPANDED_TAIL_OFFSET + 4 + i] =
      record[MILAN_FEATURE_TAIL_OFFSET + 7 - i];
}

void
goodix_milan_feature_transform_record (uint8_t *record,
                                       int      reverse_bits)
{
  static const uint8_t swap_order[MILAN_FEATURE_PAIR_COUNT] = {
    0, 1, 1, 0, 1, 0, 0, 1,
  };
  uint8_t first[MILAN_FEATURE_PAIR_COUNT];
  uint8_t second[MILAN_FEATURE_PAIR_COUNT];

  for (size_t i = 0; i < MILAN_FEATURE_PAIR_COUNT; i++)
    {
      uint8_t left = record[MILAN_FEATURE_PAIR_OFFSET + i * 2];
      uint8_t right = record[MILAN_FEATURE_PAIR_OFFSET + i * 2 + 1];
      uint8_t high_left = ((left ^ right) & 0x0f) ^ left;
      uint8_t high_right = ((left ^ right) & 0x0f) ^ right;

      first[i] = swap_order[i] == 0 ? high_right : high_left;
      second[i] = swap_order[i] == 0 ? high_left : high_right;
    }
  memcpy (record + MILAN_FEATURE_PAIR_OFFSET, first, sizeof (first));
  memcpy (record + MILAN_FEATURE_PAIR_OFFSET + MILAN_FEATURE_PAIR_COUNT,
          second, sizeof (second));
  memset (record + MILAN_FEATURE_REVERSED_WORD_OFFSET, 0, sizeof (uint32_t));

  if (reverse_bits)
    milan_feature_expand_record (record);
}

/* Partition Boolean low classes in place. End swaps make this deliberately
 * unstable; low classes 2/3 are outside the producer contract. */
size_t
goodix_milan_feature_partition_records (uint8_t *records,
                                        size_t   record_count)
{
  size_t left = 0;
  size_t right;

  if (record_count == 0)
    return 0;
  right = record_count - 1;
  while (left < right)
    {
      while (left < right &&
             (records[left * MILAN_FEATURE_RECORD_SIZE] &
              MILAN_FEATURE_PARTITION_MASK) == MILAN_FEATURE_LEFT_CLASS)
        left++;
      while (left < right &&
             (records[right * MILAN_FEATURE_RECORD_SIZE] &
              MILAN_FEATURE_PARTITION_MASK) == MILAN_FEATURE_RIGHT_CLASS)
        right--;
      if (left < right)
        {
          uint8_t temporary[MILAN_FEATURE_RECORD_SIZE];

          memcpy (temporary, records + left * MILAN_FEATURE_RECORD_SIZE,
                  sizeof (temporary));
          memcpy (records + left * MILAN_FEATURE_RECORD_SIZE,
                  records + right * MILAN_FEATURE_RECORD_SIZE,
                  sizeof (temporary));
          memcpy (records + right * MILAN_FEATURE_RECORD_SIZE,
                  temporary, sizeof (temporary));
        }
    }
  return left +
         ((records[left * MILAN_FEATURE_RECORD_SIZE] &
           MILAN_FEATURE_PARTITION_MASK) == MILAN_FEATURE_LEFT_CLASS);
}
