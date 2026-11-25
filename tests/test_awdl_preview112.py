"""Ground-truth regressions for Continuity Wi-Fi Preview 112 (2.4.0d112):
1. U1: Keep PHY 1 bound while associated on en1 in listener mode (eliminating 2.0s ADD_STA/REMOVE_STA
   rebind thrash and IO80211AWDLPeerManager::monitorAWEvents AVAILABILITY_WINDOW_START starvation /
   AirDropNW ETIMEDOUT Code=60) and unconditionally set parkIdleOnInfra = true on all 0-slots.
2. U2a: Broaden unicast PTP classification (0x88f7 L2 IEEE 1588 + IPv4/IPv6 UDP 319/320 including
   single IPv6 extension header), head-insert unicast PTP ahead of non-retry non-PTP pending[] while
   preserving PTP-to-PTP FIFO order, route unicast PTP over llwQueue (TID 7 VO QoS, falling back to
   dataQueue TID 0 QoS when QuZ rejects TID 7 on Station 3, NEVER non-QoS awdlRadio.queue at 12 Mbps)
   when peer blockAckTX is paused/full/not-Agreed while respecting Asking solicitDeadline, and keep
   400 us closeLeadUS / 500 us guardUS when <= 2 QoS descriptors are queued on Station 3.
3. U2b: Single-wakeup T - 400 us llwRealtime pre-tune across 48 <-> 149 RealTimeGroup slot boundaries
   when untilChannelChangeUS <= 500 us and Station 3 depth <= 2, while keeping updateAvailability()
   aligned to the active window channel/counter.
4. U2c: Unconditional AirportItlwmSkywalkInterface::abortScanForContinuityMedia() + setSCAN_CONTROL
   on macOS 15.8.1 (24H32) without __MAC_15_2 gating, invoked when llwParamsEnabled or TXBA Agreed.
5. U3: Once-per-packet outbound TXBA demand counting at enqueue (pullOutputPackets) so 1 stuck heavy
   packet never inflates demandFrames across 16 ticks.
6. C1-C4 Guard: Preserve pure 2.4.0d104 in-order hardware-deaggregated multi-subframe A-MSDU fast
   delivery (Subframe 0 delivered immediately with num_stored == 0 and zero 8 ms / 40 ms stall even
   if trailing subframe is lost) and forbid SESSION_PROTECTION_CMD / MAC_LOW_LATENCY_CMD on LISTENER.
"""
import unittest

import test_awdl_preview82 as p82
import test_awdl_preview91 as p91
import test_regressions as support
from test_offline import definition


def _tick_harness(main_body: str, extra_defs=()) -> str:
    tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
    tick_src = tick_src.replace(
        "// NATIVE TX SCENARIOS",
        (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
    )
    extra = "\n".join(
        definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
        for n in ("drainPendingData", *extra_defs)
    )
    tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines() + "\n" + extra)
    tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
    tick_src = tick_src.replace(
        "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {return width==20 && (c==6||c==44||(c==149&&allow149));}",
        "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {"
        "return (width==20 || (width==80 && c==149)) && (c==6||c==44||c==48||(c==149&&allow149));}",
    )
    tick_src = tick_src.replace(
        "awdlRadio.blockAckTX.demand(peer,bytes,clockUS,accepted);",
        "awdlRadio.blockAckTX.demand(peer,bytes,clockUS,accepted,!awdlRadio.blockAckTX.configured || !com.txq[awdlRadio.blockAckTX.queue].queued);",
    )
    helpers = r"""
static mbuf_t sizedIPv6UDP(const uint8_t *src, const uint8_t *dst, uint16_t dstPort, size_t len, uint8_t tag = 0) {
    auto p = awdlPacket(len);
    auto *b = p->bytes.data();
    std::memset(b, 0, len);
    copyAddress(b, dst);
    copyAddress(b + 6, src);
    b[12] = 0x86; b[13] = 0xdd;
    b[14] = 0x60;
    const uint16_t plen = uint16_t(len - 54);
    b[18] = uint8_t(plen >> 8); b[19] = uint8_t(plen);
    b[20] = 17; b[21] = 255;
    b[22] = 0xfe; b[23] = 0x80; b[37] = 1;
    b[38] = 0xfe; b[39] = 0x80; b[53] = 2;
    b[54] = uint8_t(dstPort >> 8); b[55] = uint8_t(dstPort);
    b[56] = uint8_t(dstPort >> 8); b[57] = uint8_t(dstPort);
    b[58] = uint8_t(plen >> 8); b[59] = uint8_t(plen);
    if (len > 62) b[len - 1] = tag;
    return p;
}

static mbuf_t l2PTPFrame(const uint8_t *src, const uint8_t *dst, size_t bodyLen = 44, uint8_t tag = 0) {
    const size_t len = 14 + bodyLen;
    auto p = awdlPacket(len);
    auto *b = p->bytes.data();
    std::memset(b, 0, len);
    copyAddress(b, dst);
    copyAddress(b + 6, src);
    b[12] = 0x88; b[13] = 0xf7;
    b[len - 1] = tag;
    return p;
}
"""
    return tick_src + helpers + main_body


