/*
 * Goodix 53x5 driver for libfprint - Milan template lifecycle
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
#include "milan/relations.h"
#include "milan/template/codec-private.h"
#include "milan/template/normalization.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int
milan_feature_scalar_offset (const uint8_t *feature_element,
                             size_t         feature_element_size,
                             uint8_t        tag,
                             size_t        *value_offset)
{
  static const uint8_t scalar_tags[11] = {
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xc0,
  };
  size_t scalar_offset;

  if (!feature_element || feature_element_size < 55 || !value_offset)
    return -1;
  scalar_offset = feature_element_size - 55;
  if (feature_element[feature_element_size - 5] == 0xc7)
    {
      if (scalar_offset < 5)
        return -1;
      scalar_offset -= 5;
    }
  for (size_t i = 0; i < 11; i++)
    {
      if (feature_element[scalar_offset + i * 5] != scalar_tags[i])
        return -1;
      if (scalar_tags[i] == tag)
        {
          *value_offset = scalar_offset + i * 5 + 1;
          return 0;
        }
    }
  return -1;
}

int
goodix_milan_template_patch_feature_scalar (uint8_t *feature_element,
                            size_t   feature_element_size,
                            uint8_t  tag,
                            int32_t  value)
{
  size_t value_offset;

  if (milan_feature_scalar_offset (
        feature_element, feature_element_size, tag, &value_offset) != 0)
    return -1;
  goodix_milan_template_write_u32 (feature_element + value_offset, (uint32_t) value);
  return 0;
}

int
goodix_milan_template_read_feature_scalar (const uint8_t *feature_element,
                           size_t         feature_element_size,
                           uint8_t        tag,
                           int32_t       *value)
{
  size_t value_offset;

  if (!value || milan_feature_scalar_offset (
        feature_element, feature_element_size, tag, &value_offset) != 0)
    return -1;
  *value = (int32_t) goodix_milan_template_read_u32 (feature_element + value_offset);
  return 0;
}

int
goodix_milan_template_update_match_lifecycle (
  const uint8_t *current_template,
  size_t         current_template_size,
  uint64_t       feature_mask,
  bool           sort_order,
  uint8_t       *packed,
  size_t         packed_capacity,
  size_t        *packed_size)
{
  GoodixMilanUnpackedTemplate *current = NULL;
  uint8_t *feature_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  uint64_t valid_feature_mask;
  int result = -1;

  if (!current_template || !packed || !packed_size)
    return -1;
  if (feature_mask == 0)
    {
      if (packed_capacity < current_template_size)
        return -1;
      memcpy (packed, current_template, current_template_size);
      *packed_size = current_template_size;
      return 0;
    }
  current = malloc (sizeof(*current));
  if (!current || goodix_milan_template_unpack (
        current_template, current_template_size, current) != 0)
    goto out;

  valid_feature_mask = current->feature_count == 64
                         ? UINT64_MAX
                         : (UINT64_C(1) << current->feature_count) - 1;
  if (feature_mask & ~valid_feature_mask)
    goto out;

  for (size_t feature_index = 0; feature_index < current->feature_count;
       feature_index++)
    {
      size_t value_offset;
      uint32_t lifecycle_count;

      if (!(feature_mask & (UINT64_C(1) << feature_index)))
        continue;
      if (milan_feature_scalar_offset (
            current->feature_elements[feature_index],
            current->feature_element_sizes[feature_index], 0xbe,
            &value_offset) != 0)
        goto out;
      lifecycle_count = goodix_milan_template_read_u32 (
        current->feature_elements[feature_index] + value_offset);
      feature_copies[feature_index] = malloc (
        current->feature_element_sizes[feature_index]);
      if (!feature_copies[feature_index])
        goto out;
      memcpy (feature_copies[feature_index],
              current->feature_elements[feature_index],
              current->feature_element_sizes[feature_index]);
      goodix_milan_template_write_u32 (feature_copies[feature_index] + value_offset,
                       lifecycle_count + UINT32_C(1));
      current->feature_elements[feature_index] = feature_copies[feature_index];
    }
  /* Match finalization sorts the incremented keys without advancing generation. */
  if (sort_order &&
      current->metadata.sensor_type == GOODIX_MILAN_PRINT_SENSOR_TYPE &&
      current->feature_count > 1 &&
      goodix_milan_template_sort_type12_order (current) != 0)
    goto out;
  result = goodix_milan_template_pack (
    current->feature_elements, current->feature_element_sizes,
    current->feature_count, current->relations, current->relation_count,
    &current->metadata, current->tail_state, sizeof(current->tail_state),
    packed, packed_capacity, packed_size);

out:
  for (size_t feature_index = 0;
       feature_index < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; feature_index++)
    free (feature_copies[feature_index]);
  free (current);
  return result;
}

int
goodix_milan_template_initialize_tail (const uint8_t *frame,
                                               size_t         rows,
                                               size_t         columns,
                                               uint8_t       *tail_state,
                                               size_t         tail_size)
{
  static const char version[] = "Milan_v_3.01.09.10.50";
  const size_t tail_bytes = 0x520;

  if (!frame || rows != GOODIX_MILAN_SENSOR_ROWS ||
      columns != GOODIX_MILAN_SENSOR_COLUMNS || !tail_state ||
      tail_size < tail_bytes)
    return -1;

  /* The vendor leaves reserved tail bytes uninitialized; keep them stable. */
  memset (tail_state, 0, tail_bytes);
  memset (tail_state, 0xff, 200);
  memset (tail_state, 0, sizeof(uint32_t));
  memset (tail_state + 0xc8, 0xff, sizeof(uint32_t));
  memcpy (tail_state + 0xcc, version, sizeof(version));
  return 0;
}

