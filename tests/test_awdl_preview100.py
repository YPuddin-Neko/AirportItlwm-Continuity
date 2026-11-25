"""Failing-first regressions for Preview 100:
1. LLWFlowRouter preserves IEEE 1588 / gPTP UDP 319 (Sync/Delay_Req) and UDP 320 (Follow_Up) on
   awdl0 even when llwParamsEnabled is true, peerMAC == llwPeerMAC, and tid >= 4 (TID 6), while
   pullOutputPackets / pullLLWOutputPackets / enqueueLLWFrame priority-insert urgent llw0 and
   unicast PTP frames (including >62B 106-byte gPTP frames) ahead of MulticastSlot8 / bulk frames
   and preserve sub-ms wakeArmed slot timers across receiveBlockAck / completeUnicast.
2. Concurrent FaceTime + Sidecar (allowOnenessWombatCoex): setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE
   retains 80 MHz on multi-slot 149/44 runs when an active non-LLW unicast data peer (Sidecar iPad)
   coexists with the LLW peer (FaceTime iPhone) while still demoting standalone LLW to 20 MHz, and
   pullOutputPackets / serviceTXBlockAck / drainPendingData refresh heavy TX demand and unlock
   ADDBA + 48-frame / software-aggregate queueing while FaceTime heavyReceiveActive is true.
3. Inbound TID 7 (LLWTID) / active-LLW RXBA ADDBA completion no longer pre-allocates the single
   TID 0 blockAckTX engine to the Continuity Camera iPhone (preventing destructive TXBA handover
   DELBA teardowns of active camera RXBA), and serviceAWDLBlockAck proactively provisions and
   retains IWX_STA_LINK for active unicast data peers (lastUnicastDataUS < 5 s) when Sequoia leaves
   nativeCached == false while still removing idle stations after DELBA.
4. setAWDLTXWindow compares effective TLC SGI and flags masks instead of raw HT Capabilities bits,
   preventing 0x886f <-> 0x006f PSF/MIF alternation and 149@80 -> 6@20 transitions from firing
   hundreds of spurious IWX_TLC_MNG_CONFIG_CMD resets that lock QuZ TLC at 26 Mbps.
"""
import re
import unittest

import test_awdl_preview82 as p82
import test_awdl_preview91 as p91
import test_regressions as support
from test_offline import definition


class Preview100Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p100_llw_flow_router_preserves_gptp_319_320_on_awdl0_and_prioritizes_urgent_tx(self):
        # Part A: LLWFlowRouter MUST keep both UDP 319 (Sync) and UDP 320 (Follow_Up) on awdl0
        # even when Mac only transmits UDP 319 (Delay_Req) and iPhone sends Follow_Up with tid=6.
        llw_src = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include "AirportItlwm/AWDL/LLWFlow.hpp"

using namespace AirportAWDL;

static void makeIPv6UDP(uint8_t (&pkt)[64], uint16_t srcPort, uint16_t dstPort) {
    std::memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x60;
    pkt[4] = 0; pkt[5] = 24;
    pkt[6] = 17; // UDP
    pkt[7] = 255;
    pkt[8] = 0xfe; pkt[9] = 0x80; pkt[23] = 0x11;
    pkt[24] = 0xfe; pkt[25] = 0x80; pkt[39] = 0x22;
    pkt[40] = uint8_t(srcPort >> 8); pkt[41] = uint8_t(srcPort & 0xff);
    pkt[42] = uint8_t(dstPort >> 8); pkt[43] = uint8_t(dstPort & 0xff);
    pkt[44] = 0; pkt[45] = 24;
}

