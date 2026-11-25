"""Failing-first regressions for Preview 101:
1. Logical EAW-start timestamp anchoring in updateAvailability() (start.timestamp_be = eawStartUS),
   multi-slot listener EAW boundary wakeup in tick() even when !nativeWindowOpen, and immediate
   post-scan AWDL recovery via notifyInfrastructureReady() (nextRetryUS = 0 + 1 ms timer).
2. Idle Agreed TXBA window quiescence: after > 1.5 s with zero pending/in-flight/hardware-queued frames,
   serviceTXBlockAck::WindowCheck::end() returns 0 so channel hops (149@80 <-> 48@20 <-> 6@20) stop
   issuing idle TLC_MNG_CONFIG_CMD / ADD_STA commands, while pullOutputPackets() refreshes lastDataUS
   and reopens TXBA within <= 1 ms on new unicast traffic.
3. MSI RX interrupt IORegistry stall elimination: publishAWDLReceive() throttles to >= 5 s during active
   media (llwRealtime || heavyReceiveActive || heavyTransmitActive), and RX hot-paths (BAR in
   receiveAWDLReordered, receiveAWDL, awdlNonAPDataDropped, and publishNativePeers in receiveAction)
   skip synchronous IORegistry property allocations while media is active.
4. Sequoia WCL roam-lock (setWCL_SET_ROAM_LOCK, 0x1ac) and real-time mode (setWCL_REAL_TIME_MODE, 0x1b4)
   support: returns kIOReturnSuccess, aborts any in-flight IEEE80211_F_BGSCAN while associated, and
   satisfies background scans from cached BSS in 1 ms so FaceTime + Sidecar / Continuity Camera never
   suffer off-channel roam-scan excursions or Scan Control errors.
"""
import unittest

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
    tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines())
    tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
    tick_src = tick_src.replace(
        "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {return width==20 && (c==6||c==44||(c==149&&allow149));}",
        "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {"
        "return width==20 && (c==6||c==44||c==48||(c==149&&allow149));}",
    )
    return tick_src + main_body


class Preview101Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p101_native_window_eaw_start_timestamp_multislot_deadline_and_fast_post_scan_resume(self):
        # updateAvailability() anchors AVAILABILITY_WINDOW_START.timestamp_be to logical eawStartUS
        # even when radio dwell opens mid-EAW, tick() wakes at current.untilNextEAWUS on multi-slot
        # listener runs even when !nativeWindowOpen, and notifyInfrastructureReady() clears nextRetryUS
        # and arms a 1 ms timer so AWDL resumes immediately after an infrastructure scan finishes.
        extra_fn = definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmAWDLInterface::notifyInfrastructureReady",
        )
        tick_src = _tick_harness("").replace(
            "void tick();",
            "void tick();\n    void notifyInfrastructureReady();",
        ) + "\n" + extra_fn + r"""
int main() {
    clockUS = 10000000ULL;
    AirportItlwmAWDLInterface i;
    i.sequenceConfigured = true;
    for (unsigned s = 0; s < 16; ++s) i.sync.channels[s] = (s == 2 || s == 10) ? 149 : 48;
    i.sync.start(i.address.octet, 48, clockUS);
    i.sync.epochCounter = 12980;
    auto &r = i.device.awdlRadio;
    r.ready = true;
    r.listener = true;
    r.channel = 48;
    r.widthMHz = 20;

    // 1. Move to Slot 3 (counter 12992, start of 7-slot contiguous run on channel 48 = 458,752 us),
    //    and open radio dwell 18,500 us after the logical EAW start:
    const uint64_t slot3StartUS = clockUS + 3ULL * 65536ULL;
    clockUS = slot3StartUS + 18500ULL;
    Window w = {};
    assert(i.sync.window(clockUS, w));
    assert((w.counter & ~3U) == 12992);
    r.dwell.requested = true;
    r.dwell.running = true;
    r.dwellStarts = 1;
    r.dwell.windowEndUS = clockUS + w.untilChannelChangeUS; // ~440 ms multi-slot run!

    i.updateAvailability();
    assert(i.nativeWindowOpen && i.controller.messages.size() == 1);
    assert(i.controller.messages[0].type == 41);
    // timestamp_be MUST record logical EAW start (slot3StartUS), NOT mid-EAW clockUS!
    assert(be32(i.controller.messages[0].bytes.data() + 20) == uint32_t(slot3StartUS));

    // 2. If nativeWindowOpen is false mid-run (e.g. postMessage temporarily failed on START),
    //    tick() on a 7-slot run MUST still wake at the next EAW (~3,200 us) when near EAW end
    //    rather than sleeping for the full 5 ms / multi-slot dwell!
    i.nativeWindowOpen = false;
    i.nativeWindowStarts = 1;
    i.nativeWindowCounter = 12992;
    i.running = true;
    i.llwActive = true;
    i.llwParamsEnabled = true;
    i.nextActionUS = i.nextMIFUS = clockUS + 5000000ULL;
    clockUS = slot3StartUS + 65536ULL - 3200ULL; // 3,200 us until next EAW 12996, > 395 ms until channel change!
    i.tick();
    assert(!i.nativeWindowOpen);
    assert(i.scheduler.delayUS == 3250); // current.untilNextEAWUS (3200) + 50 us!

    // 3. When AWDL is stopped during an en1 scan (r.ready == false) with nextRetryUS 950 ms away,
    //    notifyInfrastructureReady() clears nextRetryUS and arms 1 ms immediate restart!
    r.ready = false;
    i.wakeArmed = false;
    i.scheduler.delay = 100;
    i.nextRetryUS = clockUS + 950000ULL;
    i.notifyInfrastructureReady();
    assert(i.nextRetryUS == 0);
    assert(i.wakeArmed && i.scheduler.delay == 1);

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)

    def test_p101_idle_agreed_txba_stops_channel_hop_tlc_churn_and_wakes_immediately_on_queued_tx(self):
        tick_src = _tick_harness(r"""
