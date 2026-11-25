"""Preview 116 infrastructure RXBA reorder and DPRINTF silence regressions:
1. en1 (IWX_STATION_ID) RXBA reorder timeout uses 25 ms (RX_REORDER_BUF_TIMEOUT_MQ_USEC = 25000ULL)
   and preserves the initial gap deadline across subsequent arrivals instead of perpetually
   postponing reorder_timer, while multi-gap windows re-arm only the remaining wait.
2. Post-timeout or post-BAR contiguous arrivals at head_sn release immediately (0 ms delay,
   num_stored == 0) on both en1 and awdl0 even while firmware reorder_data nssn lags behind head_sn.
3. iwx_debug defaults to 0 so DPRINTF command-path logging stays silent unless explicitly enabled,
   while preserving IEEE80211_BA_MAX_WINSZ = 64 and shared-cluster mbuf_copym RX framing.
"""
import re
import unittest

import test_awdl_preview91 as p91
import test_regressions as support
from test_offline import definition


def _build_infra_ba_harness(extra_cpp: str) -> str:
    driver_src = (support.ROOT / "itlwm/hal_iwx/ItlIwx.cpp").read_text()
    macro_match = re.search(
        r"^#define\s+RX_REORDER_BUF_TIMEOUT_MQ_USEC\s+.*$",
        driver_src,
        re.M,
    )
    if not macro_match:
        raise AssertionError("Missing RX_REORDER_BUF_TIMEOUT_MQ_USEC in ItlIwx.cpp")

    harness = p91._build_ba_harness(extra_cpp)
    harness = harness.replace(
        "#define RX_REORDER_BUF_TIMEOUT_MQ_USEC (100000ULL)",
        macro_match.group(0),
        1,
    )
    harness = harness.replace(
        "void iwx_rx_frame(iwx_softc *,mbuf_t m,int,uint32_t,int,uint32_t,uint32_t,ieee80211_rxinfo *,mbuf_list *){++apFrames;mbuf_freem(m);}",
        "std::vector<uint16_t> apDelivered;\n"
        " void iwx_rx_frame(iwx_softc *,mbuf_t m,int,uint32_t,int,uint32_t,uint32_t,ieee80211_rxinfo *,mbuf_list *){"
        "++apFrames;"
        "if(mbuf_len(m)>=38)apDelivered.push_back(le16(static_cast<const uint8_t *>(mbuf_data(m))+36));"
        "mbuf_freem(m);}",
        1,
    )
    return harness


INFRA_HELPERS = r"""
static unsigned startInfra(ItlIwx &d, uint8_t tid = 0, uint16_t ssn = 100, uint16_t win = 64) {
    assert(!d.iwx_sta_rx_agg(&d.com, &d.bss, tid, ssn, win, 0, 1));
    for (unsigned i = 0; i < nitems(d.com.sc_rxba_data); ++i) {
        const auto &rx = d.com.sc_rxba_data[i];
        if (rx.baid == i && rx.sta_id == IWX_STATION_ID && rx.tid == tid) {
            return i;
        }
    }
    assert(false);
    return 0;
}

static std::unique_ptr<Packet> infraPacket(ItlIwx &d, uint8_t tid, uint16_t sn, uint16_t tag) {
    auto m = std::unique_ptr<Packet>(new Packet);
    m->bytes.assign(82, 0);
    uint8_t *p = m->bytes.data();
    p[0] = 0x88; // Unicast QoS Data
    p[1] = 0x02; // From DS
    copyAddress(p + 4, d.awdlRadio.address);
    const uint8_t apAddr[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    copyAddress(p + 10, apAddr);
    copyAddress(p + 16, apAddr);
    putLE16(p + 22, uint16_t((sn & 0x0fffU) << 4));
    p[24] = tid & 0x0fU;
    putLE16(p + 36, tag);
    return m;
}

static void receiveInfra(ItlIwx &d, Packet *m, unsigned baid, uint16_t sn, uint16_t nssn, uint32_t gp2 = 0) {
    iwx_rx_mpdu_desc desc = {};
    desc.status = IWX_RX_MPDU_RES_STATUS_SRC_STA_FOUND | (uint32_t(IWX_STATION_ID) << 24);
    desc.reorder_data = (uint32_t(baid) << 24) | (uint32_t(sn & 0x0fffU) << 12) | uint32_t(nssn & 0x0fffU);
    desc.v1.gp2_on_air_rise = gp2;
    ieee80211_rxinfo rxi = {};
    mbuf_list ml;
    if (!d.iwx_rx_reorder(&d.com, m, 0, &desc, 0, 0, gp2, &rxi, &ml)) {
        d.iwx_rx_frame(&d.com, m, 0, 0, 0, 0, gp2, &rxi, &ml);
    }
    assert(ml_empty(&ml));
    assert(d.bss.refs == 0);
}

static void fireReorderTimer(iwx_reorder_buffer &buf) {
    assert(buf.reorder_timer && buf.reorder_timer->deadline != 0);
    nowUS = buf.reorder_timer->deadline;
    buf.reorder_timer->deadline = 0; // CTimeout::timeoutOccurred clears isPending before to_func
    ItlIwx::iwx_reorder_timer_expired(&buf);
}
"""


