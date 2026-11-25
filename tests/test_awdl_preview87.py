"""Failing-first regressions for Preview 87:
1. Principled non-overlapping 80 MHz on quz0034 + self-healing wide circuit breaker.
2. Complete 80->20 MHz shrink ordering: TLC downgrade -> STA FAT_EN downgrade -> PHY shrink (even with stale peer AF).
3. Non-infra 5 GHz AWDL channel-sequence 80 MHz promotion (never colliding with infra AP channel block).
4. Zero-bubble synchronous TX drain (outputStart -> submitOutputPackets) + interrupt-driven post-BA-advance refill (awdlTxReady).
5. AWDL-scoped 40 ms RX reorder timeout (en1 stays 100 ms), peak TLC / RX rate telemetry, and firmware assert telemetry.
"""
import re
import unittest

import test_awdl_preview82 as p82
import test_regressions as support
from iwx_firmware import declarations as firmware_declarations
from test_offline import definition


class Preview87Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p87_quz0034_non_overlapping_80mhz_and_circuit_breaker(self):
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
static void preview87QuzWideAndBreaker() {
    // 1. Pure overlap helper checks.
    assert(RadioState::wide80Block(48) == 36);
    assert(RadioState::wide80Block(44) == 36);
    assert(RadioState::wide80Block(149) == 149);
    assert(RadioState::wide80Block(6) == 0);
    assert(RadioState::wide80OverlapsInfra(48, 48, true));
    assert(RadioState::wide80OverlapsInfra(44, 48, true));
    assert(RadioState::wide80OverlapsInfra(44, 52, true));
    assert(!RadioState::wide80OverlapsInfra(44, 52, false));
    assert(!RadioState::wide80OverlapsInfra(149, 48, true));
    assert(!RadioState::wide80OverlapsInfra(149, 6, false));

    auto allowWide = [](ItlIwx &d) {
        for (unsigned c : {36U, 40U, 44U, 48U, 149U, 153U, 157U, 161U}) {
            d.com.sc_ic.ic_channels[c] = {IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_VHT80, uint16_t(5000 + 5 * c)};
            d.com.sc_ic.ic_chan_active[c / 8] |= 1U << (c % 8);
        }
    };

    // 2. On quz0034 with en1 on ch 48, ch 149 runs at 80 MHz while overlapping ch 48 / ch 44 stay at 20 MHz.
    ItlIwx q;
    allowWide(q);
    q.rlcVersion = 2;
    q.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    q.com.sc_cfg = &iwl9560_quz_a0_jf_b0_cfg;
    q.awdlRadio.quz0034 = iwx_awdl_is_quz_0034(0x02f0, 0x8086, 0x0034, q.com.sc_cfg);
    assert(q.awdlRadio.quz0034);
    q.com.sc_phyctxt[0].channel = &q.com.sc_ic.ic_channels[48];
    assert(!q.startAWDLRadio(self));
    assert(q.awdlRadio.channel == 149 && q.awdlRadio.widthMHz == 80);

    RadioState::GrantPolicy req80 = {};
    req80.widthMHz = 80;
    assert(!q.serviceAWDLChannel(48, clockUS + 65536, req80));
    assert(q.awdlRadio.channel == 48 && q.awdlRadio.widthMHz == 20);
    assert(!q.serviceAWDLChannel(44, clockUS + 65536, req80));
    assert(q.awdlRadio.channel == 44 && q.awdlRadio.widthMHz == 20);
    assert(!q.serviceAWDLChannel(149, clockUS + 65536, req80));
    assert(q.awdlRadio.channel == 149 && q.awdlRadio.widthMHz == 80);

    // 3. Self-healing circuit breaker: 1 fault still allows 80 MHz; >= 2 faults falls back to 20 MHz and survives resetAWDLRadio().
    q.awdlRadio.fwHealth.wideFaultCount = 1;
    assert(!q.awdlRadio.wideCircuitTripped());
    assert(!q.serviceAWDLChannel(149, clockUS + 65536, req80));
    assert(q.awdlRadio.widthMHz == 80);
    q.awdlRadio.fwHealth.wideFaultCount = 2;
    assert(q.awdlRadio.wideCircuitTripped());
    assert(!q.serviceAWDLChannel(6, clockUS + 65536, {}));
    assert(!q.serviceAWDLChannel(149, clockUS + 65536, req80));
    assert(q.awdlRadio.widthMHz == 20);
    assert(!q.stopAWDLRadio());
    q.resetAWDLRadio();
    assert(q.awdlRadio.fwHealth.wideFaultCount == 2 && q.awdlRadio.wideCircuitTripped());

    // 4. RX rate decoder accuracy on v1 (QuZ/22000) and v2 (AX210+) rate_n_flags.
    RadioState rx = {};
    // v1 HT20 MCS7 2SS SGI: bit8=HT, bit3=NSS2, code=7, bw=0, bit13=SGI -> 144444 kbps
    rx.recordRXRate((1U << 8) | (1U << 3) | 7U | (1U << 13), false);
    assert(rx.lastRXRateKbps == 144444U && rx.peakRXRateKbps == 144444U);
    // v1 VHT80 MCS9 2SS SGI: bit26=VHT, bit4=NSS2, code=9, bw=2(<<11), bit13=SGI -> 866666 kbps
    rx.recordRXRate((1U << 26) | (1U << 4) | 9U | (2U << 11) | (1U << 13), false);
    assert(rx.lastRXRateKbps == 866666U && rx.peakRXRateKbps == 866666U);
    // v1 OFDM 6 Mbps: plcp=13 -> last drops to 6000 while peak stays 866666
    rx.recordRXRate(13U, false);
    assert(rx.lastRXRateKbps == 6000U && rx.peakRXRateKbps == 866666U);
    // v2 VHT80 MCS9 2SS SGI: mod=3(<<8), bit4=NSS2, code=9, bw=2(<<11), bit20=SGI -> 866666 kbps
    RadioState rx2 = {};
    rx2.recordRXRate((3U << 8) | (1U << 4) | 9U | (2U << 11) | (1U << 20), true);
    assert(rx2.lastRXRateKbps == 866666U && rx2.peakRXRateKbps == 866666U);
}

