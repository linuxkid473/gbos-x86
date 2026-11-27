/*
 * Goodix 53x5 driver for libfprint - profile-9 gain-tail state
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/preprocess/gain.h"

void
goodix_milan_profile9_update_gain_ready (uint32_t  update_state,
                                         uint32_t  update_counter,
                                         uint32_t *ready)
{
  if (update_state == 0 && update_counter > 5)
    *ready = 1;
  if (update_state > 14)
    *ready = 0;
}

uint16_t
goodix_milan_profile9_combine_gain (uint16_t gain,
                                    uint16_t application_gain,
                                    uint16_t auxiliary_gain,
                                    uint32_t ready)
{
  uint32_t combined =
    ((uint32_t) gain * application_gain + GOODIX_MILAN_GAIN_ONE / 2) >> 13;

  if (!ready)
    combined =
      (combined * auxiliary_gain + GOODIX_MILAN_GAIN_ONE / 2) >> 13;
  return (uint16_t) combined;
}
