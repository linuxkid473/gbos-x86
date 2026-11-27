/*
 * Goodix 53x5 driver for libfprint - Milan preprocessing state
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

#define GOODIX_MILAN_SENSOR_ROWS 88
#define GOODIX_MILAN_SENSOR_COLUMNS 108
#define GOODIX_MILAN_SENSOR_PIXELS \
  (GOODIX_MILAN_SENSOR_ROWS * GOODIX_MILAN_SENSOR_COLUMNS)
#define GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS 88
#define GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS 104
#define GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS \
  (GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS * \
   GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS)
#define GOODIX_MILAN_PREPROCESS_RETRY_RAW_ADMISSION 0x29aa
#define GOODIX_MILAN_PREPROCESS_RETRY_SETUP_ADMISSION 0x29bb
#define GOODIX_MILAN_PREPROCESS_NOT_READY 0x80
#define GOODIX_MILAN_PREPROCESS_RETRY 0x7531
#define GOODIX_MILAN_PREPROCESS_RETRY_CLASSIFICATION 0xc351

typedef enum {
  GOODIX_MILAN_PURPOSE_IDENTIFY = 0,
  GOODIX_MILAN_PURPOSE_ENROLL = 1,
} GoodixMilanPreprocessPurpose;

typedef struct
{
  uint32_t calibration_ready;
  uint32_t setup_initialized;
  uint32_t setup_refresh_pending;
  uint32_t setup_not_ready;
} GoodixMilanProfileState;

typedef struct
{
  int32_t primary_metric;
  int32_t fallback_metric;
  int32_t disagreement;
  int32_t component_score;
  int32_t component_flag;
  int32_t quality_gate;
  int32_t update_applied;
  int32_t status;
} GoodixMilanPostRenderObservations;

typedef struct
{
  uint32_t profile9_class1_count;
  uint32_t profile9_class2_count;
  uint32_t profile9_class3_count;
} GoodixMilanProfile9ClassCounts;

typedef struct
{
  uint8_t  retained_class_planes[3]
  [GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS];
  uint32_t retained_count;
  uint32_t high_class_hysteresis;
} GoodixMilanExtractionClassificationState;

typedef struct
{
  uint8_t  retained_class_planes[3]
  [GOODIX_MILAN_EXTRACTION_CLASSIFICATION_PIXELS];
  uint32_t retained_count;
} GoodixMilanExtractionPersistenceState;

typedef struct
{
  uint8_t primary_histogram_state;
  uint8_t promoted_secondary_histogram_state;
} GoodixMilanExtractionAuxiliaryState;

typedef struct
{
  uint16_t                          setup_map[GOODIX_MILAN_SENSOR_PIXELS];
  uint16_t                          calibration_map[GOODIX_MILAN_SENSOR_PIXELS];
  uint16_t                          coarse_reference[GOODIX_MILAN_SENSOR_PIXELS / 4];
  uint16_t                          auxiliary_gain_map[GOODIX_MILAN_SENSOR_PIXELS];
  uint16_t                          secondary_auxiliary_gain_map[GOODIX_MILAN_SENSOR_PIXELS];
  uint16_t                          application_gain_map[GOODIX_MILAN_SENSOR_PIXELS];
  uint32_t                          sample_count;
  uint32_t                          stable_count;
  uint32_t                          update_state;
  uint32_t                          auxiliary_sample_count;
  uint32_t                          profile9_history_count;
  uint32_t                          profile9_history_update_count;
  uint32_t                          profile9_history_mask_threshold;
  uint32_t                          profile9_history_mask_average;
  uint16_t                          profile9_history_reference[GOODIX_MILAN_SENSOR_PIXELS];
  uint8_t                           profile9_reference_age[GOODIX_MILAN_SENSOR_PIXELS];
  uint8_t                           profile9_component_age[GOODIX_MILAN_SENSOR_PIXELS];
  GoodixMilanProfile9ClassCounts    profile9_class_counts;
  uint8_t                           primary_contrast[GOODIX_MILAN_SENSOR_PIXELS];
  int32_t                           primary_contrast_valid;
  int32_t                           selected_refined;
  GoodixMilanPostRenderObservations post_render;
  /* Retained planes/hysteresis persist in generation-owned preprocess state.
   * Auxiliary bytes describe the latest completed preprocessing call and feed
   * extraction/matcher policy. */
  GoodixMilanExtractionClassificationState extraction_classification;
  /* Native serializes the retained ring before extraction appends the current
   * sample, so persistence needs the corresponding entry snapshot. */
  GoodixMilanExtractionPersistenceState extraction_persistence;
  GoodixMilanExtractionAuxiliaryState   extraction_auxiliary;
  uint8_t                               application_gain_initialized;
  uint8_t                               extraction_auxiliary_valid;
  /* Transient native auxiliary-buffer prefix, including the summary overwrite.
   * The decision writer and extraction's borrowed view both start at byte 0.
   * Neither this workspace nor its validity is serialized. */
  uint8_t extraction_auxiliary_workspace[GOODIX_MILAN_SENSOR_PIXELS];
  /* Native packet import runs only before the first completed classifier. */
  uint32_t profile9_classifier_initialized;
} GoodixMilanPreprocessState;

