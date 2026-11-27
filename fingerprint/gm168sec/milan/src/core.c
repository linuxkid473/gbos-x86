/*
 * Goodix 53x5 driver for libfprint - Milan image preprocessing
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/* Produces profile-9/type-12 processed images, quality, and retained state.
 * Native owners are preprocessor, FUN_18006d540, FUN_180067500, and
 * FUN_180069820. */

#include "milan/milan.h"
#include "milan/private.h"
#include "milan/preprocess/gain.h"
#include "milan/preprocess/state.h"
#include "milan/study/order.h"
#include "milan/study/policy.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MILAN_ADC_MAX UINT16_C(0x0fff)
#define MILAN_FIXED_ONE UINT32_C(0x2000)
#define MILAN_PROFILE9_SETUP_OFFSET UINT16_C(7095)
#define MILAN_PROFILE9_UPDATE_BASE UINT16_C(9000)
#define MILAN_PROFILE9_RAW_ADMISSION_SAMPLE_MIN_EXCLUSIVE UINT16_C(100)
#define MILAN_PROFILE9_RAW_ADMISSION_SAMPLE_MAX_EXCLUSIVE UINT16_C(0x0ed8)

enum
{
  MILAN_QUALITY_PERCENT_SCALE = 100,
  MILAN_QUALITY_SCORE_BIN_COUNT = 101,
  MILAN_QUALITY_Q8_ONE = 0x100,
  MILAN_QUALITY_Q8_SHIFT = 8,
  MILAN_QUALITY_Q16_ONE = 0x10000,
  MILAN_QUALITY_Q16_SHIFT = 16,
  MILAN_QUALITY_PATCH_REDUCTION_CEILING = 70,
  MILAN_QUALITY_VALID_COVERAGE_DELTA = 0x3333,
  MILAN_QUALITY_GRADIENT_WINDOW_RECIPROCAL_Q16 = 291,
  MILAN_QUALITY_COVERAGE_THRESHOLD = 0x78,
  MILAN_QUALITY_SELECTION_THRESHOLD = 100,
  MILAN_QUALITY_PATCH_WIDTH = 25,
  MILAN_QUALITY_PATCH_RADIUS = 12,
  MILAN_QUALITY_PATCH_STRIDE = 8,
  MILAN_QUALITY_PATCH_GRADIENT_FLOOR = 25,
  MILAN_QUALITY_PATCH_MINIMUM_VALID_DIVISOR = 2,
  MILAN_QUALITY_PATCH_MINIMUM_SELECTED_DIVISOR = 6,
  MILAN_QUALITY_REFINED_MASK_MARKER = 0x20,
  MILAN_QUALITY_RING_SIZE = 8,
  MILAN_QUALITY_RING_INDEX_MASK = 7,
  MILAN_QUALITY_RING_CONTRAST_DELTA = 4,
  MILAN_QUALITY_RING_TRANSITION_LIMIT = 3,
  MILAN_QUALITY_RING_TARGET_CLASS = 4,
  MILAN_QUALITY_RING_UNCLASSIFIED = 9,
  MILAN_QUALITY_RING_HISTOGRAM_SIZE = 10,
};

static const uint16_t milan_update_kernel[5] = {
  7869, 15328, 19142, 15328, 7869,
};

static const uint16_t milan_quality_gaussian7_kernel[5] = {
  3571, 16004, 26386, 16004, 3571,
};

static const uint16_t milan_quality_gaussian6_kernel[3] = {
  21845, 21845, 21845,
};

static int first_update_frame_core (const uint16_t *normalized_live,
                                    const uint16_t *setup_map,
                                    const uint16_t *application_map,
                                    const uint16_t *application_gain_map,
                                    uint16_t       *persistent_auxiliary_gain,
                                    uint32_t        auxiliary_samples,
                                    GoodixMilanPreprocessState *state,
                                    uint32_t        calibration_ready,
                                    size_t          rows,
                                    size_t          columns,
                                    uint16_t       *gain_map,
                                    uint16_t       *output,
                                    uint16_t       *optional_plane0);
static void profile9_update_source (const uint16_t *normalized_live,
                                    const uint16_t *setup_map,
                                    size_t          count,
                                    uint16_t       *output);
static int profile9_normalize_outer_edges (uint16_t *frame,
                                           size_t    rows,
                                           size_t    columns);
static void milan_profile9_encode_metadata (uint8_t       *image,
                                            const uint8_t *mask,
                                            size_t         rows,
                                            size_t         columns,
                                            int            mode,
                                            int            apply_mask);
static void milan_profile9_make_reciprocal_plane (
  const uint16_t *application_gain,
  size_t          count,
  uint16_t       *output);
static int preprocess_refine_core (const uint16_t *source,
                                   const uint8_t  *mask,
                                   uint16_t        valid_percent,
                                   size_t          rows,
                                   size_t          columns,
                                   uint16_t       *centered,
                                   uint8_t        *output);
static int milan_profile9_post_render (
  GoodixMilanPreprocessState *state,
  const uint16_t             *normalized_live,
  const uint16_t             *setup_map,
  const uint8_t              *render_mask,
  const uint16_t             *working,
  const uint16_t             *optional_plane0,
  const uint16_t             *reciprocal_plane,
  const uint8_t              *selected,
  const uint8_t              *selection_mask,
  int                         selected_refined,
  GoodixMilanPreprocessPurpose purpose,
  uint32_t                    *calibration_ready,
  const uint8_t              **first_metadata_mask,
  uint8_t                    **first_metadata_owned_mask,
  int                         *first_metadata_mode,
  int                         *first_metadata_apply);

static void
profile9_initialize_gain_state (GoodixMilanPreprocessState *state)
{
  const size_t count = GOODIX_MILAN_SENSOR_PIXELS;
  uint32_t sum = 0;
  uint32_t mean;

  if (state->sample_count == 0)
    {
      for (size_t i = 0; i < count; i++)
        {
          state->calibration_map[i] = 0;
          state->auxiliary_gain_map[i] = MILAN_FIXED_ONE;
          state->secondary_auxiliary_gain_map[i] = MILAN_FIXED_ONE;
          state->application_gain_map[i] = MILAN_FIXED_ONE;
        }
      state->auxiliary_sample_count = 0;
      state->application_gain_initialized = 1;
      return;
    }

  if (state->sample_count <= 3)
    {
      for (size_t i = 0; i < count; i++)
        state->application_gain_map[i] = MILAN_FIXED_ONE;
      state->application_gain_initialized = 1;
      return;
    }

  if (state->application_gain_initialized)
    return;

  for (size_t i = 0; i < count; i++)
    sum += state->calibration_map[i];
  mean = (sum + count / 2) / count;
  if (mean == 0)
    {
      state->sample_count = 0;
      state->application_gain_initialized = 1;
      return;
    }

  for (size_t i = 0; i < count; i++)
    state->application_gain_map[i] =
      (uint16_t) (((uint32_t) state->calibration_map[i] * MILAN_FIXED_ONE +
                   mean / 2) /
                  mean);
  state->application_gain_initialized = 1;
}

void
goodix_milan_preprocess_reset (GoodixMilanPreprocessState *state)
{
  if (state)
    {
      memset (state, 0, sizeof(*state));
      for (size_t i = 0; i < GOODIX_MILAN_SENSOR_PIXELS; i++)
        {
          state->auxiliary_gain_map[i] = MILAN_FIXED_ONE;
          state->secondary_auxiliary_gain_map[i] = MILAN_FIXED_ONE;
          state->application_gain_map[i] = MILAN_FIXED_ONE;
        }
    }
}

