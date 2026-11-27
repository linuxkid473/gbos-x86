/*
 * Goodix 53x5 driver for libfprint - native Milan enrollment templates
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define FP_COMPONENT "goodix53x5"

#include "milan/enrollment/propagation.h"
#include "milan/match/match.h"
#include "milan/match/info-private.h"
#include "milan/milan.h"
#include "milan/relations.h"
#include "milan/study/policy.h"
#include "milan/transform-private.h"

#include <string.h>

GBytes *
goodix_milan_match_serialize_template (GoodixMatchInfo *info)
{
  return info && info->template ? g_bytes_ref (info->template) : NULL;
}

#define GOODIX_MILAN_ENROLLMENT_RECORD_CAPACITY 150

typedef struct
{
  gboolean                         populated;
  GoodixMilanFeatureBitmaps        bitmaps;
  guint8                           inline_mask[72];
  GoodixMilanFeatureRecord         records[GOODIX_MILAN_ENROLLMENT_RECORD_CAPACITY];
  guint32                          record_count;
  GoodixMilanAntifakeBlob          antifake;
  GoodixMilanFeatureTemplateFields fields;
  gint32                           neighbor_count;
} GoodixMilanEnrollmentFeature;

struct _GoodixMilanEnrollmentTransaction
{
  GoodixMilanEnrollmentFeature features[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  GoodixMilanRelationMatrix    matrix;
  guint                        feature_count;
  guint                        registration_count;
  guint32                      sensor_type;
  gint32                       graph_companion_f3;
  gint32                       graph_companion_f4;
  guint32                      queue_state;
  guint32                      queue_transaction_counter;
  guint8                       tail_state[0x520];
};

static void
goodix_milan_match_set_feature_scalar (guint8 *element,
                                 size_t  element_size,
                                 size_t  field,
                                 gint32  value)
{
  guint32 little_endian = GUINT32_TO_LE ((guint32) value);
  size_t optional_size = element_size >= 5 && element[element_size - 5] == 0xc7 ?
                         5 :
                         0;

  memcpy (element + element_size - 54 - optional_size + field * 5,
          &little_endian,
          sizeof (little_endian));
}

static void
goodix_enrollment_feature_view (const GoodixMilanEnrollmentFeature *feature,
                                GoodixMilanFeatureView             *view)
{
  memset (view, 0, sizeof (*view));
  view->high_bitmap = feature->bitmaps.high_bitmap;
  view->enhanced_bitmap = feature->bitmaps.enhanced_bitmap;
  view->inline_mask = feature->inline_mask;
  view->low_bitmap = feature->bitmaps.low_bitmap;
  view->antifake = &feature->antifake;
  view->record_count = feature->record_count;
  view->fields = feature->fields;
}

static gboolean
goodix_enrollment_feature_load (
  GoodixMilanEnrollmentTransaction *transaction,
  guint                             slot,
  GBytes                           *probe_template)
{
  g_autofree GoodixMilanUnpackedTemplate *unpacked = NULL;
  GoodixMilanEnrollmentFeature *feature;
  GoodixMilanFeatureView view;
  const guint8 *data;
  gsize size;
  gint32 retained_bb;
  gint32 retained_neighbor_count;

  if (!transaction || !probe_template ||
      slot >= GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT)
    return FALSE;
  data = g_bytes_get_data (probe_template, &size);
  unpacked = g_try_new0 (GoodixMilanUnpackedTemplate, 1);
  if (!unpacked || size > GOODIX_MILAN_TEMPLATE_MAX_SIZE ||
      goodix_milan_template_unpack (data, size, unpacked) != 0 ||
      unpacked->feature_count != 1 ||
      goodix_milan_template_parse_feature_element (
        unpacked->feature_elements[0], unpacked->feature_element_sizes[0],
        &view) != 0 ||
      view.record_count > GOODIX_MILAN_ENROLLMENT_RECORD_CAPACITY ||
      view.fields.tagged_values[2] < 0 ||
      (guint32) view.fields.tagged_values[2] > view.record_count)
    return FALSE;

  feature = &transaction->features[slot];
  retained_bb = feature->fields.tagged_values[6];
  retained_neighbor_count = feature->neighbor_count;
  memcpy (feature->bitmaps.high_bitmap, view.high_bitmap,
          sizeof (feature->bitmaps.high_bitmap));
  memcpy (feature->bitmaps.enhanced_bitmap, view.enhanced_bitmap,
          sizeof (feature->bitmaps.enhanced_bitmap));
  memcpy (feature->bitmaps.low_bitmap, view.low_bitmap,
          sizeof (feature->bitmaps.low_bitmap));
  memcpy (feature->inline_mask, view.inline_mask,
          sizeof (feature->inline_mask));
  if (goodix_milan_feature_unpack_template_records (
        view.packed_records, view.record_count,
        (guint32) view.fields.tagged_values[2], feature->records,
        G_N_ELEMENTS (feature->records)) != 0)
    return FALSE;
  memcpy (&feature->antifake, view.antifake, sizeof (feature->antifake));
  feature->record_count = (guint32) view.record_count;
  feature->fields = view.fields;
  feature->fields.tagged_values[6] = retained_bb;
  feature->neighbor_count = retained_neighbor_count;
  feature->populated = TRUE;

  if (slot == 0)
    {
      transaction->sensor_type = unpacked->metadata.sensor_type;
      transaction->queue_state = unpacked->metadata.queue_state;
      transaction->queue_transaction_counter =
        unpacked->metadata.queue_transaction_counter;
      memcpy (transaction->tail_state, unpacked->tail_state,
              sizeof (transaction->tail_state));
    }
  else if (unpacked->metadata.sensor_type != transaction->sensor_type)
    {
      return FALSE;
    }
  return TRUE;
}

static GBytes *
goodix_enrollment_feature_pack (
  const GoodixMilanEnrollmentFeature *feature)
{
  gsize capacity;
  gsize size = 0;
  guint8 *packed;

  if (!feature || !feature->populated)
    return NULL;
  capacity = 7945 + (gsize) feature->record_count * 32 +
             (feature->fields.optional_c7 != 0 ? 5 : 0);
  packed = g_try_malloc (capacity);
  if (!packed || goodix_milan_template_pack_feature_element (
        feature->bitmaps.high_bitmap, feature->bitmaps.enhanced_bitmap,
        feature->inline_mask, feature->bitmaps.low_bitmap, feature->records,
        feature->record_count, &feature->antifake, &feature->fields, packed,
        capacity, &size) != 0)
    {
      g_free (packed);
      return NULL;
    }
  return g_bytes_new_take (packed, size);
}

static void
goodix_milan_match_update_antifake_score (GoodixMilanAntifakeBlob *antifake,
                                    gint32                   score)
{
  gint32 current = goodix_milan_antifake_pair_score (antifake);

  if (current == -1)
    {
      current = score;
    }
  else
    {
      uint32_t sum = (uint32_t) current + (uint32_t) score;

      current = goodix_milan_transform_s32 (sum) / 2;
    }
  goodix_milan_antifake_set_pair_score (antifake, current);
}

typedef struct
{
  GoodixMilanRelationMatrix    *matrix;
  const GoodixMilanFeatureView *views;
} GoodixEnrollmentPropagationContext;

static int
goodix_enrollment_relation_overlap (
  void    *user_data,
  size_t   first,
  size_t   second,
  int32_t *overlap)
{
  GoodixEnrollmentPropagationContext *context = user_data;
  size_t high = MAX (first, second);
  size_t low = MIN (first, second);
  GoodixMilanRelationSlot *slot = goodix_milan_enrollment_relation_slot (
    context->matrix, high, low);
  int32_t coverage;
  int32_t detail;
  int32_t low_metrics[3];

  if (!slot || slot->values[0] < 0 ||
      goodix_milan_match_overlap_metrics (
        &context->views[low], &context->views[high], slot->values + 1,
        overlap, &coverage, &detail, low_metrics) != 0)
    return -1;
  return 0;
}

static int
goodix_enrollment_propagate_lower (
  GoodixMilanRelationMatrix   *matrix,
  const GoodixMilanFeatureView views[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  int                          active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  size_t                       pivot,
  const int32_t                pivot_to_reference[6])
{
  GoodixEnrollmentPropagationContext context = { matrix, views };

  return goodix_milan_enrollment_propagate_lower (
    matrix, active, pivot, pivot_to_reference, 205,
    goodix_enrollment_relation_overlap, &context);
}

static int
goodix_enrollment_propagate_higher (
  GoodixMilanRelationMatrix   *matrix,
  const GoodixMilanFeatureView views[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  int                          active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  size_t                       pivot,
  size_t                       current,
  const int32_t                pivot_to_reference[6])
{
  GoodixEnrollmentPropagationContext context = { matrix, views };

  return goodix_milan_enrollment_propagate_higher (
    matrix, active, pivot, current, pivot_to_reference, 205,
    goodix_enrollment_relation_overlap, &context);
}

static int
goodix_enrollment_bridge_feature (
  GoodixMilanRelationMatrix   *matrix,
  const GoodixMilanFeatureView views[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  int                          active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  size_t                       current,
  size_t                       pivot)
{
  GoodixMilanRelationSlot *direct = goodix_milan_enrollment_relation_slot (
    matrix, current, pivot);
  int32_t current_to_reference[6];
  int32_t pivot_to_current[6];
  int32_t pivot_to_reference[6];

  if (!direct || direct->values[0] < 0 ||
      goodix_milan_enrollment_feature_to_reference (
        matrix, current, current_to_reference) != 0 ||
      goodix_milan_transform_invert (
        direct->values + 1, pivot_to_current) != 0)
    return -1;
  goodix_milan_transform_compose (
    current_to_reference, pivot_to_current, pivot_to_reference);
  if (goodix_milan_relation_matrix_store_reference (
        matrix, pivot, pivot_to_reference) != 0)
    return -1;
  active[pivot] = TRUE;
  if (goodix_enrollment_propagate_lower (
        matrix, views, active, pivot, pivot_to_reference) != 0 ||
      goodix_enrollment_propagate_higher (
        matrix, views, active, pivot, current, pivot_to_reference) != 0)
    return -1;
  return 0;
}

static int32_t
goodix_enrollment_residual_area (
  GoodixMilanRelationMatrix   *matrix,
  const GoodixMilanFeatureView views[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  const int                    active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  size_t                       current)
{
  uint8_t residual[GOODIX_MILAN_STUDY_MASK_SIZE];
  int32_t current_to_reference[6];
  int32_t residual_count = 0;

  goodix_milan_study_policy_expand_mask (
    views[current].inline_mask, residual);
  if (goodix_milan_enrollment_feature_to_reference (
        matrix, current, current_to_reference) != 0)
    return -1;
  for (size_t prior = 0; prior < current; prior++)
    {
      int32_t prior_to_reference[6];
      int32_t reference_to_prior[6];
      int32_t current_to_prior[6];

      if (!active[prior] || views[prior].fields.tagged_values[5] == 5)
        continue;
      if (goodix_milan_enrollment_feature_to_reference (
            matrix, prior, prior_to_reference) != 0 ||
          goodix_milan_transform_invert (
            prior_to_reference, reference_to_prior) != 0)
        return -1;
      goodix_milan_transform_compose (
        reference_to_prior, current_to_reference, current_to_prior);
      current_to_prior[2] = goodix_milan_transform_sar32 (
        (uint32_t) current_to_prior[2], 1);
      current_to_prior[5] = goodix_milan_transform_sar32 (
        (uint32_t) current_to_prior[5], 1);
      goodix_milan_study_policy_remove_footprint (
        residual, current_to_prior);
    }
  for (size_t i = 0; i < sizeof (residual); i++)
    residual_count += residual[i] != 0;
  return residual_count < 20 ? 0 : residual_count * 4;
}

static int
goodix_enrollment_metrics (
  GoodixMilanRelationMatrix   *matrix,
  const GoodixMilanFeatureView views[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  const int                    active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  const int                    accepted_priors[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  const int32_t                accepted_overlaps[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  size_t                       current,
  GoodixMilanEnrollmentResult *result)
{
  const int32_t total_area =
    GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS *
    GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS;
  int32_t residual = total_area;

  result->previous_overlap = 0;
  for (size_t prior = current; prior-- > 0;)
    {
      GoodixMilanRelationSlot *slot;

      if (views[prior].fields.tagged_values[5] == 5)
        continue;
      slot = goodix_milan_enrollment_relation_slot (matrix, current, prior);
      if (!slot)
        return -1;
      if (slot->values[0] >= 6)
        {
          int32_t area = goodix_milan_study_policy_full_footprint_area (
            slot->values + 1);

          result->previous_overlap = 100 - (guint8)
                                     ((total_area - area) * 100 / (total_area + 1));
        }
      break;
    }

  if (matrix->graph_established && active[current])
    {
      residual = goodix_enrollment_residual_area (
        matrix, views, active, current);
      if (residual < 0)
        return -1;
    }
  else
    {
      for (size_t prior = 0; prior < current; prior++)
        if (accepted_priors[prior] && accepted_overlaps[prior] > 205)
          {
            GoodixMilanRelationSlot *slot =
              goodix_milan_enrollment_relation_slot (matrix, current, prior);
            int32_t uncovered;

            if (!slot)
              return -1;
            uncovered = total_area -
                        goodix_milan_study_policy_full_footprint_area (
              slot->values + 1);
            if (uncovered < residual)
              residual = uncovered;
          }
    }
  result->overlap = 100 - (guint8)
                    (residual * 100 / (total_area + 1));
  return 0;
}

GBytes *
goodix_milan_match_combine_templates (GPtrArray *templates)
{
  GoodixMilanUnpackedTemplate *unpacked = NULL;
  GoodixMilanRelationMatrix *relation_matrix = NULL;
  const guint8 *elements[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  guint8 *element_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  GoodixMilanAntifakeBlob *mutable_antifakes[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  size_t element_sizes[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  GoodixMilanFeatureView feature_views[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  GoodixMilanTemplateRelation relations[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  int active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  size_t relation_count = 0;
  GoodixMilanTemplateMetadata metadata = { 0 };
  guint8 tail_state[0x520] = { 0 };
  guint8 *combined = NULL;
  guint8 *normalized = NULL;
  size_t combined_capacity = 1433;
  size_t combined_size = 0;
  size_t normalized_size = 0;
  GBytes *result = NULL;

  if (!templates || templates->len == 0 ||
      templates->len > GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT)
    return NULL;
  unpacked = g_new0 (GoodixMilanUnpackedTemplate, 1);
  relation_matrix = g_new0 (GoodixMilanRelationMatrix, 1);
  for (guint i = 0; i < templates->len; i++)
    {
      GBytes *template_bytes = g_ptr_array_index (templates, i);
      gsize template_size;
      const guint8 *template_data = g_bytes_get_data (
        template_bytes, &template_size);

      if (template_size > GOODIX_MILAN_TEMPLATE_MAX_SIZE ||
          goodix_milan_template_unpack (
            template_data, template_size, unpacked) != 0 ||
          unpacked->feature_count != 1)
        goto out;
      element_sizes[i] = unpacked->feature_element_sizes[0];
      if (element_sizes[i] < 55 ||
          element_sizes[i] > GOODIX_MILAN_TEMPLATE_MAX_SIZE - combined_capacity)
        goto out;
      element_copies[i] = g_memdup2 (
        unpacked->feature_elements[0], element_sizes[i]);
      elements[i] = element_copies[i];
      mutable_antifakes[i] = goodix_milan_template_mutable_feature_antifake (
        element_copies[i], element_sizes[i]);
      if (!mutable_antifakes[i] ||
          goodix_milan_template_parse_feature_element (
            element_copies[i], element_sizes[i], &feature_views[i]) != 0)
        goto out;
      relation_matrix->row_bases[i] =
        goodix_milan_relation_matrix_expected_row_base (i);
      if (i == 0)
        {
          metadata.sensor_type = unpacked->metadata.sensor_type;
          memcpy (tail_state, unpacked->tail_state, sizeof (tail_state));
        }
      else if (unpacked->metadata.sensor_type != metadata.sensor_type)
        {
          goto out;
        }
      combined_capacity += element_sizes[i];
    }
  relation_matrix->feature_count = templates->len;
  relation_matrix->reference_feature_index = SIZE_MAX;
  for (size_t i = 0; i < GOODIX_MILAN_TEMPLATE_RELATION_CAPACITY; i++)
    goodix_milan_relation_slot_unset (&relation_matrix->slots[i]);
  metadata.maximum_features = GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT;
  metadata.registration_count = 1;
  metadata.maximum_records = 150;
  metadata.graph_reference_index = -1;
  metadata.graph_companion_f3 = -1;
  metadata.graph_companion_f4 = -1;
  memset (tail_state, 0xff, 200);
  for (guint i = 0; i < templates->len; i++)
    {
      goodix_milan_match_set_feature_scalar (
        element_copies[i], element_sizes[i], 1,
        (gint32) metadata.registration_count);
      goodix_milan_match_set_feature_scalar (
        element_copies[i], element_sizes[i], 7, (gint32) i);
      memcpy (tail_state + i * 4, &i, sizeof (i));
      if (i == 0)
        {
          goodix_milan_match_set_feature_scalar (
            element_copies[i], element_sizes[i], 1, 0);
          continue;
        }

      GoodixMilanFeatureView current_view;
      GoodixMilanFeatureRecord current_records[150];
      int accepted_priors[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
      int32_t accepted_overlaps[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
      int32_t best_inliers = -1;
      int32_t best_connected_inliers = -1;
      guint best_prior = 0;
      guint best_connected_prior = 0;

      current_view = feature_views[i];
      if (goodix_milan_feature_unpack_template_records (
            current_view.packed_records, current_view.record_count,
            current_view.fields.tagged_values[2], current_records, 150) != 0)
        goto out;
      for (guint prior = 0; prior < i; prior++)
        {
          GoodixMilanFeatureView prior_view;
          GoodixMilanFeatureRecord prior_records[150];
          GoodixMilanTemplateRelation candidate;
          int32_t registration_detail;
          int32_t registration_coverage;

          prior_view = feature_views[prior];
          if (goodix_milan_feature_unpack_template_records (
                prior_view.packed_records, prior_view.record_count,
                prior_view.fields.tagged_values[2], prior_records, 150) != 0)
            goto out;
          if (goodix_milan_estimate_relation (
                prior_records, prior_view.record_count, current_records,
                current_view.record_count,
                (int32_t) metadata.registration_count + prior,
                &candidate) != 0)
            continue;
          if (goodix_milan_registration_gate_metrics (
                &prior_view, &current_view, candidate.values + 1,
                &registration_detail, &registration_coverage) != 0)
            continue;
          int accepted = candidate.values[0] > 10 ||
                         (candidate.values[0] > 5 && registration_detail > 215) ||
                         (candidate.values[0] > 6 && registration_detail > 208 &&
                          registration_coverage > 64);

          if (!accepted)
            continue;
          int32_t expected_index = relation_matrix->row_bases[i] + prior;

          if (candidate.index != expected_index || expected_index < 1 ||
              expected_index > GOODIX_MILAN_TEMPLATE_RELATION_CAPACITY)
            goto out;
          memcpy (relation_matrix->slots[expected_index - 1].values,
                  candidate.values, sizeof (candidate.values));
          accepted_priors[prior] = TRUE;
          GoodixEnrollmentPropagationContext context = {
            relation_matrix, feature_views
          };

          if (goodix_enrollment_relation_overlap (
                &context, i, prior,
                &accepted_overlaps[prior]) != 0)
            goto out;
          if (active[prior] &&
              candidate.values[0] > best_connected_inliers)
            {
              best_connected_inliers = candidate.values[0];
              best_connected_prior = prior;
            }
          if (prior_view.fields.tagged_values[4] > 40 &&
              current_view.fields.tagged_values[4] > 40)
            {
              gint32 pair_metrics[5];

              if (goodix_milan_antifake_pair_metrics (
                    prior_view.antifake, GOODIX_MILAN_ANTIFAKE_SIZE,
                    current_view.antifake, GOODIX_MILAN_ANTIFAKE_SIZE,
                    candidate.values + 1, pair_metrics) != 0)
                goto out;
              if (pair_metrics[1] != 0)
                {
                  goodix_milan_match_update_antifake_score (
                    mutable_antifakes[prior], pair_metrics[4]);
                  goodix_milan_match_update_antifake_score (
                    mutable_antifakes[i], pair_metrics[4]);
                }
            }
          if (candidate.values[0] > best_inliers)
            {
              best_inliers = candidate.values[0];
              best_prior = prior;
            }
        }
      if (!relation_matrix->graph_established && best_inliers > 5 &&
          accepted_overlaps[best_prior] > 205)
        {
          active[i] = TRUE;
          active[best_prior] = TRUE;
          relation_matrix->reference_feature_index = best_prior;
          relation_matrix->graph_established = 1;
          metadata.graph_reference_index = (gint32) best_prior;
          metadata.graph_established = 1;
        }
      else if (best_connected_inliers >= 0)
        {
          GoodixMilanRelationSlot *direct =
            goodix_milan_enrollment_relation_slot (
              relation_matrix, i, best_connected_prior);
          GoodixMilanRelationSlot *reference_slot =
            goodix_milan_enrollment_relation_slot (
              relation_matrix, i,
              relation_matrix->reference_feature_index);
          int32_t current_to_reference[6];

          if (!direct || !reference_slot)
            goto out;
          if (best_connected_prior ==
              relation_matrix->reference_feature_index)
            {
              memcpy (current_to_reference, direct->values + 1,
                      sizeof (current_to_reference));
            }
          else
            {
              GoodixMilanRelationSlot *connected =
                goodix_milan_enrollment_relation_slot (
                  relation_matrix, best_connected_prior,
                  relation_matrix->reference_feature_index);

              if (!connected || connected->values[0] < 0)
                goto out;
              goodix_milan_transform_route (
                connected->values + 1, direct->values + 1,
                (int32_t) best_connected_prior,
                (int32_t) relation_matrix->reference_feature_index,
                current_to_reference);
            }
          memcpy (reference_slot->values + 1, current_to_reference,
                  sizeof (current_to_reference));
          if (reference_slot->values[0] < 0)
            reference_slot->values[0] = 0;
          active[i] = TRUE;
        }
      if (relation_matrix->graph_established && active[i])
        {
          int bridge_candidates[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };

          for (guint prior = 0; prior < i; prior++)
            bridge_candidates[prior] = accepted_priors[prior] && !active[prior];
          for (guint prior = 0; prior < i; prior++)
            if (bridge_candidates[prior] && accepted_overlaps[prior] > 208 &&
                goodix_enrollment_bridge_feature (
                  relation_matrix, feature_views, active, i, prior) != 0)
              goto out;
        }
      metadata.registration_count += i;
    }
  if (templates->len == GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT &&
      goodix_milan_relation_matrix_close (relation_matrix, active) < 0)
    goto out;
  for (guint i = 0; i < templates->len; i++)
    goodix_milan_match_set_feature_scalar (
      element_copies[i], element_sizes[i], 0, active[i] ? 1 : 0);
  if (goodix_milan_relation_matrix_project_reference_star (
        relation_matrix, relations, G_N_ELEMENTS (relations),
        &relation_count) != 0)
    goto out;
  if (relation_count >
      (GOODIX_MILAN_TEMPLATE_MAX_SIZE - combined_capacity) / 45)
    goto out;
  combined_capacity += relation_count * 45;
  combined = g_malloc (combined_capacity);
  if (goodix_milan_template_pack (
        elements, element_sizes, templates->len, relations, relation_count,
        &metadata,
        tail_state, sizeof (tail_state), combined, combined_capacity,
        &combined_size) != 0)
    goto out;
  if (templates->len == GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT)
    {
      normalized = g_malloc (combined_capacity);
      if (goodix_milan_template_normalize (
            combined, combined_size, normalized, combined_capacity,
            &normalized_size) != 0)
        goto out;
      result = g_bytes_new_take (normalized, normalized_size);
      normalized = NULL;
    }
  else
    {
      result = g_bytes_new_take (combined, combined_size);
      combined = NULL;
    }

out:
  for (guint i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    g_free (element_copies[i]);
  g_free (combined);
  g_free (normalized);
  g_free (relation_matrix);
  g_free (unpacked);
  return result;
}

GoodixMilanEnrollmentTransaction *
goodix_milan_enrollment_transaction_new (void)
{
  GoodixMilanEnrollmentTransaction *transaction = g_try_new0 (
    GoodixMilanEnrollmentTransaction, 1);

  if (!transaction)
    return NULL;
  transaction->matrix.reference_feature_index = SIZE_MAX;
  transaction->graph_companion_f3 = -1;
  transaction->graph_companion_f4 = -1;
  memset (transaction->tail_state, 0xff, 200);
  for (guint i = 0; i < GOODIX_MILAN_TEMPLATE_RELATION_CAPACITY; i++)
    goodix_milan_relation_slot_unset (&transaction->matrix.slots[i]);
  return transaction;
}

void
goodix_milan_enrollment_transaction_free (
  GoodixMilanEnrollmentTransaction *transaction)
{
  g_free (transaction);
}

guint
goodix_milan_enrollment_transaction_count (
  const GoodixMilanEnrollmentTransaction *transaction)
{
  return transaction ? transaction->feature_count : 0;
}

static GoodixMilanEnrollmentTransaction *
goodix_enrollment_transaction_clone (
  const GoodixMilanEnrollmentTransaction *transaction)
{
  GoodixMilanEnrollmentTransaction *copy;

  if (!transaction)
    return NULL;
  copy = g_try_malloc (sizeof (*copy));
  if (copy)
    memcpy (copy, transaction, sizeof (*copy));
  return copy;
}

static void
goodix_enrollment_transaction_views (
  const GoodixMilanEnrollmentTransaction *transaction,
  GoodixMilanFeatureView                  views[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  int                                     active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY])
{
  memset (views, 0,
          GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY * sizeof (*views));
  memset (active, 0,
          GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY * sizeof (*active));
  for (guint i = 0; i < transaction->feature_count; i++)
    {
      goodix_enrollment_feature_view (&transaction->features[i], &views[i]);
      active[i] = transaction->features[i].fields.tagged_values[0] == 1;
    }
}

static gboolean
goodix_enrollment_transaction_recompute_live_fields (
  GoodixMilanEnrollmentTransaction *transaction)
{
  static const gint32 identity[6] = { 0x100, 0, 0, 0, 0x100, 0 };
  GoodixMilanFeatureView views[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  int active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  guint reference = transaction->matrix.reference_feature_index;

  if (transaction->matrix.graph_established &&
      reference >= transaction->feature_count)
    return FALSE;

  goodix_enrollment_transaction_views (transaction, views, active);
  for (guint feature = 0; feature < transaction->feature_count; feature++)
    {
      GoodixMilanEnrollmentFeature *current = &transaction->features[feature];
      guint8 residual[GOODIX_MILAN_STUDY_MASK_SIZE];
      gint32 current_to_reference[6];
      gint32 residual_count = 0;

      current->neighbor_count = 0;
      current->fields.tagged_values[6] = 52 * 44;
      if (!active[feature])
        continue;
      if (goodix_milan_enrollment_feature_to_reference (
            &transaction->matrix, feature, current_to_reference) != 0)
        return FALSE;
      goodix_milan_study_policy_expand_mask (current->inline_mask, residual);
      for (guint other = 0; other < transaction->feature_count; other++)
        {
          GoodixMilanRelationSlot *slot;
          gint32 reference_to_other[6];
          gint32 current_to_other[6];

          if (other == feature || !active[other])
            continue;
          if (other == reference)
            {
              memcpy (reference_to_other, identity, sizeof (identity));
            }
          else
            {
              slot = goodix_milan_enrollment_relation_slot (
                &transaction->matrix, other, reference);
              if (!slot || slot->values[0] < 0)
                return FALSE;
              if (reference < other)
                {
                  if (goodix_milan_transform_invert (
                        slot->values + 1, reference_to_other) != 0)
                    return FALSE;
                }
              else
                {
                  memcpy (reference_to_other, slot->values + 1,
                          sizeof (reference_to_other));
                }
            }
          goodix_milan_transform_compose (
            reference_to_other, current_to_reference, current_to_other);
          current_to_other[2] = goodix_milan_transform_sar32 (
            (guint32) current_to_other[2], 1);
          current_to_other[5] = goodix_milan_transform_sar32 (
            (guint32) current_to_other[5], 1);
          if (goodix_milan_study_policy_footprint_area (current_to_other) * 100 >
              52 * 44 * 40)
            current->neighbor_count++;
          goodix_milan_study_policy_remove_footprint (
            residual, current_to_other);
        }
      for (guint i = 0; i < G_N_ELEMENTS (residual); i++)
        residual_count += residual[i] != 0;
      current->fields.tagged_values[6] =
        residual_count < 20 ? 0 : residual_count;
    }
  return TRUE;
}

static gboolean
goodix_enrollment_transaction_insert (
  GoodixMilanEnrollmentTransaction *transaction,
  GBytes                           *probe_template,
  GoodixMilanEnrollmentResult      *result)
{
  GoodixMilanFeatureView views[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  int active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  guint current;
  gint32 row_base;

  if (!transaction || !probe_template || !result ||
      transaction->feature_count >= GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT ||
      transaction->matrix.feature_count != transaction->feature_count)
    return FALSE;
  current = transaction->feature_count;
  row_base = goodix_milan_relation_matrix_expected_row_base (current);
  if (!goodix_enrollment_feature_load (
        transaction, current, probe_template) ||
      goodix_milan_relation_matrix_append_row (
        &transaction->matrix, row_base) != 0)
    return FALSE;

  transaction->features[current].fields.tagged_values[1] = row_base;
  transaction->features[current].fields.tagged_values[5] = 0;
  transaction->features[current].fields.tagged_values[7] = (gint32) current;
  transaction->features[current].fields.tagged_values[8] = 0;
  transaction->features[current].fields.tagged_values[9] = 0;
  transaction->feature_count++;
  transaction->registration_count = current == 0 ?
                                    1 : (guint) row_base + current;
  goodix_enrollment_transaction_views (transaction, views, active);

  if (current != 0)
    {
      int accepted_priors[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
      gint32 accepted_overlaps[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
      gint32 best_inliers = -1;
      gint32 best_connected_inliers = -1;
      guint best_prior = 0;
      guint best_connected_prior = 0;

      for (guint prior = 0; prior < current; prior++)
        {
          GoodixMilanTemplateRelation candidate;
          GoodixMilanRelationSlot *slot;
          gint32 registration_detail;
          gint32 registration_coverage;

          slot = goodix_milan_enrollment_relation_slot (
            &transaction->matrix, current, prior);
          if (!slot)
            return FALSE;
          goodix_milan_relation_slot_unset (slot);
          if (goodix_milan_estimate_relation (
                transaction->features[prior].records,
                transaction->features[prior].record_count,
                transaction->features[current].records,
                transaction->features[current].record_count,
                row_base + (gint32) prior, &candidate) != 0 ||
              goodix_milan_registration_gate_metrics (
                &views[prior], &views[current], candidate.values + 1,
                &registration_detail, &registration_coverage) != 0)
            continue;
          if (!(candidate.values[0] > 10 ||
                (candidate.values[0] > 5 && registration_detail > 215) ||
                (candidate.values[0] > 6 && registration_detail > 208 &&
                 registration_coverage > 64)))
            continue;

          memcpy (slot->values, candidate.values, sizeof (slot->values));
          accepted_priors[prior] = 1;
          GoodixEnrollmentPropagationContext context = {
            &transaction->matrix, views
          };

          if (goodix_enrollment_relation_overlap (
                &context, current, prior, &accepted_overlaps[prior]) != 0)
            return FALSE;
          if (active[prior] &&
              candidate.values[0] > best_connected_inliers)
            {
              best_connected_inliers = candidate.values[0];
              best_connected_prior = prior;
            }
          if (transaction->features[prior].fields.tagged_values[4] > 40 &&
              transaction->features[current].fields.tagged_values[4] > 40)
            {
              gint32 pair_metrics[5];

              if (goodix_milan_antifake_pair_metrics (
                    &transaction->features[prior].antifake,
                    GOODIX_MILAN_ANTIFAKE_SIZE,
                    &transaction->features[current].antifake,
                    GOODIX_MILAN_ANTIFAKE_SIZE, candidate.values + 1,
                    pair_metrics) != 0)
                return FALSE;
              /* Native leaves both scores untouched when no mutual pair exists. */
              if (pair_metrics[1] != 0)
                {
                  goodix_milan_match_update_antifake_score (
                    &transaction->features[prior].antifake, pair_metrics[4]);
                  goodix_milan_match_update_antifake_score (
                    &transaction->features[current].antifake, pair_metrics[4]);
                }
            }
          if (candidate.values[0] > best_inliers)
            {
              best_inliers = candidate.values[0];
              best_prior = prior;
            }
        }

      if (!transaction->matrix.graph_established && best_inliers > 5 &&
          accepted_overlaps[best_prior] > 205)
        {
          active[current] = 1;
          active[best_prior] = 1;
          transaction->matrix.reference_feature_index = best_prior;
          transaction->matrix.graph_established = 1;
        }
      else if (best_connected_inliers >= 0)
        {
          GoodixMilanRelationSlot *direct =
            goodix_milan_enrollment_relation_slot (
              &transaction->matrix, current, best_connected_prior);
          GoodixMilanRelationSlot *reference_slot =
            goodix_milan_enrollment_relation_slot (
              &transaction->matrix, current,
              transaction->matrix.reference_feature_index);
          gint32 current_to_reference[6];

          if (!direct || !reference_slot)
            return FALSE;
          if (best_connected_prior ==
              transaction->matrix.reference_feature_index)
            {
              memcpy (current_to_reference, direct->values + 1,
                      sizeof (current_to_reference));
            }
          else
            {
              GoodixMilanRelationSlot *connected =
                goodix_milan_enrollment_relation_slot (
                  &transaction->matrix, best_connected_prior,
                  transaction->matrix.reference_feature_index);

              if (!connected || connected->values[0] < 0)
                return FALSE;
              goodix_milan_transform_route (
                connected->values + 1, direct->values + 1,
                (gint32) best_connected_prior,
                (gint32) transaction->matrix.reference_feature_index,
                current_to_reference);
            }
          memcpy (reference_slot->values + 1, current_to_reference,
                  sizeof (current_to_reference));
          if (reference_slot->values[0] < 0)
            reference_slot->values[0] = 0;
          active[current] = 1;
        }
      if (transaction->matrix.graph_established && active[current])
        {
          int bridge_candidates[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };

          for (guint prior = 0; prior < current; prior++)
            bridge_candidates[prior] = accepted_priors[prior] && !active[prior];
          for (guint prior = 0; prior < current; prior++)
            if (bridge_candidates[prior] && accepted_overlaps[prior] > 208 &&
                goodix_enrollment_bridge_feature (
                  &transaction->matrix, views, active, current, prior) != 0)
              return FALSE;
        }

      for (guint i = 0; i < transaction->feature_count; i++)
        transaction->features[i].fields.tagged_values[0] = active[i] ? 1 : 0;
      if (best_inliers >= 0 && goodix_enrollment_metrics (
            &transaction->matrix, views, active, accepted_priors,
            accepted_overlaps, current, result) != 0)
        return FALSE;
    }

  for (guint i = 0; i < transaction->feature_count; i++)
    memcpy (transaction->tail_state + i * sizeof (i), &i, sizeof (i));
  for (guint i = transaction->feature_count;
       i < GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT; i++)
    memset (transaction->tail_state + i * sizeof (i), 0xff, sizeof (i));

  if (transaction->feature_count == GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT)
    {
      int final_active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };

      for (guint i = 0; i < transaction->feature_count; i++)
        final_active[i] =
          transaction->features[i].fields.tagged_values[0] == 1;
      if (goodix_milan_relation_matrix_close (
            &transaction->matrix, final_active) < 0)
        return FALSE;
      for (guint i = 0; i < transaction->feature_count; i++)
        transaction->features[i].fields.tagged_values[0] =
          final_active[i] ? 1 : 0;
      return goodix_enrollment_transaction_recompute_live_fields (transaction);
    }
  return TRUE;
}

