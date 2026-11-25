#include <cstdio>
// The production RX dispatcher and handlers are inserted without editing their bodies.
// This host boundary models DMA copies, shared mbufs, command wakeups and register writes.
// Firmware scheduling and RF delivery remain outside this test.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <sys/time.h>
#include <libkern/OSByteOrder.h>
#include "AirportItlwm/AWDL/DataFrame.hpp"
#include "AirportItlwm/AWDL/BlockAck.hpp"
#include "AirportItlwm/AWDL/RadioState.hpp"
#include "itlwm/hal_iwx/BTCoex.hpp"
#include "include/HAL/ItlTx.hpp"

#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define ETHER_ADDR_LEN 6
#define IEEE80211_NWID_LEN 32
using __le16=uint16_t;using __le32=uint32_t;using __le64=uint64_t;
using __be16=uint16_t;using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;
#define BIT(x) (1UL<<(x))
#define offsetofend(t,m) (offsetof(t,m)+sizeof(((t*)0)->m))
#define htole16 OSSwapHostToLittleInt16
#define htole32 OSSwapHostToLittleInt32
#define le16toh OSSwapLittleToHostInt16
#define le32toh OSSwapLittleToHostInt32
#define le64toh OSSwapLittleToHostInt64
#define le16_to_cpu le16toh
static uint32_t le32_to_cpup(const uint32_t *p) {uint32_t value;std::memcpy(&value,p,4);return le32toh(value);}
struct ieee80211_frame {uint8_t i_fc[2],i_dur[2],i_addr1[6],i_addr2[6],i_addr3[6],i_seq[2];};
#include "itlwm/hal_iwx/if_iwxreg.h"

#define AIRPORT_WCL
#define DPRINTF(...) do {} while (0)
#define DPRINTFN(...) do {} while (0)
#define XYLog(...) do {} while (0)
#define KASSERT(condition, message) assert(condition)
#define MIN(a,b) std::min((a),(b))
#define MAX(a,b) std::max((a),(b))
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define roundup(x,y) ((((x)+(y)-1)/(y))*(y))
#define IC2IFP(ic) (&(ic)->ic_if)
#define mtod(m,t) reinterpret_cast<t>(mbuf_data(m))
#define IEEE80211_ADDR_EQ(a,b) (std::memcmp((a),(b),6)==0)
#define isset(a,n) ((a)[(n)/8] & (1U<<((n)%8)))
#define BUS_DMASYNC_POSTREAD 1
#define MBUF_COPYALL -1
#define MBUF_DONTWAIT 0
#define MBUF_TYPE_FREE 0
#define MBUF_LIST_INITIALIZER() {}
static uint32_t le32_get_bits(uint32_t value,uint32_t mask) {return (le32toh(value)&mask)>>__builtin_ctz(mask);}

struct Packet {
    std::shared_ptr<std::vector<uint8_t>> storage;
    size_t offset=0,length=0,total=0;
    unsigned frees=0;
};
using mbuf_t=Packet *;
static std::vector<std::unique_ptr<Packet>> packets;
static unsigned copies=0;
static bool failCopy=false;
static mbuf_t packet(size_t size) {
    std::unique_ptr<Packet> p(new Packet);
    p->storage=std::make_shared<std::vector<uint8_t>>(size,0);p->length=p->total=size;
    auto result=p.get();packets.push_back(std::move(p));return result;
}
static size_t mbuf_len(mbuf_t m) {assert(!m->frees);return m->length;}
static size_t mbuf_pkthdr_len(mbuf_t m) {assert(!m->frees);return m->total;}
static void *mbuf_data(mbuf_t m) {assert(!m->frees);return m->storage->data()+m->offset;}
static void mbuf_freem(mbuf_t m) {assert(m && m->frees++==0);}
static int mbuf_type(mbuf_t m) {return m->frees?MBUF_TYPE_FREE:1;}
static void mbuf_setdata(mbuf_t m,void *p,size_t n) {
    auto offset=static_cast<uint8_t *>(p)-m->storage->data();
    assert(offset>=0 && size_t(offset)+n<=m->storage->size());m->offset=offset;m->length=n;
}
static void mbuf_pkthdr_setlen(mbuf_t m,size_t n) {m->total=n;}
static void mbuf_setlen(mbuf_t m,size_t n) {m->length=n;}
static void mbuf_adj(mbuf_t m,size_t n) {
    assert(!m->frees && n<=m->length && n<=m->total);
    m->offset+=n;m->length-=n;m->total-=n;
}
static void mbuf_copym(mbuf_t m,int offset,int length,int flags,mbuf_t *result) {
    assert(offset==0 && length==MBUF_COPYALL && flags==MBUF_DONTWAIT);
    if(failCopy) {*result=nullptr;return;}
    std::unique_ptr<Packet> p(new Packet(*m));*result=p.get();packets.push_back(std::move(p));++copies;
}
struct DMAMap {std::vector<uint8_t> bytes;bool failRead=false;unsigned reads=0;};
using bus_dmamap_t=DMAMap *;
using bus_addr_t=uint64_t;
static bool bus_dmamap_read(DMAMap *map,mbuf_t m,size_t size) {
    assert(map && map->bytes.size()==size && m->storage->size()==size);++map->reads;
    if(map->failRead)return false;
    std::memcpy(mbuf_data(m),map->bytes.data(),size);return true;
}
static void bus_dmamap_sync(void *,DMAMap *,size_t,size_t,unsigned) {}

enum {IEEE80211_M_STA=1,IEEE80211_M_MONITOR=2,IEEE80211_FC0_TYPE_MASK=12,
    IEEE80211_FC0_SUBTYPE_MASK=240,IEEE80211_FC0_TYPE_MGT=0,IEEE80211_FC0_TYPE_CTL=4,
    IEEE80211_FC0_TYPE_DATA=8,IEEE80211_FC1_DIR_MASK=3,IEEE80211_FC1_DIR_FROMDS=2,
    IEEE80211_FC0_SUBTYPE_CTS=192,IEEE80211_FC0_SUBTYPE_ACK=208,IEEE80211_CCMP_HDRLEN=8,
    IEEE80211_FC0_SUBTYPE_DEAUTH=192,IEEE80211_FC0_SUBTYPE_DISASSOC=160,
    IEEE80211_RXI_HWDEC_SAME_PN=1,IEEE80211_QOS_AMSDU=128,IEEE80211_CATEG_BA=3};
