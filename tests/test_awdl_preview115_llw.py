"""Preview 115 (2.4.0d115) LLW / AWDL protocol & queue regressions (zero HAL/firmware delta vs d113):
1. FIX-P115-1: Unconditional boot-time clampWidePeerRates = true in serviceUnicast() and
   sendDueAnnouncements() (even when !llwParamsEnabled, before en1 IEEE80211_S_RUN, and when
   sync.has20MHz5GHzSlot() == false on [149x16] @ 80 MHz): omits outbound TLV 17 (0xbf VHT80)
   and clears 0x0042 (HT40 | SGI40) in TLV 7 from T+0 across PSF, auto MIF, immediate MIF, and
   unicast UMI so peers never latch IO80211Peer +0x1db = 1 during the 10-13 s boot window, while
   preserving 100% of Mac -> Peer VHT80/MIMO2 TX capabilities from inbound peer advertisements.
2. FIX-P115-2: SyncEngine::action() rebuilds 11-byte TLV 32 (ChanSeqExtTLV) sub-entries from
   non-zero sync.channels + sync.channelClasses (128 on class 128, operatingClass(c) on class 0)
   instead of stripping TLV 32, preserving header id/mapid/timeBitmapCtrl, emitting at most once
   even if extra has duplicate TLV 32s, and validating exact pass-1 rebuiltTlv32Size bounds under
   heap-exact ASan buffers (including Scan + Data where rebuilt TLV 32 exceeds input extra size).
3. FIX-P115-3: FaceTime RealTimeGroup [48x2, 149, 48x7, 149, 48x5] keeps isolated 1-slot Ch 149
   hops at 20 MHz (0 width switches across all 16 slots & U2b pre-tunes), while Sidecar
   [149x6, 48x2, 6, 149x5, 48x2] promotes all 11 multi-slot Ch 149 slots to 80 MHz (class 128)
   and clamps Ch 48 (== infra) to 20 MHz (class 0).
4. FIX-P115-4: WindowCheck::end() in serviceTXBlockAck() returns 0 when Agreed on widthMHz < 80
   (!wideCircuitTripped) and sync shares >= 2 wide 80 MHz 5 GHz slots with the peer — holding
   Station 4 paused across 20 MHz Ch 48 excursions (Slots 6-7) so 64 bulk video packets are never
   stuffed onto contested 26 Mbps Ch 48@20 while unicast gPTP and llw0 bypass immediately on
   Station 3, and still opening Station 4 normally on 20 MHz-only schedules or tripped breakers.
"""
import re
import unittest

import test_awdl_preview82 as p82
import test_regressions as support
from test_offline import definition


def _tick_harness(main_body: str, include_drain_pending: bool = False) -> str:
    tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
    tick_src = tick_src.replace(
        "// NATIVE TX SCENARIOS",
        (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
    )
    prod = p82._native_tick_routines()
    if include_drain_pending:
        prod += "\n" + definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmAWDLInterface::drainPendingData",
        )
    tick_src = tick_src.replace("// PRODUCTION", prod)
    tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
    tick_src = tick_src.replace(
        "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {return width==20 && (c==6||c==44||(c==149&&allow149));}",
        "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {"
        "return (width==20 && (c==6||c==44||c==48||(c==149&&allow149))) || "
        "(width==80 && c==149 && allow149);}",
    )
    return tick_src + main_body


