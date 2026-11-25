#include <cstdio>
// Production IWX RX, crypto validation, duplicate handling and PN processing.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>
#include <sys/time.h>
#include <libkern/OSByteOrder.h>
#include "AirportItlwm/AWDL/DataFrame.hpp"
#include "AirportItlwm/AWDL/BlockAck.hpp"
#include "AirportItlwm/AWDL/RadioState.hpp"
#define htole16 OSSwapHostToLittleInt16
#define le16toh OSSwapLittleToHostInt16
#define letoh16 OSSwapLittleToHostInt16
#define le32toh OSSwapLittleToHostInt32
#define le64toh OSSwapLittleToHostInt64
#define AIRPORT_WCL
#define NBPFILTER 0
#define MIN(a,b) std::min((a),(b))
#define MAX(a,b) std::max((a),(b))
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define IC2IFP(ic) (&(ic)->ifp)
#define mtod(m,t) reinterpret_cast<t>((m)->data)
#define IEEE80211_IS_MULTICAST(p) ((p)[0]&1)
#define IEEE80211_ADDR_EQ(a,b) (std::memcmp((a),(b),6)==0)
#define LE_READ_2(p) (uint16_t((p)[0]) | (uint16_t((p)[1]) << 8))
#define _KASSERT assert

using u8=uint8_t;using __le16=uint16_t;using __be16=uint16_t;
using __le32=uint32_t;using __le64=uint64_t;
enum {IEEE80211_M_STA=1,IEEE80211_M_MONITOR=2,IEEE80211_FC0_TYPE_MASK=12,
    IEEE80211_FC0_SUBTYPE_MASK=240,IEEE80211_FC0_TYPE_MGT=0,IEEE80211_FC0_TYPE_CTL=4,
    IEEE80211_FC0_TYPE_DATA=8,IEEE80211_FC0_SUBTYPE_QOS=128,IEEE80211_FC0_SUBTYPE_NODATA=64,
    IEEE80211_FC1_DIR_MASK=3,IEEE80211_FC1_DIR_DSTODS=3,IEEE80211_FC1_DIR_FROMDS=2,
    IEEE80211_FC1_PROTECTED=64,IEEE80211_FC1_ORDER=128,IEEE80211_FC1_RETRY=8,
    IEEE80211_FC0_SUBTYPE_CTS=192,IEEE80211_FC0_SUBTYPE_ACK=208,
    IEEE80211_FC0_SUBTYPE_DEAUTH=192,IEEE80211_FC0_SUBTYPE_DISASSOC=160,
    IEEE80211_CCMP_HDRLEN=8,IEEE80211_TKIP_HDRLEN=8,IEEE80211_ADDR_LEN=6,
    IEEE80211_RXI_HWDEC_SAME_PN=1,IEEE80211_RXI_HWDEC=2,IEEE80211_RXI_SAME_SEQ=4,
    IEEE80211_QOS_AMSDU=128,IEEE80211_QOS_TID=15,IEEE80211_SEQ_SEQ_SHIFT=4,
    IEEE80211_CATEG_BA=3,IEEE80211_NODE_RXPROT=1,IEEE80211_F_RSNON=1,
    IEEE80211_WEP_EXTIV=32,IEEE80211_MMIE_LEN=18,IEEE80211_ELEMID_MMIE=76,
    IEEE80211_CIPHER_USEGROUP=0,IEEE80211_CIPHER_CCMP=1,IEEE80211_CIPHER_TKIP=2};