struct ieee80211_frame_cts {uint8_t bytes[10];};
static const uint8_t etherbroadcastaddr[6]={255,255,255,255,255,255};
struct ieee80211_frame_ack {uint8_t bytes[10];};
struct ieee80211_frame_min {uint8_t bytes[16];};
struct ieee80211_qosframe {ieee80211_frame header;uint16_t i_qos[1];};
struct ieee80211_qosframe_addr4 {ieee80211_frame header;uint8_t addr4[6];uint16_t i_qos[1];};
static bool ieee80211_has_qos(const ieee80211_frame *f) {return (f->i_fc[0]&0x8c)==0x88;}
static bool ieee80211_has_addr4(const ieee80211_frame *f) {return (f->i_fc[1]&3)==3;}
static unsigned ieee80211_get_hdrlen(const ieee80211_frame *f) {
    assert(!ieee80211_has_qos(f) && !ieee80211_has_addr4(f));return sizeof(*f);
}
struct ieee80211_node {unsigned releases=0;uint8_t ni_bssid[6]={};};
struct iwx_node {ieee80211_node in_ni;int in_id=0;};
struct Stats {unsigned inputErrors=0,outputErrors=0;};
struct _ifnet {Stats stats;Stats *netStat=&stats;unsigned inputs=0;};
struct ieee80211com {
    int ic_opmode=IEEE80211_M_STA,ic_max_rssi=100;
    uint8_t ic_myaddr[6]={2,0,0,0,0,1};ieee80211_node *ic_bss=nullptr;
    struct {int is_rx_tooshort=0,is_rx_wrongbss=0;} ic_stats;
    _ifnet ic_if;
};
struct ieee80211_rxinfo {int rxi_flags,rxi_rssi,rxi_chan;uint32_t rxi_tstamp;};
struct mbuf_list {};
static bool ml_empty(const mbuf_list *) {return false;}
static void if_input(_ifnet *ifp,mbuf_list *) {++ifp->inputs;}
static void ieee80211_release_node(ieee80211com *,ieee80211_node *node) {++node->releases;}
static void getmicrouptime(timeval *t) {t->tv_sec=42;t->tv_usec=0;}

// DRIVER DECLARATIONS
struct iwx_tx_ring {
    unsigned ring_count=64;int tail=0,cur=0,queued=0,qid=0;
    iwx_tx_data data[64]={};iwx_tfh_tfd descriptors[64]={};iwx_tfh_tfd *desc=descriptors;
};
struct iwx_rx_ring {iwx_rx_data data[IWX_RX_MQ_RING_COUNT]={};int cur=0;void *stat=nullptr;};
struct iwx_reorder_buffer {unsigned removed=1,buf_size=0;};
struct iwx_rxba_data {unsigned baid=IWX_RX_REORDER_DATA_INVALID_BAID,tid=0,sta_id=0;iwx_reorder_buffer reorder_buf;};
struct iwx_softc {
    ieee80211com sc_ic;int sc_device_family=IWX_DEVICE_FAMILY_22000;
    void *sc_dmat=nullptr;iwx_tx_ring txq[16];iwx_rx_ring rxq;
    uint8_t sc_enabled_capa[32]={},sc_ucode_api[32]={};
    unsigned sc_flags=0,sc_tx_timer=10;uint8_t sc_mgmt_last_antenna_idx=0;
    struct {uint32_t uc_lmac_error_event_table[2],uc_log_event_table,uc_umac_error_event_table;
        int uc_ok,uc_intr;} sc_uc={};
    uint32_t sku_id[3]={};int sc_noise=0,sc_init_complete=0,init_task=0,bt_coex_task=0;
    void *sc_cmd_resp_pkt[64]={};size_t sc_cmd_resp_len[64]={};
    iwx_rxba_data sc_rxba_data[32];
    std::vector<unsigned> doorbells;
};
#undef IWX_WRITE
#define IWX_WRITE(sc,reg,value) do {assert((reg)==IWX_RFH_Q0_FRBDCB_WIDX_TRG);(sc)->doorbells.push_back(value);} while(0)
static void *systq=nullptr;
static void task_add(void *,int *) {assert(false);}
struct Gate {
    std::vector<const void *> wakes;
    void commandWakeup(const void *p) {wakes.push_back(p);}
    unsigned count(const void *p) const {return std::count(wakes.begin(),wakes.end(),p);}
};

