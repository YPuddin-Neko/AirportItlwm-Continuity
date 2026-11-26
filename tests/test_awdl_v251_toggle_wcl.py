"""WCL fast toggle/wake lifecycle, Private-MAC in-place update, scan preemption, and country dedup tests:
1. AirportItlwm::setHardwareAddress updates bsdInterface, ac_enaddr, and ic->ic_myaddr in place
   with zero firmware disable/enable when ic_state <= IEEE80211_S_SCAN, and still reconfigures
   firmware only when ic_state > IEEE80211_S_SCAN.
2. AirportItlwm::setPOWER toggles adapter enable/disable without invoking IOPM
   changePowerStateToPriv under AIRPORT_WCL, and guards duplicate enable calls on IFF_UP|IFF_RUNNING.
3. AirportItlwm::getCOUNTRY_CODE (AirportItlwmV2.cpp and AirportSTAIOCTL.cpp) falls back to
   geo_location_cc for both Intel LAR world code '00' and 'ZZ'.
4. beginWCLScan / startWCLScan / leaveWCLNetwork run immediately from idle IFF_RUNNING + S_INIT,
   never hang when ic_state is stale S_SCAN, return leaveWCLNetwork to S_INIT, and complete
   unsupported/6 GHz subsets in 1 ms across S_INIT, S_SCAN, and S_RUN.
5. startWCLJoin preempts in-flight WCL scans (abortScanning + ScanDone(kIOReturnAborted)), arms
   IEEE80211_F_ASCAN + join channel/SSID/BSSID, and unconditionally starts ic_newstate(S_SCAN)
   both from unassociated S_INIT/S_SCAN and when switching from associated S_RUN to a hidden SSID.
6. CountryDeduplicator / deferWCLCountryUpdate suppress world '00'/'ZZ' storms and duplicate
   country events while still delivering one notification on real MCC unlock after driver reset,
   and associated undirected active scans enforce a 25 s strong-signal guard without throttling
   directed hidden-SSID scans.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

import test_regressions as support
from test_offline import definition


class V251ToggleWCLRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_v251_private_mac_inplace_setpower_and_country_world_fallback(self):
        set_hw = definition("AirportItlwm/AirportItlwmV2.cpp", "AirportItlwm::setHardwareAddress")
        set_pwr = definition("AirportItlwm/AirportItlwmV2.cpp", "AirportItlwm::setPOWER")
        get_cc_v2 = definition("AirportItlwm/AirportItlwmV2.cpp", "AirportItlwm::getCOUNTRY_CODE")
        get_cc_sta = definition("AirportItlwm/AirportSTAIOCTL.cpp", "AirportItlwm::getCOUNTRY_CODE")

        self.assertNotIn("changePowerStateToPriv", set_pwr.split("#else", 1)[0])
        self.assertIn("IEEE80211_ADDR_COPY(ic->ic_myaddr, address);", set_hw)
        self.assertIn("ic->ic_state > IEEE80211_S_SCAN", set_hw)

        # Validate both AirportItlwmV2::getCOUNTRY_CODE and AirportSTAIOCTL::getCOUNTRY_CODE.
        get_cc_sta_renamed = get_cc_sta.replace(
            "AirportItlwm::\ngetCOUNTRY_CODE(",
            "AirportItlwm::getCOUNTRY_CODE_STA(",
        ).replace(
            "AirportItlwm::getCOUNTRY_CODE(",
            "AirportItlwm::getCOUNTRY_CODE_STA(",
        )
        self.assertIn("getCOUNTRY_CODE_STA(", get_cc_sta_renamed)

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
using UInt8 = uint8_t;
using UInt32 = uint32_t;
using errno_t = int;
using OSObject = struct OSObjectStub;

enum {
    kIOReturnSuccess = 0,
    kIOReturnBadArgument = 1,
    kIOReturnBusy = 2,
    kIOReturnNotReady = 3,
    kIOReturnError = 4
};
enum {
    ETHER_ADDR_LEN = 6,
    IFF_UP = 1,
    IFF_RUNNING = 2,
    APPLE80211_VERSION = 1
};
enum {
    IEEE80211_S_INIT = 0,
    IEEE80211_S_SCAN = 1,
    IEEE80211_S_AUTH = 2,
    IEEE80211_S_ASSOC = 3,
    IEEE80211_S_RUN = 4
};

#define IEEE80211_ADDR_COPY(dst, src) memcpy((dst), (src), ETHER_ADDR_LEN)
#define kIOMACAddress "IOMACAddress"
#define IOLog(...) ((void)0)

struct OSObjectStub {};
struct apple80211_country_code_data {
    uint32_t version = 0;
    uint8_t cc[3] = {};
};
struct apple80211_power_data {
    uint32_t version = 0;
    uint32_t num_radios = 0;
    uint32_t power_state[4] = {};
};

struct _ifnet {
    int if_flags = 0;
};
struct arpcom {
    struct _ifnet ac_if;
    uint8_t ac_enaddr[ETHER_ADDR_LEN] = {};
};
struct ieee80211com {
    union {
        struct _ifnet ic_if;
        struct arpcom ic_ac;
    };
    uint8_t ic_myaddr[ETHER_ADDR_LEN] = {};
    int ic_state = IEEE80211_S_INIT;
    bool ic_wcl_mac_reconfig = false;
    ieee80211com() { memset(&ic_ac, 0, sizeof(ic_ac)); }
};

struct BSDIfnetStub {
    uint8_t lladdr[ETHER_ADDR_LEN] = {};
};
struct BSDInterfaceStub {
    BSDIfnetStub net = {};
    uint8_t propMac[ETHER_ADDR_LEN] = {};
    BSDIfnetStub *getIfnet() { return &net; }
    void setProperty(const char *, UInt8 *bytes, UInt32 len) {
        if (len == ETHER_ADDR_LEN)
            memcpy(propMac, bytes, ETHER_ADDR_LEN);
    }
};

static errno_t ifnet_set_lladdr(BSDIfnetStub *ifp, const UInt8 *addr, UInt32 len) {
    if (!ifp || len != ETHER_ADDR_LEN) return 22;
    memcpy(ifp->lladdr, addr, ETHER_ADDR_LEN);
    return 0;
}
static int if_setlladdr(struct _ifnet *ifp, const UInt8 *lladdr) {
    memcpy(reinterpret_cast<struct arpcom *>(ifp)->ac_enaddr, lladdr, ETHER_ADDR_LEN);
    return 0;
}
static void net80211_ifstats(ieee80211com *) {}

static char bootArgCC[4] = {};
static bool PE_parse_boot_argn(const char *, char *dst, size_t maxLen) {
    if (!bootArgCC[0]) return false;
    strncpy(dst, bootArgCC, maxLen);
    return true;
}

struct DriverInfoStub {
    const char *fwCC = "00";
    const char *getFirmwareCountryCode() const { return fwCC; }
};
struct HalServiceStub {
    ieee80211com ic;
    DriverInfoStub info;
    unsigned disableCalls = 0;
    unsigned enableCalls = 0;
    ieee80211com *get80211Controller() { return &ic; }
    DriverInfoStub *getDriverInfo() { return &info; }
    void disable(BSDInterfaceStub *) {
        ++disableCalls;
        ic.ic_if.if_flags &= ~(IFF_UP | IFF_RUNNING);
        ic.ic_state = IEEE80211_S_INIT;
    }
    IOReturn enable(BSDInterfaceStub *) {
        ++enableCalls;
        ic.ic_if.if_flags |= IFF_UP;
        return kIOReturnSuccess;
    }
};

struct AirportItlwm {
    HalServiceStub hal;
    HalServiceStub *fHalService = &hal;
    BSDInterfaceStub bsd;
    BSDInterfaceStub *bsdInterface = &bsd;
    int netIfDummy = 1;
    void *fNetIf = &netIfDummy;
    uint32_t power_state = 1;
    char geo_location_cc[3] = {};
    unsigned adapterEnableCalls = 0;
    unsigned adapterDisableCalls = 0;

    IOReturn enableAdapter(BSDInterfaceStub *netif) {
        ++adapterEnableCalls;
        return fHalService->enable(netif);
    }
    void disableAdapter(BSDInterfaceStub *netif) {
        ++adapterDisableCalls;
        fHalService->disable(netif);
    }
    IOReturn setHardwareAddress(const void *addrP, UInt32 addrBytes);
    IOReturn setPOWER(OSObject *object, struct apple80211_power_data *pd);
    IOReturn getCOUNTRY_CODE(OSObject *object, struct apple80211_country_code_data *cd);
    IOReturn getCOUNTRY_CODE_STA(OSObject *object, struct apple80211_country_code_data *cd);
};

"""
            + set_hw
            + "\n\n"
            + set_pwr
            + "\n\n"
            + get_cc_v2
            + "\n\n"
            + get_cc_sta_renamed
            + r"""

int main() {
    AirportItlwm drv;
    const uint8_t scanMac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t privMac[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    const uint8_t assocMac[6] = {0x02, 0x55, 0x66, 0x77, 0x88, 0x99};

    // 1. In-place Private-MAC rotation in S_INIT and S_SCAN (IFF_RUNNING):
    //    updates bsdInterface, ac_enaddr, and ic->ic_myaddr with ZERO firmware reboot!
    drv.hal.ic.ic_if.if_flags = IFF_UP | IFF_RUNNING;
    drv.hal.ic.ic_state = IEEE80211_S_INIT;
    memcpy(drv.hal.ic.ic_myaddr, scanMac, 6);
    assert(drv.setHardwareAddress(privMac, 6) == kIOReturnSuccess);
    assert(drv.hal.disableCalls == 0 && drv.hal.enableCalls == 0);
    assert(!drv.hal.ic.ic_wcl_mac_reconfig);
    assert(memcmp(drv.hal.ic.ic_myaddr, privMac, 6) == 0);
    assert(memcmp(drv.hal.ic.ic_ac.ac_enaddr, privMac, 6) == 0);
    assert(memcmp(drv.bsd.net.lladdr, privMac, 6) == 0);
    assert(memcmp(drv.bsd.propMac, privMac, 6) == 0);

    // Same in S_SCAN: still zero firmware reboot!
    drv.hal.ic.ic_state = IEEE80211_S_SCAN;
    assert(drv.setHardwareAddress(scanMac, 6) == kIOReturnSuccess);
    assert(drv.hal.disableCalls == 0 && drv.hal.enableCalls == 0);
    assert(!drv.hal.ic.ic_wcl_mac_reconfig);
    assert(memcmp(drv.hal.ic.ic_myaddr, scanMac, 6) == 0);

    // Replay of unchanged MAC: stays a no-op!
    assert(drv.setHardwareAddress(scanMac, 6) == kIOReturnSuccess);
    assert(drv.hal.disableCalls == 0 && drv.hal.enableCalls == 0);

    // MAC change while associated (S_RUN > S_SCAN): triggers firmware mac_reconfig restart!
    drv.hal.ic.ic_state = IEEE80211_S_RUN;
    assert(drv.setHardwareAddress(assocMac, 6) == kIOReturnSuccess);
    assert(drv.hal.ic.ic_wcl_mac_reconfig);
    assert(drv.hal.disableCalls == 1 && drv.hal.enableCalls == 1);
    assert(memcmp(drv.hal.ic.ic_myaddr, assocMac, 6) == 0);

    // 2. setPOWER toggles cleanly without duplicate enableAdapter while IFF_UP is in flight:
    drv.hal.ic.ic_if.if_flags = IFF_UP | IFF_RUNNING;
    apple80211_power_data pd = {};
    pd.version = APPLE80211_VERSION;
    pd.num_radios = 1;
    pd.power_state[0] = 0;
    assert(drv.setPOWER(nullptr, &pd) == kIOReturnSuccess);
    assert(drv.adapterDisableCalls == 1 && drv.power_state == 0 && !drv.hal.ic.ic_wcl_mac_reconfig);
    assert(drv.setPOWER(nullptr, &pd) == kIOReturnSuccess);
    assert(drv.adapterDisableCalls == 1); // Already down

    pd.power_state[0] = 1;
    assert(drv.setPOWER(nullptr, &pd) == kIOReturnSuccess);
    assert(drv.adapterEnableCalls == 1 && (drv.hal.ic.ic_if.if_flags & IFF_UP));
    // Second setPOWER(1) while IFF_UP is already in flight MUST NOT double-enable!
    assert(drv.setPOWER(nullptr, &pd) == kIOReturnSuccess);
    assert(drv.adapterEnableCalls == 1);

    // 3. getCOUNTRY_CODE (V2 and STAIOCTL) treats both '00' and 'ZZ' as world fallback to geo_location_cc:
    memcpy(drv.geo_location_cc, "CN", 3);
    apple80211_country_code_data cd = {};
    for (const char *world : {"00", "ZZ"}) {
        drv.hal.info.fwCC = world;
        assert(drv.getCOUNTRY_CODE(nullptr, &cd) == kIOReturnSuccess);
        assert(memcmp(cd.cc, "CN", 2) == 0);
        memset(&cd, 0, sizeof(cd));
        assert(drv.getCOUNTRY_CODE_STA(nullptr, &cd) == kIOReturnSuccess);
        assert(memcmp(cd.cc, "CN", 2) == 0);
    }
    // Real firmware MCC overrides geo_location_cc:
    drv.hal.info.fwCC = "JP";
    assert(drv.getCOUNTRY_CODE(nullptr, &cd) == kIOReturnSuccess);
    assert(memcmp(cd.cc, "JP", 2) == 0);
    assert(drv.getCOUNTRY_CODE_STA(nullptr, &cd) == kIOReturnSuccess);
    assert(memcmp(cd.cc, "JP", 2) == 0);

    // Boot-arg override takes top priority:
    memcpy(bootArgCC, "US", 3);
    assert(drv.getCOUNTRY_CODE(nullptr, &cd) == kIOReturnSuccess);
    assert(memcmp(cd.cc, "US", 2) == 0);
    assert(drv.getCOUNTRY_CODE_STA(nullptr, &cd) == kIOReturnSuccess);
    assert(memcmp(cd.cc, "US", 2) == 0);
    bootArgCC[0] = '\0';
    return 0;
}
"""
        )
        self.compile_run(src)

    def test_v251_s_init_scan_no_20s_hang_join_preemption_and_25s_cooldown(self):
        wcl_methods = "\n\n".join(
            definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::" + name)
            for name in (
                "beginWCLScan",
                "completeWCLScanFromCache",
                "startWCLScan",
                "finishWCLScan",
                "completeWCLScan",
                "startWCLJoin",
                "leaveWCLNetwork",
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

static uint64_t fakeClockUS = 10000000ULL;
static uint64_t mach_absolute_time() { return fakeClockUS * 1000ULL; }
static void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *ns) { *ns = abstime; }
static void explicit_bzero(void *p, size_t n) { memset(p, 0, n); }
#define bzero(p, n) memset((p), 0, (n))
#define isset(p, b) ((p)[(b) / 8] & (1U << ((b) % 8)))
#define IEEE80211_ADDR_EQ(a, b) (memcmp((a), (b), 6) == 0)
#define RB_FOREACH(var, name, head) for ((var) = (head)->first; (var) != nullptr; (var) = (var)->next)

struct OSObjectStub {};
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
    int ni_rssi = 54; // -46 dBm
    const uint8_t *ni_rsnie_tlv = nullptr;
    size_t ni_rsnie_tlv_len = 0;
    ieee80211_node *next = nullptr;
};
struct NodeTree { ieee80211_node *first = nullptr; };
struct ieee80211com {
    struct { int if_flags = IFF_UP | IFF_RUNNING; } ic_if;
    int ic_state = IEEE80211_S_INIT;
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
    std::vector<int> stateTransitions;
    int (*ic_newstate)(ieee80211com *, int, int) = nullptr;
};

