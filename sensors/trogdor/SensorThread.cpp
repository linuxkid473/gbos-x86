/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "SensorThread.h"
#include "Sensor.h"

namespace android::hardware::sensors::V2_1::subhal::implementation {

SensorThread::SensorThread(SensorBase* sensor)
    : mSensor(sensor), mStopThread(false), mWaitCV(), mRunMutex() {}

void SensorThread::start() {
    mThread = std::thread([this]() -> void {
        if (mSensor) {
            while (!mStopThread) {
                mSensor->pollSensor();
            }
        }
    });
}

SensorThread::~SensorThread() {
    {
        std::unique_lock<std::mutex> lck(lock());
        stop();
        notifyAll();
    }
    join();
}

bool SensorThread::isStopped() const {
    return mStopThread;
}

void SensorThread::join() {
    if (mThread.joinable()) mThread.join();
}

void SensorThread::notifyAll() {
    mWaitCV.notify_all();
}

void SensorThread::stop() {
    mStopThread = true;
}

std::unique_lock<std::mutex> SensorThread::lock() {
    return std::unique_lock<std::mutex>(mRunMutex);
}

}  // namespace android::hardware::sensors::V2_1::subhal::implementation