_Static_assert (sizeof (GoodixMilanProfile9ClassCounts) == 12,
                "Milan profile-9 class counts size changed");
_Static_assert (_Alignof (GoodixMilanProfile9ClassCounts) == 4,
                "Milan profile-9 class counts alignment changed");
_Static_assert (offsetof (GoodixMilanProfile9ClassCounts,
                          profile9_class1_count) == 0,
                "Milan profile-9 class-1 count moved");
_Static_assert (offsetof (GoodixMilanProfile9ClassCounts,
                          profile9_class2_count) == 4,
                "Milan profile-9 class-2 count moved");
_Static_assert (offsetof (GoodixMilanProfile9ClassCounts,
                          profile9_class3_count) == 8,
                "Milan profile-9 class-3 count moved");
_Static_assert (offsetof (GoodixMilanPreprocessState,
                          profile9_class_counts) == 137840,
                "Milan preprocess profile-9 class counts moved");
_Static_assert (offsetof (GoodixMilanPreprocessState,
                          profile9_class_counts) +
                offsetof (GoodixMilanProfile9ClassCounts,
                          profile9_class1_count) == 137840,
                "Milan preprocess profile-9 class-1 count moved");
_Static_assert (offsetof (GoodixMilanPreprocessState,
                          profile9_class_counts) +
                offsetof (GoodixMilanProfile9ClassCounts,
                          profile9_class2_count) == 137844,
                "Milan preprocess profile-9 class-2 count moved");
_Static_assert (offsetof (GoodixMilanPreprocessState,
                          profile9_class_counts) +
                offsetof (GoodixMilanProfile9ClassCounts,
                          profile9_class3_count) == 137848,
                "Milan preprocess profile-9 class-3 count moved");
_Static_assert (offsetof (GoodixMilanPreprocessState, primary_contrast) ==
                137852,
                "Milan preprocess primary contrast moved");
_Static_assert (offsetof (GoodixMilanPreprocessState,
                          primary_contrast_valid) == 147356,
                "Milan preprocess primary contrast validity moved");
_Static_assert (offsetof (GoodixMilanPreprocessState, selected_refined) ==
                147360,
                "Milan preprocess selected output moved");
_Static_assert (sizeof (GoodixMilanPostRenderObservations) == 32,
                "Milan post-render observations size changed");
_Static_assert (_Alignof (GoodixMilanPostRenderObservations) == 4,
                "Milan post-render observations alignment changed");
_Static_assert (offsetof (GoodixMilanPostRenderObservations, primary_metric) == 0,
                "Milan post-render primary metric moved");
_Static_assert (offsetof (GoodixMilanPostRenderObservations, fallback_metric) == 4,
                "Milan post-render fallback metric moved");
_Static_assert (offsetof (GoodixMilanPostRenderObservations, disagreement) == 8,
                "Milan post-render disagreement moved");
_Static_assert (offsetof (GoodixMilanPostRenderObservations, component_score) == 12,
                "Milan post-render component score moved");
_Static_assert (offsetof (GoodixMilanPostRenderObservations, component_flag) == 16,
                "Milan post-render component flag moved");
