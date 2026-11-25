"""Failing-first regressions for Preview 85: LLW Skywalk prepare(nullptr)/isQueueReady/InfraEnable & AC 9462 80 MHz stability."""
import re
import unittest

import test_regressions as support
from iwx_firmware import declarations as firmware_declarations
from test_offline import definition


class Preview85Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p85_llw_skywalk_null_source_queue_and_ready_gate(self):
        routines = "\n".join([
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmLLWInterface::txSubmissionAction"),
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmLLWInterface::inputSkywalkEthernet"),
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmLLWInterface::setInterfaceEnable"),
        ])
        src = f"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

using IOReturn = int;
using SInt32 = int32_t;
enum {{
    kIOReturnSuccess = 0,
    kIOReturnError = -1,
    kIOReturnNoMemory = -2,
    kIOReturnNotReady = -536870185,
    kIO80211NetworkLinkDown = 0,
    kIO80211NetworkLinkUp = 1
}};

struct OSObject {{}};
struct IOSkywalkPacket;
struct IOSkywalkPacketQueue {{
    unsigned direction = 0;
    bool ready = true;
    bool enabled = false;
    bool overridesPacketCompletion = false;
    bool isQueueReady() const {{ return ready; }}
    void enable() {{ enabled = true; }}
    void disable() {{ enabled = false; }}
    // Models Sequoia IOSkywalkPacketQueue::packetCompletion() base-class panic stub.
    void packetCompletion(IOSkywalkPacket *, IOSkywalkPacketQueue *, unsigned int) {{
        assert(overridesPacketCompletion && "default packetCompletion() @IOSkywalkPacketQueue.cpp:318");
    }}
}};
struct IOSkywalkTxSubmissionQueue : IOSkywalkPacketQueue {{
    IOSkywalkTxSubmissionQueue() {{ direction = 1; overridesPacketCompletion = true; }}
}};
struct IOSkywalkTxCompletionQueue : IOSkywalkPacketQueue {{
    std::vector<IOSkywalkPacket *> completed;
    IOSkywalkTxCompletionQueue() {{ direction = 1; overridesPacketCompletion = false; }}
    IOReturn enqueuePackets(IOSkywalkPacket * const *packets, unsigned int count, unsigned int) {{
        for (unsigned int i = 0; i < count; ++i) completed.push_back(packets[i]);
        return kIOReturnSuccess;
    }}
}};
struct IOSkywalkRxCompletionQueue;

struct IOSkywalkPacket {{
    bool allocated = false;
    bool prepared = false;
    bool failPrepare = false;
    IOSkywalkPacketQueue *sourceQueue = nullptr;
    unsigned transferDirection = 0;
    uint16_t dataOffset = 0;
    uint32_t dataLength = 0;
    uint8_t linkHeaderLen = 0;
    std::vector<uint8_t> storage;

    // Models Sequoia IOSkywalkPacket::prepare + prepareWithQueue (0xffffff800297e4a2..e5c4).
    IOReturn prepare(IOSkywalkPacketQueue *queue, uint64_t = 0, unsigned int = 0) {{
        assert(allocated);
        if (prepared) return kIOReturnSuccess;
        if (failPrepare) return kIOReturnError;
        sourceQueue = queue;
        transferDirection = 0;
        prepared = true;
        return kIOReturnSuccess;
    }}
    // Models Sequoia IOSkywalkPacket::completeWithQueue (0xffffff800297e612..e6aa).
    void completeWithQueue(IOSkywalkPacketQueue *destQueue, unsigned dir, unsigned flags) {{
        if (dir == 2 && sourceQueue != nullptr) {{
            sourceQueue->packetCompletion(this, destQueue, flags);
        }}
        prepared = false;
    }}
    void *getDataVirtualAddress() const {{
        assert(prepared && "getDataVirtualAddress called before IOSkywalkPacket::prepare");
        return const_cast<uint8_t *>(storage.data());
    }}
    unsigned int getDataLength() const {{
        assert(prepared && "getDataLength called before IOSkywalkPacket::prepare");
        return dataLength;
    }}
    unsigned short getDataOffset() const {{
        assert(prepared && "getDataOffset called before IOSkywalkPacket::prepare");
        return dataOffset;
    }}
    unsigned int getBufferSize() const {{
        assert(prepared && "getBufferSize called before IOSkywalkPacket::prepare");
        return unsigned(storage.size());
    }}
    IOReturn setDataOffsetAndLength(unsigned short offset, unsigned int length) {{
        assert(prepared && "setDataOffsetAndLength called before IOSkywalkPacket::prepare");
        if (uint32_t(offset) + length > storage.size()) return kIOReturnError;
        dataOffset = offset;
        dataLength = length;
        return kIOReturnSuccess;
    }}
}};

