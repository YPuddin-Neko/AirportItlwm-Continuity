"""Failing-first regressions for Preview 81 AirDrop QUIC RX, Sidecar queue, and llw0."""
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


class Preview81Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p81_airdrop_quic_nonzero_awdl_flags_and_unstripped_hw_amsdu(self):
        src = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include "AirportItlwm/AWDL/DataFrame.hpp"

using namespace AirportAWDL;
using Bytes = std::vector<uint8_t>;

static Bytes makeIPv6(uint8_t nextHeader, uint16_t srcPort, uint16_t dstPort, size_t bodyBytes) {
    const size_t payloadLen = 8 + bodyBytes;
    Bytes ip(40 + payloadLen, 0);
    ip[0] = 0x60;
    putBE16(ip.data() + 4, uint16_t(payloadLen));
    ip[6] = nextHeader;
    ip[7] = 64;
    ip[8] = 0xfe; ip[9] = 0x80; ip[23] = 2;
    ip[24] = 0xfe; ip[25] = 0x80; ip[39] = 1;
    putBE16(ip.data() + 40, srcPort);
    putBE16(ip.data() + 42, dstPort);
    putBE16(ip.data() + 44, uint16_t(payloadLen));
    for (size_t i = 0; i < bodyBytes; ++i) ip[48 + i] = uint8_t((i * 17) & 0xff);
    return ip;
}

static Bytes makeBody(uint16_t seq, uint16_t flags, const Bytes &ip) {
    Bytes body(16 + ip.size(), 0);
    memcpy(body.data(), SNAP, 8);
    putLE16(body.data() + 8, 0x0403);
    putLE16(body.data() + 10, seq);
    putLE16(body.data() + 12, flags);
    putBE16(body.data() + 14, 0x86dd);
    memcpy(body.data() + 16, ip.data(), ip.size());
    return body;
}

static Bytes makeQoSHeader(const uint8_t da[6], const uint8_t sa[6], uint8_t qos0, uint16_t seq) {
    Bytes hdr(26, 0);
    hdr[0] = 0x88;
    copyAddress(hdr.data() + 4, da);
    copyAddress(hdr.data() + 10, sa);
    copyAddress(hdr.data() + 16, BSSID);
    putLE16(hdr.data() + 22, uint16_t(seq << 4));
    hdr[24] = qos0;
    return hdr;
}

