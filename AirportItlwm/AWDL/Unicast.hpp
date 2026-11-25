// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_UNICAST_HPP
#define AIRPORT_AWDL_UNICAST_HPP
#include "ActionFrame.hpp"

namespace AirportAWDL {
static inline bool oobPayload(const uint8_t *bytes, size_t size, bool unicast = false)
{
    if (!bytes || !size || size > 1200) return false;
    bool election = false, version = false;
    for (size_t at = 0; at < size;) {
        if (size - at < 3 || le16(bytes + at + 1) > size - at - 3) return false;
        const unsigned length = le16(bytes + at + 1);
        switch (bytes[at]) {
        case 4: case 5:
            return false; // Filled by the driver for OOB flags 0x6/0xe.
        case 24:
            if (election || length != 40) return false;
            election = true;
            break;
        case 21:
            if (version || length != 2) return false;
            version = true;
            break;
        }
        at += length + 3;
    }
    // Native UMI templates omit Version21; broadcast templates include it.
    return election && (unicast || version);
}

struct UnicastRequest {
    enum State { Free, Queued, Submitted, Complete } state;
    uint64_t token, queuedUS, deadlineUS;
    uint16_t windowCounter, channel;
    uint8_t destination[6], status;
    uint16_t size;
    uint8_t payload[1200];
};

struct BroadcastTemplate {
    bool configured, sent;
    uint32_t channelMap, offsetUS;
    uint16_t lastWindowCounter;
    uint16_t size;
    uint8_t payload[1200];
};

struct BroadcastRequest {
    uint64_t queuedUS;
    uint16_t size;
    uint8_t payload[1200];
};
}
#endif