int
goodix_milan_preprocess (GoodixMilanPreprocessState *state,
                                GoodixMilanProfileState    *profile_state,
                                const uint16_t             *setup_frame,
                                const uint16_t             *live_frame,
                                GoodixMilanPreprocessPurpose purpose,
                                uint8_t                    *output,
                                int                        *quality,
                                int                        *coverage)
{
  const size_t count = GOODIX_MILAN_SENSOR_PIXELS;
  uint16_t *setup_map = NULL;
  uint16_t *normalized_live = NULL;
  uint16_t *difference = NULL;
  uint16_t *gain = NULL;
  uint16_t *working = NULL;
  uint16_t *refined_source = NULL;
  uint16_t *optional_plane0 = NULL;
  uint16_t *reciprocal_plane = NULL;
  uint8_t *mask = NULL;
  uint8_t *contrast = NULL;
  uint8_t *refined = NULL;
  uint8_t *broken_mask = NULL;
  uint8_t *contrast_mask = NULL;
  uint8_t *selection_mask = NULL;
  uint32_t rounded_mean;
  uint16_t threshold;
  uint16_t valid_percent;
  uint16_t refined_percent;
  size_t admitted_pixels = 0;
  size_t refined_pixels = 0;
  uint32_t auxiliary_samples;
  const uint8_t *first_metadata_mask;
  uint8_t *first_metadata_owned_mask = NULL;
  int selected_refined;
  int first_metadata_mode;
  int first_metadata_apply;
  int metadata_mode;
  int apply_metadata_mask;
  int classification_status;
  int post_status;
  int result = -1;

  if (state)
    {
      state->primary_contrast_valid = 0;
      state->extraction_auxiliary_valid = 0;
      state->post_render.primary_metric = 0;
      state->post_render.fallback_metric = 0;
      state->post_render.disagreement = 0;
      state->post_render.component_score = 0;
      state->post_render.component_flag = 0;
      state->post_render.quality_gate = 0;
      state->post_render.update_applied = 0;
      state->post_render.status = 0;
    }
  if (!state || !profile_state || !setup_frame || !live_frame ||
      (purpose != GOODIX_MILAN_PURPOSE_IDENTIFY &&
       purpose != GOODIX_MILAN_PURPOSE_ENROLL) ||
      !output || !quality || !coverage)
    return -1;
  *quality = 0;
  *coverage = 0;
  setup_map = malloc (count * sizeof(*setup_map));
  normalized_live = malloc (count * sizeof(*normalized_live));
  difference = malloc (count * sizeof(*difference));
  gain = malloc (count * sizeof(*gain));
  working = malloc (count * sizeof(*working));
  refined_source = malloc (count * sizeof(*refined_source));
  optional_plane0 = malloc (count * sizeof(*optional_plane0));
  reciprocal_plane = malloc (count * sizeof(*reciprocal_plane));
  mask = malloc (count);
  contrast = malloc (count);
  refined = malloc (count);
  broken_mask = malloc (count);
  contrast_mask = malloc (count);
  selection_mask = malloc (count);
  if (!setup_map || !normalized_live || !difference || !gain || !working ||
      !refined_source || !optional_plane0 || !reciprocal_plane || !mask ||
      !contrast || !refined || !broken_mask || !contrast_mask ||
      !selection_mask)
    goto out;

  memcpy (setup_map, setup_frame, count * sizeof(*setup_map));
  memcpy (normalized_live, live_frame, count * sizeof(*normalized_live));
  for (size_t i = 0; i < count; i++)
    {
      if (setup_map[i] > MILAN_ADC_MAX)
        setup_map[i] = MILAN_ADC_MAX;
    }
  if (!goodix_milan_preprocess_clamp_and_admit_raw (
        normalized_live, GOODIX_MILAN_SENSOR_ROWS,
        GOODIX_MILAN_SENSOR_COLUMNS, 2, 15))
    {
      result = GOODIX_MILAN_PREPROCESS_RETRY_RAW_ADMISSION;
      goto out;
    }
  if (goodix_milan_preprocess_make_setup_map (
        setup_map, GOODIX_MILAN_SENSOR_ROWS, GOODIX_MILAN_SENSOR_COLUMNS,
        setup_map, &rounded_mean) != 0 ||
      profile9_normalize_outer_edges (
        normalized_live, GOODIX_MILAN_SENSOR_ROWS,
        GOODIX_MILAN_SENSOR_COLUMNS) != 0 ||
      goodix_milan_preprocess_build_mask (
        normalized_live, setup_map, GOODIX_MILAN_SENSOR_ROWS,
        GOODIX_MILAN_SENSOR_COLUMNS, mask, &threshold, &valid_percent) != 0 ||
      goodix_milan_preprocess_no_update_frame (
        normalized_live, setup_map, GOODIX_MILAN_SENSOR_ROWS,
        GOODIX_MILAN_SENSOR_COLUMNS, difference) != 0 ||
      goodix_milan_profile9_build_contrast_mask (
        normalized_live, setup_map, GOODIX_MILAN_SENSOR_ROWS,
        GOODIX_MILAN_SENSOR_COLUMNS, contrast_mask, &admitted_pixels) != 0)
    goto out;

  profile9_initialize_gain_state (state);
  milan_profile9_make_reciprocal_plane (
    state->application_gain_map, count, reciprocal_plane);
  goodix_milan_profile9_update_gain_ready (
    state->auxiliary_sample_count, state->sample_count,
    &profile_state->calibration_ready);

  if (admitted_pixels * 10 > count * 9)
    {
      auxiliary_samples = state->auxiliary_sample_count;
      if (first_update_frame_core (
            normalized_live, setup_map, state->calibration_map,
            state->application_gain_map, state->auxiliary_gain_map,
            auxiliary_samples, state, profile_state->calibration_ready,
            GOODIX_MILAN_SENSOR_ROWS, GOODIX_MILAN_SENSOR_COLUMNS, gain,
            working, optional_plane0) != 0)
        goto out;
    }
  else
    {
      /* Native quirk: rejection suppresses map evolution, not persisted gain. */
      profile9_update_source (normalized_live, setup_map, count, working);
      for (size_t i = 0; i < count; i++)
        {
          uint32_t combined_gain = goodix_milan_profile9_combine_gain (
            MILAN_FIXED_ONE, state->application_gain_map[i],
            state->auxiliary_gain_map[i], profile_state->calibration_ready);
          uint32_t secondary_divisor =
            state->sample_count <= 3 || profile_state->calibration_ready != 0
              ? MILAN_FIXED_ONE
              : state->secondary_auxiliary_gain_map[i];

          gain[i] = (uint16_t) combined_gain;
          if (state->calibration_map[i] != 0)
            {
              optional_plane0[i] = secondary_divisor == 0
                                     ? (uint16_t) ((uint32_t) working[i] << 14)
                                     : (uint16_t) (((uint32_t) working[i] *
                                                     (MILAN_FIXED_ONE * 2) +
                                                   secondary_divisor / 2) /
                                                  secondary_divisor);
              working[i] = combined_gain == 0
                             ? (uint16_t) ((uint32_t) working[i] << 14)
                             : (uint16_t) (((uint32_t) working[i] *
                                             (MILAN_FIXED_ONE * 2) +
                                           combined_gain / 2) /
                                          combined_gain);
            }
          else
            optional_plane0[i] = working[i];
        }
    }
  for (size_t i = 0; i < count; i++)
    refined_pixels += contrast_mask[i] != 0;
  refined_percent = (uint16_t) (refined_pixels * 100 / count);

  if (goodix_milan_preprocess_contrast (
        working, contrast_mask, GOODIX_MILAN_SENSOR_ROWS,
        GOODIX_MILAN_SENSOR_COLUMNS, contrast) != 0 ||
      goodix_milan_preprocess_selection_mask (
        contrast, GOODIX_MILAN_SENSOR_ROWS, GOODIX_MILAN_SENSOR_COLUMNS,
        selection_mask) != 0 ||
      preprocess_refine_core (
        working, contrast_mask, refined_percent, GOODIX_MILAN_SENSOR_ROWS,
        GOODIX_MILAN_SENSOR_COLUMNS, refined_source, refined) != 0 ||
      goodix_milan_preprocess_select_output (
        contrast, refined, selection_mask, GOODIX_MILAN_SENSOR_ROWS,
        GOODIX_MILAN_SENSOR_COLUMNS, 800, output, &selected_refined) != 0)
    goto out;
  post_status = milan_profile9_post_render (
    state, normalized_live, setup_map, contrast_mask, working,
    optional_plane0, reciprocal_plane, output, selection_mask,
    selected_refined, purpose,
    &profile_state->calibration_ready, &first_metadata_mask,
    &first_metadata_owned_mask,
    &first_metadata_mode, &first_metadata_apply);
  if (post_status < 0)
    goto out;
  /* The quality arm republishes the selected candidate before metadata. */
  if (state->post_render.quality_gate && selected_refined)
    memcpy (contrast, output, count);
  milan_profile9_encode_metadata (
    output, first_metadata_mask, GOODIX_MILAN_SENSOR_ROWS,
    GOODIX_MILAN_SENSOR_COLUMNS, first_metadata_mode, first_metadata_apply);
  free (first_metadata_owned_mask);
  first_metadata_owned_mask = NULL;
  classification_status = goodix_milan_profile9_build_broken_mask (
    state, difference, setup_map, normalized_live, contrast_mask,
    GOODIX_MILAN_SENSOR_ROWS, GOODIX_MILAN_SENSOR_COLUMNS, broken_mask,
    NULL, &metadata_mode, &apply_metadata_mask);
  if (classification_status != 0 &&
      classification_status != GOODIX_MILAN_PREPROCESS_RETRY_CLASSIFICATION)
    goto out;
  milan_profile9_encode_metadata (
    output, broken_mask, GOODIX_MILAN_SENSOR_ROWS,
    GOODIX_MILAN_SENSOR_COLUMNS, metadata_mode, apply_metadata_mask);
  if (
      goodix_milan_preprocess_quality (
        output, GOODIX_MILAN_SENSOR_ROWS, GOODIX_MILAN_SENSOR_COLUMNS,
        quality, coverage) != 0)
    goto out;
  if (*coverage <= 5)
    *quality = 0;
  memcpy (state->setup_map, setup_map, count * sizeof(*setup_map));
  memcpy (state->primary_contrast, selected_refined ? contrast : output, count);
  state->primary_contrast_valid = 1;
  state->selected_refined = selected_refined;
  result = classification_status == GOODIX_MILAN_PREPROCESS_RETRY_CLASSIFICATION
             ? classification_status : post_status;

out:
  free (first_metadata_owned_mask);
  free (selection_mask);
  free (contrast_mask);
  free (broken_mask);
  free (refined);
  free (contrast);
  free (mask);
  free (reciprocal_plane);
  free (optional_plane0);
  free (refined_source);
  free (working);
  free (gain);
  free (difference);
  free (normalized_live);
  free (setup_map);
  return result;
}

static int
is_invalid_border_sample (uint16_t sample)
{
  return sample == 0 || sample == MILAN_ADC_MAX;
}

int
goodix_milan_preprocess_clamp_and_admit_raw (uint16_t *frame,
                                              size_t    rows,
                                              size_t    columns,
                                              size_t    border,
                                              unsigned  required_percent)
{
  size_t samples = 0;
  size_t admitted = 0;

  if (!frame || required_percent > 100 || border > rows / 2 ||
      border > columns / 2 || rows - border <= border ||
      columns - border <= border || columns > SIZE_MAX / rows)
    return 0;

  for (size_t i = 0; i < rows * columns; i++)
    if (frame[i] > MILAN_ADC_MAX)
      frame[i] = MILAN_ADC_MAX;
  for (size_t row = border; row < rows - border; row++)
    for (size_t column = border; column < columns - border; column++)
      {
        uint16_t sample = frame[row * columns + column];

        samples++;
        admitted += sample > MILAN_PROFILE9_RAW_ADMISSION_SAMPLE_MIN_EXCLUSIVE &&
                    sample < MILAN_PROFILE9_RAW_ADMISSION_SAMPLE_MAX_EXCLUSIVE;
      }
  return admitted > (samples / 100) * required_percent +
                    ((samples % 100) * required_percent) / 100;
}

int
goodix_milan_preprocess_normalize (uint16_t *frame,
                               size_t    rows,
                               size_t    columns)
{
  size_t count;

  if (!frame || rows < 4 || columns < 4 || columns > SIZE_MAX / rows)
    return -1;

  count = rows * columns;
  for (size_t i = 0; i < count; i++)
    {
      if (frame[i] > MILAN_ADC_MAX)
        frame[i] = MILAN_ADC_MAX;
    }

  for (size_t column = 1; column < columns - 1; column++)
    {
      size_t top_inner = columns + column;
      size_t bottom_inner = (rows - 2) * columns + column;

      if (is_invalid_border_sample (frame[top_inner]))
        frame[top_inner] = frame[top_inner + columns];
      if (is_invalid_border_sample (frame[bottom_inner]))
        frame[bottom_inner] = frame[bottom_inner - columns];

      frame[column] = frame[top_inner];
      frame[(rows - 1) * columns + column] = frame[bottom_inner];
    }

  for (size_t row = 0; row < rows; row++)
    {
      size_t start = row * columns;

      if (is_invalid_border_sample (frame[start + 1]))
        frame[start + 1] = frame[start + 2];
      if (is_invalid_border_sample (frame[start + columns - 2]))
        frame[start + columns - 2] = frame[start + columns - 3];

      frame[start] = frame[start + 1];
      frame[start + columns - 1] = frame[start + columns - 2];
    }

  return 0;
}

static int
profile9_normalize_outer_edges (uint16_t *frame,
                                size_t    rows,
                                size_t    columns)
{
  size_t count;

  if (rows < 3 || columns < 2 || columns > SIZE_MAX / rows)
    return -1;

  count = rows * columns;
  for (size_t i = 0; i < count; i++)
    if (frame[i] > MILAN_ADC_MAX)
      frame[i] = MILAN_ADC_MAX;

  /* Native quirk: validated frames copy outer edges but preserve inner edges. */
  memcpy (frame, frame + columns, columns * sizeof(*frame));
  memcpy (frame + (rows - 1) * columns, frame + (rows - 2) * columns,
          columns * sizeof(*frame));
  for (size_t row = 0; row < rows; row++)
    {
      size_t start = row * columns;

      frame[start] = frame[start + 1];
      frame[start + columns - 1] = frame[start + columns - 2];
    }

  return 0;
}

int
goodix_milan_preprocess_make_setup_map (const uint16_t *frame,
                                    size_t          rows,
                                    size_t          columns,
                                    uint16_t       *setup_map,
                                    uint32_t       *rounded_mean)
{
  size_t count;
  uint64_t count64;
  uint64_t sum = 0;

  if (columns == 0 || rows > SIZE_MAX / columns)
    return -1;

  count = rows * columns;
  if (count > SIZE_MAX / sizeof(*setup_map))
    return -1;
  count64 = count;
  if (count64 > (UINT64_MAX - count64 / 2) / MILAN_ADC_MAX)
    return -1;

  memmove (setup_map, frame, count * sizeof(*setup_map));
  if (profile9_normalize_outer_edges (setup_map, rows, columns) != 0)
    return -1;

  for (size_t i = 0; i < count; i++)
    sum += setup_map[i];

  *rounded_mean = (uint32_t) ((sum + count64 / 2) / count64);
  for (size_t i = 0; i < count; i++)
    setup_map[i] += MILAN_PROFILE9_SETUP_OFFSET;
  return 0;
}

int
goodix_milan_preprocess_build_mask (const uint16_t *normalized_live,
                                const uint16_t *setup_map,
                                size_t          rows,
                                size_t          columns,
                                uint8_t         *mask,
                                uint16_t        *threshold,
                                uint16_t        *valid_percent)
{
  size_t count;
  int difference_count = 0;
  int difference_sum = 0;
  int invalid_count = 0;
  int selected_threshold = 50;

  if (rows == 0 || columns == 0 ||
      columns > SIZE_MAX / rows)
    return -1;

  count = rows * columns;
  if (count > INT16_MAX || columns > INT_MAX / 10)
    return -1;

  for (size_t i = 0; i < count; i++)
    {
      int difference = (int) setup_map[i] - (int) normalized_live[i];

      if (difference > 50)
        {
          difference_count++;
          difference_sum += difference;
        }
    }

  if (difference_count > 0)
    {
      int learned_threshold = (difference_sum / difference_count) / 5;

      if (difference_count > (int) columns * 10 ||
          learned_threshold > selected_threshold)
        selected_threshold = learned_threshold;
    }

  for (size_t i = 0; i < count; i++)
    {
      int difference = (int) setup_map[i] - (int) normalized_live[i];
      int valid = 1;

      if (difference < selected_threshold)
        {
          valid = 0;
          invalid_count++;
        }
      if (normalized_live[i] == MILAN_ADC_MAX)
        {
          valid = 0;
          invalid_count++;
        }

      mask[i] = valid ? UINT8_MAX : 0;
    }

  *threshold = (uint16_t) selected_threshold;
  *valid_percent =
    (uint16_t) (((int) count - invalid_count) * 100 / (int) count);
  return 0;
}

