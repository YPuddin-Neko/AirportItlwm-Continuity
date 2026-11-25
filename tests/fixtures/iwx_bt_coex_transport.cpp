#define IEEE80211_CHAN_VHT80 0x10000000
#define IEEE80211_CHAN_NOADHOC 0x20000000
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <sys/time.h>
#include <cstring>
#include <cerrno>
#include <functional>
#include <vector>
#include <string>
#include <memory>
#include <utility>
#include <cstdio>
#include "include/HAL/ItlTx.hpp"
#include "itlwm/hal_iwx/BTCoex.hpp"
#include "AirportItlwm/AWDL/SyncEngine.hpp"
#include "AirportItlwm/AWDL/DataFrame.hpp"
#include "AirportItlwm/AWDL/RadioState.hpp"
using AirportAWDL::RadioState;
#define htole16(x) (x)
#define htole32(x) (x)
#define le32toh(x) (x)
#define le16toh(x) (x)
#define le16_to_cpu(x) (x)
#define htole64(x) (x)
#define AIRPORT_WCL 1
#define BIT(n) (1U << (n))
#define AIRPORT_VTD 1
#define mtod(m,t) reinterpret_cast<t>(mbuf_data(m))
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#ifndef howmany
#define howmany(x,y) (((x)+(y)-1)/(y))
#endif
#define _ALIGN(x,y) (((x)+(y)-1)&~((y)-1))
#define IC2IFP(ic) (&(ic)->ic_if)
#define KASSERT(v,msg) assert(v)
#define DPRINTFN(n,x) ((void)0)
#define DPRINTF(x) ((void)0)
#define XYLog(...) ((void)0)
#define bus_dmamap_sync(...) ((void)0)
#define IWX_WRITE(sc,reg,value) writeDoorbell(sc,reg,value)
#define MIN(a,b) std::min((a),(b))
#define MAX(a,b) std::max((a),(b))
#define isset(p,b) ((p)[(b)/8] & (1U<<((b)%8)))
#define IEEE80211_IS_CHAN_2GHZ(c) ((c)->ic_flags&IEEE80211_CHAN_2GHZ)
using IOReturn=int;
struct OSObject {};
static uint64_t clockUS=1000000;
static uint64_t awdlRadioNow() {return clockUS;}
static void getmicrouptime(timeval *t) {t->tv_sec=clockUS/1000000;t->tv_usec=clockUS%1000000;}
enum { IEEE80211_M_STA=1, IEEE80211_S_RUN=4,
    IEEE80211_CHAN_DFS=2, IEEE80211_CHAN_PASSIVE=4,
    IEEE80211_CHAN_2GHZ=8, IEEE80211_CHAN_5GHZ=16, IEEE80211_CHAN_MAX=255 };
