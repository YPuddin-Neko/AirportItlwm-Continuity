#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "AirportItlwm/AWDL/DataFrame.hpp"
#include "AirportItlwm/AWDL/NativePeer.hpp"
using namespace AirportAWDL;
using Bytes = std::vector<uint8_t>;

static const uint8_t selfAddress[6] = {2, 0, 0, 0, 0, 1};
static const uint8_t remoteAddress[6] = {2, 0, 0, 0, 0, 2};
static const uint8_t otherAddress[6] = {2, 0, 0, 0, 0, 3};
static const uint8_t groupAddress[6] = {0x33, 0x33, 0, 0, 0, 0xfb};
static std::vector<std::string> trace;
static unsigned allocations, frees, outstanding;
static bool allocationFails;
static std::function<void()> onNativeNotification;
using AbsoluteTime = uint64_t;
static constexpr AbsoluteTime clockAbsolute = 0x123456789abcdef0ULL;
static unsigned clockReads, rxActivityCalls, txActivityCalls;
static unsigned counterCalls;
static uint64_t rxCounterPackets, rxCounterBytes, txCounterPackets, txCounterBytes;
static void clock_get_uptime(AbsoluteTime *time) { ++clockReads; *time = clockAbsolute; }

struct Packet {
    Bytes bytes;
    size_t length = 0, total = 0;
    explicit Packet(size_t capacity) : bytes(capacity, 0xa5) {}
};
using mbuf_t = Packet *;
enum { MBUF_DONTWAIT = 1 };
static int mbuf_allocpacket(unsigned flags, size_t length, unsigned *chunks, mbuf_t *out) {
    assert(flags == MBUF_DONTWAIT && chunks && *chunks == 1 && out && !*out);
    trace.push_back("allocate");
    ++allocations;
    if (allocationFails) return ENOMEM;
    *out = new Packet(length);
    ++outstanding;
    return 0;
}
static void mbuf_setlen(mbuf_t packet, size_t length) { packet->length = length; }
static void mbuf_pkthdr_setlen(mbuf_t packet, size_t length) { packet->total = length; }
static void *mbuf_data(mbuf_t packet) { return packet->bytes.data(); }
static void mbuf_freem(mbuf_t packet) {
    assert(packet && outstanding);
    trace.push_back("free");
    ++frees; --outstanding;
    delete packet;
}

