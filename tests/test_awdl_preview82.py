"""Failing-first regressions for Preview 82 Sidecar TX BA, queue depth, and llw0 readiness."""
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


class Preview82Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p82_llw0_boot_order_and_async_ifnet_create(self):
        # 1. Startup order: startAWDLInterface must not publish awdlBSD before startLLWInterface binds llw0.
        start_awdl = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwm::startAWDLInterface")
        self.assertNotIn("awdlBSD->registerService()", start_awdl)

        v2_src = (support.ROOT / "AirportItlwm/AirportItlwmV2.cpp").read_text()
        start_ctrl = v2_src.split("bool AirportItlwm::start(", 1)[1].split("void AirportItlwm::stop(", 1)[0]
        llw_pos = start_ctrl.find("startLLWInterface()")
        awdl_reg_pos = start_ctrl.find("awdlBSD->registerService()")
        self.assertGreater(llw_pos, 0)
        self.assertGreater(awdl_reg_pos, llw_pos)

        # 2. Companion Skywalk create succeeds even when IONetworkStack has not finished attaching llwBSD's ifnet yet,
        #    and syncs llwInterface/llwBSD link state once ifnet attaches.
        headers = (support.ROOT / "include/Airport/apple80211_var.h").read_text() + "\n" + (
            support.ROOT / "include/Airport/apple80211_ioctl.h"
        ).read_text()
        comp_decl = re.search(
            r"struct apple80211_companion_skywalk_if_create\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;",
            headers, re.S,
        )[0]
        routines = "\n".join(
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
            for n in ("handleCompanionSkywalkCreate", "syncLLWLinkState")
        )
        src = f"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "AirportItlwm/AWDL/LLWFlow.hpp"

using namespace AirportAWDL;
using IOReturn = int;
using SInt32 = int;
enum {{
    kIOReturnSuccess = 0,
    kIOReturnBadArgument = 1,
    kIOReturnNotReady = 2,
    kIO80211NetworkLinkDown = 1,
    kIO80211NetworkLinkUp = 2
}};
{comp_decl}

struct Ifnet {{ const char *name = "llw"; unsigned unit = 0; }};
using ifnet_t = Ifnet *;
static const char *ifnet_name(ifnet_t i) {{ return i->name; }}
static unsigned ifnet_unit(ifnet_t i) {{ return i->unit; }}

struct MockLLWIf {{
    bool enabled = false;
    unsigned enableCalls = 0, disableCalls = 0;
    bool isEnabled() const {{ return enabled; }}
    bool isSkywalkNative() const {{ return false; }}
    ifnet_t getBSDInterface() const {{ return nullptr; }}
    const char *getBSDNamePrefix() {{ return "llw"; }}
    SInt32 setInterfaceEnable(bool on) {{
        if (on) ++enableCalls; else ++disableCalls;
        enabled = on;
        return kIOReturnSuccess;
    }}
}};

struct MockBSD {{
    Ifnet ifnet;
    bool attached = false;
    bool linkUp = false;
    unsigned startThreads = 0, stopThreads = 0, flushes = 0;
    const char *getNamePrefix() const {{ return "llw"; }}
    ifnet_t getIfnet() {{ return attached ? &ifnet : nullptr; }}
    void setLinkState(int s) {{ linkUp = (s == kIO80211NetworkLinkUp); }}
    void startOutputThread() {{ ++startThreads; }}
    void stopOutputThread() {{ ++stopThreads; }}
    void flushOutputQueue() {{ ++flushes; }}
}};

struct Radio {{ struct {{ bool ready = true; }} awdlRadio; }};
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
    bool stopping = false, running = true, requested = true;
    bool llwActive = false, llwLinkUp = false, llwParamsEnabled = false;
    LLWFlowRouter llwFlows = {{}};
    uint64_t llwCreateRequests = 0, llwDestroyRequests = 0;

    IOReturn handleCompanionSkywalkCreate(apple80211_companion_skywalk_if_create *);
    void syncLLWLinkState();
}};

{routines}

