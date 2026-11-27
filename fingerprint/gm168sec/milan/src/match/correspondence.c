/*
 * Goodix 53x5 driver for libfprint - Milan match correspondence
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/match/correspondence.h"
#include "milan/preprocess/state.h"
#include "milan/relations.h"
#include "milan/transform-private.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int
milan_descriptor_distance (const GoodixMilanFeatureRecord *first,
                           const GoodixMilanFeatureRecord *second)
{
  const uint8_t *a = (const uint8_t *) first + 16;
  const uint8_t *b = (const uint8_t *) second + 16;
  int distance = 0;

  for (size_t i = 0; i < 24; i++)
    {
      uint8_t value = a[i] ^ b[i];

      while (value != 0)
        {
          value &= (uint8_t) (value - 1);
          distance++;
        }
    }
  return distance;
}

static int32_t
milan_spatial_distance_squared_s32 (int32_t dx,
                                    int32_t dy)
{
  return goodix_milan_transform_s32 (
    (uint32_t) dx * (uint32_t) dx + (uint32_t) dy * (uint32_t) dy);
}

size_t
goodix_milan_match_feature_records (const GoodixMilanFeatureRecord *prior,
                             size_t                          prior_count,
                             const GoodixMilanFeatureRecord *current,
                             size_t                          current_count,
                             MilanFeatureMatch              matches[31])
{
  size_t match_count = 0;

  for (size_t prior_index = 0; prior_index < prior_count; prior_index++)
    {
      int best_distance = 192;
      int second_distance = 192;
      int best_index = -1;

      for (size_t current_index = 0; current_index < current_count;
           current_index++)
        {
          int distance = milan_descriptor_distance (
            &prior[prior_index], &current[current_index]);

          if (distance < best_distance)
            {
              second_distance = best_distance;
              best_distance = distance;
              best_index = (int) current_index;
            }
          else if (distance < second_distance)
            second_distance = distance;
        }
      if (best_index < 0 || best_distance * 40 >= second_distance * 38)
        continue;

      const uint16_t current_x = (uint16_t) current[best_index].refined_x;
      const uint16_t current_y = (uint16_t) current[best_index].refined_y;
      int replaced = 0;

      for (size_t i = 0; i < match_count;)
        {
          const GoodixMilanFeatureRecord *selected =
            &current[matches[i].current_index];
          int32_t dx = (int32_t) current_x -
                       (int32_t) (uint16_t) selected->refined_x;
          int32_t dy = (int32_t) current_y -
                       (int32_t) (uint16_t) selected->refined_y;

          if (milan_spatial_distance_squared_s32 (dx, dy) >= 0x10000)
            {
              i++;
              continue;
            }
          if (best_distance >= matches[i].best_distance)
            {
              if (!replaced)
                {
                  replaced = -1;
                  break;
                }
              i++;
              continue;
            }
          if (!replaced)
            {
              matches[i] = (MilanFeatureMatch) {
                (int32_t) prior_index, best_index,
                best_distance, second_distance,
              };
              replaced = 1;
              i++;
            }
          else
            {
              matches[i] = matches[match_count - 1];
              match_count--;
              i++;
            }
        }
      if (replaced != 0)
        continue;
      if (match_count == 31)
        {
          size_t worst = 0;

          for (size_t i = 1; i < match_count; i++)
            if (matches[i].best_distance > matches[worst].best_distance)
              worst = i;
          if (best_distance >= matches[worst].best_distance)
            continue;
          matches[worst] = (MilanFeatureMatch) {
            (int32_t) prior_index, best_index,
            best_distance, second_distance,
          };
        }
      else
        matches[match_count++] = (MilanFeatureMatch) {
          (int32_t) prior_index, best_index,
          best_distance, second_distance,
        };
    }
  return match_count;
}

static int
milan_descriptor_hamming64 (const uint8_t *source,
                            const uint8_t *target)
{
  uint64_t a, b;

  memcpy (&a, source, sizeof (a));
  memcpy (&b, target, sizeof (b));
  return __builtin_popcountll (a ^ b);
}

static int
milan_descriptor_hamming32 (const uint8_t *source,
                            const uint8_t *target)
{
  uint32_t a, b;

  memcpy (&a, source, sizeof (a));
  memcpy (&b, target, sizeof (b));
  return __builtin_popcount (a ^ b);
}

static int
milan_recognition_descriptor_details (const GoodixMilanFeatureRecord *enrolled,
                                      const GoodixMilanFeatureRecord *probe,
                                      int                            *normal_layout)
{
  const uint8_t *source = (const uint8_t *) enrolled;
  const uint8_t *target = (const uint8_t *) probe;
  int first = 0;
  int second = 0;
  int normal_tail = 0;
  int shifted_tail = 0;

  first = milan_descriptor_hamming64 (source + 16, target + 16);
  if (first > 23)
    return -1;

  second = milan_descriptor_hamming64 (source + 24, target + 24);

  int shifted = first - second + 64;
  if (first + second > 47)
    {
      if (shifted > 47)
        return -1;
      normal_tail = 192;
    }
  else
    {
      normal_tail = milan_descriptor_hamming32 (source + 32, target + 32) +
                    milan_descriptor_hamming32 (source + 40, target + 40);
      if (shifted > 47)
        shifted_tail = 192;
    }

  if (shifted_tail != 192)
    shifted_tail = milan_descriptor_hamming32 (source + 32, target + 36) +
                   milan_descriptor_hamming32 (source + 40, target + 48);

  int normal_distance = first + second + normal_tail;
  int shifted_distance = shifted + shifted_tail;
  if (normal_layout)
    *normal_layout = normal_distance < shifted_distance;
  return normal_distance < shifted_distance ? normal_distance : shifted_distance;
}

static int
milan_recognition_descriptor_distance (const GoodixMilanFeatureRecord *enrolled,
                                       const GoodixMilanFeatureRecord *probe)
{
  return milan_recognition_descriptor_details (enrolled, probe, NULL);
}

int
goodix_milan_match_correspondences_partitioned (
  const GoodixMilanFeatureRecord    *enrolled_records,
  size_t                             enrolled_record_count,
  size_t                             enrolled_partition_count,
  const GoodixMilanFeatureRecord    *probe_records,
  size_t                             probe_record_count,
  size_t                             probe_partition_count,
  int32_t                           *pairs,
  size_t                             pair_capacity,
  size_t                            *pair_count,
  const MilanMatchDistanceReduction *primary_reductions)
{
  MilanFeatureMatch matches[MILAN_MATCH_MAX_PAIRS];
  size_t match_count = 0;

  if (!enrolled_records || !probe_records || !pairs || !pair_count ||
      enrolled_record_count == 0 || enrolled_record_count > 150 ||
      probe_record_count == 0 || probe_record_count > 150 ||
      enrolled_partition_count > enrolled_record_count ||
      probe_partition_count > probe_record_count ||
      pair_capacity < 31 || pair_capacity > MILAN_MATCH_MAX_PAIRS)
    return -1;

  for (size_t enrolled_index = 0;
       enrolled_index < enrolled_record_count; enrolled_index++)
    {
      size_t probe_begin = enrolled_index < enrolled_partition_count
                             ? 0
                             : probe_partition_count;
      size_t probe_end = enrolled_index < enrolled_partition_count
                           ? probe_partition_count
                           : probe_record_count;
      int best_distance = 192;
      int second_distance = 192;
      int best_index = -1;

      if (primary_reductions)
        {
          best_distance = primary_reductions[enrolled_index].best_distance;
          second_distance = primary_reductions[enrolled_index].second_distance;
          best_index = primary_reductions[enrolled_index].best_index;
        }
      else
        {
          for (size_t probe_index = probe_begin; probe_index < probe_end;
               probe_index++)
            {
              int distance = milan_recognition_descriptor_distance (
                &enrolled_records[enrolled_index], &probe_records[probe_index]);

              if (distance < 0)
                continue;
              if (distance < best_distance)
                {
                  second_distance = best_distance;
                  best_distance = distance;
                  best_index = (int) probe_index;
                }
              else if (distance < second_distance)
                {
                  second_distance = distance;
                }
            }
        }
      if (best_index < 0 || best_distance * 40 >= second_distance * 38)
        continue;

      const uint16_t probe_x = (uint16_t) probe_records[best_index].refined_x;
      const uint16_t probe_y = (uint16_t) probe_records[best_index].refined_y;
      int replaced = 0;

      for (size_t i = 0; i < match_count;)
        {
          const GoodixMilanFeatureRecord *selected =
            &probe_records[matches[i].current_index];
          int32_t dx = (int32_t) probe_x -
                       (int32_t) (uint16_t) selected->refined_x;
          int32_t dy = (int32_t) probe_y -
                       (int32_t) (uint16_t) selected->refined_y;

          if (milan_spatial_distance_squared_s32 (dx, dy) >= 0x10000)
            {
              i++;
              continue;
            }
          if (best_distance >= matches[i].best_distance)
            {
              if (!replaced)
                {
                  replaced = -1;
                  break;
                }
              i++;
              continue;
            }
          if (!replaced)
            {
              matches[i] = (MilanFeatureMatch) {
                (int32_t) enrolled_index, best_index,
                best_distance, second_distance,
              };
              replaced = 1;
              i++;
            }
          else
            {
              matches[i] = matches[match_count - 1];
              match_count--;
              i++;
            }
        }
      if (replaced != 0)
        continue;
      if (match_count == pair_capacity)
        {
          size_t worst = 0;

          for (size_t i = 1; i < match_count; i++)
            if (matches[i].best_distance > matches[worst].best_distance)
              worst = i;
          if (best_distance >= matches[worst].best_distance)
            continue;
          matches[worst] = (MilanFeatureMatch) {
            (int32_t) enrolled_index, best_index,
            best_distance, second_distance,
          };
        }
      else
        matches[match_count++] = (MilanFeatureMatch) {
          (int32_t) enrolled_index, best_index,
          best_distance, second_distance,
        };
    }

  for (size_t i = 0; i < pair_capacity; i++)
    {
      pairs[i * 2] = -1;
      pairs[i * 2 + 1] = -1;
    }
  for (size_t i = 0; i < match_count; i++)
    {
      pairs[i * 2] = matches[i].prior_index;
      pairs[i * 2 + 1] = matches[i].current_index;
    }
  *pair_count = match_count;
  return 0;
}

size_t
goodix_milan_match_relaxed_correspondences (
  const GoodixMilanFeatureRecord *enrolled_records,
  size_t                          enrolled_record_count,
  size_t                          enrolled_partition_count,
  const GoodixMilanFeatureRecord *probe_records,
  size_t                          probe_record_count,
  size_t                          probe_partition_count,
  int32_t                         pairs[62],
  MilanMatchDistanceReduction    *primary_reductions)
{
  MilanFeatureMatch matches[31];
  size_t match_count = 0;

  if (primary_reductions)
    for (size_t i = 0; i < enrolled_record_count; i++)
      primary_reductions[i] = (MilanMatchDistanceReduction){ 192, 192, -1 };

  for (size_t probe_index = 0; probe_index < probe_record_count; probe_index++)
    {
      size_t enrolled_begin = probe_index < probe_partition_count
                                ? 0
                                : enrolled_partition_count;
      size_t enrolled_end = probe_index < probe_partition_count
                              ? enrolled_partition_count
                              : enrolled_record_count;
      int best_distance = 192;
      int second_distance = 192;
      int best_index = -1;

      for (size_t enrolled_index = enrolled_begin;
           enrolled_index < enrolled_end; enrolled_index++)
        {
          int distance = milan_recognition_descriptor_distance (
            &enrolled_records[enrolled_index], &probe_records[probe_index]);

          if (distance < 0)
            continue;
          /* Each enrolled record still sees probe indices in ascending order. */
          if (primary_reductions)
            {
              MilanMatchDistanceReduction *primary =
                &primary_reductions[enrolled_index];

              if (distance < primary->best_distance)
                {
                  primary->second_distance = primary->best_distance;
                  primary->best_distance = distance;
                  primary->best_index = (int32_t) probe_index;
                }
              else if (distance < primary->second_distance)
                {
                  primary->second_distance = distance;
                }
            }
          if (distance < best_distance)
            {
              second_distance = best_distance;
              best_distance = distance;
              best_index = (int) enrolled_index;
            }
          else if (distance < second_distance)
            second_distance = distance;
        }
      if (best_index < 0 || best_distance * 40 >= second_distance * 38)
        continue;

      uint16_t enrolled_x =
        (uint16_t) enrolled_records[best_index].refined_x;
      uint16_t enrolled_y =
        (uint16_t) enrolled_records[best_index].refined_y;
      int replaced = 0;

      for (size_t i = 0; i < match_count;)
        {
          const GoodixMilanFeatureRecord *selected =
            &enrolled_records[matches[i].prior_index];
          int32_t dx = (int32_t) enrolled_x -
                       (int32_t) (uint16_t) selected->refined_x;
          int32_t dy = (int32_t) enrolled_y -
                       (int32_t) (uint16_t) selected->refined_y;

          if (milan_spatial_distance_squared_s32 (dx, dy) >= 0x10000)
            {
              i++;
              continue;
            }
          if (best_distance >= matches[i].best_distance)
            {
              if (!replaced)
                {
                  replaced = -1;
                  break;
                }
              i++;
              continue;
            }
          if (!replaced)
            {
              matches[i] = (MilanFeatureMatch) {
                best_index, (int32_t) probe_index,
                best_distance, second_distance,
              };
              replaced = 1;
              i++;
            }
          else
            {
              matches[i] = matches[match_count - 1];
              match_count--;
              i++;
            }
        }
      if (replaced != 0)
        continue;
      if (match_count == 31)
        {
          size_t worst = 0;

          for (size_t i = 1; i < match_count; i++)
            if (matches[i].best_distance > matches[worst].best_distance)
              worst = i;
          if (best_distance >= matches[worst].best_distance)
            continue;
          matches[worst] = (MilanFeatureMatch) {
            best_index, (int32_t) probe_index,
            best_distance, second_distance,
          };
        }
      else
        matches[match_count++] = (MilanFeatureMatch) {
          best_index, (int32_t) probe_index,
          best_distance, second_distance,
        };
    }

  for (size_t i = 0; i < 31; i++)
    {
      pairs[i * 2] = i < match_count ? matches[i].prior_index : -1;
      pairs[i * 2 + 1] = i < match_count ? matches[i].current_index : -1;
    }
  return match_count;
}

