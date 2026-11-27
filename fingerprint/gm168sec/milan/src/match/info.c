/*
 * Goodix 53x5 driver for libfprint - native Milan match information
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define FP_COMPONENT "goodix53x5"

#include "milan/debug-hooks.h"
#include "milan/match/match.h"
#include "milan/match/info-private.h"
#include "milan/match/rescue.h"
#include "milan/milan.h"
#include "milan/print.h"
#include "milan/private.h"

#include <string.h>

#define GOODIX_MILAN_OPTIONAL_C7_LEN 5
#define GOODIX_MILAN_EXTRACTION_COMPONENT_QUEUE_CAPACITY 2450

static void
goodix_milan_match_pack_rescue_mask (
  const guint8 dense[GOODIX_MILAN_MATCH_RESCUE_MASK_WIDTH *
                     GOODIX_MILAN_MATCH_RESCUE_MASK_HEIGHT],
  guint8       packed[GOODIX_MILAN_MATCH_RESCUE_MASK_SIZE])
{
  memset (packed, 0, GOODIX_MILAN_MATCH_RESCUE_MASK_SIZE);
  for (size_t y = 0; y < GOODIX_MILAN_MATCH_RESCUE_MASK_HEIGHT; y++)
    for (size_t x = 0; x < GOODIX_MILAN_MATCH_RESCUE_MASK_WIDTH; x++)
      if (dense[y * GOODIX_MILAN_MATCH_RESCUE_MASK_WIDTH + x])
        packed[y * GOODIX_MILAN_MATCH_RESCUE_MASK_STRIDE + x / 8] |=
          (guint8) (1U << (x & 7));
}

static size_t
goodix_milan_match_record_limit (guint16 sensor_subtype,
                           int     coverage,
                           size_t  configured_limit)
{
  if (sensor_subtype == GOODIX_MILAN_PRINT_SENSOR_TYPE)
    {
      size_t limit = (size_t) coverage * configured_limit / 100;

      return limit > 0 ? limit : 1;
    }

  return ((size_t) coverage * configured_limit + 50) / 100;
}

static void
goodix_milan_match_retain_class_components (
  guint8 mask[GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS],
  guint  maximum_label,
  guint  strict_size_floor,
  gint  *labels,
  gint  *queue)
{
  guint component_sizes[72] = { 0 };
  guint selected_labels[3] = { 0 };
  guint label_count = 0;
  guint begin = 0;
  guint end = 0;
  guint pending = 0;

  memset (labels, 0xff,
          GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS * sizeof(*labels));
  for (guint seed = 0;
       seed < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS;
       seed++)
    {
      if (labels[seed] != -1)
        continue;
      if (mask[seed] == 0)
        {
          labels[seed] = 0;
          continue;
        }
      if (label_count == maximum_label)
        break;
      label_count++;
      labels[seed] = (gint) label_count;
      component_sizes[label_count]++;
      queue[end++] = (gint) seed;
      if (end == GOODIX_MILAN_EXTRACTION_COMPONENT_QUEUE_CAPACITY)
        end = 0;
      pending++;
      while (pending != 0)
        {
          gint current = queue[begin++];
          gint row = current / GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS;
          gint column = current % GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS;
          static const gint dx[4] = { -1, 0, 1, 0 };
          static const gint dy[4] = { 0, -1, 0, 1 };

          if (begin == GOODIX_MILAN_EXTRACTION_COMPONENT_QUEUE_CAPACITY)
            begin = 0;
          pending--;
          for (guint direction = 0; direction < 4; direction++)
            {
              gint x = column + dx[direction];
              gint y = row + dy[direction];
              gint neighbor;

              if (x < 0 ||
                  x >= GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS ||
                  y < 0 || y >= GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS)
                continue;
              neighbor =
                y * GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS + x;
              if (labels[neighbor] != -1)
                continue;
              if (mask[neighbor] == 0)
                {
                  labels[neighbor] = 0;
                  continue;
                }
              labels[neighbor] = (gint) label_count;
              component_sizes[label_count]++;
              /* Native labels/counts discoveries even when enqueue is full. */
              if (pending < GOODIX_MILAN_EXTRACTION_COMPONENT_QUEUE_CAPACITY)
                {
                  queue[end++] = neighbor;
                  if (end == GOODIX_MILAN_EXTRACTION_COMPONENT_QUEUE_CAPACITY)
                    end = 0;
                  pending++;
                }
            }
        }
    }

  for (guint slot = 0; slot < G_N_ELEMENTS (selected_labels); slot++)
    {
      guint best_size = strict_size_floor;

      for (guint label = 1; label <= label_count; label++)
        if (component_sizes[label] > best_size)
          {
            selected_labels[slot] = label;
            best_size = component_sizes[label];
          }
      if (selected_labels[slot] != 0)
        component_sizes[selected_labels[slot]] = 0;
    }

  for (guint i = 0; i < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS; i++)
    mask[i] = (selected_labels[0] != 0 &&
               labels[i] == (gint) selected_labels[0]) ||
              (selected_labels[1] != 0 &&
               labels[i] == (gint) selected_labels[1]) ||
              (selected_labels[2] != 0 &&
               labels[i] == (gint) selected_labels[2])
                ? UINT8_MAX : 0;
}

