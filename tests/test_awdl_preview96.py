"""Failing-first regressions for Preview 96:
1. Pre-retune excursion width handling in GrantPolicy::windowEnd plus single-count,
   entry-anchored listener parking: leaving 149@80 for a 2-slot [48, 48] (131.072 ms) or
   1-slot [6] (65.536 ms) 20 MHz excursion returns the actual untilChannelChangeUS deadline
   instead of 0 (20 != 80), and multi-tick 5 ms polling keeps the 149@80 park intact across
   the full <= 140 ms window while counting one park per excursion.
2. Continuity media protection for associated WCL and legacy STA background scans:
   active Continuity Camera (llwActive && llwParamsEnabled), Sidecar (Agreed TXBA / heavy TX),
   or AirDrop RX (heavy RX / active RXBA) completes associated scans from cache in 1 ms without
   issuing multi-channel firmware bgscans, while idle and unassociated scans stay live.
3. RealTimeGroup (llwParamsEnabled) and isolated 1-slot social 20 MHz width discipline:
   [48, 48, 149, 48x7, 149, 48x5] keeps isolated 1-slot ch 149 discovery hops at 20 MHz,
   setLLW_PARAMS dynamically updates sequence widths in either call order so 48 <-> 149 hops
   issue zero ADD_STA width mutations or TLC resets, and multi-slot Sidecar/AirDrop runs keep 80 MHz.
4. High-priority microsecond slot-boundary wakeups, unblocked llw0 RTCP NACK/FIR transmission,
   contiguous listener ADDBA Response eligibility, and 3-attempt accepted ADDBA Response retries.
"""
import re
import unittest

import test_awdl_preview82 as p82
import test_awdl_preview91 as p91
import test_regressions as support
from test_offline import definition