struct IOSkywalkNetworkPacket : IOSkywalkPacket {{
    IOReturn setLinkHeaderLength(unsigned char length) {{
        assert(prepared && "setLinkHeaderLength called before IOSkywalkPacket::prepare");
        linkHeaderLen = length;
        return kIOReturnSuccess;
    }}
}};

struct IOSkywalkPacketBufferPool {{
    IOSkywalkNetworkPacket *nextPacket = nullptr;
    unsigned allocatedCount = 0;
    unsigned deallocated = 0;
    IOReturn allocatePacket(IOSkywalkPacket **out, unsigned int count) {{
        assert(count == 1);
        ++allocatedCount;
        if (!nextPacket) return kIOReturnNoMemory;
        nextPacket->allocated = true;
        nextPacket->prepared = false;
        nextPacket->sourceQueue = nullptr;
        nextPacket->transferDirection = 0;
        *out = nextPacket;
        nextPacket = nullptr;
        return kIOReturnSuccess;
    }}
    // Models Sequoia IOSkywalkPacketBufferPool::deallocatePacket -> completePreparedPacket (0xffffff800297d84a).
    void deallocatePacket(IOSkywalkPacket *pkt) {{
        assert(pkt != nullptr && pkt->allocated);
        if (pkt->prepared) {{
            const unsigned dir = pkt->transferDirection;
            pkt->completeWithQueue(nullptr, dir, 0);
            if (dir != 2 && pkt->sourceQueue != nullptr) {{
                pkt->sourceQueue->packetCompletion(nullptr, nullptr, 0);
            }}
        }}
        pkt->allocated = false;
        pkt->sourceQueue = nullptr;
        ++deallocated;
    }}
}};

struct IOSkywalkRxCompletionQueue : IOSkywalkPacketQueue {{
    bool failEnqueue = false;
    std::vector<IOSkywalkPacket *> enqueued;
    IOSkywalkRxCompletionQueue() {{ direction = 2; overridesPacketCompletion = false; }}
    // Models Sequoia IOSkywalkRxCompletionQueue::enqueuePackets -> enqueuePacketsForNetworking.
    IOReturn enqueuePackets(IOSkywalkPacket * const *packets, unsigned int count, unsigned int) {{
        if (!ready || failEnqueue) return kIOReturnNotReady;
        for (unsigned int i = 0; i < count; ++i) {{
            assert(packets[i]->prepared);
            packets[i]->completeWithQueue(this, 2, 0);
            enqueued.push_back(packets[i]);
        }}
        return kIOReturnSuccess;
    }}
}};

struct MockMbuf {{ std::vector<uint8_t> bytes; }};
using mbuf_t = MockMbuf *;
static mbuf_t awdlPacket(size_t len) {{ return new MockMbuf{{std::vector<uint8_t>(len, 0)}}; }}
static void *mbuf_data(mbuf_t m) {{ return m->bytes.data(); }}

