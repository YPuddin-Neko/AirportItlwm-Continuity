// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_NATIVE_FRAME_HPP
#define AIRPORT_AWDL_NATIVE_FRAME_HPP
#include "SyncEngine.hpp"

namespace AirportAWDL {
// apple80211_act_frm_hdr as consumed by 24H32 bpfTapInput(type 12).
// Remaining unavailable metadata stays zero, as in the native proximity RX path.
struct NativeActionMetadata {
    uint8_t bytes[72];

    bool fill(const uint8_t *frame, size_t size, uint16_t channel, int16_t rssi,
              uint32_t bandFlags, uint64_t receivedUS)
    {
        Advertisement advertisement;
        if (!frame || parseMPDU(frame, size, advertisement) != ParseResult::Valid ||
            !unicastAddress(frame + 10) || !channel) return false;
        *this = {};
        copyAddress(bytes, frame + 10);
        putLE32(bytes + 12, 1); // apple80211_channel.version
        putLE32(bytes + 16, channel);
        putLE32(bytes + 20, bandFlags);
        putLE32(bytes + 44, uint32_t(int32_t(rssi)));
        // The peer manager swaps this word on input, then swaps it back in
        // ADVERTISERS. Use the low 32 bits of the calibrated local RX clock.
        for (unsigned i = 0; i < 4; ++i) bytes[48 + i] = uint32_t(receivedUS) >> (24 - 8 * i);
        return true;
    }
};
static_assert(sizeof(NativeActionMetadata) == 72, "Sequoia action metadata size");
}
#endif
