#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

// Appended to the native-control fixture with the complete production handle().
// stdout is the production MPDU; Python and the external decoder inspect it.
int main(int argc, char **argv)
{
    assert(argc == 3 || argc == 4);
    const std::string mode = argv[1];
    const uint64_t now = argc == 4 ? std::stoull(argv[3]) : 1005000;
    std::ifstream input(argv[2], std::ios::binary);
    std::vector<uint8_t> payload((std::istreambuf_iterator<char>(input)), {});
    AirportItlwmAWDLInterface i;
    i.sync.start(i.address.octet, 44, 1000000);
    const bool oob = mode == "native-mif" || mode == "umi";
    const bool fallback = mode == "fallback-psf" || mode == "fallback-mif";
    if (!fallback) {
        apple80211_awdl_sync_frame_template request = {
            1, uint32_t(payload.size()), payload.data()
        };
        assert(i.handle(Request::setAWDL_SYNC_FRAME_TEMPLATE, &request) == kIOReturnSuccess);
    }
    // Update after the template was installed: a supplied TLV18 is now stale.
    const uint8_t schedule[16] = {44,0,0,149,0,44,0,149,6,0,44,149,0,44,0,149};
    for (unsigned n = 0; n < 16; ++n) i.sync.channels[n] = schedule[n];
    if (argc == 4) for (auto &channel : i.sync.channels) channel = 44;
    const bool mif = mode != "psf" && mode != "fallback-psf";
    const uint8_t peer[6] = {2,0,0,0,0,2};
    const uint8_t *extra = fallback ? nullptr : (oob ? payload.data() : i.frameTemplate);
    const size_t extraSize = fallback ? 0 : (oob ? payload.size() : i.templateSize);
    std::vector<uint8_t> packet(1538, 0xa5);
    const auto length = i.sync.action(packet.data() + 1, 1536, now, mif,
        extra, extraSize, mode == "umi" ? peer : nullptr, mode == "native-mif");
    assert(length && packet.front() == 0xa5 && packet[1 + length] == 0xa5);
    // A failed size check must not consume a sequence number or modify output.
    for (size_t cap = 0; cap < length; ++cap) {
        std::vector<uint8_t> small(1538, 0xa5);
        const auto sequence = i.sync.wifiSequence;
        assert(!i.sync.action(small.data() + 1, cap, now, mif, extra, extraSize,
            mode == "umi" ? peer : nullptr, mode == "native-mif"));
        for (auto byte : small) assert(byte == 0xa5);
        assert(i.sync.wifiSequence == sequence);
    }
    assert(fwrite(packet.data() + 1, 1, length, stdout) == length);
}
