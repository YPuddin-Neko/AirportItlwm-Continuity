"""Failing-first ground-truth regressions for Preview 103 (2.4.0d103):
1. Unblock en1 Wi-Fi throughput in XNU Skywalk FQ_CODEL: declare and invoke
   IOSkywalkNetworkInterface::reportDataBandwidths(maxDl, maxUl, effDl, effUl) and
   override AirportItlwmSkywalkInterface::reportDetailedLinkStatus so IO80211InfraInterface's
   legacy AC_80_SISO_PHY_SNR_RATE_TABLE timer never caps en1 at 197.73 Mbps [eff] /
   304.20 Mbps [max], plus populate SNR/NF/CCA in getLINK_CHANGED_EVENT_DATA.
2. Restore AirDrop 80 MHz speed and stop InfraTrafficStateChange -> Infra Priority
   preemptions: remove unconditional en1 recordOutputPacket / recordInputPacket calls
   and remove tick() 0-slot parkIdleOnInfra downgrades.
3. Exit permanent 2-PHY UMAC MCC when Continuity is idle: unbind PHY 1 (Station 3 ->
   queue -> awdlBinding(false)) after the 3.0 s zero-peer idle grace window, probing
   briefly once per 2.0 s (>=96% single-PHY 160 MHz airtime for en1) and rebinding
   in <1 ms upon any peer, OOB request, realtime registration, or queued frame.
4. Fix FaceTime + Sidecar RealTimeGroup (Channel 48) packet loss and clock stalls:
   accept promoted host-election root masters in NativeParentClock::configure +
   SyncEngine::elect, track 24H32 setAWDL_PEER_TRAFFIC_REGISTRATION ("FaceTime" /
   "sidecar" / "airplay") in continuityMediaActive() and proactive IWX_STA_LINK
   provisioning, accept WCL roam-profile RPCs, and throttle back-to-back associated
   AutoJoin bgscan storms when RSSI is strong.
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
    return tick_src + main_body


class Preview103Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p103_en1_bandwidth_reporting_and_no_infra_priority_preemption(self):
        # 1. Verify AirportWCL::computeInfraBandwidths computes gigabit HE/VHT max & effective rates
        #    without overflowing uint32_t if_wifi_status_v1 fields.
        src = r"""
#include <cassert>
#include <cstdint>
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"

