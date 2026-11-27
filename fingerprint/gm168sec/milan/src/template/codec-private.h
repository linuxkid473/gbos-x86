/*
 * Goodix 53x5 driver for libfprint - Milan template codec internals
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <stdint.h>

void goodix_milan_template_write_u32 (uint8_t  *output,
                      uint32_t  value);
uint32_t goodix_milan_template_read_u32 (const uint8_t *input);
