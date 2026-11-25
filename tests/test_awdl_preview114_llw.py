"""Preview 114 LLW / AWDL protocol regressions (updated for Preview 115 Binding-1 safety invariants):
1. Strict infra-overlap 20 MHz clamping: Ch 48 (== infra), adjacent Ch 44, and Ch 52 under 48@160 all
   clamp strictly to 20 MHz (class 0) so MacID 1 never shares Binding 0, while multi-slot Ch 149 runs
   at 80 MHz (class 128) and wideCircuitTripped() runtime fallback reconciles GrantPolicy::windowEnd
   and sync.channelClasses cleanly.
2. setAWDL_SYNC_FRAME_TEMPLATE retains TLV 32 (ChanSeqExtTLV) and SyncEngine::action rebuilds TLV 32
   from sync.channels + sync.channelClasses across normal Sidecar and Scan + Data preservation.
3. FIX-LLW-3: Scan + Data strict zero-punchout guard preserves active non-zero Data + Infra slots
   during continuityMediaActive() && !wclLiveScanActive while keeping nativeSequence readback intact,
   rejecting Low Power / non-subset / all-zero / live-scan sequences, and auto-expiring after 1,000 ms.
4. FIX-LLW-4: tick() runs serviceBlockAck(); serviceUnicast(); sendDueAnnouncements(); so active
   LLW / heavy-RX ADDBA Responses enter Station 3 ahead of due MIF/PSF while idle ticks still drain
   unicast/BA after MIF/PSF.
"""
import re
import unittest

import test_awdl_preview82 as p82
import test_regressions as support
from test_offline import definition


def _control_harness(main_cpp: str) -> str:
    source = (support.ROOT / "tests/fixtures/awdl_control.cpp").read_text().rsplit("int main()", 1)[0]
    source = source.replace(
        "struct ieee80211_node { ieee80211_channel *ni_chan=nullptr; };",
        "struct ieee80211_node { ieee80211_channel *ni_chan=nullptr; uint8_t ni_chw=0; };",
    )
    source = source.replace(
        "struct ieee80211com { unsigned ic_state=IEEE80211_S_INIT;ieee80211_node *ic_bss=nullptr; };",
        "struct ieee80211com { unsigned ic_state=IEEE80211_S_INIT; unsigned ic_flags=0; ieee80211_node *ic_bss=nullptr; };",
    )
    source = source.replace(
        "static uint64_t awdlNow() { return 1010000; }",
        "static uint64_t mockNowUS = 1010000;\nstatic uint64_t awdlNow() { return mockNowUS; }",
    )
    source = source.replace(
        "return (width==20 || (width==80&&channel==149&&allow80)) &&",
        "return (width==20 || (width==80&&(channel==149||channel==44||channel==48||channel==52)&&allow80)) &&",
    )
    source = source.replace(
        "(channel==6||channel==44||(channel==48&&allow48)||(channel==149&&allow149));",
        "(channel==6||channel==44||channel==52||(channel==48&&allow48)||(channel==149&&allow149));",
    )
    handle = "\n".join(
        definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + name)
        for name in ("handle", "handleSetLLWParams", "updatePeerCache", "configureBroadcast", "broadcastDue")
    )
    headers = {
        name: (support.ROOT / "include/Airport" / name).read_text()
        for name in ("apple80211_var.h", "apple80211_ioctl.h")
    }
    names = ["apple80211_channel", "awdl_peer_advert_add"] + sorted(
        set(re.findall(r"\bapple80211_\w+", handle)) | {"apple80211_awdl_af_tx_mode"}
    )
    declarations = []
    for name in names:
        pattern = r"struct " + name + r"\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;"
        matches = [re.search(pattern, header, re.S) for header in headers.values()]
        declaration = next((m[0] for m in matches if m), None)
        if declaration is None:
            raise ValueError("Missing native declaration: " + name)
        declarations.append(declaration)
    interface = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()
    request = re.search(r"enum class Request\s*\{.*?\};", interface, re.S)[0]
    declarations.append(
        re.search(r"^#define APPLE80211_M_PEER_CACHE_CONTROL\s+\d+", headers["apple80211_var.h"], re.M)[0]
    )
    source = source.replace("// NATIVE DECLARATIONS", "\n".join(declarations))
    source = source.replace("// NATIVE REQUESTS", request)
    source = source.replace(
        "IOReturn handleSetLLWParams(const apple80211_llw_params *) { return kIOReturnSuccess; }",
        "IOReturn handleSetLLWParams(const apple80211_llw_params *);",
    )
    source = source.replace("// PRODUCTION", handle)
    return source + main_cpp


