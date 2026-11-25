// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_TX_BLOCK_ACK_HPP
#define AIRPORT_AWDL_TX_BLOCK_ACK_HPP

#include "BlockAck.hpp"
#include "RadioTraits.hpp"

namespace AirportAWDL {

struct BlockAckAnswer {
    uint8_t peer[6], dialog, tid;
    uint16_t status, window, timeout;
    bool immediate, amsdu;
};

inline bool blockAckAnswer(const uint8_t *frame, size_t size, const uint8_t *self,
                           BlockAckAnswer &out)
{
    if (size < 24 || frame[0] != 0xd0 || (frame[1] & 0x47) ||
        !sameAddress(frame + 4, self) || !unicastAddress(frame + 10) ||
        sameAddress(frame + 10, self) ||
        (le16(frame + 22) & 15)) return false;
    const size_t header = (frame[1] & 0x80) ? 28 : 24;
    if (size < header + 9 || frame[header] != 3 || frame[header + 1] != 1) return false;
    copyAddress(out.peer, frame + 10);
    out.dialog = frame[header + 2];
    out.status = le16(frame + header + 3);
    const uint16_t parameters = le16(frame + header + 5);
    out.tid = uint8_t((parameters >> 2) & 15);
    out.window = parameters >> 6;
    out.timeout = le16(frame + header + 7);
    out.immediate = parameters & 2;
    out.amsdu = parameters & 1;
    return true;
}

inline bool blockAckTXEnd(const uint8_t *frame, size_t size, const uint8_t *self,
                          BlockAckRequest &out)
{
    if (size < 24 || frame[0] != 0xd0 || (frame[1] & 0x47) ||
        !sameAddress(frame + 4, self) || !unicastAddress(frame + 10) ||
        sameAddress(frame + 10, self) ||
        (le16(frame + 22) & 15)) return false;
    const size_t header = (frame[1] & 0x80) ? 28 : 24;
    if (size < header + 6 || frame[header] != 3 || frame[header + 1] != 2) return false;
    const uint16_t parameters = le16(frame + header + 2);
    // A recipient's DELBA closes our transmit agreement.
    if (parameters & 0xfff) return false;
    out = {};
    copyAddress(out.peer, frame + 10);
    out.tid = parameters >> 12;
    out.teardown = true;
    return true;
}

inline void blockAckSolicit(uint8_t (&out)[33], const uint8_t *self,
                           const BlockAckRequest &request, uint16_t sequence)
{
    for (auto &byte : out) byte = 0;
    out[0] = 0xd0;
    copyAddress(out + 4, request.peer);
    copyAddress(out + 10, self);
    copyAddress(out + 16, BSSID);
    putLE16(out + 22, uint16_t(sequence << 4));
    out[24] = 3; out[25] = 0; out[26] = request.dialog;
    putLE16(out + 27, uint16_t(request.tid << 2) | uint16_t(blockAckWindow(request) << 6) |
        (request.immediate ? 2 : 0) | (request.amsdu ? 1 : 0));
    putLE16(out + 29, request.timeout);
    putLE16(out + 31, uint16_t(request.ssn << 4));
}

inline void blockAckTXTeardown(uint8_t (&out)[30], const uint8_t *self,
                              const BlockAckRequest &request, uint16_t sequence)
{
    blockAckTeardown(out, self, request, sequence);
    putLE16(out + 26, uint16_t((request.tid << 12) | 0x800));
}

// Ownership survives pause and teardown until REMOVE_STA or hardware reset.
struct TXBlockAckQueue {
    static constexpr unsigned MaxAttempts = 40;
    static constexpr unsigned DemandThreshold = 16;
    static constexpr unsigned MaxConsecutiveFailures = 8;
    static constexpr uint64_t RetryDelayUS = 20000000;
    static constexpr uint64_t RecoveryDelayUS = 250000;
    static constexpr uint64_t ReceiveProbeDurationUS = 60000;
    static constexpr uint64_t HeavyIdleHandoverUS = 500000;
    static constexpr uint64_t IdleHandoverUS = 1000000;
    static constexpr size_t HeavyFrameBytes = 512;
    enum Phase : uint8_t { Idle, Requested, Preparing, Asking, Enabling, Agreed, Retiring } phase;
    bool configured;
    uint16_t queue;
    uint8_t station, tid;
    uint64_t epoch, cookie;
    uint64_t transmittedReports, acknowledged, failed, flushed, rejected;
    uint8_t peer[6], dialog, attempts, mcs, antenna, floorMbps, legacyMbps;
    uint8_t mcs2, widthMHz, consecutiveFailures, sgiMask, tlcMode;
    uint16_t sequence, window, htCap, tlcFlags;
    uint32_t lastTLCRate, lastTLCRateKbps;
    bool enabled, paused, amsdu, needDelba, retireFlushed, keepAgreedOnFailure, wasAgreed;
    uint64_t nextAskUS, retireDeadlineUS, windowEndUS, lastProgressUS, managementToken;
    uint64_t lastAskUS, receiveProbeUntilUS, lastDataUS, lastHeavyDataUS;
    bool receiveProbeTried, lastAskAcked, releasePeer;
    uint64_t demandStartUS, handoverStartUS;
    uint8_t handoverPeer[6];
    unsigned demandFrames, handoverFrames;
    uint64_t negotiations, agreements, pauses, resumes, recoveries;

    bool forPeer(const uint8_t *address) const { return phase != Idle && sameAddress(peer, address); }

    static constexpr bool isFatalTXStatus(uint8_t status) {
        // Intel firmware fatal TX statuses: RFKILL_FLUSH (0x86), HOST_ABORTED (0x89),
        // STA_INVALID (0x8b), STA_COLOR_MISMATCH (0x91).
        return status == 0x86 || status == 0x89 || status == 0x8b || status == 0x91;
    }

