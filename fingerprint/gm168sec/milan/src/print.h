/*
 * Goodix 53x5 driver for libfprint - native Milan persisted print format
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <glib.h>

#include "milan/milan.h"

#define GOODIX_MILAN_PRINT_SCHEMA        4U
#define GOODIX_MILAN_PRINT_PROFILE       9U
#define GOODIX_MILAN_PRINT_SENSOR_TYPE   12U
#define GOODIX_MILAN_PRINT_ANTIFAKE_MODE 1U
#define GOODIX_MILAN_PRINT_BOUNDARY_POLICY "canonical-zero-v1"
#define GOODIX_MILAN_PRINT_MAX_SIZE      (1024U * 1024U)

typedef enum
{
  GOODIX_MILAN_PRINT_ERROR_INVALID,
  GOODIX_MILAN_PRINT_ERROR_INCOMPATIBLE,
  GOODIX_MILAN_PRINT_ERROR_TOO_LARGE,
  GOODIX_MILAN_PRINT_ERROR_NONCANONICAL,
} GoodixMilanPrintError;

#define GOODIX_MILAN_PRINT_ERROR (goodix_milan_print_error_quark ())
GQuark goodix_milan_print_error_quark (void);

typedef struct
{
  gsize    byte_size;
  guint32  feature_count;
  guint32  partition0_count;
  guint32  partition1_count;
  guint32  relation_count;
  guint32  maximum_features;
  guint32  registration_count;
  guint32  maximum_records;
  guint32  queue_state;
  guint32  queue_transaction_counter;
  gint32   graph_reference_index;
  gint32   graph_companion_f3;
  gint32   graph_companion_f4;
  guint32  graph_established;
} GoodixMilanPrintTemplateInfo;

gboolean goodix_milan_print_validate_template (
  GBytes                       *template_bytes,
  GoodixMilanPrintTemplateInfo *info,
  GError                      **error);

/* gbOS86-533c: build_data / parse_data removed (GVariant envelope,
 * see print.c note). Templates are stored verbatim. */