static gboolean
goodix_enrollment_transaction_delete_last (
  GoodixMilanEnrollmentTransaction *transaction)
{
  GoodixMilanEnrollmentFeature *removed;
  guint removed_index;
  gint32 row_base;

  if (!transaction || transaction->feature_count == 0 ||
      transaction->matrix.feature_count != transaction->feature_count)
    return FALSE;
  removed_index = transaction->feature_count - 1;
  removed = &transaction->features[removed_index];
  row_base = removed->fields.tagged_values[1];
  if (row_base < 0 || (guint) row_base > transaction->registration_count)
    return FALSE;
  for (guint index = MAX (1U, (guint) row_base);
       index < transaction->registration_count; index++)
    {
      GoodixMilanRelationSlot *slot = goodix_milan_relation_matrix_slot (
        &transaction->matrix, (gint32) index);

      if (!slot)
        return FALSE;
      goodix_milan_relation_slot_unset (slot);
    }
  transaction->registration_count = (guint) row_base;
  transaction->feature_count--;
  transaction->matrix.feature_count--;
  if (removed_index == transaction->matrix.reference_feature_index)
    {
      transaction->matrix.graph_established = 0;
      for (guint i = 0; i < transaction->feature_count; i++)
        transaction->features[i].fields.tagged_values[0] = 0;
    }
  return goodix_enrollment_transaction_recompute_live_fields (transaction);
}

