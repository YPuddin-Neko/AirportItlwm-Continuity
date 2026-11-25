"""Preview 113 regressions for Continuity Camera / FaceTime wide-PPDU RX and ADDBA Response latency:
1. (a) RealTimeGroup [48, 48, 149, 48x7, 149, 48x5] sets class 128 (80 MHz) on non-overlapping
   Ch 149/44 slots during llwParamsEnabled while clamping Ch 48 (and adjacent 80 MHz blocks under
   160 MHz APs via infra160) to 20 MHz, keeping U2b GrantPolicy::windowEnd width-consistent across
   48@20 <-> 149@80 transitions.
2. (b) SyncEngine::action and setAWDL_SYNC_FRAME_TEMPLATE strip TLV 32 (ChanSeqExtTLV); when
   clampWidePeerRates is active (llwParamsEnabled with 20 MHz-clamped 5 GHz slots), SyncEngine::action
   omits TLV 17 (VHT Capabilities 0xbf) and clears 0x0042 (HT40 | SGI40) in TLV 7 (HT Capabilities)
   across PSF, MIF, and UMI, while preserving full TLV 7 + TLV 17 VHT80 when clampWidePeerRates is false.
3. (c) Station 3 iwx_flush_sta_tids completes 0 .. first-1 as acknowledged (+awdlRadio.completed) and
   first .. end-1 MGT (ADDBA Response) as flushed via awdlTxFlushed -> completeManagement, re-queuing
   live BlockAckReply without incrementing blockAckFailed or burning entry.attempts.
4. (d) serviceBlockAck prioritizes ADDBA Response ahead of psfDue || broadcastDue under active LLW
   (llwActive && llwParamsEnabled) or heavyReceiveActive(now), while preserving idle announcement order.
"""
import re
import unittest

import test_awdl_preview82 as p82
import test_regressions as support
from test_offline import definition


