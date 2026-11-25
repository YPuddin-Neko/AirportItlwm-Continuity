// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_NATIVE_PEER_SNAPSHOT_HPP
#define AIRPORT_AWDL_NATIVE_PEER_SNAPSHOT_HPP
#if DEBUG
#include <stddef.h>
#include <stdint.h>
#include "NativePeer.hpp"

extern "C" int AirportAWDLPrintNativePeers(void *, void *)
    __asm__("__ZN22IO80211AWDLPeerManager14printAwdlPeersEP19apple80211_awdl_ctl");

namespace AirportAWDL {
// 24H32 printAwdlPeers reads the fixed ctl prefix and writes its inline text.
struct NativePeerSnapshotControl {
    uint32_t version;
    uint8_t reserved[0x8c];
    uint32_t capacity;
    char text[0x3b800];
    uint32_t used;
    uint16_t color;
    uint8_t tail[6];
};
static_assert(offsetof(NativePeerSnapshotControl, capacity) == 0x90, "native ctl capacity");
static_assert(offsetof(NativePeerSnapshotControl, text) == 0x94, "native ctl text");
static_assert(offsetof(NativePeerSnapshotControl, used) == 0x3b894, "native ctl used");
static_assert(offsetof(NativePeerSnapshotControl, color) == 0x3b898, "native ctl color");
static_assert(sizeof(NativePeerSnapshotControl) == 0x3b8a0, "native ctl size");
constexpr uint32_t NativePeerSnapshotCapacity = 8192;
}
#endif
#endif
