"""Failing-first regressions for Continuity-WiFi-Preview77 Sidecar & AirDrop fixes."""
import re
import unittest

import awdl_native_sync
from iwx_firmware import declarations as firmware_declarations
import test_regressions as support
from test_offline import definition


class Preview77SidecarAirDropRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_sidecar_contiguous_slot_listener_and_tx_ba_continuity(self):
        # 1. Verify real ItlIwx::serviceAWDLListener keeps a contiguous run open across
        #    adjacent 149 slots without flushing TX rings, then closes ahead of 149->48.
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
        radio_src += r"""
int main() {
    ItlIwx d;
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    for (unsigned c : {149U, 153U, 157U, 161U}) {
        d.com.sc_ic.ic_channels[c] = {IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_VHT80, 5000 + 5 * c};
        d.com.sc_ic.ic_chan_active[c / 8] |= 1U << (c % 8);
    }
    assert(!d.startAWDLRadio(self));
    AirportAWDL::SyncEngine sync = {};
    sync.start(self, 149, clockUS);
    const uint16_t sidecarSeq[16] = {149, 149, 149, 149, 149, 149, 48, 48, 6, 149, 149, 149, 149, 149, 48, 48};
    for (unsigned s = 0; s < 16; ++s) {
        sync.channels[s] = sidecarSeq[s];
        sync.channelClasses[s] = sidecarSeq[s] == 149 ? 128 : 0;
    }
    const uint64_t epoch = sync.epochUS;
    // Queue an in-flight MIF/data frame and step across all 6 contiguous 149 slots.
    clockUS = epoch + 4000;
    AirportAWDL::Window w = {};
    assert(sync.window(clockUS, w));
    assert(w.widthMHz() == 80);
    // Window must expose contiguous channel run timing across all 6 slots.
    assert(w.untilChannelChangeUS == 6ULL * 65536 - 4000);
    assert(w.sinceChannelStartUS == 4000);

    RadioState::GrantPolicy policy = {};
    policy.slot = w.runStartEAW;
    policy.widthMHz = w.widthMHz();
    assert(!d.serviceAWDLChannel(w.channel, clockUS + w.untilChannelChangeUS, policy));
    assert(d.awdlRadio.widthMHz == 80 && d.awdlRadio.dwellStarts == 1 && d.awdlRadio.dwellEnds == 0);
    auto mif = packet();
    assert(!d.transmitAWDL(&mif, 2001));

    // Step across every internal 65.536 ms EAW boundary (slots 0->1, 1->2, ..., 4->5):
    // neither flush nor dwell restart may occur, and localDataWindow stays open 500 us after boundary.
    for (unsigned slot = 1; slot < 6; ++slot) {
        for (int delta : {-2000, -500, 500, 4000}) {
            clockUS = uint64_t(int64_t(epoch + slot * 65536ULL) + delta);
            AirportAWDL::Window cur = {};
            assert(sync.localDataWindow(clockUS, false, cur, true));
            assert(cur.runStartEAW == w.runStartEAW);
            policy.slot = cur.runStartEAW;
            policy.widthMHz = cur.widthMHz();
            assert(!d.serviceAWDLChannel(cur.channel, clockUS + cur.untilChannelChangeUS, policy));
            assert(d.awdlTxWindowOpen());
            assert(d.awdlRadio.dwellStarts == 1 && d.awdlRadio.dwellEnds == 0);
            assert(commandCount(d, "flush") == 0 && !mif.frees);
        }
    }
    // 2 ms before slot 5 -> slot 6 (149 -> 48), listener window closes and flushes cleanly.
    clockUS = epoch + 6ULL * 65536 - 2000;
    assert(sync.window(clockUS, w) && w.untilChannelChangeUS == 2000);
    assert(!sync.localDataWindow(clockUS, false, w, true));
    assert(!d.awdlTxWindowOpen());
    policy.slot = w.runStartEAW;
    assert(!d.serviceAWDLChannel(w.channel, clockUS + w.untilChannelChangeUS, policy));
    assert(d.awdlRadio.dwellEnds == 1 && commandCount(d, "flush") == 1 && mif.frees == 1);
    assert(!d.stopAWDLRadio());
    return 0;
}
"""
        self.compile_run(radio_src)

        # 2. Verify AirportItlwmAWDLInterface::tick + serviceTXBlockAck keep TX BA unpaused
        #    across contiguous 149->149 boundaries while still posting per-EAW availability events.
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        tick_src = tick_src.replace(
            "// PRODUCTION",
            awdl_native_sync.routines() + "\n" + "\n".join(
                definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
                for n in (
                    "drain", "releaseData", "peerGrantEnd", "recordNativeDataActivity",
                    "tick", "pullOutputPackets", "setEnabled", "setInterfaceEnable",
                    "updateAvailability", "queueUnicast", "serviceUnicast",
                    "completeUnicast", "completeData", "configureBroadcast",
                    "broadcastDue", "psfDue", "nextManagementToken",
                    "receiveBlockAck", "receiveBlockAckEnd", "serviceBlockAck",
                    "serviceTXBlockAck",
                )
            ),
        )
        headers = (support.ROOT / "include/Airport/apple80211_var.h").read_text() + "\n" + (
            support.ROOT / "include/Airport/apple80211_ioctl.h"
        ).read_text()
        decls = [
            re.search(r"struct " + name + r"\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;", headers, re.S)[0]
            for name in (
                "apple80211_channel",
                "apple80211_availability_window_start",
                "apple80211_availability_window_extensions_end",
                "apple80211_awdl_oob_request",
            )
        ]
        decls.append(awdl_native_sync.declaration())
        for name in (
            "APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START",
            "APPLE80211_M_AWDL_AVAILABILITY_WINDOW_EXTENSIONS_END",
            "APPLE80211_M_AWDL_UNICAST_AF_STATUS",
        ):
            decls.append(re.search(r"^#define " + name + r"\s+\d+", headers, re.M)[0])
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", "\n".join(decls))
        tick_src += r"""
int main() {
    clockUS = 10000000;
    const uint8_t peerAddr[] = {2, 0, 0, 0, 0, 9};
    AirportItlwmAWDLInterface i;
    i.device.awdlRadio.listener = true;
    i.tick();
    const uint16_t seq[16] = {149, 149, 149, 149, 149, 149, 0, 0, 6, 149, 149, 149, 149, 149, 0, 0};
    for (unsigned s = 0; s < 16; ++s) i.sync.channels[s] = seq[s];
    i.device.awdlRadio.dwell = {};
    const uint64_t epoch = i.sync.epochUS;

    // Configure matching Sidecar schedule on peer and active TX BlockAck session.
    Advertisement adv = {};
    adv.fields = Sync | Channels | ElectionV2 | HTCapabilities;
    adv.windowPeriodTU = 16;
    adv.presenceMode = 4;
    adv.windowCounter = i.sync.epochCounter;
    adv.nextWindowTU = 64;
    adv.masterChannel = 149;
    copyAddress(adv.syncMaster, i.address.octet);
    adv.sequence.count = 16;
    adv.sequence.encoding = 3;
    adv.sequence.step = 3;
    adv.sequence.fill = 0xffff;
    for (unsigned s = 0; s < 16; ++s) {
        adv.sequence.channel[s] = seq[s];
        adv.sequence.flags[s] = operatingClass(seq[s]);
    }
    adv.electionID = i.sync.electionID;
    copyAddress(adv.electionMaster, i.address.octet);
    copyAddress(adv.electionSync, i.address.octet);
    adv.distance = 1;
    adv.rxHTMcs = 0xff;
    clockUS = epoch;
    assert(i.device.awdlPeers.update(peerAddr, i.address.octet, adv, clockUS, 149, -40));

    auto &tx = i.device.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.enabled = true;
    tx.paused = false;
    tx.phase = TXBlockAckQueue::Agreed;
    tx.cookie = 77;
    tx.epoch = i.device.dmaEpoch;
    copyAddress(tx.peer, peerAddr);

    // Tick 4 ms before EAW 0->1 boundary: TX BA must NOT pause and listener window must span 6 slots!
    clockUS = epoch + 65536 - 4000;
    i.nextActionUS = i.nextMIFUS = epoch + 5000000;
    i.tick();
    assert(!tx.paused);
    assert(tx.windowEndUS > epoch + 5ULL * 65536);

    // Step across slots 1..5: availability window advances every 64 TU while TX BA stays unpaused!
    const uint64_t startsBefore = i.nativeWindowStarts;
    for (unsigned slot = 1; slot < 6; ++slot) {
        clockUS = epoch + slot * 65536ULL + 500;
        i.tick();
        assert(!tx.paused);
    }
    assert(i.nativeWindowStarts >= startsBefore + 5);

    // 4 ms before slot 5 -> slot 6 (149 -> 0), TX BA pauses ahead of the 5.484 ms PPDU limit.
    clockUS = epoch + 6ULL * 65536 - 4000;
    i.tick();
    assert(tx.paused);
    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run("#define DEBUG 0\n" + tick_src)

    def test_low_power_multicast_bonjour_uses_social_149_not_infra_slot0(self):
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        tick_src = tick_src.replace(
            "// PRODUCTION",
            awdl_native_sync.routines() + "\n" + "\n".join(
                definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
                for n in (
                    "drain", "releaseData", "peerGrantEnd", "recordNativeDataActivity",
                    "tick", "pullOutputPackets", "setEnabled", "setInterfaceEnable",
                    "updateAvailability", "queueUnicast", "serviceUnicast",
                    "completeUnicast", "completeData", "configureBroadcast",
                    "broadcastDue", "psfDue", "nextManagementToken",
                    "receiveBlockAck", "receiveBlockAckEnd", "serviceBlockAck",
                    "serviceTXBlockAck",
                )
            ),
        )
        headers = (support.ROOT / "include/Airport/apple80211_var.h").read_text() + "\n" + (
            support.ROOT / "include/Airport/apple80211_ioctl.h"
        ).read_text()
        decls = [
            re.search(r"struct " + name + r"\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;", headers, re.S)[0]
            for name in (
                "apple80211_channel",
                "apple80211_availability_window_start",
                "apple80211_availability_window_extensions_end",
                "apple80211_awdl_oob_request",
            )
        ]
        decls.append(awdl_native_sync.declaration())
        for name in (
            "APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START",
            "APPLE80211_M_AWDL_AVAILABILITY_WINDOW_EXTENSIONS_END",
            "APPLE80211_M_AWDL_UNICAST_AF_STATUS",
        ):
            decls.append(re.search(r"^#define " + name + r"\s+\d+", headers, re.M)[0])
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", "\n".join(decls))
        # Allow channel 48 in mock Radio so slot 0 = 48 is radio-allowed (associated 5 GHz AP channel)
        tick_src = tick_src.replace(
            "return width==20 && (c==6||c==44||(c==149&&allow149));",
            "return width==20 && (c==6||c==44||c==48||(c==149&&allow149));",
        )
        tick_src += r"""