static gint32
goodix_milan_match_update_extraction_classification (
  GoodixMilanExtractionClassificationState *state,
  const guint8 classification_source[GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS],
  const guint8 classification_validity[GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS],
  guint8       primary_histogram_state,
  guint8       promoted_secondary_histogram_state,
  gint         coverage,
  gint32       entry_low_class,
  gint32       entry_high_class,
  guint8      *published_class,
  const guint8 *optional_source,
  GBytes      **published_output)
{
  g_autofree guint8 *component_mask = g_malloc (
    GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS);
  g_autofree guint8 *current_classes = g_malloc0 (
    GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS);
  g_autofree guint8 *stable_classes = g_malloc0 (
    GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS);
  g_autofree gint *labels = g_new (gint,
                                   GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS);
  g_autofree gint *queue = g_new (gint,
                                  GOODIX_MILAN_EXTRACTION_COMPONENT_QUEUE_CAPACITY);
  guint stable_class1_count = 0;
  guint stable_class2_count = 0;
  gint32 current_class = primary_histogram_state;
  gint32 high_class;
  gint32 packed;
  gboolean append;

  for (guint i = 0; i < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS; i++)
    component_mask[i] = classification_source[i] < 0x80 ? UINT8_MAX : 0;
  goodix_milan_match_retain_class_components (
    component_mask, 71, 500, labels, queue);
  for (guint i = 0; i < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS; i++)
    if (component_mask[i] != 0)
      current_classes[i] = 1;

  for (guint i = 0; i < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS; i++)
    component_mask[i] = classification_source[i] >= 0x80 &&
                         classification_validity[i] != 0 ? UINT8_MAX : 0;
  goodix_milan_match_retain_class_components (
    component_mask, 71, 500, labels, queue);
  for (guint i = 0; i < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS; i++)
    if (component_mask[i] != 0)
      current_classes[i] = 2;

  if (state->retained_count == 3)
    {
      for (guint class_value = 1; class_value <= 2; class_value++)
        {
          for (guint i = 0;
               i < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS;
               i++)
            component_mask[i] =
              current_classes[i] == class_value &&
              state->retained_class_planes[0][i] == class_value &&
              state->retained_class_planes[1][i] == class_value &&
              state->retained_class_planes[2][i] == class_value ?
              UINT8_MAX : 0;
          goodix_milan_match_retain_class_components (
            component_mask, 31, 350, labels, queue);
          for (guint i = 0;
               i < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS;
               i++)
            if (component_mask[i] != 0 && stable_classes[i] == 0)
              stable_classes[i] = (guint8) class_value;
        }
    }

  for (guint i = 0; i < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS; i++)
    {
      stable_class1_count += stable_classes[i] == 1;
      stable_class2_count += stable_classes[i] == 2;
    }
  if (stable_class1_count > 600)
    {
      if (current_class <= 2)
        current_class += 3;
    }
  else if (stable_class2_count > 600)
    {
      if (current_class == 0)
        current_class = 3;
    }
  else if ((stable_class1_count > 300 || stable_class2_count > 300) &&
           current_class == 0)
    current_class = 2;

  high_class = MAX (entry_high_class, current_class);
  if (high_class < 2 && promoted_secondary_histogram_state > 0)
    high_class++;
  if (high_class > 1 || current_class > 1 ||
      promoted_secondary_histogram_state > 1)
    {
      if (state->high_class_hysteresis < 5)
        state->high_class_hysteresis++;
    }
  else if (state->high_class_hysteresis != 0)
    state->high_class_hysteresis--;
  if (state->high_class_hysteresis >= 3)
    high_class = MAX (high_class, 1);
  packed = high_class * 0x100 + entry_low_class;

  append = coverage >= 75;
  if (append)
    {
      if (state->retained_count < 3)
        memcpy (state->retained_class_planes[state->retained_count],
                current_classes,
                GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS);
      else
        {
          memcpy (state->retained_class_planes[0],
                  state->retained_class_planes[1],
                  GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS);
          memcpy (state->retained_class_planes[1],
                  state->retained_class_planes[2],
                  GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS);
          memcpy (state->retained_class_planes[2], current_classes,
                  GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS);
        }
      if (state->retained_count < 3)
        state->retained_count++;
    }
  *published_class = (guint8) current_class;
  guint8 output[52 * 44];
  gsize output_size = 3;

  if (current_class == 3 || current_class == 4)
    {
      for (guint y = 0; y < 44; y++)
        for (guint x = 0; x < 52; x++)
          output[y * 52 + x] = stable_classes[y * 2 * 104 + x * 2] ? 2 : 1;
      output_size = sizeof (output);
    }
  else if (current_class == 5)
    {
      if (!optional_source)
        return packed;
      for (guint y = 0; y < 44; y++)
        for (guint x = 0; x < 52; x++)
          output[y * 52 + x] = optional_source[y * 2 * 108 + x * 2];
      output_size = sizeof (output);
    }
  /* The native summary overwrites the first three projected pixels. Ordinary
   * calls have selector zero. Do not manufacture a mode-5 workspace source. */
  output[0] = (guint8) current_class;
  output[1] = 0;
  output[2] = promoted_secondary_histogram_state;
  *published_output = g_bytes_new (output, output_size);
  return packed;
}

