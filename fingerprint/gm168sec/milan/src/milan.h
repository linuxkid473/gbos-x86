/*
 * Goodix 53x5 driver for libfprint - Milan image preprocessing
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "milan/antifake/antifake.h"
#include "milan/capacity.h"
#include "milan/feature/feature.h"
#include "milan/preprocess/state.h"

#define GOODIX_MILAN_TEMPLATE_MAX_SIZE (1024U * 1024U)

int goodix_milan_template_initialize_tail (const uint8_t *frame,
                                                   size_t         rows,
                                                   size_t         columns,
                                                   uint8_t       *tail_state,
                                                   size_t         tail_size);

typedef struct
{
  int32_t tagged_values[11];
  int32_t optional_c7;
} GoodixMilanFeatureTemplateFields;

typedef struct
{
  const uint8_t *high_bitmap;
  const uint8_t *enhanced_bitmap;
  const uint8_t *inline_mask;
  const uint8_t *low_bitmap;
  const uint8_t *packed_records;
  const GoodixMilanAntifakeBlob *antifake;
  size_t         record_count;
  GoodixMilanFeatureTemplateFields fields;
} GoodixMilanFeatureView;

int goodix_milan_template_parse_feature_element (
  const uint8_t          *packed,
  size_t                  packed_size,
  GoodixMilanFeatureView *view);

GoodixMilanAntifakeBlob *goodix_milan_template_mutable_feature_antifake (
  uint8_t *packed,
  size_t   packed_size);

int goodix_milan_feature_unpack_template_records (
  const uint8_t            *packed,
  size_t                    record_count,
  size_t                    partition_count,
  GoodixMilanFeatureRecord *records,
  size_t                    record_capacity);

typedef struct
{
  int32_t index;
  int32_t values[7];
} GoodixMilanTemplateRelation;

int goodix_milan_estimate_relation (
  const GoodixMilanFeatureRecord *prior_records,
  size_t                          prior_record_count,
  const GoodixMilanFeatureRecord *current_records,
  size_t                          current_record_count,
  int32_t                         relation_index,
  GoodixMilanTemplateRelation    *relation);

int goodix_milan_registration_gate_metrics (
  const GoodixMilanFeatureView *prior,
  const GoodixMilanFeatureView *current,
  const int32_t                 transform[6],
  int32_t                      *registration_detail,
  int32_t                      *registration_coverage);

int goodix_milan_feature_mask_forward_overlap (
  const GoodixMilanFeatureView *first_feature,
  const GoodixMilanFeatureView *second_feature,
  const int32_t                 transform[6],
  int32_t                       half_resolution,
  int32_t                      *overlap,
  int32_t                      *overlap_count);
int goodix_milan_match_overlap_metrics (
  const GoodixMilanFeatureView *enrolled_feature,
  const GoodixMilanFeatureView *probe_feature,
  const int32_t                 transform[6],
  int32_t                      *overlap_score,
  int32_t                      *overlap_coverage,
  int32_t                      *overlap_detail,
  int32_t                       low_metrics[3]);

int goodix_milan_filter_recognition_pairs (
  const GoodixMilanFeatureRecord *enrolled_records,
  const GoodixMilanFeatureRecord *probe_records,
  const int32_t                  *pairs,
  size_t                          pair_count,
  int32_t                         transform[6],
  size_t                         *filtered_count,
  int32_t                        *residual);

int goodix_milan_match_initial_flags (
  const int32_t metrics[77],
  int32_t       image_quality,
  int32_t       image_coverage,
  const int32_t configuration[19],
  int32_t      *match_flag,
  int32_t      *candidate_flag,
  int32_t      *optional_flag);

int goodix_milan_match_transform_proximity (
  const int32_t transform[6],
  int32_t       mode,
  uint32_t      sensor_type);

int goodix_milan_match_bitmap_classes (
  const uint8_t *target_bitmap,
  const uint8_t *source_bitmap,
  const uint8_t *overlap_mask,
  size_t         bitmap_rows,
  size_t         bitmap_columns,
  size_t         overlap_x,
  size_t         overlap_y,
  size_t         overlap_rows,
  size_t         overlap_columns,
  const int32_t  transform[6],
  int32_t        classes[4],
  int32_t       *valid_count);

int32_t goodix_milan_match_secondary_result (
  int32_t primary_score,
  int32_t secondary_score,
  int32_t secondary_detail);

int goodix_milan_match_overlap_result (
  const int32_t classes[4],
  int32_t       valid_count,
  int32_t       full_count,
  int32_t       mode,
  const int32_t weights[3],
  int32_t       context_count,
  int32_t      *context_confidence,
  int32_t      *score,
  int32_t      *coverage,
  int32_t      *detail);

int goodix_milan_match_low_bitmap_metrics (
  const uint8_t enrolled_bitmap[286],
  const uint8_t enrolled_inline_mask[72],
  const uint8_t probe_bitmap[286],
  const uint8_t probe_inline_mask[72],
  const int32_t transform[6],
  int32_t       metrics[3]);

#ifdef GOODIX53X5_DEBUG
typedef struct
{
  int32_t  order_index;
  int32_t  enrolled_feature_index;
  int32_t  probe_feature_index;
  int32_t  metrics[77];
  int32_t  transform[6];
  int32_t  final_flags[2];
  int32_t  recognition_mode_before;
  int32_t  recognition_mode_after;
  int32_t  outer_eligible;
  int32_t  contributes;
  int32_t  q8_contribution;
  int32_t  blocking_recorded;
  int32_t  blocking_metric;
} GoodixMilanMatchCandidateDiagnostic;

typedef struct
{
  int32_t descriptor_score;
  int32_t filtered_count;
  int32_t overlap_score;
  int32_t overlap_detail;
  int32_t overlap_coverage;
  int32_t transform[6];
  int32_t fallback_count;
  int32_t fallback_feature_index;
  int32_t fallback_metrics[15];
  int32_t feature_metrics[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY][2][15];
  int32_t feature_transforms[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY][2][6];
  int32_t direct_aggregate[15];
  int32_t policy_aggregate[15];
  int32_t probe_policy_aggregate[15];
  int32_t direct_feature_index;
  int32_t policy_feature_index;
  int32_t probe_policy_feature_index;
  int32_t candidate_count;
  GoodixMilanMatchCandidateDiagnostic
    candidates[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  int32_t q8_sum;
  int32_t q8_contributor_count;
  int32_t final_selected_feature_index;
  int32_t probe_optional_c7;
  int32_t initial_packed_policy_mode;
  int32_t queue_study_evidence;
  int32_t queue_configuration_enabled;
  int32_t queue_probe_low_class;
  int32_t queue_accumulated_high_class;
  int32_t queue_status_count;
  int32_t postloop_blocking_count;
  int32_t postloop_blocking_sum;
  int32_t postloop_blocking_override;
} GoodixMilanMatchDiagnostics;
#endif

typedef struct
{
  int32_t relation_count;
  int32_t relation_values[7];
  int     relation_valid;
} GoodixMilanMatchRelationPublication;

typedef struct
{
  int32_t study_finalization_gate;
  int32_t study_action_gate;
  int32_t queue_candidate_eligible;
} GoodixMilanMatchStudyControl;

typedef struct
{
  size_t  matched_feature_index;
  int32_t score;
  int32_t match_transform[6];
  GoodixMilanMatchRelationPublication relation;
  uint64_t direct_positive_feature_mask;
  uint64_t contributor_feature_mask;
  uint64_t lifecycle_update_feature_mask;
  size_t retained_evidence_count;
  int32_t retained_evidence_feature_indices[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  int32_t retained_evidence_transforms[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY][6];
  int32_t retained_evidence_flag;
  GoodixMilanMatchStudyControl study_control;
} GoodixMilanMatchResult;

_Static_assert (offsetof (GoodixMilanMatchRelationPublication,
                          relation_count) == 0,
                "match relation count offset changed");
_Static_assert (offsetof (GoodixMilanMatchRelationPublication,
                          relation_values) == 4,
                "match relation values offset changed");
_Static_assert (offsetof (GoodixMilanMatchRelationPublication,
                          relation_valid) == 32,
                "match relation validity offset changed");
_Static_assert (sizeof (GoodixMilanMatchRelationPublication) == 36,
                "match relation publication size changed");
_Static_assert (_Alignof (GoodixMilanMatchRelationPublication) == 4,
                "match relation publication alignment changed");
_Static_assert (offsetof (GoodixMilanMatchStudyControl,
                          study_finalization_gate) == 0,
                "match study finalization gate offset changed");
_Static_assert (offsetof (GoodixMilanMatchStudyControl,
                          study_action_gate) == 4,
                "match study action gate offset changed");
_Static_assert (offsetof (GoodixMilanMatchStudyControl,
                          queue_candidate_eligible) == 8,
                "match study queue eligibility offset changed");
_Static_assert (sizeof (GoodixMilanMatchStudyControl) == 12,
                "match study control size changed");
_Static_assert (_Alignof (GoodixMilanMatchStudyControl) == 4,
                "match study control alignment changed");
_Static_assert (offsetof (GoodixMilanMatchResult, relation) == 36,
                "match result relation offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult, relation) +
                  offsetof (GoodixMilanMatchRelationPublication,
                            relation_count) == 36,
                "match result relation count offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult, relation) +
                  offsetof (GoodixMilanMatchRelationPublication,
                            relation_values) == 40,
                "match result relation values offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult, relation) +
                  offsetof (GoodixMilanMatchRelationPublication,
                            relation_valid) == 68,
                "match result relation validity offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult,
                          direct_positive_feature_mask) == 72,
                "match result direct mask offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult,
                          contributor_feature_mask) == 80,
                "match result contributor mask offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult,
                          lifecycle_update_feature_mask) == 88,
                "match result lifecycle mask offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult,
                          retained_evidence_count) == 96,
                "match result retained count offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult,
                          retained_evidence_feature_indices) == 104,
                "match result retained indices offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult,
                          retained_evidence_transforms) == 304,
                "match result retained transforms offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult,
                          retained_evidence_flag) == 1504,
                "match result retained flag offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult, study_control) == 1508,
                "match result study control offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult, study_control) +
                  offsetof (GoodixMilanMatchStudyControl,
                            study_finalization_gate) == 1508,
                "match result finalization gate offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult, study_control) +
                  offsetof (GoodixMilanMatchStudyControl,
                            study_action_gate) == 1512,
                "match result action gate offset changed");
_Static_assert (offsetof (GoodixMilanMatchResult, study_control) +
                  offsetof (GoodixMilanMatchStudyControl,
                            queue_candidate_eligible) == 1516,
                "match result queue eligibility offset changed");
_Static_assert (sizeof (GoodixMilanMatchResult) == 1520,
                "match result size changed");
_Static_assert (_Alignof (GoodixMilanMatchResult) == 8,
                "match result alignment changed");

typedef struct
{
  size_t   feature_count;
  uint32_t order[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  int32_t  active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  int32_t  generation[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  int32_t  ordinal[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  uint32_t transaction_tail;
  int      finalized;
  int      valid;
} GoodixMilanStudyTransientState;

#ifdef GOODIX53X5_DEBUG
int goodix_milan_match_probe_result_debug (
  const uint8_t                  probe_high_bitmap[286],
  const uint8_t                  probe_enhanced_bitmap[286],
  const uint8_t                  probe_low_bitmap[286],
  const uint8_t                  probe_inline_mask[72],
  const uint8_t                  probe_rescue_mask[308],
  const GoodixMilanFeatureRecord *probe_records,
  size_t                         probe_record_count,
  size_t                         probe_partition_count,
  int32_t                        image_quality,
  int32_t                        image_coverage,
  int32_t                        probe_optional_c7,
  const GoodixMilanAntifakeBlob *probe_antifake,
  const uint8_t                 *enrolled_template,
  size_t                         enrolled_template_size,
  GoodixMilanMatchResult        *match_result,
  GoodixMilanMatchDiagnostics   *diagnostics);
#endif

int goodix_milan_match_live_probe_result (
  const uint8_t                  probe_high_bitmap[286],
  const uint8_t                  probe_enhanced_bitmap[286],
  const uint8_t                  probe_low_bitmap[286],
  const uint8_t                  probe_inline_mask[72],
  const uint8_t                  probe_rescue_mask[308],
  const GoodixMilanFeatureRecord *probe_records,
  size_t                         probe_record_count,
  size_t                         probe_partition_count,
  int32_t                        image_quality,
  int32_t                        image_coverage,
  int32_t                        probe_optional_c7,
  const uint8_t                 *enrolled_template,
  size_t                         enrolled_template_size,
  const GoodixMilanFeatureRecord *const live_records[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  const size_t                   live_record_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  const size_t                   live_partition_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  size_t                         triggering_index,
  GoodixMilanMatchResult        *match_result);

#ifdef GOODIX53X5_DEBUG
int goodix_milan_match_live_probe_result_debug (
  const uint8_t                  probe_high_bitmap[286],
  const uint8_t                  probe_enhanced_bitmap[286],
  const uint8_t                  probe_low_bitmap[286],
  const uint8_t                  probe_inline_mask[72],
  const uint8_t                  probe_rescue_mask[308],
  const GoodixMilanFeatureRecord *probe_records,
  size_t                         probe_record_count,
  size_t                         probe_partition_count,
  int32_t                        image_quality,
  int32_t                        image_coverage,
  int32_t                        probe_optional_c7,
  const uint8_t                 *enrolled_template,
  size_t                         enrolled_template_size,
  const GoodixMilanFeatureRecord *const live_records[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  const size_t                   live_record_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  const size_t                   live_partition_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  size_t                         triggering_index,
  GoodixMilanMatchResult        *match_result,
  GoodixMilanMatchDiagnostics   *diagnostics);
#endif

int goodix_milan_template_normalize (
  const uint8_t *current_template,
  size_t         current_template_size,
  uint8_t       *packed,
  size_t         packed_capacity,
  size_t        *packed_size);

int goodix_milan_match_fallback_candidate_eligible (
  int32_t composite_score,
  int32_t score_threshold,
  int32_t quality_metric);

int goodix_milan_match_final_score (
  const int32_t metrics[15],
  uint32_t      template_type,
  int           alternate_policy,
  int32_t      *score);

typedef struct
{
  uint32_t sensor_type;
  uint32_t maximum_features;
  uint32_t registration_count;
  uint32_t maximum_records;
  uint32_t queue_state;
  uint32_t queue_transaction_counter;
  int32_t graph_reference_index;
  int32_t graph_companion_f3;
  int32_t graph_companion_f4;
  uint32_t graph_established;
} GoodixMilanTemplateMetadata;

typedef struct
{
  const uint8_t *feature_elements[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  size_t feature_element_sizes[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  size_t feature_count;
  GoodixMilanTemplateRelation relations[GOODIX_MILAN_TEMPLATE_RELATION_CAPACITY];
  size_t relation_count;
  GoodixMilanTemplateMetadata metadata;
  uint8_t tail_state[0x520];
  int32_t normalization_overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
} GoodixMilanUnpackedTemplate;

int goodix_milan_match_reference_transform (
  const GoodixMilanUnpackedTemplate *enrolled,
  size_t                              matched_feature_index,
  const int32_t                       match_transform[6],
  int32_t                             relation_values[7]);

int goodix_milan_feature_pack_template_records (
  const GoodixMilanFeatureRecord *records,
  size_t                          record_count,
  uint8_t                        *packed,
  size_t                          packed_size);

int goodix_milan_template_pack_feature_element (
  const uint8_t                         *high_bitmap,
  const uint8_t                         *enhanced_bitmap,
  const uint8_t                         *inline_mask,
  const uint8_t                         *low_bitmap,
  const GoodixMilanFeatureRecord        *records,
  size_t                                 record_count,
  const GoodixMilanAntifakeBlob         *antifake,
  const GoodixMilanFeatureTemplateFields *fields,
  uint8_t                               *packed,
  size_t                                 packed_capacity,
  size_t                                *packed_size);

int goodix_milan_template_pack_one_feature (
  const uint8_t *feature_element,
  size_t         feature_element_size,
  const uint8_t *tail_state,
  size_t         tail_state_size,
  uint8_t       *packed,
  size_t         packed_capacity,
  size_t        *packed_size);

int goodix_milan_template_pack (
  const uint8_t *const                 *feature_elements,
  const size_t                         *feature_element_sizes,
  size_t                                feature_count,
  const GoodixMilanTemplateRelation    *relations,
  size_t                                relation_count,
  const GoodixMilanTemplateMetadata    *metadata,
  const uint8_t                        *tail_state,
  size_t                                tail_state_size,
  uint8_t                              *packed,
  size_t                                packed_capacity,
  size_t                               *packed_size);

int goodix_milan_template_unpack (
  const uint8_t                 *packed,
  size_t                         packed_size,
  GoodixMilanUnpackedTemplate   *unpacked);

int goodix_milan_template_update_match_lifecycle (
  const uint8_t *current_template,
  size_t         current_template_size,
  uint64_t       feature_mask,
  bool           sort_order,
  uint8_t       *packed,
  size_t         packed_capacity,
  size_t        *packed_size);

int goodix_milan_study_append (
  const uint8_t  *current_template,
  size_t          current_template_size,
  const uint8_t  *probe_template,
  size_t          probe_template_size,
  size_t          matched_feature_index,
  const int32_t   relation_values[7],
  const int32_t  *retained_feature_indices,
  const int32_t   retained_transforms[][6],
  size_t          retained_count,
  int32_t         retained_flag,
  int             apply_dispatcher_prepass,
  int             complete_dispatcher_transaction,
  int             finalize_study,
  int32_t         live_overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  uint8_t        *packed,
  size_t          packed_capacity,
  size_t         *packed_size);

int goodix_milan_study_replace (
  const uint8_t  *current_template,
  size_t          current_template_size,
  const uint8_t  *probe_template,
  size_t          probe_template_size,
  size_t          matched_feature_index,
  const int32_t   relation_values[7],
  const int32_t  *retained_feature_indices,
  const int32_t   retained_transforms[][6],
  size_t          retained_count,
  int32_t         retained_flag,
  int             apply_dispatcher_prepass,
  int32_t         probe_quality,
  int32_t         probe_coverage,
  const int32_t   primary_transform[6],
  int             complete_dispatcher_transaction,
  int             finalize_study,
  int32_t         live_overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  uint8_t        *packed,
  size_t          packed_capacity,
  size_t         *packed_size,
  int32_t        *action_code,
  size_t         *selected_feature_index,
  GoodixMilanStudyTransientState *transient_state);

int goodix_milan_study_action0_transient (
  const uint8_t *current_template,
  size_t         current_template_size,
  const int32_t  relation_values[7],
  const int32_t *retained_feature_indices,
  const int32_t  retained_transforms[][6],
  size_t         retained_count,
  int32_t        retained_flag,
  GoodixMilanStudyTransientState *transient_state);

/* Dispatcher prepass state for a retained action-zero gallery; no finalization. */
int goodix_milan_study_action0_gallery (const uint8_t *current_template,
                                        size_t         current_template_size,
                                        const int32_t  relation_values[7],
                                        const int32_t *retained_feature_indices,
                                        const int32_t  retained_transforms[][6],
                                        size_t         retained_count,
                                        int32_t        retained_flag,
                                        int32_t        live_overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
                                        uint8_t       *packed,
                                        size_t         packed_capacity,
                                        size_t        *packed_size);

int goodix_milan_study_finalize_action0_transient (
  GoodixMilanStudyTransientState *transient_state);

int goodix_milan_study_finalize (const uint8_t *current_template,
                                 size_t         current_template_size,
                                 uint32_t       queue_state,
                                 uint32_t       queue_transaction_counter,
                                 int            finalize_transaction,
                                 uint8_t       *packed,
                                 size_t         packed_capacity,
                                 size_t        *packed_size);