int main() {
    const uint8_t self[6] = {2, 0, 0, 0, 0, 1};
    const uint8_t peer[6] = {2, 0, 0, 0, 0, 2};
    const Bytes ip = makeIPv6(17, 50885, 50885, 1200);

    // 1. Modern AirDrop QUIC frames carrying non-zero AWDL data header flags (p[12..13]) must parse as Valid.
    for (uint16_t flags : {uint16_t(0x0010), uint16_t(0x0100), uint16_t(0x1230), uint16_t(0xffff)}) {
        Bytes frame = makeQoSHeader(self, peer, 0x00, 10);
        Bytes body = makeBody(42, flags, ip);
        frame.insert(frame.end(), body.begin(), body.end());
        DataBatch batch = {};
        assert(parseDataMPDU(frame.data(), frame.size(), batch, false) == ParseResult::Valid);
        assert(batch.count == 1);
        assert(batch.frames[0].sequence == 42);
        assert(batch.frames[0].flags == flags);
        assert(batch.frames[0].size == ip.size());
        assert(memcmp(batch.frames[0].payload, ip.data(), ip.size()) == 0);
    }

    // 2. Hardware-deaggregated direct-link QoS A-MSDU where hardware kept the 14-byte A-MSDU subframe header
    //    (both exact length and with 1..3 bytes of 4-byte alignment padding) must parse cleanly.
    for (size_t extraBody : {size_t(1199), size_t(1200), size_t(1201), size_t(1202)}) {
        Bytes quic = makeIPv6(17, 50885, 50885, extraBody);
        Bytes body = makeBody(77, 0x0020, quic);
        Bytes sub(14 + body.size(), 0);
        copyAddress(sub.data(), self);
        copyAddress(sub.data() + 6, peer);
        putBE16(sub.data() + 12, uint16_t(body.size()));
        memcpy(sub.data() + 14, body.data(), body.size());
        const size_t pad = (4 - (sub.size() % 4)) % 4;

        // 2a. Exact unstripped subframe with hardwareDeaggregated = true
        Bytes exact = makeQoSHeader(self, peer, 0x80 | 6, 11);
        exact.insert(exact.end(), sub.begin(), sub.end());
        DataBatch batch = {};
        assert(parseDataMPDU(exact.data(), exact.size(), batch, true) == ParseResult::Valid);
        assert(batch.count == 1 && batch.frames[0].sequence == 77 && batch.frames[0].tid == 6);
        assert(batch.frames[0].size == quic.size());

        // 2b. Unstripped subframe retaining non-last 4-byte A-MSDU alignment padding
        if (pad) {
            Bytes padded = exact;
            padded.insert(padded.end(), pad, 0);
            batch = {};
            assert(parseDataMPDU(padded.data(), padded.size(), batch, true) == ParseResult::Valid);
            assert(batch.count == 1 && batch.frames[0].sequence == 77 && batch.frames[0].tid == 6);
            assert(batch.frames[0].size == quic.size());
        }
    }

    // 3. Hardware-deaggregated subframe with stripped 14-byte header still works.
    {
        Bytes frame = makeQoSHeader(self, peer, 0x80 | 5, 12);
        Bytes body = makeBody(88, 0, ip);
        frame.insert(frame.end(), body.begin(), body.end());
        DataBatch batch = {};
        assert(parseDataMPDU(frame.data(), frame.size(), batch, true) == ParseResult::Valid);
        assert(batch.count == 1 && batch.frames[0].sequence == 88 && batch.frames[0].tid == 5);
    }

    // 4. Genuine unsupported SNAP or EtherType still returns Unsupported and records sanitized diagnostics.
    {
        Bytes bad = makeQoSHeader(self, peer, 0x00, 13);
        Bytes body = makeBody(99, 0x0000, ip);
        body[14] = 0x08; body[15] = 0x00; // IPv4 EtherType (unsupported)
        bad.insert(bad.end(), body.begin(), body.end());
        DataBatch batch = {};
        assert(parseDataMPDU(bad.data(), bad.size(), batch, false) == ParseResult::Unsupported);

        PeerTable peers = {};
        peers.peers[0].occupied = true;
        peers.peers[0].lastSeenUS = 1000000;
        copyAddress(peers.peers[0].address, peer);
        DataReceiveState rx = {};
        assert(rx.receive(bad.data(), bad.size(), peers, self, 1000000, false));
        assert(rx.unsupported == 1 && rx.valid == 0);
        assert(rx.lastUnsupported.valid);
        assert(rx.lastUnsupported.fc0 == 0x88 && rx.lastUnsupported.snapMatch);
        assert(rx.lastUnsupported.head == 0x0403 && rx.lastUnsupported.etherType == 0x0800);
    }
    return 0;
}
"""
        self.compile_run(src)

    def test_p81_sidecar_queue_96_batch_32_and_ch44_wide_discovery(self):
        awdl_hpp = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()
        self.assertRegex(awdl_hpp, r"DataQueueCapacity\s*=\s*96\b")

        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace("// NATIVE TX SCENARIOS", (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text())
        tick_src = tick_src.replace("// PRODUCTION", _native_tick_routines())
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", _native_tick_decls())
        tick_src = tick_src.replace("} pending[64]={};", "} pending[96]={};")
        tick_src = tick_src.replace("inFlight[64]={};", "inFlight[96]={};")
        tick_src += r"""
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

    // SyncEngine::discoverySchedule enables 80 MHz class 128 for channel 44 when wide44 is true.
    SyncEngine s = {};
    const uint8_t self[6] = {2, 0, 0, 0, 0, 1};
    s.start(self, 44, 1000000);
    s.discoverySchedule(true, true, false, false, true);
    bool sawWide44 = false;
    for (unsigned slot = 0; slot < 16; ++slot) {
        if (s.channels[slot] == 44) {
            assert(s.channelClasses[slot] == 128);
            sawWide44 = true;
        } else {
            assert(s.channelClasses[slot] == 0);
        }
    }
    assert(sawWide44);
    s.discoverySchedule(true, true, false, false, false);
    for (unsigned slot = 0; slot < 16; ++slot) assert(s.channelClasses[slot] == 0);
    return 0;
}
"""
        self.compile_run("#define DEBUG 0\n" + tick_src)

    def test_p81_llw_abi_flow_routing_and_control_requests(self):
        headers = (support.ROOT / "include/Airport/apple80211_var.h").read_text() + "\n" + (
            support.ROOT / "include/Airport/apple80211_ioctl.h"
        ).read_text()
        self.assertIn("APPLE80211_VIF_LOW_LATENCY", headers)
        self.assertIn("struct apple80211_companion_skywalk_if_create", headers)
        self.assertIn("struct apple80211_llw_params", headers)

        comp_decl = re.search(
            r"struct apple80211_companion_skywalk_if_create\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;",
            headers, re.S,
        )[0]
        llw_decl = re.search(
            r"struct apple80211_llw_params\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;",
            headers, re.S,
        )[0]

        src = f"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include "AirportItlwm/AWDL/LLWFlow.hpp"

{comp_decl}
{llw_decl}

static_assert(sizeof(apple80211_companion_skywalk_if_create) == 0x18, "companion create ABI");
static_assert(sizeof(apple80211_llw_params) == 0x2c, "llw params ABI");

using namespace AirportAWDL;

static void makeIPv6UDP(uint8_t *buf, uint16_t srcPort, uint16_t dstPort) {{
    memset(buf, 0, 56);
    buf[0] = 0x60;
    buf[4] = 0; buf[5] = 16;
    buf[6] = 17; // UDP
    buf[7] = 64;
    buf[40] = srcPort >> 8; buf[41] = srcPort & 0xff;
    buf[42] = dstPort >> 8; buf[43] = dstPort & 0xff;
    buf[44] = 0; buf[45] = 16;
}}

static void makeIPv6ICMP(uint8_t *buf) {{
    memset(buf, 0, 64);
    buf[0] = 0x60;
    buf[4] = 0; buf[5] = 24;
    buf[6] = 58; // ICMPv6 (NDP NS/NA)
    buf[7] = 255;
}}

int main() {{
    LLWFlowRouter router = {{}};
    const uint8_t iphone[6] = {{2, 0, 0, 0, 0, 2}};
    const uint8_t ipad[6]   = {{2, 0, 0, 0, 0, 3}};
    uint64_t now = 10000000;

    // Outbound UDP on llw0 (local 49152 -> remote 16402) records LLW flow.
    uint8_t txLLW[56];
    makeIPv6UDP(txLLW, 49152, 16402);
    router.noteTransmit(iphone, txLLW, sizeof(txLLW), true, now);

    // Outbound UDP on awdl0 (local 50885 -> remote 51000) records AWDL flow.
    uint8_t txAWDL[56];
    makeIPv6UDP(txAWDL, 50885, 51000);
    router.noteTransmit(iphone, txAWDL, sizeof(txAWDL), false, now);

    // Inbound reply to llw0 local port 49152 routes to LLW.
    uint8_t rxLLW[56];
    makeIPv6UDP(rxLLW, 16402, 49152);
    assert(router.classifyReceive(iphone, rxLLW, sizeof(rxLLW), 0, true, false, nullptr, now + 1000) == LLWRoute::LLW);

    // Inbound reply to awdl0 local port 50885 routes to AWDL.
    uint8_t rxAWDL[56];
    makeIPv6UDP(rxAWDL, 51000, 50885);
    assert(router.classifyReceive(iphone, rxAWDL, sizeof(rxAWDL), 0, true, false, nullptr, now + 1000) == LLWRoute::AWDL);

    // Inbound ICMPv6 NDP mirrors to Both while llw0 is active, AWDL only when inactive.
    uint8_t rxNDP[64];
    makeIPv6ICMP(rxNDP);
    assert(router.classifyReceive(iphone, rxNDP, sizeof(rxNDP), 0, true, false, nullptr, now + 1000) == LLWRoute::Both);
    assert(router.classifyReceive(iphone, rxNDP, sizeof(rxNDP), 0, false, false, nullptr, now + 1000) == LLWRoute::AWDL);

    // Unsolicited VI/VO UDP (TID 6, non-mDNS) from active LLW peer routes to LLW; mDNS always stays AWDL.
    uint8_t rxVideo[56];
    makeIPv6UDP(rxVideo, 16404, 49999);
    assert(router.classifyReceive(iphone, rxVideo, sizeof(rxVideo), 6, true, true, iphone, now + 1000) == LLWRoute::LLW);
    assert(router.classifyReceive(ipad, rxVideo, sizeof(rxVideo), 6, true, true, iphone, now + 1000) == LLWRoute::AWDL);
    assert(router.classifyReceive(iphone, rxVideo, sizeof(rxVideo), 0, true, true, iphone, now + 1000) == LLWRoute::AWDL);

    uint8_t rxMDNS[56];
    makeIPv6UDP(rxMDNS, 5353, 5353);
    assert(router.classifyReceive(iphone, rxMDNS, sizeof(rxMDNS), 6, true, true, iphone, now + 1000) == LLWRoute::AWDL);

    // Expired flow (>30s) falls back to AWDL.
    assert(router.classifyReceive(iphone, rxLLW, sizeof(rxLLW), 0, true, false, nullptr, now + 31000000) == LLWRoute::AWDL);
    return 0;
}}
"""
        self.compile_run(src)

    def test_p81_llw_production_control_and_rx_tx_bridge(self):
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
            for n in (
                "syncLLWLinkState",
                "handleCompanionSkywalkCreate",
                "handleGetLLWParams",
                "handleSetLLWParams",
                "deliverLLWOrMirror",
                "pullLLWOutputPackets",
            )
        )
        src = f"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
