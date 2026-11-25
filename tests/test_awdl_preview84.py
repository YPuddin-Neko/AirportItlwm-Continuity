"""Failing-first regressions for Preview 84: Skywalk LLW packet prepare and 80 MHz AWDL sequence promotion."""
import re
import unittest

import test_regressions as support
from test_offline import definition


class Preview84Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p84_llw_skywalk_packet_prepare_before_access(self):
        routines = "\n".join([
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmLLWInterface::txSubmissionAction"),
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmLLWInterface::inputSkywalkEthernet"),
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
    kIOReturnNoMemory = -2
}};

struct OSObject {{}};
struct IOSkywalkPacketQueue {{
    unsigned direction = 0;
    bool ready = true;
    bool isQueueReady() const {{ return ready; }}
}};
struct IOSkywalkTxSubmissionQueue : IOSkywalkPacketQueue {{
    IOSkywalkTxSubmissionQueue() {{ direction = 1; }}
}};
struct IOSkywalkRxCompletionQueue;
struct IOSkywalkTxCompletionQueue;

struct IOSkywalkPacket {{
    bool allocated = false;
    bool prepared = false;
    bool failPrepare = false;
    IOSkywalkPacketQueue *preparedQueue = nullptr;
    uint16_t dataOffset = 0;
    uint32_t dataLength = 0;
    uint8_t linkHeaderLen = 0;
    std::vector<uint8_t> storage;

