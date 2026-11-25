"""Continuity Camera delivery through the production AWDL RX reorder path."""
import unittest

import test_awdl_preview91 as support
import test_regressions


class LLWAggregationRegressions(unittest.TestCase):
    compile_run = test_regressions.Regressions.compile_run

    def test_bar_leaves_partial_amsdu_until_its_last_subframe(self):
        self.compile_run(support._build_ba_harness(r"""
int main() {
    ItlIwx d;
    const auto r = request(d, 0, 7, 100);
    const unsigned baid = start(d, r);
    auto first = packet(d, r, 101, 1);
    auto last = packet(d, r, 101, 2);
    auto following = packet(d, r, 102, 3);
    receive(d, first.get(), baid, 101, 100, 0, false);
    receive(d, following.get(), baid, 102, 100);

    Packet bar;
    bar.bytes.resize(20);
    bar.bytes[0] = 0x84;
    copyAddress(bar.bytes.data() + 4, d.awdlRadio.address);
    copyAddress(bar.bytes.data() + 10, r.peer);
    putLE16(bar.bytes.data() + 16, (r.tid << 12) | 4);
    putLE16(bar.bytes.data() + 18, 101 << 4);
    iwx_rx_mpdu_desc desc = {};
    desc.reorder_data = uint32_t(IWX_RX_REORDER_DATA_INVALID_BAID) << IWX_RX_MPDU_REORDER_BAID_SHIFT;
    mbuf_list ml;
    assert(d.receiveAWDLReordered(&bar, &desc, &ml));
    assert(bar.frees && ml_empty(&ml));
    assert(d.delivered.empty() && !first->frees && !following->frees);
    assert(d.com.sc_rxba_data[baid].reorder_buf.head_sn == 101);

    receive(d, last.get(), baid, 101, 103, 1, true);
    assert((d.delivered == std::vector<uint16_t>{1, 2, 3}));
    assert(first->frees && last->frees && following->frees);
    assert(!d.clearAWDLBlockAck());
}
"""))

    def test_llw_retry_keeps_distinct_subframes_but_drops_duplicate_mpdu(self):
        self.compile_run(support._build_ba_harness(r"""
int main() {
    ItlIwx d;
    const auto r = request(d, 0, 7, 100);
    const unsigned baid = start(d, r);
    d.awdlInput = [](void *ctx, const DataView &frame) {
        assert(frame.lowLatency && frame.tid == 7);
        static_cast<ItlIwx *>(ctx)->delivered.push_back(frame.payload[39]);
    };
    auto subframe = [&](uint8_t tag) {
        auto m = packet(d, r, 100, 0);
        m->bytes.resize(74);
        auto bytes = m->bytes.data();
        bytes[1] = 0x08;
        memcpy(bytes + 26, LLW_SNAP, 8);
        memset(bytes + 34, 0, 40);
        bytes[34] = 0x60;
        bytes[40] = 59;
        bytes[73] = tag;
        return m;
    };
    auto first = subframe(1), second = subframe(2), repeated = subframe(2);
    receive(d, first.get(), baid, 100, 101, 0, false);
    receive(d, second.get(), baid, 100, 101, 1, true);
    assert((d.delivered == std::vector<uint16_t>{1, 2}));
    receive(d, repeated.get(), baid, 100, 101, 1, true);
    assert(d.delivered.size() == 2 && repeated->frees);
    assert(!d.clearAWDLBlockAck());
}
"""))

    def test_software_amsdu_retry_is_checked_once_per_mpdu(self):
        self.compile_run(support._build_ba_harness(r"""
int main() {
    ItlIwx d;
    const auto r = request(d, 0, 7, 100);
    auto m = packet(d, r, 100, 0);
    // Each subframe uses a 14-byte header and 48-byte SNAP body.
    m->bytes.resize(26 + 64 + 62);
    auto bytes = m->bytes.data();
    bytes[1] = 0x08;
    bytes[24] |= 0x80;
    for (unsigned n = 0; n < 2; ++n) {
        auto subframe = bytes + 26 + n * 64;
        memset(subframe, 0, 62);
        copyAddress(subframe, d.awdlRadio.address);
        copyAddress(subframe + 6, r.peer);
        putBE16(subframe + 12, 48);
        memcpy(subframe + 14, LLW_SNAP, 8);
        subframe[22] = 0x60;
        subframe[28] = 59;
        subframe[61] = n + 1;
    }
    auto deliver = [](void *ctx, const DataView &frame) {
        static_cast<ItlIwx *>(ctx)->delivered.push_back(frame.payload[39]);
    };
    auto feed = [&] {
        assert(d.awdlData.receive(bytes, m->bytes.size(), d.awdlPeers, d.awdlRadio.address,
                                 nowUS, false, deliver, &d));
    };
    feed();
    assert((d.delivered == std::vector<uint16_t>{1, 2}));
    feed();
    assert(d.delivered.size() == 2 && d.awdlData.duplicate == 2);
}
"""))
