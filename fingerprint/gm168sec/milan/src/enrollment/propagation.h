/*
 * Goodix 53x5 driver for libfprint - enrollment relation propagation
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "milan/relations.h"

typedef int (*GoodixMilanEnrollmentOverlapFunc) (void    *user_data,
                                                  size_t   first,
                                                  size_t   second,
                                                  int32_t *overlap);

GoodixMilanRelationSlot *
goodix_milan_enrollment_relation_slot (GoodixMilanRelationMatrix *matrix,
                                       size_t first,
                                       size_t second);

int
goodix_milan_enrollment_feature_to_reference (
  GoodixMilanRelationMatrix *matrix,
  size_t                          feature,
  int32_t                         transform[6]);

int
goodix_milan_enrollment_propagate_lower (
  GoodixMilanRelationMatrix *matrix,
  int                             active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  size_t                          pivot,
  const int32_t                   pivot_to_reference[6],
  int32_t                         threshold,
  GoodixMilanEnrollmentOverlapFunc overlap_func,
  void                           *user_data);

int
goodix_milan_enrollment_propagate_higher (
  GoodixMilanRelationMatrix *matrix,
  int                             active[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY],
  size_t                          pivot,
  size_t                          current,
  const int32_t                   pivot_to_reference[6],
  int32_t                         threshold,
  GoodixMilanEnrollmentOverlapFunc overlap_func,
  void                           *user_data);
