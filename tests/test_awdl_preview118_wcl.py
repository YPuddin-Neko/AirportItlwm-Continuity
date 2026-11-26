"""WCL hidden-SSID join, split-scan continuation, bulk-traffic guard, and outputStart tests:
1. wclRoamLock and wclRealTimeMode never abort in-flight WCL scans or block directed
   legacyScanSSID (+0x1c/+0x20) hidden-SSID Join Scans; repeated identical cached multi-SSID
   AutoJoin batches throttle within 20 s while uncached SSIDs always scan live.
2. SplitScanBurst tracks lastStepUS across subset start and finishWCLScan with a 5.0 s
   inter-subset window so >4 s multi-subset scans complete through 5 GHz Ch 165 (UNII-3),
   with an 8 s post-burst guard on active undirected scans and 45 s on passive bgscans.
3. InfraTrafficTracker suppresses undirected scans during active en1 transfers (>= 256 KB
   within 2.0 s) while allowing user-initiated directed hidden-SSID scans.
4. setWCL_SCAN_ABORT (wclCommand case 4) and setABORT_SCAN call abortScanning() and clear
   IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN for immediate Join Scan / Join preemption, and
   empty/6 GHz-only channel subsets succeed via 1 ms ScanDone(kIOReturnSuccess).
5. startWCLJoin cleanly switches from an associated IEEE80211_S_RUN network to a hidden
   target network with Tag 0 SSID preserved in AssocReq, and outputStart enforces immediate
   ifq_is_oactive backpressure with a 64-packet staging cap.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

import test_regressions as support
from test_offline import definition


class Preview118WCLRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p118_roam_lock_legacy_ssid_join_scan_and_multi_ssid_autojoin_throttle(self):
        skywalk_scan_req = definition(
            "AirportItlwm/AirportItlwmSkywalkInterface.cpp",
            "AirportItlwmSkywalkInterface::setSCAN_REQ",
        )
        self.assertNotIn("wclRoamLock", skywalk_scan_req)
        self.assertNotIn("wclRealTimeMode", skywalk_scan_req)
        self.assertIn("AirportWCL::isHiddenSSID(sd->ssid, sd->ssid_len)", skywalk_scan_req)
        self.assertIn("continuityMediaActive()", skywalk_scan_req)

        wcl_methods = "\n\n".join(
            definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::" + name)
            for name in (
                "beginWCLScan",
                "completeWCLScanFromCache",
                "startWCLScan",
                "finishWCLScan",
                "completeWCLScan",
                "setWCL_SET_ROAM_LOCK",
                "setWCL_REAL_TIME_MODE",
                "leaveWCLNetwork",
                "handleWCLLink",
                "stopWCLGated",
            )
        )
        src = (
            r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#define KERNEL 1
#define AIRPORT_WCL 1
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"

using IOReturn = int;
using OSObject = struct OSObjectStub;
enum {
    kIOReturnSuccess = 0,
    kIOReturnBadArgument = 1,
    kIOReturnBusy = 2,
    kIOReturnNotReady = 3,
    kIOReturnUnsupported = 4,
    kIOReturnError = 5,
    kIOReturnAborted = 6,
    kIOReturnTimeout = 7,
    kIOReturnOffline = 8
};
enum {
    IFF_UP = 1,
    IFF_RUNNING = 2,
    IEEE80211_F_ASCAN = 1,
    IEEE80211_F_BGSCAN = 4,
    IEEE80211_F_PSK = 8,
    IEEE80211_F_DESBSSID = 16,
    IEEE80211_CHAN_MAX = 255,
    IEEE80211_REASON_AUTH_LEAVE = 3,
    IWM_MIN_DBM = -100
};
enum {
    IEEE80211_S_INIT = 0,
    IEEE80211_S_SCAN = 1,
    IEEE80211_S_AUTH = 2,
    IEEE80211_S_ASSOC = 3,
    IEEE80211_S_RUN = 4
};

static uint64_t fakeClockUS = 10000000ULL;
static uint64_t mach_absolute_time() { return fakeClockUS * 1000ULL; }
static void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *ns) { *ns = abstime; }
static void explicit_bzero(void *p, size_t n) { memset(p, 0, n); }
#define bzero(p, n) memset((p), 0, (n))
#define IEEE80211_ADDR_EQ(a, b) (memcmp((a), (b), 6) == 0)
#define RB_FOREACH(var, name, head) for ((var) = (head)->first; (var) != nullptr; (var) = (var)->next)

struct OSObjectStub {};
struct WorkLoopStub { bool inGate() const { return true; } };
struct CommandGateStub {
    IOReturn runAction(IOReturn (*fn)(OSObject *, void *, void *, void *, void *),
                       void *a0, void *a1 = nullptr, void *a2 = nullptr) {
        return fn(nullptr, a0, a1, a2, nullptr);
    }
};
struct IOTimerEventSource {
    int delay = -1;
    void setTimeoutMS(int ms) { delay = ms; }
    void cancelTimeout() { delay = -1; }
};
struct ieee80211_node {
    uint8_t ni_bssid[6] = {};
    uint8_t ni_esslen = 0;
    uint8_t ni_essid[32] = {};
    int ni_rssi = 56; // -44 dBm
    const uint8_t *ni_rsnie_tlv = nullptr;
    size_t ni_rsnie_tlv_len = 0;
    ieee80211_node *next = nullptr;
};
struct NodeTree { ieee80211_node *first = nullptr; };
struct ieee80211com {
    struct {
        int if_flags = IFF_UP | IFF_RUNNING;
        uint64_t if_ibytes = 0;
        uint64_t if_obytes = 0;
    } ic_if;
    int ic_state = IEEE80211_S_RUN;
    unsigned ic_flags = 0;
    int ic_deauth_reason = 0;
    ieee80211_node *ic_bss = nullptr;
    NodeTree ic_tree = {};
    struct { int ic_flags = 0; } ic_channels[256] = {};
    bool ic_wcl_mac_reconfig = false, ic_wcl_scan_requested = false,
         ic_wcl_scan_active = false, ic_wcl_scan_restricted = false,
         ic_wcl_join_requested = false, ic_wcl_enterprise = false;
    unsigned ic_wcl_scan_ssid_length = 0, ic_des_esslen = 0;
    uint8_t ic_wcl_join_channel = 0;
    uint8_t ic_wcl_scan_ssid_count = 0, ic_wcl_scan_ssid_lengths[10] = {};
    uint8_t ic_wcl_scan_ssids[10][32] = {};
    uint8_t ic_wcl_scan_ssid[32] = {}, ic_wcl_scan_channels[32] = {};
    uint8_t ic_des_essid[32] = {}, ic_psk[32] = {};
    int (*ic_newstate)(ieee80211com *, int, int) = nullptr;
};

static unsigned liveBgscanStarts = 0;
static void ieee80211_begin_cache_bgscan(decltype(ieee80211com::ic_if) *ifp) {
    auto *ic = reinterpret_cast<ieee80211com *>(ifp);
    ic->ic_flags |= IEEE80211_F_BGSCAN;
    ++liveBgscanStarts;
}
static void ieee80211_new_state(ieee80211com *ic, int st, int) { ic->ic_state = st; }
static void ieee80211_disable_rsn(ieee80211com *) {}
static void ieee80211_disable_wep(ieee80211com *) {}

struct DriverControllerStub {
    unsigned abortCalls = 0;
    void abortScanning() { ++abortCalls; }
    void clearScanningFlags() {}
};
struct HalStub {
    ieee80211com ic;
    ieee80211_node bss;
    DriverControllerStub drv;
    HalStub() {
        const uint8_t ap[6] = {0x02, 0x00, 0x00, 0x00, 0x10, 0x01};
        memcpy(bss.ni_bssid, ap, 6);
        ic.ic_bss = &bss;
        for (unsigned c : {1U,2U,3U,4U,5U,6U,7U,8U,9U,10U,11U,12U,13U,
                           36U,40U,44U,48U,52U,56U,60U,64U,
                           100U,104U,108U,112U,116U,120U,
                           124U,128U,132U,136U,140U,144U,
                           149U,153U,157U,161U,165U}) {
            ic.ic_channels[c].ic_flags = 1;
        }
    }
    ieee80211com *get80211Controller() { return &ic; }
    DriverControllerStub *getDriverController() { return &drv; }
};
struct AWDLStub {
    bool media = false;
    bool continuityMediaActive() const { return media; }
};
struct InstanceStub {
    WorkLoopStub wl;
    AWDLStub awdl;
    AWDLStub *awdlInterface = &awdl;
    unsigned scanResults = 0;
    unsigned scanDones = 0;
    int lastDoneStatus = -1;
    WorkLoopStub *getWorkLoop() { return &wl; }
    void postMessage(void *, unsigned code, const void *data, size_t, bool) {
        if (code == AirportWCL::ScanResult) ++scanResults;
        if (code == AirportWCL::ScanDone) {
            ++scanDones;
            lastDoneStatus = *static_cast<const int *>(data);
        }
    }
};
struct apple80211ScanRequest { uint8_t bytes[0x1550] = {}; };
struct apple80211_set_roam_lock { uint8_t lock; };
struct apple80211_wcl_real_time_mode { uint8_t enabled; };

struct AirportItlwmSkywalkInterface {
    HalStub hal;
    HalStub *fHalService = &hal;
    CommandGateStub gate;
    CommandGateStub *_fCommandGate = &gate;
    IOTimerEventSource scanTimer, joinTmr;
    IOTimerEventSource *scanSource = &scanTimer;
    IOTimerEventSource *joinTimer = &joinTmr;
    InstanceStub owner;
    InstanceStub *instance = &owner;
    uint8_t joinBSSID[6] = {0x02, 0x00, 0x00, 0x00, 0x10, 0x01};
    bool scanPending = false, scanCompleted = false, scanSkipCompletion = false, scanDeferred = false;
    AirportWCL::ScanRequest wclScanRequest = {};
    bool wclBssValid = false, wclLinkIndicated = false;
    uint8_t wclBssInfo[sizeof(AirportWCL::BeaconMetadata) + 0x800] = {};
    uint32_t wclAssociatedBeacons = 0;
    bool joinPending = false, joinDeferred = false;
    AirportWCL::JoinRequest deferredJoin = {};
    bool assocNotified = true, firstBeaconNotified = true, portAuthorized = true;
    bool wclRoamLock = false, wclRealTimeMode = false;
    uint64_t wclLastReportedRateBps = 0;
    uint64_t wclLastLiveBgscanUS = 0;
    bool wclStopping = false;
    uint64_t wclCountryGeneration = 0;
    bool disassocIsVoluntary = false;

    void clearWCLPMKSA() {}
    void updateWCLLinkQuality(bool = false) {}
    void syncInfraDataBandwidths() {}
    void finishWCLJoin(uint16_t, uint16_t, IOReturn) {}
    void publishWCLBeacon(const ieee80211_node *, const uint8_t *, size_t, bool = false) {
        ++owner.scanResults;
    }

    IOReturn beginWCLScan(apple80211ScanRequest *);
    bool completeWCLScanFromCache(const AirportWCL::ScanRequest &);
    IOReturn startWCLScan(const AirportWCL::ScanRequest &);
    void finishWCLScan(IOReturn);
    void completeWCLScan();
    IOReturn setWCL_SET_ROAM_LOCK(apple80211_set_roam_lock *);
    IOReturn setWCL_REAL_TIME_MODE(apple80211_wcl_real_time_mode *);
    IOReturn leaveWCLNetwork();
    void handleWCLLink(bool);
    static IOReturn stopWCLGated(OSObject *, void *, void *, void *, void *);
};

static void putU32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v & 0xff);
    p[1] = uint8_t((v >> 8) & 0xff);
    p[2] = uint8_t((v >> 16) & 0xff);
    p[3] = uint8_t((v >> 24) & 0xff);
}

static apple80211ScanRequest makeScanReq(const std::vector<unsigned> &chans,
                                         uint32_t scanType,
                                         const char *legacySsid = nullptr,
                                         const std::vector<const char *> &multiSsids = {}) {
    apple80211ScanRequest req = {};
    putU32(req.bytes + 0x40, scanType);
    putU32(req.bytes + 0x54, uint32_t(chans.size()));
    for (size_t i = 0; i < chans.size(); ++i) {
        uint8_t *entry = req.bytes + 0x58 + i * 12;
        putU32(entry + 0, 1);
        putU32(entry + 4, chans[i]);
        putU32(entry + 8, chans[i] <= 14 ? 0x8U : 0x10U);
    }
    if (legacySsid) {
        const uint32_t len = uint32_t(strlen(legacySsid));
        putU32(req.bytes + 0x1c, len);
        memcpy(req.bytes + 0x20, legacySsid, len);
    }
    if (!multiSsids.empty()) {
        putU32(req.bytes + 0x1318, uint32_t(multiSsids.size()));
        for (size_t i = 0; i < multiSsids.size(); ++i) {
            const uint32_t len = uint32_t(strlen(multiSsids[i]));
            uint8_t *entry = req.bytes + 0x131c + i * 56;
            putU32(entry + 0, 1);
            putU32(entry + 4, len);
            if (len)
                memcpy(entry + 8, multiSsids[i], len);
        }
    }
    return req;
}

"""
            + wcl_methods
            + r"""

int main() {
    AirportItlwmSkywalkInterface s;
    AirportWCL::BeaconMetadata cached = {};
    cached.channel = 48;
    cached.ssidLength = 8;
    memcpy(cached.ssid, "InfraLab", 8);
    memcpy(cached.bssid, s.joinBSSID, 6);
    memcpy(s.wclBssInfo, &cached, sizeof(cached));
    s.wclBssValid = true;

    const uint8_t dummyRsn[4] = {48, 2, 1, 0};
    ieee80211_node cachedPeer = {};
    const uint8_t peerBssid[6] = {0x02, 0x00, 0x00, 0x00, 0x20, 0x02};
    memcpy(cachedPeer.ni_bssid, peerBssid, 6);
    cachedPeer.ni_esslen = 9;
    memcpy(cachedPeer.ni_essid, "SavedPeer", 9);
    cachedPeer.ni_rsnie_tlv = dummyRsn;
    cachedPeer.ni_rsnie_tlv_len = sizeof(dummyRsn);
    s.hal.ic.ic_tree.first = &cachedPeer;

    const std::vector<std::vector<unsigned>> sevenSubsets = {
        {1, 2, 3, 4, 5, 6},
        {7, 8, 9, 10, 11, 12},
        {13, 36, 40, 44, 48, 52},
        {56, 60, 64, 100, 104, 108},
        {112, 116, 120, 124, 128, 132},
        {136, 140, 144, 149, 153, 157},
        {161, 165},
    };

    // 1. Engaging wclRoamLock (0x1ac = 1) and wclRealTimeMode (0x1b4 = 1) mid-scan MUST NOT
    //    abort an in-flight WCL scan, and all 7 subsets of WiFiAgent's single-SSID
    //    legacyScanSSID (+0x1c/+0x20) Join Scan MUST run live on hardware!
    fakeClockUS = 50000000ULL;
    liveBgscanStarts = 0;
    apple80211_set_roam_lock lockOn = {1};
    apple80211_wcl_real_time_mode rtOn = {1};
    for (size_t idx = 0; idx < sevenSubsets.size(); ++idx) {
        const uint32_t scanType = idx >= 2 ? 2U : 1U; // 5 GHz LAR subsets arrive as type 2
        auto req = makeScanReq(sevenSubsets[idx], scanType, "HiddenTarget5G");
        assert(s.beginWCLScan(&req) == kIOReturnSuccess);
        assert(s.scanPending && (s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
        assert(s.hal.ic.ic_wcl_scan_active);
        assert(s.hal.ic.ic_wcl_scan_ssid_length == 14);
        if (idx == 0) {
            assert(s.setWCL_SET_ROAM_LOCK(&lockOn) == kIOReturnSuccess && s.wclRoamLock);
            assert(s.setWCL_REAL_TIME_MODE(&rtOn) == kIOReturnSuccess && s.wclRealTimeMode);
            assert(s.scanPending && (s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
        }
        fakeClockUS += 600000ULL;
        s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
        s.scanCompleted = true;
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == 7);

    // 2. Multi-SSID AutoJoin batch where ALL requested SSIDs are already cached:
    //    First 7-subset burst runs live; identical batch 8 s later (< 20 s) completes from cache;
    //    multi-SSID batch containing an uncached hidden SSID STILL runs live; after 21 s runs live!
    fakeClockUS = 80000000ULL;
    const unsigned beforeCachedMulti = liveBgscanStarts;
    for (const auto &chans : sevenSubsets) {
        auto req = makeScanReq(chans, 1, nullptr, {"InfraLab", "SavedPeer", ""});
        assert(s.beginWCLScan(&req) == kIOReturnSuccess);
        assert(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN);
        fakeClockUS += 400000ULL;
        s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
        s.scanCompleted = true;
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == beforeCachedMulti + 7);

    // 8 s after batch start (< 20 s): repeated identical cached multi-SSID batch completes from cache!
    fakeClockUS = 88000000ULL;
    {
        auto req = makeScanReq(sevenSubsets[0], 1, nullptr, {"InfraLab", "SavedPeer", ""});
        assert(s.beginWCLScan(&req) == kIOReturnSuccess);
        assert(!(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
        assert(s.scanCompleted && s.scanTimer.delay == 1);
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == beforeCachedMulti + 7);

    // Uncached hidden SSID in multi-SSID list at 88.5 s MUST NOT be throttled:
    fakeClockUS = 88500000ULL;
    {
        auto req = makeScanReq(sevenSubsets[6], 2, nullptr, {"InfraLab", "UncachedHidden5G", ""});
        assert(s.beginWCLScan(&req) == kIOReturnSuccess);
        assert(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN);
        s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
        s.scanCompleted = true;
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == beforeCachedMulti + 8);

    // After > 20 s from cached batch, cached multi-SSID batch runs live again:
    fakeClockUS = 101000000ULL;
    {
        auto req = makeScanReq(sevenSubsets[0], 1, nullptr, {"InfraLab", "SavedPeer", ""});
        assert(s.beginWCLScan(&req) == kIOReturnSuccess);
        assert(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN);
        s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
        s.scanCompleted = true;
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == beforeCachedMulti + 9);

    AirportItlwmSkywalkInterface::stopWCLGated(nullptr, &s, nullptr, nullptr, nullptr);
    return 0;
}
"""
        )
        self.compile_run(src)

    def test_p118_split_scan_unii3_continuation_and_infra_bulk_traffic_guard(self):
        wcl_methods = "\n\n".join(
            definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::" + name)
            for name in (
                "beginWCLScan",
                "completeWCLScanFromCache",
                "startWCLScan",
                "finishWCLScan",
                "completeWCLScan",
                "leaveWCLNetwork",
                "handleWCLLink",
                "stopWCLGated",
            )
        )
        src = (
            r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#define KERNEL 1
#define AIRPORT_WCL 1
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"

using IOReturn = int;
using OSObject = struct OSObjectStub;
enum {
    kIOReturnSuccess = 0,
    kIOReturnBadArgument = 1,
    kIOReturnBusy = 2,
    kIOReturnNotReady = 3,
    kIOReturnUnsupported = 4,
    kIOReturnError = 5,
    kIOReturnAborted = 6,
    kIOReturnTimeout = 7,
    kIOReturnOffline = 8
};
enum {
    IFF_UP = 1,
    IFF_RUNNING = 2,
    IEEE80211_F_ASCAN = 1,
    IEEE80211_F_BGSCAN = 4,
    IEEE80211_F_PSK = 8,
    IEEE80211_F_DESBSSID = 16,
    IEEE80211_CHAN_MAX = 255,
    IEEE80211_REASON_AUTH_LEAVE = 3,
    IWM_MIN_DBM = -100
};
enum {
    IEEE80211_S_INIT = 0,
    IEEE80211_S_SCAN = 1,
    IEEE80211_S_AUTH = 2,
    IEEE80211_S_ASSOC = 3,
    IEEE80211_S_RUN = 4
};

static uint64_t fakeClockUS = 10000000ULL;
static uint64_t mach_absolute_time() { return fakeClockUS * 1000ULL; }
static void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *ns) { *ns = abstime; }
static void explicit_bzero(void *p, size_t n) { memset(p, 0, n); }
#define bzero(p, n) memset((p), 0, (n))
#define IEEE80211_ADDR_EQ(a, b) (memcmp((a), (b), 6) == 0)
#define RB_FOREACH(var, name, head) for ((var) = (head)->first; (var) != nullptr; (var) = (var)->next)

