"""Failing-first regressions for Preview 91:
1. Bounded off-wide PHY parking (<= 75 ms cumulative span, strictly < 100 ms XNU FQ_CODEL stall
   threshold): multi-slot [48, 48] (131 ms) Sidecar windows tune to 48@20 and keep TX/RX active
   instead of blacking out awdl0 for 131-197 ms, while single-slot (<= 65.5 ms) excursions still
   park cleanly and back-to-back single slots never exceed 75 ms total pause.
2. High-capacity native Skywalk llw0 pool (2048 packets) and completion rings (rxCompQueue = 512,
   txSubQueue = 256, txCompQueue = 256) absorb 84-150+ packet 1080p HEVC IDR keyframe bursts in
   one interrupt batch without tail drops or continuous FIR keyframe storms, plus 16-slot LLW
   headroom reservation in awdl0 software queue when llwActive is true.
3. 802.11 BlockAckReq (BAR, 0x84, 20 bytes / 22 bytes padded) acceptance in STA mode and immediate
   AWDL RX reorder buffer release on both TID 0 (Sidecar) and TID 7 (Continuity Camera LLW),
   including contiguous post-SSN flush and >buf_size SSN jump safety without leaked mbufs.
4. Proactive IWX_STA_LINK station install & retention for nativeCached peers in serviceAWDLBlockAck
   (with idle-slot eviction when 4 station slots are full) so MacID 1 on infraChannel (IN_PROMISC=0)
   receives and ACKs unicast PTP/LLW/data frames before RXBA setup, plus non-deadlocking ADDBA
   solicit-window check when dataQueue is draining or announcements are due.
"""
import re
import unittest

import test_awdl_preview82 as p82
import test_regressions as support
from iwx_firmware import declarations as firmware_declarations
from test_offline import definition


def _build_radio_harness(extra_cpp: str) -> str:
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
    return radio_src + extra_cpp


def _build_ba_harness(extra_cpp: str) -> str:
    source = (support.ROOT / "tests/fixtures/awdl_ba.cpp").read_text().rsplit("int main()", 1)[0]
    source += (support.ROOT / "tests/fixtures/awdl_tx_live.cpp").read_text().rsplit("int main()", 1)[0]
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
    source = source.replace(
        "// PRODUCTION",
        "\n".join(definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::" + name) for name in hal_ba)
        + "\n"
        + "\n".join(definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name) for name in rx),
    )
    source = source.replace(
        "// FIRMWARE",
        firmware_declarations(
            (
                "iwx_rx_mpdu_desc_v1", "iwx_rx_mpdu_desc_v3", "iwx_rx_mpdu_desc",
                "iwx_add_sta_cmd", "iwx_rm_sta_cmd", "iwx_reorder_buffer",
                "iwx_reorder_buf_entry", "iwx_rxba_data", "iwx_tlc_config_cmd_v4",
                "iwl_tlc_config_cmd_v3",
            ),
            source,
            ("iwx_tlc_mng_cfg_mode", "iwx_tlc_mng_cfg_cw"),
        ),
    )
    source = source.replace(
        "// FROM REORDER BUFFER",
        definition("itlwm/hal_iwx/if_iwxvar.h", "iwx_rxba_data_from_reorder_buf"),
    )
    return source + extra_cpp


