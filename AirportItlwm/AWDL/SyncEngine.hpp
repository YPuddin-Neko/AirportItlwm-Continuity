// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_SYNC_ENGINE_HPP
#define AIRPORT_AWDL_SYNC_ENGINE_HPP
#include "DataFrame.hpp"
#include "NativeParentClock.hpp"
#include "RadioTraits.hpp"
#include "Unicast.hpp"

namespace AirportAWDL {
static inline void putLE32(uint8_t *p, uint32_t v)
{
    putLE16(p, v); putLE16(p + 2, v >> 16);
}

static inline uint8_t operatingClass(uint8_t channel)
{
    if (!channel) return 0;
    if (channel <= 13) return 81;
    if (channel == 14) return 82;
    if (channel <= 48) return 115;
    if (channel <= 64) return 118;
    if (channel <= 144) return 121;
    return channel <= 161 ? 124 : 125;
}

static inline uint16_t wide80Block(uint32_t channel)
{
    if (channel >= 36 && channel <= 48 && ((channel - 36) % 4) == 0) return 36;
    if (channel >= 52 && channel <= 64 && ((channel - 52) % 4) == 0) return 52;
    if (channel >= 100 && channel <= 112 && ((channel - 100) % 4) == 0) return 100;
    if (channel >= 116 && channel <= 128 && ((channel - 116) % 4) == 0) return 116;
    if (channel >= 132 && channel <= 144 && ((channel - 132) % 4) == 0) return 132;
    if (channel >= 149 && channel <= 161 && ((channel - 149) % 4) == 0) return 149;
    return 0;
}

static inline bool wide80OverlapsInfra(uint32_t channel, uint32_t infraChannel, bool infra160 = false)
{
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

namespace Detail {
template <unsigned N>
struct SyncRank : SyncRank<N - 1> {};
template <>
struct SyncRank<0> {};

template <typename N>
inline auto nodeIs160MHz(const N &n, int) -> decltype(n.ni_chw == 4 || n.ni_chw == 5)
{
    return n.ni_chw == 4 || n.ni_chw == 5;
}
template <typename N>
inline bool nodeIs160MHz(const N &, long)
{
    return false;
}

template <typename IC>
inline auto icLiveScanActive(const IC &ic, SyncRank<2>)
    -> decltype((ic.ic_flags & 0x08000000U) != 0)
{
#ifdef KERNEL
    return ((ic.ic_flags & 0x08000000U) != 0) || (static_cast<int>(ic.ic_state) == 1);
#else
    return (ic.ic_flags & 0x08000000U) != 0;
#endif
}
template <typename IC>
inline auto icLiveScanActive(const IC &ic, SyncRank<1>)
    -> decltype(static_cast<bool>(ic.wclLiveScanActive))
{
    return static_cast<bool>(ic.wclLiveScanActive);
}
template <typename IC>
inline bool icLiveScanActive(const IC &, SyncRank<0>)
{
    return false;
}

template <typename I>
inline auto ifaceContinuityMedia(const I &iface, uint64_t, SyncRank<2>)
    -> decltype(iface.continuityMediaActive())
{
    return iface.continuityMediaActive();
}
template <typename I>
inline auto ifaceContinuityMedia(const I &iface, uint64_t nowUS, SyncRank<1>)
    -> decltype(iface.running && iface.radio->awdlRadio.heavyTransmitActive(nowUS))
{
    if (!iface.requested || !iface.running || iface.stopping || !iface.radio ||
        !iface.radio->awdlRadio.ready || iface.radio->awdlRadio.stopPending ||
        iface.radio->awdlRadio.fault)
        return false;
    const auto &st = iface.radio->awdlRadio;
    if (realtimeSessionMaskOf(st) != 0 || (iface.llwActive && iface.llwParamsEnabled) ||
        st.heavyReceiveActive(nowUS) || st.heavyTransmitActive(nowUS))
        return true;
    const auto &tx = st.blockAckTX;
    return tx.configured && int(tx.phase) == 2 &&
        tx.lastDataUS && nowUS >= tx.lastDataUS && nowUS - tx.lastDataUS < 2000000ULL;
}
template <typename I>
inline bool ifaceContinuityMedia(const I &iface, uint64_t nowUS, SyncRank<0>)
{
    if (!iface.requested || iface.stopping || !iface.radio || !iface.radio->awdlRadio.ready)
        return false;
    if (iface.llwParamsEnabled || realtimeSessionMaskOf(iface.radio->awdlRadio) != 0)
        return true;
    for (const auto &p : iface.radio->awdlPeers.peers)
        if (p.occupied && p.lastUnicastDataUS && nowUS >= p.lastUnicastDataUS &&
            nowUS - p.lastUnicastDataUS < 2000000ULL)
            return true;
    return false;
}
}

template <typename N>
static inline bool bssNodeIs160MHz(const N *n)
{
    return n && Detail::nodeIs160MHz(*n, 0);
}

template <typename IC>
static inline bool controllerLiveScanActive(const IC *ic)
{
    return ic && Detail::icLiveScanActive(*ic, Detail::SyncRank<2>{});
}

template <typename I>
static inline bool interfaceContinuityMediaActive(const I &iface, uint64_t nowUS)
{
    return Detail::ifaceContinuityMedia(iface, nowUS, Detail::SyncRank<2>{});
}

// Timing is expressed in the receiver's monotonic clock, never wall-clock time.
struct SyncEngine {
    uint8_t address[6], master[6], sync[6], channel;
    uint8_t localMasterChannel, configuredMasterChannel;
    uint8_t channels[16];
    uint8_t channelClasses[16];
    uint16_t periodTU, presence, actionPeriodTU, wifiSequence, dataSequence;
    uint16_t nonElectionMasterPeriodTU;
    uint32_t electionID;
    int16_t electionRSSI;
    uint32_t metric, counter, masterMetric, masterCounter, distance;
    uint64_t epochUS, counterUpdatedUS;
    uint16_t epochCounter;
    bool nativeElection, nativeClockReady, lockedFixedParent;
    uint8_t nativeElectionValue[40], nativeVersionValue[2];
    uint64_t nativeSyncUS, nativeSequenceUS;
    uint8_t syncMode, syncFlags, fixedParent[6], electionFlags;
    uint8_t punchoutFallbackChannels[16];
    uint8_t punchoutFallbackClasses[16];
    uint64_t scanPunchoutPreservedUntilUS;
    bool punchoutFallbackDiverged;
    bool sequenceDiverged;
    NativeParentClock nativeParent;

    void start(const uint8_t *self, uint8_t operatingChannel, uint64_t nowUS)
    {
        uint8_t saved[6]; copyAddress(saved, self);
        *this = {};
        copyAddress(address, saved); copyAddress(master, saved); copyAddress(sync, saved);
        channel = localMasterChannel = operatingChannel;
        for (auto &entry : channels) entry = operatingChannel;
        periodTU = 16; presence = 4; actionPeriodTU = 110;
        metric = masterMetric = 100;
        electionRSSI = -70;
        copyAddress(fixedParent, saved);
        epochUS = counterUpdatedUS = nowUS;
    }

    void configureSyncState(uint8_t mode, uint8_t flags, const uint8_t *parent)
    {
        if (syncMode != mode || syncFlags != flags || !sameAddress(fixedParent, parent)) {
            nativeClockReady = false;
            lockedFixedParent = false;
        }
        syncMode = mode; syncFlags = flags;
        copyAddress(fixedParent, parent);
    }

    void restart(uint8_t operatingChannel, uint64_t nowUS)
    {
        // Native configuration and packet sequences survive a sync disable/enable.
        channel = localMasterChannel = configuredMasterChannel ? configuredMasterChannel : operatingChannel;
        if (!nativeElection) {
            copyAddress(master, address); copyAddress(sync, address);
            distance = counter = masterCounter = 0;
            masterMetric = metric;
        }
        nativeClockReady = lockedFixedParent = false;
        scanPunchoutPreservedUntilUS = 0;
        punchoutFallbackDiverged = false;
        nativeParent = {};
        epochCounter = 0;
        epochUS = counterUpdatedUS = nowUS;
    }

    bool expireScanPunchout(uint64_t nowUS)
    {
        if (!scanPunchoutPreservedUntilUS || nowUS < scanPunchoutPreservedUntilUS)
            return false;
        for (unsigned i = 0; i < 16; ++i) {
            channels[i] = punchoutFallbackChannels[i];
            channelClasses[i] = punchoutFallbackClasses[i];
        }
        sequenceDiverged = punchoutFallbackDiverged;
        scanPunchoutPreservedUntilUS = 0;
        return true;
    }

    bool configureTemplate(const uint8_t *bytes, size_t size)
    {
        if (size > 1024 || (size && !bytes)) return false;
        const uint8_t *election = nullptr, *version = nullptr;
        for (size_t at = 0; at < size;) {
            if (size - at < 3 || le16(bytes + at + 1) > size - at - 3) return false;
            const unsigned length = le16(bytes + at + 1);
            if (bytes[at] == 24) {
                if (election || length != 40) return false;
                election = bytes + at + 3;
                if (!unicastAddress(election) || !unicastAddress(election + 6) ||
                    le32(election + 16) > 255) return false;
            } else if (bytes[at] == 21) {
                if (version || length != 2) return false;
                version = bytes + at + 3;
            }
            at += 3 + length;
        }
        // Sequoia supplies the election and protocol version together.
        if (bool(election) != bool(version)) return false;
        if (election) {
            // The root/parent and election parameters identify the locked tree.
            // Root and self counters can advance without changing it.
            for (unsigned i = 0; lockedFixedParent && i < 32; ++i) {
                if ((i < 12 || i >= 16) && nativeElectionValue[i] != election[i])
                    lockedFixedParent = false;
            }
            if (!nativeElection || !sameAddress(master, election) ||
                !sameAddress(sync, election + 6) || electionID != le32(election + 28))
                nativeClockReady = false;
            for (unsigned i = 0; i < 40; ++i) nativeElectionValue[i] = election[i];
            for (unsigned i = 0; i < 2; ++i) nativeVersionValue[i] = version[i];
            copyAddress(master, election); copyAddress(sync, election + 6);
            masterCounter = le32(election + 12); distance = le32(election + 16);
            masterMetric = le32(election + 20); metric = le32(election + 24);
            electionID = le32(election + 28); counter = le32(election + 36);
            if (sameAddress(master, address) && sameAddress(sync, address) && !distance)
                channel = localMasterChannel;
        } else if (nativeElection) {
            copyAddress(master, address); copyAddress(sync, address);
            channel = localMasterChannel;
            distance = counter = masterCounter = 0;
            masterMetric = metric;
            nativeClockReady = lockedFixedParent = false;
        }
        nativeElection = election != nullptr;
        return true;
    }

    bool matchesNativeElection() const
    {
        return electionID == le32(nativeElectionValue + 28) && metric == le32(nativeElectionValue + 24) &&
            masterMetric == le32(nativeElectionValue + 20) && distance == le32(nativeElectionValue + 16) &&
            sameAddress(master, nativeElectionValue);
    }

    bool transmitReady(uint64_t nowUS) const
    {
        if (!nativeElection && !syncMode) return true;
        if (nativeElection && !matchesNativeElection()) return false;
        if (nativeElection && syncMode && !(syncFlags & 0x20) && !sameAddress(fixedParent, sync))
            return false;
        const uint8_t *parent = syncMode ? fixedParent : sync;
        if (sameAddress(parent, address))
            return sameAddress(master, address) && sameAddress(sync, address) && !distance;
        return nativeClockReady && nowUS >= nativeSyncUS && nowUS >= nativeSequenceUS &&
            nowUS - nativeSyncUS < Peer::ScheduleFreshnessUS && nowUS - nativeSequenceUS < Peer::ScheduleFreshnessUS;
    }

    // Sequoia's END of slot zero can run the host election after a relay disappears.
    // Only the existing host-selected root can corroborate this receive window.
    bool rootConfirmsElectionWindow(const PeerTable &table, uint64_t nowUS, const Window &local) const
    {
        if (!lockedFixedParent || distance < 2 || !nativeElection || syncMode != 1 || syncFlags ||
            !matchesNativeElection() || !sameAddress(fixedParent, sync) ||
            !sameAddress(sync, nativeElectionValue + 6) || !presence || (local.counter / presence) % 16 ||
            nowUS < nativeSyncUS || nowUS < nativeSequenceUS ||
            (nowUS - nativeSyncUS < Peer::ScheduleFreshnessUS && nowUS - nativeSequenceUS < Peer::ScheduleFreshnessUS))
            return false;
        const Peer *relay = table.find(fixedParent, nowUS);
        if (relay && nowUS - relay->lastSeenUS < Peer::ScheduleFreshnessUS) return false;
        const Peer *root = table.find(master, nowUS);
        Window observed;
        if (!root || !(root->metadata.fields & ElectionV2) || nowUS < root->electionV2ReceivedUS ||
            nowUS - root->electionV2ReceivedUS >= Peer::ScheduleFreshnessUS || root->metadata.distance ||
            root->metadata.electionID != electionID || !sameAddress(root->metadata.electionMaster, master) ||
            !sameAddress(root->metadata.electionSync, master) || !sameAddress(root->sync.syncMaster, master) ||
            root->sync.windowPeriodTU != periodTU || root->sync.presenceMode != presence ||
            !root->window(nowUS, observed) || observed.counter != local.counter || observed.channel != local.channel)
            return false;
        const uint64_t a = observed.untilNextEAWUS, b = local.untilNextEAWUS;
        return (a > b ? a - b : b - a) <= 3000;
    }

    // A lost parent or relay+root tree leaves the local counter running. Report its
    // next slot-zero receive grant so the host can elect again; TX remains gated.
    bool parentLostElectionWindow(const PeerTable &table, uint64_t nowUS, const Window &local) const
    {
        if (!lockedFixedParent || !nativeElection || syncMode != 1 || syncFlags || !distance ||
            !matchesNativeElection() || !sameAddress(fixedParent, sync) ||
            !sameAddress(sync, nativeElectionValue + 6) ||
            (distance == 1 && !sameAddress(fixedParent, master)) ||
            !presence || (local.counter / presence) % 16 ||
            nowUS < nativeSyncUS || nowUS < nativeSequenceUS ||
            nowUS - nativeSyncUS < Peer::ScheduleFreshnessUS ||
            nowUS - nativeSequenceUS < Peer::ScheduleFreshnessUS)
            return false;
        const Peer *parent = table.find(fixedParent, nowUS);
        if (parent && (nowUS < parent->lastSeenUS ||
            nowUS - parent->lastSeenUS < Peer::ScheduleFreshnessUS))
            return false;
        if (distance > 1) {
            const Peer *root = table.find(master, nowUS);
            if (root && (nowUS < root->lastSeenUS ||
                nowUS - root->lastSeenUS < Peer::ScheduleFreshnessUS))
                return false;
        }
        return true;
    }

    void discoverySchedule(bool ch6, bool ch44, bool ch149, bool wide149 = false, bool wide44 = false)
    {
        localMasterChannel = configuredMasterChannel ? configuredMasterChannel :
            (ch149 ? 149 : (ch44 ? 44 : (ch6 ? 6 : 0)));
        if (!distance) channel = localMasterChannel;
        // Zero entries leave airtime for the AP. Move two preferred-channel
        // visits into Sequoia's multicast slots without adding radio occupancy.
        for (unsigned i = 0; i < 16; ++i) channels[i] = (i & 1) ? localMasterChannel : 0;
        channels[0] = channels[10] = localMasterChannel;
        channels[1] = channels[9] = 0;
        channels[8] = ch6 ? 6 : localMasterChannel;
        channels[13] = ch44 ? 44 : localMasterChannel;
        for (unsigned i = 0; i < 16; ++i)
            channelClasses[i] = ((wide149 && channels[i] == 149) || (wide44 && channels[i] == 44)) ? 128 : 0;
    }

    void setMasterChannel(uint8_t value)
    {
        configuredMasterChannel = localMasterChannel = value;
        channel = value;
    }

    bool window(uint64_t nowUS, Window &out) const
    {
        if (!periodTU || !presence || nowUS < epochUS) return false;
        const uint64_t period = uint64_t(periodTU) * 1024;
        const uint64_t eawUS = uint64_t(presence) * period;
        const uint64_t elapsed = nowUS - epochUS;
        out = {};
        out.counter = uint16_t(epochCounter + elapsed / period);
        out.untilNextAWUS = period - elapsed % period;
        out.untilNextEAWUS = (presence - out.counter % presence) * period - elapsed % period;
        const uint64_t elapsedInEAWUS = eawUS - out.untilNextEAWUS;
        const unsigned slot = (out.counter / presence) % 16;
        out.channel = channels[slot];
        out.encoding = 3;
        out.channelFlags = channelClasses[slot] ? channelClasses[slot] : operatingClass(out.channel);
        const uint8_t width = out.widthMHz();
        auto sameSlot = [&](unsigned s) {
            const uint8_t cls = channelClasses[s] ? channelClasses[s] : operatingClass(channels[s]);
            return channels[s] == out.channel && (cls == 128 ? 80 : 20) == width;
        };
        unsigned ahead = 0;
        while (ahead < 15 && sameSlot((slot + 1 + ahead) % 16)) ++ahead;
        out.untilChannelChangeUS = out.untilNextEAWUS + uint64_t(ahead) * eawUS;
        unsigned behind = 0;
        while (behind < 15 && uint64_t(behind) < elapsed / eawUS &&
               sameSlot((slot + 16 - 1 - behind) % 16)) ++behind;
        unsigned runBehind = behind;
        if (ahead < 15) {
            while (runBehind < 15 && sameSlot((slot + 16 - 1 - runBehind) % 16))
                ++runBehind;
        }
        out.runStartEAW = uint16_t(out.counter / presence - behind);
        out.sinceChannelStartUS = uint64_t(runBehind) * eawUS + elapsedInEAWUS;
        return true;
    }

    void elect(const PeerTable &table, uint64_t nowUS)
    {
        if (nowUS < epochUS || nowUS < counterUpdatedUS) {
            restart(localMasterChannel, nowUS);
            return;
        }
        if (nativeElection || syncMode) {
            // Fixed mode names a parent; native election owns the advertised tree.
            nativeClockReady = false;
            const uint8_t *parent = syncMode ? fixedParent : sync;
            if (sameAddress(parent, address)) {
                if (!nativeElection) {
                    copyAddress(master, address); copyAddress(sync, address);
                    distance = 0; channel = localMasterChannel;
                    const uint64_t increments = (nowUS - counterUpdatedUS) / 3140000;
                    counter += uint32_t(increments); counterUpdatedUS += increments * 3140000;
                    masterCounter = counter; masterMetric = metric;
                }
                return;
            }
            const Peer *peer = table.find(parent, nowUS);
            Window w;
            if (!peer || !peer->window(nowUS, w) ||
                peer->sync.windowPeriodTU != periodTU || peer->sync.presenceMode != presence)
                return;
            if (!nativeElection && (!(peer->metadata.fields & ElectionV2) ||
                !unicastAddress(peer->metadata.electionMaster) || peer->metadata.distance >= 10))
                return;
            // Warmup explicitly allows clock/sequence adoption independently of election.
            if (!(syncFlags & 0x20)) {
                const bool haveFreshV2 = (peer->metadata.fields & ElectionV2) &&
                    nowUS >= peer->electionV2ReceivedUS &&
                    nowUS - peer->electionV2ReceivedUS < Peer::ScheduleFreshnessUS;
                const bool allowMIFV1 = nativeElection && !haveFreshV2 &&
                    peer->receivedMIF && (peer->metadata.fields & ElectionV1);
                if (!(peer->metadata.fields & ElectionV2) && !allowMIFV1)
                    return;
                const bool idMismatch = allowMIFV1 ?
                    (uint16_t(peer->metadata.electionID) != uint16_t(electionID)) :
                    (peer->metadata.electionID != electionID);
                if ((syncMode != 2 && idMismatch) ||
                    sameAddress(peer->metadata.electionSync, address)) return;
                const bool parentAdvertiserMatchesMaster = nativeParent.matches(*peer) &&
                    sameAddress(nativeParent.root, master);
                if (nativeElection && !parentAdvertiserMatchesMaster &&
                    (!sameAddress(peer->metadata.electionMaster, master) ||
                     !sameAddress(peer->sync.syncMaster, master))) return;
            }
            const auto &a = peer->sync;
            const uint64_t phase = peer->syncPhaseUS();
            if (phase > peer->syncReceivedUS) return;
            epochUS = peer->syncReceivedUS - phase;
            epochCounter = uint16_t(a.windowCounter - a.windowCounter % a.presenceMode);
            if (a.masterChannel) channel = a.masterChannel;
            if (nativeParent.matches(*peer)) {
                epochUS = nativeParent.epochUS;
                epochCounter = nativeParent.counter;
                if (nativeParent.channel) channel = nativeParent.channel;
            }
            if (!nativeElection) {
                const auto &e = peer->metadata;
                copyAddress(master, e.electionMaster); copyAddress(sync, peer->address);
                masterMetric = e.masterMetric; masterCounter = e.masterCounter;
                distance = e.distance + 1;
            }
            nativeSyncUS = peer->syncReceivedUS; nativeSequenceUS = peer->sequenceReceivedUS;
            nativeClockReady = true;
            lockedFixedParent = nativeElection && syncMode == 1 && !syncFlags && distance >= 1 &&
                transmitReady(nowUS);
            return;
        }
        const Peer *selected = nullptr;
        uint32_t bestCounter = counter, bestMetric = metric, bestDistance = 0;
        const uint8_t *bestAddress = address;
        for (const auto &peer : table.peers) {
            Window w;
            const auto &a = peer.metadata;
            if (!(a.fields & ElectionV2) || a.electionID != electionID ||
                peer.rssi < electionRSSI || !peer.window(nowUS, w) ||
                peer.sync.windowPeriodTU != 16 || peer.sync.presenceMode != 4 ||
                a.distance >= 10 || sameAddress(a.electionSync, address) ||
                sameAddress(a.electionMaster, address) ||
                !unicastAddress(a.electionMaster)) continue;
            int addressOrder = 0;
            for (unsigned i = 0; i < 6 && !addressOrder; ++i)
                addressOrder = int(peer.address[i]) - bestAddress[i];
            bool better = a.masterCounter > bestCounter ||
                (a.masterCounter == bestCounter && (a.masterMetric > bestMetric ||
                 (a.masterMetric == bestMetric && (a.distance < bestDistance ||
                  (a.distance == bestDistance && addressOrder > 0)))));
            if (!better) continue;
            selected = &peer; bestCounter = a.masterCounter; bestMetric = a.masterMetric;
            bestDistance = a.distance; bestAddress = peer.address;
        }
        if (selected) {
            const auto &a = selected->sync;
            const uint64_t phase = selected->syncPhaseUS();
            // Ignore frames whose projected epoch predates local uptime.
            if (phase > selected->syncReceivedUS) return;
            epochUS = selected->syncReceivedUS - phase;
            epochCounter = uint16_t(a.windowCounter - a.windowCounter % a.presenceMode);
            periodTU = a.windowPeriodTU;
            presence = a.presenceMode;
            copyAddress(master, selected->metadata.electionMaster);
            copyAddress(sync, selected->address);
            masterMetric = bestMetric; masterCounter = bestCounter;
            distance = selected->metadata.distance + 1;
            if (a.masterChannel) channel = a.masterChannel;
            counterUpdatedUS = nowUS;
        } else {
            copyAddress(master, address); copyAddress(sync, address);
            channel = localMasterChannel;
            distance = 0;
            const uint64_t increments = (nowUS - counterUpdatedUS) / 3140000;
            counter += uint32_t(increments);
            counterUpdatedUS += increments * 3140000;
            masterCounter = counter; masterMetric = metric;
        }
    }

    static bool isSocialChannel(uint16_t c) { return c == 6 || c == 44 || c == 149; }

    bool isMulticastSocialSlot(unsigned slot) const
    {
        slot %= 16;
        const uint16_t c = channels[slot];
        if (!c) return false;
        bool haveSocial = false;
        for (uint16_t entry : channels) haveSocial |= isSocialChannel(entry);
        if (haveSocial && !isSocialChannel(c)) return false;
        // In Low Power mode, slot 0 holds the associated AP channel while slot 2 holds 5 GHz social.
        const unsigned firstSocial = (haveSocial && !isSocialChannel(channels[0]) &&
            isSocialChannel(channels[2])) ? 2U : 0U;
        return slot == firstSocial || slot == 8 || slot == 10;
    }

    bool localDataWindow(uint64_t nowUS, bool multicast, Window &local, bool contiguous = false,
                         uint64_t guardUS = 3000) const
    {
        if (!transmitReady(nowUS) || !window(nowUS, local) || !local.channel) return false;
        if (!multicast && contiguous)
            return local.untilChannelChangeUS >= guardUS && local.sinceChannelStartUS >= guardUS;
        const uint64_t duration = uint64_t(presence) * periodTU * 1024;
        if (local.untilNextEAWUS < guardUS || duration - local.untilNextEAWUS < guardUS) return false;
        const unsigned slot = (local.counter / presence) % 16;
        return !multicast || isMulticastSocialSlot(slot);
    }

    bool canSendTo(const Peer &peer, uint64_t nowUS, bool multicast, bool contiguous = false,
                   uint64_t guardUS = 3000) const
    {
        Window local, remote;
        if (!localDataWindow(nowUS, multicast, local, contiguous, guardUS) || !peer.window(nowUS, remote) ||
            remote.channel != local.channel) return false;
        if (!multicast && contiguous)
            return remote.untilChannelChangeUS >= guardUS && remote.sinceChannelStartUS >= guardUS;
        const uint64_t duration = uint64_t(peer.sync.presenceMode) * peer.sync.windowPeriodTU * 1024;
        return remote.untilNextEAWUS >= guardUS && duration - remote.untilNextEAWUS >= guardUS;
    }

    bool has20MHz5GHzSlot() const
    {
        for (unsigned i = 0; i < 16; ++i) {
            if (channels[i] > 14 && channelClasses[i] != 128 && channelClasses[i] != 130)
                return true;
        }
        return false;
    }

    size_t action(uint8_t *out, size_t capacity, uint64_t nowUS, bool mif,
                  const uint8_t *extra, size_t extraSize, const uint8_t *destination = nullptr,
                  bool nativeMIF = false, bool clampWidePeerRates = false,
                  bool stripChanSeqExt = false)
    {
        Window w;
        const bool oob = destination || nativeMIF;
        if (!transmitReady(nowUS) || !window(nowUS, w) || !w.channel ||
            extraSize > (oob ? 1200U : 1024U) || (extraSize && !extra)) return 0;
        if (oob && (!mif || !oobPayload(extra, extraSize, destination != nullptr))) return 0;
        if (destination && (!mif || !unicastAddress(destination) ||
            sameAddress(destination, address))) return 0;
        struct ExtGroup {
            uint16_t opClass;
            uint16_t chan;
            uint16_t bitmap;
        };
        ExtGroup extGroups[16] = {};
        unsigned numExtGroups = 0;
        for (unsigned sIdx = 0; sIdx < 16; ++sIdx) {
            const uint8_t c = channels[sIdx];
            if (!c) continue;
            const uint8_t cls = channelClasses[sIdx];
            const uint16_t opClass = (cls == 128 || cls == 130) ? uint16_t(cls) : uint16_t(operatingClass(c));
            const uint16_t primaryChan = uint16_t(c);
            unsigned g = 0;
            for (; g < numExtGroups; ++g) {
                if (extGroups[g].opClass == opClass && extGroups[g].chan == primaryChan) {
                    extGroups[g].bitmap |= uint16_t(1U << sIdx);
                    break;
                }
            }
            if (g == numExtGroups)
                extGroups[numExtGroups++] = {opClass, primaryChan, uint16_t(1U << sIdx)};
        }
        const size_t rebuiltTlv32Size = numExtGroups ? (3U + 11U * numExtGroups) : 0U;
        bool suppliedDataPath = false, suppliedService = false;
        bool haveValidTlv32 = false;
        uint8_t extId = 0, extMapId = 0;
        uint16_t extTimeCtrl = 0;
        size_t forwardedSize = extraSize;
        for (size_t at = 0; at < extraSize;) {
            if (extraSize - at < 3 || le16(extra + at + 1) > extraSize - at - 3) return 0;
            const uint8_t type = extra[at];
            const size_t payloadLen = le16(extra + at + 1);
            suppliedDataPath |= type == 12;
            suppliedService |= type == 6;
            const size_t length = 3 + payloadLen;
            if (type == 18 || (clampWidePeerRates && type == 17)) {
                forwardedSize -= length;
            } else if (type == 32) {
                forwardedSize -= length;
                if (!stripChanSeqExt && !haveValidTlv32 && payloadLen >= 9 && rebuiltTlv32Size) {
                    ChannelSequence parsedExt = {};
                    if (parseSequenceExt(extra + at + 3, payloadLen, parsedExt) == ParseResult::Valid) {
                        haveValidTlv32 = true;
                        extId = extra[at + 3];
                        extMapId = extra[at + 4];
                        extTimeCtrl = le16(extra + at + 9);
                        forwardedSize += rebuiltTlv32Size;
                    }
                }
            }
            at += length;
        }
        const size_t size = 24 + 16 + 76 + 24 + 44 +
            (oob ? 0 : 43 + (suppliedService ? 0 : 12) + (suppliedDataPath ? 0 : 18) + 5) + forwardedSize;
        if (capacity < size) return 0;
        for (size_t i = 0; i < size; ++i) out[i] = 0;
        out[0] = 0xd0;
        for (unsigned i = 0; i < 6; ++i) out[4 + i] = destination ? destination[i] : 0xff;
        copyAddress(out + 10, address); copyAddress(out + 16, BSSID);
        putLE16(out + 22, (wifiSequence++ & 0xfff) << 4);
        uint8_t *p = out + 24;
        p[0] = 127; p[2] = 0x17; p[3] = 0xf2; p[4] = 8; p[5] = 0x10;
        p[6] = mif ? 3 : 0;
        putLE32(p + 8, uint32_t(nowUS)); putLE32(p + 12, uint32_t(nowUS));
        p += 16;
        auto tlv = [&](uint8_t type, unsigned length) {
            p[0] = type; putLE16(p + 1, length);
            uint8_t *value = p + 3; p += length + 3; return value;
        };
        uint8_t *s = tlv(4, 73);
        const uint32_t nextWindowTU = (w.untilNextEAWUS + 1023) / 1024;
        const uint32_t elapsedTU = uint32_t(presence) * periodTU - nextWindowTU;
        s[0] = w.channel; putLE16(s + 1, nextWindowTU);
        s[3] = channel; putLE16(s + 5, periodTU); putLE16(s + 7, actionPeriodTU);
        putLE16(s + 9, 0x1800); putLE16(s + 11, periodTU); putLE16(s + 13, periodTU);
        putLE16(s + 15, elapsedTU < periodTU ? periodTU - elapsedTU : 0);
        for (unsigned i = 17; i < 21; ++i) s[i] = presence - 1;
        copyAddress(s + 21, master); s[27] = presence;
        putLE16(s + 29, w.counter); putLE16(s + 31, w.counter);
        s += 33; s[0] = 15; s[1] = 3; s[3] = presence - 1; putLE16(s + 4, 0xffff);
        const uint8_t *sequence = s;
        uint16_t socialChannels = 0;
        for (unsigned i = 0; i < 16; ++i) {
            const uint8_t c = channels[i];
            s[6 + 2*i] = c;
            s[7 + 2*i] = channelClasses[i] ? channelClasses[i] : operatingClass(c);
            socialChannels |= c == 6 ? 1 : (c == 44 ? 2 : (c == 149 ? 4 : 0));
        }
        s = tlv(5, 21); s[0] = electionFlags; putLE16(s + 1, uint16_t(electionID));
        s[3] = distance; copyAddress(s + 5, master);
        putLE32(s + 11, masterMetric); putLE32(s + 15, metric);
        // Embedded and standalone sequences share the current schedule, including OOB.
        s = tlv(18, 41);
        for (unsigned i = 0; i < 38; ++i) s[i] = sequence[i];
        if (!oob) {
            s = tlv(24, 40); copyAddress(s, master); copyAddress(s + 6, sync);
            putLE32(s + 12, masterCounter); putLE32(s + 16, distance);
            putLE32(s + 20, masterMetric); putLE32(s + 24, metric); putLE32(s + 36, counter);
            putLE32(s + 28, electionID);
            if (nativeElection)
                for (unsigned i = 0; i < 40; ++i) s[i] = nativeElectionValue[i];
            if (!suppliedService) tlv(6, 9);
            // Native templates carry Data Path capabilities and session state.
            if (!suppliedDataPath) {
                s = tlv(12, 15); putLE16(s, 0x8f24); s[2] = 'X'; s[3] = '0';
                putLE16(s + 5, socialChannels);
                copyAddress(s + 7, address);
            }
            s = tlv(21, 2);
            s[0] = nativeElection ? nativeVersionValue[0] : 0x31;
            s[1] = nativeElection ? nativeVersionValue[1] : 2;
        }
        bool emittedTlv32 = false;
        for (size_t at = 0; at < extraSize;) {
            const size_t payloadLen = le16(extra + at + 1);
            const size_t length = 3 + payloadLen;
            const uint8_t type = extra[at];
            if (type == 32) {
                if (haveValidTlv32 && !emittedTlv32) {
                    uint8_t *s32 = tlv(32, 11U * numExtGroups);
                    for (unsigned g = 0; g < numExtGroups; ++g) {
                        uint8_t *rec = s32 + 11U * g;
                        rec[0] = extId;
                        rec[1] = extMapId;
                        putLE16(rec + 2, extGroups[g].opClass);
                        putLE16(rec + 4, extGroups[g].chan);
                        putLE16(rec + 6, extTimeCtrl);
                        rec[8] = 2;
                        putLE16(rec + 9, extGroups[g].bitmap);
                    }
                    emittedTlv32 = true;
                }
            } else if (type != 18 && (!clampWidePeerRates || type != 17)) {
                for (size_t i = 0; i < length; ++i) p[i] = extra[at + i];
                if (clampWidePeerRates && type == 7 && payloadLen >= 4)
                    putLE16(p + 5, uint16_t(le16(p + 5) & ~uint16_t(0x0042U)));
                p += length;
            }
            at += length;
        }
        return size;
    }
};
}
#endif