    bool negotiatingNow(uint64_t nowUS, uint64_t dwellWindowEndUS = 0) const {
        if (!configured || phase != Asking) return false;
        if (managementToken != 0 || receiveProbeUntilUS > nowUS) return true;
        return attempts < MaxAttempts && nowUS >= nextAskUS &&
            (!dwellWindowEndUS || dwellWindowEndUS > nowUS + 19000);
    }

    bool allowsSoftwareAggregate(uint64_t nowUS) const {
        return phase == Asking && !managementToken && receiveProbeUntilUS <= nowUS &&
            nextAskUS > nowUS + 15000;
    }

    void retryDialog(uint64_t now, uint64_t delayUS = RetryDelayUS) {
        if (!++dialog) ++dialog;
        attempts = 0;
        consecutiveFailures = 0;
        keepAgreedOnFailure = false;
        wasAgreed = false;
        lastAskUS = receiveProbeUntilUS = 0;
        receiveProbeTried = lastAskAcked = false;
        nextAskUS = now + delayUS;
    }

    void demand(const uint8_t *address, size_t bytes, uint64_t now, bool acceptedRX, bool queueEmpty = false) {
        if (phase == Idle) {
            if (!sameAddress(peer, address) || now < demandStartUS || now - demandStartUS > 250000) {
                copyAddress(peer, address); demandStartUS = now; demandFrames = 0;
            }
            if (bytes >= HeavyFrameBytes) ++demandFrames;
            if (acceptedRX || demandFrames >= DemandThreshold) {
                phase = Requested;
                lastDataUS = now;
                lastHeavyDataUS = (demandFrames >= DemandThreshold || bytes >= HeavyFrameBytes) ? now : 0;
                releasePeer = false;
                handoverFrames = 0;
            }
            return;
        }
        if (forPeer(address)) {
            if (phase != Retiring) {
                lastDataUS = now;
                if (bytes >= HeavyFrameBytes) {
                    lastHeavyDataUS = now;
                    handoverFrames = 0;
                }
            }
            return;
        }
        if ((phase != Agreed && !(phase == Asking && !managementToken)) || !configured)
            return;
        if (!sameAddress(handoverPeer, address) || now < handoverStartUS || now - handoverStartUS > 250000) {
            copyAddress(handoverPeer, address); handoverStartUS = now; handoverFrames = 0;
        }
        if (bytes >= HeavyFrameBytes) ++handoverFrames;
        if (!queueEmpty) return;
        const bool fullIdle = now >= lastDataUS && now - lastDataUS >= IdleHandoverUS;
        const bool heavyIdle = !lastHeavyDataUS || (now >= lastHeavyDataUS && now - lastHeavyDataUS >= HeavyIdleHandoverUS);
        const bool heavyTakeover = handoverFrames >= DemandThreshold && (heavyIdle || fullIdle);
        const bool rxTakeover = acceptedRX && fullIdle &&
            (!lastHeavyDataUS || (now >= lastHeavyDataUS && now - lastHeavyDataUS >= IdleHandoverUS));
        if (heavyTakeover || rxTakeover) {
            releasePeer = true;
            retire(now, phase != Agreed);
        }
    }

    void retire(uint64_t now, bool remote = false) {
        if (!configured) return;
        if (phase != Retiring) {
            wasAgreed = phase == Agreed;
            phase = Retiring;
            needDelba = true; retireFlushed = false;
            consecutiveFailures = 0;
            keepAgreedOnFailure = false;
            lastAskAcked = false;
            managementToken = 0; attempts = 0; nextAskUS = now;
            receiveProbeUntilUS = 0;
            retireDeadlineUS = now + 1000000;
            ++recoveries;
        }
        if (remote) needDelba = false;
        windowEndUS = 0;
    }

    bool ownsQueue(unsigned qid) const { return configured && queue == qid; }

    bool matches(unsigned qid, unsigned sta, unsigned trafficID, uint64_t currentEpoch) const {
        return ownsQueue(qid) && cookie && epoch == currentEpoch && station == sta && tid == trafficID &&
            station >= BlockAckState::FirstStation &&
            station < BlockAckState::FirstStation + BlockAckState::PeerCount && tid < BlockAckState::TIDCount;
    }

    template <class Ring>
    bool completion(const Ring &ring, unsigned read, unsigned pointerCount,
                    unsigned sta, unsigned trafficID, uint64_t currentEpoch,
                    unsigned requiredFlags, unsigned &end) const {
        if (!matches(ring.qid, sta, trafficID, currentEpoch) ||
            !pointerCount || (pointerCount & (pointerCount - 1)) ||
            !ring.ring_count || (ring.ring_count & (ring.ring_count - 1)) ||
            ring.ring_count > pointerCount || read >= pointerCount ||
            unsigned(ring.cur) >= pointerCount || unsigned(ring.tail) >= ring.ring_count ||
            unsigned(ring.queued) >= ring.ring_count) return false;
        const unsigned tail = (ring.cur - ring.queued) & (pointerCount - 1);
        const unsigned count = (read - tail) & (pointerCount - 1);
        if (count > unsigned(ring.queued) || (tail & (ring.ring_count - 1)) != unsigned(ring.tail))
            return false;
        for (unsigned offset = 0; offset < count; ++offset) {
            const auto &data = ring.data[(ring.tail + offset) & (ring.ring_count - 1)];
            if (!data.m || (data.flags & requiredFlags) != requiredFlags || data.awdlBACookie != cookie)
                return false;
        }
        end = read & (ring.ring_count - 1);
        return true;
    }
};

}
#endif
