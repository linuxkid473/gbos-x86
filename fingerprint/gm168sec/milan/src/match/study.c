/*
 * Goodix 53x5 driver for libfprint - native Milan queued-study integration
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

#define FP_COMPONENT "goodix53x5"

#include "milan/match/match.h"
#include "milan/match/info-private.h"
#include "milan/match/lifecycle-private.h"
#include "milan/match/overlap.h"
#include "milan/match/rescue.h"
#include "milan/milan.h"
#include "milan/print.h"
#include "milan/private.h"
#include "milan/study/queue.h"

#include <string.h>

static gboolean
goodix_milan_match_queue_duplicate_metric (const GoodixMatchInfo *incoming,
                                     const GoodixMatchInfo *newest,
                                     gint                  *metric,
                                     gpointer               user_data)
{
  GoodixMilanFeatureView incoming_view = { 0 };
  GoodixMilanFeatureView newest_view = { 0 };

  (void) user_data;
  if (!goodix_milan_match_info_is_complete (incoming) ||
      !goodix_milan_match_info_is_complete (newest) || !metric)
    return FALSE;
  incoming_view.high_bitmap = incoming->feature_bitmaps.high_bitmap;
  incoming_view.enhanced_bitmap = incoming->feature_bitmaps.enhanced_bitmap;
  incoming_view.inline_mask = incoming->inline_mask;
  newest_view.high_bitmap = newest->feature_bitmaps.high_bitmap;
  newest_view.enhanced_bitmap = newest->feature_bitmaps.enhanced_bitmap;
  newest_view.inline_mask = newest->inline_mask;
  return goodix_milan_match_queue_duplicate_score (
    &newest_view, &incoming_view, metric) == 0;
}

static GoodixStudyQueueEnqueueResult
goodix_milan_match_enqueue_queue_candidate (GoodixStudyQueue      *queue,
                                      const GoodixMatchInfo *probe_info)
{
  g_autofree GoodixMilanUnpackedTemplate *unpacked = NULL;
  g_autofree guint8 *feature_copy = NULL;
  g_autofree guint8 *packed = NULL;

  g_autoptr(GBytes) queued_template = NULL;
  GoodixMatchInfo queued;
  const guint8 *template_data;
  gsize template_size;
  size_t packed_size = 0;

  if (!goodix_milan_match_info_is_complete (probe_info))
    return GOODIX_STUDY_QUEUE_INVALID;
  template_data = g_bytes_get_data (probe_info->template, &template_size);
  unpacked = g_new (GoodixMilanUnpackedTemplate, 1);
  if (goodix_milan_template_unpack (template_data, template_size, unpacked) != 0 ||
      unpacked->metadata.sensor_type != GOODIX_MILAN_PRINT_SENSOR_TYPE ||
      unpacked->feature_count != 1 || unpacked->relation_count != 0)
    return GOODIX_STUDY_QUEUE_INVALID;

  /* Native enqueue owns a ba=-1 copy, not the original probe's lifecycle state. */
  feature_copy = g_memdup2 (unpacked->feature_elements[0],
                            unpacked->feature_element_sizes[0]);
  if (goodix_milan_template_patch_feature_scalar (
        feature_copy, unpacked->feature_element_sizes[0], 0xba, -1) != 0)
    return GOODIX_STUDY_QUEUE_INVALID;
  unpacked->feature_elements[0] = feature_copy;
  packed = g_malloc (template_size);
  if (goodix_milan_template_pack (
        unpacked->feature_elements, unpacked->feature_element_sizes,
        unpacked->feature_count, unpacked->relations, unpacked->relation_count,
        &unpacked->metadata, unpacked->tail_state, sizeof (unpacked->tail_state),
        packed, template_size, &packed_size) != 0 || packed_size != template_size)
    return GOODIX_STUDY_QUEUE_INVALID;

  queued_template = g_bytes_new_take (g_steal_pointer (&packed), packed_size);
  queued = *probe_info;
  queued.template = queued_template;
  return goodix_milan_study_queue_enqueue (
    queue, &queued, goodix_milan_match_queue_duplicate_metric, NULL);
}

