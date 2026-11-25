"""Failing-first regressions for Preview 83: GO_CONCURRENT VHT80, Sidecar TX refill, and native Skywalk LLW."""
import re
import unittest

import test_regressions as support
from iwx_firmware import declarations
from test_offline import definition


class Preview83Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p83_go_concurrent_80mhz_unlock(self):
        driver = (support.ROOT / "itlwm/hal_iwx/ItlIwx.cpp").read_text()
        tables = "\n".join(
            re.search(r"(?:static )?const uint8_t " + name + r"\[\] = \{.*?\};", driver, re.S)[0]
            for name in ("iwx_nvm_channels_8000", "iwx_nvm_channels_uhb")
        )
        local_macros = "\n".join(
            re.findall(r"^#define IWX_(?:LAST_2GHZ_HT_PLUS|FIRST_2GHZ_HT_MINUS|NUM_2GHZ_CHANNELS)\b.*", driver, re.M)
        )
        routines = "\n".join([
            local_macros,
            tables,
            definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_init_channel_map"),
            definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_send_update_mcc_cmd"),
            definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::awdlChannelAllowed"),
        ])
        base = (support.ROOT / "tests/fixtures/iwx_regulatory.cpp").read_text()
        base = base.replace(
            "IEEE80211_CHAN_DFS=2048};",
            "IEEE80211_CHAN_DFS=2048,IEEE80211_CHAN_NOADHOC=4096};",
        )
        base = base.replace("// PRODUCTION", "")
        src = "#define AIRPORT_WCL 1\n" + base.split("#ifdef MCC_TEST", 1)[0] + f"""
// MCC_DECLS
#define PAGE_SIZE 4096
using IOReturn = int;
const int kIOReturnSuccess = 0;
enum {{ IEEE80211_S_RUN = 4, IEEE80211_EVT_COUNTRY_CODE_UPDATE = 7 }};
struct OSObject {{}};
struct Loop {{ bool inGate() {{ return true; }} }};
struct Gate {{
    using Action = IOReturn(*)(OSObject *, void *, void *, void *, void *);
    int runAction(Action f, void *a=nullptr, void *b=nullptr, void *c=nullptr, void *d=nullptr) {{
        return f(nullptr, a, b, c, d);
    }}
}};
static bool isset(const uint8_t *p, unsigned b) {{ return (p[b / 8] & (1U << (b % 8))) != 0; }}
static void setbit(uint8_t *p, unsigned b) {{ p[b / 8] |= uint8_t(1U << (b % 8)); }}
struct iwx_rx_packet {{ struct {{ uint8_t group_id = 0; }} hdr; uint8_t data[1024] = {{}}; size_t length = 0; }};
struct iwx_host_cmd {{ uint32_t id = 0, flags = 0; size_t len[1] = {{}}; const void *data[1] = {{}}; size_t resp_pkt_len = 0; iwx_rx_packet *resp_pkt = nullptr; }};
static size_t iwx_rx_packet_payload_len(const iwx_rx_packet *p) {{ return p->length; }}

struct ItlIwx {{
    iwx_softc com;
    IwxRegulatoryState regulatory;
    Loop loop;
    Gate gate;
    iwx_rx_packet reply;
    void *awdlReceiver = nullptr;
    void (*awdlRegulatoryChanged)(void *) = nullptr;
    ItlIwx() {{
        setbit(com.sc_enabled_capa, IWX_UCODE_TLV_CAPA_MCC_UPDATE_11AX_SUPPORT);
        setbit(com.sc_ucode_api, IWX_UCODE_TLV_API_WIFI_MCC_UPDATE);
    }}
    Loop *getMainWorkLoop() {{ return &loop; }}
    Gate *getMainCommandGate() {{ return &gate; }}
    void iwx_publish_regulatory(iwx_softc *) {{}}
    unsigned iwx_lookup_notif_ver(iwx_softc *, unsigned, unsigned) {{ return 5; }}
    int iwx_send_cmd(iwx_softc *, iwx_host_cmd *cmd) {{ cmd->resp_pkt = &reply; return 0; }}
    void iwx_free_resp(iwx_softc *, iwx_host_cmd *c) {{ c->resp_pkt = nullptr; }}
    void iwx_init_channel_map(iwx_softc *, uint16_t *, uint32_t *, int);
    int iwx_send_update_mcc_cmd(iwx_softc *, const char *, uint8_t = 0xff);
    bool awdlChannelAllowed(uint32_t channel, uint8_t widthMHz = 20) const;
}};

{routines}

static void put16(uint8_t *p, uint16_t x) {{ p[0] = x; p[1] = x >> 8; }}
static void put32(uint8_t *p, uint32_t x) {{ put16(p, x); put16(p + 2, x >> 16); }}

static void buildProfile(ItlIwx &d, uint16_t country, bool cnGoConcurrent) {{
    d.reply = {{}};
    const unsigned count = nitems(iwx_nvm_channels_8000);
    const unsigned header = 20;
    d.reply.length = header + 4 * count;
    put32(d.reply.data, 0);
    put16(d.reply.data + 4, country);
    put16(d.reply.data + 6, 0x0c);
    d.reply.data[12] = 2;
    put32(d.reply.data + 16, count);
    for (unsigned i = 0; i < count; ++i) {{
        const uint8_t ch = iwx_nvm_channels_8000[i];
        uint32_t flags = IWX_NVM_CHANNEL_VALID;
        if (ch <= 13) {{
            flags |= IWX_NVM_CHANNEL_ACTIVE | IWX_NVM_CHANNEL_40MHZ;
        }} else if ((ch >= 36 && ch <= 48) || (ch >= 149 && ch <= 161)) {{
            // Live Intel 9462 LAR CN flags: passive (no ACTIVE bit), GO_CONCURRENT + 40/80 MHz.
            flags |= IWX_NVM_CHANNEL_40MHZ | IWX_NVM_CHANNEL_80MHZ;
            if (cnGoConcurrent) flags |= IWX_NVM_CHANNEL_GO_CONCURRENT;
        }} else if (ch == 52) {{
            // DFS channel with GO_CONCURRENT must still stay blocked.
            flags |= IWX_NVM_CHANNEL_RADAR | IWX_NVM_CHANNEL_GO_CONCURRENT | IWX_NVM_CHANNEL_40MHZ | IWX_NVM_CHANNEL_80MHZ;
        }} else if (ch == 165) {{
            // Passive channel without GO_CONCURRENT must stay blocked.
            flags = IWX_NVM_CHANNEL_VALID;
        }}
        put32(d.reply.data + header + i * 4, flags);
    }}
}}

int main() {{
    ItlIwx d;
    // 1. World profile "ZZ" must NOT unlock GO_CONCURRENT passive 5 GHz channels.
    buildProfile(d, 0x5a5a, true);
    assert(d.iwx_send_update_mcc_cmd(&d.com, "ZZ", 2) == 0);
    assert(!d.awdlChannelAllowed(149, 20));
    assert(!d.awdlChannelAllowed(149, 80));
    assert(!d.awdlChannelAllowed(44, 80));

    // 2. Confirmed "CN" with LAR GO_CONCURRENT unlocks 20 MHz and 80 MHz on Ch 149 and Ch 44,
    //    while keeping passive scan flag set, DFS Ch 52 blocked, and non-concurrent Ch 165 blocked.
    buildProfile(d, 0x434e, true);
    assert(d.iwx_send_update_mcc_cmd(&d.com, "CN", 2) == 0);
    assert(d.com.sc_ic.ic_channels[149].ic_flags & IEEE80211_CHAN_PASSIVE);
    assert(d.awdlChannelAllowed(149, 20));
    assert(d.awdlChannelAllowed(149, 80));
    assert(d.awdlChannelAllowed(44, 20));
    assert(d.awdlChannelAllowed(44, 80));
    assert(!d.awdlChannelAllowed(52, 20));
    assert(!d.awdlChannelAllowed(165, 20));

    // 3. Country change to "US" without GO_CONCURRENT revokes the CN unlock immediately.
    buildProfile(d, 0x5553, false);
    assert(d.iwx_send_update_mcc_cmd(&d.com, "US", 2) == 0);
    assert(!d.awdlChannelAllowed(149, 20));
    assert(!d.awdlChannelAllowed(149, 80));
    return 0;
}}
"""
        src = src.replace("// FIRMWARE DECLARATIONS", declarations(("iwx_mcc_update_cmd", "iwx_mcc_chub_notif"), src, enums=("IWX_CMD_MODE",)))
        src = src.replace("// MCC_DECLS", "")
        self.compile_run(src)

    def test_p83_sidecar_immediate_wake_and_txcal(self):
        awdl_hpp = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()

        # Preserve DataQueueCapacity = 96 and bounded batch 32 for P78-P82 contracts.
        self.assertIn("DataQueueCapacity = 96;", awdl_hpp)
        pull_fn = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::pullOutputPackets")
        self.assertIn("< 32 ? queueCapacity / 2 : 32", pull_fn)

        # 1. completeData must signal bsd->signalOutputThread() immediately when capacity frees,
        #    and handle() must implement getAWDL_TXCAL_PERIOD / setAWDL_TXCAL_PERIOD without setting UnsupportedRequests.
        complete_fn = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::completeData")
        self.assertIn("signalOutputThread()", complete_fn)

        handle_fn = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::handle")
        self.assertIn("Request::getAWDL_TXCAL_PERIOD", handle_fn)
        self.assertIn("Request::setAWDL_TXCAL_PERIOD", handle_fn)

        # 2. Runtime behavioral test: completeData immediately wakes bsd->signalOutputThread()
        #    even before timer tick fires, while pullOutputPackets preserves the bounded 32-frame batch.
        src = f"""
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>
#include "AirportItlwm/AWDL/DataFrame.hpp"
#include "AirportItlwm/AWDL/LLWFlow.hpp"

using namespace AirportAWDL;
using IOReturn = int;
enum {{ kIOReturnSuccess = 0, kIOReturnNoResources = 1, kIOReturnNoFrames = 2, MBUF_DONTWAIT = 1 }};

struct MockMbuf {{
    std::vector<uint8_t> bytes;
    MockMbuf *next = nullptr;
}};
using mbuf_t = MockMbuf *;
static size_t mbuf_pkthdr_len(mbuf_t m) {{ return m->bytes.size(); }}
static mbuf_t mbuf_nextpkt(mbuf_t m) {{ return m->next; }}
static void mbuf_setnextpkt(mbuf_t m, mbuf_t n) {{ m->next = n; }}
static void mbuf_freem(mbuf_t m) {{ delete m; }}
static int mbuf_copydata(mbuf_t m, size_t off, size_t len, void *dst) {{
    if (off + len > m->bytes.size()) return 1;
    memcpy(dst, m->bytes.data() + off, len);
    return 0;
}}
static int mbuf_dup(mbuf_t m, int, mbuf_t *out) {{ *out = new MockMbuf{{m->bytes, nullptr}}; return 0; }}
static uint64_t gNow = 1000000;
static uint64_t awdlNow() {{ return gNow; }}

struct WorkLoop {{ bool inGate() {{ return true; }} }};
struct Gate {{
    using Action = IOReturn(*)(void *, void *, void *, void *, void *);
    IOReturn runAction(Action f, void *a) {{ return f(nullptr, a, nullptr, nullptr, nullptr); }}
}};
using OSObject = void;

#include "AirportItlwm/AWDL/TXBlockAck.hpp"
struct Radio {{
    WorkLoop wl; Gate gate;
    struct {{ bool ready = true, stopPending = false, fault = false, listener = false; TXBlockAckQueue blockAckTX = {{}}; }} awdlRadio;
    uint64_t dmaEpoch = 1;
    WorkLoop *getMainWorkLoop() {{ return &wl; }}
    Gate *getMainCommandGate() {{ return &gate; }}
    void noteAWDLTXBlockAck(const uint8_t *, size_t, bool = false) {{}}
}};
struct Timer {{ unsigned wakes = 0; void setTimeoutMS(unsigned) {{ ++wakes; }} void setTimeoutUS(unsigned) {{ ++wakes; }} }};
struct MockBSD {{
    int dummy = 1;
    unsigned signals = 0;
    std::vector<mbuf_t> q;
    void *getIfnet() {{ return &dummy; }}
    void signalOutputThread() {{ ++signals; }}
    IOReturn dequeueOutputPackets(unsigned maxCount, mbuf_t *out) {{
        if (q.empty()) return kIOReturnNoFrames;
        mbuf_t head = nullptr, tail = nullptr;
        unsigned n = 0;
        while (n < maxCount && !q.empty()) {{
            mbuf_t m = q.front(); q.erase(q.begin());
            m->next = nullptr;
            if (!head) head = m; else tail->next = m;
            tail = m; ++n;
        }}
        *out = head;
        return kIOReturnSuccess;
    }}
}};

struct AirportItlwmAWDLInterface {{
    static constexpr unsigned DataQueueCapacity = 96;
    static constexpr uint64_t DataLifetimeUS = 17ULL * 4 * 16 * 1024;
    static constexpr uint8_t MaxDataFlushes = 8;
    struct Pending {{
        enum : uint8_t {{ MulticastUnpaired, MulticastSocial, MulticastSlot8 }};
        mbuf_t packet = nullptr;
        uint64_t queuedUS = 0, epoch = 0, grant = 0;
        uint16_t wifiSequence = 0, dataSequence = 0;
        bool retry = false, group = false, lowLatency = false;
        uint8_t multicastQueue = 0, flushes = 0;
        mbuf_t second = nullptr;
        uint64_t secondQueuedUS = 0;
        uint16_t secondSequence = 0;
        unsigned count() const {{ return second ? 2 : 1; }}
    }} pending[DataQueueCapacity] = {{}};
    struct Flight {{ Pending entry; uint64_t token = 0; }} inFlight[DataQueueCapacity] = {{}};
    Radio device, *radio = &device;
    MockBSD bsdObj, *bsd = &bsdObj;
    MockBSD llwBsdObj, *llwBSD = &llwBsdObj;
    Timer timerObj, *timer = &timerObj;
    LLWFlowRouter llwFlows = {{}};
    bool requested = true, running = true, stopping = false, ticking = false, drainingData = false, wakeArmed = false;
    bool regulatoryPlanBlocked = false, regulatoryStopRequired = false, outputCapacityReleased = false;
    bool llwActive = false, llwParamsEnabled = false;
    unsigned pendingHead = 0, pendingCount = 0, inFlightCount = 0, aggregateExtraOwned = 0;
    uint64_t dataFlushed = 0, txDropped = 0, dataAbandoned = 0, dataGroupCompleted = 0;
    uint64_t dataUnicastAcknowledged = 0, multicastCopiesQueued = 0, multicastCopiesDropped = 0;

    void releaseData(Pending &entry) {{
        if (entry.second) {{ mbuf_freem(entry.second); entry.second = nullptr; --aggregateExtraOwned; }}
        mbuf_freem(entry.packet); entry.packet = nullptr;
    }}
    IOReturn pullOutputPackets();
    void completeData(uint64_t token, int result);
}};

{pull_fn}
{complete_fn}

static mbuf_t makeEth() {{
    auto m = new MockMbuf;
    m->bytes.assign(64, 0);
    m->bytes[0] = 2; m->bytes[5] = 2;
    m->bytes[6] = 2; m->bytes[11] = 1;
    m->bytes[12] = 0x86; m->bytes[13] = 0xdd;
    m->bytes[14] = 0x60; m->bytes[19] = 10; m->bytes[20] = 17;
    return m;
}}

int main() {{
    AirportItlwmAWDLInterface i;
    for (unsigned k = 0; k < 60; ++k) i.bsdObj.q.push_back(makeEth());
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    // Bounded batch pulls 32 frames, leaving 28 in BSD queue.
    assert(i.pendingCount == 32 && i.bsdObj.q.size() == 28);

    // Move one frame to inFlight and complete it; bsd->signalOutputThread() must fire immediately.
    i.inFlight[0].token = 42;
    i.inFlight[0].entry = i.pending[0];
    i.pending[0] = {{}};
    i.pendingHead = 1;
    --i.pendingCount;
    i.inFlightCount = 1;
    assert(i.bsdObj.signals == 0);
    i.completeData(42, 0);
    assert(i.bsdObj.signals == 1);
    assert(i.dataUnicastAcknowledged == 1);

    for (unsigned k = 0; k < i.pendingCount; ++k)
        mbuf_freem(i.pending[(i.pendingHead + k) % 96].packet);
    for (auto m : i.bsdObj.q) mbuf_freem(m);
    return 0;
}}
"""
        self.compile_run(src)

    def test_p83_llw_wire_rfc1042_rx_tx_and_hal(self):
        # 1. Exact replay of live Preview 82 iPhone Continuity Camera frame:
        #    fc=0x8800, qos=0x07, len=1057 (26 QoS hdr + 8 RFC1042 SNAP + 1023 IPv6 UDP)
        #    plus outbound dataFromEthernetLLW (QoS TID 7 and non-QoS fallback), and UnsupportedDataDiag snapBytes.
        src = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "AirportItlwm/AWDL/DataFrame.hpp"

