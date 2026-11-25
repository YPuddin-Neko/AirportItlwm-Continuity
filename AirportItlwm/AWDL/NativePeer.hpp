// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_NATIVE_PEER_HPP
#define AIRPORT_AWDL_NATIVE_PEER_HPP
#include <stddef.h>
#include <stdint.h>

// 24H32 updateLocalCounters reads these delta slots through offset 0x100.
// This adapter is only for that callee, not updateInterfaceDataStats.
struct AirportAWDLNativeDataCountersDelta {
    uint64_t reservedHeader;
    uint64_t txPackets, txBytes, rxPackets, rxBytes;
    uint64_t reserved[28];
};
static_assert(sizeof(AirportAWDLNativeDataCountersDelta) == 0x108, "native counter read extent");
static_assert(alignof(AirportAWDLNativeDataCountersDelta) == 8, "native counter alignment");
static_assert(offsetof(AirportAWDLNativeDataCountersDelta, txPackets) == 0x08, "native TX packet delta");
static_assert(offsetof(AirportAWDLNativeDataCountersDelta, txBytes) == 0x10, "native TX byte delta");
static_assert(offsetof(AirportAWDLNativeDataCountersDelta, rxPackets) == 0x18, "native RX packet delta");
static_assert(offsetof(AirportAWDLNativeDataCountersDelta, rxBytes) == 0x20, "native RX byte delta");

class OSObject;
extern "C" OSObject *AirportAWDLGetNativePeerManager(void *)
    __asm__("__ZN23IO80211SkywalkInterface14getPeerManagerEv");
extern "C" bool AirportAWDLLockNativePeers(void *)
    __asm__("__ZN18IO80211PeerManager12lockDataPathEv");
extern "C" void AirportAWDLUnlockNativePeers(void *)
    __asm__("__ZN18IO80211PeerManager14unlockDataPathEv");
extern "C" OSObject *AirportAWDLFindNativePeer(void *, uint8_t *)
    __asm__("__ZN18IO80211PeerManager8findPeerEPh");
extern "C" void AirportAWDLNativePeerReceivedData(void *)
    __asm__("__ZN15IO80211AWDLPeer24peerDiscoveredOnBothSideEv");
extern "C" int AirportAWDLNativePeerReachability(void *)
    __asm__("__ZN15IO80211AWDLPeer19getPeerReachabilityEv");
extern "C" void AirportAWDLIncrementNativeReachability(void *, int)
    __asm__("__ZN22IO80211AWDLPeerManager23incNumReachabilityArrayEi");
extern "C" void AirportAWDLDecrementNativeReachability(void *, int)
    __asm__("__ZN22IO80211AWDLPeerManager23decNumReachabilityArrayEi");
extern "C" void AirportAWDLSetNativeRxActivity(void *, uint64_t)
    __asm__("__ZN18IO80211PeerManager28setLastUnicastRxLinkActivityEy");
extern "C" void AirportAWDLSetNativeTxActivity(void *, uint64_t)
    __asm__("__ZN18IO80211PeerManager28setLastUnicastTxLinkActivityEy");
extern "C" void AirportAWDLUpdateNativeDataCounters(void *, const AirportAWDLNativeDataCountersDelta *)
    __asm__("__ZN18IO80211PeerManager19updateLocalCountersEP36apple80211_data_path_interface_stats");
extern "C" void AirportAWDLNativeSyncStateChanged(void *)
    __asm__("__ZN22IO80211AWDLPeerManager20postSyncStateChangedEv");
#endif
