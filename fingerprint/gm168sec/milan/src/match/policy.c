/*
 * Goodix 53x5 driver for libfprint - profile-9/type-12 matcher policy
 * Copyright (C) 2026 goodix-fp-linux-dev contributors
 *
 * This is the narrow profile-9/subtype-12 policy. Its helper boundaries retain
 * the policy's evaluation stages so each decision rule has one clear owner.
 */

#include "milan/match/policy.h"
#include "milan/match/candidate.h"
#include "milan/print.h"

#include <limits.h>
#include <string.h>

enum
{
  GOODIX_MILAN_POLICY_METRIC_PRIMARY = GOODIX_MILAN_CANDIDATE_PRIMARY_COUNT,
  GOODIX_MILAN_POLICY_METRIC_FILTERED = GOODIX_MILAN_CANDIDATE_RETAINED_COUNT,
  GOODIX_MILAN_POLICY_METRIC_OVERLAP = GOODIX_MILAN_CANDIDATE_OVERLAP_SCORE,
  GOODIX_MILAN_POLICY_METRIC_DETAIL = GOODIX_MILAN_CANDIDATE_OVERLAP_DETAIL,
  GOODIX_MILAN_POLICY_METRIC_COMBINED = GOODIX_MILAN_CANDIDATE_LOW_DETAIL,
  GOODIX_MILAN_POLICY_METRIC_COVERAGE =
    GOODIX_MILAN_CANDIDATE_OVERLAP_COVERAGE_Q8,
  GOODIX_MILAN_POLICY_METRIC_TOPOLOGY =
    GOODIX_MILAN_CANDIDATE_TOPOLOGY_PERCENT,
  GOODIX_MILAN_POLICY_METRIC_GEOMETRY =
    GOODIX_MILAN_CANDIDATE_GEOMETRIC_PERCENT,
  GOODIX_MILAN_POLICY_METRIC_PENALTY_FIRST =
    GOODIX_MILAN_CANDIDATE_SCALE_PENALTY,
  GOODIX_MILAN_POLICY_METRIC_PENALTY_SECOND =
    GOODIX_MILAN_CANDIDATE_ORTHOGONALITY_PENALTY,
  GOODIX_MILAN_POLICY_METRIC_PENALTY_THIRD =
    GOODIX_MILAN_CANDIDATE_STRONG_ORTHOGONALITY_PENALTY,
  GOODIX_MILAN_POLICY_METRIC_TOPOLOGY_DISTANCE =
    GOODIX_MILAN_CANDIDATE_TOPOLOGY_DISTANCE,
};

enum
{
  GOODIX_MILAN_POLICY_CONFIG_METRIC_OFFSET = 0,
  GOODIX_MILAN_POLICY_CONFIG_PRIMARY_OFFSET = 1,
  GOODIX_MILAN_POLICY_CONFIG_SUBTYPE = 15,
  GOODIX_MILAN_POLICY_CONFIG_ALTERNATE_PENALTY = 16,
  GOODIX_MILAN_POLICY_CONFIG_PACKED_MODE = 18,
  GOODIX_MILAN_POLICY_CONFIG_RECOGNITION_MODE = 19,
};

static int32_t
policy_u32_as_s32 (uint32_t value)
{
  if (value <= INT32_MAX)
    return (int32_t) value;
  return -1 - (int32_t) (UINT32_MAX - value);
}

static int32_t
policy_wrap_multiply (int32_t left,
                      int32_t right)
{
  return policy_u32_as_s32 ((uint32_t) left * (uint32_t) right);
}

static int32_t
policy_arithmetic_shift_8 (int32_t value)
{
  if (value >= 0)
    return value / 256;
  return -1 - (int32_t) ((uint32_t) (-(value + 1)) / 256);
}

static int32_t
clamp (int32_t value,
       int32_t minimum,
       int32_t maximum)
{
  if (value < minimum)
    return minimum;
  if (value > maximum)
    return maximum;
  return value;
}

static void
initial_classifier (const int32_t metrics[GOODIX_MILAN_CANDIDATE_WORDS],
                    int32_t       image_quality,
                    int32_t       image_coverage,
                    const int32_t config[20],
                    int32_t       output[2])
{
  static const int32_t broad_by_primary[22] = {
    0xfffffff, 0xfffffff, 0xfffffff, 222, 218, 214, 212, 210,
    207, 204, 201, 199, 195, 190, 185, 185, 180, 180, 180, 180, 180, 180,
  };
  static const int32_t broad_by_filtered[22] = {
    0xfffffff, 0xfffffff, 0xfffffff, 228, 224, 220, 218, 216,
    213, 210, 207, 205, 200, 195, 190, 190, 190, 190, 190, 190, 190, 190,
  };
  static const int32_t strict_by_primary[22] = {
    0xfffffff, 0xfffffff, 0xfffffff, 227, 223, 219, 217, 215,
    212, 209, 206, 204, 200, 195, 190, 190, 185, 185, 185, 185, 185, 185,
  };
  static const int32_t strict_by_filtered[22] = {
    0xfffffff, 0xfffffff, 0xfffffff, 233, 229, 225, 223, 221,
    218, 215, 209, 210, 206, 201, 196, 195, 195, 195, 195, 195, 195, 195,
  };
  int32_t primary = metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY];
  int32_t filtered = metrics[GOODIX_MILAN_POLICY_METRIC_FILTERED];
  int32_t overlap = metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP];
  int32_t detail = metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL];
  int32_t coverage = metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE];
  int32_t penalty = 0;
  int32_t adjusted_overlap = overlap - config[GOODIX_MILAN_POLICY_CONFIG_METRIC_OFFSET];
  int32_t adjusted_detail = detail - config[GOODIX_MILAN_POLICY_CONFIG_METRIC_OFFSET];

  if (config[GOODIX_MILAN_POLICY_CONFIG_ALTERNATE_PENALTY] != 0 && filtered < 11)
    {
      penalty = (metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_FIRST] +
                 metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_SECOND]) * 4;
      adjusted_overlap -= penalty;
      adjusted_detail -= penalty;
      overlap -= penalty;
      detail -= penalty;
    }
  if (metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 60 && primary > 4)
    {
      int32_t adjustment = 1 +
                           (metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] - 60) / 5;

      primary += adjustment;
      filtered += adjustment;
    }

  int32_t primary_index =
    clamp (primary - config[GOODIX_MILAN_POLICY_CONFIG_PRIMARY_OFFSET], 0, 21);
  int32_t filtered_index =
    clamp (filtered - config[GOODIX_MILAN_POLICY_CONFIG_PRIMARY_OFFSET], 0, 21);

  output[1] = adjusted_detail > broad_by_primary[primary_index] ||
              adjusted_overlap > broad_by_filtered[primary_index] ||
              adjusted_detail > broad_by_primary[filtered_index] + 4 ||
              adjusted_overlap > broad_by_filtered[filtered_index] + 4 ||
              primary > 11;
  output[0] = output[1] && image_quality >= 16 && image_coverage >= 65 &&
              coverage >= 40 &&
              (detail > strict_by_primary[clamp (primary, 0, 21)] ||
               overlap > strict_by_filtered[clamp (primary, 0, 21)] ||
               detail > strict_by_primary[clamp (filtered, 0, 21)] + 5 ||
               overlap > strict_by_filtered[clamp (filtered, 0, 21)] + 5);
}

