#pragma once
#include <stdint.h>

namespace IntelPrivacy {
struct ControllerConfig {
    int64_t tryLock;
    int64_t unlock;
    int64_t lock;
    int64_t active;
    int64_t devices;
    int64_t count;
};
}

extern "C" {
extern __attribute__((visibility("hidden"))) const IntelPrivacy::ControllerConfig intelPrivacyControllerConfig;
uint32_t intelPrivacyActiveController();
}