int
goodix_milan_preprocess_no_update_frame (const uint16_t *normalized_live,
                                     const uint16_t *setup_map,
                                     size_t          rows,
                                     size_t          columns,
                                     uint16_t       *output)
{
  size_t count;

  if (rows != 0 && columns > SIZE_MAX / rows)
    return -1;

  count = rows * columns;
  for (size_t i = 0; i < count; i++)
    output[i] = setup_map[i] > normalized_live[i]
                  ? setup_map[i] - normalized_live[i]
                  : 0;

  return 0;
}

static void
update_gaussian_core (const uint16_t *source,
                      size_t          rows,
                      size_t          columns,
                      uint16_t       *horizontal,
                      uint16_t       *output)
{
  for (size_t row = 0; row < rows; row++)
    {
      for (size_t column = 0; column < columns; column++)
        {
          uint64_t sum = 0;

          for (size_t tap = 0; tap < 5; tap++)
            {
              size_t x = goodix_milan_reflect101_index (
                (ptrdiff_t) column + (ptrdiff_t) tap - 2, columns);

              sum += (uint64_t) source[row * columns + x] *
                     milan_update_kernel[tap];
            }
          horizontal[row * columns + column] = (uint16_t) (sum >> 16);
        }
    }

  for (size_t row = 0; row < rows; row++)
    {
      for (size_t column = 0; column < columns; column++)
        {
          uint64_t sum = 0;

          for (size_t tap = 0; tap < 5; tap++)
            {
              size_t y = goodix_milan_reflect101_index (
                (ptrdiff_t) row + (ptrdiff_t) tap - 2, rows);

              sum += (uint64_t) horizontal[y * columns + column] *
                     milan_update_kernel[tap];
            }
          output[row * columns + column] = (uint16_t) (sum >> 16);
        }
    }
}

static uint16_t
median3 (uint16_t a,
         uint16_t b,
         uint16_t c)
{
  uint16_t lower = a < b ? a : b;
  uint16_t upper = a > b ? a : b;
  uint16_t capped = upper < c ? upper : c;

  return lower > capped ? lower : capped;
}

static void
median3x3_core (const uint16_t *source,
                size_t          rows,
                size_t          columns,
                uint16_t       *output)
{
  memcpy (output, source, rows * columns * sizeof(*source));
  for (size_t row = 1; row + 1 < rows; row++)
    {
      for (size_t column = 1; column + 1 < columns; column++)
        {
          uint16_t row_medians[3];

          for (ptrdiff_t row_offset = -1; row_offset <= 1; row_offset++)
            {
              size_t index = (row + row_offset) * columns + column;

              row_medians[row_offset + 1] =
                median3 (source[index - 1], source[index], source[index + 1]);
            }

          output[row * columns + column] =
            median3 (row_medians[0], row_medians[1], row_medians[2]);
        }
    }
}

static int
profile9_calibration_admit (const uint16_t             *source,
                            size_t                      rows,
                            size_t                      columns,
                            GoodixMilanPreprocessState *state)
{
  size_t coarse_rows = rows / 2;
  size_t coarse_columns = columns / 2;
  int refresh_reference = state->auxiliary_sample_count == 0;

  if (state->auxiliary_sample_count != 0)
    {
      uint64_t difference_sum = 0;

      for (size_t row = 0; row < coarse_rows; row++)
        for (size_t column = 0; column < coarse_columns; column++)
          {
            uint16_t current =
              source[(row * 2) * columns + column * 2];
            uint16_t previous =
              state->coarse_reference[row * coarse_columns + column];

            difference_sum += current > previous ? current - previous
                                                  : previous - current;
          }

      if (difference_sum / (coarse_rows * coarse_columns) < 30)
        state->stable_count++;
      else
        {
          state->stable_count = 0;
          refresh_reference = 1;
        }
    }
  else
    {
      state->stable_count = 0;
    }

  if (refresh_reference)
    for (size_t row = 0; row < coarse_rows; row++)
      for (size_t column = 0; column < coarse_columns; column++)
        state->coarse_reference[row * coarse_columns + column] =
          source[(row * 2) * columns + column * 2];

  /* Native increments a wrapping dword, then compares it as signed. */
  if ((int32_t) state->stable_count > 3)
    return 0;

  return 1;
}

static void
profile9_update_calibration (const uint32_t             *ratio,
                             size_t                      count,
                             GoodixMilanPreprocessState *state)
{
  uint32_t sum = 0;
  uint32_t map_sum = 0;
  uint32_t mean;
  uint32_t scale;

  for (size_t i = 0; i < count; i++)
    sum += ratio[i];
  mean = (sum + count / 2) / count;
  if (mean == 0)
    mean = 8000;
  scale = (UINT32_C(8000) * MILAN_FIXED_ONE + mean / 2) / mean;

  for (size_t i = 0; i < count; i++)
    {
      uint32_t candidate =
        (uint32_t) (((uint64_t) ratio[i] * scale + MILAN_FIXED_ONE / 2) /
                    MILAN_FIXED_ONE);
      int32_t deviation = (int32_t) candidate - 8000;

      if (deviation < 0)
        deviation = -deviation;
      if (deviation < 6400)
        {
          uint32_t divisor = state->sample_count + 1;

          state->calibration_map[i] =
            (uint16_t) (((divisor >> 1) +
                         (uint32_t) state->calibration_map[i] *
                           state->sample_count + candidate) /
                        divisor);
        }
      map_sum += state->calibration_map[i];
    }

  state->sample_count++;
  if (state->sample_count > 400)
    state->sample_count = 400;

  if (state->sample_count > 3)
    {
      uint32_t map_mean = (map_sum + count / 2) / count;

      if (map_mean != 0)
        for (size_t i = 0; i < count; i++)
          state->application_gain_map[i] =
            (uint16_t) (((uint32_t) state->calibration_map[i] *
                           MILAN_FIXED_ONE + map_mean / 2) /
                        map_mean);
    }

}

static void
profile9_update_source (const uint16_t *normalized_live,
                        const uint16_t *setup_map,
                        size_t          count,
                        uint16_t       *output)
{
  for (size_t i = 0; i < count; i++)
    {
      uint16_t normalized_setup =
        setup_map[i] >= MILAN_PROFILE9_SETUP_OFFSET
          ? setup_map[i] - MILAN_PROFILE9_SETUP_OFFSET
          : 0;
      int32_t value =
        MILAN_PROFILE9_UPDATE_BASE +
        3 * ((int32_t) normalized_setup - (int32_t) normalized_live[i]);

      output[i] = (uint16_t) (value <= 0
                                ? 0
                                : value <= UINT16_MAX ? value : UINT16_MAX);
    }
}

static void
profile9_update_auxiliary_map (uint16_t       *map,
                               uint32_t        samples,
                               const uint16_t *adjusted,
                               const uint16_t *gaussian,
                               size_t          count)
{
  for (size_t i = 0; i < count; i++)
    {
      uint16_t candidate = MILAN_FIXED_ONE;
      int32_t deviation;

      if (gaussian[i] != 0)
        candidate =
          ((uint32_t) adjusted[i] * MILAN_FIXED_ONE + gaussian[i] / 2) /
          gaussian[i];
      deviation = (int32_t) candidate - (int32_t) MILAN_FIXED_ONE;

      if (deviation < 0)
        deviation = -deviation;
      if (deviation < 328)
        {
          uint32_t divisor = samples + 1;

          map[i] =
            (uint16_t) (((divisor >> 1) + (uint32_t) map[i] * samples +
                         candidate) /
                        divisor);
        }
    }
}

static void
profile9_build_temporary_gain (const uint16_t *difference,
                               const uint16_t *median,
                               size_t          count,
                               uint16_t       *gain_map,
                               uint32_t       *ratio,
                               uint16_t       *adjusted)
{
  for (size_t i = 0; i < count; i++)
    {
      uint32_t gain = MILAN_FIXED_ONE;

      if (difference[i] != 0 && median[i] != 0)
        {
          int32_t deviation = (int32_t) difference[i] - (int32_t) median[i];

          if (deviation < 0)
            deviation = -deviation;
          if (deviation > 1800)
            gain = ((uint32_t) difference[i] * MILAN_FIXED_ONE +
                    median[i] / 2) /
                   median[i];
          if (gain > INT16_MAX)
            gain = INT16_MAX;
        }

      gain_map[i] = (uint16_t) gain;
      ratio[i] = gain != 0 ?
                 ((uint32_t) difference[i] * MILAN_FIXED_ONE + gain / 2) /
                 gain :
                 (uint32_t) difference[i] << 13;
      adjusted[i] = (uint16_t) ratio[i];
    }
}

static void
profile9_update_calibration_state (
  const uint16_t             *difference,
  const uint32_t             *ratio,
  const uint16_t             *adjusted,
  const uint16_t             *gaussian,
  const uint16_t             *application_gain_map,
  uint32_t                    calibration_ready,
  size_t                      rows,
  size_t                      columns,
  uint16_t                   *horizontal_scratch,
  uint16_t                   *gain_map,
  uint16_t                   *optional_plane0,
  uint16_t                   *application_adjusted,
  uint16_t                   *application_gaussian,
  GoodixMilanPreprocessState *state)
{
  size_t count = rows * columns;
  int calibration_admitted =
    profile9_calibration_admit (difference, rows, columns, state);

  if (calibration_admitted)
    {
      for (size_t i = 0; i < count; i++)
        {
          uint32_t combined_gain =
            ((uint32_t) gain_map[i] * application_gain_map[i] + 0x1000) >> 13;
          uint32_t value =
            combined_gain != 0 ?
            ((uint32_t) difference[i] * MILAN_FIXED_ONE +
             combined_gain / 2) /
            combined_gain :
            (uint32_t) difference[i] << 13;

          application_adjusted[i] = (uint16_t) value;
        }
      update_gaussian_core (application_adjusted, rows, columns,
                            horizontal_scratch, application_gaussian);
      profile9_update_auxiliary_map (
        state->auxiliary_gain_map, state->auxiliary_sample_count,
        application_adjusted, application_gaussian, count);
      profile9_update_auxiliary_map (
        state->secondary_auxiliary_gain_map, state->auxiliary_sample_count,
        adjusted, gaussian, count);
      if (state->auxiliary_sample_count < 30)
        state->auxiliary_sample_count++;
      state->update_state = 1;
    }
  if (optional_plane0)
    {
      for (size_t i = 0; i < count; i++)
        optional_plane0[i] =
          state->sample_count <= 3 || calibration_ready != 0 ?
          gain_map[i] :
          (uint16_t) (((uint32_t) gain_map[i] *
                       state->secondary_auxiliary_gain_map[i] +
                       MILAN_FIXED_ONE / 2) >>
                      13);
    }
  for (size_t i = 0; i < count; i++)
    gain_map[i] = goodix_milan_profile9_combine_gain (
      gain_map[i], application_gain_map[i], state->auxiliary_gain_map[i],
      calibration_ready);
  if (calibration_admitted)
    profile9_update_calibration (ratio, count, state);
}

static void
profile9_update_stateless_gain (const uint16_t *adjusted,
                                const uint16_t *gaussian,
                                uint32_t        auxiliary_samples,
                                size_t          count,
                                uint16_t       *persistent_auxiliary_gain,
                                uint16_t       *gain_map,
                                uint16_t       *optional_plane0)
{
  if (persistent_auxiliary_gain)
    profile9_update_auxiliary_map (persistent_auxiliary_gain,
                                   auxiliary_samples, adjusted, gaussian,
                                   count);
  for (size_t i = 0; i < count; i++)
    {
      uint32_t auxiliary_gain = MILAN_FIXED_ONE;

      if (persistent_auxiliary_gain)
        {
          auxiliary_gain = persistent_auxiliary_gain[i];
        }
      else if (gaussian[i] != 0)
        {
          uint32_t candidate =
            ((uint32_t) adjusted[i] * MILAN_FIXED_ONE + gaussian[i] / 2) /
            gaussian[i];
          int32_t deviation =
            (int32_t) candidate - (int32_t) MILAN_FIXED_ONE;

          if (deviation < 0)
            deviation = -deviation;
          if (deviation < 328)
            auxiliary_gain = candidate;
        }

      gain_map[i] =
        (uint16_t) (((uint32_t) gain_map[i] * auxiliary_gain + 0x1000) >> 13);
      if (optional_plane0)
        optional_plane0[i] = gain_map[i];
    }
}

