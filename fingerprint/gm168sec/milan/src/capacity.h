/*
 * Goodix 53x5 driver for libfprint - Milan template capacities
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#define GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY 50
#define GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT 40
#define GOODIX_MILAN_TEMPLATE_RELATION_CAPACITY \
  (GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY * \
   (GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY - 1) / 2)