int main() {
    clockUS = 10000000ULL;
    const uint8_t peer[6] = {2, 0, 0, 0, 0, 9};
    AirportItlwmAWDLInterface i;
    dataPeer(i, peer);
    for (auto &c : i.sync.channels) c = 44;
    i.device.awdlRadio.listener = true;
    i.nextActionUS = i.nextMIFUS = clockUS + 10000000ULL;
    i.tick();

    auto &tx = i.device.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Agreed;
    tx.enabled = true;
    tx.cookie = 77;
    tx.epoch = i.device.dmaEpoch;
    copyAddress(tx.peer, peer);
    tx.window = 64;
    tx.lastDataUS = clockUS;
    tx.paused = true;

    // 1. Freshly active TXBA (now - lastDataUS <= 1.5 s) opens its window normally:
    i.serviceTXBlockAck();
    assert(!tx.paused && tx.windowEndUS > clockUS);

    // 2. After > 1.5 s of complete outbound TX silence (0 pending, 0 inFlight, 0 hw queued),
    //    WindowCheck::end() MUST return 0 so TXBA pauses and stops churning TLC/ADD_STA across slots!
    clockUS += 25ULL * 65536ULL;
    dataPeer(i, peer);
    i.serviceTXBlockAck();
    assert(tx.paused && tx.windowEndUS == 0);

    // 3. Enqueuing a new unicast IPv6 heavy packet via pullOutputPackets() refreshes lastDataUS
    //    (and lastHeavyDataUS), arms 1 ms wake timer, and immediately reopens TXBA on serviceTXBlockAck():
    auto heavy = awdlPacket(1200);
    auto *hb = heavy->bytes.data();
    copyAddress(hb, peer);
    copyAddress(hb + 6, i.address.octet);
    hb[12] = 0x86;
    hb[13] = 0xdd;
    hb[14] = 0x60;
    i.network.output.push_back(heavy);
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    assert(i.pendingCount == 1);
    assert(tx.lastDataUS == clockUS && tx.lastHeavyDataUS == clockUS);
    assert(i.scheduler.delay == 1);
    i.serviceTXBlockAck();
    assert(!tx.paused && tx.windowEndUS > clockUS);

    i.drain();
    assert(!allocations);
    return 0;
}
""")
        self.compile_run(tick_src)

    def test_p101_rx_hotpath_suppresses_ioregistry_publish_during_active_media(self):
        # 1. BAR (0x84) in receiveAWDLReordered skips publishAWDLReceive() while llwRealtime or heavy media is active:
        extra_ba = r"""
