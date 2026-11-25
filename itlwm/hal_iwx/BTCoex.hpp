// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// Channel inhibition masks: Linux iwlwifi mvm/coex.c.
// Copyright (C) 2013-2014, 2018-2020 Intel Corporation
// Copyright (C) 2013-2015 Intel Mobile Communications GmbH
#ifndef IWX_BT_COEX_HPP
#define IWX_BT_COEX_HPP
#include <stdint.h>
#include "BTCoexPolicy.hpp"

struct IwxBTCoexState {
    static constexpr uint32_t HighTraffic = 3; // BT_COEX_BT_ACTIVITY_GRADING_API_E_VER_1
    bool enabled, configuring, ciValid;
    bool mimoDisabled, txSisoDiversity;
    uint64_t generation, notifications, configNotifications, malformed, ciUpdates, ciCommandSerial;
    uint32_t modules, activity, compliance, primaryLUT, secondaryLUT;
    uint8_t ttc, rrc, ci[24];
    uint8_t rxMask, txMask, nvmRxMask, nvmTxMask, configVersion, ciVersion, rlcVersion;
    IwxBTPolicy::State policy;
    int beaconDBm, smpsError, phyError, powerError;
    int lastError;
    void clearProfile() {
        activity = compliance = primaryLUT = secondaryLUT = 0;
        ttc = rrc = 0;
    }
    void reset() {
        ++generation;
        policy.resetLink(false, false, 1);
        beaconDBm = smpsError = phyError = powerError = 0;
        enabled = configuring = ciValid = false;
        mimoDisabled = txSisoDiversity = false;
        rxMask = txMask = nvmRxMask = nvmTxMask = configVersion = ciVersion = rlcVersion = 0;
        modules = 0;
        clearProfile();
        lastError = 0;
    }
};

// Columns are 20 MHz, HT40 with secondary below, HT40 with secondary above.
static inline uint64_t iwxBTChannelInhibition(unsigned channel, unsigned column)
{
    static const uint64_t masks[15][3] = {
        {0, 0, 0},
        {0x0000001fffULL, 0, 0x00007fffffULL},
        {0x000000ffffULL, 0, 0x0003ffffffULL},
        {0x000003fffcULL, 0, 0x000ffffffcULL},
        {0x00001fffe0ULL, 0, 0x007fffffe0ULL},
        {0x00007fff80ULL, 0x00007fffffULL, 0x01ffffff80ULL},
        {0x0003fffc00ULL, 0x0003ffffffULL, 0x0ffffffc00ULL},
        {0x000ffff000ULL, 0x000ffffffcULL, 0x3ffffff000ULL},
        {0x007fff8000ULL, 0x007fffffe0ULL, 0xffffff8000ULL},
        {0x01fffe0000ULL, 0x01ffffff80ULL, 0xfffffe0000ULL},
        {0x0ffff00000ULL, 0x0ffffffc00ULL, 0},
        {0x3fffc00000ULL, 0x3ffffff000ULL, 0},
        {0xfffe000000ULL, 0xffffff8000ULL, 0},
        {0xfff8000000ULL, 0xfffffe0000ULL, 0},
        {0xfe00000000ULL, 0, 0}
    };
    return channel < 15 && column < 3 ? masks[channel][column] : 0;
}
#endif