int main() {{
    AirportItlwmAWDLInterface i;

    // 1. Null llwBSD returns NotReady.
    i.llwBSD = nullptr;
    apple80211_companion_skywalk_if_create req = {{1, 1, {{}}}};
    assert(i.handleCompanionSkywalkCreate(&req) == kIOReturnNotReady);
    assert(!i.llwActive && i.llwCreateRequests == 0);

    // 2. Non-null llwBSD with async ifnet attach still in progress succeeds and populates "llw0".
    i.llwBSD = &i.llwBsdObj;
    i.llwBsdObj.attached = false;
    assert(i.handleCompanionSkywalkCreate(&req) == kIOReturnSuccess);
    assert(strcmp(req.bsd_name, "llw0") == 0);
    assert(i.llwActive && !i.llwLinkUp && i.llwCreateRequests == 1);
    assert(i.timerObj.wakes >= 1);

    // 3. Before ifnet finishes attaching, syncLLWLinkState is a safe no-op.
    i.syncLLWLinkState();
    assert(!i.llwLinkUp && !i.llwBsdObj.linkUp && i.llwBsdObj.startThreads == 0);

    // 4. Once IONetworkStack finishes attaching ifnet, syncLLWLinkState brings llw0 up idempotently.
    i.llwBsdObj.attached = true;
    i.syncLLWLinkState();
    assert(i.llwLinkUp && i.llwObj.enabled && i.llwBsdObj.linkUp && i.llwBsdObj.startThreads == 1);
    i.syncLLWLinkState();
    assert(i.llwBsdObj.startThreads == 1 && i.llwObj.enableCalls == 1);

    // 5. Radio pause brings llwLinkUp down; resume brings it back up.
    i.running = false;
    i.syncLLWLinkState();
    assert(!i.llwLinkUp && !i.llwBsdObj.linkUp);
    i.running = true;
    i.syncLLWLinkState();
    assert(i.llwLinkUp && i.llwBsdObj.linkUp && i.llwBsdObj.startThreads == 2);

    // 6. Action 0 destroys cleanly.
    apple80211_companion_skywalk_if_create del = {{1, 0, {{}}}};
    assert(i.handleCompanionSkywalkCreate(&del) == kIOReturnSuccess);
    assert(!i.llwActive && !i.llwLinkUp && !i.llwObj.enabled && !i.llwBsdObj.linkUp);
    assert(i.llwDestroyRequests == 1 && i.llwBsdObj.stopThreads == 1 && i.llwBsdObj.flushes == 1);
    return 0;
}}
"""
        self.compile_run(src)

    def test_p82_siocsiflladdr_isolation_and_ula_udp_routing(self):
        # 1. AirportItlwmEthernetInterface::performCommand intercepts SIOCSIFLLADDR for AWDL/LLW
        #    without ever falling through to super::performCommand (en1).
        bsd_src = (support.ROOT / "tests/fixtures/awdl_bsd_ioctl.cpp").read_text().rsplit("int main()", 1)[0]
        bsd_routines = "\n".join(
            definition("AirportItlwm/AirportItlwmEthernetInterface.cpp", "AirportItlwmEthernetInterface::" + n)
            for n in ("performCommand", "syncAWDLFlagsGated", "syncAWDLMulticastGated")
        )
        bsd_src = bsd_src.replace("// PRODUCTION: ioctl", bsd_routines) + r"""
