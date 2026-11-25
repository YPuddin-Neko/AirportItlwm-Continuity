"""Preview 117 PHY/MAC/HAL/net80211 throughput and hidden-SSID regressions:
1. Station 0 (en1) two-phase TX A-MPDU ADDBA negotiation, 12-bit TVQM writePointer SSN
   tracking, IWX_STA_MODIFY_TID_DISABLE_TX firmware enable/disable, TVQM queue reuse across
   DELBA/timeouts, 20 ms initial ADDBA trigger, and clean TVQM ring teardown on iwx_rm_sta /
   iwx_stop_device.
2. Station 0 (en1) RX reorder multi-subframe VHT A-MSDU !awdl_complete guard in both the
   nssn and head_sn loops, plus 10 ms RX_REORDER_BUF_TIMEOUT_MQ_USEC non-wedging expiry.
3. m_dup_pkt ETHER_ALIGN=2 payload alignment in _mbuf.h.
4. UMAC scan dense unique non-NUL direct_scan[] packing, scan.type==2 hidden-SSID active
   promotion (omitting FORCE_PASSIVE), and 5 GHz LAR-passive PASSIVE2ACTIVE chan->flags
   bitmap (keeping DFS radar channels passive).
5. net80211 empty/all-NUL hidden SSID rejection, Probe Response IE preservation against
   subsequent blank Beacons, event.isprobe propagation, and hidden-BSSID adoption in
   ieee80211_match_bss during WCL join.
"""
import re
import unittest

import test_awdl_preview116_infra as p116
import test_regressions as support
from iwx_firmware import declarations as firmware_declarations
from test_offline import definition