int main() {
    preview87QuzWideAndBreaker();
    return 0;
}
"""
        self.compile_run(radio_src + extra)

    def test_p87_width_shrink_downgrades_tlc_then_sta_before_phy(self):
        source = (support.ROOT / "tests/fixtures/awdl_ba.cpp").read_text().rsplit("int main()", 1)[0]
        hal = (
            "awdlMAC", "awdlChannelAllowed", "awdlWideChannelAllowed", "awdlPHY", "awdlBinding",
            "prepareAWDLListener", "closeAWDLListenerWindow", "serviceAWDLListener",
            "addAWDLBlockAckPeer", "awdlHasRXBA", "awdlRXBAOwned", "awdlBlockAckLive",
            "removeAWDLBlockAckTID", "removeAWDLBlockAckPeer", "clearAWDLBlockAck",
            "resetAWDLBlockAck", "prepareAWDLBlockAck", "receiveAWDLBlockAckEnd",
            "requestAWDLBlockAckEnd", "completeAWDLBlockAckEnd", "serviceAWDLBlockAck",
            "deliverAWDLReordered", "receiveAWDLReordered", "noteAWDLTXBlockAck",
            "receiveAWDLTXBlockAck", "serviceAWDLTXBlockAck", "configureAWDLTXStation",
            "awdlTXAntenna", "awdlTXMcs", "awdlTXWidth", "configureAWDLTXRates", "setAWDLTXWindow",
            "completeAWDLTXManagement", "configureAWDLPeerWidth", "downgradeAWDLTXRates",
        )
        rx = (
            "iwx_init_reorder_buffer", "iwx_clear_reorder_buffer", "iwx_is_sn_less",
            "iwx_release_frames", "iwx_oldsn_workaround", "iwx_rx_reorder",
            "iwx_reorder_timer_expired", "iwx_sta_rx_agg",
        )
        # Verify downgradeAWDLTXRates exists and serviceAWDLListener invokes it before configureAWDLPeerWidth & awdlPHY.
        svc_body = definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::serviceAWDLListener")
        self.assertIn("downgradeAWDLTXRates", svc_body)
        idx_tlc = svc_body.index("downgradeAWDLTXRates")
        idx_sta = svc_body.index("configureAWDLPeerWidth")
        idx_phy = svc_body.index("awdlPHY(channel")
        self.assertLess(idx_tlc, idx_sta)
        self.assertLess(idx_sta, idx_phy)

        # Runtime behavioral test on TLC v3 and v4 including stale peer HT cap.
        source += r"""