int main() {
    Ifnet ifnet, other;
    IONetworkController controller;
    for (unsigned role : {APPLE80211_VIF_AWDL, APPLE80211_VIF_LOW_LATENCY}) {
        Skywalk sw; sw.role = role;
        AirportItlwmEthernetInterface bridge;
        bridge.ifnet = &ifnet; bridge.interface = &sw;
        ifreq req = {};
        req.ifr_addr.sa_len = ETHER_ADDR_LEN;
        memcpy(req.ifr_addr.sa_data, sw.mac.octet, ETHER_ADDR_LEN);
        assert(bridge.performCommand(&controller, SIOCSIFLLADDR, &ifnet, &req) == 0);
        assert(bridge.genericCalls == 0);
        req.ifr_addr.sa_data[5] ^= 0xff;
        assert(bridge.performCommand(&controller, SIOCSIFLLADDR, &ifnet, &req) == EOPNOTSUPP);
        assert(bridge.genericCalls == 0);
        assert(bridge.performCommand(&controller, SIOCSIFLLADDR, &other, &req) == EINVAL);
        assert(bridge.performCommand(&controller, SIOCSIFLLADDR, &ifnet, nullptr) == EINVAL);
        req.ifr_addr.sa_len = 4;
        assert(bridge.performCommand(&controller, SIOCSIFLLADDR, &ifnet, &req) == EINVAL);
        assert(bridge.genericCalls == 0);
    }
    return 0;
}
"""
        self.compile_run(bsd_src)

        # 2. ULA (fd00::/8) UDP packets (Continuity Camera UDPNWPath) route to LLW even on initial TID 0,
        #    while explicit awdlFlows, mDNS, and link-local non-LLW UDP stay on AWDL.
        src = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include "AirportItlwm/AWDL/LLWFlow.hpp"

using namespace AirportAWDL;

static void makeIPv6UDP(uint8_t *buf, uint8_t srcPrefix, uint8_t dstPrefix,
                        uint16_t srcPort, uint16_t dstPort) {
    memset(buf, 0, 56);
    buf[0] = 0x60;
    buf[4] = 0; buf[5] = 16;
    buf[6] = 17;
    buf[7] = 64;
    buf[8] = srcPrefix; buf[9] = srcPrefix == 0xfe ? 0x80 : 0xad; buf[23] = 7;
    buf[24] = dstPrefix; buf[25] = dstPrefix == 0xfe ? 0x80 : 0xad; buf[39] = 1;
    buf[40] = srcPort >> 8; buf[41] = srcPort & 0xff;
    buf[42] = dstPort >> 8; buf[43] = dstPort & 0xff;
    buf[44] = 0; buf[45] = 16;
}

int main() {
    LLWFlowRouter router = {};
    const uint8_t iphone[6] = {2, 0, 0, 0, 0, 2};
    uint64_t now = 10000000;

    // Initial server-mode Continuity Camera ULA UDP packet (TID 0) routes to LLW.
    uint8_t ulaUDP[56];
    makeIPv6UDP(ulaUDP, 0xfd, 0xfd, 50594, 64264);
    assert(router.classifyReceive(iphone, ulaUDP, sizeof(ulaUDP), 0, true, false, nullptr, now) == LLWRoute::LLW);

    // Mixed ULA source to link-local dst (TID 0) also routes to LLW when active.
    uint8_t mixedULA[56];
    makeIPv6UDP(mixedULA, 0xfd, 0xfe, 50594, 64264);
    assert(router.classifyReceive(iphone, mixedULA, sizeof(mixedULA), 0, true, false, nullptr, now) == LLWRoute::LLW);

    // Inactive LLW never diverts ULA UDP.
    assert(router.classifyReceive(iphone, ulaUDP, sizeof(ulaUDP), 0, false, false, nullptr, now) == LLWRoute::AWDL);

    // Explicit awdl0 transmit on the same local port keeps replies on AWDL.
    uint8_t awdlTX[56];
    makeIPv6UDP(awdlTX, 0xfd, 0xfd, 64264, 50594);
    router.noteTransmit(iphone, awdlTX, sizeof(awdlTX), false, now);
    assert(router.classifyReceive(iphone, ulaUDP, sizeof(ulaUDP), 0, true, true, iphone, now + 1000) == LLWRoute::AWDL);

    // Link-local non-LLW UDP at TID 0 remains AWDL, and ULA mDNS stays AWDL.
    uint8_t llUDP[56];
    makeIPv6UDP(llUDP, 0xfe, 0xfe, 50885, 51000);
    assert(router.classifyReceive(iphone, llUDP, sizeof(llUDP), 0, true, true, iphone, now + 1000) == LLWRoute::AWDL);
    uint8_t ulaMDNS[56];
    makeIPv6UDP(ulaMDNS, 0xfd, 0xfd, 5353, 5353);
    assert(router.classifyReceive(iphone, ulaMDNS, sizeof(ulaMDNS), 6, true, true, iphone, now + 1000) == LLWRoute::AWDL);
    return 0;
}
"""
        self.compile_run(src)

    def test_p82_tx_block_ack_heavy_peer_preempts_low_rate_owner(self):
        src = r"""
#include <cassert>
#include <cstdint>
#include "AirportItlwm/AWDL/TXBlockAck.hpp"

using namespace AirportAWDL;

int main() {
    const uint8_t ucPeer[6]      = {2, 0, 0, 0, 0, 10}; // Universal Control (small TCP/HID packets)
    const uint8_t sidecarPeer[6] = {2, 0, 0, 0, 0, 20}; // Sidecar (1400B video burst)

    // 1. Low-rate peer claims TX BA via acceptedRX, sends continuous <1200B packets every 100ms.
    //    Sidecar's 50x1400B burst MUST preempt it without waiting for full idle.
    TXBlockAckQueue tx = {};
    uint64_t now = 10000000;
    tx.demand(ucPeer, 0, now, true, true);
    assert(tx.phase == TXBlockAckQueue::Requested && sameAddress(tx.peer, ucPeer));
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Agreed;

    for (unsigned step = 0; step < 5; ++step) {
        now += 100000;
        tx.demand(ucPeer, 120, now, false, true);
    }
    // Feed 15 large Sidecar frames interspersed with small UC keepalives; must not lose count.
    for (unsigned n = 0; n < 15; ++n) {
        now += 1000;
        if (n % 5 == 0) tx.demand(ucPeer, 120, now, false, true);
        tx.demand(sidecarPeer, 1400, now, false, n % 3 != 0);
        assert(tx.phase == TXBlockAckQueue::Agreed && !tx.releasePeer);
    }
    // 16th frame while queueEmpty=false accumulates without retiring yet; next with queueEmpty=true retires!
    now += 1000;
    tx.demand(sidecarPeer, 1400, now, false, false);
    assert(tx.phase == TXBlockAckQueue::Agreed && !tx.releasePeer);
    now += 1000;
    tx.demand(sidecarPeer, 1400, now, false, true);
    assert(tx.phase == TXBlockAckQueue::Retiring && tx.releasePeer);
    assert(sameAddress(tx.handoverPeer, sidecarPeer) && tx.handoverFrames >= 16);

    // 2. Active heavy-TX owner cannot be preempted by another heavy peer within HeavyIdleHandoverUS (500ms).
    tx = {};
    now = 20000000;
    for (unsigned n = 0; n < 16; ++n) tx.demand(sidecarPeer, 1400, now + n * 1000, false, true);
    assert(tx.phase == TXBlockAckQueue::Requested && sameAddress(tx.peer, sidecarPeer));
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Agreed;

    now += 200000;
    tx.demand(sidecarPeer, 1400, now, false, true); // heavy refresh
    for (unsigned n = 0; n < 60; ++n)
        tx.demand(ucPeer, 1400, now + 100000 + n * 1000, false, true); // only 160ms since heavy refresh
    assert(tx.phase == TXBlockAckQueue::Agreed && !tx.releasePeer && sameAddress(tx.peer, sidecarPeer));

    // 3. Once heavy owner goes light-only (<1200B) for >= HeavyIdleHandoverUS, heavy challenger preempts.
    now += TXBlockAckQueue::HeavyIdleHandoverUS;
    tx.demand(sidecarPeer, 120, now, false, true); // tiny keepalive only
    for (unsigned n = 0; n < 16; ++n)
        tx.demand(ucPeer, 1400, now + 1000 + n * 1000, false, true);
    assert(tx.phase == TXBlockAckQueue::Retiring && tx.releasePeer && sameAddress(tx.handoverPeer, ucPeer));

    // 4. acceptedRX from idle peer still cannot steal from an owner with recent small traffic (<IdleHandoverUS).
    tx = {};
    now = 30000000;
    tx.demand(sidecarPeer, 1400, now, true, true);
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Agreed;
    now += 600000;
    tx.demand(sidecarPeer, 120, now, false, true); // updates lastDataUS
    tx.demand(ucPeer, 0, now + 500000, true, true); // acceptedRX only, <1s since lastDataUS
    assert(tx.phase == TXBlockAckQueue::Agreed && !tx.releasePeer);
    return 0;
}
"""
        self.compile_run(src)

    def test_p82_bounded_driver_ring_96_32_and_bsd_pull_256(self):
        awdl_hpp = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()
        self.assertRegex(awdl_hpp, r"DataQueueCapacity\s*=\s*96\b")

        cfg = definition("AirportItlwm/AirportItlwmV2.cpp", "AirportItlwm::configureInterface")
        self.assertRegex(cfg, r"configureOutputPullModel\s*\(\s*256\s*,")

        pull_awdl = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::pullOutputPackets")
        pull_llw = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::pullLLWOutputPackets")
        self.assertIn("< 32 ? queueCapacity / 2 : 32", pull_awdl)
        self.assertIn("< 32 ? queueCapacity / 2 : 32", pull_llw)

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
    return 0;
}
"""
        self.compile_run("#define DEBUG 0\n" + tick_src)


if __name__ == "__main__":
    unittest.main()
