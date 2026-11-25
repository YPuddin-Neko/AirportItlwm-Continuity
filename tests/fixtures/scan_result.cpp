#include <cassert>
#include <cstring>
#include <cstdint>
#include <algorithm>
#define MIN(a,b) std::min<size_t>(a,b)
#define APPLE80211_VERSION 1
#define IEEE80211_ADDR_LEN 6
#define IWM_MIN_DBM -100
struct ieee80211_channel { int channel, ic_flags; };
struct ieee80211_node {
    uint8_t *ni_rsnie_tlv; size_t ni_rsnie_tlv_len;
    unsigned ni_intval, ni_age_ts, ni_capinfo;
    struct { uint8_t rs_nrates, rs_rates[16]; } ni_rates;
    ieee80211_channel *ni_chan;
    int ni_rssi; uint8_t ni_bssid[6], ni_esslen, ni_essid[32];
};
struct apple80211_scan_result {
    unsigned version, asr_ie_len; uint8_t asr_ie_data[8];
    unsigned asr_beacon_int, asr_nrates; uint8_t asr_rates[16];
    unsigned asr_age, asr_cap;
    struct { int version, channel, flags; } asr_channel;
    int asr_noise, asr_rssi; uint8_t asr_bssid[6], asr_ssid_len, asr_ssid[32];
};
struct Info { int getBSSNoise() { return 95; } };
struct ItlHalService {
    Info info; void *get80211Controller() { return nullptr; }
    Info *getDriverInfo() { return &info; }
};
static unsigned airport_up_time() { return 200; }
static int ieee80211_chan2ieee(void *, ieee80211_channel *c) { return c->channel; }
static int ieeeChanFlag2appleScanFlagVentura(int f) { return f; }
// PRODUCTION: convert
int main() {
    ItlHalService hal; ieee80211_channel channel = {36, 16};
    uint8_t ies[16]; memset(ies, 0xa5, sizeof(ies));
    ieee80211_node node = {}; node.ni_chan = &channel;
    node.ni_rsnie_tlv = ies; node.ni_rsnie_tlv_len = sizeof(ies);
    node.ni_rates.rs_nrates = 3;
    memcpy(node.ni_rates.rs_rates, "abc", 3);
    node.ni_age_ts = 123;
    node.ni_esslen = 3; memcpy(node.ni_essid, "a\0b", 3);
    apple80211_scan_result result;
    convertNodeToScanResult(&hal, &node, &result);
    assert(result.asr_ie_len == sizeof(result.asr_ie_data));
    assert(!memcmp(result.asr_ie_data, ies, result.asr_ie_len));
    assert(result.asr_nrates == 3 && !memcmp(result.asr_rates, "abc", 3));
    assert(result.asr_age == 77 && !memcmp(result.asr_ssid, "a\0b", 3));
    node.ni_rsnie_tlv = nullptr; node.ni_rsnie_tlv_len = 0;
    node.ni_rates.rs_nrates = 0;
    convertNodeToScanResult(&hal, &node, &result);
    assert(result.asr_ie_len == 0 && result.asr_nrates == 0 && !result.asr_rates[0]);
}
