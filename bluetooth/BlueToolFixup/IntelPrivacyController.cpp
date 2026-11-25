#include "IntelPrivacyController.hpp"

namespace {
template <typename T> T native(int64_t offset) {
    return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(&intelPrivacyControllerConfig) + offset);
}
}

extern "C" uint32_t intelPrivacyActiveController() {
    using Mutex = int (*)(void *);
    void *lock = native<void *>(intelPrivacyControllerConfig.lock);
    if (native<Mutex>(intelPrivacyControllerConfig.tryLock)(lock))
        return 0;

    const auto active = __atomic_load_n(
        native<const uintptr_t *>(intelPrivacyControllerConfig.active), __ATOMIC_RELAXED);
    const int32_t count = *native<const int32_t *>(intelPrivacyControllerConfig.count);
    const auto devices = native<const uintptr_t *>(intelPrivacyControllerConfig.devices);
    uint32_t supported = 0;
    if (active && count > 0 && count <= 10) {
        for (int32_t i = 0; i < count; ++i) {
            // Removal unlinks under this mutex before releasing the USB context.
            if (devices[i] != active)
                continue;
            const auto ids = reinterpret_cast<const uint16_t *>(active + 0x14);
            supported = ids[0] == 0x8087 && ids[1] == 0x0aaa;
            break;
        }
    }
    native<Mutex>(intelPrivacyControllerConfig.unlock)(lock);
    return supported;
}