#define offsetofend(t,m) (offsetof(t,m)+sizeof(((t*)0)->m))
// FIRMWARE DECLARATIONS
struct ieee80211_frame {uint8_t i_fc[2],duration[2],i_addr1[6],i_addr2[6],i_addr3[6],i_seq[2];};
static const uint8_t etherbroadcastaddr[6]={255,255,255,255,255,255};
struct ieee80211_frame_cts {uint8_t bytes[10];};
struct ieee80211_frame_ack {uint8_t bytes[10];};
struct ieee80211_frame_min {uint8_t bytes[16];};
struct ieee80211_qosframe {ieee80211_frame h;uint8_t i_qos[2];};
struct ieee80211_qosframe_addr4 {ieee80211_frame h;uint8_t addr4[6],i_qos[2];};
// HEADER ROUTINES
struct Packet {uint8_t *data;size_t length,total;unsigned frees=0;};
using mbuf_t=Packet *;
static size_t mbuf_len(mbuf_t m) {return m->length;}
static size_t mbuf_pkthdr_len(mbuf_t m) {return m->total;}
static void *mbuf_data(mbuf_t m) {return m->data;}
static void mbuf_freem(mbuf_t m) {assert(m->frees++==0);}
static void mbuf_setdata(mbuf_t m,void *p,size_t n) {m->data=static_cast<uint8_t *>(p);m->length=n;}
static void mbuf_pkthdr_setlen(mbuf_t m,size_t n) {m->total=n;}
static void mbuf_setlen(mbuf_t m,size_t n) {m->length=n;}
static void mbuf_adj(mbuf_t m,size_t n) {assert(n<=m->length);m->data+=n;m->length-=n;m->total-=n;}
struct ieee80211_key {int k_cipher=IEEE80211_CIPHER_CCMP;uint64_t k_rsc[16]={},k_mgmt_rsc=0;};
struct ieee80211_node {
    uint8_t ni_bssid[6]={2,0xaa,0xbb,0xcc,0xdd,1};
    unsigned ni_flags=IEEE80211_NODE_RXPROT;
    int ni_rsncipher=IEEE80211_CIPHER_CCMP,ni_rsngroupcipher=IEEE80211_CIPHER_CCMP;
    ieee80211_key ni_pairwise_key;
    unsigned references=0,acquired=0;
};
struct iwx_node {ieee80211_node node;iwx_rxq_dup_data dup_data={};};
struct Channel {};
struct Stats {unsigned inputErrors=0;};
struct _ifnet {Stats stats;Stats *netStat=&stats;std::vector<std::vector<uint8_t>> delivered,management;};
struct ieee80211com {
    int ic_opmode=IEEE80211_M_STA,ic_max_rssi=100;
    unsigned ic_flags=IEEE80211_F_RSNON;
    uint8_t ic_myaddr[6]={2,0,0,0,0,1};
    iwx_node ap;ieee80211_node *ic_bss=&ap.node;
    ieee80211_key ic_nw_keys[6];Channel ic_channels[256];Channel *ic_ibss_chan=&ic_channels[48];
    struct {unsigned is_rx_tooshort=0,is_ccmp_dec_errs=0,is_ccmp_replays=0,is_tkip_replays=0,is_rx_wrongbss=0;} ic_stats;
    _ifnet ifp;
};
struct iwx_softc {ieee80211com sc_ic;int sc_device_family=IWX_DEVICE_FAMILY_22000;};
struct ieee80211_rxinfo {int rxi_flags,rxi_rssi,rxi_chan;uint32_t rxi_tstamp;};
struct mbuf_list {};
static int ieee80211_chan2ieee(ieee80211com *,Channel *) {return 48;}
static ieee80211_node *ieee80211_find_rxnode(ieee80211com *ic,const ieee80211_frame *) {
    assert(ic->ic_bss);++ic->ic_bss->references;++ic->ic_bss->acquired;return ic->ic_bss;
}
static void ieee80211_release_node(ieee80211com *,ieee80211_node *node) {
    assert(node->references);--node->references;
}
static void getmicrouptime(timeval *t) {t->tv_sec=42;t->tv_usec=0;}
static int ieee80211_tkip_get_tsc(uint64_t *,uint64_t **,mbuf_t,ieee80211_key *) {assert(false);return EINVAL;}
// NET80211 CRYPTO