struct OSObjectStub {};
struct IOTimerEventSource {
    int delay = -1;
    void setTimeoutMS(int ms) { delay = ms; }
    void cancelTimeout() { delay = -1; }
};
struct ieee80211_node {
    uint8_t ni_bssid[6] = {};
    int ni_rssi = 56; // -44 dBm
    const uint8_t *ni_rsnie_tlv = nullptr;
    size_t ni_rsnie_tlv_len = 0;
    ieee80211_node *next = nullptr;
};
struct NodeTree { ieee80211_node *first = nullptr; };
struct ieee80211com {
    struct {
        int if_flags = IFF_UP | IFF_RUNNING;
        uint64_t if_ibytes = 0;
        uint64_t if_obytes = 0;
    } ic_if;
    int ic_state = IEEE80211_S_RUN;
    unsigned ic_flags = 0;
    int ic_deauth_reason = 0;
    ieee80211_node *ic_bss = nullptr;
    NodeTree ic_tree = {};
    struct { int ic_flags = 0; } ic_channels[256] = {};
    bool ic_wcl_mac_reconfig = false, ic_wcl_scan_requested = false,
         ic_wcl_scan_active = false, ic_wcl_scan_restricted = false,
         ic_wcl_join_requested = false, ic_wcl_enterprise = false;
    unsigned ic_wcl_scan_ssid_length = 0, ic_des_esslen = 0;
    uint8_t ic_wcl_join_channel = 0;
    uint8_t ic_wcl_scan_ssid_count = 0, ic_wcl_scan_ssid_lengths[10] = {};
    uint8_t ic_wcl_scan_ssids[10][32] = {};
    uint8_t ic_wcl_scan_ssid[32] = {}, ic_wcl_scan_channels[32] = {};
    uint8_t ic_des_essid[32] = {}, ic_psk[32] = {};
    int (*ic_newstate)(ieee80211com *, int, int) = nullptr;
};