using AirportAWDL::RadioState;
// RX DIAGNOSTIC DECLARATIONS
struct ItlIwx {
    bool receiveAWDLTXBlockAck(const uint8_t *,size_t);
    uint64_t dmaEpoch = 7;
    unsigned getTxQueueSize(){return 256;}
    iwx_softc com;Gate gate;
    AirportAWDL::ReceiveState awdlReceive={};AirportAWDL::PeerTable awdlPeers={};
    AirportAWDL::DataReceiveState awdlData={};RadioState awdlRadio={};
    uint64_t awdlNonAPDataDropped=0;
    void *awdlReceiver=nullptr;
    void (*awdlAvailabilityChanged)(void *)=nullptr;
    void (*awdlDataComplete)(void *,uint64_t,int)=nullptr;
    void (*awdlTxComplete)(void *,uint64_t,bool)=nullptr;
    void (*awdlInput)(void *,const AirportAWDL::DataView &)=nullptr;
    void (*awdlActionInput)(void *,const uint8_t *,size_t,uint16_t,int16_t,uint64_t)=nullptr;
    void (*awdlBlockAckInput)(void *,const AirportAWDL::BlockAckRequest &)=nullptr;
    unsigned forwarded=0,publications=0,refills=0,crypto=0,duplicates=0;bool failRefill=false;
    unsigned refillAttempts=0,refillFailuresRemaining=0;
    std::vector<std::unique_ptr<DMAMap>> maps;
    Gate *getMainCommandGate() {return &gate;}
    int iwx_rx_pkt_valid(iwx_rx_packet *);
    void iwx_rx_pkt(iwx_softc *,iwx_rx_data *,mbuf_list *);
    void iwx_notif_intr(iwx_softc *);
    void iwx_cmd_done(iwx_softc *,int,int,int);
    void iwx_rx_session_protection(iwx_softc *,const iwx_rx_packet *);
    void receiveAWDLSession(const iwx_rx_packet *);
    IwxBTCoexState btCoex = {};
    void iwx_bt_coex_complete_tx(iwx_softc *,iwx_tx_data *d,bool){assert(!d->coexToken);}
    void iwx_bt_coex_statistics(iwx_softc *,const iwx_rx_packet *){}
    void iwx_rx_bt_coex_profile(iwx_softc *, const iwx_rx_packet *);
    void iwx_rx_tx_cmd(iwx_softc *,iwx_rx_packet *,iwx_rx_data *);
    void iwx_rx_tx_cmd_single(iwx_softc *,iwx_rx_packet *,iwx_tx_data *);
    void iwx_txd_done(iwx_softc *,iwx_tx_data *);
    void iwx_ampdu_txq_advance(iwx_softc *,iwx_tx_ring *,int);
    void completeAWDLTx(iwx_tx_data *,bool,bool=false);
    void sampleAWDLRate(iwx_softc *,iwx_rx_packet *,iwx_tx_data *d) {assert(!d->awdlRatePeer);}
    void iwx_rx_mpdu_mq(iwx_softc *,mbuf_t,void *,size_t,mbuf_list *);
    IwxAWDLReceiveDiagnostics awdlReceiveDiagnostics = {};
    void observeAWDLReceiveDescriptor(const iwx_rx_mpdu_desc *,size_t);
    void formatAWDLReceiveDiagnostics(char *,size_t) const;
    bool receiveAWDL(mbuf_t,iwx_rx_mpdu_desc *);
    int iwx_rxmq_get_signal_strength(iwx_softc *,iwx_rx_mpdu_desc *);
    uint32_t awdlReceiveDelay(const iwx_rx_mpdu_desc *,uint64_t) {return 0;}
    void publishAWDLReceive() {++publications;}
    int iwx_rx_addbuf(iwx_softc *sc,size_t size,unsigned slot) {
        ++refillAttempts;
        if(failRefill)return 12;
        if(refillFailuresRemaining) {--refillFailuresRemaining;return 12;}
        assert(sc==&com && slot==unsigned(sc->rxq.cur));++refills;
        sc->rxq.data[slot].m=packet(size);return 0;
    }
    void iwx_clear_oactive(iwx_softc *,iwx_tx_ring *) {}
    void iwx_toggle_tx_ant(iwx_softc *,uint8_t *) {assert(false);}
    void iwx_rx_rx_phy_cmd(iwx_softc *,iwx_rx_packet *,iwx_rx_data *) {assert(false);}
    void iwx_rx_tx_ba_notif(iwx_softc *,iwx_rx_packet *,iwx_rx_data *) {assert(false);}
    void iwx_rx_bmiss(iwx_softc *,iwx_rx_packet *,iwx_rx_data *) {assert(false);}
    int iwx_get_noise(uint8_t *) {assert(false);return 0;}
    void iwx_rx_time_event_response(iwx_softc *,iwx_rx_packet *) {assert(false);}
    void iwx_endscan(iwx_softc *) {assert(false);}
    void iwx_mcc_update(iwx_softc *,iwx_mcc_chub_notif *) {assert(false);}
    void iwx_rx_time_event_notification(iwx_softc *,iwx_rx_packet *) {assert(false);}
    void iwx_rs_update(iwx_softc *,iwx_tlc_update_notif *) {assert(false);}
    unsigned barReleases=0,lastBarNSSN=0;
    void iwx_release_frames(iwx_softc *,ieee80211_node *,iwx_rxba_data *,iwx_reorder_buffer *,unsigned nssn,mbuf_list *) {++barReleases;lastBarNSSN=nssn;}
    int iwx_rx_hwdecrypt(iwx_softc *,mbuf_t,uint16_t,ieee80211_rxinfo *) {++crypto;return 0;}
    int iwx_detect_duplicate(iwx_softc *,mbuf_t,iwx_rx_mpdu_desc *,ieee80211_rxinfo *) {++duplicates;return 0;}
    bool receiveAWDLReordered(mbuf_t,iwx_rx_mpdu_desc *,mbuf_list *) { return false; }
    bool receiveAWDLBlockAckEnd(const AirportAWDL::BlockAckRequest &,bool=false);
    AirportAWDL::BlockAckState awdlBA={};
    void (*awdlBlockAckEndInput)(void *,const AirportAWDL::BlockAckRequest &)=nullptr;
    int iwx_rx_reorder(iwx_softc *,mbuf_t,int,iwx_rx_mpdu_desc *,int,int,uint32_t,ieee80211_rxinfo *,mbuf_list *) {return 0;}
    void iwx_rx_frame(iwx_softc *,mbuf_t m,int,uint16_t,int,int,uint32_t,ieee80211_rxinfo *,mbuf_list *) {
        ++forwarded;mbuf_freem(m);
    }
    ~ItlIwx() {for(auto p:com.sc_cmd_resp_pkt)std::free(p);}
};

// DISPATCH DECLARATIONS
// PRODUCTION ROUTINES

struct Batch {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(IWX_RBUF_SIZE,0);
    size_t offset=0;
    void add(uint8_t group,uint8_t command,uint8_t qid,uint8_t index,const void *payload,size_t size) {
        assert(offset+sizeof(iwx_rx_packet)+size<bytes.size());
        auto p=reinterpret_cast<iwx_rx_packet *>(bytes.data()+offset);
        p->len_n_flags=htole32(sizeof(p->hdr)+size);
        p->hdr.cmd=command;p->hdr.group_id=group;p->hdr.qid=qid;p->hdr.idx=index;
        if(size)std::memcpy(p->data,payload,size);
        offset+=roundup(sizeof(*p)+size,IWX_FH_RSCSR_FRAME_ALIGN);
    }
    void session(unsigned mac,unsigned start,unsigned status=1,unsigned config=IWX_SESSION_PROTECT_CONF_P2P_GO_NEGOTIATION) {
        iwx_session_prot_notif n={};n.mac_id=htole32(mac);n.start=htole32(start);
        n.status=htole32(status);n.conf_id=htole32(config);
        add(IWX_MAC_CONF_GROUP,IWX_SESSION_PROTECTION_NOTIF,0x80,0,&n,sizeof(n));
    }
    void ack(unsigned index,bool narrow=false) {
        uint32_t status=htole32(1);
        add(narrow?IWX_LONG_GROUP:IWX_MAC_CONF_GROUP,
            narrow?IWX_MAC_CONTEXT_CMD:IWX_SESSION_PROTECTION_CMD,0,index,&status,sizeof(status));
    }
    void tx(unsigned ssn,unsigned status=IWX_TX_STATUS_SUCCESS) {
        std::vector<uint8_t> body(sizeof(iwx_tx_resp)+sizeof(uint32_t),0);
        auto response=reinterpret_cast<iwx_tx_resp *>(body.data());
        response->frame_count=1;response->tx_queue=htole16(2);response->ra_tid=IWX_MAX_TID_COUNT;
        response->status.status=htole16(status);uint32_t wire=htole32(ssn);
        std::memcpy(&response->status+1,&wire,sizeof(wire));
        add(IWX_LEGACY_GROUP,IWX_TX_CMD,2,0,body.data(),body.size());
    }
    void action(bool crc=true) {
        static const uint8_t body[]={127,0,0x17,0xf2,8,0x10,0,0,0,0,0,0,0,0,0,0,21,2,0,0x31,2};
        std::vector<uint8_t> payload(IWX_RX_DESC_SIZE_V1+24+sizeof(body),0);
        auto desc=reinterpret_cast<iwx_rx_mpdu_desc *>(payload.data());
        desc->status=htole16(crc?IWX_RX_MPDU_RES_STATUS_CRC_OK|IWX_RX_MPDU_RES_STATUS_OVERRUN_OK:0);
        desc->mpdu_len=htole16(24+sizeof(body));desc->v1.energy_a=45;desc->v1.energy_b=60;desc->v1.channel=44;
        auto frame=payload.data()+IWX_RX_DESC_SIZE_V1;frame[0]=0xd0;frame[10]=2;frame[15]=9;
        std::fill(frame+4,frame+10,0xff);std::memcpy(frame+24,body,sizeof(body));
        add(IWX_LEGACY_GROUP,IWX_REPLY_RX_MPDU_CMD,0x80,0,payload.data(),payload.size());
    }
    void data(const uint8_t *receiver,const uint8_t *transmitter) {
        std::vector<uint8_t> payload(IWX_RX_DESC_SIZE_V1+32,0);
        auto desc=reinterpret_cast<iwx_rx_mpdu_desc *>(payload.data());
        desc->status=htole16(IWX_RX_MPDU_RES_STATUS_CRC_OK|IWX_RX_MPDU_RES_STATUS_OVERRUN_OK);
        desc->mpdu_len=htole16(32);desc->v1.energy_a=45;desc->v1.channel=44;
        auto frame=payload.data()+IWX_RX_DESC_SIZE_V1;frame[0]=IEEE80211_FC0_TYPE_DATA;
        frame[1]=IEEE80211_FC1_DIR_FROMDS;
        AirportAWDL::copyAddress(frame+4,receiver);AirportAWDL::copyAddress(frame+10,transmitter);
        add(IWX_LEGACY_GROUP,IWX_REPLY_RX_MPDU_CMD,0x80,0,payload.data(),payload.size());
    }
    void control(const uint8_t *receiver,const uint8_t *transmitter,unsigned length=24) {
        std::vector<uint8_t> payload(IWX_RX_DESC_SIZE_V1+length,0);
        auto desc=reinterpret_cast<iwx_rx_mpdu_desc *>(payload.data());
        desc->status=htole16(IWX_RX_MPDU_RES_STATUS_CRC_OK|IWX_RX_MPDU_RES_STATUS_OVERRUN_OK);
        desc->mpdu_len=htole16(length);desc->v1.channel=6;
        auto frame=payload.data()+IWX_RX_DESC_SIZE_V1;frame[0]=0x84;
        AirportAWDL::copyAddress(frame+4,receiver);AirportAWDL::copyAddress(frame+10,transmitter);
        add(IWX_LEGACY_GROUP,IWX_REPLY_RX_MPDU_CMD,0x80,0,payload.data(),payload.size());
    }
};