class Preview117PhyRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p117_station0_two_phase_tx_ampdu_ssn_reuse_and_teardown(self):
        iwx_defs = "\n".join(
            definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + fn)
            for fn in (
                "iwx_tvqm_alloc_txq",
                "iwx_free_sta_tx_queues",
                "iwx_ba_task",
                "iwx_ampdu_tx_start",
                "iwx_ampdu_tx_stop",
            )
        )
        proto_defs = "\n".join(
            definition("itl80211/openbsd/net80211/ieee80211_proto.c", fn)
            for fn in (
                "ieee80211_tx_ba_timeout",
                "ieee80211_addba_request",
                "ieee80211_delba_request",
            )
        )
        input_defs = "\n".join(
            definition("itl80211/openbsd/net80211/ieee80211_input.c", fn)
            for fn in (
                "ieee80211_recv_addba_resp",
                "ieee80211_addba_resp_accept",
                "ieee80211_addba_resp_refuse",
            )
        )
        node_defs = definition(
            "itl80211/openbsd/net80211/ieee80211_node.c",
            "ieee80211_node_trigger_addba_req",
        )
        add_sta_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_add_sta_cmd")
        rm_sta_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_rm_sta")

        harness = r"""
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define AIRPORT_VTD 1
#define AIRPORT_WCL 1
#define IEEE80211_STA_ONLY 1
#define IWX_STA_TX_AGG_SUPPORT 1
#define XYLog(...) ((void)0)
#define DPRINTF(x) ((void)0)
#define DEVNAME(sc) "iwx0"
#define htole16(x) uint16_t(x)
#define htole32(x) uint32_t(x)
#define le16toh(x) uint16_t(x)
#define le32toh(x) uint32_t(x)
#define LE_READ_2(p) (uint16_t((p)[0]) | (uint16_t((p)[1]) << 8))
#define LE_WRITE_2(p, v) do { (p)[0] = uint8_t((v) & 0xff); (p)[1] = uint8_t(((v) >> 8) & 0xff); } while (0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define u8_get_bits(v, m) uint8_t(((v) & (m)) / ((m) & -(m)))
#define container_of(p, t, m) reinterpret_cast<t *>(reinterpret_cast<uint8_t *>(p) - offsetof(t, m))
#define IC2IFP(ic) (&(ic)->ic_if)
#define IEEE80211_ADDR_COPY(dst, src) memcpy((dst), (src), 6)

using u_int8_t = uint8_t;
using u_int16_t = uint16_t;
using caddr_t = char *;
using u8 = uint8_t;
using __le16 = uint16_t;
using __le32 = uint32_t;
using IOReturn = int;
struct OSObject {};

static const uint8_t etheranyaddr[6] = {0, 0, 0, 0, 0, 0};
static const char *ether_sprintf(const uint8_t *) { return "<MAC>"; }
static int splnet() { return 1; }
static void splx(int) {}

enum {
    EDCA_AC_BE = 0,
    ETHER_ADDR_LEN = 6,
    IEEE80211_NUM_TID = 16,
    IEEE80211_BA_MAX_WINSZ = 64,
    IEEE80211_ADDBA_REQ_INTVAL_MAX = 60,
    IEEE80211_BA_INIT = 0,
    IEEE80211_BA_REQUESTED = 1,
    IEEE80211_BA_AGREED = 2,
    IEEE80211_S_INIT = 0,
    IEEE80211_S_RUN = 4,
    IEEE80211_M_STA = 1,
    IEEE80211_M_MONITOR = 8,
    IEEE80211_C_AMSDU_IN_AMPDU = 1 << 0,
    IEEE80211_C_TX_AMPDU_SETUP_IN_HW = 1 << 1,
    IEEE80211_HTCAP_DELAYEDBA = 1 << 2,
    IEEE80211_ADDBA_AMSDU = 1 << 0,
    IEEE80211_ADDBA_BA_POLICY = 1 << 1,
    IEEE80211_ADDBA_TID_SHIFT = 2,
    IEEE80211_ADDBA_BUFSZ_SHIFT = 6,
    IEEE80211_CATEG_BA = 3,
    IEEE80211_ACTION_ADDBA_REQ = 0,
    IEEE80211_ACTION_ADDBA_RESP = 1,
    IEEE80211_ACTION_DELBA = 2,
    IEEE80211_STATUS_SUCCESS = 0,
    IEEE80211_STATUS_UNSPECIFIED = 1,
    IEEE80211_REASON_UNSPECIFIED = 1,
    IEEE80211_REASON_TIMEOUT = 39,
    IEEE80211_REASON_SETUP_REQUIRED = 38,
    IEEE80211_DUR_TU = 1024,
    IEEE80211_NODE_HT = 1 << 0,
    IEEE80211_NODE_VHT = 1 << 1,
    IEEE80211_NODE_HE = 1 << 2,
    IEEE80211_S_ASSOC = 3,
    IEEE80211_CHAN_WIDTH_20 = 0,
    IEEE80211_CHAN_WIDTH_40 = 1,
    IEEE80211_CHAN_WIDTH_80 = 2,
    IEEE80211_CHAN_WIDTH_160 = 3,
    IEEE80211_CHAN_WIDTH_80P80 = 4,
    IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_MASK = 7 << 23,
    IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT = 23,
    IEEE80211_HE_MAC_CAP3_MAX_AMPDU_LEN_EXP_MASK = 0x18,
    IEEE80211_AMPDU_PARAM_SS = 0x1c,
    IEEE80211_AMPDU_PARAM_SS_2 = 0x04,
    IEEE80211_AMPDU_PARAM_SS_4 = 0x08,
    IEEE80211_AMPDU_PARAM_SS_8 = 0x0c,
    IEEE80211_AMPDU_PARAM_SS_16 = 0x10,
    kIOReturnSuccess = 0,
};

// FIRMWARE

struct Packet {
    std::vector<uint8_t> bytes;
    unsigned frees = 0;
};
using mbuf_t = Packet *;
#define mtod(m, t) reinterpret_cast<t>((m)->bytes.data())
static size_t mbuf_len(mbuf_t m) { return m ? m->bytes.size() : 0; }
static void mbuf_freem(mbuf_t m) { if (m) ++m->frees; }

struct CTimeout {
    void (*fn)(void *) = nullptr;
    void *arg = nullptr;
    uint64_t deadlineMS = 0;
    bool pending = false;
};
static uint64_t nowMS = 1000;
static void timeout_set(CTimeout **t, void (*fn)(void *), void *arg) {
    if (!*t) *t = new CTimeout;
    (*t)->fn = fn;
    (*t)->arg = arg;
}
static void timeout_del(CTimeout **t) { if (*t) (*t)->pending = false; }
static bool timeout_pending(CTimeout **t) { return *t && (*t)->pending; }
static int timeout_add_msec(CTimeout **t, int ms) {
    if (!*t) *t = new CTimeout;
    (*t)->deadlineMS = nowMS + uint64_t(ms);
    (*t)->pending = true;
    return 1;
}
static int timeout_add_sec(CTimeout **t, int sec) { return timeout_add_msec(t, sec * 1000); }
static int timeout_add_usec(CTimeout **t, uint64_t us) { return timeout_add_msec(t, int(us / 1000)); }

struct ieee80211_frame {
    uint8_t i_fc[2], i_dur[2], i_addr1[6], i_addr2[6], i_addr3[6], i_seq[2];
};

struct ieee80211_node;
struct ieee80211com;

struct ieee80211_ba_buf { mbuf_t m; };
struct ieee80211_tx_ba {
    ieee80211_node *ba_ni = nullptr;
    int ba_state = 0;
    uint8_t ba_token = 0;
    int ba_timeout_val = 0;
    CTimeout *ba_to = nullptr;
    uint16_t ba_winsize = 0, ba_winstart = 0, ba_winend = 0, ba_params = 0;
};
struct ieee80211_rx_ba {
    ieee80211_node *ba_ni = nullptr;
    int ba_state = 0;
    int ba_timeout_val = 0;
    uint16_t ba_winsize = 0, ba_winstart = 0;
    CTimeout *ba_to = nullptr, *ba_gap_to = nullptr;
    ieee80211_ba_buf *ba_buf = nullptr;
};

struct ieee80211_node {
    ieee80211com *ni_ic = nullptr;
    uint8_t ni_macaddr[6] = {2, 3, 4, 5, 6, 7};
    uint32_t ni_flags = IEEE80211_NODE_HT | IEEE80211_NODE_VHT;
    uint8_t ni_chw = IEEE80211_CHAN_WIDTH_160;
    uint8_t ni_rx_nss = 2;
    uint8_t ni_ampdu_param = IEEE80211_AMPDU_PARAM_SS_4;
    uint32_t ni_vhtcaps = 7 << IEEE80211_VHTCAP_MAX_A_MPDU_LENGTH_EXPONENT_SHIFT;
    struct { uint8_t mac_cap_info[6] = {}; } ni_he_cap_elem;
    uint16_t ni_qos_txseqs[16] = {};
    int ni_addba_req_intval[16] = {1, 1, 1, 1, 1, 1, 1, 1};
    CTimeout *ni_addba_req_to[16] = {};
    ieee80211_tx_ba ni_tx_ba[16] = {};
    ieee80211_rx_ba ni_rx_ba[16] = {};
};

struct _ifnet { void *if_softc = nullptr; };
struct ActionRecord { int categ, action, arg; uint16_t winstart, winsize; uint8_t token; };
static std::vector<ActionRecord> sentActions;
static int sendActionFail = 0;

struct ieee80211com {
    _ifnet ic_if;
    void *ic_softc = nullptr;
    int ic_opmode = IEEE80211_M_STA;
    int ic_state = IEEE80211_S_RUN;
    uint32_t ic_caps = IEEE80211_C_AMSDU_IN_AMPDU | IEEE80211_C_TX_AMPDU_SETUP_IN_HW;
    uint16_t ic_htcaps = 0;
    uint8_t ic_ampdu_params = IEEE80211_AMPDU_PARAM_SS_4;
    uint8_t ic_dialog_token = 1;
    ieee80211_node *ic_bss = nullptr;
    struct {
        unsigned is_ht_tx_ba_agreements = 0;
        unsigned is_ht_tx_ba_timeout = 0;
    } ic_stats;
    int (*ic_ampdu_tx_start)(ieee80211com *, ieee80211_node *, uint8_t) = nullptr;
    void (*ic_ampdu_tx_stop)(ieee80211com *, ieee80211_node *, uint8_t) = nullptr;
    int (*ic_ampdu_rx_stop)(ieee80211com *, ieee80211_node *, uint8_t) = nullptr;
    int (*ic_send_action)(ieee80211com *, ieee80211_node *, int, int, int) = nullptr;
};
#define IEEE80211_SEND_ACTION(ic, ni, categ, action, arg) ((ic)->ic_send_action((ic), (ni), (categ), (action), (arg)))

struct iwx_node {
    ieee80211_node in_ni;
    uint32_t in_id = 0, in_color = 1;
    uint8_t in_macaddr[6] = {2, 3, 4, 5, 6, 7};
};

struct task {};
static int systq_dummy = 0;
static int *systq = &systq_dummy;

struct iwx_tid_data {
    uint16_t seq_number;
    uint16_t next_reclaimed;
    uint16_t qid;
    uint16_t ssn;
    uint16_t next_ssn;
    uint8_t agg_awaiting_resp;
};
struct iwx_ba_task_data {
    uint32_t start_tidmask;
    uint32_t enable_tidmask;
    uint32_t stop_tidmask;
};
struct iwx_tx_ring {
    unsigned ring_count = 0;
    int qid = IWX_INVALID_QUEUE, cur = 0, tail = 0, queued = 0;
    bool dynamicallyAllocated = false;
};
struct IwxTxQueueSetup {
    uint32_t writePointer = 0;
};
struct iwx_softc {
    ieee80211com sc_ic;
    int sc_generation = 1;
    int sc_device_family = IWX_DEVICE_FAMILY_22000;
    uint32_t sc_flags = IWX_FLAG_STA_ACTIVE;
    int ns_nstate = IEEE80211_S_RUN;
    int first_data_qid = 1;
    unsigned sc_rx_ba_sessions = 0;
    task ba_task;
    iwx_ba_task_data ba_rx = {}, ba_tx = {};
    iwx_tid_data sc_tid_data[IWX_MAX_TID_COUNT + 1] = {};
    uint16_t sc_tx_agg_tidmask = 0;
    iwx_tx_ring txq[31] = {};
};

struct Loop { bool gated = true; bool inGate() const { return gated; } };
struct Gate {
    Loop *loop;
    IOReturn runAction(IOReturn (*fn)(OSObject *, void *, void *, void *, void *), void *arg) {
        bool old = loop->gated; loop->gated = true;
        IOReturn r = fn(nullptr, arg, nullptr, nullptr, nullptr);
        loop->gated = old;
        return r;
    }
};

struct ItlIwx {
    iwx_softc com = {};
    iwx_node bss = {};
    Loop loop;
    Gate gate{&loop};
    struct {
        bool ownsQueue(unsigned q) const { return q == 9; }
        struct { bool ownsQueue(unsigned q) const { return q == 10; } } blockAckTX;
    } awdlRadio;

    unsigned tvqmAllocCalls = 0, resets = 0, frees = 0, taskSchedules = 0, rmStaCalls = 0;
    unsigned nicLocks = 0;
    int nextAssignedQid = 5;
    uint16_t nextWritePointer = 0x00fe;
    int failAddSta = 0;
    std::vector<struct iwx_add_sta_cmd> addStaCmds;

    ItlIwx() {
        com.sc_ic.ic_softc = &com;
        com.sc_ic.ic_if.if_softc = &com;
        bss.in_ni.ni_ic = &com.sc_ic;
        com.sc_ic.ic_bss = &bss.in_ni;
        com.sc_ic.ic_ampdu_tx_start = &ItlIwx::iwx_ampdu_tx_start;
        com.sc_ic.ic_ampdu_tx_stop = &ItlIwx::iwx_ampdu_tx_stop;
        com.sc_ic.ic_send_action = [](ieee80211com *ic, ieee80211_node *ni, int categ, int action, int arg) -> int {
            if (sendActionFail) return sendActionFail;
            const auto &ba = ni->ni_tx_ba[arg & 0xf];
            sentActions.push_back({categ, action, arg, ba.ba_winstart, ba.ba_winsize, ba.ba_token});
            (void)ic;
            return 0;
        };
        for (unsigned q = 0; q <= IWX_QID_MGMT; ++q) {
            com.txq[q].qid = int(q);
            com.txq[q].ring_count = 256;
        }
        for (auto &td : com.sc_tid_data) td.qid = IWX_INVALID_QUEUE;
    }
    ~ItlIwx() {
        for (auto *to : bss.in_ni.ni_addba_req_to) delete to;
        for (auto &ba : bss.in_ni.ni_tx_ba) delete ba.ba_to;
    }

    Loop *getMainWorkLoop() { return &loop; }
    Gate *getMainCommandGate() { return &gate; }
    int iwx_nic_lock(iwx_softc *) { ++nicLocks; return 1; }
    void iwx_nic_unlock(iwx_softc *) { assert(nicLocks > 0); --nicLocks; }
    void iwx_add_task(iwx_softc *, int *, task *) { ++taskSchedules; }
    bool iwx_mimo_enabled(iwx_softc *) { return true; }
    int iwx_flush_sta(iwx_softc *, iwx_node *) { return 0; }
    int iwx_rm_sta_cmd(iwx_softc *sc, iwx_node *) { ++rmStaCalls; sc->sc_flags &= ~IWX_FLAG_STA_ACTIVE; return 0; }
    int iwx_sta_rx_agg(iwx_softc *, ieee80211_node *, uint8_t, uint16_t, uint16_t, int, int) { return 0; }

    int iwx_tvqm_enable_txq(iwx_softc *sc, int tid, int, uint32_t size, uint8_t station, IwxTxQueueSetup *result) {
        assert(station == IWX_STATION_ID && tid >= 0 && tid < IWX_MAX_TID_COUNT);
        ++tvqmAllocCalls;
        const int q = nextAssignedQid;
        sc->txq[q].qid = q;
        sc->txq[q].ring_count = size;
        sc->txq[q].cur = nextWritePointer & (size - 1);
        sc->txq[q].tail = sc->txq[q].cur;
        sc->txq[q].dynamicallyAllocated = q > IWX_QID_MGMT;
        if (result) result->writePointer = nextWritePointer;
        return q;
    }
    void iwx_reset_tx_ring(iwx_softc *, iwx_tx_ring *r) {
        ++resets;
        r->queued = r->cur = r->tail = 0;
    }
    void iwx_free_tx_ring(iwx_softc *, iwx_tx_ring *r) {
        assert(r->qid > IWX_QID_MGMT && "Static rings <= IWX_QID_MGMT must never be freed");
        ++frees;
        r->dynamicallyAllocated = false;
        r->ring_count = 0;
        r->qid = IWX_INVALID_QUEUE;
    }
    int iwx_send_cmd_pdu_status(iwx_softc *, uint32_t id, uint16_t len, const void *data, uint32_t *status) {
        assert(id == IWX_ADD_STA && len == sizeof(struct iwx_add_sta_cmd));
        if (failAddSta) return failAddSta;
        addStaCmds.push_back(*static_cast<const struct iwx_add_sta_cmd *>(data));
        *status = IWX_ADD_STA_SUCCESS;
        return 0;
    }

    int iwx_tvqm_alloc_txq(iwx_softc *, int, int, IwxTxQueueSetup * = nullptr);
    void iwx_free_sta_tx_queues(iwx_softc *);
    int iwx_add_sta_cmd(iwx_softc *, iwx_node *, int);
    int iwx_rm_sta(iwx_softc *, iwx_node *);
    static void iwx_ba_task(void *);
    static int iwx_ampdu_tx_start(ieee80211com *, ieee80211_node *, uint8_t);
    static void iwx_ampdu_tx_stop(ieee80211com *, ieee80211_node *, uint8_t);
};

int ieee80211_addba_request(ieee80211com *, ieee80211_node *, uint16_t, uint8_t);
void ieee80211_delba_request(ieee80211com *, ieee80211_node *, uint16_t, uint8_t, uint8_t);
void ieee80211_tx_ba_timeout(void *);
void ieee80211_recv_addba_resp(ieee80211com *, mbuf_t, ieee80211_node *);
void ieee80211_addba_resp_accept(ieee80211com *, ieee80211_node *, uint8_t);
void ieee80211_addba_resp_refuse(ieee80211com *, ieee80211_node *, uint8_t, uint16_t);
void ieee80211_node_trigger_addba_req(ieee80211_node *, int);

// PRODUCTION

static Packet makeAddbaResp(uint8_t token, uint16_t status, uint8_t tid, uint16_t bufsz) {
    Packet p;
    p.bytes.assign(sizeof(ieee80211_frame) + 9, 0);
    uint8_t *frm = p.bytes.data() + sizeof(ieee80211_frame);
    frm[0] = IEEE80211_CATEG_BA;
    frm[1] = IEEE80211_ACTION_ADDBA_RESP;
    frm[2] = token;
    LE_WRITE_2(&frm[3], status);
    const uint16_t params = uint16_t((bufsz << 6) | (tid << 2) | IEEE80211_ADDBA_BA_POLICY);
    LE_WRITE_2(&frm[5], params);
    LE_WRITE_2(&frm[7], 0);
    return p;
}

int main() {
    ItlIwx d;
    auto *ic = &d.com.sc_ic;
    auto *ni = &d.bss.in_ni;

    // 1. Initial trigger schedules ADDBA in 20 ms (not 1000 ms) when intval == 1.
    ieee80211_node_trigger_addba_req(ni, 0);
    assert(timeout_pending(&ni->ni_addba_req_to[0]));
    assert(ni->ni_addba_req_to[0]->deadlineMS == nowMS + 20);

    // 2. Singleton TVQM setup on Station 0: ieee80211_addba_request queues start_tidmask without
    //    sending OTA ADDBA_REQ; iwx_ba_task allocates TVQM qid=5, keeps tid_disable_tx == 0xffff,
    //    and locally transitions ba_state -> IEEE80211_BA_AGREED.
    sentActions.clear();
    assert(ieee80211_addba_request(ic, ni, 0, 0) == EBUSY);
    assert(sentActions.empty());
    assert(ni->ni_tx_ba[0].ba_state == IEEE80211_BA_REQUESTED);
    assert(d.com.ba_tx.start_tidmask == 1);

    ItlIwx::iwx_ba_task(&d.com);
    assert(d.nicLocks == 0);
    assert(d.tvqmAllocCalls == 1);
    assert(d.com.sc_tid_data[0].qid == 5);
    assert(d.com.sc_tid_data[0].agg_awaiting_resp == 0);
    assert(d.com.sc_tx_agg_tidmask == 0 && d.addStaCmds.empty());
    assert(sentActions.empty());
    assert(ni->ni_tx_ba[0].ba_state == IEEE80211_BA_AGREED);
    assert(ni->ni_tx_ba[0].ba_winstart == 0x00fe);
    assert(ni->ni_qos_txseqs[0] == 0x00fe);
    assert(ic->ic_stats.is_ht_tx_ba_agreements == 1);

    // 3. Station 0 update=0 and update=1 always keep tid_disable_tx == 0xffff.
    d.com.sc_flags &= ~IWX_FLAG_STA_ACTIVE;
    assert(d.iwx_add_sta_cmd(&d.com, &d.bss, 0) == 0);
    assert(d.addStaCmds.size() == 1 && d.addStaCmds.back().tid_disable_tx == 0xffff);
    assert(d.iwx_add_sta_cmd(&d.com, &d.bss, 1) == 0);
    assert(d.addStaCmds.size() == 2 && d.addStaCmds.back().tid_disable_tx == 0xffff);

    // 4. DELBA preserves qid=5 for reuse without reallocating TVQM queues.
    d.com.txq[5].cur = (d.com.txq[5].cur + 4) & (d.com.txq[5].ring_count - 1);
    ieee80211_delba_request(ic, ni, 0, 1, 0);
    assert(ni->ni_tx_ba[0].ba_state == IEEE80211_BA_INIT);
    assert(d.com.sc_tid_data[0].qid == 5);

    assert(ieee80211_addba_request(ic, ni, 999, 0) == EBUSY);
    ItlIwx::iwx_ba_task(&d.com);
    assert(d.tvqmAllocCalls == 1);
    assert(sentActions.empty());
    assert(ni->ni_tx_ba[0].ba_state == IEEE80211_BA_AGREED);

    // 5. iwx_rm_sta frees dynamically allocated Station 0 TVQM ring (qid=5 > IWX_QID_MGMT)
    //    while keeping static rings 0..IWX_QID_MGMT intact.
    assert(d.iwx_rm_sta(&d.com, &d.bss) == 0);
    assert(d.frees == 1 && !d.com.txq[5].dynamicallyAllocated);
    assert(d.com.sc_tid_data[0].qid == IWX_INVALID_QUEUE);
    assert(d.com.txq[IWX_QID_MGMT].ring_count == 256);
    return 0;
}
"""
        prod = "\n".join((iwx_defs, add_sta_src, rm_sta_src, proto_defs, input_defs, node_defs))
        fw = firmware_declarations(("iwx_add_sta_cmd",), harness + "\n" + prod)
        full = harness.replace("// FIRMWARE", fw).replace("// PRODUCTION", prod)
        self.compile_run(full)

    def test_p117_station0_amsdu_incomplete_guard_in_both_loops_and_10ms_timeout(self):
        extra = p116.INFRA_HELPERS + r"""
static void receiveInfraSubframe(ItlIwx &d, Packet *m, unsigned baid, uint16_t sn, uint16_t nssn,
                                 int sub, bool last) {
    iwx_rx_mpdu_desc desc = {};
    desc.status = IWX_RX_MPDU_RES_STATUS_SRC_STA_FOUND | (uint32_t(IWX_STATION_ID) << 24);
    desc.reorder_data = (uint32_t(baid) << 24) | (uint32_t(sn & 0x0fffU) << 12) | uint32_t(nssn & 0x0fffU);
    if (sub >= 0) {
        desc.mac_flags2 = IWX_RX_MPDU_MFLG2_AMSDU;
        desc.amsdu_info = uint8_t(sub) | (last ? IWX_RX_MPDU_AMSDU_LAST_SUBFRAME : 0);
        m->bytes[24] |= 0x80;
    }
    ieee80211_rxinfo rxi = {};
    mbuf_list ml;
    if (!d.iwx_rx_reorder(&d.com, m, 0, &desc, 0, 0, 0, &rxi, &ml)) {
        d.iwx_rx_frame(&d.com, m, 0, 0, 0, 0, 0, &rxi, &ml);
    }
    assert(ml_empty(&ml));
}

int main() {
    // 1. Loop 2 (head_sn continuation) + BAR mid-A-MSDU on Station 0:
    //    SN=200 missing, SN=201 subframe 0 (last=false) + SN=202 stored, then BAR(201) arrives.
    //    Station 0 MUST NOT release 201.0 or drop 201.1; once 201.1 (last=true) arrives,
    //    201.0, 201.1, and 202 release in exact order with 0 below_ba_winstart drops!
    {
        ItlIwx d;
        const unsigned baid = startInfra(d, 0, 200, 64);
        auto &buf = d.com.sc_rxba_data[baid].reorder_buf;

        auto s201_0 = infraPacket(d, 0, 201, 2010);
        auto s201_1 = infraPacket(d, 0, 201, 2011);
        auto s202 = infraPacket(d, 0, 202, 2020);
        receiveInfraSubframe(d, s201_0.get(), baid, 201, 200, 0, false);
        receiveInfraSubframe(d, s202.get(), baid, 202, 200, -1, true);
        assert(d.apDelivered.empty() && buf.num_stored == 2);

        mbuf_list ml;
        d.iwx_release_frames(&d.com, &d.bss, &d.com.sc_rxba_data[baid], &buf, 201, &ml);
        assert(d.apDelivered.empty() && buf.head_sn == 201 && buf.num_stored == 2);

        receiveInfraSubframe(d, s201_1.get(), baid, 201, 203, 1, true);
        assert((d.apDelivered == std::vector<uint16_t>{2010, 2011, 2020}));
        assert(d.com.sc_ic.ic_stats.is_ht_rx_frame_below_ba_winstart == 0);
        assert(buf.head_sn == 203 && buf.num_stored == 0);

        // 2. Loop 1 (nssn loop) mid-A-MSDU on Station 0:
        //    SN=203 missing, SN=204 subframe 0 (last=false) + SN=205 stored, then SN=203 arrives
        //    carrying hardware nssn=206 before SN=204 subframe 1! SN=203 releases immediately,
        //    Loop 1 pauses at 204, and SN=204 subframe 1 releases 204.0, 204.1, 205 cleanly!
        d.apDelivered.clear();
        auto s203 = infraPacket(d, 0, 203, 2030);
        auto s204_0 = infraPacket(d, 0, 204, 2040);
        auto s204_1 = infraPacket(d, 0, 204, 2041);
        auto s205 = infraPacket(d, 0, 205, 2050);
        receiveInfraSubframe(d, s204_0.get(), baid, 204, 203, 0, false);
        receiveInfraSubframe(d, s205.get(), baid, 205, 203, -1, true);
        receiveInfraSubframe(d, s203.get(), baid, 203, 206, -1, true);
        assert((d.apDelivered == std::vector<uint16_t>{2030}));
        assert(buf.head_sn == 204 && buf.num_stored == 2);

        nowUS += 50ULL;
        receiveInfraSubframe(d, s204_1.get(), baid, 204, 206, 1, true);
        assert((d.apDelivered == std::vector<uint16_t>{2030, 2040, 2041, 2050}));
        assert(d.com.sc_ic.ic_stats.is_ht_rx_frame_below_ba_winstart == 0);
        assert(buf.head_sn == 206 && buf.num_stored == 0);

        // 3. Anti-wedge check: if SN=206 is missing and SN=207 subframe 1 fails FCS on air,
        //    the 10 ms reorder timer flushes SN=207 subframe 0 cleanly without wedging head_sn!
        d.apDelivered.clear();
        auto s207_0 = infraPacket(d, 0, 207, 2070);
        receiveInfraSubframe(d, s207_0.get(), baid, 207, 206, 0, false);
        assert(d.apDelivered.empty() && buf.num_stored == 1);
        assert(buf.reorder_timer->deadline == nowUS + 100000ULL);
        fireReorderTimer(buf);
        assert((d.apDelivered == std::vector<uint16_t>{2070}));
        assert(buf.head_sn == 208 && buf.num_stored == 0);
        assert(!d.iwx_sta_rx_agg(&d.com, &d.bss, 0, 0, 0, 0, 0));
    }
    return 0;
}
"""
        self.compile_run(p116._build_infra_ba_harness(extra))

    def test_p117_m_dup_pkt_honors_ether_align_offset(self):
        dup_src = definition("itl80211/openbsd/sys/_mbuf.h", "m_dup_pkt")
        harness = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