class Preview114LLWRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p114_a_strict_infra_20mhz_clamp_multi_slot_149_80mhz_and_breaker_fallback(self):
        ctrl_main = r"""
int main() {
    AirportItlwmAWDLInterface i;
    i.device.ic.ic_state = IEEE80211_S_RUN;
    i.device.ic.ic_bss = &i.device.ap;
    i.device.apChannel.channel = 48;
    i.device.ap.ni_chw = 5; // IEEE80211_CHAN_WIDTH_160
    i.device.allow80 = true;
    i.device.awdlRadio.ready = true;

    // 1. Sidecar [149x6, 48x2, 6, 149x5, 48x2] on 48@160:
    //    Ch 149 (11 slots) promotes to 128 (80 MHz); Ch 48 (4 slots) & Ch 6 clamp to 0 (20 MHz)!
    const unsigned sidecar[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    apple80211_awdl_sync_channel_sequence scSeq = {};
    scSeq.version = 1; scSeq.pad1 = 0x03; scSeq.length = 16; scSeq.encoding = 1; scSeq.step_count = 3; scSeq.fill_channel = 0xff;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = sidecar[n];
        scSeq.seqs[n] = {1, c, c == 48 ? 0x890U : (c == 6 ? 0x8aU : 0x92U)};
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &scSeq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == (sidecar[n] == 149 ? 128 : 0));
    assert(i.sync.has20MHz5GHzSlot() && i.sync.sequenceDiverged);

    // 2. FaceTime RTG [48, 48, 149, 48x7, 149, 48x5] on 48@160: uniform 20 MHz across all 16 slots!
    const unsigned rtg[16] = {48,48,149,48,48,48,48,48,48,48,149,48,48,48,48,48};
    apple80211_awdl_sync_channel_sequence rtgSeq = scSeq;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = rtg[n];
        rtgSeq.seqs[n] = {1, c, c == 48 ? 0x890U : 0x92U};
    }
    apple80211_llw_params llw = {};
    llw.version = 1; llw.enable = 1;
    assert(i.handle(Request::setLLW_PARAMS, &llw) == kIOReturnSuccess);
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &rtgSeq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == 0);
    assert(i.sync.has20MHz5GHzSlot() && i.sync.sequenceDiverged);

    // 3. Adjacent Ch 44 (44 != infra 48, same 80 MHz block) ALSO stays 20 MHz (0):
    apple80211_awdl_sync_channel_sequence adj44 = rtgSeq;
    adj44.seqs[2] = {1, 44, 0x92U};
    adj44.seqs[3] = {1, 44, 0x92U};
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &adj44) == kIOReturnSuccess);
    assert(i.sync.channelClasses[0] == 0);
    assert(i.sync.channelClasses[2] == 0 && i.sync.channelClasses[3] == 0);
    return 0;
}
"""
        self.compile_run(_control_harness(ctrl_main))

        # Verify wideCircuitTripped runtime fallback mid-serviceAWDLChannel on Ch 149 keeps
        # GrantPolicy::windowEnd non-zero and tick() reconciles sync.channelClasses[i] = 0:
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines())
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src = tick_src.replace(
            "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {return width==20 && (c==6||c==44||(c==149&&allow149));}",
            "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {"
            "return (width==20 && (c==6||c==44||c==48||(c==149&&allow149))) || (width==80 && c==149 && allow149);}",
        )
        tick_src = tick_src.replace(
            "RadioState::GrantPolicy lastPolicy={};",
            "bool forceBreakerFallback = false;\n"
            "    uint64_t postFallbackWindowEnd = 0;\n"
            "    RadioState::GrantPolicy lastPolicy={};",
        )
        tick_src = tick_src.replace(
            "awdlRadio.channel=c;awdlRadio.widthMHz=policy.widthMHz;awdlRadio.queue=1;",
            "awdlRadio.channel=c;\n"
            "        if (forceBreakerFallback && c == 149) {\n"
            "            awdlRadio.fwHealth.wideFaultCount = 2;\n"
            "            awdlRadio.widthMHz = 20;\n"
            "            postFallbackWindowEnd = policy.windowEnd ? policy.windowEnd(policy.context, clockUS, policy.slot, c) : 0;\n"
            "        } else {\n"
            "            awdlRadio.widthMHz = policy.widthMHz;\n"
            "        }\n"
            "        awdlRadio.queue=1;",
        )
        tick_src += r"""
int main() {
    clockUS = 10000000;
    AirportItlwmAWDLInterface i;
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.ready = true;
    i.running = true;

    const uint16_t sidecar[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = sidecar[s];
        i.sync.channelClasses[s] = sidecar[s] == 149 ? 128 : 0;
    }
    i.sync.epochCounter = 0;
    i.sync.epochUS = clockUS;

    clockUS = i.sync.epochUS + 5000;
    i.tick();
    assert(i.device.awdlRadio.channel == 149 && i.device.awdlRadio.widthMHz == 80);

    i.device.forceBreakerFallback = true;
    clockUS += 1000;
    i.tick();
    assert(i.device.postFallbackWindowEnd == i.sync.epochUS + 6ULL * 65536);
    assert(i.device.awdlRadio.channel == 149 && i.device.awdlRadio.widthMHz == 20);
    for (unsigned s = 0; s < 16; ++s)
        assert(i.sync.channelClasses[s] == 0);
    assert(i.sync.sequenceDiverged);

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)

    def test_p114_b_and_c_tlv32_rebuild_and_scan_plus_data_punchout_guard(self):
        ctrl_main = r"""
