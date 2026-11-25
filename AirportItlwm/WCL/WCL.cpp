//
//  WCL.cpp
//  AirportItlwm
//
//  Created by laobamac on 2026/10/1.
//  Copyright © 2026 laobamac. All rights reserved.
//

#ifdef AIRPORT_WCL
#include "../AirportItlwmV2.hpp"
#include "../AirportItlwmSkywalkInterface.hpp"
#if __IO80211_TARGET == __MAC_15_2
#include "../AirportItlwmAWDLInterface.hpp"
#endif
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <net80211/ieee80211_node.h>
#include <net80211/ieee80211_ioctl.h>
#include <net80211/ieee80211_priv.h>

extern IOCommandGate *_fCommandGate;

struct AirportItlwmSkywalkInterface::CountryUpdate {
    AirportItlwmSkywalkInterface *interface;
    AirportItlwm *controller;
    IOCommandGate *gate;
    IO80211WorkQueue *queue;
    thread_call_t call;
    uint64_t generation;
    uint64_t readSerial;
    bool channelComplete;
};

void AirportItlwmSkywalkInterface::deferWCLCountryUpdate(bool channelComplete)
{
    auto queue = instance->getWorkQueue();
    if (wclStopping || !_fCommandGate || !queue)
        return;
    auto update = static_cast<CountryUpdate *>(IOMalloc(sizeof(CountryUpdate)));
    if (!update)
        return;
    update->call = thread_call_allocate(wclCountryUpdateThread, update);
    if (!update->call) {
        IOFree(update, sizeof(*update));
        return;
    }
    update->interface = this;
    update->controller = instance;
    update->gate = _fCommandGate;
    update->queue = queue;
    update->generation = channelComplete ? wclCountryReloadPending : ++wclCountryGeneration;
    update->readSerial = wclChannelReadSerial;
    update->channelComplete = channelComplete;
    retain();
    instance->retain();
    update->gate->retain();
    queue->retain();
    thread_call_enter(update->call);
}

void AirportItlwmSkywalkInterface::wclCountryUpdateThread(void *arg, void *)
{
    auto update = static_cast<CountryUpdate *>(arg);
    // Native channel reconfiguration may synchronously query WCL and must run
    // under its gate from outside the driver's workloop thread.
    update->gate->runAction(wclCountryUpdateGated, update);
    thread_call_free(update->call);
    update->interface->release();
    update->controller->release();
    update->gate->release();
    update->queue->release();
    IOFree(update, sizeof(*update));
}

IOReturn AirportItlwmSkywalkInterface::wclCountryUpdateGated(
    OSObject *, void *arg, void *, void *, void *)
{
    auto update = static_cast<CountryUpdate *>(arg);
    if (update->interface->wclStopping ||
        update->generation != update->interface->wclCountryGeneration)
        return kIOReturnNotReady;
    if (!update->queue->inGate() || update->queue->onThread())
        return kIOReturnNotPermitted;
    if (!update->channelComplete) {
        update->interface->wclCountryReloadPending = update->generation;
        update->interface->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, nullptr, 0, true);
    } else if (update->generation == update->interface->wclCountryReloadPending &&
               update->readSerial == update->interface->wclChannelReadSerial) {
        // WCL commits its channel cache after IOC449 returns. This deferred
        // gated completion runs after that commit, then lets AWDL rebuild its plan.
        update->interface->wclCountryReloadPending = 0;
#if __IO80211_TARGET == __MAC_15_2
        auto awdl = update->controller->awdlInterface;
        if (awdl) {
            awdl->retain();
            // Infrastructure routing excludes both country and channel-list events.
            awdl->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, nullptr, 0, false);
            if (!update->interface->wclStopping &&
                update->generation == update->interface->wclCountryGeneration &&
                update->controller->awdlInterface == awdl)
                awdl->postMessage(AirportWCL::ChannelSpecQueryCompleted, nullptr, 0, false);
            awdl->release();
        }
#endif
    }
    return kIOReturnSuccess;
}

bool AirportItlwmSkywalkInterface::initWCL()
{
    joinTimer = IOTimerEventSource::timerEventSource(this, wclJoinTimeout);
    if (!joinTimer)
        return false;
    return instance->getWorkQueue()->addEventSource(joinTimer) == kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::stopWCL()
{
    // Pending country tasks must observe shutdown under the same gate.
    if (_fCommandGate)
        _fCommandGate->runAction(stopWCLGated, this);
    wclStopping = true;
    if (joinTimer) {
        joinTimer->cancelTimeout();
        if (joinTimer->getWorkLoop())
            joinTimer->getWorkLoop()->removeEventSource(joinTimer);
        OSSafeReleaseNULL(joinTimer);
    }
    if (scanSource)
        scanSource->cancelTimeout();
    scanPending = joinPending = false;
    joinDeferred = scanDeferred = false;
    explicit_bzero(&deferredJoin, sizeof(deferredJoin));
    if (fHalService) {
        ieee80211com *ic = fHalService->get80211Controller();
        ic->ic_wcl_join_requested = false;
        ic->ic_wcl_join_channel = 0;
        ic->ic_wcl_scan_requested = false;
        explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    }
}

IOReturn AirportItlwmSkywalkInterface::stopWCLGated(
    OSObject *, void *arg, void *, void *, void *)
{
    auto self = static_cast<AirportItlwmSkywalkInterface *>(arg);
    self->portAuthorized = false;
#ifdef KERNEL
    self->wclLastLiveBgscanUS = 0;
    AirportWCL::resetSplitScanBurst(self, true);
#endif
    self->updateWCLLinkQuality(true);
    self->wclStopping = true;
    ++self->wclCountryGeneration;
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::updateWCLLinkQuality(bool force)
{
    if (_fCommandGate)
        _fCommandGate->runAction(updateWCLLinkQualityGated, this,
                                reinterpret_cast<void *>(uintptr_t(force)));
}

IOReturn AirportItlwmSkywalkInterface::updateWCLLinkQualityGated(
    OSObject *, void *arg0, void *arg1, void *, void *)
{
    auto self = static_cast<AirportItlwmSkywalkInterface *>(arg0);
    if (self->wclStopping || !self->fHalService)
        return kIOReturnNotReady;
    auto queue = self->instance->getWorkQueue();
    if (!queue || !queue->inGate())
        return kIOReturnNotPermitted;
    auto interface = self->mExpansionData ? self->getBSDInterface() : nullptr;
    if (!interface) {
        self->wclLinkQualityInterface = nullptr;
        self->wclReportedLinkQuality = -2;
        return kIOReturnNotReady;
    }
    auto ic = self->fHalService->get80211Controller();
    int quality = -2;
    if (self->portAuthorized && self->linkState() == kIO80211NetworkLinkUp &&
        (ifnet_flags(interface) & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING) &&
        ic && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss &&
        IEEE80211_ADDR_EQ(ic->ic_bss->ni_bssid, self->joinBSSID)) {
        int rssi = IWM_MIN_DBM + ic->ic_bss->ni_rssi;
        quality = rssi >= -70 ? 100 : (rssi >= -85 ? 50 : 20);
    }
    if (arg1 || quality != self->wclReportedLinkQuality ||
        interface != self->wclLinkQualityInterface) {
        self->setLQM(static_cast<unsigned long long>(static_cast<long long>(quality)));
        self->wclReportedLinkQuality = quality;
        self->wclLinkQualityInterface = interface;
    }
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::free()
{
    if (joinTimer)
        stopWCL();
    IO80211InfraProtocol::free();
}

IOReturn AirportItlwmSkywalkInterface::wclCommand(OSObject *, void *a0, void *a1, void *a2, void *)
{
    auto self = static_cast<AirportItlwmSkywalkInterface *>(a0);
    if (self->wclStopping)
        return kIOReturnNotReady;
    switch (uintptr_t(a1)) {
        case 13: return self->copyWCLChannels(a2);
        case 14: return self->copyWCLTrafficCounters(a2);
        case 15:
            return self->fHalService->setScanHomeAwayTime(
                AirportWCL::read32(static_cast<const uint8_t *>(a2)));
        case 12:
            self->handleWCLEvent(int(uintptr_t(a2)), nullptr);
            return kIOReturnSuccess;
        case 11: self->clearWCLPMKSA(); return kIOReturnSuccess;
        case 8: return self->installWCLKey(static_cast<apple80211_key *>(a2));
        case 9: return self->accessWCLRSN(static_cast<apple80211_rsn_ie_data *>(a2), true);
        case 10: return self->accessWCLRSN(static_cast<apple80211_rsn_ie_data *>(a2), false);
        case 0: return self->beginWCLScan(static_cast<apple80211ScanRequest *>(a2));
        case 1: return self->beginWCLJoin(static_cast<apple80211AssocCandidates *>(a2));
        case 2: return self->abortWCLJoin();
        case 3: return self->leaveWCLNetwork();
        case 6: return self->copyWCLExtendedBss(a2);
        case 7:
            if (!a2 || !self->wclBssValid || !self->wclAssociatedBeacons)
                return kIOReturnNotReady;
            memcpy(a2, self->wclBeaconInfo, sizeof(self->wclBeaconInfo));
            return kIOReturnSuccess;
        case 5:
            if (!a2 || !self->wclBssValid)
                return kIOReturnNotReady;
            memcpy(a2, self->wclBssInfo, sizeof(self->wclBssInfo));
            return kIOReturnSuccess;
        case 4: {
            ieee80211com *ic = self->fHalService ? self->fHalService->get80211Controller() : nullptr;
            if (ic) {
                if (ic->ic_state == IEEE80211_S_SCAN)
                    AirportWCL::noteAbortedScanInScanState(self);
                ic->ic_flags &= ~(IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN);
                ic->ic_wcl_scan_requested = false;
                ic->ic_wcl_scan_ssid_length = 0;
                ic->ic_wcl_scan_ssid_count = 0;
            }
#ifdef KERNEL
            if (self->fHalService && self->fHalService->getDriverController())
                self->fHalService->getDriverController()->abortScanning();
#endif
            self->finishWCLScan(kIOReturnAborted);
            return kIOReturnSuccess;
        }
    }
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_SCAN_REQ(apple80211ScanRequest *request)
{
    return _fCommandGate->runAction(wclCommand, this, nullptr, request);
}

IOReturn AirportItlwmSkywalkInterface::setWCL_SCAN_ABORT(void *)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(4));
}

IOReturn AirportItlwmSkywalkInterface::setWCL_ASSOCIATE(apple80211AssocCandidates *request)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(1), request);
}