class Preview91Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p91_multi_slot_off_wide_excursion_tunes_instead_of_stalling_fq_codel(self):
        extra = r"""
static void preview91BoundedParkSpan() {
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

    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 9};
    auto &peer = d.awdlPeers.peers[0];
    peer = {};
    peer.occupied = true;
    AirportAWDL::copyAddress(peer.address, peerAddr);
    peer.haveSync = peer.haveSequence = true;
    peer.metadata.fields = AirportAWDL::HTCapabilities;
    peer.metadata.rxHTMcs = 0xff;
    auto refreshPeer = [&](uint64_t t) {
        peer.lastSeenUS = peer.syncReceivedUS = peer.sequenceReceivedUS = peer.htCapabilitiesReceivedUS = t;
    };
    refreshPeer(clockUS);

    auto &tx = d.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.epoch = d.dmaEpoch;
    tx.phase = AirportAWDL::TXBlockAckQueue::Agreed;
    AirportAWDL::copyAddress(tx.peer, peerAddr);
    tx.acknowledged = 64;
    tx.enabled = true;
    tx.paused = false;
    tx.widthMHz = 80;
    tx.lastHeavyDataUS = clockUS;
    d.awdlRadio.lastWideSocialVisitUS = clockUS;

    RadioState::GrantPolicy p80 = {}; p80.widthMHz = 80; p80.slot = 0;
    RadioState::GrantPolicy p20 = {}; p20.widthMHz = 20; p20.slot = 6; p20.bridgeInfraToWide = true;
    assert(!d.serviceAWDLChannel(149, clockUS + 393216, p80));
    assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

    // 1. Multi-slot >140 ms [48, 48, 48] excursion (3 * 65536 = 196608 us > 140000 us MaxParkWindowUS)
    //    MUST NOT park channel=0; it MUST tune to 48@20 cleanly (TLC -> STA -> PHY) and open dwell!
    const uint64_t infraParksBefore = d.awdlRadio.blockAckTXStats.infraParks;
    assert(!d.serviceAWDLChannel(48, clockUS + 196608, p20));
    assert(d.awdlRadio.dwell.running);
    assert(d.awdlRadio.channel == 48 && d.awdlRadio.widthMHz == 20 && tx.widthMHz == 20);
    assert(d.awdlRadio.blockAckTXStats.infraParks == infraParksBefore);

    // 2. Bounded <= 2-slot [48, 48] excursion (131072 us <= 140000 us MaxParkWindowUS)
    //    with bridgeInfraToWide == true DOES park PHY 1 at 149@80 without retuning.
    clockUS += 196608;
    refreshPeer(clockUS);
    tx.lastHeavyDataUS = clockUS;
    tx.widthMHz = 80;
    p80.slot = 9;
    assert(!d.serviceAWDLChannel(149, clockUS + 327680, p80));
    assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

    const size_t phyBeforeSingle = d.phyCommands.size();
    p20.slot = 14;
    assert(!d.serviceAWDLChannel(48, clockUS + 131072, p20));
    assert(!d.awdlRadio.dwell.running && d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);
    assert(d.phyCommands.size() == phyBeforeSingle);
    assert(d.awdlRadio.blockAckTXStats.infraParks == infraParksBefore + 1);

    // 3. Single-slot [6] immediately after [48, 48] MUST ALWAYS tune to ch 6 (never parked over)!
    clockUS += 131072;
    refreshPeer(clockUS);
    tx.lastHeavyDataUS = clockUS;
    d.awdlRadio.lastWideSocialVisitUS = clockUS - 100000;
    p20.slot = 15;
    assert(!d.serviceAWDLChannel(6, clockUS + 65536, p20));
    assert(d.awdlRadio.dwell.running && d.awdlRadio.channel == 6 && d.awdlRadio.widthMHz == 20);

    assert(!d.stopAWDLRadio());
}

int main() {
    preview91BoundedParkSpan();
    return 0;
}
"""
        self.compile_run(_build_radio_harness(extra))

    def test_p91_llw_skywalk_high_capacity_pool_and_rx_ring_absorb_keyframe_burst(self):
        reg_fn = definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmLLWInterface::registerSkywalk",
        )
        rx_fn = definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmLLWInterface::inputSkywalkEthernet",
        )
        src = f"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

using IOReturn = int;
using UInt32 = uint32_t;
enum {{ kIOReturnSuccess = 0, kIOReturnNoMemory = -1, kIOReturnNoSpace = -2, kIOSkywalkPacketTypeNetwork = 1 }};
static const void *kOSBooleanTrue = reinterpret_cast<const void *>(1);
static inline bool unicastAddress(const uint8_t a[6]) {{ return (a[0] & 1) == 0 && (a[0]|a[1]|a[2]|a[3]|a[4]|a[5]); }}