static gboolean
goodix_enrollment_policy_should_rollback (
  const GoodixMilanEnrollmentResult *result,
  guint                              bad_record_count,
  guint                              bad_continue_count)
{
  guint post_insertion_count;

  if (!result)
    return FALSE;
  post_insertion_count = result->pre_insertion_accepted_count + 1;
  return result->pre_insertion_accepted_count > 2 &&
         bad_record_count <= 4 && bad_continue_count <= 2 &&
         result->overlap > 85 &&
         (post_insertion_count < 7 || result->previous_overlap > 75);
}

static void
goodix_enrollment_policy_commit_non_rollback (guint *bad_continue_count)
{
  if (*bad_continue_count < 3)
    *bad_continue_count = 0;
}

static guint
goodix_enrollment_policy_commit_rollback (guint *bad_record_count,
                                          guint *bad_continue_count)
{
  static const guint details[] = { 2, 4, 1, 3 };
  guint detail = details[*bad_record_count % G_N_ELEMENTS (details)];

  (*bad_record_count)++;
  (*bad_continue_count)++;
  return detail;
}

GoodixMilanEnrollmentAttemptStatus
goodix_milan_enrollment_transaction_attempt (
  GoodixMilanEnrollmentTransaction **transaction,
  GBytes                            *probe_template,
  guint                             *bad_record_count,
  guint                             *bad_continue_count,
  GoodixMilanEnrollmentResult       *result)
{
  GoodixMilanEnrollmentTransaction *tentative;

  if (!transaction || !*transaction || !probe_template ||
      !bad_record_count || !bad_continue_count || !result)
    return GOODIX_MILAN_ENROLLMENT_RETRY_REMOVE;
  memset (result, 0, sizeof (*result));
  result->pre_insertion_accepted_count = (*transaction)->feature_count;
  tentative = goodix_enrollment_transaction_clone (*transaction);
  if (!tentative || !goodix_enrollment_transaction_insert (
        tentative, probe_template, result))
    {
      goodix_milan_enrollment_transaction_free (tentative);
      goodix_enrollment_policy_commit_non_rollback (bad_continue_count);
      return GOODIX_MILAN_ENROLLMENT_RETRY_REMOVE;
    }

  if (goodix_enrollment_policy_should_rollback (
        result, *bad_record_count, *bad_continue_count))
    {
      if (!goodix_enrollment_transaction_delete_last (tentative))
        {
          goodix_milan_enrollment_transaction_free (tentative);
          goodix_enrollment_policy_commit_non_rollback (bad_continue_count);
          return GOODIX_MILAN_ENROLLMENT_RETRY_REMOVE;
        }
      result->reject_detail = goodix_enrollment_policy_commit_rollback (
        bad_record_count, bad_continue_count);
      goodix_milan_enrollment_transaction_free (*transaction);
      *transaction = tentative;
      return GOODIX_MILAN_ENROLLMENT_RETRY_CENTER;
    }

  goodix_enrollment_policy_commit_non_rollback (bad_continue_count);
  goodix_milan_enrollment_transaction_free (*transaction);
  *transaction = tentative;
  return GOODIX_MILAN_ENROLLMENT_ACCEPTED;
}

