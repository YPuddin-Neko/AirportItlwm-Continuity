#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <initializer_list>
#include <sys/types.h>
enum { IEEE80211_F_RSNON = 1, IEEE80211_NODE_MFP = 2,
       IEEE80211_NODE_RXMGMTPROT = 16,
       IEEE80211_RXI_HWDEC_SAME_PN = 4, IEEE80211_RXI_HWDEC = 8,
       IEEE80211_FC0_TYPE_MASK = 0x0c, IEEE80211_FC0_TYPE_MGT = 0,
       IEEE80211_FC0_TYPE_DATA = 8, IEEE80211_FC1_PROTECTED = 0x40,
       IEEE80211_CCMP_HDRLEN = 8, IEEE80211_TKIP_HDRLEN = 8,
       IEEE80211_WEP_EXTIV = 0x20, IEEE80211_QOS_TID = 15,
       IEEE80211_MMIE_LEN = 18, IEEE80211_ELEMID_MMIE = 76 };
enum { IEEE80211_FC0_SUBTYPE_DISASSOC = 0xa0, IEEE80211_FC0_SUBTYPE_DEAUTH = 0xc0,
       IEEE80211_FC0_SUBTYPE_ACTION = 0xd0 };
enum { IEEE80211_CIPHER_USEGROUP, IEEE80211_CIPHER_CCMP, IEEE80211_CIPHER_TKIP,
       IEEE80211_CIPHER_BIP };
