// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_PEER_STATE_HPP
#define AIRPORT_AWDL_PEER_STATE_HPP

#include "ActionFrame.hpp"
#include "RadioTraits.hpp"
#include "TxRate.hpp"

namespace AirportAWDL {

static inline bool sameAddress(const uint8_t *a, const uint8_t *b)
{
    for (unsigned i = 0; i < 6; ++i)
        if (a[i] != b[i]) return false;
    return true;
}

static inline bool unicastAddress(const uint8_t *a)
{
    uint8_t bits = 0;
    for (unsigned i = 0; i < 6; ++i) bits |= a[i];
    return bits && !(a[0] & 1);
}

struct Window {
    uint16_t counter, runStartEAW;
    uint64_t untilNextAWUS, untilNextEAWUS;
    uint64_t sinceChannelStartUS, untilChannelChangeUS;
    uint8_t channel, channelFlags, encoding;
    uint8_t widthMHz() const {
        return encoding == 3 && (channelFlags == 128 || channelFlags == 130) ? 80 : 20;
    }
};

// Native AWDL separates four sequence classes and unicast/multicast histories.
struct DataSequenceState {
    uint8_t initialized;
    uint16_t newest[8];

    bool accept(uint16_t word, bool multicast)
    {
        const unsigned lane = (multicast ? 4u : 0u) | ((word >> 12) & 3u);
        const uint8_t mask = uint8_t(1u << lane);
        const uint16_t sequence = word & 0x0fff;
        if ((initialized & mask) && newest[lane] == sequence) return false;
        // A changed 12-bit sequence may wrap or restart while the peer stays present.
        initialized |= mask;
        newest[lane] = sequence;
        return true;
    }
};

// Each peer retains its own clock estimate; observing a peer does not elect it.
struct Peer {
    static constexpr uint64_t ScheduleFreshnessUS = 3000000;
    static constexpr uint64_t HTCapabilityLifetimeUS = 10000000;
    bool occupied, receivedMIF, havePhyTime, haveSync, haveSequence, nativeCached, haveActionSequence;
    uint8_t address[6];
    uint64_t lastSeenUS, lastReceiveUS, syncReceivedUS, sequenceReceivedUS, electionV2ReceivedUS;
    uint64_t lastUnicastDataUS;
    uint64_t htCapabilitiesReceivedUS;
    uint32_t phyTime;
    uint16_t actionSequence;
    uint16_t lastLLWSeq;
    bool hasLLWSeq;
    uint16_t rxChannel;
    int16_t rssi;
    Advertisement metadata, sync;
    ChannelSequence sequence;
    DataSequenceState dataSequence;
    TxRateControl txRate;

    uint8_t txHTMcs(uint64_t now) const {
        if (!occupied || !(metadata.fields & HTCapabilities) || now < htCapabilitiesReceivedUS)
            return 0;
        const bool activeSession = nativeCached ||
            (lastUnicastDataUS && now >= lastUnicastDataUS &&
             now - lastUnicastDataUS < HTCapabilityLifetimeUS);
        const uint64_t freshnessUS = activeSession ? HTCapabilityLifetimeUS : ScheduleFreshnessUS;
        return (!activeSession || (now >= lastSeenUS && now - lastSeenUS < HTCapabilityLifetimeUS)) &&
            now - htCapabilitiesReceivedUS < freshnessUS ? metadata.rxHTMcs : 0;
    }
    uint8_t txHTMcs2(uint64_t now) const { return txHTMcs(now) ? metadata.rxHTMcs2 : 0; }
    uint16_t txHTCap(uint64_t now) const { return txHTMcs(now) ? metadata.htCapabilities : 0; }

    bool aggregateTX(uint64_t now, uint64_t dma, uint16_t channel, uint8_t floor) const {
        const uint8_t mcs = txHTMcs(now);
        return mcs && txRate.epoch && txRate.dmaEpoch == dma && txRate.channel == channel &&
            txRate.requestedFloor == floor && txRate.htMcsMask == mcs &&
            (txRate.current & 0xf8) == 0x80 && (mcs & (1u << (txRate.current & 7)));
    }

    void resetObservation()
    {
        const bool cached = nativeCached;
        uint8_t savedAddress[6]; copyAddress(savedAddress, address);
        *this = {};
        if (cached) {
            nativeCached = true;
            copyAddress(address, savedAddress);
        }
    }

    uint64_t syncPhaseUS() const
    {
        // The countdown describes target TX time; PHY TX may follow it later.
        const uint32_t delay = sync.phyTxTime - sync.targetTxTime;
        return uint64_t(sync.presenceMode * sync.windowPeriodTU - sync.nextWindowTU) * 1024 +
            (sync.phyTxTime && sync.targetTxTime && delay <= 0xffff ? delay : 0);
    }