// Observe the net80211 input boundary and run its actual PN update/IV removal.
// BA reorder and the final BSD protocol stack are outside this fixture.
static ieee80211com *activeIC;
static void ieee80211_inputm(_ifnet *ifp,mbuf_t m,ieee80211_node *ni,ieee80211_rxinfo *rxi,mbuf_list *) {
    auto wh=mtod(m,ieee80211_frame *);
    if((wh->i_fc[0]&IEEE80211_FC0_TYPE_MASK)==IEEE80211_FC0_TYPE_MGT) {
        ifp->management.emplace_back(m->data,m->data+m->length);mbuf_freem(m);return;
    }
    assert(activeIC && ifp==&activeIC->ifp && (rxi->rxi_flags&IEEE80211_RXI_HWDEC));
    m=ieee80211_input_hwdecrypt(activeIC,ni,m,rxi);
    if(!m)return;
    assert(!(m->data[1]&IEEE80211_FC1_PROTECTED));
    const unsigned header=ieee80211_get_hdrlen(mtod(m,ieee80211_frame *));
    ifp->delivered.emplace_back(m->data+header,m->data+m->length);
    mbuf_freem(m);
}
// RX DIAGNOSTIC DECLARATIONS
struct ItlIwx {
    bool receiveAWDLTXBlockAck(const uint8_t *,size_t) {return false;}
    uint64_t dmaEpoch = 7;
    iwx_softc com;
    AirportAWDL::ReceiveState awdlReceive={};AirportAWDL::DataReceiveState awdlData={};
    AirportAWDL::PeerTable awdlPeers={};AirportAWDL::RadioState awdlRadio={};
    uint64_t awdlNonAPDataDropped=0;
    void *awdlReceiver=nullptr;
    void (*awdlInput)(void *,const AirportAWDL::DataView &)=nullptr;
    void (*awdlActionInput)(void *,const uint8_t *,size_t,uint16_t,int16_t,uint64_t)=nullptr;
    void (*awdlBlockAckInput)(void *,const AirportAWDL::BlockAckRequest &)=nullptr;
    void publishAWDLReceive() {}
    uint32_t awdlReceiveDelay(const iwx_rx_mpdu_desc *,uint64_t) {return 0;}
    IwxAWDLReceiveDiagnostics awdlReceiveDiagnostics = {};
    void observeAWDLReceiveDescriptor(const iwx_rx_mpdu_desc *,size_t);
    void formatAWDLReceiveDiagnostics(char *,size_t) const;
    bool receiveAWDL(mbuf_t,iwx_rx_mpdu_desc *);
    int iwx_rxmq_get_signal_strength(iwx_softc *,iwx_rx_mpdu_desc *);
    void iwx_rx_mpdu_mq(iwx_softc *,mbuf_t,void *,size_t,mbuf_list *);
    int iwx_rx_hwdecrypt(iwx_softc *,mbuf_t,uint32_t,ieee80211_rxinfo *);
    int iwx_detect_duplicate(iwx_softc *,mbuf_t,iwx_rx_mpdu_desc *,ieee80211_rxinfo *);
    int iwx_ccmp_decap(iwx_softc *,mbuf_t,ieee80211_node *,ieee80211_rxinfo *);
    bool receiveAWDLReordered(mbuf_t,iwx_rx_mpdu_desc *,mbuf_list *) { return false; }
    bool receiveAWDLBlockAckEnd(const AirportAWDL::BlockAckRequest &,bool=false) {return false;}
    int iwx_rx_reorder(iwx_softc *,mbuf_t,int,iwx_rx_mpdu_desc *,int,int,uint32_t,ieee80211_rxinfo *,mbuf_list *) {return 0;}
    void iwx_rx_frame(iwx_softc *,mbuf_t,int,uint32_t,int,int,uint32_t,ieee80211_rxinfo *,mbuf_list *);
    ItlIwx() {awdlRadio.mac=true;awdlRadio.ready=true;}
};
// DRIVER ROUTINES

static const std::vector<uint8_t> payload={0xaa,0xaa,3,0,0,0,8,0,0x45,0,0,20,0,0,0,0,64,17,0,0,10,0,0,1,10,0,0,2};
static std::vector<uint8_t> apData(ItlIwx &d,uint16_t sequence,uint64_t pn,bool group=false) {
    std::vector<uint8_t> b(26+8,0);
    b[0]=0x88;b[1]=IEEE80211_FC1_DIR_FROMDS|IEEE80211_FC1_PROTECTED;b[24]=3;
    AirportAWDL::copyAddress(b.data()+4,d.com.sc_ic.ic_myaddr);
    if(group)std::memset(b.data()+4,0xff,6);
    AirportAWDL::copyAddress(b.data()+10,d.com.sc_ic.ic_bss->ni_bssid);
    b[16]=2;b[21]=9;AirportAWDL::putLE16(b.data()+22,sequence<<4);
    b[26]=pn;b[27]=pn>>8;b[29]=IEEE80211_WEP_EXTIV|(group?64:0);
    for(unsigned i=0;i<4;++i)b[30+i]=pn>>(16+8*i);
    b.insert(b.end(),payload.begin(),payload.end());return b;
}
static constexpr unsigned good=IWX_RX_MPDU_RES_STATUS_CRC_OK|IWX_RX_MPDU_RES_STATUS_OVERRUN_OK|
    IWX_RX_MPDU_RES_STATUS_SEC_CCM_ENC|IWX_RX_MPDU_RES_STATUS_DEC_DONE|IWX_RX_MPDU_RES_STATUS_MIC_OK;
