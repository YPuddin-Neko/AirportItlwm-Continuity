"""Failing-first regressions for Preview 99:
1. Restore 15 ms (15000 us) TID 7 RX reorder gap timeout even when llwRealtime is active while
   retaining immediate 0 ms BAR (0x84) advancement and reordered-delivery publish bypass:
   prevents premature 8 ms gap expiries inside the 9-14 ms 802.11 MAC BlockAck retry window
   (LLWReorderBelowWindow: 6 -> 0) that tore zero-FEC Continuity Camera HEVC frames into SendFIR storms.
2. Stable 80 MHz Sidecar awdlTXWidth across TLV-4/legacy encoding==1 channel sequences and 5 GHz
   operating classes, plus continuous non-LLW listener dwell and sequence-anchored mid-run
   sinceChannelStartUS across parent MIF/PSF clock refreshes: eliminates the 405 TLC_MNG_CONFIG_CMD
   resets (26 Mbps lock), mid-run ADD_STA station pauses, IWX_TXPATH_FLUSH discards, and false 3 ms
   guard stalls at internal EAW boundaries.
3. Unconditional 2.4 GHz (channel <= 14) non_shared_ant SISO isolation in awdlTXAntenna() and
   downgradeAWDLTXRates() whenever btCoex.enabled is true (even when btCoex.activity == 0 because
   en1 is on 5 GHz and bt_primary_ci == 0): prevents PHY 1 TLC on Channel 6 from claiming shared
   CNVi Bluetooth Antenna A and triggering QuZ LMAC assert 0x20A2 (8354), while keeping full 2x2 MIMO
   (chains == 3) on all 5 GHz channels.
4. Per-peer EAGAIN rotation in drainPendingData() and tick(): when one peer returns EAGAIN (e.g. full
   64-MPDU TXBA window or Retiring/Enabling transition), frames for other AWDL peers, unicast gPTP
   (UDP 319/320), and urgent llw0 traffic transmit immediately while preserving strict per-peer FIFO order.
"""
import unittest

import test_awdl_preview82 as p82
import test_awdl_preview91 as p91
import test_regressions as support
from test_offline import definition


