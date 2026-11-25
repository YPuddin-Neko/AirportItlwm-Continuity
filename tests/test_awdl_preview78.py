"""Failing-first regressions for Continuity-WiFi-Preview78 Sidecar & AirDrop fixes."""
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


class Preview78SidecarAirDropRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_sidecar_tx_queue_512_and_asking_software_amsdu_aggregation(self):
        awdl_hpp = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()
        self.assertRegex(awdl_hpp, r"DataQueueCapacity\s*=\s*96\b")

        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace("// NATIVE TX SCENARIOS", (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text())
        tick_src = tick_src.replace("// PRODUCTION", _native_tick_routines())
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", _native_tick_decls())
        # Expand mock interface arrays to production 96 capacity (32 dequeue batch limit).
        tick_src = tick_src.replace("} pending[64]={};", "} pending[96]={};")
        tick_src = tick_src.replace("inFlight[64]={};", "inFlight[96]={};")
        tick_src += r"""
int main() {
    clockUS = 10000000;
    const uint8_t peerAddr[] = {2, 0, 0, 0, 0, 9};
    AirportItlwmAWDLInterface i;
    aggregatePeer(i, peerAddr);
    i.device.awdlRadio.listener = true;
    i.nextActionUS = i.nextMIFUS = clockUS + 5000000;

    // 1. Enqueue 64 RTP packets within the bounded 96-slot queue:
    for (unsigned n = 0; n < 64; ++n)
        i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    while (!i.network.output.empty())
        assert(i.pullOutputPackets() == kIOReturnSuccess);
    assert(i.pendingCount == 64);

    // 2. Put TXBlockAck in Asking after Attempt #1 (nextAskUS = now + 200 ms, !managementToken):
    //    Software 2-MPDU A-MSDU on dataQueue MUST remain active during the inter-ask interval!
    auto &tx = i.device.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Asking;
    tx.cookie = 88;
    tx.epoch = i.device.dmaEpoch;
    copyAddress(tx.peer, peerAddr);
    tx.dialog = 1;
    tx.attempts = 1;
    tx.nextAskUS = clockUS + 200000;

    i.tick();
    assert(i.device.awdlRadio.dataQueueConfigured);
    assert(i.dataAggregatesSubmitted == 32);
    assert(i.dataSubmitted == 64 && i.pendingCount == 0 && i.txDropped == 0);

    // 3. Within the 15 ms pre-ADDBA drain guard (nextAskUS == now + 10 ms), software A-MSDU
    //    yields cleanly to single MPDUs so dataQueue stays empty for ADDBA Request!
    clockUS += 190000;
    aggregatePeer(i, peerAddr);
    i.nextActionUS = i.nextMIFUS = clockUS + 5000000;
    const uint64_t aggBefore = i.dataAggregatesSubmitted;
    i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    i.tick();
    assert(i.dataAggregatesSubmitted == aggBefore);
    assert(i.dataSubmitted == 66 && i.pendingCount == 0);

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run("#define DEBUG 0\n" + tick_src)

    def test_tx_blockack_handshake_dwell_dialog_and_flush_survival(self):
        # Part A: Native serviceTXBlockAck >=25ms dwell guard + dialog advance after ACKed unanswered ask.
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace("// NATIVE TX SCENARIOS", (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text())
        tick_src = tick_src.replace("// PRODUCTION", _native_tick_routines())
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", _native_tick_decls())
        # Wire mock completeAWDLTXManagement to real ItlIwx::completeAWDLTXManagement logic for lastAskAcked.
        tick_src = tick_src.replace(
            "bool completeAWDLTXManagement(uint64_t token,bool ack) {",
            "static uint64_t awdlRadioNow() { return clockUS; }\n"
            + definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::completeAWDLTXManagement")
            .replace("ItlIwx::completeAWDLTXManagement", "completeAWDLTXManagement")
            + "\n    bool unusedCompleteAWDLTXManagement(uint64_t token,bool ack) {",
        )
        tick_src += r"""
int main() {
    // 1. With only 18 ms remaining before EAW end (< 25 ms minimum ADDBA RTT budget),
    //    serviceTXBlockAck must NOT waste an ADDBA Request or open a truncated receiveProbe!
    {
        AirportItlwmAWDLInterface i;
        setupNativeTX(i);
        auto &tx = i.device.awdlRadio.blockAckTX;
        clockUS = i.sync.epochUS + 65536 - 18000;
        tx.nextAskUS = 0;
        i.serviceTXBlockAck();
        assert(tx.attempts == 0 && !tx.managementToken && i.device.filterWindowEnd == 0);
        i.drain();
    }

    // 2. With 60.5 ms remaining in the common window, Attempt #1 sends dialog == 3;
    //    when 802.11-ACKed without an ADDBA Response, Attempt #2 advances dialog to 4,
    //    unacknowledged retry keeps dialog == 4, and 255 wraps cleanly to nonzero 1!
    {
        AirportItlwmAWDLInterface i;
        setupNativeTX(i);
        auto &tx = i.device.awdlRadio.blockAckTX;
        tx.nextAskUS = 0;
        i.serviceTXBlockAck();
        assert(tx.attempts == 1 && tx.managementToken);
        BlockAckRequest req1 = {};
        assert(blockAckRequest(i.device.lastUnicast.data(), i.device.lastUnicast.size(), tx.peer, req1));
        assert(req1.dialog == 3);
        i.completeUnicast(tx.managementToken, true);
        assert(!tx.managementToken);

        // Unanswered after ACK -> next solicitation uses fresh dialog token 4!
        tx.nextAskUS = 0;
        i.serviceTXBlockAck();
        assert(tx.attempts == 2 && tx.managementToken);
        BlockAckRequest req2 = {};
        assert(blockAckRequest(i.device.lastUnicast.data(), i.device.lastUnicast.size(), tx.peer, req2));
        assert(req2.dialog == 4);

        // Unacknowledged ask keeps dialog == 4 on retry!
        i.completeUnicast(tx.managementToken, false);
        tx.nextAskUS = 0;
        i.serviceTXBlockAck();
        assert(tx.attempts == 3 && tx.managementToken);
        BlockAckRequest req3 = {};
        assert(blockAckRequest(i.device.lastUnicast.data(), i.device.lastUnicast.size(), tx.peer, req3));
        assert(req3.dialog == 4);

        // Dialog 255 wraps to nonzero 1 after ACKed unanswered ask!
        tx.dialog = 255;
        i.completeUnicast(tx.managementToken, true);
        tx.nextAskUS = 0;
        i.serviceTXBlockAck();
        BlockAckRequest req4 = {};
        assert(blockAckRequest(i.device.lastUnicast.data(), i.device.lastUnicast.size(), tx.peer, req4));
        assert(req4.dialog == 1);
        i.completeUnicast(tx.managementToken, false);
        i.drain();
    }
    assert(!allocations);
    return 0;
}
"""
        self.compile_run("#define DEBUG 0\n" + tick_src)

        # Part B: FIFO/drain flush in Agreed must NOT retire TXBlockAck!
        comp_src = (support.ROOT / "tests/fixtures/awdl_completion.cpp").read_text().rsplit("int main()", 1)[0]
        wire = [
            "using u8=uint8_t;using __le16=uint16_t;using __le32=uint32_t;",
            "#define IWX_TX_FLUSH_QUEUE_RSP 16",
            "enum {IWX_BA_RESP_TX_AGG=0,IWX_BA_RESP_TX_BAR=1,IWX_BA_RESP_TX_AGG_FAIL=2};",
        ]
        for path, names in (
            (
                "itlwm/hal_iwx/if_iwxreg.h",
                (
                    "iwx_agg_tx_status", "iwx_tx_resp", "iwx_tx_path_flush_cmd",
                    "iwx_flush_queue_info", "iwx_tx_path_flush_cmd_rsp", "iwx_compressed_ba_ratid",
                    "iwx_compressed_ba_tfd", "iwx_compressed_ba_notif",
                ),
            ),
            ("itlwm/hal_iwx/if_iwxvar.h", ("iwx_tx_data",)),
        ):
            header = (support.ROOT / path).read_text()
            wire.extend(re.search(r"struct " + name + r"\s*\{.*?\}[^;]*;", header, re.S)[0] for name in names)
        comp_src = comp_src.replace("// NATIVE DECLARATIONS", "\n".join(wire))
        routines = definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::completeAWDLTx") + "\n"
        routines += "\n".join(
            definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name)
            for name in (
                "iwx_rx_tx_cmd_single", "iwx_rx_tx_cmd", "iwx_rx_tx_ba_notif", "iwx_txd_done",
                "iwx_ampdu_txq_advance", "iwx_reset_tx_ring", "iwx_flush_sta_tids",
            )
        )
        comp_src = comp_src.replace("// PRODUCTION", routines) + r"""
int main() {
    for (unsigned status : {0x8eU, 0x85U}) {
        ItlIwx d;
        Packet pkt;
        auto &o = d.awdlRadio.blockAckTX;
        o.configured = true;
        o.phase = AirportAWDL::TXBlockAckQueue::Agreed;
        o.queue = 1;
        o.station = 4;
        o.tid = 0;
        o.epoch = d.dmaEpoch;
        o.cookie = 99;
        auto &r = d.com.txq[1];
        r.tail = 0;
        r.cur = 1;
        r.queued = 1;
        r.data[0].m = &pkt;
        r.data[0].flags = IWX_TXDATA_FLAG_AWDL | IWX_TXDATA_FLAG_AWDL_BA;
        r.data[0].awdlToken = 50;
        r.data[0].awdlBACookie = o.cookie;
        r.data[0].type = IEEE80211_FC0_TYPE_DATA;

        iwx_rx_packet rxp;
        auto &t = *reinterpret_cast<iwx_tx_resp *>(rxp.data);
        t.frame_count = 1;
        t.tx_queue = 1;
        t.ra_tid = 0x40;
        t.status.status = status;
        rxp.length = sizeof(t) + 4;
        uint32_t next = 1;
        memcpy(&t.status + 1, &next, 4);
        iwx_rx_data rx;
        d.iwx_rx_tx_cmd(&d.com, &rxp, &rx);
        assert(o.phase == AirportAWDL::TXBlockAckQueue::Agreed);
        assert(o.flushed == 1 && o.failed == 0 && o.consecutiveFailures == 0 && o.recoveries == 0);
        assert((d.dataResults == std::vector<std::pair<uint64_t, int>>{{50, ECANCELED}}));
    }
    return 0;
}
"""
        self.compile_run(comp_src)

    def test_airdrop_wakeup_stale_parent_slot0_alignment_and_announcement_guard(self):
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace("// NATIVE TX SCENARIOS", (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text())
        tick_src = tick_src.replace("// PRODUCTION", _native_tick_routines())
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", _native_tick_decls())
        tick_src += r"""
static std::vector<uint8_t> makeElectionTemplate(const uint8_t *master, const uint8_t *parent, uint32_t distance) {
    std::vector<uint8_t> buf(3 + 40 + 3 + 2, 0);
    buf[0] = 24; putLE16(&buf[1], 40);
    copyAddress(&buf[3], master);
    copyAddress(&buf[9], parent);
    putLE32(&buf[15], 100);
    putLE32(&buf[19], distance);
    putLE32(&buf[23], 528);
    putLE32(&buf[27], 65);
    putLE32(&buf[31], 1);
    putLE32(&buf[39], 5);
    buf[43] = 21; putLE16(&buf[44], 2); buf[46] = 0x31; buf[47] = 2;
    return buf;
}

int main() {
    for (uint32_t dist : {1U, 2U}) {
        clockUS = 10000000;
        const uint8_t rootAddr[] = {2, 0, 0, 0, 0, 18};
        const uint8_t relayAddr[] = {2, 0, 0, 0, 0, 19};
        const uint8_t *parentAddr = dist == 1 ? rootAddr : relayAddr;
        AirportItlwmAWDLInterface i;
        i.device.awdlRadio.listener = true;
        i.tick();

        // Program native locked fixed-parent sync (distance 1 or 2) and post an active AW_START.
        auto tpl = makeElectionTemplate(rootAddr, parentAddr, dist);
        assert(i.sync.configureTemplate(tpl.data(), tpl.size()));
        i.sync.configureSyncState(1, 0, parentAddr);
        Advertisement adv = {};
        adv.fields = Sync | Channels | ElectionV2;
        adv.windowPeriodTU = 16;
        adv.presenceMode = 4;
        adv.windowCounter = 1000;
        adv.nextWindowTU = 64;
        adv.masterChannel = 149;
        copyAddress(adv.syncMaster, rootAddr);
        adv.sequence.count = 16;
        adv.sequence.encoding = 3;
        adv.sequence.step = 3;
        adv.sequence.fill = 0xffff;
        for (unsigned s = 0; s < 16; ++s) {
            adv.sequence.channel[s] = (s == 8) ? 6 : ((s < 3 || s == 9 || s == 10) ? 149 : 0);
            adv.sequence.flags[s] = operatingClass(adv.sequence.channel[s]);
            i.sync.channels[s] = adv.sequence.channel[s];
        }
        i.sequenceConfigured = true;
        adv.electionID = 1;
        copyAddress(adv.electionMaster, rootAddr);
        copyAddress(adv.electionSync, dist == 1 ? rootAddr : rootAddr);
        adv.distance = dist - 1;
        adv.masterMetric = 528;
        adv.selfMetric = 520;
        adv.masterCounter = 100;
        assert(i.device.awdlPeers.update(parentAddr, i.address.octet, adv, clockUS, 149, -45));
        i.tick();
        assert(i.sync.lockedFixedParent && i.nativeWindowOpen);
        const uint16_t beforePauseCounter = i.nativeWindowCounter;

        // Pause AWDL (Sidecar disconnect), then re-enable 5.05 s later at a non-zero slot (slot 6)
        // after parent & root have disappeared (> 3 s ScheduleFreshnessUS):
        assert(i.setEnabled(false) == kIOReturnSuccess);
        clockUS += 5050000; // Lands mid-cycle away from slot 0!
        i.device.awdlPeers = {};
        const uint64_t startsBeforeEnable = i.nativeWindowStarts;
        const unsigned psfBeforeEnable = i.device.psf, mifBeforeEnable = i.device.mif;
        assert(i.setEnabled(true) == kIOReturnSuccess);
        i.tick();

        // Must immediately open a receive-only slot-0 AW_START on the VERY FIRST tick (0 ms stall!),
        // monotonically ahead of beforePauseCounter, with zero frame transmissions!
        assert(i.nativeWindowOpen);
        assert(i.nativeWindowStarts == startsBeforeEnable + 1);
        assert(uint16_t(i.nativeWindowCounter - beforePauseCounter) > 0);
        assert(((i.nativeWindowCounter / i.sync.presence) % 16) == 0);
        assert(!i.sync.transmitReady(clockUS));
        assert(i.device.psf == psfBeforeEnable && i.device.mif == mifBeforeEnable);
    }

    // Announcement end-of-slot guard: an immediate AirDrop broadcast queued 1.2 ms before
    // dwell.windowEndUS must NOT be consumed & flushed at slot end; it transmits cleanly next active slot!
    {
        clockUS = 20000000;
        AirportItlwmAWDLInterface i;
        i.tick();
        const uint64_t endUS = i.device.awdlRadio.dwell.windowEndUS;
        clockUS = endUS - 1200;
        auto payload = makeElectionTemplate(i.address.octet, i.address.octet, 0);
        apple80211_awdl_oob_request oob = {};
        memset(oob.destination, 0xff, 6);
        copyAddress(oob.bssid, BSSID);
        oob.flags = 6;
        oob.data_len = uint16_t(payload.size());
        oob.data = payload.data();
        assert(i.configureBroadcast(oob, false) == kIOReturnSuccess);
        const unsigned mifBefore = i.device.mif;
        i.tick();
        assert(i.device.mif == mifBefore && i.immediateBroadcast.size == payload.size());

        // Advance into next active slot (slot 8 = channel 6) -> immediate broadcast transmits once cleanly!
        clockUS = 20000000 + 8ULL * 65536 + 10000;
        i.nextMIFUS = i.nextActionUS = clockUS + 2000000;
        i.tick();
        assert(i.broadcastSubmitted == 1 && i.device.mif == mifBefore + 1 && i.immediateBroadcast.size == 0);
        i.drain();
    }
    assert(!allocations);
    return 0;
}
"""
        self.compile_run("#define DEBUG 1\n" + tick_src)

    def test_he_operation_embedded_vht_info_unlocks_80mhz_awdl_beacon_hints(self):
        raw = (support.ROOT / "itl80211/openbsd/net80211/ieee80211.h").read_text()
        vht_decl = re.search(r"struct ieee80211_ie_vht_operation \{.*?\} __packed;", raw, re.S)[0]
        he_decl = re.search(r"struct ieee80211_he_operation \{.*?\} __packed;", raw, re.S)[0]
        he_flag = re.search(r"^#define\s+IEEE80211_HE_OPERATION_VHT_OPER_INFO\s+.*$", raw, re.M)[0]
        hint_src = (support.ROOT / "tests/fixtures/awdl_beacon_hints.cpp").read_text().rsplit("int main()", 1)[0]
        hint_src = hint_src.replace(
            "// VHT DECLARATION",
            "static uint32_t le32toh(uint32_t v) { return v; }\n" + he_flag + "\n" + vht_decl + "\n" + he_decl,
        )
        input_c = (support.ROOT / "itl80211/openbsd/net80211/ieee80211_input.c").read_text()
        self.assertIn("ieee80211_he_vht_operation", input_c)
        hint_routines = (
            definition("itl80211/openbsd/net80211/ieee80211_input.c", "ieee80211_note_beacon_hint") + "\n"
            + definition("itl80211/openbsd/net80211/ieee80211_input.c", "ieee80211_he_vht_operation") + "\n"
            + definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::awdlChannelAllowed")
        )
        hint_src = hint_src.replace("// PRODUCTION", hint_routines) + r"""
int main() {
    for (uint8_t seg1 : {uint8_t(155), uint8_t(163)}) {
        ItlIwx d;
        setup(d);
        auto &ic = d.com.sc_ic;
        ic.ic_channels[149].ic_flags |= IEEE80211_CHAN_VHT80;
        // Build IEEE 802.11ax HE Operation element (Element ID 255, Ext ID 36) with embedded 3-byte VHT Oper Info:
        uint8_t ie[2 + 1 + sizeof(ieee80211_he_operation) + 3] = {};
        ie[0] = 255;
        ie[1] = 1 + sizeof(ieee80211_he_operation) + 3;
        ie[2] = 36;
        uint32_t params = IEEE80211_HE_OPERATION_VHT_OPER_INFO;
        memcpy(&ie[3], &params, 4);
        ie[3 + sizeof(ieee80211_he_operation) + 0] = 1; // 80/160 MHz VHT chan_width
        ie[3 + sizeof(ieee80211_he_operation) + 1] = seg1;
        ie[3 + sizeof(ieee80211_he_operation) + 2] = 0;

        ieee80211_ie_vht_operation synth = {};
        const auto *vht = ieee80211_he_vht_operation(ie, &synth);
        assert(vht != nullptr);
        ieee80211_note_beacon_hint(&ic, 149, 149, 0, IEEE80211_CAPINFO_ESS, vht);
        assert(d.awdlChannelAllowed(149, 80));
    }
    return 0;
}
"""
        self.compile_run(hint_src)


if __name__ == "__main__":
    unittest.main()