int main() {
    clockUS = 10000000;
    AirportItlwmAWDLInterface i;
    i.tick();
    // Exact 24H32 Low Power sequence when associated to AP on channel 48:
    const uint16_t lowPower[16] = {48, 0, 149, 0, 0, 0, 0, 0, 6, 0, 149, 0, 0, 0, 0, 0};
    for (unsigned s = 0; s < 16; ++s) i.sync.channels[s] = lowPower[s];
    const uint64_t epoch = i.sync.epochUS;
    i.nextActionUS = i.nextMIFUS = epoch + 5000000;

    // Enqueue _airdrop._tcp.local mDNS packet during slot 0 (AP channel 48):
    clockUS = epoch + 5000;
    i.network.output.push_back(bonjourPacket(i.address.octet, true));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    assert(i.pendingCount == 2);

    // Slot 0 (ch=48) MUST NOT consume MulticastSocial on the non-social AP channel!
    i.tick();
    assert(i.device.data == 0 && i.pendingCount == 2);

    // Slot 2 (ch=149) MUST transmit the 5 GHz social mDNS packet immediately!
    clockUS = epoch + 2ULL * 65536 + 5000;
    i.tick();
    assert(i.device.data == 1 && i.pendingCount == 1 && i.device.awdlRadio.channel == 149);

    // Slot 8 (ch=6) MUST transmit the paired 2.4 GHz social mDNS packet!
    clockUS = epoch + 8ULL * 65536 + 5000;
    i.tick();
    assert(i.device.data == 2 && i.pendingCount == 0 && i.device.awdlRadio.channel == 6);

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run("#define DEBUG 0\n" + tick_src)

    def test_wcl_country_channels_info_and_160mhz_beacon_unlock_80mhz_awdl(self):
        # 1. AirportItlwmSkywalkInterface must override getCOUNTRY_CHANNELS_INFO under AIRPORT_WCL
        #    instead of returning kIOReturnUnsupported, and copyWCLChannels must align AWDL 80MHz
        #    advertisement with awdlChannelAllowed(ch, 80).
        skywalk_hpp = (support.ROOT / "AirportItlwm/AirportItlwmSkywalkInterface.hpp").read_text()
        self.assertRegex(
            skywalk_hpp,
            r"#ifdef AIRPORT_WCL\s+IOReturn getCOUNTRY_CHANNELS_INFO\s*\(\s*apple80211_channels_info\s*\*\s*\)\s*override;",
        )

        wcl_src = (support.ROOT / "tests/fixtures/wcl_regulatory.cpp").read_text().rsplit("int main()", 1)[0]
        wcl_src = wcl_src.replace(
            "IOReturn copyWCLChannels(void *);",
            "IOReturn copyWCLChannels(void *);\n    IOReturn getCHANNELS_INFO(apple80211_channels_info *d) { return copyWCLChannels(d); }\n    IOReturn getCOUNTRY_CHANNELS_INFO(apple80211_channels_info *);",
        )
        wcl_src = "struct apple80211_channels_info {};\n" + wcl_src
        routines = (
            definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::awdlChannelAllowed") + "\n"
            + definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::copyWCLChannels") + "\n"
            + definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::getCOUNTRY_CHANNELS_INFO")
        )
        wcl_src = wcl_src.replace("// PRODUCTION", routines) + r"""
int main() {
    AirportItlwmSkywalkInterface s;
    auto &ic = s.device.com.sc_ic;
    for (unsigned c : {6U, 48U, 149U, 153U, 157U, 161U}) {
        ic.ic_chan_active[c] = true;
        ic.ic_channels[c] = {c == 6 ? 2437U : 5000 + 5 * c, unsigned(c == 6 ? IEEE80211_CHAN_2GHZ :
            IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_PASSIVE | HT40 | VHT80 | IEEE80211_CHAN_VHT80)};
    }
    // Only 20 MHz beacon on 149: 149 is AWDL-allowed at 20 MHz, NOT at 80 MHz -> supports80MHz must be 0
    // on 149 so IO80211AWDLPeerManager does not request rejected 80 MHz flags, while STA ch 48 keeps 80 MHz!
    ic.ic_beacon_hint[149] = true;
    AirportWCL::NativeChannelInfo info = {};
    assert(s.getCOUNTRY_CHANNELS_INFO(reinterpret_cast<apple80211_channels_info *>(&info)) == kIOReturnSuccess);
    assert(info.channel[1] == 48 && info.supports80MHz[1]);
    assert(info.channel[2] == 149 && !(info.channelBitmap[2] & 0x100) && !info.supports80MHz[2]);

    // All four 149..161 subchannels hinted: awdlChannelAllowed(149, 80) is true -> supports80MHz becomes 1!
    for (unsigned c : {149U, 153U, 157U, 161U}) ic.ic_beacon_hint[c] = true;
    assert(s.getCOUNTRY_CHANNELS_INFO(reinterpret_cast<apple80211_channels_info *>(&info)) == kIOReturnSuccess);
    assert(info.channel[2] == 149 && !(info.channelBitmap[2] & 0x100) && info.supports80MHz[2]);
    return 0;
}
"""
        self.compile_run(wcl_src)

        # 2. 160 MHz ESS beacons on 149..161 (center_freq_seg1_idx=163) must unlock all 4 subchannels.
        raw = (support.ROOT / "itl80211/openbsd/net80211/ieee80211.h").read_text()
        vht_decl = re.search(r"struct ieee80211_ie_vht_operation \{.*?\} __packed;", raw, re.S)[0]
        hint_src = (support.ROOT / "tests/fixtures/awdl_beacon_hints.cpp").read_text().rsplit("int main()", 1)[0]
        hint_src = hint_src.replace("// VHT DECLARATION", vht_decl)
        hint_routines = (
            definition("itl80211/openbsd/net80211/ieee80211_input.c", "ieee80211_note_beacon_hint") + "\n"
            + definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::awdlChannelAllowed")
        )
        hint_src = hint_src.replace("// PRODUCTION", hint_routines) + r"""
int main() {
    for (unsigned width : {1U, 2U}) {
        ItlIwx d;
        setup(d);
        auto &ic = d.com.sc_ic;
        ic.ic_channels[149].ic_flags |= IEEE80211_CHAN_VHT80;
        ieee80211_ie_vht_operation vht = {};
        vht.chan_width = width;
        vht.center_freq_seg1_idx = 163;
        ieee80211_note_beacon_hint(&ic, 149, 149, 0, IEEE80211_CAPINFO_ESS, &vht);
        assert(d.awdlChannelAllowed(149, 80));
    }
    return 0;
}
"""
        self.compile_run(hint_src)

    def test_airlink_secondary_master_traffic_reg_ext_params_and_coex_ioctls(self):
        source = (support.ROOT / "tests/fixtures/awdl_control.cpp").read_text().rsplit("int main()", 1)[0]
        handle = (
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::handle") + "\n"
            + definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::updatePeerCache") + "\n"
            + "\n".join(
                definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
                for n in ("configureBroadcast", "broadcastDue")
            )
        )
        headers = {
            name: (support.ROOT / "include/Airport" / name).read_text()
            for name in ("apple80211_var.h", "apple80211_ioctl.h")
        }
        wanted = ["apple80211_channel", "awdl_peer_advert_add"] + sorted(
            (
                set(re.findall(r"\bapple80211_\w+", handle))
                | {
                    "apple80211_awdl_af_tx_mode",
                    "apple80211_awdl_secondary_master_channel",
                    "apple80211_awdl_peer_traffic_registration",
                    "apple80211_awdl_extension_state_machine_parameter",
                }
            )
            - {"apple80211_channel", "awdl_peer_advert_add"}
        )
        declarations = []
        for name in wanted:
            pattern = r"struct " + name + r"\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;"
            matches = [re.search(pattern, h, re.S) for h in headers.values()]
            decl = next((m[0] for m in matches if m), None)
            if decl is None:
                raise ValueError("Missing native declaration: " + name)
            declarations.append(decl)
        interface = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()
        request = re.search(r"enum class Request\s*\{.*?\};", interface, re.S)[0]
        declarations.append(
            re.search(r"^#define APPLE80211_M_PEER_CACHE_CONTROL\s+\d+", headers["apple80211_var.h"], re.M)[0]
        )
        source = source.replace("// NATIVE DECLARATIONS", "\n".join(declarations))
        source = source.replace("// NATIVE REQUESTS", request)
        source = source.replace("// PRODUCTION", handle) + r"""
int main() {
    AirportItlwmAWDLInterface i;
    // 1. Secondary master channel (0xcd)
    apple80211_awdl_secondary_master_channel sec = {1, 149};
    assert(i.handle(Request::setAWDL_SECONDARY_MASTER_CHANNEL, &sec) == kIOReturnSuccess);
    apple80211_awdl_secondary_master_channel secOut = {};
    assert(i.handle(Request::getAWDL_SECONDARY_MASTER_CHANNEL, &secOut) == kIOReturnSuccess);
    assert(secOut.version == 1 && secOut.secondary_master_channel == 149);
    sec.secondary_master_channel = 36; // disallowed
    assert(i.handle(Request::setAWDL_SECONDARY_MASTER_CHANNEL, &sec) == kIOReturnUnsupported);
    sec.secondary_master_channel = 0; // clear allowed
    assert(i.handle(Request::setAWDL_SECONDARY_MASTER_CHANNEL, &sec) == kIOReturnSuccess);

    // 2. Peer traffic registration (0xa4) — wifid-assisted-discovery / sidecar
    apple80211_awdl_peer_traffic_registration reg = {};
    reg.version = 1;
    strcpy(reg.name, "wifid-assisted-discovery");
    reg.name_len = uint32_t(strlen(reg.name));
    reg.active = 1;
    i.nextActionUS = 999;
    i.nextMIFUS = 888;
    assert(i.handle(Request::setAWDL_PEER_TRAFFIC_REGISTRATION, &reg) == kIOReturnSuccess);
    assert(i.nextActionUS == 0 && i.nextMIFUS == 0);
    reg.name_len = 200;
    assert(i.handle(Request::setAWDL_PEER_TRAFFIC_REGISTRATION, &reg) == kIOReturnBadArgument);

    // 3. Extension state machine parameters (0x76)
    apple80211_awdl_extension_state_machine_parameter ext = {1, 10, 20, 30, 40};
    assert(i.handle(Request::setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS, &ext) == kIOReturnSuccess);
    apple80211_awdl_extension_state_machine_parameter extOut = {};
    assert(i.handle(Request::getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS, &extOut) == kIOReturnSuccess);
    assert(extOut.version == 1 && extOut.unk1 == 10 && extOut.unk2 == 20 && extOut.unk3 == 30 && extOut.unk4 == 40);

    // 4. P2P coex protection (0x19e)
    uint8_t coex[0x8c] = {};
    assert(i.handle(Request::setP2P_COEX_PROTECTION, coex) == kIOReturnSuccess);

    assert(i.unsupportedRequests == 0);
    return 0;
}
"""
        self.compile_run("#define AWDL_CONTROL_CASE 0\n" + source)


if __name__ == "__main__":
    unittest.main()
