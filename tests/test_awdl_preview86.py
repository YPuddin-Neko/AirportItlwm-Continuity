"""Failing-first regressions for Preview 86: AirDrop RX BA probe protection, HT SGI/LDPC/STBC/4ms TXOP, AWDL TLC telemetry, and LLW flush TID reclamation."""
import re
import unittest

import test_regressions as support
from iwx_firmware import declarations
from test_offline import definition


class Preview86Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p86_bulk_rx_defers_promisc_probe_while_sidecar_tx_proceeds(self):
        source = (support.ROOT / "tests/fixtures/awdl_filter.cpp").read_text()
        interface = "AirportItlwm/AirportItlwmAWDLInterface.cpp"
        radio = "itlwm/hal_iwx/ItlIwxAWDL.cpp"
        routines = "\n".join(
            definition(interface, "AirportItlwmAWDLInterface::" + name)
            for name in ("setProperties", "publishReceiveFilter", "setEnabled", "setReceiveAllData", "setCompareTxAntennas")
        )
        routines += "\n" + definition(radio, "ItlIwx::awdlCompareArm")
        routines += "\n" + "\n".join(
            definition(radio, "ItlIwx::" + name)
            for name in ("awdlMAC", "startAWDLRadio", "stopAWDLRadio", "resetAWDLRadio", "serviceAWDLReceiveFilter")
        )
        source = source.replace("// PRODUCTION", routines)
        tick = definition(interface, "AirportItlwmAWDLInterface::tick")
        source = source.replace("// TIMER ENTRY", tick[tick.index("{") + 1:tick.index("    const bool available")])
        source = source.replace("// FIRMWARE DECLARATIONS", declarations(
            ("iwx_mac_data_ap", "iwx_mac_data_ibss", "iwx_mac_data_sta", "iwx_mac_data_go",
             "iwx_mac_data_p2p_sta", "iwx_mac_data_pibss", "iwx_mac_data_p2p_dev", "iwx_ac_qos",
             "iwx_mac_ctx_cmd", "iwx_device_power_cmd"), source, ("iwx_gen2_tx_fifo",)))
        test_body = r"""
    // 1. AirDrop RX: active bulk inbound RX (>=512B MPDUs) with NO heavy outbound TX MUST NOT open IN_PROMISC.
    {
        AirportItlwmAWDLInterface i;
        prepareReceiveProbe(i);
        auto &d = i.device;
        auto &r = d.awdlRadio;
        auto &tx = r.blockAckTX;
        uint64_t end = clockUS + 400000;
        r.lastRXPayloadUS = clockUS - 5000;
        r.lastHeavyRXPayloadUS = clockUS - 5000;
        tx.lastHeavyDataUS = 0;
        assert(r.heavyReceiveActive(clockUS));
        assert(!r.heavyTransmitActive(clockUS));
        assert(r.deferReceiveProbe(clockUS));
        assert(!d.serviceAWDLReceiveFilter(probeWindow, &end));
        assert(!r.listenerPromisc && !tx.receiveProbeUntilUS);
        assert(r.blockAckTXStats.receiveProbes == 0);
        assert(r.blockAckTXStats.receiveProbesDeferred >= 1);
        assert(d.commands.empty());

        // 2. If a probe was already open when bulk inbound RX starts (with no heavy TX), it closes immediately.
        r.lastHeavyRXPayloadUS = 0;
        assert(!d.serviceAWDLReceiveFilter(probeWindow, &end));
        assert(r.listenerPromisc && tx.receiveProbeUntilUS == clockUS + 60000);
        assert(r.blockAckTXStats.receiveProbes == 1 && d.commands.size() == 1);
        clockUS += 4000;
        r.lastRXPayloadUS = clockUS;
        r.lastHeavyRXPayloadUS = clockUS;
        assert(!d.serviceAWDLReceiveFilter(probeWindow, &end));
        assert(!r.listenerPromisc && !tx.receiveProbeUntilUS && d.commands.size() == 2);

        // 3. Once bulk inbound RX goes quiet (>=300ms), or once Mac has heavy outbound TX (Sidecar / AirDrop TX),
        //    the probe is allowed immediately.
        tx.lastHeavyDataUS = clockUS;
        assert(r.heavyTransmitActive(clockUS));
        assert(!r.deferReceiveProbe(clockUS));
        assert(!d.serviceAWDLReceiveFilter(probeWindow, &end));
        assert(r.listenerPromisc && r.blockAckTXStats.receiveProbes == 2);
    }
    // 4. Sidecar cadence (small <512B RTCP/HID RX every 20ms, lastHeavyRXPayloadUS==0) MUST still open probe.
    {
        AirportItlwmAWDLInterface i;
        prepareReceiveProbe(i);
        auto &d = i.device;
        auto &r = d.awdlRadio;
        uint64_t end = clockUS + 400000;
        r.lastRXPayloadUS = clockUS - 20000;
        r.lastHeavyRXPayloadUS = 0;
        assert(!r.heavyReceiveActive(clockUS));
        assert(!r.deferReceiveProbe(clockUS));
        assert(!d.serviceAWDLReceiveFilter(probeWindow, &end));
        assert(r.listenerPromisc && r.blockAckTXStats.receiveProbes == 1);
    }
"""
        source = source.replace("// TEST ENTRY", test_body)
        self.compile_run("#define DEBUG 1\n" + source)

        # Also verify receiveAWDLReordered updates lastHeavyRXPayloadUS for >=512B frames only,
        # and completeUnicast does not trigger rxTakeover/TX BA during active bulk RX or when another peer owns TX BA.
        rx_body = definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::receiveAWDLReordered")
        self.assertIn("lastHeavyRXPayloadUS", rx_body)
        cu_body = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::completeUnicast")
        self.assertIn("heavyReceiveActive", cu_body)
        self.assertIn("RadioState::LLWTID", cu_body)
        self.assertIn("rxTakeover", (support.ROOT / "AirportItlwm/AWDL/TXBlockAck.hpp").read_text())
        stx_body = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::serviceTXBlockAck")
        self.assertIn("deferReceiveProbe", stx_body)

    def test_p86_ht_cap_sgi20_ldpc_stbc_and_4ms_txop_v3_v4(self):
        # 1. ActionFrame TLV 7 parses htCapabilities and Peer::txHTCap honors freshness.
        parse_src = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "AirportItlwm/AWDL/ActionFrame.hpp"
