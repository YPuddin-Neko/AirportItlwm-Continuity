#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <utility>
#include <cstdio>
#include "itlwm/hal_iwx/BTCoex.hpp"
#include "include/HAL/ItlTx.hpp"
#include "AirportItlwm/AWDL/RadioState.hpp"
#include "AirportItlwm/AWDL/PeerState.hpp"
static uint64_t awdlRadioNow() { return 1000000; }
#define AIRPORT_WCL 1
#define __packed __attribute__((packed))
#define le16toh(x) (x)
#define le16_to_cpu(x) (x)
#define le32toh(x) (x)
#define IC2IFP(ic) (&(ic)->ic_if)
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define DPRINTFN(n,x) ((void)0)
#define KASSERT(v,msg) assert(v)
#define IWX_AGG_SSN_TO_TXQ_IDX(x,a) ((x)&((a)-1))
#define bus_dmamap_sync(...) ((void)0)
#define bus_dmamap_destroy(...) ((void)0)
enum {IWX_TX_STATUS_SUCCESS=1,IWX_TX_STATUS_DIRECT_DONE=2,IWX_TX_STATUS_MSK=255,
      IWX_TX_STATUS_FAIL_FIFO_FLUSHED=0x8e,IWX_TX_STATUS_FAIL_DRAIN_FLOW=0x85,
      IEEE80211_FC0_TYPE_DATA=8,IEEE80211_FC0_TYPE_MGT=0,IWX_DQA_CMD_QUEUE=0,IWX_INVALID_QUEUE=0xffff};
