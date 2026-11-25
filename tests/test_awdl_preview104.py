"""Ground-truth regressions for Preview 104 (2.4.0d104):
1. RC-S1: Allow non-LLW QoS (VI/VO TID 4/5/6) RX-ADDBA acceptance to seed TXBlockAck and invoke
   rxTakeover when current owner is idle, break the Station 3 dataQueue <-> solicitDeadline
   Asking deadlock, scan outbound-only pending[] when blockAckTX is Idle, lower TXBA demand /
   handover threshold to 16 heavy frames with a 250 ms handover DELBA deadline, and evaluate
   txIdle per blockAckTX.peer with O(1) paused setAWDLTXWindow fast-return.
2. RC-S2: Remove 2.4 GHz Ch 6 social parking (MaxSocialParkWindowUS) and restrict <= 2-slot
   (<= 140 ms) infra parking to bridgeInfraToWide spans returning immediately to the same 80 MHz
   channel (Sidecar Slots 14-15 [48, 48] bridging 149x5 -> 149x6 = 852 ms MIMO window, while
   Slots 6-7 [48, 48] and Slot 8 [6] tune cleanly).
3. RC-S3: Normalize IEEE 80/160 MHz center channels (155->149, 42->44, 58->52, 106->100,
   122->116, 138->132, 50->48, 114->100) in TLV 32 (ChanSeqExtTLV) while preserving TLV 18's
   exact primary channel in the same block and retaining TLV 18 on slots omitted from TLV 32.
4. RC-S4: Preserve TxRateControl current rate across channel hops on the same dmaEpoch, extend
   Peer::txHTMcs lifetime to 10.0s while occupied, and preserve strict per-peer FIFO order for
   temporarily ineligible unicast frames in pending[].
5. RC-F1: Shrink serviceAWDLListener closeLeadUS to 400 us under active llwRealtime when
   Station 3 has 0 queued descriptors (1500 us when queued) and lower TID 7 RXBA reorder gap
   timeout (llwTimeoutUS) from 15 ms to 8 ms.
6. RC-E1: Ignore ambient PSF/MIF broadcasts (lastUnicastDataUS == 0, !nativeCached) in
   hasActivePeer so idle AWDL unbinds PHY 1 after 3.0s, while setting parkIdleOnInfra on 0-slots
   whenever an active unicast / cached peer keeps PHY 1 bound.
"""
import unittest

import test_awdl_preview82 as p82
import test_awdl_preview91 as p91
import test_regressions as support
from test_offline import definition


def _tick_harness(main_body: str, extra_defs=()) -> str:
    tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
    tick_src = tick_src.replace(
        "// NATIVE TX SCENARIOS",
        (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
    )
    extra = "\n".join(
        definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
        for n in ("drainPendingData", *extra_defs)
    )
    tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines() + "\n" + extra)
    tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
    tick_src = tick_src.replace(
        "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {return width==20 && (c==6||c==44||(c==149&&allow149));}",
        "bool awdlChannelAllowed(unsigned c,uint8_t width=20) {"
        "return (width==20 || (width==80 && c==149)) && (c==6||c==44||c==48||(c==149&&allow149));}",
    )
    tick_src = tick_src.replace(
        "awdlRadio.blockAckTX.demand(peer,bytes,clockUS,accepted);",
        "awdlRadio.blockAckTX.demand(peer,bytes,clockUS,accepted,!awdlRadio.blockAckTX.configured || !com.txq[awdlRadio.blockAckTX.queue].queued);",
    )
    helper = r"""
static mbuf_t sizedEthernet(const uint8_t *self, const uint8_t *dest, size_t len, uint8_t tag = 0) {
    auto p = awdlPacket(len); auto b = p->bytes.data();
    copyAddress(b, dest); copyAddress(b + 6, self); b[12] = 0x86; b[13] = 0xdd; b[14] = 0x60;
    const uint16_t plen = uint16_t(len - 54);
    b[18] = uint8_t(plen >> 8); b[19] = uint8_t(plen);
    b[20] = 17; b[21] = 64;
    if (len >= 62) {
        const uint16_t ulen = uint16_t(len - 54);
        b[54] = 0x13; b[55] = 0x88; b[56] = 0x13; b[57] = 0x89;
        b[58] = uint8_t(ulen >> 8); b[59] = uint8_t(ulen);
    }
    if (len > 64) b[64] = tag;
    return p;
}
"""
    return tick_src + helper + main_body