static int32_t
milan_match_scan_raw_s32 (int32_t first,
                          uint32_t first_coordinate,
                          int32_t second,
                          uint32_t second_coordinate,
                          int32_t translation)
{
  return goodix_milan_transform_affine_s32 (
    first, first_coordinate, second, second_coordinate,
    (uint32_t) translation << 8);
}

static int32_t
milan_match_scan_round_pixel_s32 (int32_t value)
{
  int32_t rounded_q8 = goodix_milan_transform_sar32 (
    (uint32_t) value + UINT32_C (0x80), 8);

  return goodix_milan_transform_sar32 (
    (uint32_t) rounded_q8 + UINT32_C (0x80), 8);
}

static int
milan_match_scan_residual_within (int32_t mapped,
                                  uint32_t expected)
{
  int32_t difference = goodix_milan_transform_s32 (
    (uint32_t) mapped - expected);

  if (difference <= 0)
    difference = goodix_milan_transform_s32 (
      expected - (uint32_t) mapped);
  return difference <= 0x1500;
}

int
goodix_milan_match_alternate_correspondences_internal (
  const GoodixMilanFeatureRecord *enrolled_records,
  size_t                          enrolled_record_count,
  size_t                          enrolled_partition,
  const GoodixMilanFeatureRecord *probe_records,
  size_t                          probe_record_count,
  size_t                          probe_partition,
  const int32_t                   primary_transform[6],
  int32_t                        *pairs,
  size_t                          pair_capacity,
  size_t                         *pair_count)
{
  MilanFeatureMatch matches[MILAN_MATCH_MAX_PAIRS];
  int32_t inverse[6];
  size_t match_count = 0;

  if (!enrolled_records || !probe_records || !primary_transform || !pairs ||
      !pair_count || enrolled_partition > enrolled_record_count ||
      probe_partition > probe_record_count || pair_capacity == 0 ||
      pair_capacity > MILAN_MATCH_MAX_PAIRS ||
      goodix_milan_transform_invert_s32_checked (
        primary_transform, inverse) != 0)
    return -1;
  for (size_t partition = 0; partition < 2; partition++)
    {
      size_t enrolled_begin = partition == 0 ? 0 : enrolled_partition;
      size_t enrolled_end = partition == 0 ? enrolled_partition
                                            : enrolled_record_count;
      size_t probe_begin = partition == 0 ? 0 : probe_partition;
      size_t probe_end = partition == 0 ? probe_partition : probe_record_count;

      for (size_t enrolled_index = enrolled_begin;
           enrolled_index < enrolled_end; enrolled_index++)
        {
          int32_t enrolled_x =
            (uint16_t) enrolled_records[enrolled_index].refined_x;
          int32_t enrolled_y =
            (uint16_t) enrolled_records[enrolled_index].refined_y;
          int32_t inverse_x = milan_match_scan_round_pixel_s32 (
            milan_match_scan_raw_s32 (
              inverse[0], (uint32_t) enrolled_x,
              inverse[1], (uint32_t) enrolled_y, inverse[2]));
          int32_t inverse_y = milan_match_scan_round_pixel_s32 (
            milan_match_scan_raw_s32 (
              inverse[3], (uint32_t) enrolled_x,
              inverse[4], (uint32_t) enrolled_y, inverse[5]));
          int best_distance = 192;
          int second_distance = 192;
          int best_index = -1;

          if (inverse_x <= 5 ||
              inverse_x >= GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS - 5 ||
              inverse_y <= 5 ||
              inverse_y >= GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS - 5)
            continue;
          for (size_t probe_index = probe_begin; probe_index < probe_end;
               probe_index++)
            {
              int distance = milan_recognition_descriptor_distance (
                &enrolled_records[enrolled_index], &probe_records[probe_index]);
              int32_t probe_x =
                (uint16_t) probe_records[probe_index].refined_x;
              int32_t probe_y =
                (uint16_t) probe_records[probe_index].refined_y;
              int32_t raw_x = milan_match_scan_raw_s32 (
                primary_transform[0], (uint32_t) probe_x,
                primary_transform[1], (uint32_t) probe_y,
                primary_transform[2]);
              int32_t raw_y = milan_match_scan_raw_s32 (
                primary_transform[3], (uint32_t) probe_x,
                primary_transform[4], (uint32_t) probe_y,
                primary_transform[5]);
              int32_t transformed_x = goodix_milan_transform_sar32 (
                (uint32_t) raw_x, 8);
              int32_t transformed_y = goodix_milan_transform_sar32 (
                (uint32_t) raw_y, 8);
              int32_t pixel_x = milan_match_scan_round_pixel_s32 (raw_x);
              int32_t pixel_y = milan_match_scan_round_pixel_s32 (raw_y);

              if (distance < 0 ||
                  !milan_match_scan_residual_within (
                    transformed_x, (uint32_t) enrolled_x) ||
                  !milan_match_scan_residual_within (
                    transformed_y, (uint32_t) enrolled_y) ||
                  pixel_x <= 5 ||
                  pixel_x > GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS - 5 ||
                  pixel_y <= 5 ||
                  pixel_y > GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS - 5)
                continue;
              if (distance < best_distance)
                {
                  second_distance = best_distance;
                  best_distance = distance;
                  best_index = (int) probe_index;
                }
              else if (distance < second_distance)
                second_distance = distance;
            }
          if (best_index < 0 || best_distance * 40 >= second_distance * 38)
            continue;

          uint16_t probe_x =
            (uint16_t) probe_records[best_index].refined_x;
          uint16_t probe_y =
            (uint16_t) probe_records[best_index].refined_y;
          int replaced = 0;
          for (size_t i = 0; i < match_count;)
            {
              const GoodixMilanFeatureRecord *selected =
                &probe_records[matches[i].current_index];
              int32_t dx = (int32_t) probe_x -
                           (int32_t) (uint16_t) selected->refined_x;
              int32_t dy = (int32_t) probe_y -
                           (int32_t) (uint16_t) selected->refined_y;

              if (milan_spatial_distance_squared_s32 (dx, dy) >= 0x10000)
                {
                  i++;
                  continue;
                }
              if (best_distance >= matches[i].best_distance)
                {
                  if (!replaced)
                    {
                      replaced = -1;
                      break;
                    }
                  i++;
                  continue;
                }
              if (!replaced)
                {
                  matches[i] = (MilanFeatureMatch) {
                    (int32_t) enrolled_index, best_index,
                    best_distance, second_distance,
                  };
                  replaced = 1;
                  i++;
                }
              else
                {
                  matches[i] = matches[match_count - 1];
                  match_count--;
                }
            }
          if (replaced != 0)
            continue;
          if (match_count == pair_capacity)
            {
              size_t worst = 0;

              for (size_t i = 1; i < match_count; i++)
                if (matches[i].best_distance > matches[worst].best_distance)
                  worst = i;
              if (best_distance >= matches[worst].best_distance)
                continue;
              matches[worst] = (MilanFeatureMatch) {
                (int32_t) enrolled_index, best_index,
                best_distance, second_distance,
              };
            }
          else
            matches[match_count++] = (MilanFeatureMatch) {
              (int32_t) enrolled_index, best_index,
              best_distance, second_distance,
            };
        }
    }

  for (size_t i = 0; i < pair_capacity; i++)
    {
      pairs[i * 2] = i < match_count ? matches[i].prior_index : -1;
      pairs[i * 2 + 1] = i < match_count ? matches[i].current_index : -1;
    }
  *pair_count = match_count;
  return 0;
}

