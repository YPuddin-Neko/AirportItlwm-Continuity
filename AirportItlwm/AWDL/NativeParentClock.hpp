// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_NATIVE_PARENT_CLOCK_HPP
#define AIRPORT_AWDL_NATIVE_PARENT_CLOCK_HPP
#include "PeerState.hpp"

namespace AirportAWDL {
enum class AdvertiserResult { Applied, Invalid, Unsupported, Unavailable };
enum class AdvertiserFailure { None, Record, Timing, PeerMissing, PeerSchedule,
    PeerTiming, StampAge, NewerStamp, Root, Phase };

// 24H32 ADVERTISERS carries a 44-byte record with no version prefix. Its RX
// timestamp echoes action metadata offset 48 in network byte order.
// Offset 16 is presence mode (sync TLV value +27), not a channel number.
struct NativeParentClock {
    bool valid;
    uint8_t parent[6], root[6], channel;
    uint16_t counter;
    uint64_t epochUS, receivedUS;

    AdvertiserResult configure(const uint8_t *record, const PeerTable &peers, uint64_t nowUS,
                               AdvertiserFailure *failure = nullptr,
                               const uint8_t *expectedRoot = nullptr,
                               const uint8_t *selfAddress = nullptr)
    {
        if (failure) *failure = AdvertiserFailure::None;
        auto reject = [&](AdvertiserResult result, AdvertiserFailure cause) {
            if (failure) *failure = cause;
            return result;
        };
        if (!record || !unicastAddress(record + 32) || !unicastAddress(record + 38) ||
            le16(record + 2) > 64 || !le16(record + 6))
            return reject(AdvertiserResult::Invalid, AdvertiserFailure::Record);
        if (le16(record + 8) != 16 || le16(record + 10) != 16 || le16(record + 12) != 16 ||
            record[16] != 4)
            return reject(AdvertiserResult::Unsupported, AdvertiserFailure::Timing);
        const Peer *peer = peers.find(record + 38, nowUS);
        Window window;
        if (!peer) return reject(AdvertiserResult::Unavailable, AdvertiserFailure::PeerMissing);
        if (!peer->window(nowUS, window))
            return reject(AdvertiserResult::Unavailable, AdvertiserFailure::PeerSchedule);
        if (peer->sync.windowPeriodTU != 16 || peer->sync.presenceMode != 4)
            return reject(AdvertiserResult::Unavailable, AdvertiserFailure::PeerTiming);
        const uint32_t stamp = (uint32_t(record[20]) << 24) | (uint32_t(record[21]) << 16) |
            (uint32_t(record[22]) << 8) | record[23];
        const uint32_t age = uint32_t(nowUS) - stamp;
        if (age >= 3000000 || age > nowUS)
            return reject(AdvertiserResult::Unavailable, AdvertiserFailure::StampAge);
        if (nowUS - age > peer->lastReceiveUS)
            return reject(AdvertiserResult::Unavailable, AdvertiserFailure::NewerStamp);

        NativeParentClock next = {};
        copyAddress(next.parent, peer->address);
        copyAddress(next.root, peer->sync.syncMaster);
        next.receivedUS = peer->syncReceivedUS;
        next.channel = peer->sync.masterChannel;
        next.counter = uint16_t(peer->sync.windowCounter & ~3U);
        uint64_t phase = peer->syncPhaseUS();
        if (nowUS - age == peer->syncReceivedUS) {
            const bool rootKnown = sameAddress(record + 32, peer->sync.syncMaster) ||
                peers.find(record + 32, nowUS) != nullptr ||
                (expectedRoot && unicastAddress(expectedRoot) && sameAddress(record + 32, expectedRoot)) ||
                (selfAddress && unicastAddress(selfAddress) && sameAddress(record + 32, selfAddress));
            if (!rootKnown)
                return reject(AdvertiserResult::Unavailable, AdvertiserFailure::Root);
            copyAddress(next.root, record + 32);
            next.counter = uint16_t(le16(record) & ~3U);
            phase = uint64_t(64 - le16(record + 2)) * 1024 + le16(record + 4);
        }
        // A newer RX supersedes an asynchronously returned record. An AF with
        // no sync TLV must not move the retained countdown to its newer RX time.
        if (phase > next.receivedUS)
            return reject(AdvertiserResult::Unavailable, AdvertiserFailure::Phase);
        next.epochUS = next.receivedUS - phase;
        next.valid = true;
        *this = next;
        return AdvertiserResult::Applied;
    }

    bool matches(const Peer &peer) const
    {
        return valid && sameAddress(parent, peer.address) &&
            receivedUS == peer.syncReceivedUS;
    }
};
}
#endif
