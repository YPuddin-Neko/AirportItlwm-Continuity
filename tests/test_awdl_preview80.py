"""Failing-first regressions for Continuity-WiFi-Bluetooth-Preview80 Wi-Fi fixes."""
import re
import unittest

import awdl_native_sync
import test_regressions as support
from test_offline import definition


def _native_tick_decls():
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
    return "\n".join(decls)


def _native_tick_routines():
    return awdl_native_sync.routines() + "\n" + "\n".join(
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
    )


class Preview80WiFiRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_w4_sidecar_queue_bounded_without_fq_starvation(self):
        awdl_hpp = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()
        self.assertRegex(awdl_hpp, r"DataQueueCapacity\s*=\s*96\b")

        src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        src = src.replace("// NATIVE TX SCENARIOS", (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text())
        src = src.replace("// PRODUCTION", _native_tick_routines())
        src = src.replace("// NATIVE DECLARATIONS", _native_tick_decls())
        src = src.replace("} pending[64]={};", "} pending[96]={};")
        src = src.replace("inFlight[64]={};", "inFlight[96]={};")
        src += r"""
int main() {
    clockUS = 10000000;
    const uint8_t peerAddr[] = {2, 0, 0, 0, 0, 9};
    AirportItlwmAWDLInterface i;
    dataPeer(i, peerAddr);
    i.nextActionUS = i.nextMIFUS = clockUS + 5000000;

    for (unsigned n = 0; n < 110; ++n)
        i.network.output.push_back(ethernet(i.address.octet, peerAddr));

    assert(i.pullOutputPackets() == kIOReturnSuccess);
    assert(i.network.lastLimit == 32 && i.pendingCount == 32);
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    assert(i.network.lastLimit == 32 && i.pendingCount == 64);
    assert(i.pullOutputPackets() == kIOReturnNoResources);
    assert(i.network.lastLimit == 32 && i.pendingCount == 96);
    assert(i.network.output.size() == 14);
    while (!i.network.output.empty()) {
        mbuf_freem(i.network.output.front());
        i.network.output.pop_front();
    }
    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run("#define DEBUG 0\n" + src)

    def test_w5_unii1_36_to_48_80mhz_beacon_hint_allowed_and_phy_ctrl_pos(self):
        raw = (support.ROOT / "itl80211/openbsd/net80211/ieee80211.h").read_text()
        vht_decl = re.search(r"struct ieee80211_ie_vht_operation \{.*?\} __packed;", raw, re.S)[0]
        he_decl = re.search(r"struct ieee80211_he_operation \{.*?\} __packed;", raw, re.S)[0]
        he_flag = re.search(r"^#define\s+IEEE80211_HE_OPERATION_VHT_OPER_INFO\s+.*$", raw, re.M)[0]
        hint_src = (support.ROOT / "tests/fixtures/awdl_beacon_hints.cpp").read_text().rsplit("int main()", 1)[0]
        hint_src = hint_src.replace(
            "// VHT DECLARATION",
            "static uint32_t le32toh(uint32_t v) { return v; }\n" + he_flag + "\n" + vht_decl + "\n" + he_decl,
        )
        hint_routines = (
            definition("itl80211/openbsd/net80211/ieee80211_input.c", "ieee80211_note_beacon_hint") + "\n"
            + definition("itl80211/openbsd/net80211/ieee80211_input.c", "ieee80211_he_vht_operation") + "\n"
            + definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::awdlChannelAllowed")
        )
        hint_src = hint_src.replace("// PRODUCTION", hint_routines) + r"""
int main() {
    // 1. 80 MHz (seg1=42) and 160 MHz (seg1=50) VHT/HE ESS beacons on channel 48 unlock 36..48 without touching 149..161.
    for (uint8_t seg1 : {uint8_t(42), uint8_t(50)}) {
        ItlIwx d;
        setup(d);
        auto &ic = d.com.sc_ic;
        for (unsigned c : {36U, 40U, 44U, 48U}) {
            setbit(ic.ic_chan_active, c);
            ic.ic_channels[c] = {5000 + 5 * c, unsigned(IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_PASSIVE | IEEE80211_CHAN_VHT80)};
        }
        assert(!d.awdlChannelAllowed(48, 80));

        ieee80211_ie_vht_operation vht = {};
        vht.chan_width = 1;
        vht.center_freq_seg1_idx = seg1;
        ieee80211_note_beacon_hint(&ic, 48, 48, 0, IEEE80211_CAPINFO_ESS, &vht);

        for (unsigned c : {36U, 40U, 44U, 48U}) {
            assert(isset(ic.ic_beacon_hint, c));
            assert(d.awdlChannelAllowed(c, 20));
            assert(d.awdlChannelAllowed(c, 80));
        }
        assert(!isset(ic.ic_beacon_hint, 149) && !d.awdlChannelAllowed(149, 80));

        // Missing beacon hint or NOADHOC on any subchannel still blocks 80 MHz on 36..48.
        ic.ic_channels[36].ic_flags |= IEEE80211_CHAN_NOADHOC;
        assert(!d.awdlChannelAllowed(48, 80));
        ic.ic_channels[36].ic_flags &= ~IEEE80211_CHAN_NOADHOC;
        assert(d.awdlChannelAllowed(48, 80));

        // Mismatched seg1 (155) on channel 48 must only hint channel 48 (20 MHz), not 36..44.
        memset(ic.ic_beacon_hint, 0, sizeof(ic.ic_beacon_hint));
        vht.center_freq_seg1_idx = 155;
        ieee80211_note_beacon_hint(&ic, 48, 48, 0, IEEE80211_CAPINFO_ESS, &vht);
        assert(d.awdlChannelAllowed(48, 20) && !d.awdlChannelAllowed(44, 20) && !d.awdlChannelAllowed(48, 80));
    }
    return 0;
}
"""
        self.compile_run(hint_src)

        # Verify awdlPHY ctrl_pos supports both 36..48 and 149..161 blocks (not hardcoded - 149).
        iwx_awdl = (support.ROOT / "itlwm/hal_iwx/ItlIwxAWDL.cpp").read_text()
        self.assertNotIn("target - com.sc_ic.ic_channels - 149", iwx_awdl)

    def test_w6_cumulative_txba_stats_persist_across_peer_removal(self):
        radio_hpp = (support.ROOT / "AirportItlwm/AWDL/RadioState.hpp").read_text()
        awdl_cpp = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.cpp").read_text()
        iwx_cpp = (support.ROOT / "itlwm/hal_iwx/ItlIwx.cpp").read_text()
        iwx_awdl = (support.ROOT / "itlwm/hal_iwx/ItlIwxAWDL.cpp").read_text()

        for sym in ("agreements", "framesAcknowledged", "framesFailed", "handovers"):
            self.assertIn(sym, radio_hpp)
        for prop in ("TXBlockAckTotalAgreements", "TXBlockAckTotalAcknowledged", "TXBlockAckTotalFailed", "TXBlockAckHandovers"):
            self.assertIn(prop, awdl_cpp)
        self.assertIn("blockAckTXStats.framesAcknowledged", iwx_cpp)
        self.assertIn("blockAckTXStats.agreements", iwx_awdl)
        self.assertIn("blockAckTXStats.handovers", iwx_awdl)


if __name__ == "__main__":
    unittest.main()
