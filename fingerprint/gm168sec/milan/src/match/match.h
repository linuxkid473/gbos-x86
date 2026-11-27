/*
 * Goodix 53x5 driver for libfprint — native Milan template matching
 * Copyright (C) 2024 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include <glib.h>

#include "milan/milan.h"
#ifdef GOODIX53X5_DEBUG
#include "milan/relations.h"
#endif

typedef struct _GoodixMatchInfo                  GoodixMatchInfo;
typedef struct _GoodixStudyQueue                 GoodixStudyQueue;
typedef struct _GoodixMilanEnrollmentTransaction GoodixMilanEnrollmentTransaction;

typedef struct
{
  guint  pre_insertion_accepted_count;
  gint32 previous_overlap;
  gint32 overlap;
  guint  reject_detail;
} GoodixMilanEnrollmentResult;

typedef enum {
  GOODIX_MILAN_ENROLLMENT_ACCEPTED,
  GOODIX_MILAN_ENROLLMENT_RETRY_CENTER,
  GOODIX_MILAN_ENROLLMENT_RETRY_REMOVE,
} GoodixMilanEnrollmentAttemptStatus;

GoodixMilanEnrollmentTransaction *goodix_milan_enrollment_transaction_new (void);
void goodix_milan_enrollment_transaction_free (GoodixMilanEnrollmentTransaction *transaction);
guint goodix_milan_enrollment_transaction_count (const GoodixMilanEnrollmentTransaction *transaction);
GoodixMilanEnrollmentAttemptStatus goodix_milan_enrollment_transaction_attempt (GoodixMilanEnrollmentTransaction **transaction,
                                                                                GBytes                            *probe_template,
                                                                                guint                             *bad_record_count,
                                                                                guint                             *bad_continue_count,
                                                                                GoodixMilanEnrollmentResult       *result);
GBytes *goodix_milan_enrollment_transaction_publish (const GoodixMilanEnrollmentTransaction *transaction);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GoodixMilanEnrollmentTransaction,
                               goodix_milan_enrollment_transaction_free)

#ifdef GOODIX53X5_DEBUG
typedef struct
{
  guint                   feature_count;
  guint                   registration_count;
  gint32                  graph_reference_index;
  gint32                  graph_companion_f3;
  gint32                  graph_companion_f4;
  guint                   graph_established;
  guint32                 queue_state;
  guint32                 queue_transaction_counter;
  gint32                  order[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  gboolean                slot_populated[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  guint32                 record_counts[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  gint32                  feature_fields[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT][11];
  gint32                  pair_scores[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  gint32                  neighbor_counts[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  GoodixMilanRelationSlot relations[GOODIX_MILAN_TEMPLATE_RELATION_CAPACITY];
} GoodixMilanEnrollmentDebugState;

gboolean goodix_milan_enrollment_transaction_debug_state (const GoodixMilanEnrollmentTransaction *transaction,
                                                          GoodixMilanEnrollmentDebugState        *state);
gboolean goodix_milan_enrollment_transaction_debug_provisional (const GoodixMilanEnrollmentTransaction *transaction,
                                                                GBytes                                 *probe_template,
                                                                GoodixMilanEnrollmentResult            *result,
                                                                GoodixMilanEnrollmentDebugState        *state);
#endif

typedef enum {
  GOODIX_MILAN_EXTRACTION_OK,
  GOODIX_MILAN_EXTRACTION_INVALID,
} GoodixMilanExtractionStatus;

#define GOODIX_MILAN_EXTRACTION_SHA256_SIZE 65

typedef struct
{
  GoodixMilanAntifakeBoundaryClass boundary_classification;
  guint32                          zero_candidate_count;
  guint32                          nonzero_candidate_count;
  gchar                            zero_projection_sha256[
    GOODIX_MILAN_EXTRACTION_SHA256_SIZE];
  gchar                            nonzero_projection_sha256[
    GOODIX_MILAN_EXTRACTION_SHA256_SIZE];
} GoodixMilanExtractionDiagnostics;

typedef enum {
  GOODIX_SIGFM_TEMPLATE_OK,
  GOODIX_SIGFM_TEMPLATE_INVALID,
} GoodixSigfmTemplateStatus;

typedef enum {
  GOODIX_MILAN_STUDY_NONE = 0,
  GOODIX_MILAN_STUDY_APPEND = 1,
  GOODIX_MILAN_STUDY_REPLACE_NO_RELATION = 2,
  GOODIX_MILAN_STUDY_GEOMETRIC = 3,
  GOODIX_MILAN_STUDY_REPLACE = 4,
  GOODIX_MILAN_STUDY_QUEUED = 5,
} GoodixMilanStudyAction;

GoodixMatchInfo *goodix_milan_match_extract_native (const guint8               *image,
                                              GoodixMilanPreprocessState *preprocess_state,
                                              const guint16              *raw_frame,
                                              guint16                     t_code,
                                              guint16                     dac_high,
                                              guint16                     dac_low,
                                              guint16                     sensor_subtype);

GoodixMilanExtractionStatus goodix_milan_match_extract_native_result (const guint8               *image,
                                                                GoodixMilanPreprocessState *preprocess_state,
                                                                const guint16              *raw_frame,
                                                                guint16                     t_code,
                                                                guint16                     dac_high,
                                                                guint16                     dac_low,
                                                                guint16                     sensor_subtype,
                                                                GoodixMatchInfo           **info);

#ifdef GOODIX53X5_DEBUG
GoodixMilanExtractionStatus goodix_milan_match_extract_native_result_debug (const guint8                     *image,
                                                                      GoodixMilanPreprocessState       *preprocess_state,
                                                                      const guint16                    *raw_frame,
                                                                      guint16                           t_code,
                                                                      guint16                           dac_high,
                                                                      guint16                           dac_low,
                                                                      guint16                           sensor_subtype,
                                                                      GoodixMatchInfo                 **info,
                                                                      GoodixMilanExtractionDiagnostics *diagnostics);
#endif

int  goodix_milan_match_keypoints_count (GoodixMatchInfo *info);

void goodix_milan_match_free_info (GoodixMatchInfo *info);

GoodixMatchInfo *goodix_milan_match_info_new_empty (void);
void             goodix_milan_match_info_clear (GoodixMatchInfo *info);
gboolean         goodix_milan_match_info_copy (GoodixMatchInfo       *destination,
                                         const GoodixMatchInfo *source);
gboolean         goodix_milan_match_info_is_complete (const GoodixMatchInfo *info);

/* Serialize extracted features into a raw native packed template. Returns NULL
 * on serialization failure. */