    bool window(uint64_t nowUS, Window &out) const
    {
        // Local freshness policy: do not extrapolate a missing peer indefinitely.
        if (!occupied || !haveSync || !haveSequence || nowUS < syncReceivedUS ||
            nowUS - syncReceivedUS >= ScheduleFreshnessUS || nowUS < sequenceReceivedUS ||
            nowUS - sequenceReceivedUS >= ScheduleFreshnessUS)
            return false;
        const unsigned presence = sync.presenceMode;
        const unsigned period = sync.windowPeriodTU;
        // Only the repeating, 16-entry channel schedule is currently understood.
        if (!presence || (presence & (presence - 1)) || !period ||
            sequence.count != 16 || sequence.duplicates || sequence.fill != 0xffff ||
            unsigned(sequence.step) + 1 != presence ||
            sync.nextWindowTU > presence * period)
            return false;
        const uint64_t awUS = uint64_t(period) * 1024;
        const uint64_t eawUS = uint64_t(presence) * awUS;
        const uint64_t phaseUS = syncPhaseUS();
        const uint64_t elapsed = nowUS - syncReceivedUS + phaseUS;
        Window next = {};
        next.counter = uint16_t(sync.windowCounter - sync.windowCounter % presence +
                                elapsed / awUS);
        next.untilNextAWUS = awUS - elapsed % awUS;
        next.untilNextEAWUS = (presence - next.counter % presence) * awUS - elapsed % awUS;
        const uint64_t elapsedInEAWUS = eawUS - next.untilNextEAWUS;
        unsigned slot = (next.counter / presence) % sequence.count;
        next.channel = sequence.channel[slot];
        next.channelFlags = sequence.flags[slot];
        next.encoding = sequence.encoding;
        const uint8_t width = next.widthMHz();
        auto sameSlot = [&](unsigned s) {
            const uint8_t sWidth = sequence.encoding == 3 &&
                (sequence.flags[s] == 128 || sequence.flags[s] == 130) ? 80 : 20;
            return sequence.channel[s] == next.channel && sWidth == width;
        };
        unsigned ahead = 0;
        while (ahead < 15 && sameSlot((slot + 1 + ahead) % 16)) ++ahead;
        next.untilChannelChangeUS = next.untilNextEAWUS + uint64_t(ahead) * eawUS;
        unsigned behind = 0;
        while (behind < 15 && uint64_t(behind) < elapsed / eawUS &&
               sameSlot((slot + 16 - 1 - behind) % 16)) ++behind;
        unsigned runBehind = behind;
        if (ahead < 15) {
            while (runBehind < 15 && sameSlot((slot + 16 - 1 - runBehind) % 16))
                ++runBehind;
        }
        next.runStartEAW = uint16_t(next.counter / presence - behind);
        next.sinceChannelStartUS = uint64_t(runBehind) * eawUS + elapsedInEAWUS;
        out = next;
        return true;
    }
};

struct PeerTable {
    enum { Capacity = 32 };
    static constexpr uint64_t LifetimeUS = 10000000;
    Peer peers[Capacity];
    uint64_t added, expired, evicted, stale, rejected, remoteClockBacksteps;

    unsigned expire(uint64_t nowUS)
    {
        unsigned count = 0;
        for (auto &peer : peers) {
            if (peer.occupied && (nowUS < peer.lastSeenUS || nowUS - peer.lastSeenUS >= LifetimeUS)) {
                peer.resetObservation();
                ++expired;
            }
            count += peer.occupied;
        }
        return count;
    }

    const Peer *find(const uint8_t *address, uint64_t nowUS) const
    {
        for (const auto &peer : peers)
            if (peer.occupied && sameAddress(peer.address, address) &&
                nowUS >= peer.lastSeenUS && nowUS - peer.lastSeenUS < LifetimeUS)
                return &peer;
        return nullptr;
    }

    Peer *find(const uint8_t *address, uint64_t nowUS)
    {
        return const_cast<Peer *>(static_cast<const PeerTable *>(this)->find(address, nowUS));
    }

    Peer *findCached(const uint8_t *address)
    {
        for (auto &peer : peers)
            if (peer.nativeCached && sameAddress(peer.address, address)) return &peer;
        return nullptr;
    }

    void clearCache()
    {
        for (auto &peer : peers) peer.nativeCached = false;
    }

