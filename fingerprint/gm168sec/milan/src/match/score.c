/*
 * Goodix 53x5 driver for libfprint - Milan match score finalization
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/milan.h"

/* Finalizes overlap and candidate scores recovered from FUN_1800717e0,
 * FUN_1800731d0, FUN_1800740f0, and FUN_1800608b0. */
enum {
  OVERLAP_CLASS_ZERO = 0,
  OVERLAP_CLASS_ONE = 1,
  OVERLAP_CLASS_TWO = 2,
  OVERLAP_CLASS_MATCH = 3,
};

enum {
  OVERLAP_WEIGHT_HIGH_VALID = 0,
  OVERLAP_WEIGHT_LOW_VALID = 1,
  OVERLAP_WEIGHT_QUALITY = 2,
};

enum {
  MATCH_METRIC_PRIMARY_COUNT = 0,
  MATCH_METRIC_RETAINED_COUNT = 1,
  MATCH_METRIC_OVERLAP_SCORE = 4,
  MATCH_METRIC_OVERLAP_DETAIL = 5,
  MATCH_METRIC_LOW_BITMAP_DETAIL = 8,
  MATCH_METRIC_OVERLAP_COVERAGE = 9,
  MATCH_METRIC_GEOMETRIC_PERCENT = 11,
  MATCH_METRIC_SCALE_PENALTY = 12,
  MATCH_METRIC_ORTHOGONALITY_PENALTY = 13,
  MATCH_METRIC_STRONG_ORTHOGONALITY_PENALTY = 14,
};

#define MATCH_Q8_ONE 0x100
#define OVERLAP_REJECTED_SCORE 128
#define EXTENDED_DENOMINATOR_TYPE_MASK 0x413000U
#define FINAL_SCORE_DEFAULT_DENOMINATOR 31
#define FINAL_SCORE_EXTENDED_DENOMINATOR 42

int32_t
goodix_milan_match_secondary_result (int32_t primary_score,
                                     int32_t secondary_score,
                                     int32_t secondary_detail)
{
  return secondary_score > primary_score && secondary_detail > 195 ?
         secondary_score :
         primary_score;
}

int
goodix_milan_match_overlap_result (
  const int32_t classes[4],
  int32_t       valid_count,
  int32_t       full_count,
  int32_t       mode,
  const int32_t weights[3],
  int32_t       context_count,
  int32_t      *context_confidence,
  int32_t      *score,
  int32_t      *coverage,
  int32_t      *detail)
{
  int32_t raw_score;
  int32_t quality;

  if (coverage)
    *coverage = (full_count / 2 + valid_count * MATCH_Q8_ONE) / full_count;
  if (detail)
    {
      int32_t non_match_total = classes[OVERLAP_CLASS_ZERO] +
                                classes[OVERLAP_CLASS_ONE] +
                                classes[OVERLAP_CLASS_TWO];
      int32_t result = non_match_total > 0 ?
                       (non_match_total / 2 +
                        classes[OVERLAP_CLASS_ZERO] * MATCH_Q8_ONE) /
                       (non_match_total + 1) :
                       0;
      int32_t match_fraction =
        (valid_count / 2 + classes[OVERLAP_CLASS_MATCH] * MATCH_Q8_ONE) /
        (valid_count + 1);

      if (match_fraction < 15)
        result -= ((15 - match_fraction) >> 1) + 3;
      *detail = result;
    }
  if (mode != 0)
    {
      if (*context_confidence < 0)
        {
          *context_confidence = classes[OVERLAP_CLASS_MATCH] * MATCH_Q8_ONE /
                                (classes[OVERLAP_CLASS_ONE] +
                                 classes[OVERLAP_CLASS_TWO] +
                                 classes[OVERLAP_CLASS_MATCH] + 1);
        }
    }
  if (valid_count == 0)
    {
      *score = OVERLAP_REJECTED_SCORE;
      return 0;
    }
  if (full_count / 2 < valid_count)
    {
      raw_score = weights[OVERLAP_WEIGHT_HIGH_VALID] + 38 +
                  (valid_count / 2 +
                   classes[OVERLAP_CLASS_ZERO] * MATCH_Q8_ONE) / valid_count;
    }
  else
    {
      raw_score = (valid_count / 2 +
                   classes[OVERLAP_CLASS_ZERO] * MATCH_Q8_ONE) /
                  (valid_count + 1) +
                  valid_count * 19 / (full_count / 2) +
                  weights[OVERLAP_WEIGHT_LOW_VALID] + 19;
    }
  quality = (valid_count / 2 +
             classes[OVERLAP_CLASS_MATCH] * MATCH_Q8_ONE) / valid_count +
            weights[OVERLAP_WEIGHT_QUALITY];
  if (mode == 0)
    {
      *score = quality > 23 || (raw_score > 230 && quality > 16) ?
               raw_score :
               OVERLAP_REJECTED_SCORE;
      return 0;
    }
  *score = quality > 23 ||
           (quality > 18 && *context_confidence > 62) ||
           (quality > 19 && *context_confidence > 50) ||
           (quality > 17 && ((*context_confidence > 40 &&
                              context_count > 7) ||
                             context_count > 8)) ||
           (raw_score > 230 && quality > 16) ?
           raw_score :
           OVERLAP_REJECTED_SCORE;
  return 0;
}