#include "AirportItlwm/AWDL/DataFrame.hpp"
#include "AirportItlwm/AWDL/LLWFlow.hpp"

using namespace AirportAWDL;
using IOReturn = int;
using SInt32 = int;
enum {{
    APPLE80211_VERSION = 1,
    kIOReturnSuccess = 0,
    kIOReturnBadArgument = 1,
    kIOReturnNotReady = 2,
    kIOReturnNoResources = 3,
    kIOReturnNoFrames = 4,
    kIO80211NetworkLinkDown = 1,
    kIO80211NetworkLinkUp = 2,
    MBUF_DONTWAIT = 1
}};

{comp_decl}
{llw_decl}

struct Packet {{
    std::vector<uint8_t> bytes;
    Packet *nextPkt = nullptr;
    explicit Packet(size_t n) : bytes(n, 0) {{}}
}};
using mbuf_t = Packet *;
static unsigned livePackets = 0;
static uint64_t nowClockUS = 20000000;
static uint64_t awdlNow() {{ return nowClockUS; }}
static mbuf_t awdlPacket(size_t n) {{ ++livePackets; return new Packet(n); }}
static void mbuf_freem(mbuf_t p) {{ assert(p && livePackets); --livePackets; delete p; }}
static void *mbuf_data(mbuf_t p) {{ return p->bytes.data(); }}
static size_t mbuf_pkthdr_len(mbuf_t p) {{ return p->bytes.size(); }}
static mbuf_t mbuf_nextpkt(mbuf_t p) {{ return p->nextPkt; }}
static void mbuf_setnextpkt(mbuf_t p, mbuf_t next) {{ p->nextPkt = next; }}
static int mbuf_copydata(mbuf_t p, size_t off, size_t len, void *dst) {{
    if (off + len > p->bytes.size()) return 1;
    memcpy(dst, p->bytes.data() + off, len);
    return 0;
}}
static int mbuf_dup(mbuf_t p, int, mbuf_t *out) {{
    auto copy = awdlPacket(p->bytes.size());
    copy->bytes = p->bytes;
    *out = copy;
    return 0;
}}