static void
fallback_classifier (const int32_t metrics[GOODIX_MILAN_CANDIDATE_WORDS],
                     const int32_t config[20],
                     int32_t       output[2])
{
  static const int32_t detail_thresholds[10] = {
    0xfffffff, 195, 195, 195, 185, 175, 170, 170, 160, 160,
  };
  static const int32_t low_primary_detail_thresholds[10] = {
    0xfffffff, 205, 205, 205, 204, 204, 203, 200, 192, 180,
  };
  static const int32_t combined_thresholds[10] = {
    0xfffffff, 207, 205, 200, 199, 193, 185, 185, 185, 185,
  };
  static const int32_t coverage_thresholds[10] = {
    0xfffffff, 90, 85, 80, 73, 63, 55, 55, 55, 55,
  };
  int32_t primary = metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY];
  int32_t filtered = metrics[GOODIX_MILAN_POLICY_METRIC_FILTERED];
  int32_t coverage = metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE];
  int32_t penalty = 0;

  if (config[GOODIX_MILAN_POLICY_CONFIG_ALTERNATE_PENALTY] == 0 && filtered < 14)
    penalty = ((coverage < 91) + (coverage < 76)) * 3 +
              (metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_SECOND] +
               metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_THIRD]) * 5;
  else if (config[GOODIX_MILAN_POLICY_CONFIG_ALTERNATE_PENALTY] != 0 &&
           (filtered < 11 || primary < 4))
    penalty = ((primary < 4) +
               metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_SECOND]) * 5;

  int32_t index =
    clamp (filtered - config[GOODIX_MILAN_POLICY_CONFIG_PRIMARY_OFFSET] - 4,
           0,
           9);
  int32_t detail = metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] - penalty;
  int32_t combined = metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] - penalty;

  output[1] = combined - config[GOODIX_MILAN_POLICY_CONFIG_METRIC_OFFSET] >=
                combined_thresholds[index] &&
              detail - config[GOODIX_MILAN_POLICY_CONFIG_METRIC_OFFSET] >=
                ((config[GOODIX_MILAN_POLICY_CONFIG_ALTERNATE_PENALTY] == 0 && primary < 5) ?
                 low_primary_detail_thresholds[index] : detail_thresholds[index]) &&
              coverage >= coverage_thresholds[index];

  index = clamp (filtered - 4, 0, 9);
  int32_t strict_coverage = coverage_thresholds[index] + 10;

  if (strict_coverage > 100)
    strict_coverage = 100;
  output[0] = !((primary < 6 && filtered < 16 &&
                  (primary < 5 ||
                   metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] < 56 ||
                   coverage < 125)) ||
                combined < combined_thresholds[index] + 2 || detail < 206 ||
                coverage < strict_coverage);
  output[1] |= output[0];
}

static int
first_veto_type12 (const int32_t             metrics[GOODIX_MILAN_CANDIDATE_WORDS],
                   GoodixMilanMatcherPolicy *policy)
{
  int32_t noise = metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_FIRST] +
                  metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_SECOND] +
                  metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_THIRD];
  int32_t primary = metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY];
  int32_t filtered = metrics[GOODIX_MILAN_POLICY_METRIC_FILTERED];
  int32_t overlap = metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] - noise * 2;
  int32_t detail = metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] - noise * 2;
  int32_t low_detail = metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] - noise * 2;
  int32_t combined = low_detail + detail;
  int32_t coverage = metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE];
  int32_t topology = metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY];
  int32_t geometry = metrics[GOODIX_MILAN_POLICY_METRIC_GEOMETRY];
  int32_t mode =
    policy->configuration[GOODIX_MILAN_POLICY_CONFIG_PACKED_MODE] >> 8;
  int32_t bucket = clamp (primary, 4, 15);
  int reject = 0;

  if (mode > 0 && ((detail < 200 && low_detail < 195) ||
                   (mode > 1 && detail < 213 && low_detail < 210)))
    {
      policy->configuration[GOODIX_MILAN_POLICY_CONFIG_RECOGNITION_MODE] = 0;
      return 0;
    }

  switch (bucket)
    {
    case 4:
      reject = (detail < 208 && filtered < 17 && combined < 411) ||
               (detail < 201 && filtered < 22 && combined < 401) ||
               (overlap < 219 && filtered < 11 && coverage < 48) ||
               (detail < 214 && filtered < 14 && topology < 71) ||
               (detail < 217 && filtered < 9 && topology < 46) ||
               (filtered < 7 && coverage < 74) ||
               (detail < 226 && filtered < 13 && coverage < 130);
      break;
    case 5:
      reject =
        (detail < 201 && ((overlap < 226 && filtered < 21 && combined < 389) ||
                          (combined < 396 && topology < 49 && coverage < 151))) ||
        (detail < 229 && filtered < 12 && coverage < 84) ||
        (detail < 207 && filtered < 10 && topology < 49 && geometry < 11) ||
        (detail < 219 && filtered < 11 && topology < 57 && geometry < 17 && combined < 401) ||
        (detail < 214 && filtered < 14 && topology < 67 && geometry < 42 && combined < 429) ||
        (detail < 208 && filtered < 18 && topology < 53 && geometry < 24) ||
        (detail < 218 && filtered < 12 && topology < 49 && geometry < 13) ||
        (detail < 235 && filtered < 7 && topology < 63 && overlap < 224 && geometry < 26) ||
        (detail < 222 && filtered < 6 && topology < 55 && geometry < 23);
      break;
    case 6:
      reject = (detail < 195 && filtered < 22 && geometry < 23) ||
               (detail < 205 && filtered < 20 && geometry < 31) ||
               (detail < 216 && filtered < 20 && geometry < 13) ||
               (detail < 208 && filtered < 19 && coverage < 129) ||
                (detail < 226 && filtered < 10 && coverage < 91) ||
               (detail < 218 && ((filtered < 7 && coverage < 156) ||
                                  (filtered < 14 && coverage < 141))) ||
               (detail < 213 && filtered < 17 && coverage < 172);
      break;
    case 7:
      reject =
        (detail < 207 && overlap < 230 && filtered < 21 && combined < 404) ||
        (detail < 212 && filtered < 18 && coverage < 151 && topology < 69) ||
        (detail < 214 && filtered < 14 && coverage < 83 && topology < 67) ||
        (detail < 211 && filtered < 15 && coverage < 63 && topology < 101) ||
        (detail < 218 && filtered < 9 && coverage < 69 && topology < 51) ||
        (detail < 223 && filtered < 8 && coverage < 46 && overlap < 224 && topology < 63) ||
        (detail < 211 && filtered < 12 && geometry < 26 && topology < 51) ||
        (detail < 219 && filtered < 9 && geometry < 24 && topology < 59) ||
        (detail < 215 && filtered < 18 && geometry < 19 && topology < 50);
      break;
    case 8:
      reject =
        (detail < 200 && overlap < 228 && filtered < 23 && combined < 393) ||
        (detail < 204 && filtered < 19 && combined < 407) ||
        (detail < 218 && filtered < 16 && coverage < 86) ||
        (detail < 222 && filtered < 12 && coverage < 103) ||
        (detail < 216 && filtered < 9 && coverage < 129) ||
        (detail < 214 && filtered < 16 && geometry < 29) ||
        (detail < 211 && topology < 56 && filtered < 16 && coverage < 171) ||
        (detail < 207 && topology < 61 && filtered < 15 && combined < 411);
      break;
    case 9:
      reject =
        (detail < 206 && overlap < 227 && filtered < 22 && combined < 407) ||
        (detail < 213 && overlap < 240 && filtered < 22 && combined < 386) ||
        (detail < 220 && overlap < 228 && filtered < 12 && coverage < 89) ||
        (detail < 191 && overlap < 228 && filtered < 15 && coverage < 122) ||
        (detail < 215 && overlap < 226 && filtered < 13 && coverage < 129) ||
        (detail < 216 && filtered < 16 && coverage < 68) ||
        (detail < 209 && filtered < 17 && topology < 60) ||
        (detail < 212 && filtered < 10 && topology < 52);
      break;
    case 10:
      reject =
        (detail < 210 && overlap < 233 && filtered < 20 && topology < 44) ||
        (detail < 203 && filtered < 11 && combined < 392 && topology < 47) ||
        (detail < 208 && filtered < 17 && combined < 384 && topology < 51) ||
        (detail < 221 && filtered < 11 && topology < 59 && coverage < 91) ||
        (detail < 211 && overlap < 226 && filtered < 16 && coverage < 129 && topology < 53) ||
        (detail < 213 && overlap < 225 && filtered < 17 && coverage < 101) ||
        (detail < 214 && overlap < 223 && filtered < 13 && coverage < 75) ||
        (detail < 202 && overlap < 228 && filtered < 22 && combined < 398) ||
        (detail < 208 && overlap < 236 && filtered < 11 && combined < 393) ||
        (detail < 202 && overlap <= 235 && filtered < 21 && combined < 381);
      break;
    case 11:
      reject =
        (detail < 206 && topology < 61 && filtered < 21 && combined < 394) ||
        (detail < 215 && overlap < 229 && filtered < 12 && topology < 63) ||
        (detail < 201 && filtered < 12 && topology < 36 && combined < 391) ||
        (detail < 202 && overlap < 227 && filtered < 20 && combined < 401) ||
        (detail < 201 && overlap < 228 && filtered < 17 && combined < 391) ||
        (detail < 220 && overlap < 227 && filtered < 12 && coverage < 87);
      break;
    case 12:
      reject =
        (detail < 191 && filtered < 21 && combined < 381) ||
        (detail < 208 && overlap < 230 && filtered < 23 && combined < 399 && geometry < 35) ||
        (detail < 207 && filtered < 18 && combined < 414 && topology < 45 && geometry < 21) ||
        (detail < 201 && filtered < 13 && combined < 391 && topology < 53 && geometry < 18) ||
        (detail < 207 && overlap < 221 && filtered < 15 && coverage < 129) ||
        (detail < 220 && overlap < 228 && filtered < 13 && coverage < 91) ||
        (detail < 211 && overlap < 211 && filtered < 23 && coverage < 83);
      break;
    case 13:
      reject =
        (detail < 201 && ((overlap < 226 && filtered < 21 && combined < 386) ||
                          (filtered < 14 && combined < 391))) ||
        (detail < 203 && overlap < 228 && filtered < 18 && combined < 395) ||
        (detail < 201 && overlap < 212 && filtered < 14 && combined < 399) ||
        (detail < 208 && overlap < 214 && filtered < 14 && combined < 414 && coverage < 69) ||
        (detail < 201 && overlap < 222 && filtered < 21 && combined < 381);
      break;
    case 14:
      reject =
        (detail < 206 && overlap < 221 && filtered < 22 && topology < 56 && combined < 376) ||
        (detail < 193 && overlap < 216 && filtered < 21 && topology < 66 && combined < 377) ||
        (detail < 201 && ((overlap < 226 && filtered < 19 && topology < 53 && combined < 393) ||
                          (overlap < 228 && filtered < 15 && topology < 37 && combined < 390))) ||
        (detail < 197 && overlap < 227 && filtered < 15 && topology < 47 && combined < 377) ||
        (detail < 199 && filtered < 22 && overlap < 226 && topology < 64 && combined < 386);
      break;
    case 15:
      reject =
        (primary < 17 && detail < 204 && filtered < 22 && overlap < 226 && combined < 396) ||
        (primary < 22 && ((detail < 216 && filtered < 22 && overlap < 129 && combined < 361) ||
                          (detail < 193 && filtered < 22 && overlap < 229 && combined < 376))) ||
        (primary < 18 && detail < 196 && filtered < 18 && overlap < 223 && combined < 383);
      break;
    }
  if (reject)
    return 0;

  int strong =
    (primary > 6 || filtered > 6 || combined > 435 || geometry > 25) &&
    (primary > 4 || ((filtered > 16 || combined > 415 || geometry > 25) &&
                     (filtered > 21 || combined > 405 || topology > 50))) &&
    (primary > 7 || filtered > 10 || combined > 403 || geometry > 15) &&
    (primary > 3 || filtered > 12 || combined > 430 || geometry > 21) &&
    (primary > 4 || ((filtered > 9 || combined > 430 || geometry > 21) &&
                     (filtered > 12 || combined > 425 || geometry > 35) &&
                     (filtered > 18 || combined > 416 || geometry > 35))) &&
    (primary > 5 || ((filtered > 7 || combined > 438 || geometry > 23) &&
                     (filtered > 20 || combined > 410 || geometry > 25 || detail > 208))) &&
    (primary > 6 || ((filtered > 20 || combined > 425 || geometry > 20) &&
                     (filtered > 16 || combined > 430 || coverage < 110))) &&
    (primary > 7 || ((filtered > 9 || combined > 430 || geometry > 23) &&
                     (filtered > 17 || combined > 402 || geometry > 30) &&
                     (filtered > 13 || combined > 415 || geometry > 12))) &&
    (primary > 8 || filtered > 16 || combined > 402 || geometry > 18) &&
    (primary > 10 || filtered > 16 ||
                     ((detail > 210 || geometry > 16) &&
                      (overlap > 229 || combined > 410 || coverage < 110))) &&
    (primary > 12 || ((filtered > 17 || combined > 405 || geometry > 22) &&
                      (filtered > 22 || combined > 370 || geometry > 30 || overlap > 215))) &&
    (primary > 7 || filtered > 14 || detail > 208 || low_detail > 202 || geometry > 18) &&
    (primary > 17 || filtered > 19 || overlap > 226 || detail > 198 ||
                      low_detail > 193 || geometry > 25);

  return strong && (primary > 7 || filtered > 20 || overlap > 215 ||
                    detail > 202 || low_detail > 195 || geometry > 45);
}