enum { M_NOWAIT = 1, ETHER_ALIGN = 2, ETHER_HDR_LEN = 14 };
using mbuf_how_t = int;

struct MockMbuf {
    alignas(16) uint8_t storage[2048] = {};
    uint8_t *data = storage;
    size_t len = 0, pkthdr_len = 0, cap = sizeof(storage);
    void *header = reinterpret_cast<void *>(0xdead);
    uint32_t tag = 0;
    unsigned frees = 0;
};
using mbuf_t = MockMbuf *;

static std::vector<MockMbuf *> pool;
static int failAlloc = 0;

static size_t mbuf_pkthdr_len(mbuf_t m) { return m->pkthdr_len; }
static void *mbuf_data(mbuf_t m) { return m->data; }
static void mbuf_pkthdr_setlen(mbuf_t m, size_t l) { m->pkthdr_len = l; }
static void mbuf_pkthdr_setheader(mbuf_t m, void *h) { m->header = h; }
static void mbuf_freem(mbuf_t m) { if (m) ++m->frees; }

static int mbuf_dup(mbuf_t src, mbuf_how_t, mbuf_t *out) {
    if (failAlloc) return 12;
    auto *m = new MockMbuf();
    pool.push_back(m);
    memcpy(m->storage, src->data, src->pkthdr_len);
    m->data = m->storage;
    m->len = m->pkthdr_len = src->pkthdr_len;
    m->tag = src->tag;
    *out = m;
    return 0;
}
static int mbuf_allocpacket(mbuf_how_t, size_t pktlen, unsigned int *maxchunks, mbuf_t *out) {
    assert(maxchunks && *maxchunks == 1);
    if (failAlloc || pktlen > sizeof(MockMbuf::storage)) return 12;
    auto *m = new MockMbuf();
    pool.push_back(m);
    m->cap = pktlen;
    *out = m;
    return 0;
}
static int mbuf_copydata(mbuf_t src, size_t off, size_t len, void *dst) {
    if (off + len > src->pkthdr_len) return 22;
    memcpy(dst, src->data + off, len);
    return 0;
}
static int mbuf_copy_pkthdr(mbuf_t dst, mbuf_t src) {
    dst->tag = src->tag;
    return 0;
}
static int mbuf_setdata(mbuf_t m, void *data, size_t len) {
    auto *p = static_cast<uint8_t *>(data);
    if (p < m->storage || size_t(p - m->storage) + len > m->cap) return 22;
    m->data = p;
    m->len = len;
    return 0;
}