struct Packet {uint8_t bytes[2048]={};size_t size=32,total=32,offset=0;unsigned frees=0;};
using mbuf_t=Packet*;
using __le16=uint16_t;using __le32=uint32_t;using __le64=uint64_t;
struct ieee80211_frame {uint8_t i_fc[2],i_dur[2],i_addr1[6],i_addr2[6],i_addr3[6],i_seq[2];};
static void mbuf_freem(mbuf_t p) {assert(p && p->frees++==0);}
static size_t mbuf_len(mbuf_t p) {return p->size;}
static size_t mbuf_pkthdr_len(mbuf_t p) {return p->total;}
static void *mbuf_data(mbuf_t p) {return p->bytes+p->offset;}
static void mbuf_adj(mbuf_t p,unsigned n) {assert(n<=p->size);p->offset+=n;p->size-=n;p->total-=n;}
struct DMAMap {std::vector<uint8_t> bytes;bool fail=false;};
using bus_dmamap_t=DMAMap*;using bus_addr_t=uint64_t;
struct IOPhysicalSegment {uint64_t location,length;};
static int bus_dmamap_packet(DMAMap *map,mbuf_t p,IOPhysicalSegment *seg,unsigned,bool receive) {
    assert(!receive && !p->frees);if(map->fail)return 0;
    map->bytes.assign(p->bytes+p->offset,p->bytes+p->offset+p->size);
    seg->location=reinterpret_cast<uint64_t>(map->bytes.data());seg->length=p->size;
    return p->size?1:0;
}
// FIRMWARE DECLARATIONS
void iwx_clear_tx_desc(struct iwx_softc *,struct iwx_tx_ring *,int);
// Kernel allocation and DMA boundary for the radio/transport integration tests.
struct Channel {unsigned ic_flags=0,ic_freq=0;};
struct Phy {int id=0,color=0;Channel *channel=nullptr;};
struct ieee80211_frame_min {uint8_t bytes[10];};
struct ieee80211_tx_ba {int ba_state=0;};
struct ieee80211_node {ieee80211_tx_ba ni_tx_ba[16];unsigned releases=0;};
struct iwx_node {ieee80211_node in_ni;Phy *in_phyctxt=nullptr;int in_id=0,in_color=0;};
struct ieee80211_key {int k_cipher;uint64_t k_tsc;};
enum {IEEE80211_FC0_TYPE_MASK=12,IEEE80211_FC0_SUBTYPE_MASK=240,
    IEEE80211_FC0_TYPE_MGT=0,IEEE80211_FC0_TYPE_CTL=4,IEEE80211_FC0_TYPE_DATA=8,
    IEEE80211_FC0_SUBTYPE_ACTION=0xd0,IEEE80211_FC0_SUBTYPE_NODATA=64,IEEE80211_FC1_PROTECTED=64,
    IEEE80211_QOS_TID=15,IEEE80211_QOS_AMSDU=128,IEEE80211_BA_AGREED=1,IEEE80211_CIPHER_CCMP=3,EDCA_AC_BE=0};
#define IEEE80211_IS_MULTICAST(p) ((p)[0]&1)
static bool ieee80211_has_qos(const ieee80211_frame *p) {return (p->i_fc[0]&0x8c)==0x88;}
static uint16_t ieee80211_get_qos(const ieee80211_frame *p) {assert(ieee80211_has_qos(p));return 0;}
static unsigned ieee80211_get_hdrlen(const ieee80211_frame *p) {assert(!ieee80211_has_qos(p));return 24;}
static int ieee80211_wcl_ac(unsigned) {return EDCA_AC_BE;}
struct Statistics {unsigned outputErrors=0;};
struct _ifnet {Statistics stats;Statistics *netStat=&stats;};
struct ieee80211com {
    int ic_opmode=1,ic_state=4;ieee80211_node *ic_bss=nullptr;
    Channel ic_channels[256];uint8_t ic_chan_active[32]={},ic_beacon_hint[32]={};uint16_t ic_beacon_hint_mcc=0;_ifnet ic_if;
    uint64_t ic_wcl_tx_packets[4]={};
};
static void ieee80211_release_node(ieee80211com *,ieee80211_node *n) {++n->releases;}
static ieee80211_key *ieee80211_get_txkey(ieee80211com *,ieee80211_frame *,ieee80211_node *) {
    assert(false);return nullptr;
}
static mbuf_t ieee80211_encrypt(ieee80211com *,mbuf_t,ieee80211_key *) {assert(false);return nullptr;}
struct Memory {void *vaddr=nullptr;size_t size=0;};
struct iwx_tx_ring {
    unsigned ring_count=0;int tail=0,cur=0,queued=0,qid=IWX_INVALID_QUEUE,hi_mark=56;
    std::vector<iwx_tx_data> data;
    std::vector<DMAMap> maps;
    std::vector<iwx_tfh_tfd> descriptors;
    std::vector<iwx_device_cmd> commands;
    std::vector<uint16_t> counts;
    iwx_tfh_tfd *desc=nullptr;iwx_device_cmd *cmd=nullptr;Memory desc_dma,bc_tbl;
    void allocate(unsigned id) {
        assert(!queued);ring_count=64;qid=id;tail=cur=0;
        data.assign(64,{});maps.assign(64,{});descriptors.assign(64,{});commands.assign(64,{});
        counts.assign(sizeof(iwx_agn_scd_bc_tbl)/2,0);
        desc=descriptors.data();cmd=commands.data();
        desc_dma={desc,descriptors.size()*sizeof(*desc)};bc_tbl={counts.data(),counts.size()*2};
        for(unsigned i=0;i<64;++i) {data[i].map=&maps[i];data[i].cmd_paddr=reinterpret_cast<uint64_t>(&cmd[i]);}
    }
};
struct iwx_softc {
    const struct { uint8_t non_shared_ant; } *sc_cfg=nullptr;
    ieee80211com sc_ic={};unsigned sc_flags=IWX_FLAG_BINDING_ACTIVE;
    int sc_device_family=IWX_DEVICE_FAMILY_22000;int first_data_qid=1;
    iwx_tx_ring txq[16];Phy sc_phyctxt[3];uint8_t sc_enabled_capa[8]={};
    struct {int qid=IWX_INVALID_QUEUE;} sc_tid_data[16];
    unsigned sc_tx_timer=0;uint8_t sc_mgmt_last_antenna_idx=0;
    void *sc_cmd_resp_pkt[64]={};size_t sc_cmd_resp_len[64]={};ItlQueueMask<16> qfullmsk={};
    std::vector<unsigned> doorbells;
};
struct iwx_rx_packet {size_t length=0;alignas(8) uint8_t data[128]={};};
struct iwx_rx_data {void *map=nullptr;};
static size_t iwx_rx_packet_payload_len(const iwx_rx_packet *p) {return p->length;}
struct ieee80211_rateset {uint8_t rs_nrates,rs_rates[16];};