struct Ifnet {{ const char *name = "llw"; unsigned unit = 0; }};
using ifnet_t = Ifnet *;
static const char *ifnet_name(ifnet_t i) {{ return i->name; }}
static unsigned ifnet_unit(ifnet_t i) {{ return i->unit; }}

struct MockLLWIf {{
    bool enabled = false;
    bool isEnabled() const {{ return enabled; }}
    bool isSkywalkNative() const {{ return false; }}
    ifnet_t getBSDInterface() const {{ return nullptr; }}
    const char *getBSDNamePrefix() {{ return "llw"; }}
    bool inputSkywalkEthernet(const uint8_t *, size_t) {{ return false; }}
    SInt32 setInterfaceEnable(bool on) {{ enabled = on; return kIOReturnSuccess; }}
}};

struct MockBSD {{
    Ifnet ifnet;
    bool attached = true;
    bool linkUp = false;
    unsigned startThreads = 0, stopThreads = 0, flushes = 0;
    std::deque<mbuf_t> txQueue;
    std::vector<std::vector<uint8_t>> rxPackets;
    const char *getNamePrefix() const {{ return "llw"; }}
    ifnet_t getIfnet() {{ return attached ? &ifnet : nullptr; }}
    void setLinkState(int s) {{ linkUp = (s == kIO80211NetworkLinkUp); }}
    void startOutputThread() {{ ++startThreads; }}
    void stopOutputThread() {{ ++stopThreads; }}
    void flushOutputQueue() {{ ++flushes; }}
    IOReturn dequeueOutputPackets(unsigned limit, mbuf_t *head) {{
        if (txQueue.empty()) return kIOReturnNoFrames;
        mbuf_t first = nullptr, tail = nullptr;
        while (limit-- && !txQueue.empty()) {{
            mbuf_t p = txQueue.front(); txQueue.pop_front();
            if (!first) first = tail = p;
            else {{ tail->nextPkt = p; tail = p; }}
        }}
        *head = first;
        return kIOReturnSuccess;
    }}
    unsigned inputPacket(mbuf_t p, unsigned len) {{
        assert(p && len == p->bytes.size());
        rxPackets.push_back(p->bytes);
        mbuf_freem(p);
        return 1;
    }}
}};

