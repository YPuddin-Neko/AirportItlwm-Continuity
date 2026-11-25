#include <cassert>
#include <cerrno>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <utility>
#include "include/HAL/ItlTx.hpp"
#include "AirportItlwm/AWDL/RadioState.hpp"
#include "AirportItlwm/AWDL/PeerState.hpp"
static uint64_t awdlRadioNow() { return 1000000; }
#define AIRPORT_WCL 1
#define DEBUG 1
#define __packed __attribute__((packed))
#define le16toh(x) (x)
#define le16_to_cpu(x) (x)
#define le32toh(x) (x)
#define htole16(x) (x)
#define htole32(x) static_cast<uint32_t>(x)
#define IC2IFP(ic) (&(ic)->ic_if)
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define DPRINTFN(n,x) ((void)0)
#define KASSERT(v,msg) assert(v)
#define IWX_AGG_SSN_TO_TXQ_IDX(x,a) ((x)&((a)-1))
#define bus_dmamap_sync(...) ((void)0)
enum {IWX_TX_STATUS_SUCCESS=1,IWX_TX_STATUS_DIRECT_DONE=2,IWX_TX_STATUS_MSK=255,
      IWX_TX_STATUS_FAIL_SHORT_LIMIT=0x82,IWX_TX_STATUS_FAIL_LONG_LIMIT=0x83,IWX_TX_STATUS_FAIL_BT_RETRY=0x8a,
      IWX_TX_STATUS_FAIL_FIFO_FLUSHED=0x8e,IWX_TX_STATUS_FAIL_DRAIN_FLOW=0x85,
      IEEE80211_FC0_TYPE_DATA=8,IEEE80211_FC0_TYPE_MGT=0,IWX_DQA_CMD_QUEUE=0,IWX_INVALID_QUEUE=0xffff};
