/*
 * Goodix 53x5 driver for libfprint - Milan anti-fake morphology
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * Anti-fake boundary score: a threshold class map completed by a
 * priority-flood watershed (native FUN_18003d1a0, FUN_18003c680), Zhang-Suen
 * thinning of the DARK class (FUN_18003b6d0), and a rounded ratio of residual
 * differences along the skeleton (FUN_18003ad10). Contracts are in
 * re/milan/functions on milan-dev; "Native quirk" comments mark behaviour that
 * must not be corrected.
 */

#include "milan/antifake/antifake.h"
#include "milan/transform-private.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Label values are native; the watershed treats every nonzero label, including
 * QUEUED, EXCLUDED, and CONFLICT, as an assigned neighbour. */
#define ANTIFAKE_CLASS_UNASSIGNED 0x00
#define ANTIFAKE_CLASS_BRIGHT 0x01
#define ANTIFAKE_CLASS_DARK 0x02
#define ANTIFAKE_CLASS_QUEUED 0xfd
#define ANTIFAKE_CLASS_EXCLUDED 0xfe
#define ANTIFAKE_CLASS_CONFLICT 0xff

#define ANTIFAKE_DARK_BELOW 0x1a
#define ANTIFAKE_BRIGHT_FROM 0xe7

/* One level per absolute 8-bit intensity difference. */
#define WATERSHED_PRIORITY_LEVELS 256

#define THINNING_MAX_ITERATIONS 100
#define BOUNDARY_SCORE_SCALE 1000

/* Visit order (left, right, up, down) decides label ties and queue order. */
static const int8_t four_neighbor_dx[4] = {-1, 1, 0, 0};
static const int8_t four_neighbor_dy[4] = { 0, 0, -1, 1};

static inline size_t
four_neighbor_index (size_t row,
                     size_t column,
                     size_t columns,
                     size_t direction)
{
  return (size_t) ((ptrdiff_t) row + four_neighbor_dy[direction]) * columns +
         (size_t) ((ptrdiff_t) column + four_neighbor_dx[direction]);
}

static inline int
absolute_difference (uint8_t first,
                     uint8_t second)
{
  int difference = (int) first - second;

  return difference < 0 ? -difference : difference;
}

/* Pixels pop from the lowest nonempty level, LIFO within a level; both
 * properties determine the class map. `next` is indexed by pixel. */
typedef struct
{
  int32_t  heads[WATERSHED_PRIORITY_LEVELS];
  int32_t *next;
  int      current_priority;
} WatershedQueue;

static int
watershed_queue_init (WatershedQueue *queue,
                      size_t          count)
{
  queue->next = malloc (count * sizeof (*queue->next));
  if (!queue->next)
    return -1;
  for (size_t i = 0; i < WATERSHED_PRIORITY_LEVELS; i++)
    queue->heads[i] = -1;
  for (size_t i = 0; i < count; i++)
    queue->next[i] = -1;
  queue->current_priority = 0;
  return 0;
}

static void
watershed_queue_free (WatershedQueue *queue)
{
  free (queue->next);
  queue->next = NULL;
}

static void
watershed_queue_push (WatershedQueue *queue,
                      size_t          pixel,
                      int             priority)
{
  queue->next[pixel] = queue->heads[priority];
  queue->heads[priority] = (int32_t) pixel;
  if (priority < queue->current_priority)
    queue->current_priority = priority;
}

static int32_t
watershed_queue_pop (WatershedQueue *queue)
{
  int32_t pixel;

  while (queue->current_priority < WATERSHED_PRIORITY_LEVELS &&
         queue->heads[queue->current_priority] == -1)
    queue->current_priority++;
  if (queue->current_priority >= WATERSHED_PRIORITY_LEVELS)
    return -1;
  pixel = queue->heads[queue->current_priority];
  queue->heads[queue->current_priority] = queue->next[pixel];
  queue->next[pixel] = -1;
  return pixel;
}

