/*
 * Goodix 53x5 driver for libfprint - Milan match admission predicates
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/milan.h"
#include "milan/print.h"

#include <string.h>

enum {
  AFFINE_XX,
  AFFINE_XY,
  AFFINE_X_OFFSET,
  AFFINE_YX,
  AFFINE_YY,
  AFFINE_Y_OFFSET,
  AFFINE_WORDS,
};

#define AFFINE_Q8_ONE 0x100
#define PROXIMITY_PRIMARY_SENSOR_MASK UINT32_C (0x472010)
#define PROXIMITY_ALTERNATE_SENSOR_MASK UINT64_C (0xc000000000001ec5)

enum {
  METRIC_PRIMARY_COUNT = 0,
  METRIC_FILTERED_COUNT = 1,
  METRIC_OVERLAP_SCORE = 4,
  METRIC_DETAIL = 5,
  METRIC_COVERAGE = 9,
  METRIC_TOPOLOGY = 10,
  METRIC_GEOMETRY = 11,
  METRIC_SCALE_PENALTY = 12,
  METRIC_ORTHOGONALITY_PENALTY = 13,
};

enum {
  CONFIG_DETAIL_BASELINE = 0,
  CONFIG_COUNT_SHIFT = 1,
  CONFIG_SENSOR_TYPE = 15,
  CONFIG_AFFINE_PENALTY = 16,
};

#define ADMISSION_THRESHOLD_FAMILY_SIZE 8
#define ADMISSION_FILTERED_FAMILY 0
#define ADMISSION_PRIMARY_FAMILY ADMISSION_THRESHOLD_FAMILY_SIZE
#define ADMISSION_FILTERED_CONFIG_FAMILY (2 * ADMISSION_THRESHOLD_FAMILY_SIZE)
#define ADMISSION_COUNT_BAND_BASE 7
#define ADMISSION_COUNT_BAND_MAX 7
#define ADMISSION_TOPOLOGY_BASELINE 60
#define ADMISSION_TOPOLOGY_STEP 5
#define ADMISSION_COVERAGE_BASELINE 128
#define ADMISSION_COVERAGE_STEP 10
#define ADMISSION_AFFINE_PENALTY_SCALE 4

static int32_t
admission_count_band (int32_t count)
{
  int32_t index = count - ADMISSION_COUNT_BAND_BASE;

  if (index < 0)
    return 0;
  if (index > ADMISSION_COUNT_BAND_MAX)
    return ADMISSION_COUNT_BAND_MAX;
  return index;
}

int
goodix_milan_match_fallback_candidate_eligible (
  int32_t composite_score,
  int32_t score_threshold,
  int32_t quality_metric)
{
  return composite_score > score_threshold && quality_metric > 195;
}

int
goodix_milan_match_transform_proximity (const int32_t transform[6],
                                        int32_t       mode,
                                        uint32_t      sensor_type)
{
  static const int32_t identity[AFFINE_WORDS] = {
    [AFFINE_XX] = AFFINE_Q8_ONE,
    [AFFINE_YY] = AFFINE_Q8_ONE,
  };
  static const int32_t default_limits[AFFINE_WORDS] = {
    15, 11, 0x400, 11, 15, 0x400,
  };
  static const int32_t mode4_limits[AFFINE_WORDS] = {
    38, 38, 0xc00, 38, 38, 0xc00,
  };
  static const int32_t mode5_limits[AFFINE_WORDS] = {
    45, 40, 0xf00, 40, 45, 0xf00,
  };
  static const int32_t alternate_sensor_limits[AFFINE_WORDS] = {
    25, 21, 0x700, 21, 25, 0x700,
  };
  int32_t limits[AFFINE_WORDS];
  int32_t multiplier = 1;

  if (mode < 1 || mode > 5)
    return 0;
  memcpy (limits, default_limits, sizeof (limits));
  if (mode == 3)
    multiplier = 2;
  else if (mode == 4)
    memcpy (limits, mode4_limits, sizeof (limits));
  else if (mode == 5)
    memcpy (limits, mode5_limits, sizeof (limits));
  if (sensor_type < 23 &&
      ((PROXIMITY_PRIMARY_SENSOR_MASK >> sensor_type) & 1) != 0)
    {
      if (mode == 2)
        multiplier = 2;
    }
  else
    {
      if (sensor_type >= 64 ||
          ((PROXIMITY_ALTERNATE_SENSOR_MASK >> sensor_type) & 1) == 0)
        return 0;
      /* Preserve current mode-1 limits for alternate-family modes 4/5. Native
       * uses their wider tables, but profile-9/type-12 cannot reach them. */
      memcpy (limits, alternate_sensor_limits, sizeof (limits));
      multiplier = 1;
    }
  for (size_t i = 0; i < AFFINE_WORDS; i++)
    {
      int32_t delta = transform[i] - identity[i];

      if ((delta < 0 ? -delta : delta) > limits[i] * multiplier)
        return 0;
    }
  return 1;
}

