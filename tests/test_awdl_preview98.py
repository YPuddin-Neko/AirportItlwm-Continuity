"""Failing-first regressions for Preview 98:
1. Latched LLWTID (TID 7) queue rejection on StationID 3 (IWX_STA_GENERAL_PURPOSE) plus
   unconditional entry.lowLatency urgent TX bypass: eliminates the 200 Hz IWX_SCD_QUEUE_CFG +
   deferRecycling() workloop/DMA storm during FaceTime Continuity Camera and lets llw0 RTCP
   NACK/FIR packets bypass psfDue/broadcastDue holds even when llwQueueConfigured is false.
2. Sequence-anchored multi-slot 80 MHz Sidecar awdlTXWidth (operating classes 115/118/124/125/128
   and encoding 0) plus post-pause deadline revalidation in setAWDLTXWindow: prevents Slot 5 and
   Slot 13 OOB MIF arrivals (behind == 0 && ahead == 0) and slot-end station-pause sleeps from
   resetting Intel QuZ TLC to 26 Mbps every superframe.
3. Active-media broadcast MIF lead/queue guard (12 ms lead, empty management queue), sub-ms
   wakeup protection (wakeArmed preserved for <= 1000 us setTimeoutUS), and 5 s IORegistry
   snapshot throttling during Continuity media: prevents untransmitted MIFs from forcing blocking
   IWX_TXPATH_FLUSH across 48 <-> 149 transitions and stops setTimeoutMS(1) from clobbering
   sub-millisecond slot-boundary wakeups.
4. Active llwRealtime 8 ms TID 7 RX reorder gap timeout plus reordered-delivery publish bypass:
   halves RTP gap detection latency during zero-FEC 30 fps Continuity Camera streaming so llw0
   RTP NACK recovers isolated 48 <-> 149 packet drops before VideoReceiver fires SendFIR.
"""
import unittest

import test_awdl_preview82 as p82
import test_awdl_preview91 as p91
import test_regressions as support
from test_offline import definition


class Preview98Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p98_llw_queue_rejection_latched_and_low_latency_tx_bypasses_announcements_without_llw_queue(self):
        # Part A: StationID 3 (IWX_STA_GENERAL_PURPOSE) rejecting TID 7 (LLWTID) MUST latch
        # llwQueueFailed and issue at most ONE SCD_QUEUE_CFG attempt per radio lifecycle,
        # while transient -ENOMEM retries cleanly and stopAWDLRadio/resetAWDLRadio clears the latch.
        extra_radio = r"""
int main() {
    ItlIwx d;
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    assert(!d.startAWDLRadio(self));

    RadioState::GrantPolicy pLLW = {};
    pLLW.llwQueue = true;
    pLLW.llwRealtime = true;
    pLLW.widthMHz = 20;
    pLLW.slot = 0;

    // 1. Transient -ENOMEM MUST NOT permanently latch failure:
    d.llwQueueReturn = -ENOMEM;
    assert(!d.serviceAWDLChannel(48, clockUS + 65536, pLLW));
    assert(d.llwQueueAttempts == 1 && !d.awdlRadio.llwQueueConfigured);

    // 2. Firmware rejection (-EIO on StationID 3 IWX_STA_GENERAL_PURPOSE) MUST latch llwQueueFailed
    //    so 20 subsequent 5 ms ticks and 48 <-> 149 hops issue ZERO additional SCD_QUEUE_CFG RPCs!
    d.llwQueueReturn = -EIO;
    for (unsigned step = 0; step < 20; ++step) {
        pLLW.slot = uint16_t(step % 4);
        const uint16_t ch = (step % 8 == 2) ? 149 : 48;
        assert(!d.serviceAWDLChannel(ch, clockUS + 65536, pLLW));
        clockUS += 5000;
    }
    assert(d.llwQueueAttempts == 2);
    assert(!d.awdlRadio.llwQueueConfigured);

    // 3. Restarting AWDL radio resets the latch cleanly:
    assert(!d.stopAWDLRadio());
    assert(!d.startAWDLRadio(self));
    assert(!d.serviceAWDLChannel(48, clockUS + 65536, pLLW));
    assert(d.llwQueueAttempts == 3);
    assert(!d.stopAWDLRadio());
    return 0;
}
"""
        radio_harness = p91._build_radio_harness(extra_radio).replace(
            "assert(stationPresent && station==3 && size==64 && (tid==IWX_MGMT_TID || tid==RadioState::DataTID));",
            "if (tid == RadioState::LLWTID) { ++llwQueueAttempts; result->error = llwQueueReturn < 0 ? -llwQueueReturn : 0; return llwQueueReturn; }\n"
            "        assert(stationPresent && station==3 && size==64 && (tid==IWX_MGMT_TID || tid==RadioState::DataTID));",
        ).replace(
            "unsigned rlcVersion=IWX_FW_CMD_VER_UNKNOWN,",
            "unsigned llwQueueAttempts=0; int llwQueueReturn=-EIO;\n    unsigned rlcVersion=IWX_FW_CMD_VER_UNKNOWN,",
        )
        self.compile_run(radio_harness)

        # Part B: Even when llwQueueConfigured == false (QuZ live state), entry.lowLatency (llw0 RTCP NACK/FIR)
        # MUST still bypass psfDue(now) and broadcastDue(now) announcement holds with the 1500 us guard!
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
        tick_src += r"""
int main() {
    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 9};
    clockUS = 10000000;
    AirportItlwmAWDLInterface i;
    dataPeer(i, peerAddr);
    i.device.acknowledgeData = true;
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.ready = true;
    i.device.awdlRadio.channel = 48;
    i.device.awdlRadio.widthMHz = 20;
    i.device.awdlRadio.dwell.running = true;
    i.device.awdlRadio.dwell.windowEndUS = clockUS + 60000;
    i.device.awdlRadio.queue = 1;
    i.device.awdlRadio.llwQueueConfigured = false; // Live QuZ state!
    i.running = true;
    i.llwActive = true;
    i.llwParamsEnabled = true;
    for (unsigned s = 0; s < 16; ++s) i.sync.channels[s] = 48;
    auto *peer = i.device.awdlPeers.find(peerAddr, clockUS);
    assert(peer);
    for (unsigned s = 0; s < 16; ++s) peer->sequence.channel[s] = 48;
    i.sync.epochCounter = 0;
    i.sync.epochUS = clockUS;
    peer->sync.nextWindowTU = 64;
    peer->syncReceivedUS = clockUS;

    // 1800 us into slot (1500 <= 1800 < 3000) WITH both psfDue and broadcastDue true, AND llwQueueConfigured == false!
    clockUS += 1800;
    i.nextActionUS = clockUS;
    i.nextMIFUS = clockUS;
    assert(i.psfDue(clockUS) && i.broadcastDue(clockUS));
    assert(i.enqueueLLWFrame(ethernet(i.address.octet, peerAddr)) == kIOReturnSuccess);
    assert(i.pendingCount == 0 && i.inFlightCount == 0 && i.device.data == 1);
    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)

    def test_p98_sidecar_multi_slot_80mhz_tlc_stays_pinned_across_slot5_slot13_oob_mifs_and_station_sleep(self):
        extra_ba = r"""