static void
profile9_render_update_source (const uint16_t *difference,
                               const uint16_t *application_map,
                               const uint16_t *gain_map,
                               size_t          count,
                               uint16_t       *output,
                               uint16_t       *optional_plane0)
{
  for (size_t i = 0; i < count; i++)
    {
      uint16_t secondary_divisor = optional_plane0 ? optional_plane0[i] : 0;

      if (application_map && application_map[i] == 0)
        {
          output[i] = difference[i];
          if (optional_plane0)
            optional_plane0[i] = difference[i];
        }
      else
        {
          output[i] = gain_map[i] == 0 ?
                      (uint16_t) ((uint32_t) difference[i] << 14) :
                      (uint16_t) (((uint32_t) difference[i] *
                                   (MILAN_FIXED_ONE * 2) +
                                   gain_map[i] / 2) /
                                  gain_map[i]);
          if (optional_plane0)
            {
              optional_plane0[i] =
                secondary_divisor == 0 ?
                (uint16_t) ((uint32_t) difference[i] << 14) :
                (uint16_t) (((uint32_t) difference[i] *
                             (MILAN_FIXED_ONE * 2) +
                             secondary_divisor / 2) /
                            secondary_divisor);
            }
        }
    }
}

static int
first_update_frame_core (const uint16_t *normalized_live,
                          const uint16_t *setup_map,
                          const uint16_t *application_map,
                          const uint16_t *application_gain_map,
                          uint16_t       *persistent_auxiliary_gain,
                          uint32_t        auxiliary_samples,
                          GoodixMilanPreprocessState *state,
                          uint32_t        calibration_ready,
                          size_t          rows,
                          size_t          columns,
                          uint16_t       *gain_map,
                          uint16_t       *output,
                          uint16_t       *optional_plane0)
{
  size_t count;
  uint32_t *ratio = NULL;
  uint16_t *adjusted = NULL;
  uint16_t *application_adjusted = NULL;
  uint16_t *difference = NULL;
  uint16_t *gaussian = NULL;
  uint16_t *application_gaussian = NULL;
  uint16_t *median = NULL;

  if (gain_map == output || optional_plane0 == gain_map ||
      optional_plane0 == output ||
      rows > PTRDIFF_MAX || columns > PTRDIFF_MAX ||
      (rows != 0 && columns > SIZE_MAX / rows))
    return -1;

  count = rows * columns;
  if (count > SIZE_MAX / sizeof(uint16_t))
    return -1;

  ratio = malloc (count * sizeof(*ratio));
  adjusted = malloc (count * sizeof(*adjusted));
  application_adjusted = state
                           ? malloc (count * sizeof(*application_adjusted))
                           : NULL;
  difference = calloc (count, sizeof(*difference));
  gaussian = malloc (count * sizeof(*gaussian));
  application_gaussian = state
                           ? malloc (count * sizeof(*application_gaussian))
                           : NULL;
  median = malloc (count * sizeof(*median));
  if (!ratio || !adjusted || (state && !application_adjusted) || !difference ||
      !gaussian || (state && !application_gaussian) || !median)
    goto allocation_failed;

  profile9_update_source (normalized_live, setup_map, count, difference);

  median3x3_core (difference, rows, columns, median);
  profile9_build_temporary_gain (
    difference, median, count, gain_map, ratio, adjusted);

  update_gaussian_core (adjusted, rows, columns, output, gaussian);
  if (state)
    {
      profile9_update_calibration_state (
        difference, ratio, adjusted, gaussian, application_gain_map,
        calibration_ready, rows, columns, output, gain_map, optional_plane0,
        application_adjusted, application_gaussian, state);
    }
  else
    {
      profile9_update_stateless_gain (
        adjusted, gaussian, auxiliary_samples, count, persistent_auxiliary_gain,
        gain_map, optional_plane0);
    }

  profile9_render_update_source (
    difference, application_map, gain_map, count, output, optional_plane0);

  free (median);
  free (application_gaussian);
  free (gaussian);
  free (difference);
  free (application_adjusted);
  free (adjusted);
  free (ratio);
  return 0;

allocation_failed:
  free (median);
  free (application_gaussian);
  free (gaussian);
  free (difference);
  free (application_adjusted);
  free (adjusted);
  free (ratio);
  return -1;
}

static int
preprocess_refine_core (const uint16_t *source,
                        const uint8_t  *mask,
                        uint16_t        valid_percent,
                        size_t          rows,
                        size_t          columns,
                        uint16_t       *centered,
                        uint8_t        *output)
{
  for (size_t row = 0; row < rows; row++)
    {
      uint32_t sum = 0;
      size_t contributors = 0;
      size_t start = row * columns;

      for (size_t column = 0; column < columns; column++)
        {
          size_t index = start + column;

          if (valid_percent >= 96 || mask[index] != 0)
            {
              sum += source[index];
              contributors++;
            }
        }

      uint16_t mean = contributors != 0 ? (uint16_t) (sum / contributors) : 0;
      for (size_t column = 0; column < columns; column++)
        {
          size_t index = start + column;
          int value;

          if (valid_percent < 96 && mask[index] == 0)
            value = 5000;
          else
            value = (int16_t) ((int) source[index] - mean + 5000);
          centered[index] = (uint16_t) (value > 0 ? value : 0);
        }
    }

  return goodix_milan_preprocess_contrast (
    centered, mask, rows, columns, output);
}

int
goodix_milan_preprocess_refine (const uint16_t *source,
                                const uint8_t  *mask,
                                uint16_t        valid_percent,
                                size_t          rows,
                                size_t          columns,
                                uint8_t         *output)
{
  size_t count;
  uint16_t *centered;
  int result;

  if (!source || !mask || !output || rows == 0 || columns == 0 ||
      columns > SIZE_MAX / rows)
    return -1;
  count = rows * columns;
  if (count > SIZE_MAX / sizeof(*centered))
    return -1;
  centered = malloc (count * sizeof(*centered));
  if (!centered)
    return -1;
  result = preprocess_refine_core (
    source, mask, valid_percent, rows, columns, centered, output);
  free (centered);
  return result;
}

static int
quality_combine_core (int  raw_coverage,
                      int  valid_score,
                      int  patch_score,
                      int  mask_fraction,
                      int  fraction_cutoff,
                      int  final_adjustment,
                      int  positive_only_adjustment,
                      int *quality,
                      int *coverage)
{
  int result;

  if (!quality || !coverage || raw_coverage < 0 || valid_score < 0 ||
      patch_score < 0 || mask_fraction < 0)
    return -1;

  result = patch_score;
  if (mask_fraction < fraction_cutoff &&
      patch_score < MILAN_QUALITY_PATCH_REDUCTION_CEILING)
    {
      int factor = mask_fraction * MILAN_QUALITY_Q8_ONE / fraction_cutoff;

      result = result * factor >> MILAN_QUALITY_Q8_SHIFT;
      result = result * factor >> MILAN_QUALITY_Q8_SHIFT;
    }

  if (valid_score - raw_coverage > MILAN_QUALITY_VALID_COVERAGE_DELTA &&
      valid_score != 0)
    {
      result = raw_coverage * result / valid_score;
      result = result * raw_coverage / valid_score;
    }

  if (!positive_only_adjustment || result > 0)
    result += final_adjustment;
  if (result < 0)
    result = 0;
  else if (result > MILAN_QUALITY_PERCENT_SCALE)
    result = MILAN_QUALITY_PERCENT_SCALE;

  *quality = result;
  *coverage = raw_coverage * MILAN_QUALITY_PERCENT_SCALE >>
              MILAN_QUALITY_Q16_SHIFT;
  return 0;
}

static void
quality_gaussian_reflect101 (const uint8_t  *source,
                             size_t          rows,
                             size_t          columns,
                             const uint16_t *kernel,
                             size_t          kernel_width,
                             uint16_t       *horizontal,
                             uint8_t        *output)
{
  ptrdiff_t radius = (ptrdiff_t) (kernel_width / 2);

  for (size_t row = 0; row < rows; row++)
    {
      for (size_t column = 0; column < columns; column++)
        {
          uint64_t sum = 0;

          for (size_t tap = 0; tap < kernel_width; tap++)
            {
              size_t x = goodix_milan_reflect101_index (
                (ptrdiff_t) column + (ptrdiff_t) tap - radius, columns);

              sum += ((uint64_t) source[row * columns + x] << 8) *
                     kernel[tap];
            }
          horizontal[row * columns + column] = (uint16_t) (sum >> 16);
        }
    }

  for (size_t row = 0; row < rows; row++)
    {
      for (size_t column = 0; column < columns; column++)
        {
          uint64_t sum = 0;

          for (size_t tap = 0; tap < kernel_width; tap++)
            {
              size_t y = goodix_milan_reflect101_index (
                (ptrdiff_t) row + (ptrdiff_t) tap - radius, rows);

              sum += (uint64_t) horizontal[y * columns + column] *
                     kernel[tap];
            }
          output[row * columns + column] = (uint8_t) ((sum >> 16) >> 8);
        }
    }
}

static void
quality_sobel_half_l1 (const uint8_t *source,
                       size_t         rows,
                       size_t         columns,
                       int16_t       *sobel_x,
                       int16_t       *sobel_y,
                       uint16_t      *gradient)
{
  for (size_t row = 0; row < rows; row++)
    for (size_t column = 0; column < columns; column++)
      {
        size_t index = row * columns + column;

        if (column == 0 || column + 1 == columns)
          {
            sobel_x[index] = 0;
          }
        else
          {
            size_t previous_row =
              goodix_milan_reflect101_index ((ptrdiff_t) row - 1, rows);
            size_t next_row =
              goodix_milan_reflect101_index ((ptrdiff_t) row + 1, rows);
            int previous = source[previous_row * columns + column + 1] -
                           source[previous_row * columns + column - 1];
            int current = source[row * columns + column + 1] -
                          source[row * columns + column - 1];
            int next = source[next_row * columns + column + 1] -
                       source[next_row * columns + column - 1];

            sobel_x[index] = (int16_t) (previous + current * 2 + next);
          }
        if (row == 0 || row + 1 == rows)
          {
            sobel_y[index] = 0;
          }
        else
          {
            size_t previous_column = goodix_milan_reflect101_index (
              (ptrdiff_t) column - 1, columns);
            size_t next_column = goodix_milan_reflect101_index (
              (ptrdiff_t) column + 1, columns);
            int previous = source[(row + 1) * columns + previous_column] -
                           source[(row - 1) * columns + previous_column];
            int current = source[(row + 1) * columns + column] -
                          source[(row - 1) * columns + column];
            int next = source[(row + 1) * columns + next_column] -
                       source[(row - 1) * columns + next_column];

            sobel_y[index] = (int16_t) (previous + current * 2 + next);
          }

        int x = sobel_x[index];
        int y = sobel_y[index];
        gradient[index] = (uint16_t) ((x < 0 ? -x : x) / 2 +
                                      (y < 0 ? -y : y) / 2);
      }
}

static size_t
quality_threshold_gradient_window (const uint16_t *gradient,
                                   size_t          rows,
                                   size_t          columns,
                                   uint16_t        threshold,
                                   uint8_t         mask_value,
                                   uint8_t        *mask)
{
  size_t selected = 0;

  for (size_t row = 0; row < rows; row++)
    {
      uint32_t sum = 0;

      for (ptrdiff_t y_offset = -7; y_offset <= 7; y_offset++)
        {
          size_t y = goodix_milan_reflect101_index (
            (ptrdiff_t) row + y_offset, rows);

          for (ptrdiff_t x_offset = -7; x_offset <= 7; x_offset++)
            {
              size_t x = goodix_milan_reflect101_index (x_offset, columns);

              sum += gradient[y * columns + x];
            }
        }
      for (size_t column = 0; column < columns; column++)
        {
          uint16_t average = (uint16_t) (
            (sum * (uint32_t) MILAN_QUALITY_GRADIENT_WINDOW_RECIPROCAL_Q16) >>
            MILAN_QUALITY_Q16_SHIFT);
          size_t index = row * columns + column;

          mask[index] = average > threshold ? mask_value : 0;
          selected += mask[index] != 0;
          if (column + 1 < columns)
            {
              size_t leaving = goodix_milan_reflect101_index (
                (ptrdiff_t) column - 7, columns);
              size_t entering = goodix_milan_reflect101_index (
                (ptrdiff_t) column + 8, columns);

              /* Keep reflected samples with their original multiplicities. */
              for (ptrdiff_t y_offset = -7; y_offset <= 7; y_offset++)
                {
                  size_t y = goodix_milan_reflect101_index (
                    (ptrdiff_t) row + y_offset, rows);

                  sum -= gradient[y * columns + leaving];
                  sum += gradient[y * columns + entering];
                }
            }
        }
    }
  return selected;
}