static void
goodix_milan_match_decode_entry_classes (const guint8 *image,
                                    gint32       *packed,
                                    gint32       *low_class,
                                    gint32       *high_class,
                                    guint8      **broken_mask)
{
  static const gint32 low_classes[4] = { 0, 2, 2, 3 };
  static const gint32 high_classes[8] = { 0, 1, 2, 4, 5, 5, 0, 0 };
  const guint metadata_start = GOODIX_MILAN_SENSOR_COLUMNS - 8;
  guint32 raw_low;
  guint32 raw_high;

  *packed = 0;
  *low_class = 0;
  *high_class = 0;
  *broken_mask = NULL;
  for (guint column = 0; column < metadata_start; column++)
    if ((image[column] & 1) != (column & 1))
      return;
  raw_low = (image[metadata_start] & 1) != 0
              ? ((image[metadata_start + 1] & 1) != 0 ? 2 : 1)
              : ((image[metadata_start + 1] & 1) != 0 ? 3 : 0);
  raw_high = (image[metadata_start + 3] & 1) |
             ((image[metadata_start + 4] & 1) << 1) |
             ((image[metadata_start + 5] & 1) << 2);
  *packed = (gint32) (raw_low + (raw_high << 8));
  *low_class = low_classes[raw_low];
  *high_class = high_classes[raw_high];
  if ((image[metadata_start + 2] & 1) != 0)
    {
      *broken_mask = g_malloc (GOODIX_MILAN_SENSOR_PIXELS);
      memset (*broken_mask, 1, GOODIX_MILAN_SENSOR_COLUMNS);
      for (guint pixel = GOODIX_MILAN_SENSOR_COLUMNS;
           pixel < GOODIX_MILAN_SENSOR_PIXELS; pixel++)
        (*broken_mask)[pixel] = image[pixel] & 1;
    }
}

