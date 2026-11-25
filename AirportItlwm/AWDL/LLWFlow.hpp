// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_LLW_FLOW_HPP
#define AIRPORT_AWDL_LLW_FLOW_HPP

#include <stddef.h>
#include <stdint.h>
#include "PeerState.hpp"

namespace AirportAWDL {

enum class LLWRoute : uint8_t {
    AWDL = 0,
    LLW  = 1,
    Both = 2
};

struct LLWFlowEntry {
    bool occupied;
    uint8_t proto;
    uint16_t localPort;
    uint8_t peer[6];
    uint64_t lastSeenUS;
};

struct LLWFlowRouter {
    static constexpr unsigned TableCapacity = 16;
    static constexpr uint64_t FlowLifetimeUS = 30000000ULL;

    LLWFlowEntry llwFlows[TableCapacity];
    LLWFlowEntry awdlFlows[TableCapacity];

    static inline uint16_t readBE16(const uint8_t *p)
    {
        return uint16_t((uint16_t(p[0]) << 8) | p[1]);
    }

    static inline constexpr bool isReservedAWDLPort(uint16_t port)
    {
        return port == 5353 || port == 319 || port == 320;
    }

    static inline void removeEntry(LLWFlowEntry (&table)[TableCapacity],
                                   const uint8_t peer[6], uint16_t localPort, uint8_t proto)
    {
        for (auto &entry : table) {
            if (entry.occupied && entry.proto == proto && entry.localPort == localPort &&
                sameAddress(entry.peer, peer)) {
                entry = {};
            }
        }
    }

    static inline void upsertEntry(LLWFlowEntry (&table)[TableCapacity],
                                   const uint8_t peer[6], uint16_t localPort,
                                   uint8_t proto, uint64_t nowUS)
    {
        LLWFlowEntry *slot = nullptr;
        for (auto &entry : table) {
            if (entry.occupied && entry.proto == proto && entry.localPort == localPort &&
                sameAddress(entry.peer, peer)) {
                entry.lastSeenUS = nowUS;
                return;
            }
            if (!entry.occupied) {
                if (!slot || slot->occupied) slot = &entry;
            } else if (!slot || (slot->occupied && entry.lastSeenUS < slot->lastSeenUS)) {
                slot = &entry;
            }
        }
        if (slot) {
            slot->occupied = true;
            slot->proto = proto;
            slot->localPort = localPort;
            copyAddress(slot->peer, peer);
            slot->lastSeenUS = nowUS;
        }
    }

    static inline bool hasLiveEntry(const LLWFlowEntry (&table)[TableCapacity],
                                    const uint8_t peer[6], uint16_t localPort,
                                    uint8_t proto, uint64_t nowUS)
    {
        for (const auto &entry : table) {
            if (!entry.occupied || entry.proto != proto || entry.localPort != localPort)
                continue;
            if (!sameAddress(entry.peer, peer))
                continue;
            if (nowUS >= entry.lastSeenUS && nowUS - entry.lastSeenUS <= FlowLifetimeUS)
                return true;
        }
        return false;
    }

    void clear()
    {
        for (auto &entry : llwFlows) entry = {};
        for (auto &entry : awdlFlows) entry = {};
    }

    void noteTransmit(const uint8_t peerMAC[6], const uint8_t *ipv6, size_t ipv6Len,
                      bool fromLLW, uint64_t nowUS)
    {
        if (!peerMAC || !ipv6 || (peerMAC[0] & 1) || ipv6Len < 48 || (ipv6[0] >> 4) != 6)
            return;
        const uint8_t proto = ipv6[6];
        if (proto != 17 && proto != 6)
            return;
        const uint16_t localPort = readBE16(ipv6 + 40);
        const uint16_t remotePort = readBE16(ipv6 + 42);
        if (!localPort || isReservedAWDLPort(localPort) || isReservedAWDLPort(remotePort))
            return;
        if (fromLLW) {
            removeEntry(awdlFlows, peerMAC, localPort, proto);
            upsertEntry(llwFlows, peerMAC, localPort, proto, nowUS);
        } else {
            removeEntry(llwFlows, peerMAC, localPort, proto);
            upsertEntry(awdlFlows, peerMAC, localPort, proto, nowUS);
        }
    }

    LLWRoute classifyReceive(const uint8_t peerMAC[6], const uint8_t *ipv6, size_t ipv6Len,
                             uint8_t tid, bool llwActive, bool llwParamsEnabled,
                             const uint8_t *llwPeerMAC, uint64_t nowUS) const
    {
        if (!llwActive || !peerMAC || !ipv6 || ipv6Len < 40 || (ipv6[0] >> 4) != 6)
            return LLWRoute::AWDL;
        const uint8_t proto = ipv6[6];
        // Mirror ICMPv6 (Neighbor Discovery NS/NA) so llw0 resolves peer MACs over AWDL.
        if (proto == 58)
            return LLWRoute::Both;
        if ((proto == 17 || proto == 6) && ipv6Len >= 48) {
            const uint16_t remotePort = readBE16(ipv6 + 40);
            const uint16_t localPort = readBE16(ipv6 + 42);
            if (isReservedAWDLPort(localPort) || isReservedAWDLPort(remotePort))
                return LLWRoute::AWDL;
            if (hasLiveEntry(llwFlows, peerMAC, localPort, proto, nowUS))
                return LLWRoute::LLW;
            if (hasLiveEntry(awdlFlows, peerMAC, localPort, proto, nowUS))
                return LLWRoute::AWDL;
            if (proto == 17 && (ipv6[8] == 0xfd || ipv6[24] == 0xfd))
                return LLWRoute::LLW;
            if (llwParamsEnabled && llwPeerMAC && sameAddress(llwPeerMAC, peerMAC) &&
                proto == 17 && tid >= 4)
                return LLWRoute::LLW;
        }
        return LLWRoute::AWDL;
    }
};

} // namespace AirportAWDL
#endif
