"""Failing-first regressions for Continuity-WiFi-Bluetooth-Preview79 Wi-Fi fixes."""
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


def _tick_base(capacity=96):
    src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
    src = src.replace("// NATIVE TX SCENARIOS", (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text())
    src = src.replace("// PRODUCTION", _native_tick_routines())
    src = src.replace("// NATIVE DECLARATIONS", _native_tick_decls())
    if capacity != 64:
        src = src.replace("assert(limit && limit<=32);", "assert(limit && limit<=64);")
        src = src.replace("} pending[64]={};", f"}} pending[{capacity}]={{}};")
        src = src.replace("inFlight[64]={};", f"inFlight[{capacity}]={{}};")
    return src


class Preview79WiFiRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_w1_strict_fifo_head_reinsert_on_enobufs_eagain_alloc_and_split(self):
        tick_src = _tick_base(96) + r"""
static uint8_t singlePayloadTag(const std::vector<uint8_t> &frame) {
    DataBatch batch = {};
    assert(parseDataMPDU(frame.data(), frame.size(), batch) == ParseResult::Valid);
    assert(batch.count == 1);
    return batch.frames[0].payload[batch.frames[0].size - 1];
}

int main() {
    const uint8_t peerAddr[] = {2, 0, 0, 0, 0, 9};

    // 1a. Mid-burst ENOBUFS must reinsert failed packet at HEAD (never tail), preserving 1..6 FIFO.
    {
        clockUS = 10000000;
        AirportItlwmAWDLInterface i;
        dataPeer(i, peerAddr);
        i.nextActionUS = i.nextMIFUS = clockUS + 5000000;
        for (uint8_t tag = 1; tag <= 6; ++tag) {
            auto p = ethernet(i.address.octet, peerAddr);
            p->bytes.back() = tag;
            i.enqueue(p);
        }
        unsigned sent = 0;
        i.device.onTransmit = [&] {
            ++sent;
            if (sent == 2) i.device.transmitError = ENOBUFS;
        };
        i.tick();
        i.device.onTransmit = {};
        i.device.transmitError = 0;
        assert(i.device.dataFrames.size() == 2);
        assert(i.pendingCount == 4);
        assert(singlePayloadTag(i.device.dataFrames[0]) == 1);
        assert(singlePayloadTag(i.device.dataFrames[1]) == 2);

        // Next tick must transmit 3, 4, 5, 6 in exact order (before fix: 4, 5, 6, 3!).
        i.tick();
        assert(i.pendingCount == 0 && i.device.dataFrames.size() == 6);
        for (unsigned idx = 0; idx < 6; ++idx)
            assert(singlePayloadTag(i.device.dataFrames[idx]) == idx + 1);
        i.drain();
    }

    // 1b. Allocation failure (!frame) must reinsert at HEAD, preserving 1..4 FIFO.
    {
        clockUS = 12000000;
        AirportItlwmAWDLInterface i;
        dataPeer(i, peerAddr);
        i.nextActionUS = i.nextMIFUS = clockUS + 5000000;
        for (uint8_t tag = 1; tag <= 4; ++tag) {
            auto p = ethernet(i.address.octet, peerAddr);
            p->bytes.back() = tag;
            i.enqueue(p);
        }
        failedAllocations = 1;
        i.tick();
        assert(i.device.dataFrames.empty() && i.pendingCount == 4);
        i.tick();
        assert(i.pendingCount == 0 && i.device.dataFrames.size() == 4);
        for (unsigned idx = 0; idx < 4; ++idx)
            assert(singlePayloadTag(i.device.dataFrames[idx]) == idx + 1);
        i.drain();
    }

    // 1c. EAGAIN on newly joined software aggregate [1,2] with [3,4] queued behind must restore [1,2,3,4].
    {
        clockUS = 14000000;
        AirportItlwmAWDLInterface i;
        aggregatePeer(i, peerAddr);
        i.nextActionUS = i.nextMIFUS = clockUS + 5000000;
        for (uint8_t tag = 1; tag <= 4; ++tag) {
            auto p = ethernet(i.address.octet, peerAddr);
            p->bytes.back() = tag;
            i.enqueue(p);
        }
        i.device.transmitError = EAGAIN;
        i.tick();
        i.device.transmitError = 0;
        assert(i.device.dataFrames.empty() && i.pendingCount == 4 && i.aggregateExtraOwned == 0);
        // Disable software aggregation so each packet goes out individually in exact order 1, 2, 3, 4.
        i.device.awdlRadio.dataQueueConfigured = false;
        i.tick();
        assert(i.pendingCount == 0 && i.device.dataFrames.size() == 4);
        for (unsigned idx = 0; idx < 4; ++idx)
            assert(singlePayloadTag(i.device.dataFrames[idx]) == idx + 1);
        i.drain();
    }

    // 1d. splitUnsubmitted on retried aggregate [1,2] with [3,4] queued behind must emit 1, 2, 3, 4 in order.
    {
        clockUS = 16000000;
        AirportItlwmAWDLInterface i;
        aggregatePeer(i, peerAddr);
        i.device.acknowledgeData = false;
        i.nextActionUS = i.nextMIFUS = clockUS + 5000000;
        for (uint8_t tag = 1; tag <= 2; ++tag) {
            auto p = ethernet(i.address.octet, peerAddr);
            p->bytes.back() = tag;
            i.enqueue(p);
        }
        i.tick();
        assert(i.inFlightCount == 1 && i.aggregateExtraOwned == 1 && i.pendingCount == 0);
        for (uint8_t tag = 3; tag <= 4; ++tag) {
            auto p = ethernet(i.address.octet, peerAddr);
            p->bytes.back() = tag;
            i.enqueue(p);
        }
        i.completeData(i.device.dataTokens.back(), ECANCELED);
        assert(i.pendingCount == 3 && i.aggregateExtraOwned == 1);
        i.device.dataFrames.clear();
        i.device.dataTokens.clear();
        auto &tx = i.device.awdlRadio.blockAckTX;
        tx.configured = true;
        tx.phase = TXBlockAckQueue::Agreed;
        tx.enabled = true;
        tx.amsdu = false;
        tx.cookie = 44;
        tx.epoch = i.device.dmaEpoch;
        copyAddress(tx.peer, peerAddr);
        ++i.device.awdlRadio.dwellStarts;
        i.tick();
        assert(i.pendingCount == 0 && i.aggregateExtraOwned == 0 && i.device.dataFrames.size() == 4);
        for (unsigned idx = 0; idx < 4; ++idx) {
            assert(singlePayloadTag(i.device.dataFrames[idx]) == idx + 1);
            i.completeData(i.device.dataTokens[idx], 0);
        }
        i.drain();
    }

    assert(!allocations);
    return 0;
}
"""
        self.compile_run("#define DEBUG 0\n" + tick_src)

    def test_w2_bounded_queue_capacity_96_and_batch_32(self):
        awdl_hpp = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()
        self.assertRegex(awdl_hpp, r"DataQueueCapacity\s*=\s*96\b")

        tick_src = _tick_base(96) + r"""
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

    def test_w3_eligible_txba_gating_and_idle_peer_handover(self):
        # Part A: eligible() holds TXBlockAck Enabling/Retiring and enforces 6ms window / queue depth in Agreed.
        tick_src = _tick_base(96) + r"""
int main() {
    const uint8_t peerAddr[] = {2, 0, 0, 0, 0, 9};

    // A1. Enabling and Retiring must be held in eligible() with zero transmitAWDL attempts.
    for (auto phase : {TXBlockAckQueue::Enabling, TXBlockAckQueue::Retiring}) {
        clockUS = 10000000;
        AirportItlwmAWDLInterface i;
        dataPeer(i, peerAddr);
        i.device.awdlRadio.listener = true;
        i.nextActionUS = i.nextMIFUS = clockUS + 5000000;
        auto &tx = i.device.awdlRadio.blockAckTX;
        tx.configured = true;
        tx.phase = phase;
        tx.queue = 3;
        tx.window = 64;
        tx.cookie = 77;
        tx.epoch = i.device.dmaEpoch;
        copyAddress(tx.peer, peerAddr);
        i.enqueue(ethernet(i.address.octet, peerAddr));
        i.tick();
        assert(i.device.data == 0 && i.pendingCount == 1);
        i.drain();
    }

    // A2. In listener Agreed mode, disabled / full BA ring / <=6ms window-tail hold cleanly with zero
    //     transmitAWDL attempts, while open window (>6ms) submits on BA queue even if legacy queue is saturated.
    {
        clockUS = 10000000;
        AirportItlwmAWDLInterface i;
        i.device.awdlRadio.listener = true;
        dataPeer(i, peerAddr);
        i.nextActionUS = i.nextMIFUS = clockUS + 5000000;
        auto &tx = i.device.awdlRadio.blockAckTX;
        tx.configured = true;
        tx.phase = TXBlockAckQueue::Agreed;
        tx.queue = 3;
        tx.window = 8;
        tx.enabled = false;
        tx.paused = true;
        tx.cookie = 78;
        tx.epoch = i.device.dmaEpoch;
        copyAddress(tx.peer, peerAddr);
        i.enqueue(ethernet(i.address.octet, peerAddr));

        // Disabled -> hold.
        i.tick();
        assert(i.device.data == 0 && i.pendingCount == 1);

        // Full BA queue -> hold.
        tx.enabled = true;
        i.device.com.txq[3].queued = 8;
        i.tick();
        assert(i.device.data == 0 && i.pendingCount == 1);

        // Configure an actual channel transition at slot 1 (epoch + 65536 us), matching real AWDL schedules.
        i.sync.channels[1] = 0;
        auto *p = i.device.awdlPeers.find(peerAddr, clockUS);
        assert(p != nullptr);
        p->sequence.channel[1] = 0;
        const uint64_t slotEnd = i.sync.epochUS + 65536;
        i.device.awdlRadio.dwell.windowEndUS = slotEnd;

        // Open window (> 6 ms left) with legacy queue saturated -> submits on BA queue with capped deadline!
        i.device.com.txq[3].queued = 0;
        i.device.com.txq[i.device.awdlRadio.queue].queued = 48;
        i.tick();
        assert(i.device.data == 1 && i.pendingCount == 0);
        assert(!i.device.transmitDeadlines.empty() && tx.windowEndUS == slotEnd - 6000);
        assert(i.device.transmitDeadlines.back() == slotEnd - 6000);
        i.device.com.txq[i.device.awdlRadio.queue].queued = 0;

        // Window tail (4.5 ms left before channel change: between 3 ms and 6 ms) -> hold with zero TX!
        clockUS = slotEnd - 4500;
        i.enqueue(ethernet(i.address.octet, peerAddr));
        i.tick();
        assert(i.device.data == 1 && i.pendingCount == 1);
        i.drain();
    }

    assert(!allocations);
    return 0;
}
"""
        self.compile_run("#define DEBUG 0\n" + tick_src)

        # Part B: TXBlockAckQueue idle-peer handover state machine.
        ba_src = r"""
#include <cassert>
#include <cstdint>
#include "AirportItlwm/AWDL/TXBlockAck.hpp"

using namespace AirportAWDL;

int main() {
    const uint8_t peerA[6] = {2, 0, 0, 0, 0, 1};
    const uint8_t peerB[6] = {2, 0, 0, 0, 0, 2};
    TXBlockAckQueue tx = {};
    uint64_t now = 10000000;

    // 1. Peer A acquires TXBlockAck via acceptedRX.
    tx.demand(peerA, 0, now, true, true);
    assert(tx.phase == TXBlockAckQueue::Requested);
    assert(sameAddress(tx.peer, peerA));
    assert(tx.lastDataUS == now);
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Agreed;

    // 2. Peer B cannot preempt while Peer A was active < 1s ago.
    tx.demand(peerB, 0, now + 500000, true, true);
    assert(tx.phase == TXBlockAckQueue::Agreed && sameAddress(tx.peer, peerA) && !tx.releasePeer);

    // 3. Peer A activity refreshes lastDataUS and blocks preemption for another 1s.
    tx.demand(peerA, 1400, now + 800000, false, true);
    assert(tx.lastDataUS == now + 800000);
    tx.demand(peerB, 0, now + 1500000, true, true);
    assert(tx.phase == TXBlockAckQueue::Agreed && !tx.releasePeer);

    // 4. Non-empty hardware ring blocks preemption even after >= 1s idle.
    tx.demand(peerB, 0, now + 2000000, true, false);
    assert(tx.phase == TXBlockAckQueue::Agreed && !tx.releasePeer);

    // 5. Idle >= 1s AND empty hardware ring: DemandThreshold-1 large frames do not trigger; next triggers releasePeer + Retiring.
    for (unsigned n = 0; n < TXBlockAckQueue::DemandThreshold - 1; ++n) {
        tx.demand(peerB, 1400, now + 2000000 + n * 1000, false, true);
        assert(tx.phase == TXBlockAckQueue::Agreed && !tx.releasePeer);
    }
    tx.demand(peerB, 1400, now + 2016000, false, true);
    assert(tx.phase == TXBlockAckQueue::Retiring);
    assert(tx.releasePeer);
    assert(tx.needDelba);
    assert(sameAddress(tx.handoverPeer, peerB));

    // 6. Idle Asking (without in-flight management token) also hands over cleanly without DELBA wait.
    tx = {};
    tx.demand(peerA, 0, now, true, true);
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Asking;
    tx.managementToken = 0;
    tx.demand(peerB, 0, now + TXBlockAckQueue::IdleHandoverUS, true, true);
    assert(tx.phase == TXBlockAckQueue::Retiring);
    assert(tx.releasePeer);
    assert(!tx.needDelba);
    assert(sameAddress(tx.handoverPeer, peerB));
    return 0;
}
"""
        self.compile_run(ba_src)

        # Part C: Hal wiring checks in ItlIwxAWDL.cpp and ItlIwx.cpp.
        iwx_awdl = (support.ROOT / "itlwm/hal_iwx/ItlIwxAWDL.cpp").read_text()
        self.assertIn("owner.releasePeer", iwx_awdl)
        self.assertIn("owner.handoverPeer", iwx_awdl)
        self.assertIn("owner.lastDataUS", iwx_awdl)
        iwx_cpp = (support.ROOT / "itlwm/hal_iwx/ItlIwx.cpp").read_text()
        self.assertIn("txBA.lastDataUS", iwx_cpp)


if __name__ == "__main__":
    unittest.main()