// PRODUCTION

int main() {
    MockMbuf src;
    src.len = src.pkthdr_len = 64;
    src.tag = 0x12345678U;
    for (size_t i = 0; i < 64; ++i) src.storage[i] = uint8_t(0xa0 + i);
    // Unaligned IP header at storage + 14 (2 mod 4):
    assert(((reinterpret_cast<uintptr_t>(src.data) + ETHER_HDR_LEN) & 3U) == 2U);

    mbuf_t aligned = m_dup_pkt(&src, ETHER_ALIGN, M_NOWAIT);
    assert(aligned != nullptr);
    assert(aligned->len == 64 && aligned->pkthdr_len == 64);
    assert(aligned->tag == 0x12345678U && aligned->header == nullptr);
    assert(((reinterpret_cast<uintptr_t>(aligned->data) + ETHER_HDR_LEN) & 3U) == 0U);
    assert(memcmp(aligned->data, src.data, 64) == 0);

    mbuf_t unshifted = m_dup_pkt(&src, 0, M_NOWAIT);
    assert(unshifted != nullptr && unshifted->len == 64);
    assert(memcmp(unshifted->data, src.data, 64) == 0);

    failAlloc = 1;
    assert(m_dup_pkt(&src, ETHER_ALIGN, M_NOWAIT) == nullptr);
    for (auto *m : pool) delete m;
    return 0;
}
"""
        self.compile_run(harness.replace("// PRODUCTION", dup_src))

    def test_p117_umac_scan_5ghz_passive2active_and_dense_direct_ssid_packing(self):
        valid_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "iwx_scan_ssid_valid")
        pack_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "iwx_pack_direct_scan_ssids")
        chan_src = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_umac_scan_fill_channels")
        harness = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#define AIRPORT_WCL 1
#include "AirportItlwm/WCL/WCL.hpp"

#define htole16(v) uint16_t(v)
#define htole32(v) uint32_t(v)
#define isset(p, b) ((p)[(b) / 8] & (1U << ((b) % 8)))
#define nitems(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) std::min(a, b)
#define IEEE80211_IS_CHAN_2GHZ(c) ((c)->ic_freq < 3000)

enum {
    IEEE80211_NWID_LEN = 32,
    IEEE80211_ELEMID_SSID = 0,
    IEEE80211_CHAN_MAX = 255,
    IEEE80211_CHAN_PASSIVE = 0x200,
    IEEE80211_CHAN_DFS = 0x400,
    IWX_PROBE_OPTION_MAX = 20,
    IWX_UCODE_TLV_API_SCAN_EXT_CHAN_VER = 0,
    IWX_PHY_BAND_24 = 0,
    IWX_PHY_BAND_5 = 1,
};

struct iwx_ssid_ie {
    uint8_t id;
    uint8_t len;
    uint8_t ssid[IEEE80211_NWID_LEN];
};
struct ieee80211_channel { int ic_flags = 0, ic_freq = 0; };
struct ieee80211com {
    ieee80211_channel ic_channels[256] = {};
    uint8_t ic_des_essid[IEEE80211_NWID_LEN] = {};
    int ic_des_esslen = 0;
    uint8_t ic_wcl_scan_channels[32] = {};
    uint8_t ic_wcl_join_channel = 0;
    bool ic_wcl_scan_requested = false;
    bool ic_wcl_scan_restricted = false;
    bool ic_wcl_scan_active = false;
    bool ic_wcl_join_requested = false;
    uint8_t ic_wcl_scan_ssid[IEEE80211_NWID_LEN] = {};
    uint8_t ic_wcl_scan_ssid_length = 0;
    uint8_t ic_wcl_scan_ssid_count = 0;
    uint8_t ic_wcl_scan_ssid_lengths[10] = {};
    uint8_t ic_wcl_scan_ssids[10][IEEE80211_NWID_LEN] = {};
};
struct iwx_softc {
    ieee80211com sc_ic;
    uint8_t sc_capa_n_scan_channels = 64;
    uint8_t sc_ucode_api[1] = {1};
};
struct iwx_scan_channel_cfg_umac {
    uint32_t flags = 0;
    struct { uint8_t channel_num, band, iter_count, iter_interval; } v2;
    struct { uint8_t channel_num, iter_count; uint16_t iter_interval; } v1;
};
static unsigned ieee80211_chan2ieee(ieee80211com *ic, ieee80211_channel *c) { return unsigned(c - ic->ic_channels); }
static unsigned ieee80211_mhz2ieee(unsigned f, int) { return (f - (f < 3000 ? 2407 : 5000)) / 5; }

struct ItlIwx {
    uint8_t iwx_umac_scan_fill_channels(iwx_softc *, iwx_scan_channel_cfg_umac *, int, int);
};

// PRODUCTION

int main() {
    iwx_softc sc = {};
    auto &ic = sc.sc_ic;
    // CN LAR profile: 5 GHz UNII-1/3 channels marked IEEE80211_CHAN_PASSIVE (non-DFS), Ch 52 DFS.
    ic.ic_channels[6] = {1, 2437};
    ic.ic_channels[36] = {IEEE80211_CHAN_PASSIVE, 5180};
    ic.ic_channels[48] = {IEEE80211_CHAN_PASSIVE, 5240};
    ic.ic_channels[52] = {IEEE80211_CHAN_PASSIVE | IEEE80211_CHAN_DFS, 5260};
    ic.ic_channels[161] = {IEEE80211_CHAN_PASSIVE, 5805};

    // 1. Unassociated 5 GHz hidden auto-join where IO80211ScanRequest set scan.type=2
    //    (ic_wcl_scan_active=false) while supplying "HiddenLab5G" in scan.ssid + scan.ssids[0]:
    //    must promote scanActive=true, deduplicate into slot 0 without clobbering, and set
    //    chan->flags=0x1 on 5g36, 5g48, 5g161 while keeping DFS 5g52 flags=0!
    ic.ic_wcl_scan_requested = true;
    ic.ic_wcl_scan_active = false;
    memcpy(ic.ic_wcl_scan_ssid, "HiddenLab5G", 11);
    ic.ic_wcl_scan_ssid_length = 11;
    ic.ic_wcl_scan_ssid_count = 2;
    ic.ic_wcl_scan_ssid_lengths[0] = 8; // All-NUL bogus entry must be ignored!
    memset(ic.ic_wcl_scan_ssids[0], 0, 8);
    memcpy(ic.ic_wcl_scan_ssids[1], "HiddenLab5G", 11); // Duplicate must not consume slot 1!
    ic.ic_wcl_scan_ssid_lengths[1] = 11;

    iwx_ssid_ie direct[IWX_PROBE_OPTION_MAX] = {};
    bool active = false;
    int firstLen = 0;
    unsigned n = iwx_pack_direct_scan_ssids(&ic, 0, direct, IWX_PROBE_OPTION_MAX, &active, &firstLen);
    assert(active && n == 1 && firstLen == 11);
    assert(direct[0].len == 11 && memcmp(direct[0].ssid, "HiddenLab5G", 11) == 0);

    ItlIwx hal;
    iwx_scan_channel_cfg_umac chans[16] = {};
    assert(hal.iwx_umac_scan_fill_channels(&sc, chans, int(n), 0) == 5);
    assert(chans[0].v2.channel_num == 6 && chans[0].flags == 0x1);
    assert(chans[1].v2.channel_num == 36 && chans[1].flags == 0x1);
    assert(chans[2].v2.channel_num == 48 && chans[2].flags == 0x1);
    assert(chans[3].v2.channel_num == 52 && chans[3].flags == 0x0); // DFS stays passive
    assert(chans[4].v2.channel_num == 161 && chans[4].flags == 0x1);

    // 2. Combined scan with wildcard (len==0) + two distinct hidden SSIDs preserves all 3 slots!
    ic.ic_wcl_scan_active = true;
    ic.ic_wcl_scan_ssid_count = 2;
    ic.ic_wcl_scan_ssid_lengths[0] = 0; // Explicit wildcard
    memcpy(ic.ic_wcl_scan_ssids[1], "OfficeHidden", 12);
    ic.ic_wcl_scan_ssid_lengths[1] = 12;
    memset(direct, 0, sizeof(direct));
    n = iwx_pack_direct_scan_ssids(&ic, 1, direct, IWX_PROBE_OPTION_MAX, &active, &firstLen);
    assert(active && n == 3 && firstLen == 11);
    assert(direct[0].len == 11 && memcmp(direct[0].ssid, "HiddenLab5G", 11) == 0);
    assert(direct[1].len == 12 && memcmp(direct[1].ssid, "OfficeHidden", 12) == 0);
    assert(direct[2].len == 0);

    memset(chans, 0, sizeof(chans));
    assert(hal.iwx_umac_scan_fill_channels(&sc, chans, int(n), 1) == 5);
    assert(chans[2].v2.channel_num == 48 && chans[2].flags == 0x7);
    assert(chans[3].v2.channel_num == 52 && chans[3].flags == 0x0);

    // 3. startWCLJoin on 5 GHz hidden AP (ic_wcl_scan_requested=false, ic_wcl_join_requested=true)
    //    packs ic_des_essid and sets chan->flags=0x1 on target 5g48 channel!
    ic = {};
    ic.ic_channels[48] = {IEEE80211_CHAN_PASSIVE, 5240};
    ic.ic_wcl_join_requested = true;
    ic.ic_wcl_join_channel = 48;
    memcpy(ic.ic_des_essid, "HiddenLab5G", 11);
    ic.ic_des_esslen = 11;
    memset(direct, 0, sizeof(direct));
    n = iwx_pack_direct_scan_ssids(&ic, 0, direct, IWX_PROBE_OPTION_MAX, &active, &firstLen);
    assert(active && n == 1 && direct[0].len == 11);
    memset(chans, 0, sizeof(chans));
    assert(hal.iwx_umac_scan_fill_channels(&sc, chans, int(n), 0) == 1);
    assert(chans[0].v2.channel_num == 48 && chans[0].flags == 0x1);
    return 0;
}
"""
        self.compile_run(
            harness.replace("// PRODUCTION", "\n".join((valid_src, pack_src, chan_src)))
        )

    def test_p117_net80211_hidden_ssid_nul_rejection_probe_ie_preservation_and_match_bss(self):
        hidden_src = definition(
            "itl80211/openbsd/net80211/ieee80211_input.c",
            "ieee80211_ssid_is_hidden",
        )
        match_src = definition(
            "itl80211/openbsd/net80211/ieee80211_node.c",
            "ieee80211_match_bss",
        )
        probe_src = (support.ROOT / "itl80211/openbsd/net80211/ieee80211_input.c").read_text()
        self.assertIn("struct ieee80211_beacon_event event = {ni, ies, (size_t)(end - ies),", probe_src)
        self.assertIn("(const uint8_t *)wh, (size_t)(end - (const uint8_t *)wh), isprobe};", probe_src)
        self.assertIn("!ieee80211_ssid_is_hidden(ssid)", probe_src)

        harness = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define AIRPORT_WCL 1