static void
classify_by_intensity (const uint8_t *image,
                       const uint8_t *mask,
                       size_t         count,
                       uint8_t       *classes)
{
  for (size_t i = 0; i < count; i++)
    {
      if (image[i] >= ANTIFAKE_BRIGHT_FROM)
        classes[i] = ANTIFAKE_CLASS_BRIGHT;
      else if (image[i] < ANTIFAKE_DARK_BELOW)
        classes[i] = ANTIFAKE_CLASS_DARK;
      else
        classes[i] = ANTIFAKE_CLASS_UNASSIGNED;
      if (mask[i] == 0)
        classes[i] = ANTIFAKE_CLASS_EXCLUDED;
    }
}

static void
exclude_border (uint8_t *classes,
                size_t   rows,
                size_t   columns)
{
  memset (classes, ANTIFAKE_CLASS_EXCLUDED, columns);
  memset (classes + (rows - 1) * columns, ANTIFAKE_CLASS_EXCLUDED, columns);
  for (size_t row = 1; row + 1 < rows; row++)
    {
      classes[row * columns] = ANTIFAKE_CLASS_EXCLUDED;
      classes[row * columns + columns - 1] = ANTIFAKE_CLASS_EXCLUDED;
    }
}

static void
watershed_seed (const uint8_t  *image,
                uint8_t        *classes,
                size_t          rows,
                size_t          columns,
                WatershedQueue *queue)
{
  for (size_t row = 1; row + 1 < rows; row++)
    for (size_t column = 1; column + 1 < columns; column++)
      {
        size_t pixel = row * columns + column;
        int priority = WATERSHED_PRIORITY_LEVELS;

        if (classes[pixel] != ANTIFAKE_CLASS_UNASSIGNED)
          continue;
        for (size_t direction = 0; direction < 4; direction++)
          {
            size_t neighbor = four_neighbor_index (row, column, columns,
                                                   direction);

            if (classes[neighbor] == ANTIFAKE_CLASS_UNASSIGNED)
              continue;
            int difference = absolute_difference (image[pixel],
                                                  image[neighbor]);
            if (difference < priority)
              priority = difference;
          }
        if (priority < WATERSHED_PRIORITY_LEVELS)
          {
            watershed_queue_push (queue, pixel, priority);
            classes[pixel] = ANTIFAKE_CLASS_QUEUED;
          }
      }
}

/* Native quirk: QUEUED, EXCLUDED, and CONFLICT neighbours take part in
 * inheritance like any other nonzero label. */
static uint8_t
watershed_inherit_label (const uint8_t *classes,
                         size_t         row,
                         size_t         column,
                         size_t         columns)
{
  uint8_t label = ANTIFAKE_CLASS_UNASSIGNED;

  for (size_t direction = 0; direction < 4; direction++)
    {
      uint8_t neighbor_label =
        classes[four_neighbor_index (row, column, columns, direction)];

      if (neighbor_label == ANTIFAKE_CLASS_UNASSIGNED)
        continue;
      if (label != ANTIFAKE_CLASS_UNASSIGNED && label != neighbor_label)
        label = ANTIFAKE_CLASS_CONFLICT;
      else
        label = neighbor_label;
    }
  return label;
}

static void
watershed_propagate (const uint8_t  *image,
                     uint8_t        *classes,
                     size_t          columns,
                     WatershedQueue *queue)
{
  int32_t popped;

  while ((popped = watershed_queue_pop (queue)) >= 0)
    {
      size_t pixel = (size_t) popped;
      size_t row = pixel / columns;
      size_t column = pixel % columns;
      uint8_t label = watershed_inherit_label (classes, row, column, columns);

      if (label != ANTIFAKE_CLASS_UNASSIGNED)
        classes[pixel] = label;
      if (label == ANTIFAKE_CLASS_CONFLICT)
        continue;
      for (size_t direction = 0; direction < 4; direction++)
        {
          size_t neighbor = four_neighbor_index (row, column, columns,
                                                 direction);

          if (classes[neighbor] != ANTIFAKE_CLASS_UNASSIGNED)
            continue;
          watershed_queue_push (queue, neighbor,
                                absolute_difference (image[pixel],
                                                     image[neighbor]));
          classes[neighbor] = ANTIFAKE_CLASS_QUEUED;
        }
    }
}