#include "AirportItlwm/AWDL/PeerState.hpp"

using namespace AirportAWDL;

int main() {
    std::vector<uint8_t> body = {127, 0, 0x17, 0xf2, 8, 0x10, 0, 0, 0,0,0,0, 0,0,0,0};
    // TLV 7: 2 prefix bytes (00 00) + HT Cap Info 0x016f (LDPC|40MHz|SGI20|SGI40|RXSTBC1) + AMPDU(1b) + MCS1(ff) + MCS2(ff)
    const uint8_t tlv7[] = {7, 8, 0, 0x00, 0x00, 0x6f, 0x01, 0x1b, 0xff, 0xff, 0x00};
    body.insert(body.end(), tlv7, tlv7 + sizeof(tlv7));
    Advertisement a = {};
    assert(parseAction(body.data(), body.size(), a) == ParseResult::Valid);
    assert(a.fields == HTCapabilities);
    assert(a.htCapabilities == 0x016f);
    assert(a.rxHTMcs == 0xff && a.rxHTMcs2 == 0xff);

    // Truncated length=3 TLV 7 parses cleanly with htCapabilities==0.
    std::vector<uint8_t> shortBody = {127, 0, 0x17, 0xf2, 8, 0x10, 0, 0, 0,0,0,0, 0,0,0,0, 7, 3, 0, 0, 0, 0};
    Advertisement aShort = {};
    assert(parseAction(shortBody.data(), shortBody.size(), aShort) == ParseResult::Valid);
    assert(aShort.htCapabilities == 0);

    PeerTable table = {};
    const uint8_t self[6] = {2,0,0,0,0,1}, peer[6] = {2,0,0,0,0,2};
    assert(table.update(peer, self, a, 1000000, 149, -45));
    const auto *p = table.find(peer, 1000000);
    assert(p && p->txHTCap(1000000) == 0x016f);
    assert(p->txHTCap(1000000 + Peer::ScheduleFreshnessUS - 1) == 0x016f);
    assert(p->txHTCap(1000000 + Peer::ScheduleFreshnessUS) == 0);
    return 0;
}
"""
        self.compile_run(parse_src)

        # 2. configureAWDLTXRates configures SGI20, LDPC, STBC (2-chain only), and 4000us TXOP on both TLC v3 and v4.
        source = (support.ROOT / "tests/fixtures/awdl_ba.cpp").read_text().rsplit("int main()", 1)[0]
        source += r"""