class Preview96Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p96_listener_window_end_allows_80mhz_to_20mhz_excursion_parking(self):
        # Part A: AirportItlwmAWDLInterface::tick's policy.windowEnd callback MUST return
        # non-zero untilChannelChangeUS when leaving 149@80 into a 48@20 or 6@20 excursion!
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

    const uint16_t seq[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = seq[s];
        i.sync.channelClasses[s] = (seq[s] == 149) ? 128 : 0;
    }
    i.sync.epochCounter = 0;
    i.sync.epochUS = clockUS;

    // Slot 0..5: 149@80 active on radio.
    clockUS += 5000;
    i.tick();
    assert(i.device.awdlRadio.channel == 149 && i.device.awdlRadio.widthMHz == 80);
    assert(i.device.preRetuneWindowEnd > clockUS + 380000);

    // Advance into Slot 6 [48, 48] (48@20 while awdlRadio is STILL 149@80 pre-retune!).
    // policy.windowEnd MUST return the 2-slot 48@20 deadline (~126 ms remaining), NOT 0!
    clockUS = i.sync.epochUS + 6ULL * 65536 + 5000;
    i.tick();
    assert(i.device.preRetuneWindowEnd == i.sync.epochUS + 8ULL * 65536);

    // Once retuned onto ch 48, a width mismatch on the SAME channel MUST still return 0.
    assert(i.device.awdlRadio.channel == 48 && i.device.awdlRadio.widthMHz == 20);
    i.device.awdlRadio.widthMHz = 80;
    assert(i.device.lastPolicy.windowEnd(i.device.lastPolicy.context, clockUS, 6, 48) == 0);
    return 0;
}
"""
        self.compile_run(tick_src)

        # Part B: ItlIwx::serviceAWDLListener multi-tick 5 ms polling across a 131 ms park
        # MUST keep PHY 1 parked at 149@80 even when clockUS - lastHeavyDataUS crosses 500 ms
        # mid-park, and MUST increment infraParks / socialParks once per parked excursion.
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

    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 9};
    auto &peer = d.awdlPeers.peers[0];
    peer = {};
    peer.occupied = true;
    AirportAWDL::copyAddress(peer.address, peerAddr);
    peer.haveSync = peer.haveSequence = true;
    peer.metadata.fields = AirportAWDL::HTCapabilities;
    peer.metadata.rxHTMcs = 0xff;
    peer.lastSeenUS = peer.syncReceivedUS = peer.sequenceReceivedUS = peer.htCapabilitiesReceivedUS = clockUS;

    auto &tx = d.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.epoch = d.dmaEpoch;
    tx.phase = AirportAWDL::TXBlockAckQueue::Agreed;
    AirportAWDL::copyAddress(tx.peer, peerAddr);
    tx.acknowledged = 64;
    tx.enabled = true;
    tx.widthMHz = 80;
    // Last heavy TX burst finished 420 ms before entering the 131 ms [48, 48] excursion:
    // heavyTransmitActive is true on entry, and crosses 500 ms at +80 ms mid-park!
    tx.lastHeavyDataUS = clockUS - 420000;

    RadioState::GrantPolicy p80 = {}; p80.widthMHz = 80; p80.slot = 0;
    assert(!d.serviceAWDLChannel(149, clockUS + 393216, p80));
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

    const uint64_t excursionEndUS = clockUS + 131072;
    const size_t phyBeforePark = d.phyCommands.size();
    RadioState::GrantPolicy p20 = {}; p20.widthMHz = 20; p20.slot = 6; p20.bridgeInfraToWide = true;
    for (unsigned step = 0; step < 25; ++step) {
        assert(!d.serviceAWDLChannel(48, excursionEndUS, p20));
        assert(!d.awdlRadio.dwell.running && tx.paused);
        assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);
        assert(d.phyCommands.size() == phyBeforePark);
        clockUS += 5000; // Reaches +125 ms -> clockUS - lastHeavyDataUS == 545 ms > 500 ms!
        peer.lastSeenUS = peer.syncReceivedUS = peer.sequenceReceivedUS = peer.htCapabilitiesReceivedUS = clockUS;
    }
    assert(d.awdlRadio.blockAckTXStats.infraParks == 1);
    assert(!d.stopAWDLRadio());
    return 0;
}
"""
        self.compile_run(p91._build_radio_harness(extra_radio))

    def test_p96_wcl_and_sta_scans_defer_live_bgscan_during_active_continuity_media(self):
        # 1. Verify AirportItlwmAWDLInterface::continuityMediaActive() across idle vs active media.
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        extra_awdl = definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmAWDLInterface::continuityMediaActive",
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines() + "\n" + extra_awdl)
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src = tick_src.replace(
            "void tick();",
            "void tick();\n    bool continuityMediaActive() const;",
        )
        tick_src = tick_src.replace(
            "Radio() {awdlRadio.widthMHz=20;}",
            "bool rxBA=false;\n    bool awdlHasRXBA() const {return rxBA;}\n    Radio() {awdlRadio.widthMHz=20;}",
        )
        tick_src += r"""
int main() {
    clockUS = 10000000;
    AirportItlwmAWDLInterface i;
    i.requested = true;
    i.running = true;
    i.device.awdlRadio.ready = true;

    // Idle discovery: MUST NOT block Wi-Fi background scans!
    assert(!i.continuityMediaActive());

    // 1. Active LLW Continuity Camera session (llwActive && llwParamsEnabled):
    i.llwActive = true;
    i.llwParamsEnabled = false;
    assert(!i.continuityMediaActive()); // Idle-attached llw0 since boot is NOT active media.
    i.llwParamsEnabled = true;
    assert(i.continuityMediaActive());
    i.llwParamsEnabled = false;

    // 2. Active Sidecar / heavy TX or Agreed TXBA with recent data (< 2 s):
    i.device.awdlRadio.blockAckTX.lastHeavyDataUS = clockUS - 200000;
    assert(i.continuityMediaActive());
    i.device.awdlRadio.blockAckTX.lastHeavyDataUS = 0;

    i.device.awdlRadio.blockAckTX.configured = true;
    i.device.awdlRadio.blockAckTX.phase = TXBlockAckQueue::Agreed;
    i.device.awdlRadio.blockAckTX.lastDataUS = clockUS - 1500000;
    assert(i.continuityMediaActive());
    i.device.awdlRadio.blockAckTX.lastDataUS = clockUS - 2500000;
    assert(!i.continuityMediaActive()); // Stale Agreed TXBA (> 2 s idle) allows scans again!

    // 3. Active AirDrop / Continuity RXBA payload (< 1.5 s):
    i.device.rxBA = true;
    i.device.awdlRadio.lastRXPayloadUS = clockUS - 800000;
    assert(i.continuityMediaActive());
    i.device.awdlRadio.lastRXPayloadUS = clockUS - 1800000;
    assert(!i.continuityMediaActive());
    return 0;
}
"""
        self.compile_run(tick_src)

        # 2. Verify AirportItlwmSkywalkInterface::startWCLScan skips ieee80211_begin_cache_bgscan
        #    when associated + portAuthorized + continuityMediaActive(), replays cached BSS, and
        #    completes ScanDone in 1 ms, while running live bgscan when idle or unassociated.
        wcl_src = (support.ROOT / "tests/fixtures/wcl_lifecycle.cpp").read_text().rsplit("int main()", 1)[0]
        scan_fns = "\n".join(
            definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::" + name)
            for name in (
                "beginWCLScan",
                "completeWCLScanFromCache",
                "startWCLScan",
                "resumeWCLScan",
                "finishWCLScan",
                "completeWCLScan",
                "resumeWCLJoin",
                "handleWCLAvailability",
            )
        )
        wcl_src = wcl_src.replace(
            "struct Instance {",
            "struct MockAWDL { bool active = false; bool continuityMediaActive() const { return active; } };\n"
            "struct Instance {\n"
            "    MockAWDL awdl;\n"
            "    MockAWDL *awdlInterface = &awdl;\n"
            "    unsigned scanResults = 0;",
        )
        wcl_src = wcl_src.replace(
            "messages.push_back({code, *static_cast<int *>(data)});",
            "if (code == AirportWCL::ScanResult) { ++scanResults; return; }\n"
            "        messages.push_back({code, *static_cast<int *>(data)});",
        )
        wcl_src = wcl_src.replace(
            "bool scanPending = false,",
            "bool wclBssValid = false;\n"
            "    uint8_t wclBssInfo[sizeof(AirportWCL::BeaconMetadata) + 0x800] = {};\n"
            "    bool scanPending = false,",
        )
        wcl_src = wcl_src.replace(
            "bool completeWCLScanFromCache(const AirportWCL::ScanRequest &) { return false; }",
            "bool completeWCLScanFromCache(const AirportWCL::ScanRequest &);",
        )
        wcl_src = wcl_src.replace("// PRODUCTION: scan\n", scan_fns + "\n")
        wcl_src += r"""
int main() {
    apple80211ScanRequest req = {};
    req.bytes[0x40] = 1;

    // 1. Associated + portAuthorized + active Continuity media:
    //    MUST NOT call ieee80211_begin_cache_bgscan, MUST replay matching wclBssInfo,
    //    and MUST arm 1 ms ScanDone completion!
    {
        AirportItlwmSkywalkInterface s;
        s.portAuthorized = true;
        s.owner.awdl.active = true;
        AirportWCL::BeaconMetadata meta = {};
        meta.channel = 36;
        meta.ssidLength = 4;
        memcpy(meta.ssid, "Test", 4);
        const uint8_t bssid[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
        memcpy(meta.bssid, bssid, 6);
        meta.ieLength = 0;
        memcpy(s.wclBssInfo, &meta, sizeof(meta));
        s.wclBssValid = true;

        assert(s.beginWCLScan(&req) == kIOReturnSuccess);
        assert(!(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
        assert(s.scanPending && s.scanCompleted && s.timer.delay == 1);
        assert(s.owner.scanResults == 1);
        s.completeWCLScan();
        assert(!s.scanPending && s.owner.scans() == 1 && s.owner.lastScan() == kIOReturnSuccess);
    }

    // 2. Associated + portAuthorized + IDLE AWDL: MUST still run live firmware bgscan!
    {
        AirportItlwmSkywalkInterface s;
        s.portAuthorized = true;
        s.owner.awdl.active = false;
        assert(s.beginWCLScan(&req) == kIOReturnSuccess);
        assert(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN);
        assert(s.timer.delay == 20000);
    }

    // 3. Unassociated (IEEE80211_S_INIT -> S_SCAN) or !portAuthorized: MUST still run live scan!
    {
        AirportItlwmSkywalkInterface s;
        s.portAuthorized = false;
        s.owner.awdl.active = true;
        assert(s.beginWCLScan(&req) == kIOReturnSuccess);
        assert(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN);
    }

    return 0;
}
"""
        self.compile_run(wcl_src)

        # 3. Verify legacy STA scan callers also guard ieee80211_begin_cache_bgscan during Continuity media.
        skywalk_cpp = (support.ROOT / "AirportItlwm/AirportItlwmSkywalkInterface.cpp").read_text()
        sta_ioctl_cpp = (support.ROOT / "AirportItlwm/AirportSTAIOCTL.cpp").read_text()
        self.assertIn("continuityMediaActive()", skywalk_cpp)
        self.assertIn("continuityMediaActive()", sta_ioctl_cpp)

    def test_p96_realtime_group_and_isolated_social_hops_stay_20mhz_without_peer_width_churn(self):
        source = (support.ROOT / "tests/fixtures/awdl_control.cpp").read_text().rsplit("int main()", 1)[0]
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
        source += r"""
int main() {
    AirportItlwmAWDLInterface i;
    i.device.ic.ic_state = IEEE80211_S_RUN;
    i.device.ic.ic_bss = &i.device.ap;
    i.device.apChannel.channel = 48;
    i.device.allow80 = true;
    i.device.awdlRadio.ready = true;

    // 1. RealTimeGroup [48, 48, 149, 48x7, 149, 48x5] with isolated 1-slot ch 149 hops (flags=0x92):
    //    Isolated 1-slot ch 149 hops (!multiSlotRun) and overlapping ch 48 infra slots stay 20 MHz (0)
    //    both before and after setLLW_PARAMS enables LLW, eliminating 4/s 48@20 <-> 149@80 width thrash!
    const unsigned rtg[16] = {48,48,149,48,48,48,48,48,48,48,149,48,48,48,48,48};
    apple80211_awdl_sync_channel_sequence rtgSeq = {};
    rtgSeq.version = 1; rtgSeq.pad1 = 0x00; rtgSeq.length = 16; rtgSeq.encoding = 1; rtgSeq.step_count = 3; rtgSeq.fill_channel = 0xff;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = rtg[n];
        rtgSeq.seqs[n] = {1, c, c == 48 ? 0x294U : 0x92U};
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &rtgSeq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == 0);

    apple80211_llw_params llw = {};
    llw.version = 1;
    llw.enable = 1;
    assert(i.handle(Request::setLLW_PARAMS, &llw) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == 0);

    llw.enable = 0;
    assert(i.handle(Request::setLLW_PARAMS, &llw) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == 0);

    // 2. Sidecar / AirDrop [149x6, 48, 48, 6, 149x5, 48, 48] with !llwParamsEnabled:
    //    Multi-slot 149 runs MUST still promote to 80 MHz (11 wide slots)!
    const unsigned sidecar[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    apple80211_awdl_sync_channel_sequence scSeq = rtgSeq;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = sidecar[n];
        scSeq.seqs[n] = {1, c, c == 48 ? 0x294U : (c == 6 ? 0x8aU : 0x92U)};
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &scSeq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == (sidecar[n] == 149 ? 128 : 0));

    // 3. Enabling LLW keeps non-overlapping ch 149 at 80 MHz while clamping infra-overlapping ch 48 to 20 MHz,
    //    and disabling LLW preserves 80 MHz on multi-slot 149 runs!
    llw.enable = 1;
    assert(i.handle(Request::setLLW_PARAMS, &llw) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == (sidecar[n] == 149 ? 128 : 0));

    llw.enable = 0;
    assert(i.handle(Request::setLLW_PARAMS, &llw) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == (sidecar[n] == 149 ? 128 : 0));

    return 0;
}
"""
        self.compile_run(source)

    def test_p96_subms_slot_boundary_wakeup_llw_tx_priority_and_addba_response_retry(self):
        # Verify high-priority IOTimerEventSource option in AirportItlwmAWDLInterface::start
        # and 1500 us LLW close-ahead lead in ItlIwx::serviceAWDLListener.
        awdl_cpp = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.cpp").read_text()
        self.assertIn("kIOTimerEventSourceOptionsPriorityHigh", awdl_cpp)

        extra_radio = r"""
int main() {
    ItlIwx d;
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    assert(!d.startAWDLRadio(self));
    RadioState::GrantPolicy pLLW = {};
    pLLW.llwRealtime = true;
    pLLW.widthMHz = 20;
    pLLW.slot = 0;
    const uint64_t endUS = clockUS + 65536;
    assert(!d.serviceAWDLChannel(48, endUS, pLLW));
    assert(d.awdlRadio.dwell.running);
    assert(d.awdlRadio.dwell.eawEndUS == endUS);
    assert(d.awdlRadio.dwell.windowEndUS == endUS - 400);
    d.com.txq[d.awdlRadio.queue].queued = 1;
    pLLW.slot = 1;
    assert(!d.serviceAWDLChannel(48, endUS, pLLW));
    assert(d.awdlRadio.dwell.windowEndUS == endUS - 1500);
    d.com.txq[d.awdlRadio.queue].queued = 0;
    assert(!d.stopAWDLRadio());
    return 0;
}
"""
        self.compile_run(p91._build_radio_harness(extra_radio))

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
            "if(!c) {awdlRadio.dwell.running=false;awdlRadio.queue=0xffff;return 0;}",
            "if(!c || endUS <= clockUS + 3000) {awdlRadio.dwell.running=false;return 0;}",
        )
        tick_src += r"""
int main() {
    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 9};

    // 1. Sub-millisecond slot-boundary wakeup via setTimeoutUS:
    {
        clockUS = 10000000;
        AirportItlwmAWDLInterface i;
        i.device.loop.gated = true;
        i.device.awdlRadio.listener = true;
        i.device.awdlRadio.ready = true;
        i.running = true;
        for (unsigned s = 0; s < 16; ++s)
            i.sync.channels[s] = (s % 2 == 0) ? 44 : 149;
        Window w = {};
        assert(i.sync.window(clockUS, w));
        clockUS += w.untilChannelChangeUS - 1400;
        i.tick();
        assert(!i.device.awdlRadio.dwell.running);
        assert(i.scheduler.delayUS == 1450);
    }

    // 2. Unblocked llw0 RTCP NACK/FIR TX on llwQueue (bypasses psfDue hold and uses 1500 us guard):
    {
        clockUS = 10000000;
        AirportItlwmAWDLInterface i;
        dataPeer(i, peerAddr);
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
        for (unsigned s = 0; s < 16; ++s) i.sync.channels[s] = 44;
        auto *peer = i.device.awdlPeers.find(peerAddr, clockUS);
        assert(peer);
        for (unsigned s = 0; s < 16; ++s) peer->sequence.channel[s] = 44;
        i.sync.epochCounter = 0;
        i.sync.epochUS = clockUS;
        peer->sync.nextWindowTU = 64;
        peer->syncReceivedUS = clockUS;

        // Advance 1800 us into slot (1500 <= 1800 < 3000) WITH psfDue(now) == true!
        clockUS += 1800;
        i.nextActionUS = clockUS; // psfDue(clockUS) == true!
        assert(i.psfDue(clockUS));
        assert(i.enqueueLLWFrame(ethernet(i.address.octet, peerAddr)) == kIOReturnSuccess);
        assert(i.inFlightCount == 0 && i.pendingCount == 0 && i.device.data == 1);
        i.drain();
    }

    // 3. Contiguous listener ADDBA Response eligibility across internal EAW boundaries
    //    AND 3-attempt retry when an accepted ADDBA Response is not ACKed!
    {
        clockUS = 10000000;
        AirportItlwmAWDLInterface i;
        dataPeer(i, peerAddr);
        i.device.loop.gated = true;
        i.device.awdlRadio.listener = true;
        i.device.awdlRadio.ready = true;
        i.device.baResult = 0; // Accepted RXBA session!
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

        // Position 1500 us before internal EAW 0->1 boundary inside contiguous [44x16]
        // (untilNextEAWUS == 1500 < 3000, while untilChannelChangeUS > 900000!).
        clockUS = i.sync.epochUS + 65536 - 1500;
        i.device.awdlRadio.channel = 44;
        i.device.awdlRadio.widthMHz = 20;
        i.device.awdlRadio.dwell.running = true;
        i.device.awdlRadio.dwell.windowEndUS = clockUS + 500000;

        BlockAckRequest req = {};
        copyAddress(req.peer, peerAddr);
        req.tid = 7;
        req.dialog = 42;
        req.window = 64;
        req.ssn = 100;
        AirportItlwmAWDLInterface::receiveBlockAck(&i, req);
        assert(i.blockAckQueued == 1);

        i.serviceBlockAck();
        assert(i.blockAckSubmitted == 1);
        const uint64_t tok1 = i.blockAckReplies[0].token;

        // First unacknowledged attempt MUST re-queue (attempts == 1 < 3), NOT drop!
        i.completeUnicast(tok1, false);
        assert(i.blockAckFailed == 1);
        assert(i.blockAckReplies[0].state == BlockAckReply::Queued);
        assert(i.blockAckReplies[0].token > tok1);

        // Second attempt transmitted and acknowledged -> session succeeds without 200 ms stall!
        i.serviceBlockAck();
        assert(i.blockAckSubmitted == 2);
        const uint64_t tok2 = i.blockAckReplies[0].token;
        i.completeUnicast(tok2, true);
        assert(i.blockAckAcknowledged == 1);
        assert(i.blockAckReplies[0].state == BlockAckReply::Free);
        i.drain();
    }

    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)


if __name__ == "__main__":
    unittest.main()