static int
first_veto (const int32_t             metrics[GOODIX_MILAN_CANDIDATE_WORDS],
            GoodixMilanMatcherPolicy *policy,
            int32_t      *flag)
{
  int32_t noise = metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_FIRST] +
                  metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_SECOND] +
                  metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_THIRD];
  int32_t primary = metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY];
  int32_t filtered = metrics[GOODIX_MILAN_POLICY_METRIC_FILTERED];
  int32_t coverage = metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE];
  int32_t topology = metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY];
  int32_t geometry = metrics[GOODIX_MILAN_POLICY_METRIC_GEOMETRY];
  int32_t detail = metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] - noise * 2;
  int32_t overlap = metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] - noise * 2;
  int32_t combined =
    metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] + detail - noise * 2;
  int32_t bucket = clamp (primary, 3, 14);
  int survives = 0;

  if ((filtered < 11 && metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 180) ||
      (coverage < 110 && filtered < 8 && combined < 389) ||
      (coverage >= 110 && filtered < 8 && topology < 36 && combined < 389))
    {
      *flag = 0;
      return 0;
    }
  if (!((filtered <= primary || coverage > 49 || filtered > 14 || combined > 390) &&
        (primary != filtered || primary > 8 || combined > 414 ||
         filtered > 10 || geometry > 30 || topology > 60 || coverage > 48)))
    {
      *flag = 0;
      return 0;
    }

  switch (bucket)
    {
    case 3:
    case 4:
      survives = !((detail < 198 && overlap < 226 && filtered < 21 && combined < 386) ||
                   (detail < 206 && filtered < 17 && combined < 410) ||
                   (detail < 225 && filtered < 16 && coverage < 129) ||
                   (detail < 218 && filtered < 14 && topology < 64));
      break;
    case 5:
      survives = !((detail < 198 && overlap < 228 && filtered < 21 && combined < 389) ||
                   (detail < 210 && filtered < 11 && geometry < 13) ||
                   (detail < 226 && filtered < 10 && coverage < 85) ||
                   (detail < 218 && filtered < 11 && coverage < 129) ||
                   (detail < 213 && filtered < 16 && coverage < 129) ||
                   (detail < 215 && filtered < 6 && topology < 39 && geometry < 21) ||
                   (detail < 212 && filtered < 15 && coverage < 181));
      survives = survives &&
                 (detail > 205 || filtered > 16 || topology > 60 || combined > 408);
      break;
    case 6:
      survives = !((detail < 195 && overlap < 226 && filtered < 21 && combined < 386) ||
                   (detail < 226 && filtered < 11 && coverage < 91) ||
                   (detail < 215 && filtered < 15 && coverage < 101) ||
                   (detail < 211 && filtered < 15 && coverage < 201) ||
                   (detail < 215 && filtered < 7 && coverage < 129) ||
                   (detail < 206 && filtered < 18 && combined < 405) ||
                   (detail < 217 && filtered < 9 && topology < 56));
      survives = survives &&
                 (detail > 205 || filtered > 16 || topology > 60 || combined > 410);
      break;
    case 7:
      survives = !((detail < 196 && overlap < 226 && filtered < 22 && combined < 389) ||
                   (detail < 211 && overlap < 228 && filtered < 19 && combined < 407) ||
                   (detail < 223 && overlap < 228 && filtered < 11 && coverage < 101) ||
                   (detail < 210 && overlap < 216 && filtered < 14 && coverage < 81) ||
                   (detail < 209 && filtered < 16 && coverage < 141 && topology < 67) ||
                   (detail < 214 && filtered < 11 && coverage < 151 && topology < 67) ||
                   (detail < 201 && filtered < 15 && combined < 399 && topology < 56) ||
                   (detail < 213 && filtered < 16 && geometry < 22 && topology < 51));
      survives = survives &&
                 (detail > 205 || filtered > 13 || geometry > 28 || topology > 55);
      break;
    case 8:
      survives = !((detail < 197 && overlap < 225 && filtered < 22 && combined < 390) ||
                   (detail < 201 && overlap < 226 &&
                    ((filtered < 16 && combined < 404) ||
                     (filtered < 18 && combined < 397))) ||
                   (detail < 223 && overlap < 226 && filtered < 13 && coverage < 81) ||
                   (detail < 216 && overlap < 229 && filtered < 11 && coverage < 141) ||
                   (detail < 212 && topology < 61 && filtered < 20 && coverage < 201));
      survives = survives &&
                 (detail > 205 || topology > 60 || filtered > 14 || combined > 405);
      break;
    case 9:
      survives = !((detail < 196 && overlap < 221 && filtered < 23 && combined < 386) ||
                   (detail < 204 && overlap < 227 && filtered < 20 && combined < 407) ||
                   (detail < 205 && overlap < 216 && filtered < 19 && coverage < 56) ||
                   (detail < 217 && overlap < 226 && filtered < 10 && coverage < 129) ||
                   (detail < 209 && overlap < 230 && filtered < 10 && topology < 66));
      survives = survives &&
                 (detail > 204 || overlap > 220 || filtered > 15 || topology > 50);
      break;
    case 10:
      survives = !((detail < 196 && overlap < 223 && filtered < 23 && combined < 379) ||
                   (detail < 211 && overlap < 228 && filtered < 20 && topology < 34) ||
                   (detail < 209 && overlap < 221 && filtered < 16 && coverage < 129 && topology < 50) ||
                   (detail < 205 && overlap < 226 && filtered < 19 && combined < 400) ||
                   (detail < 213 && overlap < 220 && filtered < 11 && coverage < 111));
      survives = survives &&
                 (detail > 205 || overlap > 228 || filtered > 15 || combined > 407);
      break;
    case 11:
      survives = !((detail < 198 && topology < 51 && filtered < 22 && combined < 387) ||
                   (detail < 210 && overlap < 219 && filtered < 12 && coverage < 81) ||
                   (detail < 207 && overlap < 221 && filtered < 16 && coverage < 129) ||
                   (detail < 205 && overlap < 228 && filtered < 12 && topology < 46));
      survives = survives &&
                 (detail > 201 || overlap > 225 || filtered > 17 || combined > 400);
      break;
    case 12:
      survives = !((detail < 191 && overlap < 220 && filtered < 22 && combined < 374) ||
                   (detail < 197 && overlap < 219 && filtered < 19 && combined < 399) ||
                   (detail < 207 && overlap < 221 && filtered < 13 && coverage < 101) ||
                   (detail < 205 && overlap < 226 && filtered < 20 && topology < 51));
      survives = survives &&
                 (detail > 195 || overlap > 225 || filtered > 15 || combined > 385);
      break;
    case 13:
      survives = !((detail < 191 && overlap < 221 && filtered < 22 && combined < 375) ||
                   (detail < 199 && overlap < 225 && filtered < 18 && combined < 391));
      survives = survives &&
                 (detail > 203 || overlap > 226 || filtered > 13 ||
                  topology > 36 || combined > 396);
      break;
    case 14:
      survives = !((detail < 192 && filtered < 25 && topology < 46 && combined < 373) ||
                   (detail < 187 && filtered < 22 && overlap < 219 && combined < 371) ||
                   (detail < 198 && filtered < 22 && overlap < 223 && combined < 385));
      survives = survives &&
                 (detail > 180 || filtered > 21 || topology > 30 || combined >= 363);
      break;
    }
  if (!survives)
    {
      *flag = 0;
      return 0;
    }

  if (!first_veto_type12 (metrics, policy))
    *flag = 0;
  return 0;
}

