/*
 * Goodix 53x5 driver for libfprint - native Milan serialized match lifecycle
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
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
#include "milan/milan.h"
#include "milan/private.h"
#include "milan/print.h"
#include "milan/study/queue.h"

#include <string.h>

static int
milan_match_decode_imported_angles (
  const guint8                *source,
  gsize                        source_size,
  GoodixMilanUnpackedTemplate *unpacked,
  GoodixMatchInfo             *decoded[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY])
{
  g_autoptr(GBytes) identity = NULL;
  int changed = 0;

  for (size_t i = 0; i < unpacked->feature_count; i++)
    {
      GoodixMilanFeatureView view;
      gboolean has_negative = FALSE;

      if (goodix_milan_template_parse_feature_element (
            unpacked->feature_elements[i], unpacked->feature_element_sizes[i],
            &view) != 0 || view.record_count == 0 || view.record_count > 150 ||
          view.fields.tagged_values[2] < 0 ||
          (size_t) view.fields.tagged_values[2] > view.record_count)
        continue;
      /* The sign-magnitude byte 0x80 decodes to zero, not a negative word. */
      for (size_t j = 0; j < view.record_count; j++)
        has_negative |= view.packed_records[j * 32] > 0x80;
      if (!has_negative)
        continue;

      if (!identity)
        identity = g_bytes_new (source, source_size);
      decoded[i] = goodix_milan_match_info_new_empty ();
      /* Keep an immutable identity; gallery overrides consume only records/counts. */
      decoded[i]->template = g_bytes_ref (identity);
      decoded[i]->record_count = (int) view.record_count;
      decoded[i]->partition_count = view.fields.tagged_values[2];
      decoded[i]->records = g_new (GoodixMilanFeatureRecord, view.record_count);
      if (goodix_milan_feature_unpack_template_records (
            view.packed_records, view.record_count,
            (size_t) view.fields.tagged_values[2], decoded[i]->records,
            view.record_count) != 0)
        return -1;
      for (size_t j = 0; j < view.record_count; j++)
        if (decoded[i]->records[j].orientation < 0)
          decoded[i]->records[j].orientation =
            (int16_t) (decoded[i]->records[j].orientation + 0x3244);
      /* Native pack quantizes the adjusted angle; retained matching must still
       * use the exact live word rather than decode that lossy projection. */
      if (goodix_milan_feature_pack_template_records (
            decoded[i]->records, view.record_count,
            (guint8 *) view.packed_records, view.record_count * 32) != 0)
        return -1;
      changed++;
    }
  return changed;
}

gboolean
goodix_milan_match_queue_matches_template (const GoodixStudyQueue *queue,
                                     const guint8           *feature,
                                     gsize                   feature_len)
{
  GoodixMilanUnpackedTemplate *unpacked;
  gboolean matches = FALSE;

  if (!queue || !goodix_milan_study_queue_validate (queue))
    return FALSE;
  if (!feature || feature_len > GOODIX_MILAN_TEMPLATE_MAX_SIZE)
    return FALSE;
  unpacked = g_malloc (sizeof(*unpacked));
  if (goodix_milan_template_unpack (feature, feature_len, unpacked) == 0)
    matches = queue->enabled_state == unpacked->metadata.queue_state &&
              queue->transaction_counter ==
                unpacked->metadata.queue_transaction_counter;
  g_free (unpacked);
  return matches;
}

