#include <cassert>
#include <cstdint>
#include <vector>
#define AIRPORT_WCL 1
#include "AirportItlwm/WCL/WCL.hpp"
#define XYLog(...) ((void)0)
#define IC2IFP(ic) (&(ic)->ic_if)
#define IFM_MODE(...) 0
#define IFM_AUTO 0
#define htole16(v) (v)
#define htole32(v) (v)
#define isset(p,b) ((p)[(b)/8] & (1U << ((b)%8)))
#define IEEE80211_IS_CHAN_2GHZ(c) ((c)->ic_freq < 3000)
enum { IEEE80211_CHAN_MAX = 255, IEEE80211_MODE_AUTO, LINK_STATE_DOWN, IEEE80211_S_SCAN };
enum { IEEE80211_CHAN_PASSIVE = 1, IEEE80211_CHAN_DFS = 2, IFF_DEBUG = 4,
       IWX_FLAG_SCANNING = 8, IWX_FLAG_BGSCAN = 16, IWX_UCODE_TLV_API_SCAN_EXT_CHAN_VER = 0,
       IWX_PHY_BAND_24 = 0, IWX_PHY_BAND_5 = 1 };
struct _ifnet { int if_flags = 0; };
struct ieee80211_channel { int ic_flags = 0, ic_freq = 0; };
struct ieee80211com {
    _ifnet ic_if; int ic_state = 0; void *ic_bss = nullptr;
    ieee80211_channel ic_channels[256];
    uint8_t ic_wcl_scan_channels[32] = {}, ic_wcl_join_channel = 0;
    bool ic_wcl_scan_requested = false, ic_wcl_scan_restricted = false, ic_wcl_join_requested = false;
};
struct iwx_softc {
    ieee80211com sc_ic; uint8_t sc_capa_n_scan_channels = 100, sc_ucode_api[1] = {};
    unsigned sc_flags = 0;
};
struct iwx_scan_channel_cfg_umac {
    struct { uint8_t channel_num, band, iter_count, iter_interval; } v2;
    struct { uint8_t channel_num, iter_count; uint16_t iter_interval; } v1;
    uint32_t flags = 0;
};
static unsigned ieee80211_chan2ieee(ieee80211com *ic, ieee80211_channel *c) { return c - ic->ic_channels; }
static unsigned ieee80211_mhz2ieee(unsigned f, int) { return (f - (f < 3000 ? 2407 : 5000)) / 5; }
static void ieee80211_setmode(ieee80211com *, int) {}
static void ieee80211_set_link_state(ieee80211com *, int) {}
static void ieee80211_node_cleanup(ieee80211com *, void *) {}
struct Gate { void commandWakeup(void *) {} };
struct ItlIwx {
    Gate gate; int scanError = 0; std::vector<unsigned> submitted;
    Gate *getMainCommandGate() { return &gate; }
    int iwx_scan_abort(iwx_softc *) { return 0; }
    uint8_t iwx_umac_scan_fill_channels(iwx_softc *, iwx_scan_channel_cfg_umac *, int, int);
    int iwx_scan(iwx_softc *);
    int iwx_umac_scan(iwx_softc *sc, int bg) {
        iwx_scan_channel_cfg_umac channels[256] = {};
        unsigned count = iwx_umac_scan_fill_channels(sc, channels, 1, bg);
        submitted.clear();
        for (unsigned i = 0; i < count; ++i) submitted.push_back(channels[i].v1.channel_num);
        return scanError;
    }
};
// PRODUCTION: iwx
int main() {
    iwx_softc sc;
    auto &ic = sc.sc_ic;
    ic.ic_channels[1] = {4, 2412}; ic.ic_channels[36] = {4, 5180};
    ic.ic_channels[40] = {4 | IEEE80211_CHAN_DFS, 5200};
    ic.ic_wcl_join_requested = true; ic.ic_wcl_join_channel = 36;
    ItlIwx hal;
    assert(!hal.iwx_scan(&sc));
    assert(hal.submitted == std::vector<unsigned>{36} && ic.ic_wcl_join_channel == 0);
    assert(!hal.iwx_scan(&sc));
    assert((hal.submitted == std::vector<unsigned>{1, 36, 40}));
    ic.ic_wcl_join_channel = 36; hal.scanError = 1;
    assert(hal.iwx_scan(&sc) && ic.ic_wcl_join_channel == 36);
    hal.scanError = 0;
    assert(!hal.iwx_scan(&sc) && ic.ic_wcl_join_channel == 0);
    ic.ic_wcl_join_channel = 36;
    hal.iwx_umac_scan(&sc, 1); // Background scans must not inherit the join hint.
    assert((hal.submitted == std::vector<unsigned>{1, 36, 40}));
    ic.ic_wcl_scan_requested = ic.ic_wcl_scan_restricted = true;
    ic.ic_wcl_scan_channels[40 / 8] = 1 << (40 % 8);
    iwx_scan_channel_cfg_umac channels[256] = {};
    assert(hal.iwx_umac_scan_fill_channels(&sc, channels, 1, 0) == 1);
    assert(channels[0].v1.channel_num == 40 && channels[0].flags == 0); // DFS stays passive.
    sc.sc_ucode_api[0] = 1;
    assert(hal.iwx_umac_scan_fill_channels(&sc, channels, 1, 0) == 1);
    assert(channels[0].v2.channel_num == 40 && channels[0].v2.band == IWX_PHY_BAND_5);
    ic.ic_wcl_scan_restricted = ic.ic_wcl_join_requested = false;
    for (bool v2 : {false, true}) {
        sc.sc_ucode_api[0] = v2;
        for (unsigned count : {1U, 3U, 10U}) {
            iwx_scan_channel_cfg_umac list[256] = {};
            assert(hal.iwx_umac_scan_fill_channels(&sc, list, count, 1) == 3);
            assert(list[0].flags == (1U << count) - 1);
            assert(list[1].flags == (1U << count) - 1);
            assert(list[2].flags == 0);
        }
    }
}