int main() {
    // 2x2 VHT 160 MHz (Ch 48 @ 160 MHz): ceiling 1733.3 Mbps, eff >= 1.17 Gbps (>> 197.73 Mbps!)
    const auto vht160 = AirportWCL::computeInfraBandwidths(0, 3, false, true, true, 2);
    assert(vht160.maxBps == 1733300000ULL);
    assert(vht160.effectiveBps >= 1170000000ULL && vht160.effectiveBps < vht160.maxBps);
    assert(vht160.maxBps <= 4000000000ULL);

    // 2x2 HE 160 MHz: ceiling 2402.0 Mbps, eff >= 1.63 Gbps
    const auto he160 = AirportWCL::computeInfraBandwidths(1921500, 3, true, true, true, 2);
    assert(he160.maxBps == 2402000000ULL);
    assert(he160.effectiveBps >= 1630000000ULL && he160.effectiveBps <= he160.maxBps);
    assert(he160.maxBps <= 4000000000ULL);

    // 2x2 VHT 80 MHz: ceiling 866.6 Mbps, eff >= 589 Mbps
    const auto vht80 = AirportWCL::computeInfraBandwidths(866700, 2, false, true, true, 2);
    assert(vht80.maxBps == 866700000ULL);
    assert(vht80.effectiveBps >= 730000000ULL);

    // Extreme negotiated rate clamps safely below UINT32_MAX for if_wifi_status_v1:
    const auto clamped = AirportWCL::computeInfraBandwidths(4800000, 3, true, true, true, 2);
    assert(clamped.maxBps == 4000000000ULL && clamped.effectiveBps <= 4000000000ULL);
    return 0;
}
"""
        self.compile_run(src)

        # 2. Verify macOS15 IOSkywalkNetworkInterface declares reportDataBandwidths(unsigned long long x4)
        #    and AirportItlwmSkywalkInterface overrides reportDetailedLinkStatus + syncInfraDataBandwidths.
        skywalk_net_h = (support.ROOT / "include/Airport/macOS15/IOSkywalkNetworkInterface.h").read_text()
        self.assertIn("reportDataBandwidths(unsigned long long", skywalk_net_h)
        skywalk_if_hpp = (support.ROOT / "AirportItlwm/AirportItlwmSkywalkInterface.hpp").read_text()
        self.assertIn("reportDetailedLinkStatus(", skywalk_if_hpp)
        wcl_cpp = (support.ROOT / "AirportItlwm/WCL/WCL.cpp").read_text()
        self.assertIn("reportDataBandwidths(", wcl_cpp)
        self.assertIn("syncInfraDataBandwidths(", wcl_cpp)

        # 3. Verify unconditional en1 recordOutputPacket / recordInputPacket calls are removed
        #    so gatherInfraStats never triggers InfraTrafficStateChange -> Infra Priority!
        v2_src = (support.ROOT / "AirportItlwm/AirportItlwmV2.cpp").read_text()
        eth_src = (support.ROOT / "AirportItlwm/AirportItlwmEthernetInterface.cpp").read_text()
        self.assertNotIn("recordOutputPacket", v2_src)
        self.assertNotIn("recordInputPacket", eth_src)

    def test_p103_idle_awdl_unbinds_phy1_exiting_mcc_and_preserves_airdrop_80mhz(self):
        # Part A: Radio-level unbindIdle hardware teardown ordering (Station 3 -> queue -> unbind)
        #         and instant <1 ms rebind via prepareAWDLListener + clean stopAWDLRadio while unbound.
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
    assert(d.awdlRadio.ready && d.awdlRadio.mac && d.awdlRadio.binding && d.awdlRadio.station);

    // 1. Enter idle unbind: MUST remove Station 3 + free queue BEFORE unbinding PHY 1!
    d.commands.clear();
    RadioState::GrantPolicy pUnbind = {};
    pUnbind.unbindIdle = true;
    pUnbind.slot = 3;
    assert(!d.serviceAWDLChannel(0, clockUS + 327680, pUnbind));
    assert(d.awdlRadio.ready && d.awdlRadio.mac);
    assert(!d.awdlRadio.binding && !d.awdlRadio.station && d.awdlRadio.queue == IWX_INVALID_QUEUE);
    assert(!d.awdlRadio.dwell.running && d.awdlRadio.idleUnbinds == 1);
    // Verify safe hardware order: remove-station -> free-ring -> unbind
    int idxRmSta = -1, idxFree = -1, idxUnbind = -1;
    for (size_t k = 0; k < d.commands.size(); ++k) {
        if (d.commands[k] == "remove-station") idxRmSta = int(k);
        if (d.commands[k] == "free-ring") idxFree = int(k);
        if (d.commands[k] == "unbind") idxUnbind = int(k);
    }
    assert(idxRmSta >= 0 && idxFree > idxRmSta && idxUnbind > idxFree);

    // 2. Subsequent idle-unbound slots issue ZERO firmware RPCs!
    const int callsWhileUnbound = d.calls;
    for (unsigned s = 4; s <= 15; ++s) {
        clockUS += 65536;
        pUnbind.slot = s;
        assert(!d.serviceAWDLChannel(0, clockUS + 65536, pUnbind));
        assert(!d.awdlRadio.binding && d.calls == callsWhileUnbound);
    }

    // 3. Instant re-bind on Continuity/AirDrop 149@80 demand:
    clockUS += 65536;
    RadioState::GrantPolicy pWide = {};
    pWide.widthMHz = 80;
    pWide.slot = 0;
    assert(!d.serviceAWDLChannel(149, clockUS + 262144, pWide));
    assert(d.awdlRadio.binding && d.awdlRadio.station && d.awdlRadio.queue != IWX_INVALID_QUEUE);
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80 && d.awdlRadio.dwell.running);
    assert(d.awdlRadio.idleRebinds == 1);

    // 4. Unbind again and verify stopAWDLRadio() cleans up PHY 1 + MAC 1 with zero errors!
    clockUS += 262144;
    assert(!d.serviceAWDLChannel(0, clockUS + 65536, pUnbind));
    assert(!d.awdlRadio.binding && d.awdlRadio.idleUnbinds == 2);
    assert(!d.stopAWDLRadio());
    assert(!d.awdlRadio.ready && !d.awdlRadio.mac && !d.awdlRadio.awake);
    return 0;
}
"""
        self.compile_run(p91._build_radio_harness(extra_radio))

        # Part B: Associated listener keeps PHY 1 bound (0 monitorAWEvents gaps) and parks 0-slots
        #         co-channel on infra, while unassociated listener unbinds after 3.0s zero-peer idle
        #         and rebinds immediately on OOB/peer!
        tick_src = _tick_harness(
            r"""
int main() {
    clockUS = 10000000;
    AirportItlwmAWDLInterface i;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.binding = true;
    unsigned unbindTicks = 0, boundTicks = 0, parkInfraTicks = 0;
    i.device.onService = [&]() {
        if (i.device.lastPolicy.parkIdleOnInfra) ++parkInfraTicks;
        if (i.device.lastPolicy.unbindIdle) {
            i.device.awdlRadio.binding = false;
            ++unbindTicks;
        } else {
            i.device.awdlRadio.binding = true;
            ++boundTicks;
        }
    };
    // Configure Low Power schedule [48, 0, 149, 0x5, 6, 0, 149, 0x5]
    const uint16_t lowPower[16] = {48, 0, 149, 0, 0, 0, 0, 0, 6, 0, 149, 0, 0, 0, 0, 0};
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = lowPower[s];
        i.sync.channelClasses[s] = 0;
    }
    i.sequenceConfigured = true;
    i.tick();
    assert(i.running && i.device.awdlRadio.binding);

    // 1. While associated on en1 (ns_nstate == IEEE80211_S_RUN), stays bound across > 4s
    //    and parks all twelve 0-slots per 16-slot cycle on en1's infra channel:
    unbindTicks = boundTicks = parkInfraTicks = 0;
    for (unsigned step = 0; step < 64; ++step) {
        clockUS += 65536;
        i.tick();
        assert(!i.device.lastPolicy.unbindIdle);
    }
    assert(unbindTicks == 0 && boundTicks == 64 && parkInfraTicks == 48);

    // 2. When unassociated (ns_nstate != IEEE80211_S_RUN), unbinds after 3.0s zero-peer idle:
    i.device.com.ns_nstate = 0;
    for (unsigned step = 0; step < 56; ++step) {
        clockUS += 65536;
        i.tick();
    }
    unbindTicks = boundTicks = parkInfraTicks = 0;
    for (unsigned step = 0; step < 64; ++step) {
        clockUS += 65536;
        i.tick();
    }
    assert(parkInfraTicks == 0);
    assert(unbindTicks >= 58 && boundTicks >= 1 && boundTicks <= 6);

    // Arrival of an OOB broadcast request MUST rebind immediately (<1 ms) and keep bound!
    auto payload = umiPayload();
    auto oob = broadcastRequest(payload, 0xffffffff, 0);
    assert(i.configureBroadcast(oob, false) == kIOReturnSuccess);
    clockUS += 1000;
    i.tick();
    assert(!i.device.lastPolicy.unbindIdle && i.device.awdlRadio.binding);
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

    def test_p103_native_parent_clock_promoted_root_and_sync_engine_elect(self):
        src = r"""
#include <cassert>
#include <cstring>
#include "AirportItlwm/AWDL/NativeFrame.hpp"
using namespace AirportAWDL;

static void stamp(uint8_t *out, uint64_t time) {
    for (unsigned i = 0; i < 4; ++i) out[i] = uint32_t(time) >> (24 - 8 * i);
}

int main() {
    const uint8_t macSelf[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    const uint8_t iphoneParent[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
    const uint8_t ipadNewRoot[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x04};
    const uint8_t unknownBogus[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x99};

    const uint64_t received = 5000000ULL, now = received + 15000ULL;
    Advertisement a = {};
    a.fields = Sync | Channels | ElectionV2 | Version;
    a.windowPeriodTU = 16;
    a.presenceMode = 4;
    a.windowCounter = 200;
    a.nextWindowTU = 64;
    a.masterChannel = 149;
    a.phyTxTime = 5000000;
    a.targetTxTime = 4998000;
    copyAddress(a.syncMaster, iphoneParent);
    copyAddress(a.electionMaster, iphoneParent);
    copyAddress(a.electionSync, iphoneParent);
    a.electionID = 42;
    a.distance = 0;
    a.masterMetric = 460;
    a.selfMetric = 460;
    a.sequence.count = 16;
    a.sequence.encoding = 3;
    a.sequence.step = 3;
    a.sequence.fill = 0xffff;
    for (unsigned s = 0; s < 16; ++s) a.sequence.channel[s] = (s == 2 || s == 10) ? 149 : 48;

    PeerTable table = {};
    assert(table.update(iphoneParent, macSelf, a, now, 48, -42, now - received));

    // Host election promotes ipadNewRoot via setAWDL_ELECTION_PARAMS + setAWDL_SYNC_STATE,
    // then calls setAWDL_AWDL_ADVERTISERS before iphoneParent's next Sync TLV arrives:
    SyncEngine local = {};
    local.start(macSelf, 149, received - 100000);
    // Simulate native election TLV + setAWDL_ELECTION_PARAMS + setAWDL_SYNC_STATE:
    uint8_t tmpl[43] = {};
    tmpl[0] = 17; tmpl[1] = 40; tmpl[2] = 0; // ElectionParametersV2 TLV
    copyAddress(tmpl + 3, ipadNewRoot);
    copyAddress(tmpl + 9, iphoneParent);
    putLE32(tmpl + 15, 42); // electionID
    tmpl[19] = 1; // distance = 1
    assert(local.configureTemplate(tmpl, sizeof(tmpl)));
    local.electionID = 42;
    local.distance = 1;
    copyAddress(local.master, ipadNewRoot);
    local.configureSyncState(1, 0, iphoneParent);

    uint8_t record[44] = {};
    putLE16(record, 200);
    putLE16(record + 2, 64);
    putLE16(record + 4, 2000);
    putLE16(record + 6, 110);
    putLE16(record + 8, 16);
    putLE16(record + 10, 16);
    putLE16(record + 12, 16);
    record[16] = 4;
    stamp(record + 20, received);
    copyAddress(record + 32, ipadNewRoot);
    copyAddress(record + 38, iphoneParent);

    // 1. Unknown bogus root (not expectedRoot, not self, not in PeerTable) MUST still fail with Root:
    uint8_t bogusRec[44];
    memcpy(bogusRec, record, 44);
    copyAddress(bogusRec + 32, unknownBogus);
    AdvertiserFailure fail = AdvertiserFailure::None;
    assert(local.nativeParent.configure(bogusRec, table, now, &fail, local.master, local.address) ==
           AdvertiserResult::Unavailable);
    assert(fail == AdvertiserFailure::Root);

    // 2. Host-elected ipadNewRoot MUST succeed AND local.elect() MUST adopt clock immediately!
    assert(local.nativeParent.configure(record, table, now, &fail, local.master, local.address) ==
           AdvertiserResult::Applied);
    assert(fail == AdvertiserFailure::None);
    local.elect(table, now);
    assert(local.transmitReady(now));
    assert(local.epochUS == received - 2000 && local.epochCounter == 200);
    return 0;
}
"""
        self.compile_run(src)

    def test_p103_realtime_registration_peer_station_and_associated_bgscan_cooldown(self):
        # 1. Verify 24H32 setAWDL_PEER_TRAFFIC_REGISTRATION ("FaceTime" / "sidecar" vs
        #    "wifid-assisted-discovery") updates realtimeSessionMask, continuityMediaActive(),
        #    and realtimePeers without dereferencing inline peer_addr[6] as a pointer!
        src = r"""
#include <cassert>
#include <cstring>
#include "AirportItlwm/AWDL/RadioState.hpp"

int main() {
    AirportAWDL::RadioState r = {};
    const uint8_t iphone[6] = {0xaa, 0xbb, 0xcc, 0x11, 0x22, 0x33};
    const uint8_t ipad[6]   = {0xaa, 0xbb, 0xcc, 0x44, 0x55, 0x66};

    // 24H32 wire buffer (184 bytes): +0x04 peer_addr[6], +0x0c name_len, +0x10 name, +0xa8 active
    uint8_t raw[184] = {};
    memcpy(raw + 4, iphone, 6);
    const char *disc = "wifid-assisted-discovery";
    uint32_t len = uint32_t(strlen(disc));
    memcpy(raw + 0x0c, &len, 4);
    memcpy(raw + 0x10, disc, len);
    uint32_t active = 1;
    memcpy(raw + 0xa8, &active, 4);

    // Discovery registration MUST NOT activate realtimeSessionMask:
    assert(!r.updateRealtimeRegistration(raw, sizeof(raw)));
    assert(r.realtimeSessionMask == 0);

    // "FaceTime" registration MUST set bit 1 and store iphone MAC:
    memset(raw, 0, sizeof(raw));
    memcpy(raw + 4, iphone, 6);
    const char *ft = "FaceTime";
    len = uint32_t(strlen(ft));
    memcpy(raw + 0x0c, &len, 4);
    memcpy(raw + 0x10, ft, len);
    active = 1;
    memcpy(raw + 0xa8, &active, 4);
    assert(r.updateRealtimeRegistration(raw, sizeof(raw)));
    assert(r.realtimeSessionMask == (1U << 1));
    assert(r.hasRealtimePeer(iphone));

    // Concurrent "sidecar" registration MUST keep BOTH bits (FaceTime + Sidecar together!):
    memset(raw, 0, sizeof(raw));
    memcpy(raw + 4, ipad, 6);
    const char *sc = "sidecar";
    len = uint32_t(strlen(sc));
    memcpy(raw + 0x0c, &len, 4);
    memcpy(raw + 0x10, sc, len);
    active = 1;
    memcpy(raw + 0xa8, &active, 4);
    assert(r.updateRealtimeRegistration(raw, sizeof(raw)));
    assert(r.realtimeSessionMask == ((1U << 0) | (1U << 1)));
    assert(r.hasRealtimePeer(iphone) && r.hasRealtimePeer(ipad));

    // Deactivating FaceTime leaves Sidecar active until Sidecar also stops:
    memset(raw, 0, sizeof(raw));
    memcpy(raw + 4, iphone, 6);
    len = uint32_t(strlen(ft));
    memcpy(raw + 0x0c, &len, 4);
    memcpy(raw + 0x10, ft, len);
    active = 0;
    memcpy(raw + 0xa8, &active, 4);
    assert(r.updateRealtimeRegistration(raw, sizeof(raw)));
    assert(r.realtimeSessionMask == (1U << 0));
    assert(!r.hasRealtimePeer(iphone) && r.hasRealtimePeer(ipad));
    return 0;
}
"""
        self.compile_run(src)

        # 2. Verify WCL associated bgscan cooldown helper: allows first scan immediately,
        #    throttles back-to-back <45s AutoJoin scans when RSSI >= -68 dBm, and never
        #    throttles weak-RSSI roam scans (< -68 dBm)!
        wcl_throttle_src = r"""
#include <cassert>
#include <cstdint>
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#include "AirportItlwm/WCL/WCL.hpp"

int main() {
    uint64_t lastScanUS = 0;
    // First scan at boot/assoc (lastScanUS == 0): MUST allow live scan and record timestamp!
    assert(!AirportWCL::shouldThrottleAssociatedBgscan(lastScanUS, 10000000ULL, -44));
    lastScanUS = 10000000ULL;

    // Back-to-back AutoJoin scan 14 ms later at -44 dBm: MUST throttle (complete from cache)!
    assert(AirportWCL::shouldThrottleAssociatedBgscan(lastScanUS, 10014000ULL, -44));

    // Weak signal (-72 dBm < -68 dBm): MUST NOT throttle so genuine roam finds a better AP!
    assert(!AirportWCL::shouldThrottleAssociatedBgscan(lastScanUS, 12000000ULL, -72));

    // After 45 s cooldown at -44 dBm: MUST allow fresh periodic scan!
    assert(!AirportWCL::shouldThrottleAssociatedBgscan(lastScanUS, 56000000ULL, -44));
    return 0;
}
"""
        self.compile_run(wcl_throttle_src)

        # 3. Verify WCL roam profile setters return kIOReturnSuccess instead of kIOReturnUnsupported,
        #    and ItlIwx::serviceAWDLBlockAck provisions realtimePeers via hasRealtimePeer!
        skywalk_hpp = (support.ROOT / "AirportItlwm/AirportItlwmSkywalkInterface.hpp").read_text()
        self.assertNotIn(
            "setWCL_ROAM_PROFILE_CONFIG(apple80211_roam_profile_config *) AIRPORT_SKYWALK_OVERRIDE { return kIOReturnUnsupported; }",
            skywalk_hpp,
        )
        ba_body = definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::serviceAWDLBlockAck")
        self.assertIn("hasRealtimePeer", ba_body)


if __name__ == "__main__":
    unittest.main()