static int
quality_coverage_mask_core (const uint8_t *frame,
                            size_t         rows,
                            size_t         columns,
                            uint16_t       threshold,
                            uint8_t        mask_value,
                            uint8_t       *mask,
                            int           *raw_coverage)
{
  size_t count;
  uint16_t *horizontal = NULL;
  uint8_t *blurred = NULL;
  int16_t *sobel_x = NULL;
  int16_t *sobel_y = NULL;
  uint16_t *gradient = NULL;
  size_t selected = 0;

  if (!frame || !mask || !raw_coverage || rows < 2 || columns < 2 ||
      rows > PTRDIFF_MAX || columns > PTRDIFF_MAX ||
      columns > SIZE_MAX / rows)
    return -1;

  count = rows * columns;
  if (count > INT_MAX || count > SIZE_MAX / sizeof(uint16_t))
    return -1;

  horizontal = malloc (count * sizeof(*horizontal));
  blurred = malloc (count);
  sobel_x = malloc (count * sizeof(*sobel_x));
  sobel_y = malloc (count * sizeof(*sobel_y));
  gradient = malloc (count * sizeof(*gradient));
  if (!horizontal || !blurred || !sobel_x || !sobel_y || !gradient)
    goto allocation_failed;

  quality_gaussian_reflect101 (
    frame, rows, columns, milan_quality_gaussian7_kernel,
    sizeof (milan_quality_gaussian7_kernel) /
    sizeof (milan_quality_gaussian7_kernel[0]),
    horizontal, blurred);
  quality_sobel_half_l1 (
    blurred, rows, columns, sobel_x, sobel_y, gradient);
  selected = quality_threshold_gradient_window (
    gradient, rows, columns, threshold, mask_value, mask);

  *raw_coverage = (int) ((selected * (uint32_t) MILAN_QUALITY_Q16_ONE) /
                         count);
  free (gradient);
  free (sobel_y);
  free (sobel_x);
  free (blurred);
  free (horizontal);
  return 0;

allocation_failed:
  free (gradient);
  free (sobel_y);
  free (sobel_x);
  free (blurred);
  free (horizontal);
  return -1;
}

int
goodix_milan_preprocess_quality_coverage_mask (const uint8_t *frame,
                                           size_t         rows,
                                           size_t         columns,
                                           uint8_t       *mask,
                                           int           *raw_coverage)
{
  return quality_coverage_mask_core (
    frame, rows, columns, MILAN_QUALITY_COVERAGE_THRESHOLD, UINT8_MAX,
    mask, raw_coverage);
}

int
goodix_milan_preprocess_selection_mask (const uint8_t *frame,
                                    size_t         rows,
                                    size_t         columns,
                                    uint8_t       *mask)
{
  int ignored_coverage;

  return quality_coverage_mask_core (
    frame, rows, columns, MILAN_QUALITY_SELECTION_THRESHOLD, UINT8_MAX,
    mask, &ignored_coverage);
}

static void
quality_prepare_patch_gradients (const uint8_t *frame,
                                 const uint8_t *valid_mask,
                                 size_t         rows,
                                 size_t         columns,
                                 int32_t       *gradient_x,
                                 int32_t       *gradient_y,
                                 int32_t       *magnitude)
{
  for (size_t row = 1; row + 1 < rows; row++)
    for (size_t column = 1; column + 1 < columns; column++)
      {
        size_t index = row * columns + column;
        int neighborhood_valid = 1;

        for (ptrdiff_t y = -1; y <= 1 && neighborhood_valid; y++)
          for (ptrdiff_t x = -1; x <= 1; x++)
            if (valid_mask[(size_t) ((ptrdiff_t) row + y) * columns +
                           (size_t) ((ptrdiff_t) column + x)] == 0)
              {
                neighborhood_valid = 0;
                break;
              }
        if (!neighborhood_valid)
          continue;

        int x = (int) frame[index + 1] - (int) frame[index - 1];
        int y = (int) frame[index + columns] - (int) frame[index - columns];
        gradient_x[index] = x;
        gradient_y[index] = y;
        magnitude[index] = x * x + y * y;
      }
}

static void
quality_rewrite_patch_mask (const int32_t *patch_scores,
                            const int      score_histogram[],
                            int            scored_patches,
                            size_t         rows,
                            size_t         columns,
                            size_t         count,
                            uint8_t       *refined_mask)
{
  int cumulative = 0;
  int median_score = 0;

  for (; median_score <= MILAN_QUALITY_PERCENT_SCALE; median_score++)
    {
      cumulative += score_histogram[median_score];
      if (cumulative >= scored_patches / 2)
        break;
    }
  for (size_t center_row = MILAN_QUALITY_PATCH_RADIUS;
       center_row < rows - MILAN_QUALITY_PATCH_RADIUS;
       center_row += MILAN_QUALITY_PATCH_STRIDE)
    for (size_t center_column = MILAN_QUALITY_PATCH_RADIUS;
         center_column < columns - MILAN_QUALITY_PATCH_RADIUS;
         center_column += MILAN_QUALITY_PATCH_STRIDE)
      {
        size_t center = center_row * columns + center_column;
        int32_t score = patch_scores[center];

        if (score < 0 ||
            ((score * MILAN_QUALITY_PERCENT_SCALE) >>
             MILAN_QUALITY_Q16_SHIFT) > median_score)
          continue;
        for (size_t row = center_row - MILAN_QUALITY_PATCH_RADIUS;
             row < center_row + MILAN_QUALITY_PATCH_RADIUS; row++)
          for (size_t column = center_column - MILAN_QUALITY_PATCH_RADIUS;
               column < center_column + MILAN_QUALITY_PATCH_RADIUS; column++)
            {
              size_t index = row * columns + column;

              if (refined_mask[index] != 0)
                refined_mask[index] = MILAN_QUALITY_REFINED_MASK_MARKER;
            }
      }
  for (size_t i = 0; i < count; i++)
    refined_mask[i] =
      refined_mask[i] == MILAN_QUALITY_REFINED_MASK_MARKER ? UINT8_MAX : 0;
}

static void
quality_accumulate_patch_scores (
  const uint8_t *valid_mask,
  const int32_t *gradient_x,
  const int32_t *gradient_y,
  const int32_t *magnitude,
  size_t         rows,
  size_t         columns,
  int            fixed_threshold,
  int32_t       *patch_scores,
  int            score_histogram[MILAN_QUALITY_SCORE_BIN_COUNT],
  int64_t       *score_sum,
  int           *scored_patches)
{
  for (size_t center_row = MILAN_QUALITY_PATCH_RADIUS;
       center_row < rows - MILAN_QUALITY_PATCH_RADIUS;
       center_row += MILAN_QUALITY_PATCH_STRIDE)
    {
      for (size_t center_column = MILAN_QUALITY_PATCH_RADIUS;
           center_column < columns - MILAN_QUALITY_PATCH_RADIUS;
           center_column += MILAN_QUALITY_PATCH_STRIDE)
        {
          size_t center = center_row * columns + center_column;
          int64_t magnitude_sum = 0;
          int valid_count = 0;

          if (valid_mask[center] == 0)
            continue;
          for (size_t row = center_row - MILAN_QUALITY_PATCH_RADIUS;
               row <= center_row + MILAN_QUALITY_PATCH_RADIUS; row++)
            for (size_t column = center_column - MILAN_QUALITY_PATCH_RADIUS;
                 column <= center_column + MILAN_QUALITY_PATCH_RADIUS;
                 column++)
              {
                size_t index = row * columns + column;

                if (valid_mask[index] != 0)
                  {
                    magnitude_sum += magnitude[index];
                    valid_count++;
                  }
              }
          if (valid_count <
              (MILAN_QUALITY_PATCH_WIDTH * MILAN_QUALITY_PATCH_WIDTH) /
              MILAN_QUALITY_PATCH_MINIMUM_VALID_DIVISOR)
            continue;

          int threshold = MILAN_QUALITY_PATCH_GRADIENT_FLOOR;
          if (!fixed_threshold)
            {
              threshold =
                (int) ((valid_count / 2 + magnitude_sum) / valid_count);
              if (threshold < MILAN_QUALITY_PATCH_GRADIENT_FLOOR)
                threshold = MILAN_QUALITY_PATCH_GRADIENT_FLOOR;
            }

          int selected_count = 0;
          int64_t x_square_sum = 0;
          int64_t y_square_sum = 0;
          int64_t cross_sum = 0;
          for (size_t row = center_row - MILAN_QUALITY_PATCH_RADIUS;
               row <= center_row + MILAN_QUALITY_PATCH_RADIUS; row++)
            for (size_t column = center_column - MILAN_QUALITY_PATCH_RADIUS;
                 column <= center_column + MILAN_QUALITY_PATCH_RADIUS;
                 column++)
              {
                size_t index = row * columns + column;

                if (magnitude[index] >= threshold)
                  {
                    int x = gradient_x[index];
                    int y = gradient_y[index];

                    x_square_sum += x * x;
                    y_square_sum += y * y;
                    cross_sum += x * y;
                    selected_count++;
                  }
              }
          if (selected_count <
              (MILAN_QUALITY_PATCH_WIDTH * MILAN_QUALITY_PATCH_WIDTH) /
              MILAN_QUALITY_PATCH_MINIMUM_SELECTED_DIVISOR)
            {
              patch_scores[center] = 0;
              score_histogram[0]++;
              (*scored_patches)++;
              continue;
            }

          int64_t rounding = selected_count / 2;
          int64_t x_square = (rounding + x_square_sum) / selected_count;
          int64_t y_square = (rounding + y_square_sum) / selected_count;
          int64_t cross = (rounding + cross_sum) / selected_count;
          int64_t average_square = (x_square + y_square) / 2;
          int64_t anisotropy =
            ((x_square * y_square - cross * cross) * MILAN_QUALITY_Q16_ONE) /
            (average_square * average_square + 1);

          if (anisotropy < 0)
            anisotropy = 0;
          int64_t score = MILAN_QUALITY_Q16_ONE - anisotropy;
          if (score < 0)
            score = 0;
          int score_percent = (int) (
            (score * MILAN_QUALITY_PERCENT_SCALE) >> MILAN_QUALITY_Q16_SHIFT);
          if (score_percent > MILAN_QUALITY_PERCENT_SCALE)
            score_percent = MILAN_QUALITY_PERCENT_SCALE;
          patch_scores[center] = (int32_t) score;
          score_histogram[score_percent]++;
          *score_sum += score;
          (*scored_patches)++;
        }
    }
}

static int
quality_patch_score_core (const uint8_t *frame,
                          const uint8_t *valid_mask,
                          size_t         rows,
                          size_t         columns,
                          int            fixed_threshold,
                          uint8_t       *refined_mask,
                          int           *patch_score)
{
  size_t count;
  int32_t *gradient_x = NULL;
  int32_t *gradient_y = NULL;
  int32_t *magnitude = NULL;
  int32_t *patch_scores = NULL;
  int score_histogram[MILAN_QUALITY_SCORE_BIN_COUNT] = { 0 };
  int64_t score_sum = 0;
  int scored_patches = 0;

  if (!frame || !valid_mask || !patch_score ||
      rows < MILAN_QUALITY_PATCH_WIDTH || columns < MILAN_QUALITY_PATCH_WIDTH ||
      columns > SIZE_MAX / rows)
    return -1;

  count = rows * columns;
  if (count > SIZE_MAX / sizeof (*gradient_x))
    return -1;
  gradient_x = calloc (count, sizeof (*gradient_x));
  gradient_y = calloc (count, sizeof (*gradient_y));
  magnitude = calloc (count, sizeof (*magnitude));
  patch_scores = malloc (count * sizeof (*patch_scores));
  if (!gradient_x || !gradient_y || !magnitude || !patch_scores)
    goto allocation_failed;
  for (size_t i = 0; i < count; i++)
    patch_scores[i] = -1;
  if (refined_mask)
    memcpy (refined_mask, valid_mask, count);

  quality_prepare_patch_gradients (
    frame, valid_mask, rows, columns, gradient_x, gradient_y, magnitude);

  quality_accumulate_patch_scores (
    valid_mask, gradient_x, gradient_y, magnitude, rows, columns,
    fixed_threshold, patch_scores, score_histogram, &score_sum,
    &scored_patches);

  *patch_score = scored_patches != 0 ?
                 (int) (((((scored_patches / 2) + score_sum) /
                          scored_patches) * MILAN_QUALITY_PERCENT_SCALE) >>
                        MILAN_QUALITY_Q16_SHIFT) :
                 0;
  if (!fixed_threshold)
    *patch_score += 5;
  if (refined_mask)
    quality_rewrite_patch_mask (
      patch_scores, score_histogram, scored_patches, rows, columns, count,
      refined_mask);
  free (patch_scores);
  free (magnitude);
  free (gradient_y);
  free (gradient_x);
  return 0;

allocation_failed:
  free (patch_scores);
  free (magnitude);
  free (gradient_y);
  free (gradient_x);
  return -1;
}

