/*
 * Goodix 53x5 driver for libfprint - transient profile-9 study queue
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/match/match.h"
#include "milan/study/queue.h"

#include <stdint.h>
#include <string.h>

GoodixStudyQueue *
goodix_milan_study_queue_new (uint32_t enabled_state,
                        uint32_t transaction_counter)
{
  GoodixStudyQueue *queue = g_new0 (GoodixStudyQueue, 1);

  if (enabled_state > 1)
    {
      g_free (queue);
      return NULL;
    }
  queue->enabled_state = enabled_state;
  queue->transaction_counter = transaction_counter;
  if (enabled_state == 1)
    {
      for (gsize i = 0; i < GOODIX_STUDY_QUEUE_CAPACITY; i++)
        queue->entries[i].rank = -1;
      return queue;
    }
  for (gsize i = 0; i < GOODIX_STUDY_QUEUE_CAPACITY; i++)
    {
      queue->entries[i].info = goodix_milan_match_info_new_empty ();
      queue->entries[i].rank = -1;
    }
  return queue;
}

void
goodix_milan_study_queue_clear_gallery (GoodixStudyQueue *queue)
{
  if (!queue)
    return;
  g_clear_pointer (&queue->live_gallery, g_bytes_unref);
  g_clear_pointer (&queue->live_input, g_bytes_unref);
  for (gsize i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    g_clear_pointer (&queue->live_features[i], goodix_milan_match_free_info);
  queue->live_overlap_counts_valid = FALSE;
}

gboolean
goodix_milan_study_queue_resolve_gallery (const GoodixStudyQueue *queue,
                                          const guint8          **feature,
                                          gsize                  *feature_len)
{
  if (!queue || !queue->live_gallery || !feature || !*feature || !feature_len)
    return FALSE;
  GBytes *identities[2] = { queue->live_gallery, queue->live_input };
  for (gsize i = 0; i < G_N_ELEMENTS (identities); i++)
    if (identities[i] && g_bytes_get_size (identities[i]) == *feature_len &&
        memcmp (g_bytes_get_data (identities[i], NULL), *feature, *feature_len) == 0)
      {
        *feature = g_bytes_get_data (queue->live_gallery, feature_len);
        return TRUE;
      }
  return FALSE;
}

void
goodix_milan_study_queue_free (GoodixStudyQueue *queue)
{
  if (!queue)
    return;
  for (gsize i = 0; i < GOODIX_STUDY_QUEUE_CAPACITY; i++)
    goodix_milan_match_free_info (queue->entries[i].info);
  goodix_milan_study_queue_clear_gallery (queue);
  g_free (queue);
}

gsize
goodix_milan_study_queue_occupied (const GoodixStudyQueue *queue)
{
  gsize occupied = 0;

  if (!queue)
    return 0;
  for (gsize i = 0; i < GOODIX_STUDY_QUEUE_CAPACITY; i++)
    occupied += queue->entries[i].rank >= 0;
  return occupied;
}

gsize
goodix_milan_study_queue_allocated (const GoodixStudyQueue *queue)
{
  gsize allocated = 0;

  if (!queue)
    return 0;
  for (gsize i = 0; i < GOODIX_STUDY_QUEUE_CAPACITY; i++)
    allocated += queue->entries[i].info != NULL;
  return allocated;
}

gboolean
goodix_milan_study_queue_validate (const GoodixStudyQueue *queue)
{
  gboolean seen[GOODIX_STUDY_QUEUE_CAPACITY] = { FALSE };
  gsize occupied;

  if (!queue || queue->enabled_state > 1)
    return FALSE;
  if (!queue->live_gallery &&
      (queue->live_input || queue->live_overlap_counts_valid))
    return FALSE;
  for (gsize i = 0; i < GOODIX_MILAN_TEMPLATE_FEATURE_CAPACITY; i++)
    if (queue->live_features[i] &&
        (!queue->live_gallery ||
         !goodix_milan_match_info_is_complete (queue->live_features[i])))
      return FALSE;
  occupied = goodix_milan_study_queue_occupied (queue);
  if (queue->enabled_state == 1)
    {
      if (occupied != 0 || goodix_milan_study_queue_allocated (queue) != 0)
        return FALSE;
      for (gsize i = 0; i < GOODIX_STUDY_QUEUE_CAPACITY; i++)
        if (queue->entries[i].rank != -1)
          return FALSE;
      return TRUE;
    }
  if (goodix_milan_study_queue_allocated (queue) != GOODIX_STUDY_QUEUE_CAPACITY)
    return FALSE;

  for (gsize i = 0; i < GOODIX_STUDY_QUEUE_CAPACITY; i++)
    {
      gint rank = queue->entries[i].rank;
      gboolean complete = goodix_milan_match_info_is_complete (
        queue->entries[i].info);

      if (rank < -1 || rank >= GOODIX_STUDY_QUEUE_CAPACITY ||
          (rank >= 0) != complete)
        return FALSE;
      if (rank >= 0)
        {
          if ((gsize) rank >= occupied || seen[rank])
            return FALSE;
          seen[rank] = TRUE;
        }
    }
  for (gsize rank = 0; rank < occupied; rank++)
    if (!seen[rank])
      return FALSE;
  return TRUE;
}

GoodixStudyQueueEnqueueResult
goodix_milan_study_queue_enqueue (GoodixStudyQueue          *queue,
                                  const GoodixMatchInfo     *incoming,
                                  GoodixStudyQueueMetricFunc metric_func,
                                  gpointer                   user_data)
{
  gsize occupied;
  gsize newest_slot = SIZE_MAX;
  gsize destination = SIZE_MAX;

  if (!queue || !incoming || !goodix_milan_match_info_is_complete (incoming) ||
      !goodix_milan_study_queue_validate (queue))
    return GOODIX_STUDY_QUEUE_INVALID;
  if (queue->enabled_state != 0)
    return GOODIX_STUDY_QUEUE_DISABLED;

  occupied = goodix_milan_study_queue_occupied (queue);
  for (gsize i = 0; i < GOODIX_STUDY_QUEUE_CAPACITY; i++)
    {
      if (queue->entries[i].rank == (gint) occupied - 1)
        newest_slot = i;
      if (destination == SIZE_MAX && queue->entries[i].rank == -1)
        destination = i;
    }
  if (occupied != 0)
    {
      gint metric;

      if (newest_slot == SIZE_MAX || !metric_func ||
          !metric_func (incoming, queue->entries[newest_slot].info, &metric,
                        user_data))
        return GOODIX_STUDY_QUEUE_INVALID;
      if (metric > 190)
        return GOODIX_STUDY_QUEUE_DUPLICATE;
    }

  if (destination == SIZE_MAX)
    for (gsize i = 0; i < GOODIX_STUDY_QUEUE_CAPACITY; i++)
      if (queue->entries[i].rank == 0)
        {
          destination = i;
          break;
        }
  if (destination == SIZE_MAX || !goodix_milan_match_info_copy (
        queue->entries[destination].info, incoming))
    return GOODIX_STUDY_QUEUE_INVALID;

  if (occupied == GOODIX_STUDY_QUEUE_CAPACITY)
    {
      for (gsize i = 0; i < GOODIX_STUDY_QUEUE_CAPACITY; i++)
        queue->entries[i].rank--;
      queue->entries[destination].rank = GOODIX_STUDY_QUEUE_CAPACITY - 1;
    }
  else
    queue->entries[destination].rank = (gint) occupied;
  return GOODIX_STUDY_QUEUE_ENQUEUED;
}

gboolean
goodix_milan_study_queue_process (GoodixStudyQueue            *queue,
                                  gsize                        primary_selected_index,
                                  GoodixStudyQueueFollowupFunc followup_func,
                                  gpointer                     user_data,
                                  gboolean                    *mutated)
{
  gsize continuation[GOODIX_STUDY_QUEUE_CAPACITY];
  gsize head = 0;
  gsize tail = 1;
  gsize pending = 1;

  if (mutated)
    *mutated = FALSE;
  if (!queue || !followup_func || !mutated ||
      primary_selected_index == SIZE_MAX ||
      !goodix_milan_study_queue_validate (queue) || queue->enabled_state != 0)
    return FALSE;
  continuation[0] = primary_selected_index;

  while (pending != 0)
    {
      gsize triggering_index = continuation[head];

      head = (head + 1) % GOODIX_STUDY_QUEUE_CAPACITY;
      pending--;
      for (gsize slot = 0; slot < GOODIX_STUDY_QUEUE_CAPACITY; slot++)
        {
          gint consumed_rank;
          gsize selected_index = SIZE_MAX;

          if (queue->entries[slot].rank < 0)
            continue;
          if (!followup_func (queue->entries[slot].info, slot,
                              triggering_index, &selected_index, user_data))
            return FALSE;
          if (selected_index == SIZE_MAX)
            continue;

          consumed_rank = queue->entries[slot].rank;
          goodix_milan_match_info_clear (queue->entries[slot].info);
          queue->entries[slot].rank = -1;
          for (gsize i = 0; i < GOODIX_STUDY_QUEUE_CAPACITY; i++)
            if (queue->entries[i].rank > consumed_rank)
              queue->entries[i].rank--;
          if (pending == GOODIX_STUDY_QUEUE_CAPACITY)
            return FALSE;
          continuation[tail] = selected_index;
          tail = (tail + 1) % GOODIX_STUDY_QUEUE_CAPACITY;
          pending++;
          *mutated = TRUE;
          if (selected_index == triggering_index)
            break;
        }
    }
  return goodix_milan_study_queue_validate (queue);
}

void
goodix_milan_study_queue_disable (GoodixStudyQueue *queue)
{
  if (!queue)
    return;
  for (gsize i = 0; i < GOODIX_STUDY_QUEUE_CAPACITY; i++)
    {
      g_clear_pointer (&queue->entries[i].info, goodix_milan_match_free_info);
      queue->entries[i].rank = -1;
    }
  queue->enabled_state = 1;
}
