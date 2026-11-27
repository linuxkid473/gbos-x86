/*
 * Goodix 53x5 driver for libfprint - Milan feature materialization
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "milan/feature/feature.h"
#include "milan/private.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void
feature_build_orientation_weights (uint32_t *weights,
                                   size_t    size,
                                   uint32_t  coefficient)
{
  for (size_t y = 0; y < size; y++)
    for (size_t x = y; x < size; x++)
      {
        uint32_t radius_squared = (uint32_t) (x * x + y * y);
        uint32_t power = radius_squared * coefficient;
        uint32_t weight = 0;

        if (power <= 0x6ee75)
          {
            uint32_t reduced = power >> 8;
            uint32_t reduced_squared = reduced * reduced;
            uint32_t approximation = reduced_squared >> 8;

            approximation =
              ((approximation * approximation >> 8) *
               ((power / 5 + 0x10000) >> 8)) /
                0xc00 +
              0x200 +
              (((power / 6 + 0x8000) >> 8) * reduced_squared >> 15) +
              (power >> 7);
            weight = ((approximation >> 1) + 0x40000) / approximation;
          }
        weights[y * size + x] = weight;
        weights[x * size + y] = weight;
      }
}

static uint32_t
feature_build_orientation_histogram (int32_t         center_x,
                                     int32_t         center_y,
                                     int32_t         scale_value,
                                     const uint32_t *magnitude,
                                     const int16_t  *orientation,
                                     size_t          rows,
                                     size_t          columns,
                                     uint32_t        histogram[36],
                                     int32_t        *selected_bin)
{
  int32_t radius_q16 = (int32_t) ((uint32_t) scale_value * 18U) >> 2;
  int32_t sigma_q16 = (int32_t) ((uint32_t) scale_value * 6U) >> 2;
  uint32_t raw[36] = { 0 };
  uint32_t extended[40];
  uint32_t weights[33 * 33] = { 0 };
  int32_t radius = radius_q16 < 0
                     ? -(((-radius_q16) >> 16) +
                         ((((uint32_t) -radius_q16) & 0x8000) != 0))
                     : (radius_q16 >> 16) +
                         (((uint32_t) radius_q16 & 0x8000) != 0);
  radius = radius > 32 ? 32 : radius;
  size_t size = (size_t) radius + 1;
  uint32_t coefficient =
    (uint32_t) (UINT64_C(0x800000000000) /
                ((int64_t) sigma_q16 * sigma_q16));

  feature_build_orientation_weights (weights, size, coefficient);
  int32_t min_y = 1 - center_y > -radius ? 1 - center_y : -radius;
  int32_t max_y = (int32_t) rows - center_y - 2 < radius
                    ? (int32_t) rows - center_y - 2
                    : radius;
  int32_t min_x = 1 - center_x > -radius ? 1 - center_x : -radius;
  int32_t max_x = (int32_t) columns - center_x - 2 < radius
                    ? (int32_t) columns - center_x - 2
                    : radius;

  for (int32_t delta_y = min_y; delta_y <= max_y; delta_y++)
    for (int32_t delta_x = min_x; delta_x <= max_x; delta_x++)
      {
        size_t pixel = (size_t) (center_y + delta_y) * columns +
                       (size_t) (center_x + delta_x);
        int32_t bin = ((int32_t) orientation[pixel] * -36 + 0x743d4) /
                      0x6488;
        uint32_t weighted =
          magnitude[pixel] *
          weights[(size_t) abs (delta_y) * size + (size_t) abs (delta_x)] >> 8;

        if (bin >= 36)
          bin = 0;
        raw[bin] += weighted;
        raw[(bin + 18) % 36] += weighted;
      }
  extended[0] = raw[34];
  extended[1] = raw[35];
  memcpy (extended + 2, raw, sizeof(raw));
  extended[38] = raw[0];
  extended[39] = raw[1];
  for (size_t bin = 0; bin < 36; bin++)
    histogram[bin] = (extended[bin] + 4 * extended[bin + 1] +
                      6 * extended[bin + 2] + 4 * extended[bin + 3] +
                      extended[bin + 4]) >>
                     4;
  uint32_t peak = histogram[0];
  int32_t peak_bin = 0;
  for (int32_t bin = 1; bin < 36; bin++)
    if (histogram[bin] > peak)
      {
        peak = histogram[bin];
        peak_bin = bin;
      }
  *selected_bin = peak_bin + 18;
  return peak;
}

static void
feature_materialize_candidate (const uint8_t                  *feature_source,
                               const uint32_t                 *magnitude,
                               const int16_t                  *orientation,
                               size_t                          rows,
                               size_t                          columns,
                               const GoodixMilanFeatureCandidate *candidate,
                               size_t                          index,
                               GoodixMilanFeatureRecord       *record,
                               GoodixMilanFeatureRank         *rank,
                               GoodixMilanFeatureAux          *auxiliary)
{
  uint32_t histogram[36];
  int32_t selected_bin;
  uint32_t peak = feature_build_orientation_histogram (
    candidate->x, candidate->y, candidate->scale_value, magnitude,
    orientation, rows, columns, histogram, &selected_bin);
  int32_t previous_bin = selected_bin == 0 ? 35 : selected_bin - 1;
  int32_t next_bin = (selected_bin + 1) % 36;
  int64_t previous = histogram[previous_bin];
  int64_t current = histogram[selected_bin];
  int64_t next = histogram[next_bin];
  int64_t denominator = 2 * previous + 2 * next - 4 * current;
  int16_t interpolated = (int16_t) (
    ((previous - next) * 0x200 - (denominator >> 1)) / denominator +
    selected_bin * 0x200);
  size_t pixel = (size_t) candidate->y * columns + (size_t) candidate->x;
  int32_t neighborhood = feature_source[pixel] + feature_source[pixel - 1] +
                         feature_source[pixel + 1] +
                         feature_source[pixel - columns] +
                         feature_source[pixel + columns];
  int32_t absolute_strength = candidate->strength;

  if (interpolated < 0)
    interpolated += 0x4800;
  else if (interpolated >= 0x4800)
    interpolated -= 0x4800;
  int16_t feature_orientation =
    (int16_t) ((((int32_t) interpolated * 0x6488) / 36) >> 9) - 0x3244;
  if (feature_orientation < 0)
    feature_orientation += 0x3244;
  if (absolute_strength <= 0)
    absolute_strength = -absolute_strength;

  memset (record, 0, sizeof(*record));
  record->foreground = neighborhood > 0x27f;
  record->refined_x = candidate->refined_x;
  record->refined_y = candidate->refined_y;
  record->orientation = feature_orientation;
  record->strength = -absolute_strength;
  rank->strength = -absolute_strength;
  rank->index = (int32_t) index;
  *auxiliary = (GoodixMilanFeatureAux) {
    candidate->x, candidate->y, candidate->scale_value, peak,
    histogram[selected_bin], histogram[selected_bin] != peak, 0,
  };
}

size_t
goodix_milan_feature_collect_materialized (
  const uint8_t            *feature_source,
  const uint16_t           *scales,
  const uint32_t           *magnitude,
  const int16_t            *orientation,
  size_t                    rows,
  size_t                    columns,
  GoodixMilanFeatureRecord *records,
  GoodixMilanFeatureRank   *ranks,
  GoodixMilanFeatureAux    *auxiliary,
  size_t                    capacity)
{
  size_t cursor[3] = FEATURE_EXTREMA_CURSOR_INIT;
  GoodixMilanFeatureExtremum extremum;
  uint8_t *visited = calloc (rows * columns, 1);
  size_t count = 0;

  if (!visited)
    return 0;
  while (count < capacity &&
         feature_next_extremum (scales, rows, columns, cursor, &extremum))
    {
      GoodixMilanFeatureCandidate candidate = {
        extremum.x, extremum.y, extremum.scale, extremum.response,
        0, 0, 0,
      };
      uint32_t curvature = 0;

      if (!goodix_milan_feature_refine_extremum (
            scales, rows, columns, &candidate, &curvature))
        continue;
      size_t pixel = (size_t) candidate.y * columns + (size_t) candidate.x;
      if (visited[pixel] != 0)
        continue;
      feature_materialize_candidate (
        feature_source, magnitude, orientation, rows, columns, &candidate,
        count, &records[count], &ranks[count], &auxiliary[count]);
      visited[pixel] = 1;
      count++;
    }
  free (visited);
  return count;
}

static void
feature_descriptor_sine_cosine (int16_t  angle,
                                int32_t *sine,
                                int32_t *cosine)
{
  static const int16_t angles[13] = {
    0x0c91, 0x076b, 0x03eb, 0x01fd, 0x0100, 0x0080, 0x0040,
    0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x0001,
  };
  int32_t reduced = angle;
  int16_t sin_value = 0;
  int16_t cos_value = 0x4000;
  int16_t accumulated = 0;

  if (reduced > 0x1922)
    reduced = 0x3244 - reduced;
  if (reduced == 0)
    {
      *sine = 0;
      *cosine = angle > 0x1922 ? -0x4000 : 0x4000;
      return;
    }
  if (reduced == 0x1922)
    {
      *sine = 0x4000;
      *cosine = 0;
      return;
    }
  for (size_t iteration = 0; iteration < 13; iteration++)
    {
      int16_t cos_shift = cos_value >> iteration;
      int16_t sin_shift = sin_value >> iteration;
      int16_t delta;

      if (reduced - accumulated < 0)
        {
          cos_shift = (int16_t) -cos_shift;
          delta = (int16_t) -angles[iteration];
        }
      else
        {
          sin_shift = (int16_t) -sin_shift;
          delta = angles[iteration];
        }
      accumulated = (int16_t) (accumulated + delta);
      sin_value = (int16_t) (sin_value + cos_shift);
      cos_value = (int16_t) (cos_value + sin_shift);
    }
  *sine = ((int32_t) sin_value * 0x9b75 + 0x8000) >> 16;
  *cosine = ((int32_t) cos_value * 0x9b75 + 0x8000) >> 16;
  if (angle > 0x1922)
    *cosine = -*cosine;
}

static void
feature_descriptor_accumulate (uint32_t accumulator[6][6][8],
                               int32_t  x,
                               int32_t  y,
                               int32_t  angle,
                               int32_t  weight)
{
  int32_t x_bin = x >> 9;
  int32_t y_bin = y >> 9;
  int32_t x_fraction = x - x_bin * 0x200;
  int32_t y_fraction = y - y_bin * 0x200;
  int32_t orientation_bin = (angle >> 12) & 7;
  int32_t next_orientation = (orientation_bin + 1) & 7;
  int32_t orientation_fraction = angle - (angle >> 12) * 0x1000;
  uint32_t scaled_weight = (uint32_t) (weight >> 9);
  uint32_t upper_y = (uint32_t) (y_fraction * (int32_t) scaled_weight) >> 9;
  uint32_t lower_y = scaled_weight - upper_y;
  uint32_t upper_right =
    (uint32_t) (x_fraction * (int32_t) upper_y) >> 9;
  uint32_t lower_right =
    (uint32_t) (x_fraction * (int32_t) lower_y) >> 9;
  uint32_t cell_weight[2][2] = {
    { lower_y - lower_right, lower_right },
    { upper_y - upper_right, upper_right },
  };

  for (size_t cell_y = 0; cell_y < 2; cell_y++)
    for (size_t cell_x = 0; cell_x < 2; cell_x++)
      {
        uint32_t value = cell_weight[cell_y][cell_x];
        uint32_t orientation_part = value * orientation_fraction;
        uint32_t *cell = accumulator[y_bin + 1 + (int32_t) cell_y]
                                    [x_bin + 1 + (int32_t) cell_x];

        cell[orientation_bin] +=
          (value - (orientation_part >> 12)) >> 5;
        cell[next_orientation] += orientation_part >> 17;
      }
}

void
goodix_milan_feature_build_descriptor_samples (
  int32_t         center_x,
  int32_t         center_y,
  int32_t         descriptor_scale,
  int16_t         feature_orientation,
  const uint32_t *magnitude,
  const int16_t  *orientation,
  size_t          rows,
  size_t          columns,
  int32_t         samples[128])
{
  int32_t scale3 = descriptor_scale * 3;
  int64_t radius_q16 = ((int64_t) scale3 * 0x38916) >> 16;
  int32_t inverse_scale = (int32_t) (UINT64_C(0x1000000000) / scale3);
  uint32_t weights[33 * 33] = { 0 };
  uint32_t accumulator[6][6][8] = { 0 };
  int32_t sine, cosine;
  int32_t radius = (int32_t) ((radius_q16 >> 16) +
                              (((uint64_t) radius_q16 & 0x8000) != 0));

  radius = radius > 32 ? 32 : radius;
  size_t size = (size_t) radius + 1;
  uint32_t coefficient =
    (uint32_t) (UINT64_C(0x200000000000) /
                ((int64_t) scale3 * scale3));
  feature_build_orientation_weights (weights, size, coefficient);
  feature_descriptor_sine_cosine (feature_orientation, &sine, &cosine);
  cosine = (int32_t) (((int64_t) cosine * inverse_scale) >> 25);
  sine = (int32_t) (((int64_t) sine * inverse_scale) >> 25);
  int32_t min_y = 1 - center_y > -radius ? 1 - center_y : -radius;
  int32_t max_y = (int32_t) rows - center_y - 2 < radius
                    ? (int32_t) rows - center_y - 2
                    : radius;
  int32_t min_x = 1 - center_x > -radius ? 1 - center_x : -radius;
  int32_t max_x = (int32_t) columns - center_x - 2 < radius
                    ? (int32_t) columns - center_x - 2
                    : radius;

  for (int32_t delta_y = min_y; delta_y <= max_y; delta_y++)
    for (int32_t delta_x = min_x; delta_x <= max_x; delta_x++)
      {
        int32_t rotated_x = delta_x * cosine - delta_y * sine;
        int32_t rotated_y = delta_x * sine + delta_y * cosine;
        size_t pixel = (size_t) (center_y + delta_y) * columns +
                       (size_t) (center_x + delta_x);

        if (abs (rotated_x) >= 0x500 || abs (rotated_y) >= 0x500)
          continue;
        int32_t relative = 0x3244 - orientation[pixel] -
                           feature_orientation;
        relative %= 0x6488;
        if (relative < 0)
          relative += 0x6488;
        int32_t angle = (int32_t) (((uint32_t) relative * 0x145f3U) >> 16);
        int32_t weighted = (int32_t) (
          magnitude[pixel] *
          weights[(size_t) abs (delta_y) * size + (size_t) abs (delta_x)]);
        feature_descriptor_accumulate (accumulator, rotated_x + 0x300,
                                       rotated_y + 0x300, angle, weighted);
      }
  for (size_t y = 0; y < 4; y++)
    for (size_t x = 0; x < 4; x++)
      for (size_t bin = 0; bin < 8; bin++)
        samples[(y * 4 + x) * 8 + bin] = accumulator[y + 1][x + 1][bin];
}

static uint64_t
feature_integer_square_root (uint64_t value)
{
  uint64_t result = 0;
  uint64_t bit = UINT64_C(1) << 62;

  while (bit > value)
    bit >>= 2;
  while (bit != 0)
    {
      if (value >= result + bit)
        {
          value -= result + bit;
          result = (result >> 1) + bit;
        }
      else
        result >>= 1;
      bit >>= 2;
    }
  return result;
}

static void
feature_normalize_descriptor (const int32_t *samples,
                              size_t         count,
                              int16_t       *normalized)
{
  uint64_t energy = 0;

  for (size_t i = 0; i < count; i++)
    energy += (uint64_t) (uint32_t) samples[i] * (uint32_t) samples[i];
  uint32_t threshold =
    (uint32_t) ((feature_integer_square_root (energy) * 0x3333) >> 16);
  int16_t clipped = (int16_t) feature_integer_square_root (threshold);
  for (size_t i = 0; i < count; i++)
    normalized[i] = (int16_t) (
      samples[i] < (int32_t) threshold
        ? (int16_t) feature_integer_square_root ((uint32_t) samples[i])
        : (int16_t) clipped);
}

static void
feature_hadamard32 (const int16_t *normalized,
                    int32_t        transformed[32])
{
  for (size_t i = 0; i < 32; i++)
    transformed[i] = normalized[i];
  for (size_t stride = 1; stride < 32; stride *= 2)
    for (size_t base = 0; base < 32; base += stride * 2)
      for (size_t i = 0; i < stride; i++)
        {
          int32_t a = transformed[base + i];
          int32_t b = transformed[base + i + stride];

          transformed[base + i] = a + b;
          transformed[base + i + stride] = a - b;
        }
}

static int16_t
feature_descriptor_median (const int16_t *values,
                           size_t         count)
{
  int16_t sorted[128];

  memcpy (sorted, values, count * sizeof(*values));
  for (size_t i = 1; i < count; i++)
    {
      int16_t value = sorted[i];
      size_t position = i;

      while (position > 0 && value < sorted[position - 1])
        {
          sorted[position] = sorted[position - 1];
          position--;
        }
      sorted[position] = value;
    }
  return sorted[(count - 1) / 2];
}

static void
feature_encode_descriptor_first (const int16_t normalized[128],
                                 uint8_t      *record)
{
  static const int coefficients[4][4] = {
    { 1, 1, 1, 1 }, { 1, -1, 1, -1 },
    { 1, 1, -1, -1 }, { 1, -1, -1, 1 },
  };
  uint32_t transform[4] = { 0 };
  uint32_t median_bits = 0;
  int16_t partial[4][32];

  for (size_t block = 0; block < 4; block++)
    {
      int32_t transformed[32];

      feature_hadamard32 (normalized + block * 32, transformed);
      /* Narrow each block before the existing four-block combination. */
      for (size_t bit = 0; bit < 32; bit++)
        partial[block][bit] = (int16_t) transformed[bit];
    }

  for (size_t bit = 0; bit < 32; bit++)
    {
      for (size_t output = 0; output < 4; output++)
        {
          int32_t sum = 0;
          for (size_t block = 0; block < 4; block++)
            sum += coefficients[output][block] * partial[block][bit];
          if (sum > 0)
            transform[output] |= 1U << bit;
        }
    }
  int16_t median = feature_descriptor_median (normalized, 128);
  for (size_t bit = 0; bit < 32; bit++)
    if (median < normalized[bit * 4 + 1])
      median_bits |= 1U << bit;
  memset (record + 0x10, 0, 0x18);
  memcpy (record + 0x10, transform, sizeof(transform));
  memcpy (record + 0x20, &median_bits, sizeof(median_bits));
}