int
goodix_milan_match_final_score (const int32_t metrics[15],
                                uint32_t      template_type,
                                int           alternate_policy,
                                int32_t      *score)
{
  int32_t overlap_detail;
  int32_t low_bitmap_detail;
  int32_t combined_detail;
  int32_t retained_count;
  int32_t overlap_coverage;
  int32_t score_denominator = FINAL_SCORE_DEFAULT_DENOMINATOR;

  *score = 0;
  if (template_type < 23 &&
      ((EXTENDED_DENOMINATOR_TYPE_MASK >> (template_type & 31)) & 1) != 0)
    score_denominator = FINAL_SCORE_EXTENDED_DENOMINATOR;

  retained_count = metrics[MATCH_METRIC_RETAINED_COUNT];
  overlap_detail = metrics[MATCH_METRIC_OVERLAP_DETAIL];
  low_bitmap_detail = metrics[MATCH_METRIC_LOW_BITMAP_DETAIL];
  overlap_coverage = metrics[MATCH_METRIC_OVERLAP_COVERAGE];
  if (template_type == 21 || template_type == 11)
    {
      int32_t penalty = metrics[MATCH_METRIC_SCALE_PENALTY] +
                        metrics[MATCH_METRIC_ORTHOGONALITY_PENALTY] +
                        metrics[MATCH_METRIC_STRONG_ORTHOGONALITY_PENALTY] +
                        (metrics[MATCH_METRIC_PRIMARY_COUNT] < 5);

      overlap_detail -= penalty * 3;
      low_bitmap_detail -= penalty * 3;
    }
  combined_detail = overlap_detail + low_bitmap_detail;

  if (alternate_policy)
    {
      if (metrics[MATCH_METRIC_PRIMARY_COUNT] < retained_count)
        {
          overlap_detail -= 2;
          low_bitmap_detail -= 2;
        }
      if (retained_count < 8 ||
          overlap_detail + low_bitmap_detail -
          (metrics[MATCH_METRIC_SCALE_PENALTY] +
           metrics[MATCH_METRIC_ORTHOGONALITY_PENALTY] +
           metrics[MATCH_METRIC_STRONG_ORTHOGONALITY_PENALTY]) * 4 < 417 ||
          overlap_coverage < 96)
        return 0;
    }
  else if (template_type == 21 || template_type == 11)
    {
      if (((metrics[MATCH_METRIC_PRIMARY_COUNT] < 5 || retained_count < 8 ||
            combined_detail < 425 ||
            metrics[MATCH_METRIC_GEOMETRIC_PERCENT] < 42) &&
           (metrics[MATCH_METRIC_PRIMARY_COUNT] < 3 ||
            metrics[MATCH_METRIC_OVERLAP_SCORE] < 235 ||
            overlap_coverage < 120 ||
            combined_detail < 400)) &&
          (metrics[MATCH_METRIC_PRIMARY_COUNT] < 6 ||
           overlap_coverage < 61 || combined_detail < 415 ||
           metrics[MATCH_METRIC_GEOMETRIC_PERCENT] < 41))
        return 0;
      *score = (((retained_count * MATCH_Q8_ONE + 15) /
                 FINAL_SCORE_DEFAULT_DENOMINATOR) * 100) >> 8;
      return 0;
    }
  else
    {
      int five_strict = retained_count >= 5 && overlap_detail >= 226 &&
                        low_bitmap_detail >= 176 && overlap_coverage >= 91;
      int five_balanced = retained_count >= 5 && overlap_detail >= 218 &&
                          low_bitmap_detail >= 176 && overlap_coverage >= 121;
      int seven_balanced = retained_count >= 7 && overlap_detail >= 216 &&
                           low_bitmap_detail >= 176 && overlap_coverage >= 121;
      int eight_low_detail = retained_count >= 8 && overlap_detail >= 209 &&
                             low_bitmap_detail >= 191 && overlap_coverage >= 106;
      int nine_combined = retained_count >= 9 && combined_detail >= 416 &&
                          overlap_coverage >= 41;
      int eight_combined = retained_count >= 8 && combined_detail >= 426 &&
                           overlap_coverage >= 41;

      if (!(five_strict || five_balanced || seven_balanced ||
            eight_low_detail || nine_combined || eight_combined))
        return 0;
    }

  *score = ((((score_denominator >> 1) + retained_count * MATCH_Q8_ONE) /
             score_denominator) * 100) >> 8;
  return 0;
}