#if __IO80211_TARGET == __MAC_15_2
IOReturn AirportItlwmSkywalkInterface::setWCL_JOIN_ABORT(void *)
#else
IOReturn AirportItlwmSkywalkInterface::setWCL_JOIN_ABORT(apple80211_wcl_abort_join *)
#endif
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(2));
}

IOReturn AirportItlwmSkywalkInterface::setWCL_LEAVE_NETWORK(apple80211_leave_network *)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(3));
}

IOReturn AirportItlwmSkywalkInterface::beginWCLScan(apple80211ScanRequest *request)
{
    AirportWCL::ScanRequest scan;
    if (!AirportWCL::decodeScan(request, 0x1550, scan))
        return kIOReturnBadArgument;
    if (scanPending || joinPending)
        return kIOReturnBusy;
    auto ic = fHalService->get80211Controller();
    if ((ic->ic_if.if_flags & IFF_UP) &&
        (ic->ic_wcl_mac_reconfig || !(ic->ic_if.if_flags & IFF_RUNNING) ||
         ic->ic_state == IEEE80211_S_INIT)) {
        // Keep the first auto-join scan while firmware applies a private MAC or wakes.
        wclScanRequest = scan;
        scanDeferred = scanPending = true;
        scanCompleted = scanSkipCompletion = false;
        scanSource->setTimeoutMS(20000);
        return kIOReturnSuccess;
    }
    return startWCLScan(scan);
}

bool AirportItlwmSkywalkInterface::completeWCLScanFromCache(const AirportWCL::ScanRequest &scan)
{
#if __IO80211_TARGET == __MAC_15_2
    if (!portAuthorized || joinPending)
        return false;
    const bool directed = AirportWCL::hasDirectedSSID(scan);
#ifdef KERNEL
    ieee80211com *ic = fHalService ? fHalService->get80211Controller() : nullptr;
    uint64_t nowNS = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nowNS);
    const uint64_t nowUS = nowNS / 1000ULL;
    auto &state = AirportWCL::splitScanBurstFor(this);
    const bool splitContinuation = state.isContinuation(scan, nowUS);
    const bool associatedRun = ic && ic->ic_state == IEEE80211_S_RUN && ic->ic_bss;
    const int32_t rssiDbm = associatedRun ? -(0 - IWM_MIN_DBM - ic->ic_bss->ni_rssi) : -100;
    const bool bulkTraffic = associatedRun &&
        state.traffic.sample(AirportWCL::sampleInfraTotalBytes(ic->ic_if, instance), nowUS);
#endif
    if (directed) {
        bool throttleRepeatedMultiBatch = false;
#ifdef KERNEL
        if (associatedRun && rssiDbm >= -68 && !splitContinuation &&
            AirportWCL::isHiddenSSID(scan.ssid, scan.ssidLength) &&
            scan.ssidCount > 1) {
            AirportWCL::BeaconMetadata cachedMeta = {};
            if (wclBssValid)
                memcpy(&cachedMeta, wclBssInfo, sizeof(cachedMeta));
            bool allCached = true;
            const unsigned count = scan.ssidCount <= 10 ? scan.ssidCount : 10;
            for (unsigned i = 0; i < count; ++i) {
                const uint8_t len = scan.ssidLengths[i];
                if (AirportWCL::isHiddenSSID(scan.ssids[i], len))
                    continue;
                bool found = wclBssValid && cachedMeta.ssidLength == len &&
                    memcmp(cachedMeta.ssid, scan.ssids[i], len) == 0;
                if (!found) {
                    struct ieee80211_node *ni = nullptr;
                    RB_FOREACH(ni, ieee80211_tree, &ic->ic_tree) {
                        if (ni && ni->ni_rsnie_tlv && ni->ni_rsnie_tlv_len &&
                            AirportWCL::nodeMatchesSSID(ni, scan.ssids[i], len)) {
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    allCached = false;
                    break;
                }
            }
            if (allCached &&
                state.shouldThrottleMultiSsidBatch(AirportWCL::multiSsidDigest(scan),
                                                   nowUS, 20000000ULL)) {
                throttleRepeatedMultiBatch = true;
            }
        }
#endif
        if (!throttleRepeatedMultiBatch)
            return false;
    }
    bool skipLiveScan = directed ||
        (instance && instance->awdlInterface &&
         instance->awdlInterface->continuityMediaActive());
#ifdef KERNEL
    if (!skipLiveScan && bulkTraffic)
        skipLiveScan = true;
    if (!skipLiveScan && associatedRun) {
        const uint64_t activeGuardRefUS = state.lastStepUS > wclLastLiveBgscanUS ?
            state.lastStepUS : wclLastLiveBgscanUS;
        const uint64_t cooldownUS = (scan.type == 2) ? 45000000ULL : 8000000ULL;
        const uint64_t throttleRefUS = (scan.type == 2) ? wclLastLiveBgscanUS : activeGuardRefUS;
        if (AirportWCL::shouldThrottleAssociatedBgscan(
                throttleRefUS, nowUS, rssiDbm, cooldownUS, -68,
                false, splitContinuation))
            skipLiveScan = true;
    }
#endif
    if (!skipLiveScan)
        return false;
    if (wclBssValid) {
        AirportWCL::BeaconMetadata cached = {};
        memcpy(&cached, wclBssInfo, sizeof(cached));
        const size_t messageSize = sizeof(cached) + cached.ieLength;
        if (cached.ssidLength <= 32 && messageSize <= sizeof(wclBssInfo) &&
            AirportWCL::matchesScan(scan, cached.bssid, cached.ssid,
                cached.ssidLength, cached.channel)) {
            instance->postMessage(this, AirportWCL::ScanResult, wclBssInfo, messageSize, true);
        }
    }
#ifdef KERNEL
    if (ic) {
        struct ieee80211_node *ni = nullptr;
        RB_FOREACH(ni, ieee80211_tree, &ic->ic_tree) {
            if (!ni || IEEE80211_ADDR_EQ(ni->ni_bssid, joinBSSID) ||
                !ni->ni_rsnie_tlv || !ni->ni_rsnie_tlv_len)
                continue;
            publishWCLBeacon(ni, ni->ni_rsnie_tlv, ni->ni_rsnie_tlv_len);
        }
    }
#endif
    scanCompleted = true;
    scanSource->setTimeoutMS(1);
    return true;
#else
    (void)scan;
    return false;
#endif
}

IOReturn AirportItlwmSkywalkInterface::startWCLScan(const AirportWCL::ScanRequest &scan)
{
    ieee80211com *ic = fHalService->get80211Controller();
    if (!(ic->ic_if.if_flags & IFF_RUNNING))
        return kIOReturnNotReady;
    if (scanPending || joinPending || (ic->ic_flags & IEEE80211_F_BGSCAN))
        return kIOReturnBusy;
    if (ic->ic_state == IEEE80211_S_AUTH || ic->ic_state == IEEE80211_S_ASSOC)
        return kIOReturnBusy;
    if (scan.channelCount) {
        bool usable = false;
        for (unsigned channel = 1; channel <= IEEE80211_CHAN_MAX; ++channel)
            if (ic->ic_channels[channel].ic_flags &&
                AirportWCL::scanChannelSelected(scan.channels, true, channel))
                usable = true;
        if (!usable) {
            if (ic->ic_state == IEEE80211_S_RUN || ic->ic_state == IEEE80211_S_SCAN) {
                wclScanRequest = scan;
                scanPending = true;
                scanCompleted = true;
                scanSkipCompletion = false;
                ic->ic_wcl_scan_requested = false;
                scanSource->setTimeoutMS(1);
                return kIOReturnSuccess;
            }
            return kIOReturnUnsupported;
        }
    }
    wclScanRequest = scan;
    scanPending = true;
    scanCompleted = false;
    const bool directed = AirportWCL::hasDirectedSSID(scan);
    const bool activeScan = scan.type != 2 || directed;
    const bool restartedAbortedScan = AirportWCL::consumeAbortedScanInScanState(this);
    scanSkipCompletion = !restartedAbortedScan && ic->ic_state == IEEE80211_S_SCAN &&
        ((activeScan && (scan.ssidLength != 0 || scan.ssidCount != 0)) ||
         ic->ic_wcl_scan_ssid_length != 0 || ic->ic_wcl_scan_ssid_count != 0 ||
         ic->ic_des_esslen != 0);
    ic->ic_wcl_join_channel = 0;
    ic->ic_wcl_scan_requested = true;
    ic->ic_wcl_scan_active = activeScan;
    ic->ic_wcl_scan_ssid_length = activeScan ? scan.ssidLength : 0;
    memcpy(ic->ic_wcl_scan_ssid, scan.ssid, sizeof(scan.ssid));
    ic->ic_wcl_scan_ssid_count = activeScan ? scan.ssidCount : 0;
    memcpy(ic->ic_wcl_scan_ssid_lengths, scan.ssidLengths, sizeof(scan.ssidLengths));
    memcpy(ic->ic_wcl_scan_ssids, scan.ssids, sizeof(scan.ssids));
    ic->ic_wcl_scan_restricted = scan.channelCount != 0;
    memcpy(ic->ic_wcl_scan_channels, scan.channels, sizeof(scan.channels));
    scanSource->setTimeoutMS(20000);
    if (ic->ic_state == IEEE80211_S_RUN) {
        if (completeWCLScanFromCache(scan))
            return kIOReturnSuccess;
        ieee80211_begin_cache_bgscan(&ic->ic_if);
        if (!(ic->ic_flags & IEEE80211_F_BGSCAN)) {
            scanPending = false;
            ic->ic_wcl_scan_requested = false;
            scanSource->cancelTimeout();
            return kIOReturnBusy;
        }
#ifdef KERNEL
        uint64_t nowNS = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &nowNS);
        const uint64_t nowUS = nowNS / 1000ULL;
        auto &burst = AirportWCL::splitScanBurstFor(this);
        const bool splitContinuation = burst.isContinuation(scan, nowUS);
        burst.recordLiveScan(scan, nowUS, splitContinuation);
        if (!splitContinuation) {
            wclLastLiveBgscanUS = nowUS;
            if (directed && AirportWCL::isHiddenSSID(scan.ssid, scan.ssidLength) &&
                scan.ssidCount > 1) {
                burst.recordMultiSsidBatch(AirportWCL::multiSsidDigest(scan), nowUS);
            }
        }
#endif
    } else if (ic->ic_state != IEEE80211_S_SCAN || restartedAbortedScan) {
        int error = ic->ic_newstate(ic, IEEE80211_S_SCAN, -1);
        if (error) {
            scanPending = false;
            ic->ic_wcl_scan_requested = false;
            scanSource->cancelTimeout();
            return kIOReturnError;
        }
    }
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::resumeWCLScan()
{
    if (!scanDeferred || wclStopping)
        return;
    if (joinPending) {
        finishWCLScan(kIOReturnAborted);
        return;
    }
    scanDeferred = scanPending = false;
    IOReturn status = startWCLScan(wclScanRequest);
    if (status != kIOReturnSuccess) {
        scanPending = true;
        finishWCLScan(status);
    }
}

void AirportItlwmSkywalkInterface::finishWCLScan(IOReturn status)
{
    if (!scanPending)
        return;
    scanDeferred = scanPending = false;
    scanCompleted = false;
    scanSource->cancelTimeout();
    fHalService->get80211Controller()->ic_wcl_scan_requested = false;
#ifdef KERNEL
    if (status == kIOReturnSuccess) {
        uint64_t nowNS = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &nowNS);
        AirportWCL::splitScanBurstFor(this).noteScanFinished(nowNS / 1000ULL);
    } else if (status == kIOReturnAborted || status == kIOReturnOffline) {
        AirportWCL::splitScanBurstFor(this).resetBurstOnly();
    }
#endif
    instance->postMessage(this, AirportWCL::ScanDone, &status, sizeof(status), true);
}