static uint64_t grant87(const void *p, uint64_t now) { auto end = *static_cast<const uint64_t *>(p); return end > now ? end : 0; }
int main() {
    for (unsigned version : {3U, 4U}) {
        ItlIwx d;
        auto r = request(d);
        d.tlcVersion = version;
        d.awdlRadio.channel = 149;
        d.awdlRadio.widthMHz = 80;
        d.awdlRadio.quz0034 = true;
        d.mimo = true; d.txAnt = 3; d.rxAnt = 3;
        auto &p = d.awdlPeers.peers[0];
        p.metadata.fields |= AirportAWDL::HTCapabilities;
        p.metadata.rxHTMcs = 0xff; p.metadata.rxHTMcs2 = 0xff;
        p.metadata.htCapabilities = 0x006f;
        p.htCapabilitiesReceivedUS = nowUS;
        p.haveSync = p.haveSequence = true;
        p.syncReceivedUS = p.sequenceReceivedUS = nowUS;
        p.sync.presenceMode = 4; p.sync.windowPeriodTU = 16; p.sync.nextWindowTU = 64;
        p.sequence.count = 16; p.sequence.fill = 0xffff; p.sequence.step = 3; p.sequence.encoding = 3;
        for (unsigned n = 0; n < 16; ++n) { p.sequence.channel[n] = 149; p.sequence.flags[n] = 128; }

        d.noteAWDLTXBlockAck(r.peer, 0, true);
        d.serviceAWDLTXBlockAck();
        auto &tx = d.awdlRadio.blockAckTX;
        assert(tx.configured && tx.widthMHz == 80 && d.tlcCommands.size() == 1);
        tx.phase = AirportAWDL::TXBlockAckQueue::Agreed;
        tx.enabled = true; tx.paused = true;

        // Even when peer HT cap has gone stale (>3s), downgradeAWDLTXRates(20) MUST NOT return EAGAIN;
        // it must send HT 20 MHz TLC command cleanly.
        nowUS += AirportAWDL::Peer::ScheduleFreshnessUS + 50000;
        assert(d.awdlTXMcs(nowUS) == 0);
        assert(!d.downgradeAWDLTXRates(20));
        assert(tx.widthMHz == 20 && d.tlcCommands.size() == 2);
        if (version == 4) {
            const auto &cfg = *reinterpret_cast<const iwx_tlc_config_cmd_v4 *>(d.tlcCommands.back().data());
            assert(cfg.mode == IWX_TLC_MNG_MODE_VHT && cfg.max_ch_width == IWX_TLC_MNG_CH_WIDTH_20MHZ);
            assert(!(cfg.sgi_ch_width_supp & (1U << IWX_TLC_MNG_CH_WIDTH_80MHZ)));
        } else {
            const auto &cfg = *reinterpret_cast<const iwl_tlc_config_cmd_v3 *>(d.tlcCommands.back().data());
            assert(cfg.mode == IWX_TLC_MNG_MODE_VHT && cfg.max_ch_width == IWX_TLC_MNG_CH_WIDTH_20MHZ);
            assert(!(cfg.sgi_ch_width_supp & (1U << IWX_TLC_MNG_CH_WIDTH_80MHZ)));
        }
        assert(!d.clearAWDLBlockAck());
    }
    return 0;
}
"""
        hal_ba = (
            "addAWDLBlockAckPeer", "awdlHasRXBA", "awdlRXBAOwned", "awdlBlockAckLive",
            "removeAWDLBlockAckTID", "removeAWDLBlockAckPeer", "clearAWDLBlockAck",
            "resetAWDLBlockAck", "prepareAWDLBlockAck", "receiveAWDLBlockAckEnd",
            "requestAWDLBlockAckEnd", "completeAWDLBlockAckEnd", "serviceAWDLBlockAck",
            "deliverAWDLReordered", "receiveAWDLReordered", "noteAWDLTXBlockAck",
            "receiveAWDLTXBlockAck", "serviceAWDLTXBlockAck", "configureAWDLTXStation",
            "awdlTXAntenna", "awdlTXMcs", "awdlTXWidth", "configureAWDLTXRates",
            "downgradeAWDLTXRates", "setAWDLTXWindow",
            "completeAWDLTXManagement", "closeAWDLListenerWindow", "configureAWDLPeerWidth",
        )
        source = source.replace("// PRODUCTION", "\n".join(
            definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::" + name) for name in hal_ba)
            + "\n" + "\n".join(definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name) for name in rx))
        source = source.replace("// FIRMWARE", firmware_declarations((
            "iwx_rx_mpdu_desc_v1", "iwx_rx_mpdu_desc_v3", "iwx_rx_mpdu_desc",
            "iwx_add_sta_cmd", "iwx_rm_sta_cmd", "iwx_reorder_buffer",
            "iwx_reorder_buf_entry", "iwx_rxba_data", "iwx_tlc_config_cmd_v4",
            "iwl_tlc_config_cmd_v3"), source, ("iwx_tlc_mng_cfg_mode", "iwx_tlc_mng_cfg_cw")))
        source = source.replace("// FROM REORDER BUFFER", definition(
            "itlwm/hal_iwx/if_iwxvar.h", "iwx_rxba_data_from_reorder_buf"))
        self.compile_run(source)

    def test_p87_channel_sequence_promotes_non_infra_5ghz_not_infra_ap(self):
        source = (support.ROOT / "tests/fixtures/awdl_control.cpp").read_text().rsplit("int main()", 1)[0]
        handle = "\n".join(
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + name)
            for name in ("handle", "updatePeerCache", "configureBroadcast", "broadcastDue")
        )
        declarations = []
        headers = {
            name: (support.ROOT / "include/Airport" / name).read_text()
            for name in ("apple80211_var.h", "apple80211_ioctl.h")
        }
        names = ["apple80211_channel", "awdl_peer_advert_add"] + sorted(
            set(re.findall(r"\bapple80211_\w+", handle)) | {"apple80211_awdl_af_tx_mode"}
        )
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
        source = source.replace("// PRODUCTION", handle)
        extra = r"""
