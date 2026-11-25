#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <sys/types.h>

#define IEEE80211_STA_ONLY
#define NBPFILTER 0
#define _KASSERT assert
#define DPRINTF(args) do {} while (0)
#define XYLog(...) do {} while (0)
#define IEEE80211_SEND_ACTION(...) do { assert(false); } while (0)
#define IEEE80211_ADDR_EQ(a,b) (std::memcmp((a),(b),6)==0)
#define IEEE80211_IS_MULTICAST(a) ((a)[0]&1)
#define LE_READ_2(p) (uint16_t((p)[0]) | (uint16_t((p)[1])<<8))
#define letoh16(v) (v)
#define mtod(m,t) reinterpret_cast<t>((m)->bytes)

enum { IEEE80211_M_STA=1, IEEE80211_M_MONITOR=2,
    IEEE80211_S_INIT, IEEE80211_S_SCAN, IEEE80211_S_AUTH, IEEE80211_S_ASSOC, IEEE80211_S_RUN };
enum { IEEE80211_FC0_VERSION_MASK=3, IEEE80211_FC0_VERSION_0=0,
    IEEE80211_FC0_TYPE_MASK=12, IEEE80211_FC0_TYPE_MGT=0,
    IEEE80211_FC0_TYPE_CTL=4, IEEE80211_FC0_TYPE_DATA=8,
    IEEE80211_FC0_SUBTYPE_MASK=240, IEEE80211_FC0_SUBTYPE_NODATA=64,
    IEEE80211_FC0_SUBTYPE_BEACON=0x80, IEEE80211_FC0_SUBTYPE_PROBE_RESP=0x50,
    IEEE80211_FC0_SUBTYPE_DEAUTH=0xc0, IEEE80211_FC0_SUBTYPE_DISASSOC=0xa0,
    IEEE80211_FC0_SUBTYPE_ACTION=0xd0, IEEE80211_FC0_SUBTYPE_BAR=0x80,
    IEEE80211_FC1_DIR_MASK=3, IEEE80211_FC1_DIR_NODS=0, IEEE80211_FC1_DIR_FROMDS=2,
    IEEE80211_FC1_MORE_FRAG=4, IEEE80211_FC1_RETRY=8, IEEE80211_FC1_PROTECTED=0x40,
    IEEE80211_QOS_TID=15, IEEE80211_QOS_ACK_POLICY_MASK=96,
    IEEE80211_QOS_ACK_POLICY_BA=96, IEEE80211_QOS_ACK_POLICY_NORMAL=0,
    IEEE80211_QOS_AMSDU=128, IEEE80211_SEQ_FRAG_MASK=15, IEEE80211_SEQ_SEQ_SHIFT=4,
    IEEE80211_BA_AGREED=1, IEEE80211_BA_REQUESTED=2,
    IEEE80211_RXI_AMPDU_DONE=1, IEEE80211_RXI_SAME_SEQ=2, IEEE80211_RXI_HWDEC=4,
    IEEE80211_NODE_RXMGMTPROT=1, IEEE80211_NODE_RXPROT=2, IEEE80211_NODE_HT=4,
    IEEE80211_F_RSNON=1, IEEE80211_F_WEPON=2, IEEE80211_F_BGSCAN=4,
    IEEE80211_F_DESBSSID=8, IEEE80211_F_DISABLE_BG_AUTO_CONNECT=16, IEEE80211_F_STAYAUTH=32,
    IEEE80211_EVT_STA_DEAUTH=1, IFF_SIMPLEX=1 };