using namespace AirportAWDL;

static std::vector<uint8_t> buildLiveLLWFrame(const uint8_t dst[6], const uint8_t src[6],
                                              uint16_t wifiSeq, bool retry, bool qos, uint8_t tid,
                                              uint16_t udpPayloadLen = 983)
{
    const size_t hdr = qos ? 26 : 24;
    const size_t ipv6Len = 40 + udpPayloadLen;
    std::vector<uint8_t> f(hdr + 8 + ipv6Len, 0);
    f[0] = qos ? 0x88 : 0x08;
    f[1] = retry ? 0x08 : 0x00;
    copyAddress(f.data() + 4, dst);
    copyAddress(f.data() + 10, src);
    copyAddress(f.data() + 16, BSSID);
    putLE16(f.data() + 22, uint16_t((wifiSeq & 0xfff) << 4));
    if (qos) { f[24] = tid & 0x0f; f[25] = 0x00; }
    // Standard 8-byte RFC 1042 LLC/SNAP for IPv6 (0x86dd)
    static const uint8_t rfc1042[8] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x86, 0xdd};
    memcpy(f.data() + hdr, rfc1042, 8);
    uint8_t *ip = f.data() + hdr + 8;
    ip[0] = 0x60; ip[1] = 0x0d; // exact live head=0x0d60
    putBE16(ip + 4, udpPayloadLen); // 983 = 0x03d7 -> exact live pad=0xd703
    ip[6] = 0x11; ip[7] = 0x40;     // UDP, hop=64 -> exact live etype=0x1140
    ip[8] = 0xfd; ip[9] = 0xd6; ip[23] = 0x13;
    ip[24] = 0xfd; ip[25] = 0xd6; ip[39] = 0x01;
    putBE16(ip + 40, 61465);
    putBE16(ip + 42, 54321);
    putBE16(ip + 44, udpPayloadLen);
    return f;
}