static int
post_veto_type12 (const int32_t metrics[GOODIX_MILAN_CANDIDATE_WORDS],
                   const int32_t config[20])
{
  int32_t noise = metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_FIRST] +
                  metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_SECOND] +
                  metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_THIRD];
  int32_t primary = metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] -
                    config[GOODIX_MILAN_POLICY_CONFIG_PRIMARY_OFFSET];
  int32_t filtered = metrics[GOODIX_MILAN_POLICY_METRIC_FILTERED] -
                     config[GOODIX_MILAN_POLICY_CONFIG_PRIMARY_OFFSET];
  int32_t coverage = metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE];
  int32_t topology = metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY];
  int32_t geometry = metrics[GOODIX_MILAN_POLICY_METRIC_GEOMETRY];
  int32_t detail = metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] -
                   config[GOODIX_MILAN_POLICY_CONFIG_METRIC_OFFSET] - noise * 2;
  int32_t overlap = metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] -
                    config[GOODIX_MILAN_POLICY_CONFIG_METRIC_OFFSET] - noise * 2;
  int32_t low_detail = metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] - noise * 2;
  int32_t combined = metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] +
                     metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] -
                     config[GOODIX_MILAN_POLICY_CONFIG_METRIC_OFFSET] - noise * 4;
  int32_t bucket = clamp (primary, 4, 11);
  int survives = 0;

  if (bucket < 7 && geometry < 10 && overlap < 210)
    return 0;
  switch (bucket)
    {
    case 4:
      survives =
        ((filtered > 13 || overlap > 220 || detail > 209) &&
         (filtered > 16 || detail > 199 || geometry > 28) &&
         (filtered > 12 || overlap > 224 || detail > 217 || coverage > 115) &&
         (filtered > 10 || detail > 211 || geometry > 17)) &&
        (filtered > 8 || coverage > 145);
      break;
    case 5:
      survives =
        ((filtered > 6 || overlap > 227 || detail > 215 || coverage > 148) &&
         (filtered > 13 || detail > 212 || geometry > 40) &&
         (filtered > 12 || overlap > 224 || detail > 215 || coverage > 111) &&
         (filtered > 17 || detail > 199 || geometry > 28)) &&
        (filtered > 8 || coverage > 76);
      break;
    case 6:
      survives =
        (filtered > 10 || ((detail > 215 || geometry > 20) &&
                           (coverage > 119 || geometry > 24))) &&
        (filtered > 11 || overlap > 224 || detail > 215 || coverage > 90) &&
        (filtered > 6 || overlap > 223 || detail > 213 || geometry > 25) &&
        (filtered > 15 || detail > 206 || geometry > 20) &&
        (filtered > 14 || overlap > 224 || detail > 214 || coverage > 128) &&
        (filtered > 8 || coverage > 44);
      break;
    case 7:
      survives =
        (filtered > 10 || overlap > 227 || detail > 206 || geometry > 15) &&
        (filtered > 8 || overlap > 225 || detail > 208 || geometry > 23) &&
        (filtered > 17 || overlap > 219 || detail > 214 || coverage > 120) &&
        (filtered > 15 || detail > 202 || geometry > 20) &&
        (filtered > 13 || overlap > 224 || detail > 214 || coverage > 129);
      break;
    case 8:
      survives =
        (filtered > 11 || detail > 211 || geometry > 25) &&
        (filtered > 8 || overlap > 218 || detail > 205 || coverage > 108) &&
        (filtered > 11 || detail > 209 || coverage > 99 || low_detail > 200) &&
        (filtered > 19 || overlap > 200 || detail > 175 || low_detail > 175) &&
        (filtered > 16 || overlap > 225 || detail > 206 || coverage > 128) &&
        (filtered > 18 || detail > 200 || geometry > 27);
      break;
    case 9:
      survives =
        (filtered > 10 || overlap > 219 || detail > 214 || coverage > 87) &&
        (filtered > 17 || overlap > 226 || detail > 208 || geometry > 26) &&
        (filtered > 15 || overlap > 214 || detail > 190 || coverage > 120) &&
        (filtered > 9 || detail > 209 || coverage > 104 || low_detail > 204);
      break;
    case 10:
      survives =
        (filtered > 19 || overlap > 224 || detail > 204 || geometry > 31) &&
        (filtered > 10 || detail > 205 || coverage > 98);
      break;
    case 11:
      survives =
        (filtered > 12 || overlap > 217 || detail > 201 || coverage > 170) &&
        (filtered > 18 || coverage > 132 || combined > 386) &&
        (filtered > 20 || overlap > 222 || detail > 196 || geometry > 15);
      break;
    }
  if (!survives)
    return 0;

  switch (bucket)
    {
    case 4:
      survives =
        (filtered > 13 || coverage > 150 || detail > 212) &&
        (filtered > 17 || detail > 203 || combined > 399) &&
        (filtered > 15 || detail > 208 || geometry > 20) &&
        (filtered > 11 || detail > 209 || combined > 410) &&
        (filtered > 9 || detail > 218 || coverage > 70) &&
        (filtered > 7);
      break;
    case 5:
      survives =
        (filtered > 9 || detail > 216 ||
         ((geometry > 18 || coverage > 133) &&
          (geometry > 15 || coverage > 145))) &&
        (filtered > 16 || detail > 197 || geometry > 36 || coverage > 109) &&
        (filtered > 19 || combined > 393 || geometry > 28) &&
        (filtered > 14 || combined > 414 || geometry > 16) &&
        (filtered > 7 || combined > 416 || geometry > 10) &&
        (filtered > 8 || coverage > 87) &&
        (filtered > 9 || coverage > 46) &&
        (filtered > 5 || coverage > 108 || geometry > 26);
      break;
    case 6:
      survives =
        (filtered > 12 || detail > 213 || geometry > 22) &&
        (filtered > 7 || detail > 217 || geometry > 15) &&
        (filtered > 20 || combined > 399 || geometry > 34) &&
        (filtered > 9 || coverage > 83) &&
        (filtered > 10 || coverage > 41) &&
        (filtered > 8 || coverage > 111 || detail > 209) &&
        (filtered > 6 || detail > 208 || geometry > 28);
      break;
    case 7:
      survives =
        (filtered > 9 || detail > 212 || geometry > 20) &&
        (filtered > 20 || combined > 390 || geometry > 28) &&
        (filtered > 13 || combined > 411 || geometry > 27) &&
        (filtered > 8 || coverage > 88) &&
        (filtered > 9 || coverage > 45) &&
        (filtered > 7 ||
         ((coverage > 101 || detail > 209) &&
          (coverage > 114 || detail > 203)));
      break;
    case 8:
      survives =
        (filtered > 20 || detail > 199 || combined > 392 || geometry > 28) &&
        (filtered > 18 || overlap > 227 || detail > 216 || coverage > 128) &&
        (filtered > 10 || detail > 212 || geometry > 20) &&
        (filtered > 8 || coverage > 82) &&
        (filtered > 15 || detail > 208 || combined > 398) &&
        (filtered > 8 || detail > 204 || geometry > 34);
      break;
    case 9:
      survives =
        (filtered > 22 || detail > 199 || combined > 390 || geometry > 22) &&
        (filtered > 15 || detail > 203 || combined > 398 || geometry > 15) &&
        (filtered > 16 || overlap > 227 || detail > 216 || coverage > 130) &&
        (filtered > 9 || detail > 212 || geometry > 26) &&
        (filtered > 9 || detail > 208 || combined > 398);
      break;
    case 10:
      survives =
        (filtered > 19 || detail > 200 || combined > 394 || geometry > 20) &&
        (filtered > 16 || overlap > 227 || detail > 210 || coverage > 128) &&
        (filtered > 10 ||
         ((overlap > 225 || detail > 210 || coverage > 138) &&
          (overlap > 220 || detail > 212 || coverage > 74)));
      break;
    case 11:
      survives =
        (filtered > 21 || detail > 196 || combined > 385 || geometry > 28) &&
        (filtered > 19 || detail > 200 || combined > 394 || geometry > 21) &&
        (filtered > 21 || detail > 190 || combined > 375 || geometry > 45) &&
        (filtered > 16 || overlap > 226 || detail > 207 || coverage > 129) &&
        (filtered > 11 || detail > 202 || combined > 396 || geometry > 25);
      break;
    }
  if (!survives)
    return 0;

  return !(
    (primary < 7 && filtered < 7 && combined < 436 && geometry < 26) ||
    (primary < 5 && filtered < 17 && combined < 416 && geometry < 26) ||
    (primary < 5 && filtered < 22 && combined < 406 && topology < 51) ||
    (primary < 8 && filtered < 11 && combined < 404 && geometry < 16) ||
    (primary < 4 && filtered < 13 && combined < 431 && geometry < 22) ||
    (primary < 5 && filtered < 10 && combined < 431 && geometry < 22) ||
    (primary < 5 && filtered < 13 && combined < 426 && geometry < 36) ||
    (primary < 5 && filtered < 19 && combined < 417 && geometry < 36) ||
    (primary < 6 && filtered < 8 && combined < 439 && geometry < 24) ||
    (primary < 6 && filtered < 21 && combined < 411 && geometry < 26 && detail < 209) ||
    (primary < 7 && filtered < 21 && combined < 426 && geometry < 21) ||
    (primary < 7 && filtered < 17 && combined < 431 && coverage > 109) ||
    (primary < 8 && filtered < 10 && combined < 431 && geometry < 24) ||
    (primary < 8 && filtered < 18 && combined < 403 && geometry < 31) ||
    (primary < 8 && filtered < 14 && combined < 416 && geometry < 13) ||
    (primary < 9 && filtered < 17 && combined < 403 && geometry < 19) ||
    (primary < 11 && filtered < 17 && detail < 211 && geometry < 17) ||
    (primary < 11 && filtered < 17 && overlap < 230 && combined < 411 && coverage > 109) ||
    (primary < 13 && filtered < 18 && combined < 406 && geometry < 23) ||
    (primary < 13 && filtered < 23 && combined < 371 && geometry < 31 && overlap < 216) ||
    (primary < 8 && filtered < 15 && detail < 209 && low_detail < 203 && geometry < 19) ||
    (primary < 18 && filtered < 20 && overlap < 227 && detail < 199 &&
     low_detail < 194 && geometry < 26) ||
    (primary < 8 && filtered < 21 && overlap < 216 && detail < 203 &&
     low_detail < 196 && geometry < 46));
}

