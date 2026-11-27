/*
 * Goodix 53x5 driver for libfprint - native Milan persisted print format
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/print.h"
#include "milan/relations.h"
#include "milan/template/codec-private.h"

#include <string.h>

#define GOODIX_MILAN_PACK_MAX_RECORDS  150U

G_DEFINE_QUARK (goodix-milan-print-error, goodix_milan_print_error)

static gboolean
goodix_milan_print_fail (GError                **error,
                         GoodixMilanPrintError   code,
                         const gchar            *message)
{
  g_set_error_literal (error, GOODIX_MILAN_PRINT_ERROR, code, message);
  return FALSE;
}

gboolean
goodix_milan_print_validate_template (GBytes                       *template_bytes,
                                      GoodixMilanPrintTemplateInfo *info,
                                      GError                      **error)
{
  g_autofree guint8 *repacked = NULL;
  g_autofree GoodixMilanUnpackedTemplate *unpacked = NULL;
  const guint8 *packed;
  gsize packed_size;
  size_t repacked_size = 0;
  guint32 partition0 = 0;
  guint32 partition1 = 0;
  gint32 row_bases[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { 0 };
  gboolean order_seen[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY] = { FALSE };
  GoodixMilanRelationMatrix relation_matrix;

  if (info)
    memset (info, 0, sizeof(*info));
  if (!template_bytes)
    return goodix_milan_print_fail (
      error, GOODIX_MILAN_PRINT_ERROR_INVALID, "Milan template is missing");

  packed = g_bytes_get_data (template_bytes, &packed_size);
  if (packed_size > GOODIX_MILAN_PRINT_MAX_SIZE)
    return goodix_milan_print_fail (
      error, GOODIX_MILAN_PRINT_ERROR_TOO_LARGE, "Milan template is too large");
  unpacked = g_new (GoodixMilanUnpackedTemplate, 1);
  if (goodix_milan_template_unpack (packed, packed_size, unpacked) != 0)
    return goodix_milan_print_fail (
      error, GOODIX_MILAN_PRINT_ERROR_INVALID, "Milan template cannot be unpacked");

  if (unpacked->metadata.sensor_type != GOODIX_MILAN_PRINT_SENSOR_TYPE ||
      unpacked->metadata.maximum_features != GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT ||
      unpacked->metadata.maximum_records != GOODIX_MILAN_PACK_MAX_RECORDS ||
      unpacked->feature_count == 0 ||
      unpacked->feature_count > GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT ||
      unpacked->metadata.registration_count == 0 ||
      unpacked->metadata.registration_count >
        1U + GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT *
               (GOODIX_MILAN_PROFILE9_ACTIVE_FEATURE_LIMIT - 1U) / 2U ||
      unpacked->relation_count >
        unpacked->feature_count * (unpacked->feature_count - 1U) / 2U)
    return goodix_milan_print_fail (
      error, GOODIX_MILAN_PRINT_ERROR_INVALID,
      "Milan template profile-9 counts are invalid");

  for (gsize i = 0; i < unpacked->feature_count; i++)
    {
      guint32 feature_index = goodix_milan_template_read_u32 (
        unpacked->tail_state + i * 4);

      if (feature_index >= unpacked->feature_count || order_seen[feature_index])
        return goodix_milan_print_fail (
          error, GOODIX_MILAN_PRINT_ERROR_INVALID,
          "Milan template order is invalid");
      order_seen[feature_index] = TRUE;
    }

  for (gsize i = 0; i < unpacked->feature_count; i++)
    {
      GoodixMilanFeatureView view;

      if (goodix_milan_template_parse_feature_element (
            unpacked->feature_elements[i], unpacked->feature_element_sizes[i],
            &view) != 0 || view.record_count == 0 ||
          view.record_count > GOODIX_MILAN_PACK_MAX_RECORDS ||
          view.fields.tagged_values[2] < 0 ||
          (gsize) view.fields.tagged_values[2] > view.record_count)
        return goodix_milan_print_fail (
          error, GOODIX_MILAN_PRINT_ERROR_INVALID,
          "Milan template contains an invalid feature");
      partition0 += (guint32) view.fields.tagged_values[2];
      partition1 += (guint32) view.record_count -
                    (guint32) view.fields.tagged_values[2];
      row_bases[i] = view.fields.tagged_values[1];
    }

  if (goodix_milan_relation_matrix_init (
        &relation_matrix, unpacked->feature_count, row_bases,
        unpacked->metadata.registration_count,
        unpacked->metadata.graph_reference_index,
        (gint32) unpacked->metadata.graph_established, unpacked->relations,
        unpacked->relation_count) != 0)
    return goodix_milan_print_fail (
      error, GOODIX_MILAN_PRINT_ERROR_INVALID,
      "Milan template graph is invalid");

  repacked = g_malloc (packed_size);
  if (goodix_milan_template_pack (
        unpacked->feature_elements, unpacked->feature_element_sizes,
        unpacked->feature_count, unpacked->relations, unpacked->relation_count,
        &unpacked->metadata, unpacked->tail_state, sizeof(unpacked->tail_state),
        repacked, packed_size, &repacked_size) != 0 ||
      repacked_size != packed_size || memcmp (repacked, packed, packed_size) != 0)
    return goodix_milan_print_fail (
      error, GOODIX_MILAN_PRINT_ERROR_NONCANONICAL,
      "Milan template is not in canonical packed form");

  if (info)
    {
      info->byte_size = packed_size;
      info->feature_count = (guint32) unpacked->feature_count;
      info->partition0_count = partition0;
      info->partition1_count = partition1;
      info->relation_count = (guint32) unpacked->relation_count;
      info->maximum_features = unpacked->metadata.maximum_features;
      info->registration_count = unpacked->metadata.registration_count;
      info->maximum_records = unpacked->metadata.maximum_records;
      info->queue_state = unpacked->metadata.queue_state;
      info->queue_transaction_counter =
        unpacked->metadata.queue_transaction_counter;
      info->graph_reference_index = unpacked->metadata.graph_reference_index;
      info->graph_companion_f3 = unpacked->metadata.graph_companion_f3;
      info->graph_companion_f4 = unpacked->metadata.graph_companion_f4;
      info->graph_established = unpacked->metadata.graph_established;
    }
  return TRUE;
}

/* gbOS86-533c: goodix_milan_print_build_data / parse_data REMOVED.
 * They wrap templates in a GVariant "(uuuusay)" FpPrint envelope
 * (print.c:149-240 upstream); Android has no libfprint FpPrint to
 * interoperate with, and GVariant is not in the glib shim. Templates
 * are stored verbatim (see milan_matcher.c). validate_template above
 * is kept - runtime.c calls it. */