static GoodixMilanExtractionStatus goodix_milan_match_extract_planes (const guint8                              *image,
                                                                      const guint8                              *primary_contrast_plane,
                                                                      GoodixMilanExtractionClassificationState  *classification_state,
                                                                      const GoodixMilanExtractionAuxiliaryState *auxiliary_state,
                                                                      const guint8                              *auxiliary_plane,
                                                                      const guint16                             *calibration,
                                                                      const guint16                             *raw_frame,
                                                                      guint16                                    t_code,
                                                                      guint16                                    dac_high,
                                                                      guint16                                    dac_low,
                                                                      guint16                                    sensor_subtype,
                                                                      gint32                                     calibration_scalar,
                                                                      GoodixMatchInfo                          **result,
                                                                      GoodixMilanExtractionDiagnostics          *diagnostics);

GoodixMatchInfo *
goodix_milan_match_extract_native (const guint8               *image,
                             GoodixMilanPreprocessState *preprocess_state,
                             const guint16              *raw_frame,
                             guint16                     t_code,
                             guint16                     dac_high,
                             guint16                     dac_low,
                             guint16                     sensor_subtype)
{
  GoodixMatchInfo *info = NULL;

  (void) goodix_milan_match_extract_native_result (
    image, preprocess_state, raw_frame, t_code, dac_high, dac_low,
    sensor_subtype, &info);
  return info;
}

GoodixMilanExtractionStatus
goodix_milan_match_extract_native_result (
  const guint8               *image,
  GoodixMilanPreprocessState *preprocess_state,
  const guint16              *raw_frame,
  guint16                     t_code,
  guint16                     dac_high,
  guint16                     dac_low,
  guint16                     sensor_subtype,
  GoodixMatchInfo           **info)
{
  if (info)
    *info = NULL;
  if (!image || !preprocess_state || !raw_frame ||
      !preprocess_state->primary_contrast_valid || !info)
    return GOODIX_MILAN_EXTRACTION_INVALID;

  return goodix_milan_match_extract_planes (
    image, preprocess_state->primary_contrast,
    &preprocess_state->extraction_classification,
    &preprocess_state->extraction_auxiliary,
    preprocess_state->extraction_auxiliary_valid ?
    preprocess_state->extraction_auxiliary_workspace : NULL,
    preprocess_state->setup_map,
    raw_frame, t_code, dac_high, dac_low, sensor_subtype,
    preprocess_state->selected_refined, info, NULL);
}

#ifdef GOODIX53X5_DEBUG
GoodixMilanExtractionStatus
goodix_milan_match_extract_native_result_debug (
  const guint8               *image,
  GoodixMilanPreprocessState *preprocess_state,
  const guint16              *raw_frame,
  guint16                     t_code,
  guint16                     dac_high,
  guint16                     dac_low,
  guint16                     sensor_subtype,
  GoodixMatchInfo           **info,
  GoodixMilanExtractionDiagnostics *diagnostics)
{
  if (info)
    *info = NULL;
  if (diagnostics)
    memset (diagnostics, 0, sizeof(*diagnostics));
  if (!image || !preprocess_state || !raw_frame ||
      !preprocess_state->primary_contrast_valid || !info)
    return GOODIX_MILAN_EXTRACTION_INVALID;

  return goodix_milan_match_extract_planes (
    image, preprocess_state->primary_contrast,
    &preprocess_state->extraction_classification,
    &preprocess_state->extraction_auxiliary,
    preprocess_state->extraction_auxiliary_valid ?
    preprocess_state->extraction_auxiliary_workspace : NULL,
    preprocess_state->setup_map,
    raw_frame, t_code, dac_high, dac_low, sensor_subtype,
    preprocess_state->selected_refined, info, diagnostics);
}
#endif

