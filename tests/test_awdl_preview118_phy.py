"""Preview 118 PHY/HAL/net80211 throughput, RX BA stability, and scan-abort regressions:
1. Station 0 singleton TVQM TX setup (tid_disable_tx == 0xffff, no OTA ADDBA_REQ, no
   IEEE80211_C_AMSDU_IN_AMPDU), non-falling-through ni_chw FAT_EN width flags, peer
   ni_ampdu_param MPDU density, 1024-byte bc_tbl DMA alignment, and [256..319] bc_tbl wrap
   mirroring on Gen2/QuZ.
2. Station 0 100 ms RX_REORDER_BUF_TIMEOUT_MQ_USEC (holding reorder window across 16-64 ms
   AWDL dwells while keeping AWDL 40 ms and LLW 8 ms intact) and iwx_rx_ba_session_expired
   honoring negotiated rxba->timeout instead of firing premature DELBA after 10 ms.
3. ItlIwx::abortScanning() issuing firmware iwx_scan_abort when IWX_FLAG_SCANNING/BGSCAN is
   active and clearing flags, plus unfragmented >= 110 TU passive dwell on 5 GHz directed
   PASSIVE2ACTIVE scans.
4. AIRPORT_VTD partial bus_dmamap_read bounce sync in iwx_rx_pkt (preserving mbuf_copym on
   coalesced multi-MPDU buffers) and !ml_empty(&ml) guard in iwx_notif_intr.
5. if_snd 256-packet cap in ieee80211_ifattach, inGate() fast-path in _mbuf.cpp if_input, and
   in-place +ETHER_ALIGN payload shift in ieee80211_decap on unshared single-segment MBUF_EXT
   clusters with m_dup_pkt fallback for shared/chained mbufs.
"""
import re
import unittest

import test_awdl_preview116_infra as p116
import test_regressions as support
from iwx_firmware import declarations as firmware_declarations
from test_offline import definition