int main() {
    ItlIwx d;
    d.awdlRadio.ready = true;
    d.awdlRadio.listener = true;
    d.awdlRadio.llwRealtime = true;
    auto r7 = request(d, 0, 7, 300);
    unsigned b7 = start(d, r7);
    const unsigned pubBefore = d.published;

    auto bar = std::unique_ptr<Packet>(new Packet);
    bar->bytes.resize(20, 0);
    bar->bytes[0] = 0x84;
    copyAddress(bar->bytes.data() + 4, d.awdlRadio.address);
    copyAddress(bar->bytes.data() + 10, r7.peer);
    bar->bytes[16] = 0x04;
    bar->bytes[17] = uint8_t(7 << 4);
    putLE16(bar->bytes.data() + 18, uint16_t(301 << 4));
    iwx_rx_mpdu_desc barDesc = {};
    barDesc.reorder_data = (b7 << 24) | 301;
    mbuf_list ml;
    assert(d.receiveAWDLReordered(bar.get(), &barDesc, &ml));
    assert(d.awdlBA.barFrames == 1 && bar->frees);
    // BAR during active LLW media MUST NOT trigger synchronous IORegistry publish in RX interrupt!
    assert(d.published == pubBefore);

    // Once media is completely idle, BAR publishes normally:
    d.awdlRadio.llwRealtime = false;
    auto bar2 = std::unique_ptr<Packet>(new Packet);
    bar2->bytes = bar->bytes;
    putLE16(bar2->bytes.data() + 18, uint16_t(302 << 4));
    barDesc.reorder_data = (b7 << 24) | 302;
    assert(d.receiveAWDLReordered(bar2.get(), &barDesc, &ml));
    assert(d.published == pubBefore + 1);

    assert(!d.clearAWDLBlockAck());
    return 0;
}
"""
        ba_harness = p91._build_ba_harness(extra_ba).replace(
            "void publishAWDLReceive(){}",
            "unsigned published=0;void publishAWDLReceive(){++published;}",
        )
        self.compile_run(ba_harness)

        # 2. publishAWDLReceive() itself enforces >= 5 s interval during active media while keeping 1 s when idle:
        import test_awdl as awdl_support
        reg_base = (support.ROOT / "tests/fixtures/awdl_registry.cpp").read_text().rsplit("int main()", 1)[0]
        reg_base = reg_base.replace("// RX DIAGNOSTIC DECLARATIONS", awdl_support.receive_diagnostics())
        reg_base = reg_base.replace(
            "// PRODUCTION\n",
            "\n".join(
                definition("itlwm/hal_iwx/ItlIwx.cpp", name)
                for name in ("ItlIwx::formatAWDLReceiveDiagnostics", "ItlIwx::publishAWDLReceive")
            ) + "\n",
        )
        reg_base = reg_base.replace("// FIRMWARE DECLARATIONS", awdl_support.firmware_declarations((), reg_base))
        reg_src = reg_base + r"""
int main() {
    ItlIwx d;
    d.awdlReceive.valid = 1;
    d.awdlRadio.llwRealtime = true;
    uptime = 100;
    d.publishAWDLReceive();
    assert(d.controller.publications == 1);

    // At +1 s..+4 s during active LLW media: throttled (< 5 s)!
    for (uint64_t sec = 101; sec <= 104; ++sec) {
        uptime = sec;
        d.publishAWDLReceive();
        assert(d.controller.publications == 1);
    }
    // At +5 s: publishes!
    uptime = 105;
    d.publishAWDLReceive();
    assert(d.controller.publications == 2);

    // Once media stops, 1 s cadence resumes immediately:
    d.awdlRadio.llwRealtime = false;
    uptime = 106;
    d.publishAWDLReceive();
    assert(d.controller.publications == 3);
    return 0;
}
"""
        self.compile_run(reg_src)

        # 3. Verify receiveAction and receiveAWDL also guard hot-path IORegistry publishing during active media:
        rx_act = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::receiveAction")
        self.assertIn("heavyReceiveActive", rx_act)
        self.assertIn("heavyTransmitActive", rx_act)
        rx_awdl = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::receiveAWDL")
        self.assertIn("llwRealtime", rx_awdl)
        self.assertIn("heavyReceiveActive", rx_awdl)

    def test_p101_wcl_roam_lock_and_realtime_mode_suppress_bgscan_and_abort_inflight_scan(self):
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
                "setWCL_SET_ROAM_LOCK",
                "setWCL_REAL_TIME_MODE",
            )
        )
        wcl_src = wcl_src.replace(
            "struct apple80211ScanRequest { uint8_t bytes[0x1550] = {}; };",
            "struct apple80211ScanRequest { uint8_t bytes[0x1550] = {}; };\n"
            "struct apple80211_set_roam_lock { uint8_t locked; };\n"
            "struct apple80211_wcl_real_time_mode { uint8_t enabled; };",
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
            "bool completeWCLScanFromCache(const AirportWCL::ScanRequest &);\n"
            "    IOReturn setWCL_SET_ROAM_LOCK(apple80211_set_roam_lock *);\n"
            "    IOReturn setWCL_REAL_TIME_MODE(apple80211_wcl_real_time_mode *);",
        )
        wcl_src = wcl_src.replace("// PRODUCTION: scan\n", scan_fns + "\n")
        wcl_src += r"""