class Preview99Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p99_llw_tid7_15ms_reorder_window_absorbs_mac_retries_without_below_window_drops(self):
        extra_ba = r"""
int main() {
    ItlIwx d;
    d.awdlRadio.ready = true;
    d.awdlRadio.listener = true;
    d.awdlRadio.llwRealtime = true;
    auto r7 = request(d, 0, 7, 200);
    unsigned b7 = start(d, r7);
    auto &buffer = d.com.sc_rxba_data[b7].reorder_buf;
    const unsigned pubBefore = d.published;

    // 1. Out-of-order SN 201 arrives while SN 200 is undergoing 802.11 MAC BlockAck retry:
    //    TID 7 reorder deadline is 8 ms (8000 us) so boundary holes release promptly!
    auto p1 = packet(d, r7, 201, 81);
    auto p2 = packet(d, r7, 202, 82);
    receive(d, p1.get(), b7, 201, 200);
    const uint64_t expectedDeadline = nowUS + 8000;
    assert(buffer.reorder_timer->deadline == expectedDeadline);
    nowUS += 3000;
    receive(d, p2.get(), b7, 202, 200);
    assert(d.delivered.empty());
    assert(buffer.reorder_timer->deadline == expectedDeadline);

    // 2. MAC retry for SN 200 arrives at +6 ms (6000 us < 8000 us):
    //    all 3 frames [80, 81, 82] MUST deliver in exact order with ZERO below-window drops!
    nowUS += 3000; // +6 ms total since SN 201 arrived
    auto p0 = packet(d, r7, 200, 80);
    receive(d, p0.get(), b7, 200, 203);
    assert((d.delivered == std::vector<uint16_t>{80, 81, 82}));
    assert(p0->frees && p1->frees && p2->frees);
    assert(d.awdlBA.llwBelowWindow == 0);
    assert(d.awdlBA.llwGapExpiries == 0);
    assert(d.published == pubBefore); // Reordered LLW delivery still skips IORegistry publish!

    // 3. When the peer genuinely abandons SN 203, an 802.11 BAR (0x84) advancing SSN to 204
    //    still releases buffered SN 204 immediately in 0 ms without waiting 15 ms:
    auto p4 = packet(d, r7, 204, 84);
    receive(d, p4.get(), b7, 204, 203);
    assert(d.delivered.size() == 3 && buffer.num_stored == 1);
    auto bar = std::unique_ptr<Packet>(new Packet);
    bar->bytes.resize(20, 0);
    bar->bytes[0] = 0x84;
    copyAddress(bar->bytes.data() + 4, d.awdlRadio.address);
    copyAddress(bar->bytes.data() + 10, r7.peer);
    bar->bytes[16] = 0x04;
    bar->bytes[17] = uint8_t(7 << 4);
    putLE16(bar->bytes.data() + 18, uint16_t(204 << 4));
    iwx_rx_mpdu_desc barDesc = {};
    barDesc.reorder_data = (b7 << 24) | 204;
    mbuf_list ml;
    assert(d.receiveAWDLReordered(bar.get(), &barDesc, &ml));
    assert((d.delivered == std::vector<uint16_t>{80, 81, 82, 84}) && p4->frees && bar->frees);
    assert(d.awdlBA.barFrames == 1);

    assert(!d.clearAWDLBlockAck());
    return 0;
}
"""
        ba_harness = p91._build_ba_harness(extra_ba).replace(
            "void publishAWDLReceive(){}",
            "unsigned published=0;void publishAWDLReceive(){++published;}",
        )
        self.compile_run(ba_harness)

    def test_p99_sidecar_80mhz_tlc_survives_encoding1_mifs_and_parent_clock_refresh_without_pause_or_flush(self):
        # Part A: awdlTXWidth + setAWDLTXWindow keep 80 MHz TLC unbroken across TLV-4/legacy encoding==1
        # [flags, channel] sequences (PSF <-> MIF alternation) and 5 GHz operating classes 124..130 / 0.
        extra_ba = r"""
int main() {
    ItlIwx d;
    auto r = txPeer(d);
    d.tlcVersion = 4;
    d.awdlRadio.channel = 149;
    d.awdlRadio.widthMHz = 80;
    d.mimo = true; d.txAnt = 3; d.rxAnt = 3;
    auto &p = d.awdlPeers.peers[0];
    p.metadata.fields |= AirportAWDL::HTCapabilities;
    p.metadata.rxHTMcs = 0xff;
    p.metadata.rxHTMcs2 = 0xff;
    p.metadata.htCapabilities = AirportAWDL::HTCapLDPC | AirportAWDL::HTCapSGI20 | AirportAWDL::HTCapSGI40;
    p.htCapabilitiesReceivedUS = nowUS;
    p.haveSync = p.haveSequence = true;
    p.syncReceivedUS = p.sequenceReceivedUS = nowUS;
    p.sync.presenceMode = 4; p.sync.windowPeriodTU = 16; p.sync.nextWindowTU = 64;
    p.sequence.count = 16; p.sequence.fill = 0xffff; p.sequence.step = 3; p.sequence.encoding = 3;

    const uint8_t sidecarCh[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    for (unsigned s = 0; s < 16; ++s) {
        p.sequence.channel[s] = sidecarCh[s];
        p.sequence.flags[s] = sidecarCh[s] == 149 ? 128 : (sidecarCh[s] == 48 ? 115 : 81);
    }
    p.sync.windowCounter = 0;
    prepareTX(d, r);
    agreeTX(d);
    uint64_t end = nowUS + 30000;
    assert(!d.setAWDLTXWindow(grant, &end));
    assert(d.awdlRadio.blockAckTX.widthMHz == 80 && !d.awdlRadio.blockAckTX.paused);
    const uint64_t tlcInitial = d.awdlRadio.blockAckTXStats.tlcConfigs;
    assert(tlcInitial == 1);

    // 1. Alternate 20 times between PSF (encoding == 3, flags == 128) and TLV-4 embedded MIF
    //    (encoding == 1, legacy bitmask flags == 0x92 / 0x02) across Slots 0..5 and 9..13:
    //    awdlTXWidth MUST remain 80 MHz with ZERO TLC_MNG_CONFIG_CMD resets!
    for (unsigned step = 0; step < 20; ++step) {
        nowUS += 12000;
        const unsigned slot = (step < 10) ? (step % 6) : (9 + (step % 5));
        p.sync.windowCounter = uint16_t(slot * 4);
        p.sync.nextWindowTU = 48;
        p.lastSeenUS = p.syncReceivedUS = p.sequenceReceivedUS = p.htCapabilitiesReceivedUS = nowUS;
        if (step % 2 == 0) {
            p.sequence.encoding = 1;
            for (unsigned s = 0; s < 16; ++s)
                p.sequence.flags[s] = sidecarCh[s] == 149 ? 0x92 : (sidecarCh[s] == 48 ? 0x94 : 0x8a);
        } else {
            p.sequence.encoding = 3;
            const uint8_t cls = uint8_t(124 + (step % 7)); // 124..130
            for (unsigned s = 0; s < 16; ++s)
                p.sequence.flags[s] = sidecarCh[s] == 149 ? cls : 0;
        }
        end = nowUS + 30000;
        assert(d.awdlTXWidth(nowUS) == 80);
        assert(!d.setAWDLTXWindow(grant, &end));
        assert(d.awdlRadio.blockAckTX.widthMHz == 80 && !d.awdlRadio.blockAckTX.paused);
        assert(d.awdlRadio.blockAckTXStats.tlcConfigs == tlcInitial);
    }

    // 2. Isolated 1-slot RTG [48,48,149,48x7,149,48x5] with encoding == 1 MUST still stay 20 MHz,
    //    and a peer without HTCapSGI40 on multi-slot encoding == 1 MUST also stay 20 MHz:
    const uint8_t rtgCh[16] = {48,48,149,48,48,48,48,48,48,48,149,48,48,48,48,48};
    p.sequence.encoding = 1;
    for (unsigned s = 0; s < 16; ++s) {
        p.sequence.channel[s] = rtgCh[s];
        p.sequence.flags[s] = 0x92;
    }
    p.sync.windowCounter = 8; // Slot 2 (isolated ch 149)
    p.syncReceivedUS = p.sequenceReceivedUS = nowUS;
    assert(d.awdlTXWidth(nowUS) == 20);

    for (unsigned s = 0; s < 16; ++s) p.sequence.channel[s] = sidecarCh[s];
    p.sync.windowCounter = 4;
    p.metadata.htCapabilities = AirportAWDL::HTCapSGI20;
    assert(d.awdlTXWidth(nowUS) == 20);

    assert(!d.clearAWDLBlockAck());
    return 0;
}
"""
        self.compile_run(p91._build_ba_harness(extra_ba))

        # Part B: Non-LLW Sidecar listener run + SyncEngine/Peer mid-run parent MIF refresh MUST NOT
        # pause TXBlockAck, flush StationID 3, or reset sinceChannelStartUS below guardUS!
        extra_radio = r"""
int main() {
    ItlIwx d;
    for (unsigned c : {6U, 48U, 149U, 153U, 157U, 161U}) {
        const unsigned band = c == 6 ? IEEE80211_CHAN_2GHZ : (IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_VHT80);
        d.com.sc_ic.ic_channels[c] = {band, uint16_t(c == 6 ? 2437 : 5000 + 5 * c)};
        d.com.sc_ic.ic_chan_active[c / 8] |= 1U << (c % 8);
    }
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    assert(!d.startAWDLRadio(self));

    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 9};
    const uint8_t sidecarSeq[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    AirportAWDL::SyncEngine sync = {};
    sync.start(self, 149, clockUS);
    for (unsigned s = 0; s < 16; ++s) {
        sync.channels[s] = sidecarSeq[s];
        sync.channelClasses[s] = sidecarSeq[s] == 149 ? 128 : 0;
    }

    auto &peer = d.awdlPeers.peers[0];
    peer = {};
    peer.occupied = true;
    AirportAWDL::copyAddress(peer.address, peerAddr);
    peer.haveSync = peer.haveSequence = true;
    peer.sync.presenceMode = 4;
    peer.sync.windowPeriodTU = 16;
    peer.sync.nextWindowTU = 64;
    peer.sync.windowCounter = 0;
    peer.sequence.count = 16;
    peer.sequence.fill = 0xffff;
    peer.sequence.step = 3;
    peer.sequence.encoding = 1;
    for (unsigned s = 0; s < 16; ++s) {
        peer.sequence.channel[s] = sidecarSeq[s];
        peer.sequence.flags[s] = 0x92;
    }
    peer.lastSeenUS = peer.syncReceivedUS = peer.sequenceReceivedUS = clockUS;

    // 1. At +500 us into Slot 0 (actual channel-run entry from Slot 15 ch 48 -> Slot 0 ch 149),
    //    3 ms startup guard MUST still hold:
    clockUS += 500;
    assert(!sync.canSendTo(peer, clockUS, false, true));

    // 2. Open non-LLW Sidecar listener run at +4000 us in Slot 0 with active TXBA + in-flight frame:
    clockUS += 3500;
    AirportAWDL::Window w0 = {};
    assert(sync.window(clockUS, w0));
    RadioState::GrantPolicy policy = {};
    policy.llwRealtime = false; // Sidecar!
    policy.widthMHz = 80;
    policy.slot = w0.runStartEAW;
    assert(!d.serviceAWDLChannel(149, clockUS + w0.untilChannelChangeUS, policy));

    auto &tx = d.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.epoch = d.dmaEpoch;
    tx.phase = AirportAWDL::TXBlockAckQueue::Agreed;
    AirportAWDL::copyAddress(tx.peer, peerAddr);
    tx.enabled = true;
    tx.paused = false;
    tx.widthMHz = 80;

    auto inFlight = packet();
    assert(!d.transmitAWDL(&inFlight, 3001));
    assert(d.com.txq[7].queued == 1);
    const auto startsBefore = d.awdlRadio.dwellStarts;
    const auto endsBefore = d.awdlRadio.dwellEnds;

    // 3. Advance into Slot 3 (+500 us into Slot 3 of [149x6]) where parent iPad sends an OOB MIF,
    //    re-anchoring BOTH sync.epochUS and peer.syncReceivedUS to the start of Slot 3 (elapsed == 500 us!):
    clockUS = (clockUS - 4000) + 3ULL * 65536 + 500;
    sync.epochCounter = 12; // Slot 3
    sync.epochUS = clockUS - 500;
    peer.sync.windowCounter = 12;
    peer.sync.nextWindowTU = 64;
    peer.lastSeenUS = peer.syncReceivedUS = peer.sequenceReceivedUS = clockUS - 500;

    AirportAWDL::Window w3 = {};
    assert(sync.window(clockUS, w3));
    assert(w3.sinceChannelStartUS == 3ULL * 65536 + 500);
    assert(sync.canSendTo(peer, clockUS, false, true)); // MUST NOT stall behind false 3 ms guard!

    policy.slot = w3.runStartEAW; // Relabeled from 0 -> 3 by parent clock refresh!
    assert(!d.serviceAWDLChannel(149, clockUS + w3.untilChannelChangeUS, policy));
    assert(!tx.paused); // Sidecar TXBA MUST NOT be paused mid-run!
    assert(commandCount(d, "flush") == 0 && !inFlight.frees); // In-flight frame MUST NOT be flushed!
    assert(d.awdlRadio.dwellStarts == startsBefore && d.awdlRadio.dwellEnds == endsBefore);
    assert(d.awdlRadio.llwWindowContinuations == 1);

    assert(!d.stopAWDLRadio());
    return 0;
}
"""
        self.compile_run(p91._build_radio_harness(extra_radio))

    def test_p99_2ghz_channel6_bt_coex_isolates_non_shared_antenna_without_profile_notifications(self):
        extra_ba = r"""
int main() {
    for (unsigned version : {3U, 4U}) {
        ItlIwx d;
        auto r = txPeer(d);
        d.tlcVersion = version;
        d.awdlRadio.channel = 149;
        d.awdlRadio.widthMHz = 80;
        d.mimo = true; d.txAnt = 3; d.rxAnt = 3;
        d.com.config.non_shared_ant = 2; // IWX_ANT_B dedicated Wi-Fi; Antenna A shared with CNVi BT
        d.btCoex.enabled = true;
        d.btCoex.txMask = 3;
        d.btCoex.notifications = 0;
        d.btCoex.activity = 0; // Live QuZ state when en1 is on 5 GHz (bt_primary_ci == 0)!

        auto &p = d.awdlPeers.peers[0];
        p.metadata.fields |= AirportAWDL::HTCapabilities;
        p.metadata.rxHTMcs = 0xff;
        p.metadata.rxHTMcs2 = 0xff;
        p.metadata.htCapabilities = AirportAWDL::HTCapLDPC | AirportAWDL::HTCapSGI20 | AirportAWDL::HTCapSGI40;
        p.htCapabilitiesReceivedUS = nowUS;
        p.haveSync = p.haveSequence = true;
        p.syncReceivedUS = p.sequenceReceivedUS = nowUS;
        p.sync.presenceMode = 4; p.sync.windowPeriodTU = 16; p.sync.nextWindowTU = 64;
        p.sequence.count = 16; p.sequence.fill = 0xffff; p.sequence.step = 3; p.sequence.encoding = 3;
        for (unsigned n = 0; n < 16; ++n) { p.sequence.channel[n] = 149; p.sequence.flags[n] = 128; }

        // 1. On 5 GHz 149@80, full 2x2 MIMO (chains == 3, 0x03ff on NSS 1 & NSS 2) MUST be used:
        prepareTX(d, r);
        agreeTX(d);
        auto &tx = d.awdlRadio.blockAckTX;
        assert(tx.antenna == 3 && tx.widthMHz == 80);

        auto checkTLC = [&](uint8_t expChains, uint8_t expMode, uint16_t expNss1, uint16_t expNss2) {
            if (version == 4) {
                const auto &cfg = *reinterpret_cast<const iwx_tlc_config_cmd_v4 *>(d.tlcCommands.back().data());
                assert(cfg.chains == expChains && cfg.mode == expMode);
                assert(cfg.ht_rates[0][0] == expNss1 && cfg.ht_rates[1][0] == expNss2);
            } else {
                const auto &cfg = *reinterpret_cast<const iwl_tlc_config_cmd_v3 *>(d.tlcCommands.back().data());
                assert(cfg.chains == expChains && cfg.mode == expMode);
                assert(cfg.ht_rates[0][0] == expNss1 && cfg.ht_rates[1][0] == expNss2);
            }
        };
        checkTLC(3, IWX_TLC_MNG_MODE_VHT, 0x03ff, 0x03ff);

        // 2. Pre-PHY downgrade 149@80 -> 6@20 MUST immediately isolate TLC onto dedicated non-shared
        //    Antenna B (chains == 2, NSS 2 == 0) BEFORE awdlPHY(6, 20), even with btCoex.activity == 0!
        assert(!d.downgradeAWDLTXRates(20, 6));
        assert(tx.widthMHz == 20 && tx.antenna == 2 && tx.mcs2 == 0);
        checkTLC(2, IWX_TLC_MNG_MODE_HT, 0x00ff, 0);

        // 3. Opening TX window on Channel 6 keeps chains == 2 without firing a redundant TLC command:
        d.awdlRadio.channel = 6;
        d.awdlRadio.widthMHz = 20;
        for (unsigned n = 0; n < 16; ++n) { p.sequence.channel[n] = 6; p.sequence.flags[n] = 81; }
        assert(d.awdlTXAntenna() == 2);
        const size_t tlcAfter6 = d.tlcCommands.size();
        uint64_t end = nowUS + 30000;
        assert(!d.setAWDLTXWindow(grant, &end) && !tx.paused);
        assert(tx.antenna == 2 && d.tlcCommands.size() == tlcAfter6);

        // 4. Returning to 5 GHz 149@80 restores full 2x2 MIMO (chains == 3, 0x03ff on NSS 1 & NSS 2):
        assert(!d.closeAWDLListenerWindow() && tx.paused);
        d.awdlRadio.channel = 149;
        d.awdlRadio.widthMHz = 80;
        for (unsigned n = 0; n < 16; ++n) { p.sequence.channel[n] = 149; p.sequence.flags[n] = 128; }
        assert(d.awdlTXAntenna() == 3);
        assert(!d.setAWDLTXWindow(grant, &end) && !tx.paused);
        assert(tx.antenna == 3 && tx.widthMHz == 80 && tx.mcs2 == 0xf0);
        checkTLC(3, IWX_TLC_MNG_MODE_VHT, 0x03ff, 0x03ff);

        assert(!d.clearAWDLBlockAck());
    }
    return 0;
}
"""
        self.compile_run(p91._build_ba_harness(extra_ba))

    def test_p99_multi_peer_eagain_preserves_per_peer_fifo_without_blocking_other_peers_or_urgent_frames(self):
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        extra_routines = "\n".join(
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
            for n in ("drainPendingData", "enqueueLLWFrame")
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines() + "\n" + extra_routines)
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src = tick_src.replace(
            "static void txReady(void *);",
            "static void txReady(void *);\n    IOReturn enqueueLLWFrame(mbuf_t);",
        )
        tick_src = tick_src.replace(
            "if (transmitError) {",
            "if (token && eagainPeer[0] && packet->bytes[0] != 0xd0 && "
            "sameAddress(packet->bytes.data() + 4, eagainPeer) && "
            "!(packet->bytes[0] == 0x88 && (packet->bytes[24] & 0x0f) == 7)) "
            "{ mbuf_freem(packet); if (onReject) onReject(token); return EAGAIN; }\n"
            "        if (transmitError) {",
        )
        tick_src = tick_src.replace(
            "int transmitError=0;",
            "int transmitError=0; uint8_t eagainPeer[6]={};",
        )
        tick_src += r"""
static mbuf_t taggedFrame(const uint8_t *src, const uint8_t *dst, uint8_t tag) {
    auto *p = ethernet(src, dst);
    p->bytes.back() = tag;
    return p;
}

int main() {
    const uint8_t peerA[6] = {2, 0, 0, 0, 0, 9};
    const uint8_t peerB[6] = {2, 0, 0, 0, 0, 10};

    for (bool viaTick : {false, true}) {
        clockUS = 10000000;
        AirportItlwmAWDLInterface i;
        dataPeer(i, peerA);
        dataPeer(i, peerB);
        i.device.acknowledgeData = true;
        i.device.loop.gated = true;
        i.device.awdlRadio.listener = true;
        i.device.awdlRadio.ready = true;
        i.device.awdlRadio.channel = 44;
        i.device.awdlRadio.widthMHz = 20;
        i.device.awdlRadio.dwell.running = true;
        i.device.awdlRadio.dwell.windowEndUS = clockUS + 60000;
        i.device.awdlRadio.queue = 1;
        i.device.awdlRadio.llwQueueConfigured = true;
        i.device.awdlRadio.llwQueue = 3;
        i.running = true;
        i.llwActive = true;
        i.llwParamsEnabled = true;
        i.nextActionUS = i.nextMIFUS = clockUS + 5000000;
        for (unsigned s = 0; s < 16; ++s) i.sync.channels[s] = 44;
        for (const uint8_t *addr : {peerA, peerB}) {
            auto *p = i.device.awdlPeers.find(addr, clockUS);
            assert(p);
            for (unsigned s = 0; s < 16; ++s) p->sequence.channel[s] = 44;
            p->sync.nextWindowTU = 64;
            p->syncReceivedUS = clockUS;
        }
        i.sync.epochCounter = 0;
        i.sync.epochUS = clockUS;
        clockUS += 5000;

        // Peer A returns EAGAIN in transmitAWDL (simulating Retiring/Enabling or full TXBA window):
        copyAddress(i.device.eagainPeer, peerA);
        i.enqueue(taggedFrame(i.address.octet, peerA, 1));
        i.enqueue(taggedFrame(i.address.octet, peerA, 2));
        i.enqueue(taggedFrame(i.address.octet, peerB, 10));
        i.enqueue(taggedFrame(i.address.octet, peerA, 3));
        i.enqueue(taggedFrame(i.address.octet, peerB, 20));
        i.enqueue(taggedFrame(i.address.octet, peerA, 4));

        if (viaTick) i.tick();
        else i.drainPendingData();

        // Peer B's frames [10, 20] MUST transmit immediately in order without HOL-blocking behind Peer A!
        assert(i.device.dataFrames.size() == 2);
        assert(i.device.dataFrames[0].back() == 10);
        assert(i.device.dataFrames[1].back() == 20);
        assert(i.pendingCount == 4);

        // Urgent llw0 frame to Peer A (QoS TID 7 on llwQueue) ALSO bypasses Peer A's data EAGAIN!
        assert(i.enqueueLLWFrame(taggedFrame(i.address.octet, peerA, 99)) == kIOReturnSuccess);
        assert(i.device.dataFrames.size() == 3);
        assert(i.device.dataFrames[2].back() == 99);

        // Once Peer A's EAGAIN clears, Peer A's frames MUST transmit in exact [1, 2, 3, 4] FIFO order!
        std::memset(i.device.eagainPeer, 0, 6);
        if (viaTick) i.tick();
        else i.drainPendingData();

        assert(i.pendingCount == 0);
        assert(i.device.dataFrames.size() == 7);
        assert(i.device.dataFrames[3].back() == 1);
        assert(i.device.dataFrames[4].back() == 2);
        assert(i.device.dataFrames[5].back() == 3);
        assert(i.device.dataFrames[6].back() == 4);
        i.drain();
    }

    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)


if __name__ == "__main__":
    unittest.main()