struct Events {
    ItlIwx *driver;std::vector<std::string> order;
    std::vector<std::pair<uint64_t,bool>> tx;
    explicit Events(ItlIwx &d):driver(&d) {
        d.awdlReceiver=this;d.awdlRadio.ready=true;d.awdlRadio.mac=true;d.awdlRadio.dwell.requested=true;
        d.awdlRadio.dwell.confID=IWX_SESSION_PROTECT_CONF_P2P_GO_NEGOTIATION;
        AirportAWDL::copyAddress(d.awdlRadio.address,d.com.sc_ic.ic_myaddr);
        d.awdlAvailabilityChanged=[](void *ctx) {
            auto &e=*static_cast<Events *>(ctx);auto &d=e.driver->awdlRadio.dwell;
            e.order.push_back(d.done?"end":d.running?"start":"unavailable");
        };
        d.awdlTxComplete=[](void *ctx,uint64_t token,bool success) {
            auto &e=*static_cast<Events *>(ctx);e.tx.push_back({token,success});e.order.push_back("tx");
        };
        d.awdlActionInput=[](void *ctx,const uint8_t *frame,size_t length,uint16_t channel,int16_t rssi,uint64_t time) {
            assert(frame[24]==127 && length==45 && channel==44 && rssi==-45 && time==42000000);
            static_cast<Events *>(ctx)->order.push_back("rx");
        };
    }
};
static mbuf_t queueCommand(ItlIwx &d,unsigned index,bool narrow=false) {
    auto &r=d.com.txq[IWX_DQA_CMD_QUEUE];auto &data=r.data[index];
    data.flags=IWX_TXDATA_FLAG_CMD_PENDING|(narrow?IWX_TXDATA_FLAG_CMD_IS_NARROW:0);
    data.m=packet(48);++r.queued;
    d.com.sc_cmd_resp_len[index]=128;d.com.sc_cmd_resp_pkt[index]=std::calloc(1,128);
    return data.m;
}
static mbuf_t queueTX(ItlIwx &d,unsigned index,uint64_t token) {
    auto &ring=d.com.txq[2];ring.qid=2;
    auto &data=ring.data[index];data.flags=IWX_TXDATA_FLAG_AWDL;data.awdlToken=token;data.m=packet(45);
    ring.desc[index].num_tbs=htole16(2);ring.desc[index].tbs[0].tb_len=htole16(8);
    ring.desc[index].tbs[1].tb_len=htole16(45);++ring.queued;
    ring.cur=(ring.cur+1)%256;
    return data.m;
}
static mbuf_t install(ItlIwx &d,unsigned slot,const Batch &batch,bool failedDMA=false) {
    std::unique_ptr<DMAMap> map(new DMAMap);map->bytes=batch.bytes;map->failRead=failedDMA;
    auto &rx=d.com.rxq.data[slot];rx.m=packet(IWX_RBUF_SIZE);rx.map=map.get();
    d.maps.push_back(std::move(map));return rx.m;
}
static void interrupt(ItlIwx &d,unsigned end) {
    iwx_rb_status status={};status.closed_rb_num=htole16(end);d.com.rxq.stat=&status;
    d.iwx_notif_intr(&d.com);d.com.rxq.stat=nullptr;
}

static void completeBatch() {
    ItlIwx d;Events events(d);iwx_node ap;d.com.sc_ic.ic_bss=&ap.in_ni;
    d.com.sc_enabled_capa[IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD/8]|=1U<<(IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD%8);
    d.com.sc_flags|=IWX_FLAG_TE_ACTIVE;
    auto command=queueCommand(d,3);auto tx=queueTX(d,0,71);
    Batch batch;batch.ack(3);batch.session(RadioState::MacID,1);batch.action();batch.tx(1);batch.session(RadioState::MacID,0);
    auto rx=install(d,0,batch);unsigned beforeCopies=copies;interrupt(d,1);
    assert((events.order==std::vector<std::string>{"start","rx","tx","end"}));
    assert(events.tx.size()==1 && events.tx[0].first==71 && events.tx[0].second);
    assert(d.awdlRadio.dwellStarts==1 && d.awdlRadio.dwellEnds==1 && d.awdlRadio.completed==1);
    assert(d.awdlRadio.session.received==2 && d.awdlRadio.dwell.done && !d.awdlRadio.dwell.running);
    assert(d.com.sc_flags&IWX_FLAG_TE_ACTIVE); // AWDL notifications do not end AP protection.
    assert(command->frees==1 && tx->frees==1 && rx->frees==1 && copies==beforeCopies+1);
    assert(d.refills==1 && d.com.rxq.data[0].m!=rx && d.com.rxq.data[0].m->frees==0);
    assert(d.com.txq[0].queued==0 && d.com.txq[2].queued==0 && d.com.txq[2].tail==1);
    assert(!d.com.txq[2].data[0].m && !d.com.txq[2].data[0].awdlToken && !d.com.txq[2].data[0].flags);
    assert(!d.com.txq[2].desc[0].num_tbs && !d.com.txq[2].desc[0].tbs[1].tb_len);
    assert(d.com.txq[2].desc[0].tbs[0].tb_len==htole16(8));
    assert(d.gate.count(&d.com.txq[0].desc[3])==1 && d.gate.count(&d.awdlRadio.dwell.done)==1);
    auto ack=static_cast<iwx_rx_packet *>(d.com.sc_cmd_resp_pkt[3]);
    assert(ack->hdr.cmd==IWX_SESSION_PROTECTION_CMD && ack->hdr.group_id==IWX_MAC_CONF_GROUP);
    assert(iwx_rx_packet_payload_len(ack)==4 && d.com.rxq.cur==1 && d.com.sc_ic.ic_if.inputs==1);
    assert(d.maps[0]->reads==1 && d.com.doorbells==std::vector<unsigned>{0});
}

