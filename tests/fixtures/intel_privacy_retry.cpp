#include "../../bluetooth/BlueToolFixup/IntelPrivacy.hpp"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <vector>

using namespace IntelPrivacy;
namespace {
constexpr uint64_t Identity = 0x005c13cce3af8cULL;
constexpr uint64_t RPA = 0x0152ef130b6facULL;
constexpr uint64_t NextRPA = 0x0152ef130b6fadULL;
alignas(8) uint8_t manager[0x600], pendingStorage[56], installedStorage[56];
uint8_t report[80], uuidBytes[16];
void *database = reinterpret_cast<void *>(0x12340000);
bool managerLocked, hciLocked, supported, multiple, noUUID;
uint64_t ticks, identityValue, resolverIdentity;
uint32_t resolverStatus, removeStatus, addStatus, restoreStatus;
uint8_t resolverFound;
bool removeMissingCompletion, addMissingCompletion, invalidateRemove, invalidateAdd;
bool sleepRemove, sleepAdd, expireRemove, expireAdd, changeGateAdd, fatalCompletion;
unsigned addCalls, removeCalls, releases, claims;
std::set<uint64_t> completed;
std::vector<unsigned> commands;
struct NativeFatal {};

NativeAddressSet &pending() { return *reinterpret_cast<NativeAddressSet *>(manager + 0x150); }
NativeAddressSet &installed() { return *reinterpret_cast<NativeAddressSet *>(manager + 0x168); }
TypedAddress address(uint64_t packed) {
    TypedAddress out {};
    for (int i = 5; i >= 0; --i) { out.bytes[i] = static_cast<uint8_t>(packed); packed >>= 8; }
    out.type = static_cast<uint8_t>(packed);
    return out;
}
uint64_t packed(const TypedAddress &a) {
    uint64_t v = a.type;
    for (unsigned i = 0; i != 6; ++i) v = (v << 8) | a.bytes[i];
    return v;
}
uint32_t resolver(const TypedAddress *, TypedAddress *out, uint8_t *found) {
    *out = address(resolverIdentity); *found = resolverFound; return resolverStatus;
}
void *builder(NativeAddressSet *, void *, void *) { std::abort(); }
uint64_t clockNow() { return ticks; }
void *allocate(size_t count, size_t bytes, uint64_t type) {
    assert(count == 1 && bytes == sizeof(State) && type == 0);
    return std::calloc(count, bytes);
}
void release(void *p) { std::free(p); }
uint32_t gate() { return supported; }
int timebase(TimebaseInfo *info) { *info = {1,1}; return 0; }
void *makeUUID(const uint8_t *bytes) {
    assert(managerLocked && std::memcmp(bytes, uuidBytes, 16) == 0);
    return noUUID ? nullptr : uuidBytes;
}
void *claim(void *uuid) { ++claims; return uuid; }
void releaseObject(void *uuid) { assert(uuid == uuidBytes); ++releases; }
void (*releaseObjectPointer)(void *) = releaseObject;
bool multipleAddresses(void *db, void *uuid) {
    assert(managerLocked && db == database && uuid == uuidBytes); return multiple;
}
uint64_t identity(void *db, void *uuid, bool originalAddress) {
    assert(managerLocked && db == database && uuid == uuidBytes && !originalAddress);
    return identityValue;
}
void lockHCI() { assert(!hciLocked); hciLocked = true; }
void unlockHCI() { assert(hciLocked); hciLocked = false; }
bool contains(const TypedAddress *a) {
    assert(hciLocked);
    // The exact native packed-key to seven-byte ABI is checked independently
    // with constants, rather than accepting a test-generated alternate layout.
    if (packed(*a) == Identity) {
        const uint8_t wire[] = {0,0x5c,0x13,0xcc,0xe3,0xaf,0x8c};
        assert(std::memcmp(a, wire, sizeof(wire)) == 0);
    }
    if (packed(*a) == RPA) {
        const uint8_t wire[] = {1,0x52,0xef,0x13,0x0b,0x6f,0xac};
        assert(std::memcmp(a, wire, sizeof(wire)) == 0);
    }
    return completed.count(packed(*a));
}
uint32_t removeFilter(void *self, uint64_t value) {
    assert(self == manager && managerLocked && !hciLocked);
    commands.push_back(0x2012); ++removeCalls;
    if (!removeStatus && !removeMissingCompletion) completed.erase(value);
    if (invalidateRemove) intelPrivacyInvalidate();
    if (sleepRemove) intelPrivacySetEnabled(0);
    if (expireRemove) ticks += 5000000001ULL;
    return removeStatus;
}
uint32_t add(void *self, uint64_t value) {
    assert(self == manager && managerLocked && !hciLocked);
    commands.push_back(0x2011); ++addCalls;
    if (fatalCompletion) throw NativeFatal {};
    const uint32_t status = value == Identity ? restoreStatus : addStatus;
    if (!status && !(addMissingCompletion && value != Identity)) completed.insert(value);
    if (invalidateAdd) intelPrivacyInvalidate();
    if (sleepAdd) intelPrivacySetEnabled(0);
    if (expireAdd) ticks += 5000000001ULL;
    if (changeGateAdd) supported = false;
    return status;
}
template <typename T> int64_t coreRelative(T p) {
    return reinterpret_cast<intptr_t>(p) - reinterpret_cast<intptr_t>(&intelPrivacyConfig);
}
template <typename T> int32_t retryRelative(T p) {
    const intptr_t relative = reinterpret_cast<intptr_t>(p) - reinterpret_cast<intptr_t>(&intelPrivacyRetryConfig);
    assert(relative >= INT32_MIN && relative <= INT32_MAX);
    return static_cast<int32_t>(relative);
}
void reset() {
    std::free(intelPrivacyRuntime.state); intelPrivacyRuntime = {};
    std::memset(manager, 0, sizeof(manager));
    std::memset(pendingStorage, 0, sizeof(pendingStorage));
    std::memset(installedStorage, 0, sizeof(installedStorage));
    auto pn = new (pendingStorage) NativeSetNode {};
    auto in = new (installedStorage) NativeSetNode {};
    new (manager + 0x150) NativeAddressSet {pn,pn,1};
    new (manager + 0x168) NativeAddressSet {in,in,1};
    pn->parent = &pending().root; in->parent = &installed().root;
    for (unsigned i = 0; i != 16; ++i) uuidBytes[i] = static_cast<uint8_t>(i + 1);
    std::memcpy(pendingStorage + 0x19, uuidBytes, 16);
    std::memcpy(installedStorage + 0x28, uuidBytes, 16);
    in->value = Identity; manager[0xb8] = 1;
    managerLocked = true; hciLocked = false; supported = true;
    multiple = noUUID = removeMissingCompletion = addMissingCompletion = false;
    invalidateRemove = invalidateAdd = sleepRemove = sleepAdd = false;
    expireRemove = expireAdd = changeGateAdd = fatalCompletion = false;
    ticks = 100000000000ULL;
    identityValue = resolverIdentity = Identity; resolverStatus = 0; resolverFound = 1;
    removeStatus = addStatus = restoreStatus = addCalls = removeCalls = claims = releases = 0;
    completed = {Identity}; commands.clear();
    std::memset(report, 0, sizeof(report)); report[0x1a] = 0x13;
    intelPrivacySetEnabled(1);
}
void observe(uint64_t value = RPA, uint8_t event = 0x13) {
    TypedAddress raw = address(value), out {}; uint8_t found = 0;
    std::memcpy(report + 1, &raw, sizeof(raw)); report[0x1a] = event;
    assert(intelPrivacyResolve(reinterpret_cast<TypedAddress *>(report + 1), &out, &found) == resolverStatus);
}
uint32_t retry() {
    assert(managerLocked && !hciLocked);
    uint32_t result = intelPrivacyRefreshRetry(manager);
    assert(!hciLocked);
    assert(claims == releases || (noUUID && claims == releases + 1));
    return result;
}
void unchanged() {
    assert(retry() == 1 && commands.empty() && installed().root->value == Identity);
}
} // namespace