GoodixSigfmTemplateStatus
goodix_milan_match_serialized_feature_result_internal (
  GoodixMatchInfo             *probe_info,
  const guint8                *feature,
  gsize                        feature_len,
  GoodixMilanMatchResult      *match_result,
  GBytes                     **updated_feature,
#ifdef GOODIX53X5_DEBUG
  GoodixMilanMatchDiagnostics *diagnostics,
#endif
  GoodixStudyQueue            *queue,
  gboolean                     normalize,
  GoodixStudyQueueEnqueueResult (*enqueue_candidate) (
    GoodixStudyQueue      *queue,
    const GoodixMatchInfo *probe_info))
{
  guint8 *updated_milan = NULL;
  guint8 *normalized_milan = NULL;
  const guint8 *matched_milan;
  size_t normalized_milan_len = 0;
  size_t updated_milan_len = 0;
  GoodixMilanUnpackedTemplate *unpacked;
  gboolean retained_gallery;
  const GoodixMilanFeatureRecord *live_records[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  size_t live_record_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  size_t live_partition_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  GoodixMatchInfo *decoded_features[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  GBytes *private_update = NULL;
  GBytes *input_identity = NULL;
  const guint8 *requested_feature = feature;
  gsize requested_size = feature_len;
  gboolean publish_result = updated_feature != NULL;

  if (queue && !updated_feature)
    updated_feature = &private_update;
  if (updated_feature)
    *updated_feature = NULL;

  if (!probe_info || !probe_info->template || !feature || !match_result)
    goto invalid;
  if (queue && !goodix_milan_study_queue_validate (queue))
    goto invalid;
  retained_gallery = goodix_milan_study_queue_resolve_gallery (
    queue, &feature, &feature_len);
  gsize enrolled_milan_len = feature_len;
  const guint8 *enrolled_milan = feature;

  if (feature_len > GOODIX_MILAN_TEMPLATE_MAX_SIZE)
    goto invalid;
  if (queue && !goodix_milan_match_queue_matches_template (
        queue, feature, feature_len))
    goto invalid;
  if (queue && !publish_result)
    input_identity = g_bytes_new (requested_feature, requested_size);
  if (retained_gallery)
    {
      normalize = FALSE;
      for (size_t i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
        if (queue->live_features[i])
          {
            live_records[i] = queue->live_features[i]->records;
            live_record_counts[i] = queue->live_features[i]->record_count;
            live_partition_counts[i] = queue->live_features[i]->partition_count;
          }
    }
  normalized_milan = g_malloc (enrolled_milan_len);
  if (normalize)
    {
      if (goodix_milan_template_normalize (
            enrolled_milan, enrolled_milan_len, normalized_milan,
            enrolled_milan_len, &normalized_milan_len) != 0)
        {
          g_free (normalized_milan);
          goto invalid;
        }
    }
  else
    {
      memcpy (normalized_milan, enrolled_milan, enrolled_milan_len);
      normalized_milan_len = enrolled_milan_len;
    }
  if (normalized_milan_len != enrolled_milan_len)
    {
      g_free (normalized_milan);
      goto invalid;
    }
  unpacked = g_malloc (sizeof (*unpacked));
  if (goodix_milan_template_unpack (
        normalized_milan, normalized_milan_len, unpacked) == 0 &&
      unpacked->metadata.sensor_type == GOODIX_MILAN_PRINT_SENSOR_TYPE)
    {
      for (size_t i = 0; i < unpacked->feature_count; i++)
        {
          GoodixMilanFeatureView view;

          if (goodix_milan_template_parse_feature_element (
                unpacked->feature_elements[i], unpacked->feature_element_sizes[i],
                &view) != 0)
            continue;
          /* Ordinary native dispatch resets every gallery owner before traversal,
           * including retained galleries and rows that matching later skips. */
          if (view.fields.tagged_values[5] == 5)
            {
              if (goodix_milan_template_patch_feature_scalar (
                    (guint8 *) unpacked->feature_elements[i],
                    unpacked->feature_element_sizes[i], 0xba, 0) != 0)
                {
                  g_free (unpacked);
                  g_free (updated_milan);
                  g_free (normalized_milan);
                  goto invalid;
                }
              if (!updated_milan)
                updated_milan = g_malloc (normalized_milan_len);
            }
          if (!retained_gallery && view.record_count > 0 &&
              view.record_count <= 150 &&
              view.fields.tagged_values[2] == (int32_t) view.record_count)
            {
              /* Unpack borrows our writable gallery copy, never the probe. */
              if (goodix_milan_template_patch_feature_scalar (
                    (guint8 *) unpacked->feature_elements[i],
                    unpacked->feature_element_sizes[i], 0xb7,
                    (int32_t) view.record_count - 1) != 0)
                {
                  g_free (unpacked);
                  g_free (updated_milan);
                  g_free (normalized_milan);
                  goto invalid;
                }
              if (!updated_milan)
                updated_milan = g_malloc (normalized_milan_len);
            }
        }
      if (!retained_gallery)
        {
          int changed = milan_match_decode_imported_angles (
            enrolled_milan, enrolled_milan_len, unpacked, decoded_features);

          if (changed < 0)
            {
              g_free (unpacked);
              g_free (updated_milan);
              g_free (normalized_milan);
              goto invalid;
            }
          if (changed && !updated_milan)
            updated_milan = g_malloc (normalized_milan_len);
          for (size_t i = 0; i < unpacked->feature_count; i++)
            if (decoded_features[i])
              {
                live_records[i] = decoded_features[i]->records;
                live_record_counts[i] = decoded_features[i]->record_count;
                live_partition_counts[i] = decoded_features[i]->partition_count;
              }
        }
    }
  if (updated_milan)
    {
      if (goodix_milan_template_pack (
            unpacked->feature_elements, unpacked->feature_element_sizes,
            unpacked->feature_count, unpacked->relations, unpacked->relation_count,
            &unpacked->metadata, unpacked->tail_state, sizeof (unpacked->tail_state),
            updated_milan, normalized_milan_len, &updated_milan_len) != 0 ||
          updated_milan_len != normalized_milan_len)
        {
          g_free (unpacked);
          g_free (updated_milan);
          g_free (normalized_milan);
          goto invalid;
        }
      g_free (normalized_milan);
      normalized_milan = g_steal_pointer (&updated_milan);
    }
  g_free (unpacked);
  matched_milan = normalized_milan;

  if (goodix_milan_match_info_result (
        probe_info, matched_milan, normalized_milan_len,
        live_records, live_record_counts, live_partition_counts,
        SIZE_MAX, match_result
#ifdef GOODIX53X5_DEBUG
        , diagnostics
#endif
        ) != 0)
    {
      g_free (normalized_milan);
      goto invalid;
    }

  if (updated_feature)
    {
      if (match_result->lifecycle_update_feature_mask != 0)
        {
          updated_milan = g_malloc (normalized_milan_len);
          if (goodix_milan_template_update_match_lifecycle (
                matched_milan, normalized_milan_len,
                match_result->lifecycle_update_feature_mask,
                match_result->direct_positive_feature_mask == 0, updated_milan,
                normalized_milan_len, &updated_milan_len) != 0 ||
              updated_milan_len != normalized_milan_len)
            {
              g_free (updated_milan);
              g_free (normalized_milan);
              goto invalid;
            }
          normalized_milan_len = updated_milan_len;
        }
      if (updated_milan)
        {
          *updated_feature = g_bytes_new_take (updated_milan,
                                               normalized_milan_len);
          updated_milan = NULL;
        }
      else
        {
          *updated_feature = g_bytes_new_take (normalized_milan,
                                               normalized_milan_len);
          normalized_milan = NULL;
        }
    }
  if (queue && match_result->study_control.queue_candidate_eligible)
    {
      GoodixStudyQueueEnqueueResult enqueue_result =
        enqueue_candidate (queue, probe_info);

      if (enqueue_result == GOODIX_STUDY_QUEUE_INVALID)
        {
          if (updated_feature)
            g_clear_pointer (updated_feature, g_bytes_unref);
          g_free (normalized_milan);
          goto invalid;
        }
    }
  g_free (normalized_milan);

  if (queue)
    {
      if (!retained_gallery)
        {
          goodix_milan_study_queue_clear_gallery (queue);
          for (size_t i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
            queue->live_features[i] = g_steal_pointer (&decoded_features[i]);
        }
      g_clear_pointer (&queue->live_gallery, g_bytes_unref);
      g_clear_pointer (&queue->live_input, g_bytes_unref);
      queue->live_input = g_steal_pointer (&input_identity);
      if (updated_feature && *updated_feature)
        queue->live_gallery = g_bytes_ref (*updated_feature);
    }

  for (size_t i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    goodix_milan_match_free_info (decoded_features[i]);
  g_clear_pointer (&private_update, g_bytes_unref);
  return GOODIX_SIGFM_TEMPLATE_OK;

invalid:
  for (size_t i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    goodix_milan_match_free_info (decoded_features[i]);
  g_clear_pointer (&input_identity, g_bytes_unref);
  g_clear_pointer (&private_update, g_bytes_unref);
  goodix_milan_study_queue_clear_gallery (queue);
  return GOODIX_SIGFM_TEMPLATE_INVALID;
}