int
goodix_milan_antifake_class_map (
  const uint8_t *image,
  const uint8_t *mask,
  size_t         rows,
  size_t         columns,
  uint8_t       *classes)
{
  WatershedQueue queue;
  size_t count;

  if (!image || !mask || !classes || rows < 2 || columns < 2 ||
      columns > SIZE_MAX / rows)
    return -1;
  count = rows * columns;
  if (watershed_queue_init (&queue, count) != 0)
    return -1;

  classify_by_intensity (image, mask, count, classes);
  exclude_border (classes, rows, columns);
  watershed_seed (image, classes, rows, columns, &queue);
  watershed_propagate (image, classes, columns, &queue);

  watershed_queue_free (&queue);
  return 0;
}

/* Ring is clockwise from north. */
static int
zhang_suen_should_delete (const uint8_t *pixels,
                          size_t         columns,
                          size_t         pixel,
                          int            second_subpass)
{
  const size_t ring[8] = {
    pixel - columns,     pixel - columns + 1, pixel + 1,
    pixel + columns + 1, pixel + columns,     pixel + columns - 1,
    pixel - 1,           pixel - columns - 1,
  };
  int population = 0;
  int transitions = 0;

  for (size_t i = 0; i < 8; i++)
    {
      int current = pixels[ring[i]] != 0;
      int following = pixels[ring[(i + 1) % 8]] != 0;

      population += current;
      transitions += !current && following;
    }
  if (population < 2 || population > 6 || transitions != 1)
    return 0;

  int north = pixels[ring[0]] != 0;
  int east = pixels[ring[2]] != 0;
  int south = pixels[ring[4]] != 0;
  int west = pixels[ring[6]] != 0;

  return second_subpass ?
         !(north && east && west) && !(north && south && west) :
         !(north && east && south) && !(east && south && west);
}

static int
zhang_suen_subpass (uint8_t       *pixels,
                    const uint8_t *snapshot,
                    size_t         rows,
                    size_t         columns,
                    int            second_subpass)
{
  int changed = 0;

  for (size_t row = 1; row + 1 < rows; row++)
    for (size_t column = 1; column + 1 < columns; column++)
      {
        size_t pixel = row * columns + column;

        if (snapshot[pixel] != 0 &&
            zhang_suen_should_delete (snapshot, columns, pixel,
                                      second_subpass))
          {
            pixels[pixel] = 0;
            changed = 1;
          }
      }
  return changed;
}

/* Native quirk: convergence is tested on the second subpass only. */
static int
zhang_suen_thin (uint8_t *pixels,
                 size_t   rows,
                 size_t   columns,
                 size_t   maximum_iterations)
{
  size_t count = rows * columns;
  uint8_t *snapshot = malloc (count);

  if (!snapshot)
    return -1;
  for (size_t iteration = 0; iteration < maximum_iterations; iteration++)
    {
      memcpy (snapshot, pixels, count);
      zhang_suen_subpass (pixels, snapshot, rows, columns, 0);
      memcpy (snapshot, pixels, count);
      if (!zhang_suen_subpass (pixels, snapshot, rows, columns, 1))
        break;
    }
  free (snapshot);
  return 0;
}

/* Native quirk: the row and column margins differ. */
static uint32_t
boundary_maximum_residual (const uint16_t *residual,
                           const uint8_t  *classes,
                           size_t          rows,
                           size_t          columns)
{
  uint32_t maximum = 0;

  for (size_t row = 1; row + 1 < rows; row++)
    for (size_t column = 2; column + 2 < columns; column++)
      {
        size_t pixel = row * columns + column;

        if (classes[pixel] != ANTIFAKE_CLASS_EXCLUDED &&
            maximum < residual[pixel])
          maximum = residual[pixel];
      }
  return maximum;
}