static void
feature_encode_descriptor_second (const int16_t normalized[32],
                                  uint8_t      *record)
{
  uint32_t transform = 0;
  uint32_t median_bits = 0;
  int16_t median = feature_descriptor_median (normalized, 32);
  int32_t transformed[32];

  feature_hadamard32 (normalized, transformed);

  for (size_t bit = 0; bit < 32; bit++)
    {
      if (transformed[bit] > 0)
        transform |= 1U << bit;
      if (median < normalized[bit])
        median_bits |= 1U << bit;
    }
  memset (record + 0x28, 0, 0x10);
  memcpy (record + 0x28, &transform, sizeof(transform));
  memcpy (record + 0x2c, &median_bits, sizeof(median_bits));
}

void
feature_build_descriptor (const uint32_t       *magnitude,
                           const int16_t        *orientation,
                           size_t                rows,
                           size_t                columns,
                           int32_t               descriptor_scale,
                           const GoodixMilanFeatureAux *auxiliary,
                           GoodixMilanFeatureRecord    *record)
{
  int32_t samples[128];
  int32_t central[32];
  int16_t normalized[128];
  int16_t central_normalized[32];

  goodix_milan_feature_build_descriptor_samples (
    auxiliary->x, auxiliary->y, descriptor_scale,
    record->orientation, magnitude, orientation, rows, columns, samples);
  feature_normalize_descriptor (samples, 128, normalized);
  feature_encode_descriptor_first (normalized, (uint8_t *) record);
  memcpy (central, samples + 40, 8 * sizeof(*central));
  memcpy (central + 8, samples + 48, 8 * sizeof(*central));
  memcpy (central + 16, samples + 72, 8 * sizeof(*central));
  memcpy (central + 24, samples + 80, 8 * sizeof(*central));
  feature_normalize_descriptor (central, 32, central_normalized);
  feature_encode_descriptor_second (central_normalized, (uint8_t *) record);
}