static int
milan_cross_class_descriptor_distance (
  const GoodixMilanFeatureRecord *enrolled,
  const GoodixMilanFeatureRecord *probe,
  int32_t                         tail_hamming_limit)
{
  const uint8_t *source = (const uint8_t *) enrolled;
  const uint8_t *target = (const uint8_t *) probe;
  int normal_layout;
  int distance = 0;

  if (milan_recognition_descriptor_details (
        enrolled, probe, &normal_layout) < 0)
    return -1;
  size_t target_offset = normal_layout ? 40 : 48;
  for (size_t i = 0; i < 8; i++)
    distance += __builtin_popcount (
      (unsigned) (source[40 + i] ^ target[target_offset + i]));
  return distance <= tail_hamming_limit ? distance : -1;
}

static int
milan_tail_descriptor_distance (const GoodixMilanFeatureRecord *enrolled,
                                const GoodixMilanFeatureRecord *probe,
                                int32_t                         hamming_limit)
{
  const uint8_t *source = (const uint8_t *) enrolled;
  const uint8_t *target = (const uint8_t *) probe;
  int normal = 0;
  int shifted = 0;

  for (size_t i = 0; i < 8; i++)
    {
      normal += __builtin_popcount (
        (unsigned) (source[40 + i] ^ target[40 + i]));
      shifted += __builtin_popcount (
        (unsigned) (source[40 + i] ^ target[48 + i]));
    }
  int distance = normal < shifted ? normal : shifted;
  return distance <= hamming_limit ? distance : -1;
}