static void preview87ChannelSeqPromotion() {
    AirportItlwmAWDLInterface i;
    i.device.ic.ic_state = IEEE80211_S_RUN;
    i.device.ic.ic_bss = &i.device.ap;
    i.device.apChannel.channel = 48;
    i.device.allow80 = true;
    i.device.awdlRadio.ready = true;

    const unsigned channels[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    apple80211_awdl_sync_channel_sequence seq = {};
    seq.version = 1; seq.pad1 = 0x00; seq.length = 16; seq.encoding = 1; seq.step_count = 3; seq.fill_channel = 0xff;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = channels[n];
        const unsigned flags = c == 48 ? 0x294U : c == 6 ? 0x8aU : (n == 0 ? 0x10U : 0x92U);
        seq.seqs[n] = {1, c, flags};
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq) == kIOReturnSuccess);
    unsigned wideCount = 0;
    for (unsigned n = 0; n < 16; ++n) {
        const uint8_t expected = (channels[n] == 149) ? 128 : 0;
        assert(i.sync.channelClasses[n] == expected);
        wideCount += (i.sync.channelClasses[n] == 128);
    }
    assert(wideCount == 11);

    // When en1 is on ch 149, ch 149 MUST NOT be promoted to 80 MHz (no PHY0/PHY1 collision),
    // while disjoint ch 44 may promote if 80 MHz allowed on ch 44.
    i.device.apChannel.channel = 149;
    apple80211_awdl_sync_channel_sequence seqOn149 = seq;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = channels[n];
        seqOn149.seqs[n] = {1, c, c == 149 ? 0x294U : (c == 6 ? 0x8aU : 0x92U)};
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seqOn149) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n) assert(i.sync.channelClasses[n] == 0);

    // Explicit maxBandwidth < 3 (e.g. 20 MHz = 1) disables promotion even on ch 149.
    i.device.apChannel.channel = 48;
    seq.pad1 = 0x01;
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n) assert(i.sync.channelClasses[n] == 0);

    // Self-healing circuit breaker tripped also forces 20 MHz.
    seq.pad1 = 0x00;
    i.device.awdlRadio.fwHealth.wideFaultCount = 2;
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n) assert(i.sync.channelClasses[n] == 0);
}

