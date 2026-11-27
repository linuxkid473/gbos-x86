/*
 * Goodix 53x5 driver for libfprint - native Milan runtime transaction
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <glib.h>

#include "milan/match/match.h"
#include "milan/preprocess/state.h"
#include "milan/print.h"

typedef struct _GoodixMilanRuntimeGalleryInput GoodixMilanRuntimeGalleryInput;
typedef struct _GoodixMilanRuntimeInput GoodixMilanRuntimeInput;

typedef enum
{
  GOODIX_MILAN_RUNTIME_MATCH,
  GOODIX_MILAN_RUNTIME_NO_MATCH,
  GOODIX_MILAN_RUNTIME_RETRY,
  GOODIX_MILAN_RUNTIME_INVALID_DATA,
  GOODIX_MILAN_RUNTIME_CANCELLED,
} GoodixMilanRuntimeStatus;

typedef enum
{
  GOODIX_MILAN_RUNTIME_CHECKPOINT_NONE,
  GOODIX_MILAN_RUNTIME_CHECKPOINT_BEFORE_PREPROCESS,
  GOODIX_MILAN_RUNTIME_CHECKPOINT_AFTER_PREPROCESS,
  GOODIX_MILAN_RUNTIME_CHECKPOINT_AFTER_EXTRACT,
  GOODIX_MILAN_RUNTIME_CHECKPOINT_BEFORE_GALLERY,
  GOODIX_MILAN_RUNTIME_CHECKPOINT_AFTER_GALLERY,
  GOODIX_MILAN_RUNTIME_CHECKPOINT_BEFORE_STUDY,
  GOODIX_MILAN_RUNTIME_CHECKPOINT_AFTER_STUDY,
} GoodixMilanRuntimeCheckpoint;

typedef struct
{
  GoodixMilanRuntimeCheckpoint cancelled_at;
  gsize                         cancelled_gallery_position;
} GoodixMilanRuntimeCancellationMetadata;

typedef gboolean (*GoodixMilanRuntimeCancelFunc) (
  GoodixMilanRuntimeCheckpoint checkpoint,
  gsize                         gallery_position,
  gpointer                      user_data);

/* Called only after an entered setup succeeds, before live preprocessing.
 * Return an adapter status for preparation/serialization failure; publication
 * I/O errors are nonfatal. The input owns user_data through worker completion. */
typedef gint32 (*GoodixMilanRuntimeSetupFunc) (gpointer user_data);

typedef struct
{
  gsize                         gallery_position;
  guint                         gallery_index;
  gboolean                      valid;
  gboolean                      evaluated;
  gboolean                      accepted;
  gint32                        score;
  GoodixMilanMatchResult        match_result;
  GoodixMilanPrintTemplateInfo  template_info;
  GError                       *validation_error;
#ifdef GOODIX53X5_DEBUG
  GBytes                       *input_template;
  GBytes                       *after_match_template;
  gchar                         input_template_sha256[65];
#endif
#if defined(GOODIX53X5_DEBUG) || defined(GOODIX53X5_PARITY)
  gchar                         after_match_sha256[65];
#endif
#ifdef GOODIX53X5_DEBUG
  gboolean                      validation_observed;
  gboolean                      queue_before_match_observed;
  gboolean                      queue_after_match_observed;
  gboolean                      queue_after_study_observed;
  guint32                       queue_state_before_match;
  guint32                       queue_counter_before_match;
  gsize                         queue_occupied_before_match;
  gsize                         queue_occupied_after_match;
  guint32                       queue_state_after_study;
  guint32                       queue_counter_after_study;
  gsize                         queue_occupied_after_study;
#endif
} GoodixMilanRuntimeGalleryResult;

typedef struct _GoodixMilanRuntimeOutput
{
  GoodixMilanRuntimeStatus      status;
  guint64                       action_epoch;
  guint64                       generation_id;
  GoodixMilanPreprocessPurpose  purpose;
  guint16                       profile;
  guint16                       tcode;
  guint16                       dac_high;
  guint16                       dac_low;
  guint16                       sensor_subtype;
  GoodixMilanRuntimeCancellationMetadata cancellation;
  guint                         winner_index;
  gsize                         winner_position;
  gint32                        score;
  GoodixMilanMatchResult        match_result;
  GoodixMilanStudyAction        study_action;
  GBytes                       *final_candidate;
  GoodixMilanPreprocessState    preprocess_state;
  GoodixMilanProfileState       profile_state;
  gboolean                      preprocess_state_valid;
  gint                          quality;
  gint                          coverage;
#ifdef GOODIX53X5_DEBUG
  gboolean                      preprocess_attempted;
  gboolean                      preprocess_completed;
  gboolean                      preprocess_status_available;
  gint32                        preprocess_status;
  gboolean                      extraction_attempted;
  gboolean                      extraction_completed;
  gboolean                      study_attempted;
  gboolean                      study_completed;
  GBytes                       *processed_image;
#endif
  GBytes                       *probe_template;
  guint32                       probe_record_count;
  guint32                       probe_partition0_count;
  guint32                       probe_partition1_count;
  GoodixMilanPrintTemplateInfo  final_candidate_info;
  guint                         valid_gallery_count;
  guint                         invalid_gallery_count;
  guint                         evaluated_gallery_count;
  GPtrArray                    *gallery_results;
  GError                       *error;
  GError                       *learning_error;
  /* Sample admission precedes extraction and its possible retry. */
  gboolean                               enrollment_sample_admitted;
  /* Setup publication is independent of later live-state cancellation. */
  gboolean                               setup_state_valid;
  gboolean                               capture_health_present;
  gboolean                               capture_enroll_allowed;
  guint32                                admission_status;
  guint32                                admission_detail;
} GoodixMilanRuntimeOutput;

