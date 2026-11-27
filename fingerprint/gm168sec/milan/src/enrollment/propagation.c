/*
 * Goodix 53x5 driver for libfprint - enrollment relation propagation
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/enrollment/propagation.h"

int
goodix_milan_enrollment_propagate_lower (
  GoodixMilanRelationMatrix *matrix,
  int                             active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  size_t                          pivot,
  const int32_t                   pivot_to_reference[6],
  int32_t                         threshold,
  GoodixMilanEnrollmentOverlapFunc overlap_func,
  void                           *user_data)
{
  size_t worklist[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  size_t worklist_count = 1;

  if (!matrix || !active || !pivot_to_reference || !overlap_func ||
      pivot >= matrix->feature_count)
    return -1;
  worklist[0] = pivot;
  for (size_t candidate = pivot; candidate-- > 0;)
    {
      if (active[candidate])
        continue;
      for (size_t position = worklist_count; position-- > 0;)
        {
          size_t node = worklist[position];
          size_t child = position;
          GoodixMilanRelationSlot *slot =
            goodix_milan_enrollment_relation_slot (matrix, candidate, node);
          int32_t overlap;
          int32_t path[6];
          int32_t candidate_to_pivot[6];
          int32_t candidate_to_reference[6];

          if (!slot || slot->values[0] < 0 ||
              overlap_func (user_data, candidate, node, &overlap) != 0 ||
              overlap <= threshold)
            continue;
          memcpy (path, slot->values + 1, sizeof(path));
          /* An absent edge leaves the accumulated path at the same child. */
          for (size_t route = position; route-- > 0;)
            {
              GoodixMilanRelationSlot *route_slot =
                goodix_milan_enrollment_relation_slot (
                  matrix, worklist[route], worklist[child]);

              if (route_slot && route_slot->values[0] >= 0)
                {
                  goodix_milan_transform_compose (
                    route_slot->values + 1, path, path);
                  child = route;
                }
            }
          if (goodix_milan_transform_invert (
                path, candidate_to_pivot) != 0)
            return -1;
          goodix_milan_transform_compose (
            pivot_to_reference, candidate_to_pivot, candidate_to_reference);
          if (goodix_milan_relation_matrix_store_reference (
                matrix, candidate, candidate_to_reference) != 0)
            return -1;
          active[candidate] = 1;
          worklist[worklist_count++] = candidate;
          break;
        }
    }
  return 0;
}

int
goodix_milan_enrollment_propagate_higher (
  GoodixMilanRelationMatrix *matrix,
  int                             active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  size_t                          pivot,
  size_t                          current,
  const int32_t                   pivot_to_reference[6],
  int32_t                         threshold,
  GoodixMilanEnrollmentOverlapFunc overlap_func,
  void                           *user_data)
{
  if (!matrix || !active || !pivot_to_reference || !overlap_func ||
      pivot >= current || current > matrix->feature_count)
    return -1;
  for (size_t candidate = pivot + 1; candidate < current; candidate++)
    {
      GoodixMilanRelationSlot *slot;
      int32_t overlap;
      int32_t candidate_to_reference[6];

      if (active[candidate])
        continue;
      slot = goodix_milan_enrollment_relation_slot (matrix, candidate, pivot);
      if (!slot || slot->values[0] < 0 ||
          overlap_func (user_data, candidate, pivot, &overlap) != 0 ||
          overlap <= threshold)
        continue;
      goodix_milan_transform_compose (
        pivot_to_reference, slot->values + 1, candidate_to_reference);
      if (goodix_milan_relation_matrix_store_reference (
            matrix, candidate, candidate_to_reference) != 0)
        return -1;
      active[candidate] = 1;
    }
  return 0;
}