struct WorkLoop {
    bool gated = true, thread = true;
    bool inGate() { return gated; }
    bool onThread() { return thread; }
};
struct Controller {
    WorkLoop loop, *queue = &loop;
    WorkLoop *getWorkQueue() { return queue; }
};
struct Radio {
    WorkLoop *loop;
    WorkLoop *getMainWorkLoop() { return loop; }
};
struct BSD {
    bool attached = true, accepts = true;
    unsigned inputs = 0;
    Bytes received;
    void *getIfnet() { return attached ? this : nullptr; }
    unsigned inputPacket(mbuf_t packet, unsigned length) {
        assert(attached && packet && packet->length == length && packet->total == length);
        assert(length == packet->bytes.size());
        trace.push_back("bsd");
        ++inputs;
        received = packet->bytes;
        mbuf_freem(packet);
        return accepts ? 1 : 0;
    }
};
struct OSObject {
    enum Kind { Manager, Peer, Other } kind;
    explicit OSObject(Kind value) : kind(value) {}
    OSObject *metaCast(const char *name) {
        if (!strcmp(name, "IO80211AWDLPeerManager")) return kind == Manager ? this : nullptr;
        assert(!strcmp(name, "IO80211AWDLPeer"));
        return kind == Peer ? this : nullptr;
    }
    void retain() { assert(false && "native peer lookup is borrowed"); }
    void release() { assert(false && "native peer lookup is borrowed"); }
};
static OSObject nativeManager(OSObject::Manager), nativePeer(OSObject::Peer);
static OSObject *manager = &nativeManager, *peer = &nativePeer;
static bool lockPermitted = true, lockHeld;
static unsigned managerReads, lockCalls, unlockCalls, findCalls, nativeCalls;
static int nativeReachability;
static uint32_t nativeReachabilityCounts[4];
static unsigned reachabilityIncrements, reachabilityDecrements;
static bool nativePolicyAllows, nativeRoutable;
static void *expectedInterface;
extern "C" OSObject *AirportAWDLGetNativePeerManager(void *interface) {
    assert(interface == expectedInterface);
    ++managerReads;
    trace.push_back("manager");
    return manager;
}
extern "C" bool AirportAWDLLockNativePeers(void *object) {
    assert(object == &nativeManager && nativeManager.kind == OSObject::Manager && !lockHeld);
    ++lockCalls;
    trace.push_back("lock");
    lockHeld = lockPermitted;
    return lockHeld;
}
extern "C" void AirportAWDLUnlockNativePeers(void *object) {
    assert(object == &nativeManager && lockHeld);
    ++unlockCalls;
    trace.push_back("unlock");
    lockHeld = false;
}
extern "C" OSObject *AirportAWDLFindNativePeer(void *object, uint8_t *address) {
    assert(object == &nativeManager && lockHeld && sameAddress(address, remoteAddress));
    ++findCalls;
    trace.push_back("find");
    return peer;
}
extern "C" void AirportAWDLNativePeerReceivedData(void *object) {
    assert(object == &nativePeer && nativePeer.kind == OSObject::Peer && lockHeld);
    ++nativeCalls;
    trace.push_back("notify");
    // This boundary does not simulate native policy, reachability, or ND insertion.
    if (onNativeNotification) onNativeNotification();
}
extern "C" int AirportAWDLNativePeerReachability(void *object) {
    assert(object == &nativePeer && nativePeer.kind == OSObject::Peer && lockHeld);
    return nativeReachability;
}
extern "C" void AirportAWDLIncrementNativeReachability(void *object, int state) {
    assert(object == &nativeManager && lockHeld && state >= 0 && state < 4);
    ++reachabilityIncrements;
    ++nativeReachabilityCounts[state];
}
extern "C" void AirportAWDLDecrementNativeReachability(void *object, int state) {
    assert(object == &nativeManager && lockHeld && state >= 0 && state < 4);
    ++reachabilityDecrements;
    --nativeReachabilityCounts[state];
}
extern "C" void AirportAWDLSetNativeRxActivity(void *object, uint64_t time) {
    assert(object == &nativeManager && nativeManager.kind == OSObject::Manager);
    assert(time == clockAbsolute && !lockHeld);
    ++rxActivityCalls;
    trace.push_back("rx-activity");
}
extern "C" void AirportAWDLSetNativeTxActivity(void *object, uint64_t time) {
    assert(object == &nativeManager && nativeManager.kind == OSObject::Manager);
    assert(time == clockAbsolute && !lockHeld);
    ++txActivityCalls;
    trace.push_back("tx-activity");
}
extern "C" void AirportAWDLUpdateNativeDataCounters(void *object, const AirportAWDLNativeDataCountersDelta *delta) {
    assert(object == &nativeManager && nativeManager.kind == OSObject::Manager && !lockHeld);
    assert(delta->txPackets + delta->rxPackets == 1);
    assert(delta->txPackets ? delta->txBytes && !delta->rxBytes : delta->rxBytes && !delta->txBytes);
    const auto bytes = reinterpret_cast<const uint8_t *>(delta);
    for (unsigned offset = 0; offset < 0x108; offset += 8) {
        uint64_t value;
        memcpy(&value, bytes + offset, sizeof(value));
        if (offset < 0x08 || offset > 0x20) assert(!value);
    }
    txCounterPackets += delta->txPackets; txCounterBytes += delta->txBytes;
    rxCounterPackets += delta->rxPackets; rxCounterBytes += delta->rxBytes;
    ++counterCalls;
    trace.push_back(delta->txPackets ? "tx-counters" : "rx-counters");
}
struct AirportItlwmAWDLInterface {
    Controller owner, *controller = &owner;
    Radio device{&owner.loop}, *radio = &device;
    BSD storage, *bsd = &storage;
    struct { uint8_t octet[6]; } address;
    bool requested = true, running = true, stopping = false;
    uint64_t rxDelivered = 0, rxDropped = 0;
    uint64_t nativeDataPeerNotifications = 0, nativeDataPeerLockMisses = 0;
    unsigned references = 1;
    AirportItlwmAWDLInterface() { copyAddress(address.octet, selfAddress); expectedInterface = this; }
    Controller *getController() { return controller; }
    void retain() { ++references; }
    void release() { assert(references > 1); --references; }
    void recordNativeDataActivity(bool, uint32_t);
    void notifyNativeDataPeer(const DataView &);
    bool deliverLLWOrMirror(const DataView &, mbuf_t, size_t) { return false; }
    static void receive(void *, const DataView &);
};