class Preview104Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p104_rcs1_qos_rxba_seed_rxtakeover_solicit_nondeadlock_and_per_peer_txidle(self):
        tick_src = _tick_harness(
            r"""
int main() {
    clockUS = 10000000ULL;
    const uint8_t tvPeer[6]     = {2, 0, 0, 0, 0, 0x10};
    const uint8_t ipadPeer[6]   = {2, 0, 0, 0, 0, 0x20};
    const uint8_t iphonePeer[6] = {2, 0, 0, 0, 0, 0x30};

    AirportItlwmAWDLInterface i;
    dataPeer(i, tvPeer);
    dataPeer(i, ipadPeer);
    dataPeer(i, iphonePeer);
    for (auto &c : i.sync.channels) c = 44;
    i.device.baResult = 0;
    i.device.awdlRadio.listener = true;
    i.tick();
    clockUS += 4000ULL;
    i.tick();
    i.device.loop.gated = true;
    i.nextActionUS = i.nextMIFUS = clockUS + 10000000ULL;

    auto refreshAll = [&]() {
        for (unsigned k = 0; k < 3; ++k) {
            auto &p = i.device.awdlPeers.peers[k];
            for (unsigned s = 0; s < 16; ++s) { p.sequence.channel[s] = 44; p.sequence.flags[s] = 0; }
            p.lastSeenUS = p.syncReceivedUS = p.sequenceReceivedUS =
                p.electionV2ReceivedUS = p.htCapabilitiesReceivedUS = clockUS;
            p.sync.phyTxTime = p.sync.targetTxTime = uint32_t(clockUS);
            p.sync.nextWindowTU = 64;
        }
        i.sync.elect(i.device.awdlPeers, clockUS);
        for (auto &c : i.sync.channels) c = 44;
        for (auto &cls : i.sync.channelClasses) cls = 0;
        clockUS += 5000ULL;
    };
    refreshAll();

    // 1. tvPeer seeds blockAckTX via TID 0 RXBA, then goes idle (> 1.5s):
    {
        BlockAckRequest req0 = {};
        copyAddress(req0.peer, tvPeer);
        req0.tid = RadioState::DataTID;
        req0.dialog = 1; req0.window = 64; req0.immediate = true;
        AirportItlwmAWDLInterface::receiveBlockAck(&i, req0);
        i.serviceBlockAck();
        assert(!i.device.tokens.empty());
        i.completeUnicast(i.device.tokens.back(), true);
        assert(i.device.awdlRadio.blockAckTX.phase == TXBlockAckQueue::Requested);
        assert(sameAddress(i.device.awdlRadio.blockAckTX.peer, tvPeer));
    }
    auto &tx = i.device.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.epoch = i.device.dmaEpoch;
    tx.cookie = 7;
    tx.queue = 2;
    tx.phase = TXBlockAckQueue::Agreed;
    tx.enabled = true;
    tx.paused = false;
    tx.lastDataUS = clockUS;
    tx.lastHeavyDataUS = 0;

    // Advance > 1.6s: tvPeer is idle while ipadPeer queues frames!
    clockUS += 25ULL * 65536ULL;
    refreshAll();
    i.device.awdlRadio.dwell.running = true;
    i.device.awdlRadio.dwell.windowEndUS = clockUS + 55000ULL;
    i.device.awdlRadio.channel = 44;
    i.device.awdlRadio.widthMHz = 20;
    i.device.awdlRadio.queue = 1;

    // Enqueue 1 packet for ipadPeer: per-peer txIdle MUST still treat tvPeer's blockAckTX as idle (windowEndUS == 0)!
    i.enqueue(sizedEthernet(i.address.octet, ipadPeer, 1400));
    assert(i.pendingCount == 1);
    i.serviceTXBlockAck();
    assert(tx.windowEndUS == 0 && tx.paused);
    i.drain();

    // 2. Sidecar ipadPeer negotiates inbound QoS Video TID 4 RXBA while tvPeer sits idle in Agreed:
    //    completeUnicast MUST invoke rxTakeover (Retiring + releasePeer + handoverPeer == ipadPeer),
    //    whereas LLW TID 7 from iphonePeer MUST NOT touch blockAckTX!
    {
        ++i.device.baCookie;
        BlockAckRequest req7 = {};
        copyAddress(req7.peer, iphonePeer);
        req7.tid = RadioState::LLWTID;
        req7.dialog = 2; req7.window = 64; req7.immediate = true;
        AirportItlwmAWDLInterface::receiveBlockAck(&i, req7);
        i.serviceBlockAck();
        i.completeUnicast(i.device.tokens.back(), true);
        assert(tx.phase == TXBlockAckQueue::Agreed && !tx.releasePeer);

        ++i.device.baCookie;
        BlockAckRequest req4 = {};
        copyAddress(req4.peer, ipadPeer);
        req4.tid = 4; // QoS Video VI TID!
        req4.dialog = 3; req4.window = 64; req4.immediate = true;
        AirportItlwmAWDLInterface::receiveBlockAck(&i, req4);
        i.serviceBlockAck();
        i.completeUnicast(i.device.tokens.back(), true);
        assert(tx.phase == TXBlockAckQueue::Retiring && tx.releasePeer);
        assert(sameAddress(tx.handoverPeer, ipadPeer));
    }

    // 3. Outbound-only 16-frame burst when blockAckTX is Idle (!configured && !heavyReceiveActive)
    //    triggers Requested via pullOutputPackets() enqueue demand, and Asking holds Station 3 submissions via
    //    solicitDeadline even when dataQueue currently has queued > 0 (breaking the Catch-22 deadlock!):
    tx = {};
    i.device.awdlRadio.lastHeavyRXPayloadUS = 0; // !heavyReceiveActive
    for (unsigned n = 0; n < 16; ++n)
        i.bsd->output.push_back(sizedEthernet(i.address.octet, ipadPeer, 1400));
    assert(i.pullOutputPackets() == kIOReturnSuccess);
    assert(i.pendingCount == 16);
    i.serviceTXBlockAck();
    assert(tx.phase == TXBlockAckQueue::Requested && sameAddress(tx.peer, ipadPeer));

    tx.configured = true;
    tx.epoch = i.device.dmaEpoch;
    tx.cookie = 9;
    tx.queue = 2;
    tx.phase = TXBlockAckQueue::Asking;
    tx.paused = true;
    tx.window = 64;
    tx.dialog = 1;
    tx.attempts = 0;
    tx.nextAskUS = clockUS;
    i.device.awdlRadio.dataQueueConfigured = true;
    i.device.awdlRadio.dataQueue = 3;
    i.device.com.txq[3].queued = 2; // Non-empty Station 3 dataQueue!

    const size_t txBeforeDrain = i.device.dataFrames.size();
    i.drainPendingData();
    // solicitDeadline MUST pause new Station 3 submissions so dataQueue drains to 0!
    assert(i.device.dataFrames.size() == txBeforeDrain);
    assert(i.pendingCount == 16);

    // Once those 2 in-flight Station 3 descriptors finish, serviceTXBlockAck() immediately sends ADDBA Request!
    i.device.com.txq[3].queued = 0;
    const size_t mgmtBefore = i.device.tokens.size();
    i.serviceTXBlockAck();
    assert(i.device.tokens.size() == mgmtBefore + 1);

    i.drain();
    assert(!allocations);
    return 0;
}
""",
            extra_defs=("txReady",),
        )
        self.compile_run(tick_src)

        # HAL check: paused + end==0 fast-return in setAWDLTXWindow & 250 ms handover DELBA wait in requestAWDLBlockAckEnd.
        extra_ba = r"""
int main() {
    ItlIwx d;
    auto r = txPeer(d);
    d.tlcVersion = 4;
    prepareTX(d, r);
    agreeTX(d);
    auto &tx = d.awdlRadio.blockAckTX;
    assert(tx.paused);

    // 1. While paused and windowEnd == 0 (txIdle), setAWDLTXWindow returns 0 with ZERO ADD_STA or TLC RPCs!
    const size_t addsBefore = d.adds.size();
    const uint64_t tlcBefore = d.awdlRadio.blockAckTXStats.tlcConfigs;
    uint64_t zeroEnd = 0;
    assert(!d.setAWDLTXWindow(grant, &zeroEnd));
    assert(tx.paused && tx.windowEndUS == 0);
    assert(d.adds.size() == addsBefore && d.awdlRadio.blockAckTXStats.tlcConfigs == tlcBefore);

    // 2. Active RXBA session on station: normal requestAWDLBlockAckEnd sets 3.0s, while TX handover sets 250ms!
    unsigned b = start(d, r);
    (void)b;
    const unsigned idx = tx.station - AirportAWDL::BlockAckState::FirstStation;
    d.requestAWDLBlockAckEnd(tx.station, r.tid);
    assert(d.awdlBA.peers[idx].sessions[r.tid].endDeadlineUS == nowUS + 3000000ULL);

    tx.releasePeer = true;
    tx.retire(nowUS, true);
    d.requestAWDLBlockAckEnd(tx.station, r.tid);
    assert(d.awdlBA.peers[idx].sessions[r.tid].endDeadlineUS == nowUS + 250000ULL);

    assert(!d.clearAWDLBlockAck());
    return 0;
}
"""
        self.compile_run(p91._build_ba_harness(extra_ba))

    def test_p104_rcs2_sidecar_slots14_15_bridge_vs_slots6_8_clean_tune(self):
        tick_src = _tick_harness(r"""
int main() {
    clockUS = 10000000ULL;
    AirportItlwmAWDLInterface i;
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.ready = true;

    // Configure Sidecar Data + Infra Idle [149x6, 48, 48, 6, 149x5, 48, 48] with 149@80 wide slots:
    const uint16_t sidecar[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = sidecar[s];
        i.sync.channelClasses[s] = sidecar[s] == 149 ? 128 : 0;
    }
    i.sequenceConfigured = true;
    i.running = true;
    i.requested = true;
    i.device.awdlRadio.channel = 149;
    i.device.awdlRadio.widthMHz = 80;

    // Slot 6 ([48, 48] followed by Slot 8 Ch 6): bridgeInfraToWide MUST be false!
    clockUS = i.sync.epochUS + 6ULL * 65536ULL + 4000ULL;
    i.device.awdlRadio.channel = 149;
    i.device.awdlRadio.widthMHz = 80;
    i.tick();
    assert(!i.device.lastPolicy.bridgeInfraToWide);

    // Slot 8 ([6]): bridgeInfraToWide MUST be false!
    clockUS = i.sync.epochUS + 8ULL * 65536ULL + 4000ULL;
    i.device.awdlRadio.channel = 149;
    i.device.awdlRadio.widthMHz = 80;
    i.tick();
    assert(!i.device.lastPolicy.bridgeInfraToWide);

    // Slot 14 ([48, 48] followed by Slot 0 149@80): FIX-P116-2 keeps bridgeInfraToWide == false
    // so PHY 1 tunes cleanly to Ch 48 @ 20 MHz instead of parking blind on Ch 149!
    clockUS = i.sync.epochUS + 14ULL * 65536ULL + 4000ULL;
    i.device.awdlRadio.channel = 149;
    i.device.awdlRadio.widthMHz = 80;
    i.tick();
    assert(!i.device.lastPolicy.bridgeInfraToWide);
    assert(i.device.awdlRadio.channel == 48 && i.device.awdlRadio.widthMHz == 20);

    i.drain();
    assert(!allocations);
    return 0;
}
""")
        self.compile_run(tick_src)

    def test_p104_rcs3_tlv32_center_channel_normalization_and_tlv18_primary_merge(self):
        src = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include "AirportItlwm/AWDL/ActionFrame.hpp"

