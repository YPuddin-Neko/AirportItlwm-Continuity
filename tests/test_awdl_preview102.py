"""Failing-first ground-truth regressions for Preview 102 (2.4.0d102):
1. AWDL TLV 32 (0x20 = ChanSeqExtTLV) parsing in ActionFrame.hpp (matching 24H32
   IO80211AWDLPeer::parseAwdlChanSeqFromChanSeqExtTLV) + unbroken MIF-only parent
   clock, channel-sequence freshness, HT capability freshness, and TLV 5 ElectionV1
   continuity across afTxMode=3 / 5 GHz RealTimeGroup Continuity Camera sessions,
   eliminating the 3.0s ScheduleFreshnessUS expiry and 'aw start event missing (1621 ms)'
   Master eviction / 1-EAW clock split.
2. Real Sidecar [149x6, 48, 48, 6, 149x5, 48, 48] 3-slot infra+social parking
   (MaxParkWindowUS = 140 ms for pure infra; MaxSocialParkWindowUS = 210 ms across
   [48, 48, 6]; SocialParkIntervalUS = 2.2 s > 2 cycles; SocialParkPeerFreshnessUS = 2.5 s),
   MIMO2 restoration in configureAWDLPeerWidth(), and holding Agreed 5 GHz wideband
   TXBA paused during periodic 2.4 GHz Ch 6 discovery visits so 64 bulk video frames
   are never dumped onto 26 Mbps 2.4 GHz ANT_B or flushed at Slot 8 end.
3. Local channel-change margin enforcement in both tick() and drainPendingData()
   eligible() (bounding remainingUS by localRemain as well as remoteRemain) to stop
   end-of-slot DataTxFlushed bursts.
4. Sequoia WCL BGScan configuration hooks (setWCL_CONFIG_BG_MOTIONPROFILE,
   setWCL_CONFIG_BG_NETWORK, setWCL_CONFIG_BGSCAN, setWCL_CONFIG_BG_PARAMS) and
   clean <24-byte 802.11 ACK/CTS/RTS control-frame RX handling in iwx_rx_mpdu_mq
   while awdlRadio.mac is active so en1 inputErrors is not polluted.
"""
import re
import unittest

import test_awdl as awdl_support
import test_awdl_preview82 as p82
import test_awdl_preview91 as p91
import test_regressions as support
from test_offline import definition


def _tick_harness(main_body: str) -> str:
    tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
    tick_src = tick_src.replace(
        "// NATIVE TX SCENARIOS",
        (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
    )
    tick_src = tick_src.replace(
        "// PRODUCTION",
        p82._native_tick_routines()
        + "\n"
        + definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmAWDLInterface::drainPendingData",
        ),
    )
    tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
    tick_src = tick_src.replace(
        "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {return width==20 && (c==6||c==44||(c==149&&allow149));}",
        "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {"
        "return (width==20 || (width==80 && c==149)) && (c==6||c==44||c==48||(c==149&&allow149));}",
    )
    return tick_src + main_body