static int
quality_mask_fraction_core (const uint8_t *source,
                            const uint8_t *valid_mask,
                            size_t         rows,
                            size_t         columns,
                            int           *mask_fraction)
{
  size_t count = rows * columns;
  uint8_t *classes = malloc (count);
  int histogram[MILAN_QUALITY_RING_HISTOGRAM_SIZE] = { 0 };

  if (!classes)
    return -1;
  memset (classes, MILAN_QUALITY_RING_UNCLASSIFIED, count);
  for (size_t row = 1; row + 1 < rows; row++)
    for (size_t column = 1; column + 1 < columns; column++)
      {
        size_t index = row * columns + column;

        if (valid_mask[index] == 0)
          continue;
        int center = source[index];
        int ring[MILAN_QUALITY_RING_SIZE] = {
          source[index - columns - 1] - center > MILAN_QUALITY_RING_CONTRAST_DELTA,
          source[index - columns] - center > MILAN_QUALITY_RING_CONTRAST_DELTA,
          source[index - columns + 1] - center > MILAN_QUALITY_RING_CONTRAST_DELTA,
          source[index + 1] - center > MILAN_QUALITY_RING_CONTRAST_DELTA,
          source[index + columns + 1] - center > MILAN_QUALITY_RING_CONTRAST_DELTA,
          source[index + columns] - center > MILAN_QUALITY_RING_CONTRAST_DELTA,
          source[index + columns - 1] - center > MILAN_QUALITY_RING_CONTRAST_DELTA,
          source[index - 1] - center > MILAN_QUALITY_RING_CONTRAST_DELTA,
        };
        int transitions = 0;
        int neighbors = 0;

        for (size_t i = 0; i < MILAN_QUALITY_RING_SIZE; i++)
          {
            transitions += ring[i] != ring[(i + 1) & MILAN_QUALITY_RING_INDEX_MASK];
            neighbors += ring[i];
          }
        if (transitions < MILAN_QUALITY_RING_TRANSITION_LIMIT)
          classes[index] = (uint8_t) neighbors;
      }
  int classified = 0;
  for (size_t i = 0; i < count; i++)
    if (valid_mask[i] != 0)
      {
        histogram[classes[i]]++;
        classified += classes[i] < MILAN_QUALITY_RING_UNCLASSIFIED;
      }
  *mask_fraction = classified != 0 ?
                     histogram[MILAN_QUALITY_RING_TARGET_CLASS] *
                       MILAN_QUALITY_PERCENT_SCALE / classified :
                     0;
  free (classes);
  return 0;
}

int
goodix_milan_preprocess_quality (const uint8_t *frame,
                             size_t         rows,
                             size_t         columns,
                             int           *quality,
                             int           *coverage)
{
  size_t count;
  uint8_t *coverage_mask = NULL;
  uint8_t *valid_mask = NULL;
  uint8_t *refined_mask = NULL;
  int raw_coverage;
  int original_valid_score;
  int propagated_valid_score;
  int patch_score;
  int mask_fraction;
  int result = -1;

  if (!frame || !quality || !coverage || rows == 0 || columns == 0 ||
      columns > SIZE_MAX / rows)
    return -1;

  count = rows * columns;
  coverage_mask = malloc (count);
  valid_mask = malloc (count);
  refined_mask = malloc (count);
  if (!coverage_mask || !valid_mask || !refined_mask)
    goto out;

  if (goodix_milan_preprocess_quality_coverage_mask (
        frame, rows, columns, coverage_mask, &raw_coverage) != 0)
    goto out;
  goodix_milan_preprocess_quality_valid_mask (
    frame, rows, columns, valid_mask, &original_valid_score);
  propagated_valid_score =
    goodix_milan_preprocess_quality_mask_coverage (valid_mask, count);
  if (quality_patch_score_core (
        frame, valid_mask, rows, columns, 1, refined_mask, &patch_score) != 0 ||
      quality_mask_fraction_core (
        frame, refined_mask, rows, columns, &mask_fraction) != 0 ||
      quality_combine_core (
        raw_coverage, propagated_valid_score, patch_score, mask_fraction,
        35, 7, 1, quality, coverage) != 0)
    goto out;

  result = 0;

out:
  free (refined_mask);
  free (valid_mask);
  free (coverage_mask);
  return result;
}

void
goodix_milan_feature_pack_inline_mask (const uint8_t *validity_mask,
                                       size_t         rows,
                                       size_t         columns,
                                       uint8_t       *inline_mask)
{
  size_t block_rows = (rows + 3) / 4;
  size_t block_columns = (columns + 3) / 4;

  memset (inline_mask, 0, (block_rows * block_columns + 7) / 8);
  for (size_t block_row = 0; block_row < block_rows; block_row++)
    for (size_t block_column = 0; block_column < block_columns; block_column++)
      {
        size_t foreground = 0;
        size_t pixels = 0;

        for (size_t y = block_row * 4;
             y < rows && y < block_row * 4 + 4; y++)
          for (size_t x = block_column * 4;
               x < columns && x < block_column * 4 + 4; x++)
            {
              foreground += validity_mask[y * columns + x] != 0;
              pixels++;
            }
        if (foreground * 2 >= pixels)
          {
            size_t bit = block_row * block_columns + block_column;

            inline_mask[bit / 8] |= (uint8_t) (1U << (bit & 7));
          }
      }
}

int
goodix_milan_feature_base_maps_with_validity (
  const uint8_t *frame,
  size_t         rows,
  size_t         columns,
  uint8_t       *high_bitmap,
  uint8_t       *low_bitmap,
  uint8_t       *feature_mask,
  uint8_t       *inline_mask,
  uint8_t       *validity_mask)
{
  size_t cropped_columns;
  size_t cropped_count;
  size_t map_columns;
  size_t map_rows;
  size_t map_count;
  size_t bitmap_size;
  uint8_t *cropped = NULL;
  uint8_t *mask = NULL;
  uint16_t *horizontal = NULL;
  uint8_t *blurred = NULL;
  int ignored_coverage;
  int result = -1;

  if (!frame || !high_bitmap || !low_bitmap || !feature_mask || rows < 2 ||
      columns < 8 || columns > SIZE_MAX / rows)
    return -1;

  cropped_columns = (columns / 8) * 8;
  cropped_count = cropped_columns * rows;
  map_columns = cropped_columns / 2;
  map_rows = rows / 2;
  map_count = map_columns * map_rows;
  bitmap_size = (map_count + 7) / 8;
  cropped = malloc (cropped_count);
  mask = malloc (cropped_count);
  horizontal = malloc (cropped_count * sizeof(*horizontal));
  blurred = malloc (cropped_count);
  if (!cropped || !mask || !horizontal || !blurred)
    goto out;

  size_t crop_offset = (columns - cropped_columns) / 2;
  for (size_t row = 0; row < rows; row++)
    memcpy (cropped + row * cropped_columns,
            frame + row * columns + crop_offset,
            cropped_columns);

  if (quality_coverage_mask_core (cropped, rows, cropped_columns, 0x6e, 1,
                                  mask, &ignored_coverage) != 0)
    goto out;
  if (validity_mask)
    memcpy (validity_mask, mask, cropped_count);

  quality_gaussian_reflect101 (
    cropped, rows, cropped_columns, milan_quality_gaussian6_kernel,
    sizeof (milan_quality_gaussian6_kernel) /
    sizeof (milan_quality_gaussian6_kernel[0]),
    horizontal, blurred);

  memset (high_bitmap, 0, bitmap_size);
  memset (low_bitmap, 0, bitmap_size);
  for (size_t row = 0; row < map_rows; row++)
    {
      for (size_t column = 0; column < map_columns; column++)
        {
          size_t source_index = row * 2 * cropped_columns + column * 2;
          size_t map_index = row * map_columns + column;
          uint8_t bit = (uint8_t) (1U << (map_index & 7));

          feature_mask[map_index] = mask[source_index];
          if (blurred[source_index] >= 0xc9)
            high_bitmap[map_index >> 3] |= bit;
          if (blurred[source_index] >= 0x38)
            low_bitmap[map_index >> 3] |= bit;
        }
    }
  if (inline_mask)
    goodix_milan_feature_pack_inline_mask (
      mask, rows, cropped_columns, inline_mask);
  result = 0;

out:
  free (blurred);
  free (horizontal);
  free (mask);
  free (cropped);
  return result;
}

int
goodix_milan_feature_base_maps (const uint8_t *frame,
                                size_t         rows,
                                size_t         columns,
                                uint8_t       *high_bitmap,
                                uint8_t       *low_bitmap,
                                uint8_t       *feature_mask,
                                uint8_t       *inline_mask)
{
  return goodix_milan_feature_base_maps_with_validity (
    frame, rows, columns, high_bitmap, low_bitmap, feature_mask, inline_mask,
    NULL);
}


static int
milan_profile9_prepare_contrast_source (const uint16_t *source,
                                         const uint8_t  *mask,
                                         size_t          rows,
                                         size_t          columns,
                                         uint16_t       *output)
{
  const uint32_t gaussian_side = 6980;
  const uint32_t gaussian_center = 51576;
  size_t count = rows * columns;
  uint32_t *sum_integral = NULL;
  uint32_t *count_integral = NULL;
  uint16_t *recentered = NULL;
  uint16_t *smoothed = NULL;
  uint16_t *horizontal = NULL;
  int result = -1;

  sum_integral = calloc (count, sizeof(*sum_integral));
  count_integral = calloc (count, sizeof(*count_integral));
  recentered = malloc (count * sizeof(*recentered));
  smoothed = malloc (count * sizeof(*smoothed));
  horizontal = malloc (count * sizeof(*horizontal));
  if (!sum_integral || !count_integral || !recentered || !smoothed ||
      !horizontal)
    goto out;

  for (size_t row = 0; row < rows; row++)
    for (size_t column = 0; column < columns; column++)
      {
        size_t index = row * columns + column;
        uint32_t value = mask[index] != 0 ? source[index] : 0;
        uint32_t valid = mask[index] != 0;

        if (column != 0)
          {
            value += sum_integral[index - 1];
            valid += count_integral[index - 1];
          }
        if (row != 0)
          {
            value += sum_integral[index - columns];
            valid += count_integral[index - columns];
          }
        if (row != 0 && column != 0)
          {
            value -= sum_integral[index - columns - 1];
            valid -= count_integral[index - columns - 1];
          }
        sum_integral[index] = value;
        count_integral[index] = valid;
      }

  for (size_t row = 0; row < rows; row++)
    for (size_t column = 0; column < columns; column++)
      {
        size_t index = row * columns + column;
        size_t first_row = row > 5 ? row - 5 : 0;
        size_t last_row = row + 5 < rows ? row + 5 : rows - 1;
        size_t first_column = column > 5 ? column - 5 : 0;
        size_t last_column =
          column + 5 < columns ? column + 5 : columns - 1;
        uint32_t sum;
        uint32_t valid;

        if (mask[index] == 0)
          {
            recentered[index] = 3000;
            continue;
          }
        sum = sum_integral[last_row * columns + last_column];
        valid = count_integral[last_row * columns + last_column];
        if (first_row != 0)
          {
            sum -= sum_integral[(first_row - 1) * columns + last_column];
            valid -= count_integral[(first_row - 1) * columns + last_column];
          }
        if (first_column != 0)
          {
            sum -= sum_integral[last_row * columns + first_column - 1];
            valid -= count_integral[last_row * columns + first_column - 1];
          }
        if (first_row != 0 && first_column != 0)
          {
            sum += sum_integral[(first_row - 1) * columns + first_column - 1];
            valid +=
              count_integral[(first_row - 1) * columns + first_column - 1];
          }
        uint32_t mean = valid != 0 ? (sum + valid / 2) / valid : 0;
        int32_t value = (int32_t) source[index] - (int32_t) mean + 3000;

        recentered[index] = (uint16_t) (value > 0 ? value : 0);
      }

  memcpy (smoothed, recentered, count * sizeof(*smoothed));
  for (size_t row = 1; row + 1 < rows; row++)
    for (size_t column = 1; column + 1 < columns; column++)
      {
        size_t index = row * columns + column;
        uint32_t sum =
          recentered[index - columns - 1] +
          2U * recentered[index - columns] +
          recentered[index - columns + 1] +
          2U * recentered[index - 1] + 4U * recentered[index] +
          2U * recentered[index + 1] + recentered[index + columns - 1] +
          2U * recentered[index + columns] +
          recentered[index + columns + 1];

        smoothed[index] = (uint16_t) ((sum + 8) >> 4);
      }

  for (size_t row = 0; row < rows; row++)
    for (size_t column = 0; column < columns; column++)
      {
        size_t left = goodix_milan_reflect101_index (
          (ptrdiff_t) column - 1, columns);
        size_t right = goodix_milan_reflect101_index (
          (ptrdiff_t) column + 1, columns);
        uint32_t sum = gaussian_side * smoothed[row * columns + left] +
                       gaussian_center * smoothed[row * columns + column] +
                       gaussian_side * smoothed[row * columns + right];

        horizontal[row * columns + column] = (uint16_t) (sum >> 16);
      }
  for (size_t row = 0; row < rows; row++)
    for (size_t column = 0; column < columns; column++)
      {
        size_t above = goodix_milan_reflect101_index (
          (ptrdiff_t) row - 1, rows);
        size_t below = goodix_milan_reflect101_index (
          (ptrdiff_t) row + 1, rows);
        uint32_t sum = gaussian_side * horizontal[above * columns + column] +
                       gaussian_center * horizontal[row * columns + column] +
                       gaussian_side * horizontal[below * columns + column];

        output[row * columns + column] = (uint16_t) (sum >> 16);
      }
  result = 0;

out:
  free (horizontal);
  free (smoothed);
  free (recentered);
  free (count_integral);
  free (sum_integral);
  return result;
}