int main() {
    preview87ChannelSeqPromotion();
    return 0;
}
"""
        self.compile_run(source + extra)

    def test_p87_synchronous_tx_drain_and_ba_refill_wiring(self):
        v2 = definition("AirportItlwm/AirportItlwmV2.cpp", "AirportItlwm::outputStart")
        self.assertIn("submitOutputPackets(false)", v2)
        self.assertIn("submitOutputPackets(true)", v2)

        sub_fn = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::submitOutputPackets")
        self.assertIn("drainPendingData()", sub_fn)
        tx_ready_fn = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::txReady")
        self.assertIn("drainPendingData()", tx_ready_fn)
        drain_fn = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::drainPendingData")
        self.assertIn("drainingData", drain_fn)
        self.assertIn("transmitAWDL", drain_fn)

        ba_notif = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_rx_tx_ba_notif")
        self.assertIn("awdlRadio.txReady", ba_notif)
        self.assertLess(ba_notif.index("iwx_ampdu_txq_advance"), ba_notif.index("awdlRadio.txReady"))

        tx_cmd = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_rx_tx_cmd")
        self.assertIn("awdlRadio.txReady", tx_cmd)
        self.assertLess(tx_cmd.index("iwx_ampdu_txq_advance"), tx_cmd.index("awdlRadio.txReady"))

        start_fn = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::start")
        stop_fn = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::stop")
        self.assertIn("awdlRadio.txReady", start_fn)
        self.assertIn("awdlRadio.txReady = nullptr", stop_fn)

        # Runtime C++ behavioral proof under ASan+UBSan:
        # 1. submitOutputPackets(false) drains BSD frames into radio->transmitAWDL() with ZERO tick() calls.
        # 2. Re-entrancy guard drainingData blocks recursive drain when onTransmit fires txReady().
        # 3. txReady() immediately pulls from BSD and submits to radio->transmitAWDL() with ZERO tick() calls.
        # 4. Announcement deference: when psfDue(clockUS) is true, drainPendingData() defers and arms timer.
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace("// NATIVE TX SCENARIOS", (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text())
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

    // Open an active dwell via a single initial tick, then confirm ZERO tick() calls are needed for TX.
    i.tick();
    assert(i.device.awdlTxWindowOpen());
    const unsigned initialData = i.device.data;

    // 1. submitOutputPackets(false) immediately transmits 4 BSD frames with ZERO tick() calls,
    //    and re-entrant txReady() inside onTransmit is safely ignored by drainingData.
    unsigned reentrantCalls = 0;
    i.device.onTransmit = [&]() {
        if (reentrantCalls == 0) {
            ++reentrantCalls;
            assert(i.drainingData);
            AirportItlwmAWDLInterface::txReady(&i);
        }
    };
    for (unsigned n = 0; n < 4; ++n)
        i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    assert(i.submitOutputPackets(false) == kIOReturnSuccess);
    assert(reentrantCalls == 1 && !i.drainingData);
    assert(i.device.data == initialData + 4 && i.pendingCount == 0 && i.inFlightCount == 4);
    i.device.onTransmit = nullptr;

    // 2. When BA completes a frame and triggers txReady(&i), queued BSD frames drain immediately with ZERO tick().
    i.completeData(i.device.dataTokens[0], 0);
    assert(i.inFlightCount == 3);
    for (unsigned n = 0; n < 3; ++n)
        i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    AirportItlwmAWDLInterface::txReady(&i);
    assert(i.device.data == initialData + 7 && i.pendingCount == 0 && i.inFlightCount == 6);

    // 3. When PSF is due inside the dwell, drainPendingData() defers data and arms timer instead of starving PSF.
    i.nextActionUS = clockUS;
    i.wakeArmed = false;
    i.scheduler.calls = 0;
    i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    assert(i.submitOutputPackets(false) == kIOReturnSuccess);
    assert(i.device.data == initialData + 7 && i.pendingCount == 1 && i.wakeArmed && i.scheduler.calls >= 1);

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run("#define DEBUG 0\n" + tick_src)

    def test_p87_awdl_40ms_reorder_timeout_and_telemetry(self):
        # 1. Runtime check: AWDL RX reorder expires after 40 ms while AP (sta_id==0) stays 100 ms.
        source = (support.ROOT / "tests/fixtures/awdl_ba.cpp").read_text().rsplit("int main()", 1)[0]
        source += r"""