struct AirportItlwmAWDLInterface {{
    std::vector<std::vector<uint8_t>> queuedTx;
    IOReturn enqueueLLWFrame(mbuf_t m) {{
        queuedTx.push_back(m->bytes);
        delete m;
        return kIOReturnSuccess;
    }}
}};

struct IO80211SkywalkInterface : OSObject {{
    unsigned skywalkEnableCalls = 0;
    SInt32 setInterfaceEnable(bool) {{ ++skywalkEnableCalls; return kIOReturnSuccess; }}
    void reportLinkStatus(unsigned, unsigned) {{}}
    void reportLinkQuality(unsigned) {{}}
}};

struct IO80211InfraInterface : IO80211SkywalkInterface {{
    unsigned infraEnableCalls = 0;
    unsigned linkStateCalls = 0;
    SInt32 setInterfaceEnable(bool enabled) {{
        ++infraEnableCalls;
        return IO80211SkywalkInterface::setInterfaceEnable(enabled);
    }}
    void setLinkState(unsigned, unsigned, bool, unsigned) {{ ++linkStateCalls; }}
}};

struct AirportItlwmLLWInterface : IO80211InfraInterface {{
    bool skywalkRegistered = true;
    bool enabledState = false;
    AirportItlwmAWDLInterface *awdlParent = nullptr;
    IOSkywalkPacketBufferPool *skywalkPool = nullptr;
    IOSkywalkTxSubmissionQueue *txSubQueue = nullptr;
    IOSkywalkTxCompletionQueue *txCompQueue = nullptr;
    IOSkywalkRxCompletionQueue *rxCompQueue = nullptr;
    uint64_t skywalkTxPackets = 0, skywalkTxDropped = 0;
    uint64_t skywalkRxPackets = 0, skywalkRxDropped = 0;

    static unsigned int txSubmissionAction(OSObject *owner,
                                           IOSkywalkTxSubmissionQueue *queue,
                                           IOSkywalkPacket * const *packets,
                                           unsigned int packetCount,
                                           void *refCon);
    bool inputSkywalkEthernet(const uint8_t *ethernet, size_t length);
    SInt32 setInterfaceEnable(bool enabled);
}};

{routines}

