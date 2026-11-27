/*
 * Goodix 53x5 driver for libfprint - Milan anti-fake interface
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define GOODIX_MILAN_ANTIFAKE_AMBIGUOUS 1
#define GOODIX_MILAN_ANTIFAKE_SIZE 0x1abcU
#define GOODIX_MILAN_ANTIFAKE_DEFINED_MATERIAL_SIZE GOODIX_MILAN_ANTIFAKE_SIZE
#define GOODIX_MILAN_ANTIFAKE_RECORD_SIZE 48U
#define GOODIX_MILAN_ANTIFAKE_RECORD_CAPACITY 100U
#define GOODIX_MILAN_ANTIFAKE_RECORDS_OFFSET 0U
#define GOODIX_MILAN_ANTIFAKE_RECORD_X_OFFSET 0U
#define GOODIX_MILAN_ANTIFAKE_RECORD_Y_OFFSET 4U
#define GOODIX_MILAN_ANTIFAKE_RECORD_DATA_8_OFFSET 8U
#define GOODIX_MILAN_ANTIFAKE_RECORD_DATA_16_OFFSET 16U
#define GOODIX_MILAN_ANTIFAKE_RECORD_DATA_24_OFFSET 24U
#define GOODIX_MILAN_ANTIFAKE_RECORD_DATA_32_OFFSET 32U
#define GOODIX_MILAN_ANTIFAKE_RECORD_DATA_40_OFFSET 40U
#define GOODIX_MILAN_ANTIFAKE_RECORD_DATA_44_OFFSET 44U
#define GOODIX_MILAN_ANTIFAKE_RECORDS_SIZE \
  (GOODIX_MILAN_ANTIFAKE_RECORD_SIZE * GOODIX_MILAN_ANTIFAKE_RECORD_CAPACITY)
#define GOODIX_MILAN_ANTIFAKE_CANDIDATE_COUNT_OFFSET 0x12c0U
#define GOODIX_MILAN_ANTIFAKE_TEXTURE_OFFSET 0x12c4U
#define GOODIX_MILAN_ANTIFAKE_MEAN_OFFSET 0x12c8U
#define GOODIX_MILAN_ANTIFAKE_VARIATION_OFFSET 0x12ccU
#define GOODIX_MILAN_ANTIFAKE_BOUNDARY_OFFSET 0x12d0U
#define GOODIX_MILAN_ANTIFAKE_MODEL_OFFSET 0x12d8U
#define GOODIX_MILAN_ANTIFAKE_MASK_OFFSET 0x12dcU
#define GOODIX_MILAN_ANTIFAKE_MASK_SIZE 2000U
#define GOODIX_MILAN_ANTIFAKE_CALIBRATION_SCALAR_OFFSET 0x1aacU
#define GOODIX_MILAN_ANTIFAKE_THRESHOLD_OFFSET 0x1ab0U
#define GOODIX_MILAN_ANTIFAKE_PAIR_SCORE_OFFSET 0x1ab4U

typedef struct _GoodixMilanFeatureRecord GoodixMilanFeatureRecord;

typedef struct
{
  uint8_t bytes[GOODIX_MILAN_ANTIFAKE_SIZE];
} GoodixMilanAntifakeBlob;

_Static_assert (sizeof (GoodixMilanAntifakeBlob) == GOODIX_MILAN_ANTIFAKE_SIZE,
                "Milan anti-fake blob size changed");
_Static_assert (_Alignof (GoodixMilanAntifakeBlob) == _Alignof (uint8_t),
                "Milan anti-fake blob alignment changed");
_Static_assert (offsetof (GoodixMilanAntifakeBlob, bytes) == 0,
                "Milan anti-fake blob data moved");
_Static_assert (GOODIX_MILAN_ANTIFAKE_RECORDS_SIZE ==
                GOODIX_MILAN_ANTIFAKE_CANDIDATE_COUNT_OFFSET,
                "Milan anti-fake record extent changed");
_Static_assert (GOODIX_MILAN_ANTIFAKE_RECORD_DATA_44_OFFSET + 4U ==
                GOODIX_MILAN_ANTIFAKE_RECORD_SIZE,
                "Milan anti-fake record layout changed");
_Static_assert (GOODIX_MILAN_ANTIFAKE_MASK_OFFSET +
                GOODIX_MILAN_ANTIFAKE_MASK_SIZE ==
                GOODIX_MILAN_ANTIFAKE_CALIBRATION_SCALAR_OFFSET,
                "Milan anti-fake mask extent changed");
_Static_assert (GOODIX_MILAN_ANTIFAKE_PAIR_SCORE_OFFSET + sizeof (int32_t) <=
                GOODIX_MILAN_ANTIFAKE_SIZE,
                "Milan anti-fake pair score exceeds the blob");

static inline uint8_t *
goodix_milan_antifake_data (GoodixMilanAntifakeBlob *blob)
{
  return blob ? blob->bytes : NULL;
}

static inline const uint8_t *
goodix_milan_antifake_const_data (const GoodixMilanAntifakeBlob *blob)
{
  return blob ? blob->bytes : NULL;
}

static inline uint8_t *
goodix_milan_antifake_record (GoodixMilanAntifakeBlob *blob,
                              size_t                   index)
{
  return blob->bytes + GOODIX_MILAN_ANTIFAKE_RECORDS_OFFSET +
         index * GOODIX_MILAN_ANTIFAKE_RECORD_SIZE;
}

static inline const uint8_t *
goodix_milan_antifake_const_record (const GoodixMilanAntifakeBlob *blob,
                                    size_t                         index)
{
  return blob->bytes + GOODIX_MILAN_ANTIFAKE_RECORDS_OFFSET +
         index * GOODIX_MILAN_ANTIFAKE_RECORD_SIZE;
}

static inline int32_t
goodix_milan_antifake_record_x (const uint8_t *record)
{
  int32_t value;

  memcpy (&value, record + GOODIX_MILAN_ANTIFAKE_RECORD_X_OFFSET,
          sizeof (value));
  return value;
}

static inline void
goodix_milan_antifake_set_record_x (uint8_t *record,
                                    int32_t  value)
{
  memcpy (record + GOODIX_MILAN_ANTIFAKE_RECORD_X_OFFSET, &value,
          sizeof (value));
}

static inline int32_t
goodix_milan_antifake_record_y (const uint8_t *record)
{
  int32_t value;

  memcpy (&value, record + GOODIX_MILAN_ANTIFAKE_RECORD_Y_OFFSET,
          sizeof (value));
  return value;
}

static inline void
goodix_milan_antifake_set_record_y (uint8_t *record,
                                    int32_t  value)
{
  memcpy (record + GOODIX_MILAN_ANTIFAKE_RECORD_Y_OFFSET, &value,
          sizeof (value));
}

static inline uint8_t *
goodix_milan_antifake_mask (GoodixMilanAntifakeBlob *blob)
{
  return blob->bytes + GOODIX_MILAN_ANTIFAKE_MASK_OFFSET;
}

static inline const uint8_t *
goodix_milan_antifake_const_mask (const GoodixMilanAntifakeBlob *blob)
{
  return blob->bytes + GOODIX_MILAN_ANTIFAKE_MASK_OFFSET;
}

static inline uint32_t
goodix_milan_antifake_candidate_count (const GoodixMilanAntifakeBlob *blob)
{
  uint32_t value;

  memcpy (&value, blob->bytes + GOODIX_MILAN_ANTIFAKE_CANDIDATE_COUNT_OFFSET,
          sizeof (value));
  return value;
}

static inline void
goodix_milan_antifake_set_candidate_count (GoodixMilanAntifakeBlob *blob,
                                           uint32_t                 value)
{
  memcpy (blob->bytes + GOODIX_MILAN_ANTIFAKE_CANDIDATE_COUNT_OFFSET, &value,
          sizeof (value));
}

#define GOODIX_MILAN_ANTIFAKE_SCALAR_ACCESSORS(name, offset) \
  static inline int32_t \
  goodix_milan_antifake_ ## name (const GoodixMilanAntifakeBlob * blob) \
  { \
    int32_t value; \
    memcpy (&value, blob->bytes + (offset), sizeof (value)); \
    return value; \
  } \
  static inline void \
  goodix_milan_antifake_set_ ## name (GoodixMilanAntifakeBlob * blob, \
                                      int32_t value) \
  { \
    memcpy (blob->bytes + (offset), &value, sizeof (value)); \
  }

GOODIX_MILAN_ANTIFAKE_SCALAR_ACCESSORS (texture,
                                        GOODIX_MILAN_ANTIFAKE_TEXTURE_OFFSET)
GOODIX_MILAN_ANTIFAKE_SCALAR_ACCESSORS (mean,
                                        GOODIX_MILAN_ANTIFAKE_MEAN_OFFSET)
GOODIX_MILAN_ANTIFAKE_SCALAR_ACCESSORS (variation,
                                        GOODIX_MILAN_ANTIFAKE_VARIATION_OFFSET)
GOODIX_MILAN_ANTIFAKE_SCALAR_ACCESSORS (boundary,
                                        GOODIX_MILAN_ANTIFAKE_BOUNDARY_OFFSET)
GOODIX_MILAN_ANTIFAKE_SCALAR_ACCESSORS (model,
                                        GOODIX_MILAN_ANTIFAKE_MODEL_OFFSET)
GOODIX_MILAN_ANTIFAKE_SCALAR_ACCESSORS (
  calibration_scalar, GOODIX_MILAN_ANTIFAKE_CALIBRATION_SCALAR_OFFSET)
GOODIX_MILAN_ANTIFAKE_SCALAR_ACCESSORS (threshold,
                                        GOODIX_MILAN_ANTIFAKE_THRESHOLD_OFFSET)
GOODIX_MILAN_ANTIFAKE_SCALAR_ACCESSORS (pair_score,
                                        GOODIX_MILAN_ANTIFAKE_PAIR_SCORE_OFFSET)

#undef GOODIX_MILAN_ANTIFAKE_SCALAR_ACCESSORS

typedef enum {
  GOODIX_MILAN_ANTIFAKE_BOUNDARY_NOT_EVALUATED,
  GOODIX_MILAN_ANTIFAKE_BOUNDARY_STABLE,
  GOODIX_MILAN_ANTIFAKE_BOUNDARY_AMBIGUOUS,
} GoodixMilanAntifakeBoundaryClass;

typedef struct
{
  GoodixMilanAntifakeBoundaryClass classification;
  uint32_t                         zero_candidate_count;
  uint32_t                         nonzero_candidate_count;
  GoodixMilanAntifakeBlob          zero_projection;
  GoodixMilanAntifakeBlob          nonzero_projection;
} GoodixMilanAntifakeBoundaryResult;

int goodix_milan_antifake_residual (const uint16_t *calibration,
                                    const uint16_t *raw_frame,
                                    size_t          rows,
                                    size_t          columns,
                                    int32_t         sensor_offset,
                                    uint16_t        chip_type,
                                    uint16_t       *residual);

int goodix_milan_antifake_impulse_filter (uint16_t *residual,
                                          size_t    rows,
                                          size_t    columns,
                                          int32_t  *threshold);

int goodix_milan_antifake_statistics (const uint16_t *residual,
                                      const uint8_t  *mask,
                                      size_t          rows,
                                      size_t          columns,
                                      int32_t        *texture,
                                      int32_t        *mean);

int goodix_milan_antifake_block_variation (const uint16_t *residual,
                                           const uint8_t  *mask,
                                           size_t          rows,
                                           size_t          columns,
                                           size_t          block_size,
                                           int32_t        *variation);

int goodix_milan_antifake_model_score (const int32_t vector[51],
                                       int32_t      *score);

int goodix_milan_antifake_model_vector (const uint16_t *residual,
                                        const uint8_t  *mask,
                                        size_t          rows,
                                        size_t          columns,
                                        size_t          border,
                                        int32_t         vector[51]);

int goodix_milan_antifake_class_map (const uint8_t *image,
                                     const uint8_t *mask,
                                     size_t         rows,
                                     size_t         columns,
                                     uint8_t       *classes);

int goodix_milan_antifake_boundary_score (const uint16_t *residual,
                                          const uint8_t  *classes,
                                          size_t          rows,
                                          size_t          columns,
                                          int32_t        *score);

int goodix_milan_antifake_build (const uint16_t          *calibration,
                                 const uint16_t          *raw_frame,
                                 const uint8_t           *classification_plane,
                                 const uint8_t           *feature_mask,
                                 size_t                   feature_mask_size,
                                 size_t                   rows,
                                 size_t                   columns,
                                 uint16_t                 t_code,
                                 uint16_t                 dac_high,
                                 uint16_t                 dac_low,
                                 uint16_t                 chip_type,
                                 int32_t                  calibration_scalar,
                                 GoodixMilanAntifakeBlob *antifake,
                                 size_t                   antifake_size);

int goodix_milan_antifake_build_with_boundary (const uint16_t                    *calibration,
                                               const uint16_t                    *raw_frame,
                                               const uint8_t                     *classification_plane,
                                               const uint8_t                     *feature_mask,
                                               size_t                             feature_mask_size,
                                               size_t                             rows,
                                               size_t                             columns,
                                               uint16_t                           t_code,
                                               uint16_t                           dac_high,
                                               uint16_t                           dac_low,
                                               uint16_t                           chip_type,
                                               int32_t                            calibration_scalar,
                                               GoodixMilanAntifakeBlob           *antifake,
                                               size_t                             antifake_size,
                                               GoodixMilanAntifakeBoundaryResult *boundary_result);

int goodix_milan_antifake_score_pair (const GoodixMilanAntifakeBlob *prior,
                                      size_t                         prior_size,
                                      const GoodixMilanAntifakeBlob *current,
                                      size_t                         current_size,
                                      const int32_t                  current_to_prior[6],
                                      int32_t                       *score);

int goodix_milan_antifake_pair_metrics (const GoodixMilanAntifakeBlob *prior,
                                        size_t                         prior_size,
                                        const GoodixMilanAntifakeBlob *current,
                                        size_t                         current_size,
                                        const int32_t                  current_to_prior[6],
                                        int32_t                        metrics[5]);

int goodix_milan_antifake_feature_update (GoodixMilanAntifakeBlob *antifake,
                                          size_t                   antifake_size,
                                          const uint16_t          *source,
                                          size_t                   rows,
                                          size_t                   columns);

int goodix_milan_antifake_feature_maps (const uint16_t *source,
                                        size_t          rows,
                                        size_t          columns,
                                        uint8_t        *dense_orientation,
                                        uint32_t       *magnitude,
                                        int16_t        *gradient_orientation);

int goodix_milan_antifake_feature_record (const uint8_t            *dense_orientation,
                                          const uint32_t           *magnitude,
                                          const int16_t            *gradient_orientation,
                                          size_t                    rows,
                                          size_t                    columns,
                                          int32_t                   x,
                                          int32_t                   y,
                                          GoodixMilanFeatureRecord *record);

int goodix_milan_antifake_build_mask (const uint8_t *feature_mask,
                                      size_t         feature_mask_size,
                                      size_t         rows,
                                      size_t         columns,
                                      uint8_t       *mask,
                                      uint8_t       *packed,
                                      size_t         packed_size);
