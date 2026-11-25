#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#define DEBUG 1
#include "AirportItlwm/AWDL/NativePeerSnapshot.hpp"
#include "AirportItlwm/AWDL/NativeFrame.hpp"
#include "AirportItlwm/AWDL/ServiceMerge.hpp"
using namespace AirportAWDL;
enum { APPLE80211_C_FLAG_2GHZ = 8, APPLE80211_C_FLAG_5GHZ = 16 };
static uint64_t awdlNow() { return 42000000; }
static std::vector<unsigned> trace;

struct OSObject {
    bool awdl = true;
    OSObject *metaCast(const char *name) {
        assert(!strcmp(name, "IO80211AWDLPeerManager"));
        return awdl ? this : nullptr;
    }
};
static OSObject nativeManager, *manager = &nativeManager;
static unsigned managerReads, prints, allocations, frees;
static bool allocationFails;
static int nativeError;
static uint32_t nativeUsed;
static void *expectedInterface;
static void *IOMallocZero(size_t size) {
    assert(size == sizeof(NativePeerSnapshotControl));
    ++allocations;
    return allocationFails ? nullptr : calloc(1, size);
}
static void IOFree(void *data, size_t size) {
    assert(size == sizeof(NativePeerSnapshotControl));
    assert(data);
    ++frees;
    free(data);
}
extern "C" OSObject *AirportAWDLGetNativePeerManager(void *interface) {
    assert(interface == expectedInterface);
    ++managerReads;
    return manager;
}
extern "C" int AirportAWDLPrintNativePeers(void *object, void *payload) {
    assert(object == &nativeManager && nativeManager.awdl);
    trace.push_back(3);
    ++prints;
    auto ctl = static_cast<NativePeerSnapshotControl *>(payload);
    assert(ctl->version == 1 && ctl->capacity == 8192 && ctl->color == 0);
    ctl->version = ctl->capacity = 0;
    for (size_t i = 0; i < sizeof(*ctl); ++i)
        assert(reinterpret_cast<uint8_t *>(ctl)[i] == 0);
    ctl->used = nativeUsed;
    memset(ctl->text, 'P', NativePeerSnapshotCapacity);
    return nativeError;
}
struct WorkLoop {
    bool gated = true, thread = true;
    bool inGate() { return gated; }
    bool onThread() { return thread; }
};
struct ItlIwx {
    WorkLoop loop;
    struct {
        bool changing = false, heavyRX = false, heavyTX = false;
        bool heavyReceiveActive(uint64_t) const { return heavyRX; }
        bool heavyTransmitActive(uint64_t) const { return heavyTX; }
    } awdlRadio;
    WorkLoop *getMainWorkLoop() { return &loop; }
};
struct IO80211Controller {
    WorkLoop *queue;
    WorkLoop *getWorkQueue() { return queue; }
};
struct Buffer {
    uint8_t bytes[1536];
    size_t size = 0;
    void *getBytesNoCopy() { return bytes; }
    size_t getCapacity() { return sizeof(bytes); }
    void setLength(size_t length) { size = length; }
    void returnBuffer() {}
};
struct Pool { Buffer buffer; Buffer *getBuffer() { return &buffer; } };
struct BSD { void *getIfnet() { return this; } };
struct AirportItlwmAWDLInterface {
    ItlIwx device, *radio = &device;
    IO80211Controller owner{&device.loop}, *controller = &owner;
    IO80211Controller *getController() { return controller; }
    BSD interface, *bsd = &interface;
    Pool pool;
    unsigned actionDropped = 0, actionDelivered = 0, references = 1;
    ServiceMergeCache serviceMerge = {};
    bool serviceMergeEnabled = false;
    bool stopDuringTap = false;
    Pool *getMgmtFramePool() { return &pool; }
    void retain() { ++references; trace.push_back(1); }
    void release() { assert(references == 2); --references; trace.push_back(4); }
    void bpfTapInput(Buffer *buffer, unsigned type, void *, size_t size) {
        assert(references == 2 && buffer == &pool.buffer && type == 12 && size == 72);
        trace.push_back(2);
        if (stopDuringTap) stopping = true;
    }
    static void receiveAction(void *, const uint8_t *, size_t, uint16_t, int16_t, uint64_t);
    bool requested = true, stopping = false, receiveFilterChanging = false;
    bool llwActive = false, llwParamsEnabled = false;
    bool nativePeerSampled = false;
    uint64_t lastNativePeerSnapshotUS = 0;
    std::map<std::string, std::string> text;
    std::map<std::string, uint64_t> numbers;
    void setProperty(const char *key, const char *value) { text[key] = value; }
    void setProperty(const char *key, uint64_t value, unsigned bits) {
        assert(bits == 32 || bits == 64);
        numbers[key] = value;
    }
    void publishNativePeers(uint64_t);
    void sample(uint64_t now) { expectedInterface = this; publishNativePeers(now); }
};

// PRODUCTION