extern "C" const Config intelPrivacyConfig = {
    coreRelative(resolver), coreRelative(builder), coreRelative(clockNow),
    coreRelative(allocate), coreRelative(release), coreRelative(gate), coreRelative(timebase),0,5000000
};
extern "C" const RetryConfig intelPrivacyRetryConfig = {
    retryRelative(&database), retryRelative(makeUUID), retryRelative(claim),
    retryRelative(&releaseObjectPointer), retryRelative(multipleAddresses), retryRelative(identity),
    retryRelative(add), retryRelative(removeFilter), retryRelative(contains), retryRelative(lockHCI), retryRelative(unlockHCI)
};

int main() {
    unsigned cases = 0;
    reset(); observe(); ticks += 15251329000ULL; unchanged();
    // Actual capture order: public was admitted with an old observation; a
    // fresh connectable report arrives after pending, before the 6 s cancel.
    ticks += 1161472000ULL; observe(); ticks += 4840528000ULL;
    assert(retry() == 1 && installed().root->value == RPA);
    assert((commands == std::vector<unsigned>{0x2012,0x2011}));
    assert(completed == std::set<uint64_t>{RPA});
    assert(std::memcmp(installedStorage + 0x28, uuidBytes, 16) == 0);
    assert(installed().begin == installed().root && installed().root->parent == &installed().root);
    // Later native cleanup uses the exact committed address.
    removeFilter(manager, installed().root->value); assert(completed.empty()); ++cases;

    reset(); observe(); ticks += 5000000000ULL;
    assert(retry() == 1 && installed().root->value == RPA); ++cases;
    reset(); observe(); ticks += 5000000001ULL; unchanged(); ++cases;
    reset(); observe(); ticks = 1; unchanged(); ++cases;

    for (uint8_t event : {0x10,0x12,0x1b}) {
        reset(); observe(RPA,event); unchanged(); ++cases;
    }
    for (unsigned offset : {0x360U,0x3a0U,0x429U,0x42aU,0x42bU}) {
        reset(); observe(); manager[offset] = 1; unchanged(); ++cases;
    }
    reset(); observe(); manager[0xb8] = 0; unchanged(); ++cases;
    reset(); observe(); pending().size = 2; unchanged(); ++cases;
    reset(); observe(); pending().size = 0; unchanged(); ++cases;
    reset(); observe(); installed().size = 2; unchanged(); ++cases;
    reset(); observe(); installed().root->left = installed().root; unchanged(); ++cases;
    reset(); observe(); pendingStorage[0x19] ^= 1; unchanged(); ++cases;
    reset(); observe(); multiple = true; unchanged(); ++cases;
    reset(); observe(); noUUID = true; unchanged(); ++cases;
    reset(); observe(); supported = false; unchanged(); ++cases;
    reset(); observe(); intelPrivacySetEnabled(0); unchanged(); ++cases;
    reset(); observe(); intelPrivacyInvalidate(); unchanged(); ++cases;
    reset(); observe(); intelPrivacyRuntime.state->lock = 1; unchanged();
    intelPrivacyRuntime.state->lock = 0; ++cases;
    reset(); observe(); identityValue ^= 1; unchanged(); ++cases;
    reset(); observe(); completed.clear(); unchanged(); ++cases;
    reset(); observe(); completed.insert(RPA); unchanged(); ++cases;

    reset(); installed().root->value = RPA; completed = {RPA}; observe(NextRPA);
    assert(retry() == 1 && installed().root->value == NextRPA && completed == std::set<uint64_t>{NextRPA});
    assert((commands == std::vector<unsigned>{0x2012,0x2011})); ++cases;
    for (unsigned fault = 0; fault != 3; ++fault) {
        reset(); installed().root->value = RPA; completed = {RPA}; observe(NextRPA);
        if (fault == 0) resolverIdentity ^= 1;
        if (fault == 1) resolverFound = 0;
        if (fault == 2) resolverStatus = 2;
        assert(retry() == 1 && commands.empty() && installed().root->value == RPA); ++cases;
    }
    reset(); observe(); installed().root->value = Identity ^ 1;
    assert(retry() == 1 && commands.empty() && installed().root->value == (Identity ^ 1)); ++cases;
    reset(); observe(); installed().root->value = RPA; completed = {RPA};
    assert(retry() == 1 && commands.empty()); ++cases;

    reset(); observe(); removeStatus = 0x67;
    assert(retry() == 0 && addCalls == 0 && installed().root->value == Identity);
    assert(completed == std::set<uint64_t>{Identity}); ++cases;
    reset(); observe(); removeMissingCompletion = true;
    assert(retry() == 0 && addCalls == 0 && installed().root->value == Identity); ++cases;
    reset(); observe(); addStatus = 0x68;
    assert(retry() == 0 && addCalls == 2 && installed().root->value == Identity);
    assert(completed == std::set<uint64_t>{Identity}); ++cases;
    reset(); observe(); addStatus = restoreStatus = 0x68;
    assert(retry() == 0 && addCalls == 2 && installed().root->value == Identity && completed.empty()); ++cases;
    reset(); observe(); addMissingCompletion = true;
    assert(retry() == 0 && installed().root->value == Identity && completed == std::set<uint64_t>{Identity}); ++cases;
    reset(); observe(); fatalCompletion = true;
    bool propagated = false;
    try { retry(); } catch (const NativeFatal &) { propagated = true; }
    assert(propagated && installed().root->value == Identity && addCalls == 1); ++cases;

    reset(); observe(); invalidateRemove = true;
    assert(retry() == 0 && addCalls == 0 && installed().root->value == Identity && completed.empty()); ++cases;
    reset(); observe(); sleepRemove = true;
    assert(retry() == 0 && addCalls == 0 && !(intelPrivacyRuntime.control & 1)); ++cases;
    reset(); observe(); expireRemove = true;
    assert(retry() == 0 && addCalls == 1 && installed().root->value == Identity);
    assert(completed == std::set<uint64_t>{Identity}); ++cases;
    for (unsigned fault = 0; fault != 4; ++fault) {
        reset(); observe();
        invalidateAdd = fault == 0; sleepAdd = fault == 1;
        expireAdd = fault == 2; changeGateAdd = fault == 3;
        assert(retry() == 0 && addCalls == 1 && installed().root->value == RPA);
        assert(completed == std::set<uint64_t>{RPA});
        removeFilter(manager, installed().root->value); assert(completed.empty()); ++cases;
    }
    // An old-generation native completion cannot leave a restart or revive the
    // old observation after wake; ownership was retained solely for cleanup.
    reset(); observe(); sleepAdd = true; assert(retry() == 0);
    intelPrivacySetEnabled(1); sleepAdd = false; commands.clear();
    assert(retry() == 1 && commands.empty()); ++cases;

    intelPrivacySetEnabled(0); std::free(intelPrivacyRuntime.state); intelPrivacyRuntime.state = nullptr;
    std::printf("IntelPrivacy: %u retry lifecycle cases passed\n", cases);
}