#define IEEE80211_STA_ONLY 1
#define XYLog(...) ((void)0)
#define ISSET(t, f) ((t) & (f))
#define IEEE80211_ADDR_EQ(a, b) (memcmp((a), (b), 6) == 0)
#define IC2IFP(ic) (&(ic)->ic_if)

using u_int8_t = uint8_t;
using u_int16_t = uint16_t;

enum {
    IEEE80211_NWID_LEN = 32,
    IEEE80211_CHAN_MAX = 255,
    IEEE80211_CAPINFO_ESS = 0x01,
    IEEE80211_CAPINFO_PRIVACY = 0x10,
    IEEE80211_F_RSNON = 1 << 0,
    IEEE80211_F_PSK = 1 << 1,
    IEEE80211_F_DESBSSID = 1 << 2,
    IEEE80211_F_AUTO_JOIN = 1 << 3,
    IEEE80211_F_DONEGO = 1 << 4,
    IEEE80211_C_MFP = 1 << 5,
    IEEE80211_F_MFPR = 1 << 6,
    IEEE80211_F_BGSCAN = 1 << 7,
    IEEE80211_F_WEPON = 1 << 8,
    IEEE80211_RATE_BASIC = 0x80,
    IEEE80211_PROTO_RSN = 1 << 0,
    IEEE80211_AKM_PSK = 1 << 0,
    IEEE80211_AKM_SHA256_PSK = 1 << 1,
    IEEE80211_CIPHER_WEP40 = 1 << 0,
    IEEE80211_CIPHER_TKIP = 1 << 1,
    IEEE80211_CIPHER_CCMP = 1 << 2,
    IEEE80211_CIPHER_WEP104 = 1 << 3,
    IEEE80211_CIPHER_BIP = 1 << 4,
    IEEE80211_RSNCAP_MFPC = 1 << 6,
    IEEE80211_RSNCAP_MFPR = 1 << 7,
    IEEE80211_NODE_ASSOCFAIL_CHAN = 0x01,
    IEEE80211_NODE_ASSOCFAIL_IBSS = 0x02,
    IEEE80211_NODE_ASSOCFAIL_PRIVACY = 0x04,
    IEEE80211_NODE_ASSOCFAIL_BASIC_RATE = 0x08,
    IEEE80211_NODE_ASSOCFAIL_ESSID = 0x10,
    IEEE80211_NODE_ASSOCFAIL_BSSID = 0x20,
    IEEE80211_NODE_ASSOCFAIL_WPA_PROTO = 0x40,
    IEEE80211_NODE_ASSOCFAIL_WPA_KEY = 0x80,
    IFF_DEBUG = 1,
};