struct ieee80211_frame {
    uint8_t i_fc[2], duration[2], i_addr1[6], i_addr2[6], i_addr3[6], sequence[2];
};
static_assert(sizeof(ieee80211_frame) == 24, "frame");
struct mbuf { uint8_t *storage, *data; size_t len, total; };
using mbuf_t = mbuf *;
static int liveBuffers;
#define mtod(m,t) reinterpret_cast<t>((m)->data)
#define IEEE80211_IS_MULTICAST(addr) ((addr)[0] & 1)
#define LE_READ_2(p) (uint16_t((p)[0]) | uint16_t((p)[1]) << 8)
static size_t mbuf_len(mbuf_t m) { return m->len; }
static size_t mbuf_pkthdr_len(mbuf_t m) { return m->total; }
static void mbuf_freem(mbuf_t m) { if(m) { delete[] m->storage; delete m; --liveBuffers; } }
static void mbuf_adj(mbuf_t m, size_t n) { m->data += n; m->len -= n; m->total -= n; }
static bool ieee80211_has_qos(const ieee80211_frame *wh) { return (wh->i_fc[0] & 0x8c) == 0x88; }
static unsigned ieee80211_get_qos(const ieee80211_frame *wh) { return reinterpret_cast<const uint8_t *>(wh)[24]; }
static int ieee80211_get_hdrlen(const ieee80211_frame *wh) { return 24 + (ieee80211_has_qos(wh) ? 2 : 0); }
struct ieee80211_key { int k_cipher; uint64_t k_rsc[16], k_mgmt_rsc; };
struct ieee80211_node { unsigned ni_flags; int ni_rsncipher; ieee80211_key ni_pairwise_key; };
struct ieee80211com {
    unsigned ic_flags; unsigned ic_igtk_kid, ic_def_txkey;
    ieee80211_key ic_nw_keys[6];
    struct { unsigned is_ccmp_replays, is_tkip_replays; } ic_stats;
};
struct ieee80211_rxinfo { unsigned rxi_flags; };
struct iwx_softc { ieee80211com sc_ic; };
struct ItlIwx { int iwx_ccmp_decap(iwx_softc *, mbuf_t, ieee80211_node *, ieee80211_rxinfo *); };
static int ieee80211_tkip_get_tsc(uint64_t *, uint64_t **, mbuf_t, ieee80211_key *) { return EINVAL; }
static int softwareDecrypts;
static mbuf_t ieee80211_decrypt(ieee80211com *, mbuf_t m, ieee80211_node *) {
    ++softwareDecrypts; return m;
}
// PRODUCTION: keys
// PRODUCTION: pn
// PRODUCTION: rx
// PRODUCTION: iwx
static mbuf_t frame(unsigned fc, uint64_t pn, bool multicast = false, int tid = 0) {
    auto m = new mbuf;
    m->storage = m->data = new uint8_t[64]{};
    m->total = m->len = 64; ++liveBuffers;
    auto wh = mtod(m, ieee80211_frame *);
    wh->i_fc[0] = fc; wh->i_fc[1] = IEEE80211_FC1_PROTECTED;
    wh->i_addr1[0] = multicast ? 1 : 2;
    if (ieee80211_has_qos(wh)) m->data[24] = tid;
    auto iv = m->data + ieee80211_get_hdrlen(wh);
    iv[0] = pn; iv[1] = pn >> 8; iv[3] = IEEE80211_WEP_EXTIV;
    for (int i = 0; i < 4; ++i) iv[4+i] = pn >> (16+8*i);
    return m;
}
int main() {
    iwx_softc sc = {}; auto &ic = sc.sc_ic;
    ic.ic_flags = IEEE80211_F_RSNON; ic.ic_igtk_kid = 4; ic.ic_def_txkey = 1;
    ieee80211_node node = {}; node.ni_flags = IEEE80211_NODE_MFP;
    node.ni_rsncipher = node.ni_pairwise_key.k_cipher = IEEE80211_CIPHER_CCMP;
    auto &key = node.ni_pairwise_key;
    ieee80211_rxinfo rxi = {}; ItlIwx hal;

    // Group data stays on GTK even when PMF is negotiated; only group management uses IGTK.
    for (auto fc : {8U, 0xc0U}) {
        auto m = frame(fc, 1, true);
        assert(ieee80211_get_txkey(&ic, mtod(m, ieee80211_frame *), &node) ==
            &ic.ic_nw_keys[fc == 8 ? 1 : 4]);
        mbuf_freem(m);
    }
    auto m = frame(0xc0, 1);
    assert(ieee80211_get_txkey(&ic, mtod(m, ieee80211_frame *), &node) == &key);
    // A low management PN after a high data PN is valid, in HAL and net80211.
    key.k_rsc[0] = 500; key.k_mgmt_rsc = 0;
    assert(!hal.iwx_ccmp_decap(&sc, m, &node, &rxi));
    m = ieee80211_input_hwdecrypt(&ic, &node, m, &rxi);
    assert(m && key.k_mgmt_rsc == 1 && key.k_rsc[0] == 500);
    assert(!(mtod(m, ieee80211_frame *)->i_fc[1] & IEEE80211_FC1_PROTECTED));
    assert(mbuf_len(m) == 56); mbuf_freem(m);
    m = frame(0xc0, 1);
    assert(hal.iwx_ccmp_decap(&sc, m, &node, &rxi) == 1);
    assert(!ieee80211_input_hwdecrypt(&ic, &node, m, &rxi));
    assert(liveBuffers == 0 && key.k_mgmt_rsc == 1);
    // QoS traffic advances its TID only, not TID 0 or the management counter.
    m = frame(0x88, 0x010203040506ULL, false, 7);
    assert(!hal.iwx_ccmp_decap(&sc, m, &node, &rxi));
    m = ieee80211_input_hwdecrypt(&ic, &node, m, &rxi);
    assert(m && key.k_rsc[7] == 0x010203040506ULL && key.k_rsc[0] == 500 && key.k_mgmt_rsc == 1);
    mbuf_freem(m);
    // Reordered A-MSDU subframes can share a PN; ordinary retransmitted PNs cannot.
    rxi.rxi_flags = IEEE80211_RXI_HWDEC_SAME_PN;
    m = ieee80211_input_hwdecrypt(&ic, &node, frame(8, 500), &rxi);
    assert(m); mbuf_freem(m);
    assert(!ieee80211_input_hwdecrypt(&ic, &node, frame(8, 499), &rxi));
    rxi.rxi_flags = 0;
    assert(!ieee80211_input_hwdecrypt(&ic, &node, frame(8, 500), &rxi));
    assert(liveBuffers == 0);
    // A chained packet's total length cannot stand in for a contiguous CCMP header.
    for (size_t length = 24; length != 32; ++length) {
        m = frame(8, 501); m->len = length;
        assert(hal.iwx_ccmp_decap(&sc, m, &node, &rxi) == 1);
        assert(!ieee80211_input_hwdecrypt(&ic, &node, m, &rxi));
        assert(liveBuffers == 0 && key.k_rsc[0] == 500);
    }
    m = frame(8, 501); m->data[27] = 0;
    assert(!ieee80211_input_hwdecrypt(&ic, &node, m, &rxi));
    assert(liveBuffers == 0);
    node.ni_flags |= IEEE80211_NODE_RXMGMTPROT;
    rxi.rxi_flags = IEEE80211_RXI_HWDEC;
    m = ieee80211_input_mgmt_decrypt(&ic, &node, frame(0xc0, 2), &rxi, 0xc0);
    assert(m && key.k_mgmt_rsc == 2 && softwareDecrypts == 0); mbuf_freem(m);
    assert(!ieee80211_input_mgmt_decrypt(&ic, &node, frame(0xc0, 2), &rxi, 0xc0));
    assert(liveBuffers == 0);
    m = frame(0xc0, 3); mtod(m, ieee80211_frame *)->i_fc[1] = 0;
    assert(!ieee80211_input_mgmt_decrypt(&ic, &node, m, &rxi, 0xc0));
    m = frame(0xc0, 3, true); mtod(m, ieee80211_frame *)->i_fc[1] = 0;
    m = ieee80211_input_mgmt_decrypt(&ic, &node, m, &rxi, 0xc0);
    assert(m && softwareDecrypts == 1); mbuf_freem(m); // BIP stays in software.
    node.ni_flags &= ~IEEE80211_NODE_RXMGMTPROT;
    assert(!ieee80211_input_mgmt_decrypt(&ic, &node, frame(0xc0, 4), &rxi, 0xc0));
    assert(liveBuffers == 0);
}