class Preview118PhyRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p118_station0_singleton_tvqm_and_add_sta_chw_mpdu_density_bc_tbl(self):
        iwx_src = (support.ROOT / "itlwm/hal_iwx/ItlIwx.cpp").read_text()
        attach_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_attach")
        self.assertNotIn("IEEE80211_C_AMSDU_IN_AMPDU", attach_src)
        self.assertNotIn("iwx_sta_tx_agg", iwx_src)

        alloc_ring_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_alloc_tx_ring")
        enable_txq_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_tvqm_enable_txq")
        for fn_src in (alloc_ring_src, enable_txq_src):
            self.assertIn("sizeof(struct iwx_gen3_bc_tbl), 1024)", fn_src)
            self.assertIn("sizeof(struct iwx_agn_scd_bc_tbl), 1024)", fn_src)

        add_sta_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_add_sta_cmd")
        bc_tbl_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_tx_update_byte_tbl")

        harness = r"""
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#define XYLog(...) ((void)0)
#define DPRINTF(x) ((void)0)
#define htole16(x) uint16_t(x)
#define htole32(x) uint32_t(x)
#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif
#define min(a, b) ((a) < (b) ? (a) : (b))
#define u8_get_bits(v, m) uint8_t(((v) & (m)) / ((m) & -(m)))
#define IEEE80211_ADDR_COPY(dst, src) memcpy((dst), (src), 6)

using u8 = uint8_t;
using __le16 = uint16_t;
using __le32 = uint32_t;

static const uint8_t etheranyaddr[6] = {0, 0, 0, 0, 0, 0};

enum {
    ETHER_ADDR_LEN = 6,
    IEEE80211_M_STA = 1,
    IEEE80211_M_MONITOR = 8,
    IEEE80211_S_ASSOC = 3,
    IEEE80211_S_RUN = 4,
    IEEE80211_NODE_HT = 1 << 0,
    IEEE80211_NODE_VHT = 1 << 1,
    IEEE80211_NODE_HE = 1 << 2,
    IEEE80211_CHAN_WIDTH_20 = 0,
    IEEE80211_CHAN_WIDTH_40 = 1,
    IEEE80211_CHAN_WIDTH_80 = 2,
    IEEE80211_CHAN_WIDTH_160 = 3,
    IEEE80211_CHAN_WIDTH_80P80 = 4,
    IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_MASK = 7 << 23,
    IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT = 23,
    IEEE80211_HE_MAC_CAP3_MAX_AMPDU_LEN_EXP_MASK = 0x18,
    IEEE80211_AMPDU_PARAM_SS = 0x1c,
    IEEE80211_AMPDU_PARAM_SS_NONE = 0x00,
    IEEE80211_AMPDU_PARAM_SS_2 = 0x04,
    IEEE80211_AMPDU_PARAM_SS_4 = 0x08,
    IEEE80211_AMPDU_PARAM_SS_8 = 0x0c,
    IEEE80211_AMPDU_PARAM_SS_16 = 0x10,
};

// FIRMWARE

struct ieee80211_node {
    uint32_t ni_flags = IEEE80211_NODE_HT | IEEE80211_NODE_VHT;
    uint8_t ni_chw = IEEE80211_CHAN_WIDTH_80;
    uint8_t ni_rx_nss = 2;
    uint8_t ni_ampdu_param = IEEE80211_AMPDU_PARAM_SS_8;
    uint32_t ni_vhtcaps = 7 << IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT;
    struct { uint8_t mac_cap_info[6] = {}; } ni_he_cap_elem;
};

struct ieee80211com {
    int ic_opmode = IEEE80211_M_STA;
    int ic_state = IEEE80211_S_RUN;
    uint8_t ic_ampdu_params = IEEE80211_AMPDU_PARAM_SS_4;
    ieee80211_node *ic_bss = nullptr;
};

struct iwx_node {
    ieee80211_node in_ni;
    uint32_t in_id = 0, in_color = 1;
    uint8_t in_macaddr[6] = {2, 3, 4, 5, 6, 7};
};

struct iwx_dma_info { void *vaddr = nullptr; };
struct iwx_tx_ring { iwx_dma_info bc_tbl; };

struct iwx_softc {
    ieee80211com sc_ic;
    int sc_device_family = IWX_DEVICE_FAMILY_22000;
    uint32_t sc_flags = 0;
    uint16_t sc_tx_agg_tidmask = 0x00ff;
};

struct ItlIwx {
    std::vector<struct iwx_add_sta_cmd> cmds;
    bool iwx_mimo_enabled(iwx_softc *) { return true; }
    int iwx_send_cmd_pdu_status(iwx_softc *, uint32_t id, uint16_t len, const void *data, uint32_t *status) {
        assert(id == IWX_ADD_STA && len == sizeof(struct iwx_add_sta_cmd));
        cmds.push_back(*static_cast<const struct iwx_add_sta_cmd *>(data));
        *status = IWX_ADD_STA_SUCCESS;
        return 0;
    }
    int iwx_add_sta_cmd(iwx_softc *, iwx_node *, int);
    void iwx_tx_update_byte_tbl(iwx_softc *, iwx_tx_ring *, int, uint16_t, uint16_t);
};

// PRODUCTION

int main() {
    ItlIwx hal;
    iwx_softc sc = {};
    iwx_node node = {};
    sc.sc_ic.ic_bss = &node.in_ni;

    // 1. Check exact non-overlapping FAT_EN width values (20/40/80/160 MHz) and peer MPDU density!
    struct Case { uint8_t chw; uint32_t expectedFat; uint8_t peerSS; uint32_t expectedDens; };
    const Case cases[] = {
        {IEEE80211_CHAN_WIDTH_20, IWX_STA_FLG_FAT_EN_20MHZ, IEEE80211_AMPDU_PARAM_SS_NONE, 0},
        {IEEE80211_CHAN_WIDTH_40, IWX_STA_FLG_FAT_EN_40MHZ, IEEE80211_AMPDU_PARAM_SS_2, IWX_STA_FLG_AGG_MPDU_DENS_2US},
        {IEEE80211_CHAN_WIDTH_80, IWX_STA_FLG_FAT_EN_80MHZ, IEEE80211_AMPDU_PARAM_SS_8, IWX_STA_FLG_AGG_MPDU_DENS_8US},
        {IEEE80211_CHAN_WIDTH_160, IWX_STA_FLG_FAT_EN_160MHZ, IEEE80211_AMPDU_PARAM_SS_16, IWX_STA_FLG_AGG_MPDU_DENS_16US},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        node.in_ni.ni_chw = cases[i].chw;
        node.in_ni.ni_ampdu_param = cases[i].peerSS;
        // Keep local ic_ampdu_params fixed at 4US to prove peer ni_ampdu_param is used!
        sc.sc_ic.ic_ampdu_params = IEEE80211_AMPDU_PARAM_SS_4;
        assert(hal.iwx_add_sta_cmd(&sc, &node, i > 0 ? 1 : 0) == 0);
        assert(hal.cmds.size() == i + 1);
        const auto &c = hal.cmds.back();
        assert(c.tid_disable_tx == 0xffffU);
        assert((c.station_flags & IWX_STA_FLG_FAT_EN_MSK) == cases[i].expectedFat);
        assert((c.station_flags & IWX_STA_FLG_AGG_MPDU_DENS_MSK) == cases[i].expectedDens);
    }

    // 2. Verify Gen2/QuZ (< AX210) bc_tbl wrap duplication in [256..319] for idx < 64 only.
    iwx_agn_scd_bc_tbl agnTbl = {};
    iwx_tx_ring ring = {};
    ring.bc_tbl.vaddr = &agnTbl;
    hal.iwx_tx_update_byte_tbl(&sc, &ring, 0, 1500, 2);
    hal.iwx_tx_update_byte_tbl(&sc, &ring, 63, 512, 3);
    hal.iwx_tx_update_byte_tbl(&sc, &ring, 64, 1024, 1);
    hal.iwx_tx_update_byte_tbl(&sc, &ring, 255, 64, 1);
    assert(agnTbl.tfd_offset[0] != 0 && agnTbl.tfd_offset[256] == agnTbl.tfd_offset[0]);
    assert(agnTbl.tfd_offset[63] != 0 && agnTbl.tfd_offset[256 + 63] == agnTbl.tfd_offset[63]);
    assert(agnTbl.tfd_offset[64] != 0);
    assert(agnTbl.tfd_offset[255] != 0);
    return 0;
}
"""
        prod = add_sta_src + "\n" + bc_tbl_src
        fw = firmware_declarations(
            ("iwx_add_sta_cmd", "iwx_agn_scd_bc_tbl", "iwx_gen3_bc_tbl", "iwx_tfh_tb", "iwx_tfh_tfd"),
            harness + "\n" + prod,
        )
        self.compile_run(harness.replace("// FIRMWARE", fw).replace("// PRODUCTION", prod))

    def test_p118_station0_100ms_reorder_timeout_and_rx_ba_session_expired_timeout(self):
        session_expired_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_rx_ba_session_expired")
        extra = p116.INFRA_HELPERS + r"""
// SESSION_EXPIRED_DEF

int main() {
    // 1. Station 0 100 ms reorder timeout survives a 64 ms AWDL channel switch without
    //    flushing out-of-order holes prematurely, then flushes cleanly at 100 ms if lost.
    {
        ItlIwx d;
        const unsigned baid = startInfra(d, 0, 100, 64);
        auto &buf = d.com.sc_rxba_data[baid].reorder_buf;

        // SN=100 missing while SN=101 arrives right before a 64 ms AWDL dwell:
        auto s101 = infraPacket(d, 0, 101, 1010);
        receiveInfra(d, s101.get(), baid, 101, 100);
        assert(d.apDelivered.empty() && buf.num_stored == 1);
        assert(buf.reorder_timer->deadline == nowUS + 100000ULL);

        // Even if timer callback is polled at +64 ms, hole has not expired yet:
        nowUS += 64000ULL;
        ItlIwx::iwx_reorder_timer_expired(&buf);
        assert(d.apDelivered.empty() && buf.head_sn == 100 && buf.num_stored == 1);

        // AP retransmits missing SN=100 upon home-channel return -> strict in-order delivery!
        auto s100 = infraPacket(d, 0, 100, 1000);
        receiveInfra(d, s100.get(), baid, 100, 102);
        assert((d.apDelivered == std::vector<uint16_t>{1000, 1010}));
        assert(d.com.sc_ic.ic_stats.is_ht_rx_frame_below_ba_winstart == 0);
        assert(buf.head_sn == 102 && buf.num_stored == 0);

        // Genuine loss of SN=102 flushes SN=103 after 100 ms:
        d.apDelivered.clear();
        auto s103 = infraPacket(d, 0, 103, 1030);
        receiveInfra(d, s103.get(), baid, 103, 102);
        assert(buf.reorder_timer->deadline == nowUS + 100000ULL);
        fireReorderTimer(buf);
        assert((d.apDelivered == std::vector<uint16_t>{1030}));
        assert(buf.head_sn == 104 && buf.num_stored == 0);

        // 2. iwx_sta_rx_agg stores timeout_val (5000 TU * 1024 = 5,120,000 us) in TUs without
        //    uint16_t truncation, and iwx_rx_ba_session_expired honors the full 5.12 s timeout!
        assert(!d.iwx_sta_rx_agg(&d.com, &d.bss, 0, 0, 0, 0, 0));
        assert(!d.iwx_sta_rx_agg(&d.com, &d.bss, 0, 104, 64, 5000 * 1024, 1));
        unsigned baid2 = IWX_MAX_BAID;
        for (unsigned i = 0; i < nitems(d.com.sc_rxba_data); ++i) {
            if (d.com.sc_rxba_data[i].baid == i && d.com.sc_rxba_data[i].sta_id == IWX_STATION_ID)
                baid2 = i;
        }
        assert(baid2 < IWX_MAX_BAID);
        auto &rxba = d.com.sc_rxba_data[baid2];
        assert(rxba.timeout == 5000);
        USEC_TO_TIMEVAL(nowUS, &rxba.last_rx);
        apDelbas = 0;

        // At +250 ms (> 100 ms reorder timeout, < 5.12 s BA session timeout): MUST NOT DELBA!
        nowUS += 250000ULL;
        ItlIwx::iwx_rx_ba_session_expired(&rxba);
        assert(d.com.sc_ic.ic_stats.is_ht_rx_ba_timeout == 0);
        assert(apDelbas == 0);
        assert(rxba.session_timer && rxba.session_timer->deadline == nowUS + 5120000ULL);

        // At +5.2 s idle (>= 5.12 s): expires session and sends DELBA(REASON_TIMEOUT).
        nowUS += 5000000ULL;
        ItlIwx::iwx_rx_ba_session_expired(&rxba);
        assert(d.com.sc_ic.ic_stats.is_ht_rx_ba_timeout == 1);
        assert(apDelbas == 1);

        // When rxba->timeout == 0 (infinite BA timeout), callback is a no-op.
        rxba.timeout = 0;
        nowUS += 99999999ULL;
        ItlIwx::iwx_rx_ba_session_expired(&rxba);
        assert(apDelbas == 1);

        assert(!d.iwx_sta_rx_agg(&d.com, &d.bss, 0, 0, 0, 0, 0));
    }
    return 0;
}
"""
        base = p116._build_infra_ba_harness(extra)
        base = base.replace(
            "static void iwx_rx_ba_session_expired(void *){}",
            "static void iwx_rx_ba_session_expired(void *);",
        )
        base = base.replace(
            "IEEE80211_REASON_UNSPECIFIED=1,kIOReturnSuccess=0",
            "IEEE80211_REASON_UNSPECIFIED=1,IEEE80211_REASON_TIMEOUT=39,IEEE80211_DUR_TU=1024,IWX_FLAG_SHUTDOWN=8,kIOReturnSuccess=0",
        )
        base = base.replace(
            "#define SEQ_LT(a,b) ((((a)-(b)) & 0x800) != 0)",
            "#define SEQ_LT(a,b) ((((a)-(b)) & 0x800) != 0)\n#define MIN(a,b) ((a)<(b)?(a):(b))",
        )
        base = base.replace(
            "is_ht_rx_ba_window_gap_timeout=0;} ic_stats;",
            "is_ht_rx_ba_window_gap_timeout=0,is_ht_rx_ba_timeout=0;} ic_stats;",
        )
        base = base.replace("// SESSION_EXPIRED_DEF", session_expired_src)
        self.compile_run(base)

    def test_p118_abort_scanning_and_5ghz_directed_passive2active_unfragmented_dwell(self):
        clear_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::clearScanningFlags")
        abort_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::abortScanning")
        v14_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_umac_scan_v14")
        self.assertIn("const bool directedScan = scanActive && scanSSIDLength != 0;", v14_src)

        harness = r"""
#include <cassert>
#include <cstdint>
#define AIRPORT_WCL 1
#include "AirportItlwm/WCL/WCL.hpp"

using IOReturn = int;
enum {
    kIOReturnSuccess = 0,
    IWX_FLAG_SCANNING = 1 << 0,
    IWX_FLAG_BGSCAN = 1 << 1,
    IWX_FLAG_SHUTDOWN = 1 << 2,
    IWL_SCAN_DWELL_PASSIVE = 110,
};
struct OSObject {};
struct IOWorkLoop {
    bool gated = true;
    bool inGate() const { return gated; }
};
struct IOCommandGate {
    IOWorkLoop *wl = nullptr;
    unsigned runActions = 0;
    IOReturn runAction(IOReturn (*fn)(OSObject *, void *, void *, void *, void *), void *arg) {
        ++runActions;
        bool prev = wl->gated;
        wl->gated = true;
        IOReturn r = fn(nullptr, arg, nullptr, nullptr, nullptr);
        wl->gated = prev;
        return r;
    }
};
struct iwx_softc { uint32_t sc_flags = 0; };
struct ItlIwx {
    iwx_softc com = {};
    IOWorkLoop wl;
    IOCommandGate gate{&wl};
    unsigned abortCmds = 0;

    IOWorkLoop *getMainWorkLoop() { return &wl; }
    IOCommandGate *getMainCommandGate() { return &gate; }
    int iwx_scan_abort(iwx_softc *) { ++abortCmds; return 0; }
    void clearScanningFlags();
    void abortScanning();
};

// PRODUCTION

int main() {
    ItlIwx d;
    // 1. Out-of-gate abortScanning marshals via gate, sends UMAC scan abort once, and clears flags.
    d.wl.gated = false;
    d.com.sc_flags = IWX_FLAG_SCANNING | IWX_FLAG_BGSCAN;
    d.abortScanning();
    assert(d.gate.runActions == 1);
    assert(d.abortCmds == 1);
    assert((d.com.sc_flags & (IWX_FLAG_SCANNING | IWX_FLAG_BGSCAN)) == 0);

    // 2. Idle or shutdown state clears flags without sending redundant firmware abort commands.
    d.wl.gated = true;
    d.abortScanning();
    assert(d.abortCmds == 1);
    d.com.sc_flags = IWX_FLAG_SCANNING | IWX_FLAG_SHUTDOWN;
    d.abortScanning();
    assert(d.abortCmds == 1);
    assert((d.com.sc_flags & IWX_FLAG_SCANNING) == 0);

    // 3. 5 GHz directed hidden-SSID scan (directedScan=true) disables 44 TU fragmentation
    //    and preserves >= 110 TU contiguous dwell even when awayTimeMS=45 during bgscan!
    const bool directed = true;
    const auto dirTiming = AirportWCL::scanTiming(directed ? 0 : 45, true, !directed && true);
    assert(!dirTiming.fragmented);
    assert(dirTiming.maxOutTU >= IWL_SCAN_DWELL_PASSIVE);

    // Wildcard low-latency background scan keeps fragmented scan active.
    const bool wildcard = false;
    const auto wildTiming = AirportWCL::scanTiming(wildcard ? 0 : 45, true, !wildcard && true);
    assert(wildTiming.fragmented);
    return 0;
}
"""
        self.compile_run(harness.replace("// PRODUCTION", clear_src + "\n" + abort_src))

    def test_p118_rx_pkt_vtd_partial_bounce_sync_and_notif_intr_empty_ml_guard(self):
        valid_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_rx_pkt_valid")
        rx_pkt_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_rx_pkt")
        notif_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_notif_intr")

        harness = r"""
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#define AIRPORT_VTD 1
#define XYLog(...) ((void)0)
#define DPRINTF(x) ((void)0)
#define DPRINTFN(n, x) ((void)0)
#define KASSERT(cond, msg) assert(cond)
#define DEVNAME(sc) "iwx0"
#define IC2IFP(ic) (&(ic)->ic_if)
#define htole16(x) uint16_t(x)
#define htole32(x) uint32_t(x)
#define le16toh(x) uint16_t(x)
#define le32toh(x) uint32_t(x)
#define MIN(a, b) std::min((a), (b))
#define roundup(x, y) ((((x) + (y) - 1) / (y)) * (y))
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#define MBUF_DONTWAIT 0
#define MBUF_COPYALL -1
#define BUS_DMASYNC_POSTREAD 1
#define MBUF_LIST_INITIALIZER() {}

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using __le16 = uint16_t;
using __le32 = uint32_t;
using __le64 = uint64_t;
using __be16 = uint16_t;
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define BIT(x) (1UL << (x))
#define offsetofend(t, m) (offsetof(t, m) + sizeof(((t *)0)->m))
#define ETHER_ADDR_LEN 6
#define IEEE80211_NWID_LEN 32
static uint32_t le32_to_cpup(const uint32_t *p) { uint32_t v; memcpy(&v, p, 4); return le32toh(v); }
struct ieee80211_frame { uint8_t i_fc[2], i_dur[2], i_addr1[6], i_addr2[6], i_addr3[6], i_seq[2]; };

#include "itlwm/hal_iwx/if_iwxreg.h"
#undef IWX_WRITE
#define IWX_WRITE(sc, reg, val) ((sc)->lastWrite = (val))
enum { IWX_RBUF_SIZE = 4096, IWX_RX_MQ_RING_COUNT = 512 };

static void OSSynchronizeIO() {}

struct Packet {
    std::vector<uint8_t> buf;
    size_t off = 0, len = 0;
    explicit Packet(size_t n = IWX_RBUF_SIZE) : buf(n, 0xcc), len(n) {}
};
using mbuf_t = Packet *;
static std::vector<std::unique_ptr<Packet>> owned;
static unsigned copymCalls = 0;

static Packet *allocPacket(size_t n = IWX_RBUF_SIZE) {
    owned.emplace_back(new Packet(n));
    return owned.back().get();
}
static void *mbuf_data(mbuf_t m) { return m->buf.data() + m->off; }
static void mbuf_adj(mbuf_t m, int n) { assert(n >= 0 && size_t(n) <= m->len); m->off += size_t(n); m->len -= size_t(n); }
static void mbuf_copym(mbuf_t src, int off, int len, int, mbuf_t *out) {
    assert(off == 0 && len == MBUF_COPYALL);
    ++copymCalls;
    auto *c = allocPacket(src->buf.size());
    c->buf = src->buf;
    c->off = src->off;
    c->len = src->len;
    *out = c;
}

struct mbuf_list { std::vector<mbuf_t> q; };
static bool ml_empty(const mbuf_list *ml) { return ml->q.empty(); }
static void ml_enqueue(mbuf_list *ml, mbuf_t m) { ml->q.push_back(m); }

struct DmaBuf { void *arena = reinterpret_cast<void *>(1); void *vaddr = nullptr; size_t size = IWX_RBUF_SIZE; };
struct bus_dmamap {
    std::vector<uint8_t> bounce;
    DmaBuf dma;
    std::vector<size_t> syncSizes;
    bus_dmamap() : bounce(IWX_RBUF_SIZE, 0) { dma.vaddr = bounce.data(); }
};
using bus_dmamap_t = bus_dmamap *;
static void bus_dmamap_sync(void *, bus_dmamap_t, size_t, size_t, int) {}
static bool bus_dmamap_read(bus_dmamap_t map, mbuf_t m, size_t size) {
    assert(map && m && size <= IWX_RBUF_SIZE);
    map->syncSizes.push_back(size);
    memcpy(mbuf_data(m), map->dma.vaddr, size);
    return true;
}

static unsigned ifInputCalls = 0;
struct Stats { unsigned inputErrors = 0; };
struct _ifnet { Stats st; Stats *netStat = &st; };
static void if_input(_ifnet *, mbuf_list *ml) { ++ifInputCalls; ml->q.clear(); }

enum { IWX_DEVICE_FAMILY_9000 = 1, IWX_DEVICE_FAMILY_AX210 = 5, IWX_TXDATA_FLAG_CMD_IS_NARROW = 1 };
struct iwx_rx_data { mbuf_t m = nullptr; bus_dmamap_t map = nullptr; };
struct iwx_tx_data { uint32_t flags = 0; };
struct iwx_tx_ring { int ring_count = 32; iwx_tx_data data[32] = {}; };
struct iwx_softc {
    struct { _ifnet ic_if; } sc_ic;
    void *sc_dmat = nullptr;
    int sc_device_family = IWX_DEVICE_FAMILY_9000;
    int sc_Wait_Alive = 0;
    uint32_t lastWrite = 0;
    iwx_tx_ring txq[4] = {};
    struct {
        iwx_rb_status statusBuf = {};
        void *stat = nullptr;
        uint16_t cur = 0;
        iwx_rx_data data[IWX_RX_MQ_RING_COUNT] = {};
    } rxq;
    iwx_softc() { rxq.stat = &rxq.statusBuf; }
};

static size_t writeFrame(std::vector<uint8_t> &b, size_t off, uint8_t gid, uint8_t cmd, size_t payloadLen, uint8_t fill) {
    const uint32_t n = uint32_t(sizeof(iwx_cmd_header) + payloadLen);
    assert(off + 4 + n <= b.size());
    uint32_t lnf = htole32(n);
    memcpy(b.data() + off, &lnf, 4);
    auto *hdr = reinterpret_cast<iwx_cmd_header *>(b.data() + off + 4);
    hdr->cmd = cmd;
    hdr->group_id = gid;
    hdr->qid = 0x80;
    hdr->idx = 1;
    memset(b.data() + off + 8, fill, payloadLen);
    return off + roundup(4 + n, IWX_FH_RSCSR_FRAME_ALIGN);
}

struct ItlIwx {
    unsigned txCmds = 0, phyCmds = 0;
    std::vector<size_t> mpduLens;

    int iwx_rx_addbuf(iwx_softc *sc, int, int idx) {
        auto *replacementMap = new bus_dmamap();
        sc->rxq.data[idx].map = replacementMap;
        sc->rxq.data[idx].m = allocPacket();
        return 0;
    }
    void iwx_rx_rx_phy_cmd(iwx_softc *, iwx_rx_packet *, iwx_rx_data *) { ++phyCmds; }
    void iwx_rx_mpdu_mq(iwx_softc *, mbuf_t m, void *pktdata, size_t maxlen, mbuf_list *ml) {
        const auto *bytes = static_cast<const uint8_t *>(pktdata);
        assert(bytes[0] == 0x5a && bytes[maxlen - 1] == 0x5a);
        mpduLens.push_back(maxlen);
        ml_enqueue(ml, m);
    }
    void iwx_rx_tx_ba_notif(iwx_softc *, iwx_rx_packet *, iwx_rx_data *) {}
    void iwx_rx_tx_cmd(iwx_softc *, iwx_rx_packet *, iwx_rx_data *) { ++txCmds; }
    void iwx_rx_bmiss(iwx_softc *, iwx_rx_packet *, iwx_rx_data *) {}
    int iwx_Trigger_Alive(iwx_softc *) { return 0; }
    void wakeupOn(void *) {}
    void iwx_rx_lms_notif(iwx_softc *, iwx_rx_packet *, iwx_rx_data *) {}
    void iwx_cmd_done(iwx_softc *, int, int, int) {}
    void iwx_update_rx_desc(iwx_softc *, iwx_rx_packet *) {}
    void iwx_bar_frame_release(iwx_softc *, iwx_rx_packet *, mbuf_list *) {}
    void iwx_rx_tlc_notif(iwx_softc *, iwx_rx_packet *) {}
    void iwx_mcc_update_notif(iwx_softc *, iwx_rx_packet *) {}
    void iwx_session_prot_notif(iwx_softc *, iwx_rx_packet *) {}
    void iwx_binding_notif(iwx_softc *, iwx_rx_packet *) {}
    void iwx_time_event_notif(iwx_softc *, iwx_rx_packet *) {}
    void iwx_temp_notif(iwx_softc *, iwx_rx_packet *) {}
    void iwx_scan_offload_complete_notif(iwx_softc *, iwx_rx_packet *) {}
    void iwx_umac_scan_complete_notif(iwx_softc *, iwx_rx_packet *) {}
    void iwx_umac_scan_iter_complete_notif(iwx_softc *, iwx_rx_packet *) {}
    void iwx_stats_update(iwx_softc *, iwx_rx_packet *) {}

    int iwx_rx_pkt_valid(iwx_rx_packet *);
    void iwx_rx_pkt(iwx_softc *, iwx_rx_data *, mbuf_list *);
    void iwx_notif_intr(iwx_softc *);
};
"""
        # Trim iwx_rx_pkt switch to the core cases used in ItlIwx.cpp up through IWX_TX_CMD so we don't need 40 unrelated notification structs, while keeping the ENTIRE top of iwx_rx_pkt (DMA sync + while loop + RX_PHY + RX_MPDU + TX_CMD) 100% verbatim!
        m = re.search(
            r"(void\s+ItlIwx::\s*iwx_rx_pkt\s*\([^{]+\{.*?case IWX_TX_CMD:\s*iwx_rx_tx_cmd\(sc, pkt, data\);\s*break;)",
            rx_pkt_src,
            re.S,
        )
        self.assertIsNotNone(m)
        rx_pkt_prefix = m.group(1) + """
            default:
                break;
        }
        offset += roundup(len, IWX_FH_RSCSR_FRAME_ALIGN);
    }
}
"""
        main_body = r"""
int main() {
    ItlIwx hal;
    iwx_softc sc;

    // 1. Control notification RB: syncs only aligned frame + 8-byte invalid trailer (< 4096B),
    //    and skips if_input because ml is empty!
    bus_dmamap map0;
    sc.rxq.data[0] = {allocPacket(), &map0};
    const size_t end0 = writeFrame(map0.bounce, 0, 0, IWX_TX_CMD, 112, 0x33);
    sc.rxq.statusBuf.closed_rb_num = 1;
    hal.iwx_notif_intr(&sc);
    assert(hal.txCmds == 1);
    assert(map0.syncSizes.size() == 1 && map0.syncSizes[0] == end0 + 8 && map0.syncSizes[0] < IWX_RBUF_SIZE);
    assert(ifInputCalls == 0);

    // 2. Single 1500-byte MPDU RB: syncs only aligned MPDU + 8-byte trailer (< 4096B),
    //    invokes if_input once, 0 mbuf_copym calls!
    bus_dmamap map1;
    auto *origM1 = sc.rxq.data[1].m = allocPacket();
    sc.rxq.data[1].map = &map1;
    const size_t end1 = writeFrame(map1.bounce, 0, 0, IWX_REPLY_RX_MPDU_CMD, 1552, 0x5a);
    sc.rxq.statusBuf.closed_rb_num = 2;
    hal.iwx_notif_intr(&sc);
    assert(map1.syncSizes.size() == 1 && map1.syncSizes[0] == end1 + 8 && map1.syncSizes[0] < IWX_RBUF_SIZE);
    assert(copymCalls == 0 && ifInputCalls == 1);
    assert(hal.mpduLens.size() == 1 && hal.mpduLens[0] == 1552);
    delete sc.rxq.data[1].map;
    (void)origM1;

    // 3. Coalesced multi-packet RB (RX_PHY_CMD + MPDU1 + MPDU2 + invalid trailer):
    //    syncs exact combined span before detaching map, copies MPDU1 via mbuf_copym, and
    //    delivers MPDU2 zero-copy!
    bus_dmamap map2;
    sc.rxq.data[2] = {allocPacket(), &map2};
    size_t off = 0;
    off = writeFrame(map2.bounce, off, 0, IWX_REPLY_RX_PHY_CMD, 40, 0x11);
    off = writeFrame(map2.bounce, off, 0, IWX_REPLY_RX_MPDU_CMD, 300, 0x5a);
    off = writeFrame(map2.bounce, off, 0, IWX_REPLY_RX_MPDU_CMD, 500, 0x5a);
    sc.rxq.statusBuf.closed_rb_num = 3;
    hal.iwx_notif_intr(&sc);
    assert(map2.syncSizes.size() == 1 && map2.syncSizes[0] == off + 8);
    assert(hal.phyCmds == 1 && copymCalls == 1 && ifInputCalls == 2);
    assert(hal.mpduLens.size() == 3 && hal.mpduLens[1] == 300 && hal.mpduLens[2] == 500);
    delete sc.rxq.data[2].map;
    return 0;
}
"""
        self.compile_run(harness + "\n" + valid_src + "\n" + rx_pkt_prefix + "\n" + notif_src + "\n" + main_body)

    def test_p118_if_snd_cap_if_input_ingate_and_decap_inplace_ether_align(self):
        ifattach_src = definition("itl80211/openbsd/net80211/ieee80211.c", "ieee80211_ifattach")
        self.assertIn("ifq_init(&ifp->if_snd, ifp, 256);", ifattach_src)

        mbuf_cpp = (support.ROOT / "itl80211/openbsd/sys/_mbuf.cpp").read_text()
        if_input_part = mbuf_cpp[mbuf_cpp.index("static IOReturn _if_input"):]
        decap_src = definition("itl80211/openbsd/net80211/ieee80211_input.c", "ieee80211_decap")

        harness = r"""
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#define ALIGNED_POINTER(p, t) (((reinterpret_cast<uintptr_t>(p)) & (alignof(t) - 1)) == 0)
#define IEEE80211_ADDR_COPY(dst, src) memcpy((dst), (src), 6)
#undef htons
#define htons(x) uint16_t((((x) & 0xff) << 8) | (((x) >> 8) & 0xff))

using caddr_t = char *;
using u_int8_t = uint8_t;
using u_int32_t = uint32_t;
using IOReturn = int;

enum {
    kIOReturnSuccess = 0,
    kIOReturnNotReady = -1,
    ETHER_ADDR_LEN = 6,
    ETHER_HDR_LEN = 14,
    ETHER_ALIGN = 2,
    LLC_SNAPFRAMELEN = 8,
    LLC_SNAP_LSAP = 0xaa,
    LLC_UI = 0x03,
    M_NOWAIT = 1,
    MBUF_EXT = 1 << 0,
    IEEE80211_FC1_DIR_MASK = 0x03,
    IEEE80211_FC1_DIR_NODS = 0x00,
    IEEE80211_FC1_DIR_TODS = 0x01,
    IEEE80211_FC1_DIR_FROMDS = 0x02,
    IEEE80211_FC1_DIR_DSTODS = 0x03,
};

struct FakeMbuf {
    alignas(16) uint8_t cluster[2048] = {};
    uint8_t *data = cluster;
    size_t len = 0, pkthdr_len = 0, cap = sizeof(cluster);
    int flags = MBUF_EXT;
    int ext_refs = 0;
    int fail_setdata = 0;
    FakeMbuf *next = nullptr;
};
using mbuf_t = FakeMbuf *;

static std::vector<FakeMbuf *> allocated;
static unsigned dupCalls = 0, freeCalls = 0;

static void *mbuf_data(mbuf_t m) { return m->data; }
#define mtod(m, t) reinterpret_cast<t>(mbuf_data(m))
static size_t mbuf_len(mbuf_t m) { return m->len; }
static size_t mbuf_pkthdr_len(mbuf_t m) { return m->pkthdr_len; }
static void mbuf_pkthdr_setlen(mbuf_t m, size_t l) { m->pkthdr_len = l; }
static int mbuf_flags(mbuf_t m) { return m->flags; }
static int mbuf_mclhasreference(mbuf_t m) { return m->ext_refs; }
static mbuf_t mbuf_next(mbuf_t m) { return m->next; }
static size_t mbuf_trailingspace(mbuf_t m) {
    return size_t((m->cluster + m->cap) - (m->data + m->len));
}
static int mbuf_setdata(mbuf_t m, void *p, size_t l) {
    if (m->fail_setdata) return 22;
    auto *b = static_cast<uint8_t *>(p);
    if (b < m->cluster || b + l > m->cluster + m->cap) return 22;
    m->data = b;
    m->len = l;
    return 0;
}
static void mbuf_adj(mbuf_t m, int req) {
    assert(req >= 0 && size_t(req) <= m->len);
    m->data += req;
    m->len -= size_t(req);
    m->pkthdr_len -= size_t(req);
}
static int mbuf_pullup(mbuf_t *mp, size_t len) {
    return (*mp && (*mp)->len >= len) ? 0 : 22;
}
static void mbuf_freem(mbuf_t m) { if (m) ++freeCalls; }
static mbuf_t m_dup_pkt(mbuf_t src, unsigned int adj, int) {
    ++dupCalls;
    auto *c = new FakeMbuf();
    allocated.push_back(c);
    c->data = c->cluster + adj;
    memcpy(c->data, src->data, src->len);
    c->len = c->pkthdr_len = src->len;
    return c;
}

struct mbuf_list { std::vector<mbuf_t> items; };
static bool ml_empty(mbuf_list *ml) { return ml->items.empty(); }
static mbuf_t ml_dequeue(mbuf_list *ml) {
    if (ml->items.empty()) return nullptr;
    mbuf_t m = ml->items.front();
    ml->items.erase(ml->items.begin());
    return m;
}
static void ml_purge(mbuf_list *ml) { ml->items.clear(); }

struct OSObject {};
struct IONetworkInterface { enum { kInputOptionQueuePacket = 1 }; };
struct IOEthernetInterface {
    unsigned queued = 0, flushes = 0;
    void inputPacket(mbuf_t, int, int) { ++queued; }
    void flushInputQueue() { ++flushes; }
};
struct NetStat { unsigned inputPackets = 0; };
struct _ifnet { IOEthernetInterface *iface = nullptr; NetStat *netStat = nullptr; };

struct IOWorkLoop {
    bool gated = false;
    bool inGate() const { return gated; }
};
struct IOCommandGate {
    typedef IOReturn (*Action)(OSObject *, void *, void *, void *, void *);
    IOWorkLoop *wl = nullptr;
    unsigned runActions = 0;
    IOWorkLoop *getWorkLoop() const { return wl; }
    IOReturn runAction(Action fn, void *a0, void *a1) {
        ++runActions;
        return fn(nullptr, a0, a1, nullptr, nullptr);
    }
};
IOCommandGate *_fCommandGate = nullptr;

struct ether_header {
    uint8_t ether_dhost[6];
    uint8_t ether_shost[6];
    uint16_t ether_type;
} __attribute__((packed));

struct llc {
    uint8_t llc_dsap, llc_ssap, llc_control;
    struct { uint8_t org_code[3]; uint16_t ether_type; } __attribute__((packed)) llc_snap;
} __attribute__((packed));

struct ieee80211_frame { uint8_t i_fc[2], i_dur[2], i_addr1[6], i_addr2[6], i_addr3[6], i_seq[2]; };
struct ieee80211_frame_addr4 { uint8_t i_fc[2], i_dur[2], i_addr1[6], i_addr2[6], i_addr3[6], i_seq[2], i_addr4[6]; };
struct ieee80211_node {};
struct ieee80211com { struct { unsigned is_rx_decap = 0; } ic_stats; };
#define IEEE80211_IS_MULTICAST(a) (((a)[0] & 1) != 0)
static std::vector<mbuf_t> decapped;
static void ieee80211_enqueue_data(ieee80211com *, mbuf_t m, ieee80211_node *, int, mbuf_list *) {
    decapped.push_back(m);
}

// PRODUCTION

static void initQoSDataFrame(FakeMbuf &m, size_t payloadLen) {
    m = FakeMbuf{};
    // 26-byte QoS header + 8-byte LLC/SNAP + payload at 16-byte aligned cluster start:
    // After mbuf_adj(26 + 8 - 14 = 20), mtod is at cluster+20, so mtod+14 is at cluster+34 (2 mod 4 -> unaligned!).
    const size_t hdrlen = 26;
    m.data = m.cluster;
    m.len = m.pkthdr_len = hdrlen + LLC_SNAPFRAMELEN + payloadLen;
    auto *wh = reinterpret_cast<ieee80211_frame *>(m.data);
    wh->i_fc[1] = IEEE80211_FC1_DIR_FROMDS;
    const uint8_t dst[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t bssid[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    const uint8_t src[6] = {0x02, 0x66, 0x77, 0x88, 0x99, 0xaa};
    memcpy(wh->i_addr1, dst, 6);
    memcpy(wh->i_addr2, bssid, 6);
    memcpy(wh->i_addr3, src, 6);
    auto *snap = reinterpret_cast<llc *>(m.data + hdrlen);
    snap->llc_dsap = LLC_SNAP_LSAP;
    snap->llc_ssap = LLC_SNAP_LSAP;
    snap->llc_control = LLC_UI;
    snap->llc_snap.ether_type = htons(0x0800);
    for (size_t i = 0; i < payloadLen; ++i)
        m.data[hdrlen + LLC_SNAPFRAMELEN + i] = uint8_t(0x40 + (i & 0x3f));
}

int main() {
    // 1. Verify _mbuf.cpp if_input empty-list skip and inGate() fast-path!
    IOWorkLoop wl;
    IOCommandGate gate{&wl};
    _fCommandGate = &gate;
    IOEthernetInterface iface;
    NetStat st;
    _ifnet ifq{&iface, &st};
    mbuf_list ml;

    assert(if_input(&ifq, &ml) == 0);
    assert(gate.runActions == 0 && iface.flushes == 0);

    FakeMbuf p1, p2;
    wl.gated = true;
    ml.items = {&p1, &p2};
    assert(if_input(&ifq, &ml) == kIOReturnSuccess);
    assert(gate.runActions == 0); // Bypassed redundant runAction while already in gate!
    assert(iface.queued == 2 && iface.flushes == 1 && st.inputPackets == 2);

    wl.gated = false;
    ml.items = {&p1};
    assert(if_input(&ifq, &ml) == kIOReturnSuccess);
    assert(gate.runActions == 1 && iface.queued == 3 && iface.flushes == 2);

    // 2. Verify ieee80211_decap shifts unshared single-segment MBUF_EXT in place with 0 m_dup_pkt!
    ieee80211com ic;
    ieee80211_node ni;
    FakeMbuf unshared;
    initQoSDataFrame(unshared, 100);
    ieee80211_decap(&ic, &unshared, &ni, 26, &ml);
    assert(dupCalls == 0 && freeCalls == 0);
    assert(decapped.size() == 1 && decapped.back() == &unshared);
    assert(((reinterpret_cast<uintptr_t>(unshared.data) + ETHER_HDR_LEN) & 3U) == 0U);
    assert(unshared.len == ETHER_HDR_LEN + 100 && unshared.pkthdr_len == ETHER_HDR_LEN + 100);
    for (size_t i = 0; i < 100; ++i)
        assert(unshared.data[ETHER_HDR_LEN + i] == uint8_t(0x40 + (i & 0x3f)));

    // 3. Shared cluster (ext_refs=1) MUST NOT mutate shared bytes in place; falls back to m_dup_pkt!
    FakeMbuf shared;
    initQoSDataFrame(shared, 64);
    shared.ext_refs = 1;
    ieee80211_decap(&ic, &shared, &ni, 26, &ml);
    assert(dupCalls == 1 && freeCalls == 1);
    assert(decapped.size() == 2 && decapped.back() != &shared);
    assert(((reinterpret_cast<uintptr_t>(decapped.back()->data) + ETHER_HDR_LEN) & 3U) == 0U);

    // 4. Simulated mbuf_setdata rejection preserves uncorrupted payload via m_dup_pkt fallback!
    FakeMbuf setdataFail;
    initQoSDataFrame(setdataFail, 48);
    setdataFail.fail_setdata = 1;
    ieee80211_decap(&ic, &setdataFail, &ni, 26, &ml);
    assert(dupCalls == 2 && freeCalls == 2);
    for (size_t i = 0; i < 48; ++i)
        assert(decapped.back()->data[ETHER_HDR_LEN + i] == uint8_t(0x40 + (i & 0x3f)));

    for (auto *m : allocated) delete m;
    return 0;
}
"""
        self.compile_run(harness.replace("// PRODUCTION", if_input_part + "\n" + decap_src))


if __name__ == "__main__":
    unittest.main()