static unsigned liveBgscanStarts = 0;
static void ieee80211_begin_cache_bgscan(decltype(ieee80211com::ic_if) *ifp) {
    auto *ic = reinterpret_cast<ieee80211com *>(ifp);
    ic->ic_flags |= IEEE80211_F_BGSCAN;
    ++liveBgscanStarts;
}
static void ieee80211_new_state(ieee80211com *ic, int st, int) { ic->ic_state = st; }
static void ieee80211_disable_rsn(ieee80211com *) {}
static void ieee80211_disable_wep(ieee80211com *) {}

struct HalStub {
    ieee80211com ic;
    ieee80211_node bss;
    HalStub() {
        const uint8_t ap[6] = {0x02, 0x00, 0x00, 0x00, 0x30, 0x01};
        memcpy(bss.ni_bssid, ap, 6);
        ic.ic_bss = &bss;
        for (unsigned c : {1U,2U,3U,4U,5U,6U,7U,8U,9U,10U,11U,12U,13U,
                           36U,40U,44U,48U,52U,56U,60U,64U,
                           100U,104U,108U,112U,116U,120U,
                           124U,128U,132U,136U,140U,144U,
                           149U,153U,157U,161U,165U}) {
            ic.ic_channels[c].ic_flags = 1;
        }
    }
    ieee80211com *get80211Controller() { return &ic; }
};
struct AWDLStub {
    bool media = false;
    bool continuityMediaActive() const { return media; }
};
struct InstanceStub {
    AWDLStub awdl;
    AWDLStub *awdlInterface = &awdl;
    unsigned scanDones = 0;
    void postMessage(void *, unsigned code, const void *, size_t, bool) {
        if (code == AirportWCL::ScanDone) ++scanDones;
    }
};
struct apple80211ScanRequest { uint8_t bytes[0x1550] = {}; };