void AirportItlwmSkywalkInterface::completeWCLScan()
{
    if (!wclStopping)
        finishWCLScan(scanCompleted ? kIOReturnSuccess : kIOReturnTimeout);
}

void AirportItlwmSkywalkInterface::publishWCLBeacon(const ieee80211_node *node, const uint8_t *ies,
                                                    size_t length, bool isProbe)
{
    if (!node || node->ni_esslen > 32 || !AirportWCL::validIEs(ies, length))
        return;
    ieee80211com *ic = fHalService->get80211Controller();
    if (!node->ni_chan || node->ni_chan == IEEE80211_CHAN_ANYC)
        return;
    unsigned channel = ieee80211_chan2ieee(ic, node->ni_chan);
    if (!channel || channel > 196)
        return;
    const size_t ieCapacity = length + 34;
    const size_t allocSize = sizeof(AirportWCL::BeaconMetadata) + ieCapacity;
    auto metadata = static_cast<AirportWCL::BeaconMetadata *>(IOMalloc(allocSize));
    if (!metadata)
        return;
    *metadata = {};
    const bool hasSSID = !AirportWCL::isHiddenSSID(node->ni_essid, node->ni_esslen);
    metadata->channelSpec = uint16_t(channel | (IEEE80211_IS_CHAN_5GHZ(node->ni_chan) ? 0xd000 : 0x1000));
    metadata->channel = channel;
    if (hasSSID) {
        metadata->ssidLength = node->ni_esslen;
        memcpy(metadata->ssid, node->ni_essid, node->ni_esslen);
    }
    memcpy(metadata->bssid, node->ni_bssid, 6);
    metadata->rssi = IWM_MIN_DBM + node->ni_rssi;
    metadata->interval = node->ni_intval;
    metadata->capability = node->ni_capinfo;
    metadata->flags = AirportWCL::RSSIValid |
        (hasSSID ? AirportWCL::SSIDPresent : 0U) |
        (isProbe ? AirportWCL::ProbeResponse : 0U);
    auto *ieBuffer = reinterpret_cast<uint8_t *>(metadata + 1);
    const size_t splicedLength = AirportWCL::spliceHiddenBeaconSSID(
        ies, length, hasSSID ? node->ni_essid : nullptr,
        hasSSID ? node->ni_esslen : 0, ieBuffer, ieCapacity);
    metadata->ieLength = uint32_t(AirportWCL::selectWPA2TransitionMode(
        ieBuffer, splicedLength));
    size_t messageSize = sizeof(*metadata) + metadata->ieLength;
    if (assocNotified && IEEE80211_ADDR_EQ(node->ni_bssid, joinBSSID)) {
        wclBssValid = messageSize <= sizeof(wclBssInfo);
        if (wclBssValid) {
            bzero(wclBssInfo, sizeof(wclBssInfo));
            memcpy(wclBssInfo, metadata, messageSize);
        }
    }
    if (!scanPending || AirportWCL::matchesScan(wclScanRequest, node->ni_bssid,
            metadata->ssid, metadata->ssidLength, channel))
        instance->postMessage(this, AirportWCL::ScanResult, metadata, messageSize, true);
    IOFree(metadata, allocSize);
}

void AirportItlwmSkywalkInterface::clearWCLPMKSA()
{
    auto ic = fHalService->get80211Controller();
    ieee80211_pmk *pmk;
    while ((pmk = TAILQ_FIRST(&ic->ic_pmksa))) {
        TAILQ_REMOVE(&ic->ic_pmksa, pmk, pmk_next);
        explicit_bzero(pmk, sizeof(*pmk));
        ::free(pmk);
    }
}

IOReturn AirportItlwmSkywalkInterface::setCLEAR_PMKSA_CACHE(void *)
{
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(11));
}

IOReturn AirportItlwmSkywalkInterface::installWCLKey(apple80211_key *key)
{
    static_assert(sizeof(apple80211_key) == 0x94, "WCL cipher key layout");
    if (!key || !AirportWCL::validCipherKey(key->key_cipher_type, key->key_len,
            key->key_flags, key->key_index, key->key_rsc_len))
        return kIOReturnBadArgument;
    auto ic = fHalService->get80211Controller();
    auto ni = ic->ic_bss;
    if (!ic->ic_wcl_enterprise || !ni || ic->ic_state != IEEE80211_S_RUN)
        return kIOReturnNotReady;
    if (key->key_cipher_type == APPLE80211_CIPHER_PMK ||
        key->key_cipher_type == APPLE80211_CIPHER_PMKSA ||
        key->key_cipher_type == APPLE80211_CIPHER_MSK) {
        // WCL's native user-client rejects PTK/GTK, and delivers a PMK/MSK
        // instead. Use the existing net80211 four-way/group-key handshake.
        if (!ieee80211_is_8021x_akm((enum ieee80211_akm)ni->ni_rsnakms))
            return kIOReturnBadArgument;
        uint8_t any = 0;
        for (auto byte : key->key_ea.octet) any |= byte;
        if (any && !IEEE80211_ADDR_EQ(key->key_ea.octet, ni->ni_bssid))
            return kIOReturnBadArgument;
        auto pmk = ieee80211_pmksa_add(ic, (enum ieee80211_akm)ni->ni_rsnakms,
                                     ni->ni_macaddr, key->key, 0);
        IOReturn result = pmk ? kIOReturnSuccess : kIOReturnNoMemory;
        return result;
    }
    return kIOReturnUnsupported;
}