static uint64_t grant(const void *p, uint64_t now) { auto end = *static_cast<const uint64_t *>(p); return end > now ? end : 0; }
int main() {
    for (unsigned version : {3U, 4U}) {
        for (unsigned chains : {1U, 2U}) {
            ItlIwx d;
            auto r = request(d);
            d.tlcVersion = version;
            d.awdlRadio.channel = 149;
            d.awdlRadio.widthMHz = 20;
            d.awdlRadio.quz0034 = true;
            d.mimo = (chains == 2);
            d.txAnt = (chains == 2) ? 3 : 1;
            d.rxAnt = (chains == 2) ? 3 : 1;
            auto &p = d.awdlPeers.peers[0];
            p.metadata.fields |= AirportAWDL::HTCapabilities;
            p.metadata.rxHTMcs = 0xff;
            p.metadata.rxHTMcs2 = (chains == 2) ? 0xff : 0;
            p.metadata.htCapabilities = 0x016f; // LDPC | SGI20 | SGI40 | RXSTBC(1)
            p.htCapabilitiesReceivedUS = nowUS;
            p.haveSync = p.haveSequence = true;
            p.syncReceivedUS = p.sequenceReceivedUS = nowUS;
            p.sync.presenceMode = 4; p.sync.windowPeriodTU = 16; p.sync.nextWindowTU = 64;
            p.sequence.count = 16; p.sequence.fill = 0xffff; p.sequence.step = 3; p.sequence.encoding = 3;
            for (unsigned n = 0; n < 16; ++n) { p.sequence.channel[n] = 149; p.sequence.flags[n] = 124; }

            d.noteAWDLTXBlockAck(r.peer, 0, true);
            d.serviceAWDLTXBlockAck();
            auto &tx = d.awdlRadio.blockAckTX;
            assert(tx.phase == AirportAWDL::TXBlockAckQueue::Asking && tx.configured);
            assert(tx.widthMHz == 20 && tx.htCap == 0x016f);
            assert(tx.sgiMask == (1U << IWX_TLC_MNG_CH_WIDTH_20MHZ));
            const uint16_t expectedFlags = IWX_TLC_MNG_CFG_FLAGS_LDPC_MSK |
                (chains == 2 ? IWX_TLC_MNG_CFG_FLAGS_STBC_MSK : 0);
            assert(tx.tlcFlags == expectedFlags);
            assert(d.tlcCommands.size() == 1);
            if (version == 4) {
                const auto &cfg = *reinterpret_cast<const iwx_tlc_config_cmd_v4 *>(d.tlcCommands[0].data());
                assert(cfg.mode == IWX_TLC_MNG_MODE_VHT);
                assert(cfg.max_ch_width == IWX_TLC_MNG_CH_WIDTH_20MHZ);
                assert(cfg.sgi_ch_width_supp == (1U << IWX_TLC_MNG_CH_WIDTH_20MHZ));
                assert(cfg.flags == expectedFlags);
                assert(cfg.max_tx_op == 4000);
            } else {
                const auto &cfg = *reinterpret_cast<const iwl_tlc_config_cmd_v3 *>(d.tlcCommands[0].data());
                assert(cfg.mode == IWX_TLC_MNG_MODE_VHT);
                assert(cfg.max_ch_width == IWX_TLC_MNG_CH_WIDTH_20MHZ);
                assert(cfg.sgi_ch_width_supp == (1U << IWX_TLC_MNG_CH_WIDTH_20MHZ));
                assert(cfg.flags == expectedFlags);
                assert(cfg.max_tx_op == 4000);
            }
            // Verify setAWDLTXWindow reconfigures TLC when peer HT cap changes.
            tx.phase = AirportAWDL::TXBlockAckQueue::Agreed;
            tx.enabled = true; tx.paused = true;
            uint64_t end = nowUS + 30000;
            assert(!d.setAWDLTXWindow(grant, &end));
            assert(d.tlcCommands.size() == 1 && !tx.paused);
            p.metadata.htCapabilities = 0x0020; // SGI20 only, no LDPC/STBC
            assert(!d.setAWDLTXWindow(grant, &end));
            assert(d.tlcCommands.size() == 2 && tx.tlcFlags == 0 && tx.sgiMask == (1U << IWX_TLC_MNG_CH_WIDTH_20MHZ));
            assert(!d.clearAWDLBlockAck());
        }
    }
    return 0;
}
"""
        hal = ("addAWDLBlockAckPeer", "awdlHasRXBA", "awdlRXBAOwned", "awdlBlockAckLive",
               "removeAWDLBlockAckTID", "removeAWDLBlockAckPeer", "clearAWDLBlockAck",
               "resetAWDLBlockAck", "prepareAWDLBlockAck", "receiveAWDLBlockAckEnd",
               "requestAWDLBlockAckEnd", "completeAWDLBlockAckEnd", "serviceAWDLBlockAck",
               "deliverAWDLReordered", "receiveAWDLReordered", "noteAWDLTXBlockAck",
               "receiveAWDLTXBlockAck", "serviceAWDLTXBlockAck", "configureAWDLTXStation",
               "awdlTXAntenna", "awdlTXMcs", "awdlTXWidth", "configureAWDLTXRates", "setAWDLTXWindow",
               "completeAWDLTXManagement", "closeAWDLListenerWindow", "configureAWDLPeerWidth")
        rx = ("iwx_init_reorder_buffer", "iwx_clear_reorder_buffer", "iwx_is_sn_less",
              "iwx_release_frames", "iwx_oldsn_workaround", "iwx_rx_reorder",
              "iwx_reorder_timer_expired", "iwx_sta_rx_agg")
        source = source.replace("// PRODUCTION", "\n".join(
            definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::" + name) for name in hal)
            + "\n" + "\n".join(definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name) for name in rx))
        source = source.replace("// FIRMWARE", declarations((
            "iwx_rx_mpdu_desc_v1", "iwx_rx_mpdu_desc_v3", "iwx_rx_mpdu_desc",
            "iwx_add_sta_cmd", "iwx_rm_sta_cmd", "iwx_reorder_buffer",
            "iwx_reorder_buf_entry", "iwx_rxba_data", "iwx_tlc_config_cmd_v4",
            "iwl_tlc_config_cmd_v3"), source, ("iwx_tlc_mng_cfg_mode", "iwx_tlc_mng_cfg_cw")))
        source = source.replace("// FROM REORDER BUFFER", definition(
            "itlwm/hal_iwx/if_iwxvar.h", "iwx_rxba_data_from_reorder_buf"))
        self.compile_run(source)

    def test_p86_awdl_tlc_update_notif_and_llw_flush_tid(self):
        rs_update = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_rs_update")
        self.assertIn("lastTLCRate", rs_update)
        self.assertIn("lastTLCRateKbps", rs_update)
        self.assertIn("awdlRadio.blockAckTX", rs_update)

        flush_fn = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_flush_sta_tids")
        self.assertIn("llwQueueConfigured", flush_fn)
        self.assertIn("LLWTID", flush_fn)

        awdl_if = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.cpp").read_text()
        for prop in ("TXBlockAckSGI", "TXBlockAckFlags", "TXBlockAckTLCRateFlags",
                     "TXBlockAckTLCRateKbps", "TXBlockAckTotalFlushed", "TXBlockAckProbesDeferred",
                     "AWDLPeerHTMcs2Mask", "AWDLPeerHTCap"):
            self.assertIn(f'"{prop}"', awdl_if)

    def test_p86_preserves_quz0034_20mhz_and_preview85_skywalk_invariants(self):
        awdl_phy = definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::awdlPHY")
        self.assertIn("awdlWideChannelAllowed", awdl_phy)
        svc_ch = definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::serviceAWDLChannel")
        self.assertIn("awdlWideChannelAllowed", svc_ch)
        llw_rx = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmLLWInterface::inputSkywalkEthernet")
        self.assertIn("prepare(nullptr", llw_rx)
        self.assertIn("isQueueReady()", llw_rx)


if __name__ == "__main__":
    unittest.main()