struct WorkLoop {bool gated=true;bool inGate() {return gated;}};
struct Gate {
    WorkLoop *loop;unsigned wakes=0;
    void commandWakeup(void *) {++wakes;}
    IOReturn runAction(IOReturn (*f)(OSObject*,void*,void*,void*,void*),void *a=nullptr,void *b=nullptr) {
        bool old=loop->gated;loop->gated=true;int r=f(nullptr,a,b,nullptr,nullptr);loop->gated=old;return r;
    }
};
// RATE DECLARATIONS
#include "itlwm/hal_iwx/AWDLTxBudget.hpp"
struct ItlIwx {
    uint8_t awdlTXAntenna() {return 1;}
    uint8_t awdlTXMcs(uint64_t,unsigned=0) const {return 0;}
    uint8_t awdlTXWidth(uint64_t) const {return 20;}
    IwxBTCoexState btCoex{};
    iwx_softc com; iwx_node infra{};
    WorkLoop loop;Gate gate{&loop};
    RadioState awdlRadio={};uint64_t dmaEpoch=1;
    AirportAWDL::PeerTable awdlPeers={};
    uint64_t awdlRateSerial=0,awdlRateProbes=0;
    void *awdlReceiver=this;
    void (*awdlAvailabilityChanged)(void *)=nullptr;
    std::vector<bool> availability;
    int failAt=0,resetAt=0,calls=0;bool startNotify=true,endOnCancel=true,flushReclaims=true;
    bool stationPresent=false,dmaOwned=false,dmaDetaching=false;
    uint8_t awdlMinRateMbps=12;
    std::vector<std::pair<uint64_t,bool>> txResults;
    void (*awdlDataComplete)(void *,uint64_t,int)=nullptr;
    void (*awdlTxComplete)(void *,uint64_t,bool)=[](void *self,uint64_t token,bool ack) {
        static_cast<ItlIwx *>(self)->txResults.push_back({token,ack});
    };
    uint32_t bindingStatus=0,stationStatus=IWX_ADD_STA_SUCCESS;
    std::function<void(const std::string&)> onCommand;
    std::vector<std::string> commands;
    ItlIwx() {
        infra.in_phyctxt=&com.sc_phyctxt[0];com.sc_ic.ic_bss=&infra.in_ni;
        for(unsigned c:{6u,44u,48u,149u}) {
            com.sc_ic.ic_channels[c]={c<14?8u:16u,c<14?2437u:5000u+c*5};
            com.sc_ic.ic_chan_active[c/8]|=1U<<(c%8);
        }
        com.sc_phyctxt[0]={0,0,&com.sc_ic.ic_channels[48]};
        com.sc_phyctxt[1]={1,0,&com.sc_ic.ic_channels[6]};
        for(unsigned c:{39u,54u}) com.sc_enabled_capa[c/8]|=1U<<(c%8);
    }
    WorkLoop *getMainWorkLoop() {return &loop;}
    Gate *getMainCommandGate() {return &gate;}
    unsigned iwx_lmac_id(iwx_softc *,const Channel *c) {return (c->ic_flags&8)?0:1;}
    unsigned iwx_fw_valid_rx_ant(iwx_softc *) {return 2;}
    unsigned iwx_num_of_ant(unsigned x) {return __builtin_popcount(x);}
    unsigned iwx_lookup_cmd_ver(iwx_softc *,unsigned g,unsigned c) {assert(g==1);if(c==IWX_TX_CMD)return 9;assert(c==8);return 3;}
    static unsigned iwx_cmd_id(unsigned id,unsigned group,unsigned version) {return id|(group<<8)|(version<<16);}
    int iwx_wait_notification(iwx_softc *,int *v,int bits,uint32_t ms) {
        assert(false && "AWDL cancellation must not block for END");return EWOULDBLOCK;
    }
    int command(const std::string &name) {
        assert(loop.gated);commands.push_back(name);++calls;
        assert(com.sc_phyctxt[0].channel==&com.sc_ic.ic_channels[48]);
        if(calls==resetAt) {++dmaEpoch;resetAWDLRadio();stationPresent=false;dmaOwned=false;return ENXIO;}
        if(onCommand)onCommand(name);
        return calls==failAt?EIO:0;
    }
    void notify(unsigned start,unsigned status=1,unsigned mac=1,unsigned conf=3,size_t length=16) {
        iwx_rx_packet p={};p.length=length;iwx_session_prot_notif n{mac,status,start,conf};
        memcpy(p.data,&n,sizeof(n));receiveAWDLSession(&p);
    }
    int iwx_send_cmd_pdu(iwx_softc*,unsigned id,unsigned,size_t size,const void *data) {
        if(id==IWX_POWER_TABLE_CMD) {
            assert(static_cast<const iwx_device_power_cmd*>(data)->flags==0);return command("awake");
        }
        if(id==IWX_MAC_CONTEXT_CMD) {
            auto c=static_cast<const iwx_mac_ctx_cmd*>(data);assert(c->id_and_color==1);
            assert(size==sizeof(*c) && size==148);
            assert(offsetof(iwx_mac_ctx_cmd,ac)==60 && offsetof(iwx_mac_ctx_cmd,p2p_dev)==100);
            if(c->action==1) {
                assert(c->mac_type==6 && c->tsf_id==1 && c->short_slot==0x10);
                const uint8_t masks[]={2,4,8,16};
                for(unsigned i=0;i<4;++i) {
                    assert(c->ac[i].fifos_mask==masks[i]);
                    assert(c->ac[i].cw_min==15 && c->ac[i].cw_max==1023 && c->ac[i].aifsn==3);
                    assert(!c->ac[i].edca_txop);
                }
                const iwx_ac_qos empty={};assert(!memcmp(&c->ac[4],&empty,sizeof(empty)));
                assert(!c->reserved_for_node_addr && !c->reserved_for_bssid_addr);
                assert(AirportAWDL::sameAddress(c->bssid_addr,AirportAWDL::BSSID));
                assert(c->filter_flags&IWX_MAC_FILTER_ACCEPT_GRP);return command("add-mac");
            }
            assert(c->action==3);return command("remove-mac");
        }
        if(id==IWX_PHY_CONTEXT_CMD) {
            auto c=static_cast<const iwx_phy_context_cmd*>(data);assert(c->id_and_color==1);
            assert(!awdlRadio.binding && !stationPresent && !c->ci.width && !c->ci.ctrl_pos);
            if(c->action==3)return command("remove-phy");
            assert(c->ci.channel && c->rxchain_info==((2<<1)|(1<<10)|(1<<12)));
            assert(c->ci.band==(c->ci.channel<14?1:0));
            return command(c->action==1?"add-phy":"modify-phy");
        }
        if(id==0x305) {
            auto c=static_cast<const iwx_session_prot_cmd*>(data);
            assert(c->id_and_color==1 && c->conf_id==IWX_SESSION_PROTECT_CONF_P2P_GO_NEGOTIATION &&
                c->conf_id==awdlRadio.dwell.confID && !c->repetition_count && !c->interval);
            if(c->action==1) {
                assert(stationPresent && dmaOwned && awdlRadio.dwell.requested);
                assert(c->duration_tu>=4 && c->duration_tu<=16);
                int e=command("start-dwell");if(!e && startNotify)notify(1);return e;
            }
            assert(c->action==3 && !c->duration_tu && !awdlRadio.dwell.running);
            int e=command("cancel-dwell");if(!e && endOnCancel)notify(0);return e;
        }
        assert(id==IWX_REMOVE_STA && static_cast<const iwx_rm_sta_cmd*>(data)->sta_id==3);
        int error=command("remove-station");if(!error){stationPresent=false;dmaOwned=false;}return error;
    }
    int iwx_send_cmd_pdu_status(iwx_softc*,unsigned id,size_t size,const void *data,uint32_t *status) {
        if(id==IWX_BINDING_CONTEXT_CMD) {
            auto c=static_cast<const iwx_binding_cmd*>(data);
            assert(size == (isset(com.sc_enabled_capa,39) ? 28 : 24));
            assert(c->phy==1 && c->id_and_color==1 && c->macs[0]==(c->action==1?1:0xffffffffU) &&
                c->macs[1]==0xffffffffU && c->macs[2]==0xffffffffU);
            assert(c->action==1||c->action==3);*status=c->action==1?bindingStatus:0;
            return command(c->action==1?"bind":"unbind");
        }
        assert(id==IWX_ADD_STA);auto c=static_cast<const iwx_add_sta_cmd*>(data);
        assert(size==sizeof(*c) && size==48);
        assert(c->sta_id==3 && c->station_type==IWX_STA_GENERAL_PURPOSE && c->mac_id_n_color==1 && c->tid_disable_tx==0xffff);
        for(auto b:c->addr)assert(b==255);
        *status=stationStatus;int error=command("add-station");
        if(!error && stationStatus==IWX_ADD_STA_SUCCESS)stationPresent=true;return error;
    }
    int iwx_tvqm_enable_txq(iwx_softc *,int tid,int,unsigned size,uint8_t station,IwxTxQueueSetup *result) {
        assert(stationPresent && station==3 && size==64 && tid==IWX_MGMT_TID);
        result->responseFlags=UINT32_MAX;
        int error=command("queue");if(error)return -error;
        com.txq[7].allocate(7);dmaOwned=true;return 7;
    }
    int iwx_flush_sta_tids(iwx_softc *,int station,uint16_t tids) {
        assert(station==3 && tids==0xffff && !awdlRadio.dwell.running);
        int error=command("flush");
        if(!error && flushReclaims)iwx_ampdu_txq_advance(&com,&com.txq[7],com.txq[7].cur%com.txq[7].ring_count);
        return error;
    }
    void iwx_free_tx_ring(iwx_softc *,iwx_tx_ring *r) {
        assert(!dmaOwned && r==&com.txq[7] && !r->queued);r->ring_count=0;
        commands.push_back("free-ring");
    }
    int iwx_power_update_device(iwx_softc *) {return command("power-restore");}
    unsigned getTxQueueSize() {return 256;}
    int iwx_tx(iwx_softc *,mbuf_t,ieee80211_node *,int,int,uint64_t,uint64_t=0,uint64_t=0);
    bool awdlTxWindowOpen() const {assert(false && "AP TX checked an AWDL grant");return false;}
    int iwx_rval2ridx(int);
    const iwx_rate *iwx_tx_fill_awdl_cmd(iwx_softc *,uint32_t *,uint8_t compareAntenna=0,uint8_t selectedRate=0);
    const iwx_rate *iwx_tx_fill_cmd(iwx_softc *,iwx_node *n,ieee80211_frame *h,uint32_t *f,uint32_t *r) {
        assert(n==&infra && h->i_fc[0]==0xd0);*f=IWX_TX_FLAGS_CMD_RATE;*r=13;return &iwx_rates[0];
    }
    void iwx_tx_update_byte_tbl(iwx_softc *,iwx_tx_ring *,int,uint16_t,uint16_t);
    std::vector<std::pair<uint64_t,bool>> coexResults;
    void iwx_bt_coex_complete_tx(iwx_softc *,iwx_tx_data *data,bool ack) {
        if(!(data->flags&IWX_TXDATA_FLAG_COEX)||!data->coexToken)return;
        coexResults.push_back({data->coexToken,ack});data->coexToken=0;
    }
    void completeAWDLTx(iwx_tx_data *,bool,bool=false);
    void sampleAWDLRate(iwx_softc *,iwx_rx_packet *,iwx_tx_data *d) {assert(!d->awdlRatePeer);}
    void iwx_rx_tx_cmd_single(iwx_softc *,iwx_rx_packet *,iwx_tx_data *);
    void iwx_rx_tx_cmd(iwx_softc *,iwx_rx_packet *,iwx_rx_data *);
    void iwx_txd_done(iwx_softc *,iwx_tx_data *);
    void iwx_ampdu_txq_advance(iwx_softc *,iwx_tx_ring *,int);
    void iwx_reset_tx_ring(iwx_softc *,iwx_tx_ring *);
    void iwx_toggle_tx_ant(iwx_softc *,uint8_t *p) {++*p;}
    void iwx_clear_oactive(iwx_softc *sc,iwx_tx_ring *) {assert(sc->qfullmsk.empty());}
    void writeDoorbell(iwx_softc *sc,unsigned reg,unsigned value) {
        assert(reg==IWX_HBUS_TARG_WRPTR);
        if(value>>16==7) assert(stationPresent && dmaOwned && awdlRadio.dwell.running);
        else {
            assert(value>>16==1);
            auto &r=sc->txq[1];const unsigned i=((value&0xffff)-1)&(r.ring_count-1);
            assert(r.data[i].coexToken && (r.data[i].flags&IWX_TXDATA_FLAG_COEX));
            assert(r.data[i].m && r.data[i].in==&infra);
        }
        sc->doorbells.push_back(value);
    }
    void response(unsigned status=1,unsigned next=UINT32_MAX) {
        iwx_rx_packet p={};p.length=sizeof(iwx_tx_resp)+4;
        auto t=reinterpret_cast<iwx_tx_resp *>(p.data);
        t->frame_count=1;t->tx_queue=7;t->status.status=status;
        if(next==UINT32_MAX)next=com.txq[7].cur;
        memcpy(&t->status+1,&next,4);iwx_rx_data rx;
        iwx_rx_tx_cmd(&com,&p,&rx);
    }
    int startAWDLRadio(const uint8_t *);int stopAWDLRadio();void resetAWDLRadio();int transmitAWDL(mbuf_t,uint64_t=0,uint64_t=0);
    int awdlMAC(uint32_t,bool=false);int awdlBinding(bool);int awdlRemoveQueue();
    bool awdlChannelAllowed(uint32_t,uint8_t=20) const;int awdlPHY(uint16_t,uint8_t=20);int finishAWDLDwell();
    int serviceAWDLChannel(uint16_t,uint64_t,RadioState::GrantPolicy = {});void receiveAWDLSession(const iwx_rx_packet *);
};
// PRODUCTION
static Packet coexFrame(unsigned mode=3) {
    Packet p;p.size=p.total=27;p.bytes[0]=0xd0;p.bytes[4]=2;
    p.bytes[10]=4;p.bytes[16]=2;p.bytes[24]=7;p.bytes[25]=1;p.bytes[26]=mode;
    return p;
}
static void coexResponse(ItlIwx &d,unsigned status=1) {
    iwx_rx_packet p={};p.length=sizeof(iwx_tx_resp)+4;
    auto t=reinterpret_cast<iwx_tx_resp *>(p.data);t->frame_count=1;t->tx_queue=1;t->status.status=status;
    unsigned next=d.com.txq[1].cur;memcpy(&t->status+1,&next,4);
    iwx_rx_data rx;d.iwx_rx_tx_cmd(&d.com,&p,&rx);
}
static void coexAPTransmit() {
    ItlIwx d;auto &r=d.com.txq[1];r.allocate(1);
    // The management queue belongs to the associated AP and not AWDL MAC1.
    for(unsigned status:{1u,2u,0x82u}) {
        Packet p=coexFrame();const Packet original=p;
        unsigned slot=r.cur&(r.ring_count-1),releases=d.infra.in_ni.releases;
        const uint64_t token=100+status;
        assert(!d.iwx_tx(&d.com,&p,&d.infra.in_ni,0,-1,0,token));
        assert(!p.frees && p.size==3 && p.offset==24 && r.queued==1);
        auto c=reinterpret_cast<const iwx_tx_cmd_gen2 *>(r.cmd[slot].data);
        assert(c->len==27 && (c->flags&IWX_TX_FLAGS_ENCRYPT_DIS));
        assert(!memcmp(reinterpret_cast<const uint8_t *>(c)+sizeof(*c),original.bytes,24));
        assert(r.maps[slot].bytes==std::vector<uint8_t>({7,1,3}));
        assert(r.data[slot].coexToken==token && !r.data[slot].awdlToken);
        assert(r.desc[slot].num_tbs==3 && r.desc[slot].tbs[2].tb_len==3);
        assert(d.coexResults.size()==releases);
        coexResponse(d,status);
        assert(!r.queued && p.frees==1 && d.infra.in_ni.releases==releases+1);
        assert(d.coexResults.back()==std::make_pair(token,status==1));
        coexResponse(d,status);assert(d.coexResults.size()==releases+1);
    }
    // Mapping failure never transfers the AP reference to a descriptor.
    auto slot=r.cur&(r.ring_count-1);r.maps[slot].fail=true;
    Packet failed=coexFrame();const auto released=d.infra.in_ni.releases;
    assert(d.iwx_tx(&d.com,&failed,&d.infra.in_ni,0,-1,0,500)==ENOMEM);
    assert(failed.frees==1 && !r.queued && !r.data[slot].coexToken && !r.data[slot].in);
    assert(d.infra.in_ni.releases==released);r.maps[slot].fail=false;
    // Protected/raw/foreign-peer submissions cannot use this ownership path.
    Packet protectedFrame=coexFrame();protectedFrame.bytes[1]=64;
    assert(d.iwx_tx(&d.com,&protectedFrame,&d.infra.in_ni,0,-1,0,501)==EINVAL && protectedFrame.frees==1);
    Packet raw=coexFrame();assert(d.iwx_tx(&d.com,&raw,nullptr,0,7,0,502)==EINVAL && raw.frees==1);
    ieee80211_node other;Packet foreign=coexFrame();
    assert(d.iwx_tx(&d.com,&foreign,&other,0,-1,0,503)==EINVAL && foreign.frees==1);
    r.queued=r.ring_count-1;Packet full=coexFrame();
    assert(d.iwx_tx(&d.com,&full,&d.infra.in_ni,0,-1,0,504)==ENOBUFS && full.frees==1);
    r.queued=0;assert(d.infra.in_ni.releases==released);
    // DMA reset resolves a pending AP action exactly once and releases its reference.
    Packet pending=coexFrame(1);assert(!d.iwx_tx(&d.com,&pending,&d.infra.in_ni,0,-1,0,505));
    d.iwx_reset_tx_ring(&d.com,&r);
    assert(pending.frees==1 && d.infra.in_ni.releases==released+1);
    assert(d.coexResults.back()==std::make_pair(uint64_t(505),false));
    const auto completed=d.coexResults.size();d.iwx_reset_tx_ring(&d.com,&r);
    assert(d.coexResults.size()==completed && d.infra.in_ni.releases==released+1);
}

int main(){coexAPTransmit();}
