"""Failing-first regressions for Preview 92:
1. Schedule-aware 2-slot infra parking (MaxParkWindowUS = 140 ms, gated on !policy.llwRealtime)
   plus zero-overhead 5 GHz retunes: Sidecar 2-slot [48, 48] (131.072 ms) windows park PHY 1 at
   149@80 so Intel QuZ TLC scales to 433-866 Mbps instead of resetting to 26 Mbps every 327 ms,
   adjacent [6] tunes to 6@20 so cumulative off-air span never reaches 196.6 ms, RealTimeGroup /
   llwRealtime sessions never park ch 48, and 48@20 <-> 149@80 retunes skip redundant MAC_CONTEXT
   MODIFY RPCs when band and promiscuous state are unchanged.
2. Contiguous 20 MHz VHT/HT TLC rate masks and mode tracking: 5 GHz 20 MHz uses VHT mode with
   MCS 0..8 (0x01ff), 2.4 GHz 20 MHz uses HT mode with MCS 0..7 (0x00ff), 80 MHz uses VHT mode
   with MCS 0..9 (0x03ff), and setAWDLTXWindow / downgradeAWDLTXRates track tlcMode across
   5 GHz <-> 2.4 GHz transitions without redundant reloads on same-band 20 MHz slots.
3. Active-TXBA 384-packet staging ring (TXBAQueueCapacity = 384 while preserving non-BA
   DataQueueCapacity = 96) and llwParamsEnabled headroom gating: Sidecar absorbs full 131 ms
   parked bursts without kIOReturnNoResources / VTP_Send ENOBUFS drops, idle-attached llw0 no
   longer steals 16 awdl0 slots when llwParamsEnabled is false, and Agreed -> Retiring shrink
   handles >96 queued packets safely without unsigned underflow or ring wrap corruption.
4. Low-latency 15 ms RX reorder gap timeout on AWDL TID 7 (LLWTID) (keeping 40 ms on TID 0 and
   100 ms on en1) plus ceiling-aligned tick() channel-switch timer so RealTimeGroup 48 <-> 149
   slot boundaries neither double-wake/overshoot nor stall Continuity Camera RTP+FEC past playout.
"""
import re
import unittest

import test_awdl_preview82 as p82
import test_awdl_preview91 as p91
import test_regressions as support
from test_offline import definition