static void
post_veto (const int32_t             metrics[GOODIX_MILAN_CANDIDATE_WORDS],
           GoodixMilanMatcherPolicy *policy,
           int32_t       flags[2])
{
  int32_t noise = metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_FIRST] +
                  metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_SECOND] +
                  metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_THIRD];
  int32_t primary = metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] -
                    policy->configuration[GOODIX_MILAN_POLICY_CONFIG_PRIMARY_OFFSET];
  int32_t filtered = metrics[GOODIX_MILAN_POLICY_METRIC_FILTERED] -
                     policy->configuration[GOODIX_MILAN_POLICY_CONFIG_PRIMARY_OFFSET];
  int32_t coverage = metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE];
  int32_t geometry = metrics[GOODIX_MILAN_POLICY_METRIC_GEOMETRY];
  int32_t detail = metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] -
                   policy->configuration[GOODIX_MILAN_POLICY_CONFIG_METRIC_OFFSET] -
                   noise * 2;
  int32_t overlap = metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] -
                    policy->configuration[GOODIX_MILAN_POLICY_CONFIG_METRIC_OFFSET] -
                    noise * 2;
  int32_t low_detail = metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] - noise * 2;
  int32_t combined = metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] +
                     metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] -
                     policy->configuration[GOODIX_MILAN_POLICY_CONFIG_METRIC_OFFSET] -
                     noise * 4;

  if ((policy->configuration[GOODIX_MILAN_POLICY_CONFIG_PACKED_MODE] >> 8) > 1 &&
      ((detail < 206 && low_detail < 205 && coverage > 100 && geometry < 45) ||
       (detail < 200 && low_detail < 208 && coverage > 110 && geometry < 45) ||
       (detail < 209 && low_detail < 195 && coverage > 120 && geometry < 45)))
    {
      flags[0] = flags[1] = 0;
      policy->configuration[GOODIX_MILAN_POLICY_CONFIG_RECOGNITION_MODE] = 0;
      return;
    }
  if ((metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_SECOND] == 1 ||
       metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 7) &&
      combined < 385 && filtered < 10)
    flags[0] = flags[1] = 0;
  if (metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] == 128 &&
      metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 175 &&
      (metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 6 ||
       metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_SECOND] != 0 ||
       (metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 13 && coverage < 180)))
    flags[0] = flags[1] = 0;
  if (metrics[GOODIX_MILAN_POLICY_METRIC_PENALTY_THIRD] == 1 && primary < 12 &&
      (combined < 385 || (combined < 395 && coverage < 100)))
    flags[0] = flags[1] = 0;
  if (flags[1] != 0 &&
      policy->configuration[GOODIX_MILAN_POLICY_CONFIG_PACKED_MODE] != 0 &&
      coverage > 200 &&
      combined < 405 &&
      ((metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] < 45 && geometry < 15 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY_DISTANCE] > 16) ||
       metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] == 128))
    flags[0] = flags[1] = 0;
  if (flags[1] != 0 &&
      ((filtered < 22 && combined < 371) ||
       (filtered < 16 && combined < 381) ||
       (filtered < 12 && combined < 390) ||
       (filtered < 8 && combined < 400) || coverage < 20 ||
       (coverage < 45 && filtered < 8 && combined < 415) ||
       (coverage < 95 && ((filtered < 8 && combined < 395) ||
                          (filtered < 10 && combined < 390))) ||
       (coverage < 110 && ((filtered < 6 && combined < 399) ||
                           (filtered < 10 && combined < 385))) ||
       (coverage < 128 && filtered < 8 && combined < 390)))
    flags[0] = flags[1] = 0;
  if (flags[1] != 0 && primary < filtered &&
      ((primary < 6 && ((filtered < 13 && combined < 406 && detail < 211) ||
                        (filtered < 11 && combined < 411 && detail < 211))) ||
       (primary < 8 && filtered < 16 && combined < 406 && detail < 208) ||
       (primary < 16 && filtered < 25 && combined < 376)))
    flags[0] = flags[1] = 0;
  if (overlap == 128 || detail < 176 || coverage < 31 || geometry < 6)
    flags[0] = flags[1] = 0;
  if (flags[1] == 0)
    return;

  if (!post_veto_type12 (metrics, policy->configuration))
    flags[0] = flags[1] = 0;
}