static uint64_t gWindowEnd = 0;
static uint64_t dynamicGrant(const void *, uint64_t now) {
    return now < gWindowEnd ? gWindowEnd : 0;
}

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

    // Live Sidecar schedule: [149x6, 48, 48, 6, 149x5, 48, 48]
    const uint8_t sidecarCh[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    for (unsigned s = 0; s < 16; ++s) {
        p.sequence.channel[s] = sidecarCh[s];
        p.sequence.flags[s] = sidecarCh[s] == 149 ? 124 : (sidecarCh[s] == 48 ? 115 : 81);
    }
    p.sync.windowCounter = 0;
    prepareTX(d, r);
    agreeTX(d);
    gWindowEnd = nowUS + 30000;
    assert(!d.setAWDLTXWindow(dynamicGrant, nullptr));
    assert(d.awdlRadio.blockAckTX.widthMHz == 80 && !d.awdlRadio.blockAckTX.paused);
    const uint64_t tlcInitial = d.awdlRadio.blockAckTXStats.tlcConfigs;
    assert(tlcInitial == 1);

    // 1. Peer sends OOB MIF at +28.8 ms inside Slot 5 (LAST slot of [149x6], ahead == 0)
    //    and inside Slot 13 (LAST slot of [149x5], ahead == 0), resetting syncReceivedUS = nowUS
    //    so elapsed < eawUS (behind == 0): awdlTXWidth MUST stay 80 MHz with ZERO TLC resets!
    for (uint8_t cls : {uint8_t(124), uint8_t(125)}) {
        for (unsigned s = 0; s < 16; ++s)
            p.sequence.flags[s] = sidecarCh[s] == 149 ? cls : (sidecarCh[s] == 48 ? 115 : 81);
        for (uint16_t slotCounter : {uint16_t(5 * 4), uint16_t(13 * 4)}) {
            nowUS += 28800;
            p.sync.windowCounter = slotCounter;
            p.sync.nextWindowTU = 36; // ~28.8 ms into EAW
            p.lastSeenUS = p.syncReceivedUS = p.sequenceReceivedUS = p.htCapabilitiesReceivedUS = nowUS;
            gWindowEnd = nowUS + 30000;
            assert(d.awdlTXWidth(nowUS) == 80);
            assert(!d.setAWDLTXWindow(dynamicGrant, nullptr));
            assert(d.awdlRadio.blockAckTX.widthMHz == 80 && !d.awdlRadio.blockAckTX.paused);
            assert(d.awdlRadio.blockAckTXStats.tlcConfigs == tlcInitial);
        }
    }

    // Compact encoding == 0 on multi-slot 149 with HTCapSGI40 also stays 80 MHz:
    p.sequence.encoding = 0;
    for (unsigned s = 0; s < 16; ++s) p.sequence.flags[s] = 0;
    assert(d.awdlTXWidth(nowUS) == 80);
    p.sequence.encoding = 3;
    for (unsigned s = 0; s < 16; ++s)
        p.sequence.flags[s] = sidecarCh[s] == 149 ? 124 : (sidecarCh[s] == 48 ? 115 : 81);

    // 2. Simulate configureAWDLTXStation(true, true) sleeping across the Slot 5 -> Slot 6 (149 -> 48)
    //    boundary when ratesChanged triggers near slot end: setAWDLTXWindow MUST recheck deadline()
    //    after configureAWDLTXStation and NOT fire a spurious 20 MHz TLC reset when deadline() == 0!
    d.awdlMinRateMbps = 24;
    gWindowEnd = nowUS + 500; // Positive before ADD_STA sleep, expired after ADD_STA sleep!
    d.onCommand = [&](int) {
        nowUS += 40000;
        d.onCommand = nullptr;
    };
    assert(!d.setAWDLTXWindow(dynamicGrant, nullptr));
    assert(d.awdlRadio.blockAckTX.paused);
    assert(d.awdlRadio.blockAckTX.widthMHz == 80);
    assert(d.awdlRadio.blockAckTXStats.tlcConfigs == tlcInitial);

    assert(!d.clearAWDLBlockAck());
    return 0;
}
"""
        self.compile_run(p91._build_ba_harness(extra_ba))

    def test_p98_media_active_announcement_lead_subms_wakeup_guard_and_snapshot_throttling(self):
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
            "if(!c) {awdlRadio.dwell.running=false;awdlRadio.queue=0xffff;return 0;}",
            "const uint64_t lead = policy.llwRealtime ? 1500ULL : 3000ULL;\n"
            "        if(!c || endUS <= clockUS + lead) {awdlRadio.dwell.running=false;return 0;}",
        )
        tick_src = tick_src.replace(
            "awdlRadio.dwell.windowEndUS=endUS;",
            "awdlRadio.dwell.windowEndUS=endUS - (policy.llwRealtime ? 1500ULL : 3000ULL);",
        )
        tick_src += r"""
