#include <cassert>
#include <cstring>
#include <initializer_list>
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"
using namespace AirportWCL;
int main() {
    auto t = scanTiming(110, true, true);
    assert(t.maxOutTU == 120 && t.suspendTU == 120 && !t.fragmented);
    for (auto ms : {0U, 103U, 1000U, UINT32_MAX}) {
        t = scanTiming(ms, true, true);
        assert(t.maxOutTU == 120 && t.suspendTU == 120 && !t.fragmented);
    }
    for (auto ms : {1U, 20U, 102U}) {
        t = scanTiming(ms, true, true);
        assert(t.maxOutTU >= 44 && t.maxOutTU < 100 && t.suspendTU == 95 && t.fragmented);
        t = scanTiming(ms, true, false);
        assert(t.maxOutTU == 120 && !t.fragmented);
    }
    t = scanTiming(110, false, true);
    assert(!t.maxOutTU && !t.suspendTU && !t.fragmented);

    uint8_t wire[0x1550] = {};
    wire[0x40] = 1;
    ScanRequest s;
    assert(decodeScan(wire, sizeof(wire), s));
    const uint8_t bssid[] = {2, 3, 4, 5, 6, 7}, ssid[] = {'a', 0, 'b'};
    assert(matchesScan(s, bssid, ssid, 3, 36));
    memset(s.bssid, 255, 6);
    assert(matchesScan(s, bssid, ssid, 3, 36));
    memcpy(s.bssid, bssid, 6);
    s.ssidLength = 3; memcpy(s.ssid, ssid, 3);
    s.channelCount = 1; s.channels[36 / 8] |= 1 << (36 % 8);
    assert(matchesScan(s, bssid, ssid, 3, 36));
    assert(!matchesScan(s, bssid, ssid, 3, 40));
    assert(!matchesScan(s, bssid, ssid, 3, 256));
    assert(!matchesScan(s, bssid, ssid, 1, 36));
    s.ssid[2] = 'c';
    assert(!matchesScan(s, bssid, ssid, 3, 36));
    s.ssid[2] = 'b'; s.bssid[5] ^= 1;
    assert(!matchesScan(s, bssid, ssid, 3, 36));
    wire[0x1c] = 33;
    assert(!decodeScan(wire, sizeof(wire), s));
    wire[0x1c] = 0; wire[0x54] = 1;
    assert(!decodeScan(wire, 0x58, s));
    wire[0x5c] = 36; wire[0x60] = 0x10;
    assert(decodeScan(wire, sizeof(wire), s));
    assert(matchesScan(s, bssid, ssid, 3, 36));
    assert(!matchesScan(s, bssid, ssid, 3, 1));
    wire[0x61] = 0x20; // 6 GHz channel 36 is not 5 GHz channel 36.
    assert(decodeScan(wire, sizeof(wire), s));
    assert(!matchesScan(s, bssid, ssid, 3, 36));
    assert(joinChannelHint(0x1001) == 1 && joinChannelHint(0x100e) == 14);
    assert(joinChannelHint(0xd024) == 36 && joinChannelHint(0xd095) == 149);
    for (auto spec : {0U, 0xd001U, 0x1024U, 0xd0ffU, 0xe02aU, 0x5001U})
        assert(joinChannelHint(spec) == 0);
    assert(scanChannelSelected(s.channels, false, 36, 36));
    assert(!scanChannelSelected(s.channels, false, 40, 36));
    assert(scanChannelSelected(s.channels, false, 40, 0));

    memset(wire, 0, sizeof(wire));
    wire[0x40] = 1;
    wire[0x1318] = 3;
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t *entry = wire + 0x131c + i * 56;
        entry[0] = 1; entry[4] = 11;
        memcpy(entry + 8, i ? "fixture-two" : "fixture-one", 11);
    }
    assert(decodeScan(wire, sizeof(wire), s) && s.ssidCount == 3);
    assert(s.ssidLengths[0] == 11 && s.ssidLengths[1] == 11 && !s.ssidLengths[2]);
    struct Entry { uint8_t id, len, ssid[32]; } entries[20] = {};
    fillScanSSIDs(entries, s.ssidCount, s.ssidLengths, s.ssids);
    assert(entries[0].len == 11 && !memcmp(entries[0].ssid, "fixture-one", 11));
    assert(entries[1].len == 11 && !memcmp(entries[1].ssid, "fixture-two", 11));
    assert(entries[2].id == 0 && entries[2].len == 0);
    assert(matchesScan(s, bssid, ssid, 3, 36));
    s.ssidCount = 2;
    assert(!matchesScan(s, bssid, ssid, 3, 36));
    assert(matchesScan(s, bssid, reinterpret_cast<const uint8_t *>("fixture-two"), 11, 36));
    wire[0x1318] = 11;
    assert(!decodeScan(wire, sizeof(wire), s));
    wire[0x1318] = 3;
    assert(!decodeScan(wire, 0x131c + 2 * 56, s));
    wire[0x1320] = 33;
    assert(!decodeScan(wire, sizeof(wire), s));
}
