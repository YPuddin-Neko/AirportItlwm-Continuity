"""Exercise AWDL wire parsing and the production IWX receive boundary."""

import gzip
import json
from pathlib import Path
import re
import struct
import tempfile
import unittest
import test_regressions as support
from test_offline import definition
from iwx_firmware import receive_diagnostics
from iwx_firmware import declarations as firmware_declarations
import awdl_native_sync


class AWDLRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def run_case(self, case):
        source = (support.ROOT / "tests/fixtures/awdl.cpp").read_text()
        self.compile_run("#define AWDL_TEST_CASE " + str(case) + "\n" + source)

    def test_action_fields_and_channel_encodings(self):
        self.run_case(1)

    def test_ht_capabilities_compact_full_and_malformed_wire_values(self):
        self.run_case(5)

    def test_ht_capabilities_merge_freshness_and_peer_reset(self):
        self.run_state_case(7)

    def test_malformed_and_future_frames_do_not_replace_state(self):
        self.run_case(2)

    def test_mpdu_classification_and_receive_counters(self):
        self.run_case(3)

    def test_truncation_unaligned_and_mutated_input(self):
        self.run_case(4)

    def run_state_case(self, case):
        source = (support.ROOT / "tests/fixtures/awdl_state.cpp").read_text()
        self.compile_run("#define AWDL_STATE_CASE " + str(case) + "\n" + source)

    def test_peer_merge_expiry_eviction_and_timestamp_wrap(self):
        self.run_state_case(1)

    def test_remote_clock_restart_local_order_and_duplicate_action_retries(self):
        self.compile_run((support.ROOT / "tests/fixtures/awdl_clock_order.cpp").read_text())

    def test_peer_windows_wrap_and_duplicate_sequences(self):
        self.run_state_case(2)

    def test_transmit_pair_wire_identity_mtu_padding_and_atomic_rejection(self):
        self.run_state_case(8)

    def test_transmit_pair_completion_flush_retry_and_sequence_ownership(self):
        self.run_tick_case(34)

    def test_transmit_pair_stale_ht_deadlines_and_failed_submission(self):
        self.run_tick_case(35)

    def test_transmit_pair_counts_original_packets_in_backpressure_capacity(self):
        self.run_tick_case(36)

    def test_ipv6_data_codec_aggregation_and_mutations(self):
        self.run_state_case(3)

    def test_data_observation_and_peer_lifetime(self):
        self.run_state_case(4)

    def test_registered_peer_reservation_expiry_reset_and_capacity(self):
        self.run_state_case(5)

    def test_receive_demand_requires_new_local_unicast_and_clears_on_reset(self):
        self.run_state_case(6)

    def iwx_receive_source(self):
        source = (support.ROOT / "tests/fixtures/iwx_awdl.cpp").read_text()
        headers = "\n".join(definition("itl80211/openbsd/net80211/ieee80211.h", name)
            for name in ("ieee80211_has_seq", "ieee80211_has_addr4", "ieee80211_has_qos", "ieee80211_has_htc"))
        headers += "\n" + definition("itl80211/openbsd/net80211/ieee80211_input.c", "ieee80211_get_hdrlen")
        source = source.replace("// HEADER LENGTH", headers)
        source = source.replace("// RX DIAGNOSTIC DECLARATIONS", receive_diagnostics())
        routines = ("ItlIwx::observeAWDLReceiveDescriptor", "ItlIwx::formatAWDLReceiveDiagnostics", "ItlIwx::iwx_rxmq_get_signal_strength", "ItlIwx::receiveAWDL", "ItlIwx::resetAWDLPeers",
                    "ItlIwx::iwx_rx_mpdu_mq")
        source = source.replace("// PRODUCTION\n", definition(
            "itl80211/openbsd/net80211/ieee80211_input.c", "ieee80211_sta_foreign_disconnect") + "\n" + "\n".join(
            definition("itlwm/hal_iwx/ItlIwx.cpp", name) for name in routines))
        source = source.replace("// FIRMWARE DECLARATIONS", firmware_declarations(
            ("iwx_rx_mpdu_desc_v1", "iwx_rx_mpdu_desc_v3", "iwx_rx_mpdu_desc"), source))
        return source

    def test_iwx_receive_routing_and_packet_ownership(self):
        self.compile_run(self.iwx_receive_source())

    def test_listener_zero_padded_5ghz_action_frames_and_native_delivery(self):
        source = self.iwx_receive_source().split("\nint main()", 1)[0]
        native = (support.ROOT / "tests/fixtures/awdl_native.cpp").read_text().split("\nint main()", 1)[0]
        native = native.replace("// NATIVE CHANNEL FLAGS", re.search(
            r"enum apple80211_channel_flag\s*\{.*?\};",
            (support.ROOT / "include/Airport/apple80211_var.h").read_text(), re.S)[0])
        native = native.replace("// PRODUCTION", definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::receiveAction"))
        harness = source + "\n#define AWDL_SERVICE_MERGE_TEST 1\n" + native + r"""
int main() {
    ItlIwx driver;
    driver.awdlRadio.ready = true;
    AirportAWDL::copyAddress(driver.awdlRadio.address, driver.com.sc_ic.ic_myaddr);
    AirportItlwmAWDLInterface nativeIface;
    driver.awdlReceiver = &nativeIface;
    driver.awdlActionInput = AirportItlwmAWDLInterface::receiveAction;

    auto build5GHzFrame = [](uint16_t seq, uint8_t subtype, bool htc, uint8_t svcByte) {
        std::vector<uint8_t> b(htc ? 28 : 24, 0);
        b[0] = 0xd0;
        b[1] = htc ? 0x80 : 0;
        std::fill(b.begin() + 4, b.begin() + 10, 0xff);
        const uint8_t sender[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
        AirportAWDL::copyAddress(b.data() + 10, sender);
        AirportAWDL::copyAddress(b.data() + 16, AirportAWDL::BSSID);
        AirportAWDL::putLE16(b.data() + 22, uint16_t(seq << 4));
        const uint8_t hdr16[] = {
            127, 0, 0x17, 0xf2, 8, 0x10, subtype, 0,
            uint8_t(seq * 100 & 0xff), uint8_t((seq * 100 >> 8) & 0xff), 0, 0,
            uint8_t(seq * 100 & 0xff), uint8_t((seq * 100 >> 8) & 0xff), 0, 0
        };
        b.insert(b.end(), std::begin(hdr16), std::end(hdr16));
        // Sync TLV (type 4, length 33) + Sequence TLV (type 18, encoding 3: ch 6 / 149)
        b.insert(b.end(), {4, 33, 0});
        const size_t syncOff = b.size();
        b.resize(syncOff + 33, 0);
        b[syncOff + 1] = 16; b[syncOff + 3] = 149; b[syncOff + 5] = 16; b[syncOff + 7] = 110;
        AirportAWDL::copyAddress(b.data() + syncOff + 21, sender);
        b[syncOff + 27] = 4; b[syncOff + 29] = uint8_t(seq & 0xff);
        const uint8_t seqTlv[] = {
            18, 38, 0, 0x0f, 0x03, 0x00, 0x03, 0xff, 0xff,
            6, 0x51, 149, 0x80, 149, 0x80, 6, 0x51, 149, 0x80, 149, 0x80, 6, 0x51, 149, 0x80,
            149, 0x80, 6, 0x51, 149, 0x80, 149, 0x80, 6, 0x51, 149, 0x80, 149, 0x80, 6, 0x51
        };
        b.insert(b.end(), std::begin(seqTlv), std::end(seqTlv));
        // HT Capabilities TLV (type 7, rxHTMcs = 0xff, rxHTMcs2 = 0xff)
        const uint8_t htTlv[] = {7, 8, 0, 0, 0, 0x6e, 0x01, 0x1b, 0xff, 0xff, 0};
        b.insert(b.end(), std::begin(htTlv), std::end(htTlv));
        // Version TLV (type 21)
        const uint8_t verTlv[] = {21, 2, 0, 0x50, 0x02};
        b.insert(b.end(), std::begin(verTlv), std::end(verTlv));
        if (subtype == 3) {
            // Service Response TLV (type 2) carrying AirDrop identity record
            const uint8_t svcTlv[] = {2, 6, 0, 0x10, 0x01, svcByte, 0xaa, 0xbb, 0xcc};
            b.insert(b.end(), std::begin(svcTlv), std::end(svcTlv));
        }
        return b;
    };

    auto deliver5GHz = [&](std::vector<uint8_t> mpdu, unsigned flags = 0) {
        if (flags & IWX_RX_MPDU_MFLG2_PAD)
            mpdu.insert(mpdu.begin() + ieee80211_get_hdrlen(
                reinterpret_cast<const ieee80211_frame *>(mpdu.data())), 2, 0);
        std::vector<uint8_t> storage(IWX_RX_DESC_SIZE_V1 + mpdu.size(), 0);
        auto *desc = reinterpret_cast<iwx_rx_mpdu_desc *>(storage.data());
        desc->status = 3;
        desc->mpdu_len = mpdu.size();
        desc->mac_flags2 = flags;
        desc->v1.energy_a = 42;
        desc->v1.energy_b = 44;
        desc->v1.channel = 149;
        driver.com.sc_device_family = IWX_DEVICE_FAMILY_22000;
        std::memcpy(storage.data() + IWX_RX_DESC_SIZE_V1, mpdu.data(), mpdu.size());
        Packet m = {storage.data(), storage.size(), storage.size(), 0};
        driver.iwx_rx_mpdu_mq(&driver.com, &m, desc, storage.size(), nullptr);
        assert(m.frees == 1);
    };

    uint16_t seq = 1;
    unsigned expectedValid = 0;
    for (bool htc : {false, true}) {
        for (unsigned flags : {0U, unsigned(IWX_RX_MPDU_MFLG2_PAD)}) {
            for (size_t pad : {1u, 2u, 3u, 6u, 8u, 12u}) {
                fixtureNowUS += 5000;
                const uint8_t subtype = (pad & 1) ? 0 : 3;
                auto clean = build5GHzFrame(seq, subtype, htc, uint8_t(seq));
                const size_t hdr = htc ? 28 : 24;
                const std::vector<uint8_t> expectedBody(clean.begin() + hdr, clean.end());
                auto padded = clean;
                padded.insert(padded.end(), pad, 0);
                deliver5GHz(padded, flags);
                ++expectedValid;
                ++seq;
                assert(driver.awdlReceive.valid == expectedValid);
                assert(driver.awdlReceive.malformed == 0);
                assert(nativeIface.actionDelivered == expectedValid);
                if (subtype == 0) {
                    assert(nativeIface.payload == expectedBody);
                } else {
                    Advertisement merged = {};
                    assert(AirportAWDL::parseAction(nativeIface.payload.data(),
                        nativeIface.payload.size(), merged) == AirportAWDL::ParseResult::Valid);
                    // Trailing zero padding must never leak into native action body.
                    assert(nativeIface.payload.size() >= expectedBody.size());
                    assert(!(nativeIface.payload.back() == 0 &&
                        nativeIface.payload[nativeIface.payload.size() - 2] == 0 &&
                        nativeIface.payload[nativeIface.payload.size() - 3] == 0));
                }
            }
        }
    }

    const uint8_t sender[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const auto *peer = driver.awdlPeers.find(sender, fixtureNowUS);
    assert(peer && peer->txHTMcs(fixtureNowUS) == 0xff && peer->txHTMcs2(fixtureNowUS) == 0xff);
    AirportAWDL::Window win = {};
    assert(peer->window(fixtureNowUS, win) && win.channel != 0);

    // Non-zero truncated TLV tails must still be rejected as Malformed, never trimmed.
    fixtureNowUS += 5000;
    auto bad1 = build5GHzFrame(seq++, 3, false, 0x77);
    bad1.insert(bad1.end(), {21, 2, 0, 0x50}); // missing 1 byte
    deliver5GHz(bad1);
    assert(driver.awdlReceive.malformed == 1 && driver.awdlReceive.valid == expectedValid);

    fixtureNowUS += 5000;
    auto bad2 = build5GHzFrame(seq++, 0, false, 0);
    bad2.insert(bad2.end(), {0, 4, 0, 0, 0}); // type=0, len=4, only 2 bytes present
    deliver5GHz(bad2);
    assert(driver.awdlReceive.malformed == 2 && driver.awdlReceive.valid == expectedValid);
    return 0;
}
"""
        self.compile_run(harness)

    def test_captured_iphone_sessions_through_iwx_and_native_action_delivery(self):
        source = self.iwx_receive_source().split("\nint main()", 1)[0]
        native = (support.ROOT / "tests/fixtures/awdl_native.cpp").read_text().split("\nint main()", 1)[0]
        native = native.replace("// NATIVE CHANNEL FLAGS", re.search(
            r"enum apple80211_channel_flag\s*\{.*?\};",
            (support.ROOT / "include/Airport/apple80211_var.h").read_text(), re.S)[0])
        native = native.replace("// PRODUCTION", definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::receiveAction"))
        source += "\n" + native + "\n" + (support.ROOT / "tests/fixtures/awdl_capture.cpp").read_text()
        # The upstream corpus uses a fixed, sanitized radiotap layout: channel and RSSI only.
        with tempfile.TemporaryDirectory(prefix="awdl-capture-") as directory:
            for path in sorted((support.ROOT / "tests/fixtures/captures").glob("*.pcap.gz")):
                data = gzip.decompress(path.read_bytes())
                self.assertEqual(data[:24], bytes.fromhex("d4c3b2a1020004000000000000000000ffff00007f000000"))
                offset = 24
                with (Path(directory) / path.name.removesuffix(".pcap.gz")).open("wb") as out:
                    while offset < len(data):
                        sec, usec, length, original = struct.unpack_from("<IIII", data, offset)
                        offset += 16
                        packet = data[offset:offset + length]
                        offset += length
                        self.assertEqual(length, original)
                        self.assertEqual(len(packet), length)
                        radiotap_length = struct.unpack_from("<H", packet, 2)[0]
                        if radiotap_length == 8:
                            self.assertEqual(packet[:8], bytes.fromhex("0000080000000000"))
                            rssi = -45  # TX echoes have no capture RSSI; the manifest fixes channel 44.
                        else:
                            self.assertEqual(packet[:8], bytes.fromhex("00000d0028000000"))
                            frequency, flags, rssi = struct.unpack_from("<HHb", packet, 8)
                            self.assertEqual(frequency, 5220)
                            self.assertEqual(flags, 0x140)
                        raw = packet[radiotap_length:]
                        out.write(struct.pack("<QIhh", sec * 1000000 + usec, len(raw), 44, rssi))
                        out.write(raw)
                self.assertEqual(offset, len(data))
            self.compile_run("#define CAPTURE_DIRECTORY " + json.dumps(directory) + "\n" + source)

    def test_iwx_receive_clock_wrap_latency_and_power_access(self):
        source = (support.ROOT / "tests/fixtures/iwx_receive_clock.cpp").read_text()
        source = source.replace("// PRODUCTION", definition(
            "itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::awdlReceiveDelay"))
        self.compile_run(source)

    def test_receive_snapshot_first_frame_and_refresh(self):
        source = (support.ROOT / "tests/fixtures/awdl_registry.cpp").read_text()
        source = source.replace("// RX DIAGNOSTIC DECLARATIONS", receive_diagnostics())
        source = source.replace("// PRODUCTION\n", "\n".join(definition(
            "itlwm/hal_iwx/ItlIwx.cpp", name) for name in (
                "ItlIwx::formatAWDLReceiveDiagnostics", "ItlIwx::publishAWDLReceive")))
        source = source.replace("// FIRMWARE DECLARATIONS", firmware_declarations((), source))
        self.compile_run(source)

    def run_sync_case(self, case):
        source = (support.ROOT / "tests/fixtures/awdl_sync.cpp").read_text()
        self.compile_run("#define AWDL_SYNC_CASE " + str(case) + "\n" + source)

    def test_transmitted_action_wire_format_and_bounds(self):
        self.run_sync_case(1)

    def test_master_election_expiry_cycles_and_local_clock_reset(self):
        self.run_sync_case(2)

    def test_unicast_and_multicast_rendezvous_windows(self):
        self.run_sync_case(3)

    def test_native_multicast_slots_sparse_peer_schedule_and_counter_wrap(self):
        self.run_sync_case(9)

    def test_social_channel_schedule_announces_home_absence(self):
        self.run_sync_case(4)

    def test_election_rssi_id_and_local_master_channel_restoration(self):
        self.run_sync_case(5)

    def test_native_election_template_authority_parent_clock_and_expiry(self):
        self.run_sync_case(6)

    def test_fixed_and_forced_parent_modes_and_warmup_clock_adoption(self):
        self.run_sync_case(7)

    def test_native_parent_clock_timestamp_wrap_async_delivery_and_expiry(self):
        self.compile_run((support.ROOT / "tests/fixtures/awdl_parent.cpp").read_text())

    def test_native_minimum_rate_drives_legacy_and_new_firmware_tx_encodings(self):
        source = (support.ROOT / "tests/fixtures/awdl_rate.cpp").read_text()
        driver = (support.ROOT / "itlwm/hal_iwx/ItlIwx.cpp").read_text()
        table = re.search(r"const struct iwx_rate \{.*?\} iwx_rates\[\] = \{.*?^\};", driver, re.S|re.M)[0]
        net = (support.ROOT / "itl80211/openbsd/net80211/ieee80211.c").read_text()
        rates = re.search(r"const struct ieee80211_rateset ieee80211_std_rateset_11a =.*?;", net, re.S)[0]
        routines = "\n".join(definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name)
                             for name in ("iwx_rval2ridx", "iwx_tx_fill_awdl_cmd"))
        header = (support.ROOT / "itlwm/hal_iwx/if_iwxreg.h").read_text()
        pending = set(re.findall(r"\bIWX_(?:RATE_\w+|FW_CMD_VER_UNKNOWN)", table + routines))
        declarations = {}
        while pending:
            name = pending.pop()
            if name in declarations:
                continue
            line = re.search(r"^#define " + name + r"\s+[^\n]+", header, re.M)[0]
            declarations[name] = line
            pending.update(set(re.findall(r"\bIWX_\w+", line)) - declarations.keys())
        source = source.replace("// RATE DECLARATIONS", "\n".join(declarations.values()) + "\n" + table + rates)
        self.compile_run(source.replace("// PRODUCTION", routines))

    def run_radio_case(self, case):
        source = (support.ROOT / "tests/fixtures/awdl_radio.cpp").read_text()
        source = source.replace("// PEER TX SCENARIOS", (support.ROOT / "tests/fixtures/awdl_tx_transport.cpp").read_text())
        debug = case in (19, 20, 23, 25, 27, 28, 30, 40)
        routines = ("awdlMAC", "awdlChannelAllowed", "awdlWideChannelAllowed", "awdlPHY", "awdlBinding",
                    "receiveAWDLSession", "finishAWDLDwell", "serviceAWDLChannel",
                    "prepareAWDLListener", "closeAWDLListenerWindow", "serviceAWDLListener", "configureAWDLPeerWidth",
                    "startAWDLRadio", "awdlRemoveQueue",
                    "stopAWDLRadio", "resetAWDLRadio", "awdlTxWindowOpen", "transmitAWDL", "completeAWDLTx",
                    "sampleAWDLRate", "noteAWDLTXBlockAck", "awdlTXAntenna", "awdlTXMcs", "awdlTXWidth")
        if debug:
            routines += ("awdlCompareArm",)
        source = source.replace("// PRODUCTION", "\n".join(definition(
            "itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::" + name) for name in routines))
        source = source.replace('#include "tests/fixtures/awdl_transport.hpp"',
                                (support.ROOT / "tests/fixtures/awdl_transport.hpp").read_text())
        source = source.replace('#include "itlwm/hal_iwx/AWDLTxBudget.hpp"',
                                (support.ROOT / "itlwm/hal_iwx/AWDLTxBudget.hpp").read_text())
        transport = "\n".join(definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name)
            for name in ("iwx_rval2ridx", "iwx_toggle_tx_ant", "iwx_tx_fill_awdl_cmd", "iwx_tx_update_byte_tbl", "iwx_tx",
                         "iwx_rx_tx_cmd_single", "iwx_rx_tx_cmd", "iwx_txd_done",
                         "iwx_ampdu_txq_advance", "iwx_reset_tx_ring"))
        transport += "\n" + definition("itlwm/hal_iwx/ItlIwx.cpp", "iwx_clear_tx_desc")
        transport += "\n" + definition("itlwm/hal_iwx/ItlIwx.cpp", "iwx_awdl_is_quz_0034")
        driver = (support.ROOT / "itlwm/hal_iwx/ItlIwx.cpp").read_text()
        table = re.search(r"const struct iwx_rate \{.*?\} iwx_rates\[\] = \{.*?^\};", driver, re.S|re.M)[0]
        net = (support.ROOT / "itl80211/openbsd/net80211/ieee80211.c").read_text()
        rates = re.search(r"const struct ieee80211_rateset ieee80211_std_rateset_11a =.*?;", net, re.S)[0]
        source = source.replace("struct ItlIwx {", table + rates + "\nstruct ItlIwx {")
        source = source.replace("static const uint8_t self", transport + "\nstatic const uint8_t self")
        source = source.replace("// TRANSPORT TYPES", "void iwx_clear_tx_desc(struct iwx_softc *,struct iwx_tx_ring *,int);")
        source = source.replace("// FIRMWARE DECLARATIONS", firmware_declarations(
            ("iwx_mac_data_ap", "iwx_mac_data_ibss", "iwx_mac_data_sta", "iwx_mac_data_go",
             "iwx_mac_data_p2p_sta", "iwx_mac_data_pibss", "iwx_mac_data_p2p_dev", "iwx_ac_qos",
             "iwx_mac_ctx_cmd", "iwx_fw_channel_info", "iwx_phy_context_cmd", "iwx_binding_cmd",
             "iwx_rlc_properties", "iwx_sad_properties", "iwx_rlc_config_cmd",
             "iwx_device_power_cmd", "iwx_add_sta_cmd", "iwx_rm_sta_cmd",
             "iwx_session_prot_cmd", "iwx_session_prot_notif", "iwx_tx_data", "iwx_tfh_tb", "iwx_tfh_tfd",
             "iwx_cmd_header", "iwx_cmd_header_wide", "iwx_device_cmd", "iwx_dram_sec_info", "iwx_tx_cmd_gen2", "iwx_tx_cmd_gen3",
             "iwx_agn_scd_bc_tbl", "iwx_gen3_bc_tbl", "iwx_agg_tx_status", "iwx_tx_resp"),
            source, ("iwx_gen2_tx_fifo",)))
        self.compile_run(("#define DEBUG 1\n" if debug else "") +
                         "#define AWDL_RADIO_CASE " + str(case) + "\n" + source)

    def test_listener_keeps_receive_context_between_transmit_windows(self):
        self.run_radio_case(36)

    def test_listener_close_preserves_negotiation_receive_readiness(self):
        self.run_radio_case(48)

    def test_listener_retune_preserves_negotiation_and_dma_ownership(self):
        self.run_radio_case(49)

    def test_listener_retunes_without_taking_infrastructure_frames(self):
        self.run_radio_case(37)

    def test_listener_start_failure_reset_and_disable_unwind(self):
        self.run_radio_case(38)

    def test_listener_queue_deadline_and_flush_failure_ownership(self):
        self.run_radio_case(39)

    def test_listener_debug_build_receive_filter(self):
        self.run_radio_case(40)

    def test_listener_retune_disable_reset_and_command_failure(self):
        self.run_radio_case(41)

    def test_listener_reuses_both_transmit_queues_after_flush(self):
        self.run_radio_case(42)

    def test_amsdu_transport_wire_descriptors_and_exact_rate_reports(self):
        self.run_radio_case(31)

    def test_amsdu_transport_rechecks_ht_and_shares_airtime_across_tids(self):
        self.run_radio_case(32)

    def test_amsdu_queue_partial_setup_deadline_reset_and_teardown(self):
        self.run_radio_case(33)

    def test_amsdu_late_queue_preserves_grant_and_outstanding_packets(self):
        self.run_radio_case(34)

    def test_amsdu_late_queue_deadline_start_end_reset_and_command_failure(self):
        self.run_radio_case(35)

    def test_amsdu_payload_uses_real_dma_mapping_and_segment_limits(self):
        source = (support.ROOT / "tests/fixtures/awdl_amsdu_dma.cpp").read_text()
        routines = "\n".join(definition("itl80211/compat.cpp", name) for name in
            ("bus_dmamap_create", "bus_dmamap_write", "bus_dmamap_packet"))
        self.compile_run(source.replace("// PRODUCTION", routines))

    def test_radio_resources_preserve_infrastructure_context(self):
        self.run_radio_case(1)

    def test_radio_command_failure_and_reset_do_not_release_owned_dma(self):
        self.run_radio_case(2)

    def test_raw_tx_queue_ownership_and_address_boundary(self):
        self.run_radio_case(3)

    def test_firmware_dwell_events_cancel_reset_and_duplicate_start(self):
        self.run_radio_case(4)

    def test_firmware_dwell_end_matches_mac_without_start_config_filter(self):
        self.run_radio_case(5)

    def test_radio_transport_action_ipv6_dma_completion_and_cross_channel_lifecycle(self):
        self.run_radio_case(6)

    def test_quz_awdl_broadcast_tx_uses_available_nonshared_antenna_under_high_bt_load(self):
        self.run_radio_case(21)

    def test_awdl_management_reclaim_selects_next_raw_tx_antenna(self):
        self.run_radio_case(22)

    def test_quz_0034_airdrop_announcements_and_ipv6_use_the_verified_antenna(self):
        self.run_radio_case(24)
        self.run_radio_case(25)

    def test_debug_broadcast_antenna_comparison_at_descriptor_publication(self):
        self.run_radio_case(23)

    def test_radio_transport_backpressure_mapping_failure_and_missing_end_ownership(self):
        self.run_radio_case(7)

    def test_radio_cancel_ack_and_flush_complete_without_end(self):
        self.run_radio_case(8)

    def test_radio_end_before_cancel_and_during_cancel_or_flush(self):
        self.run_radio_case(9)

    def test_radio_cancel_flush_failure_latches_and_epoch_reset_owns_dma(self):
        self.run_radio_case(10)

    def test_radio_disable_restart_without_end_preserves_real_fault(self):
        self.run_radio_case(11)

    def test_radio_short_grants_tx_deadline_and_same_eaw_reentry(self):
        self.run_radio_case(12)
        self.run_radio_case(28)

    def test_radio_end_gap_survives_disable_and_channel_transition(self):
        self.run_radio_case(13)

    def test_radio_repeated_cancel_and_late_end_cannot_wedge_or_revive_tx(self):
        self.run_radio_case(14)

    def test_radio_empty_flush_ack_retains_tokens_until_station_removal(self):
        self.run_radio_case(15)

    def test_awdl_phy1_rlc_v2_wire_configuration_cache_and_legacy(self):
        self.run_radio_case(16)

    def test_awdl_phy1_rlc_failure_restores_unbound_channel(self):
        self.run_radio_case(17)

    def test_awdl_phy1_rlc_reset_and_disable_during_commands(self):
        self.run_radio_case(18)

    def test_broadcast_firmware_reports_exclude_unicast_data_and_implicit_status(self):
        self.run_radio_case(19)

    def test_tx_during_dma_requires_the_same_live_grant_at_ring_publication(self):
        self.run_radio_case(20)

    def test_nonblocking_end_drain_native_disable_and_restart(self):
        self.run_tick_case(19)

    def test_logical_windows_resume_and_unicast_crosses_short_grant_gaps(self):
        self.run_tick_case(20)

    def test_native_unicast_retries_rejected_dma_within_original_request_window(self):
        self.run_tick_case(24)

    def test_native_link_survives_transient_infrastructure_loss(self):
        self.run_tick_case(25)

    def test_native_sync_state_follows_real_radio_readiness(self):
        for debug in (False, True):
            with self.subTest(debug=debug):
                self.run_tick_case(26, debug=debug)

    def test_native_tx_activity_records_submission_without_waiting_for_ack(self):
        for debug in (False, True):
            with self.subTest(debug=debug):
                self.run_tick_case(29, debug=debug)

    def test_80mhz_phy_positions_cache_rollback_and_listener_retune(self):
        self.run_radio_case(47)

    def test_data_burst_fits_remaining_firmware_window(self):
        self.run_radio_case(26)
        self.run_radio_case(27)

    def test_unicast_rate_uses_explicit_firmware_samples_and_peer_lifetime(self):
        self.run_radio_case(29)
        self.run_radio_case(30)

    def test_data_submission_preserves_the_qualified_recipient_cutoff(self):
        for debug in (False, True):
            with self.subTest(debug=debug):
                self.run_tick_case(30, debug=debug)

    def test_native_action_body_metadata_and_pool_ownership(self):
        source = (support.ROOT / "tests/fixtures/awdl_native.cpp").read_text()
        source = source.replace("// NATIVE CHANNEL FLAGS", re.search(
            r"enum apple80211_channel_flag\s*\{.*?\};",
            (support.ROOT / "include/Airport/apple80211_var.h").read_text(), re.S)[0])
        source = source.replace("// PRODUCTION", definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::receiveAction"))
        self.compile_run(source)

    def test_multi_peer_service_tlv_merge_and_sui_reset(self):
        source = (support.ROOT / "tests/fixtures/awdl_native.cpp").read_text()
        source = source.replace("// NATIVE CHANNEL FLAGS", re.search(
            r"enum apple80211_channel_flag\s*\{.*?\};",
            (support.ROOT / "include/Airport/apple80211_var.h").read_text(), re.S)[0])
        source = source.replace("// PRODUCTION", definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::receiveAction"))
        self.compile_run("#define AWDL_SERVICE_MERGE_TEST 1\n" + source)

    def test_captured_iphone_service_tlv_merge_across_sui_transition(self):
        native = (support.ROOT / "tests/fixtures/awdl_native.cpp").read_text().split("\nint main()", 1)[0]
        native = native.replace("// NATIVE CHANNEL FLAGS", re.search(
            r"enum apple80211_channel_flag\s*\{.*?\};",
            (support.ROOT / "include/Airport/apple80211_var.h").read_text(), re.S)[0])
        native = native.replace("// PRODUCTION", definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::receiveAction"))
        capture_path = support.ROOT / "tests/fixtures/captures/send-ios26.x-mt7925e-ch44-1.pcap.gz"
        data = gzip.decompress(capture_path.read_bytes())
        with tempfile.TemporaryDirectory(prefix="awdl-svc-merge-") as directory:
            bin_path = Path(directory) / "send.bin"
            offset = 24
            with bin_path.open("wb") as out:
                while offset < len(data):
                    sec, usec, length, _ = struct.unpack_from("<IIII", data, offset)
                    offset += 16
                    packet = data[offset:offset + length]
                    offset += length
                    rt = struct.unpack_from("<H", packet, 2)[0]
                    raw = packet[rt:]
                    out.write(struct.pack("<QI", sec * 1000000 + usec, len(raw)))
                    out.write(raw)
            harness = f"""
#define AWDL_SERVICE_MERGE_TEST 1
#include <fstream>
{native}
int main() {{
    AirportItlwmAWDLInterface iface;
    assert(iface.serviceMergeEnabled);
    std::ifstream in({json.dumps(str(bin_path))}, std::ios::binary);
    assert(in.is_open());
    unsigned mifs = 0;
    while (in) {{
        uint64_t us = 0; uint32_t len = 0;
        in.read(reinterpret_cast<char *>(&us), 8);
        if (in.eof()) break;
        in.read(reinterpret_cast<char *>(&len), 4);
        std::vector<uint8_t> raw(len);
        in.read(reinterpret_cast<char *>(raw.data()), len);
        assert(in);
        Advertisement adv = {{}};
        if (parseMPDU(raw.data(), raw.size(), adv) != ParseResult::Valid) continue;
        const size_t before = iface.taps;
        iface.receiveAction(&iface, raw.data(), raw.size(), 44, -45, 42000000 + us);
        assert(iface.taps == before + 1);
        assert(iface.payload.size() <= ServiceMergeCache::MaxMergedBodyBytes);
        Advertisement mergedAdv = {{}};
        assert(parseAction(iface.payload.data(), iface.payload.size(), mergedAdv) == ParseResult::Valid);
        if (adv.subtype == 0) {{
            const size_t hdr = (raw[1] & 0x80) ? 28 : 24;
            assert(iface.payload.size() == raw.size() - hdr);
        }} else {{
            ++mifs;
        }}
    }}
    assert(mifs > 400);
    assert(iface.serviceMerge.mergedFrames == 0);
    assert(iface.serviceMerge.resets == 1);
    assert(iface.serviceMerge.overflow == 0);

    // Second pass: split real iOS 26 TLV 2 records across alternating MIFs and verify full reconstruction + SUI reset.
    AirportItlwmAWDLInterface splitIface;
    std::ifstream in2({json.dumps(str(bin_path))}, std::ios::binary);
    unsigned parity = 0;
    while (in2) {{
        uint64_t us = 0; uint32_t len = 0;
        in2.read(reinterpret_cast<char *>(&us), 8);
        if (in2.eof()) break;
        in2.read(reinterpret_cast<char *>(&len), 4);
        std::vector<uint8_t> raw(len);
        in2.read(reinterpret_cast<char *>(raw.data()), len);
        assert(in2);
        Advertisement adv = {{}};
        if (parseMPDU(raw.data(), raw.size(), adv) != ParseResult::Valid) continue;
        if (adv.subtype == 3) {{
            const size_t hdr = (raw[1] & 0x80) ? 28 : 24;
            std::vector<uint8_t> trimmed(raw.begin(), raw.begin() + hdr + 16);
            size_t off = hdr + 16;
            unsigned idx = 0;
            while (off + 3 <= raw.size()) {{
                size_t n = size_t(raw[off + 1]) | (size_t(raw[off + 2]) << 8);
                if (raw[off] != 2 || ((idx++ & 1) == (parity & 1)))
                    trimmed.insert(trimmed.end(), raw.begin() + off, raw.begin() + off + 3 + n);
                off += 3 + n;
            }}
            ++parity;
            raw.swap(trimmed);
        }}
        splitIface.receiveAction(&splitIface, raw.data(), raw.size(), 44, -45, 52000000 + us);
        Advertisement mergedAdv = {{}};
        assert(parseAction(splitIface.payload.data(), splitIface.payload.size(), mergedAdv) == ParseResult::Valid);
    }}
    assert(splitIface.serviceMerge.mergedFrames > 400);
    assert(splitIface.serviceMerge.mergedRecords > 1200);
    assert(splitIface.serviceMerge.resets == 1);
    assert(splitIface.serviceMerge.overflow == 0);
}}
"""
            self.compile_run(harness)

    def run_tick_case(self, case, debug=False):
        source = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text()
        source = source.replace("// NATIVE TX SCENARIOS", (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text())
        source = source.replace("// PRODUCTION", awdl_native_sync.routines() + "\n" + "\n".join(definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + name)
            for name in ("drain", "releaseData", "peerGrantEnd", "recordNativeDataActivity", "tick", "pullOutputPackets", "setEnabled", "setInterfaceEnable", "updateAvailability", "queueUnicast", "serviceUnicast", "completeUnicast", "completeData", "configureBroadcast", "broadcastDue", "psfDue", "nextManagementToken", "receiveBlockAck", "receiveBlockAckEnd", "serviceBlockAck", "serviceTXBlockAck")))
        headers = (support.ROOT / "include/Airport/apple80211_var.h").read_text() + "\n" + (
            support.ROOT / "include/Airport/apple80211_ioctl.h").read_text()
        declarations = [re.search(r"struct " + name + r"\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;",
                                 headers, re.S)[0] for name in ("apple80211_channel",
            "apple80211_availability_window_start", "apple80211_availability_window_extensions_end", "apple80211_awdl_oob_request")]
        declarations.append(awdl_native_sync.declaration())
        for name in ("APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START",
                     "APPLE80211_M_AWDL_AVAILABILITY_WINDOW_EXTENSIONS_END", "APPLE80211_M_AWDL_UNICAST_AF_STATUS"):
            declarations.append(re.search(r"^#define " + name + r"\s+\d+", headers, re.M)[0])
        source = source.replace("// NATIVE DECLARATIONS", "\n".join(declarations))
        self.compile_run(f"#define DEBUG {int(debug)}\n#define AWDL_TICK_CASE {case}\n" + source)

    def test_block_ack_acceptance_waits_for_session_and_preserves_response_order(self):
        self.run_tick_case(38)

    def test_action_timers_and_disable_during_firmware_command(self):
        self.run_tick_case(1)

    def test_listener_close_timer_and_native_schedule_revalidation(self):
        self.run_tick_case(37)

    def test_data_flush_single_retry_sequence_epoch_deadline_and_capacity(self):
        self.run_tick_case(21)

    def test_isolated_grant_demand_tracks_receiving_and_inflight_data(self):
        self.run_tick_case(27)

    def test_shared_channel_demand_keeps_the_anchor_peer_deadline(self):
        self.run_tick_case(32)

    def test_multicast_pair_uses_slot8_and_other_social_windows_independently(self):
        self.run_tick_case(28)

    def test_block_ack_decline_queue_windows_completion_and_teardown(self):
        self.run_tick_case(31)

    def test_pending_data_expiry_queue_pressure_and_radio_loss(self):
        self.run_tick_case(2)

    def test_native_configuration_survives_radio_restart(self):
        self.run_tick_case(3)

    def test_action_submission_retries_and_priority_over_pending_data(self):
        self.run_tick_case(4)

    def test_output_backpressure_batch_limit_wakeup_and_restart(self):
        self.run_tick_case(5)

    def test_cache_registration_sync_pause_and_native_interface_disable(self):
        self.run_tick_case(6)

    def test_native_availability_window_messages_retry_expiry_and_disable(self):
        self.run_tick_case(7)

    def test_native_unicast_owned_payload_ack_completion_and_capacity(self):
        self.run_tick_case(8)

    def test_native_unicast_expiry_retry_reset_and_stale_completion(self):
        self.run_tick_case(9)

    def test_native_broadcast_channel_map_offset_and_payload_separation(self):
        self.run_tick_case(10)

    def test_native_broadcast_retry_expiry_disable_and_unicast_progress(self):
        self.run_tick_case(11)

    def test_native_psf_suppression_keeps_mif_unicast_and_ipv6_running(self):
        self.run_tick_case(12)

    def test_pending_ipv6_rechecks_peer_window_after_allocation(self):
        self.run_tick_case(13)

    def test_pending_ipv6_resource_retry_preserves_payload_sequence_and_expiry(self):
        self.run_tick_case(14)

    def test_native_window_survives_parent_clock_corrections_and_umi(self):
        self.run_tick_case(15)

    def test_native_window_overlap_counter_changes_and_firmware_deadlines(self):
        self.run_tick_case(16)

    def test_native_broadcast_clock_corrections_do_not_duplicate_mif(self):
        self.run_tick_case(17)

    def test_multicast_output_reaches_native_discovery_windows(self):
        self.run_tick_case(18)

    def test_bonjour_multicast_without_a_scheduled_peer(self):
        self.run_tick_case(22)

    def test_election_windows_during_fixed_relay_loss(self):
        self.run_tick_case(23)

    def test_direct_parent_loss_reports_only_real_election_receive_windows(self):
        for debug in (False, True):
            with self.subTest(debug=debug):
                self.run_tick_case(33, debug=debug)

    def test_iwx_unicast_completion_batch_mapping_flush_and_reset(self):
        source = (support.ROOT / "tests/fixtures/awdl_completion.cpp").read_text()
        definitions = []
        for path, names in (("itlwm/hal_iwx/if_iwxreg.h", ("iwx_agg_tx_status", "iwx_tx_resp",
                             "iwx_tx_path_flush_cmd", "iwx_flush_queue_info", "iwx_tx_path_flush_cmd_rsp")),
                            ("itlwm/hal_iwx/if_iwxvar.h", ("iwx_tx_data",))):
            header = (support.ROOT / path).read_text()
            definitions.extend(re.search(r"struct " + name + r"\s*\{.*?\}[^;]*;", header, re.S)[0]
                               for name in names)
        source = source.replace("// NATIVE DECLARATIONS", "#define IWX_TX_FLUSH_QUEUE_RSP 16\n" + "\n".join(definitions))
        routines = definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::completeAWDLTx") + "\n"
        routines += "\n".join(definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name) for name in
            ("iwx_rx_tx_cmd_single", "iwx_rx_tx_cmd", "iwx_txd_done", "iwx_ampdu_txq_advance", "iwx_reset_tx_ring", "iwx_flush_sta_tids"))
        self.compile_run(source.replace("// PRODUCTION", routines))

    def test_unicast_management_wire_payload_and_bounds(self):
        self.run_sync_case(8)

    def run_control_case(self, case):
        source = (support.ROOT / "tests/fixtures/awdl_control.cpp").read_text()
        handle = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp",
                            "AirportItlwmAWDLInterface::handle") + "\n" + definition(
                            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
                            "AirportItlwmAWDLInterface::updatePeerCache")
        handle += "\n" + "\n".join(definition("AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmAWDLInterface::" + name) for name in ("configureBroadcast", "broadcastDue"))
        declarations = []
        headers = {name: (support.ROOT / "include/Airport" / name).read_text()
                   for name in ("apple80211_var.h", "apple80211_ioctl.h")}
        names = ["apple80211_channel", "awdl_peer_advert_add"] + sorted(
            set(re.findall(r"\bapple80211_\w+", handle)) | {"apple80211_awdl_af_tx_mode"})
        for name in names:
            pattern = r"struct " + name + r"\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;"
            matches = [re.search(pattern, header, re.S) for header in headers.values()]
            declaration = next((m[0] for m in matches if m), None)
            if declaration is None:
                raise ValueError("Missing native declaration: " + name)
            declarations.append(declaration)
        interface = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()
        request = re.search(r"enum class Request\s*\{.*?\};", interface, re.S)[0]
        declarations.append(re.search(r"^#define APPLE80211_M_PEER_CACHE_CONTROL\s+\d+",
                                      headers["apple80211_var.h"], re.M)[0])
        source = source.replace("// NATIVE DECLARATIONS", "\n".join(declarations))
        source = source.replace("// NATIVE REQUESTS", request)
        self.compile_run("#define AWDL_CONTROL_CASE " + str(case) + "\n" + source.replace("// PRODUCTION", handle))

    def test_native_sync_controls_and_election_wire_values(self):
        self.run_control_case(0)

    def test_native_peer_cache_requests_indications_and_failed_enqueue(self):
        self.run_control_case(1)

    def test_native_data_path_template_preservation_and_replacement(self):
        self.run_control_case(2)

    def test_native_broadcast_request_dispatch_ownership_and_invalid_replacement(self):
        self.run_control_case(3)

    def test_native_psf_mode_request_layout_and_policy_transitions(self):
        self.run_control_case(4)

    def test_native_80mhz_channel_metadata_matches_both_wire_sequences(self):
        self.run_control_case(6)

    def test_native_channel_metadata_roundtrip_primary20_wire_and_atomic_rejection(self):
        self.run_control_case(5)


if __name__ == "__main__":
    unittest.main()