def _control_harness(main_cpp: str, extra_node_fields: str = "") -> str:
    source = (support.ROOT / "tests/fixtures/awdl_control.cpp").read_text().rsplit("int main()", 1)[0]
    if extra_node_fields:
        source = source.replace(
            "struct ieee80211_node { ieee80211_channel *ni_chan=nullptr; };",
            f"struct ieee80211_node {{ ieee80211_channel *ni_chan=nullptr; {extra_node_fields} }};",
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


class Preview113Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p113_a_rtg_non_overlapping_80mhz_infra160_clamp_and_u2b_window_end(self):
        # 1. Sequence width calculation across RTG [48, 48, 149, 48x7, 149, 48x5] and infra160 overlap:
        ctrl_main = r"""
int main() {
    AirportItlwmAWDLInterface i;
    i.device.ic.ic_state = IEEE80211_S_RUN;
    i.device.ic.ic_bss = &i.device.ap;
    i.device.apChannel.channel = 48;
    i.device.ap.ni_chw = 5; // IEEE80211_CHAN_WIDTH_160 (spans UNII-1 + UNII-2A: 36..64)
    i.device.allow80 = true;
    i.device.awdlRadio.ready = true;

    const unsigned rtg[16] = {48,48,149,48,48,48,48,48,48,48,149,48,48,48,48,48};
    apple80211_awdl_sync_channel_sequence rtgSeq = {};
    rtgSeq.version = 1;
    rtgSeq.pad1 = 0x03; // maxBandwidth >= 3 (80 MHz allowed)
    rtgSeq.length = 16;
    rtgSeq.encoding = 1;
    rtgSeq.step_count = 3;
    rtgSeq.fill_channel = 0xff;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = rtg[n];
        // Ch 48 carries AP 160 MHz metadata (0x890 = ACTIVE | 5GHZ | 160MHZ); Ch 149 carries 0x92.
        rtgSeq.seqs[n] = {1, c, c == 48 ? 0x890U : 0x92U};
    }

    // Order A: setLLW_PARAMS(enable=1) BEFORE setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE:
    // On Preview 115+, Ch 48 (== infra) clamps to 20 MHz (0) and isolated 1-slot Ch 149 hops
    // (Slots 2 & 10, !multiSlotRun) ALSO stay 20 MHz (0) to prevent 4/s 20<->80 MHz RTG width thrash:
    apple80211_llw_params llw = {};
    llw.version = 1;
    llw.enable = 1;
    const uint8_t peer[6] = {2, 0, 0, 0, 0, 9};
    copyAddress(llw.peer_mac, peer);
    assert(i.handle(Request::setLLW_PARAMS, &llw) == kIOReturnSuccess);
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &rtgSeq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == 0);
    assert(i.sync.has20MHz5GHzSlot());

    // Verify infra160 (Ch 48 @ 160 MHz) clamps Ch 52 (DFS/UNII-2A half of 36..64 160 MHz block)
    // to 20 MHz even if explicit80 is requested on Ch 52, while multi-slot Ch 149 (Slots 9..10) promotes to 128!
    apple80211_awdl_sync_channel_sequence overlap160Seq = rtgSeq;
    overlap160Seq.seqs[2] = {1, 52, 0x490U}; // Explicit 80 MHz on Ch 52 (overlaps 48@160!)
    overlap160Seq.seqs[9] = {1, 149, 0x92U};
    overlap160Seq.seqs[10] = {1, 149, 0x92U};
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &overlap160Seq) == kIOReturnSuccess);
    assert(i.sync.channelClasses[0] == 0);
    assert(i.sync.channelClasses[2] == 0);
    assert(i.sync.channelClasses[9] == 128 && i.sync.channelClasses[10] == 128);

    // When AP is 80 MHz (ni_chw = 3, Ch 48 flags = 0x490), Ch 52 does NOT overlap 48@80 -> explicit80 gets 128,
    // while Ch 48 (== infra) ALWAYS stays 0 (20 MHz on Binding 1 / PHY 1)!
    i.device.ap.ni_chw = 3; // IEEE80211_CHAN_WIDTH_80
    for (unsigned n = 0; n < 16; ++n)
        if (overlap160Seq.seqs[n].channel == 48) overlap160Seq.seqs[n].flags = 0x490U;
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &overlap160Seq) == kIOReturnSuccess);
    assert(i.sync.channelClasses[0] == 0);
    assert(i.sync.channelClasses[2] == 128);
    assert(i.sync.channelClasses[9] == 128 && i.sync.channelClasses[10] == 128);

    // Order B: setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE BEFORE setLLW_PARAMS(enable=1):
    // Uniform 20 MHz RTG remains 0 across all 16 slots both before and after LLW enables:
    llw.enable = 0;
    assert(i.handle(Request::setLLW_PARAMS, &llw) == kIOReturnSuccess);
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &rtgSeq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == 0);
    llw.enable = 1;
    assert(i.handle(Request::setLLW_PARAMS, &llw) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == 0);

    return 0;
}
"""
        self.compile_run(_control_harness(ctrl_main, "uint8_t ni_chw=0;"))

        # 2. U2b pre-tune 48@20 <-> 149@80 keeps GrantPolicy::windowEnd width-consistent:
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
            "RadioState::GrantPolicy lastPolicy={};\n    uint64_t preRetuneWindowEnd=0;",
        )
        tick_src = tick_src.replace(
            "lastPolicy=policy;",
            "lastPolicy=policy;\n"
            "        preRetuneWindowEnd = (c && policy.windowEnd) ? "
            "policy.windowEnd(policy.context, clockUS, policy.slot, c) : 0;",
        )
        tick_src += r"""
int main() {
    clockUS = 10000000;
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
        i.sync.channelClasses[s] = (rtg[s] == 149) ? 128 : 0;
    }
    i.sync.epochCounter = 0;
    i.sync.epochUS = clockUS;

    // Slot 0..1 on 48@20:
    clockUS = i.sync.epochUS + 5000;
    i.tick();
    assert(i.device.awdlRadio.channel == 48 && i.device.awdlRadio.widthMHz == 20);

    // At T_slot2 - 400 us (48@20 -> 149@80, 14 -> 2 slots): FIX-P116-4 does NOT cut off Ch 48 early!
    const uint64_t slot2Start = i.sync.epochUS + 2ULL * 65536;
    const uint64_t slot3Start = i.sync.epochUS + 3ULL * 65536;
    clockUS = slot2Start - 400;
    i.tick();
    assert(i.device.awdlRadio.channel == 48 && i.device.awdlRadio.widthMHz == 20);
    assert(i.device.preRetuneWindowEnd == slot2Start);
    assert(i.device.lastPolicy.windowEnd(i.device.lastPolicy.context, clockUS + 200, 2, 149) == 0);

    // Inside Slot 2 (149@80):
    clockUS = slot2Start + 1000;
    i.tick();
    assert(i.device.awdlRadio.channel == 149 && i.device.awdlRadio.widthMHz == 80);
    assert(i.device.preRetuneWindowEnd == slot3Start);

    // At T_slot3 - 350 us (149@80 -> 48@20, 2 -> 14 slots): U2b pre-tunes 149@80 -> 48@20 cleanly,
    // and GrantPolicy::windowEnd stays width-consistent across the remaining 350 us pre-slot lead!
    clockUS = slot3Start - 350;
    i.tick();
    assert(i.device.awdlRadio.channel == 48 && i.device.awdlRadio.widthMHz == 20);
    const uint64_t slot10Start = i.sync.epochUS + 10ULL * 65536;
    assert(i.device.preRetuneWindowEnd == slot10Start);
    assert(i.device.lastPolicy.windowEnd(i.device.lastPolicy.context, clockUS + 150, 3, 48) == slot10Start);
    i.device.awdlRadio.widthMHz = 80;
    assert(i.device.lastPolicy.windowEnd(i.device.lastPolicy.context, clockUS + 150, 3, 48) == 0);
    i.device.awdlRadio.widthMHz = 20;

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)

    def test_p113_b_tlv32_strip_and_outbound_rate_capability_clamp_for_rtg_vs_sidecar(self):
        # Verify setAWDL_SYNC_FRAME_TEMPLATE + SyncEngine::action + tick() / serviceUnicast():
        ctrl_main = r"""