GoodixSigfmTemplateStatus
goodix_milan_match_serialized_feature_result_queued (
  GoodixMatchInfo             *probe_info,
  const guint8                *feature,
  gsize                        feature_len,
  GoodixMilanMatchResult      *match_result,
  GBytes                     **updated_feature,
  GoodixStudyQueue            *queue)
{
  return goodix_milan_match_serialized_feature_result_internal (
    probe_info, feature, feature_len, match_result, updated_feature,
#ifdef GOODIX53X5_DEBUG
    NULL,
#endif
    queue, TRUE, goodix_milan_match_enqueue_queue_candidate);
}

#ifdef GOODIX53X5_DEBUG
GoodixSigfmTemplateStatus
goodix_milan_match_serialized_feature_result_queued_debug (
  GoodixMatchInfo             *probe_info,
  const guint8                *feature,
  gsize                        feature_len,
  GoodixMilanMatchResult      *match_result,
  GBytes                     **updated_feature,
  GoodixMilanMatchDiagnostics *diagnostics,
  GoodixStudyQueue            *queue)
{
  return goodix_milan_match_serialized_feature_result_internal (
    probe_info, feature, feature_len, match_result, updated_feature,
    diagnostics, queue, TRUE, goodix_milan_match_enqueue_queue_candidate);
}
#endif