#include "AirportItlwm/AWDL/TXBlockAck.hpp"
struct OSObject {{}};
struct WorkLoop {{ bool inGate() {{ return true; }} }};
struct Gate {{
    template <typename Fn>
    IOReturn runAction(Fn fn, void *self) {{ return fn(nullptr, self, nullptr, nullptr, nullptr); }}
}};
struct Radio {{
    WorkLoop wl;
    Gate gate;
    struct {{ bool ready = true; TXBlockAckQueue blockAckTX = {{}}; }} awdlRadio;
    uint64_t dmaEpoch = 7;
    WorkLoop *getMainWorkLoop() {{ return &wl; }}
    Gate *getMainCommandGate() {{ return &gate; }}
}};
struct Timer {{ unsigned wakes = 0; void setTimeoutMS(unsigned) {{ ++wakes; }} void setTimeoutUS(unsigned) {{ ++wakes; }} }};

struct apple80211_awdl_sync_channel_sequence {{}};
struct AirportItlwmAWDLInterface {{
    enum class Request {{ setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE }};
    bool nativeSequenceValid = false;
    apple80211_awdl_sync_channel_sequence nativeSequence = {{}};
    IOReturn handle(Request, void *) {{ return kIOReturnSuccess; }}
    Radio device, *radio = &device;
    MockLLWIf llwObj, *llwInterface = &llwObj;
    MockBSD llwBsdObj, *llwBSD = &llwBsdObj;
    Timer timerObj, *timer = &timerObj;
    bool stopping = false, running = true, requested = true, ticking = false, wakeArmed = false;
    bool regulatoryPlanBlocked = false, regulatoryStopRequired = false;
    bool llwActive = false, llwLinkUp = false, llwParamsConfigured = false, llwParamsEnabled = false;
    apple80211_llw_params llwParams = {{}};
    LLWFlowRouter llwFlows = {{}};
    uint64_t llwCreateRequests = 0, llwDestroyRequests = 0;
    uint64_t llwParamsGetCalls = 0, llwParamsSetCalls = 0;
    uint64_t llwTxQueued = 0, llwRxDelivered = 0, llwRxDropped = 0, llwNDPMirrored = 0;
    uint64_t txDropped = 0, multicastCopiesQueued = 0, multicastCopiesDropped = 0;
    struct Pending {{
        enum : uint8_t {{ MulticastUnpaired, MulticastSocial, MulticastSlot8 }};
        mbuf_t packet; uint64_t queuedUS, epoch;
        bool retry = false, lowLatency = false;
        uint8_t multicastQueue = 0;
    }} pending[96] = {{}};
    unsigned pendingHead = 0, pendingCount = 0, inFlightCount = 0, aggregateExtraOwned = 0;