int main() {
    // 1. Active media announcement lead & busy-queue guard:
    //    During active LLW media, periodic MIFs MUST NOT submit when < 12 ms remain before dwell.windowEndUS
    //    or when com.txq[awdlRadio.queue].queued > 0 (preventing slot-end IWX_TXPATH_FLUSH stalls),
    //    while submitting normally when >= 12 ms remain and queue is empty!
    {
        clockUS = 10000000;
        AirportItlwmAWDLInterface i;
        i.device.loop.gated = true;
        i.device.awdlRadio.listener = true;
        i.device.awdlRadio.ready = true;
        i.running = true;
        i.llwActive = true;
        i.llwParamsEnabled = true;
        for (unsigned s = 0; s < 16; ++s) i.sync.channels[s] = (s % 2 == 0) ? 48 : 149;
        auto payload = umiPayload();
        auto req = broadcastRequest(payload, 0xffffffff, 0);
        assert(i.configureBroadcast(req, true) == kIOReturnSuccess);

        // Position 8 ms before slot change (< 12 ms media lead before dwell.windowEndUS): periodic MIF skipped!
        Window w = {};
        assert(i.sync.window(clockUS, w));
        clockUS += w.untilChannelChangeUS - 8000;
        i.tick();
        assert(i.broadcastSubmitted == 0);

        // Next slot at start (> 50 ms remaining), but management queue already has 1 uncompleted frame: skipped!
        clockUS += 10000;
        i.device.com.txq[i.device.awdlRadio.queue].queued = 1;
        i.tick();
        assert(i.broadcastSubmitted == 0);

        // Management queue drains with > 40 ms remaining: periodic MIF submits cleanly!
        i.device.com.txq[i.device.awdlRadio.queue].queued = 0;
        i.tick();
        assert(i.broadcastSubmitted == 1);

        // Sub-ms slot-boundary wakeup protection: arming <= 1000 us setTimeoutUS MUST keep wakeArmed == true
        // so pullOutputPackets() cannot clobber a 450 us slot-switch timer with setTimeoutMS(1)!
        assert(i.sync.window(clockUS, w));
        clockUS += w.untilChannelChangeUS - 1900; // windowEndUS = eawEnd - 1500 -> 400 us remaining -> delayUS = 450!
        i.tick();
        assert(i.scheduler.delayUS == 450);
        assert(i.wakeArmed);

        const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 9};
        i.network.output.push_back(ethernet(i.address.octet, peerAddr));
        assert(i.pullOutputPackets() == kIOReturnSuccess);
        assert(i.pendingCount == 1);
        assert(i.scheduler.delayUS == 450); // NOT overwritten by setTimeoutMS(1)!
        i.drain();
    }

    // 2. 1 Hz IORegistry snapshot storm throttled to 5 s during active Continuity media,
    //    while initial snapshot and idle 1 s cadence remain intact:
    {
        clockUS = 20000000;
        AirportItlwmAWDLInterface i;
        i.device.loop.gated = true;
        i.device.awdlRadio.listener = true;
        i.device.awdlRadio.ready = true;
        i.running = true;
        i.llwActive = true;
        i.llwParamsEnabled = true;
        for (unsigned s = 0; s < 16; ++s) i.sync.channels[s] = 48;

        // Initial snapshot (lastSnapshotUS == 0) publishes immediately:
        i.tick();
        assert(i.lastSnapshotUS == clockUS);

        // At +1.2 s and +3.5 s during active LLW media: throttled (< 5.0 s)!
        const uint64_t firstSnap = i.lastSnapshotUS;
        clockUS += 1200000;
        i.tick();
        assert(i.lastSnapshotUS == firstSnap);
        clockUS += 2300000;
        i.tick();
        assert(i.lastSnapshotUS == firstSnap);

        // At +5.1 s: refreshes!
        clockUS += 1600000;
        i.tick();
        assert(i.lastSnapshotUS == clockUS);

        // Once media stops (idle), 1.0 s cadence resumes immediately:
        i.llwParamsEnabled = false;
        const uint64_t secondSnap = i.lastSnapshotUS;
        clockUS += 1050000;
        i.tick();
        assert(i.lastSnapshotUS == clockUS && i.lastSnapshotUS > secondSnap);
    }

    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)

    def test_p98_llw_realtime_8ms_tid7_reorder_timeout_and_reordered_publish_bypass(self):
        extra_ba = r"""
int main() {
    // 1. Active llwRealtime == true -> TID 7 reorder gap expires in 8 ms (8000 us),
    //    while skipping publishAWDLReceive() inside deliverAWDLReordered()!
    {
        ItlIwx d;
        d.awdlRadio.llwRealtime = true;
        auto r7 = request(d, 0, 7, 200);
        unsigned b7 = start(d, r7);
        auto &buffer = d.com.sc_rxba_data[b7].reorder_buf;
        auto p1 = packet(d, r7, 201, 81);
        auto p2 = packet(d, r7, 202, 82);
        const unsigned pubBefore = d.published;
        receive(d, p1.get(), b7, 201, 200);
        const uint64_t firstDeadline = nowUS + 8000;
        assert(buffer.reorder_timer->deadline == firstDeadline);
        nowUS += 5000;
        receive(d, p2.get(), b7, 202, 200);
        assert(d.delivered.empty());
        assert(buffer.reorder_timer->deadline == firstDeadline);

        nowUS = firstDeadline;
        buffer.reorder_timer->deadline = 0;
        ItlIwx::iwx_reorder_timer_expired(&buffer);
        assert((d.delivered == std::vector<uint16_t>{81, 82}) && p1->frees && p2->frees);
        assert(d.published == pubBefore); // Reordered LLW delivery did not build registry dictionary!
        assert(!d.clearAWDLBlockAck());
    }

    // 2. Non-realtime (llwRealtime == false) preserves 8 ms TID 7 and 40 ms TID 0 contracts:
    {
        ItlIwx d;
        auto r7 = request(d, 0, 7, 300);
        unsigned b7 = start(d, r7);
        auto p1 = packet(d, r7, 301, 91);
        receive(d, p1.get(), b7, 301, 300);
        assert(d.com.sc_rxba_data[b7].reorder_buf.reorder_timer->deadline == nowUS + 8000);
        nowUS += 8000;
        ItlIwx::iwx_reorder_timer_expired(&d.com.sc_rxba_data[b7].reorder_buf);
        assert((d.delivered == std::vector<uint16_t>{91}) && p1->frees);
        assert(d.published > 0);
        assert(!d.clearAWDLBlockAck());
    }

    return 0;
}
"""
        ba_harness = p91._build_ba_harness(extra_ba).replace(
            "void publishAWDLReceive(){}",
            "unsigned published=0;void publishAWDLReceive(){++published;}",
        )
        self.compile_run(ba_harness)


if __name__ == "__main__":
    unittest.main()