int main() {{
    AirportItlwmAWDLInterface parent;
    IOSkywalkPacketBufferPool pool;
    IOSkywalkTxSubmissionQueue txSub;
    IOSkywalkTxCompletionQueue txComp;
    IOSkywalkRxCompletionQueue rxComp;
    AirportItlwmLLWInterface llw;
    llw.awdlParent = &parent;
    llw.skywalkPool = &pool;
    llw.txSubQueue = &txSub;
    llw.txCompQueue = &txComp;
    llw.rxCompQueue = &rxComp;

    std::vector<uint8_t> frame(70, 0x5a);
    frame[12] = 0x86; frame[13] = 0xdd;

    // 1. RX happy path & error path MUST pass nullptr to prepare() so neither
    //    enqueuePackets->completeWithQueue(rxComp,2,0) nor deallocatePacket->completePreparedPacket()
    //    invokes IOSkywalkPacketQueue::packetCompletion() ("default packetCompletion()").
    llw.enabledState = true;
    IOSkywalkNetworkPacket rxPkt;
    rxPkt.storage.assign(2048, 0);
    pool.nextPacket = &rxPkt;
    assert(llw.inputSkywalkEthernet(frame.data(), frame.size()) == true);
    assert(llw.skywalkRxPackets == 1 && llw.skywalkRxDropped == 0);
    assert(rxPkt.sourceQueue == nullptr);
    assert(rxPkt.dataOffset == 0 && rxPkt.dataLength == frame.size() && rxPkt.linkHeaderLen == 14);
    assert(memcmp(rxPkt.storage.data(), frame.data(), frame.size()) == 0);
    assert(rxComp.enqueued.size() == 1 && rxComp.enqueued[0] == &rxPkt);

    // 2. RX enqueue failure after prepare: deallocatePacket -> completePreparedPacket NEVER panics.
    IOSkywalkNetworkPacket rxEnqFailPkt;
    rxEnqFailPkt.storage.assign(2048, 0);
    pool.nextPacket = &rxEnqFailPkt;
    rxComp.failEnqueue = true;
    assert(llw.inputSkywalkEthernet(frame.data(), frame.size()) == false);
    assert(llw.skywalkRxPackets == 1 && llw.skywalkRxDropped == 1 && pool.deallocated == 1);
    rxComp.failEnqueue = false;

    // 3. RX buffer too small after prepare: deallocatePacket NEVER panics.
    IOSkywalkNetworkPacket rxSmallPkt;
    rxSmallPkt.storage.assign(32, 0);
    pool.nextPacket = &rxSmallPkt;
    assert(llw.inputSkywalkEthernet(frame.data(), frame.size()) == false);
    assert(llw.skywalkRxPackets == 1 && llw.skywalkRxDropped == 2 && pool.deallocated == 2);

    // 4. When rxCompQueue is not ready (0xcc == 0), inputSkywalkEthernet drops BEFORE allocating from pool.
    const unsigned allocBefore = pool.allocatedCount;
    rxComp.ready = false;
    assert(llw.inputSkywalkEthernet(frame.data(), frame.size()) == false);
    assert(pool.allocatedCount == allocBefore && llw.skywalkRxDropped == 3 && llw.skywalkRxPackets == 1);
    rxComp.ready = true;

    // 5. setInterfaceEnable delegates to IO80211InfraInterface::setInterfaceEnable (matching AppleBCMWLANLowLatencyInterface).
    llw.enabledState = false;
    assert(llw.setInterfaceEnable(true) == kIOReturnSuccess);
    assert(llw.infraEnableCalls == 1 && llw.enabledState && rxComp.enabled);

    // 6. TX submission path remains intact and safe.
    IOSkywalkNetworkPacket txPkt;
    txPkt.allocated = true;
    txPkt.prepared = false;
    txPkt.storage.assign(2048, 0);
    txPkt.dataOffset = 16;
    txPkt.dataLength = uint32_t(frame.size());
    memcpy(txPkt.storage.data() + 16, frame.data(), frame.size());
    IOSkywalkPacket *txBatch[1] = {{&txPkt}};
    assert(AirportItlwmLLWInterface::txSubmissionAction(&llw, &txSub, txBatch, 1, &llw) == 1);
    assert(llw.skywalkTxPackets == 1 && parent.queuedTx.size() == 1 && txComp.completed.size() == 1);
    return 0;
}}
"""
        self.compile_run(src)
        header = (support.ROOT / "include/Airport/macOS15/IOSkywalkSequoiaQueues.h").read_text()
        self.assertIn("bool isQueueReady() const;", header)

    def test_p85_quz0034_stays_20mhz_and_width_shrink_precedes_phy(self):
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
        # Record exact PHY vs peer-STA FAT_EN modification ordering in the fixture.
        radio_src = radio_src.replace(
            "std::vector<iwx_phy_context_cmd> phyCommands;",
            "std::vector<iwx_phy_context_cmd> phyCommands;std::vector<uint32_t> orderLog;",
        )
        radio_src = radio_src.replace(
            "phyCommands.push_back(*c);",
            "phyCommands.push_back(*c);orderLog.push_back((1U<<16)|c->ci.width);",
        )
        radio_src = radio_src.replace(
            "assert(id==IWX_ADD_STA);auto c=static_cast<const iwx_add_sta_cmd*>(data);",
            "assert(id==IWX_ADD_STA);auto c=static_cast<const iwx_add_sta_cmd*>(data);"
            "if(c->add_modify==IWX_STA_MODE_MODIFY && (c->station_flags_msk & IWX_STA_FLG_FAT_EN_MSK)) {"
            "orderLog.push_back((2U<<16)|((c->station_flags & IWX_STA_FLG_FAT_EN_MSK)?2U:0U));"
            "*status=IWX_ADD_STA_SUCCESS;return command(\"modify-sta-width\");}",
        )
        extra = r"""