struct Packet {unsigned frees=0;};
using mbuf_t=Packet*;using bus_dmamap_t=void*;using bus_addr_t=uint64_t;
static void mbuf_freem(mbuf_t p) {assert(p && p->frees++==0);}
struct ieee80211_node {unsigned releases=0;};
struct iwx_node {ieee80211_node in_ni;};
// NATIVE DECLARATIONS
struct Statistics {unsigned outputErrors=0;};
struct _ifnet {Statistics stats;Statistics *netStat=&stats;};
enum { IEEE80211_S_RUN=4, IWX_STATION_ID=0 };
struct ieee80211com {_ifnet ic_if;int ic_state=IEEE80211_S_RUN;};
static void ieee80211_release_node(ieee80211com *,ieee80211_node *n) {++n->releases;}
struct Memory {char bytes[64]={};void *vaddr=bytes;size_t size=sizeof(bytes);};
struct iwx_tx_ring {
    iwx_tx_data data[8]={};unsigned ring_count=8;int tail=0,cur=0,queued=0,qid=1;
    Memory desc_dma,bc_tbl;char descriptor[64]={};char *desc=descriptor;
};
struct iwx_softc {
    ieee80211com sc_ic; iwx_tx_ring txq[3];unsigned sc_tx_timer=0;uint8_t sc_mgmt_last_antenna_idx=0;
    void *sc_cmd_resp_pkt[8]={};size_t sc_cmd_resp_len[8]={};ItlQueueMask<3> qfullmsk={};
    struct {int qid=-1;} sc_tid_data[8];
};
struct iwx_rx_packet {size_t length=0;struct {uint8_t group_id=0;} hdr;alignas(8) uint8_t data[256]={};};
enum { IWX_TXPATH_FLUSH=0x1e,IWX_CMD_WANT_RESP=1,IWX_CMD_FAILED_MSK=0x40,IWX_MGMT_TID=15 };
struct iwx_host_cmd {
    unsigned id,len[1];const void *data[1];unsigned flags;size_t resp_pkt_len;
    iwx_rx_packet *resp_pkt;
};
struct iwx_rx_data {void *map=nullptr;};
static size_t iwx_rx_packet_payload_len(const iwx_rx_packet *p) {return p->length;}
struct Gate {void commandWakeup(void *) {}};
struct ItlIwx {
    iwx_softc com;AirportAWDL::RadioState awdlRadio={};Gate gate;
    AirportAWDL::PeerTable awdlPeers={};
    struct {
        std::vector<std::pair<unsigned,unsigned>> flushRanges;
        void response(const iwx_tx_ring &, unsigned, int, const iwx_tx_resp &, size_t) {}
        void flush(const iwx_tx_ring &, unsigned first, unsigned end) {
            if (end) flushRanges.push_back({first,end});
        }
    } awdlBroadcastReports;
    void *awdlReceiver=this;
    void (*awdlDataComplete)(void *,uint64_t,int)=[](void *self,uint64_t token,int result) {
        static_cast<ItlIwx *>(self)->dataResults.push_back({token,result});
    };
    void (*awdlTxComplete)(void *,uint64_t,bool)=[](void *self,uint64_t token,bool ack) {
        auto d=static_cast<ItlIwx *>(self);d->results.push_back({token,ack});
    };
    std::vector<std::pair<uint64_t,bool>> results;
    std::vector<std::pair<uint64_t,int>> dataResults;
    uint64_t dmaEpoch=1;
    iwx_rx_packet flushResponse;
    bool resetDuringFlush=false;
    unsigned flushStation=AirportAWDL::RadioState::StationID;
    unsigned getTxQueueSize(){return 256;}
    int iwx_send_cmd(iwx_softc *,iwx_host_cmd *cmd) {
        assert(cmd->id==IWX_TXPATH_FLUSH && cmd->len[0]==sizeof(iwx_tx_path_flush_cmd));
        auto request=static_cast<const iwx_tx_path_flush_cmd *>(cmd->data[0]);
        assert(request->sta_id==flushStation && request->tid_mask==0xffff);
        if(resetDuringFlush)++dmaEpoch;
        cmd->resp_pkt=&flushResponse;return 0;
    }
    void iwx_free_resp(iwx_softc *,iwx_host_cmd *cmd){assert(cmd->resp_pkt==&flushResponse);}
    int iwx_flush_sta_tids(iwx_softc *,int,uint16_t);
    unsigned cleared=0,antennas=0;
    Gate *getMainCommandGate() {return &gate;}
    void iwx_bt_coex_complete_tx(iwx_softc *,iwx_tx_data *d,bool){assert(!d->coexToken);}
    void completeAWDLTx(iwx_tx_data *,bool,bool=false);
    void sampleAWDLRate(iwx_softc *,iwx_rx_packet *,iwx_tx_data *d) {assert(!d->awdlRatePeer);}
    void iwx_rx_tx_cmd_single(iwx_softc *,iwx_rx_packet *,iwx_tx_data *);
    void iwx_rx_tx_cmd(iwx_softc *,iwx_rx_packet *,iwx_rx_data *);
    void iwx_rx_tx_ba_notif(iwx_softc *,iwx_rx_packet *,iwx_rx_data *);
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
static void completions() {
    for(unsigned status:{1,2,0x82}) {
        ItlIwx d;auto &r=d.com.txq[1];r.tail=7;r.cur=2;r.queued=3;
        Packet packets[3];unsigned n=0;
        for(unsigned at:{7,0,1}) {
            r.data[at].m=&packets[n];r.data[at].flags=IWX_TXDATA_FLAG_AWDL;
            r.data[at].awdlToken=++n;
        }
        d.response(status,2);
        const std::vector<std::pair<uint64_t,bool>> expected={{1,status==1||status==2},{2,true},{3,true}};
        assert(d.results==expected && !r.queued && r.tail==2 && d.cleared==3);
        assert(d.awdlRadio.completed==(status==0x82?2:3));
        assert(d.awdlRadio.failed==(status==0x82?1:0));
        for(auto &p:packets)assert(p.frees==1);
        d.response(1,2);assert(d.results==expected); // duplicate notification
    }
    ItlIwx d;auto &r=d.com.txq[1];r.queued=1;r.cur=1;
    Packet packet;r.data[0].m=&packet;r.data[0].flags=IWX_TXDATA_FLAG_AWDL;r.data[0].awdlToken=40;
    d.response(1,2);d.response(1,1,0);d.response(1,1,1,sizeof(iwx_tx_resp));
    assert(d.results.empty() && r.queued==1 && !packet.frees);
    d.iwx_reset_tx_ring(&d.com,&r);
    assert((d.results==std::vector<std::pair<uint64_t,bool>>({{40,false}})));
    assert(packet.frees==1 && !r.queued && !r.data[0].awdlToken);
    d.response(1,1);assert(d.results.size()==1); // no ACK after reset
    // A flush without an individual status completes once as failure.
    Packet flush;r.data[0].m=&flush;r.data[0].flags=IWX_TXDATA_FLAG_AWDL;r.data[0].awdlToken=41;
    d.iwx_txd_done(&d.com,&r.data[0]);assert(d.results.back()==std::make_pair(uint64_t(41),false));
    assert(!r.data[0].flags && !r.data[0].m);
    // Ordinary AP processing retains its node ownership and last-descriptor status.
    ItlIwx ap;auto &a=ap.com.txq[1];a.queued=2;a.cur=2;Packet p[2];iwx_node nodes[2];
    for(unsigned n=0;n<2;++n) {a.data[n].m=&p[n];a.data[n].in=&nodes[n];a.data[n].type=0;}
    ap.response(0x82,2);
    assert(ap.results.empty() && ap.com.sc_ic.ic_if.stats.outputErrors==1 && ap.antennas==1);
    for(unsigned n=0;n<2;++n)assert(p[n].frees==1 && nodes[n].in_ni.releases==1);
}
static void dataStatus() {
    for(unsigned status:{1,2,0x82,0x85,0x8e}) {
        ItlIwx d; auto &r=d.com.txq[1];r.cur=r.queued=2;Packet packets[2];
        for(unsigned i=0;i<2;++i) {
            auto &s=r.data[i];s.m=&packets[i];s.flags=IWX_TXDATA_FLAG_AWDL;
            s.awdlToken=10+i;s.type=IEEE80211_FC0_TYPE_DATA;
        }
        d.response(status,2);
        const int first=status<=2?0:(status==0x82?EIO:ECANCELED);
        assert((d.dataResults==std::vector<std::pair<uint64_t,int>>{{10,first},{11,0}}));
        assert(d.results.empty() && packets[0].frees==1 && packets[1].frees==1);
        d.response(status,2);assert(d.dataResults.size()==2);
    }
    for (unsigned cur : {65,0}) {
        ItlIwx d; auto &r=d.com.txq[1];r.queued=1;r.cur=cur;r.tail=(cur+7)%8;
        Packet p;auto &s=r.data[r.tail];s.m=&p;s.flags=IWX_TXDATA_FLAG_AWDL;
        s.awdlToken=77;s.type=IEEE80211_FC0_TYPE_DATA;
        d.response(0x8e,(cur+256-64)%256);
        assert(d.dataResults.empty() && !p.frees && r.queued==1 && s.awdlToken==77);
        d.response(0x85,cur);
        assert((d.dataResults==std::vector<std::pair<uint64_t,int>>{{77,ECANCELED}}));
        assert(p.frees==1 && !r.queued && !s.awdlToken);
    }
}
static void flushRange() {
    for(unsigned variant=0;variant<13;++variant) {
        ItlIwx d;auto &r=d.com.txq[1];
        // Current 256-entry TFD pointer range is [252,1); physical slots 4..7,0.
        r.cur=1;r.tail=4;r.queued=5;Packet packets[5];
        for(unsigned i=0;i<5;++i) {
            auto &s=r.data[(r.tail+i)%8];s.m=&packets[i];
            s.flags=IWX_TXDATA_FLAG_AWDL;s.awdlToken=50+i;s.type=IEEE80211_FC0_TYPE_DATA;
        }
        r.data[6].type=IEEE80211_FC0_TYPE_MGT;
        d.awdlRadio.station=true;d.awdlRadio.queue=1;
        d.flushResponse.length=sizeof(iwx_tx_path_flush_cmd_rsp);
        auto &resp=*reinterpret_cast<iwx_tx_path_flush_cmd_rsp *>(d.flushResponse.data);
        resp.sta_id=AirportAWDL::RadioState::StationID;resp.num_flushed_queues=1;
        auto &q=resp.queues[0];q.tid=IWX_MGMT_TID;q.queue_num=1;
        q.read_before_flush=253;q.read_after_flush=1;
        switch(variant) {
        case 0:break;
        case 1:d.flushResponse.length=0;break;
        case 2:q.read_before_flush=5; q.read_after_flush=9;break; // old lap, same physical slots
        case 3:q.read_before_flush=0; q.read_after_flush=254;break; // reversed span
        case 4:q.read_before_flush=509;break; // invalid pointer high bits
        case 5:q.read_after_flush=257;break;
        case 6:q.read_after_flush=2;break; // beyond producer
        case 7:q.tid=0;break;
        case 8:d.awdlRadio.queue=0;break;
        case 9:d.awdlRadio.station=false;break;
        case 10:d.resetDuringFlush=true;break;
        case 11:q.read_before_flush=q.read_after_flush;break; // all reclaimed, none confirmed flushed
        case 12:d.flushResponse.length--;break;
        }
        int err=d.iwx_flush_sta_tids(&d.com,AirportAWDL::RadioState::StationID,0xffff);
        assert(err==(variant==12?EIO:0));
        if(!variant || variant==11) {
            assert(r.queued==0 && d.dataResults.size()==4 && d.results.size()==1);
            const std::vector<std::pair<unsigned,unsigned>> expected = {{variant ? 5U : 1U,5U}};
            assert(d.awdlBroadcastReports.flushRanges==expected);
            assert(!d.iwx_flush_sta_tids(&d.com,AirportAWDL::RadioState::StationID,0xffff));
            assert(d.awdlBroadcastReports.flushRanges==expected);
        } else {
            assert(d.awdlBroadcastReports.flushRanges.empty());
            assert(d.dataResults.empty() && d.results.empty() && r.queued==5);
            for(auto &p:packets)assert(!p.frees);
        }
        d.iwx_reset_tx_ring(&d.com,&r);
        std::sort(d.dataResults.begin(),d.dataResults.end());
        const int prefix=(!variant||variant==11)?0:EIO;
        const int suffix=!variant?ECANCELED:(variant==11?0:EIO);
        assert((d.dataResults==std::vector<std::pair<uint64_t,int>>{{50,prefix},{51,suffix},{53,suffix},{54,suffix}}));
        assert((d.results==std::vector<std::pair<uint64_t,bool>>{{52,variant==11}}));
        for(auto &p:packets)assert(p.frees==1);
    }
}

static void dualTIDFlush() {
    for(unsigned variant=0;variant<4;++variant) {
        ItlIwx d;d.awdlRadio.station=true;d.awdlRadio.queue=1;
        d.awdlRadio.dataQueueConfigured=true;d.awdlRadio.dataQueue=2;
        Packet packets[2];
        for(unsigned q=1;q<=2;++q) {
            auto &r=d.com.txq[q];r.qid=q;r.cur=r.queued=1;
            r.data[0].m=&packets[q-1];r.data[0].awdlToken=q;r.data[0].flags=IWX_TXDATA_FLAG_AWDL;
            r.data[0].type=IEEE80211_FC0_TYPE_DATA;
        }
        d.flushResponse.length=sizeof(iwx_tx_path_flush_cmd_rsp);
        auto &r=*reinterpret_cast<iwx_tx_path_flush_cmd_rsp *>(d.flushResponse.data);
        r.sta_id=AirportAWDL::RadioState::StationID;r.num_flushed_queues=2;
        for(unsigned n=0;n<2;++n) {
            auto &q=r.queues[n];q.queue_num=n+1;q.tid=n?0:IWX_MGMT_TID;
            q.read_before_flush=0;q.read_after_flush=1;
        }
        if(variant==1)r.queues[1].tid=IWX_MGMT_TID;
        if(variant==2)d.awdlRadio.dataQueueConfigured=false;
        if(variant==3)d.resetDuringFlush=true;
        assert(!d.iwx_flush_sta_tids(&d.com,AirportAWDL::RadioState::StationID,0xffff));
        assert(d.com.txq[1].queued==(variant==3?1:0));
        assert(d.com.txq[2].queued==(variant?1:0));
        for(unsigned q=1;q<=2;++q)d.iwx_reset_tx_ring(&d.com,&d.com.txq[q]);
        assert(packets[0].frees==1 && packets[1].frees==1 && d.dataResults.size()==2);
        assert(d.dataResults[0].second==(variant==3?EIO:ECANCELED));
        assert(d.dataResults[1].second==(variant?EIO:ECANCELED));
    }
}

int main() {completions();dataStatus();flushRange();dualTIDFlush();}