static void
milan_horizontal_extrema (const uint16_t *source,
                          size_t          rows,
                          size_t          columns,
                          uint16_t       *minimums,
                          uint16_t       *maximums)
{
  for (size_t row = 0; row < rows; row++)
    for (size_t column = 0; column < columns; column++)
      {
        size_t first = column > 5 ? column - 5 : 0;
        size_t last = column + 5 < columns ? column + 5 : columns - 1;
        uint16_t minimum = UINT16_MAX;
        uint16_t maximum = 0;

        for (size_t x = first; x <= last; x++)
          {
            uint16_t value = source[row * columns + x];

            if (value < minimum)
              minimum = value;
            if (value > maximum)
              maximum = value;
          }
        minimums[row * columns + column] = minimum;
        maximums[row * columns + column] = maximum;
      }
}

static void
milan_vertical_extrema (const uint16_t *source,
                        size_t          rows,
                        size_t          columns,
                        uint16_t       *minimums,
                        uint16_t       *maximums)
{
  for (size_t row = 0; row < rows; row++)
    for (size_t column = 0; column < columns; column++)
      {
        size_t first = row > 5 ? row - 5 : 0;
        size_t last = row + 5 < rows ? row + 5 : rows - 1;
        uint16_t minimum = UINT16_MAX;
        uint16_t maximum = 0;

        for (size_t y = first; y <= last; y++)
          {
            uint16_t value = source[y * columns + column];

            if (value < minimum)
              minimum = value;
            if (value > maximum)
              maximum = value;
          }
        minimums[row * columns + column] = minimum;
        maximums[row * columns + column] = maximum;
      }
}

static void
milan_merge_extrema (const uint16_t *vertical_min,
                     const uint16_t *vertical_max,
                     size_t          count,
                     uint16_t       *horizontal_min,
                     uint16_t       *horizontal_max)
{
  for (size_t i = 0; i < count; i++)
    {
      if (vertical_max[i] > horizontal_max[i])
        horizontal_max[i] = vertical_max[i];
      if (vertical_min[i] < horizontal_min[i])
        horizontal_min[i] = vertical_min[i];
    }
}

static void
milan_render_local_contrast (const uint16_t *source,
                             const uint8_t  *mask,
                             size_t          rows,
                             size_t          columns,
                             const uint16_t *minimums,
                             const uint16_t *maximums,
                             int             refine_diagonals,
                             uint8_t        *output)
{
  for (size_t row = 0; row < rows; row++)
    for (size_t column = 0; column < columns; column++)
      {
        size_t index = row * columns + column;
        uint16_t lower = minimums[index];
        uint16_t upper = maximums[index];

        if (refine_diagonals)
          {
            for (int row_delta = -1; row_delta <= 1; row_delta += 2)
              {
                size_t neighbor_row;

                if ((row_delta < 0 && row == 0) ||
                    (row_delta > 0 && row + 1 == rows))
                  continue;
                neighbor_row = row_delta < 0 ? row - 1 : row + 1;
                for (int column_delta = -1; column_delta <= 1;
                     column_delta += 2)
                  {
                    size_t neighbor_column;
                    size_t neighbor;

                    if ((column_delta < 0 && column == 0) ||
                        (column_delta > 0 && column + 1 == columns))
                      continue;
                    neighbor_column =
                      column_delta < 0 ? column - 1 : column + 1;
                    neighbor = neighbor_row * columns + neighbor_column;
                    if (minimums[neighbor] > lower)
                      lower = minimums[neighbor];
                    if (maximums[neighbor] < upper)
                      upper = maximums[neighbor];
                  }
              }
          }

        if (mask[index] == 0)
          {
            output[index] = UINT8_MAX;
          }
        else if (upper == lower)
          {
            output[index] = 0;
          }
        else
          {
            int scaled = ((int) source[index] - (int) lower) * UINT8_MAX /
                         ((int) upper - (int) lower);

            if (scaled < 0)
              scaled = 0;
            else if (scaled > UINT8_MAX)
              scaled = UINT8_MAX;
            output[index] = (uint8_t) (UINT8_MAX - scaled);
          }
      }
}

static int
milan_contrast_core (const uint16_t *source,
                     const uint8_t  *mask,
                     size_t          rows,
                     size_t          columns,
                     uint8_t        *output,
                     int             refine_diagonals)
{
  size_t count;
  uint16_t *horizontal_min;
  uint16_t *horizontal_max;
  uint16_t *vertical_min;
  uint16_t *vertical_max;

  if (rows != 0 && columns > SIZE_MAX / rows)
    return -1;

  count = rows * columns;
  if (count > SIZE_MAX / sizeof (uint16_t))
    return -1;

  horizontal_min = malloc (count * sizeof (*horizontal_min));
  horizontal_max = malloc (count * sizeof (*horizontal_max));
  vertical_min = malloc (count * sizeof (*vertical_min));
  vertical_max = malloc (count * sizeof (*vertical_max));
  if (!horizontal_min || !horizontal_max || !vertical_min || !vertical_max)
    goto allocation_failed;

  milan_horizontal_extrema (
    source, rows, columns, horizontal_min, horizontal_max);
  milan_vertical_extrema (
    source, rows, columns, vertical_min, vertical_max);
  milan_merge_extrema (
    vertical_min, vertical_max, count, horizontal_min, horizontal_max);
  milan_render_local_contrast (
    source, mask, rows, columns, horizontal_min, horizontal_max,
    refine_diagonals, output);

  free (vertical_max);
  free (vertical_min);
  free (horizontal_max);
  free (horizontal_min);
  return 0;

allocation_failed:
  free (vertical_max);
  free (vertical_min);
  free (horizontal_max);
  free (horizontal_min);
  return -1;
}

int
goodix_milan_preprocess_contrast (const uint16_t *source,
                              const uint8_t  *mask,
                              size_t          rows,
                              size_t          columns,
                              uint8_t         *output)
{
  size_t count;
  uint16_t *profile_source;
  int result;

  if (!source || !mask || !output || rows == 0 || columns == 0 ||
      columns > SIZE_MAX / rows)
    return -1;
  count = rows * columns;
  if (count > SIZE_MAX / sizeof(*profile_source))
    return -1;
  profile_source = malloc (count * sizeof(*profile_source));
  if (!profile_source)
    return -1;
  result = milan_profile9_prepare_contrast_source (
    source, mask, rows, columns, profile_source);
  if (result == 0)
    result = milan_contrast_core (
      profile_source, mask, rows, columns, output, 1);
  free (profile_source);
  return result;
}

static void
milan_profile9_encode_metadata (uint8_t       *image,
                                const uint8_t *mask,
                                size_t         rows,
                                size_t         columns,
                                int            mode,
                                int            apply_mask)
{
  if (!image || !mask || rows == 0 || columns < 8 || mode == 0)
    return;
  size_t prefix = columns - 8;
  int prefix_ok = 1;

  for (size_t column = 0; column < prefix; column++)
    if ((image[column] & 1) != (column & 1))
      {
        prefix_ok = 0;
        break;
      }
  if (!prefix_ok)
    {
      for (size_t column = prefix; column < columns; column++)
        image[column] &= 0xfe;
      for (size_t column = 0; column < prefix; column++)
        image[column] = (uint8_t) ((image[column] & 0xfe) | (column & 1));
    }

  switch (mode)
    {
    case 1:
      image[columns - 8] |= 1;
      image[columns - 7] &= 0xfe;
      break;
    case 2:
      image[columns - 8] |= 1;
      image[columns - 7] |= 1;
      break;
    case 3:
      image[columns - 8] &= 0xfe;
      image[columns - 7] |= 1;
      break;
    case 4:
      image[columns - 8] &= 0xfe;
      image[columns - 7] &= 0xfe;
      break;
    case 5:
      image[columns - 5] |= 1;
      image[columns - 4] &= 0xfe;
      image[columns - 3] &= 0xfe;
      break;
    case 6:
      image[columns - 5] &= 0xfe;
      image[columns - 4] |= 1;
      image[columns - 3] &= 0xfe;
      break;
    case 7:
      image[columns - 5] |= 1;
      image[columns - 4] |= 1;
      image[columns - 3] &= 0xfe;
      break;
    case 8:
      image[columns - 5] &= 0xfe;
      image[columns - 4] &= 0xfe;
      image[columns - 3] |= 1;
      break;
    case 9:
      image[columns - 5] |= 1;
      image[columns - 4] &= 0xfe;
      image[columns - 3] |= 1;
      break;
    default:
      image[columns - 5] &= 0xfe;
      image[columns - 4] &= 0xfe;
      image[columns - 3] &= 0xfe;
      break;
    }
  if (apply_mask)
    {
      int marker_was_set = (image[columns - 6] & 1) != 0;

      image[columns - 6] |= 1;
      for (size_t i = columns; i < rows * columns; i++)
        {
          if (!marker_was_set)
            image[i] = (uint8_t) ((image[i] & 0xfe) | (mask[i] == 0));
          else if (mask[i] != 0)
            image[i] &= 0xfe;
        }
    }
}

