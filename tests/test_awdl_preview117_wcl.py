"""WCL hidden-SSID discovery/join and en1 outputStart queue regression suite:
1. completeWCLScanFromCache / shouldThrottleAssociatedBgscan never short-circuits
   user-initiated directed-SSID scans or active split-scan bursts while preserving
   continuityMediaActive() protection on undirected scans.
2. startWCLScan preserves directed SSIDs and sets ic_wcl_scan_active = true whenever
   AirportWCL::hasDirectedSSID(scan) holds (including scan.type == 2 on 5 GHz LAR-passive subsets),
   leaving copyWCLChannels untouched.
3. publishWCLBeacon (wired to event.isprobe in handleWCLEvent) sets
   BeaconFlags::ProbeResponse (0x1) on Probe Responses, omits SSIDPresent (0x2) on empty/all-NUL
   SSIDs, and splices known node->ni_essid Tag 0 IEs into subsequent hidden Beacons.
4. classifyLegacyAuth and startWCLJoin accept WPA3-Transition Personal profiles
   (Personal bit + extra WPA3/SAE upperAuth bits + 32-byte PSK) while rejecting pure SAE-only.
5. AirportItlwm::outputStart enforces immediate ifq_is_oactive(&ifp->if_snd) backpressure and
   bounds per-turn dequeue/staging at 64 packets so Skywalk FQ_CODEL retains AQM control.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

import test_regressions as support
from test_offline import definition


class Preview117WCLRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p117_wcl_directed_scan_cache_bypass_and_7chunk_split_scan_continuation(self):
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
    int ni_rssi = 56; // -100 + 56 = -44 dBm
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
};
struct AWDLStub {
    bool media = false;
    bool continuityMediaActive() const { return media; }
};
struct InstanceStub {
    AWDLStub awdl;
    AWDLStub *awdlInterface = &awdl;
    unsigned scanResults = 0;
    unsigned scanDones = 0;
    void postMessage(void *, unsigned code, const void *, size_t, bool) {
        if (code == AirportWCL::ScanResult) ++scanResults;
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

static apple80211ScanRequest makeChunkRequest(const std::vector<unsigned> &chans,
                                              const char *directedSsid = nullptr,
                                              bool viaMultiList = false) {
    apple80211ScanRequest req = {};
    putU32(req.bytes + 0x40, 1); // Active scan
    putU32(req.bytes + 0x54, uint32_t(chans.size()));
    for (size_t i = 0; i < chans.size(); ++i) {
        uint8_t *entry = req.bytes + 0x58 + i * 12;
        putU32(entry + 0, 1);
        putU32(entry + 4, chans[i]);
        putU32(entry + 8, chans[i] <= 14 ? 0x8U : 0x10U);
    }
    if (directedSsid) {
        const uint32_t len = uint32_t(strlen(directedSsid));
        if (viaMultiList) {
            putU32(req.bytes + 0x1318, 2); // directed + wildcard
            putU32(req.bytes + 0x131c + 4, len);
            memcpy(req.bytes + 0x131c + 8, directedSsid, len);
        } else {
            putU32(req.bytes + 0x1c, len);
            memcpy(req.bytes + 0x20, directedSsid, len);
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
    cached.ssidLength = 7;
    memcpy(cached.ssid, "HomeLab", 7);
    memcpy(cached.bssid, s.joinBSSID, 6);
    memcpy(s.wclBssInfo, &cached, sizeof(cached));
    s.wclBssValid = true;

    const std::vector<std::vector<unsigned>> sevenChunks = {
        {1, 2, 3, 4, 5, 6},
        {7, 8, 9, 10, 11, 12},
        {13, 36, 40, 44, 48, 52},
        {56, 60, 64, 100, 104, 108},
        {112, 116, 120, 124, 128, 132},
        {136, 140, 144, 149, 153, 157},
        {161, 165},
    };

    // 1. Unthrottled 7-chunk split-scan burst (<= 2.5 s from Chunk #1): ALL 7 chunks run live!
    fakeClockUS = 100000000ULL;
    liveBgscanStarts = 0;
    for (size_t idx = 0; idx < sevenChunks.size(); ++idx) {
        auto req = makeChunkRequest(sevenChunks[idx]);
        assert(s.beginWCLScan(&req) == kIOReturnSuccess);
        assert(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN);
        assert(s.scanTimer.delay == 20000);
        s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
        s.scanCompleted = true;
        s.completeWCLScan();
        fakeClockUS += 350000ULL; // +350 ms per chunk (total 2.10 s <= 2.5 s)
    }
    assert(liveBgscanStarts == 7);
    assert(s.wclLastLiveBgscanUS == 100000000ULL);

    // 2. Duplicate same-channel AutoJoin scan 14 ms later (already in coveredChannels) MUST throttle!
    fakeClockUS += 14000ULL;
    {
        auto dupReq = makeChunkRequest(sevenChunks[0]);
        assert(s.beginWCLScan(&dupReq) == kIOReturnSuccess);
        assert(!(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
        assert(s.scanCompleted && s.scanTimer.delay == 1);
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == 7);

    // 3. Broadcast non-directed split scan 6.2 s later (> 2.5 s burst window, < 45 s cooldown) MUST throttle!
    fakeClockUS = 106200000ULL;
    for (const auto &chunk : sevenChunks) {
        auto bgReq = makeChunkRequest(chunk);
        assert(s.beginWCLScan(&bgReq) == kIOReturnSuccess);
        assert(!(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
        assert(s.scanCompleted && s.scanTimer.delay == 1);
        s.completeWCLScan();
    }
    assert(liveBgscanStarts == 7);

    // 4. User-initiated directed hidden-SSID 7-chunk scans at +6.2 s (single SSID) and +15.0 s
    //    (multi-SSID table) MUST bypass cache on ALL 7/7 chunks!
    for (bool multi : {false, true}) {
        fakeClockUS = multi ? 115000000ULL : 106200000ULL;
        const unsigned before = liveBgscanStarts;
        for (const auto &chunk : sevenChunks) {
            auto dirReq = makeChunkRequest(chunk, "HiddenLab5G", multi);
            assert(s.beginWCLScan(&dirReq) == kIOReturnSuccess);
            assert(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN);
            assert(s.scanTimer.delay == 20000);
            s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
            s.scanCompleted = true;
            s.completeWCLScan();
            fakeClockUS += 350000ULL;
        }
        assert(liveBgscanStarts == before + 7);
    }

    // 5. Active Continuity media MUST short-circuit undirected scans from cache in 1 ms,
    //    while user-initiated directed hidden-SSID scans run live even under wclRoamLock,
    //    wclRealTimeMode, or continuityMediaActive()!
    {
        fakeClockUS = 170000000ULL;
        auto undirReq = makeChunkRequest(sevenChunks[2]);
        const unsigned before = liveBgscanStarts;
        s.owner.awdl.media = true;
        assert(s.beginWCLScan(&undirReq) == kIOReturnSuccess);
        assert(!(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN) && s.scanTimer.delay == 1);
        s.completeWCLScan();
        assert(liveBgscanStarts == before);

        auto dirReq = makeChunkRequest(sevenChunks[2], "HiddenLab5G", false);
        s.wclRoamLock = true;
        s.wclRealTimeMode = true;
        assert(s.beginWCLScan(&dirReq) == kIOReturnSuccess);
        assert((s.hal.ic.ic_flags & IEEE80211_F_BGSCAN) && s.scanTimer.delay == 20000);
        s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
        s.scanCompleted = true;
        s.completeWCLScan();
        s.owner.awdl.media = false;
        s.wclRoamLock = false;
        s.wclRealTimeMode = false;
        assert(liveBgscanStarts == before + 1);
    }

    // 6. Lifecycle teardown resets wclLastLiveBgscanUS and SplitScanBurst slot cleanly.
    s.handleWCLLink(false);
    assert(s.wclLastLiveBgscanUS == 0);
    assert(!AirportWCL::splitScanBurstFor(&s).startUS);
    AirportItlwmSkywalkInterface::stopWCLGated(nullptr, &s, nullptr, nullptr, nullptr);
    return 0;
}
"""
        )
        self.compile_run(src)

    def test_p117_wcl_passive_5ghz_scan_type2_preserves_directed_hidden_ssids(self):
        wcl_src = (support.ROOT / "tests/fixtures/wcl_lifecycle.cpp").read_text().rsplit("int main()", 1)[0]
        scan_fns = "\n\n".join(
            definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::" + name)
            for name in (
                "beginWCLScan",
                "startWCLScan",
                "resumeWCLScan",
                "finishWCLScan",
                "completeWCLScan",
                "handleWCLAvailability",
                "resumeWCLJoin",
            )
        )
        wcl_src = wcl_src.replace("// PRODUCTION: scan\n", scan_fns + "\n")
        wcl_src += r"""
static void putU32(uint8_t *p, uint32_t v) {
    p[0] = uint8_t(v & 0xff);
    p[1] = uint8_t((v >> 8) & 0xff);
    p[2] = uint8_t((v >> 16) & 0xff);
    p[3] = uint8_t((v >> 24) & 0xff);
}

int main() {
    // 1. Boot Auto-Join on 5 GHz passive subset (36, 48, 161 -> scan.type == 2) with
    //    directed hidden SSID via both +0x1c/+0x20 and +0x1318/+0x131c MUST keep
    //    ic_wcl_scan_active = true and preserve SSID lengths/counts!
    {
        AirportItlwmSkywalkInterface s;
        s.hal.ic.ic_state = IEEE80211_S_INIT;
        s.hal.ic.ic_channels[48].ic_flags = 1;
        s.hal.ic.ic_channels[161].ic_flags = 1;

        apple80211ScanRequest req = {};
        putU32(req.bytes + 0x40, 2); // Passive scan type emitted on 5 GHz LAR channels
        putU32(req.bytes + 0x1c, 11);
        memcpy(req.bytes + 0x20, "HiddenLab5G", 11);
        putU32(req.bytes + 0x54, 3);
        const unsigned chans[3] = {48, 36, 161};
        for (unsigned i = 0; i < 3; ++i) {
            uint8_t *entry = req.bytes + 0x58 + i * 12;
            putU32(entry + 0, 1);
            putU32(entry + 4, chans[i]);
            putU32(entry + 8, 0x10U);
        }
        putU32(req.bytes + 0x1318, 2);
        putU32(req.bytes + 0x131c + 4, 11);
        memcpy(req.bytes + 0x131c + 8, "HiddenLab5G", 11);

        assert(s.beginWCLScan(&req) == kIOReturnSuccess);
        s.resumeWCLScan();
        assert(s.hal.ic.ic_wcl_scan_requested);
        assert(s.hal.ic.ic_wcl_scan_active);
        assert(s.hal.ic.ic_wcl_scan_ssid_length == 11);
        assert(memcmp(s.hal.ic.ic_wcl_scan_ssid, "HiddenLab5G", 11) == 0);
        assert(s.hal.ic.ic_wcl_scan_ssid_count == 2);
        assert(s.hal.ic.ic_wcl_scan_ssid_lengths[0] == 11);
        assert(memcmp(s.hal.ic.ic_wcl_scan_ssids[0], "HiddenLab5G", 11) == 0);
    }

    // 2. Non-directed passive scan (scan.type == 2 with wildcard/empty or all-NUL SSIDs)
    //    MUST remain strictly passive (ic_wcl_scan_active == false, counts zeroed).
    {
        AirportItlwmSkywalkInterface s;
        apple80211ScanRequest passiveWildcard = {};
        putU32(passiveWildcard.bytes + 0x40, 2);
        putU32(passiveWildcard.bytes + 0x1318, 2);
        putU32(passiveWildcard.bytes + 0x131c + 4, 0); // wildcard
        putU32(passiveWildcard.bytes + 0x131c + 56 + 4, 4); // 4 NUL bytes (hidden placeholder)

        assert(s.beginWCLScan(&passiveWildcard) == kIOReturnSuccess);
        assert(!s.hal.ic.ic_wcl_scan_active);
        assert(s.hal.ic.ic_wcl_scan_ssid_length == 0);
        assert(s.hal.ic.ic_wcl_scan_ssid_count == 0);
    }
    return 0;
}
"""
        self.compile_run(wcl_src)

        # Verify copyWCLChannels remains untouched per Preview 117 guardrail.
        copy_fn = definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::copyWCLChannels")
        self.assertIn("channel->ic_flags & IEEE80211_CHAN_PASSIVE", copy_fn)
        self.assertNotIn("ic_beacon_hint", copy_fn)

    def test_p117_wcl_publish_beacon_probe_flag_nul_suppression_and_tag0_splicing(self):
        publish_fn = definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::publishWCLBeacon")
        wcl_cpp = (support.ROOT / "AirportItlwm/WCL/WCL.cpp").read_text()
        self.assertIn("publishWCLBeacon(beacon->node, beacon->ies, beacon->length, beacon->isprobe != 0);", wcl_cpp)

        src = (
            r"""
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

#define AIRPORT_WCL 1
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"

enum { IWM_MIN_DBM = -100 };
#define IEEE80211_CHAN_ANYC reinterpret_cast<const ieee80211_channel *>(-1)
#define IEEE80211_ADDR_EQ(a, b) (memcmp((a), (b), 6) == 0)
#define bzero(p, n) memset((p), 0, (n))

static void *IOMalloc(size_t n) { return malloc(n); }
static void IOFree(void *p, size_t) { free(p); }

struct ieee80211_channel { bool fiveGHz = true; };
struct ieee80211com { ieee80211_channel ch48; };
static unsigned ieee80211_chan2ieee(ieee80211com *, const ieee80211_channel *) { return 48; }
static bool IEEE80211_IS_CHAN_5GHZ(const ieee80211_channel *c) { return c->fiveGHz; }

struct ieee80211_node {
    const ieee80211_channel *ni_chan = nullptr;
    uint8_t ni_esslen = 0;
    uint8_t ni_essid[32] = {};
    uint8_t ni_bssid[6] = {0x02, 0x00, 0x00, 0x00, 0x20, 0x48};
    int ni_rssi = 45; // -55 dBm
    uint16_t ni_intval = 100;
    uint16_t ni_capinfo = 0x0431;
};

struct HalStub {
    ieee80211com ic;
    ieee80211com *get80211Controller() { return &ic; }
};

struct CapturedScanResult {
    AirportWCL::BeaconMetadata meta;
    std::vector<uint8_t> ies;
};

struct InstanceStub {
    std::vector<CapturedScanResult> results;
    void postMessage(void *, unsigned code, const void *data, size_t len, bool toWcl) {
        assert(code == AirportWCL::ScanResult && toWcl);
        assert(len >= sizeof(AirportWCL::BeaconMetadata));
        CapturedScanResult r = {};
        memcpy(&r.meta, data, sizeof(r.meta));
        const auto *bytes = static_cast<const uint8_t *>(data) + sizeof(r.meta);
        r.ies.assign(bytes, bytes + (len - sizeof(r.meta)));
        assert(r.meta.ieLength == r.ies.size());
        results.push_back(r);
    }
};

struct AirportItlwmSkywalkInterface {
    HalStub hal;
    HalStub *fHalService = &hal;
    InstanceStub owner;
    InstanceStub *instance = &owner;
    bool assocNotified = false;
    bool scanPending = false;
    uint8_t joinBSSID[6] = {0x02, 0x00, 0x00, 0x00, 0x20, 0x48};
    bool wclBssValid = false;
    uint8_t wclBssInfo[sizeof(AirportWCL::BeaconMetadata) + 0x800] = {};
    AirportWCL::ScanRequest wclScanRequest = {};

    void publishWCLBeacon(const ieee80211_node *node, const uint8_t *ies, size_t length, bool isProbe = false);
};

"""
            + publish_fn
            + r"""

// Build WPA3-Transition RSN IE (AKM 00-0f-ac:2 PSK + 00-0f-ac:8 SAE, PMF capable 0x0080) + DS IE.
static std::vector<uint8_t> makeTransitionTailIEs() {
    return {
        3, 1, 48, // DS Parameter Set: channel 48
        48, 24,   // RSN IE (24 bytes body)
        1, 0,     // Version 1
        0x00, 0x0f, 0xac, 4, // Group cipher: CCMP
        1, 0, 0x00, 0x0f, 0xac, 4, // Pairwise cipher: CCMP
        2, 0, 0x00, 0x0f, 0xac, 2, 0x00, 0x0f, 0xac, 8, // AKMs: PSK + SAE
        0x80, 0x00 // RSN Capabilities: MFPC=1, MFPR=0
    };
}

int main() {
    AirportItlwmSkywalkInterface s;
    ieee80211_node node = {};
    node.ni_chan = &s.hal.ic.ch48;
    const auto tail = makeTransitionTailIEs();

    // 1. Unrevealed hidden Beacon with all-NUL SSID (ignore_broadcast_ssid=2):
    //    MUST omit SSIDPresent (0x2), omit ProbeResponse (0x1), and set ssidLength = 0!
    node.ni_esslen = 11;
    memset(node.ni_essid, 0, 11);
    std::vector<uint8_t> nulBeacon = {0, 11, 0,0,0,0,0,0,0,0,0,0,0};
    nulBeacon.insert(nulBeacon.end(), tail.begin(), tail.end());
    s.publishWCLBeacon(&node, nulBeacon.data(), nulBeacon.size(), false);
    assert(s.owner.results.size() == 1);
    assert(s.owner.results.back().meta.ssidLength == 0);
    assert(s.owner.results.back().meta.flags == AirportWCL::RSSIValid);

    // 2. Directed Probe Response (isProbe = true) reveals "HiddenLab5G":
    //    MUST set ProbeResponse | SSIDPresent | RSSIValid (0x4003) and convert RSN PSK+SAE -> PSK!
    const char kHiddenName[] = "HiddenLab5G";
    memcpy(node.ni_essid, kHiddenName, 11);
    node.ni_esslen = 11;
    std::vector<uint8_t> probeResp = {0, 11};
    probeResp.insert(probeResp.end(), kHiddenName, kHiddenName + 11);
    probeResp.insert(probeResp.end(), tail.begin(), tail.end());
    s.assocNotified = true;
    s.publishWCLBeacon(&node, probeResp.data(), probeResp.size(), true);
    assert(s.owner.results.size() == 2);
    const CapturedScanResult pr = s.owner.results.back();
    assert(pr.meta.ssidLength == 11 && memcmp(pr.meta.ssid, "HiddenLab5G", 11) == 0);
    assert(pr.meta.flags == (AirportWCL::ProbeResponse | AirportWCL::SSIDPresent | AirportWCL::RSSIValid));
    assert(pr.ies.size() == probeResp.size() - 4); // SAE AKM removed on scan copy
    assert(pr.ies[0] == 0 && pr.ies[1] == 11 && memcmp(pr.ies.data() + 2, "HiddenLab5G", 11) == 0);
    assert(AirportWCL::validIEs(pr.ies.data(), pr.ies.size()));

    // 3. Subsequent 100 ms hidden Beacons (empty Tag 0, all-NUL Tag 0, and missing Tag 0) with
    //    isProbe = false MUST splice "HiddenLab5G" into Tag 0 so IO80211BSSBeacon never reverts +0x52c!
    std::vector<uint8_t> emptyTag0 = {0, 0};
    emptyTag0.insert(emptyTag0.end(), tail.begin(), tail.end());

    for (const auto &hiddenFrame : {emptyTag0, nulBeacon, tail}) {
        s.owner.results.clear();
        s.publishWCLBeacon(&node, hiddenFrame.data(), hiddenFrame.size(), false);
        assert(s.owner.results.size() == 1);
        const auto &bcn = s.owner.results.back();
        assert(bcn.meta.ssidLength == 11 && memcmp(bcn.meta.ssid, "HiddenLab5G", 11) == 0);
        assert(bcn.meta.flags == (AirportWCL::SSIDPresent | AirportWCL::RSSIValid));
        assert(bcn.ies == pr.ies);
        assert(s.wclBssValid);
        AirportWCL::BeaconMetadata cachedMeta = {};
        memcpy(&cachedMeta, s.wclBssInfo, sizeof(cachedMeta));
        assert(cachedMeta.ssidLength == 11 && memcmp(cachedMeta.ssid, "HiddenLab5G", 11) == 0);
        assert(memcmp(s.wclBssInfo + sizeof(cachedMeta), pr.ies.data(), pr.ies.size()) == 0);
    }
    return 0;
}
"""
        )
        self.compile_run(src)

    def test_p117_wcl_wpa3_transition_psk_auth_and_start_wcl_join(self):
        join_fn = definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::startWCLJoin")
        src = (
            r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#define AIRPORT_WCL 1
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"

using IOReturn = int;
enum {
    kIOReturnSuccess = 0,
    kIOReturnBadArgument = 1,
    kIOReturnBusy = 2,
    kIOReturnUnsupported = 3,
    kIOReturnNoMemory = 4,
    kIOReturnError = 5
};
enum {
    IFF_UP = 1,
    IFF_RUNNING = 2,
    ENETRESET = 52,
    IEEE80211_PMK_LEN = 32,
    IEEE80211_ELEMID_RSN = 48,
    IEEE80211_NWKEY_WEP = 1,
    IEEE80211_S_INIT = 0,
    IEEE80211_S_SCAN = 1,
    IEEE80211_S_AUTH = 2,
    IEEE80211_S_ASSOC = 3,
    IEEE80211_S_RUN = 4,
    IEEE80211_F_AUTO_JOIN = 1 << 0,
    IEEE80211_F_PSK = 1 << 1,
    IEEE80211_F_DESBSSID = 1 << 2,
    IEEE80211_WPA_PROTO_WPA1 = 1 << 0,
    IEEE80211_WPA_PROTO_WPA2 = 1 << 1,
    IEEE80211_WPA_AKM_PSK = 1 << 0,
    IEEE80211_WPA_AKM_8021X = 1 << 1,
    IEEE80211_WPA_AKM_SHA256_PSK = 1 << 2,
    IEEE80211_WPA_AKM_SHA256_8021X = 1 << 3,
    IEEE80211_WPA_CIPHER_TKIP = 1 << 0,
    IEEE80211_WPA_CIPHER_CCMP = 1 << 1,
    IEEE80211_AKM_8021X = 1,
    IEEE80211_AKM_SHA256_8021X = 2,
    APPLE80211_AUTHTYPE_WPA = 1 << 0,
    APPLE80211_AUTHTYPE_WPA_PSK = 1 << 1,
    APPLE80211_AUTHTYPE_WPA2 = 1 << 2,
    APPLE80211_AUTHTYPE_WPA2_PSK = 1 << 3,
    APPLE80211_AUTHTYPE_FT_PSK = 1 << 4,
    APPLE80211_AUTHTYPE_SHA256_PSK = 1 << 10,
    APPLE80211_AUTHTYPE_SHA256_8021X = 1 << 11,
    APPLE80211_AUTHTYPE_WPA3_SAE = 1 << 12,
    APPLE80211_AUTHTYPE_WPA3_FT_SAE = 1 << 13,
};

static void explicit_bzero(void *p, size_t n) { memset(p, 0, n); }
#define bzero(p, n) memset((p), 0, (n))
#define isset(p, b) ((p)[(b) / 8] & (1U << ((b) % 8)))

struct ieee80211_wpaparams {
    int i_enabled;
    unsigned i_protos, i_akms, i_ciphers, i_groupcipher;
};
struct ieee80211_nwkey {
    int i_wepon, i_defkid;
    struct { uint32_t i_keylen; const uint8_t *i_keydat; } i_key[4];
};
struct ieee80211com {
    struct { int if_flags = IFF_UP | IFF_RUNNING; } ic_if;
    int ic_state = IEEE80211_S_INIT;
    unsigned ic_flags = 0;
    bool ic_wcl_mac_reconfig = false, ic_wcl_enterprise = false;
    bool ic_wcl_join_requested = false, ic_wcl_scan_requested = false;
    uint8_t ic_wcl_join_channel = 0;
    uint8_t ic_chan_active[32] = {};
    struct { int ic_flags = 0; } ic_channels[256] = {};
    uint32_t ic_des_esslen = 0;
    uint8_t ic_des_essid[32] = {}, ic_des_bssid[6] = {}, ic_psk[32] = {};
    ieee80211_wpaparams lastWpa = {};
    int (*ic_newstate)(ieee80211com *, int, int) = nullptr;
};

static int newState(ieee80211com *ic, int st, int) { ic->ic_state = st; return 0; }
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

struct TimerStub {
    int delay = -1;
    void setTimeoutMS(int ms) { delay = ms; }
    void cancelTimeout() { delay = -1; }
};
struct HalStub {
    ieee80211com ic;
    HalStub() {
        ic.ic_newstate = newState;
        ic.ic_channels[48].ic_flags = 1;
        ic.ic_chan_active[48 / 8] |= 1U << (48 % 8);
    }
    ieee80211com *get80211Controller() { return &ic; }
};

struct AirportItlwmSkywalkInterface {
    HalStub hal;
    HalStub *fHalService = &hal;
    TimerStub jt;
    TimerStub *joinTimer = &jt;
    AirportWCL::JoinRequest deferredJoin = {};
    uint8_t joinBSSID[6] = {};
    bool joinDeferred = false, joinPending = false, scanPending = false;
    bool wclBssValid = false, assocNotified = false, firstBeaconNotified = false, portAuthorized = false;
    uint32_t wclAssociatedBeacons = 0;
    uint8_t wclBeaconInfo[0x808] = {};
    bool disassocIsVoluntary = false;
    uint32_t current_authtype_lower = 0, current_authtype_upper = 0;

    void leaveWCLNetwork() { hal.ic.ic_state = IEEE80211_S_SCAN; joinPending = false; }
    void clearWCLPMKSA() {}
    IOReturn startWCLJoin(AirportWCL::JoinRequest &join);
};

"""
            + join_fn
            + r"""

static AirportWCL::JoinRequest makePersonalJoin(uint32_t upperAuth, uint32_t keyLen = 32, uint32_t cipher = 6) {
    AirportWCL::JoinRequest j = {};
    j.lowerAuth = 1;
    j.upperAuth = upperAuth;
    j.ssidLength = 11;
    memcpy(j.ssid, "HiddenLab5G", 11);
    j.keyLength = keyLen;
    j.keyCipher = cipher;
    for (unsigned i = 0; i < 32; ++i) j.key[i] = uint8_t(0xa0 + i);
    const uint8_t bssid[6] = {0x02, 0x00, 0x00, 0x00, 0x48, 0x01};
    memcpy(j.bssid, bssid, 6);
    j.channelSpec = 0xd030; // 5 GHz channel 48
    return j;
}

int main() {
    // 1. WPA3-Transition Personal profiles (WPA2_PSK | WPA3_SAE, + FT_PSK / SHA256_PSK / FT_SAE)
    //    with a 32-byte PSK key MUST classify as Personal and succeed in startWCLJoin!
    for (uint32_t mask : {
            uint32_t(APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_WPA3_SAE),
            uint32_t(APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_SHA256_PSK |
                     APPLE80211_AUTHTYPE_WPA3_SAE | APPLE80211_AUTHTYPE_WPA3_FT_SAE |
                     APPLE80211_AUTHTYPE_FT_PSK),
            uint32_t(APPLE80211_AUTHTYPE_SHA256_PSK | APPLE80211_AUTHTYPE_WPA3_SAE)}) {
        auto j = makePersonalJoin(mask);
        assert(AirportWCL::classifyLegacyAuth(j) == AirportWCL::LegacyAuth::Personal);
        AirportItlwmSkywalkInterface s;
        assert(s.startWCLJoin(j) == kIOReturnSuccess);
        assert(s.joinPending);
        assert(s.hal.ic.ic_wcl_join_requested && s.hal.ic.ic_wcl_join_channel == 48);
        assert(s.hal.ic.ic_des_esslen == 11 && memcmp(s.hal.ic.ic_des_essid, "HiddenLab5G", 11) == 0);
        assert(s.hal.ic.ic_flags & IEEE80211_F_PSK);
        assert(s.hal.ic.ic_psk[0] == 0xa0 && s.hal.ic.ic_psk[31] == 0xbf);
        assert(s.hal.ic.lastWpa.i_protos & IEEE80211_WPA_PROTO_WPA2);
        assert(s.hal.ic.lastWpa.i_akms & IEEE80211_WPA_AKM_PSK);
    }

    // 2. Pure WPA3-SAE-only, missing/short PSK, mixed Personal+Enterprise, and unknown upperAuth
    //    bits MUST still be rejected!
    for (auto bad : {
            makePersonalJoin(APPLE80211_AUTHTYPE_WPA3_SAE, 32, 6),
            makePersonalJoin(APPLE80211_AUTHTYPE_WPA3_FT_SAE, 32, 6),
            makePersonalJoin(APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_WPA3_SAE, 0, 6),
            makePersonalJoin(APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_WPA3_SAE, 16, 6),
            makePersonalJoin(APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_WPA2, 32, 6),
            makePersonalJoin(APPLE80211_AUTHTYPE_WPA2_PSK | (1U << 20), 32, 6)}) {
        assert(AirportWCL::classifyLegacyAuth(bad) == AirportWCL::LegacyAuth::Invalid);
        AirportItlwmSkywalkInterface s;
        assert(s.startWCLJoin(bad) == kIOReturnUnsupported);
    }
    return 0;
}
"""
        )
        self.compile_run(src)

    def test_p117_output_start_uses_full_2048_if_snd_capacity_when_oactive(self):
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
    uint32_t capacity = 2048;
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
    unsigned startCalls = 0;
    unsigned hwRingQueued = 0;
    unsigned hwRingHiMark = 224;
    std::vector<mbuf_t> transmitted;

    static void startEntry(_ifnet *ifp) {
        auto *self = static_cast<HalServiceStub *>(ifp->if_softc);
        ++self->startCalls;
        if (ifp->if_snd.ifq_oactive) return;
        while (true) {
            if (self->hwRingQueued >= self->hwRingHiMark) {
                ifp->if_snd.ifq_oactive = 1;
                break;
            }
            mbuf_t m = ifp->if_snd.queue->lockDequeue();
            if (!m) break;
            ++self->hwRingQueued;
            self->transmitted.push_back(m);
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
    unsigned awdlCalls = 0, llwCalls = 0;
    IOReturn submitOutputPackets(bool llw) {
        if (llw) ++llwCalls; else ++awdlCalls;
        return kIOReturnSuccess;
    }
};

struct AirportItlwm {
    HalServiceStub hal;
    HalServiceStub *fHalService = &hal;
    IONetworkInterface en1BSD, awdlIf, llwIf;
    IONetworkInterface *awdlBSD = &awdlIf;
    IONetworkInterface *llwBSD = &llwIf;
    AWDLInterfaceStub awdl;
    AWDLInterfaceStub *awdlInterface = &awdl;
    unsigned freed = 0;

    void freePacket(mbuf_t m) { m->type = MBUF_TYPE_FREE; ++freed; }
    UInt32 enqueuePacket(mbuf_t m);
    IOReturn outputStart(IONetworkInterface *interface, IOOptionBits options);
};

"""
            + v2_methods
            + r"""

int main() {
    AirportItlwm drv;
    // Prepare 500 synthetic packets in Skywalk/XNU output queue:
    // 3 batches of 64 (192 pkts) return kIOReturnSuccess while HW ring has room;
    // 4th batch of 64 drains 32 into HW ring (hitting hi_mark=224 -> ifq_oactive=1),
    // stages the remaining 32 in if_snd (<= 64 cap), and immediately returns kIOReturnNoResources!
    std::vector<FakeMbuf> pool(500);
    for (auto &pkt : pool) drv.en1BSD.pending.push_back(&pkt);

    for (unsigned call = 0; call < 3; ++call) {
        assert(drv.outputStart(&drv.en1BSD, 0) == kIOReturnSuccess);
    }
    assert(drv.hal.hwRingQueued == 192);
    assert(drv.hal.ic.ic_ac.ac_if.if_snd.ifq_oactive == 0);
    assert(drv.hal.pq.getSize() == 0);

    // 4th call hits hwRingHiMark (224), latches ifq_oactive=1, and signals NoResources immediately:
    assert(drv.outputStart(&drv.en1BSD, 0) == kIOReturnNoResources);
    assert(drv.hal.hwRingQueued == 224);
    assert(drv.hal.ic.ic_ac.ac_if.if_snd.ifq_oactive == 1);
    assert(drv.hal.pq.getSize() == 32);
    const unsigned callsAtOactive = drv.hal.startCalls;

    // Subsequent call while ifq_oactive == 1 immediately returns NoResources without dequeuing:
    assert(drv.outputStart(&drv.en1BSD, 0) == kIOReturnNoResources);
    assert(drv.hal.pq.getSize() == 32);
    assert(drv.hal.startCalls == callsAtOactive);
    assert(drv.en1BSD.pending.size() == 500 - 256);
    assert(drv.hal.stats.outputErrors == 0 && drv.freed == 0);

    // Simulate TX completion reclaiming HW ring and clearing ifq_oactive:
    drv.hal.hwRingQueued = 0;
    drv.hal.ic.ic_ac.ac_if.if_snd.ifq_oactive = 0;
    assert(drv.outputStart(&drv.en1BSD, 0) == kIOReturnSuccess);
    assert(drv.hal.transmitted.size() == 224 + 32 + 64);

    // Verify awdlBSD and llwBSD fast paths remain intact:
    assert(drv.outputStart(drv.awdlBSD, 0) == kIOReturnSuccess && drv.awdl.awdlCalls == 1);
    assert(drv.outputStart(drv.llwBSD, 0) == kIOReturnSuccess && drv.awdl.llwCalls == 1);
    return 0;
}
"""
        )
        self.compile_run(src)

    def test_p117_kext_freestanding_abi_zero_cxa_guards(self):
        probe_cpp = r"""
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"

extern "C" int preview117_wcl_kext_abi_probe(const void *owner,
                                             const AirportWCL::ScanRequest *scan,
                                             const AirportWCL::JoinRequest *join,
                                             const uint8_t *ies,
                                             size_t len,
                                             uint8_t *out) {
    auto &burst = AirportWCL::splitScanBurstFor(owner);
    const bool cont = burst.isContinuation(*scan, 1000ULL);
    burst.recordLiveScan(*scan, 1000ULL, cont);
    const bool thr = AirportWCL::shouldThrottleAssociatedBgscan(
        500ULL, 1000ULL, -50, 45000000ULL, -68,
        AirportWCL::hasDirectedSSID(*scan), cont);
    const size_t n = AirportWCL::spliceHiddenBeaconSSID(
        ies, len, scan->ssid, scan->ssidLength, out, len + 34);
    AirportWCL::resetSplitScanBurst(owner, true);
    return int(n) + int(thr) + int(AirportWCL::classifyLegacyAuth(*join));
}
"""
        with tempfile.TemporaryDirectory(prefix="p117-wcl-kext-abi-") as tmp:
            src_path = Path(tmp) / "probe.cpp"
            obj_path = Path(tmp) / "probe.o"
            src_path.write_text(probe_cpp)
            subprocess.run(
                [
                    "xcrun",
                    "clang++",
                    "-arch",
                    "x86_64",
                    "-std=c++14",
                    "-O2",
                    "-mkernel",
                    "-fapple-kext",
                    "-fno-exceptions",
                    "-fno-rtti",
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
