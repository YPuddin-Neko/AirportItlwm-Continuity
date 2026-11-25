"""Preview 116 (2.4.0d116) Sidecar BWE & FaceTime LLW boundary regressions:
1. FIX-P116-1: WindowCheck::end() in serviceTXBlockAck() keeps Station 4 UNPAUSED across Ch 48 @
   20 MHz slots (Slots 6-7 & 14-15) on Sidecar Data + Infra Idle [149x6, 48x2, 6, 149x5, 48x2],
   draining bulk VI frames continuously without triggering OWRD delay-gradient RemoteBWE collapse
   or DataTxExpired drops (restoring 2.4.0d104 unpaused behavior now that FIX-P115-1..2 solved
   Ch 48 20 MHz RX deafness).
2. FIX-P116-2: AirportItlwmAWDLInterface::tick() keeps policy.bridgeInfraToWide = false across all
   slots so Mac tunes PHY 1 onto Ch 48 @ 20 MHz during Slots 14-15 instead of parking blind on
   Ch 149 — achieving 15/16 slots (93.75%) of active bidirectional TX+RX with the peer and
   reducing max contiguous TX pause from 3 EAWs (196.61 ms) to 1 EAW (65.54 ms on Slot 8 Ch 6).
3. FIX-P116-4: <= 500 us early LLW pre-tune in tick() and GrantPolicy::windowEnd fires ONLY when
   hopping to a strictly higher-slot-count channel in sync.channels[0..15]
   (slotCount(nextChan) > slotCount(currentChan)): 149 -> 48 (2 -> 14 slots) pre-tunes early onto
   Ch 48 across 7/5-slot runs, while 48 -> 149 (14 -> 2 slots) and equal-count ties NEVER cut off
   end-of-slot HEVC RTP or two-step UDPv6 gPTP FollowUp packets early.
"""
import unittest

import test_awdl_preview82 as p82
import test_regressions as support
from test_offline import definition


def _tick_harness(main_body: str, include_drain_pending: bool = False) -> str:
    tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
    tick_src = tick_src.replace(
        "// NATIVE TX SCENARIOS",
        (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
    )
    prod = p82._native_tick_routines()
    if include_drain_pending:
        prod += "\n" + definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmAWDLInterface::drainPendingData",
        )
    tick_src = tick_src.replace("// PRODUCTION", prod)
    tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
    tick_src = tick_src.replace(
        "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {return width==20 && (c==6||c==44||(c==149&&allow149));}",
        "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {"
        "return (width==20 && (c==6||c==44||c==48||(c==149&&allow149))) || "
        "(width==80 && c==149 && allow149);}",
    )
    return tick_src + main_body


