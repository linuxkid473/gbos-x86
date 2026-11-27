/*
 * Goodix 53x5 driver for libfprint - transient profile-9 study queue
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <glib.h>
#include <stdint.h>

#include "milan/capacity.h"

#define GOODIX_STUDY_QUEUE_CAPACITY 20

typedef struct _GoodixMatchInfo GoodixMatchInfo;

typedef struct
{
  GoodixMatchInfo *info;
  gint             rank;
} GoodixStudyQueueEntry;

typedef struct _GoodixStudyQueue
{
  GoodixStudyQueueEntry entries[GOODIX_STUDY_QUEUE_CAPACITY];
  uint32_t               enabled_state;
  uint32_t               transaction_counter;
  /* Last published gallery for this retained queue; a fresh input still takes
  * serialized admission normalization. Never serialized as queue metadata. */
  GBytes          *live_gallery;
  /* Input alias when action zero advances live state without publishing bytes. */
  GBytes          *live_input;
  GoodixMatchInfo *live_features[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  /* Unencoded live +0x148 values, refreshed only at normalization boundaries. */
  int32_t          live_overlap_counts[GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY];
  gboolean         live_overlap_counts_valid;
} GoodixStudyQueue;

void goodix_milan_study_queue_clear_gallery (GoodixStudyQueue *queue);
gboolean goodix_milan_study_queue_resolve_gallery (const GoodixStudyQueue *queue,
                                                   const guint8          **feature,
                                                   gsize                  *feature_len);

typedef enum
{
  GOODIX_STUDY_QUEUE_ENQUEUED = 0,
  GOODIX_STUDY_QUEUE_DUPLICATE,
  GOODIX_STUDY_QUEUE_DISABLED,
  GOODIX_STUDY_QUEUE_INVALID,
} GoodixStudyQueueEnqueueResult;

typedef gboolean (*GoodixStudyQueueMetricFunc) (const GoodixMatchInfo *incoming,
                                                const GoodixMatchInfo *newest,
                                                gint                  *metric,
                                                gpointer               user_data);

typedef gboolean (*GoodixStudyQueueFollowupFunc) (GoodixMatchInfo *queued,
                                                  gsize            physical_slot,
                                                  gsize            triggering_index,
                                                  gsize           *selected_index,
                                                  gpointer         user_data);

GoodixStudyQueue *goodix_milan_study_queue_new (uint32_t enabled_state,
                                          uint32_t transaction_counter);
void              goodix_milan_study_queue_free (GoodixStudyQueue *queue);

gboolean goodix_milan_study_queue_validate (const GoodixStudyQueue *queue);
gsize    goodix_milan_study_queue_occupied (const GoodixStudyQueue *queue);
gsize    goodix_milan_study_queue_allocated (const GoodixStudyQueue *queue);

GoodixStudyQueueEnqueueResult goodix_milan_study_queue_enqueue (GoodixStudyQueue          *queue,
                                                                const GoodixMatchInfo     *incoming,
                                                                GoodixStudyQueueMetricFunc metric_func,
                                                                gpointer                   user_data);

gboolean goodix_milan_study_queue_process (GoodixStudyQueue            *queue,
                                           gsize                        primary_selected_index,
                                           GoodixStudyQueueFollowupFunc followup_func,
                                           gpointer                     user_data,
                                           gboolean                    *mutated);

void goodix_milan_study_queue_disable (GoodixStudyQueue *queue);
