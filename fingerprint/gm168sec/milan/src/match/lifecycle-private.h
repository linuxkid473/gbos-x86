/*
 * Goodix 53x5 driver for libfprint - Milan serialized match lifecycle internals
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "milan/match/match.h"
#include "milan/study/queue.h"

gboolean goodix_milan_match_queue_matches_template (const GoodixStudyQueue *queue,
                                               const guint8           *feature,
                                               gsize                   feature_len);

GoodixSigfmTemplateStatus goodix_milan_match_serialized_feature_result_internal (GoodixMatchInfo                *probe_info,
                                                                                 const guint8                   *feature,
                                                                                 gsize                           feature_len,
                                                                                 GoodixMilanMatchResult         *match_result,
                                                                                 GBytes                        **updated_feature,
#ifdef GOODIX53X5_DEBUG
  GoodixMilanMatchDiagnostics *diagnostics,
#endif
  GoodixStudyQueue            *queue,
  gboolean                     normalize,
  GoodixStudyQueueEnqueueResult (*enqueue_candidate) (
    GoodixStudyQueue      *queue,
    const GoodixMatchInfo *probe_info));