GBytes *goodix_milan_match_serialize_template (GoodixMatchInfo *info);

GBytes *goodix_milan_match_combine_templates (GPtrArray *templates);

GoodixSigfmTemplateStatus goodix_milan_match_serialized_feature_result_queued (GoodixMatchInfo        *probe_info,
                                                                         const guint8           *feature,
                                                                         gsize                   feature_len,
                                                                         GoodixMilanMatchResult *match_result,
                                                                         GBytes                **updated_feature,
                                                                         GoodixStudyQueue       *queue);

#ifdef GOODIX53X5_DEBUG
GoodixSigfmTemplateStatus goodix_milan_match_serialized_feature_result_queued_debug (GoodixMatchInfo             *probe_info,
                                                                               const guint8                *feature,
                                                                               gsize                        feature_len,
                                                                               GoodixMilanMatchResult      *match_result,
                                                                               GBytes                     **updated_feature,
                                                                               GoodixMilanMatchDiagnostics *diagnostics,
                                                                               GoodixStudyQueue            *queue);
#endif

GoodixSigfmTemplateStatus goodix_milan_match_study_feature_queued (GoodixMatchInfo              *probe_info,
                                                             const guint8                 *feature,
                                                             gsize                         feature_len,
                                                             const GoodixMilanMatchResult *match_result,
                                                             gboolean                      study_eligible,
                                                             GoodixStudyQueue             *queue,
                                                             GBytes                      **updated_feature,
                                                             GoodixMilanStudyAction       *action);
