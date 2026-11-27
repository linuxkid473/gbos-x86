/*
 * Goodix 53x5 driver for libfprint - Milan algorithm debug hooks
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "milan/runtime.h"
#include "milan/study/queue.h"

#ifdef GOODIX53X5_DEBUG
void goodix_milan_debug_runtime_gallery_result_free (GoodixMilanRuntimeGalleryResult *result);
void goodix_milan_debug_runtime_setup_failed (GoodixMilanRuntimeOutput *output,
                                              gint32                    status);
void goodix_milan_debug_runtime_preprocess_started (GoodixMilanRuntimeOutput *output);
void goodix_milan_debug_runtime_preprocess_status (GoodixMilanRuntimeOutput *output,
                                                   gint32                    status);
void goodix_milan_debug_runtime_preprocess_finished (GoodixMilanRuntimeOutput *output,
                                                     gint32                    status,
                                                     const guint8             *processed);
void goodix_milan_debug_runtime_extraction_started (GoodixMilanRuntimeOutput *output);
void goodix_milan_debug_runtime_extraction_finished (GoodixMilanRuntimeOutput *output);
void goodix_milan_debug_runtime_gallery_input (GoodixMilanRuntimeGalleryResult *result,
                                               GBytes                          *input_template);
void goodix_milan_debug_runtime_gallery_validated (GoodixMilanRuntimeGalleryResult *result);
void goodix_milan_debug_runtime_queue_before_match (GoodixMilanRuntimeGalleryResult *result,
                                                    const GoodixStudyQueue          *queue);
void goodix_milan_debug_runtime_queue_after_match (GoodixMilanRuntimeGalleryResult *result,
                                                   const GoodixStudyQueue          *queue);
void goodix_milan_debug_runtime_after_match (GoodixMilanRuntimeGalleryResult *result,
                                             GBytes                          *after_match);
void goodix_milan_debug_runtime_study_started (GoodixMilanRuntimeOutput *output);
void goodix_milan_debug_runtime_queue_after_study (GoodixMilanRuntimeOutput *output,
                                                   gsize                     winner_position,
                                                   const GoodixStudyQueue   *queue);
void goodix_milan_debug_runtime_study_finished (GoodixMilanRuntimeOutput *output);
void goodix_milan_debug_runtime_output_free (GoodixMilanRuntimeOutput *output);
void goodix_milan_debug_extraction_antifake (GoodixMilanExtractionDiagnostics *diagnostics,
                                             const guint16                    *calibration,
                                             const guint16                    *raw_frame,
                                             const guint8                     *primary_contrast_plane,
                                             const guint8                     *feature_mask,
                                             guint16                           t_code,
                                             guint16                           dac_high,
                                             guint16                           dac_low,
                                             guint16                           sensor_subtype,
                                             gint32                            calibration_scalar);
#else
static inline void
goodix_milan_debug_runtime_gallery_result_free (
  GoodixMilanRuntimeGalleryResult *result)
{
  (void) result;
}

static inline void
goodix_milan_debug_runtime_setup_failed (GoodixMilanRuntimeOutput *output,
                                         gint32                    status)
{
  (void) output;
  (void) status;
}

static inline void
goodix_milan_debug_runtime_preprocess_started (GoodixMilanRuntimeOutput *output)
{
  (void) output;
}

static inline void
goodix_milan_debug_runtime_preprocess_status (GoodixMilanRuntimeOutput *output,
                                              gint32                    status)
{
  (void) output;
  (void) status;
}

static inline void
goodix_milan_debug_runtime_preprocess_finished (GoodixMilanRuntimeOutput *output,
                                                gint32                    status,
                                                const guint8             *processed)
{
  (void) output;
  (void) status;
  (void) processed;
}

static inline void
goodix_milan_debug_runtime_extraction_started (GoodixMilanRuntimeOutput *output)
{
  (void) output;
}

static inline void
goodix_milan_debug_runtime_extraction_finished (GoodixMilanRuntimeOutput *output)
{
  (void) output;
}

static inline void
goodix_milan_debug_runtime_gallery_input (GoodixMilanRuntimeGalleryResult *result,
                                          GBytes                          *input_template)
{
  (void) result;
  (void) input_template;
}

static inline void
goodix_milan_debug_runtime_gallery_validated (GoodixMilanRuntimeGalleryResult *result)
{
  (void) result;
}

static inline void
goodix_milan_debug_runtime_queue_before_match (GoodixMilanRuntimeGalleryResult *result,
                                               const GoodixStudyQueue          *queue)
{
  (void) result;
  (void) queue;
}

static inline void
goodix_milan_debug_runtime_queue_after_match (GoodixMilanRuntimeGalleryResult *result,
                                              const GoodixStudyQueue          *queue)
{
  (void) result;
  (void) queue;
}

static inline void
goodix_milan_debug_runtime_after_match (GoodixMilanRuntimeGalleryResult *result,
                                        GBytes                          *after_match)
{
  (void) result;
  (void) after_match;
}

static inline void
goodix_milan_debug_runtime_study_started (GoodixMilanRuntimeOutput *output)
{
  (void) output;
}

static inline void
goodix_milan_debug_runtime_queue_after_study (GoodixMilanRuntimeOutput *output,
                                              gsize                     winner_position,
                                              const GoodixStudyQueue   *queue)
{
  (void) output;
  (void) winner_position;
  (void) queue;
}

static inline void
goodix_milan_debug_runtime_study_finished (GoodixMilanRuntimeOutput *output)
{
  (void) output;
}

static inline void
goodix_milan_debug_runtime_output_free (GoodixMilanRuntimeOutput *output)
{
  (void) output;
}

static inline void
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
  (void) diagnostics;
  (void) calibration;
  (void) raw_frame;
  (void) primary_contrast_plane;
  (void) feature_mask;
  (void) t_code;
  (void) dac_high;
  (void) dac_low;
  (void) sensor_subtype;
  (void) calibration_scalar;
}
#endif

#if defined(GOODIX53X5_DEBUG) || defined(GOODIX53X5_PARITY)
void goodix_milan_debug_runtime_hash_after_match (GoodixMilanRuntimeGalleryResult *result,
                                                  GBytes                          *after_match);
#else
static inline void
goodix_milan_debug_runtime_hash_after_match (GoodixMilanRuntimeGalleryResult *result,
                                             GBytes                          *after_match)
{
  (void) result;
  (void) after_match;
}
#endif
