#include <cassert>
#include <cstring>
#include <cstdint>
#include <sys/types.h>
#include <algorithm>
#include <initializer_list>
#define _KASSERT(x) assert(x)
#define min(a,b) std::min<int>(a,b)
#define mtod(m,t) reinterpret_cast<t>((m)->data)
#define IEEE80211_ADDR_COPY(a,b) memcpy(a,b,6)
#define IEEE80211_IS_CHAN_5GHZ(c) ((c)->five)
#define IEEE80211_IS_CHAN_2GHZ(c) (!(c)->five)
#define LE_WRITE_2(p,v) do { (p)[0] = (v); (p)[1] = (v) >> 8; } while(0)
#define LE_WRITE_4(p,v) do { for(int i=0;i<4;++i) (p)[i] = (v) >> (8*i); } while(0)
enum { IEEE80211_F_HTON = 1, IEEE80211_F_VHTON = 2, IEEE80211_F_RSNON = 4,
       IEEE80211_F_WEPON = 8, IEEE80211_F_SHPREAMBLE = 16, IEEE80211_F_HEON = 32 };
enum { IEEE80211_C_SHSLOT = 1, IEEE80211_NODE_QOS = 1, IEEE80211_PROTO_RSN = 1, IEEE80211_PROTO_WPA = 2 };
enum { IEEE80211_RATE_SIZE = 8, IEEE80211_ELEMID_SSID = 0, IEEE80211_ELEMID_RATES = 1,
       IEEE80211_ELEMID_XRATES = 50, IEEE80211_ELEMID_VHT_CAP = 191,
       IEEE80211_FC0_SUBTYPE_REASSOC_REQ = 0x20, IEEE80211_ADDR_LEN = 6,
       IEEE80211_RSNIE_MAXLEN = 255, IEEE80211_WPAIE_MAXLEN = 255,
       IEEE80211_CAPINFO_ESS = 1, IEEE80211_CAPINFO_PRIVACY = 16,
       IEEE80211_CAPINFO_SHORT_PREAMBLE = 32, IEEE80211_CAPINFO_SHORT_SLOTTIME = 1024,
       IEEE80211_HE_PPE_THRES_MAX_LEN = 25, MBUF_DONTWAIT = 0, MT_DATA = 0 };
struct ieee80211_ie_htcap { uint8_t bytes[28]; };
struct ieee80211_wme_info { uint8_t bytes[9]; };
struct ieee80211_ie_vhtcap { uint8_t bytes[14]; };
struct ieee80211_he_cap_elem { uint8_t bytes[17]; };
struct ieee80211_he_mcs_nss_supp { uint8_t bytes[12]; };
struct ieee80211_rateset { uint8_t rs_nrates, rs_rates[16]; };
struct ieee80211_channel { bool five; };
struct ieee80211_node {
    ieee80211_rateset ni_rates; ieee80211_channel *ni_chan;
    uint8_t ni_esslen, ni_essid[32], ni_bssid[6];
    unsigned ni_flags, ni_rsnprotos; bool ni_he_requested;
};
struct ieee80211com {
    ieee80211_rateset ic_sup_rates[1]; ieee80211_node *ic_bss;
    unsigned ic_flags, ic_caps, ic_lintval;
    uint8_t ic_des_esslen, ic_des_essid[32];
    uint32_t ic_vhtcaps;
    uint16_t ic_vht_rx_mcs_map, ic_vht_rx_highest, ic_vht_tx_mcs_map, ic_vht_tx_highest;
};
struct mbuf { uint8_t *data; size_t len, capacity; };
using mbuf_t = mbuf *;
static mbuf_t ieee80211_getmgmt(int, int, size_t n) { return new mbuf{new uint8_t[n]{}, 0, n}; }
static void mbuf_setlen(mbuf_t m, size_t n) { assert(n <= m->capacity); m->len = n; }
static void mbuf_pkthdr_setlen(mbuf_t m, size_t n) { mbuf_setlen(m,n); }
static int ieee80211_chan2mode(ieee80211com *, ieee80211_channel *) { return 0; }
static bool ieee80211_he_supported(ieee80211com *, ieee80211_node *) { return false; }
static uint8_t *ie(uint8_t *p, uint8_t id, size_t size) {
    p[0] = id; p[1] = size - 2; memset(p+2, 0, size-2); return p+size;
}
static uint8_t *ieee80211_add_htcaps(uint8_t *p, ieee80211com *) { return ie(p,45,28); }
static uint8_t *ieee80211_add_wme_info(uint8_t *p, ieee80211com *) { return ie(p,221,9); }
static uint8_t *ieee80211_add_qos_capability(uint8_t *p, ieee80211com *) { return ie(p,46,3); }
static uint8_t *ieee80211_add_rsn(uint8_t *p, ieee80211com *, ieee80211_node *) { assert(false); return p; }
static uint8_t *ieee80211_add_wpa(uint8_t *p, ieee80211com *, ieee80211_node *) { assert(false); return p; }
static uint8_t *ieee80211_add_hecaps(uint8_t *p, ieee80211com *) { assert(false); return p; }
// PRODUCTION: frames
static void check(mbuf_t m, unsigned offset, bool five) {
    unsigned vht = 0, ht = 0;
    while (offset < m->len) {
        assert(offset + 2 <= m->len);
        unsigned size = m->data[offset+1] + 2;
        assert(offset + size <= m->len);
        vht += m->data[offset] == 191;
        ht += m->data[offset] == 45;
        offset += size;
    }
    assert(vht == unsigned(five) && ht == 1);
    delete[] m->data; delete m;
}
int main() {
    ieee80211com ic = {}; ieee80211_node ni = {};
    ic.ic_flags = IEEE80211_F_HTON | IEEE80211_F_VHTON;
    ic.ic_bss = &ni; ni.ni_rates.rs_nrates = 12;
    for (int i = 0; i < 12; ++i) ni.ni_rates.rs_rates[i] = i + 1;
    ic.ic_sup_rates[0] = ni.ni_rates;
    for (bool five : {false, true}) {
        ieee80211_channel ch = {five}; ni.ni_chan = &ch;
        check(ieee80211_get_probe_req(&ic, &ni), 0, five);
        check(ieee80211_get_assoc_req(&ic, &ni, 0), 4, five);
        check(ieee80211_get_assoc_req(&ic, &ni, IEEE80211_FC0_SUBTYPE_REASSOC_REQ), 10, five);
    }
}