using namespace AirportAWDL;

static void put16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }

int main() {
    // 1. Standalone TLV 32 with IEEE 80/160 MHz center channels (155, 42, 50) normalizes to primary channels:
    {
        uint8_t f[128] = {};
        f[0] = 127; f[2] = 0x17; f[3] = 0xf2; f[4] = 8; f[5] = 0x10; f[6] = 3;
        size_t off = 16;
        f[off] = 32; put16(f + off + 1, 33); // 3 records * 11 bytes = 33
        uint8_t *r0 = f + off + 3;
        r0[0] = 2; // count - 1 = 2 -> 3 records
        put16(r0 + 2, 130); put16(r0 + 4, 155); r0[8] = 2; put16(r0 + 9, 0x3e3f); // slots 0..5, 9..13
        uint8_t *r1 = r0 + 11;
        put16(r1 + 2, 128); put16(r1 + 4, 42);  r1[8] = 2; put16(r1 + 9, 0x00c0); // slots 6..7
        uint8_t *r2 = r1 + 11;
        put16(r2 + 2, 129); put16(r2 + 4, 50);  r2[8] = 2; put16(r2 + 9, 0xc000); // slots 14..15
        off += 36;

        Advertisement ad = {};
        assert(parseAction(f, off, ad) == ParseResult::Valid);
        assert(ad.fields & Channels);
        for (unsigned s = 0; s < 6; ++s) assert(ad.sequence.channel[s] == 149);
        assert(ad.sequence.channel[6] == 44 && ad.sequence.channel[7] == 44);
        assert(ad.sequence.channel[14] == 48 && ad.sequence.channel[15] == 48);
    }

    // 2. Frame carrying BOTH TLV 18 (primary channels: 149, 48, 6) AND TLV 32 (center channels: 155@130,
    //    42@128 on slots 6-7 & 14-15, and omitting slot 8 = 0):
    //    MUST preserve TLV 18's exact primary channel 48 (NOT 44!) in the 36..48 block, adopt opClass 128/130,
    //    and preserve TLV 18's channel 6 on Slot 8 where TLV 32 left 0!
    {
        uint8_t f[192] = {};
        f[0] = 127; f[2] = 0x17; f[3] = 0xf2; f[4] = 8; f[5] = 0x10; f[6] = 0;
        size_t off = 16;

        // TLV 18 (encoding 3: [channel, opClass] pairs):
        const uint8_t sidecar[16] = {149,149,149,149,149,149,48,48,6,149,149,149,149,149,48,48};
        f[off] = 18; put16(f + off + 1, 38);
        uint8_t *s18 = f + off + 3;
        s18[0] = 15; s18[1] = 3; s18[3] = 3; put16(s18 + 4, 0xffff);
        for (unsigned s = 0; s < 16; ++s) {
            s18[6 + 2 * s] = sidecar[s];
            s18[7 + 2 * s] = sidecar[s] == 6 ? 81 : (sidecar[s] == 48 ? 115 : 124);
        }
        off += 41;

        // TLV 32 after TLV 18: center=155 (opClass 130) on 149 slots, center=42 (opClass 128) on 48 slots, slot 8 omitted!
        f[off] = 32; put16(f + off + 1, 22);
        uint8_t *r0 = f + off + 3;
        r0[0] = 1; // 2 records
        put16(r0 + 2, 130); put16(r0 + 4, 155); r0[8] = 2; put16(r0 + 9, 0x3e3f); // slots 0..5, 9..13
        uint8_t *r1 = r0 + 11;
        put16(r1 + 2, 128); put16(r1 + 4, 42);  r1[8] = 2; put16(r1 + 9, 0xc0c0); // slots 6..7, 14..15 (slot 8 = 0!)
        off += 25;

        Advertisement ad = {};
        assert(parseAction(f, off, ad) == ParseResult::Valid);
        assert(ad.fields & Channels);
        for (unsigned s = 0; s < 16; ++s) {
            assert(ad.sequence.channel[s] == sidecar[s]);
            if (sidecar[s] == 149) assert(ad.sequence.flags[s] == 130 || ad.sequence.flags[s] == 128);
            else if (sidecar[s] == 48) assert(ad.sequence.flags[s] == 128);
            else assert(ad.sequence.channel[s] == 6);
        }
    }
    return 0;
}
"""
        self.compile_run(src)

    def test_p104_rcs4_txrate_preserves_across_channel_hops_peer_ht_10s_and_fifo_retry(self):
        # Part A: TxRateControl preserves current rate across channel hops on same dmaEpoch + Peer::txHTMcs 10s lifetime.
        src = r"""
