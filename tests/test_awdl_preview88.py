"""Failing-first regressions for Preview 88:
1. VHT 80 MHz TLC enables full MCS 0..9 (0x03ff), 20/40/80 MHz SGI (0x7), 256K A-MPDU, and counts tlcConfigs.
2. During active heavy 80 MHz TX, infrastructure AP channel slots park PHY 1 at 149@80 without wiping TLC/STA/PHY.
3. TXBlockAck demand/handover/heavyTransmitActive classify >=512B Sidecar RTP frames as heavy (ignoring <512B control).
4. ioreg publishes TXBlockAckTLCConfigs and TXBlockAckInfraParks telemetry.
"""
import re
import unittest

import test_regressions as support
from iwx_firmware import declarations as firmware_declarations
from test_offline import definition


class Preview88Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p88_vht80_tlc_enables_mcs0_to_9_40_80_sgi_and_256k_agg(self):
        source = (support.ROOT / "tests/fixtures/awdl_ba.cpp").read_text().rsplit("int main()", 1)[0]
        source += r"""
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
        p.metadata.htCapabilities = 0x006f; // LDPC | SGI20 | SGI40
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
        assert(d.awdlRadio.blockAckTXStats.tlcConfigs == 1);
        assert((d.adds[0].station_flags & IWX_STA_FLG_MAX_AGG_SIZE_MSK) == IWX_STA_FLG_MAX_AGG_SIZE_256K);
        const uint8_t expectedSgi = (1U << IWX_TLC_MNG_CH_WIDTH_20MHZ) |
            (1U << IWX_TLC_MNG_CH_WIDTH_40MHZ) | (1U << IWX_TLC_MNG_CH_WIDTH_80MHZ);
        if (version == 4) {
            const auto &cfg = *reinterpret_cast<const iwx_tlc_config_cmd_v4 *>(d.tlcCommands[0].data());
            assert(cfg.mode == IWX_TLC_MNG_MODE_VHT && cfg.max_ch_width == IWX_TLC_MNG_CH_WIDTH_80MHZ);
            assert(cfg.ht_rates[0][0] == 0x03ff && cfg.ht_rates[1][0] == 0x03ff);
            assert(cfg.sgi_ch_width_supp == expectedSgi);
        } else {
            const auto &cfg = *reinterpret_cast<const iwl_tlc_config_cmd_v3 *>(d.tlcCommands[0].data());
            assert(cfg.mode == IWX_TLC_MNG_MODE_VHT && cfg.max_ch_width == IWX_TLC_MNG_CH_WIDTH_80MHZ);
            assert(cfg.ht_rates[0][0] == 0x03ff && cfg.ht_rates[1][0] == 0x03ff);
            assert(cfg.sgi_ch_width_supp == expectedSgi);
        }

        // Downgrading to 20 MHz on 5 GHz preserves VHT mode with MCS 0..8 (0x01ff) and strips 40 & 80 MHz SGI.
        tx.phase = AirportAWDL::TXBlockAckQueue::Agreed;
        tx.enabled = true; tx.paused = true;
        assert(!d.downgradeAWDLTXRates(20));
        assert(d.awdlRadio.blockAckTXStats.tlcConfigs == 2);
        const uint8_t expectedSgi20 = (1U << IWX_TLC_MNG_CH_WIDTH_20MHZ);
        if (version == 4) {
            const auto &cfg = *reinterpret_cast<const iwx_tlc_config_cmd_v4 *>(d.tlcCommands.back().data());
            assert(cfg.mode == IWX_TLC_MNG_MODE_VHT && cfg.ht_rates[0][0] == 0x01ff);
            assert(cfg.sgi_ch_width_supp == expectedSgi20);
        } else {
            const auto &cfg = *reinterpret_cast<const iwl_tlc_config_cmd_v3 *>(d.tlcCommands.back().data());
            assert(cfg.mode == IWX_TLC_MNG_MODE_VHT && cfg.ht_rates[0][0] == 0x01ff);
            assert(cfg.sgi_ch_width_supp == expectedSgi20);
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

    def test_p88_park_wide_phy_on_infra_channel_preserves_tlc_without_retune(self):
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
static void preview88InfraPark() {
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

    // Simulate active heavy 80 MHz Sidecar TX BlockAck session on ch 149.
    auto &tx = d.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.epoch = d.dmaEpoch;
    tx.phase = AirportAWDL::TXBlockAckQueue::Agreed;
    tx.enabled = true;
    tx.paused = false;
    tx.widthMHz = 80;
    tx.lastHeavyDataUS = clockUS;
    RadioState::GrantPolicy p80 = {}; p80.widthMHz = 80; p80.slot = 1;
    assert(!d.serviceAWDLChannel(149, clockUS + 96000, p80));
    assert(d.awdlRadio.dwell.running);

    // 1. Entering infra AP channel 48 MUST close dwell cleanly while parking PHY 1 at 149@80
    //    (ZERO PHY commands, ZERO TLC downgrade, infraParks incremented!).
    const size_t phyBefore = d.phyCommands.size();
    RadioState::GrantPolicy p20 = {}; p20.widthMHz = 20; p20.slot = 2; p20.bridgeInfraToWide = true;
    assert(!d.serviceAWDLChannel(48, clockUS + 32000, p20));
    assert(!d.awdlRadio.dwell.running && tx.paused);
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80 && tx.widthMHz == 80);
    assert(d.phyCommands.size() == phyBefore);
    assert(d.awdlRadio.blockAckTXStats.infraParks == 1);

    // 2. Returning to ch 149@80 MUST reopen dwell with ZERO PHY commands and keep tx.widthMHz == 80!
    p80.slot = 3;
    assert(!d.serviceAWDLChannel(149, clockUS + 96000, p80));
    assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);
    assert(d.phyCommands.size() == phyBefore);

    // 3. Switching to non-infra social ch 6 MUST still shrink TLC -> STA -> PHY normally!
    p20.slot = 4;
    assert(!d.serviceAWDLChannel(6, clockUS + 16000, p20));
    assert(d.awdlRadio.channel == 6 && d.awdlRadio.widthMHz == 20 && tx.widthMHz == 20);
    assert(d.phyCommands.size() > phyBefore);

    // 4. Return to 149@80, then go idle (>500ms): switching to ch 48 MUST retune to ch 48 normally.
    p80.slot = 5; tx.widthMHz = 80;
    assert(!d.serviceAWDLChannel(149, clockUS + 96000, p80));
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);
    clockUS += 600000; // heavyTransmitActive now false
    p20.slot = 6;
    assert(!d.serviceAWDLChannel(48, clockUS + 32000, p20));
    assert(d.awdlRadio.channel == 48 && d.awdlRadio.widthMHz == 20);
    assert(!d.stopAWDLRadio());
}

int main() {
    preview88InfraPark();
    return 0;
}
"""
        self.compile_run(radio_src + extra)

    def test_p88_tx_ba_demand_and_handover_accept_512b_sidecar_frames(self):
        src = r"""
#include <cassert>
#include <cstdint>
#include "AirportItlwm/AWDL/TXBlockAck.hpp"
#include "AirportItlwm/AWDL/RadioState.hpp"

using namespace AirportAWDL;

int main() {
    const uint8_t iphone[6] = {2, 0, 0, 0, 0, 2};
    const uint8_t ipad[6] = {2, 0, 0, 0, 0, 3};

    // 1. 643B Sidecar RTP frames (average observed Sidecar packet size) trigger TX BA demand & heavyTransmitActive.
    RadioState r = {};
    uint64_t now = 10000000;
    for (unsigned n = 0; n < TXBlockAckQueue::DemandThreshold - 1; ++n) {
        r.blockAckTX.demand(ipad, 643, now + n * 1000, false, true);
        assert(r.blockAckTX.phase == TXBlockAckQueue::Idle);
    }
    r.blockAckTX.demand(ipad, 643, now + (TXBlockAckQueue::DemandThreshold - 1) * 1000, false, true);
    assert(r.blockAckTX.phase == TXBlockAckQueue::Requested);
    assert(r.heavyTransmitActive(now + (TXBlockAckQueue::DemandThreshold - 1) * 1000));

    // 2. Tiny 120B control/keepalive frames MUST NOT trigger demand or refresh lastHeavyDataUS.
    RadioState r2 = {};
    for (unsigned n = 0; n < 100; ++n) {
        r2.blockAckTX.demand(ipad, 120, now + n * 1000, false, true);
    }
    assert(r2.blockAckTX.phase == TXBlockAckQueue::Idle);
    assert(!r2.heavyTransmitActive(now + 100000));

    // 3. 643B Sidecar frames preempt an idle/light TX BA owner (e.g. iPhone after AirDrop) via handover.
    TXBlockAckQueue tx = {};
    tx.demand(iphone, 0, now, true, true);
    tx.configured = true; tx.phase = TXBlockAckQueue::Agreed;
    now += 600000;
    for (unsigned n = 0; n < TXBlockAckQueue::DemandThreshold; ++n) {
        tx.demand(ipad, 643, now + n * 1000, false, true);
    }
    assert(tx.phase == TXBlockAckQueue::Retiring && tx.releasePeer && sameAddress(tx.handoverPeer, ipad));
    return 0;
}
"""
        self.compile_run(src)

    def test_p88_ioreg_telemetry_publishes_tlc_configs_and_infra_parks(self):
        awdl_if = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.cpp").read_text()
        self.assertIn('"TXBlockAckTLCConfigs"', awdl_if)
        self.assertIn('"TXBlockAckInfraParks"', awdl_if)


if __name__ == "__main__":
    unittest.main()