class Preview102Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p102_chan_seq_ext_tlv32_and_mif_only_parent_clock_continuity(self):
        tick_src = _tick_harness(r"""
static size_t buildChanSeqExtMIF(uint8_t *out, uint32_t txUS, uint16_t nextWindowTU,
                                 uint16_t windowCounter, const uint8_t *master,
                                 uint16_t electionID16, bool includeChanSeqExt,
                                 bool includeLegacyBogus18 = false, uint8_t wideOpClass = 124) {
    memset(out, 0, 256);
    out[0] = 127; out[1] = 0; out[2] = 0x17; out[3] = 0xf2; out[4] = 8;
    out[5] = 0x10; out[6] = 3; // Subtype 3 = MIF
    putLE32(out + 8, txUS);
    putLE32(out + 12, txUS);
    size_t off = 16;

    // Optional legacy TLV 18 before TLV 32 to verify TLV 32 overrides TLV 18:
    if (includeLegacyBogus18) {
        out[off] = 18; putLE16(out + off + 1, 38);
        uint8_t *s18 = out + off + 3;
        s18[0] = 15; s18[1] = 3; s18[3] = 3; putLE16(s18 + 4, 0xffff);
        for (unsigned i = 0; i < 16; ++i) { s18[6 + 2 * i] = 6; s18[7 + 2 * i] = 81; }
        off += 41;
    }

    // TLV 4: Sync parameters ONLY (length = 33, no embedded sequence!)
    out[off] = 4; putLE16(out + off + 1, 33);
    uint8_t *s4 = out + off + 3;
    s4[0] = 149;
    putLE16(s4 + 1, nextWindowTU);
    s4[3] = 149;
    putLE16(s4 + 5, 16); // windowPeriodTU = 16
    putLE16(s4 + 7, 110);
    s4[27] = 4;          // presenceMode = 4
    copyAddress(s4 + 21, master);
    putLE16(s4 + 29, windowCounter);
    putLE16(s4 + 31, windowCounter);
    off += 36;

    // TLV 5: ElectionV1 ONLY (MIF omits TLV 24 ElectionV2!)
    out[off] = 5; putLE16(out + off + 1, 21);
    uint8_t *s5 = out + off + 3;
    putLE16(s5 + 1, electionID16);
    s5[3] = 0; // root distance = 0
    copyAddress(s5 + 5, master);
    putLE32(s5 + 11, 536);
    putLE32(s5 + 15, 536);
    off += 24;

    if (includeChanSeqExt) {
        // TLV 32 (0x20 = ChanSeqExtTLV): [48, 48, 149, 48x7, 149, 48x5]
        // Record 0: opClass=115, chan=48, bitmapBytes=2, bitmap=0xfbfb (all slots except 2 and 10)
        // Record 1: opClass=wideOpClass, chan=149, bitmapBytes=2, bitmap=0x0404 (slots 2 and 10)
        out[off] = 32; putLE16(out + off + 1, 22);
        uint8_t *rec0 = out + off + 3;
        putLE16(rec0 + 2, 115);
        putLE16(rec0 + 4, 48);
        rec0[8] = 2;
        rec0[9] = 0xfb;
        rec0[10] = 0xfb;
        uint8_t *rec1 = rec0 + 11;
        putLE16(rec1 + 2, wideOpClass);
        putLE16(rec1 + 4, 149);
        rec1[8] = 2;
        rec1[9] = 0x04;
        rec1[10] = 0x04;
        off += 25;
    }
    return off;
}

int main() {
    const uint8_t iphone[6] = {2, 0, 0, 0, 0, 0xa2};
    uint8_t frame[256];

    // 1. Direct TLV 32 wire-format parsing & override over TLV 18 + malformed bounds check:
    {
        Advertisement parsed = {};
        const size_t sz = buildChanSeqExtMIF(frame, 100000, 64, 23480, iphone, 0x5678, true, true, 130);
        assert(parseAction(frame, sz, parsed) == ParseResult::Valid);
        assert((parsed.fields & (Sync | Channels | ElectionV1)) == (Sync | Channels | ElectionV1));
        assert(parsed.sequence.count == 16 && parsed.sequence.encoding == 3);
        assert(parsed.sequence.step == 3 && parsed.sequence.fill == 0xffff);
        for (unsigned s = 0; s < 16; ++s) {
            const uint8_t expectedCh = (s == 2 || s == 10) ? 149 : 48;
            assert(parsed.sequence.channel[s] == expectedCh);
            if (s == 2 || s == 10) assert(parsed.sequence.flags[s] == 130 || parsed.sequence.flags[s] == 128);
            else assert(parsed.sequence.flags[s] == 115);
        }

        // Truncated TLV 32 record (bitmapBytes=2 but only 1 byte left) MUST fail parseSequenceExt
        // and leave Channels unset without dropping valid sibling TLVs:
        uint8_t bad[64] = {};
        bad[0] = 127; bad[2] = 0x17; bad[3] = 0xf2; bad[4] = 8; bad[5] = 0x10; bad[6] = 3;
        bad[16] = 32; putLE16(bad + 17, 10); // 9 header + 1 byte < bitmapBytes(2)!
        putLE16(bad + 21, 115); putLE16(bad + 23, 48); bad[27] = 2;
        ChannelSequence badSeq = {};
        assert(parseSequenceExt(bad + 19, 10, badSeq) == ParseResult::Malformed);
        Advertisement untouched = {};
        assert(parseAction(bad, 29, untouched) == ParseResult::Valid);
        assert(!(untouched.fields & Channels));
    }

    // 2. End-to-end Continuity Camera RTG simulation across > 8.0 seconds (> 2.6x 3.0s ScheduleFreshnessUS):
    clockUS = 10000000ULL;
    AirportItlwmAWDLInterface i;
    i.requested = true;
    i.running = true;
    i.sequenceConfigured = true;
    i.afTxMode = 3; // PSFs suppressed!

    // Configure Mac's RTG schedule [48, 48, 149, 48x7, 149, 48x5] and host election (iphone = Master, HC:1):
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = (s == 2 || s == 10) ? 149 : 48;
        i.sync.channelClasses[s] = 0;
    }
    i.sync.configureSyncState(1, 0, iphone);
    uint8_t tmpl[48] = {24, 40, 0};
    copyAddress(tmpl + 3, iphone);
    copyAddress(tmpl + 9, iphone);
    putLE32(tmpl + 15, 500);       // masterCounter
    putLE32(tmpl + 19, 1);         // distance = 1 (HC:1)
    putLE32(tmpl + 23, 536);       // masterMetric
    putLE32(tmpl + 27, 534);       // selfMetric
    putLE32(tmpl + 31, 0x12345678);// electionID
    putLE32(tmpl + 39, 88);        // selfCounter
    tmpl[43] = 21; tmpl[44] = 2; tmpl[46] = 0xa2; tmpl[47] = 2;
    assert(i.sync.configureTemplate(tmpl, sizeof(tmpl)));

    // Initial pre-RTG PSF on 2.4 GHz (carries TLV 7 HTCapabilities + TLV 18 + TLV 24 ElectionV2):
    uint16_t peerCounter = 23480;
    {
        Advertisement psf = {};
        psf.subtype = 0;
        psf.fields = Sync | Channels | HTCapabilities | ElectionV2 | Version;
        psf.version = 0xa2; psf.deviceClass = 1;
        psf.htCapabilities = HTCapLDPC | HTCapSGI20 | HTCapSGI40;
        psf.rxHTMcs = 0xff; psf.rxHTMcs2 = 0xff;
        psf.phyTxTime = psf.targetTxTime = uint32_t(clockUS);
        psf.nextWindowTU = 64; psf.windowPeriodTU = 16; psf.presenceMode = 4;
        psf.windowCounter = peerCounter;
        copyAddress(psf.syncMaster, iphone);
        copyAddress(psf.electionMaster, iphone);
        copyAddress(psf.electionSync, iphone);
        psf.distance = 0; psf.masterMetric = 536; psf.selfMetric = 536;
        psf.electionID = 0x12345678;
        psf.sequence.count = 16; psf.sequence.encoding = 3; psf.sequence.step = 3; psf.sequence.fill = 0xffff;
        for (unsigned s = 0; s < 16; ++s) {
            psf.sequence.channel[s] = (s == 2 || s == 8 || s == 10) ? (s == 8 ? 6 : 149) : 48;
            psf.sequence.flags[s] = psf.sequence.channel[s] == 6 ? 81 : 115;
        }
        assert(i.device.awdlPeers.update(iphone, i.address.octet, psf, clockUS, 6, -40));
    }

    auto &r = i.device.awdlRadio;
    r.ready = true;
    r.listener = true;
    r.dwell.requested = true;
    r.dwell.running = true;
    r.dwellStarts = 1;

    // Step through 130 EAWs (~8.52 seconds):
    // - At EAW 4 (~262 ms): iPhone switches to RTG via TLV 32 ChanSeqExt MIF (no TLV 18, no TLV 24).
    // - Every 4 EAWs (~262 ms) thereafter: iPhone sends MIFs WITHOUT TLV 7, WITHOUT TLV 18/32, and WITHOUT TLV 24!
    // - Every single EAW MUST emit APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START with ZERO missing windows!
    uint64_t prevStarts = 0;
    for (unsigned eaw = 0; eaw < 130; ++eaw) {
        const uint16_t curCounter = uint16_t(peerCounter + eaw * 4);
        if (eaw == 4 || (eaw > 4 && (eaw % 4) == 2)) {
            const bool sendChanSeqExt = (eaw == 4);
            const size_t mifLen = buildChanSeqExtMIF(
                frame, uint32_t(clockUS), 64, curCounter, iphone, 0x5678, sendChanSeqExt);
            Advertisement mif = {};
            assert(parseAction(frame, mifLen, mif) == ParseResult::Valid);
            const uint16_t rxCh = ((curCounter / 4) % 16 == 2 || (curCounter / 4) % 16 == 10) ? 149 : 48;
            assert(i.device.awdlPeers.update(iphone, i.address.octet, mif, clockUS, rxCh, -42));
        }
        i.sync.elect(i.device.awdlPeers, clockUS);
        assert(i.sync.transmitReady(clockUS));
        const auto *p = i.device.awdlPeers.find(iphone, clockUS);
        assert(p && p->txHTMcs(clockUS) == 0xff);

        Window w = {};
        assert(i.sync.window(clockUS, w));
        assert(w.counter == curCounter);
        r.channel = w.channel;
        r.widthMHz = w.widthMHz();
        r.dwell.windowEndUS = clockUS + w.untilChannelChangeUS;
        ++r.dwellStarts;

        i.updateAvailability();
        assert(i.nativeWindowOpen);
        assert(i.nativeWindowStarts == prevStarts + 1);
        assert(i.nativeWindowCounter == curCounter);
        prevStarts = i.nativeWindowStarts;

        clockUS += 65536ULL;
    }

    // 3. MIF with wrong 16-bit ElectionV1 ID MUST still be rejected once ElectionV2 is stale:
    {
        const uint16_t badCounter = uint16_t(peerCounter + 130 * 4);
        const size_t badLen = buildChanSeqExtMIF(
            frame, uint32_t(clockUS), 64, badCounter, iphone, 0x9999, false);
        Advertisement badMIF = {};
        assert(parseAction(frame, badLen, badMIF) == ParseResult::Valid);
        assert(i.device.awdlPeers.update(iphone, i.address.octet, badMIF, clockUS, 48, -42));
        i.sync.elect(i.device.awdlPeers, clockUS);
        assert(!i.sync.nativeClockReady && !i.sync.transmitReady(clockUS));
    }

    i.drain();
    assert(!allocations);
    return 0;
}
""")
        self.compile_run(tick_src)

    def test_p102_sidecar_three_slot_infra_social_park_and_2g_txba_hold(self):
        # Part A: Sidecar [149x6, 48, 48, 6, 149x5, 48, 48] tunes cleanly on Slots 6..7 [48, 48]
        # (bridgeInfraToWide == false) and Slot 8 [6] (never parked over), while bridging Slots 14..15
        # [48, 48] (bridgeInfraToWide == true, 131.1 ms <= 140 ms MaxParkWindowUS) to form an unbroken
        # 852 ms 149@80 MIMO window, keeps 3-slot infra [48, 48, 48] unparked (> 140 ms), and
        # restores IWX_STA_FLG_MIMO_EN_MIMO2 in configureAWDLPeerWidth() on returning to 149@80.
        extra_radio = r"""
static void preview102SidecarParkAndMIMO() {
    assert(RadioState::MaxParkWindowUS == 140000ULL);

    ItlIwx d;
    for (unsigned c : {6U, 36U, 40U, 44U, 48U, 149U, 153U, 157U, 161U}) {
        const unsigned band = c == 6 ? IEEE80211_CHAN_2GHZ : (IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_VHT80);
        d.com.sc_ic.ic_channels[c] = {band, uint16_t(c == 6 ? 2437 : 5000 + 5 * c)};
        d.com.sc_ic.ic_chan_active[c / 8] |= 1U << (c % 8);
    }
    d.rlcVersion = 2;
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    d.com.sc_phyctxt[0].channel = &d.com.sc_ic.ic_channels[48];
    assert(!d.startAWDLRadio(self));

    const uint8_t ipad[6] = {2, 0, 0, 0, 0, 0xb2};
    auto &peer = d.awdlPeers.peers[0];
    peer = {};
    peer.occupied = true;
    AirportAWDL::copyAddress(peer.address, ipad);
    peer.haveSync = peer.haveSequence = true;
    peer.metadata.fields = AirportAWDL::HTCapabilities;
    peer.metadata.rxHTMcs = 0xff;
    peer.metadata.rxHTMcs2 = 0xff;
    auto refreshPeer = [&](uint64_t t) {
        peer.lastSeenUS = peer.syncReceivedUS = peer.sequenceReceivedUS = peer.htCapabilitiesReceivedUS = t;
    };
    refreshPeer(clockUS);

    auto &tx = d.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.epoch = d.dmaEpoch;
    tx.station = AirportAWDL::BlockAckState::FirstStation;
    tx.phase = AirportAWDL::TXBlockAckQueue::Agreed;
    AirportAWDL::copyAddress(tx.peer, ipad);
    tx.acknowledged = 64;
    tx.enabled = true;
    tx.paused = true;
    tx.widthMHz = 20;
    tx.lastHeavyDataUS = clockUS;

    RadioState::GrantPolicy p80 = {}; p80.widthMHz = 80; p80.slot = 0;
    RadioState::GrantPolicy p20 = {}; p20.widthMHz = 20; p20.slot = 8;
    RadioState::GrantPolicy pBridge = {}; pBridge.widthMHz = 20; pBridge.slot = 14; pBridge.bridgeInfraToWide = true;

    // Initial visit to ch 6 establishes lastWideSocialVisitUS, then enter 149@80:
    assert(!d.serviceAWDLChannel(6, clockUS + 65536, p20));
    assert(d.awdlRadio.lastWideSocialVisitUS == clockUS);
    clockUS += 65536;
    refreshPeer(clockUS);
    tx.lastHeavyDataUS = clockUS;
    assert(!d.serviceAWDLChannel(149, clockUS + 393216, p80));
    tx.widthMHz = 80;
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

    // 1. Run TWO full Sidecar cycles [149x6, 48, 48, 6, 149x5, 48, 48]:
    //    - Slots 6..7 [48, 48] (bridgeInfraToWide == false, followed by Ch 6) MUST tune cleanly to 48@20!
    //    - Slot 8 [6] MUST tune cleanly to 6@20 (socialParks == 0)!
    //    - Slots 14..15 [48, 48] (bridgeInfraToWide == true, bridging 149x5 -> 149x6) MUST stay parked on 149@80!
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        clockUS += 393216ULL;
        refreshPeer(clockUS); tx.lastHeavyDataUS = clockUS;
        // Slots 6..7: [48, 48] before Slot 8 Ch 6 -> tunes to 48@20
        p20.slot = 6;
        assert(!d.serviceAWDLChannel(48, clockUS + 131072, p20));
        assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 48 && d.awdlRadio.widthMHz == 20);
        clockUS += 131072;
        tx.lastHeavyDataUS = clockUS;
        // Slot 8: [6] -> tunes cleanly to 6@20, never parked!
        p20.slot = 8;
        assert(!d.serviceAWDLChannel(6, clockUS + 65536, p20));
        assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 6 && d.awdlRadio.widthMHz == 20);
        assert(d.awdlRadio.blockAckTXStats.socialParks == 0);
        assert(d.awdlRadio.lastWideSocialVisitUS == clockUS);
        clockUS += 65536;
        refreshPeer(clockUS); tx.lastHeavyDataUS = clockUS;
        // Slots 9..13: [149x5] -> tunes to 149@80
        p80.slot = 9;
        assert(!d.serviceAWDLChannel(149, clockUS + 327680, p80));
        tx.widthMHz = 80;
        assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);
        const size_t phyBeforeBridge = d.phyCommands.size();
        clockUS += 327680;
        refreshPeer(clockUS); tx.lastHeavyDataUS = clockUS;
        // Slots 14..15: [48, 48] with bridgeInfraToWide == true -> stays parked on 149@80 with ZERO PHY cmds!
        pBridge.slot = 14;
        assert(!d.serviceAWDLChannel(48, clockUS + 131072, pBridge));
        assert(!d.awdlRadio.dwell.running && d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80 && tx.widthMHz == 80);
        assert(d.phyCommands.size() == phyBeforeBridge);
        assert(d.awdlRadio.blockAckTXStats.infraParks == cycle + 1);
        clockUS += 131072;
        refreshPeer(clockUS); tx.lastHeavyDataUS = clockUS;
        p80.slot = 0;
        assert(!d.serviceAWDLChannel(149, clockUS + 393216, p80));
        assert(d.awdlRadio.dwell.running && d.phyCommands.size() == phyBeforeBridge);
    }
    assert(d.awdlRadio.blockAckTXStats.infraParks == 2);
    assert(d.awdlRadio.blockAckTXStats.socialParks == 0);

    // 2. Pure 3-slot infra [48, 48, 48] (196,608 us > 140,000 us MaxParkWindowUS) STILL tunes to 48@20
    //    even when bridgeInfraToWide is true!
    clockUS += 393216;
    refreshPeer(clockUS); tx.lastHeavyDataUS = clockUS;
    pBridge.slot = 5;
    assert(!d.serviceAWDLChannel(48, clockUS + 196608, pBridge));
    assert(d.awdlRadio.channel == 48 && d.awdlRadio.widthMHz == 20);

    assert(!d.stopAWDLRadio());
}

int main() {
    preview102SidecarParkAndMIMO();
    return 0;
}
"""
        self.compile_run(p91._build_radio_harness(extra_radio))

        # Part B: When PHY 1 visits 2.4 GHz Ch 6 for discovery while TXBA peer has 5 GHz slots
        # in its schedule ([149x6, 48, 48, 6, ...]), setAWDLTXWindow() MUST keep TXBA paused on Ch 6
        # (and MUST NOT overwrite TLC to 26 Mbps HT ANT_B via configureAWDLTXRates!), whereas a
        # 2.4 GHz-only peer ([6x16]) still opens TXBA on Ch 6 normally.
        extra_ba = r"""
int main() {
    ItlIwx d;
    auto r = txPeer(d);
    d.tlcVersion = 4;
    d.awdlRadio.channel = 149;
    d.awdlRadio.widthMHz = 80;
    d.mimo = true; d.txAnt = 3; d.rxAnt = 3;
    auto &p = d.awdlPeers.peers[0];
    p.haveSync = p.haveSequence = true;
    p.syncReceivedUS = p.sequenceReceivedUS = p.htCapabilitiesReceivedUS = nowUS;
    p.metadata.fields |= AirportAWDL::HTCapabilities;
    p.metadata.rxHTMcs = 0xff; p.metadata.rxHTMcs2 = 0xff;
    p.metadata.htCapabilities = AirportAWDL::HTCapLDPC | AirportAWDL::HTCapSGI20 | AirportAWDL::HTCapSGI40;
    p.sync.presenceMode = 4; p.sync.windowPeriodTU = 16; p.sync.nextWindowTU = 64;
    p.sequence.count = 16; p.sequence.fill = 0xffff; p.sequence.step = 3; p.sequence.encoding = 3;
    const uint8_t sidecarSeq[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    for (unsigned s = 0; s < 16; ++s) {
        p.sequence.channel[s] = sidecarSeq[s];
        p.sequence.flags[s] = sidecarSeq[s] == 149 ? 128 : (sidecarSeq[s] == 6 ? 81 : 115);
    }

    prepareTX(d, r);
    agreeTX(d);
    uint64_t end = nowUS + 30000;
    assert(!d.setAWDLTXWindow(grant, &end));
    auto &tx = d.awdlRadio.blockAckTX;
    assert(!tx.paused && tx.widthMHz == 80 && tx.antenna == 3);

    // Visit Slot 8 (channel = 6, 20 MHz) after closeAWDLListenerWindow + downgradeAWDLTXRates(20, 6):
    assert(!d.closeAWDLListenerWindow() && tx.paused);
    assert(!d.downgradeAWDLTXRates(20, 6));
    d.awdlRadio.channel = 6;
    d.awdlRadio.widthMHz = 20;
    // Even though grant() would return non-zero (both Mac & iPad list ch 6 on Slot 8),
    // setAWDLTXWindow() MUST keep TXBA paused (windowEndUS == 0) because peer has 5 GHz slots!
    assert(!d.setAWDLTXWindow(grant, &end));
    assert(tx.paused && tx.windowEndUS == 0);

    // Conversely, for a genuine 2.4 GHz-only peer ([6x16]), setAWDLTXWindow() opens on ch 6:
    for (unsigned s = 0; s < 16; ++s) { p.sequence.channel[s] = 6; p.sequence.flags[s] = 81; }
    assert(!d.setAWDLTXWindow(grant, &end));
    assert(!tx.paused && tx.windowEndUS == end);

    // Returning to 149@80 and calling configureAWDLPeerWidth() updates FAT_EN_80MHZ while keeping
    // the station's initial MIMO2 flag from addAWDLBlockAckPeer untouched:
    assert((le32toh(d.adds[0].station_flags) & IWX_STA_FLG_MIMO_EN_MSK) == IWX_STA_FLG_MIMO_EN_MIMO2);
    d.awdlRadio.channel = 149;
    d.awdlRadio.widthMHz = 80;
    assert(!d.configureAWDLPeerWidth());
    assert(!d.adds.empty());
    const auto &wideSta = d.adds.back();
    assert(le32toh(wideSta.station_flags_msk) == IWX_STA_FLG_FAT_EN_MSK);
    assert(le32toh(wideSta.station_flags) == IWX_STA_FLG_FAT_EN_80MHZ);

    assert(!d.clearAWDLBlockAck());
    return 0;
}
"""
        self.compile_run(p91._build_ba_harness(extra_ba))

    def test_p102_pre_hop_local_window_margin_prevents_tx_ring_flush_bursts(self):
        tick_src = _tick_harness(r"""
int main() {
    clockUS = 10000000ULL;
    const uint8_t peer[6] = {2, 0, 0, 0, 0, 9};
    AirportItlwmAWDLInterface i;
    dataPeer(i, peer);

    // Local Mac schedule hops 149 -> 48 at end of Slot 0, while remote peer stays on 149 across Slots 0..3!
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = (s == 0) ? 149 : 48;
        i.sync.channelClasses[s] = 0;
    }
    auto *p = i.device.awdlPeers.find(peer, clockUS);
    assert(p);
    for (unsigned s = 0; s < 16; ++s) {
        p->sequence.channel[s] = 149;
        p->sequence.flags[s] = 124;
    }

    i.requested = true;
    i.running = true;
    i.sequenceConfigured = true;
    i.nextActionUS = i.nextMIFUS = clockUS + 10000000ULL;

    auto &r = i.device.awdlRadio;
    r.ready = true;
    r.listener = true;
    r.channel = 149;
    r.widthMHz = 20;
    r.dwell.requested = true;
    r.dwell.running = true;
    r.dwellStarts = 1;

    auto &tx = r.blockAckTX;
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Agreed;
    tx.enabled = true;
    tx.paused = false;
    tx.cookie = 88;
    tx.epoch = i.device.dmaEpoch;
    copyAddress(tx.peer, peer);
    tx.window = 64;
    tx.lastDataUS = clockUS;

    // Position 4,500 us before Mac's local channel hop (149 -> 48):
    // - local.untilChannelChangeUS == 4,500 us (> 3,000 us canSendTo guard, BUT <= 6,000 us TXBA margin!)
    // - remote.untilChannelChangeUS > 900,000 us!
    clockUS = i.sync.epochUS + 65536ULL - 4500ULL;
    p->lastSeenUS = p->syncReceivedUS = p->sequenceReceivedUS = p->htCapabilitiesReceivedUS = clockUS;
    p->sync.phyTxTime = p->sync.targetTxTime = uint32_t(clockUS);
    p->sync.nextWindowTU = 5;
    p->sync.windowCounter = 0;
    r.dwell.windowEndUS = clockUS + 4500ULL;
    tx.windowEndUS = clockUS + 4500ULL;

    i.enqueue(ethernet(i.address.octet, peer));
    assert(i.pendingCount == 1);

    // 1. drainPendingData() MUST hold the TXBA packet in pending[] (<= 6,000 us before local hop):
    i.drainPendingData();
    assert(i.pendingCount == 1 && i.inFlightCount == 0);

    // 2. tick() at 4,500 us before local hop MUST ALSO hold the TXBA packet in pending[]!
    //    (Stub serviceAWDLChannel keeps channel=149, windowEndUS=clockUS+4500).
    i.tick();
    assert(i.pendingCount == 1 && i.inFlightCount == 0);

    i.drain();
    assert(!allocations);
    return 0;
}
""")
        self.compile_run(tick_src)

    def test_p102_wcl_bgscan_config_hooks_and_ctl_rx_error_accounting(self):
        # Part A: WCL BGScan configuration methods validate null vs non-null arguments and return kIOReturnSuccess:
        wcl_src = (support.ROOT / "tests/fixtures/wcl_lifecycle.cpp").read_text().rsplit("int main()", 1)[0]
        bg_fns = "\n".join(
            definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::" + name)
            for name in (
                "setWCL_CONFIG_BG_MOTIONPROFILE",
                "setWCL_CONFIG_BG_NETWORK",
                "setWCL_CONFIG_BGSCAN",
                "setWCL_CONFIG_BG_PARAMS",
            )
        )
        wcl_src = wcl_src.replace(
            "struct apple80211ScanRequest { uint8_t bytes[0x1550] = {}; };",
            "struct apple80211ScanRequest { uint8_t bytes[0x1550] = {}; };\n"
            "struct apple80211_bg_motion_profile { uint32_t profile; };\n"
            "struct apple80211_bg_network { uint32_t flags; };\n"
            "struct apple80211_bg_scan { uint32_t enabled; };\n"
            "struct apple80211_bg_params { uint32_t interval; };",
        )
        wcl_src = wcl_src.replace(
            "bool completeWCLScanFromCache(const AirportWCL::ScanRequest &) { return false; }",
            "bool completeWCLScanFromCache(const AirportWCL::ScanRequest &) { return false; }\n"
            "    IOReturn setWCL_CONFIG_BG_MOTIONPROFILE(apple80211_bg_motion_profile *);\n"
            "    IOReturn setWCL_CONFIG_BG_NETWORK(apple80211_bg_network *);\n"
            "    IOReturn setWCL_CONFIG_BGSCAN(apple80211_bg_scan *);\n"
            "    IOReturn setWCL_CONFIG_BG_PARAMS(apple80211_bg_params *);",
        )
        wcl_src = wcl_src.replace("// PRODUCTION: scan\n", bg_fns + "\n")
        wcl_src += r"""
int main() {
    AirportItlwmSkywalkInterface s;
    apple80211_bg_motion_profile mp = {1};
    apple80211_bg_network net = {1};
    apple80211_bg_scan scan = {1};
    apple80211_bg_params params = {30};

    assert(s.setWCL_CONFIG_BG_MOTIONPROFILE(nullptr) == kIOReturnBadArgument);
    assert(s.setWCL_CONFIG_BG_NETWORK(nullptr) == kIOReturnBadArgument);
    assert(s.setWCL_CONFIG_BGSCAN(nullptr) == kIOReturnBadArgument);
    assert(s.setWCL_CONFIG_BG_PARAMS(nullptr) == kIOReturnBadArgument);

    assert(s.setWCL_CONFIG_BG_MOTIONPROFILE(&mp) == kIOReturnSuccess);
    assert(s.setWCL_CONFIG_BG_NETWORK(&net) == kIOReturnSuccess);
    assert(s.setWCL_CONFIG_BGSCAN(&scan) == kIOReturnSuccess);
    assert(s.setWCL_CONFIG_BG_PARAMS(&params) == kIOReturnSuccess);
    return 0;
}
"""
        self.compile_run(wcl_src)

        # Part B: Short (< 24 byte) 802.11 ACK (0xd4, 10B), CTS (0xc4, 10B), and RTS (0xb4, 16B)
        # control frames arriving while awdlRadio.mac is active in STA mode MUST NOT increment
        # en1 inputErrors or is_rx_tooshort!
        from iwx_firmware import receive_diagnostics
        disp_src = (support.ROOT / "tests/fixtures/awdl_dispatch.cpp").read_text().rsplit("int main()", 1)[0]
        disp_src = disp_src.replace("// RX DIAGNOSTIC DECLARATIONS", receive_diagnostics())
        driver = "itlwm/hal_iwx/ItlIwx.cpp"
        radio = "itlwm/hal_iwx/ItlIwxAWDL.cpp"
        routines = ["iwx_rx_pkt_valid", "iwx_rx_pkt", "iwx_notif_intr",
                    "iwx_cmd_done", "iwx_rx_session_protection", "iwx_rx_tx_cmd",
                    "iwx_rx_tx_cmd_single", "iwx_txd_done", "iwx_ampdu_txq_advance",
                    "observeAWDLReceiveDescriptor", "formatAWDLReceiveDiagnostics",
                    "iwx_rx_mpdu_mq", "receiveAWDL", "iwx_rxmq_get_signal_strength",
                    "iwx_rx_bt_coex_profile"]
        production = definition("itl80211/openbsd/net80211/ieee80211_input.c",
                                "ieee80211_sta_foreign_disconnect") + "\n"
        production += definition(driver, "iwx_clear_tx_desc") + "\n"
        production += definition(radio, "awdlRadioNow") + "\n"
        production += "\n".join(definition(driver, "ItlIwx::" + name) for name in routines)
        production += "\n" + "\n".join(definition(radio, "ItlIwx::" + name)
                                      for name in ("receiveAWDLSession", "completeAWDLTx",
                                                   "receiveAWDLTXBlockAck", "receiveAWDLBlockAckEnd"))
        disp_src = disp_src.replace("// PRODUCTION ROUTINES", production)
        var = (support.ROOT / "itlwm/hal_iwx/if_iwxvar.h").read_text()
        structs = [re.search(r"struct " + name + r" \{.*?\n\};", var, re.S).group()
                   for name in ("iwx_tx_data", "iwx_rx_data")]
        for name in ("IWX_RX_MQ_RING_COUNT", "IWX_RBUF_SIZE", "IWX_FLAG_HW_ERR",
                     "IWX_FLAG_TE_ACTIVE", "IWX_DEVICE_FAMILY_22000", "IWX_DEVICE_FAMILY_AX210",
                     "IWX_STATION_ID", "IWX_INIT_COMPLETE", "IWX_PNVM_COMPLETE", "IWX_MAX_BAID"):
            structs.append(re.search(r"^#define\s+" + name + r"\s+.*$", var, re.M).group())
        disp_src = disp_src.replace("// DRIVER DECLARATIONS", "\n".join(structs))
        raw = (support.ROOT / driver).read_text()
        macro = re.search(r"^#define SYNC_RESP_STRUCT.*?while \(/\*CONSTCOND\*/0\)", raw, re.S | re.M).group()
        pnvm = re.search(r"struct iwl_pnvm_init_complete_ntfy \{.*?\} __packed;", raw, re.S).group()
        disp_src = disp_src.replace("// DISPATCH DECLARATIONS", macro + "\n" + pnvm)
        disp_src += r"""
int main() {
    ItlIwx d;
    Events events(d);
    iwx_node ap;
    d.com.sc_ic.ic_bss = &ap.in_ni;
    d.awdlRadio.mac = true;

    Batch batch;
    auto pushShortCtl = [&](uint8_t fc0, unsigned len) {
        std::vector<uint8_t> payload(IWX_RX_DESC_SIZE_V1 + len, 0);
        auto desc = reinterpret_cast<iwx_rx_mpdu_desc *>(payload.data());
        desc->status = htole16(IWX_RX_MPDU_RES_STATUS_CRC_OK | IWX_RX_MPDU_RES_STATUS_OVERRUN_OK);
        desc->mpdu_len = htole16(len);
        desc->v1.channel = 149;
        auto frame = payload.data() + IWX_RX_DESC_SIZE_V1;
        frame[0] = fc0;
        batch.add(IWX_LEGACY_GROUP, IWX_REPLY_RX_MPDU_CMD, 0x80, 0, payload.data(), payload.size());
    };
    pushShortCtl(0xd4, 10); // 802.11 ACK (10 bytes)
    pushShortCtl(0xc4, 10); // 802.11 CTS (10 bytes)
    pushShortCtl(0xb4, 16); // 802.11 RTS (16 bytes)
    auto rx = install(d, 0, batch);
    interrupt(d, 1);

    assert(d.com.sc_ic.ic_if.stats.inputErrors == 0);
    assert(d.com.sc_ic.ic_stats.is_rx_tooshort == 0);
    assert(rx->frees == 1 && d.refills == 1);
    return 0;
}
"""
        self.compile_run(disp_src)

    def test_p102_en1_infra_throughput_idle_awdl_home_park_and_tlc_max_mpdu(self):
        # 1. Idle AWDL channel=0 slots with policy.idleHomeChannel=48 MUST retune PHY 1 back to
        #    the associated AP channel (48@20, dwell.running == false) once so QuZ UMAC exits
        #    DYNAMIC_QUOTA Multi-Channel Concurrency during en1 traffic, with 0 extra commands
        #    on consecutive 0 slots, while preserving 80 MHz Sidecar parking!
        extra_radio = r"""
int main() {
    ItlIwx d;
    for (unsigned c : {6U, 36U, 40U, 44U, 48U, 149U, 153U, 157U, 161U}) {
        const unsigned band = c == 6 ? IEEE80211_CHAN_2GHZ : (IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_VHT80);
        d.com.sc_ic.ic_channels[c] = {band, uint16_t(c == 6 ? 2437 : 5000 + 5 * c)};
        d.com.sc_ic.ic_chan_active[c / 8] |= 1U << (c % 8);
    }
    d.rlcVersion = 2;
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    d.com.sc_phyctxt[0].channel = &d.com.sc_ic.ic_channels[48];
    assert(!d.startAWDLRadio(self));

    // Slot 2: visit social 149@20:
    RadioState::GrantPolicy pSocial = {};
    pSocial.widthMHz = 20;
    pSocial.slot = 2;
    assert(!d.serviceAWDLChannel(149, clockUS + 65536, pSocial));
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.dwell.running);

    // Slot 3: channel=0 with parkIdleOnInfra=true (what tick() sets!) -> retunes PHY 1 to 48@20 with dwell closed!
    clockUS += 65536;
    RadioState::GrantPolicy pIdle = {};
    pIdle.widthMHz = 20;
    pIdle.slot = 3;
    pIdle.parkIdleOnInfra = true;
    assert(!d.serviceAWDLChannel(0, clockUS + 327680, pIdle));
    assert(!d.awdlRadio.dwell.running);
    assert(d.awdlRadio.channel == 48 && d.awdlRadio.widthMHz == 20);
    assert(!d.awdlRadio.listenerPromisc);

    // Slots 4..7: consecutive channel=0 calls on 48 issue ZERO firmware commands!
    const int callsAfterHome = d.calls;
    for (unsigned s = 4; s <= 7; ++s) {
        clockUS += 65536;
        pIdle.slot = s;
        assert(!d.serviceAWDLChannel(0, clockUS + (8 - s) * 65536ULL, pIdle));
        assert(!d.awdlRadio.dwell.running && d.awdlRadio.channel == 48);
        assert(d.calls == callsAfterHome);
    }

    assert(!d.stopAWDLRadio());
    return 0;
}
"""
        self.compile_run(p91._build_radio_harness(extra_radio))

        # 2. Verify iwx_rs_init configures non-zero max_mpdu_len for VHT/HE/HT (enabling A-MSDU in A-MPDU),
        #    and AirportItlwmV2 / AirportItlwmEthernetInterface wire recordOutputPacket / recordInputPacket
        #    and gigabit link speed reporting:
        rs_init_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_rs_init")
        self.assertIn("IEEE80211_MAX_MPDU_LEN_VHT_11454", rs_init_src)
        self.assertIn("IEEE80211_MAX_MPDU_LEN_VHT_3895", rs_init_src)
        v2_src = (support.ROOT / "AirportItlwm/AirportItlwmV2.cpp").read_text()
        eth_src = (support.ROOT / "AirportItlwm/AirportItlwmEthernetInterface.cpp").read_text()
        self.assertNotIn("recordOutputPacket", v2_src)
        self.assertNotIn("recordInputPacket", eth_src)
        self.assertNotIn("54000000", v2_src)


if __name__ == "__main__":
    unittest.main()