int main() {
    LLWFlowRouter router = {};
    const uint8_t iphone[6] = {2, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint64_t now = 5000000ULL;

    // Mac IOTimeSync Slave sends Delay_Req from UDP 319 -> 319 on awdl0 (never sends from 320!).
    uint8_t delayReq[64];
    makeIPv6UDP(delayReq, 319, 319);
    router.noteTransmit(iphone, delayReq, sizeof(delayReq), false, now);

    // 1. Inbound gPTP Sync (319 -> 319, tid=6) and Follow_Up (320 -> 320, tid=6) from llwPeerMAC
    //    MUST BOTH stay on LLWRoute::AWDL so IOTimeSyncUnicastUDPv6EtEPort(awdl0) never times out!
    uint8_t syncPkt[64], followUpPkt[64], rtpPkt[64];
    makeIPv6UDP(syncPkt, 319, 319);
    makeIPv6UDP(followUpPkt, 320, 320);
    makeIPv6UDP(rtpPkt, 16402, 16402);

    assert(router.classifyReceive(iphone, syncPkt, sizeof(syncPkt), 6,
                                  true, true, iphone, now + 500) == LLWRoute::AWDL);
    assert(router.classifyReceive(iphone, followUpPkt, sizeof(followUpPkt), 6,
                                  true, true, iphone, now + 900) == LLWRoute::AWDL);
    // Ephemeral srcPort -> dstPort 320 Follow_Up variant MUST also stay on AWDL:
    makeIPv6UDP(followUpPkt, 55123, 320);
    assert(router.classifyReceive(iphone, followUpPkt, sizeof(followUpPkt), 6,
                                  true, true, iphone, now + 1000) == LLWRoute::AWDL);

    // 2. Unmatched Continuity Camera media UDP (tid=7) from llwPeerMAC still routes to LLW:
    assert(router.classifyReceive(iphone, rtpPkt, sizeof(rtpPkt), 7,
                                  true, true, iphone, now + 1000) == LLWRoute::LLW);
    return 0;
}
"""
        self.compile_run(llw_src)

        # Part B: Urgent >62B unicast gPTP (106-byte Delay_Req) + llw0 RTCP bypass queued
        # MulticastSlot8 / bulk frames and transmit immediately even when psfDue holds bulk TX;
        # receiveBlockAck preserves sub-ms wakeArmed slot timers.
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        extra_routines = "\n".join(
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
            for n in ("drainPendingData", "enqueueLLWFrame")
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines() + "\n" + extra_routines)
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src = tick_src.replace(
            "static void txReady(void *);",
            "static void txReady(void *);\n    IOReturn enqueueLLWFrame(mbuf_t);",
        )
        tick_src += r"""
static mbuf_t makeMulticastIPv6(const uint8_t *src) {
    mbuf_t p = awdlPacket(86);
    auto *b = p->bytes.data();
    b[0] = 0x33; b[1] = 0x33; b[2] = 0; b[3] = 0; b[4] = 0; b[5] = 0x01;
    copyAddress(b + 6, src);
    putBE16(b + 12, 0x86dd);
    b[14] = 0x60;
    putBE16(b + 18, 32);
    b[20] = 17; b[21] = 255;
    b[22] = 0xfe; b[23] = 0x80; b[37] = 1;
    b[38] = 0xff; b[39] = 0x02; b[53] = 1;
    putBE16(b + 54, 5000); putBE16(b + 56, 5000); putBE16(b + 58, 32);
    return p;
}

static mbuf_t makeGPTP106(const uint8_t *src, const uint8_t *dst, uint16_t port) {
    const size_t total = 106; // 14 eth + 40 ipv6 + 8 udp + 44 ptp (> 62-byte header copy!)
    mbuf_t p = awdlPacket(total);
    auto *b = p->bytes.data();
    copyAddress(b, dst);
    copyAddress(b + 6, src);
    putBE16(b + 12, 0x86dd);
    b[14] = 0x60;
    putBE16(b + 18, uint16_t(total - 54));
    b[20] = 17; b[21] = 255;
    b[22] = 0xfe; b[23] = 0x80; b[37] = 1;
    b[38] = 0xfe; b[39] = 0x80; b[53] = 2;
    putBE16(b + 54, port);
    putBE16(b + 56, port);
    putBE16(b + 58, uint16_t(total - 54));
    b[105] = 0x77;
    return p;
}

static mbuf_t makeBulk(const uint8_t *src, const uint8_t *dst, size_t total, uint8_t tag) {
    mbuf_t p = awdlPacket(total);
    auto *b = p->bytes.data();
    copyAddress(b, dst);
    copyAddress(b + 6, src);
    putBE16(b + 12, 0x86dd);
    b[14] = 0x60;
    putBE16(b + 18, uint16_t(total - 54));
    b[20] = 17; b[21] = 64;
    b[22] = 0xfe; b[23] = 0x80; b[37] = 1;
    b[38] = 0xfe; b[39] = 0x80; b[53] = 2;
    putBE16(b + 54, 51000); putBE16(b + 56, 51000);
    putBE16(b + 58, uint16_t(total - 54));
    b[total - 1] = tag;
    return p;
}

int main() {
    clockUS = 10000000;
    const uint8_t iphone[6] = {2, 0, 0, 0, 0, 9};
    const uint8_t ipad[6] = {2, 0, 0, 0, 0, 10};
    AirportItlwmAWDLInterface i;
    dataPeer(i, iphone);
    dataPeer(i, ipad);
    for (auto &c : i.sync.channels) c = 44;
    i.llwActive = true;
    i.llwParamsEnabled = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.llwQueueConfigured = true;
    i.device.awdlRadio.llwQueue = 3;
    i.tick();
    clockUS += 4000;
    i.tick();

    // Stage Multicast (creates MulticastSocial + MulticastSlot8) + 3 bulk Sidecar frames while
    // psfDue(now) is TRUE (nextActionUS = clockUS), holding non-urgent data at the head of pending[]!
    i.nextActionUS = clockUS;
    i.nextMIFUS = clockUS + 5000000;
    i.network.output.push_back(makeMulticastIPv6(i.address.octet));
    i.network.output.push_back(makeBulk(i.address.octet, ipad, 1200, 1));
    i.network.output.push_back(makeBulk(i.address.octet, ipad, 1200, 2));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    assert(i.pendingCount == 4); // MulticastSocial, MulticastSlot8, bulk1, bulk2
    const unsigned dataBefore = i.device.data;

    // Now IOTimeSync queues a 106-byte unicast gPTP Delay_Req (UDP 319) on awdl0:
    i.network.output.push_back(makeGPTP106(i.address.octet, iphone, 319));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    i.drainPendingData();
    // Urgent 106B gPTP MUST have jumped ahead of non-urgent frames and transmitted immediately!
    assert(i.device.data == dataBefore + 1);
    assert(i.device.dataFrames.back().back() == 0x77);

    // And an urgent llw0 RTCP NACK via enqueueLLWFrame MUST also jump ahead and transmit immediately!
    assert(i.enqueueLLWFrame(makeBulk(i.address.octet, iphone, 96, 0x99)) == kIOReturnSuccess);
    assert(i.device.data == dataBefore + 2);
    assert(i.device.dataFrames.back().back() == 0x99);

    // Sub-ms wakeArmed slot timer (450 us) MUST NOT be clobbered to 1000 us by receiveBlockAck!
    i.scheduler.setTimeoutUS(450);
    i.wakeArmed = true;
    i.device.loop.gated = true;
    BlockAckRequest req = {};
    copyAddress(req.peer, iphone);
    req.tid = 7; req.dialog = 1; req.window = 64; req.immediate = true;
    AirportItlwmAWDLInterface::receiveBlockAck(&i, req);
    assert(i.scheduler.delayUS == 450);

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)

    def test_p100_oneness_wombat_coex_preserves_80mhz_sidecar_and_unblocks_txba_under_llw_rx(self):
        # Part A: allowOnenessWombatCoex keeps 80 MHz on multi-slot 149 when an active non-LLW
        # unicast data peer (Sidecar iPad) coexists with the LLW peer (FaceTime iPhone), while
        # still demoting standalone LLW (even with an idle ATV in awdlPeers) to 20 MHz.
        ctrl_src = (support.ROOT / "tests/fixtures/awdl_control.cpp").read_text().rsplit("int main()", 1)[0]
        handle = "\n".join(
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + name)
            for name in ("handle", "handleSetLLWParams", "updatePeerCache", "configureBroadcast", "broadcastDue")
        )
        headers = {
            name: (support.ROOT / "include/Airport" / name).read_text()
            for name in ("apple80211_var.h", "apple80211_ioctl.h")
        }
        names = ["apple80211_channel", "awdl_peer_advert_add"] + sorted(
            set(re.findall(r"\bapple80211_\w+", handle)) | {"apple80211_awdl_af_tx_mode"}
        )
        decls = []
        for name in names:
            pattern = r"struct " + name + r"\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;"
            matches = [re.search(pattern, header, re.S) for header in headers.values()]
            decl = next((m[0] for m in matches if m), None)
            if decl is None:
                raise ValueError("Missing native declaration: " + name)
            decls.append(decl)
        interface = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()
        request_enum = re.search(r"enum class Request\s*\{.*?\};", interface, re.S)[0]
        decls.append(
            re.search(r"^#define APPLE80211_M_PEER_CACHE_CONTROL\s+\d+", headers["apple80211_var.h"], re.M)[0]
        )
        ctrl_src = ctrl_src.replace("// NATIVE DECLARATIONS", "\n".join(decls))
        ctrl_src = ctrl_src.replace("// NATIVE REQUESTS", request_enum)
        ctrl_src = ctrl_src.replace(
            "IOReturn handleSetLLWParams(const apple80211_llw_params *) { return kIOReturnSuccess; }",
            "IOReturn handleSetLLWParams(const apple80211_llw_params *);",
        )
        ctrl_src = ctrl_src.replace("// PRODUCTION", handle)
        ctrl_src += r"""
int main() {
    AirportItlwmAWDLInterface i;
    i.device.ic.ic_state = IEEE80211_S_RUN;
    i.device.ic.ic_bss = &i.device.ap;
    i.device.apChannel.channel = 48;
    i.device.allow80 = true;
    i.device.awdlRadio.ready = true;

    const uint8_t iphone[6] = {2, 0, 0, 0, 0, 11};
    const uint8_t ipad[6]   = {2, 0, 0, 0, 0, 22};
    const uint8_t idleAtv[6]= {2, 0, 0, 0, 0, 33};

    // Populate awdlPeers with iPhone + an idle Apple TV (lastUnicastDataUS == 0):
    auto &pPhone = i.device.awdlPeers.peers[0];
    pPhone.occupied = true; copyAddress(pPhone.address, iphone);
    pPhone.lastSeenUS = awdlNow(); pPhone.lastUnicastDataUS = awdlNow() - 10000;
    pPhone.metadata.fields = HTCapabilities; pPhone.metadata.htCapabilities = 0x886f;
    pPhone.htCapabilitiesReceivedUS = awdlNow();

    auto &pAtv = i.device.awdlPeers.peers[1];
    pAtv.occupied = true; copyAddress(pAtv.address, idleAtv);
    pAtv.lastSeenUS = awdlNow(); pAtv.lastUnicastDataUS = 0; // Idle!
    pAtv.metadata.fields = HTCapabilities; pAtv.metadata.htCapabilities = 0x886f;
    pAtv.htCapabilitiesReceivedUS = awdlNow();

    const unsigned sidecar[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    apple80211_awdl_sync_channel_sequence scSeq = {};
    scSeq.version = 1; scSeq.length = 16; scSeq.encoding = 1; scSeq.step_count = 3; scSeq.fill_channel = 0xff;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = sidecar[n];
        scSeq.seqs[n] = {1, c, c == 48 ? 0x294U : (c == 6 ? 0x8aU : 0x92U)};
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &scSeq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == (sidecar[n] == 149 ? 128 : 0));

    // 1. Standalone FaceTime (no active non-LLW unicast data peer; ATV is idle):
    //    setLLW_PARAMS(enable=1, peer_mac=iphone) keeps 80 MHz on non-overlapping ch 149 while clamping ch 48 to 20 MHz!
    apple80211_llw_params llw = {};
    llw.version = 1; llw.enable = 1;
    copyAddress(llw.peer_mac, iphone);
    assert(i.handle(Request::setLLW_PARAMS, &llw) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == (sidecar[n] == 149 ? 128 : 0));

    // 2. Concurrent Sidecar + FaceTime (allowOnenessWombatCoex):
    //    Sidecar iPad has active unicast data (lastUnicastDataUS within 5 s).
    //    Both starting FaceTime while Sidecar is active AND installing coex sequence while
    //    FaceTime is active MUST preserve 80 MHz on all 11 multi-slot ch 149 slots!
    llw.enable = 0;
    assert(i.handle(Request::setLLW_PARAMS, &llw) == kIOReturnSuccess);
    auto &pPad = i.device.awdlPeers.peers[2];
    pPad.occupied = true; copyAddress(pPad.address, ipad);
    pPad.lastSeenUS = awdlNow(); pPad.lastUnicastDataUS = awdlNow() - 15000;

    llw.enable = 1;
    assert(i.handle(Request::setLLW_PARAMS, &llw) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == (sidecar[n] == 149 ? 128 : 0));

    // While isolated 1-slot RealTimeGroup ch 149 visits and overlapping ch 48 stay 20 MHz (0):
    const unsigned rtg[16] = {48,48,149,48,48,48,48,48,48,48,149,48,48,48,48,48};
    apple80211_awdl_sync_channel_sequence rtgSeq = scSeq;
    for (unsigned n = 0; n < 16; ++n) {
        const unsigned c = rtg[n];
        rtgSeq.seqs[n] = {1, c, c == 48 ? 0x294U : 0x92U};
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &rtgSeq) == kIOReturnSuccess);
    for (unsigned n = 0; n < 16; ++n)
        assert(i.sync.channelClasses[n] == 0);

    return 0;
}
"""
        self.compile_run(ctrl_src)

        # Part B: Staging >=512B Sidecar frames in pullOutputPackets / serviceTXBlockAck refreshes
        # lastHeavyDataUS to unblock ADDBA Request even while FaceTime heavyReceiveActive is true,
        # and drainPendingData does not clamp limit=4U if deferReceiveProbe is active.
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        extra_routines = definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmAWDLInterface::drainPendingData",
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines() + "\n" + extra_routines)
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src += r"""
static mbuf_t makeSizedFrame(const uint8_t *src, const uint8_t *dst, size_t total) {
    mbuf_t p = awdlPacket(total);
    auto *b = p->bytes.data();
    copyAddress(b, dst);
    copyAddress(b + 6, src);
    putBE16(b + 12, 0x86dd);
    b[14] = 0x60;
    putBE16(b + 18, uint16_t(total - 54));
    b[20] = 17; b[21] = 64;
    b[22] = 0xfe; b[23] = 0x80; b[37] = 1;
    b[38] = 0xfe; b[39] = 0x80; b[53] = 2;
    putBE16(b + 54, 52000); putBE16(b + 56, 52000);
    putBE16(b + 58, uint16_t(total - 54));
    return p;
}

int main() {
    clockUS = 10000000;
    const uint8_t ipad[6] = {2, 0, 0, 0, 0, 22};
    AirportItlwmAWDLInterface i;
    dataPeer(i, ipad);
    for (auto &c : i.sync.channels) c = 44;
    i.device.awdlRadio.listener = true;
    i.tick();
    clockUS += 4000;
    i.tick();
    i.nextActionUS = i.nextMIFUS = clockUS + 5000000;

    // FaceTime Continuity Camera RX is actively delivering >=512B frames (heavyReceiveActive == true),
    // while TXBlockAck for iPad is in Asking with lastHeavyDataUS == 0 (stale across off-channel gap):
    auto &r = i.device.awdlRadio;
    auto &tx = r.blockAckTX;
    r.lastRXPayloadUS = clockUS - 4000;
    r.lastHeavyRXPayloadUS = clockUS - 4000;
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Asking;
    copyAddress(tx.peer, ipad);
    tx.dialog = 1;
    tx.nextAskUS = 0;
    tx.lastHeavyDataUS = 0;
    assert(r.deferReceiveProbe(clockUS));

    // 1. Even with deferReceiveProbe == true and small (<512B) frames, drainPendingData MUST NOT
    //    clamp txq queued limit to 4U (must allow up to 48U)!
    i.device.com.txq[r.queue].queued = 4; // Would hit limit=4U in P99!
    for (unsigned n = 0; n < 6; ++n)
        i.network.output.push_back(makeSizedFrame(i.address.octet, ipad, 120));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    const unsigned beforeSmall = i.device.data;
    i.drainPendingData();
    assert(i.device.data == beforeSmall + 6);
    i.device.com.txq[r.queue].queued = 0;

    // 2. When Sidecar queues >=512B (1200B) video frames, serviceTXBlockAck refreshes
    //    lastHeavyDataUS from pending[] so deferReceiveProbe clears and ADDBA Request is sent!
    i.network.output.push_back(makeSizedFrame(i.address.octet, ipad, 1200));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    i.serviceTXBlockAck();
    assert(!r.deferReceiveProbe(clockUS));
    assert(r.blockAckTXStats.requests == 1 && tx.managementToken != 0);
    i.completeUnicast(tx.managementToken, true);

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)

    def test_p100_tid7_rxba_does_not_steal_tid0_txba_and_active_data_peers_retain_sta_link(self):
        # Part A: Completing ADDBA Response for TID 7 (LLWTID) or while llwParamsEnabled is true
        # MUST NOT pre-allocate TID 0 blockAckTX to the camera peer, while TID 0 with !llwParamsEnabled still does.
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines())
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src += r"""
int main() {
    clockUS = 10000000;
    const uint8_t iphone[6] = {2, 0, 0, 0, 0, 11};
    const uint8_t airdropPeer[6] = {2, 0, 0, 0, 0, 22};
    AirportItlwmAWDLInterface i;
    dataPeer(i, iphone);
    dataPeer(i, airdropPeer);
    for (auto &c : i.sync.channels) c = 44;
    i.device.baResult = 0;
    i.device.awdlRadio.listener = true;
    i.tick();
    clockUS += 4000;
    i.tick();
    i.nextActionUS = i.nextMIFUS = clockUS + 5000000;

    // 1. iPhone negotiates inbound TID 7 (LLWTID) RXBA for Continuity Camera before first heavy MPDU:
    i.device.loop.gated = true;
    BlockAckRequest req7 = {};
    copyAddress(req7.peer, iphone);
    req7.tid = RadioState::LLWTID; // TID 7!
    req7.dialog = 1; req7.window = 64; req7.immediate = true;
    AirportItlwmAWDLInterface::receiveBlockAck(&i, req7);
    i.serviceBlockAck();
    assert(!i.device.tokens.empty());
    const uint64_t tok7 = i.device.tokens.back();
    i.completeUnicast(tok7, true);
    // TID 0 blockAckTX MUST remain Idle (NOT stolen by iPhone TID 7)!
    assert(i.device.awdlRadio.blockAckTX.phase == TXBlockAckQueue::Idle);

    // 2. Standard TID 0 RXBA when !llwParamsEnabled STILL pre-allocates blockAckTX:
    ++i.device.baCookie;
    BlockAckRequest req0 = {};
    copyAddress(req0.peer, airdropPeer);
    req0.tid = RadioState::DataTID; // TID 0
    req0.dialog = 2; req0.window = 64; req0.immediate = true;
    AirportItlwmAWDLInterface::receiveBlockAck(&i, req0);
    i.serviceBlockAck();
    const uint64_t tok0 = i.device.tokens.back();
    i.completeUnicast(tok0, true);
    assert(i.device.awdlRadio.blockAckTX.phase == TXBlockAckQueue::Requested);
    assert(sameAddress(i.device.awdlRadio.blockAckTX.peer, airdropPeer));

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)

        # Part B: ItlIwx::serviceAWDLBlockAck proactively provisions and retains IWX_STA_LINK across
        # DELBA transitions for active unicast data peers (lastUnicastDataUS < 5 s) when nativeCached == false,
        # while still removing idle (lastUnicastDataUS == 0 or >= 5 s stale) stations after DELBA.
        extra_ba = r"""
int main() {
    ItlIwx d;
    auto r = request(d, 0, 6, 100);
    auto &peer = d.awdlPeers.peers[0];
    peer.nativeCached = false; // Live Sequoia: NativeCachedPeers == 0!
    peer.lastUnicastDataUS = nowUS - 50000; // Active unicast gPTP/Sidecar data 50 ms ago

    // 1. Before any RXBA session is negotiated, serviceAWDLBlockAck MUST proactively install
    //    IWX_STA_LINK so MacID 1 on Channel 48 (IN_PROMISC == 0) hardware-matches unicast frames:
    d.serviceAWDLBlockAck();
    assert(d.awdlBA.peers[0].station && sameAddress(d.awdlBA.peers[0].address, r.peer));
    assert(d.removes.empty());

    // 2. Start RXBA on TID 6, then tear it down via DELBA while lastUnicastDataUS is still fresh:
    //    RXBA TID MUST be removed, while IWX_STA_LINK remains installed (0 REMOVE_STA churn)!
    unsigned b = start(d, r);
    assert(d.com.sc_rx_ba_sessions == 1);
    auto end = r; end.teardown = true;
    assert(d.receiveAWDLBlockAckEnd(end));
    d.serviceAWDLBlockAck();
    assert(d.com.sc_rx_ba_sessions == 0);
    assert(d.awdlBA.peers[0].station && d.removes.empty());

    // 3. Once unicast data has been idle >= 5 s (even if idle PSFs keep lastSeenUS fresh),
    //    serviceAWDLBlockAck MUST cleanly remove the station:
    nowUS += 5000001ULL;
    peer.lastSeenUS = nowUS;
    d.serviceAWDLBlockAck();
    assert(!d.awdlBA.peers[0].station && d.removes.size() == 1);
    return 0;
}
"""
        self.compile_run(p91._build_ba_harness(extra_ba))

    def test_p100_effective_tlc_cap_comparison_prevents_htcap_alternation_tlc_resets(self):
        extra_ba = r"""
int main() {
    ItlIwx d;
    auto r = txPeer(d);
    d.tlcVersion = 4;
    d.awdlRadio.channel = 149;
    d.awdlRadio.widthMHz = 80;
    d.mimo = true; d.txAnt = 3; d.rxAnt = 3;
    d.com.config.non_shared_ant = 2;
    d.btCoex.enabled = true; d.btCoex.txMask = 3;

    auto &p = d.awdlPeers.peers[0];
    p.metadata.fields |= AirportAWDL::HTCapabilities;
    p.metadata.rxHTMcs = 0xff;
    p.metadata.rxHTMcs2 = 0xff;
    // Live iOS PSF HT Cap = 0x886f (34927: MaxAMSDU | LSIG_TXOP | SGI40 | SGI20 | HT40 | LDPC)
    p.metadata.htCapabilities = 0x886f;
    p.htCapabilitiesReceivedUS = nowUS;
    p.haveSync = p.haveSequence = true;
    p.syncReceivedUS = p.sequenceReceivedUS = nowUS;
    p.sync.presenceMode = 4; p.sync.windowPeriodTU = 16; p.sync.nextWindowTU = 64;
    p.sequence.count = 16; p.sequence.fill = 0xffff; p.sequence.step = 3; p.sequence.encoding = 3;
    for (unsigned s = 0; s < 16; ++s) { p.sequence.channel[s] = 149; p.sequence.flags[s] = 128; }

    prepareTX(d, r);
    agreeTX(d);
    uint64_t end = nowUS + 30000;
    assert(!d.setAWDLTXWindow(grant, &end));
    auto &tx = d.awdlRadio.blockAckTX;
    assert(tx.widthMHz == 80 && !tx.paused);
    assert(d.awdlRadio.blockAckTXStats.tlcConfigs == 1);

    // 1. Alternate 20 times between PSF htCapabilities = 0x886f (34927) and MIF htCapabilities = 0x006f (111):
    //    effective sgiMask (0x07) and tlcFlags (LDPC) are IDENTICAL -> ZERO TLC_MNG_CONFIG_CMD resets!
    for (unsigned step = 0; step < 20; ++step) {
        nowUS += 10000;
        p.lastSeenUS = p.syncReceivedUS = p.sequenceReceivedUS = p.htCapabilitiesReceivedUS = nowUS;
        p.metadata.htCapabilities = (step % 2 == 0) ? 0x006f : 0x886f;
        end = nowUS + 30000;
        assert(!d.setAWDLTXWindow(grant, &end));
        assert(!tx.paused && tx.widthMHz == 80);
        assert(d.awdlRadio.blockAckTXStats.tlcConfigs == 1);
    }

    // 2. Shrink 149@80 (0x886f) -> downgradeAWDLTXRates(20, 6) -> open window on 6@20 with MIF 0x006f:
    //    downgradeAWDLTXRates issues 1 TLC config (tlcConfigs == 2); setAWDLTXWindow on ch 6 MUST NOT
    //    issue a duplicate 3rd TLC config just because raw htCap changed 0x886f -> 0x006f!
    assert(!d.closeAWDLListenerWindow() && tx.paused);
    assert(!d.downgradeAWDLTXRates(20, 6));
    assert(d.awdlRadio.blockAckTXStats.tlcConfigs == 2);
    d.awdlRadio.channel = 6;
    d.awdlRadio.widthMHz = 20;
    for (unsigned s = 0; s < 16; ++s) { p.sequence.channel[s] = 6; p.sequence.flags[s] = 81; }
    p.metadata.htCapabilities = 0x006f;
    end = nowUS + 30000;
    assert(!d.setAWDLTXWindow(grant, &end) && !tx.paused);
    assert(d.awdlRadio.blockAckTXStats.tlcConfigs == 2);

    // 3. Genuine TLC capability change (0x006f -> 0x0020, dropping LDPC) STILL reconfigures TLC:
    p.metadata.htCapabilities = 0x0020;
    assert(!d.setAWDLTXWindow(grant, &end) && !tx.paused);
    assert(d.awdlRadio.blockAckTXStats.tlcConfigs == 3 && tx.tlcFlags == 0);

    assert(!d.clearAWDLBlockAck());
    return 0;
}
"""
        self.compile_run(p91._build_ba_harness(extra_ba))


if __name__ == "__main__":
    unittest.main()