static void coexistenceBatch() {
    ItlIwx d;Events events(d);d.btCoex.enabled=true;
    auto command=queueCommand(d,9,true);
    auto rlc=queueCommand(d,10,true);
    auto txp=queueCommand(d,11,true);
    iwx_bt_coex_profile_notif profile={};profile.bt_activity_grading=htole32(3);
    profile.bt_ci_compliance=htole32(1);profile.primary_ch_lut=htole32(2);
    profile.ttc_status=1;profile.rrc_status=2;
    Batch batch;batch.session(RadioState::MacID,1);
    batch.add(IWX_LEGACY_GROUP,IWX_BT_PROFILE_NOTIFICATION,0x80,0,&profile,sizeof(profile));
    batch.add(IWX_LONG_GROUP,IWX_BT_COEX_CI,IWX_DQA_CMD_QUEUE,9,nullptr,0);
    batch.add(IWX_DATA_PATH_GROUP,IWX_RLC_CONFIG_CMD,IWX_DQA_CMD_QUEUE,10,nullptr,0);
    batch.add(IWX_LONG_GROUP,IWX_BT_COEX_UPDATE_REDUCED_TXP,IWX_DQA_CMD_QUEUE,11,nullptr,0);
    batch.add(IWX_LEGACY_GROUP,IWX_BT_PROFILE_NOTIFICATION,0x80,0,&profile,sizeof(profile)-1);
    batch.session(RadioState::MacID,0);
    auto rx=install(d,0,batch);interrupt(d,1);
    assert(command->frees==1 && rlc->frees==1 && txp->frees==1 && d.com.txq[0].queued==0);
    assert(!rx->frees && d.com.rxq.data[0].m==rx); // Control-only batches retain the RX ring buffer.
    assert(d.btCoex.notifications==1 && d.btCoex.malformed==1 && d.btCoex.activity==3);
    assert(d.btCoex.compliance==1 && d.btCoex.primaryLUT==2 && d.btCoex.ttc==1 && d.btCoex.rrc==2);
    assert(d.gate.count(&d.com.txq[0].desc[9])==1 && d.awdlRadio.dwellEnds==1);
    assert((events.order==std::vector<std::string>{"start","end"}));
}

static void cancellationAndIsolation() {
    ItlIwx d;Events events(d);iwx_node ap;d.com.sc_ic.ic_bss=&ap.in_ni;
    d.com.sc_enabled_capa[IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD/8]|=1U<<(IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD%8);
    d.awdlRadio.dwell.cancelling=true;
    auto command=queueCommand(d,7,true);auto tx0=queueTX(d,0,101);auto tx1=queueTX(d,1,102);
    Batch first;first.ack(7,true);first.session(0,1,1,IWX_SESSION_PROTECT_CONF_ASSOC);
    first.session(RadioState::MacID,1,1,IWX_SESSION_PROTECT_CONF_ASSOC);
    first.session(RadioState::MacID,1);first.tx(3); // Invalid SSN cannot relinquish queued TX.
    install(d,0,first);interrupt(d,1);
    assert(command->frees==1 && d.com.txq[0].queued==0 && !d.awdlRadio.dwell.running && !d.awdlRadio.dwell.done);
    assert(d.com.sc_flags&IWX_FLAG_TE_ACTIVE);
    assert(events.tx.empty() && !tx0->frees && !tx1->frees && d.com.txq[2].queued==2);
    Batch second;second.tx(2,IWX_TX_STATUS_FAIL_SHORT_LIMIT);
    second.session(RadioState::MacID,0,0,IWX_SESSION_PROTECT_CONF_ASSOC);
    second.session(RadioState::MacID,0);second.tx(2);second.ack(7,true);
    install(d,1,second);interrupt(d,2);
    assert(events.tx.size()==2 && events.tx[0]==std::make_pair(uint64_t(101),false));
    assert(events.tx[1]==std::make_pair(uint64_t(102),true));
    assert(tx0->frees==1 && tx1->frees==1 && d.com.txq[2].queued==0);
    assert(d.awdlRadio.failed==1 && d.awdlRadio.completed==1 && d.awdlRadio.dwellEnds==1);
    assert(d.awdlRadio.session.otherMAC==1 && d.awdlRadio.session.unexpectedStart==1 && d.awdlRadio.session.inactive==1);
    assert(d.awdlRadio.session.lastEndConfig==IWX_SESSION_PROTECT_CONF_ASSOC && d.com.sc_flags&IWX_FLAG_TE_ACTIVE);
    assert(d.gate.count(&d.awdlRadio.dwell.done)==1 && d.gate.count(&d.com.txq[0].desc[7])==1);
}

static void receiveOwnershipAndFailures() {
    ItlIwx d;Events events(d);
    Batch first;first.session(RadioState::MacID,1);first.action(false);first.session(RadioState::MacID,0);
    auto badCRC=install(d,0,first);Batch last;last.action();auto finalFrame=install(d,1,last);
    unsigned beforeCopies=copies;interrupt(d,2);
    assert((events.order==std::vector<std::string>{"start","end","rx"}));
    assert(d.awdlReceive.valid==1 && d.refills==2 && badCRC->frees==1 && finalFrame->frees==1);
    assert(copies==beforeCopies+1); // Non-final CRC packet is a clone; final MPDU transfers original.

    ItlIwx read;Events readEvents(read);auto m=install(read,0,first,true);interrupt(read,1);
    assert(read.com.sc_ic.ic_if.stats.inputErrors==1 && read.awdlRadio.session.received==0 && !m->frees);
    assert(read.com.rxq.cur==1 && read.maps[0]->reads==1);

}

static Batch allocationBatch() {
    Batch b;b.session(RadioState::MacID,1);b.action();b.action();
    b.session(RadioState::MacID,0);b.ack(5);b.tx(1);return b;
}