IOReturn AirportItlwmSkywalkInterface::accessWCLRSN(apple80211_rsn_ie_data *data, bool write)
{
    if (!data) return kIOReturnBadArgument;
    auto ic = fHalService->get80211Controller();
    if (write) {
        if (!AirportWCL::validRSNOverride(data->ie, data->len))
            return kIOReturnBadArgument;
        // net80211 constructs negotiated IEs for its own key handshake.
        return kIOReturnUnsupported;

    }
    const uint8_t *ie = ic->ic_bss ? ic->ic_bss->ni_rsnie : nullptr;
    if (!ie) return kIOReturnNotReady;
    size_t length = size_t(ie[1]) + 2;
    if (length > sizeof(data->ie)) return kIOReturnBadArgument;
    bzero(data, sizeof(*data));
    data->version = APPLE80211_VERSION;
    data->len = length;
    memcpy(data->ie, ie, length);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::beginWCLJoin(apple80211AssocCandidates *request)
{
    AirportWCL::JoinRequest join;
    if (!AirportWCL::decodeJoin(request, 0x2cc, join)) {
        return kIOReturnBadArgument;
    }
    return startWCLJoin(join);
}

IOReturn AirportItlwmSkywalkInterface::startWCLJoin(AirportWCL::JoinRequest &join)
{
    auto auth = AirportWCL::classifyLegacyAuth(join);
    bool personal = auth == AirportWCL::LegacyAuth::Personal;
    bool enterprise = auth == AirportWCL::LegacyAuth::Enterprise;
    bool supported = auth != AirportWCL::LegacyAuth::Invalid;
    ieee80211com *ic = fHalService->get80211Controller();
    // Applying a private MAC restarts firmware asynchronously. WCL can issue
    // the association before that restart has completed; keep one request
    // until the HAL reports READY rather than making the user retry it.
    if (supported && !joinPending && ic->ic_wcl_mac_reconfig &&
        (ic->ic_if.if_flags & IFF_UP)) {
        deferredJoin = join;
        memcpy(joinBSSID, join.bssid, sizeof(joinBSSID));
        explicit_bzero(&join, sizeof(join));
        joinDeferred = joinPending = true;
        joinTimer->setTimeoutMS(30000);
        return kIOReturnSuccess;
    }
    if (!supported || !(ic->ic_if.if_flags & IFF_RUNNING) || joinPending || scanPending) {
        explicit_bzero(&join, sizeof(join));
        return !supported ? kIOReturnUnsupported : kIOReturnBusy;
    }
    const bool leftActiveBss = ic->ic_state == IEEE80211_S_RUN ||
        ic->ic_state == IEEE80211_S_AUTH || ic->ic_state == IEEE80211_S_ASSOC;
    if (leftActiveBss)
        leaveWCLNetwork();
    ieee80211_disable_rsn(ic);
    ieee80211_disable_wep(ic);
    explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    clearWCLPMKSA();
    ic->ic_flags &= ~(IEEE80211_F_AUTO_JOIN | IEEE80211_F_PSK);
    ic->ic_wcl_enterprise = enterprise;

    if (personal || enterprise) {
        ieee80211_wpaparams wpa = {};
        wpa.i_enabled = 1;
        if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA_PSK))
            wpa.i_protos |= IEEE80211_WPA_PROTO_WPA1;
        if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA2 | APPLE80211_AUTHTYPE_WPA2_PSK |
                              APPLE80211_AUTHTYPE_SHA256_PSK | APPLE80211_AUTHTYPE_SHA256_8021X))
            wpa.i_protos |= IEEE80211_WPA_PROTO_WPA2;
        if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA_PSK | APPLE80211_AUTHTYPE_WPA2_PSK))
            wpa.i_akms |= IEEE80211_WPA_AKM_PSK;
        if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA2))
            wpa.i_akms |= IEEE80211_WPA_AKM_8021X;
        if (join.upperAuth & APPLE80211_AUTHTYPE_SHA256_PSK)
            wpa.i_akms |= IEEE80211_WPA_AKM_SHA256_PSK;
        if (join.upperAuth & APPLE80211_AUTHTYPE_SHA256_8021X)
            wpa.i_akms |= IEEE80211_WPA_AKM_SHA256_8021X;
        if (personal && (join.upperAuth & (APPLE80211_AUTHTYPE_WPA3_SAE | APPLE80211_AUTHTYPE_WPA3_FT_SAE))) {
            wpa.i_protos |= IEEE80211_WPA_PROTO_WPA2;
            wpa.i_akms |= IEEE80211_WPA_AKM_PSK;
        }
        wpa.i_ciphers = IEEE80211_WPA_CIPHER_CCMP | IEEE80211_WPA_CIPHER_TKIP;
        if (join.rsnLength >= 8 && join.rsn[0] == IEEE80211_ELEMID_RSN &&
            join.rsn[4] == 0 && join.rsn[5] == 0x0f && join.rsn[6] == 0xac) {
            if (join.rsn[7] == 2) wpa.i_groupcipher = IEEE80211_WPA_CIPHER_TKIP;
            if (join.rsn[7] == 4) wpa.i_groupcipher = IEEE80211_WPA_CIPHER_CCMP;
        }
        int error = ieee80211_ioctl_setwpaparms(ic, &wpa);
        if (error && error != ENETRESET) {
            explicit_bzero(&join, sizeof(join));
            return kIOReturnBadArgument;
        }
        if (personal) {
            memcpy(ic->ic_psk, join.key, sizeof(ic->ic_psk));
            ic->ic_flags |= IEEE80211_F_PSK;
        } else if (join.keyLength == IEEE80211_PMK_LEN) {
            bool cached = true;
            if (join.upperAuth & (APPLE80211_AUTHTYPE_WPA | APPLE80211_AUTHTYPE_WPA2))
                cached = ieee80211_pmksa_add(ic, IEEE80211_AKM_8021X, join.bssid, join.key, 0) != nullptr;
            if (cached && (join.upperAuth & APPLE80211_AUTHTYPE_SHA256_8021X))
                cached = ieee80211_pmksa_add(ic, IEEE80211_AKM_SHA256_8021X, join.bssid, join.key, 0) != nullptr;
            if (!cached) {
                clearWCLPMKSA();
                explicit_bzero(&join, sizeof(join));
                return kIOReturnNoMemory;
            }
        }
    } else if (auth == AirportWCL::LegacyAuth::Wep) {
        ieee80211_nwkey keys = {};
        keys.i_wepon = IEEE80211_NWKEY_WEP;
        keys.i_defkid = join.keyIndex + 1;
        keys.i_key[join.keyIndex].i_keylen = join.keyLength;
        keys.i_key[join.keyIndex].i_keydat = join.key;
        int error = ieee80211_ioctl_setnwkeys(ic, &keys);
        if (error && error != ENETRESET) {
            explicit_bzero(&join, sizeof(join));
            return kIOReturnBadArgument;
        }
    }
    ic->ic_des_esslen = join.ssidLength;
    memcpy(ic->ic_des_essid, join.ssid, sizeof(ic->ic_des_essid));
    memcpy(ic->ic_des_bssid, join.bssid, 6);
    memcpy(joinBSSID, join.bssid, 6);
    ic->ic_flags |= IEEE80211_F_DESBSSID;
    ic->ic_wcl_join_requested = true;
    unsigned channel = AirportWCL::joinChannelHint(join.channelSpec);
    ic->ic_wcl_join_channel = channel && ic->ic_channels[channel].ic_flags &&
        isset(ic->ic_chan_active, channel) ? channel : 0;
    ic->ic_wcl_scan_requested = false;
    current_authtype_lower = join.lowerAuth;
    current_authtype_upper = join.upperAuth;
    explicit_bzero(&join, sizeof(join));
    wclBssValid = false;
    joinPending = true;
    assocNotified = firstBeaconNotified = portAuthorized = false;
    wclAssociatedBeacons = 0;
    bzero(wclBeaconInfo, sizeof(wclBeaconInfo));
    disassocIsVoluntary = false;
    joinTimer->setTimeoutMS(30000);
    const bool restartedAbortedScan = AirportWCL::consumeAbortedScanInScanState(this);
    if (!leftActiveBss && (ic->ic_state != IEEE80211_S_SCAN || restartedAbortedScan) &&
        ic->ic_newstate && ic->ic_newstate(ic, IEEE80211_S_SCAN, -1)) {
        joinPending = false;
        joinTimer->cancelTimeout();
        ic->ic_wcl_join_requested = false;
        ic->ic_wcl_join_channel = 0;
        clearWCLPMKSA();
        ic->ic_wcl_enterprise = false;
        explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
        return kIOReturnError;
    }
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::resumeWCLJoin()
{
    if (!joinDeferred || wclStopping)
        return;
    if (scanDeferred)
        finishWCLScan(kIOReturnAborted);
    AirportWCL::JoinRequest join = deferredJoin;
    explicit_bzero(&deferredJoin, sizeof(deferredJoin));
    joinDeferred = joinPending = false;
    joinTimer->cancelTimeout();
    IOReturn status = startWCLJoin(join);
    explicit_bzero(&join, sizeof(join));
    if (status != kIOReturnSuccess) {
        joinPending = true;
        finishWCLJoin(1, 0, status);
    }
}