_Static_assert (offsetof (GoodixMilanPostRenderObservations, quality_gate) == 20,
                "Milan post-render quality gate moved");
_Static_assert (offsetof (GoodixMilanPostRenderObservations, update_applied) == 24,
                "Milan post-render update applied moved");
_Static_assert (offsetof (GoodixMilanPostRenderObservations, status) == 28,
                "Milan post-render status moved");
_Static_assert (offsetof (GoodixMilanPreprocessState, post_render) == 147364,
                "Milan preprocess post-render observations moved");
_Static_assert (offsetof (GoodixMilanPreprocessState, post_render) +
                offsetof (GoodixMilanPostRenderObservations, primary_metric) ==
                147364,
                "Milan preprocess post-render primary metric moved");
_Static_assert (offsetof (GoodixMilanPreprocessState, post_render) +
                offsetof (GoodixMilanPostRenderObservations, fallback_metric) ==
                147368,
                "Milan preprocess post-render fallback metric moved");
_Static_assert (offsetof (GoodixMilanPreprocessState, post_render) +
                offsetof (GoodixMilanPostRenderObservations, disagreement) ==
                147372,
                "Milan preprocess post-render disagreement moved");
_Static_assert (offsetof (GoodixMilanPreprocessState, post_render) +
                offsetof (GoodixMilanPostRenderObservations, component_score) ==
                147376,
                "Milan preprocess post-render component score moved");
_Static_assert (offsetof (GoodixMilanPreprocessState, post_render) +
                offsetof (GoodixMilanPostRenderObservations, component_flag) ==
                147380,
                "Milan preprocess post-render component flag moved");
_Static_assert (offsetof (GoodixMilanPreprocessState, post_render) +
                offsetof (GoodixMilanPostRenderObservations, quality_gate) ==
                147384,
                "Milan preprocess post-render quality gate moved");
_Static_assert (offsetof (GoodixMilanPreprocessState, post_render) +
                offsetof (GoodixMilanPostRenderObservations, update_applied) ==
                147388,
                "Milan preprocess post-render update applied moved");
_Static_assert (offsetof (GoodixMilanPreprocessState, post_render) +
                offsetof (GoodixMilanPostRenderObservations, status) == 147392,
                "Milan preprocess post-render status moved");
_Static_assert (sizeof (GoodixMilanExtractionClassificationState) == 27464,
                "Milan extraction classification state size changed");
_Static_assert (_Alignof (GoodixMilanExtractionClassificationState) == 4,
                "Milan extraction classification state alignment changed");
_Static_assert (offsetof (GoodixMilanPreprocessState,
                          extraction_classification) == 147396,
                "Milan extraction classification state moved");
_Static_assert (offsetof (GoodixMilanExtractionClassificationState,
                          retained_count) == 27456,
                "Milan extraction retained count moved");
_Static_assert (offsetof (GoodixMilanExtractionClassificationState,
                          high_class_hysteresis) == 27460,
                "Milan extraction hysteresis moved");
_Static_assert (sizeof (GoodixMilanExtractionPersistenceState) == 27460,
                "Milan extraction persistence state size changed");
_Static_assert (_Alignof (GoodixMilanExtractionPersistenceState) == 4,
                "Milan extraction persistence state alignment changed");
_Static_assert (offsetof (GoodixMilanExtractionPersistenceState,
                          retained_count) == 27456,
                "Milan persisted extraction retained count moved");
_Static_assert (offsetof (GoodixMilanPreprocessState,
                          extraction_persistence) == 174860,
                "Milan extraction persistence state moved");
_Static_assert (sizeof (GoodixMilanExtractionAuxiliaryState) == 2,
                "Milan extraction auxiliary state size changed");
_Static_assert (offsetof (GoodixMilanPreprocessState,
                          extraction_auxiliary) == 202320,
                "Milan extraction auxiliary state moved");
_Static_assert (offsetof (GoodixMilanPreprocessState,
                          application_gain_initialized) == 202322,
                "Milan application gain initialization state moved");
_Static_assert (offsetof (GoodixMilanPreprocessState,
                          extraction_auxiliary_workspace) == 202324,
                "Milan extraction auxiliary workspace moved");
