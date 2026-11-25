#include "../../bluetooth/BlueToolFixup/IntelPrivacy.hpp"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace IntelPrivacy;

namespace {
uint8_t report[80];
TypedAddress resolved {0, {0x4c, 0x2e, 0xb4, 0x6c, 0x76, 0xae}};
uint64_t ticks;
uint32_t resolverStatus;
uint8_t resolverFound;
unsigned resolverCalls;
unsigned builderCalls;
unsigned allocations;
bool allocationFails;
bool intelController;
bool invalidateInResolver;
bool invalidateInClock;
bool invalidateInAllocator;
uint32_t timebaseNumerator;
uint32_t timebaseDenominator;
int timebaseError;
uint64_t nativeValue;
uint64_t nativeCount;
NativeSetNode node;

uint32_t resolver(const TypedAddress *, TypedAddress *identity, uint8_t *found) {
    ++resolverCalls;
    if (invalidateInResolver)
        intelPrivacyInvalidate();
    if (identity)
        *identity = resolved;
    if (found)
        *found = resolverFound;
    return resolverStatus;
}

void *builder(NativeAddressSet *result, void *database, void *uuid) {
    assert(database == reinterpret_cast<void *>(0x1234));
    assert(uuid == reinterpret_cast<void *>(0x5678));
    ++builderCalls;
    node = {};
    node.parent = &result->root;
    node.black = 1;
    node.value = nativeValue;
    result->root = nativeCount ? &node : nullptr;
    result->begin = nativeCount ? &node : reinterpret_cast<NativeSetNode *>(&result->root);
    result->size = nativeCount;
    return result;
}

uint64_t clockNow() {
    if (invalidateInClock)
        intelPrivacyInvalidate();
    return ticks;
}

void *allocate(size_t count, size_t size, uint64_t type) {
    assert(count == 1 && size == sizeof(State));
    assert(type == 0x1122334455667788ULL);
    ++allocations;
    if (invalidateInAllocator)
        intelPrivacySetEnabled(0);
    return allocationFails ? nullptr : calloc(count, size);
}

void release(void *memory) { free(memory); }
uint32_t gate() { return intelController; }
int timebase(TimebaseInfo *info) {
    info->numerator = timebaseNumerator;
    info->denominator = timebaseDenominator;
    return timebaseError;
}

template <typename Function> int64_t relative(Function function) {
    return reinterpret_cast<intptr_t>(function) -
           reinterpret_cast<intptr_t>(&intelPrivacyConfig);
}

Config makeConfig() {
    return {relative(resolver), relative(builder), relative(clockNow),
            relative(allocate), relative(release), relative(gate), relative(timebase),
            0x1122334455667788ULL, 5000000};
}

void reset() {
    free(intelPrivacyRuntime.state);
    intelPrivacyRuntime = {};
    memset(report, 0, sizeof(report));
    const TypedAddress raw {1, {0x68, 0x73, 0xe4, 0xce, 0x47, 0x8a}};
    memcpy(report + 1, &raw, sizeof(raw));
    report[0x1a] = 0x13;
    resolved = {0, {0x4c, 0x2e, 0xb4, 0x6c, 0x76, 0xae}};
    ticks = 10000000000ULL;
    resolverStatus = 0;
    resolverFound = 1;
    resolverCalls = builderCalls = allocations = 0;
    allocationFails = invalidateInResolver = invalidateInClock = invalidateInAllocator = false;
    intelController = true;
    timebaseNumerator = timebaseDenominator = 1;
    timebaseError = 0;
    nativeValue = 0x004c2eb46c76aeULL;
    nativeCount = 1;
    intelPrivacySetEnabled(1);
}

void observe() {
    TypedAddress output {};
    uint8_t found = 0;
    assert(intelPrivacyResolve(reinterpret_cast<TypedAddress *>(report + 1),
                               &output, &found) == resolverStatus);
    assert(found == resolverFound);
    assert(memcmp(&output, &resolved, sizeof(output)) == 0);
}

uint64_t admit() {
    NativeAddressSet result {};
    assert(intelPrivacyBuildFilterSet(&result, reinterpret_cast<void *>(0x1234),
                                     reinterpret_cast<void *>(0x5678)) == &result);
    assert(result.size == nativeCount);
    return result.root ? result.root->value : 0;
}
} // namespace

extern "C" const Config intelPrivacyConfig = makeConfig();