static void checkControlAfterAllocationFailure(ItlIwx &d,Events &events,mbuf_t command,mbuf_t tx,bool recovered) {
    assert(d.awdlRadio.dwell.done && "allocation failure must not discard the following session END");
    assert(d.awdlRadio.dwellStarts==1 && d.awdlRadio.dwellEnds==1 && !d.awdlRadio.dwell.running);
    assert(command->frees==1 && tx->frees==1 && d.com.txq[0].queued==0 && d.com.txq[2].queued==0);
    assert(!d.com.txq[2].data[0].m && !d.com.txq[2].data[0].awdlToken && !d.com.txq[2].desc[0].num_tbs);
    assert(d.gate.count(&d.com.txq[0].desc[5])==1 && d.gate.count(&d.awdlRadio.dwell.done)==1);
    assert(events.tx.size()==1 && events.tx[0]==std::make_pair(uint64_t(401),true));
    const std::vector<std::string> expected=recovered?
        std::vector<std::string>{"start","rx","end","tx"}:std::vector<std::string>{"start","end","tx"};
    assert(events.order==expected && d.com.rxq.cur==1 && d.com.sc_ic.ic_if.inputs==1);
    assert(d.com.doorbells==std::vector<unsigned>{0});
}

static void cloneAllocationFailure() {
    ItlIwx d;Events events(d);auto command=queueCommand(d,5);auto tx=queueTX(d,0,401);
    auto rx=install(d,0,allocationBatch());unsigned beforeCopies=copies;
    failCopy=true;interrupt(d,1);failCopy=false;
    checkControlAfterAllocationFailure(d,events,command,tx,false);
    assert(d.com.sc_ic.ic_if.stats.inputErrors==2 && d.awdlReceive.valid==0 && copies==beforeCopies);
    assert(d.refillAttempts==1 && d.refills==1 && rx->frees==1);
    assert(d.com.rxq.data[0].m!=rx && !d.com.rxq.data[0].m->frees);
}

static void refillAllocationFailure() {
    ItlIwx d;Events events(d);auto command=queueCommand(d,5);auto tx=queueTX(d,0,401);
    d.failRefill=true;auto rx=install(d,0,allocationBatch());unsigned beforeCopies=copies;interrupt(d,1);
    checkControlAfterAllocationFailure(d,events,command,tx,false);
    assert(d.com.sc_ic.ic_if.stats.inputErrors==2 && d.awdlReceive.valid==0 && copies==beforeCopies);
    assert(d.refillAttempts==2 && d.refills==0 && d.com.rxq.data[0].m==rx && !rx->frees);
    assert(rx->offset==0 && rx->length==IWX_RBUF_SIZE && rx->total==IWX_RBUF_SIZE);
}

static void refillAllocationRecovery() {
    ItlIwx d;Events events(d);auto command=queueCommand(d,5);auto tx=queueTX(d,0,401);
    d.refillFailuresRemaining=1;auto rx=install(d,0,allocationBatch());unsigned beforeCopies=copies;interrupt(d,1);
    checkControlAfterAllocationFailure(d,events,command,tx,true);
    assert(d.com.sc_ic.ic_if.stats.inputErrors==1 && d.awdlReceive.valid==1 && copies==beforeCopies+1);
    assert(d.refillAttempts==2 && d.refills==1 && rx->frees==1);
    assert(d.com.rxq.data[0].m!=rx && !d.com.rxq.data[0].m->frees);
}

static void finalMPDURecoveryAndAX210Boundary() {
    for(bool failRefill:{false,true}) {
        ItlIwx d;Events events(d);d.refillFailuresRemaining=failRefill?1:0;
        Batch b;b.session(RadioState::MacID,1);b.action();b.action();
        auto rx=install(d,0,b);unsigned beforeCopies=copies;
        failCopy=!failRefill;interrupt(d,1);failCopy=false;
        assert(d.awdlRadio.dwellStarts==1 && d.awdlReceive.valid==1 && d.com.sc_ic.ic_if.stats.inputErrors==1);
        assert((events.order==std::vector<std::string>{"start","rx"}));
        assert(d.refillAttempts==(failRefill?2U:1U) && d.refills==1 && copies==beforeCopies);
        assert(rx->frees==1 && d.com.rxq.data[0].m!=rx && !d.com.rxq.data[0].m->frees);
        // The final MPDU hands off the original buffer, even after an earlier allocation failure.
        Batch end;end.session(RadioState::MacID,0);install(d,1,end);interrupt(d,2);
        assert(d.awdlRadio.dwellEnds==1 && d.gate.count(&d.awdlRadio.dwell.done)==1 && d.com.rxq.cur==2);
    }

    ItlIwx ax;Events events(ax);ax.com.sc_device_family=IWX_DEVICE_FAMILY_AX210;
    Batch first;first.session(RadioState::MacID,1);first.session(RadioState::MacID,0);
    auto rx=install(ax,0,first);interrupt(ax,1);
    assert(ax.awdlRadio.dwellStarts==1 && ax.awdlRadio.dwellEnds==0 && ax.awdlRadio.session.received==1);
    assert(!rx->frees && ax.com.rxq.data[0].m==rx);
    Batch second;second.session(RadioState::MacID,0);install(ax,1,second);interrupt(ax,2);
    assert(ax.awdlRadio.dwellEnds==1 && ax.awdlRadio.session.received==2);
}

static void malformedMPDUEnvelope(bool truncatedDescriptor) {
    ItlIwx d;Events events(d);auto command=queueCommand(d,5);auto tx=queueTX(d,0,401);
    Batch b;b.session(RadioState::MacID,1);
    iwx_rx_mpdu_desc desc={};
    desc.status=htole16(IWX_RX_MPDU_RES_STATUS_CRC_OK|IWX_RX_MPDU_RES_STATUS_OVERRUN_OK);
    desc.mpdu_len=htole16(truncatedDescriptor?24:26);
    desc.mac_flags2=truncatedDescriptor?0:IWX_RX_MPDU_MFLG2_PAD;
    const size_t descriptorLength=truncatedDescriptor?IWX_RX_DESC_SIZE_V1-1:IWX_RX_DESC_SIZE_V1;
    std::vector<uint8_t> payload(descriptorLength+(truncatedDescriptor?0:8),0);
    std::memcpy(payload.data(),&desc,descriptorLength);
    if(!truncatedDescriptor)payload[descriptorLength]=0xd0;
    b.add(IWX_LEGACY_GROUP,IWX_REPLY_RX_MPDU_CMD,0x80,0,payload.data(),payload.size());
    // The padded frame claims a 26-byte MPDU but supplies only eight bytes.
    // Its header memmove would otherwise overwrite the END at the next 64-byte boundary.
    b.session(RadioState::MacID,0);b.ack(5);b.tx(1);
    auto rx=install(d,0,b);interrupt(d,1);
    checkControlAfterAllocationFailure(d,events,command,tx,false);
    assert(d.com.sc_ic.ic_if.stats.inputErrors==1 && "the current notification must bound the descriptor and MPDU");
    assert(d.awdlReceive.valid==0 && d.awdlReceive.malformed==0 && d.forwarded==0 && d.crypto==0 && d.duplicates==0);
    assert(d.refills==1 && rx->frees==1 && d.com.rxq.data[0].m!=rx && !d.com.rxq.data[0].m->frees);
}

static void truncatedMPDUDescriptor() {malformedMPDUEnvelope(true);}
static void mpduEnvelopeOverlap() {malformedMPDUEnvelope(false);}