static void reset() {
    trace.clear();
    nativeManager.awdl = true;
    manager = &nativeManager;
    managerReads = prints = allocations = frees = 0;
    allocationFails = false;
    nativeError = 0;
    nativeUsed = 11;
}
static void failures() {
    reset();
    AirportItlwmAWDLInterface interface;
    interface.text["NativePeerSummary"] = "old snapshot";
    manager = nullptr;
    interface.sample(1);
    assert(interface.numbers["NativePeerSummaryError"] == ENXIO);
    assert(interface.text["NativePeerSummary"].empty());
    assert(managerReads == 1 && !prints && !allocations && !frees);
    manager = &nativeManager;
    nativeManager.awdl = false;
    interface.sample(10000001);
    assert(interface.numbers["NativePeerSummaryError"] == EINVAL);
    assert(managerReads == 2 && !prints && !allocations && !frees);
    nativeManager.awdl = true;
    allocationFails = true;
    interface.sample(20000001);
    assert(interface.numbers["NativePeerSummaryError"] == ENOMEM);
    assert(!prints && allocations == 1 && !frees);
    allocationFails = false;
    nativeError = EIO;
    nativeUsed = UINT32_MAX;
    interface.sample(30000001);
    assert(interface.numbers["NativePeerSummaryError"] == EIO);
    assert(interface.numbers["NativePeerSummaryBytes"] == 0);
    assert(interface.text["NativePeerSummary"].empty());
    assert(prints == 1 && allocations == 2 && frees == 1);
    nativeError = 0;
    interface.sample(40000001);
    assert(interface.numbers["NativePeerSummaryError"] == EOVERFLOW);
    assert(interface.numbers["NativePeerSummaryBytes"] == 0);
    assert(interface.text["NativePeerSummary"].empty());
    assert(prints == 2 && allocations == 3 && frees == 2);
}
static void success() {
    reset();
    AirportItlwmAWDLInterface interface;
    interface.sample(42);
    assert(interface.text["NativePeerSummary"] == std::string(nativeUsed, 'P'));
    assert(interface.numbers["NativePeerSummaryError"] == 0);
    assert(interface.numbers["NativePeerSummaryBytes"] == nativeUsed);
    assert(interface.numbers["NativePeerSummaryTimeUS"] == 42);
    assert(prints == 1 && allocations == frees);
    nativeUsed = NativePeerSnapshotCapacity;
    interface.sample(10000042);
    assert(interface.text["NativePeerSummary"].size() == NativePeerSnapshotCapacity);
    assert(interface.numbers["NativePeerSummaryError"] == 0);
    assert(prints == 2 && allocations == frees);
    nativeUsed = 0;
    interface.sample(20000042);
    assert(interface.text["NativePeerSummary"].empty());
    assert(interface.numbers["NativePeerSummaryError"] == 0);
    assert(prints == 3 && allocations == frees);
}
static void scheduling() {
    reset();
    AirportItlwmAWDLInterface interface;
    interface.requested = false;
    interface.sample(0);
    interface.requested = true;
    interface.stopping = true;
    interface.sample(0);
    interface.stopping = false;
    interface.controller = nullptr;
    interface.sample(0);
    interface.controller = &interface.owner;
    interface.owner.queue = nullptr;
    interface.sample(0);
    interface.owner.queue = &interface.device.loop;
    interface.radio->loop.gated = false;
    interface.sample(0);
    interface.radio->loop.gated = true;
    interface.radio->loop.thread = false;
    interface.sample(0);
    interface.radio->loop.thread = true;
    interface.radio->awdlRadio.changing = true;
    interface.sample(0);
    interface.radio->awdlRadio.changing = false;
    interface.receiveFilterChanging = true;
    interface.sample(0);
    interface.receiveFilterChanging = false;
    auto radio = interface.radio;
    interface.radio = nullptr;
    interface.sample(0);
    interface.radio = radio;
    assert(!managerReads && !interface.nativePeerSampled);
    interface.sample(0);
    interface.sample(9999999);
    assert(managerReads == 1);
    interface.sample(10000000);
    assert(managerReads == 2);
    interface.sample(1);
    assert(managerReads == 3);
    assert(allocations == frees);
}
static void receiveContext() {
    reset();
    AirportItlwmAWDLInterface interface;
    const uint8_t senderAddress[] = {2, 3, 4, 5, 6, 7};
    SyncEngine sender;
    sender.start(senderAddress, 6, awdlNow());
    uint8_t frame[1536];
    const size_t length = sender.action(frame, sizeof(frame), awdlNow(), true, nullptr, 0);
    assert(length);
    expectedInterface = &interface;
    interface.receiveAction(&interface, frame, length, 6, -40, awdlNow());
    assert((trace == std::vector<unsigned>{1, 2, 3, 4}));
    assert(interface.references == 1 && prints == 1 && allocations == frees);
    trace.clear();
    interface.receiveAction(&interface, frame, length, 6, -40, awdlNow());
    assert((trace == std::vector<unsigned>{1, 2, 4}));
    interface.stopDuringTap = true;
    interface.nativePeerSampled = false;
    trace.clear();
    interface.receiveAction(&interface, frame, length, 6, -40, awdlNow());
    assert((trace == std::vector<unsigned>{1, 2, 4}));
    assert(!interface.nativePeerSampled && interface.references == 1 && prints == 1);
}
int main() { failures(); success(); scheduling(); receiveContext(); }