class Preview115LLWRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p115_1_boot_time_clamp_across_psf_mif_umi_and_vht80_tx_asymmetry(self):
        tick_src = _tick_harness(r"""
struct WireSummary {
    bool hasTLV7 = false;
    bool hasTLV17 = false;
    bool hasTLV32 = false;
    uint16_t htCap = 0;
    uint8_t mcs1 = 0;
    uint8_t mcs2 = 0;
};

static WireSummary scanWire(const std::vector<uint8_t> &mpdu) {
    assert(mpdu.size() >= 40 && mpdu[0] == 0xd0);
    WireSummary out = {};
    for (size_t at = 40; at < mpdu.size();) {
        assert(mpdu.size() - at >= 3);
        const uint8_t t = mpdu[at];
        const size_t len = le16(mpdu.data() + at + 1);
        assert(len <= mpdu.size() - at - 3);
        const uint8_t *val = mpdu.data() + at + 3;
        if (t == 7 && len >= 7) {
            out.hasTLV7 = true;
            out.htCap = le16(val + 2);
            out.mcs1 = val[5];
            out.mcs2 = val[6];
        } else if (t == 17) {
            out.hasTLV17 = true;
        } else if (t == 32) {
            out.hasTLV32 = true;
        }
        at += 3 + len;
    }
    return out;
}

static void appendTLVVec(std::vector<uint8_t> &v, uint8_t type, const void *data, size_t len) {
    const size_t off = v.size();
    v.resize(off + 3 + len);
    v[off] = type;
    putLE16(v.data() + off + 1, uint16_t(len));
    if (len) memcpy(v.data() + off + 3, data, len);
}

int main() {
    clockUS = 10000000ULL;
    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 0xa2};
    AirportItlwmAWDLInterface i;
    dataPeer(i, peerAddr);
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.ready = true;
    i.running = true;

    // Simulate T+0 boot BEFORE en1 associates and BEFORE llwParamsEnabled:
    // Even with all 16 slots at 149 @ 80 MHz (!has20MHz5GHzSlot() && !llwParamsEnabled),
    // outbound PSF, auto MIF, immediate MIF, and unicast UMI MUST omit TLV 17 and clear 0x0042!
    i.llwActive = false;
    i.llwParamsEnabled = false;
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = 149;
        i.sync.channelClasses[s] = 128;
    }
    assert(!i.sync.has20MHz5GHzSlot());
    auto *peer = i.device.awdlPeers.find(peerAddr, clockUS);
    assert(peer);
    for (unsigned s = 0; s < 16; ++s) {
        peer->sequence.channel[s] = 149;
        peer->sequence.flags[s] = 128;
    }
    // Inbound iPad advertisement carries full HT40 | SGI40 (0x886f) + 2SS MCS:
    peer->metadata.fields |= HTCapabilities;
    peer->metadata.htCapabilities = 0x886f;
    peer->metadata.rxHTMcs = 0xff;
    peer->metadata.rxHTMcs2 = 0xff;
    peer->htCapabilitiesReceivedUS = clockUS;
    i.sync.epochCounter = 0;
    i.sync.epochUS = clockUS;
    peer->sync.nextWindowTU = 64;
    peer->syncReceivedUS = clockUS;

    // Build payload with TLV 24 + TLV 12 + TLV 21 + TLV 7 (0x886f) + TLV 17 (VHT80) + TLV 32 (ChanSeqExt):
    std::vector<uint8_t> oobPayload;
    uint8_t el24[40] = {};
    copyAddress(el24, i.address.octet);
    copyAddress(el24 + 6, i.address.octet);
    putLE32(el24 + 20, 500);
    putLE32(el24 + 24, 500);
    appendTLVVec(oobPayload, 24, el24, sizeof(el24));
    uint8_t dp12[15] = {0x24, 0x8f, 'X', '0', 0, 0x04, 0};
    copyAddress(dp12 + 7, i.address.octet);
    appendTLVVec(oobPayload, 12, dp12, sizeof(dp12));
    const uint8_t ver21[2] = {0x93, 0x05};
    appendTLVVec(oobPayload, 21, ver21, sizeof(ver21));
    const uint8_t ht7[9] = {0x22, 0x11, 0x6f, 0x88, 0x1b, 0xff, 0xff, 0, 0};
    appendTLVVec(oobPayload, 7, ht7, sizeof(ht7));
    const uint8_t vht17[14] = {0xbf, 12, 0x92, 0x01, 0x80, 0x33, 0xfa, 0xff, 0, 0, 0xfa, 0xff, 0, 0};
    appendTLVVec(oobPayload, 17, vht17, sizeof(vht17));
    const uint8_t ext32[11] = {1, 2, 128, 0, 155, 0, 0x34, 0x12, 2, 0xff, 0xff};
    appendTLVVec(oobPayload, 32, ext32, sizeof(ext32));

    // Configure frameTemplate (PSF), auto broadcast MIF, immediate broadcast MIF, and unicast UMI:
    i.requested = true;
    i.sequenceConfigured = true;
    memcpy(i.frameTemplate, oobPayload.data() + 43 + 18 + 5, oobPayload.size() - (43 + 18 + 5));
    i.templateSize = oobPayload.size() - (43 + 18 + 5);

    apple80211_awdl_oob_request bcastReq = {};
    bcastReq.version = 0;
    memset(bcastReq.destination, 0xff, 6);
    copyAddress(bcastReq.bssid, BSSID);
    bcastReq.flags = 6;
    bcastReq.tx_channel_map = 0xffff;
    bcastReq.data_len = uint16_t(oobPayload.size());
    bcastReq.data = oobPayload.data();
    assert(i.configureBroadcast(bcastReq, true) == kIOReturnSuccess);
    assert(i.configureBroadcast(bcastReq, false) == kIOReturnSuccess);

    peer->nativeCached = true;
    clockUS = i.sync.epochUS + 65536ULL + 5000ULL;
    i.tick(); // Opens availability window on Slot 1 (149@80, counter == 4 != 0)
    assert(i.nativeWindowOpen);
    i.device.broadcastFrames.clear();

    auto umiReq = umiRequest(oobPayload, 0xa2);
    assert(i.queueUnicast(umiReq) == kIOReturnSuccess);
    assert(i.configureBroadcast(bcastReq, false) == kIOReturnSuccess);

    clockUS += 1000;
    i.nextActionUS = clockUS - 1000;
    i.nextMIFUS = clockUS - 1000;
    i.tick();

    // Verify unicast UMI on wire: TLV 17 omitted, TLV 7 HT40|SGI40 cleared (0x886f -> 0x882d), TLV 32 present!
    assert(i.unicastSubmitted == 1 && !i.device.lastUnicast.empty());
    WireSummary umiSum = scanWire(i.device.lastUnicast);
    assert(umiSum.hasTLV7 && !umiSum.hasTLV17 && umiSum.hasTLV32);
    assert(umiSum.htCap == (0x886f & ~0x0042));
    assert(umiSum.mcs1 == 0xff && umiSum.mcs2 == 0xff);

    // Verify immediate MIF + PSF on wire: BOTH omit TLV 17, clamp TLV 7, and carry rebuilt TLV 32!
    assert(i.device.broadcastFrames.size() >= 2);
    for (const auto &bf : i.device.broadcastFrames) {
        WireSummary bs = scanWire(bf);
        assert(bs.hasTLV7 && !bs.hasTLV17 && bs.hasTLV32);
        assert(bs.htCap == (0x886f & ~0x0042));
        assert(bs.mcs1 == 0xff && bs.mcs2 == 0xff);
    }

    // Next superframe (+16 * 65536 us, exact phase multiple!) tests periodic auto-MIF as well:
    i.completeUnicast(i.unicast[0].token, true);
    i.device.broadcastFrames.clear();
    clockUS = i.sync.epochUS + 16ULL * 65536ULL + 5000ULL;
    peer->syncReceivedUS = i.sync.epochUS + 16ULL * 65536ULL;
    peer->lastSeenUS = peer->htCapabilitiesReceivedUS = clockUS;
    i.tick();
    assert(!i.device.broadcastFrames.empty());
    WireSummary autoMifSum = scanWire(i.device.broadcastFrames[0]);
    assert(autoMifSum.hasTLV7 && !autoMifSum.hasTLV17 && autoMifSum.hasTLV32);
    assert(autoMifSum.htCap == (0x886f & ~0x0042));

    // Verify outbound RX clamp has ZERO effect on inbound peer's SGI40 / 2SS TX capabilities (433/866 Mbps TX):
    assert((peer->txHTCap(clockUS) & HTCapSGI40) != 0);
    assert(peer->txHTMcs(clockUS) == 0xff && peer->txHTMcs2(clockUS) == 0xff);

    i.drain();
    assert(!allocations);
    return 0;
}
""")
        self.compile_run(tick_src)

    def test_p115_2_tlv32_rebuilder_wire_format_duplicates_and_asan_heap_bounds(self):
        src = r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "AirportItlwm/AWDL/SyncEngine.hpp"