static void preview85QuzAndWidthOrder() {
    auto allowWide = [](ItlIwx &d) {
        for (unsigned c : {36U, 40U, 44U, 48U, 149U, 153U, 157U, 161U}) {
            d.com.sc_ic.ic_channels[c] = {IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_VHT80, uint16_t(5000 + 5 * c)};
            d.com.sc_ic.ic_chan_active[c / 8] |= 1U << (c % 8);
        }
    };

    // 1. On Intel AC 9462 (quz0034) with en1 on ch 48, Preview 87 allows disjoint 80 MHz
    //    on ch 149 while keeping overlapping ch 48 / ch 44 at 20 MHz and clamping to 20 MHz
    //    if the self-healing wide circuit breaker trips.
    {
        ItlIwx q;
        allowWide(q);
        q.rlcVersion = 2;
        q.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
        q.com.sc_cfg = &iwl9560_quz_a0_jf_b0_cfg;
        q.awdlRadio.quz0034 = iwx_awdl_is_quz_0034(0x02f0, 0x8086, 0x0034, q.com.sc_cfg);
        q.com.sc_phyctxt[0].channel = &q.com.sc_ic.ic_channels[48];
        assert(q.awdlRadio.quz0034);
        assert(q.awdlChannelAllowed(149, 20));
        assert(q.awdlChannelAllowed(149, 80));
        assert(!q.startAWDLRadio(self));
        assert(q.awdlRadio.widthMHz == 80);
        RadioState::GrantPolicy req80 = {};
        req80.widthMHz = 80;
        assert(!q.serviceAWDLChannel(48, clockUS + 65536, req80));
        assert(q.awdlRadio.widthMHz == 20);
        assert(!q.serviceAWDLChannel(44, clockUS + 65536, req80));
        assert(q.awdlRadio.widthMHz == 20);
        assert(!q.serviceAWDLChannel(149, clockUS + 65536, req80));
        assert(q.awdlRadio.widthMHz == 80);
        q.awdlRadio.fwHealth.wideFaultCount = 2;
        assert(!q.serviceAWDLChannel(6, clockUS + 65536, {}));
        assert(!q.serviceAWDLChannel(149, clockUS + 65536, req80));
        assert(q.awdlRadio.widthMHz == 20);
        assert(!q.stopAWDLRadio());
    }

    // 2. On non-quz0034 wide-capable cards:
    //    - Shrinking 80 -> 20 MUST modify peer STA FAT_EN -> 20 BEFORE shrinking PHY -> 20.
    //    - Growing 20 -> 80 MUST grow PHY -> 80 BEFORE modifying peer STA FAT_EN -> 80.
    {
        ItlIwx w;
        allowWide(w);
        w.rlcVersion = 2;
        w.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
        assert(!w.awdlRadio.quz0034);
        assert(w.awdlChannelAllowed(149, 80));
        assert(!w.startAWDLRadio(self) && w.awdlRadio.widthMHz == 80);

        // Attach an active BA peer station at 80 MHz.
        w.awdlBA.peers[0].station = true;
        w.awdlBA.peers[0].epoch = w.dmaEpoch;
        w.orderLog.clear();

        // Shrink 149@80 -> 6@20: STA FAT_EN=20 (0x20000) MUST precede PHY width=20 (0x10000).
        RadioState::GrantPolicy p20 = {};
        p20.widthMHz = 20;
        assert(!w.serviceAWDLChannel(6, clockUS + 65536, p20));
        assert(w.awdlRadio.widthMHz == 20 && w.awdlRadio.channel == 6);
        int staShrinkIdx = -1, phyShrinkIdx = -1;
        for (size_t i = 0; i < w.orderLog.size(); ++i) {
            if (w.orderLog[i] == ((2U << 16) | 0U) && staShrinkIdx < 0) staShrinkIdx = int(i);
            if (w.orderLog[i] == ((1U << 16) | IWX_PHY_VHT_CHANNEL_MODE20) && phyShrinkIdx < 0) phyShrinkIdx = int(i);
        }
        assert(staShrinkIdx >= 0 && phyShrinkIdx >= 0 && staShrinkIdx < phyShrinkIdx);

        // Grow 6@20 -> 149@80: PHY width=80 (0x10002) MUST precede STA FAT_EN=80 (0x20002).
        w.orderLog.clear();
        RadioState::GrantPolicy p80 = {};
        p80.widthMHz = 80;
        assert(!w.serviceAWDLChannel(149, clockUS + 65536, p80));
        assert(w.awdlRadio.widthMHz == 80 && w.awdlRadio.channel == 149);
        int phyGrowIdx = -1, staGrowIdx = -1;
        for (size_t i = 0; i < w.orderLog.size(); ++i) {
            if (w.orderLog[i] == ((1U << 16) | IWX_PHY_VHT_CHANNEL_MODE80) && phyGrowIdx < 0) phyGrowIdx = int(i);
            if (w.orderLog[i] == ((2U << 16) | 2U) && staGrowIdx < 0) staGrowIdx = int(i);
        }
        assert(phyGrowIdx >= 0 && staGrowIdx >= 0 && phyGrowIdx < staGrowIdx);
        assert(!w.stopAWDLRadio());
    }
}