static void failedStartAndMalformed() {
    ItlIwx d;Events events(d);Batch first;
    first.session(RadioState::MacID,1,0);first.session(RadioState::MacID,1);
    uint32_t shortPayload=0;
    first.add(IWX_MAC_CONF_GROUP,IWX_SESSION_PROTECTION_NOTIF,0x80,0,&shortPayload,sizeof(shortPayload));
    install(d,0,first);interrupt(d,1);
    assert(d.awdlRadio.dwell.failed && !d.awdlRadio.dwell.running && !d.awdlRadio.dwell.done);
    assert(d.awdlRadio.dwellFailures==1 && d.awdlRadio.dwellStarts==0 && d.awdlRadio.session.malformed==1);
    Batch end;end.session(RadioState::MacID,0);install(d,1,end);interrupt(d,2);
    assert(d.awdlRadio.dwell.done && d.awdlRadio.dwellEnds==1 && d.gate.count(&d.awdlRadio.dwell.done)==1);

    ItlIwx invalid;Events invalidEvents(invalid);Batch bad;bad.session(RadioState::MacID,0);
    reinterpret_cast<iwx_rx_packet *>(bad.bytes.data())->len_n_flags=htole32(IWX_RBUF_SIZE+1);
    install(invalid,0,bad);interrupt(invalid,1);
    assert(!invalid.awdlRadio.dwell.done && invalid.awdlRadio.session.received==0);
}

static void ringWrapAndNotificationOrigin() {
    ItlIwx d;Events events(d);
    d.com.rxq.cur=IWX_RX_MQ_RING_COUNT-1;d.com.txq[2].cur=255;d.com.txq[2].tail=63;
    auto last=queueTX(d,63,201);auto first=queueTX(d,0,202);auto command=queueCommand(d,4);
    Batch before;before.session(RadioState::MacID,1);
    uint32_t status=htole32(1);
    before.add(IWX_MAC_CONF_GROUP,IWX_SESSION_PROTECTION_CMD,0x80,4,&status,sizeof(status));
    // Unsolicited firmware traffic cannot acknowledge the pending command slot.
    install(d,IWX_RX_MQ_RING_COUNT-1,before);interrupt(d,0);
    assert(d.com.rxq.cur==0 && d.com.txq[0].queued==1 && !command->frees);
    assert(d.gate.count(&d.com.txq[0].desc[4])==0 && d.com.doorbells.back()==504);
    Batch after;after.tx(1,IWX_TX_STATUS_DIRECT_DONE);after.session(RadioState::MacID,0);after.ack(4);
    install(d,0,after);interrupt(d,1);
    assert(last->frees==1 && first->frees==1 && command->frees==1);
    assert(d.com.txq[2].tail==1 && d.com.txq[2].queued==0 && d.awdlRadio.completed==2);
    assert((events.tx==std::vector<std::pair<uint64_t,bool>>{{201,true},{202,true}}));
    assert(d.awdlRadio.dwell.done && d.com.rxq.cur==1 && d.com.doorbells.back()==0);

    ItlIwx truncated;Events truncatedEvents(truncated);auto pending=queueTX(truncated,0,301);
    Batch shortTX;iwx_tx_resp response={};response.frame_count=1;response.tx_queue=htole16(2);
    response.status.status=htole16(IWX_TX_STATUS_SUCCESS);
    shortTX.add(IWX_LEGACY_GROUP,IWX_TX_CMD,2,0,&response,sizeof(response)); // Missing trailing SSN.
    shortTX.session(RadioState::MacID,0);install(truncated,0,shortTX);interrupt(truncated,1);
    assert(!pending->frees && truncated.com.txq[2].queued==1 && truncatedEvents.tx.empty());
    assert(truncated.awdlRadio.dwell.done); // A short TX payload must not hide the following END.
}

static void concurrentAPData() {
    ItlIwx d;Events events(d);iwx_node ap;d.com.sc_ic.ic_bss=&ap.in_ni;
    const uint8_t bssid[]={2,4,6,8,10,12},foreign[]={2,4,6,8,10,14},broadcast[]={255,255,255,255,255,255};
    AirportAWDL::copyAddress(ap.in_ni.ni_bssid,bssid);
    Batch b;b.session(RadioState::MacID,1);b.action();
    b.data(d.com.sc_ic.ic_myaddr,foreign); // Peer/foreign traffic cannot enter the AP state machine.
    b.data(foreign,bssid); // Even the correct AP's frame belongs to a different station.
    b.data(d.com.sc_ic.ic_myaddr,bssid);b.data(broadcast,bssid);b.session(RadioState::MacID,0);
    auto rx=install(d,0,b);unsigned beforeCopies=copies;interrupt(d,1);
    assert(d.awdlNonAPDataDropped==2 && d.forwarded==2 && d.crypto==2 && d.duplicates==2);
    assert(d.awdlReceive.valid==1 && d.awdlRadio.dwellEnds==1 && d.refills==1 && rx->frees==1);
    assert(copies==beforeCopies+5); // All five MPDUs share one detached RX buffer before END.
}

static void controlFrameIsolation() {
    for(bool enabled:{false,true}) {
        ItlIwx d;Events events(d);iwx_node ap;d.com.sc_ic.ic_bss=&ap.in_ni;
        d.awdlRadio.mac=enabled;
        const uint8_t bssid[]={2,4,6,8,10,12},peer[]={2,4,6,8,10,14},awdl[]={2,4,6,8,10,16};
        AirportAWDL::copyAddress(ap.in_ni.ni_bssid,bssid);
        Batch batch;
        batch.control(awdl,peer);batch.control(d.com.sc_ic.ic_myaddr,peer);
        batch.control(awdl,bssid);batch.control(d.com.sc_ic.ic_myaddr,bssid);
        batch.control(awdl,peer,20);batch.session(RadioState::MacID,0);
        auto rx=install(d,0,batch);interrupt(d,1);
        assert(d.com.sc_ic.ic_stats.is_rx_wrongbss==(enabled?3:0));
        assert(d.forwarded==(enabled?1U:4U) && d.crypto==d.forwarded && d.duplicates==d.forwarded);
        assert(d.com.sc_ic.ic_stats.is_rx_tooshort==1 && !d.awdlNonAPDataDropped);
        assert(d.awdlRadio.dwell.done && rx->frees==1 && d.refills==1);
    }
    ItlIwx d;Events events(d);Batch batch;
    batch.control(d.com.sc_ic.ic_myaddr,d.awdlRadio.address);install(d,0,batch);interrupt(d,1);
    assert(d.com.sc_ic.ic_stats.is_rx_wrongbss==1 && !d.forwarded && !d.crypto && !d.duplicates);
}