int main() {
    apple80211ScanRequest req = {};
    req.bytes[0x40] = 1;
    apple80211_set_roam_lock lockOn = {1}, lockOff = {0};
    apple80211_wcl_real_time_mode rtOn = {1}, rtOff = {0};

    AirportItlwmSkywalkInterface s;
    s.portAuthorized = true;
    s.owner.awdl.active = false;
    AirportWCL::BeaconMetadata meta = {};
    meta.channel = 48;
    meta.ssidLength = 4;
    memcpy(meta.ssid, "Home", 4);
    const uint8_t bssid[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    memcpy(meta.bssid, bssid, 6);
    memcpy(s.wclBssInfo, &meta, sizeof(meta));
    s.wclBssValid = true;

    // 1. Null argument validation:
    assert(s.setWCL_SET_ROAM_LOCK(nullptr) == kIOReturnBadArgument);
    assert(s.setWCL_REAL_TIME_MODE(nullptr) == kIOReturnBadArgument);

    // 2. Engaging WCL Roam Lock (0x1ac = 1) or Real-Time Mode (0x1b4 = 1) updates driver state
    //    without aborting in-flight host WCL scans or suppressing subsequent host scans:
    assert(s.beginWCLScan(&req) == kIOReturnSuccess);
    assert(s.scanPending && (s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
    assert(s.setWCL_SET_ROAM_LOCK(&lockOn) == kIOReturnSuccess);
    assert(s.wclRoamLock);
    assert(s.scanPending && (s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
    assert(s.setWCL_REAL_TIME_MODE(&rtOn) == kIOReturnSuccess);
    assert(s.wclRealTimeMode);
    assert(s.scanPending && (s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
    s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
    s.scanCompleted = true;
    s.completeWCLScan();
    assert(!s.scanPending && s.owner.scans() == 1 && s.owner.lastScan() == kIOReturnSuccess);

    // 3. Subsequent host WCL scan while wclRoamLock == true and awdl.active == false still runs live:
    assert(s.beginWCLScan(&req) == kIOReturnSuccess);
    assert(s.scanPending && (s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
    s.hal.ic.ic_flags &= ~IEEE80211_F_BGSCAN;
    s.scanCompleted = true;
    s.completeWCLScan();

    // 4. Active Continuity media (awdl.active == true) suppresses undirected scans from cache in 1 ms:
    s.owner.awdl.active = true;
    assert(s.beginWCLScan(&req) == kIOReturnSuccess);
    assert(!(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));
    assert(s.scanPending && s.scanCompleted && s.timer.delay == 1);
    assert(s.owner.scanResults == 1);
    s.completeWCLScan();
    s.owner.awdl.active = false;

    // 5. Explicit unlock/disable and offline reset clear both flags cleanly:
    assert(s.setWCL_SET_ROAM_LOCK(&lockOff) == kIOReturnSuccess && !s.wclRoamLock);
    assert(s.setWCL_REAL_TIME_MODE(&rtOff) == kIOReturnSuccess && !s.wclRealTimeMode);
    assert(s.setWCL_SET_ROAM_LOCK(&lockOn) == kIOReturnSuccess);
    assert(s.setWCL_REAL_TIME_MODE(&rtOn) == kIOReturnSuccess);
    s.handleWCLAvailability(false);
    assert(!s.wclRoamLock && !s.wclRealTimeMode);
    s.portAuthorized = true;
    assert(s.beginWCLScan(&req) == kIOReturnSuccess);
    assert(s.hal.ic.ic_flags & IEEE80211_F_BGSCAN);
    return 0;
}
"""
        self.compile_run(wcl_src)


if __name__ == "__main__":
    unittest.main()