GBytes *
goodix_milan_enrollment_transaction_publish (
  const GoodixMilanEnrollmentTransaction *transaction)
{
  GBytes *feature_bytes[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT] = { 0 };
  const guint8 *elements[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  gsize element_sizes[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  GoodixMilanTemplateRelation relations[GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT];
  GoodixMilanTemplateMetadata metadata = { 0 };
  gsize relation_count = 0;
  gsize capacity = 1433;
  gsize packed_size = 0;
  gsize normalized_size = 0;
  guint8 *packed = NULL;
  guint8 *normalized = NULL;
  GBytes *result = NULL;

  if (!transaction || transaction->feature_count == 0 ||
      transaction->feature_count > GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT ||
      transaction->matrix.feature_count != transaction->feature_count)
    return NULL;
  for (guint i = 0; i < transaction->feature_count; i++)
    {
      feature_bytes[i] = goodix_enrollment_feature_pack (
        &transaction->features[i]);
      if (!feature_bytes[i])
        goto out;
      elements[i] = g_bytes_get_data (feature_bytes[i], &element_sizes[i]);
      if (element_sizes[i] > GOODIX_MILAN_TEMPLATE_MAX_SIZE - capacity)
        goto out;
      capacity += element_sizes[i];
    }
  if (goodix_milan_relation_matrix_project_reference_star (
        &transaction->matrix, relations, G_N_ELEMENTS (relations),
        &relation_count) != 0 ||
      relation_count > (GOODIX_MILAN_TEMPLATE_MAX_SIZE - capacity) / 45)
    goto out;
  capacity += relation_count * 45;
  packed = g_try_malloc (capacity);
  if (!packed)
    goto out;
  metadata.sensor_type = transaction->sensor_type;
  metadata.maximum_features = GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT;
  metadata.registration_count = transaction->registration_count;
  metadata.maximum_records = GOODIX_MILAN_ENROLLMENT_RECORD_CAPACITY;
  metadata.queue_state = transaction->queue_state;
  metadata.queue_transaction_counter = transaction->queue_transaction_counter;
  metadata.graph_reference_index =
    transaction->matrix.reference_feature_index == SIZE_MAX ?
    -1 : (gint32) transaction->matrix.reference_feature_index;
  metadata.graph_companion_f3 = transaction->graph_companion_f3;
  metadata.graph_companion_f4 = transaction->graph_companion_f4;
  metadata.graph_established = transaction->matrix.graph_established;
  if (goodix_milan_template_pack (
        elements, element_sizes, transaction->feature_count, relations,
        relation_count, &metadata, transaction->tail_state,
        sizeof (transaction->tail_state), packed, capacity, &packed_size) != 0)
    goto out;
  if (transaction->feature_count == GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT)
    {
      normalized = g_try_malloc (capacity);
      if (!normalized || goodix_milan_template_normalize (
            packed, packed_size, normalized, capacity, &normalized_size) != 0)
        goto out;
      result = g_bytes_new_take (normalized, normalized_size);
      normalized = NULL;
    }
  else
    {
      result = g_bytes_new_take (packed, packed_size);
      packed = NULL;
    }

out:
  for (guint i = 0; i < G_N_ELEMENTS (feature_bytes); i++)
    g_clear_pointer (&feature_bytes[i], g_bytes_unref);
  g_free (packed);
  g_free (normalized);
  return result;
}

#ifdef GOODIX53X5_DEBUG
gboolean
goodix_milan_enrollment_transaction_debug_state (
  const GoodixMilanEnrollmentTransaction *transaction,
  GoodixMilanEnrollmentDebugState        *state)
{
  if (!transaction || !state)
    return FALSE;
  memset (state, 0, sizeof (*state));
  state->feature_count = transaction->feature_count;
  state->registration_count = transaction->registration_count;
  state->graph_reference_index =
    transaction->matrix.reference_feature_index == SIZE_MAX ?
    -1 : (gint32) transaction->matrix.reference_feature_index;
  state->graph_companion_f3 = transaction->graph_companion_f3;
  state->graph_companion_f4 = transaction->graph_companion_f4;
  state->graph_established = transaction->matrix.graph_established;
  state->queue_state = transaction->queue_state;
  state->queue_transaction_counter = transaction->queue_transaction_counter;
  memcpy (state->order, transaction->tail_state, sizeof (state->order));
  for (guint i = 0; i < GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT; i++)
    {
      state->slot_populated[i] = transaction->features[i].populated;
      state->record_counts[i] = transaction->features[i].record_count;
      memcpy (state->feature_fields[i],
              transaction->features[i].fields.tagged_values,
              sizeof (state->feature_fields[i]));
      state->pair_scores[i] = goodix_milan_antifake_pair_score (
        &transaction->features[i].antifake);
      state->neighbor_counts[i] = transaction->features[i].neighbor_count;
    }
  memcpy (state->relations, transaction->matrix.slots,
          sizeof (state->relations));
  return TRUE;
}

gboolean
goodix_milan_enrollment_transaction_debug_provisional (
  const GoodixMilanEnrollmentTransaction *transaction,
  GBytes                                 *probe_template,
  GoodixMilanEnrollmentResult            *result,
  GoodixMilanEnrollmentDebugState        *state)
{
  g_autoptr(GoodixMilanEnrollmentTransaction) provisional = NULL;

  if (!transaction || !probe_template || !result || !state)
    return FALSE;
  memset (result, 0, sizeof (*result));
  result->pre_insertion_accepted_count = transaction->feature_count;
  provisional = goodix_enrollment_transaction_clone (transaction);
  return provisional && goodix_enrollment_transaction_insert (
    provisional, probe_template, result) &&
         goodix_milan_enrollment_transaction_debug_state (provisional, state);
}
#endif