struct task {unsigned queued=0;};
static void *systq=nullptr;
static void task_add(void *,task *t){++t->queued;}
struct Packet {unsigned frees=0;};
using mbuf_t=Packet*;using bus_dmamap_t=void*;using bus_addr_t=uint64_t;
static void mbuf_freem(mbuf_t p) {assert(p && p->frees++==0);}
struct ieee80211_node {unsigned releases=0;};
struct iwx_node {ieee80211_node in_ni;};
// NATIVE DECLARATIONS
struct Statistics {unsigned outputErrors=0;};
struct _ifnet {Statistics stats;Statistics *netStat=&stats;};
enum {IEEE80211_S_RUN=5};
struct ieee80211com {_ifnet ic_if;unsigned ic_state=IEEE80211_S_RUN;};
static void ieee80211_release_node(ieee80211com *,ieee80211_node *n) {++n->releases;}
struct Memory {char bytes[64]={};void *vaddr=bytes;size_t size=sizeof(bytes);};
struct iwx_tx_ring {
    iwx_tx_data data[8]={};unsigned ring_count=8;int tail=0,cur=0,queued=0,qid=1;
    Memory desc_dma,cmd_dma,bc_tbl;unsigned hi_mark=0,low_mark=0;char descriptor[64]={};char *desc=descriptor;
};
struct iwx_softc {
    ieee80211com sc_ic;unsigned sc_flags=0;task bt_coex_task;void *sc_dmat=nullptr; iwx_tx_ring txq[2];unsigned sc_tx_timer=0;uint8_t sc_mgmt_last_antenna_idx=0;
    void *sc_cmd_resp_pkt[8]={};size_t sc_cmd_resp_len[8]={};ItlQueueMask<2> qfullmsk={};
};
struct iwx_rx_packet {size_t length=0;alignas(8) uint8_t data[128]={};};
struct iwx_rx_data {void *map=nullptr;};
static size_t iwx_rx_packet_payload_len(const iwx_rx_packet *p) {return p->length;}
struct Gate {void commandWakeup(void *) {}};
struct ItlIwx {
    uint64_t dmaEpoch=1;
    unsigned getTxQueueSize(){return 256;}
    IwxBTCoexState btCoex={};
    iwx_softc com;AirportAWDL::RadioState awdlRadio={};Gate gate;
    AirportAWDL::PeerTable awdlPeers={};
    void *awdlReceiver=this;
    void (*awdlDataComplete)(void *,uint64_t,int)=nullptr;
    void (*awdlTxComplete)(void *,uint64_t,bool)=[](void *self,uint64_t token,bool ack) {
        auto d=static_cast<ItlIwx *>(self);d->results.push_back({token,ack});
    };
    std::vector<std::pair<uint64_t,bool>> results;
    unsigned cleared=0,antennas=0;
    Gate *getMainCommandGate() {return &gate;}
    void iwx_bt_coex_complete_tx(iwx_softc *,iwx_tx_data *,bool);
    void iwx_rx_tx_ba_notif(iwx_softc *,iwx_rx_packet *,iwx_rx_data *);
    void iwx_free_tx_ring(iwx_softc *,iwx_tx_ring *);
    void iwx_dma_contig_free(Memory *){}
    void completeAWDLTx(iwx_tx_data *,bool,bool=false);
    void sampleAWDLRate(iwx_softc *,iwx_rx_packet *,iwx_tx_data *d) {assert(!d->awdlRatePeer);}
    void iwx_rx_tx_cmd_single(iwx_softc *,iwx_rx_packet *,iwx_tx_data *);
    void iwx_rx_tx_cmd(iwx_softc *,iwx_rx_packet *,iwx_rx_data *);
    void iwx_txd_done(iwx_softc *,iwx_tx_data *);
    void iwx_ampdu_txq_advance(iwx_softc *,iwx_tx_ring *,int);
    void iwx_reset_tx_ring(iwx_softc *,iwx_tx_ring *);
    void iwx_toggle_tx_ant(iwx_softc *,uint8_t *) {++antennas;}
    void iwx_clear_tx_desc(iwx_softc *,iwx_tx_ring *,int) {++cleared;}
    void iwx_clear_oactive(iwx_softc *,iwx_tx_ring *) {}
    void response(unsigned status,unsigned next,unsigned count=1,size_t length=sizeof(iwx_tx_resp)+4) {
        iwx_rx_packet p;p.length=length;auto t=reinterpret_cast<iwx_tx_resp *>(p.data);
        t->frame_count=count;t->tx_queue=1;t->status.status=status;
        memcpy(&t->status+1,&next,4);iwx_rx_data rx;
        iwx_rx_tx_cmd(&com,&p,&rx);
    }
};
// PRODUCTION
static void coexCompletionContract() {
    for(unsigned status:{1u,2u,0x82u})for(unsigned position=0;position<3;++position) {
        ItlIwx d;d.btCoex.enabled=true;d.btCoex.policy.resetLink(true,true,2);
        d.btCoex.policy.request(IwxBTPolicy::SMPS::Static,false);IwxBTPolicy::SMPS mode;
        const auto token=d.btCoex.policy.beginAction(mode);
        auto &r=d.com.txq[1];r.tail=7;r.cur=2;r.queued=3;Packet packets[3];iwx_node nodes[3];
        const unsigned slots[]={7,0,1};
        for(unsigned n=0;n<3;++n){auto &entry=r.data[slots[n]];entry.m=&packets[n];entry.in=&nodes[n];entry.type=0;
            if(n==position){entry.flags=IWX_TXDATA_FLAG_COEX;entry.coexToken=token;}}
        d.response(status,2);
        assert(d.btCoex.policy.actionAcks==unsigned(position>0||status==1));
        assert(d.btCoex.policy.actionFailures==unsigned(position==0&&status!=1));
        for(unsigned n=0;n<3;++n)assert(packets[n].frees==1&&nodes[n].in_ni.releases==1);
        auto acks=d.btCoex.policy.actionAcks;d.response(1,2);assert(d.btCoex.policy.actionAcks==acks);
    }
    for(bool resetEpoch:{false,true}) {
        ItlIwx d;d.btCoex.enabled=true;d.btCoex.policy.resetLink(true,true,2);
        d.btCoex.policy.request(IwxBTPolicy::SMPS::Static,false);IwxBTPolicy::SMPS mode;
        auto token=d.btCoex.policy.beginAction(mode);
        auto &r=d.com.txq[1];r.queued=1;r.cur=1;Packet p;iwx_node node;
        r.data[0].m=&p;r.data[0].in=&node;r.data[0].flags=IWX_TXDATA_FLAG_COEX;r.data[0].coexToken=token;
        if(resetEpoch)d.btCoex.reset();
        d.iwx_reset_tx_ring(&d.com,&r);
        assert(p.frees==1&&node.in_ni.releases==1&&r.data[0].in==nullptr&&r.data[0].coexToken==0);
        assert(d.btCoex.policy.actionFailures==unsigned(!resetEpoch)&&!d.btCoex.policy.actionAcks);
        d.iwx_free_tx_ring(&d.com,&r);assert(node.in_ni.releases==1);
    }
    // Free without a prior reset must also release the AP node exactly once.
    {
        ItlIwx d;d.btCoex.enabled=true;d.btCoex.policy.resetLink(true,true,2);
        d.btCoex.policy.request(IwxBTPolicy::SMPS::Dynamic,false);IwxBTPolicy::SMPS mode;
        auto &r=d.com.txq[1];Packet p;iwx_node node;
        r.data[0].m=&p;r.data[0].in=&node;r.data[0].flags=IWX_TXDATA_FLAG_COEX;
        r.data[0].coexToken=d.btCoex.policy.beginAction(mode);
        d.iwx_free_tx_ring(&d.com,&r);assert(p.frees==1&&node.in_ni.releases==1);
        assert(d.btCoex.policy.actionFailures==1);
    }
    // A compressed BA reclaim is a successful completion; a flush reclaim is not.
    {
        ItlIwx d;d.btCoex.enabled=true;d.btCoex.policy.resetLink(true,true,2);
        d.btCoex.policy.request(IwxBTPolicy::SMPS::Static,false);IwxBTPolicy::SMPS mode;
        auto &r=d.com.txq[1];r.cur=1;r.queued=1;Packet p;iwx_node node;
        r.data[0].m=&p;r.data[0].in=&node;r.data[0].flags=IWX_TXDATA_FLAG_COEX;
        r.data[0].coexToken=d.btCoex.policy.beginAction(mode);
        iwx_rx_packet pkt;pkt.length=sizeof(iwx_compressed_ba_notif)+sizeof(iwx_compressed_ba_tfd);
        auto b=reinterpret_cast<iwx_compressed_ba_notif *>(pkt.data);
        b->sta_id=IWX_STATION_ID;b->tfd_cnt=1;b->tfd[0].q_num=1;b->tfd[0].tfd_index=1;
        iwx_rx_data rx;d.iwx_rx_tx_ba_notif(&d.com,&pkt,&rx);
        assert(d.btCoex.policy.actionAcks==1&&p.frees==1&&node.in_ni.releases==1);
    }
}

int main(){coexCompletionContract();}