static void
late_eligibility (const int32_t metrics[GOODIX_MILAN_CANDIDATE_WORDS],
                  int32_t       image_quality,
                  int32_t       image_coverage,
                  int32_t       output[2])
{
  int32_t quality = image_quality;
  int32_t primary = metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] > 12 ?
                    12 : metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY];
  int32_t quality_band = (quality - 1) / 10;
  int strict = 0;
  int broad = 0;

  if (image_coverage > 34)
    {
      switch (primary)
        {
        case 6:
          strict = quality_band < 6 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] > 229 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 210 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 40 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 115;
          break;
        case 7:
          strict =
            (quality_band < 6 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] > 224 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 206 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 35 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 149) ||
            (quality_band < 5 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 50 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 212 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] > 195 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 55 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] > 210);
          break;
        case 8:
          strict = quality_band < 6 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] > 224 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 206 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 34 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 153;
          break;
        case 9:
          strict =
            (quality_band < 7 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] > 230 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 206 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 35 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 110) ||
            (quality_band < 5 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] > 218 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 202 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 29 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 152 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] > 182);
          break;
        case 10:
          strict =
            (quality_band < 7 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] > 221 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 206 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 30 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 111) ||
            (quality_band < 6 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] > 218 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 203 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 42 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 143);
          break;
        case 11:
          strict =
            (quality_band < 3 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] > 219 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 195 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 35 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 119) ||
            (quality_band < 6 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] > 219 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 201 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 36 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 150) ||
            (quality_band < 7 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] > 219 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 203 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 37 &&
             metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 135);
          break;
        case 12:
          strict = quality_band < 6 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_OVERLAP] > 215 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 203 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] > 33 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 115;
          break;
        }
    }
  if (!strict && image_coverage >= 35)
    {
      switch (primary)
        {
        case 5:
          broad =
            (quality_band <= 4 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 48 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 213 && metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] >= 52) ||
            (quality_band <= 3 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 41 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 210 && metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] >= 52) ||
            (quality_band <= 2 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 36 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 209 && metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 65);
          break;
        case 6:
          broad =
            (quality_band <= 4 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 46 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 211 && metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] >= 51) ||
            (quality_band <= 3 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 41 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 207 && metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] >= 51) ||
            (quality_band <= 2 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 36 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 206 && metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 50);
          break;
        case 7:
          broad =
            (quality_band <= 4 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 46 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 204 && metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] >= 65 && metrics[GOODIX_MILAN_POLICY_METRIC_FILTERED] >= 8) ||
            (quality_band <= 3 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 41 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 204 && metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 50);
          break;
        case 8:
          broad =
            (quality_band <= 4 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 46 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 204 && metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] >= 61) ||
            (quality_band <= 3 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 41 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 203 && metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] >= 51) ||
            (quality_band <= 2 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 36 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 201 && metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] > 50);
          break;
        case 9:
          broad =
            (quality_band <= 4 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 46 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 201) ||
            (quality_band <= 3 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 41 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 196) ||
            (quality_band <= 2 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 36 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 195);
          break;
        case 10:
          broad =
            (quality_band <= 4 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 51 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 194) ||
            (quality_band <= 3 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 47 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 193) ||
            (quality_band <= 2 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 36 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 194);
          break;
        case 11:
          broad =
            (quality_band <= 4 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 47 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 187) ||
            (quality_band <= 3 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 47 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 192) ||
            (quality_band <= 2 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 47 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 191);
          break;
        case 12:
          broad =
            (quality_band <= 4 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 52 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 186) ||
            (quality_band <= 3 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 49 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] >= 193) ||
            (quality_band <= 2 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] >= 36 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] > 196);
          break;
        }
    }
  output[0] = strict && image_quality > 15 && image_coverage > 64;
  output[1] = strict || broad;
}

void
goodix_milan_matcher_policy_init (GoodixMilanMatcherPolicy *policy,
                                  int32_t                   packed_mode)
{
  static const int32_t profile9_type12[20] = {
    0, 0, 5, 218, 1, 1, 23, 47, 40, 38,
    -1, 16, 246, 1, 0, GOODIX_MILAN_PRINT_SENSOR_TYPE, 0, 1, 0, 1,
  };

  memcpy (policy->configuration, profile9_type12, sizeof(profile9_type12));
  policy->configuration[GOODIX_MILAN_POLICY_CONFIG_PACKED_MODE] = packed_mode;
}

