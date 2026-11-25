#include <cassert>
#include <cstring>
#include <cstdint>
#include <vector>
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"
using IOReturn = int;
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnBusy, kIOReturnNotReady,
       kIOReturnUnsupported, kIOReturnError, kIOReturnAborted, kIOReturnTimeout,
       kIOReturnOffline };
enum { IFF_UP = 1, IFF_RUNNING = 2, IEEE80211_F_ASCAN = 1, IEEE80211_F_BGSCAN = 4, IEEE80211_CHAN_MAX = 255 };
enum { IEEE80211_S_INIT, IEEE80211_S_SCAN, IEEE80211_S_AUTH, IEEE80211_S_ASSOC, IEEE80211_S_RUN };
struct ieee80211com {
    struct { int if_flags = IFF_UP | IFF_RUNNING; } ic_if;
    int ic_state = IEEE80211_S_RUN, ic_flags = 0;
    struct { int ic_flags; } ic_channels[256] = {};
    bool ic_wcl_mac_reconfig = false, ic_wcl_scan_requested = false,
         ic_wcl_scan_active = false, ic_wcl_scan_restricted = false,
         ic_wcl_join_requested = false, ic_wcl_enterprise = false;
    unsigned ic_wcl_scan_ssid_length = 0, ic_des_esslen = 0;
    uint8_t ic_wcl_join_channel = 0;
    uint8_t ic_wcl_scan_ssid_count = 0, ic_wcl_scan_ssid_lengths[10] = {};
    uint8_t ic_wcl_scan_ssids[10][32] = {};
    uint8_t ic_wcl_scan_ssid[32] = {}, ic_wcl_scan_channels[32] = {}, ic_psk[32] = {};
    int (*ic_newstate)(ieee80211com *, int, int);
};
static int transitions, transitionError;
static bool backgroundAccepted = true;
static int newState(ieee80211com *ic, int state, int) {
    ++transitions;
    if (!transitionError) ic->ic_state = state;
    return transitionError;
}
static void ieee80211_begin_cache_bgscan(decltype(ieee80211com::ic_if) *ifp) {
    auto ic = reinterpret_cast<ieee80211com *>(ifp);
    if (backgroundAccepted) ic->ic_flags |= IEEE80211_F_BGSCAN;
}
static void explicit_bzero(void *p, size_t n) { memset(p, 0, n); }
struct Timer {
    int delay = -1;
    void setTimeoutMS(int ms) { delay = ms; }
    void cancelTimeout() { delay = -1; }
};
struct Hal {
    ieee80211com ic;
    Hal() { ic.ic_newstate = newState; ic.ic_channels[36].ic_flags = 1; }
    ieee80211com *get80211Controller() { return &ic; }
};
struct Instance {
    struct Message { unsigned code; int status; };
    std::vector<Message> messages;
    void postMessage(void *, unsigned code, void *data, size_t, bool) {
        messages.push_back({code, *static_cast<int *>(data)});
    }
    unsigned scans() const {
        unsigned n = 0;
        for (auto m : messages) n += m.code == AirportWCL::ScanDone;
        return n;
    }
    int lastScan() const {
        for (auto i = messages.rbegin(); i != messages.rend(); ++i)
            if (i->code == AirportWCL::ScanDone) return i->status;
        assert(false); return -1;
    }
};
struct apple80211ScanRequest { uint8_t bytes[0x1550] = {}; };
struct AirportItlwmSkywalkInterface {
    Hal hal; Hal *fHalService = &hal;
    Timer timer; Timer *scanSource = &timer;
    Instance owner; Instance *instance = &owner;
    bool scanPending = false, joinPending = false, joinDeferred = false,
         scanDeferred = false, scanCompleted = false, scanSkipCompletion = false,
         wclStopping = false, portAuthorized = false,
         wclRoamLock = false, wclRealTimeMode = false;
    AirportWCL::ScanRequest wclScanRequest = {};
    AirportWCL::JoinRequest deferredJoin = {};
    Timer jt; Timer *joinTimer = &jt;
    unsigned joinCalls = 0;
    void resumeWCLJoin();
    IOReturn startWCLJoin(AirportWCL::JoinRequest &request) {
        assert(request.key[0] == 42); ++joinCalls;
        joinPending = true;
        return kIOReturnSuccess;
    }
    void clearWCLPMKSA() {}
    void updateWCLLinkQuality(bool) {}
    void finishWCLJoin(int, int, IOReturn) { joinPending = joinDeferred = false; }
    void scanDone() {
        // PRODUCTION: scan_done
    }
    IOReturn beginWCLScan(apple80211ScanRequest *);
    bool completeWCLScanFromCache(const AirportWCL::ScanRequest &) { return false; }
    IOReturn startWCLScan(const AirportWCL::ScanRequest &);
    void resumeWCLScan(); void finishWCLScan(IOReturn);
    void completeWCLScan(); void handleWCLAvailability(bool);
};
// PRODUCTION: scan
int main() {
    apple80211ScanRequest request;
    request.bytes[0x40] = 1;
    for (bool passive : {false, true}) {
        AirportItlwmSkywalkInterface s;
        auto multi = request;
        multi.bytes[0x40] = passive ? 2 : 1;
        multi.bytes[0x1318] = 3;
        multi.bytes[0x1320] = multi.bytes[0x1358] = 3;
        memcpy(multi.bytes + 0x1324, "one", 3);
        memcpy(multi.bytes + 0x135c, "two", 3);
        s.hal.ic.ic_if.if_flags = IFF_UP;
        assert(s.beginWCLScan(&multi) == kIOReturnSuccess && s.scanDeferred);
        s.hal.ic.ic_if.if_flags |= IFF_RUNNING;
        s.resumeWCLScan();
        assert(s.hal.ic.ic_wcl_scan_ssid_count == 3 && s.hal.ic.ic_wcl_scan_active);
        assert(s.hal.ic.ic_wcl_scan_ssid_lengths[1] == 3);
        assert(!memcmp(s.hal.ic.ic_wcl_scan_ssids[1], "two", 3));
        s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
        s.scanDone();
        s.completeWCLScan();
        assert(s.owner.scans() == 1 && !s.hal.ic.ic_wcl_scan_requested);
        auto wildcard = request;
        wildcard.bytes[0x40] = passive ? 2 : 1;
        wildcard.bytes[0x1318] = 1;
        assert(s.beginWCLScan(&wildcard) == kIOReturnSuccess);
        assert(s.hal.ic.ic_wcl_scan_ssid_count == (passive ? 0 : 1));
        assert(s.hal.ic.ic_wcl_scan_active == !passive);
    }
    {
        AirportItlwmSkywalkInterface s;
        s.hal.ic.ic_state = IEEE80211_S_INIT;
        s.hal.ic.ic_wcl_mac_reconfig = true;
        assert(s.beginWCLScan(&request) == kIOReturnSuccess);
        assert(s.scanDeferred && s.scanPending && s.timer.delay == 20000);
        assert(s.beginWCLScan(&request) == kIOReturnBusy);
        s.handleWCLAvailability(false);
        assert(s.scanDeferred && s.scanPending && s.owner.scans() == 0);
        s.scanDone(); // Completion of the scan interrupted by MAC rotation.
        assert(!s.scanCompleted && s.timer.delay == 20000);
        s.hal.ic.ic_state = IEEE80211_S_INIT;
        s.handleWCLAvailability(true);
        assert(!s.scanDeferred && s.scanPending && !s.hal.ic.ic_wcl_mac_reconfig);
        assert(s.hal.ic.ic_state == IEEE80211_S_SCAN && s.hal.ic.ic_wcl_scan_requested);
        s.scanDone(); assert(s.scanCompleted && s.timer.delay == 0);
        s.completeWCLScan(); s.completeWCLScan();
        assert(s.owner.scans() == 1 && s.owner.lastScan() == kIOReturnSuccess);
        assert(!s.hal.ic.ic_wcl_scan_requested && s.timer.delay == -1);
    }
    for (auto status : {kIOReturnAborted, kIOReturnTimeout}) {
        AirportItlwmSkywalkInterface s;
        s.hal.ic.ic_if.if_flags = IFF_UP;
        assert(s.beginWCLScan(&request) == kIOReturnSuccess);
        if (status == kIOReturnTimeout) s.completeWCLScan(); else s.finishWCLScan(status);
        s.hal.ic.ic_if.if_flags |= IFF_RUNNING;
        s.handleWCLAvailability(true);
        assert(!s.scanPending && !s.scanDeferred && s.owner.scans() == 1);
        assert(s.owner.lastScan() == status);
    }
    {
        AirportItlwmSkywalkInterface s;
        s.hal.ic.ic_wcl_mac_reconfig = true;
        s.beginWCLScan(&request);
        s.joinPending = s.joinDeferred = true;
        s.deferredJoin.key[0] = 42;
        s.handleWCLAvailability(false);
        assert(s.joinPending && s.joinDeferred && s.joinCalls == 0);
        s.handleWCLAvailability(true);
        assert(s.joinCalls == 1 && s.joinPending && !s.joinDeferred);
        assert(!s.scanPending && s.owner.lastScan() == kIOReturnAborted);
        for (auto byte : s.deferredJoin.key) assert(byte == 0);
    }
    {
        AirportItlwmSkywalkInterface s;
        s.hal.ic.ic_if.if_flags = IFF_UP;
        s.beginWCLScan(&request); s.joinPending = true;
        s.resumeWCLScan();
        assert(s.owner.lastScan() == kIOReturnAborted && !s.scanPending);
    }
    {
        AirportItlwmSkywalkInterface s;
        assert(s.beginWCLScan(&request) == kIOReturnSuccess);
        s.handleWCLAvailability(false);
        assert(s.owner.lastScan() == kIOReturnOffline && !s.scanPending);
    }
    {
        AirportItlwmSkywalkInterface s;
        s.hal.ic.ic_if.if_flags = 0;
        assert(s.beginWCLScan(&request) == kIOReturnNotReady && !s.scanPending);
        s.hal.ic.ic_if.if_flags = IFF_UP;
        assert(s.beginWCLScan(&request) == kIOReturnSuccess);
        s.hal.ic.ic_if.if_flags |= IFF_RUNNING;
        s.hal.ic.ic_state = IEEE80211_S_INIT;
        transitionError = 1;
        s.resumeWCLScan();
        assert(s.owner.lastScan() == kIOReturnError && !s.scanPending);
        transitionError = 0;
    }
    {
        AirportItlwmSkywalkInterface s;
        backgroundAccepted = false;
        assert(s.beginWCLScan(&request) == kIOReturnBusy && !s.scanPending);
        assert(!s.hal.ic.ic_wcl_scan_requested && s.timer.delay == -1);
        backgroundAccepted = true;
    }
    {
        AirportItlwmSkywalkInterface s;
        s.hal.ic.ic_if.if_flags = IFF_UP;
        s.beginWCLScan(&request); s.wclStopping = true;
        int count = transitions;
        s.resumeWCLScan();
        assert(transitions == count);
    }
}