struct ParsedExtra {
    bool hasTLV7 = false;
    bool hasTLV17 = false;
    bool hasTLV32 = false;
    uint16_t awdlPrefix = 0;
    uint16_t htCap = 0;
    uint8_t rxMcs1 = 0;
    uint8_t rxMcs2 = 0;
    size_t vhtLen = 0;
};

static ParsedExtra inspectActionTLVs(const uint8_t *mpdu, size_t size) {
    assert(size >= 40 && mpdu[0] == 0xd0);
    ParsedExtra out = {};
    for (size_t at = 40; at < size;) {
        assert(size - at >= 3);
        const uint8_t type = mpdu[at];
        const size_t len = le16(mpdu + at + 1);
        assert(len <= size - at - 3);
        const uint8_t *val = mpdu + at + 3;
        if (type == 7) {
            out.hasTLV7 = true;
            assert(len >= 7);
            out.awdlPrefix = le16(val);
            out.htCap = le16(val + 2);
            out.rxMcs1 = val[5];
            out.rxMcs2 = val[6];
        } else if (type == 17) {
            out.hasTLV17 = true;
            out.vhtLen = len;
        } else if (type == 32) {
            out.hasTLV32 = true;
        }
        at += 3 + len;
    }
    return out;
}

static size_t appendTLV(uint8_t *buf, size_t off, uint8_t type, const uint8_t *val, size_t len) {
    buf[off] = type;
    putLE16(buf + off + 1, uint16_t(len));
    if (len) memcpy(buf + off + 3, val, len);
    return off + 3 + len;
}