static int
feature_rank_compare (const void *left,
                      const void *right)
{
  const GoodixMilanFeatureRank *a = left;
  const GoodixMilanFeatureRank *b = right;

  if (a->strength != b->strength)
    return a->strength < b->strength ? -1 : 1;
  return (a->index > b->index) - (a->index < b->index);
}

size_t
feature_finish_pretransform_records (
  const uint32_t                 *magnitude,
  const int16_t                  *orientation,
  size_t                          rows,
  size_t                          columns,
  GoodixMilanFeatureRecord       *records,
  size_t                          capacity,
  GoodixMilanFeatureRecord       *materialized,
  GoodixMilanFeatureRank         *ranks,
  const GoodixMilanFeatureAux    *auxiliary,
  size_t                          count)
{
  size_t output_count = count < capacity ? count : capacity;

  if (count > capacity)
    qsort (ranks, count, sizeof(*ranks), feature_rank_compare);
  for (size_t i = 0; i < output_count; i++)
    {
      size_t index = count > capacity ? (size_t) ranks[i].index : i;
      records[i] = materialized[index];
      feature_build_descriptor (magnitude, orientation, rows, columns, 0x1adfe,
                                 &auxiliary[index], &records[i]);
      int32_t boundary;
      if ((uint16_t) records[i].refined_y < 0x1400)
        boundary = 1;
      else if ((uint16_t) records[i].refined_y >=
               (uint16_t) (rows * 0x100 - 0x1400))
        boundary = 2;
      else
        boundary = 0;
      memcpy (records[i].payload, &boundary, sizeof(boundary));
      records[i].refined_x =
        (int16_t) (((uint16_t) records[i].refined_x + 8) & 0xfff0);
      records[i].refined_y =
        (int16_t) (((uint16_t) records[i].refined_y + 8) & 0xfff0);
      if (records[i].orientation < 0)
        records[i].orientation = (int16_t) -(
          (0x80 - (int32_t) records[i].orientation) & 0xff00);
      else
        records[i].orientation = (int16_t) (
          ((int32_t) records[i].orientation + 0x80) & 0xff00);
    }
  return output_count;
}