_Static_assert (offsetof (GoodixMilanRuntimeCancellationMetadata,
                          cancelled_at) == 0,
                "runtime cancellation checkpoint offset changed");
_Static_assert (offsetof (GoodixMilanRuntimeCancellationMetadata,
                          cancelled_gallery_position) == 8,
                "runtime cancellation gallery position offset changed");
_Static_assert (sizeof (GoodixMilanRuntimeCancellationMetadata) == 16,
                "runtime cancellation metadata size changed");
_Static_assert (_Alignof (GoodixMilanRuntimeCancellationMetadata) == 8,
                "runtime cancellation metadata alignment changed");
_Static_assert (offsetof (GoodixMilanRuntimeOutput, cancellation) == 40,
                "runtime output cancellation metadata offset changed");
_Static_assert (offsetof (GoodixMilanRuntimeOutput, cancellation) +
                  offsetof (GoodixMilanRuntimeCancellationMetadata,
                             cancelled_at) == 40,
                "runtime output cancellation checkpoint offset changed");
_Static_assert (offsetof (GoodixMilanRuntimeOutput, cancellation) +
                  offsetof (GoodixMilanRuntimeCancellationMetadata,
                             cancelled_gallery_position) == 48,
                "runtime output cancellation gallery position offset changed");
_Static_assert (offsetof (GoodixMilanRuntimeOutput, winner_index) == 56,
                "runtime output winner index offset changed");
_Static_assert (_Alignof (GoodixMilanRuntimeOutput) == 8,
                "runtime output alignment changed");

#define GOODIX_MILAN_ENROLL_MIN_QUALITY 25
#define GOODIX_MILAN_ENROLL_MIN_COVERAGE 65

gboolean goodix_milan_runtime_enrollment_admitted (
  const GoodixMilanRuntimeOutput *output);

GoodixMilanRuntimeGalleryInput *goodix_milan_runtime_gallery_input_new (
  guint   gallery_index,
  GBytes *template_bytes);
void goodix_milan_runtime_gallery_input_free (
  GoodixMilanRuntimeGalleryInput *input);

GoodixMilanRuntimeInput *goodix_milan_runtime_input_new (
  guint64                               action_epoch,
  guint64                               generation_id,
  GoodixMilanPreprocessPurpose          purpose,
  const GoodixMilanPreprocessState     *preprocess_state,
  const GoodixMilanProfileState        *profile_state,
  const guint16                         setup_tx_on[GOODIX_MILAN_SENSOR_PIXELS],
  const guint16                         live_raw[GOODIX_MILAN_SENSOR_PIXELS],
  guint16                               tcode,
  guint16                               dac_high,
  guint16                               dac_low,
  guint16                               sensor_subtype,
  GoodixMilanRuntimeGalleryInput *const *gallery,
  gsize                                 gallery_count);
void goodix_milan_runtime_input_set_cancel_check (
  GoodixMilanRuntimeInput     *input,
  GoodixMilanRuntimeCancelFunc cancel_func,
  gpointer                     user_data,
  GDestroyNotify               destroy);
void goodix_milan_runtime_input_set_setup_hook (
  GoodixMilanRuntimeInput *input, GoodixMilanRuntimeSetupFunc setup_func,
  gpointer user_data, GDestroyNotify destroy);
/* Production binds the completed capture snapshot before worker submission.
 * Existing offline inputs without hardware evidence retain default allowance. */
void goodix_milan_runtime_input_set_capture_health (
  GoodixMilanRuntimeInput *input, gboolean enroll_allowed);
static inline gint32
goodix_milan_runtime_initialize_setup (GoodixMilanProfileState *profile_state,
                                       guint16                 *setup_tx_on)
{
  gboolean setup_required;

  g_return_val_if_fail (profile_state != NULL,
                        GOODIX_MILAN_PREPROCESS_RETRY_SETUP_ADMISSION);
  g_return_val_if_fail (setup_tx_on != NULL,
                        GOODIX_MILAN_PREPROCESS_RETRY_SETUP_ADMISSION);
  setup_required = !profile_state->setup_initialized ||
                   profile_state->setup_refresh_pending;
  if (!setup_required)
    return profile_state->setup_not_ready ?
           GOODIX_MILAN_PREPROCESS_NOT_READY : 0;

  /* Gain-ready is process state, distinct from native setup-ready. */
  profile_state->setup_refresh_pending = 0;
  /* Native retries setup once against the same retained frame. */
  for (guint attempt = 0; attempt < 2; attempt++)
    if (goodix_milan_preprocess_clamp_and_admit_raw (
          setup_tx_on, GOODIX_MILAN_SENSOR_ROWS,
          GOODIX_MILAN_SENSOR_COLUMNS, 2, 85))
      {
        profile_state->setup_initialized = 1;
        profile_state->setup_not_ready = 0;
        return 0;
      }

  profile_state->setup_not_ready = profile_state->setup_initialized;
  return GOODIX_MILAN_PREPROCESS_RETRY_SETUP_ADMISSION;
}
void goodix_milan_runtime_input_free (GoodixMilanRuntimeInput *input);

GoodixMilanRuntimeOutput *goodix_milan_runtime_run (
  const GoodixMilanRuntimeInput *input);
void goodix_milan_runtime_output_free (GoodixMilanRuntimeOutput *output);
