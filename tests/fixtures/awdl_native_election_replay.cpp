#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/types.h>
#include "AirportItlwm/AWDL/SyncEngine.hpp"
#include "AirportItlwm/AWDL/RadioState.hpp"

using namespace AirportAWDL;
static uint64_t nowUS = 100000000;
static uint64_t lastStartUS = 0;
static unsigned programmed = 0, lockCount = 0;
static uint8_t manager[0x30], state[0xd000], base[0x600];
static uint8_t controller[0x200], controllerState[0x200];
static uint8_t peer[0x30], peerState[0x400], peerBase[0x500];
static uint8_t queue[0x20], queueState[0x40], head[0x18], node[0x18];
static void *lockVtable[0x128 / sizeof(void *)];
static void *managerVtable[8];
static void **lockObject = lockVtable;
static const uint8_t self[6] = {2,0,0,0,0,1}, parent[6] = {2,0,0,0,0,2};
static SyncEngine *driver = nullptr;

template<class T> static void put(uint8_t *bytes, size_t offset, T value) {
    std::memcpy(bytes + offset, &value, sizeof(value));
}
template<class T> static T get(const uint8_t *bytes, size_t offset) {
    T value; std::memcpy(&value, bytes + offset, sizeof(value)); return value;
}

extern "C" {
uint64_t fixtureGuard = 0x12345678;
uint64_t fixtureClock() {return nowUS * 1000;}
void fixtureNanoseconds(uint64_t ticks, uint64_t *out) {*out = ticks;}
void fixtureLog(void *, uint64_t, const char *, ...) {}
const char *fixtureStateName(unsigned) {return "synthetic";}
const char *fixtureName(void *) {return "synthetic";}
void *fixtureCast(void *object, void *) {return object;}
[[noreturn]] void fixtureUnexpected(uint64_t address) {
    std::fprintf(stderr, "Unmodeled native call: %llx\n", (unsigned long long)address);
    std::abort();
}
void *fixtureFindPeer(void *, const uint8_t *address) {
    assert(lockCount == 1);
    return sameAddress(address, parent) ? peer : nullptr;
}
void fixtureUpdateBroadcast(void *, unsigned, bool, bool) {}
void fixtureUpdateTemplate(void *, bool) {}
unsigned fixtureServices(void *) {return 0;}
void fixtureProgramMaster(void *) {
    ++programmed;
    if (!driver) return;
    uint8_t profile[48] = {24,40,0};
    std::memcpy(profile + 3, state + 0x6a57, 40);
    profile[43] = 21; profile[44] = 2; profile[46] = 0x93; profile[47] = 2;
    assert(driver->configureTemplate(profile, sizeof(profile)));
    driver->configureSyncState(1, 0, state + 0x6a5d);
}
bool native_shouldRunElection(void *);
void native_runElection(void *);
bool native_isPeerQualifiedForElection(void *, void *);
}
static bool lock(void *) {assert(!lockCount); ++lockCount; return true;}
static void unlock(void *) {assert(lockCount == 1); --lockCount;}

// NATIVE DECLARATIONS
enum {kIOReturnSuccess = 0, APPLE80211_VERSION = 1,
    APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START = 41,
    APPLE80211_M_AWDL_AVAILABILITY_WINDOW_EXTENSIONS_END = 43,
    APPLE80211_C_FLAG_80MHZ = 0x400, APPLE80211_C_FLAG_20MHZ = 1, APPLE80211_C_FLAG_2GHZ = 2, APPLE80211_C_FLAG_5GHZ = 4};
