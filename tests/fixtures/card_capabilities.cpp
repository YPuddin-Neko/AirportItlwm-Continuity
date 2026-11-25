#include <cassert>
#include <cstdint>
#include <cstring>
#define AIRPORT_WCL 1
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#define OSDynamicCast(type, value) ((value)->iwx ? (value) : nullptr)
// CAPABILITY DECLARATIONS
using IOReturn = int;
constexpr int kIOReturnSuccess = 0, APPLE80211_VERSION = 1;
struct OSObject {};
struct apple80211_capability_data { uint32_t version; uint8_t capabilities[19]; };
struct Controller { uint32_t ic_caps = IEEE80211_C_RSN, ic_modecaps = 0; };
struct HAL {
    bool iwx = true;
    Controller controller;
    Controller *get80211Controller() { return &controller; }
};
struct AirportItlwm {
    HAL hal, *fHalService = &hal;
    IOReturn getCARD_CAPABILITIES(OSObject *, apple80211_capability_data *);
};
// PRODUCTION
int main() {
    AirportItlwm driver;
    for (unsigned modes = 0; modes < 16; ++modes) {
        driver.hal.controller.ic_modecaps = modes;
        apple80211_capability_data data;
        memset(&data, 0xff, sizeof(data));
        assert(driver.getCARD_CAPABILITIES(nullptr, &data) == 0 && data.version == 1);
        // 24H32 loads this struct at +0xb20, then tests byte +0xb27 bit 5.
        const auto bytes = reinterpret_cast<const uint8_t *>(&data);
        const bool dualBand = (modes & 2) && (modes & 12);
        assert(bool(bytes[7] & 0x20) == dualBand);
        assert(data.capabilities[3] == (dualBand ? 0x20 : 0));
        assert(data.capabilities[0] == 0x0a); // Preserve CCMP/TKIP, no AES-OCB.
        assert(!(data.capabilities[1] & 0x20)); // WPA1 remains disabled.
        assert(data.capabilities[17] == 0);
        assert(data.capabilities[7] == 0x80);
        assert(data.capabilities[9] == 0x02);
    }
    driver.hal.controller.ic_caps |= IEEE80211_C_HE;
    apple80211_capability_data data = {};
    driver.getCARD_CAPABILITIES(nullptr, &data);
    assert(data.capabilities[17] == 0x80);
    driver.hal.iwx = false;
    driver.getCARD_CAPABILITIES(nullptr, &data);
    assert(data.capabilities[7] == 0);
    assert(data.capabilities[9] == 0);
}