#include <cassert>
#include <cstdint>
#include "AirportItlwm/AWDL/PeerState.hpp"
#include "AirportItlwm/AWDL/TxRate.hpp"

using namespace AirportAWDL;

int main() {
    // 1. TxRateControl across 149 -> 48 -> 6 -> 149 hops on same dmaEpoch:
    TxRateControl rc = {};
    uint64_t serial = 0;
    rc.prepare(1, 149, 12, serial, 0xff);
    const uint8_t floorRate = rc.current;
    for (uint64_t tok = 1; tok <= 40; ++tok) {
        const uint8_t r = rc.select();
        rc.submitted(tok, r);
        rc.sample(tok, rc.epoch, r, true, 0, serial);
    }
    const uint8_t raisedRate = rc.current;
    assert((raisedRate & 0x80) && TxRateControl::halfMbps(raisedRate) > TxRateControl::halfMbps(floorRate));

    rc.prepare(1, 48, 12, serial, 0xff);
    assert(rc.current == raisedRate); // NOT wiped to 12 Mbps!
    rc.prepare(1, 6, 12, serial, 0xff);
    assert(rc.current == raisedRate);
    rc.prepare(1, 149, 12, serial, 0xff);
    assert(rc.current == raisedRate);

    // Narrower HT MCS mask clamps preserved rate down cleanly without wiping below maxHT:
    rc.prepare(1, 48, 12, serial, 0x0f);
    assert(rc.current == (0x80 | 3));

    // Changing dmaEpoch DOES reset back to floor:
    rc.prepare(2, 149, 12, serial, 0xff);
    assert(rc.current == floorRate);

    // 2. Peer::txHTMcs remains valid across 6.0s (> 3.0s ScheduleFreshnessUS) for an active unicast session (< 10.0s):
    Peer p = {};
    p.occupied = true;
    p.metadata.fields = HTCapabilities;
    p.metadata.rxHTMcs = 0xff;
    p.htCapabilitiesReceivedUS = 10000000ULL;
    p.lastUnicastDataUS = 15000000ULL;
    p.lastSeenUS = 16000000ULL;
    assert(p.txHTMcs(16000000ULL) == 0xff); // 6.0s since HT TLV, active session still live!
    assert(p.txHTMcs(26000001ULL) == 0);    // > 10.0s since htCapabilitiesReceivedUS / lastSeenUS -> expired!
    return 0;
}
"""
        self.compile_run(src)

        # Part B: Temporarily ineligible unicast frames preserve strict FIFO order in pending[].
        tick_src = _tick_harness(r"""