static void barReleaseOwnership() {
    for(unsigned scenario=0;scenario<6;++scenario) {
        ItlIwx d;Events events(d);Batch batch;
        auto &ba=d.com.sc_rxba_data[2];ba.baid=2;ba.sta_id=4;ba.tid=3;
        ba.reorder_buf.removed=0;ba.reorder_buf.buf_size=64;
        if(scenario==1)ba.baid=IWX_RX_REORDER_DATA_INVALID_BAID;
        if(scenario==2)ba.reorder_buf.removed=1;
        if(scenario==3)ba.reorder_buf.buf_size=0;
        if(scenario==4)ba.sta_id=0;
        if(scenario==5)ba.tid=1;
        iwx_bar_frame_release release={};release.sta_tid=htole32((4<<4)|3);
        release.ba_info=htole32((2<<24)|301);
        batch.add(IWX_LEGACY_GROUP,IWX_BAR_FRAME_RELEASE,0x80,0,&release,sizeof(release));
        install(d,0,batch);interrupt(d,1);
        assert(d.barReleases==(scenario==0?1U:0U));
        if(!scenario)assert(d.lastBarNSSN==301);
    }
}


static void peerManagementBSSID() {
    using namespace AirportAWDL;
    for(unsigned scenario=0;scenario<10;++scenario) {
        ItlIwx d;Events events(d);iwx_node ap;d.com.sc_ic.ic_bss=&ap.in_ni;
        const uint8_t peer[]={2,4,6,8,10,14},bssid[]={2,4,6,8,10,12};
        copyAddress(ap.in_ni.ni_bssid,bssid);
        auto &tx=d.awdlRadio.blockAckTX;tx.configured=true;tx.epoch=d.dmaEpoch;tx.cookie=9;
        tx.phase=TXBlockAckQueue::Asking;tx.dialog=19;tx.tid=0;tx.attempts=3;copyAddress(tx.peer,peer);
        BlockAckRequest request={};copyAddress(request.peer,d.awdlRadio.address);
        request.dialog=tx.dialog;request.tid=tx.tid;request.immediate=true;request.window=64;
        uint8_t reply[33];blockAckResponse(reply,peer,request,0,true);
        if(scenario!=0)copyAddress(reply+16,bssid);
        if(scenario==2)reply[26]++;
        if(scenario==3)reply[29]|=4;
        if(scenario==4)reply[15]^=2;
        if(scenario==5)reply[9]^=2;
        if(scenario==6)copyAddress(reply+10,bssid);
        if(scenario==7)reply[1]|=0x40;
        if(scenario==8)putLE16(reply+27,37);
        if(scenario==9)reply[25]=0;
        std::vector<uint8_t> payload(IWX_RX_DESC_SIZE_V1+sizeof(reply),0);
        auto desc=reinterpret_cast<iwx_rx_mpdu_desc *>(payload.data());
        desc->status=htole32(IWX_RX_MPDU_RES_STATUS_CRC_OK|IWX_RX_MPDU_RES_STATUS_OVERRUN_OK);
        desc->mpdu_len=htole16(sizeof(reply));desc->v1.channel=6;
        memcpy(payload.data()+IWX_RX_DESC_SIZE_V1,reply,sizeof(reply));
        Batch batch;batch.add(IWX_LEGACY_GROUP,IWX_REPLY_RX_MPDU_CMD,0x80,0,payload.data(),payload.size());
        auto rx=install(d,0,batch);interrupt(d,1);
        const bool parsed=scenario<4 || scenario==8;
        assert(d.awdlRadio.blockAckTXStats.responses==unsigned(parsed));
        assert(d.awdlRadio.blockAckTXStats.responsesOtherBSSID==unsigned(parsed&&scenario!=0));
        assert((tx.phase==TXBlockAckQueue::Enabling)==(scenario<2));
        if(scenario==2 || scenario==3)assert(d.awdlRadio.blockAckTXStats.unmatched==1);
        if(scenario==8)assert(d.awdlRadio.blockAckTXStats.refusals==1 && !tx.attempts);
        if(parsed)assert(!d.crypto && !d.duplicates && !d.forwarded);
        if(scenario==6)assert(d.forwarded==1 && d.crypto==1 && d.duplicates==1);
        assert(rx->frees==1);
    }
}

static void peerDELBAAcrossBSSID() {
 using namespace AirportAWDL;
 for(unsigned scenario=0;scenario<8;++scenario) {
  ItlIwx d;Events events(d);iwx_node ap;d.com.sc_ic.ic_bss=&ap.in_ni;
  const uint8_t peer[]={2,4,6,8,10,14},bssid[]={2,4,6,8,10,12};copyAddress(ap.in_ni.ni_bssid,bssid);
  auto &sessionPeer=d.awdlBA.peers[0];sessionPeer.station=true;sessionPeer.epoch=d.dmaEpoch;
  copyAddress(sessionPeer.address,peer);sessionPeer.sessions[2].firmware=true;
  d.awdlBlockAckEndInput=[](void *p,const BlockAckRequest &r){assert(r.teardown);static_cast<Events *>(p)->order.push_back("delba");};
  BlockAckRequest request={};request.tid=2;copyAddress(request.peer,d.awdlRadio.address);
  uint8_t frame[30];blockAckTeardown(frame,peer,request,0);putLE16(frame+26,0x2800);copyAddress(frame+16,bssid);
  if(scenario==1)sessionPeer.sessions[2].firmware=false;
  if(scenario==2)frame[15]^=2;
  if(scenario==3)--sessionPeer.epoch;
  if(scenario==4)putLE16(frame+26,0x3800);
  if(scenario==5)copyAddress(frame+10,bssid);
  if(scenario==6)frame[1]|=0x40;
  if(scenario==7){copyAddress(frame+16,BSSID);sessionPeer.station=false;}
  std::vector<uint8_t> payload(IWX_RX_DESC_SIZE_V1+sizeof(frame),0);
  auto desc=reinterpret_cast<iwx_rx_mpdu_desc *>(payload.data());
  desc->status=htole32(IWX_RX_MPDU_RES_STATUS_CRC_OK|IWX_RX_MPDU_RES_STATUS_OVERRUN_OK);
  desc->mpdu_len=htole16(sizeof(frame));desc->v1.channel=6;
  memcpy(payload.data()+IWX_RX_DESC_SIZE_V1,frame,sizeof(frame));
  Batch batch;batch.add(IWX_LEGACY_GROUP,IWX_REPLY_RX_MPDU_CMD,0x80,0,payload.data(),payload.size());
  auto rx=install(d,0,batch);interrupt(d,1);
  assert(sessionPeer.removeTids==unsigned(scenario==0?4:0));
  assert(events.order.size()==unsigned(scenario==0||scenario==7));
  if(scenario==0||scenario==7)assert(!d.forwarded&&!d.crypto&&!d.duplicates);
  if(scenario==5)assert(d.forwarded==1&&d.crypto==1&&d.duplicates==1);
  assert(rx->frees==1);
 }
}

int main() {
    static_assert(sizeof(iwx_rx_packet)==8,"real firmware header");
    static_assert(sizeof(iwx_session_prot_notif)==16,"session notification ABI");
    static_assert(IWX_RX_DESC_SIZE_V1==48 && sizeof(iwx_rx_mpdu_desc)==64,"RX descriptor ABI");
    // TEST ENTRY
}