int main() {
    AirportItlwmAWDLInterface i;
    i.sync.restart(149, 1000000);
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = (s == 2 || s == 10) ? 149 : 48;
        i.sync.channelClasses[s] = (s == 2 || s == 10) ? 128 : 0;
    }

    // Build native template payload with:
    // - TLV 24 (Election v2, 40B) + TLV 21 (Version, 2B)
    // - TLV 7 (HT Capabilities: 2B AWDL prefix 0x1122, HT Cap 0x01ef [LDPC|HT40|SGI20|SGI40|STBC],
    //          AMPDU 0x1b, MCS 0xff, 0xff)
    // - TLV 17 (IEEE 802.11 VHT Capabilities 0xbf container, 14B)
    // - TLV 32 (ChanSeqExtTLV claiming opClass 128 on Ch 48)
    uint8_t tpl[256] = {};
    size_t off = 0;
    uint8_t el24[40] = {};
    copyAddress(el24, i.address.octet);
    copyAddress(el24 + 6, i.address.octet);
    putLE32(el24 + 20, 100);
    putLE32(el24 + 24, 100);
    putLE32(el24 + 28, 0x12345678);
    off = appendTLV(tpl, off, 24, el24, sizeof(el24));
    const uint8_t ver21[2] = {0x31, 0x02};
    off = appendTLV(tpl, off, 21, ver21, sizeof(ver21));
    uint8_t ht7[9] = {};
    putLE16(ht7, 0x1122);
    putLE16(ht7 + 2, 0x01ef); // includes 0x0002 (HT40) and 0x0040 (SGI40)
    ht7[4] = 0x1b;
    ht7[5] = 0xff;
    ht7[6] = 0xff;
    off = appendTLV(tpl, off, 7, ht7, sizeof(ht7));
    const uint8_t vht17[14] = {0xbf, 12, 0x92, 0x01, 0x80, 0x33, 0xfa, 0xff, 0x00, 0x00, 0xfa, 0xff, 0x00, 0x00};
    off = appendTLV(tpl, off, 17, vht17, sizeof(vht17));
    const uint8_t ext32[11] = {0, 0, 128, 0, 48, 0, 0, 0, 2, 0xff, 0xff};
    off = appendTLV(tpl, off, 32, ext32, sizeof(ext32));

    apple80211_awdl_sync_frame_template req = {};
    req.version = 1;
    req.payload_len = uint32_t(off);
    req.payload = tpl;
    assert(i.handle(Request::setAWDL_SYNC_FRAME_TEMPLATE, &req) == kIOReturnSuccess);
    // Preview 114 retains TLV 32 in frameTemplate and strips dynamically only on divergence/clamp:
    bool storedTLV32 = false;
    for (size_t at = 0; at < i.templateSize;) {
        if (i.frameTemplate[at] == 32) storedTLV32 = true;
        at += 3 + le16(i.frameTemplate + at + 1);
    }
    assert(storedTLV32);

    // 1. PSF & OOB MIF when schedule matches host template (!sequenceDiverged && !clampWidePeerRates):
    //    TLV 32, TLV 17 (VHT80), and full TLV 7 (0x01ef) are ALL 100% preserved!
    uint8_t frame[1536] = {};
    size_t psfWide = i.sync.action(frame, sizeof(frame), 1005000, false,
                                   i.frameTemplate, i.templateSize, nullptr, false, false);
    assert(psfWide > 0);
    ParsedExtra pWide = inspectActionTLVs(frame, psfWide);
    assert(pWide.hasTLV7 && pWide.hasTLV17 && pWide.hasTLV32);
    assert(pWide.awdlPrefix == 0x1122 && pWide.htCap == 0x01ef);
    assert(pWide.rxMcs1 == 0xff && pWide.rxMcs2 == 0xff && pWide.vhtLen == 14);

    size_t mifWide = i.sync.action(frame, sizeof(frame), 1005000, true,
                                   tpl, off, nullptr, true, false);
    assert(mifWide > 0);
    ParsedExtra mWide = inspectActionTLVs(frame, mifWide);
    assert(mWide.hasTLV7 && mWide.hasTLV17 && mWide.hasTLV32);
    assert(mWide.htCap == 0x01ef);

    // 2. PSF, OOB MIF, & Unicast UMI with clampWidePeerRates == true (Preview 115+):
    //    TLV 32 REBUILT from sync.channels/channelClasses (2 groups: 48@115 + 149@128 = 22B payload vs 11B in tpl),
    //    TLV 17 OMITTED, and TLV 7 clears ONLY 0x0042 -> 0x01ad!
    size_t psfClamped = i.sync.action(frame, sizeof(frame), 1005000, false,
                                      i.frameTemplate, i.templateSize, nullptr, false, true);
    assert(psfClamped == psfWide - (3 + sizeof(vht17)));
    ParsedExtra pClamp = inspectActionTLVs(frame, psfClamped);
    assert(pClamp.hasTLV7 && !pClamp.hasTLV17 && pClamp.hasTLV32);
    assert(pClamp.awdlPrefix == 0x1122);
    assert(pClamp.htCap == (0x01ef & ~0x0042));
    assert(pClamp.rxMcs1 == 0xff && pClamp.rxMcs2 == 0xff);

    // Confirm ActionFrame::parseMPDU sees the exact clamped htCapabilities (0x01ad) and rebuilt TLV 32 sequence:
    Advertisement parsed = {};
    assert(parseMPDU(frame, psfClamped, parsed) == ParseResult::Valid);
    assert((parsed.fields & HTCapabilities) && parsed.htCapabilities == (0x01ef & ~0x0042));
    assert(parsed.rxHTMcs == 0xff && parsed.rxHTMcs2 == 0xff);
    assert(parsed.sequence.flags[0] == 115 && parsed.sequence.flags[2] == 128);

    size_t mifClamped = i.sync.action(frame, sizeof(frame), 1005000, true,
                                      tpl, off, nullptr, true, true);
    assert(mifClamped == mifWide - (3 + sizeof(vht17)));
    ParsedExtra mClamp = inspectActionTLVs(frame, mifClamped);
    assert(mClamp.hasTLV7 && !mClamp.hasTLV17 && mClamp.hasTLV32);
    assert(mClamp.htCap == (0x01ef & ~0x0042));

    const uint8_t dst[6] = {2, 0, 0, 0, 0, 9};
    size_t umiClamped = i.sync.action(frame, sizeof(frame), 1005000, true,
                                      tpl, off, dst, false, true);
    assert(umiClamped > 0);
    ParsedExtra uClamp = inspectActionTLVs(frame, umiClamped);
    assert(uClamp.hasTLV7 && !uClamp.hasTLV17 && uClamp.hasTLV32);
    assert(uClamp.htCap == (0x01ef & ~0x0042));

    return 0;
}
"""
        self.compile_run(_control_harness(ctrl_main))

    def test_p113_c_station3_flush_acks_transmitted_prefix_and_requeues_flushed_addba_response(self):
        # 1. Verify ItlIwx::iwx_flush_sta_tids + ItlIwx::completeAWDLTx end-to-end on Station 3:
        #    0 .. first-1 (already transmitted before flush) complete as acked (+awdlRadio.completed),
        #    while first .. end-1 complete DATA with ECANCELED and MGT via awdlTxFlushed!
        comp_src = (support.ROOT / "tests/fixtures/awdl_completion.cpp").read_text().rsplit(
            "static void completions()", 1
        )[0]
        decls = ["#define IWX_TX_FLUSH_QUEUE_RSP 16"]
        for path, names in (
            (
                "itlwm/hal_iwx/if_iwxreg.h",
                (
                    "iwx_agg_tx_status",
                    "iwx_tx_resp",
                    "iwx_tx_path_flush_cmd",
                    "iwx_flush_queue_info",
                    "iwx_tx_path_flush_cmd_rsp",
                ),
            ),
            ("itlwm/hal_iwx/if_iwxvar.h", ("iwx_tx_data",)),
        ):
            header = (support.ROOT / path).read_text()
            decls.extend(
                re.search(r"struct " + name + r"\s*\{.*?\}[^;]*;", header, re.S)[0] for name in names
            )
        comp_src = comp_src.replace("// NATIVE DECLARATIONS", "\n".join(decls))
        comp_src = comp_src.replace(
            "std::vector<std::pair<uint64_t,int>> dataResults;",
            "void (*awdlTxFlushed)(void *, uint64_t) = nullptr;\n"
            "    std::vector<uint64_t> flushedTokens;\n"
            "    std::vector<std::pair<uint64_t,int>> dataResults;",
        )
        prod_iwx = "\n".join(
            [
                definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::completeAWDLTx"),
                definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_rx_tx_cmd_single"),
                definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_rx_tx_cmd"),
                definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_txd_done"),
                definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_ampdu_txq_advance"),
                definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_reset_tx_ring"),
                definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_flush_sta_tids"),
            ]
        )
        comp_src = comp_src.replace("// PRODUCTION", prod_iwx)
        comp_src += r"""