void
goodix_milan_matcher_policy_evaluate (
  GoodixMilanMatcherPolicy      *policy,
  const int32_t             metrics[GOODIX_MILAN_CANDIDATE_WORDS],
  int32_t                        image_quality,
  int32_t                        image_coverage,
  int32_t                        accumulated_high_class,
  int32_t                       *match_flag,
  int32_t                       *candidate_flag)
{
  int32_t flags[2] = { *match_flag, *candidate_flag };
  int32_t output[2] = { 0, 0 };

  if (!(flags[0] == 2 && accumulated_high_class <= 1))
    {
      if (flags[0] == 0 || flags[1] == 0)
        {
          initial_classifier (metrics, image_quality, image_coverage,
                              policy->configuration, output);
          flags[0] |= output[0];
          flags[1] |= output[1];
        }
      if (flags[1] == 0)
        {
          fallback_classifier (metrics, policy->configuration, output);
          flags[0] = output[0];
          flags[1] |= output[1];
        }
      if (flags[0] != 0)
        first_veto (metrics, policy, &flags[0]);
      if (flags[1] != 0 && flags[0] == 0)
        post_veto (metrics, policy, flags);
      if (flags[0] == 0 &&
          policy->configuration[GOODIX_MILAN_POLICY_CONFIG_RECOGNITION_MODE] == 1)
        {
          output[0] = output[1] = 0;
          late_eligibility (metrics, image_quality, image_coverage, output);
          flags[0] |= output[0];
          flags[1] |= output[1];
        }
    }

  *match_flag = flags[0];
  *candidate_flag = flags[1];
}

void
goodix_milan_matcher_policy_apply_final (
  const int32_t metrics[GOODIX_MILAN_CANDIDATE_WORDS],
  int32_t       probe_coverage,
  int32_t       accumulated_high_class,
  int32_t       probe_low_class,
  int32_t       support_ratio_q8,
  int32_t      *match_flag,
  int32_t      *candidate_flag)
{
  int32_t scaled;
  int entry_guard;
  int weak_metric;

  if (!metrics || !match_flag || !candidate_flag || *candidate_flag == 0 ||
      (accumulated_high_class < 2 && probe_low_class == 0))
    return;

  scaled = policy_arithmetic_shift_8 (
    policy_wrap_multiply (metrics[GOODIX_MILAN_POLICY_METRIC_GEOMETRY],
                          metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE]));
  entry_guard =
    (accumulated_high_class >= 3 && probe_low_class >= 4 &&
     probe_coverage < 80 && metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] < 70) ||
    (accumulated_high_class >= 2 && probe_low_class >= 2 &&
     probe_coverage < 55 && scaled < 20);
  weak_metric =
    (metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 19 &&
     metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 201 &&
     metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 200 &&
     scaled < 15) ||
    (metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 13 &&
     ((accumulated_high_class >= 4 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 201 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 200) ||
      (metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 216 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 210 && scaled < 18))) ||
    (metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 11 &&
     ((metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 211 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 215 && scaled < 15) ||
      (metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 201 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 200)));
  if (*candidate_flag == 1 && entry_guard && weak_metric)
    *match_flag = *candidate_flag = 0;

  if ((accumulated_high_class >= 4 && probe_low_class == 1 &&
       probe_coverage < 50 && support_ratio_q8 > 219) ||
      (accumulated_high_class >= 3 && probe_low_class >= 2 &&
       probe_coverage < 50 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 8 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 203 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 200 && scaled < 17 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] < 170) ||
      (accumulated_high_class >= 2 &&
       ((probe_low_class >= 3 && support_ratio_q8 > 149 &&
         metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 201 &&
         metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 200 && scaled < 17) ||
        (probe_coverage < 35 &&
         metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 208 &&
         metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 197 &&
         scaled < 20))))
    *match_flag = 0;
}

void
goodix_milan_matcher_policy_apply_late_veto (
  const GoodixMilanMatcherPolicy *policy,
  const int32_t                   metrics[GOODIX_MILAN_CANDIDATE_WORDS],
  int32_t                        *match_flag,
  int32_t                        *candidate_flag)
{
  if (policy->configuration[GOODIX_MILAN_POLICY_CONFIG_PACKED_MODE] > 0 &&
      policy->configuration[GOODIX_MILAN_POLICY_CONFIG_SUBTYPE] ==
      GOODIX_MILAN_PRINT_SENSOR_TYPE &&
      ((metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 9 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_GEOMETRY] < 12) ||
       (metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 8 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_GEOMETRY] < 14) ||
       (metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 7 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_GEOMETRY] < 16) ||
       (metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 6 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_GEOMETRY] < 18) ||
       (metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 5 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_GEOMETRY] < 20)))
    *match_flag = *candidate_flag = 0;
}

static int
transform_within_limits (const int32_t transform[6],
                         const int32_t limits[6])
{
  static const int32_t identity[6] = { 256, 0, 0, 0, 256, 0 };

  for (size_t i = 0; i < 6; i++)
    {
      int64_t delta = (int64_t) transform[i] - identity[i];

      if (delta < 0)
        delta = -delta;
      if (delta > limits[i])
        return 0;
    }
  return 1;
}

static int
mode_transform_close_type12 (const int32_t transform[6],
                             int32_t       mode)
{
  static const int32_t limits[5][6] = {
    { 25, 21, 1792, 21, 25, 1792 },
    { 25, 21, 1792, 21, 25, 1792 },
    { 25, 21, 1792, 21, 25, 1792 },
    { 38, 38, 3072, 38, 38, 3072 },
    { 45, 40, 3840, 40, 45, 3840 },
  };

  return mode >= 1 && mode <= 5 &&
         transform_within_limits (transform, limits[mode - 1]);
}

static int
transform_class_close_type12 (const int32_t transform[6],
                              int32_t       threshold_class)
{
  static const int32_t limits[4][6] = {
    { 4, 6, 256, 6, 4, 256 },
    { 10, 10, 512, 10, 10, 512 },
    { 20, 20, 1280, 20, 20, 1280 },
    { 30, 30, 2560, 30, 30, 2560 },
  };

  return threshold_class >= 0 && threshold_class <= 3 &&
         transform_within_limits (transform, limits[threshold_class]);
}

static void
decode_packed_c7 (int32_t  packed_c7,
                  int32_t *low_class,
                  int32_t *high_class)
{
  static const int32_t low_classes[4] = { 0, 2, 2, 3 };
  static const int32_t high_classes[8] = { 0, 1, 2, 4, 5, 5, 0, 0 };
  uint32_t packed = (uint32_t) packed_c7;

  *low_class = low_classes[packed & 3];
  *high_class = high_classes[(packed >> 8) & 7];
}

void
goodix_milan_matcher_late_context_init (
  GoodixMilanMatcherLateContext *context,
  int32_t                        packed_probe_c7,
  int32_t                        probe_primary_histogram_class)
{
  decode_packed_c7 (packed_probe_c7, &context->probe_low_class,
                    &context->accumulated_high_class);
  /* Object-aware production paths provide the independent histogram class;
   * raw legacy APIs fall back to the decoded packed-c7 low class. */
  context->probe_primary_histogram_class =
    probe_primary_histogram_class < 0
      ? context->probe_low_class : probe_primary_histogram_class;
}

void
goodix_milan_matcher_late_context_derive (
  GoodixMilanMatcherLateContext *context,
  int32_t                        packed_feature_c7,
  int32_t                        state[3])
{
  int32_t feature_low_class;
  int32_t feature_high_class;

  decode_packed_c7 (packed_feature_c7, &feature_low_class,
                    &feature_high_class);
  state[1] = context->probe_primary_histogram_class;
  state[2] = context->accumulated_high_class + feature_high_class;
  if (state[2] > 5)
    state[2] = 5;
  if (feature_high_class > 3)
    context->accumulated_high_class =
      context->accumulated_high_class > feature_high_class
        ? context->accumulated_high_class : feature_high_class;
  state[0] = context->accumulated_high_class;
}