static void
milan_store_cross_class_match (
  MilanFeatureMatch                 matches[MILAN_MATCH_MAX_PAIRS],
  size_t                           *match_count,
  const GoodixMilanFeatureRecord   *probe_records,
  int32_t                           enrolled_index,
  int32_t                           probe_index,
  int32_t                           best_distance,
  int32_t                           second_distance,
  size_t                            pair_capacity)
{
  uint16_t probe_x = (uint16_t) probe_records[probe_index].refined_x;
  uint16_t probe_y = (uint16_t) probe_records[probe_index].refined_y;
  int replaced = 0;

  for (size_t i = 0; i < *match_count;)
    {
      const GoodixMilanFeatureRecord *selected =
        &probe_records[matches[i].current_index];
      int32_t dx = (int32_t) probe_x -
                   (int32_t) (uint16_t) selected->refined_x;
      int32_t dy = (int32_t) probe_y -
                   (int32_t) (uint16_t) selected->refined_y;

      if (milan_spatial_distance_squared_s32 (dx, dy) >= 0x10000)
        {
          i++;
          continue;
        }
      if (best_distance >= matches[i].best_distance)
        {
          if (!replaced)
            {
              replaced = -1;
              break;
            }
          i++;
          continue;
        }
      if (!replaced)
        {
          matches[i] = (MilanFeatureMatch) {
            enrolled_index, probe_index, best_distance, second_distance,
          };
          replaced = 1;
          i++;
        }
      else
        {
          matches[i] = matches[*match_count - 1];
          (*match_count)--;
          i++;
        }
    }
  if (replaced != 0)
    return;
  if (*match_count == pair_capacity)
    {
      size_t worst = 0;

      for (size_t i = 1; i < *match_count; i++)
        if (matches[i].best_distance > matches[worst].best_distance)
          worst = i;
      if (best_distance >= matches[worst].best_distance)
        return;
      matches[worst] = (MilanFeatureMatch) {
        enrolled_index, probe_index, best_distance, second_distance,
      };
    }
  else
    matches[(*match_count)++] = (MilanFeatureMatch) {
      enrolled_index, probe_index, best_distance, second_distance,
    };
}