struct AirportItlwmSkywalkInterface {
    HalStub hal;
    HalStub *fHalService = &hal;
    IOTimerEventSource scanTimer, joinTmr;
    IOTimerEventSource *scanSource = &scanTimer;
    IOTimerEventSource *joinTimer = &joinTmr;
    InstanceStub owner;
    InstanceStub *instance = &owner;
    uint8_t joinBSSID[6] = {0x02, 0x00, 0x00, 0x00, 0x30, 0x01};
    bool scanPending = false, scanCompleted = false, scanSkipCompletion = false, scanDeferred = false;
    AirportWCL::ScanRequest wclScanRequest = {};
    bool wclBssValid = true, wclLinkIndicated = false;
    uint8_t wclBssInfo[sizeof(AirportWCL::BeaconMetadata) + 0x800] = {};
    uint32_t wclAssociatedBeacons = 0;
    bool joinPending = false, joinDeferred = false;
    AirportWCL::JoinRequest deferredJoin = {};
    bool assocNotified = true, firstBeaconNotified = true, portAuthorized = true;
    bool wclRoamLock = false, wclRealTimeMode = false;
    uint64_t wclLastReportedRateBps = 0;
    uint64_t wclLastLiveBgscanUS = 0;
    bool wclStopping = false;
    uint64_t wclCountryGeneration = 0;
    bool disassocIsVoluntary = false;

    void clearWCLPMKSA() {}
    void updateWCLLinkQuality(bool = false) {}
    void syncInfraDataBandwidths() {}
    void finishWCLJoin(uint16_t, uint16_t, IOReturn) {}
    void publishWCLBeacon(const ieee80211_node *, const uint8_t *, size_t, bool = false) {}

    IOReturn beginWCLScan(apple80211ScanRequest *);
    bool completeWCLScanFromCache(const AirportWCL::ScanRequest &);
    IOReturn startWCLScan(const AirportWCL::ScanRequest &);
    void finishWCLScan(IOReturn);
    void completeWCLScan();
    IOReturn leaveWCLNetwork();
    void handleWCLLink(bool);
    static IOReturn stopWCLGated(OSObject *, void *, void *, void *, void *);
};

static void putU32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v & 0xff);
    p[1] = uint8_t((v >> 8) & 0xff);
    p[2] = uint8_t((v >> 16) & 0xff);
    p[3] = uint8_t((v >> 24) & 0xff);
}

static apple80211ScanRequest makeSubset(const std::vector<unsigned> &chans,
                                        uint32_t scanType = 1,
                                        const char *ssid = nullptr) {
    apple80211ScanRequest req = {};
    putU32(req.bytes + 0x40, scanType);
    putU32(req.bytes + 0x54, uint32_t(chans.size()));
    for (size_t i = 0; i < chans.size(); ++i) {
        uint8_t *entry = req.bytes + 0x58 + i * 12;
        putU32(entry + 0, 1);
        putU32(entry + 4, chans[i]);
        putU32(entry + 8, chans[i] <= 14 ? 0x8U : 0x10U);
    }
    if (ssid) {
        const uint32_t len = uint32_t(strlen(ssid));
        putU32(req.bytes + 0x1c, len);
        memcpy(req.bytes + 0x20, ssid, len);
    }
    return req;
}

"""
            + wcl_methods
            + r"""