int main() {
    clockUS = 10000000ULL;
    const uint8_t ipad[6] = {2, 0, 0, 0, 0, 0x44};
    AirportItlwmAWDLInterface i;
    dataPeer(i, ipad);
    for (auto &c : i.sync.channels) c = 44;
    for (auto &cls : i.sync.channelClasses) cls = 0;
    i.sequenceConfigured = true;
    i.device.awdlRadio.listener = true;
    i.tick();
    clockUS = i.sync.epochUS + 5000ULL;
    i.tick();
    i.nextActionUS = i.nextMIFUS = clockUS + 10000000ULL;

    auto *p = i.device.awdlPeers.find(ipad, clockUS);
    assert(p);
    // Put peer temporarily on Ch 48 while Mac is on Ch 44 (ineligible!), plus a 2nd on-channel peer
    // OR multi-packet scan so keptCount > 0 exercises restoreFIFO():
    for (unsigned s = 0; s < 16; ++s) { p->sequence.channel[s] = 48; p->sequence.flags[s] = 115; }
    i.device.awdlRadio.ready = true;
    i.device.awdlRadio.channel = 44;
    i.device.awdlRadio.widthMHz = 20;
    i.device.awdlRadio.queue = 1;
    i.device.awdlRadio.dwell.running = true;
    i.device.awdlRadio.dwell.windowEndUS = clockUS + 55000ULL;

    // Enqueue 3 tagged RTP frames [0x11, 0x22, 0x33]:
    for (uint8_t tag : {uint8_t(0x11), uint8_t(0x22), uint8_t(0x33)}) {
        i.enqueue(sizedEthernet(i.address.octet, ipad, 200, tag));
    }
    assert(i.pendingCount == 3);

    // Run drainPendingData() while peer is ineligible:
    i.drainPendingData();
    assert(i.pendingCount == 3 && i.device.dataFrames.empty());

    // Now bring peer onto Ch 44 and disable software A-MSDU aggregation (metadata.fields = 0) so each frame transmits 1-by-1:
    p->metadata.fields = 0;
    for (unsigned s = 0; s < 16; ++s) { p->sequence.channel[s] = 44; p->sequence.flags[s] = 0; }
    p->lastSeenUS = p->syncReceivedUS = p->sequenceReceivedUS = p->electionV2ReceivedUS = clockUS;
    p->sync.phyTxTime = p->sync.targetTxTime = uint32_t(clockUS);
    p->sync.nextWindowTU = 64;
    i.sync.elect(i.device.awdlPeers, clockUS);
    for (auto &c : i.sync.channels) c = 44;
    for (auto &cls : i.sync.channelClasses) cls = 0;
    clockUS += 5000ULL;
    i.device.awdlRadio.channel = 44;
    i.device.awdlRadio.dwell.running = true;
    i.device.awdlRadio.dwell.windowEndUS = clockUS + 55000ULL;
    i.drainPendingData();
    assert(i.device.dataFrames.size() == 3);
    // Verify payload tag byte order is strictly [0x11, 0x22, 0x33]:
    std::vector<uint8_t> seen;
    for (const auto &frame : i.device.dataFrames) {
        // Locate our tag byte at the end-offset (frame size - (200 - 64) = frame.size() - 136):
        seen.push_back(frame[frame.size() - 136]);
    }
    assert((seen == std::vector<uint8_t>{0x11, 0x22, 0x33}));

    i.drain();
    assert(!allocations);
    return 0;
}
""")
        self.compile_run(tick_src)

    def test_p104_rcf1_llw_realtime_400us_close_lead_and_8ms_tid7_reorder_timeout(self):
        extra_radio = r"""