    bool update(const uint8_t *address, const uint8_t *self, const Advertisement &a,
                uint64_t nowUS, uint16_t channel, int16_t rssi, uint32_t rxDelayUS = 0)
    {
        if (!unicastAddress(address) || sameAddress(address, self) || rxDelayUS > nowUS) {
            ++rejected;
            return false;
        }
        expire(nowUS);
        Peer *selected = nullptr, *empty = nullptr, *oldest = nullptr;
        for (auto &peer : peers) {
            if ((peer.occupied || peer.nativeCached) && sameAddress(peer.address, address)) selected = &peer;
            if (!peer.occupied && !peer.nativeCached && !empty) empty = &peer;
            if (peer.occupied && !peer.nativeCached && (!oldest || peer.lastSeenUS < oldest->lastSeenUS))
                oldest = &peer;
        }
        const uint64_t receivedUS = nowUS - rxDelayUS;
        if (selected && selected->occupied) {
            // Remote PHY time can restart; order observations on the local RX clock.
            if (receivedUS < selected->lastReceiveUS ||
                (a.haveActionSequence && a.retry && selected->haveActionSequence &&
                 a.actionSequence == selected->actionSequence)) {
                ++stale;
                return false;
            }
            if (selected->havePhyTime && a.phyTxTime &&
                uint32_t(a.phyTxTime - selected->phyTime) >= 0x80000000U)
                ++remoteClockBacksteps;
        }
        if (!selected) {
            selected = empty ? empty : oldest;
            if (!selected) { ++rejected; return false; }
            if (selected->occupied) ++evicted;
            *selected = {};
            copyAddress(selected->address, address);
        }
        if (!selected->occupied) {
            selected->occupied = true;
            ++added;
        }
        Peer &peer = *selected;
        if (nowUS < peer.lastSeenUS || nowUS - peer.lastSeenUS >= LifetimeUS)
            peer.txRate = {};
        peer.lastSeenUS = nowUS;
        peer.lastReceiveUS = receivedUS;
        peer.rxChannel = channel;
        peer.rssi = rssi;
        if (a.phyTxTime) { peer.phyTime = a.phyTxTime; peer.havePhyTime = true; }
        if (a.haveActionSequence) {
            peer.haveActionSequence = true;
            peer.actionSequence = a.actionSequence;
        }
        peer.receivedMIF |= a.subtype == 3;
        // PSF and MIF may carry different TLVs; absent fields retain their values.
        if (a.fields & HTCapabilities) {
            peer.metadata.htCapabilities = a.htCapabilities;
            peer.metadata.rxHTMcs = a.rxHTMcs;
            peer.metadata.rxHTMcs2 = a.rxHTMcs2;
            peer.metadata.fields |= HTCapabilities;
            peer.htCapabilitiesReceivedUS = receivedUS;
        } else if (a.subtype == 3 && (a.fields & Sync) &&
                   (peer.metadata.fields & HTCapabilities) && peer.metadata.rxHTMcs &&
                   receivedUS >= peer.htCapabilitiesReceivedUS &&
                   receivedUS - peer.htCapabilitiesReceivedUS < LifetimeUS) {
            // OOB / RTG MIF omits TLV 7 HTCapabilities; preserve advertised HT/VHT capabilities while MIF sync stays live.
            peer.htCapabilitiesReceivedUS = receivedUS;
        }
        if (a.fields & Version) {
            peer.metadata.version = a.version;
            peer.metadata.deviceClass = a.deviceClass;
            peer.metadata.fields |= Version;
        }
        if (a.fields & ElectionV2) {
            copyAddress(peer.metadata.electionMaster, a.electionMaster);
            copyAddress(peer.metadata.electionSync, a.electionSync);
            peer.metadata.distance = a.distance;
            peer.metadata.masterMetric = a.masterMetric;
            peer.metadata.selfMetric = a.selfMetric;
            peer.metadata.masterCounter = a.masterCounter;
            peer.metadata.selfCounter = a.selfCounter;
            peer.metadata.electionID = a.electionID;
            peer.metadata.fields |= a.fields & (ElectionV1 | ElectionV2);
            peer.electionV2ReceivedUS = receivedUS;
        } else if ((a.fields & ElectionV1) &&
                   (!(peer.metadata.fields & ElectionV2) ||
                    receivedUS < peer.electionV2ReceivedUS ||
                    receivedUS - peer.electionV2ReceivedUS >= Peer::ScheduleFreshnessUS)) {
            if (!sameAddress(peer.metadata.electionMaster, a.electionMaster))
                for (unsigned i = 0; i < 6; ++i) peer.metadata.electionSync[i] = 0;
            copyAddress(peer.metadata.electionMaster, a.electionMaster);
            peer.metadata.distance = a.distance;
            peer.metadata.masterMetric = a.masterMetric;
            peer.metadata.selfMetric = a.selfMetric;
            peer.metadata.electionID = (peer.metadata.electionID & 0xffff0000U) | (a.electionID & 0xffffU);
            peer.metadata.fields |= ElectionV1;
        }
        if (a.fields & Sync) {
            if (peer.haveSync && !sameAddress(peer.sync.syncMaster, a.syncMaster))
                peer.haveSequence = false;
            peer.sync = a;
            peer.syncReceivedUS = receivedUS;
            peer.haveSync = true;
        }
        if (a.fields & Channels) {
            peer.sequence = a.sequence;
            peer.sequenceReceivedUS = receivedUS;
            peer.haveSequence = true;
        } else if (a.subtype == 3 && (a.fields & Sync) && peer.haveSequence &&
                   receivedUS >= peer.sequenceReceivedUS &&
                   receivedUS - peer.sequenceReceivedUS < LifetimeUS) {
            // MIFs on the same syncMaster retain the active channel sequence when channel TLVs are omitted.
            peer.sequenceReceivedUS = receivedUS;
        }
        return true;
    }
};

} // namespace AirportAWDL
#endif