// PRODUCTION

static void reset() {
    assert(!outstanding && !lockHeld);
    trace.clear();
    allocations = frees = managerReads = lockCalls = unlockCalls = findCalls = nativeCalls = 0;
    clockReads = rxActivityCalls = txActivityCalls = 0;
    counterCalls = 0;
    rxCounterPackets = rxCounterBytes = txCounterPackets = txCounterBytes = 0;
    allocationFails = false;
    nativeManager.kind = OSObject::Manager; nativePeer.kind = OSObject::Peer;
    manager = &nativeManager; peer = &nativePeer;
    lockPermitted = true;
    nativePolicyAllows = nativeRoutable = false;
    nativeReachability = 3;
    std::fill(std::begin(nativeReachabilityCounts), std::end(nativeReachabilityCounts), 0);
    nativeReachabilityCounts[nativeReachability] = 1;
    reachabilityIncrements = reachabilityDecrements = 0;
    onNativeNotification = nullptr;
}
static Bytes ethernet(const uint8_t *destination = selfAddress) {
    Bytes bytes(14 + 40 + 20, 0);
    copyAddress(bytes.data(), destination);
    copyAddress(bytes.data() + 6, remoteAddress);
    putBE16(bytes.data() + 12, 0x86dd);
    bytes[14] = 0x60;
    putBE16(bytes.data() + 18, 20);
    bytes[20] = 6; bytes[21] = 64;
    for (size_t i = 22; i < bytes.size(); ++i) bytes[i] = uint8_t(i);
    return bytes;
}
static DataView view(Bytes &bytes) {
    DataView result = {};
    copyAddress(result.destination, bytes.data());
    copyAddress(result.source, bytes.data() + 6);
    result.payload = bytes.data() + 14; result.size = bytes.size() - 14;
    return result;
}
static Bytes mpdu(const uint8_t *destination = selfAddress, uint16_t sequence = 7) {
    auto bytes = ethernet(destination);
    Bytes packet(bytes.size() + 26);
    assert(dataFromEthernet(bytes.data(), bytes.size(), packet.data(), packet.size(), 1, sequence) == packet.size());
    return packet;
}
static void knownPeer(PeerTable &table) {
    Advertisement advertisement = {};
    assert(table.update(remoteAddress, selfAddress, advertisement, 100, 6, -40));
}
static bool receiveMPDU(AirportItlwmAWDLInterface &interface, DataReceiveState &state,
                        PeerTable &table, Bytes &packet, uint64_t now = 200) {
    return state.receive(packet.data(), packet.size(), table, selfAddress, now, false,
                         AirportItlwmAWDLInterface::receive, &interface);
}