int main() {
    ItlIwx d;
    AirportAWDL::setTxFlushedCallbackOn(d, [](void *self, uint64_t tok) {
        static_cast<ItlIwx *>(self)->flushedTokens.push_back(tok);
    });
    d.awdlRadio.station = true;
    d.awdlRadio.queue = 1;
    auto &r = d.com.txq[1];
    r.qid = 1;
    r.tail = 0;
    r.cur = 4;
    r.queued = 4;
    Packet packets[4];

    // Slots 0..1 (< first == 2): 1 DATA + 1 MGT already transmitted & acked before flush!
    r.data[0].m = &packets[0]; r.data[0].flags = IWX_TXDATA_FLAG_AWDL;
    r.data[0].awdlToken = 100; r.data[0].type = IEEE80211_FC0_TYPE_DATA;
    r.data[1].m = &packets[1]; r.data[1].flags = IWX_TXDATA_FLAG_AWDL;
    r.data[1].awdlToken = 101; r.data[1].type = IEEE80211_FC0_TYPE_MGT;

    // Slots 2..3 (first == 2 .. end == 4): 1 DATA + 1 MGT (ADDBA Response) flushed in queue!
    r.data[2].m = &packets[2]; r.data[2].flags = IWX_TXDATA_FLAG_AWDL;
    r.data[2].awdlToken = 102; r.data[2].type = IEEE80211_FC0_TYPE_DATA;
    r.data[3].m = &packets[3]; r.data[3].flags = IWX_TXDATA_FLAG_AWDL;
    r.data[3].awdlToken = 103; r.data[3].type = IEEE80211_FC0_TYPE_MGT;

    d.flushResponse.length = sizeof(iwx_tx_path_flush_cmd_rsp);
    auto &resp = *reinterpret_cast<iwx_tx_path_flush_cmd_rsp *>(d.flushResponse.data);
    resp.sta_id = AirportAWDL::RadioState::StationID;
    resp.num_flushed_queues = 1;
    resp.queues[0].tid = IWX_MGMT_TID;
    resp.queues[0].queue_num = 1;
    resp.queues[0].read_before_flush = 2;
    resp.queues[0].read_after_flush = 4;

    assert(!d.iwx_flush_sta_tids(&d.com, AirportAWDL::RadioState::StationID, 0xffff));
    assert(r.queued == 0);
    assert(d.awdlRadio.completed == 2 && d.awdlRadio.failed == 0);
    assert((d.dataResults == std::vector<std::pair<uint64_t, int>>{{100, 0}, {102, ECANCELED}}));
    assert((d.results == std::vector<std::pair<uint64_t, bool>>{{101, true}}));
    assert((d.flushedTokens == std::vector<uint64_t>{103}));
    for (auto &p : packets) assert(p.frees == 1);
    return 0;
}
"""
        self.compile_run(comp_src)

        # 2. Verify AirportItlwmAWDLInterface::completeManagement via setTxFlushedCallbackOn re-queues
        #    flushed BlockAckReply (ADDBA Response) without incrementing blockAckFailed or burning attempts!
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        extra_mgmt = definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmAWDLInterface::completeManagement",
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines() + "\n" + extra_mgmt)
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src = tick_src.replace(
            "void serviceUnicast();void completeUnicast(uint64_t,bool);",
            "void serviceUnicast();void completeUnicast(uint64_t,bool);\n"
            "    void completeManagement(uint64_t,bool,bool=false);",
        )
        tick_src = tick_src.replace(
            "Radio() {awdlRadio.widthMHz=20;}",
            "void *awdlReceiver=nullptr;\n"
            "    void (*awdlTxFlushed)(void *, uint64_t)=nullptr;\n"
            "    Radio() {awdlRadio.widthMHz=20;}",
        )
        tick_src += r"""