int
goodix_milan_match_initial_flags (
  const int32_t metrics[77],
  int32_t       image_quality,
  int32_t       image_coverage,
  const int32_t configuration[19],
  int32_t      *match_flag,
  int32_t      *candidate_flag,
  int32_t      *optional_flag)
{
  static const int32_t thresholds[24] = {
    0x0fffffff, 0x0fffffff, 0xd1, 0xd0, 0xd0, 0xcf, 0xcf, 0xcf,
    0x0fffffff, 0xce, 0xc9, 0xc4, 0xb9, 0xb9, 0xb9, 0xb9,
    0x0fffffff, 0x0fffffff, 0x0fffffff, 0xcf, 0xcd, 0xcb, 0xc4, 0xbb,
  };
  int32_t policy_thresholds[24];
  int32_t primary_count;
  int32_t filtered_count;
  int32_t detail;
  int32_t coverage;
  int32_t topology;
  int32_t detail_delta;
  int32_t adjusted_detail;
  int32_t filtered_index;
  int32_t primary_threshold_index;
  int32_t adjustment = 0;

  if (!metrics || !configuration || !match_flag || !candidate_flag)
    return -1;
  memcpy (policy_thresholds, thresholds, sizeof (policy_thresholds));
  if (configuration[CONFIG_SENSOR_TYPE] == GOODIX_MILAN_PRINT_SENSOR_TYPE)
    {
      policy_thresholds[2] = 0xdc;
      policy_thresholds[3] = 0xd8;
      policy_thresholds[4] = 0xd7;
      policy_thresholds[5] = 0xd4;
      policy_thresholds[6] = 0xd3;
      policy_thresholds[7] = 0xd2;
    }

  primary_count = metrics[METRIC_PRIMARY_COUNT];
  filtered_count = metrics[METRIC_FILTERED_COUNT];
  detail = metrics[METRIC_DETAIL];
  coverage = metrics[METRIC_COVERAGE];
  topology = metrics[METRIC_TOPOLOGY];
  detail_delta = detail - configuration[CONFIG_DETAIL_BASELINE];
  adjusted_detail = detail - (primary_count < 5 ? 4 : 0);
  if (topology > ADMISSION_TOPOLOGY_BASELINE && primary_count > 4)
    {
      adjustment = 1 + (topology - ADMISSION_TOPOLOGY_BASELINE) /
                   ADMISSION_TOPOLOGY_STEP;
      primary_count += adjustment;
      filtered_count += adjustment;
    }
  if (coverage < ADMISSION_COVERAGE_BASELINE)
    {
      adjustment = 1 + (ADMISSION_COVERAGE_BASELINE - coverage) /
                   ADMISSION_COVERAGE_STEP;
      primary_count -= adjustment;
      filtered_count -= adjustment;
    }

  filtered_index = admission_count_band (filtered_count);
  primary_threshold_index = admission_count_band (
    primary_count - configuration[CONFIG_COUNT_SHIFT]);
  int32_t filtered_threshold_index = admission_count_band (
    filtered_count - configuration[CONFIG_COUNT_SHIFT]);

  if (configuration[CONFIG_AFFINE_PENALTY] != 0 && filtered_count < 11)
    {
      int32_t penalty =
        (metrics[METRIC_SCALE_PENALTY] +
         metrics[METRIC_ORTHOGONALITY_PENALTY]) *
        ADMISSION_AFFINE_PENALTY_SCALE;

      detail_delta -= penalty;
      adjusted_detail -= penalty;
    }
  *candidate_flag = 1;
  if (detail_delta <=
      policy_thresholds[primary_threshold_index + ADMISSION_PRIMARY_FAMILY] &&
      detail_delta <=
      policy_thresholds[filtered_threshold_index +
                        ADMISSION_FILTERED_CONFIG_FAMILY] &&
      primary_count < 14)
    *candidate_flag = 0;

  if (configuration[CONFIG_SENSOR_TYPE] == 7)
    {
      int32_t first_adjust = filtered_count > primary_count && primary_count <= 7 &&
                             metrics[METRIC_ORTHOGONALITY_PENALTY] != 0;
      int32_t second_adjust = coverage <= 100 && filtered_count <= 11 &&
                              topology <= 65;

      adjusted_detail -= (first_adjust * 3 + second_adjust) * 2;
    }
  if (*candidate_flag == 0 || image_quality < 16 || image_coverage < 65 ||
      ((primary_count < 5 && metrics[METRIC_OVERLAP_SCORE] < 235 &&
        adjusted_detail < 217)) ||
      (((adjusted_detail <=
         policy_thresholds[filtered_index + ADMISSION_FILTERED_FAMILY] &&
         (adjusted_detail < 195 || primary_count < 16) &&
         (adjusted_detail < 190 || primary_count < 18) &&
         (filtered_count < 19 || primary_count < 11 || adjusted_detail < 197) &&
         (topology < 61 || detail_delta < 198 || primary_count < 13) &&
         (filtered_count < 17 || primary_count < 11 || detail_delta < 201)))))
    *match_flag = 0;
  else
    *match_flag = 1;

  if (optional_flag)
    {
      if ((primary_count < 21 || detail_delta < 185) && detail_delta < 205 &&
          detail_delta <
          policy_thresholds[primary_threshold_index +
                            ADMISSION_PRIMARY_FAMILY] + 10 &&
          detail_delta <
          policy_thresholds[filtered_threshold_index +
                            ADMISSION_FILTERED_CONFIG_FAMILY] + 10)
        *optional_flag = 1;
      else
        *optional_flag = 0;
    }
  if (*match_flag == 1 && metrics[METRIC_PRIMARY_COUNT] >= 8 &&
      metrics[METRIC_FILTERED_COUNT] >= 12 &&
      metrics[METRIC_GEOMETRY] >= 36)
    (*match_flag)++;
  return 0;
}
