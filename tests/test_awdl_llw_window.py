"""LLW receive deadlines and the repeated 48-channel slots used by Continuity Camera."""
import unittest

import test_awdl_preview91 as fixtures
import test_regressions


class LLWWindowRegressions(unittest.TestCase):
    compile_run = test_regressions.Regressions.compile_run

    def test_parent_clock_refresh_inside_same_radio_window(self):
        self.compile_run(fixtures._build_radio_harness(r"""
int main() {
    ItlIwx d;
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    assert(!d.startAWDLRadio(self));
    AirportAWDL::SyncEngine sync = {};
    sync.periodTU = 16;
    sync.presence = 4;
    for (unsigned i = 0; i < 16; ++i) sync.channels[i] = i % 8 == 2 ? 149 : 48;
    sync.epochCounter = 12;
    sync.epochUS = clockUS;
    auto update = [&]() {
        AirportAWDL::Window w = {};
        assert(sync.window(clockUS, w));
        RadioState::GrantPolicy policy = {};
        policy.llwRealtime = true;
        policy.widthMHz = w.widthMHz();
        policy.slot = w.runStartEAW;
        assert(!d.serviceAWDLChannel(w.channel, clockUS + w.untilChannelChangeUS, policy));
        assert(d.awdlRadio.dwell.running && d.awdlRadio.dwell.slot == policy.slot);
    };
    update();
    const auto started = d.awdlRadio.dwellStarts;
    const auto ends = d.awdlRadio.dwellEnds;
    const auto endUS = d.awdlRadio.dwell.eawEndUS;
    clockUS += 65536;
    sync.epochCounter = 16;
    sync.epochUS = clockUS;
    update();
    assert(d.awdlRadio.dwell.eawEndUS == endUS);
    assert(d.awdlRadio.llwWindowContinuations == 1);
    assert(d.awdlRadio.dwellStarts == started && d.awdlRadio.dwellEnds == ends);
    clockUS = endUS + 1000;
    update();
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.dwellEnds == ends + 1);
    // Return to the same channel after missing a full schedule cycle.
    clockUS += 16 * 65536;
    update();
    assert(d.awdlRadio.dwellEnds == ends + 2);
    assert(!d.stopAWDLRadio());
}
"""))

    def test_filled_gap_releases_even_when_firmware_cursor_lags(self):
        self.compile_run(fixtures._build_ba_harness(r"""
int main() {
    ItlIwx d;
    auto r = request(d, 0, 7, 4095);
    unsigned b = start(d, r);
    auto following = packet(d, r, 0, 2);
    auto gap = packet(d, r, 4095, 1);
    receive(d, following.get(), b, 0, 4094);
    assert(d.delivered.empty());
    receive(d, gap.get(), b, 4095, 4094);
    assert((d.delivered == std::vector<uint16_t>{1, 2}));
    auto &rx = d.com.sc_rxba_data[b];
    assert(rx.reorder_buf.head_sn == 1 && !rx.reorder_buf.num_stored);
    // A delayed firmware release within the negotiated window keeps the current head.
    mbuf_list ml;
    d.iwx_release_frames(&d.com, nullptr, &rx, &rx.reorder_buf, 4090, &ml);
    assert(rx.reorder_buf.head_sn == 1);
    assert(!d.clearAWDLBlockAck());
}
"""))

    def test_expired_gap_does_not_force_new_partial_aggregate(self):
        self.compile_run(fixtures._build_ba_harness(r"""
int main() {
    ItlIwx d;
    const auto r = request(d, 0, 7, 100);
    const unsigned b = start(d, r);
    auto queued = packet(d, r, 101, 1);
    auto first = packet(d, r, 102, 2);
    auto last = packet(d, r, 102, 3);
    receive(d, queued.get(), b, 101, 100);
    nowUS += 7000;
    receive(d, first.get(), b, 102, 100, 0, false);
    nowUS += 1000;
    auto &buf = d.com.sc_rxba_data[b].reorder_buf;
    ItlIwx::iwx_reorder_timer_expired(&buf);
    assert((d.delivered == std::vector<uint16_t>{1}));
    assert(buf.head_sn == 102 && buf.num_stored == 1);
    receive(d, last.get(), b, 102, 103, 1, true);
    assert((d.delivered == std::vector<uint16_t>{1, 2, 3}));
    assert(!d.clearAWDLBlockAck());
}
"""))

    def test_first_buffered_partial_aggregate_has_a_deadline(self):
        self.compile_run(fixtures._build_ba_harness(r"""
int main() {
    ItlIwx d;
    const auto r = request(d, 0, 7, 100);
    const unsigned b = start(d, r);
    auto partial = packet(d, r, 101, 1);
    receive(d, partial.get(), b, 101, 100, 0, false);
    auto &buf = d.com.sc_rxba_data[b].reorder_buf;
    assert(d.delivered.empty() && buf.head_sn == 100);
    assert(buf.reorder_timer->deadline == nowUS + 8000);
    nowUS += 8000;
    ItlIwx::iwx_reorder_timer_expired(&buf);
    assert(d.delivered.size() == 1 && !buf.num_stored && buf.head_sn == 102);
    assert(d.awdlBA.llwGapExpiries == 1);
    auto late = packet(d, r, 100, 2);
    receive(d, late.get(), b, 100, 102);
    assert(d.awdlBA.llwBelowWindow == 1 && d.delivered.size() == 1);
    assert(!d.clearAWDLBlockAck());
}
"""))