int main() {
    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 9};
    clockUS = 10000000;
    AirportItlwmAWDLInterface i;
    dataPeer(i, peerAddr);
    i.device.awdlReceiver = &i;
    AirportAWDL::setTxFlushedCallbackOn(i.device, [](void *owner, uint64_t token) {
        static_cast<AirportItlwmAWDLInterface *>(owner)->completeManagement(token, false, true);
    });
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.ready = true;
    i.device.baResult = 0;
    i.device.baLive = true;
    i.running = true;
    i.nextActionUS = i.nextMIFUS = clockUS + 5000000;
    for (unsigned s = 0; s < 16; ++s) i.sync.channels[s] = 44;
    auto *peer = i.device.awdlPeers.find(peerAddr, clockUS);
    assert(peer);
    for (unsigned s = 0; s < 16; ++s) peer->sequence.channel[s] = 44;
    i.sync.epochCounter = 0;
    i.sync.epochUS = clockUS;
    peer->sync.nextWindowTU = 64;
    peer->syncReceivedUS = clockUS;
    clockUS += 10000;
    i.device.awdlRadio.channel = 44;
    i.device.awdlRadio.widthMHz = 20;
    i.device.awdlRadio.dwell.running = true;
    i.device.awdlRadio.dwell.windowEndUS = clockUS + 40000;
    i.device.awdlRadio.queue = 1;

    BlockAckRequest req = {};
    copyAddress(req.peer, peerAddr);
    req.tid = 7;
    req.dialog = 9;
    req.window = 64;
    req.ssn = 300;
    AirportItlwmAWDLInterface::receiveBlockAck(&i, req);
    assert(i.blockAckQueued == 1);

    i.serviceBlockAck();
    assert(i.blockAckSubmitted == 1 && i.blockAckReplies[0].attempts == 1);
    uint64_t tok = i.blockAckReplies[0].token;

    // Simulate 4 consecutive channel-boundary flushes before RF transmission:
    // NONE of them may increment blockAckFailed or burn the 3 RF retry attempts!
    for (unsigned f = 0; f < 4; ++f) {
        assert(AirportAWDL::notifyTxFlushedOn(i.device, i.device.awdlReceiver, tok));
        assert(i.blockAckFailed == 0);
        assert(i.blockAckReplies[0].state == BlockAckReply::Queued);
        assert(i.blockAckReplies[0].attempts == 0);
        assert(i.blockAckReplies[0].token > tok);
        i.serviceBlockAck();
        assert(i.blockAckSubmitted == 2 + f);
        assert(i.blockAckReplies[0].attempts == 1);
        tok = i.blockAckReplies[0].token;
    }

    // Subsequent RF ACK completes the ADDBA Response cleanly with 0 failures!
    i.completeUnicast(tok, true);
    assert(i.blockAckAcknowledged == 1 && i.blockAckFailed == 0);
    assert(i.blockAckReplies[0].state == BlockAckReply::Free);

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)

    def test_p113_d_service_block_ack_bypasses_announcements_under_active_llw_or_heavy_rx(self):
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

    // Position 8 ms before slot end: announcementWindowOpen() for active media (12 ms lead)
    // has already deferred PSF/MIF, leaving BOTH psfDue(clockUS) and broadcastDue(clockUS) true!
    clockUS = i.sync.epochUS + 65536 - 8000;
    i.nextActionUS = clockUS - 1000;
    i.nextMIFUS = clockUS - 1000;
    assert(i.psfDue(clockUS) && i.broadcastDue(clockUS));
    i.device.awdlRadio.channel = 48;
    i.device.awdlRadio.widthMHz = 20;
    i.device.awdlRadio.dwell.running = true;
    i.device.awdlRadio.dwell.windowEndUS = i.sync.epochUS + 65536 - 400;
    i.device.awdlRadio.queue = 1;

    BlockAckRequest req = {};
    copyAddress(req.peer, peerAddr);
    req.tid = 5;
    req.dialog = 1;
    req.window = 64;
    req.ssn = 0;
    AirportItlwmAWDLInterface::receiveBlockAck(&i, req);
    assert(i.blockAckQueued == 1);

    // 1. Idle (no LLW session, no heavy RX): ADDBA Response still yields to due PSF/MIF:
    i.serviceBlockAck();
    assert(i.blockAckSubmitted == 0 && i.blockAckReplies[0].state == BlockAckReply::Queued);

    // 2. Active LLW session (llwActive && llwParamsEnabled): ADDBA Response bypasses psfDue/broadcastDue!
    i.llwActive = true;
    i.llwParamsEnabled = true;
    i.serviceBlockAck();
    assert(i.blockAckSubmitted == 1 && i.blockAckReplies[0].state == BlockAckReply::Submitted);
    i.completeUnicast(i.blockAckReplies[0].token, true);
    assert(i.blockAckAcknowledged == 1);

    // 3. Heavy RX burst (!llwParamsEnabled, heavyReceiveActive(clockUS) == true): also prioritized!
    i.llwActive = false;
    i.llwParamsEnabled = false;
    i.device.awdlRadio.lastHeavyRXPayloadUS = clockUS - 50000;
    assert(i.device.awdlRadio.heavyReceiveActive(clockUS));
    req.dialog = 2;
    AirportItlwmAWDLInterface::receiveBlockAck(&i, req);
    i.serviceBlockAck();
    assert(i.blockAckSubmitted == 2 && i.blockAckReplies[0].state == BlockAckReply::Submitted);
    i.completeUnicast(i.blockAckReplies[0].token, true);
    assert(i.blockAckAcknowledged == 2);

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)


if __name__ == "__main__":
    unittest.main()