static GoodixSigfmTemplateStatus
goodix_milan_match_study_feature_internal (
  const guint8                 *probe_feature,
  gsize                         probe_feature_len,
  const guint8                 *feature,
  gsize                         feature_len,
  const GoodixMilanMatchResult *match_result,
  gboolean                      study_eligible,
  GBytes                      **updated_feature,
  GoodixMilanStudyAction       *action,
  gsize                        *selected_feature_index,
  gboolean                      apply_dispatcher_prepass,
  gboolean                      finalize_study,
  int32_t                       live_overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  GoodixMilanStudyTransientState *transient_state)
{
  const guint8 *enrolled_milan;
  const guint8 *probe_milan;
  gsize enrolled_milan_len;
  gsize probe_milan_len;
  GoodixMilanUnpackedTemplate *enrolled = NULL;
  GoodixMilanUnpackedTemplate *probe = NULL;
  GoodixMilanUnpackedTemplate *updated = NULL;
  GoodixMilanStudyTransientState transient_storage;
  GoodixMilanFeatureView probe_view;
  guint8 *packed = NULL;
  gsize packed_capacity;
  size_t packed_size = 0;
  int study_status;
  int32_t action_code = 0;
  gboolean finalize_current_study;

  if (updated_feature)
    *updated_feature = NULL;
  if (action)
    *action = GOODIX_MILAN_STUDY_NONE;
  if (selected_feature_index)
    *selected_feature_index = SIZE_MAX;
  if (!transient_state)
    transient_state = &transient_storage;
  memset (transient_state, 0, sizeof(*transient_state));
  if (!probe_feature || !feature || !match_result || !live_overlap_counts ||
      !updated_feature || !action)
    return GOODIX_SIGFM_TEMPLATE_INVALID;
  enrolled_milan = feature;
  enrolled_milan_len = feature_len;
  if (enrolled_milan_len > GOODIX_MILAN_TEMPLATE_MAX_SIZE)
    return GOODIX_SIGFM_TEMPLATE_INVALID;
  if (!study_eligible)
    return GOODIX_SIGFM_TEMPLATE_OK;
  if (match_result->score <= 0)
    return GOODIX_SIGFM_TEMPLATE_INVALID;

  probe_milan = probe_feature;
  probe_milan_len = probe_feature_len;
  if (probe_milan_len > GOODIX_MILAN_TEMPLATE_MAX_SIZE)
    return GOODIX_SIGFM_TEMPLATE_INVALID;

  enrolled = g_malloc (sizeof(*enrolled));
  probe = g_malloc (sizeof(*probe));
  updated = g_malloc (sizeof(*updated));
  packed_capacity = GOODIX_MILAN_TEMPLATE_MAX_SIZE;
  packed = g_malloc (packed_capacity);
  if (goodix_milan_template_unpack (
        enrolled_milan, enrolled_milan_len, enrolled) != 0 ||
      goodix_milan_template_unpack (
        probe_milan, probe_milan_len, probe) != 0 ||
      probe->feature_count != 1 ||
      goodix_milan_template_parse_feature_element (
        probe->feature_elements[0], probe->feature_element_sizes[0],
        &probe_view) != 0)
    goto invalid;
  finalize_current_study = finalize_study &&
                           (enrolled->metadata.sensor_type != GOODIX_MILAN_PRINT_SENSOR_TYPE ||
                            match_result->study_control.study_finalization_gate != 0);
  if (match_result->study_control.study_action_gate == 0)
    {
      g_free (packed);
      g_free (updated);
      g_free (probe);
      g_free (enrolled);
      return GOODIX_SIGFM_TEMPLATE_OK;
    }
  if (match_result->matched_feature_index == SIZE_MAX)
    {
      if (enrolled->metadata.sensor_type == GOODIX_MILAN_PRINT_SENSOR_TYPE && transient_state &&
          goodix_milan_study_action0_transient (
            enrolled_milan, enrolled_milan_len,
            match_result->relation.relation_values,
            match_result->retained_evidence_feature_indices,
            match_result->retained_evidence_transforms,
            match_result->retained_evidence_count,
            match_result->retained_evidence_flag, transient_state) != 0)
        goto invalid;
      if (transient_state && transient_state->valid && finalize_current_study &&
          goodix_milan_study_finalize_action0_transient (transient_state) != 0)
        goto invalid;
      g_free (packed);
      g_free (updated);
      g_free (probe);
      g_free (enrolled);
      return GOODIX_SIGFM_TEMPLATE_OK;
    }
  if (!match_result->relation.relation_valid ||
      match_result->relation.relation_values[0] != 0 ||
      match_result->matched_feature_index >= enrolled->feature_count)
    goto invalid;
  if (probe_view.fields.tagged_values[3] <= 15 ||
      probe_view.fields.tagged_values[4] <= 65)
    {
      if (enrolled->metadata.sensor_type == GOODIX_MILAN_PRINT_SENSOR_TYPE && transient_state &&
          (goodix_milan_study_action0_transient (
             enrolled_milan, enrolled_milan_len,
             match_result->relation.relation_values,
             match_result->retained_evidence_feature_indices,
             match_result->retained_evidence_transforms,
             match_result->retained_evidence_count,
             match_result->retained_evidence_flag, transient_state) != 0 ||
           (finalize_current_study &&
            goodix_milan_study_finalize_action0_transient (
              transient_state) != 0)))
        goto invalid;
      g_free (packed);
      g_free (updated);
      g_free (probe);
      g_free (enrolled);
      return GOODIX_SIGFM_TEMPLATE_OK;
    }
  if (enrolled->feature_count < enrolled->metadata.maximum_features)
    {
      study_status = goodix_milan_study_append (
        enrolled_milan, enrolled_milan_len, probe_milan, probe_milan_len,
        match_result->matched_feature_index,
        match_result->relation.relation_values,
        match_result->retained_evidence_feature_indices,
        match_result->retained_evidence_transforms,
        match_result->retained_evidence_count,
        match_result->retained_evidence_flag,
        apply_dispatcher_prepass,
        finalize_study,
        finalize_current_study,
        live_overlap_counts,
        packed, packed_capacity, &packed_size);
      *action = GOODIX_MILAN_STUDY_APPEND;
      if (selected_feature_index)
        *selected_feature_index = enrolled->feature_count;
    }
  else if (enrolled->feature_count == enrolled->metadata.maximum_features)
    {
      study_status = goodix_milan_study_replace (
        enrolled_milan, enrolled_milan_len, probe_milan, probe_milan_len,
        match_result->matched_feature_index,
        match_result->relation.relation_values,
        match_result->retained_evidence_feature_indices,
        match_result->retained_evidence_transforms,
        match_result->retained_evidence_count,
        match_result->retained_evidence_flag,
        apply_dispatcher_prepass,
        probe_view.fields.tagged_values[3], probe_view.fields.tagged_values[4],
        match_result->match_transform,
        finalize_study,
        finalize_current_study,
        live_overlap_counts,
        packed, packed_capacity, &packed_size, &action_code,
        selected_feature_index, transient_state);
      *action = (GoodixMilanStudyAction) action_code;
    }
  else
    goto invalid;
  if (study_status != 0)
    goto invalid;
  if (*action == GOODIX_MILAN_STUDY_NONE)
    {
      if (transient_state && transient_state->valid && finalize_current_study &&
          goodix_milan_study_finalize_action0_transient (transient_state) != 0)
        goto invalid;
      g_free (packed);
      g_free (updated);
      g_free (probe);
      g_free (enrolled);
      return GOODIX_SIGFM_TEMPLATE_OK;
    }
  if (packed_size == 0 ||
      goodix_milan_template_unpack (packed, packed_size, updated) != 0)
    goto invalid;
  *updated_feature = g_bytes_new_take (packed, packed_size);
  packed = NULL;

  g_free (packed);
  g_free (updated);
  g_free (probe);
  g_free (enrolled);
  return GOODIX_SIGFM_TEMPLATE_OK;

invalid:
  *action = GOODIX_MILAN_STUDY_NONE;
  g_clear_pointer (updated_feature, g_bytes_unref);
  g_free (packed);
  g_free (updated);
  g_free (probe);
  g_free (enrolled);
  return GOODIX_SIGFM_TEMPLATE_INVALID;
}