struct ieee80211_frame { uint8_t i_fc[2],duration[2],i_addr1[6],i_addr2[6],i_addr3[6],i_seq[2]; };
struct ieee80211_frame_min { uint8_t bytes[16]; };
struct Packet { alignas(8) uint8_t bytes[64]={}; size_t length=26; unsigned frees=0; };
using mbuf_t=Packet *;
struct mbuf_list {};
static const uint8_t etherbroadcastaddr[6]={255,255,255,255,255,255};
static size_t mbuf_len(mbuf_t m) { return m->length; }
static void mbuf_freem(mbuf_t m) { assert(m && m->frees++==0); }
static bool ieee80211_has_qos(const ieee80211_frame *w) { return (w->i_fc[0]&0x8c)==0x88; }
static bool ieee80211_has_seq(const ieee80211_frame *w) { return (w->i_fc[0]&12)!=4; }
static unsigned ieee80211_get_qos(const ieee80211_frame *w) { return reinterpret_cast<const uint8_t *>(w)[24]; }
static unsigned ieee80211_get_hdrlen(const ieee80211_frame *w) { return 24+(ieee80211_has_qos(w)?2:0); }
struct Stats { unsigned inputErrors=0; };
struct _ifnet { Stats stats; Stats *netStat=&stats; unsigned if_flags=0; };
struct ieee80211_node {
    uint8_t ni_bssid[6]={2,0xaa,0xbb,0xcc,0xdd,1};
    unsigned ni_flags=0; uint16_t ni_rxseq=40,ni_qos_rxseqs[16]={};
    int ni_rssi=55,ni_inact=7; uint32_t ni_rstamp=10;
    struct { int ba_state=0; } ni_rx_ba[16];
};
struct ieee80211_rxinfo { unsigned rxi_flags=0; int rxi_rssi=99; uint32_t rxi_tstamp=100; };
struct ieee80211com {
    _ifnet ic_if; // The production entry point casts ifp back to ieee80211com.
    int ic_opmode=IEEE80211_M_STA,ic_state=IEEE80211_S_RUN;
    unsigned ic_flags=0,ic_userflags=0;
    uint8_t ic_myaddr[6]={2,0,0,0,0,1};
    ieee80211_node node; ieee80211_node *ic_bss=&node;
    int ic_bgscan_start=0,ic_bgscan_timeout=0,ic_bgscan_fail=0;
    int (*ic_node_checkrssi)(ieee80211com *,ieee80211_node *)=nullptr;
    void (*ic_recv_mgmt)(ieee80211com *,mbuf_t,ieee80211_node *,ieee80211_rxinfo *,int)=nullptr;
    void (*ic_event_handler)(ieee80211com *,int,void *)=nullptr;
    unsigned ic_deauth_reason=0;
    struct {
        unsigned is_rx_tooshort=0,is_rx_badversion=0,is_rx_wrongbss=0,
            is_rx_dup=0,is_rx_wrongdir=0,is_rx_mcastecho=0,is_rx_unencrypted=0,
            is_rx_wepfail=0,is_rx_nowep=0,is_rx_mgtdiscard=0,is_rx_ctl=0,
            is_rx_deauth=0,is_rx_disassoc=0;
    } ic_stats;
    unsigned events=0,stateChanges=0,management=0,softwareDecrypts=0,hardwareDecrypts=0;
    bool decryptValid=true;
};
static void timeout_del(int *) { assert(false); }
static bool timeout_pending(int *) { assert(false); return false; }
static void timeout_add_msec(int *,int) { assert(false); }
static void ieee80211_input_ba(ieee80211com *,mbuf_t,ieee80211_node *,int,ieee80211_rxinfo *,mbuf_list *) { assert(false); }
static void ieee80211_amsdu_decap(ieee80211com *,mbuf_t,ieee80211_node *,int,mbuf_list *) { assert(false); }
static void ieee80211_decap(ieee80211com *,mbuf_t,ieee80211_node *,int,mbuf_list *) { assert(false); }
static void ieee80211_recv_bar(ieee80211com *,mbuf_t,ieee80211_node *) { assert(false); }
static mbuf_t ieee80211_decrypt(ieee80211com *ic,mbuf_t m,ieee80211_node *) {
    ++ic->softwareDecrypts;
    if (!ic->decryptValid) { mbuf_freem(m); return nullptr; }
    return m;
}
static mbuf_t ieee80211_input_hwdecrypt(ieee80211com *ic,ieee80211_node *,mbuf_t m,ieee80211_rxinfo *) {
    ++ic->hardwareDecrypts;
    if (!ic->decryptValid) { mbuf_freem(m); return nullptr; }
    return m;
}
static void ieee80211_new_state(ieee80211com *ic,int state,int) { ++ic->stateChanges; ic->ic_state=state; }
// PRODUCTION INPUT

