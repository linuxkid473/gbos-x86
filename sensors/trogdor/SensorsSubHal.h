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
#ifndef ANDROID_HARDWARE_SENSORS_V2_1_SENSORS_SUBHAL_H
#define ANDROID_HARDWARE_SENSORS_V2_1_SENSORS_SUBHAL_H

#include <vector>
#include "Sensor.h"
#include "SubHal.h"

using ::android::hardware::sensors::V2_1::SensorType;

namespace android {
namespace hardware {
namespace sensors {
namespace V2_1 {
namespace subhal {
namespace implementation {

using ::android::hardware::sensors::V1_0::OperationMode;
using ::android::hardware::sensors::V1_0::Result;
using ::android::hardware::sensors::V2_1::implementation::IHalProxyCallback;
using ::android::hardware::sensors::V2_1::implementation::ISensorsSubHal;
using ::android::hardware::sensors::V2_1::subhal::implementation::ISensorsEventCallback;
using ::sensor::hal::configuration::V1_0::Configuration;

/**
 * Implementation of a ISensorsSubHal that can be used as a reference HAL implementation of sensors
 * multihal 2.0. See the README file for more details.
 */
class SensorsSubHal : public ISensorsSubHal, public ISensorsEventCallback {
    using Event = ::android::hardware::sensors::V2_1::Event;
    using RateLevel = ::android::hardware::sensors::V1_0::RateLevel;
    using SharedMemInfo = ::android::hardware::sensors::V1_0::SharedMemInfo;
    using ScopedWakelock = ::android::hardware::sensors::V2_0::implementation::ScopedWakelock;

  public:
    SensorsSubHal();

    // Methods from ::android::hardware::sensors::V2_1::ISensors follow.
    Return<void> getSensorsList_2_1(getSensorsList_2_1_cb _hidl_cb) override;

    Return<Result> setOperationMode(OperationMode mode) override;

    OperationMode getOperationMode() const { return mCurrentOperationMode; }

    Return<Result> activate(int32_t sensorHandle, bool enabled) override;

    Return<Result> batch(int32_t sensorHandle, int64_t samplingPeriodNs,
                         int64_t maxReportLatencyNs) override;

    Return<Result> flush(int32_t sensorHandle) override;

    Return<Result> injectSensorData_2_1(const Event& event) override;

    Return<void> registerDirectChannel(const SharedMemInfo& mem,
                                       registerDirectChannel_cb _hidl_cb) override;

    Return<Result> unregisterDirectChannel(int32_t channelHandle) override;

    Return<void> configDirectReport(int32_t sensorHandle, int32_t channelHandle, RateLevel rate,
                                    configDirectReport_cb _hidl_cb) override;

    Return<void> debug(const hidl_handle& fd, const hidl_vec<hidl_string>& args) override;

    // Methods from ::android::hardware::sensors::V2_0::implementation::ISensorsSubHal follow.
    const std::string getName() override { return "Google-IIO-SensorsSubhal"; }

    Return<Result> initialize(const sp<IHalProxyCallback>& halProxyCallback) override;

    // Method from ISensorsEventCallback.
    void postEvents(const std::vector<Event>& events, ScopedWakelock wakelock) override;

    ScopedWakelock createScopedWakelock(bool lock) override;

  protected:
    void AddSensor(const struct iio_device_data& iio_data,
                   const std::optional<std::vector<Configuration>>& config);

    /**
     * A map of the available sensors
     */
    std::map<int32_t, std::unique_ptr<SensorBase>> mSensors;

    /**
     * Callback used to communicate to the HalProxy when dynamic sensors are connected /
     * disconnected, sensor events need to be sent to the framework, and when a wakelock should be
     * acquired.
     */
    sp<IHalProxyCallback> mCallback;

  private:
    /**
     * The current operation mode of the multihal framework. Ensures that all subhals are set to
     * the same operation mode.
     */
    OperationMode mCurrentOperationMode = OperationMode::NORMAL;

    /**
     * The next available sensor handle
     */
    int32_t mNextHandle;
};

}  // namespace implementation
}  // namespace subhal
}  // namespace V2_1
}  // namespace sensors
}  // namespace hardware
}  // namespace android
#endif