static bool hasTLV(const uint8_t *mpdu, size_t size, uint8_t target) {
    for (size_t at = 40; at < size;) {
        const uint8_t t = mpdu[at];
        const size_t len = le16(mpdu + at + 1);
        if (t == target) return true;
        at += 3 + len;
    }
    return false;
}

static size_t putTLV(uint8_t *buf, size_t off, uint8_t type, const uint8_t *val, size_t len) {
    buf[off] = type;
    putLE16(buf + off + 1, uint16_t(len));
    if (len) memcpy(buf + off + 3, val, len);
    return off + 3 + len;
}

int main() {
    AirportItlwmAWDLInterface i;
    i.requested = true;
    i.device.awdlRadio.ready = true;
    i.device.ic.ic_state = IEEE80211_S_RUN;
    i.device.ic.ic_bss = &i.device.ap;
    i.device.apChannel.channel = 48;
    i.device.ap.ni_chw = 5; // 160 MHz AP
    i.device.allow80 = true;

    // Active Sidecar peer (recent unicast data < 2s, unregistered: cached=0, llwParamsEnabled=false):
    const uint8_t ipad[6] = {2, 0, 0, 0, 0, 22};
    auto &p = i.device.awdlPeers.peers[0];
    p.occupied = true;
    copyAddress(p.address, ipad);
    p.lastSeenUS = mockNowUS;
    p.lastUnicastDataUS = mockNowUS - 100000;

    // Install sync frame template containing TLV 24 + TLV 21 + TLV 7 + TLV 17 + TLV 32:
    uint8_t tpl[256] = {};
    size_t off = 0;
    uint8_t el24[40] = {};
    copyAddress(el24, i.address.octet);
    copyAddress(el24 + 6, i.address.octet);
    putLE32(el24 + 20, 100); putLE32(el24 + 24, 100); putLE32(el24 + 28, 0x12345678);
    off = putTLV(tpl, off, 24, el24, sizeof(el24));
    const uint8_t ver21[2] = {0x93, 0x05};
    off = putTLV(tpl, off, 21, ver21, sizeof(ver21));
    const uint8_t ht7[9] = {0x22, 0x11, 0xef, 0x01, 0x1b, 0xff, 0xff, 0, 0};
    off = putTLV(tpl, off, 7, ht7, sizeof(ht7));
    const uint8_t vht17[14] = {0xbf, 12, 0x92, 0x01, 0x80, 0x33, 0xfa, 0xff, 0, 0, 0xfa, 0xff, 0, 0};
    off = putTLV(tpl, off, 17, vht17, sizeof(vht17));
    const uint8_t ext32[11] = {0, 0, 128, 0, 155, 0, 0, 0, 2, 0xff, 0xff};
    off = putTLV(tpl, off, 32, ext32, sizeof(ext32));
    apple80211_awdl_sync_frame_template tplReq = {1, uint32_t(off), tpl};
    assert(i.handle(Request::setAWDL_SYNC_FRAME_TEMPLATE, &tplReq) == kIOReturnSuccess);
    i.sync.restart(149, mockNowUS);

    // 1. Normal Sidecar Data + Infra [149x6, 48x2, 6, 149x5, 48x2]:
    const unsigned dataInfra[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    apple80211_awdl_sync_channel_sequence dataSeq = {};
    dataSeq.version = 1; dataSeq.pad1 = 0x03; dataSeq.length = 16; dataSeq.encoding = 1; dataSeq.step_count = 3; dataSeq.fill_channel = 0xff;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = dataInfra[n];
        dataSeq.seqs[n] = {1, c, c == 48 ? 0x890U : (c == 6 ? 0x8aU : 0x92U)};
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &dataSeq) == kIOReturnSuccess);
    assert(i.sync.scanPunchoutPreservedUntilUS == 0);

    uint8_t mpdu[1536] = {};
    size_t len = i.sync.action(mpdu, sizeof(mpdu), mockNowUS + 5000, false, i.frameTemplate, i.templateSize, nullptr, false, true);
    assert(len > 0 && hasTLV(mpdu, len, 32) && !hasTLV(mpdu, len, 17) && hasTLV(mpdu, len, 7));

    // 2. Transient Scan + Data [149x5, 0x3, 6, 149x4, 0x3] while !wclLiveScanActive:
    //    - sync.channels & sync.channelClasses stay [149x6, 48x2, 6, 149x5, 48x2] (11 wide 149 slots, 4 20MHz 48 slots)!
    //    - getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE returns exact acknowledged scanSeq!
    //    - SyncEngine::action rebuilds TLV 32 from preserved dataInfra schedule!
    const unsigned scanData[16] = {149,149,149,149,149,0,0,0,6,149,149,149,149,0,0,0};
    apple80211_awdl_sync_channel_sequence scanSeq = dataSeq;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = scanData[n];
        scanSeq.seqs[n] = {1, c, !c ? 0U : (c == 6 ? 0x8aU : 0x92U)};
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &scanSeq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n) {
        assert(i.sync.channels[n] == dataInfra[n]);
        assert(i.sync.channelClasses[n] == (dataInfra[n] == 149 ? 128 : 0));
    }
    assert(i.sync.sequenceDiverged && i.sync.scanPunchoutPreservedUntilUS == mockNowUS + 1000000ULL);
    apple80211_awdl_sync_channel_sequence readback = {};
    assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &readback) == kIOReturnSuccess);
    assert(!memcmp(&readback, &scanSeq, sizeof(scanSeq)));

    len = i.sync.action(mpdu, sizeof(mpdu), mockNowUS + 5000, false, i.frameTemplate, i.templateSize, nullptr, false, true);
    assert(len > 0 && hasTLV(mpdu, len, 32));
    Advertisement advPreserved = {};
    assert(parseMPDU(mpdu, len, advPreserved) == ParseResult::Valid);
    for (unsigned n = 0; n < 16; ++n) {
        assert(advPreserved.sequence.channel[n] == dataInfra[n]);
        assert(advPreserved.sequence.flags[n] == (dataInfra[n] == 149 ? 128 : (dataInfra[n] == 48 ? 115 : 81)));
    }

    // 3. Auto-expiry after 1,000 ms in expireScanPunchout() adopts scanSeq:
    assert(!i.sync.expireScanPunchout(mockNowUS + 999999ULL));
    assert(i.sync.expireScanPunchout(mockNowUS + 1000000ULL));
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channels[n] == scanData[n]);
    assert(!i.sync.sequenceDiverged && i.sync.scanPunchoutPreservedUntilUS == 0);
    len = i.sync.action(mpdu, sizeof(mpdu), mockNowUS + 1048576ULL + 5000ULL, false, i.frameTemplate, i.templateSize, nullptr, false, true);
    assert(len > 0 && hasTLV(mpdu, len, 32));

    // 4. Restore dataSeq, punch out again, then verify Low Power [48, 0, 149, 0x5, 6, 0, 149, 0x5]
    //    (Slot 0: 149 -> 48, NOT a zero-punchout subset) is immediately applied without blocking!
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &dataSeq) == kIOReturnSuccess);
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &scanSeq) == kIOReturnSuccess);
    assert(i.sync.scanPunchoutPreservedUntilUS != 0);
    const unsigned lowPower[16] = {48,0,149,0,0,0,0,0,6,0,149,0,0,0,0,0};
    apple80211_awdl_sync_channel_sequence lpSeq = dataSeq;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = lowPower[n];
        lpSeq.seqs[n] = {1, c, !c ? 0U : (c == 48 ? 0x890U : (c == 6 ? 0x8aU : 0x92U))};
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &lpSeq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channels[n] == lowPower[n]);
    assert(i.sync.channelClasses[0] == 0 && i.sync.channelClasses[2] == 0);
    assert(i.sync.scanPunchoutPreservedUntilUS == 0);

    // 5. Live hardware bgscan (IEEE80211_F_BGSCAN = 0x08000000) MUST bypass punchout preservation:
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &dataSeq) == kIOReturnSuccess);
    i.device.ic.ic_flags = 0x08000000U;
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &scanSeq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channels[n] == scanData[n]);
    assert(i.sync.scanPunchoutPreservedUntilUS == 0);
    return 0;
}
"""
        self.compile_run(_control_harness(ctrl_main))

    def test_p114_d_tick_services_addba_response_before_due_announcements(self):
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines())
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src = tick_src.replace(
            "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {return width==20 && (c==6||c==44||(c==149&&allow149));}",
            "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {"
            "return width==20 && (c==6||c==44||c==48||(c==149&&allow149));}",
        )
        tick_src = tick_src.replace(
            "lastUnicast.assign(packet->bytes.begin(),packet->bytes.begin()+packet->length);",
            "lastUnicast.assign(packet->bytes.begin(),packet->bytes.begin()+packet->length);\n"
            "            com.txq[awdlRadio.queue].queued++;",
        )
        tick_src += r"""