    IOReturn prepare(IOSkywalkPacketQueue *queue, uint64_t = 0, unsigned int = 0) {{
        assert(allocated);
        if (failPrepare) return kIOReturnError;
        prepared = true;
        preparedQueue = queue;
        return kIOReturnSuccess;
    }}
    void *getDataVirtualAddress() const {{
        // Mirrors Sequoia IOSkywalkPacketBuffer::getMemorySegment() panic when unprepared.
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
    unsigned deallocated = 0;
    IOReturn allocatePacket(IOSkywalkPacket **out, unsigned int count) {{
        assert(count == 1);
        if (!nextPacket) return kIOReturnNoMemory;
        nextPacket->allocated = true;
        nextPacket->prepared = false;
        nextPacket->preparedQueue = nullptr;
        *out = nextPacket;
        nextPacket = nullptr;
        return kIOReturnSuccess;
    }}
    void deallocatePacket(IOSkywalkPacket *pkt) {{
        assert(pkt != nullptr && pkt->allocated);
        pkt->allocated = false;
        pkt->prepared = false;
        ++deallocated;
    }}
}};

struct IOSkywalkRxCompletionQueue : IOSkywalkPacketQueue {{
    bool failEnqueue = false;
    std::vector<IOSkywalkPacket *> enqueued;
    IOSkywalkRxCompletionQueue() {{ direction = 2; }}
    IOReturn enqueuePackets(IOSkywalkPacket * const *packets, unsigned int count, unsigned int) {{
        if (failEnqueue) return kIOReturnError;
        for (unsigned int i = 0; i < count; ++i) {{
            assert(packets[i]->prepared && packets[i]->preparedQueue == nullptr);
            enqueued.push_back(packets[i]);
        }}
        return kIOReturnSuccess;
    }}
}};

struct IOSkywalkTxCompletionQueue : IOSkywalkPacketQueue {{
    std::vector<IOSkywalkPacket *> completed;
    IOSkywalkTxCompletionQueue() {{ direction = 1; }}
    IOReturn enqueuePackets(IOSkywalkPacket * const *packets, unsigned int count, unsigned int) {{
        for (unsigned int i = 0; i < count; ++i) completed.push_back(packets[i]);
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

struct AirportItlwmLLWInterface : OSObject {{
    bool skywalkRegistered = true;
    bool enabledState = true;
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

    // 1. RX happy path: prepares packet with nullptr sourceQueue before touching buffer.
    std::vector<uint8_t> frame(70, 0x5a);
    frame[12] = 0x86; frame[13] = 0xdd;
    IOSkywalkNetworkPacket rxPkt;
    rxPkt.storage.assign(2048, 0);
    pool.nextPacket = &rxPkt;
    assert(llw.inputSkywalkEthernet(frame.data(), frame.size()) == true);
    assert(llw.skywalkRxPackets == 1 && llw.skywalkRxDropped == 0);
    assert(rxPkt.prepared && rxPkt.preparedQueue == nullptr);
    assert(rxPkt.dataOffset == 0 && rxPkt.dataLength == frame.size() && rxPkt.linkHeaderLen == 14);
    assert(memcmp(rxPkt.storage.data(), frame.data(), frame.size()) == 0);
    assert(rxComp.enqueued.size() == 1 && rxComp.enqueued[0] == &rxPkt);

    // 2. RX prepare failure: deallocates packet and drops without accessing buffer.
    IOSkywalkNetworkPacket rxFailPkt;
    rxFailPkt.storage.assign(2048, 0);
    rxFailPkt.failPrepare = true;
    pool.nextPacket = &rxFailPkt;
    assert(llw.inputSkywalkEthernet(frame.data(), frame.size()) == false);
    assert(llw.skywalkRxPackets == 1 && llw.skywalkRxDropped == 1 && pool.deallocated == 1);

    // 3. RX enqueue failure after prepare: deallocates packet and increments drop counter.
    IOSkywalkNetworkPacket rxEnqFailPkt;
    rxEnqFailPkt.storage.assign(2048, 0);
    pool.nextPacket = &rxEnqFailPkt;
    rxComp.failEnqueue = true;
    assert(llw.inputSkywalkEthernet(frame.data(), frame.size()) == false);
    assert(llw.skywalkRxPackets == 1 && llw.skywalkRxDropped == 2 && pool.deallocated == 2);
    rxComp.failEnqueue = false;

    // 4. TX submission path: prepares unprepared packet against txSubQueue before reading.
    IOSkywalkNetworkPacket txPkt;
    txPkt.allocated = true;
    txPkt.prepared = false;
    txPkt.storage.assign(2048, 0);
    txPkt.dataOffset = 16;
    txPkt.dataLength = uint32_t(frame.size());
    memcpy(txPkt.storage.data() + 16, frame.data(), frame.size());
    IOSkywalkNetworkPacket txFailPkt;
    txFailPkt.allocated = true;
    txFailPkt.prepared = false;
    txFailPkt.failPrepare = true;
    txFailPkt.storage.assign(2048, 0);
    IOSkywalkPacket *txBatch[2] = {{&txPkt, &txFailPkt}};
    assert(AirportItlwmLLWInterface::txSubmissionAction(&llw, &txSub, txBatch, 2, &llw) == 2);
    assert(txPkt.prepared && txPkt.preparedQueue == &txSub);
    assert(llw.skywalkTxPackets == 1 && llw.skywalkTxDropped == 1);
    assert(parent.queuedTx.size() == 1 && parent.queuedTx[0] == frame);
    assert(txComp.completed.size() == 2);
    return 0;
}}
"""
        self.compile_run(src)
        header = (support.ROOT / "include/Airport/macOS15/IOSkywalkSequoiaQueues.h").read_text()
        self.assertIn("IOReturn prepare(IOSkywalkPacketQueue *queue", header)

    def test_p84_awdl_channel_sequence_promotes_native_5ghz_to_80mhz(self):
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
        source = source.replace(
            "width==80&&channel==149&&allow80",
            "width==80&&(channel==149||channel==44)&&allow80",
        )
        extra = r"""
static void preview84WideSequencePromotion() {
    // Real Sequoia 24H32 sends encoding=1, pad1 low nibble 0 (default) or 3 (80 MHz),
    // and 5 GHz slots with flags 0x92 (ACTIVE|5GHZ|20MHZ) or 0x10 (5GHZ) without 0x400.
    AirportItlwmAWDLInterface i;
    i.device.ic.ic_state = IEEE80211_S_RUN;
    i.device.ic.ic_bss = &i.device.ap;
    i.device.allow80 = true;
    i.device.awdlRadio.ready = true;

    const unsigned channels[16] = {149,149,149,149,149,149,48,48,6,149,149,149,44,44,48,48};
    auto buildSeq = [&](uint8_t pad1, uint8_t encoding) {
        apple80211_awdl_sync_channel_sequence seq = {};
        seq.version = 1;
        seq.pad1 = pad1;
        seq.length = 16;
        seq.encoding = encoding;
        seq.step_count = 3;
        seq.fill_channel = 0xff;
        for (unsigned n = 0; n < 16; ++n) {
            const unsigned c = channels[n];
            const unsigned flags = c == 48 ? 0x294U : c == 6 ? 0x8aU : (n == 0 ? 0x10U : 0x92U);
            seq.seqs[n] = {1, c, flags};
        }
        return seq;
    };

    // 1. With en1 on ch 48, Preview 87 promotes disjoint non-infra ch 149 to OpClass 128
    //    while keeping overlapping ch 48, ch 44, and 2.4 GHz ch 6 at 20 MHz.
    for (uint8_t pad1 : {uint8_t(0x00), uint8_t(0x10), uint8_t(0x20), uint8_t(0x30), uint8_t(0x03), uint8_t(0x13), uint8_t(0x04)}) {
        auto seq = buildSeq(pad1, 1);
        assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq) == kIOReturnSuccess);
        apple80211_awdl_sync_channel_sequence read = {};
        assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &read) == kIOReturnSuccess);
        assert(!memcmp(&seq, &read, sizeof(read)));
        for (unsigned n = 0; n < 16; ++n) {
            assert(i.sync.channelClasses[n] == (channels[n] == 149 ? 128 : 0));
        }
    }