struct _ifnet { int if_flags = 0; const char *if_xname = "en1"; };
struct ieee80211_channel { int ic_flags = 1; };
struct ieee80211_node {
    uint8_t ni_macaddr[6] = {};
    uint8_t ni_bssid[6] = {};
    ieee80211_channel *ni_chan = nullptr;
    uint16_t ni_capinfo = IEEE80211_CAPINFO_ESS | IEEE80211_CAPINFO_PRIVACY;
    uint8_t ni_esslen = 0;
    uint8_t ni_essid[IEEE80211_NWID_LEN] = {};
    unsigned ni_rsnprotos = IEEE80211_PROTO_RSN;
    unsigned ni_rsnakms = IEEE80211_AKM_PSK;
    unsigned ni_rsngroupcipher = IEEE80211_CIPHER_CCMP;
    unsigned ni_rsnciphers = IEEE80211_CIPHER_CCMP;
    unsigned ni_rsngroupmgmtcipher = IEEE80211_CIPHER_BIP;
    unsigned ni_rsncaps = 0;
    int ni_assoc_fail = 0;
};
struct ieee80211com {
    _ifnet ic_if;
    ieee80211_node *ic_bss = nullptr;
    ieee80211_channel ic_channels[256] = {};
    uint8_t ic_chan_active[32] = {};
    ieee80211_channel *ic_des_chan = nullptr;
    uint32_t ic_flags = IEEE80211_F_RSNON | IEEE80211_F_PSK | IEEE80211_F_DESBSSID;
    uint32_t ic_caps = IEEE80211_C_MFP;
    bool ic_wcl_join_requested = false;
    uint8_t ic_des_bssid[6] = {};
    int ic_des_esslen = 0;
    uint8_t ic_des_essid[IEEE80211_NWID_LEN] = {};
    unsigned ic_rsnprotos = IEEE80211_PROTO_RSN;
    unsigned ic_rsnakms = IEEE80211_AKM_PSK;
    unsigned ic_rsnciphers = IEEE80211_CIPHER_CCMP;
};
#define IEEE80211_CHAN_ANYC reinterpret_cast<ieee80211_channel *>(-1)
#define isset(a, i) ((a)[(i) / 8] & (1U << ((i) % 8)))
#define isclr(a, i) (!isset((a), (i)))
static int ieee80211_debug = 0;
static unsigned ieee80211_chan2ieee(ieee80211com *ic, ieee80211_channel *c) { return unsigned(c - ic->ic_channels); }
static uint8_t ieee80211_fix_rate(ieee80211com *, ieee80211_node *, int) { return 12; }
static const char *ether_sprintf(uint8_t *) { return "<MAC>"; }
static void ieee80211_print_essid(uint8_t *, int) {}