class Preview112Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p112_u1_associated_idle_parks_0slots_on_infra_with_zero_monitor_aw_gap(self):
        tick_src = _tick_harness(
            r"""
int main() {
    clockUS = 10000000ULL;
    AirportItlwmAWDLInterface i;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.binding = true;
    i.device.com.ns_nstate = IEEE80211_S_RUN;

    unsigned unbindTicks = 0, parkInfraTicks = 0;
    i.device.onService = [&]() {
        if (i.device.lastPolicy.unbindIdle) ++unbindTicks;
        if (i.device.lastPolicy.parkIdleOnInfra) ++parkInfraTicks;
    };

    // Low Power schedule [6, 0, 0, 0, 149, 0, 0, 0, 6, 0, 0, 0, 149, 0, 0, 0]:
    // 4 social EAWs spaced 4 slots (262.144 ms) apart, 12 zero-slots per 16-slot cycle.
    const uint16_t lowPower[16] = {6, 0, 0, 0, 149, 0, 0, 0, 6, 0, 0, 0, 149, 0, 0, 0};
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = lowPower[s];
        i.sync.channelClasses[s] = 0;
    }
    i.sequenceConfigured = true;
    i.tick();
    assert(i.running);

    uint64_t lastStartUS = clockUS;
    uint64_t maxStartGapUS = 0;
    unsigned awStarts = 0;

    // Run 160 AWDL slots (~10.49 seconds) with ZERO peers and ZERO traffic while associated:
    for (unsigned step = 0; step < 160; ++step) {
        const size_t msgBefore = i.controller.messages.size();
        clockUS = i.sync.epochUS + uint64_t(step) * 65536ULL + 4000ULL;
        i.tick();
        for (size_t m = msgBefore; m < i.controller.messages.size(); ++m) {
            if (i.controller.messages[m].type == 41) { // APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START
                const uint64_t gap = clockUS - lastStartUS;
                if (gap > maxStartGapUS) maxStartGapUS = gap;
                lastStartUS = clockUS;
                ++awStarts;
            }
        }
    }

    // 1. ZERO idleUnbinds while associated on en1!
    assert(unbindTicks == 0);
    // 2. All 120 zero-slots (10 cycles * 12 zero-slots) parked co-channel on en1's infra channel!
    assert(parkInfraTicks == 120);
    // 3. Every scheduled social slot posted AVAILABILITY_WINDOW_START (1 initial + 39 in loop = 40) with max gap ~262 ms (<< 1,000 ms watchdog)!
    assert(awStarts == 39 && i.nativeWindowStarts == 40);
    assert(maxStartGapUS <= 300000ULL);

    i.drain();
    assert(!allocations);
    return 0;
}
""",
            extra_defs=("continuityMediaActive",),
        ).replace(
            "void tick();",
            "void tick();\n    bool continuityMediaActive() const;",
        ).replace(
            "Radio() {awdlRadio.widthMHz=20;}",
            "bool rxBA=false;\n    bool awdlHasRXBA() const {return rxBA;}\n    Radio() {awdlRadio.widthMHz=20;}",
        )
        self.compile_run(tick_src)

    def test_p112_u2a_unicast_ptp_head_fifo_qos_queue_routing_and_solicit_deadline_respect(self):
        # Part 1: Interface-level head-FIFO insertion, paused/Retiring TXBA bypass, and Asking solicitDeadline respect.
        tick_src = _tick_harness(r"""
int main() {
    clockUS = 10000000ULL;
    const uint8_t selfAddr[6] = {2, 0, 0, 0, 0, 1};
    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 0x22};
    const uint8_t mcastAddr[6] = {0x33, 0x33, 0, 0, 0, 0xfb};

    // A. Classifier checks (0x88f7, IPv4 UDP 319/320, IPv6 UDP 319/320 + Hop-by-Hop ext hdr):
    {
        uint8_t ipv6Ext[70] = {};
        copyAddress(ipv6Ext, peerAddr); copyAddress(ipv6Ext + 6, selfAddr);
        ipv6Ext[12] = 0x86; ipv6Ext[13] = 0xdd;
        ipv6Ext[14] = 0x60;
        ipv6Ext[18] = 0; ipv6Ext[19] = 16; // payload = 8B ext + 8B UDP
        ipv6Ext[20] = 0; // Hop-by-Hop Options
        ipv6Ext[21] = 255;
        ipv6Ext[54] = 17; // Next Header = UDP
        ipv6Ext[55] = 0;  // Ext len = (0 + 1) * 8 = 8B
        ipv6Ext[62] = 1; ipv6Ext[63] = 0x3f; // src 319
        ipv6Ext[64] = 1; ipv6Ext[65] = 0x3f; // dst 319
        ipv6Ext[66] = 0; ipv6Ext[67] = 8;
        assert(AirportAWDL::isUnicastPTP(ipv6Ext, sizeof(ipv6Ext)));

        uint8_t ipv4PTP[46] = {};
        copyAddress(ipv4PTP, peerAddr); copyAddress(ipv4PTP + 6, selfAddr);
        ipv4PTP[12] = 0x08; ipv4PTP[13] = 0x00;
        ipv4PTP[14] = 0x45;
        ipv4PTP[16] = 0; ipv4PTP[17] = 32;
        ipv4PTP[23] = 17; // UDP
        ipv4PTP[34] = 1; ipv4PTP[35] = 0x40; // 320
        ipv4PTP[36] = 1; ipv4PTP[37] = 0x40;
        ipv4PTP[38] = 0; ipv4PTP[39] = 12;
        assert(AirportAWDL::isUnicastPTP(ipv4PTP, sizeof(ipv4PTP)));

        auto *l2 = l2PTPFrame(selfAddr, peerAddr, 44, 0x77);
        auto *l2Mcast = l2PTPFrame(selfAddr, mcastAddr, 44, 0x77);
        assert(AirportAWDL::isUnicastPTP(l2->bytes.data(), l2->length));
        assert(!AirportAWDL::isUnicastPTP(l2Mcast->bytes.data(), l2Mcast->length));
        mbuf_freem(l2); mbuf_freem(l2Mcast);
    }

    // B. Head-FIFO ordering ahead of bulk pending[] + bypass when peer TXBA is Agreed & paused:
    AirportItlwmAWDLInterface i;
    dataPeer(i, peerAddr);
    for (auto &c : i.sync.channels) c = 48;
    for (auto &cls : i.sync.channelClasses) cls = 0;
    i.sequenceConfigured = true;
    i.device.awdlRadio.listener = true;
    i.tick();
    clockUS = i.sync.epochUS + 4000ULL;
    i.tick();
    i.device.loop.gated = true;
    i.nextActionUS = i.nextMIFUS = clockUS + 10000000ULL;

    auto *peer = i.device.awdlPeers.find(peerAddr, clockUS);
    assert(peer);
    for (unsigned s = 0; s < 16; ++s) { peer->sequence.channel[s] = 48; peer->sequence.flags[s] = 0; }
    peer->lastSeenUS = peer->syncReceivedUS = peer->sequenceReceivedUS = peer->electionV2ReceivedUS = clockUS;
    peer->sync.phyTxTime = peer->sync.targetTxTime = uint32_t(clockUS);
    peer->sync.nextWindowTU = 64;
    i.sync.elect(i.device.awdlPeers, clockUS);
    for (auto &c : i.sync.channels) c = 48;
    clockUS += 4000ULL;

    i.device.awdlRadio.ready = true;
    i.device.awdlRadio.channel = 48;
    i.device.awdlRadio.widthMHz = 20;
    i.device.awdlRadio.queue = 1;
    i.device.awdlRadio.llwQueueConfigured = true;
    i.device.awdlRadio.llwQueue = 3;
    i.device.awdlRadio.dwell.running = true;
    i.device.awdlRadio.dwell.windowEndUS = clockUS + 55000ULL;

    auto &tx = i.device.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.epoch = i.device.dmaEpoch;
    tx.cookie = 11;
    tx.queue = 2;
    tx.phase = TXBlockAckQueue::Agreed;
    tx.enabled = true;
    tx.paused = true; // Peer TXBA is PAUSED!
    tx.window = 64;
    copyAddress(tx.peer, peerAddr);

    // Queue 2 regular 1200B UDP packets, then Sync (319, tag 0x11), FollowUp (320, tag 0x22), and L2 0x88f7 (tag 0x33):
    i.bsd->output.push_back(sizedIPv6UDP(i.address.octet, peerAddr, 5000, 1200, 0x91));
    i.bsd->output.push_back(sizedIPv6UDP(i.address.octet, peerAddr, 5000, 1200, 0x92));
    i.bsd->output.push_back(sizedIPv6UDP(i.address.octet, peerAddr, 319, 90, 0x11));
    i.bsd->output.push_back(sizedIPv6UDP(i.address.octet, peerAddr, 320, 90, 0x22));
    i.bsd->output.push_back(l2PTPFrame(i.address.octet, peerAddr, 44, 0x33));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    assert(i.pendingCount == 5);

    // Drain while peer TXBA is paused: ONLY the 3 PTP packets drain in exact [0x11, 0x22, 0x33] FIFO order,
    // while the 2 regular packets remain held for TXBA!
    i.drainPendingData();
    assert(i.pendingCount == 2);
    assert(i.device.dataFrames.size() == 3);
    assert(i.device.dataFrames[0].back() == 0x11);
    assert(i.device.dataFrames[1].back() == 0x22);
    assert(i.device.dataFrames[2].back() == 0x33);

    // C. During Asking when solicitDeadline is active (negotiatingNow == true), even unicast PTP yields so ADDBA Request sends!
    tx.phase = TXBlockAckQueue::Asking;
    tx.paused = true;
    tx.attempts = 0;
    tx.nextAskUS = clockUS;
    tx.managementToken = 0;
    i.bsd->output.push_back(sizedIPv6UDP(i.address.octet, peerAddr, 319, 90, 0x44));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    const size_t beforeAskDrain = i.device.dataFrames.size();
    i.drainPendingData();
    assert(i.device.dataFrames.size() == beforeAskDrain); // Held for ADDBA Request!

    // D. Station 3 depth <= 2 keeps 500 us llwRealtime guardUS (remainingUS == 900 > 500 -> transmits!):
    tx = {};
    i.llwActive = true;
    i.llwParamsEnabled = true;
    i.device.awdlRadio.llwRealtime = true;
    i.device.com.txq[3].queued = 2; // Depth == 2 (<= 2)!
    i.device.awdlRadio.dwell.windowEndUS = clockUS + 900ULL;
    i.drain();
    i.bsd->output.push_back(sizedIPv6UDP(i.address.octet, peerAddr, 319, 90, 0x55));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    const size_t before900 = i.device.dataFrames.size();
    i.drainPendingData();
    assert(i.device.dataFrames.size() == before900 + 1);

    // At depth == 3 (> 2), guardUS becomes 1500 us (> 900 us remaining -> held!):
    i.device.com.txq[3].queued = 3;
    i.bsd->output.push_back(sizedIPv6UDP(i.address.octet, peerAddr, 320, 90, 0x66));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    i.drainPendingData();
    assert(i.device.dataFrames.size() == before900 + 1 && i.pendingCount == 1);

    i.drain();
    assert(!allocations);
    return 0;
}
""")
        self.compile_run(tick_src)

        # Part 2: HAL ItlIwx::transmitAWDL & serviceAWDLListener QoS routing (llwQueue -> dataQueue fallback, NEVER awdlRadio.queue!).
        extra_radio = r"""
static Packet makePTPMPDU(uint8_t dstPortLow = 0x3f) {
    uint8_t eth[102] = {};
    memcpy(eth, self, 6); eth[5] = 2;
    memcpy(eth + 6, self, 6);
    eth[12] = 0x86; eth[13] = 0xdd;
    eth[14] = 0x60;
    AirportAWDL::putBE16(eth + 18, 48);
    eth[20] = 17; eth[21] = 255;
    eth[54] = 0x01; eth[55] = dstPortLow; // 0x013f = 319, 0x0140 = 320
    eth[56] = 0x01; eth[57] = dstPortLow;
    AirportAWDL::putBE16(eth + 58, 48);
    Packet p;
    p.size = p.total = AirportAWDL::dataFromEthernet(eth, sizeof(eth), p.bytes, sizeof(p.bytes), 77, 12);
    assert(p.size > 0);
    return p;
}

int main() {
    // 1. When peer TXBA is Agreed & unpaused -> unicast PTP transmits on Station 4 tx.queue (qid 9):
    {
        ItlIwx d;
        setupPeerTX(d);
        auto p = makePTPMPDU(0x3f);
        assert(!d.transmitAWDL(&p, 901));
        assert(d.com.txq[9].queued == 1 && !d.com.txq[7].queued && !d.com.txq[8].queued);
        peerTXReport(d);
        // Pause peer TXBA + configure llwQueue (qid 6): unicast PTP routes to llwQueue as QoS TID 7 (NEVER qid 7 mgmt)!
        d.awdlRadio.blockAckTX.paused = true;
        d.com.txq[6].allocate(6);
        d.awdlRadio.llwQueue = 6;
        d.awdlRadio.llwQueueConfigured = true;
        auto p2 = makePTPMPDU(0x40);
        assert(!d.transmitAWDL(&p2, 902));
        assert(d.com.txq[6].queued == 1 && !d.com.txq[7].queued && !d.com.txq[9].queued);
        const auto *hdr6 = d.com.txq[6].cmd[0].data + sizeof(iwx_tx_cmd_gen2);
        assert(hdr6[0] == 0x88 && hdr6[24] == RadioState::LLWTID && hdr6[25] == 0);
        d.iwx_reset_tx_ring(&d.com, &d.com.txq[6]);
        d.awdlRadio.llwQueueConfigured = false;
        d.awdlRadio.llwQueue = IWX_INVALID_QUEUE;

        // Fall back to dataQueue (qid 8, QoS TID 0) when llwQueue is unavailable on QuZ:
        auto p3 = makePTPMPDU(0x3f);
        assert(!d.transmitAWDL(&p3, 903));
        assert(d.com.txq[8].queued == 1 && !d.com.txq[7].queued && !d.com.txq[9].queued);
        const auto *hdr8 = d.com.txq[8].cmd[0].data + sizeof(iwx_tx_cmd_gen2);
        assert(hdr8[0] == 0x88 && hdr8[24] == RadioState::DataTID && hdr8[25] == 0);
        d.iwx_reset_tx_ring(&d.com, &d.com.txq[8]);

        // If neither QoS queue is configured yet, returns EAGAIN (NEVER falls back to legacy 12 Mbps qid 7!):
        d.awdlRadio.dataQueueConfigured = false;
        auto p4 = makePTPMPDU(0x40);
        assert(d.transmitAWDL(&p4, 904) == EAGAIN && !d.com.txq[7].queued);
        d.awdlRadio.dataQueueConfigured = true;
        finishPeerTX(d);
    }

    // 2. Live QuZ path: when Station 3 rejects LLWTID (-EIO -> llwQueueFailed), serviceAWDLListener
    //    automatically provisions dataQueue so unicast PTP has an active QoS queue immediately!
    {
        ItlIwx d;
        d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
        assert(!d.startAWDLRadio(self));
        RadioState::GrantPolicy pol = {};
        pol.llwQueue = true;
        pol.llwRealtime = true;
        pol.widthMHz = 20;
        assert(!d.serviceAWDLChannel(48, clockUS + 65536, pol));
        assert(d.awdlRadio.llwQueueFailed && !d.awdlRadio.llwQueueConfigured);
        assert(d.awdlRadio.dataQueueConfigured && d.awdlRadio.dataQueue == 8);
        assert(!d.stopAWDLRadio());
    }
    return 0;
}
"""
        radio_harness = p91._build_radio_harness(extra_radio).replace(
            "assert(stationPresent && station==3 && size==64 && (tid==IWX_MGMT_TID || tid==RadioState::DataTID));",
            "if (tid == RadioState::LLWTID) { result->error = EIO; return -EIO; }\n"
            "        assert(stationPresent && station==3 && size==64 && (tid==IWX_MGMT_TID || tid==RadioState::DataTID));",
        ).replace(
            "value>>16==8 ||",
            "value>>16==8 || (awdlRadio.llwQueueConfigured && value>>16==unsigned(awdlRadio.llwQueue)) ||",
        )
        self.compile_run(radio_harness)

    def test_p112_u2b_single_wakeup_t_minus_400us_llw_realtime_pretune(self):
        tick_src = _tick_harness(r"""
int main() {
    clockUS = 10000000ULL;
    AirportItlwmAWDLInterface i;
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.ready = true;

    // Configure Continuity Camera RealTimeGroup [48, 48, 149, 48x7, 149, 48x5]:
    const uint16_t rtg[16] = {48, 48, 149, 48, 48, 48, 48, 48, 48, 48, 149, 48, 48, 48, 48, 48};
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = rtg[s];
        i.sync.channelClasses[s] = 0;
    }
    i.sequenceConfigured = true;
    i.llwActive = true;
    i.llwParamsEnabled = true;

    unsigned lastServicedChan = 0;
    uint64_t lastServicedEndUS = 0;
    i.device.onService = [&]() {
        lastServicedChan = i.device.lastPolicy.unbindIdle ? 0 : 1;
    };
    (void)lastServicedChan;

    // Start in Slot 0 (Ch 48, 2-slot run = 131072 us), then advance into Slot 1 (Ch 48):
    i.tick();
    assert(i.running && i.device.awdlRadio.channel == 48);
    clockUS = i.sync.epochUS + 65536ULL + 4000ULL;
    i.tick();
    assert(i.device.awdlRadio.channel == 48 && i.nativeWindowOpen);

    // 1. At T - 400 us before Slot 2 (48 -> 149, 14 -> 2 slots): FIX-P116-4 keeps PHY 1 on Ch 48
    //    so end-of-slot HEVC RTP / gPTP FollowUp frames are never cut off early, while arming
    //    boundary wakeup for 450 us (slot2StartUS + 50 us):
    const uint64_t slot2StartUS = i.sync.epochUS + 2ULL * 65536ULL;
    clockUS = slot2StartUS - 400ULL;
    const size_t msgBeforeSlot2 = i.controller.messages.size();
    i.tick();
    assert(i.device.awdlRadio.channel == 48);
    assert(i.device.awdlRadio.dwell.windowEndUS == slot2StartUS);
    lastServicedEndUS = i.device.awdlRadio.dwell.windowEndUS;
    assert(lastServicedEndUS == slot2StartUS);
    assert(i.scheduler.delayUS == 450);
    for (size_t m = msgBeforeSlot2; m < i.controller.messages.size(); ++m)
        assert(i.controller.messages[m].type != 41);

    // At T + 50 us (inside Slot 2), tick() tunes to Ch 149 and emits Ch 149 AVAILABILITY_WINDOW_START:
    clockUS = slot2StartUS + 50ULL;
    i.tick();
    assert(i.device.awdlRadio.channel == 149);
    assert(i.device.awdlRadio.dwell.windowEndUS == slot2StartUS + 65536ULL);
    assert(!i.controller.messages.empty() && i.controller.messages.back().type == 41);

    // 2. At T - 400 us before Slot 3 (return 149 -> 48x7, 2 -> 14 slots): single wakeup pre-tunes
    //    back onto Ch 48 across 7 slots without premature AVAILABILITY_WINDOW_START, and at T + 50 us
    //    emits Ch 48 AVAILABILITY_WINDOW_START with ZERO extra retune!
    const uint64_t slot3StartUS = i.sync.epochUS + 3ULL * 65536ULL;
    clockUS = slot3StartUS - 400ULL;
    const size_t msgBeforePretune = i.controller.messages.size();
    i.tick();
    assert(i.device.awdlRadio.channel == 48);
    assert(i.device.awdlRadio.dwell.windowEndUS == slot3StartUS + 7ULL * 65536ULL);
    assert(i.scheduler.delayUS == 450);
    for (size_t m = msgBeforePretune; m < i.controller.messages.size(); ++m)
        assert(i.controller.messages[m].type != 41);

    const unsigned dwellStartsAfterPretune = i.device.awdlRadio.dwellStarts;
    clockUS = slot3StartUS + 50ULL;
    i.tick();
    assert(i.device.awdlRadio.channel == 48);
    assert(i.device.awdlRadio.dwellStarts == dwellStartsAfterPretune);
    assert(!i.controller.messages.empty() && i.controller.messages.back().type == 41);

    i.drain();
    assert(!allocations);
    return 0;
}
""")
        self.compile_run(tick_src)

    def test_p112_u2c_abort_scan_for_continuity_media_on_24h32(self):
        # Compile & execute both setSCAN_CONTROL and abortScanForContinuityMedia with __IO80211_TARGET = 150400 (not 150200!).
        wcl_methods = "\n".join(
            definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::" + n)
            for n in ("setSCAN_CONTROL", "abortScanForContinuityMedia")
        )
        src = r"""
#include <cassert>
#include <cstdint>
#define __MAC_15_2 150200
#define __IO80211_TARGET 150400
using IOReturn = int;
enum { kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnNotReady = 2 };
enum { IEEE80211_S_RUN = 4, IEEE80211_F_BGSCAN = 0x200 };
struct apple80211_scan_control_params { uint8_t bytes[5]; };
struct OSObject {};
struct ieee80211com { int ic_state = IEEE80211_S_RUN; unsigned ic_flags = IEEE80211_F_BGSCAN; };
struct DriverController { unsigned cleared = 0; void clearScanningFlags() { ++cleared; } };
struct HalService {
    ieee80211com ic; DriverController drv;
    ieee80211com *get80211Controller() { return &ic; }
    DriverController *getDriverController() { return &drv; }
};
struct AWDLStub { bool media = true; bool continuityMediaActive() const { return media; } };
struct InstanceStub { AWDLStub *awdlInterface = nullptr; };
struct GateStub {
    IOReturn runAction(IOReturn (*fn)(OSObject *, void *, void *, void *, void *), void *self, void *arg = nullptr) {
        return fn(nullptr, self, arg, nullptr, nullptr);
    }
};
struct AirportItlwmSkywalkInterface {
    HalService *fHalService = nullptr;
    GateStub *_fCommandGate = nullptr;
    InstanceStub *instance = nullptr;
    bool scanPending = true;
    unsigned finished = 0;
    void finishWCLScan(IOReturn status) { assert(status == kIOReturnSuccess); scanPending = false; ++finished; }
    void abortScanForContinuityMedia();
    IOReturn setSCAN_CONTROL(apple80211_scan_control_params *params);
};
""" + wcl_methods + r"""
int main() {
    HalService hal;
    GateStub gate;
    AWDLStub awdl;
    InstanceStub inst{&awdl};
    AirportItlwmSkywalkInterface sky{&hal, &gate, &inst, true, 0};

    // 1. abortScanForContinuityMedia clears IEEE80211_F_BGSCAN and finishes pending WCL scan on 24H32:
    sky.abortScanForContinuityMedia();
    assert(!(hal.ic.ic_flags & IEEE80211_F_BGSCAN));
    assert(!sky.scanPending && sky.finished == 1);

    // 2. setSCAN_CONTROL ALSO aborts active scan without __MAC_15_2 gating:
    hal.ic.ic_flags = IEEE80211_F_BGSCAN;
    sky.scanPending = true;
    apple80211_scan_control_params params = {};
    assert(sky.setSCAN_CONTROL(&params) == kIOReturnSuccess);
    assert(!(hal.ic.ic_flags & IEEE80211_F_BGSCAN));
    assert(!sky.scanPending && sky.finished == 2);
    return 0;
}
"""
        self.compile_run(src)

        awdl_if_src = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.cpp").read_text()
        self.assertGreaterEqual(awdl_if_src.count("abortScanForContinuityMedia()"), 2)

    def test_p112_u3_once_per_packet_txba_demand_without_per_tick_rescan_inflation(self):
        tick_src = _tick_harness(r"""
int main() {
    clockUS = 10000000ULL;
    const uint8_t ipadPeer[6] = {2, 0, 0, 0, 0, 0x55};
    AirportItlwmAWDLInterface i;
    dataPeer(i, ipadPeer);
    for (auto &c : i.sync.channels) c = 44;
    i.sequenceConfigured = true;
    i.device.awdlRadio.listener = true;
    i.tick();
    clockUS += 4000ULL;
    i.tick();

    auto &tx = i.device.awdlRadio.blockAckTX;
    tx = {};

    // 1. Enqueue ONE 1400B heavy packet and call serviceTXBlockAck() 32 times across 32 ms:
    //    demandFrames MUST stay 1 and phase MUST stay Idle (never inflated 1 -> 16 by per-tick rescans!).
    i.bsd->output.push_back(sizedIPv6UDP(i.address.octet, ipadPeer, 5000, 1400, 1));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    assert(tx.demandFrames == 1 && tx.phase == TXBlockAckQueue::Idle);
    for (unsigned t = 0; t < 32; ++t) {
        clockUS += 1000ULL;
        i.serviceTXBlockAck();
    }
    assert(tx.demandFrames == 1 && tx.phase == TXBlockAckQueue::Idle);

    // 2. Enqueuing 15 more distinct heavy packets reaches DemandThreshold (16) and enters Requested!
    for (unsigned n = 2; n <= 16; ++n)
        i.bsd->output.push_back(sizedIPv6UDP(i.address.octet, ipadPeer, 5000, 1400, uint8_t(n)));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    assert(tx.demandFrames == 16 && tx.phase == TXBlockAckQueue::Requested);

    i.drain();
    assert(!allocations);
    return 0;
}
""")
        self.compile_run(tick_src)

    def test_p112_c1_c4_guards_inorder_amsdu_fastpath_and_no_listener_low_latency_or_session_prot(self):
        # 1. Verify ItlIwx.cpp preserves pure 2.4.0d104 in-order multi-subframe A-MSDU immediate delivery
        #    on both TID 7 (Continuity Camera LLW) and TID 0 (Sidecar): Subframe 0 delivers immediately
        #    with num_stored == 0 and does NOT freeze subsequent SNs if trailing subframe is lost!
        self.compile_run(p91._build_ba_harness(r"""
int main() {
    for (uint8_t tid : {uint8_t(7), uint8_t(0)}) {
        ItlIwx d;
        const auto r = request(d, 0, tid, 200);
        const unsigned baid = start(d, r);
        auto &buf = d.com.sc_rxba_data[baid].reorder_buf;

        // Case A: Complete 2-subframe in-order hardware A-MSDU at sn=200, nssn=201:
        auto sf0 = packet(d, r, 200, 10);
        auto sf1 = packet(d, r, 200, 11);
        receive(d, sf0.get(), baid, 200, 201, 0, false);
        assert((d.delivered == std::vector<uint16_t>{10}));
        assert(buf.num_stored == 0 && buf.head_sn == 200 && !timeout_pending(&buf.reorder_timer));

        receive(d, sf1.get(), baid, 200, 201, 1, true);
        assert((d.delivered == std::vector<uint16_t>{10, 11}));
        assert(buf.num_stored == 0 && buf.head_sn == 201 && !timeout_pending(&buf.reorder_timer));

        // Case B: Truncated in-order A-MSDU at sn=201 (Subframe 0 arrives, trailing subframe lost!),
        // followed by sn=202 (nssn=203): MUST deliver sn=202 immediately with ZERO 8ms/40ms stall!
        auto trunc0 = packet(d, r, 201, 12);
        auto nextMPDU = packet(d, r, 202, 13);
        receive(d, trunc0.get(), baid, 201, 202, 0, false);
        assert((d.delivered == std::vector<uint16_t>{10, 11, 12}));
        assert(buf.num_stored == 0 && !timeout_pending(&buf.reorder_timer));

        receive(d, nextMPDU.get(), baid, 202, 203);
        assert((d.delivered == std::vector<uint16_t>{10, 11, 12, 13}));
        assert(buf.num_stored == 0 && buf.head_sn == 203 && !timeout_pending(&buf.reorder_timer));

        assert(!d.clearAWDLBlockAck());
    }
    return 0;
}
"""))

        # 2. Hard invariants: zero MAC_LOW_LATENCY_CMD anywhere in ItlIwxAWDL.cpp, and
        #    serviceAWDLListener contains zero IWX_SESSION_PROTECTION_CMD.
        awdl_hal = (support.ROOT / "itlwm/hal_iwx/ItlIwxAWDL.cpp").read_text()
        listener_fn = definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::serviceAWDLListener")
        self.assertNotIn("LOW_LATENCY_CMD", awdl_hal)
        self.assertNotIn("IWX_SESSION_PROTECTION_CMD", listener_fn)


if __name__ == "__main__":
    unittest.main()