int main() {
    ItlIwx d;
    auto r = request(d);
    unsigned b = start(d, r);
    // Queue out-of-order AWDL frame seq=105 while head=103.
    auto gap = packet(d, r, 105, 55);
    receive(d, gap.get(), b, 105, 103);
    assert(d.delivered.empty() && !gap->frees);

    // At +39 ms: NOT yet expired.
    nowUS += 39000;
    ItlIwx::iwx_reorder_timer_expired(&d.com.sc_rxba_data[b].reorder_buf);
    assert(d.delivered.empty() && !gap->frees);

    // At +41 ms total: AWDL reorder gap expires cleanly without waiting 100 ms!
    nowUS += 2000;
    ItlIwx::iwx_reorder_timer_expired(&d.com.sc_rxba_data[b].reorder_buf);
    assert(d.delivered.size() == 1 && d.delivered.back() == 55 && gap->frees);
    assert(!d.clearAWDLBlockAck());
    return 0;
}
"""
        hal_ba = (
            "addAWDLBlockAckPeer", "awdlHasRXBA", "awdlRXBAOwned", "awdlBlockAckLive",
            "removeAWDLBlockAckTID", "removeAWDLBlockAckPeer", "clearAWDLBlockAck",
            "resetAWDLBlockAck", "prepareAWDLBlockAck", "receiveAWDLBlockAckEnd",
            "requestAWDLBlockAckEnd", "completeAWDLBlockAckEnd", "serviceAWDLBlockAck",
            "deliverAWDLReordered", "receiveAWDLReordered", "noteAWDLTXBlockAck",
            "receiveAWDLTXBlockAck", "serviceAWDLTXBlockAck", "configureAWDLTXStation",
            "awdlTXAntenna", "awdlTXMcs", "awdlTXWidth", "configureAWDLTXRates",
            "downgradeAWDLTXRates", "setAWDLTXWindow",
            "completeAWDLTXManagement", "closeAWDLListenerWindow", "configureAWDLPeerWidth",
        )
        rx = (
            "iwx_init_reorder_buffer", "iwx_clear_reorder_buffer", "iwx_is_sn_less",
            "iwx_release_frames", "iwx_oldsn_workaround", "iwx_rx_reorder",
            "iwx_reorder_timer_expired", "iwx_sta_rx_agg",
        )
        source = source.replace("// PRODUCTION", "\n".join(
            definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::" + name) for name in hal_ba)
            + "\n" + "\n".join(definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name) for name in rx))
        source = source.replace("// FIRMWARE", firmware_declarations((
            "iwx_rx_mpdu_desc_v1", "iwx_rx_mpdu_desc_v3", "iwx_rx_mpdu_desc",
            "iwx_add_sta_cmd", "iwx_rm_sta_cmd", "iwx_reorder_buffer",
            "iwx_reorder_buf_entry", "iwx_rxba_data", "iwx_tlc_config_cmd_v4",
            "iwl_tlc_config_cmd_v3"), source, ("iwx_tlc_mng_cfg_mode", "iwx_tlc_mng_cfg_cw")))
        source = source.replace("// FROM REORDER BUFFER", definition(
            "itlwm/hal_iwx/if_iwxvar.h", "iwx_rxba_data_from_reorder_buf"))
        self.compile_run(source)

        # 2. Verify peak TLC, RX rate, firmware health telemetry, and Skywalk LLW preservation.
        awdl_if = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.cpp").read_text()
        for prop in (
            "TXBlockAckPeakTLCRateKbps", "TXBlockAckLastTLCRateKbps",
            "AWDLDataRXLastRateKbps", "AWDLDataRXPeakRateKbps",
            "FWResetCount", "LastFWAssertID", "LastFWAssertPC", "LastFWLastCmd",
            "LastFWUMACAssertID", "AWDLWideFaultCount", "AWDLWideCircuitTripped",
        ):
            self.assertIn(f'"{prop}"', awdl_if)
        llw_rx = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmLLWInterface::inputSkywalkEthernet")
        self.assertIn("prepare(nullptr", llw_rx)
        self.assertIn("isQueueReady()", llw_rx)


if __name__ == "__main__":
    unittest.main()
