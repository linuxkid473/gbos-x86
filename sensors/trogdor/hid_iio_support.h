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

/*
 * Latitude 7320 axis mappings. The accelerometer's was derived from Windows pose captures and
 * confirmed by auto-rotation: HID (like Windows) reports gravity with the opposite sign to
 * Android (-1 g on the axis facing up) on the same axes, so all three are negated. Angular
 * velocity has no such sign convention difference, so the gyroscope keeps the axes unchanged.
 */
inline ProvisionalAxisMapping latitude7320ProvisionalAxisMapping(std::string_view name = "accel_3d") {
    if (name == "gyro_3d") return {0, 1, 2, false, false, false};
    return {0, 1, 2, true, true, true};
}

inline const char* hidSamplingFrequencyAttribute(std::string_view name) {
    return name == "gyro_3d" ? "in_anglvel_sampling_frequency" : "in_accel_sampling_frequency";
}

/* Provisional gyroscope full scale when the device exposes none: +-2000 deg/s in rad/s. */
constexpr double kProvisionalGyroFullScaleRadPerSec = 34.906585;

/* Ambient light: lux from the IIO raw value, never negative. */
inline double illuminanceLux(double raw, double scale, double offset) {
    const double lux = (raw + offset) * scale;
    return lux > 0 ? lux : 0;
}

/*
 * Report a new light value when it moved by more than 1 lux and more than 2% (the larger
 * threshold wins): no event per 1-lux flicker in bright light, none for noise near dark.
 */
inline bool illuminanceChanged(double last, double now) {
    const double delta = now > last ? now - last : last - now;
    const double relative = (last > now ? last : now) * 0.02;
    return delta > 1.0 && delta > relative;
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
