// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef IWX_RECEIVE_CLOCK_HPP
#define IWX_RECEIVE_CLOCK_HPP
#include <stdint.h>

// Each measurement pairs a fresh GP2 read with host uptime. No clock offset
// survives a firmware restart or a power-save interval.
struct IwxReceiveClock {
    enum { MaxSampleUS = 256, MaxAgeUS = 1000000 };
    uint64_t mapped, unavailable, rejected;
    uint32_t lastDelayUS, maxDelayUS;

    uint32_t delay(uint64_t processedUS, uint64_t beforeUS, uint64_t afterUS,
                   uint32_t currentGP2, uint32_t receiveGP2)
    {
        const uint32_t age = currentGP2 - receiveGP2;
        if (beforeUS < processedUS || afterUS < beforeUS ||
            afterUS - beforeUS > MaxSampleUS || age > MaxAgeUS) {
            ++rejected;
            return 0;
        }
        const uint64_t span = afterUS - beforeUS;
        const uint64_t middle = beforeUS + span / 2;
        if (age > middle) { ++rejected; return 0; }
        uint64_t receivedUS = middle - age;
        if (receivedUS > processedUS) {
            // The frame predates processing; only the sampling uncertainty may
            // put its estimate just beyond that bound.
            if (receivedUS - processedUS > (span + 1) / 2) {
                ++rejected;
                return 0;
            }
            receivedUS = processedUS;
        }
        lastDelayUS = uint32_t(processedUS - receivedUS);
        if (lastDelayUS > maxDelayUS) maxDelayUS = lastDelayUS;
        ++mapped;
        return lastDelayUS;
    }
};
#endif
