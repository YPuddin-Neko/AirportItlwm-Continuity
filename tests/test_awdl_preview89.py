"""Failing-first regressions for Preview 89:
1. Native Skywalk llw0 registration sets IFNET_SUBFAMILY_WIFI (3) at RegistrationInfo+0x0c
   so libnetwork classifies llw0 as Wi-Fi and satisfies wireless Continuity Camera nw_path.
2. Bounded 2.4 GHz social-slot parking (SocialParkIntervalUS = 2s) keeps PHY 1 & 80 MHz TLC hot
   during active heavy Sidecar TX while still visiting ch 6 every 2s for discovery.
3. txReady() drains pre-staged pending[] first and refills past the old pendingCount < 32 cap,
   and tick() pulls waiting awdl0 packets synchronously on channel reopen.
4. tick() aligns timer wakeups to parked slot end and the 3 ms channel-start guard (<= 1-3 ms
   instead of blind 5 ms polling).
"""
import re
import unittest

import test_awdl_preview82 as p82
import test_regressions as support
from iwx_firmware import declarations as firmware_declarations
from test_offline import definition


class Preview89Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p89_llw_skywalk_registration_sets_wifi_subfamily(self):
        reg_fn = definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmLLWInterface::registerSkywalk",
        )
        # Runtime C++ verification of exact RegistrationInfo byte layout written by registerSkywalk().
        src = f"""
#include <cassert>
#include <cstdint>
#include <cstring>

using IOReturn = int;
using UInt32 = uint32_t;
enum {{ kIOReturnSuccess = 0, kIOSkywalkPacketTypeNetwork = 1 }};
static const void *kOSBooleanTrue = reinterpret_cast<const void *>(1);
static inline bool unicastAddress(const uint8_t a[6]) {{ return (a[0] & 1) == 0 && (a[0]|a[1]|a[2]|a[3]|a[4]|a[5]); }}

struct ether_addr {{ uint8_t octet[6]; }};
struct IOSkywalkPacket {{}};
struct IOSkywalkPacketQueue {{
    void enable() {{}}
    void disable() {{}}
    void release() {{}}
}};
struct IOSkywalkPacketBufferPool {{
    struct PoolOptions {{
        uint32_t packetCount, bufferCount, bufferSize, maxBuffersPerPacket, memorySegmentSize, poolFlags;
    }};
    static IOSkywalkPacketBufferPool *withName(const char *, void *, int, const PoolOptions *) {{
        static IOSkywalkPacketBufferPool pool;
        return &pool;
    }}
    void release() {{}}
}};
struct IOSkywalkTxSubmissionQueue : IOSkywalkPacketQueue {{
    using Action = unsigned int(*)(void *, IOSkywalkTxSubmissionQueue *, IOSkywalkPacket * const *, unsigned int, void *);
    static IOSkywalkTxSubmissionQueue *withPool(IOSkywalkPacketBufferPool *, uint32_t, uint32_t, void *, Action, void *, uint32_t) {{
        static IOSkywalkTxSubmissionQueue q;
        return &q;
    }}
}};
struct IOSkywalkTxCompletionQueue : IOSkywalkPacketQueue {{
    static IOSkywalkTxCompletionQueue *withPool(IOSkywalkPacketBufferPool *, uint32_t, uint32_t, void *, void *, void *, uint32_t) {{
        static IOSkywalkTxCompletionQueue q;
        return &q;
    }}
}};
struct IOSkywalkRxCompletionQueue : IOSkywalkPacketQueue {{
    static IOSkywalkRxCompletionQueue *withPool(IOSkywalkPacketBufferPool *, uint32_t, uint32_t, void *, void *, void *, uint32_t) {{
        static IOSkywalkRxCompletionQueue q;
        return &q;
    }}
}};
struct IOSkywalkEthernetInterface {{
    struct RegistrationInfo {{ uint8_t bytes[0x130]; }};
}};
struct IOWorkLoop {{
    IOReturn addEventSource(void *) {{ return kIOReturnSuccess; }}
    void removeEventSource(void *) {{}}
}};
#define OSSafeReleaseNULL(x) do {{ (x) = nullptr; }} while (0)

struct AirportItlwmLLWInterface {{
    ether_addr address = {{{{0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01}}}};
    bool skywalkRegistered = false;
    IOSkywalkPacketBufferPool *skywalkPool = nullptr;
    IOSkywalkTxSubmissionQueue *txSubQueue = nullptr;
    IOSkywalkTxCompletionQueue *txCompQueue = nullptr;
    IOSkywalkRxCompletionQueue *rxCompQueue = nullptr;
    IOWorkLoop *queueWorkLoop = nullptr;
    IOSkywalkEthernetInterface::RegistrationInfo captured = {{}};
    bool registeredCalled = false;

    static unsigned int txSubmissionAction(void *, IOSkywalkTxSubmissionQueue *, IOSkywalkPacket * const *, unsigned int, void *) {{ return 0; }}
    bool initRegistrationInfo(IOSkywalkEthernetInterface::RegistrationInfo *info, uint32_t ver, size_t sz) {{
        memset(info, 0, sizeof(*info));
        uint32_t v = ver, s = uint32_t(sz), fam = 2, sub = 0;
        memcpy(info->bytes + 0x00, &v, 4);
        memcpy(info->bytes + 0x04, &s, 4);
        memcpy(info->bytes + 0x08, &fam, 4);
        memcpy(info->bytes + 0x0c, &sub, 4);
        return true;
    }}
    IOReturn registerEthernetInterface(const IOSkywalkEthernetInterface::RegistrationInfo *info,
                                       IOSkywalkPacketQueue **, uint32_t,
                                       IOSkywalkPacketBufferPool *, IOSkywalkPacketBufferPool *, uint32_t) {{
        captured = *info;
        registeredCalled = true;
        return kIOReturnSuccess;
    }}
    void deregisterSkywalk() {{}}
    void setProperty(const char *, const void *) {{}}
    bool registerSkywalk(IOWorkLoop *wl);
}};

{reg_fn}

int main() {{
    AirportItlwmLLWInterface llw;
    IOWorkLoop wl;
    assert(llw.registerSkywalk(&wl));
    assert(llw.registeredCalled);
    uint32_t family = 0, subFamily = 0;
    int32_t unit = -1;
    memcpy(&family, llw.captured.bytes + 0x08, 4);
    memcpy(&subFamily, llw.captured.bytes + 0x0c, 4);
    memcpy(&unit, llw.captured.bytes + 0x38, 4);
    assert(family == 2);
    assert(subFamily == 3); // IFNET_SUBFAMILY_WIFI -> type: Wi-Fi, nw_interface_type_wifi(1)
    assert(unit == 0);
    assert(memcmp(llw.captured.bytes + 0x108, llw.address.octet, 6) == 0);
    return 0;
}}
"""
        self.compile_run(src)

        # Preserve Preview 85 Skywalk RX safety invariants.
        llw_rx = definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmLLWInterface::inputSkywalkEthernet",
        )
        self.assertIn("prepare(nullptr", llw_rx)
        self.assertIn("isQueueReady()", llw_rx)

    def test_p89_bounded_2g_social_park_preserves_80mhz_tlc_and_visits_periodically(self):
        radio_src = (support.ROOT / "tests/fixtures/awdl_radio.cpp").read_text().rsplit("int main()", 1)[0]
        routines = (
            "awdlMAC", "awdlChannelAllowed", "awdlWideChannelAllowed", "awdlPHY", "awdlBinding",
            "receiveAWDLSession", "finishAWDLDwell", "serviceAWDLChannel",
            "prepareAWDLListener", "closeAWDLListenerWindow", "serviceAWDLListener",
            "configureAWDLPeerWidth", "startAWDLRadio", "awdlRemoveQueue",
            "stopAWDLRadio", "resetAWDLRadio", "awdlTxWindowOpen", "transmitAWDL",
            "completeAWDLTx", "sampleAWDLRate", "noteAWDLTXBlockAck",
            "awdlTXAntenna", "awdlTXMcs", "awdlTXWidth",
        )
        radio_src = radio_src.replace(
            "// PEER TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_transport.cpp").read_text(),
        )
        radio_src = radio_src.replace(
            "// PRODUCTION",
            "\n".join(definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::" + n) for n in routines),
        )
        radio_src = radio_src.replace(
            '#include "tests/fixtures/awdl_transport.hpp"',
            (support.ROOT / "tests/fixtures/awdl_transport.hpp").read_text(),
        )
        radio_src = radio_src.replace(
            '#include "itlwm/hal_iwx/AWDLTxBudget.hpp"',
            (support.ROOT / "itlwm/hal_iwx/AWDLTxBudget.hpp").read_text(),
        )
        transport = "\n".join(
            definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name)
            for name in (
                "iwx_rval2ridx", "iwx_toggle_tx_ant", "iwx_tx_fill_awdl_cmd", "iwx_tx_update_byte_tbl",
                "iwx_tx", "iwx_rx_tx_cmd_single", "iwx_rx_tx_cmd", "iwx_txd_done",
                "iwx_ampdu_txq_advance", "iwx_reset_tx_ring",
            )
        )
        transport += "\n" + definition("itlwm/hal_iwx/ItlIwx.cpp", "iwx_clear_tx_desc")
        transport += "\n" + definition("itlwm/hal_iwx/ItlIwx.cpp", "iwx_awdl_is_quz_0034")
        driver = (support.ROOT / "itlwm/hal_iwx/ItlIwx.cpp").read_text()
        table = re.search(r"const struct iwx_rate \{.*?\} iwx_rates\[\] = \{.*?^\};", driver, re.S | re.M)[0]
        net = (support.ROOT / "itl80211/openbsd/net80211/ieee80211.c").read_text()
        rates = re.search(r"const struct ieee80211_rateset ieee80211_std_rateset_11a =.*?;", net, re.S)[0]
        radio_src = radio_src.replace("struct ItlIwx {", table + rates + "\nstruct ItlIwx {")
        radio_src = radio_src.replace("static const uint8_t self", transport + "\nstatic const uint8_t self")
        radio_src = radio_src.replace(
            "// TRANSPORT TYPES",
            "void iwx_clear_tx_desc(struct iwx_softc *,struct iwx_tx_ring *,int);",
        )
        radio_src = radio_src.replace(
            "// FIRMWARE DECLARATIONS",
            firmware_declarations(
                (
                    "iwx_mac_data_ap", "iwx_mac_data_ibss", "iwx_mac_data_sta", "iwx_mac_data_go",
                    "iwx_mac_data_p2p_sta", "iwx_mac_data_pibss", "iwx_mac_data_p2p_dev", "iwx_ac_qos",
                    "iwx_mac_ctx_cmd", "iwx_fw_channel_info", "iwx_phy_context_cmd", "iwx_binding_cmd",
                    "iwx_rlc_properties", "iwx_sad_properties", "iwx_rlc_config_cmd",
                    "iwx_device_power_cmd", "iwx_add_sta_cmd", "iwx_rm_sta_cmd",
                    "iwx_session_prot_cmd", "iwx_session_prot_notif", "iwx_tx_data", "iwx_tfh_tb", "iwx_tfh_tfd",
                    "iwx_cmd_header", "iwx_cmd_header_wide", "iwx_device_cmd", "iwx_dram_sec_info",
                    "iwx_tx_cmd_gen2", "iwx_tx_cmd_gen3", "iwx_agn_scd_bc_tbl", "iwx_gen3_bc_tbl",
                    "iwx_agg_tx_status", "iwx_tx_resp",
                ),
                radio_src,
                ("iwx_gen2_tx_fifo",),
            ),
        )
        extra = r"""
static void preview89SocialPark() {
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
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 9};
    auto &peer = d.awdlPeers.peers[0];
    peer = {};
    peer.occupied = true;
    AirportAWDL::copyAddress(peer.address, peerAddr);
    peer.haveSync = true;
    peer.haveSequence = true;
    peer.metadata.fields = AirportAWDL::HTCapabilities;
    peer.metadata.rxHTMcs = 0xff;
    auto refreshPeer = [&](uint64_t t) {
        peer.lastSeenUS = peer.syncReceivedUS = peer.sequenceReceivedUS = peer.htCapabilitiesReceivedUS = t;
    };
    refreshPeer(clockUS);

    auto &tx = d.awdlRadio.blockAckTX;
    tx.configured = true; tx.epoch = d.dmaEpoch;
    tx.phase = AirportAWDL::TXBlockAckQueue::Agreed;
    AirportAWDL::copyAddress(tx.peer, peerAddr);
    tx.acknowledged = 16;
    tx.enabled = true; tx.paused = false; tx.widthMHz = 80;
    tx.lastHeavyDataUS = clockUS;

    RadioState::GrantPolicy p80 = {}; p80.widthMHz = 80; p80.slot = 1;
    assert(!d.serviceAWDLChannel(149, clockUS + 96000, p80));

    // 1. Ch 6 visit tunes to ch 6 for 2.4 GHz discovery and records visit time.
    RadioState::GrantPolicy p20 = {}; p20.widthMHz = 20; p20.slot = 2;
    assert(!d.serviceAWDLChannel(6, clockUS + 16000, p20));
    assert(d.awdlRadio.channel == 6 && d.awdlRadio.widthMHz == 20 && tx.widthMHz == 20);
    assert(d.awdlRadio.lastWideSocialVisitUS == clockUS);

    // Return to 149@80 and restore TLC width.
    clockUS += 16000;
    refreshPeer(clockUS);
    tx.lastHeavyDataUS = clockUS;
    tx.widthMHz = 80;
    p80.slot = 3;
    assert(!d.serviceAWDLChannel(149, clockUS + 96000, p80));
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

    // 2. Subsequent ch 6 slots MUST ALWAYS tune to ch 6 (socialParks == 0) so 2.4 GHz PSFs/MIFs are never dropped!
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        clockUS += 262144;
        refreshPeer(clockUS);
        tx.lastHeavyDataUS = clockUS;
        p20.slot = uint16_t(10 + cycle * 2);
        assert(!d.serviceAWDLChannel(6, clockUS + 16000, p20));
        assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 6 && d.awdlRadio.widthMHz == 20);
        assert(d.awdlRadio.blockAckTXStats.socialParks == 0);
        assert(d.awdlRadio.lastWideSocialVisitUS == clockUS);
        clockUS += 16000;
        p80.slot = uint16_t(11 + cycle * 2);
        assert(!d.serviceAWDLChannel(149, clockUS + 96000, p80));
        tx.widthMHz = 80;
        assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);
    }

    assert(!d.stopAWDLRadio());
    d.resetAWDLRadio();
    assert(d.awdlRadio.lastWideSocialVisitUS == 0);
}

int main() {
    preview89SocialPark();
    return 0;
}
"""
        self.compile_run(radio_src + extra)
        awdl_if = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.cpp").read_text()
        self.assertIn('"TXBlockAckSocialParks"', awdl_if)

    def test_p89_txready_refills_past_32_pending_and_tick_aligns_timer(self):
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        extra_routines = "\n".join(
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
            for n in ("submitOutputPackets", "drainPendingData", "txReady")
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines() + "\n" + extra_routines)
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src = tick_src.replace("} pending[64]={};", "} pending[96]={};")
        tick_src = tick_src.replace("inFlight[64]={};", "inFlight[96]={};")
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

    // 1. Open active ch 149 window.
    i.tick();
    assert(i.device.awdlTxWindowOpen());

    // Pre-fill 32 frames into pending[] and 40 frames waiting in BSD FQ_CODEL queue,
    // simulating BA completion freeing HW slots while pendingCount == 32.
    for (unsigned n = 0; n < 32; ++n) {
        i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    }
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    assert(i.pendingCount == 32 && i.inFlightCount == 0);
    for (unsigned n = 0; n < 40; ++n) {
        i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    }
    // Simulate 32 frames still in flight in HW, leaving 16 slots on non-BA queue (max 48) or up to 64 total.
    // Call txReady(&i): MUST drain pre-staged pending[] into HW AND immediately pull from network.output!
    const size_t beforeOutput = i.network.output.size();
    AirportItlwmAWDLInterface::txReady(&i);
    assert(i.device.data >= 32);
    assert(i.network.output.size() < beforeOutput); // P88 failed here: pendingCount < 32 blocked pull!

    // 2. Smart tick() timer when listener is parked (!dwell.running) near end of slot:
    //    Schedule [149, 0, 149...] so slot 1 has !dwell.running with 2.2 ms left before slot 2 (ch 149).
    i.drain();
    for (auto *p : i.network.output) mbuf_freem(p);
    i.network.output.clear();
    i.device.awdlPeers = {};
    i.device.awdlRadio.dwell.running = false;
    i.device.awdlRadio.channel = 149;
    i.device.awdlRadio.widthMHz = 20;
    for (unsigned s = 0; s < 16; ++s) { i.sync.channels[s] = 149; i.sync.channelClasses[s] = 0; }
    i.sync.channels[1] = 0;
    i.sync.epochUS = clockUS;
    i.sync.epochCounter = 0;
    const uint64_t eawUS = uint64_t(i.sync.presence) * i.sync.periodTU * 1024;
    // Place clock 2200 us before slot 2 (ch 149) starts, with radio parked (!dwell.running).
    clockUS = i.sync.epochUS + 2 * eawUS - 2200;
    i.scheduler.delay = 99;
    i.tick();
    assert(!i.device.awdlRadio.dwell.running);
    assert(i.scheduler.delay == 3); // Ceiling of 2200 us (P88 slept blind 5 ms here!)

    // 3. Smart tick() timer during 3 ms channel-start guard (sinceChannelStartUS == 500 us) with pending data:
    //    MUST wake at <= 3 ms instead of blind 5 ms!
    clockUS = i.sync.epochUS + 2 * eawUS + 500;
    i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    assert(i.pendingCount == 1);
    i.scheduler.delay = 99;
    i.tick();
    assert(i.device.awdlRadio.dwell.running);
    assert(i.scheduler.delay <= 3); // P88 slept blind 5 ms here!

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run("#define DEBUG 0\n" + tick_src)


if __name__ == "__main__":
    unittest.main()
