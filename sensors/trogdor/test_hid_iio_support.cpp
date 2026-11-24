/*
 * Copyright 2026 gbos-x86 contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hid_iio_support.h"
#include "iio_utils.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

using android::hardware::sensors::V2_1::subhal::implementation::iio_device_data;
using android::hardware::sensors::V2_1::subhal::implementation::iio_info_channel;
using android::hardware::sensors::V2_1::subhal::implementation::decode_scan_value;
using android::hardware::sensors::V2_1::subhal::implementation::enable_sensor;
using android::hardware::sensors::V2_1::subhal::implementation::convertIioTimestampToBoottime;
using android::hardware::sensors::V2_1::subhal::implementation::isHidMotionSensor;
using android::hardware::sensors::V2_1::subhal::implementation::isLatitude7320Dmi;
using android::hardware::sensors::V2_1::subhal::implementation::load_iio_devices;
using android::hardware::sensors::V2_1::subhal::implementation::latitude7320ProvisionalAxisMapping;
using android::hardware::sensors::V2_1::subhal::implementation::parse_scan_type;
using android::hardware::sensors::V2_1::subhal::implementation::scale_iio_value;
using android::hardware::sensors::V2_1::subhal::implementation::scan_elements;
using android::hardware::sensors::V2_1::subhal::implementation::set_sampling_frequency;
using android::hardware::sensors::V2_1::subhal::implementation::validHidAxisMapping;
using android::hardware::sensors::V2_1::subhal::implementation::valid_hid_scan_layout;
using android::hardware::sensors::V2_1::subhal::implementation::find_light_sensor;
using android::hardware::sensors::V2_1::subhal::implementation::hidSamplingFrequencyAttribute;
using android::hardware::sensors::V2_1::subhal::implementation::illuminanceChanged;
using android::hardware::sensors::V2_1::subhal::implementation::illuminanceLux;
using android::hardware::sensors::V2_1::subhal::implementation::kProvisionalGyroFullScaleRadPerSec;
using android::hardware::sensors::V2_1::subhal::implementation::light_sensor_data;
using android::hardware::sensors::V2_1::subhal::implementation::read_illuminance_lux;

int main() {
    assert(isHidMotionSensor("accel_3d"));
    assert(isHidMotionSensor("gyro_3d"));
    assert(!isHidMotionSensor("cros-ec-accel"));
    assert(!isHidMotionSensor("accel_3d_extra"));
    assert(isLatitude7320Dmi("Dell Inc.", "Latitude 7320 Detachable", "0TNW0H"));
    assert(!isLatitude7320Dmi("Dell Inc.", "Latitude 7320", "0TNW0H"));
    assert(!isLatitude7320Dmi("Dell Inc.", "Latitude 7320 Detachable", "OTHER"));
    const auto provisionalMapping = latitude7320ProvisionalAxisMapping();
    assert(provisionalMapping.xMap == 0 && provisionalMapping.yMap == 1 &&
           provisionalMapping.zMap == 2);
    assert(provisionalMapping.xNegate && provisionalMapping.yNegate &&
           provisionalMapping.zNegate);
    // Gyroscope: same axes as the accelerometer, no gravity sign convention to undo.
    const auto gyroMapping = latitude7320ProvisionalAxisMapping("gyro_3d");
    assert(gyroMapping.xMap == 0 && gyroMapping.yMap == 1 && gyroMapping.zMap == 2);
    assert(!gyroMapping.xNegate && !gyroMapping.yNegate && !gyroMapping.zNegate);
    assert(latitude7320ProvisionalAxisMapping("accel_3d").xNegate);
    assert(std::string(hidSamplingFrequencyAttribute("gyro_3d")) == "in_anglvel_sampling_frequency");
    assert(std::string(hidSamplingFrequencyAttribute("accel_3d")) == "in_accel_sampling_frequency");
    // Light: lux = (raw + offset) * scale, clamped at 0; reports past both 1 lux and 2%.
    assert(illuminanceLux(1500, 0.1, 0) == 150);
    assert(illuminanceLux(10, 1, -20) == 0);
    assert(!illuminanceChanged(150, 150.5));
    assert(!illuminanceChanged(1000, 1015));
    assert(illuminanceChanged(1000, 1025));
    assert(illuminanceChanged(10, 11.5));
    assert(illuminanceChanged(0, 2));
    assert(validHidAxisMapping(true, 2, 0, 1));
    assert(!validHidAxisMapping(false, 0, 1, 2));
    assert(!validHidAxisMapping(true, 0, 0, 2));
    assert(!validHidAxisMapping(true, -1, 1, 2));
    assert(!validHidAxisMapping(true, 0, 1, 3));

    int64_t boottimeTimestamp = 0;
    assert(convertIioTimestampToBoottime(1794556383383675000LL, 1794556383352298101LL,
                                         113530000000LL, &boottimeTimestamp));
    assert(boottimeTimestamp == 113561376899LL);
    assert(!convertIioTimestampToBoottime(1, 100, 50, &boottimeTimestamp));
    assert(!convertIioTimestampToBoottime(std::numeric_limits<int64_t>::max(), 0, 1,
                                          &boottimeTimestamp));
    assert(!convertIioTimestampToBoottime(1, 1, 1, nullptr));

    iio_info_channel channel{};
    double value = 0;
    assert(parse_scan_type("le:s24/32>>8\n", &channel) == 0);
    const uint8_t little_negative[] = {0, 0xfe, 0xff, 0xff};
    assert(decode_scan_value(channel, little_negative, sizeof(little_negative), &value) == 0);
    assert(value == -2);
    assert(decode_scan_value(channel, little_negative, 3, &value) != 0);
    assert(parse_scan_type("be:u12/16>>4", &channel) == 0);
    const uint8_t big_positive[] = {0xab, 0xc0};
    assert(decode_scan_value(channel, big_positive, sizeof(big_positive), &value) == 0);
    assert(value == 0xabc);
    assert(parse_scan_type("le:s32/32>>1", &channel) != 0);
    assert(parse_scan_type("le:s16/16X2>>0", &channel) != 0);
    assert(parse_scan_type("le:s0/32>>0", &channel) != 0);
    assert(parse_scan_type("le:s32/32>>0junk", &channel) != 0);
    assert(parse_scan_type("bogus", &channel) != 0);

    iio_device_data layout{};
    layout.name = "accel_3d";
    for (int i = 0; i < 4; ++i) {
        iio_info_channel c{};
        c.index = i;
        c.name = i == 3 ? "in_timestamp" : std::string("in_accel_") + "xyz"[i];
        assert(parse_scan_type(i == 3 ? "le:s64/64>>0" : "le:s24/32>>0", &c) == 0);
        layout.channelInfo.push_back(c);
    }
    assert(valid_hid_scan_layout(layout));
    layout.channelInfo[1].index = 0;
    assert(!valid_hid_scan_layout(layout));
    layout.channelInfo[1].index = 1;
    layout.channelInfo[3].storage_bytes = 4;
    assert(!valid_hid_scan_layout(layout));
    layout.channelInfo[3].storage_bytes = 8;
    layout.channelInfo[0].name = "in_accel_y";
    assert(!valid_hid_scan_layout(layout));

    char tmp[] = "/tmp/gbos-hid-iio-XXXXXX";
    const char* dir = mkdtemp(tmp);
    assert(dir);
    namespace fs = std::filesystem;
    const fs::path root(dir);
    auto device = [&](int number, const char* name, const char* scale, const char* offset) {
        const auto path = root / ("iio:device" + std::to_string(number));
        fs::create_directory(path);
        std::ofstream(path / "name") << name << '\n';
        if (scale) std::ofstream(path / "in_accel_scale") << scale << '\n';
        if (offset) std::ofstream(path / (std::string(name) == "accel_3d"
                                          ? "in_accel_offset" : "in_anglvel_offset")) << offset << '\n';
    };
    device(0, "accel_3d", "0.000009806", "10");
    device(1, "gyro_3d", "0", "0");
    device(2, "accel_3d", "nan", "0");
    device(3, "gyro_3d", nullptr, "0");
    device(4, "accel_3d", "1", nullptr);
    device(5, "accel_3d", "1", "nan");
    fs::create_directory(root / "iio:device6");
    device(7, "accel_3d", "0.000009806", "0");
    device(8, "accel_3d", "0.000009806", "0");
    std::ofstream(root / "iio:device0" / "in_accel_raw_available") << "-2147483648 1 2147483647\n";
    std::ofstream(root / "iio:device8" / "in_accel_raw_available") << "-1 0 1\n";
    std::vector<iio_device_data> found;
    assert(load_iio_devices(root.string(), &found, [](iio_device_data* sensor) {
        return isHidMotionSensor(sensor->name);
    }) == 0);
    assert(found.size() == 1);
    assert(found[0].iio_dev_num == 0);
    assert(found[0].scale > 0.0000098f && found[0].scale < 0.0000099f);
    assert(found[0].offset == 10);
    assert(scale_iio_value(found[0], -10) == 0);
    assert(scale_iio_value(found[0], 90) > 0.00098);
    fs::create_directory(root / "missing");
    fs::create_directory(root / "missing" / "iio:device9");
    std::vector<iio_device_data> missing;
    assert(load_iio_devices((root / "missing").string(), &missing,
                            [](iio_device_data*) { return true; }) == 0);
    assert(missing.empty());

    const auto profileRoot = root / "latitude7320";
    fs::create_directory(profileRoot);
    auto profileAccel = profileRoot / "iio:device0";
    fs::create_directory(profileAccel);
    std::ofstream(profileAccel / "name") << "accel_3d\n";
    std::ofstream(profileAccel / "in_accel_scale") << "0.000009806\n";
    std::ofstream(profileAccel / "in_accel_offset") << "0\n";
    std::ofstream(profileAccel / "in_accel_sampling_frequency") << "10\n";
    auto profileGyro = profileRoot / "iio:device1";
    fs::create_directory(profileGyro);
    std::ofstream(profileGyro / "name") << "gyro_3d\n";
    std::ofstream(profileGyro / "in_anglvel_scale") << "0.000000174\n";
    std::ofstream(profileGyro / "in_anglvel_offset") << "0\n";
    auto invalidRangeAccel = profileRoot / "iio:device2";
    fs::create_directory(invalidRangeAccel);
    std::ofstream(invalidRangeAccel / "name") << "accel_3d\n";
    std::ofstream(invalidRangeAccel / "in_accel_scale") << "0.000009806\n";
    std::ofstream(invalidRangeAccel / "in_accel_offset") << "0\n";
    std::ofstream(invalidRangeAccel / "in_accel_sampling_frequency") << "10\n";
    std::ofstream(invalidRangeAccel / "in_accel_raw_available") << "invalid\n";
    std::vector<iio_device_data> profiled;
    assert(load_iio_devices(profileRoot.string(), &profiled,
                            [](iio_device_data* sensor) {
                                return isHidMotionSensor(sensor->name);
                            }, true) == 0);
    assert(profiled.size() == 1);
    assert(profiled[0].name == "accel_3d");
    assert(profiled[0].provisional_latitude7320_profile);
    assert(profiled[0].resolution == 1.0f && profiled[0].max_range == 2000000);
    assert(profiled[0].sampling_freq_avl.size() == 1 &&
           profiled[0].sampling_freq_avl[0] == 10.0);
    std::vector<iio_device_data> unprofiled;
    assert(load_iio_devices(profileRoot.string(), &unprofiled,
                            [](iio_device_data* sensor) {
                                return isHidMotionSensor(sensor->name);
                            }) == 0);
    assert(unprofiled.empty());

    const auto scan = root / "iio:device0" / "scan_elements";
    fs::create_directory(scan);
    for (int i = 0; i < 4; ++i) {
        std::string name = i == 3 ? "in_timestamp" : std::string("in_accel_") + "xyz"[i];
        std::ofstream(scan / (name + "_en")) << "0\n";
        std::ofstream(scan / (name + "_index")) << i << '\n';
        std::ofstream(scan / (name + "_type"))
                << (i == 3 ? "le:s64/64>>0" : "le:s24/32>>0") << '\n';
    }
    assert(scan_elements((root / "iio:device0").string(), &found[0]) == 0);
    assert(valid_hid_scan_layout(found[0]));
    std::ofstream(scan / "in_accel_y_type") << "invalid\n";
    found[0].channelInfo.clear();
    assert(scan_elements((root / "iio:device0").string(), &found[0]) != 0);
    assert(set_sampling_frequency(root.string(), 25) < 0);
    fs::create_symlink("/dev/full", root / "in_accel_sampling_frequency");
    assert(set_sampling_frequency(root.string(), 25) < 0);
    fs::create_directory(root / "buffer");
    fs::create_symlink("/dev/full", root / "buffer" / "enable");
    assert(enable_sensor(root.string(), false) < 0);

    // A complete Latitude gyroscope gets the profile, its own frequency and a 2000 deg/s range.
    const auto gyroRoot = root / "latitude7320gyro";
    fs::create_directory(gyroRoot);
    auto gyro = gyroRoot / "iio:device3";
    fs::create_directory(gyro);
    std::ofstream(gyro / "name") << "gyro_3d\n";
    std::ofstream(gyro / "in_anglvel_scale") << "0.000000174\n";
    std::ofstream(gyro / "in_anglvel_offset") << "0\n";
    std::ofstream(gyro / "in_anglvel_sampling_frequency") << "10.000000\n";
    std::vector<iio_device_data> gyros;
    assert(load_iio_devices(gyroRoot.string(), &gyros,
                            [](iio_device_data* sensor) { return isHidMotionSensor(sensor->name); },
                            true) == 0);
    assert(gyros.size() == 1 && gyros[0].name == "gyro_3d");
    assert(gyros[0].provisional_latitude7320_profile);
    assert(gyros[0].sampling_freq_avl.size() == 1 && gyros[0].sampling_freq_avl[0] == 10.0);
    const double gyroFullScale = gyros[0].max_range * static_cast<double>(gyros[0].scale);
    assert(gyroFullScale > kProvisionalGyroFullScaleRadPerSec * 0.999 &&
           gyroFullScale < kProvisionalGyroFullScaleRadPerSec * 1.001);

    // Light sensor discovery: first "als" (numeric order) with a raw value and a positive scale.
    const auto lightRoot = root / "light";
    fs::create_directory(lightRoot);
    auto als = [&](int number, const char* name, const char* raw, const char* scale,
                   const char* offset) {
        const auto path = lightRoot / ("iio:device" + std::to_string(number));
        fs::create_directory(path);
        std::ofstream(path / "name") << name << '\n';
        if (raw) std::ofstream(path / "in_illuminance_raw") << raw << '\n';
        if (scale) std::ofstream(path / "in_illuminance_scale") << scale << '\n';
        if (offset) std::ofstream(path / "in_illuminance_offset") << offset << '\n';
    };
    als(0, "accel_3d", "5", "1", "0");
    als(1, "als", "5", "0", "0");         // scale 0: unusable
    als(2, "als", nullptr, "1", "0");     // no value
    als(10, "als", "999", "1", "0");
    als(3, "als", "1500", "0.1", nullptr); // offset optional
    light_sensor_data light;
    assert(find_light_sensor(lightRoot.string(), &light) == 0);
    assert(light.sysfspath == (lightRoot / "iio:device3").string());
    assert(light.scale > 0.0999 && light.scale < 0.1001 && light.offset == 0);
    double lux = 0;
    assert(read_illuminance_lux(light, &lux) == 0 && lux > 149.9 && lux < 150.1);
    std::ofstream(lightRoot / "iio:device3" / "in_illuminance_raw") << "30\n";
    assert(read_illuminance_lux(light, &lux) == 0 && lux > 2.99 && lux < 3.01);
    const auto noLight = root / "nolight";
    fs::create_directory(noLight);
    assert(find_light_sensor(noLight.string(), &light) == -ENOENT);

    fs::remove_all(root);
}
