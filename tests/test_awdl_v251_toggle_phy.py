"""Exercise production Wi-Fi PHY/HAL/net80211 toggle and wake paths under ASan+UBSan."""

import unittest

import test_regressions as support
from test_offline import definition


class WiFiTogglePHYTests(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_iwx_init_wcl_stays_in_init_without_autonomous_scan(self):
        iwx_init = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_init")
        source = r"""
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>

#define AIRPORT_WCL 1
#define XYLog(...) ((void)0)
#define DEVNAME(sc) "iwx0"
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

enum {
    IEEE80211_S_INIT = 0,
    IEEE80211_S_SCAN = 1,
    IEEE80211_S_RUN = 4,
    IEEE80211_M_STA = 0,
    IEEE80211_M_MONITOR = 4,
    IEEE80211_EVT_DRIVER_RESET_BEGIN = 10,
    IEEE80211_EVT_DRIVER_READY = 11,
    IFF_UP = 1,
    IFF_RUNNING = 2,
    IWX_FLAG_RFKILL = 1,
    IWX_FLAG_HW_ERR = 2,
    IWX_INVALID_QUEUE = 0xff,
    THREAD_AWAKENED = 0,
    THREAD_UNINT = 0,
    kSecondScale = 1000000000,
    kIOReturnSuccess = 0,
};

typedef int IOReturn;
typedef uint64_t AbsoluteTime;
struct OSObject {};
struct WorkLoop { bool inGate() { return true; } };
struct CommandGate {
    template <typename Fn>
    IOReturn runAction(Fn, void * = nullptr, void * = nullptr, void * = nullptr, void * = nullptr) { return 0; }
    int commandSleep(void *, AbsoluteTime, int) { return THREAD_AWAKENED; }
};

struct ifqueue {};
struct _ifnet { void *if_softc = nullptr; unsigned if_flags = IFF_UP; ifqueue if_snd; };
struct ieee80211_node { void *ni_chan = nullptr; };
struct ieee80211com {
    _ifnet ic_if;
    int ic_opmode = IEEE80211_M_STA;
    enum ieee80211_state_e { S0 = 0 } unused;
    int ic_state = IEEE80211_S_SCAN;
    void *ic_ibss_chan = nullptr;
    ieee80211_node bss;
    ieee80211_node *ic_bss = &bss;
    void (*ic_event_handler)(ieee80211com *, int, void *) = nullptr;
};
struct iwx_tid_data { uint8_t qid = 0; };
struct iwx_nvm { bool sku_cap_11n_enable = true, sku_cap_11ac_enable = true, sku_cap_11ax_enable = false; };
struct iwx_softc {
    ieee80211com sc_ic;
    iwx_tid_data sc_tid_data[4];
    iwx_nvm sc_nvm;
    int sc_generation = 0;
    unsigned sc_flags = 0;
    int ns_nstate = IEEE80211_S_SCAN;
};

static int g_begin_scan_calls = 0;
static std::vector<int> g_events;
static void ifq_clr_oactive(ifqueue *) {}
static void ifq_flush(ifqueue *) {}
static void ieee80211_new_state(ieee80211com *ic, int st, int) { ic->ic_state = st; }
static void ieee80211_begin_scan(_ifnet *) { ++g_begin_scan_calls; }
static void clock_interval_to_deadline(uint32_t, uint32_t, uint64_t *d) { *d = 1; }

struct ItlIwx {
    WorkLoop wl;
    CommandGate gate;
    int initHwCalls = 0;
    WorkLoop *getMainWorkLoop() { return &wl; }
    CommandGate *getMainCommandGate() { return &gate; }
    int iwx_init_hw(iwx_softc *) { ++initHwCalls; return 0; }
    void iwx_stop_device(iwx_softc *) {}
    void iwx_stop(_ifnet *) {}
    void iwx_setup_ht_rates(iwx_softc *) {}
    void iwx_setup_vht_rates(iwx_softc *) {}
    void iwx_setup_he_rates(iwx_softc *) {}
    int iwx_init(_ifnet *ifp);
};

// PRODUCTION

int main() {
    iwx_softc sc;
    sc.sc_ic.ic_if.if_softc = &sc;
    sc.sc_ic.ic_event_handler = [](ieee80211com *, int ev, void *) { g_events.push_back(ev); };
    ItlIwx hal;
    assert(hal.iwx_init(&sc.sc_ic.ic_if) == 0);
    assert(hal.initHwCalls == 1);
    assert(g_begin_scan_calls == 0);
    assert((sc.sc_ic.ic_if.if_flags & IFF_RUNNING) != 0);
    assert(sc.sc_ic.ic_state == IEEE80211_S_INIT);
    assert(sc.ns_nstate == IEEE80211_S_INIT);
    assert(g_events.size() == 2);
    assert(g_events[0] == IEEE80211_EVT_DRIVER_RESET_BEGIN);
    assert(g_events[1] == IEEE80211_EVT_DRIVER_READY);
    return 0;
}
"""
        self.compile_run(source.replace("// PRODUCTION", iwx_init))

    def test_end_scan_wcl_returns_to_init_without_autonomous_next_scan(self):
        end_scan = definition("itl80211/openbsd/net80211/ieee80211_node.c", "ieee80211_end_scan")
        iwx_endscan = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_endscan")
        source = r"""
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

#define AIRPORT_WCL 1
#define IEEE80211_STA_ONLY 1
#define XYLog(...) ((void)0)
#define DPRINTF(x) ((void)0)
#define RB_MIN(name, head) ((head)->minNode)
#define IEEE80211_ADDR_COPY(dst, src) std::memcpy((dst), (src), 6)

enum {
    IEEE80211_S_INIT = 0,
    IEEE80211_S_SCAN = 1,
    IEEE80211_S_RUN = 4,
    IEEE80211_M_STA = 0,
    IEEE80211_F_ASCAN = 1,
    IEEE80211_F_BGSCAN = 2,
    IEEE80211_F_DISABLE_BG_AUTO_CONNECT = 4,
    IEEE80211_F_TX_MGMT_ONLY = 8,
    IEEE80211_C_SCANALLBAND = 1,
    IEEE80211_MODE_AUTO = 0,
    IEEE80211_MODE_11AX = 1,
    IEEE80211_MODE_11AC = 2,
    IEEE80211_MODE_11N = 3,
    IEEE80211_INACT_SCAN = 1,
    IEEE80211_BGSCAN_FAIL_MAX = 5,
    IEEE80211_NODE_HE = 1,
    IEEE80211_NODE_VHT = 2,
    IEEE80211_NODE_HT = 4,
    IEEE80211_FC0_SUBTYPE_DEAUTH = 0xc0,
    IEEE80211_REASON_AUTH_LEAVE = 3,
    IEEE80211_EVT_SCAN_DONE = 20,
    IFF_DEBUG = 1,
    IWX_FLAG_SCANNING = 1,
    IWX_FLAG_BGSCAN = 2,
};

struct _ifnet { unsigned if_flags = 0; const char *if_xname = "en1"; };
struct ieee80211_node {
    uint8_t ni_macaddr[6] = {};
    unsigned ni_flags = 0;
    void *ni_chan = nullptr;
    void *ni_unref_arg = nullptr;
    size_t ni_unref_arg_size = 0;
    void (*ni_unref_cb)(void *, void *) = nullptr;
};
struct ieee80211_tree_head { ieee80211_node *minNode = nullptr; };
struct ieee80211_node_switch_bss_arg { uint8_t cur_macaddr[6]; uint8_t sel_macaddr[6]; };
struct ieee80211com {
    _ifnet ic_if;
    unsigned ic_flags = 0;
    unsigned ic_xflags = 0;
    unsigned ic_caps = IEEE80211_C_SCANALLBAND;
    int ic_opmode = IEEE80211_M_STA;
    int ic_state = IEEE80211_S_SCAN;
    int ic_scan_count = 0;
    int ic_bgscan_fail = 0;
    bool ic_wcl_join_requested = false;
    ieee80211_tree_head ic_tree;
    ieee80211_node bss;
    ieee80211_node *ic_bss = &bss;
    void (*ic_event_handler)(ieee80211com *, int, void *) = nullptr;
};
struct iwx_softc {
    ieee80211com sc_ic;
    unsigned sc_flags = 0;
    int ns_nstate = IEEE80211_S_SCAN;
};

static int g_next_scan_calls = 0;
static int g_reset_scan_calls = 0;
static int g_join_bss_calls = 0;
static int g_scan_done_events = 0;
static ieee80211_node *g_chosen_bss = nullptr;

static void ieee80211_clean_inactive_nodes(ieee80211com *, int) {}
static void ieee80211_reset_scan(_ifnet *) { ++g_reset_scan_calls; }
static int ieee80211_next_mode(_ifnet *) { return IEEE80211_MODE_AUTO; }
static void ieee80211_next_scan(_ifnet *) { ++g_next_scan_calls; }
static void ieee80211_switch_ess(ieee80211com *) {}
static ieee80211_node *ieee80211_node_choose_bss(ieee80211com *, int, ieee80211_node **curbs) {
    *curbs = nullptr;
    return g_chosen_bss;
}
static void ieee80211_setmode(ieee80211com *, int) {}
static int ieee80211_chan2mode(ieee80211com *, void *) { return IEEE80211_MODE_AUTO; }
static void ieee80211_stop_ampdu_tx(ieee80211com *, ieee80211_node *, int) {}
static int IEEE80211_SEND_MGMT(ieee80211com *, ieee80211_node *, int, int) { return 0; }
static void ieee80211_node_switch_bss(void *, void *) {}
static void ieee80211_node_join_bss(ieee80211com *, ieee80211_node *) { ++g_join_bss_calls; }
static void *malloc_shim(size_t sz, int, int) { return std::malloc(sz); }
#define malloc(sz, t, f) malloc_shim((sz), (t), (f))

// PRODUCTION_NODE

struct ItlIwx {
    void iwx_endscan(iwx_softc *sc);
};

// PRODUCTION_IWX

int main() {
    ItlIwx hal;
    ieee80211_node apNode;

    // 1. Unassociated WCL subset scan with candidates present, no join requested:
    // must fire SCAN_DONE, clear F_ASCAN, preserve ic_tree, transition synchronously to S_INIT,
    // and never call ieee80211_next_scan.
    iwx_softc sc;
    sc.sc_flags = IWX_FLAG_SCANNING;
    sc.ns_nstate = IEEE80211_S_SCAN;
    sc.sc_ic.ic_state = IEEE80211_S_SCAN;
    sc.sc_ic.ic_flags = IEEE80211_F_ASCAN;
    sc.sc_ic.ic_tree.minNode = &apNode;
    sc.sc_ic.ic_wcl_join_requested = false;
    sc.sc_ic.ic_event_handler = [](ieee80211com *, int ev, void *) {
        if (ev == IEEE80211_EVT_SCAN_DONE) ++g_scan_done_events;
    };
    g_chosen_bss = nullptr;
    hal.iwx_endscan(&sc);
    assert(g_scan_done_events == 1);
    assert(g_next_scan_calls == 0);
    assert(g_reset_scan_calls == 1);
    assert((sc.sc_ic.ic_flags & IEEE80211_F_ASCAN) == 0);
    assert(sc.sc_ic.ic_state == IEEE80211_S_INIT);
    assert(sc.ns_nstate == IEEE80211_S_INIT);
    assert(sc.sc_ic.ic_tree.minNode == &apNode);

    // 2. Unassociated WCL subset scan with empty tree: same synchronous S_INIT return.
    sc.sc_flags = IWX_FLAG_SCANNING;
    sc.ns_nstate = IEEE80211_S_SCAN;
    sc.sc_ic.ic_state = IEEE80211_S_SCAN;
    sc.sc_ic.ic_flags = IEEE80211_F_ASCAN;
    sc.sc_ic.ic_tree.minNode = nullptr;
    hal.iwx_endscan(&sc);
    assert(g_scan_done_events == 2);
    assert(g_next_scan_calls == 0);
    assert(sc.sc_ic.ic_state == IEEE80211_S_INIT);
    assert(sc.ns_nstate == IEEE80211_S_INIT);

    // 3. Explicit WCL join scan with AP not yet found: retries via ieee80211_next_scan.
    sc.sc_flags = IWX_FLAG_SCANNING;
    sc.ns_nstate = IEEE80211_S_SCAN;
    sc.sc_ic.ic_state = IEEE80211_S_SCAN;
    sc.sc_ic.ic_wcl_join_requested = true;
    hal.iwx_endscan(&sc);
    assert(g_next_scan_calls == 1);
    assert(sc.sc_ic.ic_state == IEEE80211_S_SCAN);

    // 4. Explicit WCL join scan with AP found: joins BSS immediately.
    sc.sc_flags = IWX_FLAG_SCANNING;
    sc.sc_ic.ic_tree.minNode = &apNode;
    g_chosen_bss = &apNode;
    hal.iwx_endscan(&sc);
    assert(g_join_bss_calls == 1);

    // 5. Associated S_RUN background scan: never leaves S_RUN and never calls next_scan.
    sc.sc_flags = IWX_FLAG_BGSCAN;
    sc.ns_nstate = IEEE80211_S_RUN;
    sc.sc_ic.ic_state = IEEE80211_S_RUN;
    sc.sc_ic.ic_flags = IEEE80211_F_BGSCAN | IEEE80211_F_DISABLE_BG_AUTO_CONNECT;
    sc.sc_ic.ic_wcl_join_requested = false;
    sc.sc_ic.ic_tree.minNode = &apNode;
    g_chosen_bss = nullptr;
    hal.iwx_endscan(&sc);
    assert(g_next_scan_calls == 1);
    assert(sc.sc_ic.ic_state == IEEE80211_S_RUN);
    assert((sc.sc_ic.ic_flags & IEEE80211_F_BGSCAN) == 0);
    return 0;
}
"""
        self.compile_run(source.replace("// PRODUCTION_NODE", end_scan).replace("// PRODUCTION_IWX", iwx_endscan))

    def test_iwx_scan_aborts_active_scan_and_mcc_deduplicates_unchanged_updates(self):
        iwx_scan = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_scan")
        iwx_mcc_update = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_mcc_update")
        iwx_send_mcc = definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_send_update_mcc_cmd")
        source = r"""
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define AIRPORT_WCL 1
#define XYLog(...) ((void)0)
#define DEVNAME(sc) "iwx0"
#define IC2IFP(ic) (&(ic)->ic_if)
#define IFM_MODE(m) (m)
#define IFM_AUTO 0
#define htole16(v) static_cast<uint16_t>(v)
#define le16toh(v) static_cast<uint16_t>(v)
#define htole32(v) static_cast<uint32_t>(v)
#define isset(p, b) ((p)[(b) / 8] & (1U << ((b) % 8)))
#define setbit(p, b) ((p)[(b) / 8] |= static_cast<uint8_t>(1U << ((b) % 8)))
#define nitems(a) (sizeof(a) / sizeof((a)[0]))

enum {
    IEEE80211_S_INIT = 0,
    IEEE80211_S_SCAN = 1,
    IEEE80211_S_RUN = 4,
    IEEE80211_CHAN_MAX = 255,
    IEEE80211_MODE_AUTO = 0,
    IEEE80211_CHAN_A = 1,
    IEEE80211_CHAN_B = 2,
    IEEE80211_CHAN_PUREG = 4,
    IEEE80211_CHAN_HT = 8,
    IEEE80211_CHAN_VHT = 16,
    IEEE80211_CHAN_DFS = 32,
    IEEE80211_EVT_COUNTRY_CODE_UPDATE = 30,
    LINK_STATE_DOWN = 0,
    IFF_DEBUG = 1,
    IWX_FLAG_SHUTDOWN = 1,
    IWX_FLAG_SCANNING = 2,
    IWX_FLAG_BGSCAN = 4,
    IWX_MCC_SOURCE_WIFI = 1,
    IWX_MCC_SOURCE_GET_CURRENT = 2,
    IWX_MCC_SOURCE_OLD_FW = 3,
    IWX_MCC_UPDATE_CMD = 0xc8,
    IWX_CMD_WANT_RESP = 1,
    IWX_CMD_RESP_MAX = 4096,
    IWX_LONG_GROUP = 1,
    IWX_FW_CMD_VER_UNKNOWN = 0xaa,
    IWX_CMD_FAILED_MSK = 0x40,
    IWX_UCODE_TLV_API_WIFI_MCC_UPDATE = 1,
    IWX_UCODE_TLV_CAPA_LAR_MULTI_MCC = 1,
    IWX_UCODE_TLV_CAPA_MCC_UPDATE_11AX_SUPPORT = 2,
    IWX_NUM_CHANNELS = 4,
    IWX_NVM_CHANNEL_VALID = 1,
    IWX_NVM_CHANNEL_GO_CONCURRENT = 2,
    IWX_NVM_CHANNEL_RADAR = 4,
    kIOReturnSuccess = 0,
};

typedef int IOReturn;
static const char *ieee80211_state_name[] = {"INIT", "SCAN", "AUTH", "ASSOC", "RUN"};
static const uint8_t iwx_nvm_channels_8000[4] = {1, 6, 36, 149};
static const uint8_t iwx_nvm_channels_uhb[4] = {1, 6, 36, 149};

struct OSObject {};
struct WorkLoop { bool inGate() { return true; } };
struct CommandGate {
    template <typename Fn>
    IOReturn runAction(Fn, void * = nullptr, void * = nullptr, void * = nullptr, void * = nullptr) { return 0; }
    void commandWakeup(void *) {}
};

struct _ifnet { unsigned if_flags = 0; const char *if_xname = "en1"; };
struct ifm_entry { int ifm_media = IFM_AUTO; };
struct ifmedia { ifm_entry cur; ifm_entry *ifm_cur = &cur; };
struct ieee80211_channel { unsigned ic_flags = 0; };
struct ieee80211com {
    _ifnet ic_if;
    ifmedia ic_media;
    int ic_state = IEEE80211_S_INIT;
    unsigned ic_curmode = 0;
    void *ic_bss = nullptr;
    uint8_t ic_wcl_join_channel = 0;
    ieee80211_channel ic_channels[256];
    uint8_t ic_chan_avail[32] = {};
    uint8_t ic_chan_active[32] = {};
    uint8_t ic_beacon_hint[32] = {};
    uint16_t ic_beacon_hint_mcc = 0;
    void (*ic_event_handler)(ieee80211com *, int, void *) = nullptr;
};
struct iwx_nvm { bool lar_enabled = true; bool vht160_supported = false; };
struct iwx_mcc_chub_notif { uint16_t mcc = 0; uint8_t source_id = 0; };
struct iwx_mcc_update_cmd { uint16_t mcc = 0; uint8_t source_id = 0; };
struct iwx_rx_hdr { uint8_t group_id = 0; };
struct iwx_rx_packet { iwx_rx_hdr hdr; uint8_t data[64] = {}; };
struct iwx_host_cmd {
    uint32_t id = 0;
    uint32_t flags = 0;
    uint16_t len[2] = {};
    const void *data[2] = {};
    uint32_t resp_pkt_len = 0;
    iwx_rx_packet *resp_pkt = nullptr;
};
struct iwx_task {};
struct iwx_softc {
    ieee80211com sc_ic;
    iwx_nvm sc_nvm;
    unsigned sc_flags = 0;
    int sc_generation = 1;
    bool sc_uhb_supported = false;
    uint8_t sc_ucode_api[4] = {};
    uint8_t sc_enabled_capa[4] = {};
    uint16_t sc_fw_mcc_int = 0;
    char sc_fw_mcc[4] = {};
    iwx_task mcc_task;
};
static int systq = 0;

struct RegulatoryState {
    uint64_t notifications = 0, serial = 0, epoch = 1, refreshes = 0;
    uint16_t requestedMCC = 0, mappedMCC = 0;
    uint8_t requestedSource = 0, mappedSource = 0, responseVersion = 0;
    uint32_t capabilities = 0, status = 0;
    int lastError = 0;
    bool pending = false, running = false, valid = false;
    uint32_t rawFlags[256] = {};
};

struct IwxRegulatoryProfile {
    uint16_t mcc = 0;
    uint8_t source = 0;
    uint32_t status = 0, capabilities = 0;
    unsigned count = 4;
    uint32_t channels[4] = {};
    static uint16_t nextMCC;
    static uint8_t nextSource;
    static uint32_t nextFlags[4];
    bool parse(const uint8_t *, size_t, bool, uint8_t, unsigned) {
        mcc = nextMCC;
        source = nextSource;
        for (unsigned i = 0; i < 4; ++i) channels[i] = nextFlags[i];
        return true;
    }
    uint32_t channelFlags(unsigned i, uint8_t) const { return channels[i]; }
};
uint16_t IwxRegulatoryProfile::nextMCC = 0x3030;
uint8_t IwxRegulatoryProfile::nextSource = 1;
uint32_t IwxRegulatoryProfile::nextFlags[4] = {1, 1, 1, 1};

static void ieee80211_setmode(ieee80211com *, int) {}
static void ieee80211_set_link_state(ieee80211com *, int) {}
static void ieee80211_node_cleanup(ieee80211com *, void *) {}

struct ItlIwx {
    WorkLoop wl;
    CommandGate gate;
    RegulatoryState regulatory;
    void (*awdlRegulatoryChanged)(void *) = nullptr;
    void *awdlReceiver = nullptr;
    iwx_rx_packet fakeResp;
    int abortCalls = 0, umacScanCalls = 0, mccTaskAdds = 0, awdlCallbacks = 0, ccEvents = 0;

    WorkLoop *getMainWorkLoop() { return &wl; }
    CommandGate *getMainCommandGate() { return &gate; }
    int iwx_scan_abort(iwx_softc *sc) { ++abortCalls; sc->sc_flags &= ~(IWX_FLAG_SCANNING | IWX_FLAG_BGSCAN); return 0; }
    int iwx_umac_scan(iwx_softc *, int) { ++umacScanCalls; return 0; }
    void iwx_add_task(iwx_softc *, int, iwx_task *) { ++mccTaskAdds; }
    int iwx_send_cmd(iwx_softc *, iwx_host_cmd *hcmd) { hcmd->resp_pkt = &fakeResp; return 0; }
    void iwx_free_resp(iwx_softc *, iwx_host_cmd *) {}
    uint8_t iwx_lookup_notif_ver(iwx_softc *, int, int) { return 0; }
    size_t iwx_rx_packet_payload_len(iwx_rx_packet *) { return 64; }
    void iwx_init_channel_map(iwx_softc *, const void *, const uint32_t *, unsigned) {}
    void iwx_publish_regulatory(iwx_softc *) {}

    int iwx_scan(iwx_softc *sc);
    void iwx_mcc_update(iwx_softc *sc, iwx_mcc_chub_notif *notif);
    int iwx_send_update_mcc_cmd(iwx_softc *sc, const char *alpha2, uint8_t source);
};

// PRODUCTION_SCAN
// PRODUCTION_MCC_UPDATE
// PRODUCTION_SEND_MCC

int main() {
    ItlIwx hal;
    iwx_softc sc;
    sc.sc_ic.ic_event_handler = [](ieee80211com *, int ev, void *) {
        if (ev == IEEE80211_EVT_COUNTRY_CODE_UPDATE) {
            // Counted via static pointer below.
        }
    };
    static ItlIwx *g_hal = &hal;
    sc.sc_ic.ic_event_handler = [](ieee80211com *, int ev, void *) {
        if (ev == IEEE80211_EVT_COUNTRY_CODE_UPDATE) ++g_hal->ccEvents;
    };
    hal.awdlRegulatoryChanged = [](void *ctx) { ++static_cast<ItlIwx *>(ctx)->awdlCallbacks; };
    hal.awdlReceiver = &hal;

    // 1. iwx_scan aborts an in-flight IWX_FLAG_SCANNING scan before issuing a new UMAC scan.
    sc.sc_flags = IWX_FLAG_SCANNING;
    assert(hal.iwx_scan(&sc) == 0);
    assert(hal.abortCalls == 1 && hal.umacScanCalls == 1);
    assert((sc.sc_flags & IWX_FLAG_SCANNING) != 0);

    // 2. First MCC ('00') commits and notifies once.
    IwxRegulatoryProfile::nextMCC = 0x3030;
    IwxRegulatoryProfile::nextSource = 1;
    assert(hal.iwx_send_update_mcc_cmd(&sc, "00", 1) == 0);
    assert(hal.ccEvents == 1 && hal.awdlCallbacks == 1);

    // 3. Repeated identical '00' notifications are deduplicated at both iwx_mcc_update and iwx_send_update_mcc_cmd.
    iwx_mcc_chub_notif notif {0x3030, 1};
    hal.iwx_mcc_update(&sc, &notif);
    assert(hal.mccTaskAdds == 0);
    assert(hal.iwx_send_update_mcc_cmd(&sc, "00", 1) == 0);
    assert(hal.ccEvents == 1 && hal.awdlCallbacks == 1);

    // 4. Genuine MCC transition ('00' -> 'CN') schedules mcc_task and emits exactly once.
    notif = {0x434e, 1};
    hal.iwx_mcc_update(&sc, &notif);
    assert(hal.mccTaskAdds == 1);
    hal.regulatory.pending = false;
    IwxRegulatoryProfile::nextMCC = 0x434e;
    assert(hal.iwx_send_update_mcc_cmd(&sc, "CN", 1) == 0);
    assert(hal.ccEvents == 2 && hal.awdlCallbacks == 2);

    // 5. Same MCC ('CN') with changed channel flags still publishes update.
    IwxRegulatoryProfile::nextFlags[3] = 3;
    assert(hal.iwx_send_update_mcc_cmd(&sc, "CN", 1) == 0);
    assert(hal.ccEvents == 3 && hal.awdlCallbacks == 3);
    return 0;
}
"""
        self.compile_run(
            source.replace("// PRODUCTION_SCAN", iwx_scan)
                  .replace("// PRODUCTION_MCC_UPDATE", iwx_mcc_update)
                  .replace("// PRODUCTION_SEND_MCC", iwx_send_mcc)
        )


if __name__ == "__main__":
    unittest.main()