using namespace AirportAWDL;

static void pushTLV(std::vector<uint8_t> &v, uint8_t type, const void *data, size_t len) {
    const size_t off = v.size();
    v.resize(off + 3 + len);
    v[off] = type;
    putLE16(v.data() + off + 1, uint16_t(len));
    if (len) memcpy(v.data() + off + 3, data, len);
}

static const uint8_t *findTLV(const uint8_t *mpdu, size_t size, uint8_t target, size_t &outLen, unsigned &count) {
    const uint8_t *found = nullptr;
    outLen = 0;
    count = 0;
    for (size_t at = 40; at < size;) {
        assert(size - at >= 3);
        const uint8_t t = mpdu[at];
        const size_t len = le16(mpdu + at + 1);
        assert(len <= size - at - 3);
        if (t == target) {
            if (!found) { found = mpdu + at + 3; outLen = len; }
            ++count;
        }
        at += 3 + len;
    }
    return found;
}

int main() {
    const uint8_t selfAddr[6] = {2, 0x11, 0x22, 0x33, 0x44, 0x55};
    SyncEngine sync = {};
    sync.start(selfAddr, 149, 1000000ULL);

    // 1. Sidecar [149x6, 48x2, 6, 149x5, 48x2]: 3 groups -> (128,149,0x3e3f), (115,48,0xc0c0), (81,6,0x0100)
    const uint8_t scChan[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    for (unsigned s = 0; s < 16; ++s) {
        sync.channels[s] = scChan[s];
        sync.channelClasses[s] = scChan[s] == 149 ? 128 : 0;
    }

    std::vector<uint8_t> extra;
    const uint8_t ht7[9] = {0x22, 0x11, 0xef, 0x01, 0x1b, 0xff, 0xff, 0, 0};
    pushTLV(extra, 7, ht7, sizeof(ht7));
    // Input TLV 32 has only 1 entry (11 bytes) claiming 128 on center 42, plus custom id=0x07, mapid=0x03, timeCtrl=0xa55a:
    const uint8_t inExt32[11] = {0x07, 0x03, 128, 0, 42, 0, 0x5a, 0xa5, 2, 0xff, 0xff};
    pushTLV(extra, 32, inExt32, sizeof(inExt32));
    // Add a duplicate second TLV 32 to prove SyncEngine::action emits rebuilt TLV 32 at most ONCE!
    pushTLV(extra, 32, inExt32, sizeof(inExt32));

    uint8_t probe[1536] = {};
    const size_t scLen = sync.action(probe, sizeof(probe), 1005000ULL, false,
                                     extra.data(), extra.size(), nullptr, false, true);
    assert(scLen > 0);
    size_t tlv32Len = 0;
    unsigned tlv32Count = 0;
    const uint8_t *t32 = findTLV(probe, scLen, 32, tlv32Len, tlv32Count);
    assert(t32 && tlv32Count == 1 && tlv32Len == 33);
    // Group 0: 149 @ opClass 128, bitmap = 0x3e3f (slots 0..5 & 9..13)
    assert(t32[0] == 0x07 && t32[1] == 0x03);
    assert(le16(t32 + 2) == 128 && le16(t32 + 4) == 149 && le16(t32 + 6) == 0xa55a);
    assert(t32[8] == 2 && le16(t32 + 9) == 0x3e3f);
    // Group 1: 48 @ opClass 115, bitmap = 0xc0c0 (slots 6..7 & 14..15)
    assert(t32[11] == 0x07 && t32[12] == 0x03);
    assert(le16(t32 + 13) == 115 && le16(t32 + 15) == 48 && le16(t32 + 17) == 0xa55a);
    assert(t32[19] == 2 && le16(t32 + 20) == 0xc0c0);
    // Group 2: 6 @ opClass 81, bitmap = 0x0100 (slot 8)
    assert(t32[22] == 0x07 && t32[23] == 0x03);
    assert(le16(t32 + 24) == 81 && le16(t32 + 26) == 6 && le16(t32 + 28) == 0xa55a);
    assert(t32[30] == 2 && le16(t32 + 31) == 0x0100);

    Advertisement parsedSC = {};
    assert(parseMPDU(probe, scLen, parsedSC) == ParseResult::Valid);
    for (unsigned s = 0; s < 16; ++s) {
        assert(parsedSC.sequence.channel[s] == scChan[s]);
        assert(parsedSC.sequence.flags[s] == (scChan[s] == 149 ? 128 : (scChan[s] == 48 ? 115 : 81)));
    }

    // 2. Heap-exact ASan bounds test when rebuilt TLV 32 (4 groups = 47B) is +33B LARGER than input (1 group = 14B):
    std::vector<uint8_t> smallExtra;
    pushTLV(smallExtra, 32, inExt32, sizeof(inExt32)); // 14 bytes total
    const uint8_t fourChan[16] = {149,149,48,48,6,6,44,44,0,0,149,149,48,48,0,0};
    for (unsigned s = 0; s < 16; ++s) {
        sync.channels[s] = fourChan[s];
        sync.channelClasses[s] = fourChan[s] == 149 ? 128 : 0; // 149@128, 48@115, 6@81, 44@115 -> 4 groups (44B payload)
    }
    const size_t exact4Len = sync.action(probe, sizeof(probe), 1005000ULL, false,
                                         smallExtra.data(), smallExtra.size(), nullptr, false, true);
    assert(exact4Len > 0);
    // Exact - 1 heap buffer MUST return 0 without touching out of bounds under ASan:
    uint8_t *tooSmall = static_cast<uint8_t *>(malloc(exact4Len - 1));
    assert(tooSmall);
    assert(sync.action(tooSmall, exact4Len - 1, 1005000ULL, false,
                       smallExtra.data(), smallExtra.size(), nullptr, false, true) == 0);
    free(tooSmall);
    // Exact heap buffer MUST succeed and parse cleanly under ASan:
    uint8_t *exactBuf = static_cast<uint8_t *>(malloc(exact4Len));
    assert(exactBuf);
    assert(sync.action(exactBuf, exact4Len, 1005000ULL, false,
                       smallExtra.data(), smallExtra.size(), nullptr, false, true) == exact4Len);
    Advertisement parsed4 = {};
    assert(parseMPDU(exactBuf, exact4Len, parsed4) == ParseResult::Valid);
    for (unsigned s = 0; s < 16; ++s)
        assert(parsed4.sequence.channel[s] == fourChan[s]);
    free(exactBuf);

    // 3. FaceTime RTG [48x2, 149, 48x7, 149, 48x5] uniform 20 MHz: (115,48,0xfbfb) & (124,149,0x0404)
    for (unsigned s = 0; s < 16; ++s) {
        sync.channels[s] = (s == 2 || s == 10) ? 149 : 48;
        sync.channelClasses[s] = 0;
    }
    const size_t rtgLen = sync.action(probe, sizeof(probe), 1005000ULL, false,
                                      smallExtra.data(), smallExtra.size(), nullptr, false, true);
    assert(rtgLen > 0);
    t32 = findTLV(probe, rtgLen, 32, tlv32Len, tlv32Count);
    assert(t32 && tlv32Count == 1 && tlv32Len == 22);
    assert(le16(t32 + 2) == 115 && le16(t32 + 4) == 48 && le16(t32 + 9) == 0xfbfb);
    assert(le16(t32 + 13) == 124 && le16(t32 + 15) == 149 && le16(t32 + 20) == 0x0404);

    // 4. Malformed TLV 32 (bitmapBytes=5 exceeding 9+1=10B payload) is cleanly dropped without failing PSF:
    std::vector<uint8_t> badExtra;
    const uint8_t badExt32[10] = {0, 0, 115, 0, 48, 0, 0, 0, 5, 0xff};
    pushTLV(badExtra, 32, badExt32, sizeof(badExt32));
    const size_t cleanLen = sync.action(probe, sizeof(probe), 1005000ULL, false,
                                        badExtra.data(), badExtra.size(), nullptr, false, true);
    assert(cleanLen > 0);
    t32 = findTLV(probe, cleanLen, 32, tlv32Len, tlv32Count);
    assert(!t32 && tlv32Count == 0);
    return 0;
}
"""
        self.compile_run(src)

    def test_p115_3_facetime_rtg_zero_width_switches_across_all_16_slots_and_u2b_pretunes(self):
        tick_src = _tick_harness(r"""
int main() {
    clockUS = 10000000ULL;
    AirportItlwmAWDLInterface i;
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.ready = true;
    i.running = true;
    i.llwActive = true;
    i.llwParamsEnabled = true;

    const uint16_t rtg[16] = {48,48,149,48,48,48,48,48,48,48,149,48,48,48,48,48};
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = rtg[s];
        i.sync.channelClasses[s] = 0; // FIX-P115-3: uniform 20 MHz across all 16 RTG slots
    }
    i.sync.epochCounter = 0;
    i.sync.epochUS = clockUS;
    i.nextActionUS = i.nextMIFUS = clockUS + 50000000ULL;

    // Step across all 16 slots AND U2b 400 us pre-tunes around Slots 2 & 10:
    // widthMHz MUST remain 20 MHz at every single step (0 width switches)!
    for (unsigned s = 0; s < 16; ++s) {
        const uint64_t slotStart = i.sync.epochUS + uint64_t(s) * 65536ULL;
        clockUS = slotStart + 5000ULL;
        i.tick();
        assert(i.device.awdlRadio.channel == rtg[s]);
        assert(i.device.awdlRadio.widthMHz == 20);

        const unsigned nextS = (s + 1) % 16;
        if (rtg[nextS] != rtg[s]) {
            clockUS = slotStart + 65536ULL - 350ULL;
            i.tick();
            assert(i.device.awdlRadio.channel == 48);
            assert(i.device.awdlRadio.widthMHz == 20);
        }
    }

    i.drain();
    assert(!allocations);
    return 0;
}
""")
        self.compile_run(tick_src)

    def test_p115_4_window_check_keeps_station4_unpaused_on_ch48_20mhz_and_gptp_active(self):
        tick_src = _tick_harness(r"""
