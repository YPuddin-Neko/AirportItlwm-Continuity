// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_RADIO_STATE_HPP
#define AIRPORT_AWDL_RADIO_STATE_HPP
#include <stdint.h>
#include "../../itlwm/hal_iwx/TxQueueSetup.hpp"
#include "RadioTraits.hpp"
#include "TXBlockAck.hpp"

namespace AirportAWDL {
// Firmware resources owned by the AWDL MAC, serialized on the IWX workloop.
struct RadioState {
    enum { MacID = 1, StationID = 3, PhyID = 1, DataTID = 0, LLWTID = 7 };
    bool changing, ready, fault, mac, binding, station, awake, stopPending;
    bool controllerReinit;
    bool listener, listenerPromisc, listenerFilterKnown;
    uint32_t listenerFilterFlags;
    uint64_t lastRXPayloadUS, lastHeavyRXPayloadUS;
    int queue;
    int dataQueue;
    bool dataQueueConfigured;
    int llwQueue;
    bool llwQueueConfigured;
    bool llwQueueFailed;
    bool llwRealtime;
    bool idleUnbound;
    uint32_t realtimeSessionMask;
    uint8_t realtimePeers[4][6];
    uint64_t idleEligibleSinceUS, lastIdleProbeUS;
    uint64_t idleUnbinds, idleRebinds;
    void clearRealtimeState() {
        realtimeSessionMask = 0;
        for (unsigned s = 0; s < 4; ++s)
            for (unsigned i = 0; i < 6; ++i) realtimePeers[s][i] = 0;
    }
    static inline bool containsIgnoreCase(const char *haystack, size_t hayLen, const char *needle) {
        size_t nLen = 0;
        while (needle[nLen]) ++nLen;
        if (!nLen || hayLen < nLen) return false;
        for (size_t i = 0; i + nLen <= hayLen; ++i) {
            bool ok = true;
            for (size_t j = 0; j < nLen; ++j) {
                char a = haystack[i + j];
                char b = needle[j];
                if (a >= 'A' && a <= 'Z') a = char(a + ('a' - 'A'));
                if (b >= 'A' && b <= 'Z') b = char(b + ('a' - 'A'));
                if (a != b) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    }
    bool updateRealtimeRegistration(const void *rawData, size_t maxBytes) {
        if (!rawData || maxBytes < 0xac) return false;
        const uint8_t *raw = static_cast<const uint8_t *>(rawData);
        const uint32_t nameLen = uint32_t(raw[0x0c]) | (uint32_t(raw[0x0d]) << 8) |
            (uint32_t(raw[0x0e]) << 16) | (uint32_t(raw[0x0f]) << 24);
        if (!nameLen || nameLen > 152 || 0x10 + size_t(nameLen) > maxBytes) return false;
        const char *name = reinterpret_cast<const char *>(raw + 0x10);
        int slot = -1;
        if (containsIgnoreCase(name, nameLen, "sidecar")) slot = 0;
        else if (containsIgnoreCase(name, nameLen, "facetime") ||
                 containsIgnoreCase(name, nameLen, "continuity")) slot = 1;
        else if (containsIgnoreCase(name, nameLen, "airplay") ||
                 containsIgnoreCase(name, nameLen, "screen")) slot = 2;
        if (slot < 0) return false;
        const uint32_t active = uint32_t(raw[0xa8]) | (uint32_t(raw[0xa9]) << 8) |
            (uint32_t(raw[0xaa]) << 16) | (uint32_t(raw[0xab]) << 24);
        if (active) {
            realtimeSessionMask |= (1U << unsigned(slot));
            const uint8_t *peer = raw + 4;
            const bool validPeer = !(peer[0] & 1) &&
                (peer[0] | peer[1] | peer[2] | peer[3] | peer[4] | peer[5]);
            if (validPeer) {
                for (unsigned i = 0; i < 6; ++i) realtimePeers[slot][i] = peer[i];
            }
            idleEligibleSinceUS = 0;
        } else {
            realtimeSessionMask &= ~(1U << unsigned(slot));
            for (unsigned i = 0; i < 6; ++i) realtimePeers[slot][i] = 0;
        }
        return true;
    }
    void setLLWRealtimePeer(const uint8_t *peer, bool active) {
        if (active && peer && !(peer[0] & 1) &&
            (peer[0] | peer[1] | peer[2] | peer[3] | peer[4] | peer[5])) {
            realtimeSessionMask |= (1U << 3U);
            for (unsigned i = 0; i < 6; ++i) realtimePeers[3][i] = peer[i];
            idleEligibleSinceUS = 0;
        } else {
            realtimeSessionMask &= ~(1U << 3U);
            for (unsigned i = 0; i < 6; ++i) realtimePeers[3][i] = 0;
        }
    }
    bool hasRealtimePeer(const uint8_t *addr) const {
        if (!addr || !realtimeSessionMask) return false;
        for (unsigned slot = 0; slot < 4; ++slot) {
            if (!(realtimeSessionMask & (1U << slot))) continue;
            bool match = true, nonZero = false;
            for (unsigned i = 0; i < 6; ++i) {
                nonZero |= realtimePeers[slot][i] != 0;
                if (realtimePeers[slot][i] != addr[i]) { match = false; break; }
            }
            if (nonZero && match) return true;
        }
        return false;
    }
    uint8_t address[6];
    uint16_t channel;
    uint8_t widthMHz;
    uint64_t phyReceiveEpoch;
    uint8_t phyReceiveAntennas; // Nonzero only after PHY1 and RLC both acknowledge.
    bool quz0034; // PCI 8086:02f0, subsystem 8086:0034, with the QuZ/JF configuration.
    uint8_t txAntennaMask; // A zero mask follows the infrastructure management antenna.
    uint64_t submitted, completed, failed;
    int lastError, cleanupError;
    enum Stage : uint32_t {
        None, PowerAwake, AddMAC, RemovePHY, ConfigurePHY, AddBinding,
        AddStation, AllocateQueue, StartSession, CancelSession, WaitSessionEnd,
        FlushStation, RemoveStation, FreeQueue, RemoveBinding, RemoveMAC, RestorePower, ModifyMAC
    };
    struct Failure {
        uint32_t stage, command, status;
        int error;
    } failure;
    IwxTxQueueSetup queueSetup;
    IwxTxQueueSetup dataQueueSetup;
    IwxTxQueueSetup llwQueueSetup;
    TXBlockAckQueue blockAckTX;
    struct {
        uint64_t requests, acknowledged, notAcknowledged, responses, unmatched, refusals;
        uint64_t receiveProbes, receiveProbesDeferred, responsesOtherBSSID;
        uint64_t agreements, framesAcknowledged, framesFailed, framesFlushed, handovers;
        uint64_t tlcConfigs, infraParks, socialParks;
        uint64_t retireStalls, retireFailures, retireDelba, retireErrors;
        uint32_t lastTLCRate, lastTLCRateKbps, peakTLCRate, peakTLCRateKbps;
        uint16_t lastStatus, lastFailStatus;
    } blockAckTXStats;
    static constexpr uint64_t MaxParkWindowUS = 140000ULL;
    uint64_t lastWideSocialVisitUS, parkStartUS;
    struct FirmwareHealth {
        uint64_t resetCount;
        uint32_t lastAssertID, lastAssertPC, lastCmdID;
        uint32_t lastUMACAssertID, lastUMACPC, lastUMACCmd;
        uint16_t lastResetChannel;
        uint8_t lastResetWidthMHz;
        uint32_t wideFaultCount;
    } fwHealth;
    uint32_t lastRXRateFlags, lastRXRateKbps, peakRXRateKbps;
    void recordRXRate(uint32_t rawRate, bool v2) {
        if (!rawRate || rawRate == 0xffffffffU) return;
        uint32_t kbps = 0;
        static constexpr unsigned tones[4] = {52, 108, 234, 468};
        static constexpr unsigned heTones[4] = {234, 468, 980, 1960};
        static constexpr unsigned bits[12] = {1, 2, 2, 4, 4, 6, 6, 6, 8, 8, 10, 10};
        static constexpr unsigned num[12] = {1, 1, 3, 1, 3, 2, 3, 5, 3, 5, 3, 5};
        static constexpr unsigned den[12] = {2, 2, 4, 2, 4, 3, 4, 6, 4, 6, 4, 6};
        static constexpr unsigned cckKbps[4] = {1000, 2000, 5500, 11000};
        static constexpr unsigned ofdmKbps[8] = {6000, 9000, 12000, 18000, 24000, 36000, 48000, 54000};
        if (v2) {
            const unsigned mod = (rawRate >> 8) & 7U;
            const unsigned code = rawRate & 0xfU;
            const unsigned bw = (rawRate >> 11) & 7U;
            const unsigned nss = ((rawRate >> 4) & 3U) + 1U;
            if (mod == 0 && (rawRate & 7U) < 4U) kbps = cckKbps[rawRate & 7U];
            else if (mod == 1 && (rawRate & 7U) < 8U) kbps = ofdmKbps[rawRate & 7U];
            else if (mod == 2 && bw <= 1U && (rawRate & 7U) <= 7U) {
                const unsigned mcs = rawRate & 7U;
                const unsigned sym = (rawRate & (1U << 20)) ? 36U : 40U;
                kbps = tones[bw] * nss * bits[mcs] * num[mcs] * 10000U / (den[mcs] * sym);
            } else if (mod == 3 && bw <= 3U && code <= 9U && (bw || code != 9U)) {
                const unsigned sym = (rawRate & (1U << 20)) ? 36U : 40U;
                kbps = tones[bw] * nss * bits[code] * num[code] * 10000U / (den[code] * sym);
            } else if (mod == 4 && bw <= 3U && code <= 11U && (((rawRate >> 23) & 3U) <= 1U)) {
                const unsigned gi = (rawRate >> 20) & 7U;
                if (gi <= 4U) {
                    const unsigned sym = 128U + (gi == 2U ? 16U : gi == 3U ? 32U : 8U);
                    const unsigned nsd = (rawRate & (1U << 26)) ? 102U : heTones[bw];
                    unsigned div = den[code] * sym;
                    if (rawRate & (1U << 18)) div *= 2U;
                    kbps = nsd * nss * bits[code] * num[code] * 10000U / div;
                }
            }
        } else {
            const unsigned bw = (rawRate >> 11) & 3U;
            const bool sgi = (rawRate & (1U << 13)) != 0;
            if (rawRate & (1U << 8)) {
                const unsigned mcs = rawRate & 7U;
                const unsigned nss = ((rawRate >> 3) & 3U) + 1U;
                if (bw <= 1U) {
                    const unsigned sym = sgi ? 36U : 40U;
                    kbps = tones[bw] * nss * bits[mcs] * num[mcs] * 10000U / (den[mcs] * sym);
                }
            } else if (rawRate & (1U << 26)) {
                const unsigned mcs = rawRate & 0xfU;
                const unsigned nss = ((rawRate >> 4) & 3U) + 1U;
                if (mcs <= 9U && (bw || mcs != 9U)) {
                    const unsigned sym = sgi ? 36U : 40U;
                    kbps = tones[bw] * nss * bits[mcs] * num[mcs] * 10000U / (den[mcs] * sym);
                }
            } else if (rawRate & (1U << 10)) {
                const unsigned mcs = rawRate & 0xfU;
                const unsigned nss = ((rawRate >> 4) & 3U) + 1U;
                const unsigned heType = (rawRate >> 22) & 3U;
                if (heType <= 1U && mcs <= 11U) {
                    unsigned gi = (rawRate >> 20) & 3U;
                    if (gi == 3U && sgi) gi = 4U;
                    const unsigned sym = 128U + (gi == 2U ? 16U : gi == 3U ? 32U : 8U);
                    const unsigned nsd = (rawRate & (1U << 28)) ? 102U : heTones[bw];
                    unsigned div = den[mcs] * sym;
                    if (rawRate & (1U << 18)) div *= 2U;
                    kbps = nsd * nss * bits[mcs] * num[mcs] * 10000U / div;
                }
            } else if (rawRate & (1U << 9)) {
                const uint8_t plcp = rawRate & 0xffU;
                kbps = plcp == 10 ? 1000U : plcp == 20 ? 2000U : plcp == 55 ? 5500U : plcp == 110 ? 11000U : 0U;
            } else {
                const uint8_t plcp = rawRate & 0xffU;
                kbps = plcp == 13 ? 6000U : plcp == 15 ? 9000U : plcp == 5 ? 12000U : plcp == 7 ? 18000U :
                    plcp == 9 ? 24000U : plcp == 11 ? 36000U : plcp == 1 ? 48000U : plcp == 3 ? 54000U : 0U;
            }
        }
        lastRXRateFlags = rawRate;
        if (kbps) {
            lastRXRateKbps = kbps;
            if (kbps > peakRXRateKbps) peakRXRateKbps = kbps;
        }
    }
    void (*txReady)(void *);
    static constexpr uint32_t WideFaultLimit = 2;
    bool wideCircuitTripped() const { return fwHealth.wideFaultCount >= WideFaultLimit; }
    static inline uint16_t wide80Block(uint32_t channel) {
        if (channel >= 36 && channel <= 48 && ((channel - 36) % 4) == 0) return 36;
        if (channel >= 52 && channel <= 64 && ((channel - 52) % 4) == 0) return 52;
        if (channel >= 100 && channel <= 112 && ((channel - 100) % 4) == 0) return 100;
        if (channel >= 116 && channel <= 128 && ((channel - 116) % 4) == 0) return 116;
        if (channel >= 132 && channel <= 144 && ((channel - 132) % 4) == 0) return 132;
        if (channel >= 149 && channel <= 161 && ((channel - 149) % 4) == 0) return 149;
        return 0;
    }
    static inline bool wide80OverlapsInfra(uint32_t channel, uint32_t infraChannel, bool infra160 = false) {
        if (!channel || !infraChannel) return false;
        if (channel == infraChannel) return true;
        const uint16_t block = wide80Block(channel);
        if (!block) return true;
        const uint16_t infraBlock = wide80Block(infraChannel);
        if (infraBlock && block == infraBlock) return true;
        if (infra160 && ((block == 36 && infraBlock == 52) || (block == 52 && infraBlock == 36) ||
                         (block == 100 && infraBlock == 116) || (block == 116 && infraBlock == 100)))
            return true;
        return false;
    }
    bool receiveQuiet(uint64_t now) const {
        return !lastRXPayloadUS || (now >= lastRXPayloadUS && now - lastRXPayloadUS >= 300000);
    }
    bool heavyReceiveActive(uint64_t now) const {
        return lastHeavyRXPayloadUS && now >= lastHeavyRXPayloadUS &&
            now - lastHeavyRXPayloadUS < 300000;
    }
    bool heavyTransmitActive(uint64_t now) const {
        return blockAckTX.lastHeavyDataUS && now >= blockAckTX.lastHeavyDataUS &&
            now - blockAckTX.lastHeavyDataUS < 500000;
    }
    bool deferReceiveProbe(uint64_t now) const {
        return heavyReceiveActive(now) && !heavyTransmitActive(now);
    }
    bool listenerWantsPromisc(bool receiveSession, bool separateChannel, uint64_t now) const {
        return separateChannel && (!receiveSession ||
            (blockAckTX.phase == TXBlockAckQueue::Asking &&
             blockAckTX.receiveProbeUntilUS > now && !deferReceiveProbe(now)));
    }
    bool ownsQueue(unsigned qid) const {
        return qid == unsigned(queue) ||
            (dataQueueConfigured && qid == unsigned(dataQueue)) ||
            (llwQueueConfigured && qid == unsigned(llwQueue));
    }
    struct {
        uint64_t received, malformed, otherMAC, unexpectedStart, inactive;
        uint64_t cancels, cancelAcknowledged, cancelCompletions, waits, waitTimeouts;
        uint64_t deferred, addUS, startUS, cancelAckUS, flushAckUS, endUS, cancelCompletedUS;
        uint32_t lastLength, lastMAC, lastConfig, lastStart, lastStatus, lastEndConfig;
    } session;
    void recordFailure(Stage stage, int error, uint32_t command = 0,
                       uint32_t status = UINT32_MAX) {
        // Keep the initiating failure when cleanup also fails.
        if (error && !failure.error) failure = {stage, command, status, error};
    }
    struct PeerGrant {
        bool valid, dense;
        uint8_t address[6];
        uint64_t epoch, endUS;
        uint16_t localCounter, remoteCounter, localPeriod, localPresence, remotePeriod, remotePresence;
    };
    struct {
        bool requested, running, cancelling, cancelAcknowledged, flushed, cancelCompleted, failed;
        int done;
        uint64_t windowEndUS, eawEndUS; // Conservative TX deadline and logical EAW end.
        uint64_t apCreditUS;
        uint32_t durationTU, confID;
        uint16_t slot;
        PeerGrant peer;
    } dwell;
    struct GrantPolicy {
        uint16_t slot;
        bool isolated, dataDemand;
        const void *context;
        uint64_t (*isolatedEnd)(const void *, uint64_t now, uint16_t slot, uint16_t channel);
        PeerGrant peer;
        uint64_t (*peerEnd)(const void *, uint64_t now, const PeerGrant &, bool requireDemand);
        bool dataQueue;
        uint64_t (*windowEnd)(const void *, uint64_t now, uint16_t slot, uint16_t channel);
        uint8_t widthMHz = 20;
        bool llwQueue = false;
        bool llwRealtime = false;
        bool parkIdleOnInfra = false;
        bool unbindIdle = false;
        uint16_t idleHomeChannel = 0;
        bool bridgeInfraToWide = false;
    };
    bool evaluateIdleUnbind(uint64_t nowUS, uint16_t windowChannel, bool baseIdleCandidate,
                            GrantPolicy &policy, uint16_t &serviceChannel) {
        const bool idleCandidate = baseIdleCandidate && !realtimeSessionMask &&
            blockAckTX.phase == TXBlockAckQueue::Idle &&
            !heavyReceiveActive(nowUS) && !heavyTransmitActive(nowUS) &&
            (!lastRXPayloadUS || (nowUS >= lastRXPayloadUS &&
             nowUS - lastRXPayloadUS >= 5000000ULL));
        if (!idleCandidate) {
            idleEligibleSinceUS = 0;
            return false;
        }
        if (!idleEligibleSinceUS)
            idleEligibleSinceUS = nowUS;
        if (nowUS < idleEligibleSinceUS || nowUS - idleEligibleSinceUS < 3000000ULL)
            return false;
        const bool socialChannel = windowChannel == 149 || windowChannel == 6 || windowChannel == 44;
        const bool probeDue = socialChannel &&
            (!lastIdleProbeUS || (nowUS >= lastIdleProbeUS &&
             nowUS - lastIdleProbeUS >= 2000000ULL));
        if (probeDue)
            lastIdleProbeUS = nowUS;
        const bool inProbeWindow = socialChannel && lastIdleProbeUS &&
            nowUS >= lastIdleProbeUS && nowUS - lastIdleProbeUS < 70000ULL;
        if (inProbeWindow)
            return false;
        policy.unbindIdle = true;
        serviceChannel = 0;
        return true;
    }
    struct {
        uint64_t settledUS;
        int64_t creditUS;
    } apTime;
    uint64_t availableAPCredit(uint64_t nowUS) const {
        if (!apTime.settledUS || nowUS < apTime.settledUS) return 0;
        const uint64_t idleUS = nowUS - apTime.settledUS;
        // Bank at most one maximum grant. Time already repaying a long grant
        // is debt, so the same infrastructure interval cannot be spent twice.
        const uint64_t debtUS = apTime.creditUS < 0 ? uint64_t(-apTime.creditUS) : 0;
        if (idleUS < debtUS) return 0;
        const uint64_t earnedUS = idleUS - debtUS;
        const uint64_t savedUS = apTime.creditUS > 0 ? uint64_t(apTime.creditUS) : 0;
        return earnedUS >= 65536 - savedUS ? 65536 : savedUS + earnedUS;
    }
    void reserveAPTime(uint64_t endUS, bool cancelled) {
        uint64_t restUS = 8000;
        uint64_t boundaryUS = endUS;
        if (dwell.durationTU > 16) {
            restUS = uint64_t(dwell.durationTU) * 1024;
            // END carries no session cookie. An early END can belong to an
            // older cancelled session; reserve through the newest known grant.
            const uint64_t startUS = session.startUS ? session.startUS : session.addUS;
            if (!cancelled && startUS + restUS > boundaryUS) boundaryUS = startUS + restUS;
        }
        uint64_t occupiedUS = boundaryUS > session.addUS ? boundaryUS - session.addUS : 0;
        if (dwell.durationTU > 16 && occupiedUS < uint64_t(dwell.durationTU) * 1024)
            occupiedUS = uint64_t(dwell.durationTU) * 1024;
        // Recompute from this grant's ADD snapshot if END and cancellation
        // completion both arrive; neither earns credit while the radio is owned.
        apTime.creditUS = int64_t(dwell.apCreditUS) - int64_t(occupiedUS);
        apTime.settledUS = boundaryUS;
        if (dwell.durationTU > 16) {
            const uint64_t debtUS = apTime.creditUS < 0 ? uint64_t(-apTime.creditUS) : 0;
            restUS = debtUS > 8000 ? debtUS : 8000;
        } else if (apTime.creditUS < 0) {
            // Ordinary discovery grants keep their existing 8 ms spacing.
            apTime.creditUS = 0;
        }
        if (boundaryUS + restUS > nextGrantUS) nextGrantUS = boundaryUS + restUS;
    }
    uint64_t dwellStarts, dwellEnds, dwellFailures, llwWindowContinuations;
    uint64_t nextGrantUS; // Earliest ADD after END or cancellation; survives normal stop/start.
};
}
#endif