static void receive(ItlIwx &d,std::vector<uint8_t> frame,unsigned status=good,
    bool padded=false,unsigned macMask=3,bool v3=false,unsigned subframe=0,bool amsdu=false) {
    unsigned header=ieee80211_get_hdrlen(reinterpret_cast<const ieee80211_frame *>(frame.data()));
    if(padded)frame.insert(frame.begin()+header+((status&IWX_RX_MPDU_RES_STATUS_SEC_ENC_MSK)==IWX_RX_MPDU_RES_STATUS_SEC_CCM_ENC?8:0),2,0);
    const size_t descriptor=v3?sizeof(iwx_rx_mpdu_desc):IWX_RX_DESC_SIZE_V1;
    std::vector<uint8_t> bytes(descriptor+frame.size(),0);
    auto desc=reinterpret_cast<iwx_rx_mpdu_desc *>(bytes.data());
    desc->status=status;desc->mpdu_len=frame.size();desc->amsdu_info=subframe;
    desc->mac_flags2=(padded?IWX_RX_MPDU_MFLG2_PAD:0)|(amsdu?IWX_RX_MPDU_MFLG2_AMSDU:0);
    if(v3){desc->v3.channel=48;desc->v3.energy_a=40;desc->v3.mac_context=macMask;}
    else{desc->v1.channel=48;desc->v1.energy_a=40;desc->v1.mac_context=macMask;}
    std::memcpy(bytes.data()+descriptor,frame.data(),frame.size());
    Packet m={bytes.data(),bytes.size(),bytes.size()};activeIC=&d.com.sc_ic;
    d.com.sc_device_family=v3?IWX_DEVICE_FAMILY_AX210:IWX_DEVICE_FAMILY_22000;
    d.iwx_rx_mpdu_mq(&d.com,&m,desc,bytes.size(),nullptr);
    assert(m.frees==1 && !d.com.sc_ic.ap.node.references);
}

static void rawCopyBeforeValid() {
    const unsigned invalid[]={3,good&~IWX_RX_MPDU_RES_STATUS_MIC_OK,
        good&~IWX_RX_MPDU_RES_STATUS_DEC_DONE,
        (good&~IWX_RX_MPDU_RES_STATUS_SEC_ENC_MSK)|IWX_RX_MPDU_RES_STATUS_SEC_ENC_ERR};
    for(bool v3:{false,true})for(bool pad:{false,true})for(bool group:{false,true})for(unsigned mask:{0U,1U,2U,3U})for(unsigned status:invalid) {
        ItlIwx d;auto &ic=d.com.sc_ic;auto &key=group?ic.ic_nw_keys[1]:ic.ap.node.ni_pairwise_key;
        key.k_rsc[3]=40;ic.ap.dup_data.last_seq[3]=90;
        auto b=apData(d,91,41,group);receive(d,b,status,pad,mask,v3);
        assert(ic.ifp.delivered.empty() && key.k_rsc[3]==40 && ic.ap.dup_data.last_seq[3]==90);
        assert(ic.ic_stats.is_ccmp_dec_errs==1 && ic.ifp.stats.inputErrors==1 && ic.ap.node.acquired==1);
        b[1]|=IEEE80211_FC1_RETRY;receive(d,b,good,pad,mask,v3);
        assert(ic.ifp.delivered.size()==1 && ic.ifp.delivered[0]==payload && key.k_rsc[3]==41);
        assert(ic.ap.dup_data.last_seq[3]==(group?90:91) && ic.ap.node.acquired==3);
        assert(!d.awdlNonAPDataDropped && !d.awdlData.valid);
    }
}