// PRODUCTION

int main() {
    // 1. ieee80211_ssid_is_hidden rejects NULL, len==0, and all-NUL hidden SSID IEs.
    const uint8_t emptyIE[2] = {0, 0};
    const uint8_t nulIE[10] = {0, 8, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t realIE[13] = {0, 11, 'H','i','d','d','e','n','L','a','b','5','G'};
    assert(ieee80211_ssid_is_hidden(nullptr) == 1);
    assert(ieee80211_ssid_is_hidden(emptyIE) == 1);
    assert(ieee80211_ssid_is_hidden(nulIE) == 1);
    assert(ieee80211_ssid_is_hidden(realIE) == 0);

    // 2. ieee80211_match_bss adopts ic_des_essid on matching target BSSID with ni_esslen == 0
    //    when ic_wcl_join_requested is true, while rejecting wrong BSSIDs and mismatched SSIDs.
    ieee80211com ic = {};
    ic.ic_des_chan = IEEE80211_CHAN_ANYC;
    ic.ic_chan_active[48 / 8] |= 1U << (48 % 8);
    const uint8_t targetBSSID[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    memcpy(ic.ic_des_bssid, targetBSSID, 6);
    memcpy(ic.ic_des_essid, "HiddenLab5G", 11);
    ic.ic_des_esslen = 11;

    ieee80211_node bssNode = {};
    ic.ic_bss = &bssNode;
    ieee80211_node ni = {};
    ni.ni_chan = &ic.ic_channels[48];
    memcpy(ni.ni_bssid, targetBSSID, 6);
    memcpy(ni.ni_macaddr, targetBSSID, 6);

    // Without ic_wcl_join_requested, hidden beacon node with ni_esslen == 0 still fails ESSID.
    assert((ieee80211_match_bss(&ic, &ni, 0) & IEEE80211_NODE_ASSOCFAIL_ESSID) != 0);

    // With ic_wcl_join_requested + matching DESBSSID, adopts ic_des_essid and succeeds (0)!
    ic.ic_wcl_join_requested = true;
    assert(ieee80211_match_bss(&ic, &ni, 0) == 0);
    assert(ni.ni_esslen == 11 && memcmp(ni.ni_essid, "HiddenLab5G", 11) == 0);

    // Wrong BSSID with ni_esslen == 0 must NOT adopt ic_des_essid and must fail!
    ieee80211_node other = ni;
    other.ni_bssid[5] ^= 0xff;
    other.ni_esslen = 0;
    memset(other.ni_essid, 0, sizeof(other.ni_essid));
    const int fail = ieee80211_match_bss(&ic, &other, 0);
    assert((fail & IEEE80211_NODE_ASSOCFAIL_BSSID) != 0);
    assert((fail & IEEE80211_NODE_ASSOCFAIL_ESSID) != 0);
    assert(other.ni_esslen == 0);
    return 0;
}
"""
        self.compile_run(harness.replace("// PRODUCTION", hidden_src + "\n" + match_src))


if __name__ == "__main__":
    unittest.main()
