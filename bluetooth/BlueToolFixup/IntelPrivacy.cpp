#include "IntelPrivacy.hpp"

using namespace IntelPrivacy;

extern "C" {
Runtime intelPrivacyRuntime {};
}

namespace {

template <typename Function> Function native(int64_t offset) {
    return reinterpret_cast<Function>(reinterpret_cast<uintptr_t>(&intelPrivacyConfig) + offset);
}

uint64_t pack(const TypedAddress &address) {
    uint64_t result = address.type;
    for (unsigned i = 0; i != 6; ++i)
        result = (result << 8) | address.bytes[i];
    return result;
}

bool isRPA(const TypedAddress &address) {
    return address.type == 1 && (address.bytes[0] & 0xc0) == 0x40;
}

bool isIdentity(const TypedAddress &address) {
    return (address.type == 0 ||
            (address.type == 1 && (address.bytes[0] & 0xc0) == 0xc0)) &&
           (pack(address) & 0xffffffffffffULL) != 0;
}

bool lock(State *state) {
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&state->lock, &expected, 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void unlock(State *state) {
    __atomic_store_n(&state->lock, 0, __ATOMIC_RELEASE);
}

State *getState() {
    return __atomic_load_n(&intelPrivacyRuntime.state, __ATOMIC_ACQUIRE);
}

uint64_t control() {
    return __atomic_load_n(&intelPrivacyRuntime.control, __ATOMIC_ACQUIRE);
}

bool active(uint64_t generation) {
    return (generation & 1) && control() == generation;
}

void remember(uint64_t identity, uint64_t address, uint64_t ticks,
              uint64_t generation) {
    State *state = getState();
    if (!state || !lock(state))
        return;
    if (active(generation)) {
        Entry *selected = &state->entries[0];
        for (Entry &entry : state->entries) {
            if (entry.generation == generation && entry.identity == identity) {
                selected = &entry;
                break;
            }
            if (entry.generation != generation ||
                (selected->generation == generation && entry.ticks < selected->ticks))
                selected = &entry;
        }
        selected->identity = identity;
        selected->address = address;
        selected->ticks = ticks;
        selected->generation = generation;
    }
    unlock(state);
}

} // namespace

extern "C" uint64_t intelPrivacyPackAddress(const TypedAddress *address) {
    return pack(*address);
}

extern "C" uint32_t intelPrivacyResolve(const TypedAddress *raw,
                                        TypedAddress *identity, uint8_t *found) {
    const uint64_t generation = control();
    const uint32_t status = native<Resolver>(intelPrivacyConfig.resolver)(raw, identity, found);
    if (status || !raw || !identity || !found || *found != 1 ||
        !active(generation) || !isRPA(*raw) || !isIdentity(*identity) ||
        !native<ControllerGate>(intelPrivacyConfig.controllerGate)())
        return status;

    // This wrapper replaces only the common ADV resolver call at 0x1001f1b01.
    const uint8_t *report = reinterpret_cast<const uint8_t *>(raw) - 1;
    const uint8_t event = report[0x1a];
    if (!(event & 1) || (event & 8))
        return status;
    remember(pack(*identity), pack(*raw),
             native<Clock>(intelPrivacyConfig.continuousTime)(), generation);
    return status;
}

extern "C" void *intelPrivacyBuildFilterSet(NativeAddressSet *result,
                                            void *database, void *uuid) {
    void *returned = native<SetBuilder>(intelPrivacyConfig.buildSet)(result, database, uuid);
    const uint64_t generation = control();
    if (!active(generation) || result->size != 1 || !result->root ||
        result->begin != result->root || result->root->left || result->root->right ||
        !native<ControllerGate>(intelPrivacyConfig.controllerGate)())
        return returned;

    const uint64_t address = intelPrivacySelectAddress(result->root->value, generation, nullptr);
    if (address && active(generation))
        result->root->value = address;
    return returned;
}

extern "C" uint64_t intelPrivacySelectAddress(uint64_t identity, uint64_t generation,
                                             uint64_t *observed) {
    State *state = getState();
    if (!state || !lock(state))
        return 0;
    uint64_t address = 0;
    const uint64_t now = native<Clock>(intelPrivacyConfig.continuousTime)();
    const uint64_t maxAgeTicks =
        __atomic_load_n(&intelPrivacyRuntime.maxAgeTicks, __ATOMIC_RELAXED);
    for (const Entry &entry : state->entries) {
        if (entry.generation == generation && entry.identity == identity &&
            now >= entry.ticks && now - entry.ticks <= maxAgeTicks &&
            active(generation)) {
            address = entry.address;
            if (observed)
                *observed = entry.ticks;
            break;
        }
    }
    unlock(state);
    return address;
}

extern "C" void intelPrivacyInvalidate() {
    __atomic_add_fetch(&intelPrivacyRuntime.control, 2, __ATOMIC_ACQ_REL);
}

extern "C" void intelPrivacySetEnabled(uint32_t enabled) {
    uint64_t previous = control();
    uint64_t suspended;
    do {
        suspended = (previous + 2) & ~uint64_t(1);
    } while (!__atomic_compare_exchange_n(&intelPrivacyRuntime.control, &previous,
                                          suspended, false, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE));
    if (!enabled)
        return;
    TimebaseInfo info {};
    if (native<Timebase>(intelPrivacyConfig.timebase)(&info) ||
        !info.numerator || !info.denominator)
        return;
    const uint64_t scaled = intelPrivacyConfig.maxAgeUS * 1000 / info.numerator;
    if (!scaled || scaled > UINT64_MAX / info.denominator)
        return;
    const uint64_t maxAgeTicks = scaled * info.denominator;
    if (!getState()) {
        State *state = static_cast<State *>(native<Allocate>(intelPrivacyConfig.allocate)(
            1, sizeof(State), intelPrivacyConfig.allocatorType));
        if (!state)
            return;
        State *expected = nullptr;
        if (!__atomic_compare_exchange_n(&intelPrivacyRuntime.state, &expected, state, false,
                                         __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
            native<Release>(intelPrivacyConfig.release)(state);
    }
    __atomic_store_n(&intelPrivacyRuntime.maxAgeTicks, maxAgeTicks, __ATOMIC_RELAXED);
    __atomic_compare_exchange_n(&intelPrivacyRuntime.control, &suspended,
                                 suspended | 1, false, __ATOMIC_RELEASE,
                                 __ATOMIC_ACQUIRE);
}
