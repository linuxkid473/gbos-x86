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

#include "MultiPoll.h"

namespace android::hardware::sensors::V2_1::subhal::implementation {

MultiPoll::MultiPoll(uint64_t periodMs) : mSamplingPeriodMs(periodMs) {}

void MultiPoll::addDescriptor(int fd) {
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    std::unique_lock<std::mutex> lck(mDescriptorsMutex);
    mDescriptors.push_back(pfd);
}

int MultiPoll::poll(OnPollIn in) {
    std::vector<pollfd> fds;
    {
        // make a copy so you don't need to lock for prolonged periods of time
        std::unique_lock<std::mutex> lck(mDescriptorsMutex);
        fds.assign(mDescriptors.begin(), mDescriptors.end());
    }

    int err = ::poll(&fds[0], fds.size(), mSamplingPeriodMs);
    if (err < 0) return err;

    for (const auto& fd : fds) {
        if (fd.revents & POLLIN) {
            in(fd.fd);
        }
    }

    return 0;
}

}  // namespace android::hardware::sensors::V2_1::subhal::implementation