static GoodixMilanExtractionStatus
goodix_milan_match_extract_planes (const guint8                              *image,
                                   const guint8                              *primary_contrast_plane,
                                   GoodixMilanExtractionClassificationState  *classification_state,
                                   const GoodixMilanExtractionAuxiliaryState *auxiliary_state,
                                   const guint8                              *auxiliary_plane,
                                   const guint16                             *calibration,
                                   const guint16                             *raw_frame,
                                   guint16                                    t_code,
                                   guint16                                    dac_high,
                                   guint16                                    dac_low,
                                   guint16                                    sensor_subtype,
                                   gint32                                     calibration_scalar,
                                   GoodixMatchInfo                          **result,
                                   GoodixMilanExtractionDiagnostics          *diagnostics)
{
  GoodixMatchInfo *info = NULL;
  guint8 *cropped = NULL;
  guint8 *high = NULL;
  guint8 *low = NULL;
  guint8 *feature_mask = NULL;
  guint8 *orientation = NULL;
  guint8 *enhanced = NULL;
  guint8 *enhanced_bitmap = NULL;
  guint8 *inline_mask = NULL;
  guint8 *validity_mask = NULL;
  guint8 *broken_mask = NULL;
  GoodixMilanAntifakeBlob *antifake = NULL;
  GoodixMilanFeatureRecord *records = NULL;
  guint8 *feature_element = NULL;
  guint8 *milan_template = NULL;
  guint8 tail_state[0x520] = { 0 };
  GoodixMilanFeatureTemplateFields fields = { 0 };
  size_t record_count = 0;
  size_t zero_count = 0;
  size_t record_limit;
  size_t feature_element_size = 0;
  size_t milan_template_size = 0;
  guint8 enhanced_threshold;
  guint8 published_class;
  gint32 entry_low_class = 0;
  gint32 entry_high_class = 0;
  int quality;
  int coverage;
  GoodixMilanExtractionStatus status = GOODIX_MILAN_EXTRACTION_INVALID;

  *result = NULL;
  if (!image || !primary_contrast_plane || !classification_state ||
      !auxiliary_state)
    return status;
  published_class = auxiliary_plane ? auxiliary_state->primary_histogram_state : 0;
  info = g_new0 (GoodixMatchInfo, 1);
  cropped = g_malloc (GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS);
  high = g_malloc0 (286);
  low = g_malloc0 (286);
  feature_mask = g_malloc0 (52 * 44);
  orientation = g_malloc (GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS);
  enhanced = g_malloc (GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS);
  enhanced_bitmap = g_malloc0 (286);
  inline_mask = g_malloc0 (72);
  validity_mask = g_malloc (GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS);
  antifake = g_malloc0 (sizeof(*antifake));
  records = g_new0 (GoodixMilanFeatureRecord, 150);
  feature_element = g_malloc (7945 + 150 * 32 + GOODIX_MILAN_OPTIONAL_C7_LEN);
  milan_template = g_malloc (7945 + 150 * 32 + GOODIX_MILAN_OPTIONAL_C7_LEN +
                             1433);
  for (size_t row = 0;
       row < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS;
       row++)
    {
      memcpy (cropped +
                row * GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS,
              image + row * GOODIX_MILAN_SENSOR_COLUMNS + 2,
              GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS);
    }
  if (sensor_subtype == GOODIX_MILAN_PRINT_SENSOR_TYPE)
    goodix_milan_match_decode_entry_classes (
      image, &fields.optional_c7, &entry_low_class, &entry_high_class,
      &broken_mask);
  if (goodix_milan_preprocess_quality (
        image, GOODIX_MILAN_SENSOR_ROWS, GOODIX_MILAN_SENSOR_COLUMNS,
        &quality, &coverage) != 0)
    goto out;
  record_limit = goodix_milan_match_record_limit (sensor_subtype, coverage, 150);
  if (goodix_milan_feature_base_maps_with_validity (
        image, GOODIX_MILAN_SENSOR_ROWS, GOODIX_MILAN_SENSOR_COLUMNS,
        high, low, feature_mask, inline_mask, validity_mask) != 0 ||
      goodix_milan_feature_enhance (
        cropped, GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS,
        GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS, orientation,
        enhanced) != 0 ||
      goodix_milan_feature_enhanced_bitmap (
        enhanced, feature_mask, GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS,
        GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS, enhanced_bitmap,
        &enhanced_threshold) != 0)
    goto out;
  if (sensor_subtype == GOODIX_MILAN_PRINT_SENSOR_TYPE && auxiliary_plane)
    {
      /* Native commits history before record extraction, anti-fake, or packing
       * failures. */
      fields.optional_c7 = goodix_milan_match_update_extraction_classification (
        classification_state, enhanced, validity_mask,
        auxiliary_state->primary_histogram_state,
        auxiliary_state->promoted_secondary_histogram_state, coverage,
        entry_low_class, entry_high_class, &published_class, auxiliary_plane,
        &info->classification);
      if (!info->classification)
        goto out;
    }
  if (goodix_milan_feature_extract_records_mode_masked (
        image, GOODIX_MILAN_SENSOR_ROWS, GOODIX_MILAN_SENSOR_COLUMNS, records,
        record_limit, &record_count, &zero_count, 1, broken_mask, validity_mask,
        entry_high_class) != 0)
    goto out;
  if (broken_mask && entry_high_class >= 4)
    /* The owned half-resolution mask was copied before filtering; only the
     * later inline reduction observes the cleared dense validity mask. */
    goodix_milan_feature_pack_inline_mask (
      validity_mask, GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS,
      GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS, inline_mask);
  if (calibration && raw_frame &&
      goodix_milan_antifake_build (
        calibration, raw_frame, primary_contrast_plane, feature_mask,
        52 * 44,
        GOODIX_MILAN_SENSOR_ROWS, GOODIX_MILAN_SENSOR_COLUMNS, t_code, dac_high,
        dac_low,
        sensor_subtype, calibration_scalar, antifake, sizeof(*antifake)) != 0)
    goto out;
  goodix_milan_debug_extraction_antifake (
    diagnostics, calibration, raw_frame, primary_contrast_plane, feature_mask,
    t_code, dac_high, dac_low, sensor_subtype, calibration_scalar);
  goodix_milan_feature_mask_expand (inline_mask, feature_mask);
  if (calibration && raw_frame &&
      goodix_milan_template_initialize_tail (
        image, GOODIX_MILAN_SENSOR_ROWS, GOODIX_MILAN_SENSOR_COLUMNS, tail_state,
        sizeof(tail_state)) != 0)
    goto out;
  fields.tagged_values[2] = (int32_t) zero_count;
  fields.tagged_values[3] = quality;
  fields.tagged_values[4] = coverage;
  if (goodix_milan_template_pack_feature_element (
        high, enhanced_bitmap, inline_mask, low, records, record_count,
        antifake, &fields, feature_element,
        7945 + 150 * 32 + GOODIX_MILAN_OPTIONAL_C7_LEN,
        &feature_element_size) != 0 ||
      goodix_milan_template_pack_one_feature (
        feature_element, feature_element_size, tail_state, sizeof(tail_state),
        milan_template,
        7945 + 150 * 32 + GOODIX_MILAN_OPTIONAL_C7_LEN + 1433,
        &milan_template_size) != 0)
    goto out;
  info->template = g_bytes_new_take (milan_template, milan_template_size);
  milan_template = NULL;
  info->record_count = (int) record_count;
  info->partition_count = (int) zero_count;
  memcpy (info->feature_bitmaps.high_bitmap, high,
          sizeof(info->feature_bitmaps.high_bitmap));
  memcpy (info->feature_bitmaps.enhanced_bitmap, enhanced_bitmap,
          sizeof(info->feature_bitmaps.enhanced_bitmap));
  memcpy (info->feature_bitmaps.low_bitmap, low,
          sizeof(info->feature_bitmaps.low_bitmap));
  memcpy (info->inline_mask, inline_mask, sizeof(info->inline_mask));
  goodix_milan_match_pack_rescue_mask (feature_mask, info->rescue_mask);
  memcpy (&info->antifake, antifake, sizeof(info->antifake));
  info->records = records;
  info->extraction_metadata.quality = quality;
  info->extraction_metadata.coverage = coverage;
  info->extraction_metadata.optional_c7 = fields.optional_c7;
  if (auxiliary_plane)
    info->extraction_metadata.auxiliary = *auxiliary_state;
  /* Live classification publishes the history-promoted mode, independently of
   * the preprocessing seed and the packed c7 high class. */
  info->extraction_metadata.auxiliary.primary_histogram_state = published_class;
  records = NULL;
  status = GOODIX_MILAN_EXTRACTION_OK;

out:
  g_free (milan_template);
  g_free (feature_element);
  g_free (records);
  g_free (antifake);
  g_free (broken_mask);
  g_free (validity_mask);
  g_free (inline_mask);
  g_free (enhanced_bitmap);
  g_free (enhanced);
  g_free (orientation);
  g_free (feature_mask);
  g_free (low);
  g_free (high);
  g_free (cropped);
  if (!info->template)
    g_clear_pointer (&info, goodix_milan_match_free_info);
  *result = info;
  return status;
}