static void directedAndOrder() {
    for (bool policyAllows : {false, true}) {
        reset();
        AirportItlwmAWDLInterface interface;
        nativePolicyAllows = policyAllows;
        nativeRoutable = policyAllows;
        PeerTable table = {}; knownPeer(table);
        DataReceiveState state = {};
        auto packet = mpdu();
        assert(receiveMPDU(interface, state, table, packet));
        assert((trace == std::vector<std::string>{"allocate", "manager", "rx-counters", "rx-activity", "manager", "lock", "find", "notify", "unlock", "bsd", "free"}));
        assert(rxActivityCalls == 1 && !txActivityCalls && clockReads == 1);
        assert(counterCalls == 1 && rxCounterPackets == 1 && rxCounterBytes == ethernet().size());
        assert(!txCounterPackets && !txCounterBytes);
        assert(nativeCalls == 1 && interface.nativeDataPeerNotifications == 1);
        assert(nativePolicyAllows == policyAllows && nativeRoutable == policyAllows);
        assert(interface.storage.received == ethernet() && interface.rxDelivered == 1 && !interface.rxDropped);
        assert(lockCalls == 1 && unlockCalls == 1 && !lockHeld && !outstanding);
        assert(state.observed == 1 && !table.find(remoteAddress, 200)->nativeCached);
        assert(table.find(remoteAddress, 200)->lastSeenUS == 100 && interface.references == 1);
    }
    reset();
    AirportItlwmAWDLInterface interface;
    auto bytes = ethernet(); const auto original = bytes; const auto frame = view(bytes);
    onNativeNotification = [&] { std::fill(bytes.begin(), bytes.end(), 0x5a); };
    interface.receive(&interface, frame);
    assert(interface.storage.received == original && nativeCalls == 1 && !outstanding);
}
static void receiveFilters() {
    reset();
    AirportItlwmAWDLInterface interface;
    PeerTable table = {};
    DataReceiveState state = {};
    auto packet = mpdu();
    assert(receiveMPDU(interface, state, table, packet));
    assert(state.unknownPeer == 1 && !allocations && !nativeCalls);
    knownPeer(table);
    assert(receiveMPDU(interface, state, table, packet));
    assert(receiveMPDU(interface, state, table, packet));
    assert(state.duplicate == 1 && nativeCalls == 1 && interface.storage.inputs == 1);
    assert(rxActivityCalls == 1 && !txActivityCalls);
    assert(counterCalls == 1 && rxCounterPackets == 1 && rxCounterBytes == ethernet().size());
    auto other = mpdu(otherAddress, 8);
    assert(receiveMPDU(interface, state, table, other));
    assert(state.otherDestination == 1 && nativeCalls == 1 && interface.storage.inputs == 1);
    auto group = mpdu(groupAddress, 9);
    assert(receiveMPDU(interface, state, table, group));
    assert(nativeCalls == 1 && interface.storage.inputs == 2 && interface.storage.received == ethernet(groupAddress));
    assert(receiveMPDU(interface, state, table, packet, 100 + PeerTable::LifetimeUS));
    assert(state.unknownPeer == 2 && interface.storage.inputs == 2 && !outstanding);
    assert(rxActivityCalls == 1 && !txActivityCalls);
    for (unsigned candidate = 0; candidate < 4; ++candidate) {
        reset();
        AirportItlwmAWDLInterface target;
        auto bytes = ethernet(); auto frame = view(bytes);
        if (candidate == 0) copyAddress(frame.destination, otherAddress);
        if (candidate == 1) copyAddress(frame.source, selfAddress);
        if (candidate == 2) copyAddress(frame.source, groupAddress);
        if (candidate == 3) memset(frame.source, 0, 6);
        target.notifyNativeDataPeer(frame);
        assert(!managerReads && !nativeCalls && !target.nativeDataPeerNotifications);
    }
}
static void nativeLookup() {
    for (unsigned failure = 0; failure < 5; ++failure) {
        reset();
        AirportItlwmAWDLInterface interface;
        auto bytes = ethernet(); const auto frame = view(bytes);
        if (failure == 0) manager = nullptr;
        if (failure == 1) nativeManager.kind = OSObject::Other;
        if (failure == 2) lockPermitted = false;
        if (failure == 3) peer = nullptr;
        if (failure == 4) nativePeer.kind = OSObject::Other;
        interface.receive(&interface, frame);
        assert(!nativeCalls && !interface.nativeDataPeerNotifications && !lockHeld);
        assert(interface.storage.inputs == 1 && interface.rxDelivered == 1 && !outstanding);
        assert(lockCalls == (failure >= 2) && unlockCalls == (failure >= 3));
        assert(findCalls == (failure >= 3));
        assert(interface.nativeDataPeerLockMisses == (failure == 2));
        assert(rxActivityCalls == (failure >= 2) && !txActivityCalls);
        assert(counterCalls == (failure >= 2));
    }
}
static void contextGuards() {
    for (unsigned failure = 0; failure < 7; ++failure) {
        reset();
        AirportItlwmAWDLInterface interface;
        auto bytes = ethernet(); const auto frame = view(bytes);
        if (failure == 0) interface.stopping = true;
        if (failure == 1) interface.requested = false;
        if (failure == 2) interface.running = false;
        if (failure == 3) interface.controller = nullptr;
        if (failure == 4) interface.owner.queue = nullptr;
        if (failure == 5) interface.owner.loop.gated = false;
        if (failure == 6) interface.owner.loop.thread = false;
        interface.notifyNativeDataPeer(frame);
        assert(!managerReads && !lockCalls && !nativeCalls && !interface.nativeDataPeerLockMisses);
    }
}
static void receiveFailures() {
    for (unsigned failure = 0; failure < 6; ++failure) {
        reset();
        AirportItlwmAWDLInterface interface;
        auto bytes = ethernet(); const auto frame = view(bytes);
        if (failure == 0) interface.bsd = nullptr;
        if (failure == 1) interface.running = false;
        if (failure == 2) interface.storage.attached = false;
        if (failure == 3) allocationFails = true;
        if (failure == 4) bytes[14] = 0x40;
        if (failure == 5) interface.storage.accepts = false;
        interface.receive(&interface, frame);
        assert(interface.rxDropped == 1 && !interface.rxDelivered && !outstanding);
        assert(nativeCalls == (failure == 5));
        assert(allocations == (failure >= 3));
        assert(frees == (failure >= 4));
        assert(interface.storage.inputs == (failure == 5));
        assert(rxActivityCalls == (failure == 5) && !txActivityCalls);
        assert(counterCalls == (failure == 5));
    }
}
static void stoppedReceive() {
    for (unsigned stop = 0; stop < 4; ++stop) {
        reset();
        AirportItlwmAWDLInterface interface;
        PeerTable table = {}; knownPeer(table);
        DataReceiveState state = {};
        auto packet = mpdu();
        if (stop == 0) interface.stopping = true;
        if (stop == 1) interface.running = false;
        if (stop == 2) interface.bsd = nullptr;
        if (stop == 3) interface.storage.attached = false;
        assert(receiveMPDU(interface, state, table, packet));
        assert(!managerReads && !lockCalls && !nativeCalls && !unlockCalls && !lockHeld);
        assert(!interface.storage.inputs && !interface.rxDelivered && interface.rxDropped == 1);
        assert(!allocations && !frees && !outstanding && interface.references == 1);
        assert(!rxActivityCalls && !txActivityCalls && !clockReads && !counterCalls);
    }
}
static void activityContext() {
    enum Scenario { Timer, Stopping, Unrequested, NotRunning, NoController,
                    NoQueue, OutsideGate, DifferentQueue, NoManager, WrongManager };
    for (bool transmit : {false, true}) for (unsigned scenario = Timer; scenario <= WrongManager; ++scenario) {
        reset();
        AirportItlwmAWDLInterface interface;
        WorkLoop unrelated; unrelated.gated = true;
        interface.owner.loop.thread = false;
        if (scenario == Stopping) interface.stopping = true;
        if (scenario == Unrequested) interface.requested = false;
        if (scenario == NotRunning) interface.running = false;
        if (scenario == NoController) interface.controller = nullptr;
        if (scenario == NoQueue) interface.owner.queue = nullptr;
        if (scenario == OutsideGate) interface.owner.loop.gated = false;
        if (scenario == DifferentQueue) interface.owner.queue = &unrelated;
        if (scenario == NoManager) manager = nullptr;
        if (scenario == WrongManager) nativeManager.kind = OSObject::Other;
        interface.recordNativeDataActivity(transmit, 1514);
        assert(clockReads == (scenario == Timer));
        assert(rxActivityCalls == (scenario == Timer && !transmit));
        assert(txActivityCalls == (scenario == Timer && transmit));
        assert(counterCalls == (scenario == Timer));
        assert(rxCounterPackets == (scenario == Timer && !transmit));
        assert(txCounterPackets == (scenario == Timer && transmit));
        assert(rxCounterBytes == 1514 * rxCounterPackets && txCounterBytes == 1514 * txCounterPackets);
        assert(!lockCalls && !findCalls && !nativeCalls);
    }
    reset();
    AirportItlwmAWDLInterface interface;
    interface.owner.loop.thread = false;
    interface.storage.accepts = false;
    auto bytes = ethernet(); const auto frame = view(bytes);
    interface.receive(&interface, frame);
    assert(rxActivityCalls == 1 && !nativeCalls && !lockCalls && interface.rxDropped == 1);
    assert(counterCalls == 1 && rxCounterBytes == bytes.size());
    assert((trace == std::vector<std::string>{"allocate", "manager", "rx-counters", "rx-activity", "bsd", "free"}));
}
static void receiveCounterDeltas() {
    reset();
    AirportItlwmAWDLInterface interface;
    PeerTable table = {}; knownPeer(table);
    DataReceiveState state = {};
    rxCounterPackets = 17; rxCounterBytes = 12345;
    txCounterPackets = 23; txCounterBytes = 67890;
    uint64_t total = 12345;
    unsigned sequence = 7;
    for (unsigned length : {74, 114, 1514}) {
        auto bytes = ethernet(); bytes.resize(length);
        putBE16(bytes.data() + 18, uint16_t(length - 54));
        Bytes packet(length + 26);
        assert(dataFromEthernet(bytes.data(), bytes.size(), packet.data(), packet.size(),
                               sequence, sequence) == packet.size());
        assert(receiveMPDU(interface, state, table, packet));
        assert(receiveMPDU(interface, state, table, packet));
        total += length;
        assert(counterCalls == sequence - 6 && rxCounterPackets == 17 + counterCalls);
        assert(rxCounterBytes == total && rxActivityCalls == counterCalls);
        assert(txCounterPackets == 23 && txCounterBytes == 67890);
        ++sequence;
    }
    assert(state.duplicate == 3 && interface.storage.inputs == 3 && !outstanding);
}
static void nativeReachabilityAccounting() {
    for (int initial : {2, 3}) {
        reset();
        AirportItlwmAWDLInterface interface;
        auto bytes = ethernet(); const auto frame = view(bytes);
        nativeReachabilityCounts[3] = 0;
        nativeReachabilityCounts[initial] = 1;
        nativeReachability = initial;
        // The native policy can leave the state unchanged (RSSI, posting or peer policy).
        interface.notifyNativeDataPeer(frame);
        assert(nativeReachability == initial && nativeReachabilityCounts[initial] == 1);
        assert(!reachabilityIncrements && !reachabilityDecrements);
        onNativeNotification = [] { nativeReachability = 1; };
        for (unsigned round = 0; round < 20; ++round) {
            interface.notifyNativeDataPeer(frame);
            assert(nativeReachabilityCounts[1] == 1 && !nativeReachabilityCounts[initial]);
            assert(reachabilityIncrements == round + 1 && reachabilityDecrements == round + 1);
            // Subsequent RX stays routable and must not migrate the counts again.
            interface.notifyNativeDataPeer(frame);
            assert(nativeReachabilityCounts[1] == 1 && !nativeReachabilityCounts[initial]);
            assert(reachabilityIncrements == round + 1 && reachabilityDecrements == round + 1);
            // 24H32 monitorPeer applies this old/new delta on a later reachability loss.
            assert(!lockHeld);
            --nativeReachabilityCounts[nativeReachability];
            nativeReachability = initial;
            ++nativeReachabilityCounts[nativeReachability];
            assert(!nativeReachabilityCounts[1] && nativeReachabilityCounts[initial] == 1);
        }
        assert(!lockHeld && lockCalls == unlockCalls && nativeCalls == 41);
    }
}
int main() {
    switch (NATIVE_RX_CASE) {
        case 1: directedAndOrder(); break;
        case 2: receiveFilters(); break;
        case 3: nativeLookup(); break;
        case 4: contextGuards(); break;
        case 5: receiveFailures(); break;
        case 6: stoppedReceive(); break;
        case 7: activityContext(); break;
        case 8: receiveCounterDeltas(); break;
        case 9: nativeReachabilityAccounting(); break;
        default: assert(false);
    }
}