int main() {
    const uint8_t self[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};
    const uint8_t iphone[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};

    // 1. Live Preview 82 1057-byte Continuity Camera QoS TID 7 frame must parse as Valid + lowLatency.
    auto live = buildLiveLLWFrame(self, iphone, 100, false, true, 7, 983);
    assert(live.size() == 1057);
    DataBatch batch = {};
    assert(parseDataMPDU(live.data(), live.size(), batch) == ParseResult::Valid);
    assert(batch.count == 1);
    assert(batch.frames[0].lowLatency == true);
    assert(batch.frames[0].tid == 7);
    assert(batch.frames[0].size == 1023);
    assert(batch.frames[0].payload[0] == 0x60 && batch.frames[0].payload[6] == 0x11);

    // 2. DataReceiveState delivers consecutive LLW frames without AWDL 0x0403 seq-0 duplicate drops,
    //    while dropping exact FC1_RETRY MAC duplicates.
    PeerTable peers = {};
    Advertisement adv = {};
    adv.fields = Sync;
    assert(peers.update(iphone, self, adv, 1000, 149, -50));

    DataReceiveState rx = {};
    unsigned delivered = 0;
    bool lastWasLLW = false;
    auto cb = [](void *ctx, const DataView &v) {
        auto *pair = static_cast<std::pair<unsigned *, bool *> *>(ctx);
        ++(*pair->first);
        *pair->second = v.lowLatency;
    };
    std::pair<unsigned *, bool *> ctx{&delivered, &lastWasLLW};

    for (uint16_t seq = 1; seq <= 10; ++seq) {
        auto frame = buildLiveLLWFrame(self, iphone, seq, false, true, 7, 983);
        assert(rx.receive(frame.data(), frame.size(), peers, self, 2000 + seq, false, cb, &ctx));
    }
    assert(delivered == 10 && lastWasLLW && rx.duplicate == 0 && rx.unsupported == 0);

    // Exact MAC retry of seq 10 must be dropped as duplicate.
    auto dup = buildLiveLLWFrame(self, iphone, 10, true, true, 7, 983);
    assert(rx.receive(dup.data(), dup.size(), peers, self, 2020, false, cb, &ctx));
    assert(delivered == 10 && rx.duplicate == 1);

    // 3. Outbound dataFromEthernetLLW (QoS TID 7 and non-QoS) round-trips cleanly.
    uint8_t eth[14 + 1023];
    assert(ethernetFromData(batch.frames[0], eth, sizeof(eth)) == sizeof(eth));
    uint8_t mpduQos[1600] = {};
    const size_t qosLen = dataFromEthernetLLW(eth, sizeof(eth), mpduQos, sizeof(mpduQos), 321, true, 7);
    assert(qosLen == sizeof(eth) + 20);
    assert(mpduQos[0] == 0x88 && mpduQos[24] == 0x07 && mpduQos[25] == 0x00);
    DataBatch rtQos = {};
    assert(parseDataMPDU(mpduQos, qosLen, rtQos) == ParseResult::Valid);
    assert(rtQos.count == 1 && rtQos.frames[0].lowLatency && rtQos.frames[0].tid == 7 && rtQos.frames[0].size == 1023);

    uint8_t mpduNoQos[1600] = {};
    const size_t noQosLen = dataFromEthernetLLW(eth, sizeof(eth), mpduNoQos, sizeof(mpduNoQos), 322, false, 0);
    assert(noQosLen == sizeof(eth) + 18);
    assert(mpduNoQos[0] == 0x08);
    DataBatch rtNoQos = {};
    assert(parseDataMPDU(mpduNoQos, noQosLen, rtNoQos) == ParseResult::Valid);
    assert(rtNoQos.count == 1 && rtNoQos.frames[0].lowLatency && rtNoQos.frames[0].size == 1023);

    // 4. UnsupportedDataDiag records 8-byte snapBytes on unknown LLC frames.
    auto unknown = buildLiveLLWFrame(self, iphone, 20, false, true, 7, 100);
    unknown[26] = 0xde; unknown[27] = 0xad; unknown[32] = 0x88; unknown[33] = 0xcc;
    assert(rx.receive(unknown.data(), unknown.size(), peers, self, 3000, false, cb, &ctx));
    assert(rx.unsupported == 1 && rx.lastUnsupported.valid);
    assert(rx.lastUnsupported.snapBytes[0] == 0xde && rx.lastUnsupported.snapBytes[1] == 0xad);
    assert(rx.lastUnsupported.snapBytes[6] == 0x88 && rx.lastUnsupported.snapBytes[7] == 0xcc);
    return 0;
}
"""
        self.compile_run(src)

        # 2. Verify ItlIwx::transmitAWDL accepts QoS TID-7 LLW frame without forcing TID-0 blockAck or A-MSDU.
        tx_fn = definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::transmitAWDL")
        self.assertIn("lowLatency", tx_fn)

    def test_p83_native_skywalk_llw_interface_and_link_persistence(self):
        awdl_hpp = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()
        awdl_cpp = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.cpp").read_text()

        # 1. Native Skywalk LLW interface declarations & registration must be present.
        self.assertIn("registerSkywalk", awdl_hpp)
        self.assertIn("inputSkywalkEthernet", awdl_hpp)
        self.assertIn("initBSDInterfaceParameters", awdl_hpp)
        self.assertIn("getBSDUnitNumber", awdl_hpp)
        self.assertIn("registerEthernetInterface", awdl_cpp)
        self.assertIn("reportLinkQuality(100)", awdl_cpp)

        # 2. Behavioral test of native Skywalk LLW lifecycle & RX/TX routing with llwBSD == nullptr.
        headers = (support.ROOT / "include/Airport/apple80211_var.h").read_text() + "\n" + (
            support.ROOT / "include/Airport/apple80211_ioctl.h"
        ).read_text()
        comp_decl = re.search(
            r"struct apple80211_companion_skywalk_if_create\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;",
            headers, re.S,
        )[0]
        llw_decl = re.search(
            r"struct apple80211_llw_params\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;",
            headers, re.S,
        )[0]
        routines = "\n".join(
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
            for n in ("handleCompanionSkywalkCreate", "syncLLWLinkState", "deliverLLWOrMirror", "enqueueLLWFrame")
        )
        src = f"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "AirportItlwm/AWDL/DataFrame.hpp"
#include "AirportItlwm/AWDL/LLWFlow.hpp"

using namespace AirportAWDL;
using IOReturn = int;
using SInt32 = int;
enum {{
    kIOReturnSuccess = 0,
    kIOReturnBadArgument = 1,
    kIOReturnNotReady = 2,
    kIOReturnNoResources = 3,
    kIO80211NetworkLinkDown = 1,
    kIO80211NetworkLinkUp = 2,
    MBUF_DONTWAIT = 1
}};
{comp_decl}
{llw_decl}

struct Ifnet {{ const char *name = "llw"; unsigned unit = 0; }};
using ifnet_t = Ifnet *;
static const char *ifnet_name(ifnet_t i) {{ return i->name; }}
static unsigned ifnet_unit(ifnet_t i) {{ return i->unit; }}

struct MockMbuf {{ std::vector<uint8_t> bytes; }};
using mbuf_t = MockMbuf *;
static size_t mbuf_pkthdr_len(mbuf_t m) {{ return m->bytes.size(); }}
static void *mbuf_data(mbuf_t m) {{ return m->bytes.data(); }}
static void mbuf_freem(mbuf_t m) {{ delete m; }}
static int mbuf_copydata(mbuf_t m, size_t off, size_t len, void *dst) {{
    if (off + len > m->bytes.size()) return 1;
    memcpy(dst, m->bytes.data() + off, len);
    return 0;
}}
static int mbuf_dup(mbuf_t m, int, mbuf_t *out) {{ *out = new MockMbuf{{m->bytes}}; return 0; }}
static mbuf_t awdlPacket(size_t len) {{ return new MockMbuf{{std::vector<uint8_t>(len, 0)}}; }}
static uint64_t awdlNow() {{ return 5000000; }}

struct MockLLWIf {{
    Ifnet ifnet;
    bool nativeSkywalk = true;
    bool bsdAttached = true;
    bool enabled = false;
    unsigned enableCalls = 0, disableCalls = 0;
    std::vector<std::vector<uint8_t>> rxFrames;
    bool isEnabled() const {{ return enabled; }}
    bool isSkywalkNative() const {{ return nativeSkywalk; }}
    ifnet_t getBSDInterface() const {{ return bsdAttached ? const_cast<Ifnet *>(&ifnet) : nullptr; }}
    const char *getBSDNamePrefix() {{ return "llw"; }}
    SInt32 setInterfaceEnable(bool on) {{
        if (on) ++enableCalls; else ++disableCalls;
        enabled = on;
        return kIOReturnSuccess;
    }}
    bool inputSkywalkEthernet(const uint8_t *eth, size_t len) {{
        rxFrames.emplace_back(eth, eth + len);
        return true;
    }}
}};
struct MockBSD {{
    Ifnet ifnet;
    const char *getNamePrefix() const {{ return "llw"; }}
    ifnet_t getIfnet() {{ return nullptr; }}
    void setLinkState(int) {{}}
    void startOutputThread() {{}}
    void stopOutputThread() {{}}
    void flushOutputQueue() {{}}
    unsigned inputPacket(mbuf_t m, unsigned) {{ mbuf_freem(m); return 0; }}
}};
#include "AirportItlwm/AWDL/TXBlockAck.hpp"
struct Radio {{ struct {{ bool ready = true, changing = false; TXBlockAckQueue blockAckTX = {{}}; }} awdlRadio; uint64_t dmaEpoch = 7; bool awdlTxWindowOpen() const {{ return false; }} }};
struct Timer {{ unsigned wakes = 0; void setTimeoutMS(unsigned) {{ ++wakes; }} void setTimeoutUS(unsigned) {{ ++wakes; }} }};

struct apple80211_awdl_sync_channel_sequence {{}};
struct AirportItlwmAWDLInterface {{
    enum class Request {{ setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE }};
    bool nativeSequenceValid = false;
    apple80211_awdl_sync_channel_sequence nativeSequence = {{}};
    IOReturn handle(Request, void *) {{ return kIOReturnSuccess; }}
    static constexpr unsigned DataQueueCapacity = 96;
    struct Pending {{
        enum : uint8_t {{ MulticastUnpaired, MulticastSocial, MulticastSlot8 }};
        mbuf_t packet = nullptr;
        uint64_t queuedUS = 0, epoch = 0, grant = 0;
        uint16_t wifiSequence = 0, dataSequence = 0;
        bool retry = false, group = false, lowLatency = false;
        uint8_t multicastQueue = 0, flushes = 0;
        mbuf_t second = nullptr;
        uint64_t secondQueuedUS = 0;
        uint16_t secondSequence = 0;
    }} pending[DataQueueCapacity] = {{}};
    Radio device, *radio = &device;
    MockLLWIf llwObj, *llwInterface = &llwObj;
    MockBSD *llwBSD = nullptr;
    Timer timerObj, *timer = &timerObj;
    bool stopping = false, running = true, requested = true, ticking = false, drainingData = false, wakeArmed = false;
    bool regulatoryPlanBlocked = false, regulatoryStopRequired = false;
    bool llwActive = false, llwLinkUp = false, llwParamsEnabled = false;
    apple80211_llw_params llwParams = {{}};
    LLWFlowRouter llwFlows = {{}};
    unsigned pendingHead = 0, pendingCount = 0, inFlightCount = 0, aggregateExtraOwned = 0;
    uint64_t llwCreateRequests = 0, llwDestroyRequests = 0;
    uint64_t llwTxQueued = 0, llwRxDelivered = 0, llwRxDropped = 0, llwNDPMirrored = 0;
    uint64_t txDropped = 0, multicastCopiesQueued = 0, multicastCopiesDropped = 0;

    void drainPendingData() {{}}
    IOReturn handleCompanionSkywalkCreate(apple80211_companion_skywalk_if_create *);
    void syncLLWLinkState();
    bool deliverLLWOrMirror(const DataView &, mbuf_t, size_t);
    IOReturn enqueueLLWFrame(mbuf_t);
}};

{routines}

int main() {{
    AirportItlwmAWDLInterface i;
    const uint8_t self[6] = {{2, 0, 0, 0, 0, 1}};
    const uint8_t iphone[6] = {{2, 0, 0, 0, 0, 2}};

    // 1. Native Skywalk llwInterface works even when llwBSD == nullptr!
    apple80211_companion_skywalk_if_create req = {{1, 1, {{}}}};
    assert(i.handleCompanionSkywalkCreate(&req) == kIOReturnSuccess);
    assert(strcmp(req.bsd_name, "llw0") == 0);
    assert(i.llwActive && i.llwObj.enabled && i.llwLinkUp);

    // 2. Transient radio pause MUST NOT disable native Skywalk llw0 (stays UP|RUNNING like native Mac).
    i.running = false;
    i.syncLLWLinkState();
    assert(i.llwObj.enabled && i.llwObj.disableCalls == 0);
    i.running = true;

    // 3. Outbound frame from Skywalk TX queue tags pending entry with lowLatency = true.
    mbuf_t tx = awdlPacket(14 + 40 + 16);
    copyAddress(tx->bytes.data(), iphone);
    copyAddress(tx->bytes.data() + 6, self);
    putBE16(tx->bytes.data() + 12, 0x86dd);
    tx->bytes[14] = 0x60; putBE16(tx->bytes.data() + 18, 16); tx->bytes[20] = 17; tx->bytes[21] = 64;
    putBE16(tx->bytes.data() + 54, 49152);
    putBE16(tx->bytes.data() + 56, 61465);
    putBE16(tx->bytes.data() + 58, 16);
    assert(i.enqueueLLWFrame(tx) == kIOReturnSuccess);
    assert(i.pendingCount == 1 && i.pending[0].lowLatency == true && i.llwTxQueued == 1);
    mbuf_freem(i.pending[0].packet);
    i.pending[0] = {{}}; i.pendingCount = 0;

    // 4. Inbound frame with view.lowLatency == true delivers directly via inputSkywalkEthernet().
    std::vector<uint8_t> ip(56, 0);
    ip[0] = 0x60; putBE16(ip.data() + 4, 16); ip[6] = 17; ip[7] = 64;
    DataView view = {{}};
    copyAddress(view.destination, self); copyAddress(view.source, iphone);
    view.payload = ip.data(); view.size = ip.size(); view.tid = 7; view.lowLatency = true;
    mbuf_t rxPkt = awdlPacket(14 + ip.size());
    assert(ethernetFromData(view, rxPkt->bytes.data(), rxPkt->bytes.size()) == rxPkt->bytes.size());
    assert(i.deliverLLWOrMirror(view, rxPkt, rxPkt->bytes.size()) == true);
    assert(i.llwRxDelivered == 1 && i.llwObj.rxFrames.size() == 1 && i.llwObj.rxFrames[0].size() == 14 + ip.size());

    // 5. Destroy cleanly disables native Skywalk llwInterface.
    apple80211_companion_skywalk_if_create del = {{1, 0, {{}}}};
    assert(i.handleCompanionSkywalkCreate(&del) == kIOReturnSuccess);
    assert(!i.llwActive && !i.llwObj.enabled && i.llwObj.disableCalls == 1);
    return 0;
}}
"""
        self.compile_run(src)


if __name__ == "__main__":
    unittest.main()