int main() {
    ItlIwx d;
    for (unsigned c : {6U, 48U, 149U}) {
        const unsigned band = c == 6 ? IEEE80211_CHAN_2GHZ : IEEE80211_CHAN_5GHZ;
        d.com.sc_ic.ic_channels[c] = {band, uint16_t(c == 6 ? 2437 : 5000 + 5 * c)};
        d.com.sc_ic.ic_chan_active[c / 8] |= 1U << (c % 8);
    }
    d.rlcVersion = 2;
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    d.com.sc_phyctxt[0].channel = &d.com.sc_ic.ic_channels[48];
    assert(!d.startAWDLRadio(self));

    // 1. llwRealtime == true with 0 queued Station 3 descriptors -> closeLeadUS == 400 us!
    RadioState::GrantPolicy pLLW = {};
    pLLW.widthMHz = 20;
    pLLW.slot = 2;
    pLLW.llwRealtime = true;
    const uint64_t end1 = clockUS + 65536ULL;
    assert(!d.serviceAWDLChannel(149, end1, pLLW));
    assert(d.awdlRadio.dwell.running);
    assert(d.awdlRadio.dwell.windowEndUS == end1 - 400ULL);

    // 2. llwRealtime == true: <= 2 QoS descriptors keep 400 us; > 2 QoS or legacy mgmt queued -> 1500 us!
    pLLW.dataQueue = true;
    pLLW.slot = 3;
    const uint64_t end2a = clockUS + 65536ULL;
    assert(!d.serviceAWDLChannel(149, end2a, pLLW));
    assert(d.awdlRadio.dataQueueConfigured);
    d.com.txq[d.awdlRadio.dataQueue].queued = 2;
    pLLW.slot = 4;
    const uint64_t end2b = clockUS + 65536ULL;
    assert(!d.serviceAWDLChannel(149, end2b, pLLW));
    assert(d.awdlRadio.dwell.windowEndUS == end2b - 400ULL);
    d.com.txq[d.awdlRadio.dataQueue].queued = 3;
    pLLW.slot = 5;
    const uint64_t end2c = clockUS + 65536ULL;
    assert(!d.serviceAWDLChannel(149, end2c, pLLW));
    assert(d.awdlRadio.dwell.windowEndUS == end2c - 1500ULL);
    d.com.txq[d.awdlRadio.dataQueue].queued = 0;

    // 3. llwRealtime == false -> closeLeadUS == 3000 us!
    RadioState::GrantPolicy pNormal = {};
    pNormal.widthMHz = 20;
    pNormal.slot = 6;
    const uint64_t end3 = clockUS + 65536ULL;
    assert(!d.serviceAWDLChannel(149, end3, pNormal));
    assert(d.awdlRadio.dwell.windowEndUS == end3 - 3000ULL);

    assert(!d.stopAWDLRadio());
    return 0;
}
"""
        self.compile_run(p91._build_radio_harness(extra_radio))

        # Verify 8 ms (8000 us) TID 7 reorder deadline vs 40 ms TID 0:
        extra_ba = r"""