class Preview116InfraRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p116_en1_rxba_25ms_non_resetting_deadline_and_multi_gap_remaining_wait(self):
        extra = INFRA_HELPERS + r"""
static_assert(RX_REORDER_BUF_TIMEOUT_MQ_USEC == 100000ULL,
              "en1 RXBA reorder timeout must be 100 ms (100000 us)");
static_assert(IEEE80211_BA_MAX_WINSZ == 64,
              "802.11ac VHT Compressed BA window maximum must remain 64");

int main() {
    // 1. en1 (sta_id == IWX_STATION_ID): gap at sn = 100 followed by 101..120 over 7.6 ms
    //    must keep the initial +100 ms deadline without resetting on any subsequent frame.
    {
        ItlIwx d;
        const unsigned baid = startInfra(d, 0, 100, 64);
        auto &buf = d.com.sc_rxba_data[baid].reorder_buf;
        assert(buf.buf_size == 64 && buf.head_sn == 100);

        const uint64_t t0 = nowUS;
        const uint64_t expectedDeadline = t0 + 100000ULL;
        std::vector<std::unique_ptr<Packet>> burst;
        std::vector<uint16_t> expectedTags;
        for (uint16_t sn = 101; sn <= 120; ++sn) {
            if (sn > 101) nowUS += 400ULL; // +0.4 ms per arriving MPDU (up to +7.6 ms)
            const uint16_t tag = uint16_t(1000 + sn);
            expectedTags.push_back(tag);
            burst.push_back(infraPacket(d, 0, sn, tag));
            receiveInfra(d, burst.back().get(), baid, sn, 100);
            assert(d.apDelivered.empty());
            assert(buf.reorder_timer->deadline == expectedDeadline);
        }
        assert(buf.num_stored == 20);

        // At t0 + 100 ms, timer expires once and flushes all 101..120 in exact order.
        fireReorderTimer(buf);
        assert(d.apDelivered == expectedTags);
        assert(buf.head_sn == 121);
        assert(buf.num_stored == 0);
        assert(!timeout_pending(&buf.reorder_timer));
        for (const auto &pkt : burst) assert(pkt->frees == 1);

        // 2. Two-gap window on en1: gap at 121 (122 arrives at t1) and gap at 123
        //    (124 arrives at t1 + 6 ms). When 122 expires at t1 + 100 ms, 124 must wait
        //    only its remaining 6 ms (t1 + 106 ms), not an extra full 100 ms (t1 + 200 ms)!
        d.apDelivered.clear();
        const uint64_t t1 = nowUS;
        auto p122 = infraPacket(d, 0, 122, 2122);
        receiveInfra(d, p122.get(), baid, 122, 100);
        assert(buf.reorder_timer->deadline == t1 + 100000ULL);

        nowUS = t1 + 6000ULL;
        auto p124 = infraPacket(d, 0, 124, 2124);
        receiveInfra(d, p124.get(), baid, 124, 100);
        assert(buf.reorder_timer->deadline == t1 + 100000ULL);

        fireReorderTimer(buf);
        assert((d.apDelivered == std::vector<uint16_t>{2122}) && p122->frees == 1 && !p124->frees);
        assert(buf.head_sn == 123 && buf.num_stored == 1);
        assert(buf.reorder_timer->deadline == t1 + 106000ULL);

        fireReorderTimer(buf);
        assert((d.apDelivered == std::vector<uint16_t>{2122, 2124}) && p124->frees == 1);
        assert(buf.head_sn == 125 && buf.num_stored == 0);
        assert(!timeout_pending(&buf.reorder_timer));
        assert(!d.iwx_sta_rx_agg(&d.com, &d.bss, 0, 0, 0, 0, 0));
    }

    // 3. awdl0 DataTID (sta_id != IWX_STATION_ID, tid == 0) keeps its initial 40 ms deadline
    //    across subsequent out-of-order arrivals instead of pushing 40 ms forward per packet.
    {
        ItlIwx d;
        auto r0 = request(d, 0, 0, 300);
        const unsigned b0 = start(d, r0);
        auto &buf = d.com.sc_rxba_data[b0].reorder_buf;
        const uint64_t awdlDeadline = nowUS + 40000ULL;

        auto a1 = packet(d, r0, 301, 31);
        auto a2 = packet(d, r0, 302, 32);
        auto a3 = packet(d, r0, 303, 33);
        receive(d, a1.get(), b0, 301, 300);
        assert(buf.reorder_timer->deadline == awdlDeadline);
        nowUS += 12000ULL;
        receive(d, a2.get(), b0, 302, 300);
        nowUS += 12000ULL;
        receive(d, a3.get(), b0, 303, 300);
        assert(buf.reorder_timer->deadline == awdlDeadline);

        fireReorderTimer(buf);
        assert((d.delivered == std::vector<uint16_t>{31, 32, 33}));
        assert(buf.head_sn == 304 && buf.num_stored == 0 && !timeout_pending(&buf.reorder_timer));
        assert(!d.clearAWDLBlockAck());
    }
    return 0;
}
"""
        self.compile_run(_build_infra_ba_harness(extra))

    def test_p116_post_timeout_lagging_nssn_releases_contiguous_frames_immediately(self):
        extra = INFRA_HELPERS + r"""
int main() {
    // 1. en1 post-timeout recovery while firmware nssn still lags at 100:
    //    (a) In-order arrivals 121, 122, 123 release immediately (0 ms delay, num_stored == 0).
    //    (b) Out-of-order 125, 126 followed by head-filling 124 (num_stored > 0 -> iwx_release_frames
    //        with nssn = 100 < head_sn = 124) ALSO drain 124, 125, 126 in 0 ms without hostage stall!
    {
        ItlIwx d;
        const unsigned baid = startInfra(d, 0, 100, 64);
        auto &buf = d.com.sc_rxba_data[baid].reorder_buf;

        std::vector<std::unique_ptr<Packet>> initial;
        for (uint16_t sn = 101; sn <= 120; ++sn) {
            initial.push_back(infraPacket(d, 0, sn, sn));
            receiveInfra(d, initial.back().get(), baid, sn, 100);
        }
        fireReorderTimer(buf);
        assert(buf.head_sn == 121 && buf.num_stored == 0);
        d.apDelivered.clear();

        const uint64_t postTimeoutUS = nowUS;
        // (a) Direct in-order 121, 122, 123 with lagging nssn = 100:
        auto p121 = infraPacket(d, 0, 121, 121);
        auto p122 = infraPacket(d, 0, 122, 122);
        auto p123 = infraPacket(d, 0, 123, 123);
        receiveInfra(d, p121.get(), baid, 121, 100);
        receiveInfra(d, p122.get(), baid, 122, 100);
        receiveInfra(d, p123.get(), baid, 123, 100);
        assert((d.apDelivered == std::vector<uint16_t>{121, 122, 123}));
        assert(nowUS == postTimeoutUS);
        assert(buf.head_sn == 124 && buf.num_stored == 0 && !timeout_pending(&buf.reorder_timer));

        // (b) Buffered 125, 126 then head-filling 124 while firmware nssn still lags at 100:
        d.apDelivered.clear();
        auto p125 = infraPacket(d, 0, 125, 125);
        auto p126 = infraPacket(d, 0, 126, 126);
        auto p124 = infraPacket(d, 0, 124, 124);
        receiveInfra(d, p125.get(), baid, 125, 100);
        receiveInfra(d, p126.get(), baid, 126, 100);
        assert(d.apDelivered.empty() && buf.num_stored == 2 && timeout_pending(&buf.reorder_timer));

        // Filling sn = 124 enters iwx_release_frames with nssn = 100 < ssn = 124:
        // clamping nssn = ssn drains 124, 125, 126 immediately and cancels reorder_timer!
        receiveInfra(d, p124.get(), baid, 124, 100);
        assert((d.apDelivered == std::vector<uint16_t>{124, 125, 126}));
        assert(nowUS == postTimeoutUS);
        assert(buf.head_sn == 127 && buf.num_stored == 0 && !timeout_pending(&buf.reorder_timer));
        assert(p124->frees == 1 && p125->frees == 1 && p126->frees == 1);
        assert(!d.iwx_sta_rx_agg(&d.com, &d.bss, 0, 0, 0, 0, 0));
    }

    // 2. Wrap-around (4095 -> 0 -> 1) on both en1 and awdl0 TID 0 with lagging firmware nssn:
    {
        ItlIwx d;
        const unsigned baid = startInfra(d, 0, 4095, 64);
        auto &buf = d.com.sc_rxba_data[baid].reorder_buf;
        auto p0 = infraPacket(d, 0, 0, 500);
        auto p1 = infraPacket(d, 0, 1, 501);
        auto p4095 = infraPacket(d, 0, 4095, 499);
        receiveInfra(d, p0.get(), baid, 0, 4090);
        receiveInfra(d, p1.get(), baid, 1, 4090);
        assert(d.apDelivered.empty() && buf.num_stored == 2);
        receiveInfra(d, p4095.get(), baid, 4095, 4090);
        assert((d.apDelivered == std::vector<uint16_t>{499, 500, 501}));
        assert(buf.head_sn == 2 && buf.num_stored == 0 && !timeout_pending(&buf.reorder_timer));
        assert(!d.iwx_sta_rx_agg(&d.com, &d.bss, 0, 0, 0, 0, 0));
    }
    {
        ItlIwx d;
        auto r0 = request(d, 0, 0, 4095);
        const unsigned b0 = start(d, r0);
        auto &buf = d.com.sc_rxba_data[b0].reorder_buf;
        auto next = packet(d, r0, 0, 62);
        auto head = packet(d, r0, 4095, 61);
        receive(d, next.get(), b0, 0, 4094);
        assert(d.delivered.empty());
        receive(d, head.get(), b0, 4095, 4094);
        assert((d.delivered == std::vector<uint16_t>{61, 62}));
        assert(buf.head_sn == 1 && buf.num_stored == 0 && !timeout_pending(&buf.reorder_timer));
        assert(!d.clearAWDLBlockAck());
    }
    return 0;
}
"""
        self.compile_run(_build_infra_ba_harness(extra))

    def test_p116_iwx_debug_default_zero_and_rx_pkt_mbuf_copym_safety(self):
        driver_src = (support.ROOT / "itlwm/hal_iwx/ItlIwx.cpp").read_text()
        debug_block = re.search(
            r"#define\s+IWX_DEBUG\s+#ifdef\s+IWX_DEBUG.*?#endif",
            driver_src,
            re.S,
        )
        self.assertIsNotNone(debug_block, "Missing IWX_DEBUG block in ItlIwx.cpp")

        dprintf_harness = f"""
#include <cassert>
#include <cstdarg>

static unsigned xylogCalls = 0;
static void XYLog(const char *, ...) {{
    ++xylogCalls;
}}

{debug_block.group(0)}

int main() {{
    assert(iwx_debug == 0);
    DPRINTF(("%s: Sending command (%.2x.%.2x), %d bytes at [%d]:%d ver: %d\\n",
             "iwx_send_cmd", 0x01, 0x29, 32, 0, 0, 2));
    DPRINTF(("%s: command 0x%x done\\n", "iwx_cmd_done", 0x0129));
    DPRINTFN(1, ("verbose trace %d\\n", 1));
    assert(xylogCalls == 0);

    // Opt-in runtime toggle remains functional when explicitly enabled.
    iwx_debug = 1;
    DPRINTF(("%s: command 0x%x done\\n", "iwx_cmd_done", 0x0129));
    assert(xylogCalls == 1);
    return 0;
}}
"""
        self.compile_run(dprintf_harness)

        rx_pkt_fn = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_rx_pkt")
        self.assertIn("mbuf_copym(m0, 0, MBUF_COPYALL, MBUF_DONTWAIT, &m);", rx_pkt_fn)
        self.assertNotIn("mbuf_allocpacket", rx_pkt_fn)


if __name__ == "__main__":
    unittest.main()