static void management(ieee80211com *ic,mbuf_t m,ieee80211_node *ni,ieee80211_rxinfo *,int subtype) {
    ++ic->management;
    if (subtype==IEEE80211_FC0_SUBTYPE_DEAUTH) ieee80211_recv_deauth(ic,m,ni);
    if (subtype==IEEE80211_FC0_SUBTYPE_DISASSOC) ieee80211_recv_disassoc(ic,m,ni);
}
static void event(ieee80211com *ic,int value,void *) { assert(value==IEEE80211_EVT_STA_DEAUTH); ++ic->events; }
static Packet frame(ieee80211com &ic,int subtype,bool broadcast=false) {
    Packet m;
    auto w=mtod(&m,ieee80211_frame *);
    w->i_fc[0]=subtype;
    std::memcpy(w->i_addr1,broadcast?etherbroadcastaddr:ic.ic_myaddr,6);
    std::memcpy(w->i_addr2,ic.node.ni_bssid,6);
    std::memcpy(w->i_addr3,ic.node.ni_bssid,6);
    w->i_seq[0]=uint8_t(41<<4); w->i_seq[1]=41>>4;
    m.bytes[24]=2;
    return m;
}
static void receive(ieee80211com &ic,Packet &m,unsigned flags=0) {
    ic.ic_recv_mgmt=management; ic.ic_event_handler=event;
    ieee80211_rxinfo rxi; rxi.rxi_flags=flags;
    ieee80211_inputm(&ic.ic_if,&m,&ic.node,&rxi,nullptr);
    assert(m.frees==1);
}
static void unchanged(const ieee80211com &ic) {
    assert(ic.ic_state==IEEE80211_S_RUN && ic.events==0 && ic.stateChanges==0);
    assert(ic.ic_deauth_reason==0 && ic.management==0);
    assert(ic.node.ni_rxseq==40 && ic.node.ni_rssi==55 && ic.node.ni_rstamp==10 && ic.node.ni_inact==7);
    assert(ic.softwareDecrypts==0 && ic.hardwareDecrypts==0);
}
static void foreignDisconnects() {
    for (int subtype:{IEEE80211_FC0_SUBTYPE_DEAUTH,IEEE80211_FC0_SUBTYPE_DISASSOC})
    for (bool broadcast:{false,true}) for (int wrong:{1,2,3,4}) {
        if (broadcast && wrong==1) continue;
        ieee80211com ic; auto m=frame(ic,subtype,broadcast); auto w=mtod(&m,ieee80211_frame *);
        if (wrong==1) w->i_addr1[5]^=1;
        if (wrong==2) w->i_addr2[5]^=1;
        if (wrong==3) w->i_addr3[5]^=1;
        if (wrong==4) ic.ic_bss=nullptr;
        receive(ic,m); unchanged(ic);
        assert(ic.ic_stats.is_rx_wrongbss==1);
    }
    // An unrelated multicast receiver is not a broadcast disconnect.
    ieee80211com ic; auto m=frame(ic,IEEE80211_FC0_SUBTYPE_DEAUTH);
    m.bytes[4]=1; receive(ic,m); unchanged(ic);
}
static void currentAPDisconnects() {
    for (int subtype:{IEEE80211_FC0_SUBTYPE_DEAUTH,IEEE80211_FC0_SUBTYPE_DISASSOC})
    for (bool broadcast:{false,true}) {
        ieee80211com ic; auto m=frame(ic,subtype,broadcast); receive(ic,m);
        assert(ic.management==1 && ic.stateChanges==1 && ic.ic_stats.is_rx_wrongbss==0);
        assert(ic.node.ni_rxseq==41 && ic.node.ni_rssi==99 && ic.node.ni_inact==0);
        assert(ic.ic_state==(subtype==IEEE80211_FC0_SUBTYPE_DEAUTH?IEEE80211_S_AUTH:IEEE80211_S_ASSOC));
        assert(ic.events==(subtype==IEEE80211_FC0_SUBTYPE_DEAUTH?1U:0U));
        assert(ic.ic_deauth_reason==(subtype==IEEE80211_FC0_SUBTYPE_DEAUTH?2U:0U));
    }
}
static void scanAndMonitor() {
    for (int state:{IEEE80211_S_SCAN,IEEE80211_S_RUN})
    for (int subtype:{IEEE80211_FC0_SUBTYPE_BEACON,IEEE80211_FC0_SUBTYPE_PROBE_RESP}) {
        ieee80211com ic; ic.ic_state=state; auto m=frame(ic,subtype,true);
        m.bytes[15]^=1; m.bytes[21]^=1; receive(ic,m);
        assert(ic.management==1 && ic.events==0 && ic.stateChanges==0 && ic.ic_stats.is_rx_wrongbss==0);
    }
    ieee80211com ic; ic.ic_opmode=IEEE80211_M_MONITOR;
    auto m=frame(ic,IEEE80211_FC0_SUBTYPE_DEAUTH); receive(ic,m); unchanged(ic);
}
static void managementProtection() {
    for (int subtype:{IEEE80211_FC0_SUBTYPE_DEAUTH,IEEE80211_FC0_SUBTYPE_DISASSOC})
    for (bool broadcast:{false,true}) for (bool valid:{false,true}) {
        ieee80211com ic; ic.ic_flags=IEEE80211_F_RSNON; ic.node.ni_flags=IEEE80211_NODE_RXMGMTPROT;
        ic.decryptValid=valid; auto m=frame(ic,subtype,broadcast);
        if (!broadcast) m.bytes[1]=IEEE80211_FC1_PROTECTED;
        receive(ic,m,IEEE80211_RXI_HWDEC);
        assert(ic.management==(valid?1U:0U) && ic.stateChanges==(valid?1U:0U));
        assert(ic.softwareDecrypts==(broadcast?1U:0U) && ic.hardwareDecrypts==(broadcast?0U:1U));
    }
    ieee80211com ic; ic.ic_flags=IEEE80211_F_RSNON; ic.node.ni_flags=IEEE80211_NODE_RXMGMTPROT;
    auto m=frame(ic,IEEE80211_FC0_SUBTYPE_DEAUTH); receive(ic,m);
    assert(ic.management==0 && ic.events==0 && ic.stateChanges==0);
}
static void shortFrames() {
    for (unsigned size=0;size<26;++size) {
        ieee80211com ic; auto m=frame(ic,IEEE80211_FC0_SUBTYPE_DEAUTH); m.length=size; receive(ic,m);
        assert(ic.events==0 && ic.stateChanges==0);
        if(size<24) assert(ic.management==0 && ic.node.ni_rxseq==40 && ic.node.ni_rssi==55);
    }
}
int main() { // TEST ENTRY
}
