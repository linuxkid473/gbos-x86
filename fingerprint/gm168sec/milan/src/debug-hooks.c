/*
 * Goodix 53x5 driver for libfprint - Milan algorithm debug hooks
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/debug-hooks.h"

#include "milan/antifake/antifake.h"

#include <string.h>

#if defined(GOODIX53X5_DEBUG) || defined(GOODIX53X5_PARITY)
static void
goodix_milan_debug_hash_bytes (GBytes *bytes,
                               gchar   digest[65])
{
  gconstpointer data;
  gsize size;
  g_autofree gchar *checksum = NULL;

  digest[0] = '\0';
  if (!bytes)
    return;
  data = g_bytes_get_data (bytes, &size);
  checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, data, size);
  g_strlcpy (digest, checksum ? checksum : "", 65);
}

void
goodix_milan_debug_runtime_hash_after_match (
  GoodixMilanRuntimeGalleryResult *result,
  GBytes                          *after_match)
{
  goodix_milan_debug_hash_bytes (after_match, result->after_match_sha256);
}
#endif

#ifdef GOODIX53X5_DEBUG
static void
goodix_milan_debug_hash_projection (
  const GoodixMilanAntifakeBlob *projection,
  gchar                          digest[GOODIX_MILAN_EXTRACTION_SHA256_SIZE])
{
  g_autofree gchar *checksum = g_compute_checksum_for_data (
    G_CHECKSUM_SHA256, goodix_milan_antifake_const_data (projection),
    GOODIX_MILAN_ANTIFAKE_DEFINED_MATERIAL_SIZE);

  g_strlcpy (digest, checksum ? checksum : "",
             GOODIX_MILAN_EXTRACTION_SHA256_SIZE);
}

void
goodix_milan_debug_extraction_antifake (
  GoodixMilanExtractionDiagnostics *diagnostics,
  const guint16                    *calibration,
  const guint16                    *raw_frame,
  const guint8                     *primary_contrast_plane,
  const guint8                     *feature_mask,
  guint16                           t_code,
  guint16                           dac_high,
  guint16                           dac_low,
  guint16                           sensor_subtype,
  gint32                            calibration_scalar)
{
  g_autofree GoodixMilanAntifakeBlob *diagnostic_antifake = NULL;

  if (diagnostics && calibration && raw_frame &&
      (diagnostic_antifake = g_try_malloc0 (
         sizeof (*diagnostic_antifake))) != NULL)
    {
      GoodixMilanAntifakeBoundaryResult boundary = { 0 };
      int status = goodix_milan_antifake_build_with_boundary (
        calibration, raw_frame, primary_contrast_plane, feature_mask, 52 * 44,
        GOODIX_MILAN_SENSOR_ROWS, GOODIX_MILAN_SENSOR_COLUMNS, t_code,
        dac_high, dac_low, sensor_subtype, calibration_scalar,
        diagnostic_antifake, sizeof (*diagnostic_antifake), &boundary);

      if (status == 0 || status == GOODIX_MILAN_ANTIFAKE_AMBIGUOUS)
        {
          diagnostics->boundary_classification = boundary.classification;
          diagnostics->zero_candidate_count = boundary.zero_candidate_count;
          diagnostics->nonzero_candidate_count = boundary.nonzero_candidate_count;
          goodix_milan_debug_hash_projection (
            &boundary.zero_projection, diagnostics->zero_projection_sha256);
          goodix_milan_debug_hash_projection (
            &boundary.nonzero_projection,
            diagnostics->nonzero_projection_sha256);
        }
    }
}

void
goodix_milan_debug_runtime_gallery_result_free (
  GoodixMilanRuntimeGalleryResult *result)
{
  g_clear_pointer (&result->input_template, g_bytes_unref);
  g_clear_pointer (&result->after_match_template, g_bytes_unref);
}

void
goodix_milan_debug_runtime_setup_failed (GoodixMilanRuntimeOutput *output,
                                         gint32                    status)
{
  output->preprocess_attempted = TRUE;
  output->preprocess_status = status;
  output->preprocess_status_available = TRUE;
}

void
goodix_milan_debug_runtime_preprocess_started (GoodixMilanRuntimeOutput *output)
{
  output->preprocess_attempted = TRUE;
}

void
goodix_milan_debug_runtime_preprocess_status (GoodixMilanRuntimeOutput *output,
                                              gint32                    status)
{
  output->preprocess_status = status;
  output->preprocess_status_available = TRUE;
}

void
goodix_milan_debug_runtime_preprocess_finished (GoodixMilanRuntimeOutput *output,
                                                gint32                    status,
                                                const guint8             *processed)
{
  output->preprocess_completed = status == 0;
  if (output->preprocess_completed ||
      status == GOODIX_MILAN_PREPROCESS_RETRY_CLASSIFICATION)
    output->processed_image = g_bytes_new (processed,
                                           GOODIX_MILAN_SENSOR_PIXELS);
}

void
goodix_milan_debug_runtime_extraction_started (GoodixMilanRuntimeOutput *output)
{
  output->extraction_attempted = TRUE;
}

void
goodix_milan_debug_runtime_extraction_finished (GoodixMilanRuntimeOutput *output)
{
  output->extraction_completed = TRUE;
}

void
goodix_milan_debug_runtime_gallery_input (GoodixMilanRuntimeGalleryResult *result,
                                          GBytes                          *input_template)
{
  result->input_template = g_bytes_ref (input_template);
  goodix_milan_debug_hash_bytes (input_template, result->input_template_sha256);
}

void
goodix_milan_debug_runtime_gallery_validated (GoodixMilanRuntimeGalleryResult *result)
{
  result->validation_observed = TRUE;
}

void
goodix_milan_debug_runtime_queue_before_match (GoodixMilanRuntimeGalleryResult *result,
                                               const GoodixStudyQueue          *queue)
{
  result->queue_before_match_observed = TRUE;
  result->queue_state_before_match = queue->enabled_state;
  result->queue_counter_before_match = queue->transaction_counter;
  result->queue_occupied_before_match = goodix_milan_study_queue_occupied (queue);
}

void
goodix_milan_debug_runtime_queue_after_match (GoodixMilanRuntimeGalleryResult *result,
                                              const GoodixStudyQueue          *queue)
{
  result->queue_after_match_observed = TRUE;
  result->queue_occupied_after_match = goodix_milan_study_queue_occupied (queue);
}

void
goodix_milan_debug_runtime_after_match (GoodixMilanRuntimeGalleryResult *result,
                                        GBytes                          *after_match)
{
  result->after_match_template = g_bytes_ref (after_match);
}

void
goodix_milan_debug_runtime_study_started (GoodixMilanRuntimeOutput *output)
{
  output->study_attempted = TRUE;
}

void
goodix_milan_debug_runtime_queue_after_study (GoodixMilanRuntimeOutput *output,
                                              gsize                     winner_position,
                                              const GoodixStudyQueue   *queue)
{
  GoodixMilanRuntimeGalleryResult *result =
    g_ptr_array_index (output->gallery_results, winner_position);

  result->queue_after_study_observed = TRUE;
  result->queue_state_after_study = queue->enabled_state;
  result->queue_counter_after_study = queue->transaction_counter;
  result->queue_occupied_after_study = goodix_milan_study_queue_occupied (queue);
}

void
goodix_milan_debug_runtime_study_finished (GoodixMilanRuntimeOutput *output)
{
  output->study_completed = TRUE;
}

void
goodix_milan_debug_runtime_output_free (GoodixMilanRuntimeOutput *output)
{
  g_clear_pointer (&output->processed_image, g_bytes_unref);
}
#endif