/* Pairs are directed, so each undirected pair is counted twice. */
static void
boundary_adjacent_differences (const uint16_t *residual,
                               const uint8_t  *skeleton,
                               size_t          rows,
                               size_t          columns,
                               uint32_t       *difference_sum,
                               uint32_t       *adjacent_count)
{
  const ptrdiff_t ring[8] = {
    -(ptrdiff_t) columns - 1, -(ptrdiff_t) columns, 1 - (ptrdiff_t) columns,
    1, (ptrdiff_t) columns, (ptrdiff_t) columns + 1,
    (ptrdiff_t) columns - 1, -1,
  };

  *difference_sum = 0;
  *adjacent_count = 0;
  for (size_t row = 3; row + 3 < rows; row++)
    for (size_t column = 3; column + 3 < columns; column++)
      {
        size_t pixel = row * columns + column;

        if (skeleton[pixel] != ANTIFAKE_CLASS_DARK)
          continue;
        for (size_t direction = 0; direction < 8; direction++)
          {
            size_t neighbor = (size_t) ((ptrdiff_t) pixel + ring[direction]);

            if (skeleton[neighbor] != ANTIFAKE_CLASS_DARK)
              continue;
            uint32_t left = residual[pixel];
            uint32_t right = residual[neighbor];

            (*adjacent_count)++;
            *difference_sum += left < right ? right - left : left - right;
          }
      }
}

/* round (difference_sum * 1000 / (adjacent_count * maximum)) in 32-bit
 * registers: products wrap, the half-divisor is an arithmetic shift, division
 * is signed and truncating. Only INT32_MIN / -1 (a native fault) is rejected. */
static int
boundary_rounded_ratio (uint32_t difference_sum,
                        uint32_t adjacent_count,
                        uint32_t maximum,
                        int32_t *score)
{
  uint32_t divisor = adjacent_count * maximum;
  uint32_t numerator = difference_sum * BOUNDARY_SCORE_SCALE;

  if (divisor == 0)
    {
      *score = goodix_milan_transform_s32 (numerator);
      return 0;
    }

  int32_t signed_divisor = goodix_milan_transform_s32 (divisor);
  int32_t half_divisor = goodix_milan_transform_sar32 (divisor, 1);
  int32_t rounded_numerator =
    goodix_milan_transform_s32 (numerator + (uint32_t) half_divisor);

  if (rounded_numerator == INT32_MIN && signed_divisor == -1)
    return -1;
  *score = rounded_numerator / signed_divisor;
  return 0;
}

int
goodix_milan_antifake_boundary_score (
  const uint16_t *residual,
  const uint8_t  *classes,
  size_t          rows,
  size_t          columns,
  int32_t        *score)
{
  uint8_t *thinned;
  uint32_t maximum;
  uint32_t difference_sum;
  uint32_t adjacent_count;
  int result = -1;

  if (!residual || !classes || !score || rows < 7 ||
      columns < 7 || columns > SIZE_MAX / rows)
    return -1;

  thinned = malloc (rows * columns);
  if (!thinned)
    return -1;
  for (size_t i = 0; i < rows * columns; i++)
    thinned[i] = classes[i] == ANTIFAKE_CLASS_DARK ? ANTIFAKE_CLASS_DARK : 0;

  maximum = boundary_maximum_residual (residual, classes, rows, columns);
  if (zhang_suen_thin (thinned, rows, columns, THINNING_MAX_ITERATIONS) != 0)
    goto out;
  boundary_adjacent_differences (residual, thinned, rows, columns,
                                 &difference_sum, &adjacent_count);
  result = boundary_rounded_ratio (difference_sum, adjacent_count, maximum,
                                   score);

out:
  free (thinned);
  return result;
}