size_t
goodix_milan_match_cross_class_correspondences (
  const GoodixMilanFeatureRecord *enrolled_records,
  size_t                          enrolled_record_count,
  size_t                          enrolled_partition_count,
  const GoodixMilanFeatureRecord *probe_records,
  size_t                          probe_record_count,
  size_t                          probe_partition_count,
  uint8_t                         enrolled_class,
  int32_t                         tail_hamming_limit,
  int32_t                        *pairs,
  size_t                          pair_capacity)
{
  MilanFeatureMatch matches[MILAN_MATCH_MAX_PAIRS];
  size_t match_count = 0;

  for (size_t enrolled_index = 0;
       enrolled_index < enrolled_record_count; enrolled_index++)
    {
      const uint8_t *enrolled =
        (const uint8_t *) &enrolled_records[enrolled_index];
      size_t probe_begin = enrolled_index < enrolled_partition_count
                             ? 0
                             : probe_partition_count;
      size_t probe_end = enrolled_index < enrolled_partition_count
                           ? probe_partition_count
                           : probe_record_count;
      int best_distance = 192;
      int second_distance = 192;
      int best_index = -1;

      if (enrolled[12] != enrolled_class)
        continue;
      for (size_t probe_index = probe_begin; probe_index < probe_end;
           probe_index++)
        {
          const uint8_t *probe =
            (const uint8_t *) &probe_records[probe_index];
          int distance;

          if (probe[12] != (enrolled_class == 1 ? 2 : 1))
            continue;
          distance = milan_cross_class_descriptor_distance (
            &enrolled_records[enrolled_index], &probe_records[probe_index],
            tail_hamming_limit);
          if (distance < 0)
            continue;
          if (distance < best_distance)
            {
              second_distance = best_distance;
              best_distance = distance;
              best_index = (int) probe_index;
            }
          else if (distance < second_distance)
            second_distance = distance;
        }
      if (best_index < 0 || best_distance * 40 >= second_distance * 38)
        continue;
      milan_store_cross_class_match (
        matches, &match_count, probe_records, (int32_t) enrolled_index,
        best_index, best_distance, second_distance, pair_capacity);
    }

  for (size_t i = 0; i < pair_capacity; i++)
    {
      pairs[i * 2] = i < match_count ? matches[i].prior_index : -1;
      pairs[i * 2 + 1] = i < match_count ? matches[i].current_index : -1;
    }
  return match_count;
}

