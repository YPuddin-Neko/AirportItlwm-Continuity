#include "IntelPrivacy.hpp"

using namespace IntelPrivacy;

namespace {
template <typename T> T native(int64_t offset) {
    return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(&intelPrivacyRetryConfig) + offset);
}
template <typename T> T core(int64_t offset) {
    return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(&intelPrivacyConfig) + offset);
}
template <typename T> T &field(void *object, size_t offset) {
    return *reinterpret_cast<T *>(static_cast<uint8_t *>(object) + offset);
}
bool active(uint64_t generation) {
    return (generation & 1) && generation ==
        __atomic_load_n(&intelPrivacyRuntime.control, __ATOMIC_ACQUIRE);
}
bool fresh(uint64_t generation, uint64_t observed) {
    const uint64_t now = core<Clock>(intelPrivacyConfig.continuousTime)();
    return active(generation) && now >= observed && now - observed <=
        __atomic_load_n(&intelPrivacyRuntime.maxAgeTicks, __ATOMIC_RELAXED);
}
bool single(const NativeAddressSet &tree) {
    return tree.size == 1 && tree.root && tree.begin == tree.root &&
        !tree.root->left && !tree.root->right;
}
TypedAddress unpack(uint64_t value) {
    TypedAddress out {};
    for (int i = 5; i >= 0; --i) {
        out.bytes[i] = static_cast<uint8_t>(value);
        value >>= 8;
    }
    out.type = static_cast<uint8_t>(value);
    return out;
}
bool contains(uint64_t value) {
    const TypedAddress address = unpack(value);
    native<void (*)()>(intelPrivacyRetryConfig.lockHCI)();
    const bool present = native<bool (*)(const TypedAddress *)>(
        intelPrivacyRetryConfig.contains)(&address);
    native<void (*)()>(intelPrivacyRetryConfig.unlockHCI)();
    return present;
}
using Filter = uint32_t (*)(void *, uint64_t);
}

// Called only by the post-cancel terminal branch at 0x100650a88. Its caller
// owns the manager lock until after this wrapper and the original restart.
extern "C" uint32_t intelPrivacyRefreshRetry(void *manager) {
    const uint64_t generation =
        __atomic_load_n(&intelPrivacyRuntime.control, __ATOMIC_ACQUIRE);
    auto &pending = field<NativeAddressSet>(manager, 0x150);
    auto &installed = field<NativeAddressSet>(manager, 0x168);
    if (!active(generation) || !single(pending) || !single(installed) ||
        field<uint32_t>(manager, 0x360) || field<uint32_t>(manager, 0x3a0) ||
        field<uint8_t>(manager, 0xb8) != 1 || field<uint8_t>(manager, 0x429) ||
        field<uint8_t>(manager, 0x42a) || field<uint8_t>(manager, 0x42b) ||
        !core<ControllerGate>(intelPrivacyConfig.controllerGate)())
        return 1;

    const auto uuidBytes = reinterpret_cast<const uint8_t *>(pending.root) + 0x19;
    const auto installedUUID = reinterpret_cast<const uint8_t *>(installed.root) + 0x28;
    for (unsigned i = 0; i != 16; ++i)
        if (uuidBytes[i] != installedUUID[i])
            return 1;

    void *uuid = native<void *(*)(const uint8_t *)>(intelPrivacyRetryConfig.uuid)(uuidBytes);
    uuid = native<void *(*)(void *)>(intelPrivacyRetryConfig.claimObject)(uuid);
    if (!uuid)
        return 1;
    void *database = *native<void **>(intelPrivacyRetryConfig.database);
    const bool multiple = native<bool (*)(void *, void *)>(
        intelPrivacyRetryConfig.multipleAddresses)(database, uuid);
    const uint64_t identity = multiple ? 0 : native<uint64_t (*)(void *, void *, bool)>(
        intelPrivacyRetryConfig.identity)(database, uuid, false);
    (*native<void (**)(void *)>(intelPrivacyRetryConfig.releaseObjectPointer))(uuid);
    if (!identity)
        return 1;

    const uint64_t old = installed.root->value;
    uint64_t observed = 0;
    const uint64_t selected = intelPrivacySelectAddress(identity, generation, &observed);
    if (!selected || selected == old)
        return 1;
    if (old != identity) {
        const TypedAddress address = unpack(old);
        if (address.type != 1 || (address.bytes[0] & 0xc0) != 0x40)
            return 1;
        TypedAddress resolved {};
        uint8_t found = 0;
        native<void (*)()>(intelPrivacyRetryConfig.lockHCI)();
        const uint32_t status = core<Resolver>(intelPrivacyConfig.resolver)(
            &address, &resolved, &found);
        native<void (*)()>(intelPrivacyRetryConfig.unlockHCI)();
        if (status || found != 1 || intelPrivacyPackAddress(&resolved) != identity)
            return 1;
    }
    if (!contains(old) || contains(selected) || !fresh(generation, observed))
        return 1;

    const uint32_t removed = native<Filter>(intelPrivacyRetryConfig.remove)(manager, old);
    if (removed || contains(old) || !active(generation))
        return 0;
    if (fresh(generation, observed)) {
        const uint32_t added = native<Filter>(intelPrivacyRetryConfig.add)(manager, selected);
        if (!added && contains(selected)) {
            // The manager lock preserves this sole node across native waits.
            // Commit actual controller ownership even if sleep invalidated the
            // selection meanwhile; native cleanup must remove this exact RPA.
            installed.root->value = selected;
            return fresh(generation, observed) &&
                core<ControllerGate>(intelPrivacyConfig.controllerGate)();
        }
    }
    if (active(generation))
        native<Filter>(intelPrivacyRetryConfig.add)(manager, old);
    // Native client timeout/cleanup still owns the request; a failed refresh
    // does not initiate another connection or extend its lifetime.
    return 0;
}