struct ether_addr {{ uint8_t octet[6]; }};
struct IOSkywalkPacket {{
    std::vector<uint8_t> buf;
    uint32_t dataOff = 0, dataLen = 0, linkHdr = 0;
    IOSkywalkPacket(uint32_t sz) : buf(sz, 0) {{}}
    IOReturn prepare(void *, uint32_t, uint32_t) {{ return kIOReturnSuccess; }}
    void *getDataVirtualAddress() {{ return buf.data(); }}
    uint32_t getBufferSize() const {{ return uint32_t(buf.size()); }}
    IOReturn setDataOffsetAndLength(uint32_t off, uint32_t len) {{
        if (off + len > buf.size()) return kIOReturnNoSpace;
        dataOff = off; dataLen = len; return kIOReturnSuccess;
    }}
}};
struct IOSkywalkNetworkPacket : IOSkywalkPacket {{
    void setLinkHeaderLength(uint8_t n) {{ linkHdr = n; }}
}};
struct IOSkywalkPacketQueue {{
    uint32_t capacity = 0;
    void enable() {{}}
    void disable() {{}}
    void release() {{}}
}};
struct IOSkywalkPacketBufferPool {{
    struct PoolOptions {{
        uint32_t packetCount, bufferCount, bufferSize, maxBuffersPerPacket, memorySegmentSize, poolFlags;
    }};
    PoolOptions captured = {{}};
    uint32_t allocated = 0;
    static IOSkywalkPacketBufferPool *withName(const char *, void *, int, const PoolOptions *opts) {{
        static IOSkywalkPacketBufferPool pool;
        pool.captured = *opts;
        pool.allocated = 0;
        return &pool;
    }}
    IOReturn allocatePacket(IOSkywalkPacket **out, uint32_t) {{
        if (allocated >= captured.packetCount) return kIOReturnNoMemory;
        ++allocated;
        *out = new IOSkywalkPacket(captured.bufferSize);
        return kIOReturnSuccess;
    }}
    void deallocatePacket(IOSkywalkPacket *p) {{
        if (p) {{ delete p; if (allocated) --allocated; }}
    }}
    void release() {{}}
}};
struct IOSkywalkTxSubmissionQueue : IOSkywalkPacketQueue {{
    using Action = unsigned int(*)(void *, IOSkywalkTxSubmissionQueue *, IOSkywalkPacket * const *, unsigned int, void *);
    static IOSkywalkTxSubmissionQueue *withPool(IOSkywalkPacketBufferPool *, uint32_t cap, uint32_t, void *, Action, void *, uint32_t) {{
        static IOSkywalkTxSubmissionQueue q;
        q.capacity = cap;
        return &q;
    }}
}};
struct IOSkywalkTxCompletionQueue : IOSkywalkPacketQueue {{
    static IOSkywalkTxCompletionQueue *withPool(IOSkywalkPacketBufferPool *, uint32_t cap, uint32_t, void *, void *, void *, uint32_t) {{
        static IOSkywalkTxCompletionQueue q;
        q.capacity = cap;
        return &q;
    }}
}};
struct IOSkywalkRxCompletionQueue : IOSkywalkPacketQueue {{
    std::vector<IOSkywalkPacket *> ring;
    static IOSkywalkRxCompletionQueue *withPool(IOSkywalkPacketBufferPool *, uint32_t cap, uint32_t, void *, void *, void *, uint32_t) {{
        static IOSkywalkRxCompletionQueue q;
        q.capacity = cap;
        q.ring.clear();
        return &q;
    }}
    bool isQueueReady() const {{ return true; }}
    IOReturn enqueuePackets(IOSkywalkPacket * const *pkts, uint32_t count, uint32_t) {{
        if (ring.size() + count > capacity) return kIOReturnNoSpace;
        for (uint32_t i = 0; i < count; ++i) ring.push_back(pkts[i]);
        return kIOReturnSuccess;
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
    bool enabledState = true;
    bool skywalkRegistered = false;
    IOSkywalkPacketBufferPool *skywalkPool = nullptr;
    IOSkywalkTxSubmissionQueue *txSubQueue = nullptr;
    IOSkywalkTxCompletionQueue *txCompQueue = nullptr;
    IOSkywalkRxCompletionQueue *rxCompQueue = nullptr;
    IOWorkLoop *queueWorkLoop = nullptr;
    uint64_t skywalkRxPackets = 0, skywalkRxDropped = 0;

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
    IOReturn registerEthernetInterface(const IOSkywalkEthernetInterface::RegistrationInfo *,
                                       IOSkywalkPacketQueue **, uint32_t,
                                       IOSkywalkPacketBufferPool *, IOSkywalkPacketBufferPool *, uint32_t) {{
        return kIOReturnSuccess;
    }}
    void deregisterSkywalk() {{}}
    void setProperty(const char *, const void *) {{}}
    bool registerSkywalk(IOWorkLoop *wl);
    bool inputSkywalkEthernet(const uint8_t *ethernet, size_t length);
}};

{reg_fn}
{rx_fn}

int main() {{
    AirportItlwmLLWInterface llw;
    IOWorkLoop wl;
    assert(llw.registerSkywalk(&wl));
    assert(llw.skywalkPool->captured.packetCount >= 2048);
    assert(llw.skywalkPool->captured.bufferCount >= 2048);
    assert(llw.rxCompQueue->capacity >= 512);
    assert(llw.txSubQueue->capacity >= 256);
    assert(llw.txCompQueue->capacity >= 256);

    // Simulate a 120-packet 1080p HEVC IDR keyframe + audio burst in one RX interrupt batch
    // before userspace ContinuityCaptureAgent wakes to drain the Skywalk channel ring.
    uint8_t frame[1037] = {{}};
    frame[0] = 0x02; frame[5] = 0x01; frame[6] = 0x02; frame[11] = 0x02;
    frame[12] = 0x86; frame[13] = 0xdd;
    for (unsigned i = 0; i < 120; ++i) {{
        assert(llw.inputSkywalkEthernet(frame, sizeof(frame)));
    }}
    assert(llw.skywalkRxPackets == 120 && llw.skywalkRxDropped == 0);
    for (auto *p : llw.rxCompQueue->ring) llw.skywalkPool->deallocatePacket(p);
    llw.rxCompQueue->ring.clear();
    return 0;
}}
"""
        self.compile_run(src)

    def test_p91_awdl_bar_0x84_releases_reorder_buffer_immediately_on_tid0_and_tid7(self):
        # Verify STA-mode iwx_rx_mpdu_mq allows 20-byte / 22-byte padded BAR (0x84) frames.
        mpdu_fn = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_rx_mpdu_mq")
        self.assertIn("0x84U", mpdu_fn)

        extra = r"""
static std::unique_ptr<Packet> makeBAR(const uint8_t ra[6], const uint8_t ta[6], uint8_t tid, uint16_t ssn) {
    auto m = std::unique_ptr<Packet>(new Packet);
    m->bytes.assign(20, 0);
    uint8_t *p = m->bytes.data();
    p[0] = 0x84; // IEEE80211_FC0_TYPE_CTL | IEEE80211_FC0_SUBTYPE_BAR
    p[1] = 0x00;
    copyAddress(p + 4, ra);
    copyAddress(p + 10, ta);
    // Compressed BAR Control: AckPolicy=0, MultiTID=0, Compressed=1 (0x0004) | (tid << 12)
    putLE16(p + 16, uint16_t(0x0004U | (uint16_t(tid & 0x0f) << 12)));
    putLE16(p + 18, uint16_t((ssn & 0x0fff) << 4));
    return m;
}

int main() {
    // 1. Continuity Camera LLW QoS TID 7 BAR with INVALID_BAID in desc->reorder_data
    //    (raw IN_CONTROL_AND_MGMT delivery on MacID 1):SN=100 lost, SN=101..103 buffered,
    //    BAR(SSN=101) MUST immediately release 101..103 in 0 ms without waiting 40 ms!
    {
        ItlIwx d;
        auto r = request(d, 0, 7, 100);
        unsigned b = start(d, r);
        auto p1 = packet(d, r, 101, 11);
        auto p2 = packet(d, r, 102, 12);
        auto p3 = packet(d, r, 103, 13);
        receive(d, p1.get(), b, 101, 100);
        receive(d, p2.get(), b, 102, 100);
        receive(d, p3.get(), b, 103, 100);
        assert(d.delivered.empty());

        auto bar = makeBAR(d.awdlRadio.address, r.peer, 7, 101);
        iwx_rx_mpdu_desc desc = {};
        desc.status = IWX_RX_MPDU_RES_STATUS_SRC_STA_FOUND | (d.com.sc_rxba_data[b].sta_id << 24);
        desc.reorder_data = uint32_t(IWX_RX_REORDER_DATA_INVALID_BAID) << IWX_RX_MPDU_REORDER_BAID_SHIFT;
        mbuf_list ml;
        assert(d.receiveAWDLReordered(bar.get(), &desc, &ml));
        assert(bar->frees);
        assert((d.delivered == std::vector<uint16_t>{11, 12, 13}));
        assert(d.com.sc_rxba_data[b].reorder_buf.head_sn == 104);
        assert(d.com.sc_rxba_data[b].reorder_buf.num_stored == 0);
        assert(!d.clearAWDLBlockAck());
    }

    // 2. Sidecar TID 0 BAR with populated BAID + NSSN AND large SSN jump (> buf_size = 64)
    //    drains all buffered frames cleanly with zero orphaned num_stored or leaked mbufs!
    {
        ItlIwx d;
        auto r = request(d, 0, 0, 100);
        unsigned b = start(d, r);
        auto p1 = packet(d, r, 105, 21);
        auto p2 = packet(d, r, 120, 22);
        receive(d, p1.get(), b, 105, 100);
        receive(d, p2.get(), b, 120, 100);
        assert(d.delivered.empty());

        // Jump SSN to 200 (100 > buf_size=64 ahead of head_sn=100).
        auto bar = makeBAR(d.awdlRadio.address, r.peer, 0, 200);
        iwx_rx_mpdu_desc desc = {};
        desc.status = IWX_RX_MPDU_RES_STATUS_SRC_STA_FOUND | (d.com.sc_rxba_data[b].sta_id << 24);
        desc.reorder_data = (b << 24) | 200;
        mbuf_list ml;
        assert(d.receiveAWDLReordered(bar.get(), &desc, &ml));
        assert(bar->frees && p1->frees && p2->frees);
        assert((d.delivered == std::vector<uint16_t>{21, 22}));
        assert(d.com.sc_rxba_data[b].reorder_buf.head_sn == 200);
        assert(d.com.sc_rxba_data[b].reorder_buf.num_stored == 0);

        // BAR addressed to AP (not awdlRadio.address) must return false untouched.
        auto apBar = makeBAR(r.peer, d.awdlRadio.address, 0, 205);
        assert(!d.receiveAWDLReordered(apBar.get(), &desc, &ml) && !apBar->frees);
        assert(!d.clearAWDLBlockAck());
    }
    return 0;
}
"""
        self.compile_run(_build_ba_harness(extra))

    def test_p91_cached_peer_retains_sta_link_on_infra_channel_and_addba_no_deadlock(self):
        # Part A: nativeCached peer proactively installs & retains IWX_STA_LINK in serviceAWDLBlockAck,
        # and yields an idle cached-only slot when all 4 station slots are full and a 5th peer starts RXBA.
        extra_ba = r"""
int main() {
    ItlIwx d;
    d.awdlRadio.listener = true;
    d.awdlRadio.binding = true;
    d.awdlRadio.channel = 48;
    d.awdlRadio.widthMHz = 20;

    // Register nativeCached peer 0 (e.g. Continuity Camera iPhone on infra channel 48 before RXBA).
    auto r0 = request(d, 0, 7, 10);
    d.awdlPeers.peers[0].nativeCached = true;
    d.serviceAWDLBlockAck();
    assert(d.awdlBA.peers[0].station);
    assert(sameAddress(d.awdlBA.peers[0].address, r0.peer));
    assert(!d.awdlHasRXBA()); // Must NOT claim active RXBA when only station link is installed!

    // Start and tear down RXBA on peer 0 while nativeCached remains true -> station link stays retained!
    start(d, r0);
    assert(d.awdlHasRXBA());
    auto end0 = r0; end0.teardown = true;
    d.receiveAWDLBlockAckEnd(end0);
    d.serviceAWDLBlockAck();
    assert(!d.awdlHasRXBA() && d.com.sc_rx_ba_sessions == 0);
    assert(d.awdlBA.peers[0].station); // Retained for unicast RX/ACK on ch 48!

    // Fill slots 1..3 with three more nativeCached peers, then have a 5th uncached peer request RXBA:
    // prepareAWDLBlockAck MUST evict an idle cached-only slot so active RXBA never fails with ENOSPC!
    for (unsigned p = 1; p <= 3; ++p) {
        request(d, p, 0, 10);
        d.awdlPeers.peers[p].nativeCached = true;
    }
    d.serviceAWDLBlockAck();
    for (unsigned s = 0; s < 4; ++s) assert(d.awdlBA.peers[s].station);

    auto r4 = request(d, 4, 0, 50);
    uint64_t cookie4 = 0;
    assert(!d.prepareAWDLBlockAck(r4, cookie4) && cookie4 != 0);
    assert(d.com.sc_rx_ba_sessions == 1);

    // Clearing nativeCached removes idle cached-only stations cleanly on next serviceAWDLBlockAck.
    for (unsigned p = 0; p < 4; ++p) d.awdlPeers.peers[p].nativeCached = false;
    d.serviceAWDLBlockAck();
    unsigned activeStations = 0;
    for (unsigned s = 0; s < 4; ++s) activeStations += d.awdlBA.peers[s].station;
    assert(activeStations == 1); // Only peer 4 with active RXBA remains!

    assert(!d.clearAWDLBlockAck());
    return 0;
}
"""
        self.compile_run(_build_ba_harness(extra_ba))

        # Part B: txReady() / drainPendingData() must NOT deadlock peerTX when dataQueue is still draining
        # (serviceTXBlockAck returns early without advancing nextAskUS), and pullOutputPackets reserves
        # 16 slots of pending[] headroom for llw0 when llwActive is true.
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        extra_routines = "\n".join(
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
            for n in ("submitOutputPackets", "drainPendingData", "txReady", "enqueueLLWFrame")
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines() + "\n" + extra_routines)
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src = tick_src.replace("static void txReady(void *);", "static void txReady(void *);\n    IOReturn enqueueLLWFrame(mbuf_t);")
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

    i.tick();
    assert(i.device.awdlTxWindowOpen());

    // 1. (a) When psfDue(now) is true while mgmt queue is busy during active media
    //    (serviceTXBlockAck defers ADDBA), txReady() MUST NOT deadlock peerTX;
    //    (b) When psfDue(now) is false and dataQueue has queued == 2, solicitDeadline
    //    MUST hold new Station 3 submissions so dataQueue drains to 0 for ADDBA Request!
    auto &tx = i.device.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Asking;
    tx.cookie = 91;
    tx.epoch = i.device.dmaEpoch;
    copyAddress(tx.peer, peerAddr);
    tx.dialog = 1;
    tx.tid = 0;
    tx.nextAskUS = clockUS;
    tx.lastHeavyDataUS = clockUS;
    i.device.awdlRadio.dataQueueConfigured = true;
    i.device.awdlRadio.dataQueue = 3;
    i.device.com.txq[i.device.awdlRadio.queue].queued = 1;
    i.device.com.txq[3].queued = 0;
    i.nextActionUS = clockUS; // psfDue(now) == true

    const unsigned dataBefore = i.device.data;
    i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    AirportItlwmAWDLInterface::txReady(&i);
    assert(i.device.data == dataBefore + 1); // Unblocked when psfDue defers ADDBA!

    i.device.com.txq[i.device.awdlRadio.queue].queued = 0;
    i.nextActionUS = clockUS + 5000000; // psfDue(now) == false
    i.device.com.txq[3].queued = 2;
    const unsigned afterFirst = i.device.data;
    i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    AirportItlwmAWDLInterface::txReady(&i);
    assert(i.device.data == afterFirst); // Held by solicitDeadline so dataQueue drains to 0!
    i.device.com.txq[3].queued = 0;
    i.drain();

    // 2. When llwActive && llwParamsEnabled, bulk awdl0 pullOutputPackets() leaves >= 16 slots of headroom
    //    so concurrent llw0 enqueueLLWFrame() never drops with kIOReturnNoResources!
    i.llwActive = true;
    i.llwParamsEnabled = true;
    for (unsigned n = 0; n < 96; ++n)
        i.network.output.push_back(ethernet(i.address.octet, peerAddr));
    while (i.pullOutputPackets() == kIOReturnSuccess && !i.network.output.empty()) {}
    assert(i.pendingCount == 80); // 96 - 16 reserved for llw0!
    for (unsigned n = 0; n < 16; ++n)
        assert(i.enqueueLLWFrame(ethernet(i.address.octet, peerAddr)) == kIOReturnSuccess);
    assert(i.pendingCount + i.inFlightCount == 96);

    for (auto *m : i.network.output) mbuf_freem(m);
    i.network.output.clear();
    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)


if __name__ == "__main__":
    unittest.main()