static uint64_t awdlNow() {return nowUS;}
static uint32_t OSSwapHostToBigInt32(uint32_t value) {return __builtin_bswap32(value);}
struct Radio {
    Radio() {awdlRadio.widthMHz=20;}
    RadioState awdlRadio = {};
    PeerTable awdlPeers = {};
    bool awdlChannelAllowed(unsigned channel,uint8_t width=20) {return width==20 && channel == 6;}
};
struct Controller {
    int postMessage(void *, unsigned type, const void *message, size_t size, bool) {
        if (type == APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START) {
            assert(size == sizeof(apple80211_availability_window_start));
            const auto start = static_cast<const apple80211_availability_window_start *>(message);
            put<uint32_t>(state, 0x3ef0, start->aw_counter);
            put<uint32_t>(state, 0x3f08, (start->aw_counter / 4) % 16);
            lastStartUS = nowUS;
        } else {
            assert(type == APPLE80211_M_AWDL_AVAILABILITY_WINDOW_EXTENSIONS_END);
            if (native_shouldRunElection(manager)) native_runElection(manager);
        }
        return kIOReturnSuccess;
    }
};
struct AirportItlwmAWDLInterface {
    Radio device;
    Radio *radio = &device;
    Controller controller;
    SyncEngine sync;
    bool requested = true, stopping = false, regulatoryPlanBlocked = false, regulatoryStopRequired = false;
    bool nativeWindowOpen = false, nativeWindowClosing = false;
    unsigned pendingCount = 0, inFlightCount = 0;
    uint64_t nativeWindowStarts = 1, nativeWindowEnds = 1, nativeWindowErrors = 0;
    uint64_t nativeWindowSerial = 0, nativeWindowEndUS = 0;
    uint16_t nativeWindowCounter = 0, nativeWindowChannel = 6, nativeWindowPeriodTU = 16, nativeWindowPresence = 4;
    apple80211_availability_window_extensions_end nativeWindowEnd = {};
    void drain() {assert(!pendingCount && !inFlightCount);}
    Controller *getController() {return &controller;}
    void updateAvailability();
};
// PRODUCTION AVAILABILITY

static void initialize(unsigned slot) {
    std::memset(state, 0, sizeof(state));
    std::memset(peerState, 0, sizeof(peerState));
    put(manager, 0x18, base); put(manager, 0x20, state);
    put(manager, 0, managerVtable); managerVtable[7] = reinterpret_cast<void *>(fixtureName);
    put(base, 0x78, &lockObject); put(base, 0x530, queue);
    put(base, 0x518, controller); put(controller, 0x120, controllerState);
    lockVtable[0x118 / sizeof(void *)] = reinterpret_cast<void *>(lock);
    lockVtable[0x120 / sizeof(void *)] = reinterpret_cast<void *>(unlock);
    put(queue, 0x10, queueState); put(queueState, 0x38, head); put<uint32_t>(queueState, 0x24, 1);
    put(head, 0, node); put(node, 0, head); put(node, 0x10, peer);
    put(peer, 0x18, peerBase); put(peer, 0x20, peerState);
    copyAddress(peerBase + 0x50, parent);
    put<uint32_t>(peerState, 0x78, 1);
    put<uint32_t>(peerState, 0x223, 42);
    put<uint64_t>(peerState, 0x2e8, (nowUS - 5000000) * 1000);
    state[0x2843] = 1;
    put<uint32_t>(state, 0x3ee8, 2);
    put<uint32_t>(state, 0x28cc, 1);
    put<uint32_t>(state, 0x3e7c, 3000);
    put<uint32_t>(state, 0x3f08, slot);
    copyAddress(state + 0x6a48, self);
    copyAddress(state + 0x6a57, parent); copyAddress(state + 0x6a5d, parent);
    put<uint32_t>(state, 0x6a63, 100);
    put<uint32_t>(state, 0x6a67, 1);
    put<uint32_t>(state, 0x6a6b, 540);
    put<uint32_t>(state, 0x6a6f, 531);
    put<uint32_t>(state, 0x6a73, 42);
    put<uint32_t>(state, 0xc278, 42);
    put<uint32_t>(state, 0x6ba0, 90);
    std::memcpy(state + 0x6a7f, state + 0x6a54, 43);
    programmed = lockCount = 0;
    lastStartUS = nowUS;
}