class Preview92Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p92_two_slot_sidecar_park_preserves_80mhz_tlc_while_llw_realtime_and_long_windows_tune(self):
        extra = r"""
static void preview92ScheduleAwareParkAndFast5GHzRetune() {
    ItlIwx d;
    for (unsigned c : {6U, 36U, 40U, 44U, 48U, 149U, 153U, 157U, 161U}) {
        const unsigned band = c == 6 ? IEEE80211_CHAN_2GHZ : (IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_VHT80);
        d.com.sc_ic.ic_channels[c] = {band, uint16_t(c == 6 ? 2437 : 5000 + 5 * c)};
        d.com.sc_ic.ic_chan_active[c / 8] |= 1U << (c % 8);
    }
    d.rlcVersion = 2;
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    d.com.sc_phyctxt[0].channel = &d.com.sc_ic.ic_channels[48]; // en1 on ch 48
    assert(!d.startAWDLRadio(self));
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 9};
    auto &peer = d.awdlPeers.peers[0];
    peer = {};
    peer.occupied = true;
    AirportAWDL::copyAddress(peer.address, peerAddr);
    peer.haveSync = peer.haveSequence = true;
    peer.metadata.fields = AirportAWDL::HTCapabilities;
    peer.metadata.rxHTMcs = 0xff;
    auto refreshPeer = [&](uint64_t t) {
        peer.lastSeenUS = peer.syncReceivedUS = peer.sequenceReceivedUS = peer.htCapabilitiesReceivedUS = t;
    };
    refreshPeer(clockUS);

    auto &tx = d.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.epoch = d.dmaEpoch;
    tx.phase = AirportAWDL::TXBlockAckQueue::Agreed;
    AirportAWDL::copyAddress(tx.peer, peerAddr);
    tx.acknowledged = 64;
    tx.enabled = true;
    tx.paused = false;
    tx.widthMHz = 80;
    tx.lastHeavyDataUS = clockUS;
    d.awdlRadio.lastWideSocialVisitUS = clockUS;

    RadioState::GrantPolicy p80 = {}; p80.widthMHz = 80; p80.slot = 0;
    RadioState::GrantPolicy p20 = {}; p20.widthMHz = 20; p20.slot = 6; p20.bridgeInfraToWide = true;
    assert(!d.serviceAWDLChannel(149, clockUS + 393216, p80));
    assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

    // 1. Live Sequoia Sidecar 2-slot [48, 48] bridge excursion (131072 us <= 140000 us) while !llwRealtime
    //    MUST park PHY 1 at 149@80 without wiping TLC or issuing PHY retunes!
    const size_t phyBeforePark = d.phyCommands.size();
    assert(!d.serviceAWDLChannel(48, clockUS + 131072, p20));
    assert(!d.awdlRadio.dwell.running && tx.paused);
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80 && tx.widthMHz == 80);
    assert(d.phyCommands.size() == phyBeforePark);
    assert(d.awdlRadio.blockAckTXStats.infraParks == 1);

    // 2. Immediately adjacent Slot 8 [6] MUST ALWAYS tune to 6@20!
    clockUS += 131072;
    refreshPeer(clockUS);
    tx.lastHeavyDataUS = clockUS;
    p20.slot = 8;
    assert(!d.serviceAWDLChannel(6, clockUS + 65536, p20));
    assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 6 && d.awdlRadio.widthMHz == 20);

    // 3. Return to 149@80, then verify:
    //    (a) 3-slot [48, 48, 48] (196608 us > 140000 us) tunes to 48@20, AND
    //    (b) llwRealtime = true tunes to 48@20 even on a 2-slot (131072 us) window!
    clockUS += 65536;
    refreshPeer(clockUS);
    tx.lastHeavyDataUS = clockUS;
    tx.widthMHz = 80;
    p80.slot = 9;
    assert(!d.serviceAWDLChannel(149, clockUS + 327680, p80));
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

    p20.slot = 14;
    assert(!d.serviceAWDLChannel(48, clockUS + 196608, p20));
    assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 48 && d.awdlRadio.widthMHz == 20);

    clockUS += 196608;
    refreshPeer(clockUS);
    tx.lastHeavyDataUS = clockUS;
    tx.widthMHz = 80;
    p80.slot = 1;
    assert(!d.serviceAWDLChannel(149, clockUS + 131072, p80));
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

    RadioState::GrantPolicy pLLW = {};
    pLLW.widthMHz = 20;
    pLLW.slot = 3;
    pLLW.llwRealtime = true;
    pLLW.bridgeInfraToWide = true;
    const uint64_t parksBeforeLLW = d.awdlRadio.blockAckTXStats.infraParks;
    assert(!d.serviceAWDLChannel(48, clockUS + 131072, pLLW));
    assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 48 && d.awdlRadio.widthMHz == 20);
    assert(d.awdlRadio.blockAckTXStats.infraParks == parksBeforeLLW);

    // 4. With active RXBA (listenerPromisc == false on both 48 and 149), 5 GHz <-> 5 GHz
    //    48@20 <-> 149@80 RealTimeGroup retunes MUST skip redundant MAC_CONTEXT MODIFY RPCs,
    //    while 149 <-> 6 (2.4 GHz cck_rates change) MUST still modify MAC1!
    d.rxBA = true;
    assert(d.awdlHasRXBA());
    assert(!d.awdlRadio.listenerPromisc && d.awdlRadio.listenerFilterKnown);

    const size_t macBefore5G = d.macCommands.size();
    clockUS += 65536;
    p80.slot = 10;
    assert(!d.serviceAWDLChannel(149, clockUS + 65536, p80));
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);
    assert(d.macCommands.size() == macBefore5G);

    clockUS += 65536;
    pLLW.slot = 11;
    assert(!d.serviceAWDLChannel(48, clockUS + 327680, pLLW));
    assert(d.awdlRadio.channel == 48 && d.awdlRadio.widthMHz == 20);
    assert(d.macCommands.size() == macBefore5G);

    // Switching to 2.4 GHz ch 6 changes cck_rates -> MUST issue MAC MODIFY.
    clockUS += 65536;
    p20.slot = 12;
    assert(!d.serviceAWDLChannel(6, clockUS + 65536, p20));
    assert(d.awdlRadio.channel == 6 && d.macCommands.size() == macBefore5G + 1);
    assert(d.macCommands.back().cck_rates == 0x0f);

    d.rxBA = false;
    assert(!d.stopAWDLRadio());
}

int main() {
    preview92ScheduleAwareParkAndFast5GHzRetune();
    return 0;
}
"""
        harness = p91._build_radio_harness(extra).replace(
            "bool awdlHasRXBA() const { return false; }",
            "bool rxBA = false; bool awdlHasRXBA() const { return rxBA; }",
        )
        self.compile_run(harness)

    def test_p92_20mhz_vht_5ghz_and_ht_2ghz_contiguous_tlc_masks_and_mode_tracking(self):
        extra = r"""
int main() {
    for (unsigned version : {3U, 4U}) {
        ItlIwx d;
        auto r = txPeer(d);
        d.tlcVersion = version;
        d.awdlRadio.channel = 149;
        d.awdlRadio.widthMHz = 80;
        d.mimo = true; d.txAnt = 3; d.rxAnt = 3;
        auto &p = d.awdlPeers.peers[0];
        p.metadata.rxHTMcs = 0xff;
        p.metadata.rxHTMcs2 = 0xff;
        p.metadata.htCapabilities = 0x006f; // LDPC | SGI20 | SGI40
        p.htCapabilitiesReceivedUS = nowUS;
        p.haveSync = p.haveSequence = true;
        p.syncReceivedUS = p.sequenceReceivedUS = nowUS;
        p.sync.presenceMode = 4; p.sync.windowPeriodTU = 16; p.sync.nextWindowTU = 64;
        p.sequence.count = 16; p.sequence.fill = 0xffff; p.sequence.step = 3; p.sequence.encoding = 3;
        for (unsigned n = 0; n < 16; ++n) { p.sequence.channel[n] = 149; p.sequence.flags[n] = 128; }

        // 1. 149@80 -> VHT mode, 0x03ff (MCS 0..9).
        prepareTX(d, r);
        agreeTX(d);
        auto &tx = d.awdlRadio.blockAckTX;
        assert(tx.widthMHz == 80 && tx.tlcMode == IWX_TLC_MNG_MODE_VHT);

        // 2. Downgrade 149@80 -> 48@20 (5 GHz): MUST keep VHT mode with contiguous MCS 0..8 (0x01ff)
        //    so firmware TLC scales up to 173.3 Mbps instead of locking at 26 Mbps (0x00f0)!
        assert(!d.downgradeAWDLTXRates(20, 48));
        assert(tx.widthMHz == 20 && tx.tlcMode == IWX_TLC_MNG_MODE_VHT);
        const auto checkCmd = [&](uint8_t expMode, uint8_t expWidth, uint16_t expRate) {
            if (version == 4) {
                const auto &cfg = *reinterpret_cast<const iwx_tlc_config_cmd_v4 *>(d.tlcCommands.back().data());
                assert(cfg.mode == expMode && cfg.max_ch_width == expWidth);
                assert(cfg.ht_rates[0][0] == expRate && cfg.ht_rates[1][0] == expRate);
            } else {
                const auto &cfg = *reinterpret_cast<const iwl_tlc_config_cmd_v3 *>(d.tlcCommands.back().data());
                assert(cfg.mode == expMode && cfg.max_ch_width == expWidth);
                assert(cfg.ht_rates[0][0] == expRate && cfg.ht_rates[1][0] == expRate);
            }
        };
        checkCmd(IWX_TLC_MNG_MODE_VHT, IWX_TLC_MNG_CH_WIDTH_20MHZ, 0x01ff);

        // Opening TX window on 48@20 MUST NOT issue a duplicate TLC command!
        d.awdlRadio.channel = 48;
        d.awdlRadio.widthMHz = 20;
        for (unsigned n = 0; n < 16; ++n) { p.sequence.channel[n] = 48; p.sequence.flags[n] = 124; }
        const size_t tlcAfter48Downgrade = d.tlcCommands.size();
        uint64_t end = nowUS + 30000;
        assert(!d.setAWDLTXWindow(grant, &end) && !tx.paused);
        assert(d.tlcCommands.size() == tlcAfter48Downgrade);

        // 3. Switching 48@20 (5 GHz VHT) -> 6@20 (2.4 GHz HT): setAWDLTXWindow MUST detect
        //    tlcMode mismatch and configure HT mode with contiguous MCS 0..7 (0x00ff)!
        assert(!d.closeAWDLListenerWindow() && tx.paused);
        d.awdlRadio.channel = 6;
        for (unsigned n = 0; n < 16; ++n) { p.sequence.channel[n] = 6; p.sequence.flags[n] = 8; }
        assert(!d.setAWDLTXWindow(grant, &end) && !tx.paused);
        assert(d.tlcCommands.size() == tlcAfter48Downgrade + 1);
        assert(tx.tlcMode == IWX_TLC_MNG_MODE_HT);
        checkCmd(IWX_TLC_MNG_MODE_HT, IWX_TLC_MNG_CH_WIDTH_20MHZ, 0x00ff);

        // 4. Direct downgrade 149@80 -> 6@20 (while awdlRadio.channel is still 149!)
        //    MUST immediately select HT mode + 0x00ff for targetChannel=6!
        assert(!d.closeAWDLListenerWindow() && tx.paused);
        d.awdlRadio.channel = 149;
        d.awdlRadio.widthMHz = 80;
        for (unsigned n = 0; n < 16; ++n) { p.sequence.channel[n] = 149; p.sequence.flags[n] = 128; }
        assert(!d.setAWDLTXWindow(grant, &end) && !tx.paused);
        assert(tx.widthMHz == 80 && tx.tlcMode == IWX_TLC_MNG_MODE_VHT);
        checkCmd(IWX_TLC_MNG_MODE_VHT, IWX_TLC_MNG_CH_WIDTH_80MHZ, 0x03ff);

        assert(!d.closeAWDLListenerWindow() && tx.paused);
        assert(!d.downgradeAWDLTXRates(20, 6));
        assert(tx.widthMHz == 20 && tx.tlcMode == IWX_TLC_MNG_MODE_HT);
        checkCmd(IWX_TLC_MNG_MODE_HT, IWX_TLC_MNG_CH_WIDTH_20MHZ, 0x00ff);

        assert(!d.clearAWDLBlockAck());
    }
    return 0;
}
"""
        self.compile_run(p91._build_ba_harness(extra))

    def test_p92_active_txba_384_staging_ring_and_llw_params_headroom_gate(self):
        awdl_hpp = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()
        self.assertRegex(awdl_hpp, r"DataQueueCapacity\s*=\s*96\b")
        self.assertRegex(awdl_hpp, r"TXBAQueueCapacity\s*=\s*384\b")
        self.assertIn("pending[TXBAQueueCapacity]", awdl_hpp)
        self.assertIn("inFlight[TXBAQueueCapacity]", awdl_hpp)

        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        extra_routines = "\n".join(
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
            for n in ("submitOutputPackets", "drainPendingData", "txReady", "enqueueLLWFrame")
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines() + "\n" + extra_routines)
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src = tick_src.replace("static void txReady(void *);", "static void txReady(void *);\n    IOReturn enqueueLLWFrame(mbuf_t);")
        tick_src = tick_src.replace("assert(limit && limit<=32);", "assert(limit && limit<=64);")
        tick_src = tick_src.replace("} pending[64]={};", "} pending[384]={};")
        tick_src = tick_src.replace("inFlight[64]={};", "inFlight[384]={};")
        tick_src += r"""
int main() {
    clockUS = 10000000;
    const uint8_t peerAddr[] = {2, 0, 0, 0, 0, 9};
    AirportItlwmAWDLInterface i;
    dataPeer(i, peerAddr);
    i.nextActionUS = i.nextMIFUS = clockUS + 5000000;
    i.device.acknowledgeData = false;
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;

    // 1. Idle-attached llw0 since boot (llwActive == true, llwParamsEnabled == false)
    //    MUST NOT steal 16 slots from awdl0, AND non-BA mode MUST still cap at 96!
    i.llwActive = true;
    i.llwParamsEnabled = false;
    i.tick();
    assert(!i.device.lastPolicy.llwRealtime);
    for (unsigned n = 0; n < 120; ++n)
        i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    while (i.pullOutputPackets() == kIOReturnSuccess && !i.network.output.empty()) {}
    assert(i.pendingCount == 96);
    assert(i.network.output.size() == 24);
    for (auto *m : i.network.output) mbuf_freem(m);
    i.network.output.clear();
    i.drain();

    // 2. Active LLW session (llwActive == true, llwParamsEnabled == true):
    //    tick() sets policy.llwRealtime = true, and non-BA awdl0 reserves 16 slots (80) for llw0!
    i.llwParamsEnabled = true;
    i.tick();
    assert(i.device.lastPolicy.llwRealtime);
    for (unsigned n = 0; n < 96; ++n)
        i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    while (i.pullOutputPackets() == kIOReturnSuccess && !i.network.output.empty()) {}
    assert(i.pendingCount == 80);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.enqueueLLWFrame(ethernet(i.address.octet, peerAddr)) == kIOReturnSuccess);
    assert(i.pendingCount + i.inFlightCount == 96);
    for (auto *m : i.network.output) mbuf_freem(m);
    i.network.output.clear();
    i.drain();

    // 3. Agreed TXBA Sidecar session (llwActive == true, llwParamsEnabled == false):
    //    awdl0 stages up to 384 RTP frames across a 131 ms parked window with wrap-around,
    //    and Agreed -> Retiring shrink with >96 queued frames never underflows or leaks!
    i.llwParamsEnabled = false;
    i.pendingHead = 300; // Exercise wrap-around across 384 and >96!
    auto &tx = i.device.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Agreed;
    tx.epoch = i.device.dmaEpoch;
    copyAddress(tx.peer, peerAddr);
    for (unsigned n = 0; n < 400; ++n)
        i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    i.device.awdlRadio.dwell.running = false;
    const unsigned dequeuesBeforeBurst = i.network.dequeues;
    assert(i.submitOutputPackets(false) == kIOReturnSuccess);
    assert(i.network.dequeues == dequeuesBeforeBurst + 4);
    assert(i.pendingCount == 128 && i.inFlightCount == 0);
    assert(i.network.output.size() == 272);
    while (i.pullOutputPackets() == kIOReturnSuccess && !i.network.output.empty()) {}
    assert(i.pendingCount == 384);
    assert(i.network.output.size() == 16);

    // Transition Agreed -> Retiring while 384 (> 96) packets are queued:
    // pullOutputPackets / pullLLWOutputPackets MUST return kIOReturnNoResources without underflow!
    BSD llwNet;
    i.llwBSD = &llwNet;
    llwNet.output.push_back(ethernet(i.address.octet, peerAddr));
    tx.phase = TXBlockAckQueue::Retiring;
    assert(i.pullOutputPackets() == kIOReturnNoResources);
    assert(i.pullLLWOutputPackets() == kIOReturnNoResources);
    for (auto *m : llwNet.output) mbuf_freem(m);
    llwNet.output.clear();
    i.llwBSD = nullptr;

    for (auto *m : i.network.output) mbuf_freem(m);
    i.network.output.clear();
    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)

    def test_p92_llw_tid7_15ms_reorder_timeout_and_ceiling_channel_switch_timer(self):
        # Part A: AWDL TID 7 (Continuity Camera LLW) reorder gap expires in 15 ms (not 40 ms),
        # while AWDL TID 0 (AirDrop / Sidecar) preserves 40 ms reorder timeout.
        extra_ba = r"""
