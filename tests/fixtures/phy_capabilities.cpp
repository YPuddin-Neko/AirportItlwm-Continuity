#include <cassert>
#include <cstdint>
#include <cstring>

#define __packed __attribute__((packed))
// DECLARATIONS

using IOReturn = int;
enum { kIOReturnSuccess, kIOReturnBadArgument, kIOReturnNotReady, kIOReturnUnsupported,
       APPLE80211_VERSION = 1 };
static void LE_WRITE_2(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void LE_WRITE_4(uint8_t *p, uint32_t v) {
    LE_WRITE_2(p, v); LE_WRITE_2(p + 2, v >> 16);
}
struct ieee80211com {
    unsigned ic_modecaps = 0;
    uint16_t ic_htcaps = 0, ic_max_rxrate = 0, ic_htxcaps = 0;
    uint8_t ic_ampdu_params = 0, ic_sup_mcs[10] = {}, ic_tx_mcs_set = 0, ic_aselcaps = 0;
    uint32_t ic_txbfcaps = 0, ic_vhtcaps = 0;
    uint16_t ic_vht_rx_mcs_map = 0, ic_vht_rx_highest = 0;
    uint16_t ic_vht_tx_mcs_map = 0, ic_vht_tx_highest = 0;
};
struct HAL {
    ieee80211com *ic = nullptr;
    ieee80211com *get80211Controller() { return ic; }
};
struct AirportItlwmSkywalkInterface {
    HAL *fHalService = nullptr;
    IOReturn getHT_CAPABILITY(apple80211_ht_capability *);
    IOReturn getVHT_CAPABILITY(apple80211_vht_capability *);
};

// PRODUCTION

template <class T> struct Guarded {
    uint64_t before = 0x1122334455667788;
    T value;
    uint64_t after = 0x8877665544332211;
    void check() const { assert(before == 0x1122334455667788 && after == 0x8877665544332211); }
};
int main() {
    AirportItlwmSkywalkInterface driver;
    Guarded<apple80211_ht_capability> ht;
    Guarded<apple80211_vht_capability> vht;
    assert(driver.getHT_CAPABILITY(nullptr) == kIOReturnBadArgument);
    assert(driver.getVHT_CAPABILITY(nullptr) == kIOReturnBadArgument);
    assert(driver.getHT_CAPABILITY(&ht.value) == kIOReturnNotReady);
    assert(driver.getVHT_CAPABILITY(&vht.value) == kIOReturnNotReady);
    HAL hal; driver.fHalService = &hal;
    assert(driver.getHT_CAPABILITY(&ht.value) == kIOReturnNotReady);
    ieee80211com ic; hal.ic = &ic;
    assert(driver.getHT_CAPABILITY(&ht.value) == kIOReturnUnsupported);
    assert(driver.getVHT_CAPABILITY(&vht.value) == kIOReturnUnsupported);

    ic.ic_modecaps = (1 << IEEE80211_MODE_11N) | (1 << IEEE80211_MODE_11AC);
    ic.ic_htcaps = 0x016e; ic.ic_ampdu_params = 0x1b;
    ic.ic_sup_mcs[0] = ic.ic_sup_mcs[1] = 0xff;
    ic.ic_max_rxrate = 0xffff; ic.ic_tx_mcs_set = 0x13;
    ic.ic_htxcaps = 0x03ef; ic.ic_txbfcaps = 0x12345678; ic.ic_aselcaps = 0xab;
    ic.ic_vhtcaps = 0x10203040;
    ic.ic_vht_rx_mcs_map = 0xfffa; ic.ic_vht_rx_highest = 866;
    ic.ic_vht_tx_mcs_map = 0xfffe; ic.ic_vht_tx_highest = 433;
    std::memset(&ht.value, 0xa5, sizeof(ht.value));
    std::memset(&vht.value, 0xa5, sizeof(vht.value));
    assert(driver.getHT_CAPABILITY(&ht.value) == kIOReturnSuccess);
    assert(driver.getVHT_CAPABILITY(&vht.value) == kIOReturnSuccess);
    const uint8_t expectedHT[] = {
        1,0,0,0, 45,26, 0x6e,1,0x1b, 0xff,0xff,0,0,0,0,0,0,0,0,
        0xff,3,0x13,0,0,0, 0xef,3, 0x78,0x56,0x34,0x12,0xab
    };
    const uint8_t expectedVHT[] = {
        1,0,0,0, 191,12, 0x40,0x30,0x20,0x10, 0xfa,0xff,0x62,3,
        0xfe,0xff,0xb1,1,0,0
    };
    static_assert(sizeof(expectedHT) == sizeof(ht.value), "HT ABI size");
    static_assert(sizeof(expectedVHT) == sizeof(vht.value), "VHT ABI size");
    assert(std::memcmp(&ht.value, expectedHT, sizeof(expectedHT)) == 0);
    assert(std::memcmp(&vht.value, expectedVHT, sizeof(expectedVHT)) == 0);
    ht.check(); vht.check();

    // An HT-only adapter must not retain another adapter's VHT stream map.
    ic.ic_modecaps = 1 << IEEE80211_MODE_11N;
    ic.ic_sup_mcs[1] = 0;
    assert(driver.getHT_CAPABILITY(&ht.value) == kIOReturnSuccess);
    assert(ht.value.hc_mcsset[0] == 0xff && ht.value.hc_mcsset[1] == 0);
    assert(driver.getVHT_CAPABILITY(&vht.value) == kIOReturnUnsupported);
    const auto bytes = reinterpret_cast<const uint8_t *>(&vht.value);
    for (unsigned i = 4; i < sizeof(vht.value); ++i) assert(bytes[i] == 0);
    ht.check(); vht.check();
}