static void replayAndSubframes() {
    ItlIwx d;auto &ic=d.com.sc_ic;auto &key=ic.ap.node.ni_pairwise_key;
    auto b=apData(d,101,101);receive(d,b);
    assert(ic.ifp.delivered.size()==1 && key.k_rsc[3]==101);
    b[1]|=IEEE80211_FC1_RETRY;receive(d,b);
    assert(ic.ifp.delivered.size()==1 && !ic.ic_stats.is_ccmp_replays);
    b=apData(d,102,100);receive(d,b);
    assert(ic.ifp.delivered.size()==1 && key.k_rsc[3]==101 && ic.ic_stats.is_ccmp_replays==1);
    b=apData(d,103,102);receive(d,b,good,true,2,false,0,true);
    receive(d,b,good,true,2,false,1,true);
    assert(ic.ifp.delivered.size()==3 && key.k_rsc[3]==102 && ic.ap.dup_data.last_sub_frame[3]==1);
    b=apData(d,104,102);receive(d,b);
    assert(ic.ifp.delivered.size()==3 && key.k_rsc[3]==102 && ic.ic_stats.is_ccmp_replays==2);
    for(const auto &received:ic.ifp.delivered)assert(received==payload);
}

static void receiveOwnership() {
    ItlIwx d;auto &ic=d.com.sc_ic;auto b=apData(d,22,22);
    b[10]^=4;receive(d,b);b=apData(d,22,22);b[9]^=4;receive(d,b);
    assert(d.awdlNonAPDataDropped==2 && ic.ap.node.acquired==0 && !ic.ap.dup_data.last_seq[3]);
    b.resize(33);b[0]=0xd0;b[1]=0;b[24]=IEEE80211_CATEG_BA;b[25]=0;
    AirportAWDL::copyAddress(b.data()+16,AirportAWDL::BSSID);receive(d,b,3);
    assert(ic.ap.node.acquired==0 && ic.ifp.management.empty());
    AirportAWDL::copyAddress(b.data()+10,ic.ap.node.ni_bssid);
    AirportAWDL::copyAddress(b.data()+16,ic.ap.node.ni_bssid);
    AirportAWDL::copyAddress(b.data()+4,ic.ic_myaddr);receive(d,b,3);
    assert(ic.ap.node.acquired==2 && ic.ifp.management.size()==1 && ic.ifp.management[0]==b);
    assert(ic.ap.dup_data.last_seq[IWX_MAX_TID_COUNT]==22 && !ic.ap.node.ni_pairwise_key.k_mgmt_rsc);
    receive(d,apData(d,22,22));assert(ic.ifp.delivered.size()==1);
}

static void disconnectOwnership() {
    for(bool v3:{false,true})for(bool pad:{false,true})for(bool group:{false,true})
    for(unsigned mask:{0U,1U,2U,3U})for(unsigned subtype:{0xc0U,0xa0U})for(unsigned wrong:{0U,1U,2U,3U}) {
        ItlIwx d;auto &ic=d.com.sc_ic;
        ic.ap.dup_data.last_seq[IWX_MAX_TID_COUNT]=90;
        std::vector<uint8_t> b(26,0);b[0]=subtype;b[24]=2;
        AirportAWDL::copyAddress(b.data()+4,group?etherbroadcastaddr:ic.ic_myaddr);
        AirportAWDL::copyAddress(b.data()+10,ic.ap.node.ni_bssid);
        AirportAWDL::copyAddress(b.data()+16,ic.ap.node.ni_bssid);
        AirportAWDL::putLE16(b.data()+22,91<<4);
        if(wrong)b[4+(wrong-1)*6+5]^=1;
        receive(d,b,3,pad,mask,v3);
        if(wrong) {
            assert(ic.ifp.management.empty() && ic.ap.node.acquired==0);
            assert(ic.ap.dup_data.last_seq[IWX_MAX_TID_COUNT]==90 && ic.ic_stats.is_rx_wrongbss==1);
            // A legitimate AP retry with the same sequence still reaches net80211.
            b[4+(wrong-1)*6+5]^=1;b[1]|=IEEE80211_FC1_RETRY;
            receive(d,b,3,pad,mask,v3);
        }
        assert(ic.ifp.management.size()==1 && ic.ap.node.acquired==2);
        assert(ic.ap.dup_data.last_seq[IWX_MAX_TID_COUNT]==(group?90:91));
        assert(!ic.ap.node.ni_pairwise_key.k_mgmt_rsc);
    }
}

int main() {
    static_assert(sizeof(ieee80211_frame)==24 && IWX_RX_DESC_SIZE_V1==48,"wire sizes");
    // TEST ENTRY
}