void AirportItlwmSkywalkInterface::finishWCLJoin(uint16_t status, uint16_t reason, IOReturn driverStatus)
{
    if (!joinPending)
        return;
    AirportWCL::ConnectionEvent event = {};
    event.status = status;
    event.reason = reason;
    memcpy(event.peers[0].bssid, joinBSSID, 6);
    event.peers[0].driverStatus = driverStatus;
    joinPending = false;
    joinDeferred = false;
    explicit_bzero(&deferredJoin, sizeof(deferredJoin));
    joinTimer->cancelTimeout();
    instance->postMessage(this, AirportWCL::ConnectComplete, &event, sizeof(event), true);
    if (!status) {
#ifdef KERNEL
        if (instance && instance->awdlInterface)
            instance->awdlInterface->notifyInfrastructureReady();
#endif
    } else {
        portAuthorized = false;
        wclRoamLock = false;
        wclRealTimeMode = false;
#ifdef KERNEL
        wclLastReportedRateBps = 0;
#endif
        updateWCLLinkQuality(true);
        ieee80211com *ic = fHalService->get80211Controller();
        ic->ic_wcl_join_requested = false;
        ic->ic_wcl_join_channel = 0;
        clearWCLPMKSA();
        ic->ic_wcl_enterprise = false;
        explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    }
}

void AirportItlwmSkywalkInterface::wclJoinTimeout(OSObject *owner, IOTimerEventSource *)
{
    auto self = OSDynamicCast(AirportItlwmSkywalkInterface, owner);
    if (self && !self->wclStopping) {
        self->finishWCLJoin(1, 1004, kIOReturnTimeout);
        self->leaveWCLNetwork();
    }
}

IOReturn AirportItlwmSkywalkInterface::leaveWCLNetwork()
{
    ieee80211com *ic = fHalService->get80211Controller();
    portAuthorized = false;
    wclRoamLock = false;
    wclRealTimeMode = false;
#ifdef KERNEL
    wclLastLiveBgscanUS = 0;
    AirportWCL::resetSplitScanBurst(this);
#endif
    updateWCLLinkQuality(true);
    joinPending = false;
    joinDeferred = false;
    explicit_bzero(&deferredJoin, sizeof(deferredJoin));
    joinTimer->cancelTimeout();
    ic->ic_wcl_join_requested = false;
    ic->ic_wcl_join_channel = 0;
    ic->ic_wcl_scan_requested = false;
    if (ic->ic_flags & (IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN)) {
        ic->ic_flags &= ~(IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN);
#ifdef KERNEL
        AirportWCL::abortDriverScan(fHalService);
#endif
    }
    wclAssociatedBeacons = 0;
    wclBssValid = false;
    disassocIsVoluntary = true;
    ic->ic_deauth_reason = IEEE80211_REASON_AUTH_LEAVE;
    if (ic->ic_state > IEEE80211_S_SCAN)
        ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
    ieee80211_disable_rsn(ic);
    ieee80211_disable_wep(ic);
    explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    clearWCLPMKSA();
    ic->ic_wcl_enterprise = false;
    ic->ic_flags &= ~(IEEE80211_F_PSK | IEEE80211_F_DESBSSID);
    ic->ic_des_esslen = 0;
    bzero(ic->ic_des_essid, sizeof(ic->ic_des_essid));
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::abortWCLJoin()
{
    leaveWCLNetwork();
    instance->postMessage(this, AirportWCL::JoinAbortComplete, nullptr, 0, true);
    return kIOReturnSuccess;
}

void AirportItlwmSkywalkInterface::handleWCLLink(bool up)
{
    if (wclStopping)
        return;
    portAuthorized = up;
    if (!up && wclLinkIndicated) {
        AirportWCL::LinkStatusEvent event = {};
        memcpy(event.bssid, joinBSSID, sizeof(event.bssid));
        event.reason = 255;
        wclLinkIndicated = false;
        wclBssValid = false;
        instance->postMessage(this, AirportWCL::LinkStatus, &event, sizeof(event), true);
    }
    if (up && joinPending && firstBeaconNotified)
        finishWCLJoin(0, 0, kIOReturnSuccess);
    updateWCLLinkQuality(true);
#ifdef KERNEL
    if (!up) {
        wclLastLiveBgscanUS = 0;
        AirportWCL::resetSplitScanBurst(this);
    }
#if __IO80211_TARGET == __MAC_15_2
    if (up)
        syncInfraDataBandwidths();
#endif
#endif
}

void AirportItlwmSkywalkInterface::handleWCLAvailability(bool ready)
{
    ieee80211com *ic = fHalService->get80211Controller();
    if (!ready) {
        if (ic->ic_wcl_mac_reconfig && scanPending) {
            scanDeferred = true;
            scanCompleted = false;
        } else
            finishWCLScan(kIOReturnOffline);
        if (!ic->ic_wcl_mac_reconfig || !joinDeferred)
            finishWCLJoin(1, 0, kIOReturnOffline);
        portAuthorized = false;
        wclRoamLock = false;
        wclRealTimeMode = false;
#ifdef KERNEL
        wclLastReportedRateBps = 0;
        wclLastLiveBgscanUS = 0;
        AirportWCL::resetSplitScanBurst(this);
#endif
        updateWCLLinkQuality(true);
        ic->ic_wcl_join_requested = false;
        ic->ic_wcl_join_channel = 0;
        clearWCLPMKSA();
        ic->ic_wcl_enterprise = false;
        explicit_bzero(ic->ic_psk, sizeof(ic->ic_psk));
    }
    // An address application is requested by WCL itself, not a new
    // adapter reset. Replaying firmware-load events here rotates the
    // scan MAC again and recursively restarts the card.
    if (AirportWCL::consumeMacReconfiguration(ic->ic_wcl_mac_reconfig, ready)) {
        if (ready) {
            resumeWCLJoin();
            resumeWCLScan();
        }
        return;
    }
    if (ready) {
        uint32_t reset = 1; // Native resetInterfacesComplete, only after HAL success.
        instance->postMessage(this, AirportWCL::ResetInterface, &reset, sizeof(reset), true);
    }
    AirportWCL::DriverAvailability event = {};
    event.version = 3;
    event.available = ready;
    event.reason = ready ? 0xe0821803 : 0xe0821804;
    instance->postMessage(this, AirportWCL::DriverAvailable, &event, sizeof(event), true);
    if (ready) {
        resumeWCLJoin();
        resumeWCLScan();
    }
}

void AirportItlwmSkywalkInterface::handleWCLEvent(int code, void *data)
{
    if (wclStopping)
        return;
    // Firmware initialization runs on systq, outside the native PostOffice gate.
    // Serialize only lifecycle notifications; never run synchronous WCL ioctls here.
    if ((code == IEEE80211_EVT_DRIVER_RESET_BEGIN || code == IEEE80211_EVT_DRIVER_READY ||
         code == IEEE80211_EVT_DRIVER_STOPPED) && !instance->getWorkQueue()->inGate()) {
        _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(12),
                                reinterpret_cast<void *>(uintptr_t(code)));
        return;
    }
    ieee80211com *ic = fHalService->get80211Controller();
    switch (code) {
        case IEEE80211_EVT_COUNTRY_CODE_UPDATE:
            deferWCLCountryUpdate();
            break;
        case IEEE80211_EVT_DRIVER_RESET_BEGIN: {
            ++wclCountryGeneration;
            portAuthorized = false;
            updateWCLLinkQuality(true);
            if (ic->ic_wcl_mac_reconfig) {
                break;
            }
            uint32_t reset = 0; // Native resetInterfacesBegin.
            instance->postMessage(this, AirportWCL::ResetInterface, &reset, sizeof(reset), true);
            break;
        }
        case IEEE80211_EVT_DRIVER_READY:
        case IEEE80211_EVT_DRIVER_STOPPED: {
            if (code == IEEE80211_EVT_DRIVER_STOPPED) ++wclCountryGeneration;
            handleWCLAvailability(code == IEEE80211_EVT_DRIVER_READY);
            break;
        }
        case IEEE80211_EVT_BEACON: {
            auto beacon = static_cast<ieee80211_beacon_event *>(data);
            if (!beacon)
                break;
            // Retain the actual associated beacon, including its advancing TSF.
            // WCL polls BEACON_INFO to distinguish fresh reception from a stale BSS.
            if (assocNotified && (joinPending || portAuthorized) &&
                IEEE80211_ADDR_EQ(beacon->node->ni_bssid, joinBSSID) &&
                beacon->frame && beacon->frame_length >= 36 &&
                beacon->frame_length <= 0x800 &&
                (beacon->frame[0] & 0xfc) == IEEE80211_FC0_SUBTYPE_BEACON) {
                uint32_t length = uint32_t(beacon->frame_length);
                bzero(wclBeaconInfo, sizeof(wclBeaconInfo));
                memcpy(wclBeaconInfo, &length, sizeof(length));
                memcpy(wclBeaconInfo + 4, beacon->frame, length);
                ++wclAssociatedBeacons;
                ++wclLqmBeacons;
                // WCL has no legacy LQMData instance at createLQMData().
                // Send the snapshot consumed by WCL instead of legacy 227/229.
                if (portAuthorized && (wclLqmBeacons % 8 == 0)) {
#if __IO80211_TARGET == __MAC_15_2
                    apple80211_rssi_data rssi = {};
                    if (getRSSI(&rssi) == kIOReturnSuccess) {
                        AirportWCL::SequoiaLqmUpdate update = {};
                        update.rssiValid = 1;
                        update.rssi = rssi.aggregate_rssi;
                        instance->postMessage(this, AirportWCL::LqmUpdate,
                            &update, sizeof(update), true);
                    }
#else
                    AirportWCL::LqmBeaconUpdate update;
                    if (AirportWCL::makeBeaconUpdate(
                            wclLqmBeacons - wclLqmReportedBeacons, update)) {
                        instance->postMessage(this, AirportWCL::LqmUpdate,
                            &update, sizeof(update), true);
                        wclLqmReportedBeacons = wclLqmBeacons;
                    }
#endif
                    updateWCLLinkQuality();
#ifdef KERNEL
                    {
                        uint64_t nowNS = 0;
                        absolutetime_to_nanoseconds(mach_absolute_time(), &nowNS);
                        AirportWCL::splitScanBurstFor(this).traffic.sample(
                            AirportWCL::sampleInfraTotalBytes(ic->ic_if, instance),
                            nowNS / 1000ULL);
                    }
                    if (ic->ic_bss && instance && instance->bsdInterface) {
                        const uint64_t rateBps = ic->ic_bss->ni_txrate_kbps ?
                            uint64_t(ic->ic_bss->ni_txrate_kbps) * 1000ULL : 866700000ULL;
                        if (rateBps != wclLastReportedRateBps) {
                            wclLastReportedRateBps = rateBps;
                            if (ifnet_t ifp = instance->bsdInterface->getIfnet())
                                ifnet_set_baudrate(ifp, rateBps);
                            instance->IONetworkController::setLinkStatus(
                                kIONetworkLinkValid | kIONetworkLinkActive,
                                instance->getCurrentMedium(), rateBps);
                        }
#if __IO80211_TARGET == __MAC_15_2
                        syncInfraDataBandwidths();
#endif
                    }
#endif
                }
            }
            if (scanPending || joinPending || ic->ic_state == IEEE80211_S_SCAN ||
                (portAuthorized && IEEE80211_ADDR_EQ(beacon->node->ni_bssid, joinBSSID)))
                publishWCLBeacon(beacon->node, beacon->ies, beacon->length, beacon->isprobe != 0);
            if (joinPending && assocNotified && !firstBeaconNotified &&
                IEEE80211_ADDR_EQ(beacon->node->ni_bssid, joinBSSID)) {
                AirportWCL::FirstBeaconEvent event = {};
                memcpy(event.peer.bssid, joinBSSID, 6);
                if (!wclBssValid)
                    break;
                firstBeaconNotified = true;
                AirportWCL::LinkStatusEvent link = {};
                memcpy(link.bssid, joinBSSID, sizeof(link.bssid));
                link.up = 1;
                link.reason = 255;
                wclLinkIndicated = true;
                instance->postMessage(this, AirportWCL::LinkStatus, &link, sizeof(link), true);
                instance->postMessage(this, AirportWCL::FirstBeacon, &event, sizeof(event), true);
                if (portAuthorized)
                    finishWCLJoin(0, 0, kIOReturnSuccess);
            }
            break;
        }
        case IEEE80211_EVT_SCAN_DONE:
            if (scanPending && !scanDeferred) {
                if (scanSkipCompletion)
                    scanSkipCompletion = false;
                else {
                    scanCompleted = true;
                    scanSource->setTimeoutMS(0);
                }
            }
#ifdef KERNEL
            if (ic->ic_state == IEEE80211_S_RUN && instance && instance->awdlInterface)
                instance->awdlInterface->notifyInfrastructureReady();
#endif
            break;
        case IEEE80211_EVT_STA_ASSOC_DONE:
            if (joinPending && !assocNotified) {
                AirportWCL::AssociationEvent event = {};
                event.status = ic->ic_assoc_status;
                memcpy(event.bssid, joinBSSID, 6);
                event.driverStatus = event.status ? kIOReturnError : kIOReturnSuccess;
                assocNotified = !event.status;
                instance->postMessage(this, AirportWCL::AssocComplete, &event, sizeof(event), true);
                if (event.status)
                    finishWCLJoin(event.status, 0, kIOReturnError);
            }
            break;
        case IEEE80211_EVT_STA_DEAUTH: {
            portAuthorized = false;
#ifdef KERNEL
            wclLastLiveBgscanUS = 0;
            AirportWCL::resetSplitScanBurst(this);
#endif
            updateWCLLinkQuality(true);
            AirportWCL::DeauthEvent event = {};
            event.reason = ic->ic_deauth_reason;
            if (ic->ic_bss)
                memcpy(event.bssid, ic->ic_bss->ni_bssid, 6);
            instance->postMessage(this, AirportWCL::Deauth, &event, sizeof(event), true);
            finishWCLJoin(1, event.reason, kIOReturnOffline);
            break;
        }
        default: break;
    }
}