typedef struct
{
  GBytes *current;
  GoodixMatchInfo *live_features[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  int32_t live_overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
} GoodixStudyFollowupContext;

static gboolean
goodix_milan_match_initialize_study_overlap_counts (
  const guint8 *feature,
  gsize         feature_len,
  int32_t       overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY])
{
  GoodixMilanUnpackedTemplate *unpacked;
  uint8_t *feature_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  gboolean result = FALSE;

  if (!feature || !overlap_counts ||
      feature_len > GOODIX_MILAN_TEMPLATE_MAX_SIZE)
    return FALSE;
  memset (overlap_counts, 0,
          GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY * sizeof(*overlap_counts));
  unpacked = g_malloc (sizeof(*unpacked));
  if (goodix_milan_template_unpack (feature, feature_len, unpacked) == 0 &&
      unpacked->metadata.sensor_type == GOODIX_MILAN_PRINT_SENSOR_TYPE &&
      goodix_milan_template_normalize_unpacked (
        unpacked, feature_copies, overlap_counts) == 0)
    result = TRUE;

  for (size_t i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    g_free (feature_copies[i]);
  g_free (unpacked);
  return result;
}

static gboolean
goodix_milan_match_set_live_feature (GoodixStudyFollowupContext *context,
                               gsize                       index,
                               const GoodixMatchInfo      *source)
{
  GoodixMatchInfo *copy;

  if (!context || index >= GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY ||
      !goodix_milan_match_info_is_complete (source))
    return FALSE;
  copy = goodix_milan_match_info_new_empty ();
  if (!goodix_milan_match_info_copy (copy, source))
    {
      goodix_milan_match_free_info (copy);
      return FALSE;
    }
  g_clear_pointer (&context->live_features[index], goodix_milan_match_free_info);
  context->live_features[index] = copy;
  return TRUE;
}

static GoodixSigfmTemplateStatus
goodix_milan_match_live_gallery_result (GoodixMatchInfo            *probe,
                                        GoodixStudyFollowupContext *context,
                                        gsize                       triggering_index,
                                        GoodixMilanMatchResult     *match_result,
                                        GBytes                    **updated_feature)
{
  const GoodixMilanFeatureRecord *live_records[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  size_t live_record_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  size_t live_partition_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  const guint8 *current_data;
  const guint8 *current_milan;
  guint8 *updated_milan = NULL;
  gsize current_size;
  gsize current_milan_size;
  size_t updated_milan_size = 0;

  *updated_feature = NULL;
  if (!goodix_milan_match_info_is_complete (probe) || !context || !context->current)
    return GOODIX_SIGFM_TEMPLATE_INVALID;
  current_data = g_bytes_get_data (context->current, &current_size);
  current_milan = current_data;
  current_milan_size = current_size;
  if (current_milan_size > GOODIX_MILAN_TEMPLATE_MAX_SIZE)
    return GOODIX_SIGFM_TEMPLATE_INVALID;
  for (size_t i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    if (context->live_features[i])
      {
        live_records[i] = context->live_features[i]->records;
        live_record_counts[i] = (size_t) context->live_features[i]->record_count;
        live_partition_counts[i] =
          (size_t) context->live_features[i]->partition_count;
      }
  if (goodix_milan_match_info_result (
        probe, current_milan, current_milan_size, live_records,
        live_record_counts, live_partition_counts, triggering_index,
        match_result
#ifdef GOODIX53X5_DEBUG
        , NULL
#endif
        ) != 0)
    return GOODIX_SIGFM_TEMPLATE_INVALID;

  if (match_result->score > 0 &&
      match_result->lifecycle_update_feature_mask != 0)
    {
      updated_milan = g_malloc (current_milan_size);
      if (goodix_milan_template_update_match_lifecycle (
            current_milan, current_milan_size,
            match_result->lifecycle_update_feature_mask,
            match_result->direct_positive_feature_mask == 0, updated_milan,
            current_milan_size, &updated_milan_size) != 0 ||
          updated_milan_size != current_milan_size)
        {
          g_free (updated_milan);
          return GOODIX_SIGFM_TEMPLATE_INVALID;
        }
      current_milan_size = updated_milan_size;
    }
  if (updated_milan)
    {
      *updated_feature = g_bytes_new_take (updated_milan, current_milan_size);
      updated_milan = NULL;
    }
  else
    *updated_feature = g_bytes_ref (context->current);
  return *updated_feature ? GOODIX_SIGFM_TEMPLATE_OK
                          : GOODIX_SIGFM_TEMPLATE_INVALID;
}

static gboolean
goodix_milan_match_study_followup (GoodixMatchInfo *queued,
                             gsize            physical_slot,
                             gsize            triggering_index,
                             gsize           *selected_index,
                             gpointer         user_data)
{
  GoodixStudyFollowupContext *context = user_data;
  GoodixMilanMatchResult match_result;
  GoodixMilanStudyAction action = GOODIX_MILAN_STUDY_NONE;
  GoodixSigfmTemplateStatus status;
  GBytes *after_match = NULL;
  GBytes *after_study = NULL;
  GBytes *probe_feature = NULL;
  const guint8 *probe_data;
  gsize probe_size;

  (void) physical_slot;
  *selected_index = SIZE_MAX;
  memset (&match_result, 0, sizeof(match_result));
  status = goodix_milan_match_live_gallery_result (
    queued, context, triggering_index, &match_result, &after_match);
  if (status != GOODIX_SIGFM_TEMPLATE_OK || !after_match)
    goto invalid;

  g_clear_pointer (&context->current, g_bytes_unref);
  context->current = g_bytes_ref (after_match);
  if (match_result.study_control.study_action_gate == 0 ||
      match_result.matched_feature_index == SIZE_MAX)
    {
      g_bytes_unref (after_match);
      return TRUE;
    }

  probe_feature = goodix_milan_match_serialize_template (queued);
  if (!probe_feature)
    goto invalid;
  probe_data = g_bytes_get_data (probe_feature, &probe_size);
  status = goodix_milan_match_study_feature_internal (
    probe_data, probe_size,
    g_bytes_get_data (context->current, NULL),
    g_bytes_get_size (context->current), &match_result, TRUE,
    &after_study, &action, selected_index, FALSE, FALSE,
    context->live_overlap_counts, NULL);
  if (status != GOODIX_SIGFM_TEMPLATE_OK)
    goto invalid;
  if (action != GOODIX_MILAN_STUDY_NONE)
    {
      if (!after_study ||
          *selected_index >= GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY)
        goto invalid;
      if (!goodix_milan_match_set_live_feature (context, *selected_index, queued))
        goto invalid;
      g_clear_pointer (&context->current, g_bytes_unref);
      context->current = g_bytes_ref (after_study);
    }
  else
    *selected_index = SIZE_MAX;

  g_clear_pointer (&after_study, g_bytes_unref);
  g_clear_pointer (&probe_feature, g_bytes_unref);
  g_clear_pointer (&after_match, g_bytes_unref);
  return TRUE;

invalid:
  g_clear_pointer (&after_study, g_bytes_unref);
  g_clear_pointer (&probe_feature, g_bytes_unref);
  g_clear_pointer (&after_match, g_bytes_unref);
  return FALSE;
}

static gboolean
goodix_milan_match_template_at_capacity (GBytes *feature)
{
  GoodixMilanUnpackedTemplate *unpacked;
  const guint8 *template_data;
  gsize template_size;
  gboolean at_capacity = FALSE;

  template_data = g_bytes_get_data (feature, &template_size);
  if (template_size > GOODIX_MILAN_TEMPLATE_MAX_SIZE)
    return FALSE;
  unpacked = g_malloc (sizeof(*unpacked));
  if (goodix_milan_template_unpack (template_data, template_size, unpacked) == 0)
    at_capacity = unpacked->feature_count == unpacked->metadata.maximum_features;
  g_free (unpacked);
  return at_capacity;
}

static GBytes *
goodix_milan_match_finalize_study (GBytes                 *feature,
                             const GoodixStudyQueue *queue,
                             gboolean                finalize_transaction)
{
  const guint8 *template_data;
  guint8 *packed = NULL;
  gsize template_size;
  size_t packed_size = 0;
  GBytes *result = NULL;

  template_data = g_bytes_get_data (feature, &template_size);
  if (template_size > GOODIX_MILAN_TEMPLATE_MAX_SIZE || !queue ||
      !goodix_milan_study_queue_validate (queue))
    return NULL;
  packed = g_malloc (template_size);
  if (goodix_milan_study_finalize (
        template_data, template_size, queue->enabled_state,
        queue->transaction_counter, finalize_transaction, packed, template_size,
        &packed_size) == 0 &&
      packed_size == template_size)
    {
      result = g_bytes_new_take (packed, packed_size);
      packed = NULL;
    }
  g_free (packed);
  return result;
}

GoodixSigfmTemplateStatus
goodix_milan_match_study_feature_queued (
  GoodixMatchInfo              *probe_info,
  const guint8                 *feature,
  gsize                         feature_len,
  const GoodixMilanMatchResult *match_result,
  gboolean                      study_eligible,
  GoodixStudyQueue             *queue,
  GBytes                      **updated_feature,
  GoodixMilanStudyAction       *action)
{
  GoodixStudyFollowupContext context = { 0 };
  GoodixMilanStudyTransientState transient = { 0 };
  GoodixSigfmTemplateStatus status = GOODIX_SIGFM_TEMPLATE_INVALID;
  GoodixMilanStudyAction primary_action = GOODIX_MILAN_STUDY_NONE;
  GBytes *probe_feature = NULL;
  GBytes *primary_update = NULL;
  GBytes *final_update = NULL;
  const guint8 *probe_data;
  gsize probe_size;
  gsize selected_index = SIZE_MAX;
  gboolean queued_mutation = FALSE;
  gboolean retained_gallery;
  GBytes *input_identity = NULL;
  const guint8 *requested_feature = feature;
  gsize requested_size = feature_len;

  if (updated_feature)
    *updated_feature = NULL;
  if (action)
    *action = GOODIX_MILAN_STUDY_NONE;
  if (!goodix_milan_match_info_is_complete (probe_info) || !feature ||
      !match_result || !queue || !updated_feature || !action ||
      !goodix_milan_study_queue_validate (queue))
    {
      goodix_milan_study_queue_clear_gallery (queue);
      return GOODIX_SIGFM_TEMPLATE_INVALID;
    }
  retained_gallery = goodix_milan_study_queue_resolve_gallery (
    queue, &feature, &feature_len);
  if (!goodix_milan_match_queue_matches_template (queue, feature, feature_len))
    goto out;
  input_identity = g_bytes_new (requested_feature, requested_size);
  context.current = g_bytes_new (feature, feature_len);
  feature = g_bytes_get_data (context.current, NULL);
  if (retained_gallery && queue->live_overlap_counts_valid)
    memcpy (context.live_overlap_counts, queue->live_overlap_counts,
            sizeof (context.live_overlap_counts));
  else if (!goodix_milan_match_initialize_study_overlap_counts (
             feature, feature_len, context.live_overlap_counts))
    goto out;
  if (retained_gallery)
    for (gsize i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
      context.live_features[i] = g_steal_pointer (&queue->live_features[i]);
  else
    goodix_milan_study_queue_clear_gallery (queue);
  probe_feature = goodix_milan_match_serialize_template (probe_info);
  if (!probe_feature)
    goto out;
  probe_data = g_bytes_get_data (probe_feature, &probe_size);
  status = goodix_milan_match_study_feature_internal (
    probe_data, probe_size, feature, feature_len, match_result, study_eligible,
    &primary_update, &primary_action, &selected_index, TRUE, FALSE,
    context.live_overlap_counts, &transient);
  if (status != GOODIX_SIGFM_TEMPLATE_OK)
    goto out;
  if (primary_action == GOODIX_MILAN_STUDY_NONE)
    {
      GBytes *original = g_bytes_new (feature, feature_len);
      GoodixStudyQueueEnqueueResult enqueue_result;

      if (transient.valid && goodix_milan_match_template_at_capacity (original))
        goodix_milan_study_queue_disable (queue);
      g_bytes_unref (original);
      if (transient.valid && selected_index == SIZE_MAX &&
          queue->enabled_state == 0 &&
          match_result->study_control.study_action_gate == 1 &&
          probe_info->extraction_metadata.quality > 15 &&
          probe_info->extraction_metadata.coverage > 65)
        {
          enqueue_result = goodix_milan_match_enqueue_queue_candidate (
            queue, probe_info);
          if (enqueue_result == GOODIX_STUDY_QUEUE_INVALID)
            {
              status = GOODIX_SIGFM_TEMPLATE_INVALID;
              goto out;
            }
        }
      if (transient.valid)
        {
          guint8 *packed = g_malloc (GOODIX_MILAN_TEMPLATE_MAX_SIZE);
          size_t packed_size = 0;

          if (goodix_milan_study_action0_gallery (
                feature, feature_len, match_result->relation.relation_values,
                match_result->retained_evidence_feature_indices,
                match_result->retained_evidence_transforms,
                match_result->retained_evidence_count,
                match_result->retained_evidence_flag, context.live_overlap_counts,
                packed, GOODIX_MILAN_TEMPLATE_MAX_SIZE, &packed_size) != 0)
            {
              g_free (packed);
              status = GOODIX_SIGFM_TEMPLATE_INVALID;
              goto out;
            }
          g_clear_pointer (&context.current, g_bytes_unref);
          context.current = g_bytes_new_take (packed, packed_size);
          final_update = goodix_milan_match_finalize_study (
            context.current, queue,
            match_result->study_control.study_finalization_gate != 0);
        }
      else
        {
          final_update = g_bytes_ref (context.current);
        }
      if (!final_update)
        {
          status = GOODIX_SIGFM_TEMPLATE_INVALID;
          goto out;
        }
      goto publish;
    }
  if (!primary_update || selected_index == SIZE_MAX)
    {
      status = GOODIX_SIGFM_TEMPLATE_INVALID;
      goto out;
    }

  if (selected_index >= GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY)
    {
      status = GOODIX_SIGFM_TEMPLATE_INVALID;
      goto out;
    }
  g_clear_pointer (&context.current, g_bytes_unref);
  context.current = g_bytes_ref (primary_update);
  if (!goodix_milan_match_set_live_feature (&context, selected_index, probe_info))
    {
      status = GOODIX_SIGFM_TEMPLATE_INVALID;
      goto out;
    }
  if (queue->enabled_state == 0 && !goodix_milan_study_queue_process (
        queue, selected_index, goodix_milan_match_study_followup, &context,
        &queued_mutation))
    {
      status = GOODIX_SIGFM_TEMPLATE_INVALID;
      goto out;
    }
  if (goodix_milan_match_template_at_capacity (context.current))
    goodix_milan_study_queue_disable (queue);

  final_update = goodix_milan_match_finalize_study (
    context.current, queue,
    match_result->study_control.study_finalization_gate != 0);
  if (!final_update)
    {
      status = GOODIX_SIGFM_TEMPLATE_INVALID;
      goto out;
    }
publish:
  goodix_milan_study_queue_clear_gallery (queue);
  queue->live_gallery = g_bytes_ref (final_update);
  if (primary_action == GOODIX_MILAN_STUDY_NONE)
    queue->live_input = g_steal_pointer (&input_identity);
  for (gsize i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    queue->live_features[i] = g_steal_pointer (&context.live_features[i]);
  memcpy (queue->live_overlap_counts, context.live_overlap_counts,
          sizeof (queue->live_overlap_counts));
  queue->live_overlap_counts_valid = TRUE;
  if (primary_action != GOODIX_MILAN_STUDY_NONE)
    {
      *updated_feature = g_steal_pointer (&final_update);
      *action = queued_mutation ? GOODIX_MILAN_STUDY_QUEUED : primary_action;
    }

out:
  if (status != GOODIX_SIGFM_TEMPLATE_OK)
    {
      *action = GOODIX_MILAN_STUDY_NONE;
      g_clear_pointer (updated_feature, g_bytes_unref);
      goodix_milan_study_queue_clear_gallery (queue);
    }
  g_clear_pointer (&context.current, g_bytes_unref);
  for (size_t i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    g_clear_pointer (&context.live_features[i], goodix_milan_match_free_info);
  g_clear_pointer (&final_update, g_bytes_unref);
  g_clear_pointer (&primary_update, g_bytes_unref);
  g_clear_pointer (&probe_feature, g_bytes_unref);
  g_clear_pointer (&input_identity, g_bytes_unref);
  return status;
}