int main() {
    preview85QuzAndWidthOrder();
    return 0;
}
"""
        self.compile_run(radio_src + extra)

    def test_p85_channel_sequence_does_not_force_80mhz_on_20mhz_flags(self):
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
static void preview85NoForced80Promotion() {
    AirportItlwmAWDLInterface i;
    i.device.ic.ic_state = IEEE80211_S_RUN;
    i.device.ic.ic_bss = &i.device.ap;
    i.device.apChannel.channel = 48;
    i.device.allow80 = true;
    i.device.awdlRadio.ready = true;

    const unsigned channels[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    // Preview 87: disjoint non-infra ch 149 promotes to OpClass 128 while infra AP ch 48 & ch 6 stay 20 MHz.
    apple80211_awdl_sync_channel_sequence seq20 = {};
    seq20.version = 1;
    seq20.pad1 = 0x00;
    seq20.length = 16;
    seq20.encoding = 1;
    seq20.step_count = 3;
    seq20.fill_channel = 0xff;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = channels[n];
        const unsigned flags = c == 48 ? 0x294U : c == 6 ? 0x8aU : (n == 0 ? 0x10U : 0x92U);
        seq20.seqs[n] = {1, c, flags};
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq20) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n) {
        assert(i.sync.channelClasses[n] == (channels[n] == 149 ? 128 : 0));
    }

    // Explicit maxBandwidth < 3 (pad1 = 1) keeps all slots at 20 MHz.
    seq20.pad1 = 0x01;
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq20) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n) {
        assert(i.sync.channelClasses[n] == 0);
    }

    // Explicit APPLE80211_C_FLAG_80MHZ still selects OpClass 128 on disjoint ch 149.
    apple80211_awdl_sync_channel_sequence seq80 = seq20;
    seq80.pad1 = 0x00;
    for (unsigned n = 0; n < 16; ++n) {
        if (channels[n] == 149) seq80.seqs[n].flags = APPLE80211_C_FLAG_ACTIVE | APPLE80211_C_FLAG_5GHZ | APPLE80211_C_FLAG_80MHZ;
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq80) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n) {
        assert(i.sync.channelClasses[n] == (channels[n] == 149 ? 128 : 0));
    }
}

int main() {
    preview85NoForced80Promotion();
    return 0;
}
"""
        self.compile_run(source + extra)


if __name__ == "__main__":
    unittest.main()