/*
 * Goodix 53x5 driver for libfprint - Milan post-render processing
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

static void
milan_profile9_make_reciprocal_plane (const uint16_t *application_gain,
                                      size_t          count,
                                      uint16_t       *output)
{
  for (size_t i = 0; i < count; i++)
    output[i] = application_gain[i] == 0
                  ? (uint16_t) (UINT32_C(8000) << 13)
                  : (uint16_t) ((UINT32_C(8000) * MILAN_FIXED_ONE +
                                 application_gain[i] / 2) /
                                application_gain[i]);
}

static int
milan_profile9_diagnostic_contrast (const uint16_t *source,
                                    const uint8_t  *render_mask,
                                    size_t          rows,
                                    size_t          columns,
                                    uint8_t        *output)
{
  size_t count = rows * columns;
  uint16_t *prepared = NULL;
  int *histogram = NULL;
  size_t selected = 0;
  int lower_bin = 0;
  int upper_bin = 4999;
  int result = -1;

  prepared = malloc (count * sizeof(*prepared));
  histogram = calloc (5000, sizeof(*histogram));
  if (!prepared || !histogram)
    goto out;
  if (milan_profile9_prepare_contrast_source (
        source, render_mask, rows, columns, prepared) != 0)
    goto out;

  for (size_t i = 0; i < count; i++)
    if (render_mask[i] != 0)
      {
        uint32_t bin = prepared[i] >> 2;

        if (bin > 4999)
          bin = 4999;
        histogram[bin]++;
        selected++;
      }

  size_t cumulative = 0;
  for (upper_bin = 4999; upper_bin >= 0; upper_bin--)
    {
      cumulative += histogram[upper_bin];
      if (selected * 15 <= cumulative * 100)
        break;
    }
  if (upper_bin < 0)
    upper_bin = 0;

  cumulative = 0;
  for (lower_bin = 0; lower_bin < 5000; lower_bin++)
    {
      cumulative += histogram[lower_bin];
      if (selected * 5 <= cumulative * 100)
        break;
    }
  if (lower_bin == 5000)
    lower_bin = INT_MAX / 4;

  int lower = lower_bin * 4;
  int upper = upper_bin * 4;

  if (upper - lower < 200)
    upper = lower + 200;
  for (size_t i = 0; i < count; i++)
    {
      int value = 0;

      if (render_mask[i] != 0 && upper > lower)
        {
          value = ((int) prepared[i] - lower) * UINT8_MAX /
                  (upper - lower);
          if (value < 0)
            value = 0;
          else if (value > UINT8_MAX)
            value = UINT8_MAX;
        }
      output[i] = (uint8_t) (UINT8_MAX - value);
    }
  result = 0;

out:
  free (histogram);
  free (prepared);
  return result;
}

static int
milan_profile9_component_score (const uint8_t *frame,
                                const uint8_t *residual_mask,
                                size_t         rows,
                                size_t         columns,
                                int           *score)
{
  int patch_score;
  int mask_fraction;

  if (quality_patch_score_core (
        frame, residual_mask, rows, columns, 1, NULL, &patch_score) != 0 ||
      quality_mask_fraction_core (
        frame, residual_mask, rows, columns, &mask_fraction) != 0)
    return -1;

  *score = patch_score;
  if (*score < 70)
    {
      if (mask_fraction < 25)
        {
          int factor = mask_fraction * 256 / 25;

          *score = *score * factor >> 8;
          *score = *score * factor >> 8;
        }
      if (mask_fraction >= 30)
        *score += 15;
    }
  if (*score < 0)
    *score = 0;
  else if (*score > 100)
    *score = 100;
  return 0;
}

static int
milan_profile9_center_rows (const uint16_t *source,
                            const uint8_t  *render_mask,
                            size_t          rows,
                            size_t          columns,
                            uint16_t       *centered)
{
  size_t count = rows * columns;
  size_t mask_count = 0;

  if (!source || !render_mask || !centered)
    return -1;
  for (size_t i = 0; i < count; i++)
    mask_count += render_mask[i] != 0;
  int mask_percent = (int) (mask_count * 100 / count);

  for (size_t row = 0; row < rows; row++)
    {
      uint32_t sum = 0;
      size_t contributors = 0;

      for (size_t column = 0; column < columns; column++)
        {
          size_t index = row * columns + column;

          if (mask_percent >= 96 || render_mask[index] != 0)
            {
              sum += source[index];
              contributors++;
            }
        }
      uint16_t mean = contributors != 0 ? (uint16_t) (sum / contributors) : 0;

      for (size_t column = 0; column < columns; column++)
        {
          size_t index = row * columns + column;
          int value;

          if (mask_percent < 96 && render_mask[index] == 0)
            value = 5000;
          else
            value = (int16_t) ((int) source[index] - mean + 5000);
          centered[index] = (uint16_t) (value > 0 ? value : 0);
        }
    }
  return 0;
}

static int
milan_profile9_disagreement (const uint16_t *working,
                             const uint8_t  *render_mask,
                             size_t          rows,
                             size_t          columns,
                             int             selected_refined,
                             uint8_t        *component_mask,
                             int            *component_score,
                             int            *component_flag,
                             int            *disagreement)
{
  size_t count = rows * columns;
  uint16_t *centered = NULL;
  const uint16_t *diagnostic_source = working;
  uint8_t *diagnostic = NULL;
  uint8_t *gradient_mask = NULL;
  size_t source_count = 0;
  size_t missing_count = 0;
  int ignored_coverage;
  int result = -1;

  diagnostic = malloc (count);
  gradient_mask = malloc (count);
  centered = selected_refined ? malloc (count * sizeof(*centered)) : NULL;
  if (!diagnostic || !gradient_mask || (selected_refined && !centered))
    goto out;
  if (selected_refined)
    {
      if (milan_profile9_center_rows (
            working, render_mask, rows, columns, centered) != 0)
        goto out;
      diagnostic_source = centered;
    }
  if (milan_profile9_diagnostic_contrast (
        diagnostic_source, render_mask, rows, columns, diagnostic) != 0 ||
      quality_coverage_mask_core (
        diagnostic, rows, columns, 80, UINT8_MAX, gradient_mask,
        &ignored_coverage) != 0)
    goto out;

  for (size_t i = 0; i < count; i++)
    {
      component_mask[i] = render_mask[i];
      if (render_mask[i] != 0)
        {
          source_count++;
          if (gradient_mask[i] == 0)
            missing_count++;
          else
            component_mask[i] = 0;
        }
    }

  *component_score = 0;
  *component_flag = 0;
  if (missing_count * 100 > source_count * 30)
    {
      if (milan_profile9_component_score (
            diagnostic, component_mask, rows, columns,
            component_score) != 0)
        goto out;
      if (*component_score < 15)
        *component_flag = 1;
    }
  *disagreement =
    (int) (((*component_flag ? missing_count : source_count) * 100) / count);
  result = 0;

out:
  free (centered);
  free (gradient_mask);
  free (diagnostic);
  return result;
}

static int
milan_profile9_rerender (const uint16_t *source,
                         const uint8_t  *render_mask,
                         size_t          rows,
                         size_t          columns,
                         int             refined,
                         uint8_t        *output)
{
  size_t count = rows * columns;
  uint16_t *centered = NULL;
  size_t mask_count = 0;
  int result;

  if (!refined)
    return milan_contrast_core (
      source, render_mask, rows, columns, output, 0);

  centered = malloc (count * sizeof(*centered));
  if (!centered)
    return -1;
  for (size_t i = 0; i < count; i++)
    mask_count += render_mask[i] != 0;
  int mask_percent = (int) (mask_count * 100 / count);

  for (size_t row = 0; row < rows; row++)
    {
      uint32_t sum = 0;
      size_t contributors = 0;

      for (size_t column = 0; column < columns; column++)
        {
          size_t index = row * columns + column;

          if (mask_percent >= 96 || render_mask[index] != 0)
            {
              sum += source[index];
              contributors++;
            }
        }
      uint16_t mean = contributors != 0 ? (uint16_t) (sum / contributors) : 0;

      for (size_t column = 0; column < columns; column++)
        {
          size_t index = row * columns + column;
          int value;

          if (mask_percent < 96 && render_mask[index] == 0)
            value = 5000;
          else
            value = (int16_t) ((int) source[index] - mean + 5000);
          centered[index] = (uint16_t) (value > 0 ? value : 0);
        }
    }
  result = milan_contrast_core (
    centered, render_mask, rows, columns, output, 0);
  free (centered);
  return result;
}

static int
milan_profile9_mean_absolute_difference (const uint8_t *first,
                                         const uint8_t *second,
                                         const uint8_t *mask,
                                         size_t         count)
{
  uint64_t sum = 0;
  size_t samples = 0;

  for (size_t i = 0; i < count; i++)
    if (mask[i] != 0)
      {
        int difference = (int) first[i] - (int) second[i];

        sum += difference < 0 ? -difference : difference;
        samples++;
      }
  return samples != 0 ? (int) ((sum + samples / 2) / samples) : 0;
}

static int
milan_profile9_post_render (GoodixMilanPreprocessState *state,
                            const uint16_t             *normalized_live,
                            const uint16_t             *setup_map,
                            const uint8_t              *render_mask,
                            const uint16_t             *working,
                            const uint16_t             *optional_plane0,
                            const uint16_t             *reciprocal_plane,
                            const uint8_t              *selected,
                            const uint8_t              *selection_mask,
                            int                         selected_refined,
                            GoodixMilanPreprocessPurpose purpose,
                            uint32_t                    *calibration_ready,
                            const uint8_t              **first_metadata_mask,
                            uint8_t                    **first_metadata_owned_mask,
                            int                         *first_metadata_mode,
                            int                         *first_metadata_apply)
{
  const size_t rows = GOODIX_MILAN_SENSOR_ROWS;
  const size_t columns = GOODIX_MILAN_SENSOR_COLUMNS;
  const size_t count = GOODIX_MILAN_SENSOR_PIXELS;
  uint16_t *nested_gain = NULL;
  uint16_t *nested_output = NULL;
  uint8_t *primary_render = NULL;
  uint8_t *fallback_render = NULL;
  uint8_t *component_mask = NULL;
  const uint8_t *quality_mask;
  size_t mask_count = 0;
  uint32_t saved_sample_count;
  uint32_t auxiliary_samples;
  int component_score;
  int component_flag;
  int disagreement;
  int primary_metric;
  int fallback_metric = 200;
  int quality_gate;
  int mode = 0;
  int status = 0;
  int result = -1;

  (void) purpose;
  primary_render = malloc (count);
  fallback_render = malloc (count);
  component_mask = malloc (count);
  if (!primary_render || !fallback_render || !component_mask)
    goto out;

  if (milan_profile9_disagreement (
        working, render_mask, rows, columns, selected_refined, component_mask,
        &component_score, &component_flag, &disagreement) != 0 ||
      milan_profile9_rerender (
        optional_plane0, render_mask, rows, columns, selected_refined,
        primary_render) != 0)
    goto out;

  quality_mask = component_flag ? component_mask : selection_mask;
  primary_metric = milan_profile9_mean_absolute_difference (
    selected, primary_render, quality_mask, count);
  quality_gate = primary_metric > 75;
  if (!quality_gate && primary_metric > 55)
    {
      if (milan_contrast_core (
            reciprocal_plane, render_mask, rows, columns,
            fallback_render, 0) != 0)
        goto out;
      fallback_metric = milan_profile9_mean_absolute_difference (
        selected, fallback_render, quality_mask, count);
      quality_gate = fallback_metric < 80;
    }

  state->post_render.primary_metric = primary_metric;
  state->post_render.fallback_metric = fallback_metric;
  state->post_render.disagreement = disagreement;
  state->post_render.component_score = component_score;
  state->post_render.component_flag = component_flag;
  state->post_render.quality_gate = quality_gate;

  if (!quality_gate)
    {
      result = 0;
      goto publish;
    }

  mode = 4;

  for (size_t i = 0; i < count; i++)
    mask_count += render_mask[i] != 0;
  int mask_percent = (int) (mask_count * 100 / count);
  int normalized_disagreement = mask_percent != 0
                                  ? disagreement * 100 / mask_percent
                                  : disagreement;

  saved_sample_count = state->sample_count;
  if (disagreement > 40 || normalized_disagreement > 33)
    {
      status = GOODIX_MILAN_PREPROCESS_RETRY;
      mode = saved_sample_count < 100 && primary_metric > 85 ? 3 : 2;
    }
  if (saved_sample_count < 100 && disagreement > 90)
    {
      auxiliary_samples = state->auxiliary_sample_count;
      nested_gain = malloc (count * sizeof(*nested_gain));
      nested_output = malloc (count * sizeof(*nested_output));
      if (!nested_gain || !nested_output)
        goto out;
      state->sample_count = 5;
      goodix_milan_profile9_update_gain_ready (
        auxiliary_samples, state->sample_count, calibration_ready);
      if (first_update_frame_core (
            normalized_live, setup_map, state->calibration_map,
            state->application_gain_map, state->auxiliary_gain_map,
            auxiliary_samples, state, *calibration_ready, rows, columns,
            nested_gain, nested_output, NULL) != 0)
        {
          state->sample_count = saved_sample_count;
          goto out;
        }
      state->sample_count = saved_sample_count;
      state->post_render.update_applied = 1;
    }
  state->post_render.status = status;
  result = status;

publish:
  *first_metadata_apply = component_flag;
  *first_metadata_mode = mode;
  if (component_flag)
    {
      *first_metadata_mask = component_mask;
      *first_metadata_owned_mask = component_mask;
      component_mask = NULL;
    }
  else
    {
      *first_metadata_mask = selection_mask;
      *first_metadata_owned_mask = NULL;
    }

out:
  free (component_mask);
  free (fallback_render);
  free (primary_render);
  free (nested_output);
  free (nested_gain);
  return result;
}
