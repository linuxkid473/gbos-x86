/*
 * Goodix 53x5 driver for libfprint - Milan study orchestration
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/milan.h"
#include "milan/print.h"
#include "milan/private.h"
#include "milan/template/codec-private.h"
#include "milan/template/normalization.h"
#include "milan/study/order.h"
#include "milan/study/policy.h"
#include "milan/relations.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline int32_t
milan_study_u32_as_s32 (uint32_t value)
{
  if (value <= INT32_MAX)
    return (int32_t) value;
  return -1 - (int32_t) (UINT32_MAX - value);
}

static inline int32_t
milan_study_wrap_increment (int32_t value)
{
  return milan_study_u32_as_s32 ((uint32_t) value + 1U);
}

static inline int32_t
milan_study_wrap_decrement (int32_t value)
{
  return milan_study_u32_as_s32 ((uint32_t) value - 1U);
}

int
goodix_milan_study_order_key_greater (const GoodixMilanStudyOrderKey *left,
                                      const GoodixMilanStudyOrderKey *right)
{
  if (left->active != right->active)
    return left->active > right->active;
  if (left->generation != right->generation)
    return left->generation > right->generation;
  return left->ordinal > right->ordinal;
}

void
goodix_milan_study_order_sort (uint32_t                       *order,
                               size_t                          feature_count,
                               const GoodixMilanStudyOrderKey *keys)
{
  for (size_t position = 0; position < feature_count; position++)
    {
      size_t selected = position;

      for (size_t candidate = position + 1;
           candidate < feature_count; candidate++)
        if (goodix_milan_study_order_key_greater (
              &keys[order[candidate]], &keys[order[selected]]))
          selected = candidate;

      if (selected != position)
        {
          uint32_t value = order[position];

          order[position] = order[selected];
          order[selected] = value;
        }
    }
}

static uint8_t *
milan_copy_study_append_feature (const uint8_t *feature_element,
                                 size_t         feature_element_size,
                                 size_t        *copied_size)
{
  size_t size;
  uint8_t *copy;

  if (!feature_element || !copied_size || feature_element_size < 5 ||
      feature_element[0] != 0x95 ||
      goodix_milan_template_read_u32 (feature_element + 1) != feature_element_size - 5)
    return NULL;

  size = feature_element_size;
  if (size >= 5 && feature_element[size - 5] == 0xc7)
    size -= 5;
  copy = malloc (size);
  if (!copy)
    return NULL;
  memcpy (copy, feature_element, size);
  goodix_milan_template_write_u32 (copy + 1, (uint32_t) (size - 5));
  *copied_size = size;
  return copy;
}

static void
milan_copy_study_antifake_scalars (GoodixMilanAntifakeBlob       *destination,
                                    const GoodixMilanAntifakeBlob *source)
{
  int32_t value;

  value = goodix_milan_antifake_texture (source);
  goodix_milan_antifake_set_texture (destination, value);
  value = goodix_milan_antifake_mean (source);
  goodix_milan_antifake_set_mean (destination, value);
  value = goodix_milan_antifake_threshold (source);
  goodix_milan_antifake_set_threshold (destination, value);
  value = goodix_milan_antifake_pair_score (source);
  goodix_milan_antifake_set_pair_score (destination, value);
}

static int
milan_study_build_relation_matrix (
  const GoodixMilanUnpackedTemplate *current,
  GoodixMilanRelationMatrix    *matrix)
{
  int32_t row_bases[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };

  if (!current || !matrix ||
      current->metadata.sensor_type != GOODIX_MILAN_PRINT_SENSOR_TYPE)
    return -1;
  for (size_t i = 0; i < current->feature_count; i++)
    if (goodix_milan_template_read_feature_scalar (
          current->feature_elements[i], current->feature_element_sizes[i],
          0xb6, &row_bases[i]) != 0)
      return -1;
  return goodix_milan_relation_matrix_init (
    matrix, current->feature_count, row_bases,
    current->metadata.registration_count,
    current->metadata.graph_reference_index,
    (int32_t) current->metadata.graph_established, current->relations,
    current->relation_count);
}

static int
milan_study_refresh_retained (
  GoodixMilanUnpackedTemplate    *current,
  GoodixMilanRelationMatrix *matrix,
  const int32_t                  *feature_indices,
  const int32_t                   transforms[][6],
  size_t                          feature_count,
  int32_t                         retained_flag,
  const int32_t                   relation_transform[6],
  uint8_t                        *feature_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY])
{
  uint64_t seen = 0;

  if (!current || !matrix || !relation_transform || !feature_copies ||
      feature_count > GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY ||
      (feature_count != 0 && (!feature_indices || !transforms)))
    return -1;
  for (size_t i = 0; i < feature_count; i++)
    {
      int32_t feature_index = feature_indices[i];

      if (feature_index < 0 ||
          (size_t) feature_index >= current->feature_count ||
          (seen & (UINT64_C (1) << feature_index)))
        return -1;
      seen |= UINT64_C (1) << feature_index;
    }
  if (feature_count == 0 || retained_flag != 1)
    return 0;

  for (size_t i = 0; i < feature_count; i++)
    {
      size_t feature_index = (size_t) feature_indices[i];

      if (!feature_copies[feature_index])
        {
          feature_copies[feature_index] = malloc (
            current->feature_element_sizes[feature_index]);
          if (!feature_copies[feature_index])
            return -1;
          memcpy (feature_copies[feature_index],
                  current->feature_elements[feature_index],
                  current->feature_element_sizes[feature_index]);
          current->feature_elements[feature_index] =
            feature_copies[feature_index];
        }
      if (goodix_milan_template_patch_feature_scalar (
            feature_copies[feature_index],
            current->feature_element_sizes[feature_index], 0xb5, 1) != 0 ||
          goodix_milan_relation_matrix_refresh (
            matrix, feature_index, transforms[i], relation_transform) != 0)
        return -1;
    }
  return 0;
}

static int
milan_study_project_relations (GoodixMilanUnpackedTemplate       *current,
                               const GoodixMilanRelationMatrix *matrix)
{
  return goodix_milan_relation_matrix_project_reference_star (
    matrix, current->relations, GOODIX_MILAN_TEMPLATE_RELATION_CAPACITY,
    &current->relation_count);
}

static int
milan_sort_legacy_template_order (GoodixMilanUnpackedTemplate *unpacked)
{
  int32_t order_keys[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  int32_t order_ordinals[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  uint8_t seen[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };

  for (size_t i = 0; i < unpacked->feature_count; i++)
    {
      uint32_t feature_index = goodix_milan_template_read_u32 (unpacked->tail_state + i * 4);

      if (feature_index >= unpacked->feature_count || seen[feature_index] ||
          goodix_milan_template_read_feature_scalar (
            unpacked->feature_elements[feature_index],
            unpacked->feature_element_sizes[feature_index], 0xbe,
            &order_keys[i]) != 0 || goodix_milan_template_read_feature_scalar (
            unpacked->feature_elements[feature_index],
            unpacked->feature_element_sizes[feature_index], 0xbc,
            &order_ordinals[i]) != 0)
        return -1;
      seen[feature_index] = 1;
    }
  for (size_t i = 1; i < unpacked->feature_count; i++)
    {
      uint32_t feature_index = goodix_milan_template_read_u32 (unpacked->tail_state + i * 4);
      int32_t key = order_keys[i];
      int32_t ordinal = order_ordinals[i];
      size_t insertion = i;

      while (insertion > 0)
        {
          if (order_keys[insertion - 1] < key ||
              (order_keys[insertion - 1] == key &&
               order_ordinals[insertion - 1] < ordinal))
            break;
          order_keys[insertion] = order_keys[insertion - 1];
          order_ordinals[insertion] = order_ordinals[insertion - 1];
          memcpy (unpacked->tail_state + insertion * 4,
                  unpacked->tail_state + (insertion - 1) * 4, 4);
          insertion--;
        }
      order_keys[insertion] = key;
      order_ordinals[insertion] = ordinal;
      goodix_milan_template_write_u32 (unpacked->tail_state + insertion * 4, feature_index);
    }
  return 0;
}

int
goodix_milan_template_sort_type12_order (GoodixMilanUnpackedTemplate *unpacked)
{
  GoodixMilanStudyOrderKey keys[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  uint32_t order[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  uint8_t seen[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };

  for (size_t i = 0; i < unpacked->feature_count; i++)
    {
      uint32_t feature_index = goodix_milan_template_read_u32 (unpacked->tail_state + i * 4);

      if (feature_index >= unpacked->feature_count || seen[feature_index] ||
          goodix_milan_template_read_feature_scalar (
            unpacked->feature_elements[feature_index],
            unpacked->feature_element_sizes[feature_index], 0xb5,
            &keys[feature_index].active) != 0 ||
          goodix_milan_template_read_feature_scalar (
            unpacked->feature_elements[feature_index],
            unpacked->feature_element_sizes[feature_index], 0xbe,
            &keys[feature_index].generation) != 0 ||
          goodix_milan_template_read_feature_scalar (
            unpacked->feature_elements[feature_index],
            unpacked->feature_element_sizes[feature_index], 0xbc,
            &keys[feature_index].ordinal) != 0)
        return -1;
      order[i] = feature_index;
      seen[feature_index] = 1;
    }

  goodix_milan_study_order_sort (order, unpacked->feature_count, keys);
  for (size_t i = 0; i < unpacked->feature_count; i++)
    goodix_milan_template_write_u32 (unpacked->tail_state + i * 4, order[i]);
  return 0;
}

static int
milan_finalize_template_study_order (GoodixMilanUnpackedTemplate *unpacked)
{
  if (unpacked->metadata.sensor_type == GOODIX_MILAN_PRINT_SENSOR_TYPE)
    return goodix_milan_template_sort_type12_order (unpacked);
  return milan_sort_legacy_template_order (unpacked);
}

static int
milan_capture_study_transient (
  const GoodixMilanUnpackedTemplate *unpacked,
  GoodixMilanStudyTransientState    *state)
{
  uint8_t seen[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };

  if (!unpacked || !state ||
      unpacked->metadata.sensor_type != GOODIX_MILAN_PRINT_SENSOR_TYPE ||
      unpacked->feature_count > GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY)
    return -1;
  memset (state, 0, sizeof(*state));
  state->feature_count = unpacked->feature_count;
  state->transaction_tail = goodix_milan_template_read_u32 (unpacked->tail_state + 0x50c);
  for (size_t i = 0; i < unpacked->feature_count; i++)
    {
      uint32_t feature_index = goodix_milan_template_read_u32 (unpacked->tail_state + i * 4);

      if (feature_index >= unpacked->feature_count || seen[feature_index] ||
          goodix_milan_template_read_feature_scalar (
            unpacked->feature_elements[feature_index],
            unpacked->feature_element_sizes[feature_index], 0xb5,
            &state->active[feature_index]) != 0 ||
          goodix_milan_template_read_feature_scalar (
            unpacked->feature_elements[feature_index],
            unpacked->feature_element_sizes[feature_index], 0xbe,
            &state->generation[feature_index]) != 0 ||
          goodix_milan_template_read_feature_scalar (
            unpacked->feature_elements[feature_index],
            unpacked->feature_element_sizes[feature_index], 0xbc,
            &state->ordinal[feature_index]) != 0)
        return -1;
      state->order[i] = feature_index;
      seen[feature_index] = 1;
    }
  state->valid = 1;
  return 0;
}

int
goodix_milan_study_finalize_action0_transient (
  GoodixMilanStudyTransientState *state)
{
  GoodixMilanStudyOrderKey keys[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];

  if (!state || !state->valid || state->finalized ||
      state->feature_count > GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY)
    return -1;
  for (size_t i = 0; i < state->feature_count; i++)
    {
      if (state->order[i] >= state->feature_count)
        return -1;
      keys[i].active = state->active[i];
      keys[i].generation = state->generation[i];
      keys[i].ordinal = state->ordinal[i];
    }
  goodix_milan_study_order_sort (state->order, state->feature_count, keys);
  state->transaction_tail++;
  state->finalized = 1;
  return 0;
}

static int
milan_study_action0_state (
  const uint8_t *current_template,
  size_t         current_template_size,
  const int32_t  relation_values[7],
  const int32_t *retained_feature_indices,
  const int32_t  retained_transforms[][6],
  size_t         retained_count,
  int32_t        retained_flag,
  GoodixMilanStudyTransientState *transient_state,
  int32_t                        *live_overlap_counts,
  uint8_t                        *packed,
  size_t                          packed_capacity,
  size_t                         *packed_size)
{
  GoodixMilanUnpackedTemplate *current = NULL;
  GoodixMilanRelationMatrix *relation_matrix = NULL;
  uint8_t *feature_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  uint8_t *normalization_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  int result = -1;

  if (!current_template || !relation_values || (!transient_state && !packed))
    return -1;
  current = malloc (sizeof(*current));
  if (!current || goodix_milan_template_unpack (
        current_template, current_template_size, current) != 0 ||
      current->metadata.sensor_type != GOODIX_MILAN_PRINT_SENSOR_TYPE)
    goto out;
  if (retained_count > 0 && retained_flag == 1)
    {
      relation_matrix = malloc (sizeof(*relation_matrix));
      if (!relation_matrix ||
          milan_study_build_relation_matrix (current, relation_matrix) != 0 ||
          milan_study_refresh_retained (
         current, relation_matrix, retained_feature_indices,
         retained_transforms, retained_count, retained_flag,
          relation_values + 1, feature_copies) != 0 ||
          milan_study_project_relations (current, relation_matrix) != 0 ||
          (current->feature_count == current->metadata.maximum_features &&
           goodix_milan_template_normalize_unpacked (
             current, normalization_copies,
             live_overlap_counts ? live_overlap_counts :
             current->normalization_overlap_counts) != 0))
        goto out;
    }
  result = transient_state ? milan_capture_study_transient (current, transient_state) : 0;
  if (result == 0 && packed)
    {
      result = goodix_milan_template_pack (
        current->feature_elements, current->feature_element_sizes,
        current->feature_count, current->relations, current->relation_count,
        &current->metadata, current->tail_state, sizeof (current->tail_state),
        packed, packed_capacity, packed_size);
    }

out:
  for (size_t i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    {
      free (normalization_copies[i]);
      free (feature_copies[i]);
    }
  free (relation_matrix);
  free (current);
  return result;
}

int
goodix_milan_study_action0_transient (
  const uint8_t                  *current_template,
  size_t                          current_template_size,
  const int32_t                   relation_values[7],
  const int32_t                  *retained_feature_indices,
  const int32_t                   retained_transforms[][6],
  size_t                          retained_count,
  int32_t                         retained_flag,
  GoodixMilanStudyTransientState *transient_state)
{
  return milan_study_action0_state (
    current_template, current_template_size, relation_values,
    retained_feature_indices, retained_transforms, retained_count, retained_flag,
    transient_state, NULL, NULL, 0, NULL);
}

int
goodix_milan_study_action0_gallery (
  const uint8_t *current_template,
  size_t         current_template_size,
  const int32_t  relation_values[7],
  const int32_t *retained_feature_indices,
  const int32_t  retained_transforms[][6],
  size_t         retained_count,
  int32_t        retained_flag,
  int32_t        live_overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  uint8_t       *packed,
  size_t         packed_capacity,
  size_t        *packed_size)
{
  if (!live_overlap_counts || !packed || !packed_size)
    return -1;
  return milan_study_action0_state (
    current_template, current_template_size, relation_values,
    retained_feature_indices, retained_transforms, retained_count, retained_flag,
    NULL, live_overlap_counts, packed, packed_capacity, packed_size);
}

int
goodix_milan_study_append (
  const uint8_t *current_template,
  size_t         current_template_size,
  const uint8_t *probe_template,
  size_t         probe_template_size,
  size_t         matched_feature_index,
  const int32_t  relation_values[7],
  const int32_t *retained_feature_indices,
  const int32_t  retained_transforms[][6],
  size_t         retained_count,
  int32_t        retained_flag,
  int            apply_dispatcher_prepass,
  int            complete_dispatcher_transaction,
  int            finalize_study,
  int32_t        live_overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  uint8_t       *packed,
  size_t         packed_capacity,
  size_t        *packed_size)
{
  GoodixMilanUnpackedTemplate *current = NULL;
  GoodixMilanUnpackedTemplate *probe = NULL;
  GoodixMilanRelationMatrix *relation_matrix = NULL;
  uint8_t *feature_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  uint8_t *normalization_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  uint8_t *new_feature = NULL;
  GoodixMilanFeatureView matched_feature_view;
  GoodixMilanAntifakeBlob *new_antifake;
  int graph_was_established;
  int result = -1;

  if (!current_template || !probe_template || !relation_values ||
      !live_overlap_counts || !packed || !packed_size)
    return -1;
  current = malloc (sizeof(*current));
  probe = malloc (sizeof(*probe));
  relation_matrix = malloc (sizeof(*relation_matrix));
  if (!current || !probe || !relation_matrix ||
      goodix_milan_template_unpack (
        current_template, current_template_size, current) != 0 ||
      goodix_milan_template_unpack (
        probe_template, probe_template_size, probe) != 0 ||
      probe->feature_count != 1 || probe->relation_count != 0 ||
      current->feature_count >= current->metadata.maximum_features ||
      current->feature_count >= GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY ||
      relation_values[0] != 0 ||
      matched_feature_index >= current->feature_count ||
      current->metadata.registration_count >
        UINT32_MAX - current->feature_count ||
      current->feature_count > INT32_MAX ||
      current->metadata.registration_count > INT32_MAX ||
      milan_study_build_relation_matrix (current, relation_matrix) != 0)
    goto out;
  graph_was_established = relation_matrix->graph_established;
  if (apply_dispatcher_prepass && retained_count > 0 && retained_flag == 1 &&
      milan_study_refresh_retained (
        current, relation_matrix, retained_feature_indices,
        retained_transforms, retained_count, retained_flag,
        relation_values + 1, feature_copies) != 0)
    goto out;

  size_t new_feature_size;
  const uint8_t *matched_feature =
    current->feature_elements[matched_feature_index];
  const size_t matched_feature_size =
    current->feature_element_sizes[matched_feature_index];
  int32_t matched_study_count;
  int32_t matched_active;

  new_feature = milan_copy_study_append_feature (
    probe->feature_elements[0], probe->feature_element_sizes[0],
    &new_feature_size);
  new_antifake = new_feature
                   ? goodix_milan_template_mutable_feature_antifake (
                       new_feature, new_feature_size)
                   : NULL;
  if (!new_feature || !new_antifake ||
      goodix_milan_template_parse_feature_element (
        matched_feature, matched_feature_size, &matched_feature_view) != 0 ||
      goodix_milan_template_read_feature_scalar (
        matched_feature, matched_feature_size, 0xbe,
        &matched_study_count) != 0 ||
      goodix_milan_template_read_feature_scalar (
        matched_feature, matched_feature_size, 0xb5,
        &matched_active) != 0)
    goto out;
  milan_copy_study_antifake_scalars (new_antifake,
                                      matched_feature_view.antifake);

  const struct
  {
    uint8_t tag;
    int32_t value;
  } scalar_patches[] = {
    { 0xb5, matched_active },
    { 0xb6, (int32_t) current->metadata.registration_count },
    { 0xba, 1 },
    { 0xbb, 0 },
    { 0xbc, (int32_t) current->feature_count },
    { 0xbd, 0 },
    { 0xbe, matched_study_count },
  };
  for (size_t i = 0; i < sizeof(scalar_patches) / sizeof(scalar_patches[0]);
       i++)
    if (goodix_milan_template_patch_feature_scalar (
          new_feature, new_feature_size, scalar_patches[i].tag,
          scalar_patches[i].value) != 0)
      goto out;

  const size_t old_feature_count = current->feature_count;
  if (goodix_milan_relation_matrix_append_row (
        relation_matrix, (int32_t) current->metadata.registration_count) != 0)
    goto out;
  current->feature_elements[current->feature_count] = new_feature;
  current->feature_element_sizes[current->feature_count] = new_feature_size;
  current->feature_count++;
  live_overlap_counts[old_feature_count] = 0;
  current->metadata.registration_count += (uint32_t) old_feature_count;

  if (!graph_was_established)
    {
      if (!feature_copies[matched_feature_index])
        {
          feature_copies[matched_feature_index] = malloc (matched_feature_size);
          if (!feature_copies[matched_feature_index])
            goto out;
          memcpy (feature_copies[matched_feature_index], matched_feature,
                  matched_feature_size);
          current->feature_elements[matched_feature_index] =
            feature_copies[matched_feature_index];
        }
      if (goodix_milan_template_patch_feature_scalar (
            feature_copies[matched_feature_index], matched_feature_size,
            0xb5, 1) != 0 ||
          goodix_milan_template_patch_feature_scalar (
            new_feature, new_feature_size, 0xb5, 1) != 0)
        goto out;
      relation_matrix->reference_feature_index = matched_feature_index;
      relation_matrix->graph_established = 1;
      current->metadata.graph_reference_index = (int32_t) matched_feature_index;
      current->metadata.graph_established = 1;
    }
  if ((!graph_was_established || matched_active == 1) &&
      goodix_milan_relation_matrix_store_reference (
        relation_matrix, old_feature_count, relation_values + 1) != 0)
    goto out;
  if (!graph_was_established && retained_count != 0 &&
      milan_study_refresh_retained (
        current, relation_matrix, retained_feature_indices,
        retained_transforms, retained_count, 1, relation_values + 1,
        feature_copies) != 0)
    goto out;

  if (complete_dispatcher_transaction &&
      current->feature_count == current->metadata.maximum_features)
    current->metadata.queue_state = 1;

  goodix_milan_template_write_u32 (current->tail_state + old_feature_count * 4,
                   (uint32_t) old_feature_count);
  if ((finalize_study && milan_finalize_template_study_order (current) != 0) ||
      milan_study_project_relations (current, relation_matrix) != 0 ||
      (current->metadata.sensor_type == GOODIX_MILAN_PRINT_SENSOR_TYPE &&
       current->feature_count == current->metadata.maximum_features &&
       goodix_milan_template_normalize_unpacked (
          current, normalization_copies, live_overlap_counts) != 0))
    goto out;

  uint32_t append_count = goodix_milan_template_read_u32 (current->tail_state + 0x50c);
  uint32_t study_count = goodix_milan_template_read_u32 (current->tail_state + 0x514);
  if (finalize_study)
    goodix_milan_template_write_u32 (current->tail_state + 0x50c, append_count + 1);
  goodix_milan_template_write_u32 (current->tail_state + 0x514, study_count + 1);

  result = goodix_milan_template_pack (
    current->feature_elements, current->feature_element_sizes,
    current->feature_count, current->relations, current->relation_count,
    &current->metadata, current->tail_state, sizeof(current->tail_state),
    packed, packed_capacity, packed_size);

out:
  for (size_t i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    {
      free (normalization_copies[i]);
      free (feature_copies[i]);
    }
  free (new_feature);
  free (relation_matrix);
  free (probe);
  free (current);
  return result;
}

static int
milan_study_policy_derive (
  GoodixMilanUnpackedTemplate *current,
  const GoodixMilanUnpackedTemplate *probe,
  size_t                       matched_feature_index,
  int32_t                      retained_flag,
  int32_t                      probe_quality,
  int32_t                      probe_coverage,
  const int32_t                primary_transform[6],
  const int32_t                relation_transform[6],
  const int32_t                live_overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  GoodixMilanStudyPolicyInput *input)
{
  GoodixMilanFeatureView probe_view;
  int32_t reference_to_features[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY][6];

  if (!current || !probe || !primary_transform || !relation_transform ||
      !live_overlap_counts || !input || current->feature_count == 0 ||
      current->feature_count > GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY ||
      probe->feature_count != 1 ||
      current->metadata.graph_established != 1 ||
      current->metadata.graph_reference_index < 0 ||
      (size_t) current->metadata.graph_reference_index >=
        current->feature_count ||
      matched_feature_index >= current->feature_count ||
      goodix_milan_template_parse_feature_element (
        probe->feature_elements[0], probe->feature_element_sizes[0],
        &probe_view) != 0)
    return -1;

  memset (input, 0, sizeof(*input));
  input->action_gate = 1;
  input->mode_enabled = 1;
  input->replacement_enabled = 1;
  input->probe_quality = probe_quality;
  input->probe_coverage = probe_coverage;
  input->feature_count = current->feature_count;
  input->maximum_features = current->metadata.maximum_features;
  input->matched_feature_index = matched_feature_index;
  input->reference_feature_index =
    (size_t) current->metadata.graph_reference_index;
  input->retained_flag = retained_flag;
  input->template_counter = (int32_t) goodix_milan_template_read_u32 (
    current->tail_state + 0x510);

  input->primary_transform_area =
    goodix_milan_study_policy_full_footprint_area (primary_transform);

  for (size_t i = 0; i < current->feature_count; i++)
    {
      GoodixMilanStudyPolicyFeature *feature = &input->features[i];

      if (goodix_milan_template_read_feature_scalar (
            current->feature_elements[i], current->feature_element_sizes[i],
            0xb5, &feature->active) != 0 ||
          goodix_milan_template_read_feature_scalar (
            current->feature_elements[i], current->feature_element_sizes[i],
            0xb8, &feature->quality) != 0 ||
          goodix_milan_template_read_feature_scalar (
            current->feature_elements[i], current->feature_element_sizes[i],
            0xb9, &feature->coverage) != 0 ||
          goodix_milan_template_read_feature_scalar (
            current->feature_elements[i], current->feature_element_sizes[i],
            0xba, &feature->state) != 0 ||
          goodix_milan_template_read_feature_scalar (
            current->feature_elements[i], current->feature_element_sizes[i],
            0xbb, &feature->residual) != 0 ||
          goodix_milan_template_reference_transform (
            current, i, 1, reference_to_features[i]) != 0)
        return -1;
      feature->overlap_count = live_overlap_counts[i];
    }

  /* Policy selects the matched zero-residual slot before consuming any of the
   * geometry below. Keep all scalar and reference-transform validation above. */
  if (input->features[matched_feature_index].residual == 0)
    return 0;

  for (size_t i = 0; i < current->feature_count; i++)
    {
      GoodixMilanStudyPolicyFeature *feature = &input->features[i];
      uint8_t probe_residual[GOODIX_MILAN_STUDY_MASK_SIZE];
      int32_t candidate_transform[6];

      goodix_milan_study_policy_expand_mask (
        probe_view.inline_mask, probe_residual);
      for (size_t other = 0; other < current->feature_count; other++)
        {
          int32_t footprint[6];

          if (other == i || input->features[other].active == 0 ||
              input->features[other].state == 5)
            continue;
          goodix_milan_transform_compose (
            reference_to_features[other], relation_transform, footprint);
          footprint[2] >>= 1;
          footprint[5] >>= 1;
          goodix_milan_study_policy_remove_footprint (
            probe_residual, footprint);
        }
      for (size_t pixel = 0; pixel < sizeof(probe_residual); pixel++)
        feature->uncovered_probe_residual += probe_residual[pixel] != 0;
      if (feature->uncovered_probe_residual < 20)
        feature->uncovered_probe_residual = 0;

      goodix_milan_transform_compose (
        reference_to_features[i], relation_transform, candidate_transform);
      candidate_transform[2] >>= 1;
      candidate_transform[5] >>= 1;
      feature->geometric_overlap_area =
        goodix_milan_study_policy_footprint_area (candidate_transform);
      feature->geometric_overlap_percent = feature->geometric_overlap_area * 100 /
                                           GOODIX_MILAN_STUDY_MASK_SIZE;

    }
  return 0;
}