IOReturn AirportItlwmSkywalkInterface::getWCL_VALID_CHANNEL_COUNT(unsigned long *count)
{
    if (!count)
        return kIOReturnBadArgument;
    *count = 0;
    ieee80211com *ic = fHalService->get80211Controller();
    for (unsigned i = 1; i < IEEE80211_CHAN_MAX; ++i)
        if (isset(ic->ic_chan_active, i))
            ++*count;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_LINK_STATE_UPDATE(apple80211_wcl_update_link_state *data)
{
    if (!data)
        return kIOReturnBadArgument;
#if __IO80211_TARGET == __MAC_15_2
    const auto bytes = reinterpret_cast<const uint8_t *>(data);
    if (bytes[6]) {
        if (bytes[8])
            setCurrentApAddress(reinterpret_cast<ether_addr *>(data));
        if (bytes[7])
            setLinkState(kIO80211NetworkLinkUp, 0, false, 0);
    } else {
        uint32_t debounce = AirportWCL::read32(bytes + 12);
        setCurrentApAddress(nullptr);
        setLinkState(kIO80211NetworkLinkDown, 0, debounce != 0, debounce);
    }
    IOReturn status = kIOReturnSuccess;
#else
    IOReturn status = IO80211InfraInterface::setWCL_LINK_STATE_UPDATE(data);
#endif
    if (status == kIOReturnSuccess) {
        updateWCLLinkQuality(true);
#if defined(KERNEL) && __IO80211_TARGET == __MAC_15_2
        if (bytes[6] && bytes[7])
            syncInfraDataBandwidths();
#endif
    }
    return status;
}

#if __IO80211_TARGET == __MAC_15_2
void AirportItlwmSkywalkInterface::syncInfraDataBandwidths()
{
#ifdef KERNEL
    if (!fHalService || !instance)
        return;
    ieee80211com *ic = fHalService->get80211Controller();
    if (!ic || !ic->ic_bss || ic->ic_state != IEEE80211_S_RUN)
        return;
    const auto *ni = ic->ic_bss;
    unsigned widthCode = 0;
    switch (ni->ni_chw) {
        case IEEE80211_CHAN_WIDTH_40:
            widthCode = 1;
            break;
        case IEEE80211_CHAN_WIDTH_80:
            widthCode = 2;
            break;
        case IEEE80211_CHAN_WIDTH_80P80:
        case IEEE80211_CHAN_WIDTH_160:
            widthCode = 3;
            break;
        default:
            widthCode = 0;
            break;
    }
    const bool isHE = (ni->ni_flags & IEEE80211_NODE_HE) != 0;
    const bool isVHT = (ni->ni_flags & IEEE80211_NODE_VHT) != 0 || ic->ic_curmode == IEEE80211_MODE_11AC;
    const bool isHT = (ni->ni_flags & IEEE80211_NODE_HT) != 0 || ic->ic_curmode >= IEEE80211_MODE_11N;
    const unsigned nss = fHalService->getDriverInfo() ?
        unsigned(MAX(1, fHalService->getDriverInfo()->getTxNSS())) : 2U;
    const uint32_t txKbps = ni->ni_txrate_kbps ? ni->ni_txrate_kbps :
        (isHE && ni->ni_he_txrate_kbps ? ni->ni_he_txrate_kbps : 0U);
    const auto bw = AirportWCL::computeInfraBandwidths(txKbps, widthCode, isHE, isVHT, isHT, nss);
    if (instance->bsdInterface) {
        if (ifnet_t ifp = instance->bsdInterface->getIfnet())
            ifnet_set_baudrate(ifp, bw.maxBps);
    }
    const uint32_t max32 = bw.maxBps > 0xffffffffULL ? 0xffffffffU : uint32_t(bw.maxBps);
    const uint32_t eff32 = bw.effectiveBps > 0xffffffffULL ? 0xffffffffU : uint32_t(bw.effectiveBps);
    uint8_t ls[0x78] = {};
    const uint32_t ver = 1U;
    const uint32_t len = 0x70U;
    const uint32_t validMask = 0x00000607U;
    const int32_t lqm = 100;
    memcpy(ls + 0x00, &ver, sizeof(ver));
    memcpy(ls + 0x04, &len, sizeof(len));
    memcpy(ls + 0x08, &validMask, sizeof(validMask));
    memcpy(ls + 0x0c, &lqm, sizeof(lqm));
    memcpy(ls + 0x10, &eff32, sizeof(eff32));
    memcpy(ls + 0x14, &max32, sizeof(max32));
    memcpy(ls + 0x30, &eff32, sizeof(eff32));
    memcpy(ls + 0x34, &max32, sizeof(max32));
    IOSkywalkNetworkInterface::reportDetailedLinkStatus(
        reinterpret_cast<const if_link_status *>(ls));
    IOSkywalkNetworkInterface::reportDataBandwidths(
        bw.maxBps, bw.maxBps, bw.effectiveBps, bw.effectiveBps);
#endif
}

IOReturn AirportItlwmSkywalkInterface::reportDetailedLinkStatus(const if_link_status *status)
{
#ifdef KERNEL
    if (status && fHalService && instance) {
        ieee80211com *ic = fHalService->get80211Controller();
        if (ic && ic->ic_bss && ic->ic_state == IEEE80211_S_RUN) {
            const auto *ni = ic->ic_bss;
            unsigned widthCode = 0;
            switch (ni->ni_chw) {
                case IEEE80211_CHAN_WIDTH_40: widthCode = 1; break;
                case IEEE80211_CHAN_WIDTH_80: widthCode = 2; break;
                case IEEE80211_CHAN_WIDTH_80P80:
                case IEEE80211_CHAN_WIDTH_160: widthCode = 3; break;
                default: widthCode = 0; break;
            }
            const bool isHE = (ni->ni_flags & IEEE80211_NODE_HE) != 0;
            const bool isVHT = (ni->ni_flags & IEEE80211_NODE_VHT) != 0 || ic->ic_curmode == IEEE80211_MODE_11AC;
            const bool isHT = (ni->ni_flags & IEEE80211_NODE_HT) != 0 || ic->ic_curmode >= IEEE80211_MODE_11N;
            const unsigned nss = fHalService->getDriverInfo() ?
                unsigned(MAX(1, fHalService->getDriverInfo()->getTxNSS())) : 2U;
            const uint32_t txKbps = ni->ni_txrate_kbps ? ni->ni_txrate_kbps :
                (isHE && ni->ni_he_txrate_kbps ? ni->ni_he_txrate_kbps : 0U);
            const auto bw = AirportWCL::computeInfraBandwidths(txKbps, widthCode, isHE, isVHT, isHT, nss);
            const auto *raw = reinterpret_cast<const uint8_t *>(status);
            const uint32_t ver = AirportWCL::read32(raw);
            const uint32_t len = AirportWCL::read32(raw + 4);
            if (ver == 1 && len >= 0x38 && len <= 0x100) {
                uint8_t patched[8 + 0x100] = {};
                memcpy(patched, raw, 8 + len);
                const uint32_t max32 = bw.maxBps > 0xffffffffULL ? 0xffffffffU : uint32_t(bw.maxBps);
                const uint32_t eff32 = bw.effectiveBps > 0xffffffffULL ? 0xffffffffU : uint32_t(bw.effectiveBps);
                uint32_t validMask = AirportWCL::read32(patched + 0x08);
                validMask |= 0x00000606U; // UL_EFF(0x2) | UL_MAX(0x4) | DL_EFF(0x200) | DL_MAX(0x400)
                memcpy(patched + 0x08, &validMask, sizeof(validMask));
                memcpy(patched + 0x10, &eff32, sizeof(eff32));
                memcpy(patched + 0x14, &max32, sizeof(max32));
                memcpy(patched + 0x30, &eff32, sizeof(eff32));
                memcpy(patched + 0x34, &max32, sizeof(max32));
                const IOReturn ret = IOSkywalkNetworkInterface::reportDetailedLinkStatus(
                    reinterpret_cast<const if_link_status *>(patched));
                IOSkywalkNetworkInterface::reportDataBandwidths(
                    bw.maxBps, bw.maxBps, bw.effectiveBps, bw.effectiveBps);
                return ret;
            }
            const IOReturn ret = IOSkywalkNetworkInterface::reportDetailedLinkStatus(status);
            IOSkywalkNetworkInterface::reportDataBandwidths(
                bw.maxBps, bw.maxBps, bw.effectiveBps, bw.effectiveBps);
            return ret;
        }
    }
    return IOSkywalkNetworkInterface::reportDetailedLinkStatus(status);
#else
    (void)status;
    return kIOReturnSuccess;
#endif
}
#endif

IOReturn AirportItlwmSkywalkInterface::getWCL_BSS_INFO(apple80211_beacon_msg *data)
{
    if (!data)
        return kIOReturnBadArgument;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(5), data);
}