int main() {
    ItlIwx d;
    d.awdlRadio.llwRealtime = true;
    auto r7 = request(d, 0, 7, 500);
    unsigned b7 = start(d, r7);
    auto p7 = packet(d, r7, 501, 1);
    receive(d, p7.get(), b7, 501, 500);
    assert(d.com.sc_rxba_data[b7].reorder_buf.reorder_timer->deadline == nowUS + 8000ULL);

    auto r0 = request(d, 0, 0, 600);
    unsigned b0 = start(d, r0);
    auto p0 = packet(d, r0, 601, 2);
    receive(d, p0.get(), b0, 601, 600);
    assert(d.com.sc_rxba_data[b0].reorder_buf.reorder_timer->deadline == nowUS + 40000ULL);

    assert(!d.clearAWDLBlockAck());
    return 0;
}
"""
        self.compile_run(p91._build_ba_harness(extra_ba))

    def test_p104_rce1_ambient_psf_allows_idle_unbind_and_active_unicast_parks_0slots_on_infra(self):
        tick_src = _tick_harness(
            r"""
int main() {
    clockUS = 10000000ULL;
    const uint8_t ambientTV[6] = {2, 0, 0, 0, 0, 0x77};
    AirportItlwmAWDLInterface i;
    dataPeer(i, ambientTV);
    auto *p = i.device.awdlPeers.find(ambientTV, clockUS);
    assert(p);
    p->nativeCached = false;
    p->lastUnicastDataUS = 0; // Ambient PSF/MIF broadcasts ONLY!

    i.device.awdlRadio.listener = true;
    i.device.awdlRadio.binding = true;
    unsigned unbindTicks = 0, boundTicks = 0, parkInfraTicks = 0;
    i.device.onService = [&]() {
        if (i.device.lastPolicy.parkIdleOnInfra) ++parkInfraTicks;
        if (i.device.lastPolicy.unbindIdle) {
            i.device.awdlRadio.binding = false;
            ++unbindTicks;
        } else {
            i.device.awdlRadio.binding = true;
            ++boundTicks;
        }
    };

    const uint16_t lowPower[16] = {48, 0, 149, 0, 0, 0, 0, 0, 6, 0, 149, 0, 0, 0, 0, 0};
    for (unsigned s = 0; s < 16; ++s) {
        i.sync.channels[s] = lowPower[s];
        i.sync.channelClasses[s] = 0;
    }
    i.sequenceConfigured = true;
    i.tick();

    // 1. While associated on en1 (ns_nstate == IEEE80211_S_RUN), PHY 1 stays bound with zero
    //    idleUnbinds and parks all twelve 0-slots on en1's infra channel even without active unicast:
    unbindTicks = boundTicks = parkInfraTicks = 0;
    for (unsigned step = 0; step < 16; ++step) {
        clockUS += 65536ULL;
        if ((step % 8) == 0) p->lastSeenUS = clockUS;
        i.tick();
    }
    assert(unbindTicks == 0 && boundTicks == 16 && parkInfraTicks == 12);

    // 2. When unassociated (ns_nstate != IEEE80211_S_RUN), ambient PSF/MIF broadcasts MUST NOT block
    //    idle unbind, while active unicast (lastUnicastDataUS < 5.0s) rebinds immediately:
    i.device.com.ns_nstate = 0;
    for (unsigned step = 0; step < 56; ++step) {
        clockUS += 65536ULL;
        if ((step % 8) == 0) p->lastSeenUS = clockUS;
        i.tick();
    }
    unbindTicks = boundTicks = parkInfraTicks = 0;
    for (unsigned step = 0; step < 64; ++step) {
        clockUS += 65536ULL;
        if ((step % 8) == 0) p->lastSeenUS = clockUS;
        i.tick();
    }
    assert(parkInfraTicks == 0);
    assert(unbindTicks >= 58 && boundTicks <= 6);

    p->lastSeenUS = p->lastUnicastDataUS = clockUS;
    i.device.com.ns_nstate = IEEE80211_S_RUN;
    unbindTicks = boundTicks = parkInfraTicks = 0;
    for (unsigned step = 0; step < 16; ++step) {
        clockUS += 65536ULL;
        p->lastSeenUS = clockUS;
        i.tick();
    }
    assert(unbindTicks == 0 && boundTicks == 16);
    assert(parkInfraTicks == 12); // All twelve 0-slots in Low Power parked on infra!

    i.drain();
    assert(!allocations);
    return 0;
}
""",
            extra_defs=("continuityMediaActive",),
        ).replace(
            "void tick();",
            "void tick();\n    bool continuityMediaActive() const;",
        ).replace(
            "Radio() {awdlRadio.widthMHz=20;}",
            "bool rxBA=false;\n    bool awdlHasRXBA() const {return rxBA;}\n    Radio() {awdlRadio.widthMHz=20;}",
        )
        self.compile_run(tick_src)


if __name__ == "__main__":
    unittest.main()