static Packet *makeIPv6UDPPacket(const uint8_t *dst, const uint8_t *src, uint16_t dstPort, size_t payloadBytes) {
    const size_t total = 14 + 40 + 8 + payloadBytes;
    auto *m = awdlPacket(total);
    uint8_t *b = m->bytes.data();
    memset(b, 0, total);
    copyAddress(b, dst);
    copyAddress(b + 6, src);
    b[12] = 0x86; b[13] = 0xdd;
    b[14] = 0x60;
    const uint16_t plen = uint16_t(8 + payloadBytes);
    b[18] = uint8_t(plen >> 8); b[19] = uint8_t(plen & 0xff);
    b[20] = 17; // UDP
    b[21] = 64;
    b[54] = uint8_t(dstPort >> 8); b[55] = uint8_t(dstPort & 0xff);
    b[56] = uint8_t(dstPort >> 8); b[57] = uint8_t(dstPort & 0xff);
    b[58] = uint8_t(plen >> 8); b[59] = uint8_t(plen & 0xff);
    return m;
}

int main() {
    clockUS = 10000000ULL;
    const uint8_t ipad[6] = {2, 0, 0, 0, 0, 0xb2};
    AirportItlwmAWDLInterface i;
    dataPeer(i, ipad);
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.ready = true;
    i.device.awdlRadio.llwQueueConfigured = true;
    i.device.awdlRadio.llwQueue = 3;
    i.running = true;
    i.nextActionUS = i.nextMIFUS = clockUS + 50000000ULL;

    const uint8_t sidecar[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    auto *peer = i.device.awdlPeers.find(ipad, clockUS);
    assert(peer);
    peer->sequence.count = 16;
    peer->sequence.encoding = 3;
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = sidecar[s];
        i.sync.channelClasses[s] = sidecar[s] == 149 ? 128 : 0;
        peer->sequence.channel[s] = sidecar[s];
        peer->sequence.flags[s] = sidecar[s] == 149 ? 128 : (sidecar[s] == 48 ? 115 : 81);
    }
    i.sync.epochCounter = 0;
    i.sync.epochUS = clockUS;
    peer->sync.nextWindowTU = 64;
    peer->syncReceivedUS = clockUS;

    auto &tx = i.device.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Agreed;
    tx.enabled = true;
    tx.paused = true;
    tx.cookie = 42;
    tx.epoch = i.device.dmaEpoch;
    copyAddress(tx.peer, ipad);
    tx.window = 64;
    tx.lastDataUS = tx.lastHeavyDataUS = clockUS;

    // 1. Slot 0 (Ch 149 @ 80 MHz): Station 4 opens normally!
    clockUS = i.sync.epochUS + 5000ULL;
    i.tick();
    assert(i.device.awdlRadio.channel == 149 && i.device.awdlRadio.widthMHz == 80);
    i.serviceTXBlockAck();
    assert(!tx.paused && tx.windowEndUS > clockUS);

    // 2. Slot 6 & Slot 14 (Ch 48 @ 20 MHz): FIX-P116-1 + FIX-P116-2 keep Station 4 UNPAUSED!
    for (unsigned targetSlot : {6U, 14U}) {
        tx.paused = true;
        tx.windowEndUS = 0;
        clockUS = i.sync.epochUS + uint64_t(targetSlot) * 65536ULL + 5000ULL;
        peer->lastSeenUS = peer->sequenceReceivedUS = clockUS;
        tx.lastDataUS = tx.lastHeavyDataUS = clockUS;
        i.tick();
        assert(!i.device.lastPolicy.bridgeInfraToWide);
        assert(i.device.awdlRadio.channel == 48 && i.device.awdlRadio.widthMHz == 20);
        i.serviceTXBlockAck();
        assert(!tx.paused && tx.windowEndUS > clockUS);
    }

    // Verify unicast gPTP (UDP 319) continues to transmit immediately on Ch 48 @ 20 MHz!
    i.device.dataFrames.clear();
    auto *ptp = makeIPv6UDPPacket(ipad, i.address.octet, 319, 44);
    i.network.output.push_back(ptp);
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    i.drainPendingData();
    assert(i.device.dataFrames.size() == 1);

    i.drain();
    assert(!allocations);
    return 0;
}
""", include_drain_pending=True)
        self.compile_run(tick_src)


if __name__ == "__main__":
    unittest.main()