    // Explicit APPLE80211_C_FLAG_80MHZ slots select OpClass 128 on non-overlapping wide-capable channels
    // (when en1 is disconnected, both ch 149 and ch 44 are non-overlapping).
    i.device.ic.ic_state = IEEE80211_S_INIT;
    auto seq80 = buildSeq(0x00, 1);
    for (unsigned n = 0; n < 16; ++n) {
        if (channels[n] == 149 || channels[n] == 44)
            seq80.seqs[n].flags = APPLE80211_C_FLAG_ACTIVE | APPLE80211_C_FLAG_5GHZ | APPLE80211_C_FLAG_80MHZ;
        else if (channels[n] == 48)
            seq80.seqs[n].flags = APPLE80211_C_FLAG_ACTIVE | APPLE80211_C_FLAG_5GHZ | APPLE80211_C_FLAG_20MHZ;
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq80) == kIOReturnSuccess);
    i.device.ic.ic_state = IEEE80211_S_RUN;
    i.sync.restart(149, 1000000);
    {
        Window w = {};
        assert(i.sync.window(1005000, w));
        assert(w.channel == 149 && w.widthMHz() == 80);
        uint8_t frame[512] = {};
        const size_t size = i.sync.action(frame, sizeof(frame), 1005000, false, nullptr, 0);
        assert(size > 40);
        unsigned found = 0;
        for (size_t at = 40; at < size; at += 3 + le16(frame + at + 1)) {
            const uint8_t type = frame[at];
            const uint8_t *s = type == 4 ? frame + at + 36 : type == 18 ? frame + at + 3 : nullptr;
            if (!s) continue;
            ++found;
            for (unsigned j = 0; j < 16; ++j) {
                assert(s[6 + 2 * j] == channels[j]);
                const uint8_t expectedOp = channels[j] == 6 ? 81 : channels[j] == 48 ? 115 : 128;
                assert(s[7 + 2 * j] == expectedOp);
            }
        }
        assert(found == 2);
    }

    // 2. Explicit 20 MHz cap (pad1 low nibble 1) and 40 MHz cap (2) stay at 20 MHz.
    for (uint8_t pad1 : {uint8_t(0x01), uint8_t(0x11), uint8_t(0x02), uint8_t(0x22)}) {
        auto seq = buildSeq(pad1, 1);
        assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq) == kIOReturnSuccess);
        for (unsigned n = 0; n < 16; ++n) assert(i.sync.channelClasses[n] == 0);
    }

    // 3. encoding == 0 stays at 20 MHz.
    {
        auto seq = buildSeq(0x00, 0);
        assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq) == kIOReturnSuccess);
        for (unsigned n = 0; n < 16; ++n) assert(i.sync.channelClasses[n] == 0);
    }

    // 4. When regulatory disallows 80 MHz, native 0x92/0x10 sequences succeed cleanly at 20 MHz.
    i.device.allow80 = false;
    {
        auto seq = buildSeq(0x00, 1);
        assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq) == kIOReturnSuccess);
        for (unsigned n = 0; n < 16; ++n) assert(i.sync.channelClasses[n] == 0);
    }
}

int main() {
    preview84WideSequencePromotion();
    return 0;
}
"""
        self.compile_run(source + extra)


if __name__ == "__main__":
    unittest.main()