int
goodix_milan_match_keypoints_count (GoodixMatchInfo *info)
{
  return info ? info->record_count : 0;
}

GoodixMatchInfo *
goodix_milan_match_info_new_empty (void)
{
  return g_new0 (GoodixMatchInfo, 1);
}

void
goodix_milan_match_info_clear (GoodixMatchInfo *info)
{
  if (!info)
    return;
  g_clear_pointer (&info->template, g_bytes_unref);
  g_clear_pointer (&info->records, g_free);
  g_clear_pointer (&info->classification, g_bytes_unref);
  memset (info, 0, sizeof(*info));
}

gboolean
goodix_milan_match_info_is_complete (const GoodixMatchInfo *info)
{
  return info && info->template && info->records && info->record_count > 0 &&
         info->record_count <= 150 && info->partition_count >= 0 &&
         info->partition_count <= info->record_count;
}

gboolean
goodix_milan_match_info_copy (GoodixMatchInfo       *destination,
                        const GoodixMatchInfo *source)
{
  GoodixMatchInfo copy = { 0 };
  const guint8 *template_data;
  gsize template_size;

  if (!destination || !goodix_milan_match_info_is_complete (source))
    return FALSE;
  template_data = g_bytes_get_data (source->template, &template_size);
  copy.template = g_bytes_new (template_data, template_size);
  copy.records = g_memdup2 (
    source->records,
    (gsize) source->record_count * sizeof(*source->records));
  for (int i = 0; i < source->record_count; i++)
    {
      memset (copy.records[i].payload + 24, 0, 4);
      memset (copy.records[i].payload + 36, 0, 8);
    }
  copy.record_count = source->record_count;
  copy.partition_count = source->partition_count;
  memcpy (copy.feature_bitmaps.high_bitmap,
          source->feature_bitmaps.high_bitmap,
          sizeof(copy.feature_bitmaps.high_bitmap));
  memcpy (copy.feature_bitmaps.enhanced_bitmap,
          source->feature_bitmaps.enhanced_bitmap,
          sizeof(copy.feature_bitmaps.enhanced_bitmap));
  memcpy (copy.feature_bitmaps.low_bitmap,
          source->feature_bitmaps.low_bitmap,
          sizeof(copy.feature_bitmaps.low_bitmap));
  memcpy (copy.inline_mask, source->inline_mask, sizeof(copy.inline_mask));
  memcpy (copy.rescue_mask, source->rescue_mask, sizeof(copy.rescue_mask));
  memcpy (&copy.antifake, &source->antifake, sizeof(copy.antifake));
  copy.extraction_metadata = source->extraction_metadata;
  /* FUN_180045a50 does not transfer the live histogram/plane owner. Both
   * callers create queue or gallery owners, which have no live summary. */
  memset (&copy.extraction_metadata.auxiliary, 0,
          sizeof(copy.extraction_metadata.auxiliary));

  goodix_milan_match_info_clear (destination);
  *destination = copy;
  return TRUE;
}

void
goodix_milan_match_free_info (GoodixMatchInfo *info)
{
  if (!info)
    return;
  goodix_milan_match_info_clear (info);
  g_free (info);
}