int main() {
    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 9};
    clockUS = 10000000;
    AirportItlwmAWDLInterface i;
    dataPeer(i, peerAddr);
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.ready = true;
    i.device.baResult = 0;
    i.device.baLive = true;
    i.running = true;
    for (unsigned s = 0; s < 16; ++s) i.sync.channels[s] = 48;
    auto *peer = i.device.awdlPeers.find(peerAddr, clockUS);
    assert(peer);
    for (unsigned s = 0; s < 16; ++s) peer->sequence.channel[s] = 48;
    i.sync.epochCounter = 0;
    i.sync.epochUS = clockUS;
    peer->sync.nextWindowTU = 64;
    peer->syncReceivedUS = clockUS;

    // Active LLW session at slot start with BOTH PSF & MIF due AND a queued ADDBA Response:
    i.llwActive = true;
    i.llwParamsEnabled = true;
    clockUS = i.sync.epochUS + 5000;
    i.nextActionUS = clockUS - 1000;
    i.nextMIFUS = clockUS - 1000;

    BlockAckRequest req = {};
    copyAddress(req.peer, peerAddr);
    req.tid = 7; req.dialog = 1; req.window = 64; req.ssn = 0;
    AirportItlwmAWDLInterface::receiveBlockAck(&i, req);
    assert(i.blockAckQueued == 1);
    i.device.transmissions.clear();

    // tick() MUST run serviceBlockAck() BEFORE sendDueAnnouncements():
    // ADDBA Response ('U') enters Station 3 FIRST, and sendDueAnnouncements() sees txq.queued > 0
    // under mediaActive(now) and defers periodic MIF/PSF!
    i.tick();
    assert(i.blockAckSubmitted == 1);
    assert(i.device.transmissions == std::vector<char>({'U'}));
    i.device.com.txq[1].queued = 0;
    i.completeUnicast(i.blockAckReplies[0].token, true);
    assert(i.blockAckAcknowledged == 1);

    // Idle session (!llwParamsEnabled && !heavyReceiveActive): MIF/PSF send first ('M','P'),
    // and then the post-announcement pass drains ADDBA Response ('U') in the SAME tick!
    i.llwActive = false;
    i.llwParamsEnabled = false;
    i.nextActionUS = clockUS - 1000;
    i.nextMIFUS = clockUS - 1000;
    req.dialog = 2;
    AirportItlwmAWDLInterface::receiveBlockAck(&i, req);
    i.device.transmissions.clear();
    i.tick();
    assert(i.blockAckSubmitted == 2);
    assert((i.device.transmissions == std::vector<char>{'M', 'P', 'U'}));

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)


if __name__ == "__main__":
    unittest.main()
