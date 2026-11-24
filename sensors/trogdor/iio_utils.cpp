/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#define LOG_TAG "GoogleIIOSensorSubHal"

#include "iio_utils.h"
#include "hid_iio_support.h"
#include <errno.h>
#include <limits.h>
#include <log/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>

static const char* IIO_DEVICE_BASE = "iio:device";
static const char* IIO_SCAN_ELEMENTS_EN = "_en";
static const char* IIO_SFA_FILENAME = "sampling_frequency_available";
static const char* IIO_SCALE_FILENAME = "_scale";
static const char* IIO_SAMPLING_FREQUENCY = "_sampling_frequency";
static const char* IIO_BUFFER_ENABLE = "buffer/enable";
static const char* IIO_NAME_FILENAME = "name";
static const char* IIO_RANGE_AVAIL_FILENAME = "raw_available";

namespace android {
namespace hardware {
namespace sensors {
namespace V2_1 {
namespace subhal {
namespace implementation {

const char* DEFAULT_IIO_DIR = "/sys/bus/iio/devices/";

using DirPtr = std::unique_ptr<DIR, decltype(&closedir)>;
using FilePtr = std::unique_ptr<FILE, decltype(&fclose)>;

static bool str_has_prefix(const char* s, const char* prefix) {
    if (!s || !prefix) return false;

    const auto len_s = strlen(s);
    const auto len_prefix = strlen(prefix);
    if (len_s < len_prefix) return false;
    return std::equal(s, s + len_prefix, prefix);
}

static bool str_has_suffix(const char* s, const char* suffix) {
    if (!s || !suffix) return false;

    const auto len_s = strlen(s);
    const auto len_suffix = strlen(suffix);
    if (len_s < len_suffix) return false;
    return std::equal(s + len_s - len_suffix, s + len_s, suffix);
}

static int sysfs_opendir(const std::string& name, DirPtr* dp) {
    if (dp == nullptr) {
        return -EINVAL;
    }

    /*
     * Check if path exists, if a component of path does not exist,
     * or path is an empty string return ENOENT
     * If path is not accessible return EACCES
     */
    struct stat sb;
    if (stat(name.c_str(), &sb) == -1) {
        return -errno;
    }

    /* Open sysfs directory */
    DIR* tmp = opendir(name.c_str());
    if (tmp == nullptr) return -errno;

    dp->reset(tmp);

    return 0;
}

// TODO(egranata): could this (and _read_ below), infer the fmt string directly
// from the type of value being passed in? that would be a safer alternative
template <typename T>
static int sysfs_write_val(const std::string& f, const std::string& fmt, const T value) {
    FilePtr fp = {fopen(f.c_str(), "r+"), fclose};
    if (nullptr == fp) return -errno;

    if (fprintf(fp.get(), fmt.c_str(), value) < 0 || fflush(fp.get()) != 0)
        return errno ? -errno : -EIO;
    return 0;
}

static int sysfs_write_uint(const std::string& file, const unsigned int val) {
    return sysfs_write_val(file, "%u", val);
}

static int sysfs_write_double(const std::string& file, const double val) {
    return sysfs_write_val(file, "%f", val);
}

template <typename T>
static int sysfs_read_val(const std::string& f, const std::string& fmt, const T* value) {
    if (!value) return -EINVAL;

    FilePtr fp = {fopen(f.c_str(), "r"), fclose};
    if (nullptr == fp) return -errno;

    const int ret = fscanf(fp.get(), fmt.c_str(), value);
    return (ret == 1) ? 0 : -EINVAL;
}

static int sysfs_read_uint8(const std::string& file, uint8_t* val) {
    return sysfs_read_val(file, "%hhu\n", val);
}

static int sysfs_read_float(const std::string& file, float* val) {
    return sysfs_read_val(file, "%f\n", val);
}

static int sysfs_read_str(const std::string& file, std::string* str) {
    std::ifstream infile(file);
    if (!infile.is_open()) return -EINVAL;

    if (!std::getline(infile, *str))
        return -EINVAL;
    else
        return 0;
}

static int check_file(const std::string& filename) {
    struct stat info;
    return stat(filename.c_str(), &info);
}

int enable_sensor(const std::string& device_dir, const bool enable) {
    int err = check_file(device_dir);
    if (!err) {
        if (enable) {
            err = sysfs_write_uint(device_dir + "/buffer/length", 256);
            if (err) return err;
            err = sysfs_write_double(device_dir + "/buffer/hwfifo_timeout", 0.04);
            if (err && err != -ENOENT) return err;
        }
        std::string enable_file = device_dir;
        enable_file += "/";
        enable_file += IIO_BUFFER_ENABLE;
        err = sysfs_write_uint(enable_file, enable);
    }

    return err;
}

static int get_sampling_frequency_available(const std::string& device_dir,
                                            std::vector<double>* sfa) {
    int ret = 0;
    char* rest;
    std::string line;
    DirPtr dp(nullptr, closedir);
    const struct dirent* ent;

    ret = sysfs_opendir(device_dir, &dp);
    if (ret) return ret;
    while (ent = readdir(dp.get()), ent != nullptr) {
        if (str_has_suffix(ent->d_name, IIO_SFA_FILENAME)) {
            std::string filename = device_dir;
            filename += "/";
            filename += ent->d_name;
            ret = sysfs_read_str(filename, &line);
            if (ret < 0) return ret;
            char* pch = strtok_r(const_cast<char*>(line.c_str()), " ,", &rest);
            while (pch != nullptr) {
                sfa->push_back(atof(pch));
                pch = strtok_r(nullptr, " ,", &rest);
            }
        }
    }

    return ret < 0 ? ret : 0;
}

static int get_sensor_range(const std::string& device_dir, float* resolution, int64_t* max_range) {
    int ret = 0;
    bool found = false;
    char* rest;
    std::string line;
    DirPtr dp(nullptr, closedir);
    const struct dirent* ent;

    ret = sysfs_opendir(device_dir, &dp);
    if (ret) return ret;
    while (ent = readdir(dp.get()), ent != nullptr) {
        if (str_has_suffix(ent->d_name, IIO_RANGE_AVAIL_FILENAME)) {
            found = true;
            std::string filename = device_dir;
            filename += "/";
            filename += ent->d_name;

            ret = sysfs_read_str(filename, &line);
            if (ret < 0) return ret;
            char* pch = strtok_r(const_cast<char*>(line.c_str()), " ", &rest);
            std::vector<std::string> range_avail;
            while (pch != nullptr) {
                range_avail.push_back(pch);
                pch = strtok_r(nullptr, " ", &rest);
            }
            if (range_avail.size() < 3) return -EINVAL;
            *resolution = atof(range_avail[1].c_str());
            *max_range = atoll(range_avail[2].c_str());
        }
    }

    return ret < 0 ? ret : (found ? 0 : -ENOENT);
}

static int get_sensor_name(const std::string& device_dir, std::string* name) {
    const std::string filename = device_dir + "/" + IIO_NAME_FILENAME;

    return sysfs_read_str(filename, name);
}

bool isLatitude7320Dmi(const std::string& sys_vendor, const std::string& product_name,
                       const std::string& board_name) {
    return sys_vendor == "Dell Inc." && product_name == "Latitude 7320 Detachable" &&
           board_name == "0TNW0H";
}

bool isLatitude7320System() {
    std::string sys_vendor;
    std::string product_name;
    std::string board_name;
    if (sysfs_read_str("/sys/class/dmi/id/sys_vendor", &sys_vendor) ||
        sysfs_read_str("/sys/class/dmi/id/product_name", &product_name) ||
        sysfs_read_str("/sys/class/dmi/id/board_name", &board_name)) {
        return false;
    }
    return isLatitude7320Dmi(sys_vendor, product_name, board_name);
}

bool applyProvisionalLatitude7320Range(iio_device_data* sensor) {
    if (!sensor || !sensor->provisional_latitude7320_profile) return false;
    if (sensor->name == "accel_3d") {
        sensor->resolution = 1.0f;
        sensor->max_range = 2000000;
        return true;
    }
    if (sensor->name == "gyro_3d" && std::isfinite(sensor->scale) && sensor->scale > 0) {
        sensor->resolution = 1.0f;
        sensor->max_range = static_cast<int64_t>(kProvisionalGyroFullScaleRadPerSec / sensor->scale);
        return sensor->max_range > 0;
    }
    return false;
}

int read_illuminance_lux(const light_sensor_data& light, double* lux) {
    float raw = 0;
    if (!lux) return -EINVAL;
    int err = sysfs_read_float(light.sysfspath + "/in_illuminance_raw", &raw);
    if (err) return err;
    if (!std::isfinite(raw)) return -EINVAL;
    *lux = illuminanceLux(raw, light.scale, light.offset);
    return 0;
}

int find_light_sensor(const std::string& iio_dir, light_sensor_data* light) {
    DirPtr dp(nullptr, closedir);
    const struct dirent* ent;

    if (!light) return -EINVAL;
    int ret = sysfs_opendir(iio_dir, &dp);
    if (ret) return ret;
    std::vector<std::string> candidates;
    while (ent = readdir(dp.get()), ent != nullptr) {
        if (strncmp(ent->d_name, "iio:device", strlen("iio:device")) == 0)
            candidates.push_back(ent->d_name);
    }
    std::sort(candidates.begin(), candidates.end(), [](const std::string& a, const std::string& b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    for (const auto& name : candidates) {
        const std::string dir = iio_dir + (iio_dir.empty() || iio_dir.back() == '/' ? "" : "/") + name;
        std::string sensor_name;
        float scale = 0, offset = 0, raw = 0;
        if (get_sensor_name(dir, &sensor_name) || sensor_name != "als") continue;
        if (sysfs_read_float(dir + "/in_illuminance_raw", &raw) ||
            sysfs_read_float(dir + "/in_illuminance_scale", &scale) || !std::isfinite(scale) ||
            scale <= 0)
            continue;
        if (sysfs_read_float(dir + "/in_illuminance_offset", &offset) || !std::isfinite(offset))
            offset = 0;
        light->sysfspath = dir;
        light->scale = scale;
        light->offset = offset;
        return 0;
    }
    return -ENOENT;
}

int set_sampling_frequency(const std::string& device_dir, const double frequency) {
    DirPtr dp(nullptr, closedir);
    const struct dirent* ent;

    int ret = sysfs_opendir(device_dir, &dp);
    if (ret) return ret;
    bool found = false;
    while (ent = readdir(dp.get()), ent != nullptr) {
        if (str_has_suffix(ent->d_name, IIO_SAMPLING_FREQUENCY) ||
            strcmp(ent->d_name, "sampling_frequency") == 0) {
            std::string filename = device_dir;
            filename += "/";
            filename += ent->d_name;
            ret = sysfs_write_double(filename, frequency);
            if (ret) return ret;
            found = true;
        }
    }
    if (!found) return -ENOENT;
    if (frequency > 0) {
        double timeout = 1.0 / frequency;
        if (timeout > 0.05) timeout = 0.05;
        ret = sysfs_write_double(device_dir + "/buffer/hwfifo_timeout", timeout);
        if (ret && ret != -ENOENT) return ret;
    }
    return 0;
}

static int get_sensor_scale(const std::string& device_dir, float* scale) {
    DirPtr dp(nullptr, closedir);
    const struct dirent* ent;
    int err = -ENOENT;
    std::string filename;
    if (scale == nullptr) {
        return -EINVAL;
    }
    err = sysfs_opendir(device_dir, &dp);
    if (err) return err;
    while (ent = readdir(dp.get()), ent != nullptr) {
        if (str_has_suffix(ent->d_name, IIO_SCALE_FILENAME) ||
            strcmp(ent->d_name, "scale") == 0) {
            filename = device_dir;
            filename += "/";
            filename += ent->d_name;
            err = sysfs_read_float(filename, scale);
            if (err == 0 && *scale != 0.0f) {
                break;
            }
        }
    }
    return err;
}

int load_iio_devices(std::string iio_dir, std::vector<iio_device_data>* iio_data,
                     DeviceFilterFunction filter, bool latitude7320_profile) {
    DirPtr dp(nullptr, closedir);
    const struct dirent* ent;
    int err;

    if (!iio_dir.empty() && iio_dir.back() != '/') iio_dir += '/';

    std::ifstream iio_file;
    const auto iio_base_len = strlen(IIO_DEVICE_BASE);
    err = sysfs_opendir(iio_dir, &dp);
    if (err) return err;
    while (ent = readdir(dp.get()), ent != nullptr) {
        if (!str_has_prefix(ent->d_name, IIO_DEVICE_BASE)) continue;

        std::string path_device = iio_dir;
        path_device += ent->d_name;

        iio_device_data iio_dev_data{};
        iio_dev_data.sysfspath.append(path_device, 0, iio_dir.size() + strlen(ent->d_name));
        err = get_sensor_name(iio_dev_data.sysfspath, &iio_dev_data.name);
        if (err) {
            ALOGE("get_sensor_name for %s returned error %d", path_device.c_str(), err);
            continue;
        }

        if (!filter(&iio_dev_data)) continue;

        iio_dev_data.provisional_latitude7320_profile =
                latitude7320_profile && isHidMotionSensor(iio_dev_data.name);
        ALOGI("found sensor %s at path %s", iio_dev_data.name.c_str(), path_device.c_str());
        err = get_sampling_frequency_available(iio_dev_data.sysfspath,
                                               &iio_dev_data.sampling_freq_avl);
        if (err) {
            ALOGE("get_sampling_frequency_available for %s returned error %d", path_device.c_str(),
                  err);
            continue;
        }

        if (iio_dev_data.provisional_latitude7320_profile) {
            float current_frequency = 0.0f;
            err = sysfs_read_float(iio_dev_data.sysfspath + "/" +
                                           hidSamplingFrequencyAttribute(iio_dev_data.name),
                                   &current_frequency);
            if (err || !std::isfinite(current_frequency) || current_frequency <= 0.0f) {
                ALOGE("No valid current sampling frequency for provisional Latitude 7320 "
                      "sensor %s", path_device.c_str());
                continue;
            }
            iio_dev_data.sampling_freq_avl.assign(1, current_frequency);
        }
        std::sort(iio_dev_data.sampling_freq_avl.begin(), iio_dev_data.sampling_freq_avl.end());
        err = get_sensor_scale(iio_dev_data.sysfspath, &iio_dev_data.scale);
        if (err || !std::isfinite(iio_dev_data.scale) || iio_dev_data.scale <= 0.0f) {
            if (isHidMotionSensor(iio_dev_data.name)) {
                ALOGE("No valid IIO scale for HID sensor %s", path_device.c_str());
                err = 0;
                continue;
            }
            ALOGW("get_sensor_scale for %s returned %d (scale=%f), using default",
                  path_device.c_str(), err, iio_dev_data.scale);
            iio_dev_data.scale = 0.0011971f;
        }
        if (isHidMotionSensor(iio_dev_data.name)) {
            const std::string offset_path = iio_dev_data.sysfspath +
                    (iio_dev_data.name == "accel_3d" ? "/in_accel_offset" : "/in_anglvel_offset");
            err = sysfs_read_float(offset_path, &iio_dev_data.offset);
            if (err || !std::isfinite(iio_dev_data.offset)) {
                ALOGE("No valid IIO offset for HID sensor %s", path_device.c_str());
                err = 0;
                continue;
            }
        }
        err = get_sensor_range(iio_dev_data.sysfspath, &iio_dev_data.resolution,
                               &iio_dev_data.max_range);
        if (err || !std::isfinite(iio_dev_data.resolution) ||
            iio_dev_data.resolution <= 0.0f || iio_dev_data.max_range <= 0) {
            if (err == -ENOENT && applyProvisionalLatitude7320Range(&iio_dev_data)) {
                ALOGW("Using provisional Latitude 7320 %s range (%s); configured full-scale "
                      "is unverified", iio_dev_data.name.c_str(),
                      iio_dev_data.name == "gyro_3d" ? "2000 deg/s" : "2 g");
                err = 0;
            } else if (isHidMotionSensor(iio_dev_data.name)) {
                ALOGE("No valid IIO range for HID sensor %s", path_device.c_str());
                continue;
            } else {
                iio_dev_data.resolution = 1.0f;
                iio_dev_data.max_range = 32768;
            }
        }

        sscanf(ent->d_name + iio_base_len, "%hhu", &iio_dev_data.iio_dev_num);

        iio_data->push_back(iio_dev_data);
    }
    return 0;
}

int parse_scan_type(const std::string& type, iio_info_channel* channel) {
    if (!channel) return -EINVAL;
    char endian[3] = {};
    char sign = 0;
    unsigned int bits = 0, storage = 0, shift = 0;
    int consumed = 0;
    if (sscanf(type.c_str(), "%2[a-z]:%c%u/%u>>%u%n", endian, &sign, &bits,
               &storage, &shift, &consumed) != 5 || consumed == 0) return -EINVAL;
    for (size_t i = static_cast<size_t>(consumed); i < type.size(); ++i)
        if (!std::isspace(static_cast<unsigned char>(type[i]))) return -EINVAL;
    if ((strcmp(endian, "le") && strcmp(endian, "be")) ||
        (sign != 's' && sign != 'u') ||
        (storage != 8 && storage != 16 && storage != 32 && storage != 64) ||
        bits == 0 || bits > storage || shift > storage - bits) return -EINVAL;
    channel->bits_used = bits;
    channel->storage_bytes = storage / 8;
    channel->shift = shift;
    channel->big_endian = strcmp(endian, "be") == 0;
    channel->sign = sign == 's';
    return 0;
}

int decode_scan_value(const iio_info_channel& channel, const uint8_t* data,
                      size_t size, double* value) {
    const unsigned int storage = channel.storage_bytes * 8;
    if (!data || !value || size < channel.storage_bytes ||
        (storage != 8 && storage != 16 && storage != 32 && storage != 64) ||
        channel.bits_used == 0 || channel.bits_used > storage ||
        channel.shift > storage - channel.bits_used) return -EINVAL;

    uint64_t raw = 0;
    for (unsigned int i = 0; i < channel.storage_bytes; ++i) {
        const unsigned int byte = channel.big_endian ? i : channel.storage_bytes - 1 - i;
        raw = (raw << 8) | data[byte];
    }
    const uint64_t mask = channel.bits_used == 64 ? UINT64_MAX
                                                   : (uint64_t{1} << channel.bits_used) - 1;
    raw = (raw >> channel.shift) & mask;
    if (channel.sign && (raw & (uint64_t{1} << (channel.bits_used - 1)))) {
        *value = -static_cast<double>((~raw & mask) + 1);
    } else {
        *value = static_cast<double>(raw);
    }
    return 0;
}

bool valid_hid_scan_layout(const iio_device_data& data) {
    if (!isHidMotionSensor(data.name) || data.channelInfo.size() != 4) return false;
    const std::string prefix = data.name == "accel_3d" ? "in_accel_" : "in_anglvel_";
    const std::string names[] = {prefix + "x", prefix + "y", prefix + "z", "in_timestamp"};
    for (size_t i = 0; i < 4; ++i) {
        const auto& channel = data.channelInfo[i];
        if (channel.index != i || channel.name != names[i] || !channel.sign ||
            channel.big_endian || channel.shift != 0) return false;
        if (i < 3) {
            if (channel.storage_bytes != 4 || channel.bits_used == 0 || channel.bits_used > 32)
                return false;
        } else if (channel.storage_bytes != 8 || channel.bits_used != 64) {
            return false;
        }
    }
    return true;
}

double scale_iio_value(const iio_device_data& data, double raw) {
    return (raw + data.offset) * data.scale;
}

static int get_scan_type(const std::string& device_dir, struct iio_info_channel* chanInfo) {
    DirPtr dp(nullptr, closedir);
    const struct dirent* ent;
    std::string scan_dir;
    std::string filename;
    std::string type_name;

    if (chanInfo == nullptr) {
        return -EINVAL;
    }
    scan_dir = device_dir;
    scan_dir += "/scan_elements";
    const int err = sysfs_opendir(scan_dir, &dp);
    if (err) return err;
    type_name = chanInfo->name;
    type_name += "_type";
    while (ent = readdir(dp.get()), ent != nullptr) {
        if (strcmp(ent->d_name, type_name.c_str()) == 0) {
            filename = scan_dir;
            filename += "/";
            filename += ent->d_name;
            FilePtr fp = {fopen(filename.c_str(), "r"), fclose};
            if (fp == nullptr) continue;
            char type[128];
            if (!fgets(type, sizeof(type), fp.get())) return -EINVAL;
            return parse_scan_type(type, chanInfo);
        }
    }
    return -ENOENT;
}

int scan_elements(const std::string& device_dir, struct iio_device_data* iio_data) {
    DirPtr dp(nullptr, closedir);
    const struct dirent* ent;
    std::string scan_dir;
    std::string filename;
    uint8_t temp;
    int ret;

    if (iio_data == nullptr) {
        return -EINVAL;
    }
    scan_dir = device_dir;
    scan_dir += "/scan_elements";
    ret = sysfs_opendir(scan_dir, &dp);
    if (ret) return ret;
    while (ent = readdir(dp.get()), ent != nullptr) {
        if (str_has_suffix(ent->d_name, IIO_SCAN_ELEMENTS_EN)) {
            filename = scan_dir;
            filename += "/";
            filename += ent->d_name;
            ret = sysfs_write_uint(filename, ENABLE_CHANNEL);
            if (ret == 0) {
                ret = sysfs_read_uint8(filename, &temp);
                if ((ret == 0) && (temp == 1)) {
                    iio_info_channel chan_info{};
                    chan_info.name.assign(ent->d_name,
                                          strlen(ent->d_name) - strlen(IIO_SCAN_ELEMENTS_EN));
                    filename = scan_dir;
                    filename += "/";
                    filename += chan_info.name;
                    filename += "_index";
                    ret = sysfs_read_uint8(filename, &chan_info.index);
                    if (ret) {
                        ALOGE("Getting index for channel %s for sensor %s returned error %d",
                              chan_info.name.c_str(), device_dir.c_str(), ret);
                        return ret;
                    }
                    ret = get_scan_type(device_dir, &chan_info);
                    if (ret) {
                        ALOGE("Getting scan type for channel %s sensor %s returned error %d",
                              chan_info.name.c_str(), device_dir.c_str(), ret);
                        return ret;
                    }
                    iio_data->channelInfo.push_back(chan_info);
                } else {
                    ALOGE("Not able to successfully enable channel %s for sensor %s error %d",
                          ent->d_name, device_dir.c_str(), ret);
                    return ret;
                }
            } else {
                ALOGE("Enabling scan channel %s for sensor %s returned error %d", ent->d_name,
                      device_dir.c_str(), ret);
                return ret;
            }
        }
    }
    std::sort(iio_data->channelInfo.begin(), iio_data->channelInfo.end(),
              [](const iio_info_channel& a, const iio_info_channel& b) {
                  return a.index < b.index;
              });
    return ret;
}
}  // namespace implementation
}  // namespace subhal
}  // namespace V2_1
}  // namespace sensors
}  // namespace hardware
}  // namespace android