static int32_t
overlap_margin_type12 (int32_t       accumulated_high_class,
                       int32_t       primary_histogram_class,
                       const int32_t metrics[GOODIX_MILAN_CANDIDATE_WORDS],
                       int32_t       context_record_count,
                       int32_t       image_quality)
{
  int32_t margin;

  if (accumulated_high_class < 2)
    margin = 10;
  else if (accumulated_high_class < 4)
    {
      if (metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 197 &&
          metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 195)
        margin = 20;
      else if (primary_histogram_class > 1)
        margin = 15;
      else if (primary_histogram_class == 1)
        margin = 13;
      else
        margin = 10;
    }
  else
    /* The caller caps accumulated_high_class at five. */
    margin = 20;

  if (metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] < 9 &&
      image_quality < 65 && margin < 15)
    margin = 15;

  int32_t normalized = context_record_count * 100 / 120;
  int32_t quality_delta = image_quality - normalized;

  if (quality_delta > 0)
    margin += quality_delta / 10;
  return margin;
}

static int
overlap_eligible_type12 (const int32_t state[3],
                         const int32_t metrics[GOODIX_MILAN_CANDIDATE_WORDS],
                         int32_t       image_quality,
                         int32_t       image_coverage)
{
  int64_t primary_filtered_delta =
    (int64_t) metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] -
    metrics[GOODIX_MILAN_POLICY_METRIC_FILTERED];

  if (primary_filtered_delta < 0)
    primary_filtered_delta = -primary_filtered_delta;
  if (state[0] >= 4 || state[2] >= 4 ||
      (state[0] >= 2 && state[1] >= 1) ||
      (state[0] >= 2 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 230 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 215 &&
       image_coverage <= 60) ||
      (state[0] >= 2 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 215 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 215 &&
       image_coverage < 87) ||
      (state[0] >= 1 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 208 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 214 &&
       image_coverage < 85) ||
      (state[0] >= 0 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] <= 195 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] <= 195 &&
       image_coverage <= 85) ||
      (state[0] >= 0 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] <= 200 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] <= 203 &&
       image_coverage <= 50) ||
      (state[0] >= 0 && metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] <= 205 &&
       metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] <= 208 &&
       image_coverage <= 40))
    return 1;

  return primary_filtered_delta < 7 && image_coverage < 46 &&
         image_quality < 45 &&
         metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] > 5;
}

static int32_t
transformed_in_bounds_area_type12 (const int32_t transform[6])
{
  const int32_t maximum_x =
    (GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS - 1) * 256;
  const int32_t maximum_y =
    (GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS - 1) * 256;
  int32_t area = 0;

  for (int32_t y = 0; y < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS; y++)
    for (int32_t x = 0; x < GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS; x++)
      {
        int64_t source_x = (int64_t) x * transform[0] +
                           (int64_t) y * transform[1] + transform[2];
        int64_t source_y = (int64_t) x * transform[3] +
                           (int64_t) y * transform[4] + transform[5];

        if (source_x >= 0 && source_x <= maximum_x &&
            source_y >= 0 && source_y <= maximum_y)
          area++;
      }
  return area;
}

static int
overlap_status_type12 (int32_t area,
                       int32_t margin)
{
  return area * 100 >
         (100 - margin) * GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS *
         GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS;
}

static int
overlap_clears_match_flag_type12 (int32_t area,
                              int32_t margin)
{
  return area * 100 >
         (95 - margin) * GOODIX_MILAN_EXTRACTION_CLASSIFICATION_ROWS *
         GOODIX_MILAN_EXTRACTION_CLASSIFICATION_COLUMNS;
}

int
goodix_milan_matcher_policy_apply_status (
  const int32_t metrics[GOODIX_MILAN_CANDIDATE_WORDS],
  const int32_t transform[6],
  const int32_t state[3],
  int32_t       context_record_count,
  int32_t       image_quality,
  int32_t       image_coverage,
  int32_t      *match_flag,
  int32_t      *status_counter)
{
  int current_close = mode_transform_close_type12 (transform, state[0]);
  int status = 0;

  if (current_close)
    {
      if (state[0] >= 4)
        status = 1;
      else if (state[0] >= 2 &&
               metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] > 6 &&
               metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] < 66 &&
               image_coverage < 85 &&
               ((metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 201 &&
                 metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 215) ||
                (metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 215 &&
                 metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 210)))
        status = 1;
      else if (state[0] == 1 &&
               metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] > 6 &&
               metrics[GOODIX_MILAN_POLICY_METRIC_TOPOLOGY] < 66 &&
               image_coverage < 85 &&
               ((metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 210 &&
                 metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 200) ||
                (metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 205 &&
                 metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 210)))
        status = 1;
    }

  if (state[0] > 1)
    {
      if (mode_transform_close_type12 (transform, 5) &&
          metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 201 &&
          metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 190)
        *match_flag = 0;
      if (mode_transform_close_type12 (transform, 4) &&
          metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 210 &&
          metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 207)
        status = 1;
      if (image_coverage < 61 &&
          metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] > 6)
        {
          if (transform_class_close_type12 (transform, 0) &&
              metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 240 &&
              metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 237)
            status = 1;
          else if (transform_class_close_type12 (transform, 1) &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 235 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 236)
            status = 1;
          else if (transform_class_close_type12 (transform, 2) &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 226 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 228)
            status = 1;
          else if (transform_class_close_type12 (transform, 3) &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 216 &&
                   metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 218)
            status = 1;
        }
    }

  if (state[0] > 0 && image_coverage < 61 &&
      metrics[GOODIX_MILAN_POLICY_METRIC_PRIMARY] > 6)
    {
      if (transform_class_close_type12 (transform, 0) &&
          metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 234 &&
          metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 233)
        status = 1;
      else if (transform_class_close_type12 (transform, 1) &&
               metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 230 &&
               metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 233)
        status = 1;
      else if (transform_class_close_type12 (transform, 2) &&
               metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 216 &&
               metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 221)
        status = 1;
      else if (transform_class_close_type12 (transform, 3) &&
               metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 206 &&
               metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 208)
        status = 1;
    }

  /* Profile-9/subtype-12 callers provide only nonnegative state classes. */
  if (transform_class_close_type12 (transform, 3) && image_coverage < 76 &&
      metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 205 &&
      metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 200)
    *match_flag = 0;

  if (transform_class_close_type12 (transform, 0) && image_coverage < 72 &&
      ((metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 211 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 215) ||
       (image_coverage < 31 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 218 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 220) ||
       (image_coverage < 16 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 223 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 220)))
    status = 1;

  if (transform_class_close_type12 (transform, 1) &&
      ((image_coverage < 76 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 205 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 202) ||
       (image_coverage < 79 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 202 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 201) ||
       /* coverage<78,m5<198,m8<196 is dominated by the preceding tuple. */
       (image_coverage < 83 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 195 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 192) ||
       (image_coverage < 32 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 218 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 220) ||
       (image_coverage < 37 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 201 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 206)))
    status = 1;

  if (transform_class_close_type12 (transform, 2) &&
      ((image_coverage < 65 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 212 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 203) ||
       (image_coverage < 84 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 194 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 189) ||
       (image_coverage < 82 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 202 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 193) ||
       (image_coverage < 33 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 212 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 215) ||
       (image_coverage < 78 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 198 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 197)))
    status = 1;

  if (transform_class_close_type12 (transform, 3) &&
      ((image_coverage < 76 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 194 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 195) ||
       (image_coverage < 17 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_DETAIL] < 199 &&
        metrics[GOODIX_MILAN_POLICY_METRIC_COMBINED] < 199)))
    status = 1;

  if (status)
    goto status_return;

  if (state[0] >= 4 && context_record_count < 50 && image_quality > 80 &&
      metrics[GOODIX_MILAN_POLICY_METRIC_COVERAGE] < 50)
    {
      status = 1;
      goto status_return;
    }

  /* This policy receives subtype 12 only; subtype-11 state is not produced. */
  if (!overlap_eligible_type12 (
        state, metrics, image_quality, image_coverage))
    return 0;

  {
    int32_t margin;
    int32_t area;

    margin = overlap_margin_type12 (
      state[0], state[1], metrics, context_record_count, image_quality);
    area = transformed_in_bounds_area_type12 (transform);
    status = overlap_status_type12 (area, margin);
    if (overlap_clears_match_flag_type12 (area, margin))
      *match_flag = 0;
  }

status_return:
  if (status)
    (*status_counter)++;
  return status;
}