int main() {
    // TID 7 packets following the same gap must not postpone its 15 ms deadline.
    {
        ItlIwx d;
        auto r7 = request(d, 0, 7, 100);
        unsigned b7 = start(d, r7);
        auto &buffer = d.com.sc_rxba_data[b7].reorder_buf;
        auto p7 = packet(d, r7, 101, 71);
        auto p8 = packet(d, r7, 102, 72);
        auto p9 = packet(d, r7, 104, 74);
        receive(d, p7.get(), b7, 101, 100);
        const uint64_t firstDeadline = nowUS + 8000;
        assert(buffer.reorder_timer->deadline == firstDeadline);
        nowUS += 4000;
        receive(d, p8.get(), b7, 102, 100);
        receive(d, p9.get(), b7, 104, 100);
        assert(d.delivered.empty());
        assert(buffer.reorder_timer->deadline == firstDeadline);

        nowUS = firstDeadline;
        buffer.reorder_timer->deadline = 0;
        ItlIwx::iwx_reorder_timer_expired(&buffer);
        assert((d.delivered == std::vector<uint16_t>{71, 72}) && !p9->frees);
        assert(buffer.reorder_timer->deadline == firstDeadline + 4000);

        // The early wake on the second gap should retain only its remaining wait.
        nowUS += 1000;
        buffer.reorder_timer->deadline = 0;
        ItlIwx::iwx_reorder_timer_expired(&buffer);
        assert(!p9->frees && buffer.reorder_timer->deadline == firstDeadline + 4000);
        nowUS += 3000;
        buffer.reorder_timer->deadline = 0;
        ItlIwx::iwx_reorder_timer_expired(&buffer);
        assert((d.delivered == std::vector<uint16_t>{71, 72, 74}) && p9->frees);
        assert(!d.clearAWDLBlockAck());
    }

    // 2. TID 0 (DataTID): SN=100 lost, SN=101 buffered -> still waits full 40 ms!
    {
        ItlIwx d;
        auto r0 = request(d, 0, 0, 100);
        unsigned b0 = start(d, r0);
        auto p0 = packet(d, r0, 101, 11);
        receive(d, p0.get(), b0, 101, 100);
        assert(d.delivered.empty());

        nowUS += 35000; // 35 ms < 40 ms: must still wait on TID 0
        ItlIwx::iwx_reorder_timer_expired(&d.com.sc_rxba_data[b0].reorder_buf);
        assert(d.delivered.empty() && !p0->frees);

        nowUS += 6000; // 41 ms > 40 ms: now releases
        ItlIwx::iwx_reorder_timer_expired(&d.com.sc_rxba_data[b0].reorder_buf);
        assert((d.delivered == std::vector<uint16_t>{11}) && p0->frees);
        assert(!d.clearAWDLBlockAck());
    }
    return 0;
}
"""
        self.compile_run(p91._build_ba_harness(extra_ba))

        # Part B: When dwell closes at eawEndUS - 3000 with 2900 us remaining until the next
        # RealTimeGroup slot, tick() MUST arm ceiling setTimeoutMS(3) (not floor 2 ms).
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines())
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src = tick_src.replace(
            "if(!c) {awdlRadio.dwell.running=false;awdlRadio.queue=0xffff;return 0;}",
            "if(!c || endUS <= clockUS + 3000) {awdlRadio.dwell.running=false;return 0;}",
        )
        tick_src += r"""
int main() {
    clockUS = 10000000;
    AirportItlwmAWDLInterface i;
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.ready = true;
    i.running = true;
    for (unsigned s = 0; s < 16; ++s)
        i.sync.channels[s] = (s % 2 == 0) ? 44 : 149;

    // Advance clock to 2900 us before the end of the current 65536 us EAW slot
    // (inside the 3000 us listener close guard where dwell.running == false).
    Window w = {};
    assert(i.sync.window(clockUS, w));
    assert(w.untilChannelChangeUS > 2900);
    clockUS += w.untilChannelChangeUS - 2900;
    i.tick();
    assert(!i.device.awdlRadio.dwell.running);
    // Ceiling of 2900 us is 3 ms (waking 100 us INTO the new slot), NOT 2 ms (900 us early!).
    assert(i.scheduler.delay == 3);
    return 0;
}
"""
        self.compile_run(tick_src)


if __name__ == "__main__":
    unittest.main()