IOReturn AirportItlwmSkywalkInterface::getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *data)
{
    if (!data)
        return kIOReturnBadArgument;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(6), data);
}

IOReturn AirportItlwmSkywalkInterface::copyWCLExtendedBss(void *data)
{
    ieee80211com *ic = fHalService->get80211Controller();
    if (!data || !ic->ic_bss || ic->ic_state != IEEE80211_S_RUN)
        return kIOReturnNotReady;
    static_assert(sizeof(apple80211_rate_set_data) == 0xbc, "Extended BSS rates size");
    static_assert(sizeof(apple80211_mcs_index_set_data) == 0x10, "Extended BSS MCS size");
    auto bytes = static_cast<uint8_t *>(data);
#if __IO80211_TARGET == __MAC_15_2
    bzero(bytes, 0x1e0);
#else
    bzero(bytes, 0x214);
#endif
    IOReturn status = getRATE_SET(reinterpret_cast<apple80211_rate_set_data *>(bytes));
    if (status)
        return status;
    status = getMCS_INDEX_SET(reinterpret_cast<apple80211_mcs_index_set_data *>(bytes + 0xbc));
    if (status)
        return status;
    apple80211_vht_mcs_index_set_data vht = {};
    vht.version = APPLE80211_VERSION;
    vht.mcs_map = 0xffff;
    if (ic->ic_bss->ni_flags & IEEE80211_NODE_VHT)
        vht.mcs_map = ic->ic_bss->ni_vht_mcsinfo.tx_mcs_map;
    memcpy(bytes + 0xcc, &vht, sizeof(vht));
    // Native updateMCSSet uses version + a 16-bit HE MCS/NSS map (8 bytes).
    uint32_t heVersion = APPLE80211_VERSION;
    uint16_t heMap = 0xffff;
    if (ic->ic_bss->ni_flags & IEEE80211_NODE_HE) {
        bool wide = ic->ic_bss->ni_chw == IEEE80211_CHAN_WIDTH_160;
        heMap = htole16(ieee80211_he_mcs_intersection(
            le16toh(wide ? ic->ic_he_mcs_nss_supp.rx_mcs_160 : ic->ic_he_mcs_nss_supp.rx_mcs_80),
            le16toh(wide ? ic->ic_bss->ni_he_mcs_nss_supp.tx_mcs_160 : ic->ic_bss->ni_he_mcs_nss_supp.tx_mcs_80)));
    }
    memcpy(bytes + 0xd4, &heVersion, sizeof(heVersion));
    memcpy(bytes + 0xd8, &heMap, sizeof(heMap));
    if (ic->ic_flags & IEEE80211_F_RSNON)
#if __IO80211_TARGET == __MAC_15_2
        ieee80211_add_rsn(bytes + 0xdd, ic, ic->ic_bss);
#else
        ieee80211_add_rsn(bytes + 0x113, ic, ic->ic_bss);
#endif
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_UPDATE_FAST_LANE(apple80211_fastlane *data)
{
    if (!data)
        return kIOReturnBadArgument;
    const auto bytes = reinterpret_cast<const uint8_t *>(data);
    return (bytes[0] || bytes[1]) ? kIOReturnUnsupported : kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info *data)
{
    if (!data)
        return kIOReturnBadArgument;
    // No low-latency or P2P session is implemented by this adapter.
    bzero(data, 4);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::getWCL_CHANNELS_INFO(apple80211ChannelInfo *data)
{
    if (!data)
        return kIOReturnBadArgument;
    auto info = reinterpret_cast<AirportWCL::ChannelInfo *>(data);
    bzero(info, sizeof(*info));
    ieee80211com *ic = fHalService->get80211Controller();
    for (unsigned i = 1; i < IEEE80211_CHAN_MAX && info->count < 400; ++i) {
        if (!isset(ic->ic_chan_active, i))
            continue;
        const ieee80211_channel *channel = &ic->ic_channels[i];
        auto &entry = info->channels[info->count++];
        entry.channel = i;
        entry.channelSpec = uint16_t(i | (IEEE80211_IS_CHAN_5GHZ(channel) ? 0xd000 : 0x1000));
        if (channel->ic_flags & IEEE80211_CHAN_DFS)
            entry.flags |= 2;
        if (channel->ic_flags & IEEE80211_CHAN_PASSIVE)
            entry.flags |= 4;
    }
    apple80211_country_code_data country = {};
    if (instance->getCOUNTRY_CODE(static_cast<OSObject *>(this), &country) == kIOReturnSuccess)
        memcpy(info->country, country.cc, 3);
    if (info->count) {
        ++wclChannelReadSerial;
        if (wclCountryReloadPending == wclCountryGeneration && wclCountryReloadPending)
            deferWCLCountryUpdate(true);
    }
    return info->count ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn AirportItlwmSkywalkInterface::getCHANNELS_INFO(apple80211_channels_info *data)
{
    if (!data)
        return kIOReturnBadArgument;
    if (!_fCommandGate)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(13), data);
}

IOReturn AirportItlwmSkywalkInterface::getCOUNTRY_CHANNELS_INFO(apple80211_channels_info *data)
{
    return getCHANNELS_INFO(data);
}

IOReturn AirportItlwmSkywalkInterface::copyWCLChannels(void *data)
{
    auto info = static_cast<AirportWCL::NativeChannelInfo *>(data);
    bzero(info, sizeof(*info));
    ieee80211com *ic = fHalService->get80211Controller();
#if __IO80211_TARGET == __MAC_15_2
    auto radio = OSDynamicCast(ItlIwx, fHalService);
#endif
    for (unsigned i = 1; i <= IEEE80211_CHAN_MAX; ++i) {
        const ieee80211_channel *channel = &ic->ic_channels[i];
        if (!isset(ic->ic_chan_active, i) || !channel->ic_freq)
            continue;
        bool supports80 = IEEE80211_IS_CHAN_VHT80(channel);
#if __IO80211_TARGET == __MAC_15_2
        if (radio && radio->awdlChannelAllowed(i))
            supports80 = radio->awdlChannelAllowed(i, 80);
#endif
        AirportWCL::appendChannel(*info, i, IEEE80211_IS_CHAN_5GHZ(channel),
            channel->ic_flags & IEEE80211_CHAN_DFS,
            channel->ic_flags & IEEE80211_CHAN_PASSIVE,
            IEEE80211_IS_CHAN_HT40(channel) || IEEE80211_IS_CHAN_VHT40(channel),
            supports80);
#if __IO80211_TARGET == __MAC_15_2
        // Keep STA channels visible while excluding them from AWDL's transmit plan.
        if (radio && !radio->awdlChannelAllowed(i))
            info->channelBitmap[info->count - 1] |= 0x100;
#endif
    }
    apple80211_country_code_data country = {};
    if (instance->getCOUNTRY_CODE(static_cast<OSObject *>(this), &country) == kIOReturnSuccess)
        memcpy(info->country, country.cc, sizeof(country.cc));
    return info->count ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn AirportItlwmSkywalkInterface::getWCL_TRAFFIC_COUNTERS(apple80211_wcl_traffic_counters *data)
{
    if (!data)
        return kIOReturnBadArgument;
    if (!_fCommandGate)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(14), data);
}

IOReturn AirportItlwmSkywalkInterface::copyWCLTrafficCounters(void *data)
{
    ieee80211com *ic = fHalService->get80211Controller();
    uint64_t timestamp;
    absolutetime_to_nanoseconds(mach_absolute_time(), &timestamp);
    const auto counters = AirportWCL::makeTrafficCounters(
        ic->ic_wcl_tx_packets, ic->ic_wcl_rx_packets, timestamp);
    memcpy(data, &counters, sizeof(counters));
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_SET_SCAN_HOME_AWAY_TIME(scanHomeAndAwayTime *data)
{
    if (!data)
        return kIOReturnBadArgument;
    if (!_fCommandGate)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(15), data);
}
#endif

#ifdef AIRPORT_WCL
IOReturn AirportItlwmSkywalkInterface::getBEACON_INFO(apple80211_beacon_info_t *data)
{
    if (!_fCommandGate || !data)
        return kIOReturnNotReady;
    return _fCommandGate->runAction(wclCommand, this, reinterpret_cast<void *>(7), data);
}

void AirportItlwmSkywalkInterface::abortScanForContinuityMedia()
{
#ifdef KERNEL
    if (instance && !instance->getWorkLoop()->inGate()) {
        if (!_fCommandGate)
            return;
        _fCommandGate->runAction(
            [](OSObject *, void *self, void *, void *, void *) -> IOReturn {
                static_cast<AirportItlwmSkywalkInterface *>(self)->abortScanForContinuityMedia();
                return kIOReturnSuccess;
            }, this);
        return;
    }
#endif
    ieee80211com *ic = fHalService ? fHalService->get80211Controller() : nullptr;
    if (!ic || ic->ic_state != IEEE80211_S_RUN)
        return;
    if (!scanPending && !(ic->ic_flags & IEEE80211_F_BGSCAN))
        return;
    ic->ic_flags &= ~IEEE80211_F_BGSCAN;
#ifdef KERNEL
    if (fHalService->getDriverController())
        fHalService->getDriverController()->clearScanningFlags();
#endif
    if (scanPending)
        finishWCLScan(kIOReturnSuccess);
}

IOReturn AirportItlwmSkywalkInterface::setSCAN_CONTROL(apple80211_scan_control_params *params)
{
    if (!params)
        return kIOReturnBadArgument;
#ifdef KERNEL
    if (instance && !instance->getWorkLoop()->inGate()) {
        if (!_fCommandGate)
            return kIOReturnNotReady;
        return _fCommandGate->runAction(
            [](OSObject *, void *self, void *arg, void *, void *) -> IOReturn {
                return static_cast<AirportItlwmSkywalkInterface *>(self)->setSCAN_CONTROL(
                    static_cast<apple80211_scan_control_params *>(arg));
            }, this, params);
    }
#endif
    ieee80211com *ic = fHalService ? fHalService->get80211Controller() : nullptr;
    if (scanPending && ic && ic->ic_state == IEEE80211_S_RUN &&
        instance && instance->awdlInterface &&
        instance->awdlInterface->continuityMediaActive()) {
        ic->ic_flags &= ~IEEE80211_F_BGSCAN;
#ifdef KERNEL
        if (fHalService->getDriverController())
            fHalService->getDriverController()->clearScanningFlags();
#endif
        finishWCLScan(kIOReturnSuccess);
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setABORT_SCAN(apple80211_abort_scan *params)
{
    (void)params;
#ifdef KERNEL
    if (instance && !instance->getWorkLoop()->inGate()) {
        if (!_fCommandGate)
            return kIOReturnNotReady;
        return _fCommandGate->runAction(
            [](OSObject *, void *self, void *arg, void *, void *) -> IOReturn {
                return static_cast<AirportItlwmSkywalkInterface *>(self)->setABORT_SCAN(
                    static_cast<apple80211_abort_scan *>(arg));
            }, this, params);
    }
#endif
    ieee80211com *ic = fHalService ? fHalService->get80211Controller() : nullptr;
    if (ic && (scanPending || (ic->ic_flags & (IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN)))) {
        if (ic->ic_state == IEEE80211_S_SCAN)
            AirportWCL::noteAbortedScanInScanState(this);
        ic->ic_flags &= ~(IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN);
        ic->ic_wcl_scan_requested = false;
        ic->ic_wcl_scan_ssid_length = 0;
        ic->ic_wcl_scan_ssid_count = 0;
#ifdef KERNEL
        if (fHalService->getDriverController())
            fHalService->getDriverController()->abortScanning();
#endif
        if (scanPending)
            finishWCLScan(kIOReturnAborted);
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_SET_ROAM_LOCK(apple80211_set_roam_lock *params)
{
    if (!params)
        return kIOReturnBadArgument;
#ifdef KERNEL
    if (instance && !instance->getWorkLoop()->inGate()) {
        if (!_fCommandGate)
            return kIOReturnNotReady;
        return _fCommandGate->runAction(
            [](OSObject *, void *self, void *arg, void *, void *) -> IOReturn {
                return static_cast<AirportItlwmSkywalkInterface *>(self)->setWCL_SET_ROAM_LOCK(
                    static_cast<apple80211_set_roam_lock *>(arg));
            }, this, params);
    }
#endif
    wclRoamLock = *reinterpret_cast<const uint8_t *>(params) != 0;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_REAL_TIME_MODE(apple80211_wcl_real_time_mode *params)
{
    if (!params)
        return kIOReturnBadArgument;
#ifdef KERNEL
    if (instance && !instance->getWorkLoop()->inGate()) {
        if (!_fCommandGate)
            return kIOReturnNotReady;
        return _fCommandGate->runAction(
            [](OSObject *, void *self, void *arg, void *, void *) -> IOReturn {
                return static_cast<AirportItlwmSkywalkInterface *>(self)->setWCL_REAL_TIME_MODE(
                    static_cast<apple80211_wcl_real_time_mode *>(arg));
            }, this, params);
    }
#endif
    wclRealTimeMode = *reinterpret_cast<const uint8_t *>(params) != 0;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_CONFIG_BG_MOTIONPROFILE(apple80211_bg_motion_profile *params)
{
    return params ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_CONFIG_BG_NETWORK(apple80211_bg_network *params)
{
    return params ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_CONFIG_BGSCAN(apple80211_bg_scan *params)
{
    return params ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn AirportItlwmSkywalkInterface::setWCL_CONFIG_BG_PARAMS(apple80211_bg_params *params)
{
    return params ? kIOReturnSuccess : kIOReturnBadArgument;
}
#endif