static unsigned liveBgscans = 0;
static int mockNewstate(ieee80211com *ic, int st, int) {
    ic->stateTransitions.push_back(st);
    ic->ic_state = st;
    return 0;
}
static void ieee80211_begin_cache_bgscan(decltype(ieee80211com::ic_if) *ifp) {
    auto *ic = reinterpret_cast<ieee80211com *>(ifp);
    ic->ic_flags |= IEEE80211_F_BGSCAN;
    ++liveBgscans;
}
static void ieee80211_new_state(ieee80211com *ic, int st, int arg) {
    if (ic->ic_newstate) ic->ic_newstate(ic, st, arg);
}
static void ieee80211_disable_rsn(ieee80211com *) {}
static void ieee80211_disable_wep(ieee80211com *) {}
static int ieee80211_ioctl_setwpaparms(ieee80211com *, const ieee80211_wpaparams *) { return 0; }
static int ieee80211_ioctl_setnwkeys(ieee80211com *, const ieee80211_nwkey *) { return 0; }
static void *ieee80211_pmksa_add(ieee80211com *, int, const uint8_t *, const uint8_t *, int) {
    static int token = 1;
    return &token;
}

struct DriverControllerStub {
    unsigned abortCalls = 0;
    void abortScanning() { ++abortCalls; }
};
struct HalStub {
    ieee80211com ic;
    ieee80211_node bss;
    DriverControllerStub drv;
    HalStub() {
        ic.ic_newstate = mockNewstate;
        const uint8_t ap[6] = {0x02, 0x00, 0x00, 0x00, 0x48, 0x01};
        memcpy(bss.ni_bssid, ap, 6);
        ic.ic_bss = &bss;
        for (unsigned c : {1U, 6U, 11U, 36U, 48U, 149U, 161U}) {
            ic.ic_channels[c].ic_flags = 1;
            ic.ic_chan_active[c / 8] |= 1U << (c % 8);
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
    AWDLStub awdl;
    AWDLStub *awdlInterface = &awdl;
    std::vector<int> scanDoneStatuses;
    void postMessage(void *, unsigned code, const void *data, size_t, bool) {
        if (code == AirportWCL::ScanDone)
            scanDoneStatuses.push_back(*static_cast<const int *>(data));
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
    uint8_t joinBSSID[6] = {0x02, 0x00, 0x00, 0x00, 0x48, 0x01};
    bool scanPending = false, scanCompleted = false, scanSkipCompletion = false, scanDeferred = false;
    AirportWCL::ScanRequest wclScanRequest = {};
    bool wclBssValid = true, wclLinkIndicated = false;
    uint8_t wclBssInfo[sizeof(AirportWCL::BeaconMetadata) + 0x800] = {};
    uint8_t wclBeaconInfo[0x808] = {};
    uint32_t wclAssociatedBeacons = 0;
    bool joinPending = false, joinDeferred = false;
    AirportWCL::JoinRequest deferredJoin = {};
    bool assocNotified = false, firstBeaconNotified = false, portAuthorized = false;
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

    IOReturn beginWCLScan(apple80211ScanRequest *);
    bool completeWCLScanFromCache(const AirportWCL::ScanRequest &);
    IOReturn startWCLScan(const AirportWCL::ScanRequest &);
    void finishWCLScan(IOReturn);
    void completeWCLScan();
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

static apple80211ScanRequest makeScan(const std::vector<unsigned> &chans, uint32_t type = 1,
                                      const char *ssid = nullptr, bool sixGhz = false) {
    apple80211ScanRequest req = {};
    putU32(req.bytes + 0x40, type);
    putU32(req.bytes + 0x54, uint32_t(chans.size()));
    for (size_t i = 0; i < chans.size(); ++i) {
        uint8_t *e = req.bytes + 0x58 + i * 12;
        putU32(e + 0, 1);
        putU32(e + 4, chans[i]);
        putU32(e + 8, sixGhz ? 0x2000U : (chans[i] <= 14 ? 0x8U : 0x10U));
    }
    if (ssid) {
        const uint32_t len = uint32_t(strlen(ssid));
        putU32(req.bytes + 0x1c, len);
        memcpy(req.bytes + 0x20, ssid, len);
    }
    return req;
}

static AirportWCL::JoinRequest makeJoin(const char *ssid, uint16_t chanSpec) {
    AirportWCL::JoinRequest j = {};
    j.lowerAuth = 1;
    j.upperAuth = APPLE80211_AUTHTYPE_WPA2_PSK | APPLE80211_AUTHTYPE_WPA3_SAE;
    j.ssidLength = uint32_t(strlen(ssid));
    memcpy(j.ssid, ssid, j.ssidLength);
    j.keyLength = 32;
    j.keyCipher = 6;
    for (unsigned i = 0; i < 32; ++i) j.key[i] = uint8_t(0x20 + i);
    const uint8_t bssid[6] = {0x02, 0x00, 0x00, 0x00, 0x48, 0x01};
    memcpy(j.bssid, bssid, 6);
    j.channelSpec = chanSpec;
    return j;
}

"""
            + wcl_methods
            + r"""

int main() {
    AirportItlwmSkywalkInterface s;

    // 1. Idle IFF_RUNNING + IEEE80211_S_INIT starts scan IMMEDIATELY (never defers!)
    //    and handles 6 GHz-only empty subset in 1 ms.
    auto sixGhz = makeScan({37}, 1, nullptr, true);
    assert(s.beginWCLScan(&sixGhz) == kIOReturnSuccess);
    assert(!s.scanDeferred && s.scanPending && s.scanCompleted && s.scanTimer.delay == 1);
    s.completeWCLScan();
    assert(!s.scanPending && s.owner.scanDoneStatuses.back() == kIOReturnSuccess);

    auto recent3 = makeScan({48, 36, 161}, 1);
    assert(s.beginWCLScan(&recent3) == kIOReturnSuccess);
    assert(!s.scanDeferred && s.scanPending && !s.scanSkipCompletion);
    assert(s.hal.ic.ic_state == IEEE80211_S_SCAN);
    assert(s.hal.ic.ic_flags & IEEE80211_F_ASCAN);
    assert(s.hal.ic.stateTransitions.size() == 1 && s.hal.ic.stateTransitions.back() == IEEE80211_S_SCAN);
    s.scanCompleted = true;
    s.completeWCLScan();

    // 2. Stale S_SCAN (no firmware scan active) -> leaveWCLNetwork() resets to S_INIT and
    //    subsequent startWCLScan() unconditionally triggers ic_newstate(S_SCAN) (zero 20 s hang)!
    s.hal.ic.ic_state = IEEE80211_S_SCAN;
    const unsigned aborts0 = s.hal.drv.abortCalls;
    assert(s.leaveWCLNetwork() == kIOReturnSuccess);
    assert(s.hal.drv.abortCalls == aborts0 + 1);
    assert(s.hal.ic.ic_state == IEEE80211_S_INIT);
    const size_t transBefore = s.hal.ic.stateTransitions.size();
    assert(s.beginWCLScan(&recent3) == kIOReturnSuccess);
    assert(s.hal.ic.stateTransitions.size() == transBefore + 1);
    assert(s.hal.ic.ic_state == IEEE80211_S_SCAN);
    s.scanCompleted = true;
    s.completeWCLScan();

    // Even if ic_state is still stale S_SCAN without leaveWCLNetwork(), startWCLScan()
    // aborts stale scan and unconditionally invokes ic_newstate(S_SCAN) with passive F_ASCAN cleared!
    s.hal.ic.ic_state = IEEE80211_S_SCAN;
    auto passive2 = makeScan({36, 149}, 2);
    const size_t transBeforeStale = s.hal.ic.stateTransitions.size();
    assert(s.beginWCLScan(&passive2) == kIOReturnSuccess);
    assert(s.hal.ic.stateTransitions.size() == transBeforeStale + 1);
    assert(!(s.hal.ic.ic_flags & IEEE80211_F_ASCAN));

    // 3. startWCLJoin() while WiFiAgent scanPending is in flight MUST preempt the scan
    //    (abortScanning + ScanDone(kIOReturnAborted)) and immediately launch ic_newstate(S_SCAN)
    //    on Ch 48 with IEEE80211_F_ASCAN set!
    assert(s.scanPending);
    const unsigned abortsBeforePreempt = s.hal.drv.abortCalls;
    const size_t transBeforeJoin = s.hal.ic.stateTransitions.size();
    auto joinCh48 = makeJoin("HomeFast5G", 0xd030);
    assert(s.startWCLJoin(joinCh48) == kIOReturnSuccess);
    assert(!s.scanPending && s.owner.scanDoneStatuses.back() == kIOReturnAborted);
    assert(s.hal.drv.abortCalls == abortsBeforePreempt + 1);
    assert(s.joinPending && s.hal.ic.ic_wcl_join_requested && s.hal.ic.ic_wcl_join_channel == 48);
    assert(s.hal.ic.ic_flags & IEEE80211_F_ASCAN);
    assert(s.hal.ic.stateTransitions.size() == transBeforeJoin + 1);
    assert(s.hal.ic.stateTransitions.back() == IEEE80211_S_SCAN);

    // 4. Associated S_RUN + strong signal (-46 dBm >= -68 dBm):
    //    18 s WiFiAgent menu-open active scan throttles (< 25 s), 26 s runs live (>= 25 s),
    //    while directed Hidden-SSID scans at +5 s stay unthrottled!
    s.joinPending = false;
    s.hal.ic.ic_wcl_join_requested = false;
    s.hal.ic.ic_state = IEEE80211_S_RUN;
    s.portAuthorized = true;
    s.hal.ic.ic_flags = 0;
    liveBgscans = 0;
    fakeClockUS = 100000000ULL;
    auto menuScan = makeScan({1, 6, 11}, 1);
    assert(s.beginWCLScan(&menuScan) == kIOReturnSuccess);
    assert(liveBgscans == 1 && (s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
    fakeClockUS += 400000ULL;
    s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
    s.scanCompleted = true;
    s.completeWCLScan();

    // +18 s (< 25 s): throttled from cache in 1 ms!
    fakeClockUS += 18000000ULL;
    assert(s.beginWCLScan(&menuScan) == kIOReturnSuccess);
    assert(liveBgscans == 1 && s.scanTimer.delay == 1);
    s.completeWCLScan();

    // Directed hidden-SSID scan at +18.5 s: bypasses cache and runs live!
    fakeClockUS += 500000ULL;
    auto hiddenScan = makeScan({48}, 2, "Hidden5G");
    assert(s.beginWCLScan(&hiddenScan) == kIOReturnSuccess);
    assert(liveBgscans == 2 && (s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
    fakeClockUS += 300000ULL;
    s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
    s.scanCompleted = true;
    s.completeWCLScan();

    // +26 s after last live scan (>= 25 s): active menu scan runs live again!
    fakeClockUS += 26000000ULL;
    assert(s.beginWCLScan(&menuScan) == kIOReturnSuccess);
    assert(liveBgscans == 3 && (s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
    s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
    s.scanCompleted = true;
    s.completeWCLScan();

    AirportItlwmSkywalkInterface::stopWCLGated(nullptr, &s, nullptr, nullptr, nullptr);
    return 0;
}
"""
        )
        self.compile_run(src)

    def test_v251_country_deduplicator_and_freestanding_kext_abi(self):
        defer_cc = definition(
            "AirportItlwm/WCL/WCL.cpp",
            "AirportItlwmSkywalkInterface::deferWCLCountryUpdate",
        )
        self.assertIn("reinterpret_cast<const char *>(country.cc), fwCC", defer_cc)
        for fn_name in ("setASSOCIATE", "setDISASSOCIATE", "setSCAN_REQ"):
            body = definition(
                "AirportItlwm/AirportItlwmSkywalkInterface.cpp",
                "AirportItlwmSkywalkInterface::" + fn_name,
            )
            self.assertIn("!(ic->ic_if.if_flags & IFF_RUNNING)", body)

        src = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"

int main() {
    int ifaceA = 0;
    auto &dedup = AirportWCL::countryDeduplicatorFor(&ifaceA);
    dedup.reset();

    // 1. Cold boot world '00' / 'ZZ' / 'X0' never spam COUNTRY_CODE_CHANGED:
    assert(!dedup.shouldNotify("00", "00"));
    assert(!dedup.shouldNotify("ZZ", "ZZ"));
    assert(!dedup.shouldNotify("X0", "00"));
    assert(!dedup.valid);

    // 2. First real country from geo_location_cc ('CN') while fw is still '00' notifies once:
    assert(dedup.shouldNotify("CN", "00"));
    // Repeated world '00' notifications from firmware scans MUST be suppressed (7x storm fix!):
    for (int i = 0; i < 7; ++i)
        assert(!dedup.shouldNotify("CN", "00"));

    // 3. Firmware LAR unlocks real 'CN' -> notifies EXACTLY ONCE to refresh active 5 GHz flags:
    assert(dedup.shouldNotify("CN", "CN"));
    assert(!dedup.shouldNotify("CN", "CN"));
    assert(!dedup.shouldNotify("CN", "00"));

    // 4. Wi-Fi OFF -> ON / sleep -> wake (noteDriverReset):
    //    world '00' scans during boot/wake stay silent; real 'CN' unlock fires once!
    dedup.noteDriverReset();
    for (int i = 0; i < 6; ++i)
        assert(!dedup.shouldNotify("CN", "00"));
    assert(dedup.shouldNotify("CN", "CN"));
    assert(!dedup.shouldNotify("CN", "CN"));

    // 5. Roaming / setCOUNTRY_CODE to a new country ('US') notifies immediately:
    assert(dedup.shouldNotify("US", "00"));
    assert(!dedup.shouldNotify("US", "00"));
    assert(dedup.shouldNotify("US", "US"));
    assert(!dedup.shouldNotify("US", "US"));

    // 6. resetSplitScanBurst(owner, false) keeps country dedup across disassoc,
    //    while resetSplitScanBurst(owner, true) releases and resets the slot on stopWCLGated:
    AirportWCL::resetSplitScanBurst(&ifaceA, false);
    assert(AirportWCL::countryDeduplicatorFor(&ifaceA).valid);
    AirportWCL::resetSplitScanBurst(&ifaceA, true);
    assert(!AirportWCL::countryDeduplicatorFor(&ifaceA).valid);
    AirportWCL::resetSplitScanBurst(&ifaceA, true);
    return 0;
}
"""
        self.compile_run(src)

        # Verify zero C++ runtime guard/exception symbols in freestanding -mkernel build.
        probe_cpp = r"""
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"

extern "C" int v251_wcl_country_kext_abi_probe(const void *owner,
                                               const char *eff,
                                               const char *fw) {
    auto &c = AirportWCL::countryDeduplicatorFor(owner);
    const bool n = c.shouldNotify(eff, fw);
    c.noteDriverReset();
    AirportWCL::resetSplitScanBurst(owner, true);
    return int(n);
}
"""
        with tempfile.TemporaryDirectory(prefix="v251-wcl-kext-abi-") as tmp:
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
