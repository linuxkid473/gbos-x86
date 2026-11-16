/*
 * Copyright 2026 gbos-x86 contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

namespace android::hardware::sensors::V2_1::subhal::implementation {

struct ProvisionalAxisMapping {
    int64_t xMap;
    int64_t yMap;
    int64_t zMap;
    bool xNegate;
    bool yNegate;
    bool zNegate;
};

inline bool isHidMotionSensor(std::string_view name) {
    return name == "accel_3d" || name == "gyro_3d";
}

inline ProvisionalAxisMapping latitude7320ProvisionalAxisMapping() {
    return {0, 1, 2, true, true, true};
}

inline bool validHidAxisMapping(bool rotate, int64_t x, int64_t y, int64_t z) {
    return rotate && x >= 0 && x < 3 && y >= 0 && y < 3 && z >= 0 && z < 3 &&
           x != y && y != z && x != z;
}

inline bool convertIioTimestampToBoottime(int64_t timestampNs, int64_t realtimeNs,
                                          int64_t boottimeNs, int64_t* convertedNs) {
    if (!convertedNs || timestampNs < 0 || realtimeNs < 0 || boottimeNs < 0) return false;

    if (boottimeNs >= realtimeNs) {
        const int64_t offset = boottimeNs - realtimeNs;
        if (timestampNs > std::numeric_limits<int64_t>::max() - offset) return false;
        *convertedNs = timestampNs + offset;
    } else {
        const int64_t offset = realtimeNs - boottimeNs;
        if (timestampNs < offset) return false;
        *convertedNs = timestampNs - offset;
    }
    return true;
}

}  // namespace android::hardware::sensors::V2_1::subhal::implementation