static uint64_t replayLoss(unsigned lastSlot, bool radioGrant) {
    nowUS = 100000000;
    initialize(lastSlot);
    AirportItlwmAWDLInterface interface;
    driver = &interface.sync;
    driver->start(self, 6, nowUS);
    fixtureProgramMaster(manager);
    programmed = 0;
    SyncEngine sender;
    sender.start(parent, 6, nowUS);
    sender.electionID = 42; sender.epochCounter = lastSlot * 4;
    uint8_t frame[512]; Advertisement advert = {};
    const auto length = sender.action(frame, sizeof(frame), nowUS, false, nullptr, 0);
    assert(parseMPDU(frame, length, advert) == ParseResult::Valid);
    auto &peers = interface.device.awdlPeers;
    assert(peers.update(parent, self, advert, nowUS, 6, -40));
    put<uint64_t>(peerState, 0x2e8, nowUS * 1000);
    driver->elect(peers, nowUS);
    assert(driver->transmitReady(nowUS));
    interface.nativeWindowCounter = lastSlot * 4;
    auto &radio = interface.device.awdlRadio;
    radio.ready = true; radio.channel = 6;
    const uint64_t origin = nowUS;
    uint64_t nextWatchdogUS = origin + 4000000;
    for (uint64_t elapsed = 3100000; elapsed < 12000000; elapsed += 8192) {
        nowUS = origin + elapsed;
        peers.expire(nowUS); driver->elect(peers, nowUS);
        assert(!driver->transmitReady(nowUS));
        assert(!driver->action(frame, sizeof(frame), nowUS, false, nullptr, 0));
        Window local = {}; assert(driver->window(nowUS, local));
        radio.dwell.running = radioGrant;
        radio.dwell.windowEndUS = nowUS + local.untilNextEAWUS;
        interface.updateAvailability();
        // Match the existing watchdog's one-second timer and missing-START threshold.
        if (nowUS >= nextWatchdogUS) {
            nextWatchdogUS += 1000000;
            if (!programmed && nowUS - lastStartUS > 1024000 && native_shouldRunElection(manager))
                native_runElection(manager);
        }
        if (programmed) {
            driver->elect(peers, nowUS);
            assert(driver->transmitReady(nowUS));
            assert(sameAddress(driver->master, self) && !driver->distance);
            driver = nullptr;
            return elapsed;
        }
    }
    driver = nullptr;
    assert(interface.nativeWindowStarts == 1);
    return 0;
}

int main() {
    unsigned blockedSlots = 0;
    for (unsigned slot = 0; slot < 16; ++slot) {
        initialize(slot);
        const bool allowed = native_shouldRunElection(manager);
        assert(allowed == (slot == 0));
        blockedSlots += !allowed;
    }
    initialize(9);
    state[0x2842] = 1;
    assert(native_shouldRunElection(manager));
    state[0x2843] = 0;
    assert(!native_shouldRunElection(manager));
    initialize(0);
    put<uint32_t>(state, 0x3ee8, 0);
    assert(!native_shouldRunElection(manager));
    initialize(9);
    assert(!native_isPeerQualifiedForElection(manager, peer));
    put<uint64_t>(peerState, 0x2e8, (nowUS - 1000000) * 1000);
    assert(native_isPeerQualifiedForElection(manager, peer));
    put<uint64_t>(peerState, 0x2e8, (nowUS - 5000000) * 1000);
    // Execute the native decision once the window has become eligible.
    put<uint32_t>(state, 0x3f08, 0);
    assert(native_shouldRunElection(manager));
    native_runElection(manager);
    assert(!lockCount && programmed == 1);
    assert(sameAddress(state + 0x6a57, self) && sameAddress(state + 0x6a5d, self));
    assert(get<uint32_t>(state, 0x6a67) == 0);
    unsigned recovered = 0;
    uint64_t longest = 0;
    for (unsigned slot = 1; slot < 16; ++slot) {
        const uint64_t recovery = replayLoss(slot, true);
        recovered += recovery != 0;
        if (recovery > longest) longest = recovery;
        assert(!replayLoss(slot, false));
    }
    std::printf("{\"normalNonzeroSlotsBlocked\":%u,\"slotZeroAllowed\":true,"
                "\"staleParentRejected\":true,\"nativeSelectedSelf\":true,"
                "\"driverRecoveredSlots\":%u,\"latestRecoveryUS\":%llu,"
                "\"withoutRadioGrantRecovered\":0}\n", blockedSlots, recovered,
                (unsigned long long)longest);
}
