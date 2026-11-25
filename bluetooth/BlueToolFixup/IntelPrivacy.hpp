#pragma once

#include <stddef.h>
#include <stdint.h>

namespace IntelPrivacy {

struct TypedAddress {
    uint8_t type;
    uint8_t bytes[6];
};

struct NativeSetNode {
    NativeSetNode *left;
    NativeSetNode *right;
    void *parent;
    uint8_t black;
    uint8_t padding[7];
    uint64_t value;
};

struct NativeAddressSet {
    NativeSetNode *begin;
    NativeSetNode *root;
    uint64_t size;
};

using Resolver = uint32_t (*)(const TypedAddress *, TypedAddress *, uint8_t *);
using SetBuilder = void *(*)(NativeAddressSet *, void *, void *);
using Clock = uint64_t (*)();
using Allocate = void *(*)(size_t, size_t, uint64_t);
using Release = void (*)(void *);
using ControllerGate = uint32_t (*)();
struct TimebaseInfo { uint32_t numerator; uint32_t denominator; };
using Timebase = int (*)(TimebaseInfo *);

struct Config {
    int64_t resolver;
    int64_t buildSet;
    int64_t continuousTime;
    int64_t allocate;
    int64_t release;
    int64_t controllerGate;
    int64_t timebase;
    uint64_t allocatorType;
    uint64_t maxAgeUS;
};

constexpr size_t Capacity = 16;
struct Entry {
    uint64_t identity;
    uint64_t address;
    uint64_t ticks;
    uint64_t generation;
};

struct State {
    uint32_t lock;
    uint32_t padding;
    Entry entries[Capacity];
};

struct Runtime {
    uint64_t control;
    State *state;
    uint64_t maxAgeTicks;
};

struct RetryConfig {
    int32_t database;
    int32_t uuid;
    int32_t claimObject;
    int32_t releaseObjectPointer;
    int32_t multipleAddresses;
    int32_t identity;
    int32_t add;
    int32_t remove;
    int32_t contains;
    int32_t lockHCI;
    int32_t unlockHCI;
};

static_assert(sizeof(TypedAddress) == 7, "Native typed address ABI");
static_assert(sizeof(NativeAddressSet) == 24, "24H32 libc++ set ABI");
static_assert(offsetof(NativeSetNode, value) == 32, "24H32 libc++ node ABI");
static_assert(sizeof(Config) == 72, "Payload configuration ABI");
static_assert(sizeof(Runtime) == 24, "Payload zero-fill state ABI");
static_assert(sizeof(RetryConfig) == 44, "Payload relative retry configuration ABI");

} // namespace IntelPrivacy

extern "C" {
extern __attribute__((visibility("hidden"))) const IntelPrivacy::Config intelPrivacyConfig;
extern IntelPrivacy::Runtime intelPrivacyRuntime;
extern __attribute__((visibility("hidden"))) const IntelPrivacy::RetryConfig intelPrivacyRetryConfig;

uint32_t intelPrivacyResolve(const IntelPrivacy::TypedAddress *raw,
                            IntelPrivacy::TypedAddress *identity, uint8_t *found);
void *intelPrivacyBuildFilterSet(IntelPrivacy::NativeAddressSet *result,
                                void *database, void *uuid);
void intelPrivacyInvalidate();
void intelPrivacySetEnabled(uint32_t enabled);
uint64_t intelPrivacySelectAddress(uint64_t identity, uint64_t generation, uint64_t *observed);
uint64_t intelPrivacyPackAddress(const IntelPrivacy::TypedAddress *address);
uint32_t intelPrivacyRefreshRetry(void *manager);
}