class Preview116LLWRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p116_1_and_2_sidecar_15_of_16_slots_unpaused_and_zero_infra_parks(self):
        tick_src = _tick_harness(r"""
static Packet *makeVideoPacket(const uint8_t *dst, const uint8_t *src, size_t payloadBytes) {
    const size_t total = 14 + 40 + 8 + payloadBytes;
    auto *m = awdlPacket(total);
    uint8_t *b = m->bytes.data();
    memset(b, 0, total);
    copyAddress(b, dst);
    copyAddress(b + 6, src);
    b[12] = 0x86; b[13] = 0xdd;
    b[14] = 0x68; // DSCP CS4 / VI
    const uint16_t plen = uint16_t(8 + payloadBytes);
    b[18] = uint8_t(plen >> 8); b[19] = uint8_t(plen & 0xff);
    b[20] = 17; // UDP
    b[21] = 64;
    b[54] = 0x40; b[55] = 0x00;
    b[56] = 0x40; b[57] = 0x01;
    b[58] = uint8_t(plen >> 8); b[59] = uint8_t(plen & 0xff);
    return m;
}

int main() {
    clockUS = 10000000ULL;
    const uint8_t ipad[6] = {2, 0, 0, 0, 0, 0xc4};
    AirportItlwmAWDLInterface i;
    dataPeer(i, ipad);
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.ready = true;
    i.running = true;
    i.nextActionUS = i.nextMIFUS = clockUS + 50000000ULL;

    const uint8_t sidecar[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    auto *peer = i.device.awdlPeers.find(ipad, clockUS);
    assert(peer);
    peer->sequence.count = 16;
    peer->sequence.encoding = 3;
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = sidecar[s];
        i.sync.channelClasses[s] = sidecar[s] == 149 ? 128 : 0;
        peer->sequence.channel[s] = sidecar[s];
        peer->sequence.flags[s] = sidecar[s] == 149 ? 128 : (sidecar[s] == 48 ? 115 : 81);
    }
    i.sync.epochCounter = 0;
    i.sync.epochUS = clockUS;
    peer->sync.nextWindowTU = 64;
    peer->syncReceivedUS = clockUS;

    auto &tx = i.device.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Agreed;
    tx.enabled = true;
    tx.paused = true;
    tx.cookie = 77;
    tx.epoch = i.device.dmaEpoch;
    copyAddress(tx.peer, ipad);
    tx.window = 64;
    tx.queue = 2;
    tx.lastDataUS = tx.lastHeavyDataUS = clockUS;

    unsigned active5GSlots = 0;
    for (unsigned s = 0; s < 16; ++s) {
        clockUS = i.sync.epochUS + uint64_t(s) * 65536ULL + 5000ULL;
        peer->lastSeenUS = peer->sequenceReceivedUS = clockUS;
        tx.lastDataUS = tx.lastHeavyDataUS = clockUS;
        tx.paused = true;
        tx.windowEndUS = 0;

        i.tick();
        // FIX-P116-2: bridgeInfraToWide is NEVER set across any of the 16 slots,
        // and PHY 1 tunes directly to the peer's exact channel on all 16 slots!
        assert(!i.device.lastPolicy.bridgeInfraToWide);
        assert(i.device.awdlRadio.channel == sidecar[s]);
        assert(i.device.awdlRadio.widthMHz == (sidecar[s] == 149 ? 80 : 20));

        if (sidecar[s] > 14) {
            ++active5GSlots;
            // FIX-P116-1: Station 4 opens on ALL 15 5 GHz slots (11x 149@80 + 4x 48@20)!
            i.serviceTXBlockAck();
            assert(!tx.paused && tx.windowEndUS > clockUS);

            // Enqueue & drain a 1200 B Sidecar VI packet on Slots 6, 7, 14, 15 (Ch 48 @ 20 MHz):
            if (sidecar[s] == 48) {
                const size_t before = i.device.dataFrames.size();
                i.bsd->output.push_back(makeVideoPacket(ipad, i.address.octet, 1200));
                assert(i.pullOutputPackets() == kIOReturnSuccess);
                i.drainPendingData();
                assert(i.pendingCount == 0);
                assert(i.device.dataFrames.size() == before + 1);
            }
        }
    }
    assert(active5GSlots == 15);
    assert(i.dataTxExpired == 0 && i.dataAbandoned == 0 && i.txDropped == 0);

    i.drain();
    assert(!allocations);
    return 0;
}
""", include_drain_pending=True)
        self.compile_run(tick_src)

    def test_p116_4_directional_early_pretune_rtg_and_grant_policy_consistency(self):
        tick_src = _tick_harness(r"""
int main() {
    clockUS = 10000000ULL;
    AirportItlwmAWDLInterface i;
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.ready = true;
    i.running = true;
    i.llwActive = true;
    i.llwParamsEnabled = true;

    // 1. FaceTime RealTimeGroup [48, 48, 149, 48x7, 149, 48x5] (slotCount(48)=14 > slotCount(149)=2):
    const uint16_t rtg[16] = {48,48,149,48,48,48,48,48,48,48,149,48,48,48,48,48};
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = rtg[s];
        i.sync.channelClasses[s] = 0;
    }
    i.sync.epochCounter = 0;
    i.sync.epochUS = clockUS;
    i.nextActionUS = i.nextMIFUS = clockUS + 50000000ULL;

    // Enter Slot 1 (Ch 48):
    clockUS = i.sync.epochUS + 65536ULL + 5000ULL;
    i.tick();
    assert(i.device.awdlRadio.channel == 48 && i.device.awdlRadio.widthMHz == 20);

    // At T_slot2 - 400 us (48 -> 149, 14 -> 2 slots): MUST NOT cut off Ch 48 early!
    const uint64_t slot2Start = i.sync.epochUS + 2ULL * 65536ULL;
    const uint64_t slot3Start = i.sync.epochUS + 3ULL * 65536ULL;
    clockUS = slot2Start - 400ULL;
    i.tick();
    assert(i.device.awdlRadio.channel == 48 && i.device.awdlRadio.widthMHz == 20);
    assert(i.scheduler.delayUS == 450); // Wakes right at slot2Start + 50 us!
    // GrantPolicy::windowEnd MUST also reject 48 -> 149 early pre-tune (returns 0):
    assert(i.device.lastPolicy.windowEnd(i.device.lastPolicy.context, clockUS, 2, 149) == 0);
    assert(i.device.lastPolicy.windowEnd(i.device.lastPolicy.context, clockUS, 0, 48) == slot2Start);

    // At slot2Start + 50 us: tunes cleanly to Ch 149 @ 20 MHz!
    clockUS = slot2Start + 50ULL;
    i.tick();
    assert(i.device.awdlRadio.channel == 149 && i.device.awdlRadio.widthMHz == 20);
    assert(i.device.awdlRadio.dwell.windowEndUS == slot3Start);

    // At T_slot3 - 400 us (149 -> 48, 2 -> 14 slots): MUST pre-tune early back onto Ch 48 across 7 slots!
    clockUS = slot3Start - 400ULL;
    i.tick();
    assert(i.device.awdlRadio.channel == 48 && i.device.awdlRadio.widthMHz == 20);
    const uint64_t slot10Start = i.sync.epochUS + 10ULL * 65536ULL;
    assert(i.device.awdlRadio.dwell.windowEndUS == slot10Start);
    // GrantPolicy::windowEnd MUST agree 100% with tick() on 149 -> 48 early pre-tune:
    assert(i.device.lastPolicy.windowEnd(i.device.lastPolicy.context, clockUS + 100ULL, 3, 48) == slot10Start);

    // Verify Slot 9 -> Slot 10 (48 -> 149) also stays on Ch 48 at T - 350 us, and Slot 10 -> Slot 11 pre-tunes to Ch 48:
    clockUS = slot10Start - 350ULL;
    i.tick();
    assert(i.device.awdlRadio.channel == 48);

    const uint64_t slot11Start = i.sync.epochUS + 11ULL * 65536ULL;
    clockUS = slot10Start + 1000ULL;
    i.tick();
    assert(i.device.awdlRadio.channel == 149);

    clockUS = slot11Start - 350ULL;
    i.tick();
    assert(i.device.awdlRadio.channel == 48);
    assert(i.device.awdlRadio.dwell.windowEndUS == i.sync.epochUS + 18ULL * 65536ULL);

    // 2. Equal-slot-count tie (8 slots Ch 48 vs 8 slots Ch 149): strict '>' prevents early cutoff in either direction!
    for (unsigned s = 0; s < 16; ++s)
        i.sync.channels[s] = (s < 8) ? 48 : 149;
    const uint64_t base2 = i.sync.epochUS + 32ULL * 65536ULL;
    clockUS = base2 + 7ULL * 65536ULL + 65536ULL - 300ULL;
    i.tick();
    assert(i.device.awdlRadio.channel == 48);
    assert(i.device.lastPolicy.windowEnd(i.device.lastPolicy.context, clockUS, 8, 149) == 0);

    i.drain();
    assert(!allocations);
    return 0;
}
""")
        self.compile_run(tick_src)


if __name__ == "__main__":
    unittest.main()