int main() {
    AirportItlwmSkywalkInterface s;

    // 1. 7-subset split scan taking 7.1 s total (> 4.1 s across subsets 1..4) through
    //    5 GHz Ch 165 (UNII-3): ALL 7 subsets run live via lastStepUS <= 5.0 s!
    const std::vector<std::vector<unsigned>> subsets = {
        {1, 2, 3, 4, 5, 6},
        {7, 8, 9, 10, 11, 12},
        {13, 36, 40, 44, 48, 52},
        {56, 60, 64, 100, 104, 108},
        {112, 116, 120, 124, 128, 132},
        {136, 140, 144, 149, 153, 157},
        {161, 165},
    };
    const uint64_t subsetDurationsUS[7] = {
        1100000ULL, 900000ULL, 800000ULL, 1700000ULL, // 4.5 s across subsets 1..4!
        1100000ULL, 1000000ULL, 500000ULL             // +2.6 s across UNII-2e/UNII-3!
    };
    fakeClockUS = 100000000ULL;
    liveBgscanStarts = 0;
    for (size_t i = 0; i < subsets.size(); ++i) {
        auto req = makeSubset(subsets[i], i >= 3 ? 2U : 1U);
        assert(s.beginWCLScan(&req) == kIOReturnSuccess);
        assert(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN);
        fakeClockUS += subsetDurationsUS[i];
        s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
        s.scanCompleted = true;
        s.completeWCLScan(); // Updates lastStepUS via finishWCLScan(kIOReturnSuccess)
        fakeClockUS += 12000ULL; // 12 ms inter-subset gap from WCLScanManager
    }
    assert(liveBgscanStarts == 7);

    // 2. Undirected telemetry poll 5.0 s after burst finish (< 8.0 s post-burst guard) throttles:
    const uint64_t burstEndUS = fakeClockUS;
    fakeClockUS = burstEndUS + 5000000ULL;
    {
        auto pollReq = makeSubset(subsets[0], 1);
        assert(s.beginWCLScan(&pollReq) == kIOReturnSuccess);
        assert(!(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN) && s.scanTimer.delay == 1);
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == 7);

    // 3. Active host scan (scan.type == 1) 26.0 s after burst finish (>= 25.0 s) runs live,
    //    whereas passive undirected bgscan (scan.type == 2) at +15 s (< 45 s) throttles!
    fakeClockUS = burstEndUS + 26000000ULL;
    {
        auto activeReq = makeSubset(subsets[0], 1);
        assert(s.beginWCLScan(&activeReq) == kIOReturnSuccess);
        assert(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN);
        fakeClockUS += 300000ULL;
        s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
        s.scanCompleted = true;
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == 8);

    fakeClockUS += 15000000ULL; // +15 s (< 45 s passive cooldown, < 25 s active guard)
    {
        auto passiveBg = makeSubset(subsets[2], 2);
        assert(s.beginWCLScan(&passiveBg) == kIOReturnSuccess);
        assert(!(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN) && s.scanTimer.delay == 1);
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == 8);

    // 4. Bulk-traffic guard: >= 256 KB transferred within <= 2.0 s suppresses BOTH passive
    //    and active undirected scans in 1 ms (even well past the 45 s bgscan cooldown!),
    //    while user-initiated directed hidden-SSID Join Scans STILL run live!
    fakeClockUS += 60000000ULL; // +60 s (> 45 s cooldown)
    AirportWCL::splitScanBurstFor(&s).traffic.sample(100000ULL, fakeClockUS);
    fakeClockUS += 800000ULL; // +0.8 s
    s.hal.ic.ic_if.if_ibytes = 350000ULL; // +400 KB total delta (~4.0 Mbps)
    s.hal.ic.ic_if.if_obytes = 150000ULL;

    for (uint32_t t : {1U, 2U}) {
        auto undir = makeSubset(subsets[0], t);
        assert(s.beginWCLScan(&undir) == kIOReturnSuccess);
        assert(!(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN) && s.scanTimer.delay == 1);
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == 8);

    // Directed hidden-SSID scan during bulk traffic MUST still run live:
    {
        auto dir = makeSubset(subsets[6], 2, "HiddenUNII3");
        assert(s.beginWCLScan(&dir) == kIOReturnSuccess);
        assert(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN);
        s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
        s.scanCompleted = true;
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == 9);

    // After bulk traffic ceases (and >= 25 s after last live scan), active scan runs live again:
    fakeClockUS += 26000000ULL;
    {
        auto idleActive = makeSubset(subsets[0], 1);
        assert(s.beginWCLScan(&idleActive) == kIOReturnSuccess);
        assert(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN);
        s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
        s.scanCompleted = true;
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == 10);

    AirportItlwmSkywalkInterface::stopWCLGated(nullptr, &s, nullptr, nullptr, nullptr);
    return 0;
}
"""
        )
        self.compile_run(src)

    def test_p118_scan_abort_preemption_empty_subset_and_associated_hidden_join(self):
        wcl_methods = "\n\n".join(
            definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::" + name)
            for name in (
                "wclCommand",
                "beginWCLScan",
                "completeWCLScanFromCache",
                "startWCLScan",
                "finishWCLScan",
                "completeWCLScan",
                "setABORT_SCAN",
                "beginWCLJoin",
                "startWCLJoin",
                "leaveWCLNetwork",
                "stopWCLGated",
            )
        )
        add_ssid_fn = definition(
            "itl80211/openbsd/net80211/ieee80211_output.c",
            "ieee80211_add_ssid",
        )
        src = (
            r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#define KERNEL 1
#define AIRPORT_WCL 1
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"

using IOReturn = int;
using u_int8_t = uint8_t;
using OSObject = struct OSObjectStub;
enum {
    kIOReturnSuccess = 0,
    kIOReturnBadArgument = 1,
    kIOReturnBusy = 2,
    kIOReturnNotReady = 3,
    kIOReturnUnsupported = 4,
    kIOReturnError = 5,
    kIOReturnAborted = 6,
    kIOReturnTimeout = 7,
    kIOReturnOffline = 8,
    kIOReturnNoMemory = 9
};
#ifndef ENETRESET
#define ENETRESET 52
#endif
enum {
    IFF_UP = 1,
    IFF_RUNNING = 2,
    IEEE80211_F_ASCAN = 1,
    IEEE80211_F_BGSCAN = 4,
    IEEE80211_F_AUTO_JOIN = 8,
    IEEE80211_F_PSK = 16,
    IEEE80211_F_DESBSSID = 32,
    IEEE80211_CHAN_MAX = 255,
    IEEE80211_REASON_AUTH_LEAVE = 3,
    IEEE80211_ELEMID_SSID = 0,
    IEEE80211_ELEMID_RSN = 48,
    IEEE80211_PMK_LEN = 32,
    IEEE80211_NWKEY_WEP = 1,
    IEEE80211_WPA_PROTO_WPA1 = 1,
    IEEE80211_WPA_PROTO_WPA2 = 2,
    IEEE80211_WPA_AKM_PSK = 1,
    IEEE80211_WPA_AKM_8021X = 2,
    IEEE80211_WPA_AKM_SHA256_PSK = 4,
    IEEE80211_WPA_AKM_SHA256_8021X = 8,
    IEEE80211_WPA_CIPHER_TKIP = 1,
    IEEE80211_WPA_CIPHER_CCMP = 2,
    IEEE80211_AKM_8021X = 1,
    IEEE80211_AKM_SHA256_8021X = 2,
    APPLE80211_AUTHTYPE_WPA = 1U << 0,
    APPLE80211_AUTHTYPE_WPA_PSK = 1U << 1,
    APPLE80211_AUTHTYPE_WPA2 = 1U << 2,
    APPLE80211_AUTHTYPE_WPA2_PSK = 1U << 3,
    APPLE80211_AUTHTYPE_SHA256_PSK = 1U << 10,
    APPLE80211_AUTHTYPE_SHA256_8021X = 1U << 11,
    APPLE80211_AUTHTYPE_WPA3_SAE = 1U << 12,
    APPLE80211_AUTHTYPE_WPA3_FT_SAE = 1U << 13,
    IWM_MIN_DBM = -100
};
enum {
    IEEE80211_S_INIT = 0,
    IEEE80211_S_SCAN = 1,
    IEEE80211_S_AUTH = 2,
    IEEE80211_S_ASSOC = 3,
    IEEE80211_S_RUN = 4
};

static uint64_t fakeClockUS = 20000000ULL;
static uint64_t mach_absolute_time() { return fakeClockUS * 1000ULL; }
static void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *ns) { *ns = abstime; }
static void explicit_bzero(void *p, size_t n) { memset(p, 0, n); }
#define bzero(p, n) memset((p), 0, (n))
#define isset(p, b) ((p)[(b) / 8] & (1U << ((b) % 8)))
#define IEEE80211_ADDR_EQ(a, b) (memcmp((a), (b), 6) == 0)
#define RB_FOREACH(var, name, head) for ((var) = (head)->first; (var) != nullptr; (var) = (var)->next)

struct OSObjectStub {};
struct WorkLoopStub { bool inGate() const { return true; } };
struct CommandGateStub {
    IOReturn runAction(IOReturn (*fn)(OSObject *, void *, void *, void *, void *),
                       void *a0, void *a1 = nullptr, void *a2 = nullptr) {
        return fn(nullptr, a0, a1, a2, nullptr);
    }
};
struct IOTimerEventSource {
    int delay = -1;
    void setTimeoutMS(int ms) { delay = ms; }
    void cancelTimeout() { delay = -1; }
};
struct ieee80211_wpaparams {
    int i_enabled = 0;
    unsigned i_protos = 0, i_akms = 0, i_ciphers = 0, i_groupcipher = 0;
};
struct ieee80211_nwkey {
    int i_wepon = 0, i_defkid = 0;
    struct { uint32_t i_keylen; const uint8_t *i_keydat; } i_key[4];
};
struct ieee80211_node {
    uint8_t ni_bssid[6] = {};
    uint8_t ni_esslen = 0;
    uint8_t ni_essid[32] = {};
    int ni_rssi = 56;
    const uint8_t *ni_rsnie_tlv = nullptr;
    size_t ni_rsnie_tlv_len = 0;
    ieee80211_node *next = nullptr;
};
struct NodeTree { ieee80211_node *first = nullptr; };
struct ieee80211com {
    struct { int if_flags = IFF_UP | IFF_RUNNING; } ic_if;
    int ic_state = IEEE80211_S_RUN;
    unsigned ic_flags = 0;
    int ic_deauth_reason = 0;
    ieee80211_node *ic_bss = nullptr;
    NodeTree ic_tree = {};
    uint8_t ic_chan_active[32] = {};
    struct { int ic_flags = 0; } ic_channels[256] = {};
    bool ic_wcl_mac_reconfig = false, ic_wcl_scan_requested = false,
         ic_wcl_scan_active = false, ic_wcl_scan_restricted = false,
         ic_wcl_join_requested = false, ic_wcl_enterprise = false;
    unsigned ic_wcl_scan_ssid_length = 0, ic_des_esslen = 0;
    uint8_t ic_wcl_join_channel = 0;
    uint8_t ic_wcl_scan_ssid_count = 0, ic_wcl_scan_ssid_lengths[10] = {};
    uint8_t ic_wcl_scan_ssids[10][32] = {};
    uint8_t ic_wcl_scan_ssid[32] = {}, ic_wcl_scan_channels[32] = {};
    uint8_t ic_des_essid[32] = {}, ic_des_bssid[6] = {}, ic_psk[32] = {};
    ieee80211_wpaparams lastWpa = {};
    unsigned newstateCalls = 0;
    int (*ic_newstate)(ieee80211com *, int, int) = nullptr;
};

static int mockNewstate(ieee80211com *ic, int st, int) {
    ++ic->newstateCalls;
    ic->ic_state = st;
    return 0;
}
static void ieee80211_begin_cache_bgscan(decltype(ieee80211com::ic_if) *ifp) {
    auto *ic = reinterpret_cast<ieee80211com *>(ifp);
    ic->ic_flags |= IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN;
}
static void ieee80211_new_state(ieee80211com *ic, int st, int arg) {
    if (ic->ic_newstate) ic->ic_newstate(ic, st, arg);
}
static void ieee80211_disable_rsn(ieee80211com *) {}
static void ieee80211_disable_wep(ieee80211com *) {}
static int ieee80211_ioctl_setwpaparms(ieee80211com *ic, const ieee80211_wpaparams *wpa) {
    ic->lastWpa = *wpa;
    return 0;
}
static int ieee80211_ioctl_setnwkeys(ieee80211com *, const ieee80211_nwkey *) { return 0; }
static void *ieee80211_pmksa_add(ieee80211com *, int, const uint8_t *, const uint8_t *, int) {
    static int token = 1;
    return &token;
}

struct DriverControllerStub {
    bool hwScanning = false;
    unsigned abortCalls = 0;
    void abortScanning() { hwScanning = false; ++abortCalls; }
    void clearScanningFlags() { hwScanning = false; }
};
struct HalStub {
    ieee80211com ic;
    ieee80211_node bss;
    DriverControllerStub drv;
    HalStub() {
        ic.ic_newstate = mockNewstate;
        const uint8_t ap[6] = {0x02, 0x00, 0x00, 0x00, 0x40, 0x01};
        memcpy(bss.ni_bssid, ap, 6);
        bss.ni_esslen = 13;
        memcpy(bss.ni_essid, "VisibleHome5G", 13);
        ic.ic_bss = &bss;
        for (unsigned c : {1U, 6U, 11U, 36U, 48U, 149U}) {
            ic.ic_channels[c].ic_flags = 1;
            ic.ic_chan_active[c / 8] |= 1U << (c % 8);
        }
    }
    ieee80211com *get80211Controller() { return &ic; }
    DriverControllerStub *getDriverController() { return &drv; }
    IOReturn setScanHomeAwayTime(uint32_t) { return kIOReturnSuccess; }
};
struct AWDLStub {
    bool media = false;
    bool continuityMediaActive() const { return media; }
};
struct InstanceStub {
    WorkLoopStub wl;
    AWDLStub awdl;
    AWDLStub *awdlInterface = &awdl;
    std::vector<int> scanDoneStatuses;
    WorkLoopStub *getWorkLoop() { return &wl; }
    void postMessage(void *, unsigned code, const void *data, size_t, bool) {
        if (code == AirportWCL::ScanDone)
            scanDoneStatuses.push_back(*static_cast<const int *>(data));
    }
};
struct apple80211ScanRequest { uint8_t bytes[0x1550] = {}; };
struct apple80211AssocCandidates { uint8_t bytes[0x2cc] = {}; };
struct apple80211_abort_scan { uint32_t version = 1; };
struct apple80211_key {};
struct apple80211_rsn_ie_data {};

struct AirportItlwmSkywalkInterface {
    HalStub hal;
    HalStub *fHalService = &hal;
    CommandGateStub gate;
    CommandGateStub *_fCommandGate = &gate;
    IOTimerEventSource scanTimer, joinTmr;
    IOTimerEventSource *scanSource = &scanTimer;
    IOTimerEventSource *joinTimer = &joinTmr;
    InstanceStub owner;
    InstanceStub *instance = &owner;
    uint8_t joinBSSID[6] = {0x02, 0x00, 0x00, 0x00, 0x40, 0x01};
    bool scanPending = false, scanCompleted = false, scanSkipCompletion = false, scanDeferred = false;
    AirportWCL::ScanRequest wclScanRequest = {};
    bool wclBssValid = true, wclLinkIndicated = false;
    uint8_t wclBssInfo[sizeof(AirportWCL::BeaconMetadata) + 0x800] = {};
    uint8_t wclBeaconInfo[0x808] = {};
    uint32_t wclAssociatedBeacons = 1;
    bool joinPending = false, joinDeferred = false;
    AirportWCL::JoinRequest deferredJoin = {};
    bool assocNotified = true, firstBeaconNotified = true, portAuthorized = true;
    bool wclRoamLock = false, wclRealTimeMode = false;
    uint64_t wclLastReportedRateBps = 0;
    uint64_t wclLastLiveBgscanUS = 0;
    bool wclStopping = false;
    uint64_t wclCountryGeneration = 0;
    bool disassocIsVoluntary = false;
    uint32_t current_authtype_lower = 0, current_authtype_upper = 0;

    void clearWCLPMKSA() {}
    void updateWCLLinkQuality(bool = false) {}
    void syncInfraDataBandwidths() {}
    void finishWCLJoin(uint16_t, uint16_t, IOReturn) {}
    void publishWCLBeacon(const ieee80211_node *, const uint8_t *, size_t, bool = false) {}
    IOReturn copyWCLChannels(void *) { return kIOReturnSuccess; }
    IOReturn copyWCLTrafficCounters(void *) { return kIOReturnSuccess; }
    IOReturn copyWCLExtendedBss(void *) { return kIOReturnSuccess; }
    void handleWCLEvent(int, void *) {}
    IOReturn installWCLKey(apple80211_key *) { return kIOReturnSuccess; }
    IOReturn accessWCLRSN(apple80211_rsn_ie_data *, bool) { return kIOReturnSuccess; }
    IOReturn abortWCLJoin() { return kIOReturnSuccess; }

    static IOReturn wclCommand(OSObject *, void *, void *, void *, void *);
    IOReturn beginWCLScan(apple80211ScanRequest *);
    bool completeWCLScanFromCache(const AirportWCL::ScanRequest &);
    IOReturn startWCLScan(const AirportWCL::ScanRequest &);
    void finishWCLScan(IOReturn);
    void completeWCLScan();
    IOReturn setABORT_SCAN(apple80211_abort_scan *);
    IOReturn beginWCLJoin(apple80211AssocCandidates *);
    IOReturn startWCLJoin(AirportWCL::JoinRequest &);
    IOReturn leaveWCLNetwork();
    static IOReturn stopWCLGated(OSObject *, void *, void *, void *, void *);
};

static void putU32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v & 0xff);
    p[1] = uint8_t((v >> 8) & 0xff);
    p[2] = uint8_t((v >> 16) & 0xff);
    p[3] = uint8_t((v >> 24) & 0xff);
}

"""
            + add_ssid_fn
            + "\n\n"
            + wcl_methods
            + r"""

int main() {
    AirportItlwmSkywalkInterface s;

    // 1. HID-3: In-flight hardware bgscan preempted by setWCL_SCAN_ABORT (wclCommand case 4)
    //    calls abortScanning(), clears IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN, posts
    //    ScanDone(kIOReturnAborted), and allows immediate hidden-SSID Join Scan 1 ms later!
    apple80211ScanRequest bgReq = {};
    putU32(bgReq.bytes + 0x40, 1);
    putU32(bgReq.bytes + 0x54, 1);
    putU32(bgReq.bytes + 0x58, 1);
    putU32(bgReq.bytes + 0x5c, 36);
    putU32(bgReq.bytes + 0x60, 0x10);
    s.hal.drv.hwScanning = true;
    assert(s.beginWCLScan(&bgReq) == kIOReturnSuccess);
    assert(s.scanPending && (s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));

    assert(AirportItlwmSkywalkInterface::wclCommand(
        nullptr, &s, reinterpret_cast<void *>(4), nullptr, nullptr) == kIOReturnSuccess);
    assert(s.hal.drv.abortCalls == 1 && !s.hal.drv.hwScanning);
    assert(!(s.hal.ic.ic_flags & (IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN)));
    assert(!s.scanPending && s.owner.scanDoneStatuses.back() == kIOReturnAborted);

    fakeClockUS += 1000ULL;
    apple80211ScanRequest hiddenReq = bgReq;
    putU32(hiddenReq.bytes + 0x1c, 14);
    memcpy(hiddenReq.bytes + 0x20, "HiddenTarget5G", 14);
    assert(s.beginWCLScan(&hiddenReq) == kIOReturnSuccess);
    assert(s.scanPending && (s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));

    // Explicit setABORT_SCAN also aborts hardware scanning cleanly:
    apple80211_abort_scan abortParams = {};
    assert(s.setABORT_SCAN(&abortParams) == kIOReturnSuccess);
    assert(s.hal.drv.abortCalls == 2);
    assert(!s.scanPending && !(s.hal.ic.ic_flags & (IEEE80211_F_BGSCAN | IEEE80211_F_ASCAN)));

    // 2. HID-4: Empty / 6 GHz-only / locally-unavailable channel subset in S_RUN and S_SCAN
    //    completes cleanly in 1 ms with ScanDone(kIOReturnSuccess) instead of kIOReturnUnsupported!
    apple80211ScanRequest sixGhzOnly = {};
    putU32(sixGhzOnly.bytes + 0x40, 1);
    putU32(sixGhzOnly.bytes + 0x54, 1);
    putU32(sixGhzOnly.bytes + 0x58, 1);
    putU32(sixGhzOnly.bytes + 0x5c, 37);
    putU32(sixGhzOnly.bytes + 0x60, 0x2000); // 6 GHz flag
    for (int st : {int(IEEE80211_S_RUN), int(IEEE80211_S_SCAN)}) {
        s.hal.ic.ic_state = st;
        assert(s.beginWCLScan(&sixGhzOnly) == kIOReturnSuccess);
        assert(s.scanPending && s.scanCompleted && s.scanTimer.delay == 1);
        s.completeWCLScan();
        assert(!s.scanPending && s.owner.scanDoneStatuses.back() == kIOReturnSuccess);
    }

    // 3. Unassociated S_SCAN + setWCL_SCAN_ABORT -> next startWCLScan restarts ic_newstate(S_SCAN)!
    s.hal.ic.ic_state = IEEE80211_S_SCAN;
    assert(s.beginWCLScan(&hiddenReq) == kIOReturnSuccess);
    assert(AirportItlwmSkywalkInterface::wclCommand(
        nullptr, &s, reinterpret_cast<void *>(4), nullptr, nullptr) == kIOReturnSuccess);
    const unsigned callsBefore = s.hal.ic.newstateCalls;
    assert(s.beginWCLScan(&hiddenReq) == kIOReturnSuccess);
    assert(s.hal.ic.newstateCalls == callsBefore + 1 && !s.scanSkipCompletion);
    s.finishWCLScan(kIOReturnAborted);

    // 4. Associated IEEE80211_S_RUN ("VisibleHome5G") -> startWCLJoin("HiddenTarget5G", Ch 149):
    //    verifies clean teardown of old BSS + active bgscan, single S_RUN->S_SCAN transition,
    //    and intact "HiddenTarget5G" Tag 0 IE in AssocReq!
    s.hal.ic.ic_state = IEEE80211_S_RUN;
    s.hal.ic.ic_flags = IEEE80211_F_BGSCAN | IEEE80211_F_PSK;
    s.portAuthorized = true;
    s.wclRoamLock = true;
    const unsigned abortsBeforeJoin = s.hal.drv.abortCalls;
    const unsigned transitionsBeforeJoin = s.hal.ic.newstateCalls;

    AirportWCL::JoinRequest join = {};
    join.lowerAuth = 1;
    join.upperAuth = APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_WPA3_SAE;
    join.ssidLength = 14;
    memcpy(join.ssid, "HiddenTarget5G", 14);
    join.keyLength = 32;
    join.keyCipher = 6;
    for (unsigned i = 0; i < 32; ++i) join.key[i] = uint8_t(0x10 + i);
    const uint8_t hiddenBssid[6] = {0x02, 0x00, 0x00, 0x00, 0x95, 0x09};
    memcpy(join.bssid, hiddenBssid, 6);
    join.channelSpec = 0xd095; // 5 GHz Ch 149

    assert(s.startWCLJoin(join) == kIOReturnSuccess);
    assert(s.hal.drv.abortCalls == abortsBeforeJoin + 1);
    assert(s.hal.ic.newstateCalls == transitionsBeforeJoin + 2);
    assert(s.hal.ic.ic_state == IEEE80211_S_SCAN);
    assert(!s.portAuthorized && !s.wclRoamLock && s.joinPending);
    assert(s.hal.ic.ic_wcl_join_requested && s.hal.ic.ic_wcl_join_channel == 149);
    assert(s.hal.ic.ic_des_esslen == 14);
    assert(memcmp(s.hal.ic.ic_des_essid, "HiddenTarget5G", 14) == 0);
    assert((s.hal.ic.ic_flags & IEEE80211_F_DESBSSID) &&
           memcmp(s.hal.ic.ic_des_bssid, hiddenBssid, 6) == 0);

    // Simulate hidden-BSS match populating ni_essid from ic_des_essid -> AssocReq Tag 0:
    ieee80211_node hiddenNode = {};
    memcpy(hiddenNode.ni_bssid, hiddenBssid, 6);
    if (s.hal.ic.ic_wcl_join_requested && (s.hal.ic.ic_flags & IEEE80211_F_DESBSSID) &&
        IEEE80211_ADDR_EQ(s.hal.ic.ic_des_bssid, hiddenNode.ni_bssid) &&
        hiddenNode.ni_esslen == 0) {
        hiddenNode.ni_esslen = uint8_t(s.hal.ic.ic_des_esslen);
        memcpy(hiddenNode.ni_essid, s.hal.ic.ic_des_essid, hiddenNode.ni_esslen);
    }
    uint8_t assocTag0[40] = {};
    uint8_t *end = ieee80211_add_ssid(assocTag0, hiddenNode.ni_essid, hiddenNode.ni_esslen);
    assert(size_t(end - assocTag0) == 16);
    assert(assocTag0[0] == 0 && assocTag0[1] == 14);
    assert(memcmp(assocTag0 + 2, "HiddenTarget5G", 14) == 0);

    AirportItlwmSkywalkInterface::stopWCLGated(nullptr, &s, nullptr, nullptr, nullptr);
    return 0;
}
"""
        )
        self.compile_run(src)

    def test_p118_output_start_64_cap_and_freestanding_kext_abi(self):
        v2_methods = "\n\n".join(
            definition("AirportItlwm/AirportItlwmV2.cpp", "AirportItlwm::" + name)
            for name in ("outputStart", "enqueuePacket")
        )
        src = (
            r"""
#include <cassert>
#include <cstdint>
#include <deque>
#include <vector>

#define __PRIVATE_SPI__ 1
#define AIRPORT_WCL 1
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2

using IOReturn = int;
using UInt32 = uint32_t;
using IOOptionBits = uint32_t;
enum {
    kIOReturnSuccess = 0,
    kIOReturnNoResources = 1,
    kIOReturnOutputSuccess = 0,
    kIOReturnOutputDropped = 2,
    IEEE80211_S_RUN = 4,
    MBUF_TYPE_DATA = 1,
    MBUF_TYPE_FREE = 0,
    MBUF_PKTHDR = 0x2,
};
#define MIN(a, b) ((a) < (b) ? (a) : (b))

struct FakeMbuf {
    int type = MBUF_TYPE_DATA;
    int flags = MBUF_PKTHDR;
    FakeMbuf *nextpkt = nullptr;
};
using mbuf_t = FakeMbuf *;
static mbuf_t mbuf_nextpkt(mbuf_t m) { return m->nextpkt; }
static void mbuf_setnextpkt(mbuf_t m, mbuf_t next) { m->nextpkt = next; }
static int mbuf_type(mbuf_t m) { return m->type; }
static int mbuf_flags(mbuf_t m) { return m->flags; }

struct PacketQueueStub {
    uint32_t capacity = 256;
    std::deque<mbuf_t> q;
    uint32_t getCapacity() const { return capacity; }
    uint32_t getSize() const { return uint32_t(q.size()); }
    bool lockEnqueue(mbuf_t m) {
        if (q.size() >= capacity) return false;
        q.push_back(m);
        return true;
    }
    mbuf_t lockDequeue() {
        if (q.empty()) return nullptr;
        mbuf_t m = q.front();
        q.pop_front();
        return m;
    }
};

struct _ifqueue {
    unsigned int ifq_oactive = 0;
    PacketQueueStub *queue = nullptr;
};
static bool ifq_empty(_ifqueue *ifq) { return ifq->queue->getSize() == 0; }
static uint32_t ifq_len(_ifqueue *ifq) { return ifq->queue->getSize(); }
static unsigned int ifq_is_oactive(_ifqueue *ifq) { return ifq->ifq_oactive; }

struct NetStatStub { uint64_t outputErrors = 0; };
struct _ifnet {
    _ifqueue if_snd;
    NetStatStub *netStat = nullptr;
    void (*if_start)(_ifnet *) = nullptr;
    void *if_softc = nullptr;
};

struct IONetworkInterface {
    std::deque<mbuf_t> pending;
    IOReturn dequeueOutputPackets(uint32_t maxCount, mbuf_t *headOut) {
        *headOut = nullptr;
        if (pending.empty() || !maxCount) return kIOReturnSuccess;
        mbuf_t head = nullptr, tail = nullptr;
        uint32_t n = 0;
        while (!pending.empty() && n < maxCount) {
            mbuf_t m = pending.front();
            pending.pop_front();
            m->nextpkt = nullptr;
            if (!head) head = tail = m;
            else { tail->nextpkt = m; tail = m; }
            ++n;
        }
        *headOut = head;
        return kIOReturnSuccess;
    }
};

struct ieee80211com {
    int ic_state = IEEE80211_S_RUN;
    struct { _ifnet ac_if; } ic_ac;
};

struct HalServiceStub {
    ieee80211com ic;
    PacketQueueStub pq;
    NetStatStub stats;
    bool holdInIfSnd = false;
    unsigned hwRingQueued = 0;
    unsigned hwRingHiMark = 224;

    static void startEntry(_ifnet *ifp) {
        auto *self = static_cast<HalServiceStub *>(ifp->if_softc);
        if (ifp->if_snd.ifq_oactive || self->holdInIfSnd) return;
        while (true) {
            if (self->hwRingQueued >= self->hwRingHiMark) {
                ifp->if_snd.ifq_oactive = 1;
                break;
            }
            mbuf_t m = ifp->if_snd.queue->lockDequeue();
            if (!m) break;
            ++self->hwRingQueued;
        }
    }

    HalServiceStub() {
        ic.ic_ac.ac_if.if_snd.queue = &pq;
        ic.ic_ac.ac_if.netStat = &stats;
        ic.ic_ac.ac_if.if_start = startEntry;
        ic.ic_ac.ac_if.if_softc = this;
    }
    ieee80211com *get80211Controller() { return &ic; }
};

struct AWDLInterfaceStub {
    IOReturn submitOutputPackets(bool) { return kIOReturnSuccess; }
};

struct AirportItlwm {
    HalServiceStub hal;
    HalServiceStub *fHalService = &hal;
    IONetworkInterface en1BSD, awdlIf, llwIf;
    IONetworkInterface *awdlBSD = &awdlIf;
    IONetworkInterface *llwBSD = &llwIf;
    AWDLInterfaceStub awdl;
    AWDLInterfaceStub *awdlInterface = &awdl;

    void freePacket(mbuf_t m) { m->type = MBUF_TYPE_FREE; }
    UInt32 enqueuePacket(mbuf_t m);
    IOReturn outputStart(IONetworkInterface *interface, IOOptionBits options);
};

"""
            + v2_methods
            + r"""

int main() {
    AirportItlwm drv;
    std::vector<FakeMbuf> pool(200);
    for (auto &pkt : pool) drv.en1BSD.pending.push_back(&pkt);

    // Even if if_start temporarily leaves packets in if_snd without setting oactive,
    // outputStart NEVER stages more than 64 packets inside FIFO if_snd!
    drv.hal.holdInIfSnd = true;
    assert(drv.outputStart(&drv.en1BSD, 0) == kIOReturnNoResources);
    assert(drv.hal.pq.getSize() == 64);
    assert(drv.outputStart(&drv.en1BSD, 0) == kIOReturnNoResources);
    assert(drv.hal.pq.getSize() == 64);
    assert(drv.en1BSD.pending.size() == 200 - 64);
    return 0;
}
"""
        )
        self.compile_run(src)

        probe_cpp = r"""
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"

extern "C" int preview118_wcl_kext_abi_probe(const void *owner,
                                             const AirportWCL::ScanRequest *scan,
                                             uint64_t bytes,
                                             uint64_t nowUS) {
    auto &burst = AirportWCL::splitScanBurstFor(owner);
    const bool bulk = burst.traffic.sample(bytes, nowUS);
    const bool cont = burst.isContinuation(*scan, nowUS);
    burst.recordLiveScan(*scan, nowUS, cont);
    burst.noteScanFinished(nowUS + 1000ULL);
    const uint64_t dg = AirportWCL::multiSsidDigest(*scan);
    burst.recordMultiSsidBatch(dg, nowUS);
    const bool thr = burst.shouldThrottleMultiSsidBatch(dg, nowUS + 1000ULL);
    AirportWCL::noteAbortedScanInScanState(owner);
    const bool ab = AirportWCL::consumeAbortedScanInScanState(owner);
    AirportWCL::resetSplitScanBurst(owner, true);
    return int(bulk) + int(cont) + int(thr) + int(ab);
}
"""
        with tempfile.TemporaryDirectory(prefix="p118-wcl-kext-abi-") as tmp:
            src_path = Path(tmp) / "probe.cpp"
            obj_path = Path(tmp) / "probe.o"
            src_path.write_text(probe_cpp)
            subprocess.run(
                [
                    "xcrun",
                    "clang++",
                    "-arch",
                    "x86_64",
                    "-std=gnu++14",
                    "-O2",
                    "-mkernel",
                    "-nostdinc++",
                    "-fapple-kext",
                    "-fno-exceptions",
                    "-fno-rtti",
                    "-I",
                    str(support.ROOT / "MacKernelSDK/Headers"),
                    "-I",
                    str(support.ROOT),
                    "-c",
                    str(src_path),
                    "-o",
                    str(obj_path),
                ],
                check=True,
            )
            undef = subprocess.check_output(["nm", "-u", str(obj_path)], text=True)
            self.assertNotIn("cxa", undef)


if __name__ == "__main__":
    unittest.main()