int main() {
    unsigned cases = 0;

    reset();
    observe();
    assert(admit() == 0x016873e4ce478aULL);
    assert(resolverCalls == 1 && builderCalls == 1 && allocations == 1);
    assert(node.left == nullptr && node.right == nullptr && node.black == 1);
    ++cases;

    const uint8_t ignoredEvents[] {0x10, 0x12, 0x1b};
    for (uint8_t event : ignoredEvents) {
        reset(); observe();
        report[7] = 0x99;
        report[0x1a] = event;
        ticks += 100;
        observe();
        assert(admit() == 0x016873e4ce478aULL);
        ticks += 5000000000ULL;
        assert(admit() == nativeValue);
        ++cases;
    }

    reset(); observe();
    ticks += 100;
    report[7] = 0x99;
    observe();
    assert(admit() == 0x016873e4ce4799ULL);
    ++cases;

    reset(); resolverStatus = 2; observe();
    assert(admit() == nativeValue);
    resolverStatus = 0; resolverFound = 0; observe();
    assert(admit() == nativeValue);
    ++cases;

    reset(); report[1] = 0; observe();
    assert(admit() == nativeValue);
    report[1] = 1; report[2] = 0xc8; observe();
    assert(admit() == nativeValue);
    report[2] = 0x08; observe();
    assert(admit() == nativeValue);
    ++cases;

    reset(); resolved.type = 1; resolved.bytes[0] = 0x4c; observe();
    assert(admit() == nativeValue);
    resolved.bytes[0] = 0xcc; nativeValue = 0x01cc2eb46c76aeULL; observe();
    assert(admit() == 0x016873e4ce478aULL);
    ++cases;

    reset(); observe();
    nativeCount = 2; assert(admit() == nativeValue);
    nativeCount = 0; assert(admit() == 0);
    nativeCount = 1; nativeValue ^= 1; assert(admit() == nativeValue);
    ++cases;

    reset(); observe();
    ticks += 5000000000ULL; assert(admit() == 0x016873e4ce478aULL);
    ++ticks; assert(admit() == nativeValue);
    ticks = 1; assert(admit() == nativeValue);
    ++cases;

    reset(); observe(); intelPrivacyInvalidate();
    assert(admit() == nativeValue);
    observe(); assert(admit() == 0x016873e4ce478aULL);
    intelPrivacySetEnabled(0); observe(); assert(admit() == nativeValue);
    intelPrivacySetEnabled(1); assert(admit() == nativeValue);
    observe(); assert(admit() == 0x016873e4ce478aULL);
    assert(allocations == 1);
    ++cases;

    reset(); invalidateInResolver = true; observe();
    assert(admit() == nativeValue);
    invalidateInResolver = false; observe();
    invalidateInClock = true; assert(admit() == nativeValue);
    ++cases;

    reset(); observe();
    intelPrivacyRuntime.state->lock = 1;
    report[7] = 0x99; observe();
    assert(admit() == nativeValue);
    intelPrivacyRuntime.state->lock = 0;
    assert(admit() == 0x016873e4ce478aULL);
    ++cases;

    reset(); observe(); intelController = false;
    assert(admit() == nativeValue);
    intelPrivacySetEnabled(1);
    assert(intelPrivacyRuntime.control & 1);
    observe(); assert(admit() == nativeValue);
    ++cases;

    reset(); intelPrivacySetEnabled(0);
    free(intelPrivacyRuntime.state); intelPrivacyRuntime.state = nullptr;
    allocationFails = true; intelPrivacySetEnabled(1);
    assert(!(intelPrivacyRuntime.control & 1));
    allocationFails = false; invalidateInAllocator = true; intelPrivacySetEnabled(1);
    assert(!(intelPrivacyRuntime.control & 1));
    ++cases;

    reset(); timebaseNumerator = 125; timebaseDenominator = 3;
    intelPrivacySetEnabled(1);
    assert(intelPrivacyRuntime.maxAgeTicks == 120000000ULL);
    timebaseNumerator = 0; intelPrivacySetEnabled(1);
    assert(!(intelPrivacyRuntime.control & 1));
    ++cases;

    reset();
    for (unsigned i = 0; i <= Capacity; ++i) {
        resolved.bytes[5] = static_cast<uint8_t>(i + 1);
        ++ticks; observe();
    }
    nativeValue = 0x004c2eb46c7601ULL;
    assert(admit() == nativeValue);
    nativeValue = 0x004c2eb46c7611ULL;
    assert(admit() == 0x016873e4ce478aULL);
    ++cases;

    intelPrivacySetEnabled(0);
    free(intelPrivacyRuntime.state);
    intelPrivacyRuntime.state = nullptr;
    printf("IntelPrivacy: %u synthetic cases passed\n", cases);
}