int
goodix_milan_study_replace (
  const uint8_t *current_template,
  size_t         current_template_size,
  const uint8_t *probe_template,
  size_t         probe_template_size,
  size_t         matched_feature_index,
  const int32_t  relation_values[7],
  const int32_t *retained_feature_indices,
  const int32_t  retained_transforms[][6],
  size_t         retained_count,
  int32_t        retained_flag,
  int            apply_dispatcher_prepass,
  int32_t        probe_quality,
  int32_t        probe_coverage,
  const int32_t  primary_transform[6],
  int            complete_dispatcher_transaction,
  int            finalize_study,
  int32_t        live_overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  uint8_t       *packed,
  size_t         packed_capacity,
  size_t        *packed_size,
  int32_t       *action_code,
  size_t        *selected_feature_index,
  GoodixMilanStudyTransientState *transient_state)
{
  GoodixMilanUnpackedTemplate *current = NULL;
  GoodixMilanUnpackedTemplate *probe = NULL;
  GoodixMilanRelationMatrix *relation_matrix = NULL;
  uint8_t *feature_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  uint8_t *pre_normalization_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  uint8_t *post_normalization_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  uint8_t *feature_storage = NULL;
  GoodixMilanStudyPolicyInput policy_input;
  GoodixMilanStudyPolicyResult policy_result;
  size_t replacement_index = SIZE_MAX;
  int result = -1;

  if (!current_template || !probe_template || !relation_values ||
      !live_overlap_counts || !packed || !packed_size || !action_code ||
      !primary_transform)
    return -1;
  *packed_size = 0;
  *action_code = GOODIX_MILAN_STUDY_ACTION_NONE;
  if (selected_feature_index)
    *selected_feature_index = SIZE_MAX;
  current = malloc (sizeof(*current));
  probe = malloc (sizeof(*probe));
  relation_matrix = malloc (sizeof(*relation_matrix));
  if (!current || !probe || !relation_matrix ||
      goodix_milan_template_unpack (
        current_template, current_template_size, current) != 0 ||
      goodix_milan_template_unpack (
        probe_template, probe_template_size, probe) != 0 ||
      probe->feature_count != 1 || probe->relation_count != 0 ||
      current->feature_count != current->metadata.maximum_features ||
      relation_values[0] != 0 ||
      matched_feature_index >= current->feature_count ||
      milan_study_build_relation_matrix (current, relation_matrix) != 0)
    goto out;
  if (apply_dispatcher_prepass && retained_count > 0 && retained_flag == 1 &&
      (milan_study_refresh_retained (
         current, relation_matrix, retained_feature_indices,
         retained_transforms, retained_count, retained_flag,
         relation_values + 1, feature_copies) != 0 ||
       milan_study_project_relations (current, relation_matrix) != 0 ||
       (current->feature_count == current->metadata.maximum_features &&
         goodix_milan_template_normalize_unpacked (
           current, pre_normalization_copies, live_overlap_counts) != 0)))
    goto out;
  if (milan_study_policy_derive (
        current, probe, matched_feature_index, retained_flag, probe_quality,
        probe_coverage, primary_transform, relation_values + 1,
        live_overlap_counts, &policy_input) != 0 ||
      goodix_milan_study_policy_select (&policy_input, &policy_result) != 0)
    goto out;
  *action_code = policy_result.action;
  if (policy_result.action == GOODIX_MILAN_STUDY_ACTION_NONE)
    {
      result = transient_state
                 ? milan_capture_study_transient (current, transient_state)
                 : 0;
      goto out;
    }
  if (policy_result.action != GOODIX_MILAN_STUDY_ACTION_GEOMETRIC &&
      policy_result.action != GOODIX_MILAN_STUDY_ACTION_REPLACE &&
      policy_result.action != GOODIX_MILAN_STUDY_ACTION_REPLACE_NO_RELATION)
    goto out;
  replacement_index = policy_result.selected_feature_index;
  if (replacement_index >= current->feature_count)
    goto out;
  if (selected_feature_index)
    *selected_feature_index = replacement_index;

  int32_t target_b5;
  int32_t target_b6;
  int32_t target_ba;
  int32_t target_bb;
  int32_t target_bc;
  int32_t target_bd;
  int32_t target_be;
  int32_t target_c0;
  int32_t probe_c0;
  int32_t matched_be;
  GoodixMilanFeatureView target_feature;
  GoodixMilanFeatureView probe_feature;
  GoodixMilanFeatureView matched_feature;
  if (goodix_milan_template_read_feature_scalar (
        current->feature_elements[replacement_index],
        current->feature_element_sizes[replacement_index], 0xb5,
        &target_b5) != 0 || goodix_milan_template_read_feature_scalar (
        current->feature_elements[replacement_index],
        current->feature_element_sizes[replacement_index], 0xb6,
        &target_b6) != 0 || goodix_milan_template_read_feature_scalar (
        current->feature_elements[replacement_index],
        current->feature_element_sizes[replacement_index], 0xba,
        &target_ba) != 0 || goodix_milan_template_read_feature_scalar (
        current->feature_elements[replacement_index],
        current->feature_element_sizes[replacement_index], 0xbb,
        &target_bb) != 0 || goodix_milan_template_read_feature_scalar (
        current->feature_elements[replacement_index],
        current->feature_element_sizes[replacement_index], 0xbc,
        &target_bc) != 0 || goodix_milan_template_read_feature_scalar (
        current->feature_elements[replacement_index],
        current->feature_element_sizes[replacement_index], 0xbd,
        &target_bd) != 0 || goodix_milan_template_read_feature_scalar (
        current->feature_elements[replacement_index],
        current->feature_element_sizes[replacement_index], 0xbe,
        &target_be) != 0 || goodix_milan_template_read_feature_scalar (
        current->feature_elements[replacement_index],
        current->feature_element_sizes[replacement_index], 0xc0,
        &target_c0) != 0 || goodix_milan_template_read_feature_scalar (
        probe->feature_elements[0], probe->feature_element_sizes[0], 0xc0,
        &probe_c0) != 0 || goodix_milan_template_read_feature_scalar (
        current->feature_elements[matched_feature_index],
        current->feature_element_sizes[matched_feature_index], 0xbe,
        &matched_be) != 0 || goodix_milan_template_parse_feature_element (
        current->feature_elements[replacement_index],
        current->feature_element_sizes[replacement_index], &target_feature) != 0 ||
      goodix_milan_template_parse_feature_element (
        probe->feature_elements[0], probe->feature_element_sizes[0],
        &probe_feature) != 0)
    goto out;

  const uint8_t *matched_source =
    current->feature_elements[matched_feature_index];
  const size_t matched_source_size =
    current->feature_element_sizes[matched_feature_index];

  const size_t probe_c7_size = probe_feature.fields.optional_c7 != 0 ? 5 : 0;
  const size_t target_c7_size = target_feature.fields.optional_c7 != 0 ? 5 : 0;
  const size_t probe_owned_size =
    probe->feature_element_sizes[0] - probe_c7_size;
  if (probe_owned_size > SIZE_MAX - target_c7_size)
    goto out;
  const size_t replacement_size = probe_owned_size + target_c7_size;
  size_t storage_size = replacement_size;
  for (size_t i = 0; i < current->feature_count; i++)
    if (i != replacement_index)
      {
        if (current->feature_element_sizes[i] > SIZE_MAX - storage_size)
          goto out;
        storage_size += current->feature_element_sizes[i];
      }
  feature_storage = malloc (storage_size);
  if (!feature_storage)
    goto out;
  uint8_t *cursor = feature_storage;
  for (size_t i = 0; i < current->feature_count; i++)
    {
      const uint8_t *source = current->feature_elements[i];
      size_t source_size = current->feature_element_sizes[i];

      if (i == replacement_index)
        {
          memcpy (cursor, probe->feature_elements[0], probe_owned_size);
          if (target_c7_size != 0)
            {
              cursor[probe_owned_size] = 0xc7;
              goodix_milan_template_write_u32 (
                cursor + probe_owned_size + 1,
                (uint32_t) target_feature.fields.optional_c7);
            }
          goodix_milan_template_write_u32 (cursor + 1, (uint32_t) (replacement_size - 5));
          source_size = replacement_size;
        }
      else
        memcpy (cursor, source, source_size);
      current->feature_elements[i] = cursor;
      current->feature_element_sizes[i] = source_size;
      cursor += source_size;
    }

  uint8_t *replacement = (uint8_t *) current->feature_elements[replacement_index];
  GoodixMilanAntifakeBlob *replacement_antifake =
    goodix_milan_template_mutable_feature_antifake (
      replacement, current->feature_element_sizes[replacement_index]);
  if (!replacement_antifake ||
      goodix_milan_template_parse_feature_element (
        matched_source, matched_source_size, &matched_feature) != 0)
    goto out;
  milan_copy_study_antifake_scalars (replacement_antifake,
                                      matched_feature.antifake);

  const struct
  {
    uint8_t tag;
    int32_t value;
  } scalar_patches[] = {
    { 0xb5, policy_result.action ==
                GOODIX_MILAN_STUDY_ACTION_REPLACE_NO_RELATION ? 0 : target_b5 },
    { 0xb6, target_b6 },
    { 0xba, target_ba == 0 ? 0 : 2 },
    { 0xbb, policy_result.action ==
                GOODIX_MILAN_STUDY_ACTION_REPLACE_NO_RELATION
              ? target_bb : 0 },
    { 0xbc, (int32_t) current->feature_count - 1 },
    { 0xbd, milan_study_wrap_increment (target_bd) },
    { 0xbe, policy_result.action ==
                GOODIX_MILAN_STUDY_ACTION_REPLACE_NO_RELATION
              ? target_be : matched_be },
    { 0xc0, probe_c0 != 0 && (probe_c0 & 1) != 0 ? probe_c0 : target_c0 },
  };
  for (size_t i = 0; i < sizeof(scalar_patches) / sizeof(scalar_patches[0]);
       i++)
    if (goodix_milan_template_patch_feature_scalar (
          replacement, current->feature_element_sizes[replacement_index],
          scalar_patches[i].tag, scalar_patches[i].value) != 0)
      goto out;
  for (size_t i = 0; i < current->feature_count; i++)
    if (i != replacement_index)
      {
        int32_t ordinal;

        if (goodix_milan_template_read_feature_scalar (
              current->feature_elements[i], current->feature_element_sizes[i],
              0xbc, &ordinal) != 0)
          goto out;
        if (ordinal > target_bc && goodix_milan_template_patch_feature_scalar (
              (uint8_t *) current->feature_elements[i],
              current->feature_element_sizes[i], 0xbc,
              milan_study_wrap_decrement (ordinal)) != 0)
          goto out;
      }

  if (((policy_result.action == GOODIX_MILAN_STUDY_ACTION_GEOMETRIC ||
        policy_result.action == GOODIX_MILAN_STUDY_ACTION_REPLACE) &&
       goodix_milan_relation_matrix_replace (
         relation_matrix, replacement_index, relation_values + 1) != 0) ||
      (policy_result.action ==
         GOODIX_MILAN_STUDY_ACTION_REPLACE_NO_RELATION &&
       replacement_index != relation_matrix->reference_feature_index &&
       goodix_milan_relation_matrix_clear_incident (
         relation_matrix, replacement_index) != 0))
    goto out;

  if ((policy_result.action == GOODIX_MILAN_STUDY_ACTION_GEOMETRIC ||
       policy_result.action == GOODIX_MILAN_STUDY_ACTION_REPLACE) &&
       (milan_study_project_relations (current, relation_matrix) != 0 ||
         goodix_milan_template_normalize_unpacked (
           current, post_normalization_copies, live_overlap_counts) != 0))
    goto out;

  if (finalize_study)
    {
      uint32_t replacement_count = goodix_milan_template_read_u32 (
        current->tail_state + 0x50c);
      goodix_milan_template_write_u32 (current->tail_state + 0x50c, replacement_count + 1);
    }
  if (complete_dispatcher_transaction)
    current->metadata.queue_state = 1;
  uint32_t policy_counter = goodix_milan_template_read_u32 (current->tail_state + 0x510);
  goodix_milan_template_write_u32 (current->tail_state + 0x510, policy_counter + 1);
  if ((finalize_study && milan_finalize_template_study_order (current) != 0) ||
      milan_study_project_relations (current, relation_matrix) != 0)
    goto out;

  result = goodix_milan_template_pack (
    current->feature_elements, current->feature_element_sizes,
    current->feature_count, current->relations, current->relation_count,
    &current->metadata, current->tail_state, sizeof(current->tail_state),
    packed, packed_capacity, packed_size);

out:
  for (size_t i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    {
      free (post_normalization_copies[i]);
      free (pre_normalization_copies[i]);
      free (feature_copies[i]);
    }
  free (feature_storage);
  free (relation_matrix);
  free (probe);
  free (current);
  return result;
}

int
goodix_milan_study_finalize (const uint8_t *current_template,
                                    size_t         current_template_size,
                                    uint32_t       queue_state,
                                    uint32_t       queue_transaction_counter,
                                    int            finalize_transaction,
                                    uint8_t       *packed,
                                    size_t         packed_capacity,
                                    size_t        *packed_size)
{
  GoodixMilanUnpackedTemplate *current = NULL;
  uint32_t transaction_count;
  int result = -1;

  if (!current_template || queue_state > 1 || !packed || !packed_size)
    return -1;
  current = malloc (sizeof(*current));
  if (!current || goodix_milan_template_unpack (
        current_template, current_template_size, current) != 0)
    goto out;
  if (finalize_transaction)
    {
      transaction_count = goodix_milan_template_read_u32 (current->tail_state + 0x50c);
      if (milan_finalize_template_study_order (current) != 0)
        goto out;
      goodix_milan_template_write_u32 (current->tail_state + 0x50c, transaction_count + 1);
    }
  current->metadata.queue_state = queue_state;
  current->metadata.queue_transaction_counter = queue_transaction_counter;
  result = goodix_milan_template_pack (
    current->feature_elements, current->feature_element_sizes,
    current->feature_count, current->relations, current->relation_count,
    &current->metadata, current->tail_state, sizeof(current->tail_state),
    packed, packed_capacity, packed_size);

out:
  free (current);
  return result;
}