_Static_assert (sizeof (GoodixMilanPreprocessState) == 211832,
                "Milan preprocess state size changed");
_Static_assert (_Alignof (GoodixMilanPreprocessState) == 4,
                "Milan preprocess state alignment changed");

void goodix_milan_preprocess_reset (GoodixMilanPreprocessState *state);

int goodix_milan_preprocess_clamp_and_admit_raw (uint16_t *frame,
                                                 size_t    rows,
                                                 size_t    columns,
                                                 size_t    border,
                                                 unsigned  required_percent);

int goodix_milan_preprocess (GoodixMilanPreprocessState  *state,
                             GoodixMilanProfileState     *profile_state,
                             const uint16_t              *setup_frame,
                             const uint16_t              *live_frame,
                             GoodixMilanPreprocessPurpose purpose,
                             uint8_t                     *output,
                             int                         *quality,
                             int                         *coverage);

int goodix_milan_preprocess_normalize (uint16_t *frame,
                                       size_t    rows,
                                       size_t    columns);

int goodix_milan_preprocess_make_setup_map (const uint16_t *frame,
                                            size_t          rows,
                                            size_t          columns,
                                            uint16_t       *setup_map,
                                            uint32_t       *rounded_mean);

int goodix_milan_preprocess_build_mask (const uint16_t *normalized_live,
                                        const uint16_t *setup_map,
                                        size_t          rows,
                                        size_t          columns,
                                        uint8_t        *mask,
                                        uint16_t       *threshold,
                                        uint16_t       *valid_percent);

int goodix_milan_preprocess_no_update_frame (const uint16_t *normalized_live,
                                             const uint16_t *setup_map,
                                             size_t          rows,
                                             size_t          columns,
                                             uint16_t       *output);

int goodix_milan_profile9_build_contrast_mask (const uint16_t *normalized_live,
                                               const uint16_t *setup_map,
                                               size_t          rows,
                                               size_t          columns,
                                               uint8_t        *contrast_mask,
                                               size_t         *admitted_pixels);

int goodix_milan_profile9_build_broken_mask (GoodixMilanPreprocessState *state,
                                             const uint16_t             *difference,
                                             const uint16_t             *setup_map,
                                             const uint16_t             *normalized_live,
                                             const uint8_t              *contrast_mask,
                                             size_t                      rows,
                                             size_t                      columns,
                                             uint8_t                    *broken_mask,
                                             uint8_t                    *class_plane,
                                             int                        *mode,
                                             int                        *apply_mask);

int goodix_milan_preprocess_refine (const uint16_t *source,
                                    const uint8_t  *mask,
                                    uint16_t        valid_percent,
                                    size_t          rows,
                                    size_t          columns,
                                    uint8_t        *output);

int goodix_milan_preprocess_select_output (const uint8_t *contrast,
                                           const uint8_t *refined,
                                           const uint8_t *mask,
                                           size_t         rows,
                                           size_t         columns,
                                           int            threshold,
                                           uint8_t       *output,
                                           int           *selected_refined);

int goodix_milan_preprocess_quality_mask_coverage (const uint8_t *mask,
                                                   size_t         count);

int goodix_milan_preprocess_quality_coverage_mask (const uint8_t *frame,
                                                   size_t         rows,
                                                   size_t         columns,
                                                   uint8_t       *mask,
                                                   int           *raw_coverage);
int goodix_milan_preprocess_selection_mask (const uint8_t *frame,
                                            size_t         rows,
                                            size_t         columns,
                                            uint8_t       *mask);
int goodix_milan_preprocess_quality_valid_mask (const uint8_t *frame,
                                                size_t         rows,
                                                size_t         columns,
                                                uint8_t       *mask,
                                                int           *valid_score);

int goodix_milan_preprocess_quality (const uint8_t *frame,
                                     size_t         rows,
                                     size_t         columns,
                                     int           *quality,
                                     int           *coverage);

int goodix_milan_preprocess_contrast (const uint16_t *source,
                                      const uint8_t  *mask,
                                      size_t          rows,
                                      size_t          columns,
                                      uint8_t        *output);
