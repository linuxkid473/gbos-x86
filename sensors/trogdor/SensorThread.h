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

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace android::hardware::sensors::V2_1::subhal::implementation {

class SensorBase;

class SensorThread {
  public:
    explicit SensorThread(SensorBase*);
    ~SensorThread();

    void notifyAll();
    void start();
    void stop();
    std::unique_lock<std::mutex> lock();
    void join();

    bool isStopped() const;

    template <typename Predicate>
    void wait(Predicate p) {
        auto lck(lock());
        mWaitCV.wait(lck, p);
    }

  private:
    SensorBase* mSensor;
    std::atomic_bool mStopThread;
    std::condition_variable mWaitCV;
    std::mutex mRunMutex;
    std::thread mThread;
};

}  // namespace android::hardware::sensors::V2_1::subhal::implementation