size_t
goodix_milan_match_cross_class_alternate_correspondences (
  const GoodixMilanFeatureRecord *enrolled_records,
  size_t                          enrolled_record_count,
  size_t                          enrolled_partition_count,
  const GoodixMilanFeatureRecord *probe_records,
  size_t                          probe_record_count,
  size_t                          probe_partition_count,
  const int32_t                   primary_transform[6],
  int32_t                         sibling_tail_hamming_limit,
  int32_t                        *pairs,
  size_t                          pair_capacity)
{
  MilanFeatureMatch matches[MILAN_MATCH_MAX_PAIRS];
  int32_t inverse[6];
  size_t match_count = 0;

  for (size_t i = 0; i < pair_capacity; i++)
    {
      pairs[i * 2] = -1;
      pairs[i * 2 + 1] = -1;
    }
  if (goodix_milan_transform_invert_s32_checked (
        primary_transform, inverse) != 0)
    return 0;

  for (size_t enrolled_index = 0;
       enrolled_index < enrolled_record_count; enrolled_index++)
    {
      int32_t enrolled_x =
        (uint16_t) enrolled_records[enrolled_index].refined_x;
      int32_t enrolled_y =
        (uint16_t) enrolled_records[enrolled_index].refined_y;
      int32_t inverse_x = milan_match_scan_round_pixel_s32 (
        milan_match_scan_raw_s32 (
          inverse[0], (uint32_t) enrolled_x,
          inverse[1], (uint32_t) enrolled_y, inverse[2]));
      int32_t inverse_y = milan_match_scan_round_pixel_s32 (
        milan_match_scan_raw_s32 (
          inverse[3], (uint32_t) enrolled_x,
          inverse[4], (uint32_t) enrolled_y, inverse[5]));
      size_t probe_begin = enrolled_index < enrolled_partition_count
                             ? 0
                             : probe_partition_count;
      size_t probe_end = enrolled_index < enrolled_partition_count
                           ? probe_partition_count
                           : probe_record_count;
      int best_distance = 192;
      int second_distance = 192;
      int best_index = -1;

      if (inverse_x <= 5 ||
          inverse_x >= GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS - 5 ||
          inverse_y <= 5 ||
          inverse_y >= GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS - 5)
        continue;
      for (size_t probe_index = probe_begin; probe_index < probe_end;
           probe_index++)
        {
          int distance = milan_tail_descriptor_distance (
            &enrolled_records[enrolled_index], &probe_records[probe_index],
            sibling_tail_hamming_limit);
          int32_t probe_x = (uint16_t) probe_records[probe_index].refined_x;
          int32_t probe_y = (uint16_t) probe_records[probe_index].refined_y;
          int32_t raw_x = milan_match_scan_raw_s32 (
            primary_transform[0], (uint32_t) probe_x,
            primary_transform[1], (uint32_t) probe_y,
            primary_transform[2]);
          int32_t raw_y = milan_match_scan_raw_s32 (
            primary_transform[3], (uint32_t) probe_x,
            primary_transform[4], (uint32_t) probe_y,
            primary_transform[5]);
          int32_t transformed_x = goodix_milan_transform_sar32 (
            (uint32_t) raw_x, 8);
          int32_t transformed_y = goodix_milan_transform_sar32 (
            (uint32_t) raw_y, 8);
          int32_t pixel_x = milan_match_scan_round_pixel_s32 (raw_x);
          int32_t pixel_y = milan_match_scan_round_pixel_s32 (raw_y);

          if (distance < 0 ||
              !milan_match_scan_residual_within (
                transformed_x, (uint32_t) enrolled_x) ||
              !milan_match_scan_residual_within (
                transformed_y, (uint32_t) enrolled_y) ||
              pixel_x <= 5 ||
              pixel_x > GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS - 5 ||
              pixel_y <= 5 ||
              pixel_y > GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS - 5)
            continue;
          if (distance < best_distance)
            {
              second_distance = best_distance;
              best_distance = distance;
              best_index = (int) probe_index;
            }
          else if (distance < second_distance)
            second_distance = distance;
        }
      if (best_index < 0 || best_distance * 40 >= second_distance * 38)
        continue;
      milan_store_cross_class_match (
        matches, &match_count, probe_records, (int32_t) enrolled_index,
        best_index, best_distance, second_distance, pair_capacity);
    }

  for (size_t i = 0; i < pair_capacity; i++)
    {
      pairs[i * 2] = i < match_count ? matches[i].prior_index : -1;
      pairs[i * 2 + 1] = i < match_count ? matches[i].current_index : -1;
    }
  return match_count;
}
