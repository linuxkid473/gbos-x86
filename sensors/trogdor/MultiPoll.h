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

#include <poll.h>
#include <functional>
#include <mutex>
#include <vector>

namespace android::hardware::sensors::V2_1::subhal::implementation {

class MultiPoll {
  public:
    explicit MultiPoll(uint64_t periodMs = 0);
    ~MultiPoll() = default;

    // TODO(egranata): add support for events other than POLLIN
    void addDescriptor(int fd);
    using OnPollIn = std::function<void(int fd)>;
    int poll(OnPollIn in);

  private:
    uint64_t mSamplingPeriodMs;
    std::mutex mDescriptorsMutex;
    std::vector<pollfd> mDescriptors;
};

}  // namespace android::hardware::sensors::V2_1::subhal::implementation