int
goodix_milan_template_normalize_unpacked (
  GoodixMilanUnpackedTemplate *unpacked,
  uint8_t *feature_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  int32_t overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY])
{
  const int32_t rows = GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS;
  const int32_t columns = GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS;
  const int32_t effective_rows =
    goodix_milan_template_normalization_sar1 (rows);
  const int32_t effective_columns =
    goodix_milan_template_normalization_sar1 (columns);

  for (size_t feature_index = 0; feature_index < unpacked->feature_count;
       feature_index++)
    {
      GoodixMilanFeatureView feature;
      uint8_t residual[44 * 52];
      int32_t active;
      int32_t current_transform[6];
      int32_t residual_count =
        goodix_milan_template_normalization_domain_area (rows, columns, 1);
      int32_t overlap_count = 0;

      if (goodix_milan_template_parse_feature_element (
            unpacked->feature_elements[feature_index],
            unpacked->feature_element_sizes[feature_index], &feature) != 0 ||
          goodix_milan_template_read_feature_scalar (
            unpacked->feature_elements[feature_index],
            unpacked->feature_element_sizes[feature_index], 0xb5,
            &active) != 0)
        return -1;
      feature_copies[feature_index] = malloc (
        unpacked->feature_element_sizes[feature_index]);
      if (!feature_copies[feature_index])
        return -1;
      memcpy (feature_copies[feature_index],
              unpacked->feature_elements[feature_index],
              unpacked->feature_element_sizes[feature_index]);
      unpacked->feature_elements[feature_index] = feature_copies[feature_index];

      if (active != 0)
        {
          goodix_milan_feature_mask_expand (feature.inline_mask, residual);
          if (goodix_milan_template_reference_transform (
                unpacked, feature_index, 0, current_transform) != 0)
            return -1;
          for (size_t other_index = 0;
               other_index < unpacked->feature_count; other_index++)
            {
              GoodixMilanFeatureView other;
              int32_t other_active;
              int32_t other_transform[6];
              int32_t footprint_transform[6];

              if (other_index == feature_index ||
                  goodix_milan_template_parse_feature_element (
                    unpacked->feature_elements[other_index],
                    unpacked->feature_element_sizes[other_index], &other) != 0 ||
                  goodix_milan_template_read_feature_scalar (
                    unpacked->feature_elements[other_index],
                    unpacked->feature_element_sizes[other_index], 0xb5,
                    &other_active) != 0)
                {
                  if (other_index == feature_index)
                    continue;
                  return -1;
                }
              if (other_active == 0)
                continue;
              if (goodix_milan_template_reference_transform (
                    unpacked, other_index, 1, other_transform) != 0)
                return -1;
              goodix_milan_transform_compose (
                other_transform, current_transform, footprint_transform);
              footprint_transform[2] = goodix_milan_template_normalization_sar1 (
                footprint_transform[2]);
              footprint_transform[5] = goodix_milan_template_normalization_sar1 (
                footprint_transform[5]);
              int32_t area = goodix_milan_template_normalization_remove_footprint (
                residual, effective_rows, effective_columns, effective_rows,
                effective_columns, footprint_transform);

              if (goodix_milan_template_normalization_overlap_qualifies (
                    area, rows, columns, 1))
                overlap_count = goodix_milan_template_normalization_add (
                  overlap_count, 1);
            }
          residual_count = goodix_milan_template_normalization_residual (
            residual, effective_rows, effective_columns);
        }
      if (overlap_counts)
        overlap_counts[feature_index] = overlap_count;
      if (goodix_milan_template_patch_feature_scalar (
            feature_copies[feature_index],
            unpacked->feature_element_sizes[feature_index], 0xbb,
            residual_count) != 0)
        return -1;
    }
  return 0;
}

int
goodix_milan_template_normalize (
  const uint8_t *current_template,
  size_t         current_template_size,
  uint8_t       *packed,
  size_t         packed_capacity,
  size_t        *packed_size)
{
  GoodixMilanUnpackedTemplate *unpacked = NULL;
  uint8_t *feature_copies[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  int result = -1;

  if (!current_template || !packed || !packed_size)
    return -1;
  unpacked = malloc (sizeof(*unpacked));
  if (!unpacked || goodix_milan_template_unpack (
        current_template, current_template_size, unpacked) != 0)
    goto out;
  if (unpacked->metadata.graph_established == 1)
    {
      if (unpacked->metadata.graph_reference_index < 0 ||
          (size_t) unpacked->metadata.graph_reference_index >=
            unpacked->feature_count)
        goto out;
    }
  else
    {
      if (unpacked->metadata.sensor_type != GOODIX_MILAN_PRINT_SENSOR_TYPE ||
          unpacked->metadata.graph_established != 0 ||
          unpacked->metadata.graph_reference_index != -1 ||
          unpacked->relation_count != 0)
        goto out;
      for (size_t i = 0; i < unpacked->feature_count; i++)
        {
          int32_t active;

          if (goodix_milan_template_read_feature_scalar (
                unpacked->feature_elements[i],
                unpacked->feature_element_sizes[i], 0xb5, &active) != 0 ||
              active != 0)
            goto out;
        }
    }
  if (goodix_milan_template_normalize_unpacked (
        unpacked, feature_copies,
        unpacked->normalization_overlap_counts) != 0)
    goto out;

  result = goodix_milan_template_pack (
    unpacked->feature_elements, unpacked->feature_element_sizes,
    unpacked->feature_count, unpacked->relations, unpacked->relation_count,
    &unpacked->metadata, unpacked->tail_state, sizeof(unpacked->tail_state),
    packed, packed_capacity, packed_size);

out:
  for (size_t i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    free (feature_copies[i]);
  free (unpacked);
  return result;
}