    void syncLLWLinkState();
    IOReturn handleCompanionSkywalkCreate(apple80211_companion_skywalk_if_create *);
    IOReturn handleGetLLWParams(apple80211_llw_params *);
    IOReturn handleSetLLWParams(const apple80211_llw_params *);
    bool deliverLLWOrMirror(const DataView &, mbuf_t, size_t);
    IOReturn pullLLWOutputPackets();
}};

{routines}

int main() {{
    AirportItlwmAWDLInterface i;
    const uint8_t self[6] = {{2, 0, 0, 0, 0, 1}};
    const uint8_t iphone[6] = {{2, 0, 0, 0, 0, 2}};

    // 1. Create fails with NotReady when llwBSD is missing.
    i.llwBSD = nullptr;
    apple80211_companion_skywalk_if_create req = {{1, 1, {{}}}};
    assert(i.handleCompanionSkywalkCreate(&req) == kIOReturnNotReady);
    i.llwBSD = &i.llwBsdObj;
    i.llwBsdObj.attached = true;

    // 2. Create succeeds, populates "llw0", activates interface and output thread, and is idempotent.
    assert(i.handleCompanionSkywalkCreate(&req) == kIOReturnSuccess);
    assert(strcmp(req.bsd_name, "llw0") == 0);
    assert(i.llwActive && i.llwObj.enabled && i.llwBsdObj.linkUp && i.llwBsdObj.startThreads == 1);
    assert(i.llwCreateRequests == 1);
    memset(req.bsd_name, 0, sizeof(req.bsd_name));
    assert(i.handleCompanionSkywalkCreate(&req) == kIOReturnSuccess);
    assert(strcmp(req.bsd_name, "llw0") == 0 && i.llwCreateRequests == 2);

    // 3. Invalid action rejected.
    apple80211_companion_skywalk_if_create badReq = {{1, 2, {{}}}};
    assert(i.handleCompanionSkywalkCreate(&badReq) == kIOReturnBadArgument);

    // 4. LLW params set/get round-trip.
    apple80211_llw_params setP = {{}};
    setP.version = 1;
    copyAddress(setP.peer_mac, iphone);
    setP.mode = 7;
    setP.enable = 1;
    setP.slot_duration = 0x3c;
    setP.max_window = 0xff;
    assert(i.handleSetLLWParams(&setP) == kIOReturnSuccess);
    assert(i.llwParamsConfigured && i.llwParamsEnabled && i.llwParamsSetCalls == 1);
    apple80211_llw_params getP = {{}};
    assert(i.handleGetLLWParams(&getP) == kIOReturnSuccess);
    assert(memcmp(&setP, &getP, sizeof(setP)) == 0 && i.llwParamsGetCalls == 1);

    // 5. Outbound IPv6 UDP packet on llw0 registers flow and enqueues onto shared AWDL pending ring.
    mbuf_t tx = awdlPacket(14 + 40 + 16);
    copyAddress(tx->bytes.data(), iphone);
    copyAddress(tx->bytes.data() + 6, self);
    putBE16(tx->bytes.data() + 12, 0x86dd);
    tx->bytes[14] = 0x60; putBE16(tx->bytes.data() + 18, 16); tx->bytes[20] = 17; tx->bytes[21] = 64;
    putBE16(tx->bytes.data() + 54, 49152); // local port
    putBE16(tx->bytes.data() + 56, 16402); // remote port
    putBE16(tx->bytes.data() + 58, 16);
    i.llwBsdObj.txQueue.push_back(tx);
    assert(i.pullLLWOutputPackets() == kIOReturnSuccess);
    assert(i.pendingCount == 1 && i.llwTxQueued == 1);
    mbuf_freem(i.pending[0].packet);
    i.pending[0] = {{}};
    i.pendingCount = 0;

    // 6. Inbound UDP reply matching llw0 local port 49152 routes exclusively to llwBSD.
    std::vector<uint8_t> rxIp(56, 0);
    rxIp[0] = 0x60; putBE16(rxIp.data() + 4, 16); rxIp[6] = 17; rxIp[7] = 64;
    putBE16(rxIp.data() + 40, 16402); putBE16(rxIp.data() + 42, 49152); putBE16(rxIp.data() + 44, 16);
    DataView view = {{}};
    copyAddress(view.destination, self); copyAddress(view.source, iphone);
    view.payload = rxIp.data(); view.size = rxIp.size();
    mbuf_t rxPkt = awdlPacket(14 + rxIp.size());
    assert(ethernetFromData(view, rxPkt->bytes.data(), rxPkt->bytes.size()) == rxPkt->bytes.size());
    assert(i.deliverLLWOrMirror(view, rxPkt, rxPkt->bytes.size()) == true);
    assert(i.llwRxDelivered == 1 && i.llwBsdObj.rxPackets.size() == 1);

    // 7. Inbound ICMPv6 NDP mirrors to llwBSD and returns false so awdlBSD still receives original.
    std::vector<uint8_t> ndpIp(64, 0);
    ndpIp[0] = 0x60; putBE16(ndpIp.data() + 4, 24); ndpIp[6] = 58; ndpIp[7] = 255;
    DataView ndpView = {{}};
    copyAddress(ndpView.destination, self); copyAddress(ndpView.source, iphone);
    ndpView.payload = ndpIp.data(); ndpView.size = ndpIp.size();
    mbuf_t ndpPkt = awdlPacket(14 + ndpIp.size());
    assert(ethernetFromData(ndpView, ndpPkt->bytes.data(), ndpPkt->bytes.size()) == ndpPkt->bytes.size());
    assert(i.deliverLLWOrMirror(ndpView, ndpPkt, ndpPkt->bytes.size()) == false);
    assert(i.llwNDPMirrored == 1 && i.llwBsdObj.rxPackets.size() == 2);
    mbuf_freem(ndpPkt);

    // 8. Destroy cleanly deactivates llw0, stops output thread, clears flows.
    apple80211_companion_skywalk_if_create delReq = {{1, 0, {{}}}};
    assert(i.handleCompanionSkywalkCreate(&delReq) == kIOReturnSuccess);
    assert(!i.llwActive && !i.llwParamsEnabled && !i.llwObj.enabled && !i.llwBsdObj.linkUp);
    assert(i.llwDestroyRequests == 1 && i.llwBsdObj.stopThreads == 1 && i.llwBsdObj.flushes == 1);
    assert(livePackets == 0);
    return 0;
}}
"""
        self.compile_run(src)


if __name__ == "__main__":
    unittest.main()
