#define IEEE80211_CHAN_VHT80 0x10000000
#define IEEE80211_CHAN_NOADHOC 0x20000000
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <sys/time.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <functional>
#include <vector>
#include <string>
#include <memory>
#include <map>
#include <utility>
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
struct Packet {uint8_t bytes[4096]={};size_t size=32,total=32,offset=0;unsigned frees=0;};
using mbuf_t=Packet*;
enum { MBUF_DONTWAIT=0 };
static std::vector<std::unique_ptr<Packet>> allocatedPackets;
static bool failAllocation=false;
static int mbuf_getpacket(int,mbuf_t *p) {
    if(failAllocation) {*p=nullptr;return ENOMEM;}
    allocatedPackets.emplace_back(new Packet);*p=allocatedPackets.back().get();return 0;
}
static size_t mbuf_maxlen(mbuf_t p) {return sizeof(p->bytes);}
static void mbuf_setlen(mbuf_t p,size_t size) {p->size=size;}
static void mbuf_pkthdr_setlen(mbuf_t p,size_t size) {p->total=size;}

using __le16=uint16_t;using __le32=uint32_t;using __le64=uint64_t;
struct ieee80211_frame {uint8_t i_fc[2],i_dur[2],i_addr1[6],i_addr2[6],i_addr3[6],i_seq[2];};
static void mbuf_freem(mbuf_t p) {assert(p && p->frees++==0);}
static size_t mbuf_len(mbuf_t p) {return p->size;}
static size_t mbuf_pkthdr_len(mbuf_t p) {return p->total;}
static void *mbuf_data(mbuf_t p) {return p->bytes+p->offset;}
static void mbuf_adj(mbuf_t p,unsigned n) {assert(n<=p->size);p->offset+=n;p->size-=n;p->total-=n;}
struct DMAMap {std::vector<uint8_t> bytes;bool fail=false;std::function<void()> onMap;};
using bus_dmamap_t=DMAMap*;using bus_addr_t=uint64_t;
struct IOPhysicalSegment {uint64_t location,length;};
static int bus_dmamap_packet(DMAMap *map,mbuf_t p,IOPhysicalSegment *seg,unsigned,bool receive) {
    assert(!receive && !p->frees);if(map->fail)return 0;
    map->bytes.assign(p->bytes+p->offset,p->bytes+p->offset+p->size);
    seg->location=reinterpret_cast<uint64_t>(map->bytes.data());seg->length=p->size;
    auto onMap = map->onMap;
    if (onMap) onMap();
    return p->size?1:0;
}
// FIRMWARE DECLARATIONS
// TRANSPORT TYPES
#include "tests/fixtures/awdl_transport.hpp"
#include "itlwm/hal_iwx/AWDLTxBudget.hpp"
enum { PCI_VENDOR_INTEL=0x8086 };
static const iwl_cfg iwl9560_quz_a0_jf_b0_cfg{2};
#if DEBUG
#include "itlwm/hal_iwx/AWDLTxReports.hpp"
enum { kIOReturnSuccess = 0 };
#endif
struct WorkLoop {bool gated=true;bool inGate() {return gated;}};
struct Gate {
    WorkLoop *loop;unsigned wakes=0;
    void commandWakeup(void *) {++wakes;}
    IOReturn runAction(IOReturn (*f)(OSObject*,void*,void*,void*,void*),void *a=nullptr,void *b=nullptr) {
        bool old=loop->gated;loop->gated=true;int r=f(nullptr,a,b,nullptr,nullptr);loop->gated=old;return r;
    }
};
struct ItlIwx {
    AirportAWDL::BlockAckState awdlBA={};
    int configureAWDLPeerWidth();
    int downgradeAWDLTXRates(uint8_t width, uint16_t = 0) {
        if (awdlRadio.blockAckTX.configured && awdlRadio.blockAckTX.widthMHz > width)
            awdlRadio.blockAckTX.widthMHz = width;
        return 0;
    }
    bool awdlHasRXBA() const { return false; }
    unsigned baClears=0;
    int clearAWDLBlockAck() { ++baClears;awdlRadio.blockAckTX={};return 0; }
    void resetAWDLBlockAck() {awdlRadio.blockAckTX={};}
    int configureAWDLTXStation(bool enabled,bool paused) {
        awdlRadio.blockAckTX.enabled=enabled;awdlRadio.blockAckTX.paused=paused;
        if(paused)awdlRadio.blockAckTX.windowEndUS=0;return 0;
    }
    bool iwx_mimo_enabled(iwx_softc *) {return true;}
    uint8_t awdlTXAntenna();uint8_t awdlTXMcs(uint64_t,unsigned=0) const;uint8_t awdlTXWidth(uint64_t) const;
    void noteAWDLTXBlockAck(const uint8_t *,size_t,bool=false);

    IwxBTCoexState btCoex{};
    iwx_softc com; iwx_node infra{};
    WorkLoop loop;Gate gate{&loop};
    RadioState awdlRadio={};uint64_t dmaEpoch=1;
    AirportAWDL::PeerTable awdlPeers{};
    uint64_t awdlRateSerial=0,awdlRateSamples=0,awdlRateIgnored=0,awdlRateProbes=0;
    uint64_t awdlRateRaised=0,awdlRateLowered=0;
    uint32_t awdlRateLastFlags=0;
    uint16_t awdlRateLastMediaUS=0;
    uint8_t awdlRateLastRetries=0,txNotifVersion=7;
#if DEBUG
    IwxAWDLBroadcastReports awdlBroadcastReports = {};
    uint64_t awdlCompareEndUS=0,awdlCompareArmSubmitted[2]={},awdlCompareBTOverrides=0;
    uint8_t awdlCompareArm() const;
    bool awdlReceiveAllData = false, awdlReceiveFilterKnown = false;
    int awdlReceiveFilterCommandError = 0, awdlReceiveFilterCleanupResult = 0;
#endif
    void *awdlReceiver=this;
    void (*awdlAvailabilityChanged)(void *)=nullptr;
    std::vector<bool> availability;
    int failAt=0,resetAt=0,calls=0;bool startNotify=true,endOnCancel=true,flushReclaims=true;
    bool stationPresent=false,dmaOwned=false,dmaDetaching=false;
    uint8_t awdlMinRateMbps=12;
    unsigned rlcVersion=IWX_FW_CMD_VER_UNKNOWN,rxAnt=2,txVersion=9;
    uint8_t txAnt=3;
    std::vector<iwx_phy_context_cmd> phyCommands;
    std::vector<iwx_mac_ctx_cmd> macCommands;
    std::vector<iwx_rlc_config_cmd> rlcCommands;
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
    unsigned iwx_fw_valid_rx_ant(iwx_softc *) {return rxAnt;}
    unsigned iwx_num_of_ant(unsigned x) {return __builtin_popcount(x);}
    unsigned iwx_lookup_cmd_ver(iwx_softc *,unsigned g,unsigned c) {if(g==IWX_DATA_PATH_GROUP){assert(c==IWX_RLC_CONFIG_CMD);return rlcVersion;}assert(g==1);if(c==IWX_TX_CMD)return txVersion;assert(c==8);return 3;}
    unsigned iwx_lookup_notif_ver(iwx_softc *,unsigned g,unsigned c) {assert(g==1 && c==IWX_TX_CMD);return txNotifVersion;}
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
            macCommands.push_back(*c);
            assert(size==sizeof(*c) && size==148);
            assert(offsetof(iwx_mac_ctx_cmd,ac)==60 && offsetof(iwx_mac_ctx_cmd,p2p_dev)==100);
            if(c->action==1 || c->action==2) {
                assert(c->mac_type==(awdlRadio.listener?2u:6u) && c->tsf_id==1 && c->short_slot==0x10);
                const uint8_t masks[]={2,4,8,16};
                for(unsigned i=0;i<4;++i) {
                    assert(c->ac[i].fifos_mask==masks[i]);
                    assert(c->ac[i].cw_min==15 && c->ac[i].cw_max==1023 && c->ac[i].aifsn==3);
                    assert(!c->ac[i].edca_txop);
                }
                const iwx_ac_qos empty={};assert(!memcmp(&c->ac[4],&empty,sizeof(empty)));
                assert(!c->reserved_for_node_addr && !c->reserved_for_bssid_addr);
                assert(AirportAWDL::sameAddress(c->bssid_addr,AirportAWDL::BSSID));
                assert(c->filter_flags&IWX_MAC_FILTER_ACCEPT_GRP);
                return command(c->action==1?"add-mac":"modify-mac");
            }
            assert(c->action==3);return command("remove-mac");
        }
        if(id==IWX_PHY_CONTEXT_CMD) {
            auto c=static_cast<const iwx_phy_context_cmd*>(data);assert(c->id_and_color==1);
            assert((!awdlRadio.binding && !stationPresent) ||
                (awdlRadio.listener && awdlRadio.changing && !awdlRadio.dwell.running && c->action==IWX_FW_CTXT_ACTION_MODIFY));
            assert(c->ci.width==IWX_PHY_VHT_CHANNEL_MODE20 || c->ci.width==IWX_PHY_VHT_CHANNEL_MODE80);
            if(c->ci.width==IWX_PHY_VHT_CHANNEL_MODE20)assert(!c->ci.ctrl_pos);
            phyCommands.push_back(*c);
            if(c->action==3)return command("remove-phy");
            const unsigned chains=__builtin_popcount(rxAnt)>1?2:1;
            const unsigned expected=(rxAnt<<1)|(chains<<10)|(chains<<12);
            assert(c->ci.channel && c->rxchain_info==(rlcVersion==2?0:expected));
            assert(c->ci.band==(c->ci.channel<14?1:0));
            return command(c->action==1?"add-phy":"modify-phy");
        }
        if(id==0x20508) {
            assert(rlcVersion==2 && ((!awdlRadio.binding && !stationPresent && !dmaOwned) ||
                (awdlRadio.listener && awdlRadio.changing && !awdlRadio.dwell.running)));
            assert(size==32);auto c=static_cast<const iwx_rlc_config_cmd *>(data);
            const unsigned chains=__builtin_popcount(rxAnt)>1?2:1;
            assert(c->phy_id==1 && c->rlc.rx_chain_info==((rxAnt<<1)|(chains<<10)|(chains<<12)));
            const auto bytes=static_cast<const uint8_t *>(data);
            for(unsigned i=8;i<32;++i)assert(!bytes[i]);
            rlcCommands.push_back(*c);return command("rx-chains");
        }
        if(id==0x305) {
            auto c=static_cast<const iwx_session_prot_cmd*>(data);
            assert(c->id_and_color==1 && c->conf_id==IWX_SESSION_PROTECT_CONF_P2P_GO_NEGOTIATION &&
                c->conf_id==awdlRadio.dwell.confID && !c->repetition_count && !c->interval);
            if(c->action==1) {
                assert(stationPresent && dmaOwned && awdlRadio.dwell.requested);
                assert(c->duration_tu>=4 && c->duration_tu<=64);
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
        assert(stationPresent && station==3 && size==64 && (tid==IWX_MGMT_TID || tid==RadioState::DataTID));
        result->responseFlags=UINT32_MAX;
        const bool data=tid==RadioState::DataTID;
        int error=command(data?"data-queue":"queue");if(error)return -error;
        const unsigned qid=data?8:7;com.txq[qid].allocate(qid);dmaOwned=true;return qid;
    }
    int iwx_flush_sta_tids(iwx_softc *,int station,uint16_t tids) {
        assert(station==3 && tids==0xffff && !awdlRadio.dwell.running);
        int error=command("flush");
        if(!error && flushReclaims)for(unsigned q:{7u,8u})if(com.txq[q].ring_count)
            iwx_ampdu_txq_advance(&com,&com.txq[q],com.txq[q].cur%com.txq[q].ring_count);
        return error;
    }
    void iwx_free_tx_ring(iwx_softc *,iwx_tx_ring *r) {
        assert(!dmaOwned && (r==&com.txq[7] || r==&com.txq[8]) && !r->queued);r->ring_count=0;
        commands.push_back("free-ring");
    }
    int iwx_power_update_device(iwx_softc *) {return command("power-restore");}
    unsigned getTxQueueSize() {return 256;}
    int iwx_tx(iwx_softc *,mbuf_t,ieee80211_node *,int,int,uint64_t,uint64_t=0,uint64_t=0);
    int iwx_rval2ridx(int);
    const iwx_rate *iwx_tx_fill_awdl_cmd(iwx_softc *,uint32_t *,uint8_t compareAntenna=0,uint8_t selectedRate=0);
    const iwx_rate *iwx_tx_fill_cmd(iwx_softc *,iwx_node *node,ieee80211_frame *,uint32_t *flags,uint32_t *rate) {
#if AWDL_RADIO_CASE == 23
        assert(node==&infra);*flags=IWX_TX_FLAGS_CMD_RATE;*rate=0x8102;return &iwx_rates[0];
#else
        assert(false);return nullptr;
#endif
    }
    void iwx_tx_update_byte_tbl(iwx_softc *,iwx_tx_ring *,int,uint16_t,uint16_t);
    void iwx_bt_coex_complete_tx(iwx_softc *,iwx_tx_data *d,bool){assert(!d->coexToken);}
    void completeAWDLTx(iwx_tx_data *,bool,bool=false);
    void sampleAWDLRate(iwx_softc *,iwx_rx_packet *,iwx_tx_data *);
    void iwx_rx_tx_cmd_single(iwx_softc *,iwx_rx_packet *,iwx_tx_data *);
    void iwx_rx_tx_cmd(iwx_softc *,iwx_rx_packet *,iwx_rx_data *);
    void iwx_txd_done(iwx_softc *,iwx_tx_data *);
    void iwx_ampdu_txq_advance(iwx_softc *,iwx_tx_ring *,int);
    void iwx_reset_tx_ring(iwx_softc *,iwx_tx_ring *);
    uint8_t iwx_fw_valid_tx_ant(iwx_softc *) {return txAnt;}
    void iwx_toggle_tx_ant(iwx_softc *,uint8_t *);
    void iwx_clear_oactive(iwx_softc *sc,iwx_tx_ring *) {assert(sc->qfullmsk.empty());}
    void writeDoorbell(iwx_softc *sc,unsigned reg,unsigned value) {
#if AWDL_RADIO_CASE == 23
        if ((value>>16)==unsigned(sc->first_data_qid)) {
            assert(reg==IWX_HBUS_TARG_WRPTR && sc->txq[sc->first_data_qid].data[0].in==&infra);
            sc->doorbells.push_back(value);return;
        }
#endif
        assert(stationPresent && dmaOwned && awdlRadio.dwell.running);
        assert(reg==IWX_HBUS_TARG_WRPTR && (value>>16==7 || value>>16==8 ||
            (awdlRadio.blockAckTX.configured && value>>16==awdlRadio.blockAckTX.queue)));
        sc->doorbells.push_back(value);
    }
    void response(unsigned status=1,unsigned next=UINT32_MAX,uint8_t raTid=0) {
        iwx_rx_packet p={};p.length=sizeof(iwx_tx_resp)+4;
        auto t=reinterpret_cast<iwx_tx_resp *>(p.data);
        t->frame_count=1;t->tx_queue=7;t->status.status=status;t->ra_tid=raTid;
        if(next==UINT32_MAX)next=com.txq[7].cur;
        memcpy(&t->status+1,&next,4);iwx_rx_data rx;
        iwx_rx_tx_cmd(&com,&p,&rx);
    }
    int startAWDLRadio(const uint8_t *);int stopAWDLRadio();void resetAWDLRadio();int transmitAWDL(mbuf_t,uint64_t=0,uint64_t=0);
    bool awdlTxWindowOpen() const;
    int awdlMAC(uint32_t,bool=false);int awdlBinding(bool);int awdlRemoveQueue();
    bool awdlChannelAllowed(uint32_t,uint8_t=20) const;bool awdlWideChannelAllowed(uint32_t) const;int awdlPHY(uint16_t,uint8_t=20);int finishAWDLDwell();
    int prepareAWDLListener(uint16_t,uint8_t=20);int closeAWDLListenerWindow();
    int serviceAWDLListener(uint16_t,uint64_t,RadioState::GrantPolicy);
    int serviceAWDLChannel(uint16_t,uint64_t,RadioState::GrantPolicy = {});void receiveAWDLSession(const iwx_rx_packet *);
};
// PRODUCTION
static const uint8_t self[6]={2,0,0,0,0,1};

static void lifecycle() {
    ItlIwx d;d.loop.gated=false;
    assert(!d.startAWDLRadio(self) && d.awdlRadio.ready && !d.loop.gated);
    assert(d.commands==std::vector<std::string>({"awake","add-mac"}));
    d.loop.gated=true;
    assert(!d.serviceAWDLChannel(149,clockUS+65000));
    assert(d.awdlRadio.dwell.running && d.awdlRadio.queue==7);
    assert(d.commands==std::vector<std::string>({"awake","add-mac","remove-phy","add-phy","bind","add-station","queue","start-dwell"}));
    unsigned before=d.calls;assert(!d.serviceAWDLChannel(149,clockUS+65000)&&d.calls==int(before));
    auto flags=d.com.sc_flags;
    d.notify(0);assert(!d.awdlRadio.dwell.running);
    assert(!d.serviceAWDLChannel(0,clockUS+65000));
    assert(!d.awdlRadio.binding && !d.stationPresent && !d.dmaOwned && d.awdlRadio.ready);
    assert(d.commands[d.commands.size()-1]=="unbind");
    clockUS+=65536;
    assert(!d.serviceAWDLChannel(6,clockUS+65000));
    assert(d.com.sc_phyctxt[1].channel==&d.com.sc_ic.ic_channels[6]);
    assert(!d.stopAWDLRadio() && d.com.sc_flags==flags && !d.awdlRadio.ready && !d.dmaOwned);
    auto n=d.commands.size();assert(!d.stopAWDLRadio()&&d.commands.size()==n);
    d.com.sc_ic.ic_channels[149].ic_flags|=IEEE80211_CHAN_DFS;
    assert(!d.awdlChannelAllowed(149) && !d.awdlChannelAllowed(256) && !d.awdlChannelAllowed(65542));
    d.com.sc_ic.ic_state=0;assert(d.startAWDLRadio(self)==ENETDOWN);
    ItlIwx older;older.com.sc_enabled_capa[39/8]&=~(1U<<(39%8));
    assert(!older.startAWDLRadio(self));assert(!older.serviceAWDLChannel(6,clockUS+65000));
    assert(!older.stopAWDLRadio());
}
static void failures() {
    const unsigned stages[] = {0, RadioState::PowerAwake, RadioState::AddMAC,
        RadioState::RemovePHY, RadioState::ConfigurePHY, RadioState::AddBinding,
        RadioState::AddStation, RadioState::AllocateQueue, RadioState::StartSession};
    for(int call=1;call<=8;++call) {
        ItlIwx d;d.failAt=call;int e=d.startAWDLRadio(self);
        if(!e)e=d.serviceAWDLChannel(149,clockUS+65000);
        assert(e==EIO && !d.awdlRadio.ready && !d.awdlRadio.changing);
        assert(!d.dmaOwned && !d.stationPresent && !d.awdlRadio.binding);
        assert(d.awdlRadio.failure.stage==stages[call] && d.awdlRadio.failure.error==EIO);
        assert(d.awdlRadio.failure.status==UINT32_MAX);
        assert(d.awdlRadio.fault);
    }
    for(int call=1;call<=8;++call) {
        ItlIwx d;d.resetAt=call;int e=d.startAWDLRadio(self);
        if(!e)e=d.serviceAWDLChannel(149,clockUS+65000);
        assert(e==ENXIO && d.calls==call && !d.awdlRadio.ready);
    }
    for(int call=9;call<=14;++call) {
        ItlIwx d;assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(149,clockUS+65000));
        d.failAt=call;assert(d.stopAWDLRadio()==EIO && d.awdlRadio.fault && !d.awdlRadio.ready);
        if(call<=11)assert(d.dmaOwned);
        assert(d.startAWDLRadio(self)==EIO);
    }
    for(int call=9;call<=14;++call) {
        ItlIwx d;assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(149,clockUS+65000));
        d.resetAt=call;assert(d.stopAWDLRadio()==ENXIO && d.calls==call && !d.awdlRadio.ready);
    }
    ItlIwx d;assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(149,clockUS+65000));
    d.endOnCancel=false;
    assert(!d.stopAWDLRadio() && !d.dmaOwned && !d.awdlRadio.fault);
    assert(!d.awdlRadio.stopPending && !d.awdlRadio.dwell.requested);
    assert(d.awdlRadio.session.cancels==1 && d.awdlRadio.session.cancelAcknowledged==1);
    assert(d.awdlRadio.session.cancelCompletions==1 && !d.awdlRadio.session.endUS);
    assert(!d.awdlRadio.session.waits && !d.awdlRadio.session.waitTimeouts);
    ItlIwx cleanup;cleanup.failAt=2;cleanup.resetAt=3;
    assert(cleanup.startAWDLRadio(self)==ENXIO && cleanup.calls==3 && !cleanup.awdlRadio.fault);
    for(bool binding:{true,false}) {
        ItlIwx rejected;
        if(binding)rejected.bindingStatus=7;else rejected.stationStatus=2;
        assert(!rejected.startAWDLRadio(self));
        assert(rejected.serviceAWDLChannel(149,clockUS+65000)==EIO);
        assert(rejected.awdlRadio.failure.stage==(binding?RadioState::AddBinding:RadioState::AddStation));
        assert(rejected.awdlRadio.failure.status==(binding?7u:2u));
        auto failure=rejected.awdlRadio.failure;
        assert(rejected.startAWDLRadio(self)==EIO);
        assert(rejected.awdlRadio.failure.stage==failure.stage && rejected.awdlRadio.failure.status==failure.status);
    }
    ItlIwx doubleFailure;doubleFailure.failAt=2;
    doubleFailure.onCommand=[&](const std::string &name) {
        if(name=="remove-mac")doubleFailure.failAt=doubleFailure.calls;
    };
    assert(doubleFailure.startAWDLRadio(self)==EIO);
    assert(doubleFailure.awdlRadio.failure.stage==RadioState::AddMAC);
}
static Packet packet() {
    Packet p={};p.bytes[0]=8;AirportAWDL::copyAddress(p.bytes+10,self);AirportAWDL::copyAddress(p.bytes+16,AirportAWDL::BSSID);return p;
}
static void output() {
    ItlIwx d;Packet p=packet();assert(d.transmitAWDL(&p)==ENETDOWN&&p.frees==1);
    assert(!d.startAWDLRadio(self));d.startNotify=false;
    assert(!d.serviceAWDLChannel(149,clockUS+65000));
    p=packet();assert(d.transmitAWDL(&p)==ENETDOWN);
    d.notify(1,1,0);d.notify(1,1,1,0);d.notify(1,1,1,2,8);assert(!d.awdlRadio.dwell.running);
    d.notify(1);assert(d.awdlRadio.dwell.running);
    for(unsigned variant=0;variant<5;++variant) {
        p=packet();if(variant==1)p.bytes[1]=0x40;if(variant==2)p.bytes[10]=4;
        if(variant==3)p.bytes[16]=4;if(variant==4)p.total++;
        assert(d.transmitAWDL(&p)==(variant?EINVAL:0));
        if(!variant) {assert(!p.frees);d.response();}
        assert(p.frees==1);
    }
    for(unsigned variant=0;variant<8;++variant) {
        p=packet();p.bytes[4]=2;
        uint64_t token=31;
        switch(variant) {
        case 0:break; // First unicast DATA
        case 1:p.bytes[4]=1;break; // First multicast DATA
        case 2:p.bytes[1]=8;break; // Same-sequence unicast retry
        case 3:p.bytes[1]=8;token=0;break;
        case 4:p.bytes[1]=8;p.bytes[4]=1;break;
        case 5:p.bytes[1]=9;break;
        case 6:p.bytes[0]=0xd0;p.bytes[1]=8;break;
        case 7:p.bytes[0]=0xd0;p.bytes[4]=1;break;
        }
        assert(d.transmitAWDL(&p,token)==(variant<=2?0:EINVAL));
        if(variant<=2) {
            auto &ring=d.com.txq[7];auto &command=ring.cmd[ring.tail];
            auto &tx=*reinterpret_cast<iwx_tx_cmd_gen2 *>(command.data);
            auto header=reinterpret_cast<const uint8_t *>(&tx)+sizeof(tx);
            assert(header[1]==(variant==2?8:0) && ring.data[ring.tail].awdlToken==token);
            d.response();
        }
        assert(p.frees==1 && d.txResults.empty());
    }
    clockUS+=65000;p=packet();assert(d.transmitAWDL(&p)==ENETDOWN); // host slot ended, firmware END delayed
    assert(!d.serviceAWDLChannel(0,clockUS+65000));assert(!d.dmaOwned);
    clockUS+=8000;
    assert(!d.serviceAWDLChannel(44,clockUS+65000));
    d.notify(1,0);assert(!d.awdlRadio.dwell.running && !d.awdlRadio.dwell.done);
    auto n=d.calls;assert(!d.serviceAWDLChannel(44,clockUS+65000)&&d.calls>n);
    assert(!d.awdlRadio.dwell.requested);
    d.notify(0);d.notify(1);assert(!d.awdlRadio.dwell.running); // no resurrection after END
    assert(!d.serviceAWDLChannel(44,clockUS+65000));
    assert(!d.awdlRadio.dwell.requested); // The END-to-next-grant gap also covers failed START.
    clockUS+=8000;
    assert(!d.serviceAWDLChannel(44,clockUS+65000) && d.awdlRadio.dwell.requested);
    assert(!d.stopAWDLRadio());
    // Setup commands can outlive the requested slot: no late START command.
    ItlIwx late;assert(!late.startAWDLRadio(self));
    late.onCommand=[&](const std::string &name){if(name=="queue")clockUS+=70000;};
    assert(!late.serviceAWDLChannel(149,clockUS+65000));
    assert(!late.awdlRadio.dwell.requested && !late.dmaOwned);
    assert(std::find(late.commands.begin(),late.commands.end(),"start-dwell")==late.commands.end());
    assert(!late.stopAWDLRadio());
}
static void availabilityEvents() {
    ItlIwx d;d.startNotify=false;
    d.awdlAvailabilityChanged=[](void *owner) {
        auto p=static_cast<ItlIwx *>(owner);
        p->availability.push_back(p->awdlRadio.ready && p->awdlRadio.dwell.running);
    };
    assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(44,clockUS+65000));
    for(bool state:d.availability)assert(!state);
    unsigned count=d.availability.size();
    d.notify(1,1,0);d.notify(1,1,1,0);d.notify(1,1,1,2,8);assert(d.availability.size()==count);
    d.notify(1);assert(d.availability.back() && d.awdlRadio.dwellStarts==1);
    d.notify(1);assert(d.availability.back() && d.awdlRadio.dwellStarts==1);
    d.onCommand=[&](const std::string &name) {
        if(name=="cancel-dwell")assert(!d.availability.back());
    };
    assert(!d.finishAWDLDwell() && !d.availability.back());
    count=d.availability.size();d.notify(1);assert(d.availability.size()==count);
    clockUS+=65536;assert(!d.serviceAWDLChannel(6,clockUS+65000));d.notify(1);
    assert(d.availability.back());d.notify(0);assert(!d.availability.back());
    count=d.availability.size();d.notify(1);assert(d.availability.size()==count);
    clockUS+=65536;assert(!d.serviceAWDLChannel(149,clockUS+65000));d.notify(1);
    assert(d.availability.back());d.resetAWDLRadio();assert(!d.availability.back());
    count=d.availability.size();d.notify(1);assert(d.availability.size()==count);
}
static void endNotificationIdentity() {
    for(unsigned config:{0u,1u,2u,3u,4u,0xffffffffu}) {
        ItlIwx d;assert(!d.startAWDLRadio(self));
        assert(!d.serviceAWDLChannel(6,clockUS+65000));
        // The AWDL MAC owns only one outstanding firmware protection.
        d.notify(0,1,0,config);assert(!d.awdlRadio.dwell.done);
        d.notify(0,1,1,config,8);assert(!d.awdlRadio.dwell.done);
        d.notify(0,1,1,config);
        assert(d.awdlRadio.dwell.done && !d.awdlRadio.dwell.running);
        assert(d.awdlRadio.session.received==4 && d.awdlRadio.session.malformed==1);
        assert(d.awdlRadio.session.otherMAC==1 && d.awdlRadio.session.lastEndConfig==config);
        assert(!d.finishAWDLDwell() && !d.dmaOwned && !d.stationPresent);
        assert(std::find(d.commands.begin(),d.commands.end(),"cancel-dwell")==d.commands.end());
        auto ends=d.awdlRadio.dwellEnds;d.notify(0,1,1,config);d.notify(1);
        assert(d.awdlRadio.dwellEnds==ends && !d.awdlRadio.dwell.running);
        assert(d.awdlRadio.session.inactive==2);
    }
    ItlIwx d;d.startNotify=false;assert(!d.startAWDLRadio(self));
    assert(!d.serviceAWDLChannel(44,clockUS+65000));
    d.notify(1,1,1,4);assert(!d.awdlRadio.dwell.running);
    assert(d.awdlRadio.session.unexpectedStart==1);
    d.notify(1);assert(d.awdlRadio.dwell.running);
    // END can arrive during cancellation, before the command ACK resumes us.
    d.endOnCancel=false;d.onCommand=[&](const std::string &name) {
        if(name=="cancel-dwell")d.notify(0,1,1,4);
    };
    assert(!d.stopAWDLRadio() && !d.dmaOwned && !d.awdlRadio.fault);
    assert(d.awdlRadio.session.lastEndConfig==4 && !d.awdlRadio.session.waitTimeouts);
    assert(d.awdlRadio.session.cancelAcknowledged==1 && !d.awdlRadio.session.waits);
    // A late END after successful cancellation cannot revive a stopped radio.
    ItlIwx late;assert(!late.startAWDLRadio(self));
    assert(!late.serviceAWDLChannel(6,clockUS+65000));late.endOnCancel=false;
    assert(!late.stopAWDLRadio() && !late.dmaOwned && !late.awdlRadio.dwellEnds);
    late.notify(0,1,1,4);late.notify(1);
    assert(!late.awdlRadio.dwell.done && !late.awdlRadio.ready && !late.awdlRadio.fault);
    assert(!late.awdlRadio.dwellEnds && !late.awdlRadio.session.endUS && late.awdlRadio.session.inactive==2);
    assert(!late.stopAWDLRadio() && !late.dmaOwned && !late.awdlRadio.stopPending);
}
static std::vector<uint8_t> submittedFrame(ItlIwx &d,Packet &p,uint64_t token=0,uint32_t rate=0x4102) {
    auto &r=d.com.txq[7];const unsigned cursor=r.cur,slot=cursor%64;
    std::vector<uint8_t> expected(p.bytes,p.bytes+p.size);
    assert(!d.transmitAWDL(&p,token) && !p.frees);
    assert(r.cmd[slot].hdr.cmd==0x1c && r.cmd[slot].hdr.qid==7 && r.cmd[slot].hdr.idx==slot);
    const auto &tx=*reinterpret_cast<const iwx_tx_cmd_gen2 *>(r.cmd[slot].data);
    assert(tx.len==expected.size() && tx.flags==3 && tx.offload_assist==0x0c00);
    assert(tx.rate_n_flags==rate);
    const auto &desc=r.desc[slot];assert(desc.num_tbs==3);
    assert(desc.tbs[0].addr==r.data[slot].cmd_paddr && desc.tbs[0].tb_len==IWX_FIRST_TB_SIZE);
    assert(desc.tbs[1].addr==desc.tbs[0].addr+IWX_FIRST_TB_SIZE);
    assert(desc.tbs[0].tb_len+desc.tbs[1].tb_len==sizeof(iwx_cmd_header)+sizeof(tx)+24);
    assert(r.counts[slot]==(expected.size()+3)/4);
    const auto header=reinterpret_cast<const uint8_t *>(&tx)+sizeof(tx);
    std::vector<uint8_t> wire(header,header+24);
    const auto body=reinterpret_cast<const uint8_t *>(desc.tbs[2].addr);
    wire.insert(wire.end(),body,body+desc.tbs[2].tb_len);
    assert(wire==expected && r.data[slot].m==&p && !r.data[slot].in);
    assert(r.data[slot].flags==IWX_TXDATA_FLAG_AWDL && r.data[slot].awdlToken==token);
    assert(d.com.doorbells.back()==((7U<<16)|((cursor+1)%256)));
    assert(d.com.qfullmsk.empty() && !d.com.sc_ic.ic_wcl_tx_packets[0]);
    return wire;
}
static void transportRoundTrip() {
    using namespace AirportAWDL;
    ItlIwx d;d.startNotify=false;assert(!d.startAWDLRadio(self));
    const uint8_t remote[6]={2,0,0,0,0,2};
    uint64_t submitted=0;
    for(unsigned channel:{149u,6u,44u}) {
        const uint64_t end=clockUS+65536;
        SyncEngine engine;engine.start(self,channel,clockUS);
        Window window;assert(engine.window(clockUS,window) && window.channel==channel);
        assert(!d.serviceAWDLChannel(window.channel,end));
        Packet premature=packet();assert(d.transmitAWDL(&premature)==ENETDOWN && premature.frees==1);
        d.notify(1);assert(d.awdlRadio.dwell.running);
        PeerTable receiverPeers={};
        for(bool mif:{false,true}) {
            Packet p;p.size=p.total=engine.action(p.bytes,sizeof(p.bytes),clockUS,mif,nullptr,0);
            assert(p.size);auto wire=submittedFrame(d,p);++submitted;
            Advertisement advert={};assert(parseMPDU(wire.data(),wire.size(),advert)==ParseResult::Valid);
            assert(receiverPeers.update(self,remote,advert,clockUS,channel,-45));
            d.response();assert(p.frees==1 && !d.com.txq[7].queued);
            ++clockUS;
        }
        // The 64-entry DMA ring and 256-entry hardware cursor wrap independently.
        DataReceiveState receive={};
        for(unsigned n=0;n<300;++n) {
            uint8_t ethernet[54]={};copyAddress(ethernet,remote);copyAddress(ethernet+6,self);
            putBE16(ethernet+12,0x86dd);ethernet[14]=0x60;ethernet[21]=255;
            Packet p;p.size=p.total=dataFromEthernet(ethernet,sizeof(ethernet),p.bytes,sizeof(p.bytes),n,n);
            assert(p.size==80);auto wire=submittedFrame(d,p);++submitted;
            std::vector<uint8_t> delivered;
            assert(receive.receive(wire.data(),wire.size(),receiverPeers,remote,clockUS,false,
                [](void *context,const DataView &v) {
                    auto out=static_cast<std::vector<uint8_t> *>(context);out->resize(v.size+14);
                    assert(ethernetFromData(v,out->data(),out->size())==out->size());
                },&delivered));
            assert(delivered==std::vector<uint8_t>(ethernet,ethernet+sizeof(ethernet)));
            d.response();assert(p.frees==1 && !d.com.txq[7].queued);
        }
        assert(receive.observed==300 && d.awdlRadio.completed==submitted);
        d.notify(0,1,1,4);clockUS=end;
        assert(!d.serviceAWDLChannel(0,clockUS+65536));
        assert(!d.dmaOwned && !d.stationPresent && !d.com.txq[7].ring_count);
    }
    assert(!d.stopAWDLRadio() && d.awdlRadio.submitted==906 && d.awdlRadio.completed==906);
}
static void btCoexActionAntenna() {
    ItlIwx d;
    const iwl_cfg quz{2};
    d.com.sc_cfg=&quz;
    d.btCoex.enabled=true; d.btCoex.txMask=3;
    assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(6,clockUS+65536));
    assert(d.com.sc_phyctxt[0].channel==&d.com.sc_ic.ic_channels[48]);
    AirportAWDL::SyncEngine engine; engine.start(self,6,clockUS);
    auto action=[&](uint32_t rate) {
        Packet p;
        p.size=p.total=engine.action(p.bytes,sizeof(p.bytes),clockUS,false,nullptr,0);
        assert(p.size && p.bytes[0]==0xd0);
        for (unsigned i=4;i<10;++i) assert(p.bytes[i]==0xff);
        submittedFrame(d,p,0,rate);
        d.response(); assert(p.frees==1);
    };
    action(0x4102); // No high-traffic profile in this generation.
    d.btCoex.activity=3; action(0x8102);
    d.btCoex.txMask=2; action(0x8102);
    d.btCoex.txMask=1; action(0x4102);
    assert(!d.stopAWDLRadio());
}
static void managementAntennaFeedback() {
    const uint32_t chainA=0x4102, chainB=0x8102;
    const iwl_cfg cfg{2};
    for (uint8_t mask : {1,2,3}) {
        ItlIwx d;
        d.com.sc_cfg=&cfg;
        d.txAnt=mask; d.com.sc_mgmt_last_antenna_idx=mask==2 ? 1 : 0;
        const uint8_t apChain=d.com.sc_mgmt_last_antenna_idx;
        const uint32_t initial=apChain ? chainB : chainA;
        const uint32_t alternate=mask==3 ? chainB : initial;
        assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(6,clockUS+65536));
        auto &ring=d.com.txq[7];
        auto submit=[&](Packet &p,uint32_t rate,bool action=true,uint64_t token=0) {
            p=packet(); p.bytes[0]=action ? 0xd0 : 0x08; p.bytes[4]=token ? 2 : 0xff;
            AirportAWDL::putLE16(p.bytes+22,ring.cur<<4);
            const unsigned slot=ring.cur % ring.ring_count;
            submittedFrame(d,p,token,rate);
            assert(ring.data[slot].awdlAntennaMask==((rate & IWX_RATE_MCS_ANT_MSK) >> IWX_RATE_MCS_ANT_POS));
        };
        auto report=[&](unsigned status,unsigned next,uint8_t station=RadioState::StationID) {
            iwx_rx_packet p={};p.length=sizeof(iwx_tx_resp)+4;
            auto &r=*reinterpret_cast<iwx_tx_resp *>(p.data);
            const auto &command=ring.cmd[ring.tail];
            const auto &tx=*reinterpret_cast<const iwx_tx_cmd_gen2 *>(command.data);
            const auto *hdr=reinterpret_cast<const ieee80211_frame *>(&tx+1);
            r.frame_count=1;r.tx_queue=ring.qid;r.ra_tid=(station<<4)|IWX_MGMT_TID;
            r.byte_cnt=tx.len;r.frame_ctrl=hdr->i_fc[0];r.seq_ctl=AirportAWDL::le16(hdr->i_seq);
            r.status.status=status;r.status.sequence=(ring.qid<<8)|command.hdr.idx;
            memcpy(&r.status+1,&next,4);iwx_rx_data rx={};
            d.iwx_rx_tx_cmd(&d.com,&p,&rx);
            assert(d.com.sc_mgmt_last_antenna_idx==apChain);
        };

        Packet first,late,next;
        submit(first,initial); submit(late,initial);
        report(IWX_TX_STATUS_FAIL_SHORT_LIMIT,1);
        assert(first.frees==1 && !late.frees);
        submit(next,alternate);
        report(IWX_TX_STATUS_FAIL_SHORT_LIMIT,2); // The old A slot cannot roll the new choice back.
        report(IWX_TX_STATUS_SUCCESS,3);
        Packet wrongStation,flushed,drained,data,implicit;
        submit(wrongStation,alternate); report(IWX_TX_STATUS_FAIL_SHORT_LIMIT,4,0);
        submit(flushed,alternate); report(IWX_TX_STATUS_FAIL_FIFO_FLUSHED,5);
        submit(drained,alternate); report(IWX_TX_STATUS_FAIL_DRAIN_FLOW,6);
        submit(data,alternate,false);submit(implicit,alternate);
        report(IWX_TX_STATUS_FAIL_SHORT_LIMIT,8); // Only the data slot has an individual response.
        assert(data.frees==1 && implicit.frees==1);
        Packet direct;
        submit(direct,alternate,true,91);report(IWX_TX_STATUS_DIRECT_DONE,9);
        assert((d.txResults==std::vector<std::pair<uint64_t,bool>>{{91,true}}));
        Packet last;submit(last,initial);report(IWX_TX_STATUS_SUCCESS,10);
        if (mask==3) {
            d.btCoex.enabled=true;d.btCoex.txMask=3;d.btCoex.activity=3;
            Packet highBT;submit(highBT,chainB);report(IWX_TX_STATUS_FAIL_BT_RETRY,ring.cur);
            d.btCoex.activity=0;
            Packet lowBT;submit(lowBT,chainA);report(IWX_TX_STATUS_SUCCESS,ring.cur);
            for (unsigned status : {IWX_TX_STATUS_FAIL_HOST_ABORTED,IWX_TX_STATUS_FAIL_STA_INVALID,
                                    IWX_TX_STATUS_FAIL_RFKILL_FLUSH,IWX_TX_STATUS_FAIL_LIFE_EXPIRE}) {
                Packet rejected;submit(rejected,chainA);report(status,ring.cur);
                assert(d.awdlRadio.txAntennaMask==1);
            }
            // Completed packets may arrive while a confirmed grant or the radio is being retired.
            auto exclude=[&](bool &flag,bool value) {
                Packet pending;submit(pending,chainA);
                const bool old=flag;
                flag=value;report(IWX_TX_STATUS_FAIL_LONG_LIMIT,ring.cur);flag=old;
                assert(pending.frees==1 && d.awdlRadio.txAntennaMask==1);
            };
            exclude(d.awdlRadio.ready,false);
            exclude(d.awdlRadio.stopPending,true);
            exclude(d.awdlRadio.dwell.cancelling,true);
            Packet oldQueue;submit(oldQueue,chainA);
            d.awdlRadio.queue=8;report(IWX_TX_STATUS_FAIL_SHORT_LIMIT,ring.cur);d.awdlRadio.queue=ring.qid;
            assert(oldQueue.frees==1 && d.awdlRadio.txAntennaMask==1);
            Packet failed;submit(failed,chainA);report(IWX_TX_STATUS_FAIL_LONG_LIMIT,ring.cur);
            assert(d.awdlRadio.txAntennaMask==2);
        }
        assert(!d.stopAWDLRadio() && d.com.sc_mgmt_last_antenna_idx==apChain);
        if (mask==3) {
            clockUS+=65536;
            assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(6,clockUS+65536));
            Packet restarted;submit(restarted,chainA);report(IWX_TX_STATUS_FAIL_SHORT_LIMIT,ring.cur);
            assert(d.awdlRadio.txAntennaMask==2);
            d.resetAWDLRadio();
            assert(!d.awdlRadio.txAntennaMask && d.com.sc_mgmt_last_antenna_idx==apChain);
        }
    }
}
static void transportFailures() {
    ItlIwx d;assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(149,clockUS+65536));
    auto &r=d.com.txq[7];r.maps[0].fail=true;
    Packet failed=packet();assert(d.transmitAWDL(&failed)==ENOMEM && failed.frees==1);
    assert(!r.cur && !r.queued && d.com.doorbells.empty() && !d.awdlRadio.submitted);
    r.maps[0].fail=false;
    std::vector<Packet> pending(63);
    for(unsigned n=0;n<pending.size();++n) {
        pending[n]=packet();pending[n].bytes[0]=0xd0;pending[n].bytes[4]=2;
        submittedFrame(d,pending[n],n+1);
    }
    Packet full=packet();assert(d.transmitAWDL(&full)==ENOBUFS && full.frees==1);
    assert(r.queued==63 && d.com.doorbells.size()==63 && d.txResults.empty());
    // Batched completion: the first failed, later frames were acknowledged.
    d.response(0x82);assert(!r.queued && d.txResults.size()==63);
    for(unsigned n=0;n<63;++n) {
        assert(pending[n].frees==1 && d.txResults[n]==std::make_pair(uint64_t(n+1),n!=0));
    }
    d.response();assert(d.txResults.size()==63);
    Packet late=packet();late.bytes[0]=0xd0;late.bytes[4]=2;
    submittedFrame(d,late,64);d.endOnCancel=false;
    assert(!d.stopAWDLRadio() && !d.dmaOwned && late.frees==1 && !r.queued);
    assert(!r.ring_count && d.txResults.size()==64);
    d.notify(0,1,1,4);assert(!d.stopAWDLRadio());
    assert(late.frees==1 && !r.ring_count && !r.queued && d.txResults.size()==64);
    assert(d.txResults.back()==std::make_pair(uint64_t(64),false));
    d.response();assert(d.txResults.size()==64);
}
static unsigned commandCount(const ItlIwx &d,const char *name) {
    return std::count(d.commands.begin(),d.commands.end(),std::string(name));
}
static void cancellationWithoutEnd() {
    ItlIwx d;assert(!d.startAWDLRadio(self));
    for(unsigned n=0;n<3;++n) {
        assert(!d.serviceAWDLChannel(6,clockUS+65536));
        clockUS+=16384;d.notify(0);assert(!d.serviceAWDLChannel(0,clockUS+65536));
        clockUS=d.awdlRadio.nextGrantUS;
    }
    assert(!d.serviceAWDLChannel(6,clockUS+65000));
    assert(d.awdlRadio.dwellStarts==4 && d.awdlRadio.dwellEnds==3);
    std::vector<Packet> pending(3);
    for(unsigned n=0;n<pending.size();++n) {
        pending[n]=packet();pending[n].bytes[0]=0xd0;pending[n].bytes[4]=2;
        submittedFrame(d,pending[n],n+1);
    }
    const uint64_t firstAdd=d.awdlRadio.session.addUS;
    d.endOnCancel=false;
    d.onCommand=[&](const std::string &) {clockUS+=10;};
    assert(!d.serviceAWDLChannel(0,clockUS+65000));
    assert(d.awdlRadio.ready && !d.awdlRadio.fault && !d.awdlRadio.stopPending);
    assert(!d.awdlRadio.dwell.requested && !d.awdlRadio.dwell.done);
    assert(!d.stationPresent && !d.dmaOwned && !d.awdlRadio.binding && !d.com.txq[7].ring_count);
    assert(d.awdlRadio.dwellStarts==4 && d.awdlRadio.dwellEnds==3 && !d.awdlRadio.session.endUS);
    assert(d.awdlRadio.session.cancelCompletions==1 && !d.awdlRadio.session.deferred);
    assert(firstAdd && d.awdlRadio.session.startUS>=firstAdd);
    assert(d.awdlRadio.session.cancelAckUS>d.awdlRadio.session.startUS);
    assert(d.awdlRadio.session.flushAckUS>d.awdlRadio.session.cancelAckUS);
    assert(d.awdlRadio.session.cancelCompletedUS==d.awdlRadio.session.flushAckUS);
    assert(d.awdlRadio.nextGrantUS==d.awdlRadio.session.cancelCompletedUS+8000);
    assert(d.txResults.size()==3);
    for(unsigned n=0;n<3;++n)assert(pending[n].frees==1 && d.txResults[n]==std::make_pair(uint64_t(n+1),false));
    unsigned calls=d.calls;
    for(unsigned n=0;n<20;++n) {
        assert(!d.serviceAWDLChannel(n%2?6:44,clockUS+65000));
        Packet p=packet();assert(d.transmitAWDL(&p)==ENETDOWN && p.frees==1);
    }
    assert(d.calls==int(calls) && commandCount(d,"cancel-dwell")==1 && commandCount(d,"flush")==4);
    assert(commandCount(d,"start-dwell")==4 && !d.awdlRadio.session.deferred);
    assert(!d.awdlRadio.session.waits && !d.awdlRadio.session.waitTimeouts && !d.awdlRadio.failure.error);
    const uint64_t next=d.awdlRadio.nextGrantUS;
    d.notify(1);d.notify(0,1,0);d.notify(0,1,1,4);
    assert(!d.awdlRadio.dwell.running && !d.awdlRadio.dwell.done && d.awdlRadio.nextGrantUS==next);
    assert(d.awdlRadio.dwellEnds==3 && !d.awdlRadio.session.endUS);
    d.startNotify=false;clockUS=next;
    assert(!d.serviceAWDLChannel(44,clockUS+65000));
    assert(commandCount(d,"start-dwell")==5 && d.awdlRadio.session.addUS>=next);
    assert(!d.awdlRadio.session.startUS && !d.awdlRadio.session.cancelAckUS &&
        !d.awdlRadio.session.flushAckUS && !d.awdlRadio.session.endUS && !d.awdlRadio.session.cancelCompletedUS);
    assert(d.awdlRadio.dwell.requested && !d.awdlRadio.dwell.done && !d.awdlRadio.dwell.running);
    d.notify(1);assert(d.awdlRadio.dwell.running);
    assert(!d.stopAWDLRadio() && d.awdlRadio.dwellEnds==3);
}
static void cancellationEndOrdering() {
    for(unsigned phase=0;phase<3;++phase) {
        ItlIwx d;assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(6,clockUS+65000));
        d.endOnCancel=false;
        if(!phase) {clockUS+=10;d.notify(0);}
        d.onCommand=[&](const std::string &name) {
            clockUS+=10;
            if((phase==1 && name=="cancel-dwell") || (phase==2 && name=="flush")) {
                d.notify(0,1,1,4);
                assert(!d.awdlRadio.dwell.running);
            }
        };
        assert(!d.stopAWDLRadio());
        assert(!d.awdlRadio.fault && !d.awdlRadio.stopPending && !d.dmaOwned);
        assert(commandCount(d,"cancel-dwell")==unsigned(phase!=0) && commandCount(d,"flush")==1);
        assert(!d.awdlRadio.session.deferred && !d.awdlRadio.session.waits);
        assert(d.awdlRadio.session.endUS && d.awdlRadio.session.flushAckUS>=d.awdlRadio.session.endUS);
        assert(d.awdlRadio.session.cancelAcknowledged==unsigned(phase!=0));
    }
}
static void cancellationErrorsAndReset() {
    for(bool viaService:{false,true}) for(unsigned offset:{1u,2u}) {
        ItlIwx d;assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(6,clockUS+65000));
        Packet pending=packet();submittedFrame(d,pending);
        d.endOnCancel=false;d.failAt=d.calls+offset;
        const int result=viaService?d.serviceAWDLChannel(0,clockUS+65000):d.stopAWDLRadio();
        assert(result==EIO && d.awdlRadio.cleanupError==EIO && d.awdlRadio.fault);
        assert(d.calls==d.failAt); // fail -> stop must not immediately repeat the failed command.
        assert(!d.awdlRadio.stopPending && d.dmaOwned && d.stationPresent && !pending.frees);
        assert(d.awdlRadio.failure.stage==(offset==1?RadioState::CancelSession:RadioState::FlushStation));
        unsigned calls=d.calls;
        for(unsigned n=0;n<5;++n) {
            assert(d.stopAWDLRadio()==EIO && d.startAWDLRadio(self)==EIO);
            assert(d.finishAWDLDwell()==EIO);
        }
        d.notify(0);assert(d.stopAWDLRadio()==EIO && d.calls==int(calls) && !pending.frees);
        // Device reset owns the ring from this point; late notifications cannot revive it.
        ++d.dmaEpoch;d.iwx_reset_tx_ring(&d.com,&d.com.txq[7]);d.resetAWDLRadio();
        d.stationPresent=d.dmaOwned=false;
        assert(pending.frees==1 && !d.awdlRadio.cleanupError && !d.awdlRadio.stopPending && !d.awdlRadio.fault);
        d.notify(0);d.notify(1);assert(!d.awdlRadio.dwell.requested && !d.awdlRadio.dwell.running);
    }
    for(bool viaService:{false,true}) for(unsigned offset:{1u,2u}) {
        ItlIwx d;assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(6,clockUS+65000));
        d.endOnCancel=false;d.resetAt=d.calls+offset;
        const int result=viaService?d.serviceAWDLChannel(0,clockUS+65000):d.stopAWDLRadio();
        assert(result==ENXIO && d.calls==d.resetAt);
        assert(!d.awdlRadio.cleanupError && !d.awdlRadio.stopPending && !d.awdlRadio.fault && !d.awdlRadio.changing);
        assert(!d.awdlRadio.dwell.cancelAcknowledged && !d.awdlRadio.dwell.flushed);
    }
}
static void disableAndRestart() {
    ItlIwx d;assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(44,clockUS+65000));
    d.endOnCancel=false;assert(!d.stopAWDLRadio());
    assert(!d.awdlRadio.stopPending && !d.awdlRadio.ready && !d.awdlRadio.fault && !d.dmaOwned);
    const uint64_t next=d.awdlRadio.nextGrantUS;
    unsigned calls=d.calls;
    for(unsigned n=0;n<5;++n)assert(!d.stopAWDLRadio());
    assert(d.calls==int(calls));
    d.notify(0);assert(!d.awdlRadio.dwellEnds && !d.awdlRadio.session.endUS);
    assert(!d.startAWDLRadio(self) && d.awdlRadio.ready && commandCount(d,"add-mac")==2);
    assert(d.awdlRadio.nextGrantUS==next);
    assert(!d.serviceAWDLChannel(44,clockUS+65000) && !d.awdlRadio.dwell.requested);
    clockUS=next;assert(!d.serviceAWDLChannel(44,clockUS+65000) && d.awdlRadio.dwell.running);
    assert(!d.stopAWDLRadio());

    // Successful cleanup after a failed ADD must preserve the initiating fault.
    ItlIwx failed;failed.endOnCancel=false;
    failed.onCommand=[&](const std::string &name) {
        if(name=="start-dwell")failed.failAt=failed.calls;
    };
    assert(!failed.startAWDLRadio(self));
    assert(failed.serviceAWDLChannel(6,clockUS+65000)==EIO);
    assert(failed.awdlRadio.fault && !failed.awdlRadio.stopPending && !failed.awdlRadio.cleanupError);
    assert(!failed.dmaOwned && !failed.awdlRadio.dwell.requested && failed.awdlRadio.session.cancelCompletions==1);
    calls=failed.calls;assert(failed.startAWDLRadio(self)==EIO && failed.calls==int(calls));
    failed.notify(1);failed.notify(0);
    assert(!failed.awdlRadio.dwell.running && !failed.awdlRadio.dwellEnds && !failed.awdlRadio.session.endUS);
    assert(!failed.stopAWDLRadio() && failed.awdlRadio.fault && failed.calls==int(calls));
}
static void boundedGrantTiming() {
    ItlIwx d;assert(!d.startAWDLRadio(self));
    const uint64_t begin=clockUS,eawEnd=begin+65536;
    for(unsigned grant=0;grant<3;++grant) {
        assert(!d.serviceAWDLChannel(6,eawEnd));
        const auto &dw=d.awdlRadio.dwell;
        const uint64_t add=d.awdlRadio.session.addUS;
        assert(dw.running && dw.eawEndUS==eawEnd && dw.durationTU==16);
        assert(dw.confID==IWX_SESSION_PROTECT_CONF_P2P_GO_NEGOTIATION);
        assert(dw.windowEndUS==add+16*1024-3000 && d.awdlRadio.dwellStarts==grant+1);
        const unsigned beforeShift=d.calls;
        assert(!d.serviceAWDLChannel(6,eawEnd+1024) && d.calls==int(beforeShift));
        assert(dw.eawEndUS==eawEnd && dw.windowEndUS==add+16*1024-3000);
        // 32-byte MPDU at 12 Mbps: 7 OFDM symbols + 20-us PHY + 6-us extension.
        clockUS=dw.windowEndUS-54;
        Packet timely=packet();submittedFrame(d,timely);d.response();assert(timely.frees==1);
        for (unsigned remaining : {53u, 1u}) {
            clockUS=dw.windowEndUS-remaining;
            auto &ring=d.com.txq[7];const auto cursor=ring.cur;
            const auto descriptor=ring.desc[cursor % ring.ring_count];
            const auto doorbells=d.com.doorbells.size();const auto submitted=d.awdlRadio.submitted;
            Packet shortTime=packet();
            assert(d.transmitAWDL(&shortTime)==ENOBUFS && shortTime.frees==1);
            assert(ring.cur==cursor && !ring.queued && !ring.data[cursor % ring.ring_count].m);
            assert(!memcmp(&ring.desc[cursor % ring.ring_count],&descriptor,sizeof(descriptor)));
            assert(d.com.doorbells.size()==doorbells && d.awdlRadio.submitted==submitted);
        }
        clockUS=dw.windowEndUS;
        Packet guarded=packet();assert(d.transmitAWDL(&guarded)==ENETDOWN && guarded.frees==1);
        unsigned calls=d.calls;
        for(unsigned poll=0;poll<4;++poll) {
            assert(!d.serviceAWDLChannel(6,eawEnd) && d.calls==int(calls));
            ++clockUS;
        }
        assert(!commandCount(d,"cancel-dwell") && d.awdlRadio.dwell.running);
        clockUS=add+16*1024;d.notify(0,1,1,4);
        assert(d.awdlRadio.nextGrantUS==clockUS+8000);
        assert(!d.serviceAWDLChannel(6,eawEnd));
        assert(!d.dmaOwned && !d.awdlRadio.dwell.requested);
        calls=d.calls;clockUS=d.awdlRadio.nextGrantUS-1;
        assert(!d.serviceAWDLChannel(6,eawEnd) && d.calls==int(calls));
        assert(commandCount(d,"start-dwell")==grant+1);
        ++clockUS;
    }
    assert(!d.serviceAWDLChannel(6,eawEnd));
    assert(commandCount(d,"start-dwell")==3 && commandCount(d,"cancel-dwell")==0);
    assert(commandCount(d,"flush")==3 && d.awdlRadio.ready && !d.awdlRadio.fault);
    assert(!d.stopAWDLRadio());

    // A delayed START cannot extend the ADD-based transmit deadline.
    ItlIwx late;assert(!late.startAWDLRadio(self));
    late.onCommand=[&](const std::string &name) {if(name=="start-dwell")clockUS+=15000;};
    const uint64_t lateEnd=clockUS+65536;
    assert(!late.serviceAWDLChannel(44,lateEnd));
    assert(late.awdlRadio.session.startUS>=late.awdlRadio.dwell.windowEndUS);
    Packet stale=packet();assert(late.transmitAWDL(&stale)==ENETDOWN && stale.frees==1);
    unsigned calls=late.calls;
    assert(!late.serviceAWDLChannel(44,lateEnd) && late.calls==int(calls));
    assert(!commandCount(late,"cancel-dwell"));
    late.onCommand=nullptr;assert(!late.stopAWDLRadio());

    // Short tail grants floor TU once, then apply the host edge guard once.
    ItlIwx tail;assert(!tail.startAWDLRadio(self));
    const uint64_t tailAdd=clockUS;
    assert(!tail.serviceAWDLChannel(6,tailAdd+9000));
    assert(tail.awdlRadio.dwell.durationTU==8 && tail.awdlRadio.dwell.eawEndUS==tailAdd+9000);
    assert(tail.awdlRadio.dwell.windowEndUS==tailAdd+8192-3000);
    assert(!tail.stopAWDLRadio());
}
static void grantGapAndTransitions() {
    ItlIwx d;assert(!d.startAWDLRadio(self));
    const uint64_t eawEnd=clockUS+65536;
    assert(!d.serviceAWDLChannel(6,eawEnd));
    clockUS+=1000;d.notify(0);
    const uint64_t next=d.awdlRadio.nextGrantUS;
    assert(!d.stopAWDLRadio() && !d.startAWDLRadio(self));
    assert(d.awdlRadio.nextGrantUS==next);
    assert(!d.serviceAWDLChannel(44,eawEnd) && commandCount(d,"start-dwell")==1);
    clockUS=next-1;
    assert(!d.serviceAWDLChannel(44,eawEnd) && commandCount(d,"start-dwell")==1);
    clockUS=next;
    assert(!d.serviceAWDLChannel(44,eawEnd) && commandCount(d,"start-dwell")==2);
    d.endOnCancel=false;
    assert(!d.serviceAWDLChannel(6,eawEnd)); // Channel change cancels before the EAW ends.
    assert(commandCount(d,"cancel-dwell")==1 && d.awdlRadio.session.cancelCompletions==1);
    assert(!d.dmaOwned && !d.awdlRadio.dwell.requested && !d.awdlRadio.session.endUS);
    Packet wrongChannel=packet();assert(d.transmitAWDL(&wrongChannel)==ENETDOWN && wrongChannel.frees==1);
    clockUS=d.awdlRadio.nextGrantUS;
    assert(!d.serviceAWDLChannel(6,clockUS+65536));
    assert(commandCount(d,"start-dwell")==3 && d.dmaOwned);
    clockUS+=16384;d.notify(0);const uint64_t end=clockUS;
    d.onCommand=[&](const std::string &name) {if(name=="remove-station")clockUS+=10000;};
    assert(!d.serviceAWDLChannel(6,clockUS+65536));
    assert(commandCount(d,"start-dwell")==4 && d.awdlRadio.session.addUS>=end+8000);
    d.onCommand=nullptr;d.endOnCancel=true;
    const uint64_t newEnd=d.awdlRadio.dwell.eawEndUS;
    clockUS=newEnd;
    assert(!d.serviceAWDLChannel(6,clockUS+65536)); // Same channel, next EAW still cancels old ownership.
    assert(commandCount(d,"cancel-dwell")==2 && commandCount(d,"start-dwell")==4);
    Packet oldWindow=packet();assert(d.transmitAWDL(&oldWindow)==ENETDOWN && oldWindow.frees==1);
    assert(!d.stopAWDLRadio());
    const uint64_t saved=d.awdlRadio.nextGrantUS;assert(saved>clockUS);
    assert(!d.startAWDLRadio(self));assert(d.awdlRadio.nextGrantUS==saved);
    assert(!d.stopAWDLRadio());
    d.resetAWDLRadio();assert(!d.awdlRadio.nextGrantUS);
    assert(!d.startAWDLRadio(self));
    assert(!d.serviceAWDLChannel(6,clockUS+65536) && d.awdlRadio.dwell.running);
    assert(!d.stopAWDLRadio());
}
static void lateEndAndRepeatedCancellation() {
    ItlIwx d;d.endOnCancel=false;assert(!d.startAWDLRadio(self));
    for(unsigned n=0;n<5;++n) {
        assert(!d.serviceAWDLChannel(n%2?44:6,clockUS+65536));
        assert(d.awdlRadio.dwell.running);
        assert(!d.serviceAWDLChannel(0,clockUS+65536));
        assert(d.awdlRadio.ready && !d.awdlRadio.fault && !d.dmaOwned);
        assert(d.awdlRadio.session.cancelCompletions==n+1 && !d.awdlRadio.session.endUS);
        assert(!d.awdlRadio.dwellEnds && !d.awdlRadio.session.deferred);
        const uint64_t next=d.awdlRadio.nextGrantUS;
        d.notify(0);d.notify(1);assert(d.awdlRadio.nextGrantUS==next && !d.awdlRadio.dwellEnds);
        clockUS=next;
    }
    // No session cookie exists: an active-MAC END stops TX even before START.
    d.startNotify=false;assert(!d.serviceAWDLChannel(6,clockUS+65536));
    d.notify(0);assert(d.awdlRadio.dwell.done && !d.awdlRadio.dwell.running);
    d.notify(1);assert(!d.awdlRadio.dwell.running);
    assert(!d.serviceAWDLChannel(6,clockUS+65536) && !d.dmaOwned);
    clockUS=d.awdlRadio.nextGrantUS;d.startNotify=true;
    assert(!d.serviceAWDLChannel(6,clockUS+65536));
    Packet pending=packet();pending.bytes[0]=0xd0;pending.bytes[4]=2;submittedFrame(d,pending,91);
    d.notify(0);assert(d.awdlRadio.dwell.done && !d.awdlRadio.dwell.running);
    Packet rejected=packet();assert(d.transmitAWDL(&rejected)==ENETDOWN && rejected.frees==1);
    assert(!d.serviceAWDLChannel(6,clockUS+65536) && !d.dmaOwned && pending.frees==1);
    assert(d.txResults.size()==1 && d.txResults[0]==std::make_pair(uint64_t(91),false));
    assert(d.awdlRadio.dwellEnds==2 && d.awdlRadio.session.cancelCompletions==5);
    const uint64_t next=d.awdlRadio.nextGrantUS;d.notify(0);
    assert(d.awdlRadio.dwellEnds==2 && d.awdlRadio.nextGrantUS==next);
    clockUS=next;assert(!d.serviceAWDLChannel(44,clockUS+65536) && d.awdlRadio.dwell.running);
    assert(!d.stopAWDLRadio() && !d.awdlRadio.fault);
}
static void cancellationEmptyFlushOwnership() {
    for(bool failRemove:{false,true}) {
        ItlIwx d;d.endOnCancel=false;d.flushReclaims=false;
        assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(6,clockUS+65536));
        Packet pending=packet();pending.bytes[0]=0xd0;pending.bytes[4]=2;submittedFrame(d,pending,78);
        d.onCommand=[&](const std::string &name) {
            if(name=="remove-station") {
                assert(!pending.frees && d.txResults.empty() && d.dmaOwned && d.stationPresent);
                assert(d.com.txq[7].queued==1 && d.awdlRadio.dwell.cancelCompleted);
                if(failRemove)d.failAt=d.calls;
            }
        };
        const int result=d.stopAWDLRadio();
        if(failRemove) {
            assert(result==EIO && !pending.frees && d.txResults.empty());
            assert(d.awdlRadio.fault && d.awdlRadio.cleanupError==EIO && d.stationPresent && d.dmaOwned);
            assert(d.awdlRadio.failure.stage==RadioState::RemoveStation && d.com.txq[7].ring_count==64);
            const unsigned calls=d.calls;assert(d.stopAWDLRadio()==EIO && d.calls==int(calls));
            assert(!commandCount(d,"free-ring"));
            ++d.dmaEpoch;d.iwx_reset_tx_ring(&d.com,&d.com.txq[7]);d.resetAWDLRadio();
            d.stationPresent=d.dmaOwned=false;
        } else {
            assert(!result && !d.dmaOwned && !d.stationPresent && !d.com.txq[7].ring_count);
            assert(commandCount(d,"remove-station")==1 && commandCount(d,"free-ring")==1);
            assert(d.awdlRadio.session.cancelCompletions==1 && !d.awdlRadio.session.endUS);
        }
        assert(pending.frees==1 && d.txResults.size()==1 &&
            d.txResults[0]==std::make_pair(uint64_t(78),false));
        d.response();assert(pending.frees==1 && d.txResults.size()==1);
    }
}
static void phyReceiveConfigurationAndCache() {
    ItlIwx d;d.rlcVersion=2;d.rxAnt=3;
    assert(!d.startAWDLRadio(self));
    assert(!d.serviceAWDLChannel(149,clockUS+65536));
    assert(d.phyCommands.size()==2 && d.rlcCommands.size()==1);
    assert(d.phyCommands[0].action==IWX_FW_CTXT_ACTION_REMOVE && !d.phyCommands[0].ci.channel);
    assert(d.phyCommands[1].action==IWX_FW_CTXT_ACTION_ADD && d.phyCommands[1].ci.channel==149);
    assert(d.phyCommands[0].rxchain_info==0 && d.phyCommands[1].rxchain_info==0);
    assert(d.phyCommands[0].lmac_id==0 && d.phyCommands[1].lmac_id==1);
    assert(d.awdlRadio.phyReceiveAntennas==3 && d.awdlRadio.phyReceiveEpoch==d.dmaEpoch);
    auto at=std::find(d.commands.begin(),d.commands.end(),"rx-chains");
    assert(at!=d.commands.end() && at+1!=d.commands.end() && *(at+1)=="bind");
    d.notify(0);assert(!d.serviceAWDLChannel(0,clockUS+65536));
    assert(!d.stopAWDLRadio() && d.awdlRadio.phyReceiveAntennas==3);
    assert(!d.startAWDLRadio(self));clockUS=d.awdlRadio.nextGrantUS;
    assert(!d.serviceAWDLChannel(149,clockUS+65536));
    assert(d.phyCommands.size()==2 && d.rlcCommands.size()==1); // Ordinary stop/start retains PHY1.
    d.notify(0);assert(!d.serviceAWDLChannel(0,clockUS+65536));
    d.rxAnt=2;clockUS=d.awdlRadio.nextGrantUS;
    assert(!d.serviceAWDLChannel(149,clockUS+65536));
    assert(d.phyCommands.size()==3 && d.rlcCommands.size()==2 && d.awdlRadio.phyReceiveAntennas==2);
    d.notify(0);assert(!d.stopAWDLRadio());
    ++d.dmaEpoch;d.resetAWDLRadio();assert(!d.awdlRadio.phyReceiveAntennas && !d.awdlRadio.phyReceiveEpoch);
    assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(149,clockUS+65536));
    assert(d.phyCommands.size()==4 && d.rlcCommands.size()==3);
    assert(d.com.sc_phyctxt[0].channel==&d.com.sc_ic.ic_channels[48]);
    assert(!d.stopAWDLRadio());
    // RLC v1/absent firmware keeps the original inline PHY receive-chain ABI.
    for(unsigned version:{1u,unsigned(IWX_FW_CMD_VER_UNKNOWN)}) {
        ItlIwx old;old.rlcVersion=version;assert(!old.awdlPHY(149));
        assert(old.rlcCommands.empty() && old.phyCommands.back().rxchain_info!=0);
        assert(!old.awdlRadio.phyReceiveAntennas);
    }
}
static void phyReceiveFailuresAndRollback() {
    for(unsigned fail:{3u,4u,5u}) {
        ItlIwx d;d.rlcVersion=2;d.rxAnt=3;d.failAt=fail;
        assert(!d.startAWDLRadio(self));
        assert(d.serviceAWDLChannel(149,clockUS+65536)==EIO);
        assert(d.awdlRadio.fault && !d.awdlRadio.ready && !d.awdlRadio.mac);
        assert(!d.stationPresent && !d.dmaOwned && !d.awdlRadio.binding);
        assert(commandCount(d,"bind")==0 && commandCount(d,"add-station")==0);
        assert(d.com.sc_phyctxt[1].channel==&d.com.sc_ic.ic_channels[6]);
        if(fail==5) {
            assert(d.rlcCommands.size()==2 && d.phyCommands.size()==4);
            assert(d.phyCommands[2].action==IWX_FW_CTXT_ACTION_REMOVE && !d.phyCommands[2].ci.channel && d.phyCommands[2].lmac_id==1);
            assert(d.phyCommands[3].action==IWX_FW_CTXT_ACTION_ADD && d.phyCommands[3].ci.channel==6);
            assert(d.awdlRadio.failure.command==0x20508 && d.awdlRadio.phyReceiveAntennas==3);
        } else assert(d.rlcCommands.empty() && !d.awdlRadio.phyReceiveAntennas);
        auto calls=d.calls;assert(d.serviceAWDLChannel(149,clockUS+65536)==ENETDOWN && d.calls==calls);
    }
    // MODIFY with a changed same-band channel restores via MODIFY, not REMOVE.
    {
        ItlIwx d;d.rlcVersion=2;d.rxAnt=3;
        d.com.sc_ic.ic_channels[11]={8,2462};d.com.sc_ic.ic_chan_active[11/8]|=1U<<(11%8);
        assert(!d.startAWDLRadio(self));d.failAt=4;
        assert(d.serviceAWDLChannel(11,clockUS+65536)==EIO);
        assert(d.phyCommands.size()==2 && commandCount(d,"remove-phy")==0);
        assert(d.phyCommands[0].action==IWX_FW_CTXT_ACTION_MODIFY && d.phyCommands[0].ci.channel==11);
        assert(d.phyCommands[1].action==IWX_FW_CTXT_ACTION_MODIFY && d.phyCommands[1].ci.channel==6);
        assert(d.rlcCommands.size()==2 && d.awdlRadio.phyReceiveAntennas==3);
    }
    // A same-channel failure is terminal, not an automatic retry of the failed command.
    {
        ItlIwx d;d.rlcVersion=2;assert(!d.startAWDLRadio(self));d.failAt=4;
        assert(d.serviceAWDLChannel(6,clockUS+65536)==EIO);
        assert(d.rlcCommands.size()==1 && !d.awdlRadio.phyReceiveAntennas && d.awdlRadio.fault);
    }
    // Failure at each restore stage stops rollback and still cleans only AWDL.
    for(unsigned fail:{6u,7u,8u}) {
        ItlIwx d;d.rlcVersion=2;assert(!d.startAWDLRadio(self));
        d.onCommand=[&](const std::string &){if(d.calls==5 || d.calls==int(fail))d.failAt=d.calls;};
        assert(d.serviceAWDLChannel(149,clockUS+65536)==EIO);
        assert(d.awdlRadio.fault && d.awdlRadio.failure.command==0x20508);
        assert(!d.awdlRadio.phyReceiveAntennas && !d.awdlRadio.mac && !d.awdlRadio.binding);
        assert(!d.stationPresent && !d.dmaOwned && commandCount(d,"bind")==0);
        assert(d.com.sc_phyctxt[0].channel==&d.com.sc_ic.ic_channels[48]);
    }
}
static void phyReceiveResetAndDisable() {
    for(unsigned reset:{3u,4u,5u,6u,7u,8u}) {
        ItlIwx d;d.rlcVersion=2;assert(!d.startAWDLRadio(self));d.resetAt=reset;
        if(reset>=6)d.failAt=5;
        assert(d.serviceAWDLChannel(149,clockUS+65536)==ENXIO);
        assert(d.calls==int(reset) && d.dmaEpoch==2);
        assert(!d.awdlRadio.ready && !d.awdlRadio.fault && !d.awdlRadio.mac);
        assert(!d.awdlRadio.phyReceiveAntennas && !d.awdlRadio.phyReceiveEpoch);
        assert(d.awdlRadio.queue==IWX_INVALID_QUEUE && !d.stationPresent && !d.dmaOwned);
    }
    ItlIwx d;d.rlcVersion=2;assert(!d.startAWDLRadio(self));unsigned stops=0;
    d.onCommand=[&](const std::string &name){if(name=="rx-chains") {
        ++stops;assert(d.stopAWDLRadio()==EBUSY);assert(d.awdlRadio.changing);
        assert(!d.awdlRadio.binding && !d.stationPresent && !d.dmaOwned);
    }};
    assert(!d.serviceAWDLChannel(149,clockUS+65536));assert(stops==1);
    // Native tick retries its disabled request after the in-flight HAL call.
    assert(!d.stopAWDLRadio() && !d.awdlRadio.ready && !d.stationPresent && !d.dmaOwned);
    assert(!d.awdlRadio.mac && !d.awdlRadio.binding && d.awdlRadio.phyReceiveAntennas==2);
    assert(d.com.sc_phyctxt[0].channel==&d.com.sc_ic.ic_channels[48]);
}

#if DEBUG
static void txGrantAtPublication() {
    using namespace AirportAWDL;
    for (int scenario = 0; scenario < 7; ++scenario) {
        ItlIwx d;
        assert(!d.startAWDLRadio(self));
        const uint64_t eawStart = clockUS;
        clockUS += 34387;
        assert(!d.serviceAWDLChannel(6, eawStart + 65536));
        auto &radio = d.awdlRadio;
        auto &ring = d.com.txq[7];
        const uint64_t deadline = radio.dwell.windowEndUS;
        assert(deadline == eawStart + 47771);
        clockUS = deadline - 100;
        SyncEngine sync;
        sync.start(self, 6, eawStart);
        Packet p;
        p.size = p.total = sync.action(p.bytes, sizeof(p.bytes), clockUS, true, nullptr, 0);
        assert(p.size);
        uint64_t token = 0;
        if (scenario == 2 || scenario == 4) {
            p = packet();
            p.bytes[4] = 2;
            token = 17;
        }
        ring.data[0].map->onMap = [&] {
            switch (scenario) {
            case 0: clockUS = deadline - 1; break;
            case 1: clockUS = deadline; break;
            case 2: clockUS = deadline + 100; break;
            case 3: radio.dwell.cancelling = true; break;
            case 4: {
                const uint64_t original = radio.dwellStarts;
                d.notify(0);
                clockUS = radio.nextGrantUS;
                assert(!d.serviceAWDLChannel(6, clockUS + 65536));
                assert(radio.queue == 7 && radio.dwellStarts == original + 1);
                break;
            }
            case 5: d.resetAWDLRadio(); ++d.dmaEpoch; break;
            case 6: radio.queue = 8; break;
            }
        };
        const int error = d.transmitAWDL(&p, token);
        assert(error == (scenario == 0 ? ENOBUFS : ENETDOWN));
        assert(p.frees == 1 && !ring.cur && !ring.tail && !ring.queued);
        const auto &slot = ring.data[0];
        assert(!slot.m && !slot.flags && !slot.awdlToken && !slot.coexToken);
        assert(!ring.desc[0].num_tbs && !ring.counts[0] && d.com.doorbells.empty());
        assert(!radio.submitted && !d.awdlBroadcastReports.submitted && d.txResults.empty());
        if (scenario == 4) {
            Packet fresh = packet();
            fresh.bytes[4] = 2;
            submittedFrame(d, fresh, 18);
            d.response();
            assert(fresh.frees == 1 && !ring.queued && !slot.awdlToken);
        }
    }
}

static void broadcastFirmwareReports() {
    ItlIwx d;
    assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(6, clockUS + 65536));
    auto &ring = d.com.txq[7];
    auto &stats = d.awdlBroadcastReports;
    std::vector<Packet> packets(18);
    unsigned serial = 0;
    auto enqueue = [&](bool group, bool action) {
        auto &p = packets[serial];
        p = packet();
        p.bytes[0] = action ? 0xd0 : 0x08;
        if (group) std::memset(p.bytes + 4, 0xff, 6);
        else p.bytes[4] = 0x02;
        AirportAWDL::putLE16(p.bytes + 22, (++serial) << 4);
        assert(!d.transmitAWDL(&p));
    };
    iwx_rx_packet reply = {};
    auto *r = reinterpret_cast<iwx_tx_resp *>(reply.data);
    iwx_rx_data rx = {};
    auto send = [&]() {
        unsigned ssn = ring.cur;
        memcpy(&r->status + 1, &ssn, sizeof(ssn));
        d.iwx_rx_tx_cmd(&d.com, &reply, &rx);
    };
    reply.length = sizeof(*r) + 4;
    r->frame_count = 1; r->tx_queue = 7; r->ra_tid = 0x3f;
    r->frame_ctrl = 0xd0; r->status.status = IWX_TX_STATUS_SUCCESS;
    r->initial_rate = 0x8102; r->wireless_media_time = 184;
    r->failure_frame = 2; r->bt_kill_count = 3;
    r->byte_cnt = 32; r->seq_ctl = 16;

    // Real descriptor order spans the ring wrap. Only the first has a status.
    ring.cur = ring.tail = 63;
    enqueue(true, true); enqueue(false, true); enqueue(true, false); enqueue(true, true);
    assert(stats.submitted == 2);
    reply.length--;
    send();
    assert(stats.single == 0 && ring.queued == 4);
    reply.length++;
    send(); send();
    assert(stats.single == 1 && stats.withAirtime == 1 && stats.implicit == 1 && stats.success == 1);
    assert(stats.lastStatus == 1 && stats.lastRate == 0x8102 && stats.retries == 2 && stats.btKills == 3);

    // Reused slot with the old command's SSN, or a different firmware station.
    enqueue(true, true); send();
    enqueue(true, true); r->seq_ctl = serial << 4; r->ra_tid = 0x0f; send();
    assert(stats.unmatched == 2 && stats.single == 1);
    assert(stats.mismatch.sequence == 1 && stats.mismatch.station == 1 && stats.mismatch.last.raTid == 0x0f);
    enqueue(true, true); r->ra_tid = 0x3f; r->seq_ctl = serial << 4; r->frame_ctrl = 8; send();
    enqueue(true, true); r->frame_ctrl = 0xd0; r->byte_cnt = 256; r->seq_ctl = serial << 4; send();
    assert(stats.unmatched == 4 && stats.single == 1);
    assert(stats.mismatch.frameControl == 1 && stats.mismatch.byteCount == 1 && stats.mismatch.sequence == 1);
    assert(stats.mismatch.last.bytes == 256 && stats.mismatch.last.expectedBytes == 32);
    // A longer response with several contradictions stays uncorrelated. Each
    // failed check is visible even when the firmware's raw airtime is nonzero.
    enqueue(true, true); reply.length += 4;
    r->frame_ctrl = 8; r->ra_tid = 0xf3; send();
    const auto &bad = stats.mismatch;
    assert(stats.unmatched == 5 && stats.single == 1 && stats.withAirtime == 1);
    assert(bad.length == 1 && bad.station == 2 && bad.frameControl == 2 &&
        bad.byteCount == 2 && bad.sequence == 2);
    assert(bad.last.payload == sizeof(*r) + 8 && bad.last.raTid == 0xf3 && bad.last.fc == 8 &&
        bad.last.seq == (serial - 1) * 16 && bad.last.expectedSeq == serial * 16 &&
        bad.last.rate == 0x8102 && bad.last.airtime == 184 && bad.last.rawStatus == 1);
    reply.length -= 4; r->frame_ctrl = 0xd0; r->ra_tid = 0x3f;
    // Failed / direct-done responses can carry an unreliable sequence number.
    enqueue(true, true); r->status.status = IWX_TX_STATUS_DIRECT_DONE;
    r->wireless_media_time = 0; r->byte_cnt = 32; send();
    enqueue(true, true); r->status.status = IWX_TX_STATUS_FAIL_FIFO_FLUSHED; send();
    assert(stats.single == 3 && stats.directDone == 1 && stats.withAirtime == 1);
    assert(stats.lastStatus == IWX_TX_STATUS_FAIL_FIFO_FLUSHED && stats.flushed == 0);

    enqueue(true, true); r->status.status = IWX_TX_STATUS_SUCCESS; r->seq_ctl = serial << 4;
    d.awdlRadio.queue = 3; send(); d.awdlRadio.queue = 7;
    assert(stats.single == 3 && stats.unmatched == 5);
    enqueue(true, true); enqueue(true, false); enqueue(true, true); enqueue(false, true);
    stats.flush(ring, 2, 4);
    d.iwx_ampdu_txq_advance(&d.com, &ring, ring.cur % ring.ring_count);
    send();
    assert(!ring.queued && stats.implicit == 2 && stats.flushed == 1 && stats.single == 3);

    // A decorated SUCCESS may not echo the Wi-Fi sequence. A matching
    // embedded command ID does not override a conflicting MAC sequence.
    enqueue(true, true);
    const uint16_t commandSeq = uint16_t(ring.qid << 8 | ring.tail);
    r->status.status = IWX_TX_MODE_IN_BURST_SEQ | IWX_TX_STATUS_SUCCESS;
    r->status.sequence = commandSeq;
    r->seq_ctl = 0xeee0;
    r->wireless_media_time = 120;
    send();
    assert(stats.single == 3 && stats.withAirtime == 1 && stats.unmatched == 6);
    assert(bad.sequence == 3 && bad.last.rawStatus == 0x0101 && bad.last.airtime == 120);
    assert(bad.last.commandSeq == commandSeq && bad.last.heldCommandSeq == commandSeq);

    // Reproduce the d46 single-frame firmware report against the live TX ring.
    // The firmware's scheduler SSN consumes the held descriptor, but neither
    // sequence in the notification echoes the submitted command and MAC header.
    struct Registry {
        std::map<std::string,uint64_t> value;
        void setProperty(const char *k,uint64_t v,unsigned) {value[k]=v;}
        void setProperty(const char *,const char *) {}
    };
    auto properties = [&] { Registry values; stats.publish(values); return values.value; };
    Packet discovery = {};
    auto submitDiscovery = [&](bool broadcast = true) {
        discovery = packet();
        discovery.size = discovery.total = 334;
        discovery.bytes[0] = 0xd0;
        std::memset(discovery.bytes + 4, 0xff, 6);
        if (!broadcast) discovery.bytes[9] = 0xfe;
        AirportAWDL::putLE16(discovery.bytes + 22, 0x2a80);
        assert(!d.transmitAWDL(&discovery));
    };
    auto start = properties();
    const char *all = "BroadcastAFQueueAssociatedReports";
    const char *ok = "BroadcastAFQueueAssociatedSuccess";
    const char *timed = "BroadcastAFQueueAssociatedWithMediaTime";
    const char *time = "BroadcastAFQueueAssociatedMediaTimeUS";
    assert(start[all] == 5 && start[ok] == 3 && start[timed] == 3 && start[time] == 488);
    submitDiscovery();
    r->status.status = IWX_TX_STATUS_SUCCESS; r->status.sequence = 0;
    r->seq_ctl = 0x2a10; r->byte_cnt = 334; r->wireless_media_time = 248;
    send(); send();
    auto values = properties();
    assert(discovery.frees == 1 && !ring.queued && stats.single == 3 && stats.unmatched == 7);
    assert(bad.last.seq == 0x2a10 && bad.last.expectedSeq == 0x2a80 &&
        !bad.last.commandSeq && bad.last.heldCommandSeq != 0);
    assert(values[all] == start[all] + 1 && values[ok] == start[ok] + 1 &&
        values[timed] == start[timed] + 1 && values[time] == start[time] + 248);
    // A later exact match belongs to both cohorts. DIRECT_DONE with zero
    // media time only increments the queue-associated report count.
    submitDiscovery(); r->seq_ctl = 0x2a80; send();
    submitDiscovery(); r->status.status = IWX_TX_STATUS_DIRECT_DONE; r->wireless_media_time = 0; send();
    values = properties();
    assert(values[all] == start[all] + 3 && values[ok] == start[ok] + 2 &&
        values[timed] == start[timed] + 2 && values[time] == start[time] + 496);
    assert(stats.single == 5 && stats.unmatched == 7);
    r->status.status = IWX_TX_STATUS_SUCCESS; r->wireless_media_time = 248;
    submitDiscovery(); r->ra_tid = 0x3e; send(); r->ra_tid = 0x3f;
    submitDiscovery(); r->ra_tid = 0x1f; send(); r->ra_tid = 0x3f;
    submitDiscovery(); r->frame_ctrl = 0x08d0; send(); r->frame_ctrl = 0xd0;
    submitDiscovery(); r->byte_cnt--; send(); r->byte_cnt++;
    submitDiscovery(); reply.length += 4; send(); reply.length -= 4;
    submitDiscovery(false); send();
    submitDiscovery(); d.awdlRadio.queue = 3; send(); d.awdlRadio.queue = 7;
    submitDiscovery();
    const uint32_t staleSSN = (ring.cur + ring.ring_count) & 0xff;
    memcpy(&r->status + 1, &staleSSN, sizeof(staleSSN));
    d.iwx_rx_tx_cmd(&d.com, &reply, &rx);
    assert(ring.queued == 1 && !discovery.frees &&
        properties()[all] == values[all] && properties()[time] == values[time]);
    send();
    assert(!ring.queued && discovery.frees == 1 && properties()[all] == values[all] + 1);
}
#endif

#if DEBUG
static void compareBroadcastAntennas() {
    clockUS=90000000;
    enum Frame { BroadcastAction, GroupAction, UnicastAction, BroadcastData, APAction };
    struct Trial {
        uint64_t remaining, mapTime;
        Frame frame;
        unsigned channel, adaptive, expected, armA, armB, overrides;
        bool highBT=false;
        bool highBTAppearsDuringDMA=false;
    };
    const Trial trials[]={
        {0,0,BroadcastAction,6,2,2,0,0,0},                    // never armed
        {30000000,0,BroadcastAction,6,2,1,1,0,0},
        {15000000,0,BroadcastAction,6,1,2,0,1,0},
        {15000001,1,BroadcastAction,6,1,2,0,1,0},             // DMA crosses A -> B
        {1,1,BroadcastAction,6,1,1,0,0,0},                    // DMA crosses B -> expired
        {30000000,0,GroupAction,6,2,2,0,0,0},                 // ff:ff:ff:ff:ff:fe
        {30000000,0,UnicastAction,6,2,2,0,0,0},
        {30000000,0,BroadcastData,6,2,2,0,0,0},
        {30000000,0,APAction,6,2,2,0,0,0},
        {30000000,0,BroadcastAction,6,1,2,0,1,1,true},        // A overridden by non-shared B
        {15000000,0,BroadcastAction,6,1,2,0,1,0,true},
        {30000000,0,BroadcastAction,44,2,1,1,0,0,true},
        {30000000,0,BroadcastAction,6,1,2,0,1,1,false,true},
    };
    for(const auto &t:trials) {
        ItlIwx d;const iwl_cfg cfg{2};d.com.sc_cfg=&cfg;
        assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(t.channel,clockUS+65000));
        d.awdlRadio.txAntennaMask=t.adaptive;
        d.btCoex.enabled=true;d.btCoex.txMask=3;
        d.btCoex.activity=t.highBT?IwxBTCoexState::HighTraffic:0;
        d.awdlCompareEndUS=t.remaining?clockUS+t.remaining:0;
        Packet p=packet();p.bytes[0]=t.frame==BroadcastData?8:0xd0;
        std::memset(p.bytes+4,0xff,6);
        if(t.frame==GroupAction)p.bytes[9]=0xfe;
        if(t.frame==UnicastAction)p.bytes[4]=2;
        const bool ap=t.frame==APAction;
        auto &ring=d.com.txq[ap?d.com.first_data_qid:7];
        if(ap)ring.allocate(d.com.first_data_qid);
        const unsigned slot=ring.cur%ring.ring_count;
        ring.maps[slot].onMap=[&]{
            clockUS+=t.mapTime;
            if(t.highBTAppearsDuringDMA)d.btCoex.activity=IwxBTCoexState::HighTraffic;
        };
        assert(!(ap?d.iwx_tx(&d.com,&p,&d.infra.in_ni,0,-1,0):d.transmitAWDL(&p)));
        const auto &cmd=*reinterpret_cast<const iwx_tx_cmd_gen2 *>(ring.cmd[slot].data);
        assert(cmd.rate_n_flags==(0x102U | t.expected<<IWX_RATE_MCS_ANT_POS));
        assert(d.com.doorbells.size()==1 && ring.queued==1 && ring.data[slot].m==&p);
        assert(d.awdlCompareArmSubmitted[0]==t.armA && d.awdlCompareArmSubmitted[1]==t.armB);
        assert(d.awdlCompareBTOverrides==t.overrides && d.awdlRadio.txAntennaMask==t.adaptive);
        if(ap) {
            d.iwx_ampdu_txq_advance(&d.com,&ring,ring.cur);
            assert(d.infra.in_ni.releases==1);
        } else {
            assert(ring.data[slot].awdlAntennaMask==((t.armA||t.armB)?0:t.expected));
            d.response();
        }
        assert(p.frees==1 && !d.stopAWDLRadio());
        assert(!d.awdlCompareEndUS && !d.awdlCompareArmSubmitted[0] &&
            !d.awdlCompareArmSubmitted[1] && !d.awdlCompareBTOverrides);
    }
    {
        ItlIwx d;const iwl_cfg cfg{2};d.com.sc_cfg=&cfg;
        assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(6,clockUS+65000));
        d.awdlRadio.txAntennaMask=1;d.awdlCompareEndUS=clockUS+100;
        Packet p=packet();p.bytes[0]=0xd0;std::memset(p.bytes+4,0xff,6);
        submittedFrame(d,p,0,0x8102);d.response(); // Last 100us of B.
        p=packet();p.bytes[0]=0xd0;std::memset(p.bytes+4,0xff,6);
        clockUS+=100;submittedFrame(d,p,0,0x4102);d.response();
        assert(!d.awdlCompareArm() && !d.awdlCompareArmSubmitted[0] &&
            d.awdlCompareArmSubmitted[1]==1 && !d.awdlCompareBTOverrides);
        assert(!d.stopAWDLRadio());
    }
    // Active comparison never counts mapping failures or descriptors rejected after DMA.
    ItlIwx d;const iwl_cfg cfg{2};d.com.sc_cfg=&cfg;
    assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(6,clockUS+65000));
    d.awdlCompareEndUS=clockUS+30000000;
    for(bool failMap:{true,false}) {
        auto &map=d.com.txq[7].maps[0];
        map.fail=failMap;map.onMap=[&]{clockUS=d.awdlRadio.dwell.windowEndUS;};
        Packet p=packet();p.bytes[0]=0xd0;std::memset(p.bytes+4,0xff,6);
        assert(d.transmitAWDL(&p)==(failMap?ENOMEM:ENETDOWN));
        assert(p.frees==1 && !d.com.txq[7].queued && d.com.doorbells.empty());
        assert(!d.awdlCompareArmSubmitted[0] && !d.awdlCompareArmSubmitted[1] && !d.awdlCompareBTOverrides);
    }
    d.awdlCompareArmSubmitted[0]=1;d.awdlCompareArmSubmitted[1]=2;d.awdlCompareBTOverrides=1;
    d.resetAWDLRadio();
    assert(!d.awdlCompareEndUS && !d.awdlCompareArmSubmitted[0] &&
        !d.awdlCompareArmSubmitted[1] && !d.awdlCompareBTOverrides);
}
#endif

static void quz0034TransmitAntenna() {
    constexpr uint32_t rateA=0x4102,rateB=0x8102;
    const iwl_cfg cfg{2};
    ItlIwx d;
    d.com.sc_cfg=&iwl9560_quz_a0_jf_b0_cfg;
    d.com.sc_mgmt_last_antenna_idx=1;
    d.awdlRadio.quz0034=iwx_awdl_is_quz_0034(0x02f0,0x8086,0x0034,d.com.sc_cfg);
    assert(d.awdlRadio.quz0034);
    assert(!iwx_awdl_is_quz_0034(0x06f0,0x8086,0x0034,&iwl9560_quz_a0_jf_b0_cfg));
    assert(!iwx_awdl_is_quz_0034(0x02f0,0x1a56,0x0034,&iwl9560_quz_a0_jf_b0_cfg));
    assert(!iwx_awdl_is_quz_0034(0x02f0,0x8086,0x0074,&iwl9560_quz_a0_jf_b0_cfg));
    assert(!iwx_awdl_is_quz_0034(0x02f0,0x8086,0x0034,&cfg));
    const uint8_t peer[6]={2,3,4,5,6,7};
    auto action=[&](bool directed,uint32_t rate,unsigned status) {
        Packet p=packet();
        p.bytes[0]=0xd0;
        if (directed) memcpy(p.bytes+4,peer,6);
        else memset(p.bytes+4,0xff,6);
        submittedFrame(d,p,directed ? 92 : 0,rate);
        d.response(status,UINT32_MAX,(RadioState::StationID<<4)|IWX_MGMT_TID);
        assert(p.frees==1 && d.com.sc_mgmt_last_antenna_idx==1);
    };
    auto data=[&](bool group,uint32_t rate) {
        const uint8_t mdns[6]={0x33,0x33,0,0,0,0xfb};
        uint8_t ethernet[54]={};
        memcpy(ethernet,group ? mdns : peer,6);memcpy(ethernet+6,self,6);
        AirportAWDL::putBE16(ethernet+12,0x86dd);
        ethernet[14]=0x60;ethernet[21]=255;
        Packet p;
        p.size=p.total=AirportAWDL::dataFromEthernet(ethernet,sizeof(ethernet),p.bytes,sizeof(p.bytes),3,5);
        assert(p.size);
        submittedFrame(d,p,81,rate);d.response();
        assert(p.frees==1 && d.com.sc_mgmt_last_antenna_idx==1);
    };
    assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(6,clockUS+65536));
    action(false,rateA,IWX_TX_STATUS_DIRECT_DONE);
    action(true,rateA,IWX_TX_STATUS_FAIL_SHORT_LIMIT);
    data(false,rateA);data(true,rateA);
#if DEBUG
    d.awdlCompareEndUS=clockUS+14000000;
    action(false,rateB,IWX_TX_STATUS_DIRECT_DONE);
    action(true,rateA,IWX_TX_STATUS_SUCCESS);data(true,rateA);
    assert(d.awdlCompareArmSubmitted[1]==1 && d.awdlRadio.txAntennaMask==1);
    d.awdlCompareEndUS=0;
#endif
    d.btCoex.enabled=true;d.btCoex.txMask=3;
    auto &ring=d.com.txq[7];
    ring.maps[ring.cur % ring.ring_count].onMap=[&]() {d.btCoex.activity=3;};
    action(false,rateB,IWX_TX_STATUS_DIRECT_DONE);data(false,rateB);
    ring.maps[ring.cur % ring.ring_count].onMap=[&]() {d.btCoex.activity=0;};
    data(true,rateA);action(true,rateA,IWX_TX_STATUS_SUCCESS);
    assert(!d.stopAWDLRadio());
    clockUS+=65536;d.txAnt=2;
    assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(48,clockUS+65536));
    action(false,rateB,IWX_TX_STATUS_DIRECT_DONE);data(false,rateB);
    assert(!d.stopAWDLRadio());
    d.txAnt=0;
    assert(d.startAWDLRadio(self)==ENOTSUP);
}

static void txAirtimeAdmission() {
    // MPDU includes the 24-byte 802.11 header; firmware supplies its FCS.
    // Independently known OFDM symbol counts, without ACK/IFS/contention.
    assert(IwxAWDLTxBudget::ofdmUS(1540, 12, true) == 2090);
    assert(IwxAWDLTxBudget::ofdmUS(1540, 24, true) == 1058);
    assert(IwxAWDLTxBudget::ofdmUS(1540, 108, true) == 258);
    assert(IwxAWDLTxBudget::ofdmUS(1540, 108, false) == 252);
    const auto frame = [](unsigned bytes, bool action = false) {
        Packet p = packet();p.size = p.total = bytes;p.bytes[4] = 2;
        if (action) {p.bytes[0] = 0xd0;memset(p.bytes + 4, 255, 6);}
        return p;
    };
    for (unsigned version : {8u, 9u, unsigned(IWX_FW_CMD_VER_UNKNOWN)})
    for (unsigned rate : {6u, 12u, 54u}) for (unsigned wait : {0u, 7000u}) {
        clockUS = 10000000;
        ItlIwx d;d.txVersion = version;d.awdlMinRateMbps = rate;d.startNotify = false;
        assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(6, clockUS + 65536));
        assert(d.awdlRadio.dwell.durationTU == 16);
        const auto deadline = d.awdlRadio.dwell.windowEndUS;
        // Observed median host START delay; the production deadline stays fixed.
        clockUS += 4455;d.notify(1);clockUS += wait;
        auto &ring = d.com.txq[7];
        const unsigned cost = IwxAWDLTxBudget::ofdmUS(1540, rate * 2, true);
        const unsigned actionCost = IwxAWDLTxBudget::ofdmUS(32, rate * 2, true);
        const unsigned expected = (deadline - clockUS - actionCost) / cost;
        Packet action = frame(32, true);
        assert(!d.transmitAWDL(&action, 0) && ring.queued == 1);
        assert(!ring.data[0].awdlToken); // Tokenless management shares the queue.
        std::vector<Packet> packets(64);
        unsigned accepted = 0;
        for (unsigned n = 0; n < packets.size(); ++n) {
            packets[n] = frame(1540);
            const unsigned idx = ring.cur % ring.ring_count;
            const auto descriptor = ring.desc[idx];
            const auto doorbells = d.com.doorbells.size();
            const int error = d.transmitAWDL(&packets[n], n + 1);
            if (n < expected) {
                assert(!error && !packets[n].frees);++accepted;
                const auto tx = reinterpret_cast<const iwx_tx_cmd_gen2 *>(ring.cmd[idx].data);
                assert(tx->len == 1540 && packets[n].total == 1540 - 24);
                assert(IwxAWDLTxBudget::rate500Kbps(tx->rate_n_flags,
                    version > 8 && version != IWX_FW_CMD_VER_UNKNOWN) == rate * 2);
            } else {
                assert(error == ENOBUFS && packets[n].frees == 1 && !ring.data[idx].m);
                assert(!memcmp(&ring.desc[idx], &descriptor, sizeof(descriptor)));
                assert(d.com.doorbells.size() == doorbells);
            }
        }
        assert(accepted == expected && ring.queued == int(accepted + 1));
        assert(uint64_t(accepted) * cost + actionCost <= deadline - clockUS);
        assert(IwxAWDLTxBudget::queuedUS(ring, false,
            version > 8 && version != IWX_FW_CMD_VER_UNKNOWN, true) == accepted * cost + actionCost);
        assert(d.awdlRadio.dwell.windowEndUS == deadline && !d.awdlRadio.nextGrantUS);
        // Completion is asynchronous: no descriptor is reclaimed during admission.
        d.response();assert(!ring.queued && action.frees == 1);
        assert(!IwxAWDLTxBudget::queuedUS(ring, false, true, true));
        for (const auto &p : packets) assert(p.frees == 1);
        clockUS = deadline + 3000;d.notify(0);
        const auto nextGrant = d.awdlRadio.nextGrantUS;
        assert(nextGrant == clockUS + 8000);
        assert(!d.serviceAWDLChannel(6, clockUS + 65536) && !d.awdlRadio.dwell.running);
        clockUS = nextGrant;d.startNotify = true;
        assert(!d.serviceAWDLChannel(6, clockUS + 65536) && d.awdlRadio.dwell.running);
        Packet next = frame(1540);assert(!d.transmitAWDL(&next, 80));d.response();
        assert(next.frees == 1 && !d.stopAWDLRadio());
    }
    // Delayed host completion intentionally keeps a conservative reservation.
    // Reclaim releases it; an empty candidate's old command is never charged.
    clockUS = 20000000;
    ItlIwx d;assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(6, clockUS + 65536));
    auto &ring = d.com.txq[7];const auto deadline = d.awdlRadio.dwell.windowEndUS;
    const unsigned cost = 1058;
    clockUS = deadline - cost * 3;
    Packet first = frame(1540), second = frame(1540), blocked = frame(1540);
    assert(!d.transmitAWDL(&first, 1) && !d.transmitAWDL(&second, 2));
    clockUS += 100;
    assert(d.transmitAWDL(&blocked, 3) == ENOBUFS && ring.queued == 2);
    const auto tail = ring.tail;
    d.response(IWX_TX_STATUS_SUCCESS, tail + 1);
    assert(first.frees == 1 && !second.frees && ring.queued == 1);
    Packet resumed = frame(1540);assert(!d.transmitAWDL(&resumed, 3));
    d.response();assert(second.frees == 1 && resumed.frees == 1 && !ring.queued);
    // DMA time and a final selected-rate change both affect publication.
    d.awdlRadio.quz0034 = true;
    d.com.sc_cfg = &iwl9560_quz_a0_jf_b0_cfg;
    d.awdlMinRateMbps = 54;
    ring.maps[ring.cur % ring.ring_count].onMap = [&] {
        d.awdlMinRateMbps = 13; // Ceiling selects the actual 18-Mbps OFDM rate.
        clockUS = deadline - 714 + 1;
    };
    Packet late = frame(1540);const auto doorbells = d.com.doorbells.size();
    assert(d.transmitAWDL(&late, 4) == ENOBUFS && late.frees == 1 && !ring.queued);
    assert(d.com.doorbells.size() == doorbells);
    ring.maps[ring.cur % ring.ring_count].onMap = nullptr;
    clockUS = deadline - 714;
    Packet exact = frame(1540);assert(!d.transmitAWDL(&exact, 4));
    const auto tx = reinterpret_cast<const iwx_tx_cmd_gen2 *>(ring.cmd[ring.tail].data);
    assert(IwxAWDLTxBudget::rate500Kbps(tx->rate_n_flags, true) == 36);
    d.response();assert(exact.frees == 1 && !d.stopAWDLRadio());

    // Candidate-specific bounds are independent of the local radio grant.
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        clockUS = 30000000;
        ItlIwx bounded;
        assert(!bounded.startAWDLRadio(self) && !bounded.serviceAWDLChannel(6, clockUS + 65536));
        auto &queue = bounded.com.txq[7];
        const auto localEnd = bounded.awdlRadio.dwell.windowEndUS;
        uint64_t peerEnd = clockUS + cost;
        if (scenario == 0) peerEnd = clockUS + 2 * cost + 1;
        if (scenario == 1) {peerEnd = localEnd + 1000;clockUS = localEnd - cost + 1;}
        if (scenario == 2) queue.maps[0].onMap = [&] {clockUS = peerEnd;};
        if (scenario == 4) peerEnd = clockUS;
        Packet candidate = frame(1540);
        bounded.loop.gated = false; // The command-gate trampoline retains the bound.
        const int error = bounded.transmitAWDL(&candidate, 90, peerEnd);
        assert(!bounded.loop.gated);
        bounded.loop.gated = true;
        if (scenario == 0 || scenario == 3) {
            assert(!error && !candidate.frees && queue.queued == 1);
            if (scenario == 0) {
                clockUS += 2;Packet over = frame(1540);
                assert(bounded.transmitAWDL(&over, 91, peerEnd) == ENOBUFS);
                assert(over.frees == 1 && queue.queued == 1);
                Packet group = frame(1540);group.bytes[4] = 1;
                assert(!bounded.transmitAWDL(&group, 92));
                assert(bounded.awdlRadio.dwell.windowEndUS == localEnd);
                bounded.response();assert(group.frees == 1);
            } else bounded.response();
            assert(candidate.frees == 1);
        } else {
            assert(error == ENOBUFS && candidate.frees == 1 && !queue.queued);
            assert(!queue.data[0].m && !queue.desc[0].num_tbs && bounded.com.doorbells.empty());
        }
        assert(!bounded.stopAWDLRadio());
    }

    // Gen3 uses its own command size, with the same length/rate field offsets.
    // Radio startup remains restricted to the existing supported device family.
    iwx_tx_ring layout;layout.allocate(7);Packet owned = frame(1540);
    layout.data[0].m = &owned;layout.data[0].flags = IWX_TXDATA_FLAG_AWDL;
    auto gen3 = reinterpret_cast<iwx_tx_cmd_gen3 *>(layout.cmd[0].data);
    gen3->len = 1540;gen3->rate_n_flags = IWX_RATE_MCS_LEGACY_OFDM_MSK | 7;
    assert(sizeof(*gen3) == 28 && offsetof(iwx_tx_cmd_gen3, rate_n_flags) == 16);
    assert(sizeof(iwx_tx_cmd_gen2) == 20 && offsetof(iwx_tx_cmd_gen2, rate_n_flags) == 16);
    assert(IwxAWDLTxBudget::queuedUS(layout, true, true, false) == 252);
    gen3->rate_n_flags = IWX_RATE_MCS_HT_MSK;
    assert(IwxAWDLTxBudget::queuedUS(layout, true, true, false) == 1940);
    gen3->rate_n_flags = IWX_RATE_MCS_HT_MSK | 7;
    assert(IwxAWDLTxBudget::queuedUS(layout, true, true, true) == 234);
    gen3->rate_n_flags |= IWX_RATE_MCS_NSS_MSK;
    assert(IwxAWDLTxBudget::queuedUS(layout, true, true, false) == UINT64_MAX);
    for(unsigned mcs=0;mcs<8;++mcs) {
        const uint32_t a=IWX_RATE_MCS_HT_MSK|mcs,b=IWX_RATE_MCS_HT_MSK_V1|mcs;
        assert(IwxAWDLTxBudget::rateCode(a,true)==(0x80|mcs));
        assert(IwxAWDLTxBudget::rateCode(b,false)==(0x80|mcs));
        assert(IwxAWDLTxBudget::frameUS(1540,a,true,true)==IwxAWDLTxBudget::frameUS(1540,b,false,true));
        assert(!IwxAWDLTxBudget::rateCode(a|IWX_RATE_MCS_SGI_MSK,true));
        assert(!IwxAWDLTxBudget::rateCode(b|IWX_RATE_MCS_SGI_MSK_V1,false));
        assert(!IwxAWDLTxBudget::rateCode(a|IWX_RATE_MCS_CHAN_WIDTH_40,true));
        assert(!IwxAWDLTxBudget::rateCode(b|IWX_RATE_MCS_CHAN_WIDTH_40,false));
    }
    gen3->rate_n_flags = 0; // No valid legacy PLCP encoding.
    assert(IwxAWDLTxBudget::queuedUS(layout, true, false, false) == UINT64_MAX);
    layout.data[0].m = nullptr;
    assert(!IwxAWDLTxBudget::queuedUS(layout, true, false, false));
}

static void adaptiveDataRate() {
    clockUS=20000000;
    ItlIwx d;
    assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(6,clockUS+65000));
    auto &peer=d.awdlPeers.peers[0];peer.occupied=true;peer.lastSeenUS=clockUS;
    const uint8_t destination[6]={2,0,0,0,0,2};
    AirportAWDL::copyAddress(peer.address,destination);
    auto frame=[&]() {
        Packet p=packet();p.bytes[0]=8;p.size=p.total=1540;
        memcpy(p.bytes+4,destination,6);return p;
    };
    uint64_t token=100;
    auto submit=[&](Packet &p,unsigned rateCode) {
        const unsigned slot=d.com.txq[7].cur%64;
        assert(!d.transmitAWDL(&p,++token));
        auto *tx=reinterpret_cast<const iwx_tx_cmd_gen2 *>(d.com.txq[7].cmd[slot].data);
        assert(IwxAWDLTxBudget::rateCode(tx->rate_n_flags,true)==rateCode);
        return slot;
    };
    auto report=[&](unsigned status=IWX_TX_STATUS_SUCCESS,unsigned retries=0,
                    std::function<void(iwx_tx_resp &)> alter={}) {
        auto &ring=d.com.txq[7];
        auto *tx=reinterpret_cast<const iwx_tx_cmd_gen2 *>(ring.cmd[ring.tail].data);
        auto *header=reinterpret_cast<const ieee80211_frame *>(tx+1);
        iwx_rx_packet p{};p.length=sizeof(iwx_tx_resp)+4;
        auto &r=*reinterpret_cast<iwx_tx_resp *>(p.data);
        r.frame_count=1;r.tx_queue=7;r.ra_tid=0x30|IWX_MGMT_TID;
        r.status.status=status;r.failure_frame=retries;r.frame_ctrl=AirportAWDL::le16(header->i_fc);
        r.byte_cnt=tx->len;r.initial_rate=tx->rate_n_flags;r.wireless_media_time=540;
        r.seq_ctl=AirportAWDL::le16(header->i_seq);
        if(alter)alter(r);
        unsigned next=ring.cur;memcpy(&r.status+1,&next,4);iwx_rx_data rx;
        d.iwx_rx_tx_cmd(&d.com,&p,&rx);
    };
    for(unsigned i=0;i<8;++i) {auto p=frame();submit(p,12);report();assert(p.frees==1);}
    assert(d.awdlRateSamples==8 && !d.awdlRateProbes && peer.txRate.select()==24);
    auto probe=frame(),other=frame();submit(probe,24);submit(other,12);
    assert(d.awdlRateProbes==1 && peer.txRate.probeToken);
    assert(IwxAWDLTxBudget::queuedUS(d.com.txq[7],false,true,true)==
           IwxAWDLTxBudget::ofdmUS(1540,48,true)+IwxAWDLTxBudget::ofdmUS(1540,24,true));
    report();assert(probe.frees==1 && other.frees==1);
    assert(d.awdlRateSamples==9 && d.awdlRateRaised==1 && peer.txRate.current==24);
    auto fail=frame();submit(fail,24);
    report(IWX_TX_STATUS_FAIL_LONG_LIMIT,3,[](iwx_tx_resp &r) {r.initial_rate=0;});
    assert(peer.txRate.current==12 && d.awdlRateLowered==1 && d.awdlRateLastRetries==3);
    // Metadata mismatches, BT/RTS failures, cancellation and inferred success
    // cannot manufacture clean ACK samples or train a probe.
    for(unsigned bad=0;bad<11;++bad) {
        const auto before=d.awdlRateSamples;
        auto p=frame();submit(p,12);
        report(bad==4?IWX_TX_STATUS_DIRECT_DONE:bad==5?IWX_TX_STATUS_FAIL_FIFO_FLUSHED:
               bad>=6?IWX_TX_STATUS_FAIL_LONG_LIMIT:IWX_TX_STATUS_SUCCESS,bad>=7?3:0,
               [&](iwx_tx_resp &r) {
            if(bad==0)r.bt_kill_count=1;
            if(bad==1)r.failure_rts=1;
            if(bad==2)r.byte_cnt++;
            if(bad==3)r.initial_rate=0x4104;
            if(bad==7)++d.awdlRadio.dwellStarts;
            if(bad==8)clockUS=d.awdlRadio.dwell.windowEndUS;
            if(bad==9)peer.lastSeenUS=clockUS-AirportAWDL::PeerTable::LifetimeUS;
            if(bad==10)++d.dmaEpoch;
        });
        assert(p.frees==1 && d.awdlRateSamples==before && !peer.txRate.clean);
        clockUS=20000000;peer.lastSeenUS=clockUS;
    }
    // Final DMA-stage rate and airtime use the new native floor.
    auto &ring=d.com.txq[7];
    auto &map=*ring.data[ring.cur%64].map;
    map.onMap=[&] {d.awdlMinRateMbps=36;};
    auto p=frame();submit(p,36);map.onMap={};report();
    assert(peer.txRate.current==36 && d.awdlRateLastFlags==0x4105);
    // Same-address reuse during DMA does not attach the old selection to a new peer.
    d.awdlMinRateMbps=12;
    auto &resetMap=*ring.data[ring.cur%64].map;
    resetMap.onMap=[&] {
        peer.resetObservation();peer.occupied=true;peer.lastSeenUS=clockUS;
        AirportAWDL::copyAddress(peer.address,destination);
    };
    auto reused=frame();auto slot=submit(reused,12);resetMap.onMap={};
    assert(!ring.data[slot].awdlRatePeer);report();
    assert(!peer.txRate.epoch);
    // The command version and response version are independent encodings.
    d.txNotifVersion=6;
    auto oldReply=frame();submit(oldReply,12);
    const auto samples=d.awdlRateSamples;
    report(IWX_TX_STATUS_SUCCESS,0,[](iwx_tx_resp &r) {r.initial_rate=0x4005;r.ra_tid=0x30|IWX_TID_NON_QOS;});
    assert(d.awdlRateSamples==samples+1 && d.awdlRateLastFlags==0x4005);
    d.txNotifVersion=7;
    peer.metadata.fields|=AirportAWDL::HTCapabilities;peer.metadata.rxHTMcs=0xff;
    peer.htCapabilitiesReceivedUS=clockUS;
    for(unsigned next:{24u,0x83u,0x84u,0x85u,0x86u,0x87u}) {
        for(unsigned i=0;i<8;++i) {
            auto clean=frame();submit(clean,i==0 && next==24?12:peer.txRate.current);report();
        }
        auto probe=frame(),following=frame();
        auto previous=peer.txRate.current;submit(probe,next);submit(following,previous);
        report();assert(peer.txRate.current==next && probe.frees==1 && following.frees==1);
    }
    assert(d.awdlRateLastFlags==0x4207);
    auto htRetry=frame();submit(htRetry,0x87);report(IWX_TX_STATUS_FAIL_LONG_LIMIT,3);
    assert(peer.txRate.current==0x86);
    d.txNotifVersion=6;
    auto htV1=frame();submit(htV1,0x86);
    const auto htSamples=d.awdlRateSamples;
    report(IWX_TX_STATUS_SUCCESS,0,[](iwx_tx_resp &r) {r.initial_rate=0x4106;});
    assert(d.awdlRateSamples==htSamples+1 && peer.txRate.current==0x86);
    d.txNotifVersion=7;
    auto action=frame();action.bytes[0]=0xd0;
    auto actionSlot=submit(action,12);assert(!ring.data[actionSlot].awdlRatePeer);report();
    auto group=frame();group.bytes[4]|=1;
    auto groupSlot=submit(group,12);assert(!ring.data[groupSlot].awdlRatePeer);report();
    auto stale=frame();submit(stale,0x86);peer.metadata.rxHTMcs=0;
    const auto beforeStale=d.awdlRateSamples;report();
    assert(d.awdlRateSamples==beforeStale);
    peer.metadata.rxHTMcs=0xff;
    auto &expireMap=*ring.data[ring.cur%64].map;
    expireMap.onMap=[&] {peer.htCapabilitiesReceivedUS=clockUS-AirportAWDL::Peer::ScheduleFreshnessUS;};
    auto expired=frame();submit(expired,12);expireMap.onMap={};report();
    assert(peer.txRate.current==12 && !peer.txRate.htMcsMask);
    assert(!d.stopAWDLRadio());
}

static void setupAggregateRadio(ItlIwx &d) {
    assert(!d.startAWDLRadio(self));
    RadioState::GrantPolicy policy={};policy.dataQueue=true;
    assert(!d.serviceAWDLChannel(6,clockUS+65536,policy));
    assert(d.awdlRadio.dataQueueConfigured && d.awdlRadio.dataQueue==8 && d.com.txq[8].ring_count==64);
    auto &peer=d.awdlPeers.peers[0];peer.occupied=true;peer.lastSeenUS=clockUS;
    AirportAWDL::copyAddress(peer.address,self);peer.address[5]=2;
    peer.metadata.fields|=AirportAWDL::HTCapabilities;peer.metadata.rxHTMcs=0xff;
    peer.htCapabilitiesReceivedUS=clockUS;
    peer.txRate.prepare(d.dmaEpoch,6,12,d.awdlRateSerial,0xff);peer.txRate.current=0x87;
}
static Packet aggregatePacket() {
    uint8_t first[1514]={},second[1514]={};
    for(auto ethernet:{first,second}) {
        memcpy(ethernet,self,6);ethernet[5]=2;memcpy(ethernet+6,self,6);
        ethernet[12]=0x86;ethernet[13]=0xdd;ethernet[14]=0x60;
        AirportAWDL::putBE16(ethernet+18,1460);
    }
    first[1513]=0x55;second[1513]=0xaa;Packet p;
    p.size=p.total=AirportAWDL::dataPairFromEthernet(first,sizeof(first),second,sizeof(second),
        p.bytes,sizeof(p.bytes),4095,65535,0);
    assert(p.size==3088);return p;
}
static void aggregateReport(ItlIwx &d,unsigned bad=0) {
    auto &ring=d.com.txq[8];auto *tx=reinterpret_cast<const iwx_tx_cmd_gen2 *>(ring.cmd[ring.tail].data);
    const auto *header=reinterpret_cast<const ieee80211_frame *>(ring.cmd[ring.tail].data+
        (d.com.sc_device_family>=IWX_DEVICE_FAMILY_AX210?sizeof(iwx_tx_cmd_gen3):sizeof(*tx)));
    iwx_rx_packet p={};p.length=sizeof(iwx_tx_resp)+4;
    auto &r=*reinterpret_cast<iwx_tx_resp *>(p.data);
    r.frame_count=1;r.tx_queue=8;r.ra_tid=0x30;r.status.status=IWX_TX_STATUS_SUCCESS;
    r.initial_rate=tx->rate_n_flags;r.byte_cnt=tx->len;r.frame_ctrl=AirportAWDL::le16(header->i_fc);
    r.seq_ctl=AirportAWDL::le16(header->i_seq);
    if(bad==1)r.ra_tid=0x3f;
    if(bad==2)r.frame_ctrl=8;
    if(bad==3)r.byte_cnt--;
    if(bad==4)r.bt_kill_count=1;
    unsigned next=ring.cur;memcpy(&r.status+1,&next,4);iwx_rx_data rx;
    d.iwx_rx_tx_cmd(&d.com,&p,&rx);
}
static void aggregateTransport() {
    for(unsigned version:{8u,9u})for(bool gen3:{false,true}) {
        clockUS=30000000;ItlIwx d;d.txVersion=version;d.txNotifVersion=version==8?6:7;
        setupAggregateRadio(d);
        // Gen3 exercises the shared descriptor layout, not an additional device admission.
        if(gen3)d.com.sc_device_family=IWX_DEVICE_FAMILY_AX210;
        auto &ring=d.com.txq[8];auto p=aggregatePacket();
        const std::vector<uint8_t> expected(p.bytes,p.bytes+p.size);
        assert(!d.transmitAWDL(&p,100));
        assert(!d.com.txq[7].queued && ring.queued==1 && !p.frees && p.size==3062);
        const auto *tx=reinterpret_cast<const iwx_tx_cmd_gen2 *>(ring.cmd[0].data);
        const unsigned cmdSize=gen3?sizeof(iwx_tx_cmd_gen3):sizeof(*tx);
        const auto offload=gen3?reinterpret_cast<const iwx_tx_cmd_gen3 *>(tx)->offload_assist:tx->offload_assist;
        assert(tx->len==3088 && offload==0x4d00 && !(offload&0x2000));
        assert(IwxAWDLTxBudget::rateCode(tx->rate_n_flags,version==9)==0x87);
        const auto &desc=ring.desc[0];assert(desc.num_tbs==3);
        assert(desc.tbs[0].tb_len+desc.tbs[1].tb_len==sizeof(iwx_cmd_header)+cmdSize+26);
        const auto header=ring.cmd[0].data+cmdSize;
        std::vector<uint8_t> wire(header,header+26);
        const auto payload=reinterpret_cast<const uint8_t *>(desc.tbs[2].addr);
        wire.insert(wire.end(),payload,payload+desc.tbs[2].tb_len);
        assert(wire==expected && d.com.doorbells.back()==((8u<<16)|1));
        assert(ring.counts[0]==(gen3?3088:(3088+3)/4));
        assert(ring.data[0].awdlToken==100 && ring.data[0].awdlRatePeer==1);
        aggregateReport(d);assert(p.frees==1 && !ring.queued && d.awdlRateSamples==1);
        for(unsigned bad=1;bad<=4;++bad) {
            auto next=aggregatePacket();assert(!d.transmitAWDL(&next,100+bad));aggregateReport(d,bad);
            assert(next.frees==1 && !ring.queued && d.awdlRateSamples==1);
        }
        assert(!d.stopAWDLRadio() && !d.awdlRadio.dataQueueConfigured && !ring.ring_count);
    }
}
static void aggregateTransportLimits() {
    for(unsigned scenario=0;scenario<8;++scenario) {
        clockUS=40000000;ItlIwx d;setupAggregateRadio(d);
        auto &ring=d.com.txq[8];auto &peer=d.awdlPeers.peers[0];
        auto candidate=aggregatePacket();const auto initial=d.com.doorbells.size();
        if(scenario==0)candidate.bytes[24]=0; // unsupported ordinary QoS raw TX
        if(scenario==1)candidate.bytes[24]|=1; // queue is TID 0 only
        if(scenario==2)candidate.bytes[4]|=1;
        if(scenario==3)peer.txRate.current=24;
        if(scenario==4)ring.maps[0].onMap=[&] {peer.metadata.rxHTMcs=0;};
        if(scenario==5)ring.maps[0].onMap=[&] {clockUS=d.awdlRadio.dwell.windowEndUS;};
        if(scenario==6)ring.maps[0].fail=true;
        if(scenario==7)ring.maps[0].onMap=[&] {++d.awdlRadio.dwellStarts;};
        int err=d.transmitAWDL(&candidate,100);
        assert(err==(scenario<3?EINVAL:scenario<5?EAGAIN:scenario==6?ENOMEM:ENETDOWN));
        assert(candidate.frees==1 && !ring.queued && !ring.data[0].m && !ring.desc[0].num_tbs);
        assert(d.com.doorbells.size()==initial);
        assert(!d.stopAWDLRadio());
    }
    // Both TIDs share the same radio airtime; neither can spend the other's reservation twice.
    for(bool dataFirst:{false,true}) {
        clockUS=50000000;ItlIwx d;setupAggregateRadio(d);
        auto pair=aggregatePacket();Packet action=packet();
        action.size=action.total=1540;
        const unsigned aggregateCost=IwxAWDLTxBudget::frameUS(3088,IWX_RATE_MCS_HT_MSK|7,true,true);
        const unsigned actionCost=IwxAWDLTxBudget::ofdmUS(1540,24,true);
        const auto limit=d.awdlRadio.dwell.windowEndUS;
        clockUS=limit-aggregateCost-actionCost+1;
        if(dataFirst) {
            assert(!d.transmitAWDL(&pair,100));
            assert(d.transmitAWDL(&action)==ENOBUFS && action.frees==1);
            aggregateReport(d);
        } else {
            assert(!d.transmitAWDL(&action));
            assert(d.transmitAWDL(&pair,100)==ENOBUFS && pair.frees==1);
            d.response();
        }
        assert(pair.frees==1 && action.frees==1 && !d.stopAWDLRadio());
    }
}
static void aggregateQueueLifecycle() {
    for(unsigned scenario=0;scenario<5;++scenario) {
        clockUS=60000000;ItlIwx d;assert(!d.startAWDLRadio(self));
        d.onCommand=[&](const std::string &name) {
            if(name!="data-queue")return;
            if(scenario==0)d.failAt=d.calls;
            if(scenario==1) {++d.dmaEpoch;d.resetAWDLRadio();}
            if(scenario==2)clockUS+=65536;
            if(scenario==3)assert(d.stopAWDLRadio()==EBUSY);
        };
        RadioState::GrantPolicy policy={};policy.dataQueue=true;
        const int err=d.serviceAWDLChannel(6,clockUS+65536,policy);
        if(scenario==0)assert(err==EIO && !d.stationPresent && !d.dmaOwned && !d.awdlRadio.dataQueueConfigured);
        if(scenario==1) {
            assert(err==ENXIO && !d.awdlRadio.ready && !d.awdlRadio.dataQueueConfigured);
            // Device reset, not a command-time cleanup, stops DMA before ring release.
            d.stationPresent=d.dmaOwned=false;
            for(unsigned q:{7u,8u})if(d.com.txq[q].ring_count) {
                d.iwx_reset_tx_ring(&d.com,&d.com.txq[q]);d.iwx_free_tx_ring(&d.com,&d.com.txq[q]);
            }
        }
        if(scenario==2)assert(!err && !d.awdlRadio.dwell.requested && !d.awdlRadio.dataQueueConfigured);
        if(scenario>=3) {
            assert(!err && d.awdlRadio.dataQueueConfigured);
            d.onCommand={};d.flushReclaims=false;
            Packet management=packet();assert(!d.transmitAWDL(&management));
            assert(!d.stopAWDLRadio() && management.frees==1);
            assert(!d.com.txq[7].ring_count && !d.com.txq[8].ring_count);
        }
    }
}

static void aggregateLateQueue() {
    clockUS=70000000;ItlIwx d;assert(!d.startAWDLRadio(self));
    const auto end=clockUS+65536;
    assert(!d.serviceAWDLChannel(6,end));
    Packet management=packet();assert(!d.transmitAWDL(&management));
    const auto dwell=d.awdlRadio.dwell;
    const auto session=d.awdlRadio.session;
    const auto nextGrant=d.awdlRadio.nextGrantUS;
    const auto commands=d.commands.size();
    clockUS+=1000;
    RadioState::GrantPolicy policy={};policy.dataQueue=true;
    assert(!d.serviceAWDLChannel(6,end,policy));
    assert(d.awdlRadio.dataQueueConfigured && d.awdlRadio.dataQueue==8);
    assert(d.commands.size()==commands+1 && d.commands.back()=="data-queue");
    assert(d.awdlRadio.dwell.windowEndUS==dwell.windowEndUS &&
        d.awdlRadio.dwell.eawEndUS==dwell.eawEndUS &&
        d.awdlRadio.dwell.durationTU==dwell.durationTU &&
        d.awdlRadio.session.addUS==session.addUS &&
        d.awdlRadio.session.startUS==session.startUS &&
        d.awdlRadio.session.cancels==session.cancels &&
        d.awdlRadio.nextGrantUS==nextGrant);
    assert(!management.frees && d.com.txq[7].queued==1);
    auto &peer=d.awdlPeers.peers[0];peer.occupied=true;peer.lastSeenUS=clockUS;
    AirportAWDL::copyAddress(peer.address,self);peer.address[5]=2;
    peer.metadata.fields|=AirportAWDL::HTCapabilities;peer.metadata.rxHTMcs=0xff;
    peer.htCapabilitiesReceivedUS=clockUS;
    peer.txRate.prepare(d.dmaEpoch,6,12,d.awdlRateSerial,0xff);peer.txRate.current=0x87;
    auto aggregate=aggregatePacket();assert(!d.transmitAWDL(&aggregate,100));
    assert(d.com.txq[8].queued==1 && !aggregate.frees && !management.frees);
    assert(!d.serviceAWDLChannel(6,end) && !d.serviceAWDLChannel(6,end,policy));
    assert(d.commands.size()==commands+1 && d.com.txq[7].queued==1 && d.com.txq[8].queued==1);
    aggregateReport(d);d.response();
    assert(aggregate.frees==1 && management.frees==1 && d.awdlRateSamples==1);
    assert(!d.stopAWDLRadio() && !d.com.txq[7].ring_count && !d.com.txq[8].ring_count);
}

static void aggregateLateQueueFailures() {
    for(unsigned scenario=0;scenario<8;++scenario) {
        clockUS=80000000;ItlIwx d;assert(!d.startAWDLRadio(self));
        const auto end=clockUS+65536;
        RadioState::GrantPolicy policy={};policy.dataQueue=true;
        if(scenario==7) {
            d.onCommand=[&](const std::string &name) {if(name=="queue")clockUS=end-8000;};
            assert(!d.serviceAWDLChannel(6,end,policy));
            assert(std::count(d.commands.begin(),d.commands.end(),"data-queue")==0);
            assert(!d.awdlRadio.dwell.requested && !d.stationPresent && !d.dmaOwned);
            assert(!d.stopAWDLRadio());continue;
        }
        d.startNotify=scenario!=1;
        assert(!d.serviceAWDLChannel(6,end));
        Packet management=packet();
        if(scenario!=1)assert(!d.transmitAWDL(&management));
        const auto commands=d.commands.size();
        if(scenario==0)clockUS=d.awdlRadio.dwell.windowEndUS-8000;
        d.onCommand=[&](const std::string &name) {
            if(name!="data-queue")return;
            if(scenario==2)d.failAt=d.calls;
            if(scenario==3) {++d.dmaEpoch;d.resetAWDLRadio();}
            if(scenario==4)d.notify(0);
            if(scenario==5)clockUS=d.awdlRadio.dwell.windowEndUS;
            if(scenario==6) {
                assert(d.serviceAWDLChannel(6,end,policy)==EBUSY);
                assert(d.stopAWDLRadio()==EBUSY);
            }
        };
        const int error=d.serviceAWDLChannel(6,end,policy);
        d.onCommand={};
        if(scenario<2) {
            assert(!error && !d.awdlRadio.dataQueueConfigured && d.commands.size()==commands);
        } else if(scenario==2) {
            assert(error==EIO && d.awdlRadio.fault && !d.awdlRadio.ready);
            assert(management.frees==1 && !d.stationPresent && !d.dmaOwned);
        } else if(scenario==3) {
            assert(error==ENXIO && !d.awdlRadio.ready && !d.awdlRadio.dataQueueConfigured);
            d.stationPresent=d.dmaOwned=false;
            for(unsigned q:{7u,8u})if(d.com.txq[q].ring_count) {
                d.iwx_reset_tx_ring(&d.com,&d.com.txq[q]);d.iwx_free_tx_ring(&d.com,&d.com.txq[q]);
            }
        } else {
            assert(!error && d.awdlRadio.dataQueueConfigured && !management.frees);
            assert(d.commands.size()==commands+1);
            if(scenario==4)assert(d.awdlRadio.dwell.done && !d.awdlRadio.dwell.running);
            if(scenario==5)assert(!d.awdlTxWindowOpen());
        }
        assert(!d.stopAWDLRadio());
        if(scenario!=1)assert(management.frees==1);
        assert(!d.com.txq[7].ring_count && !d.com.txq[8].ring_count);
    }
}

static void listener_keeps_receive_context() {
    ItlIwx d;
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    d.com.sc_enabled_capa[54 / 8] &= ~(1U << (54 % 8));
    assert(!d.startAWDLRadio(self));
    // Listening must exist before the first local transmit window.
    assert(d.awdlRadio.listener && d.awdlRadio.binding && d.stationPresent && d.dmaOwned);
    assert(d.awdlRadio.listenerPromisc && d.awdlRadio.listenerFilterKnown);
    assert(d.macCommands.front().filter_flags == (IWX_MAC_FILTER_ACCEPT_GRP |
        IWX_MAC_FILTER_IN_PROMISC | IWX_MAC_FILTER_IN_CONTROL_AND_MGMT));
    assert(!d.macCommands.front().cck_rates && d.macCommands.front().ofdm_rates==0x15);
    const auto installed=d.calls;
    assert(!d.awdlTxWindowOpen());
    auto blocked=packet();assert(d.transmitAWDL(&blocked)==ENETDOWN && blocked.frees==1);
    assert(!d.serviceAWDLChannel(0,clockUS+65536) && d.calls==installed);
    RadioState::GrantPolicy policy={};policy.slot=11;
    const uint64_t end=clockUS+65536;
    assert(!d.serviceAWDLChannel(149,end,policy) && d.calls==installed);
    assert(d.awdlTxWindowOpen() && !d.awdlRadio.dwell.requested);
    assert(d.awdlRadio.dwell.windowEndUS==end-3000);
    d.notify(0);d.notify(1,0);
    assert(d.awdlTxWindowOpen() && d.awdlRadio.session.inactive==2);
    auto p=packet();assert(!d.transmitAWDL(&p,1001) && d.com.txq[7].queued==1);
    clockUS=end-3000;assert(!d.awdlTxWindowOpen());
    assert(!d.serviceAWDLChannel(149,end,policy));
    assert(p.frees==1 && !d.com.txq[7].queued && d.com.txq[7].cur==1);
    assert(d.awdlRadio.binding && d.stationPresent && d.dmaOwned && d.awdlRadio.ready);
    assert(d.awdlRadio.dwellStarts==1 && d.awdlRadio.dwellEnds==1);
    auto idle=d.calls;
    for(unsigned n=0;n<10;++n)assert(!d.serviceAWDLChannel(0,end) && d.calls==idle);
    clockUS=end+65536;policy.slot=13;
    assert(!d.serviceAWDLChannel(149,clockUS+65536,policy));
    assert(d.awdlTxWindowOpen() && d.com.txq[7].cur==1 && d.calls==idle);
    assert(d.awdlRadio.dwellStarts==2 && commandCount(d,"start-dwell")==0);
    assert(commandCount(d,"add-station")==1 && commandCount(d,"queue")==1);
    assert(commandCount(d,"flush")==1 && !commandCount(d,"unbind") && !commandCount(d,"free-ring"));
    assert(!d.stopAWDLRadio());
    assert(!d.dmaOwned && !d.stationPresent && !d.awdlRadio.binding);
    assert(d.awdlRadio.dwellEnds==2 && commandCount(d,"remove-station")==1 && commandCount(d,"unbind")==1);
}

static void listener_retune_and_filter() {
    ItlIwx d;d.com.sc_enabled_capa[44/8]|=1U<<(44%8);
    assert(!d.startAWDLRadio(self));
    assert(!d.serviceAWDLChannel(149,clockUS+65536));
    auto pending=packet();assert(!d.transmitAWDL(&pending,1002));
    assert(!d.serviceAWDLChannel(6,clockUS+65536));
    assert(pending.frees==1 && d.awdlRadio.channel==6 && d.awdlTxWindowOpen());
    assert(d.awdlRadio.binding && !commandCount(d,"remove-station") && !commandCount(d,"unbind"));
    assert(!commandCount(d,"free-ring") && commandCount(d,"add-station")==1);
    assert(d.macCommands.back().cck_rates==0x0f && d.awdlRadio.listenerPromisc);
    assert(!d.serviceAWDLChannel(48,clockUS+65536));
    assert(!d.awdlRadio.listenerPromisc && d.macCommands.back().filter_flags==
        (IWX_MAC_FILTER_ACCEPT_GRP|IWX_MAC_FILTER_IN_CONTROL_AND_MGMT));
    assert(!d.serviceAWDLChannel(0,clockUS+65536));
    // A changed AP PHY must refresh filtering even while our TX schedule is idle.
    auto other=d.com.sc_phyctxt[0];other.channel=&d.com.sc_ic.ic_channels[6];
    d.infra.in_phyctxt=&other;
    assert(!d.serviceAWDLChannel(0,clockUS+65536) && d.awdlRadio.listenerPromisc);
    auto calls=d.calls;
    d.com.sc_ic.ic_channels[149].ic_flags|=IEEE80211_CHAN_PASSIVE;
    assert(d.serviceAWDLChannel(149,clockUS+65536)==EACCES && d.calls==calls);
    assert(!d.stopAWDLRadio());
}

static void listener_start_failures() {
    for(unsigned scenario=0;scenario<4;++scenario)for(int call=1;call<=7;++call) {
        ItlIwx d;d.com.sc_enabled_capa[44/8]|=1U<<(44%8);
        if(scenario==0)d.failAt=call;
        if(scenario==1)d.resetAt=call;
        d.onCommand=[&](const std::string &) {
            if(d.calls!=call)return;
            if(scenario==2)assert(d.stopAWDLRadio()==EBUSY);
            if(scenario==3)d.com.sc_ic.ic_bss=nullptr;
        };
        const int error=d.startAWDLRadio(self);
        assert(error==(scenario==0?EIO:scenario==1?ENXIO:ECANCELED));
        assert(!d.awdlRadio.ready && !d.awdlRadio.changing && !d.stationPresent && !d.dmaOwned);
        assert(!d.awdlRadio.binding && !d.awdlRadio.stopPending);
        if(scenario==1)assert(d.calls==call);
        else assert(d.awdlRadio.fault==(scenario==0));
    }
    for(bool binding:{true,false}) {
        ItlIwx d;d.com.sc_enabled_capa[44/8]|=1U<<(44%8);
        if(binding)d.bindingStatus=7;else d.stationStatus=2;
        assert(d.startAWDLRadio(self)==EIO && !d.awdlRadio.ready);
        assert(d.awdlRadio.failure.stage==(binding?RadioState::AddBinding:RadioState::AddStation));
        assert(d.awdlRadio.failure.status==(binding?7u:2u));
        assert(!d.dmaOwned && !d.stationPresent);
    }
}

static void listener_queue_and_deadline() {
    for(unsigned scenario=0;scenario<6;++scenario) {
        ItlIwx d;d.com.sc_enabled_capa[44/8]|=1U<<(44%8);
        assert(!d.startAWDLRadio(self));
        uint64_t end=clockUS+65536;
        RadioState::GrantPolicy policy={};policy.dataQueue=true;policy.context=&end;
        policy.windowEnd=[](const void *p,uint64_t,uint16_t,uint16_t){return *static_cast<const uint64_t *>(p);};
        assert(!d.serviceAWDLChannel(149,end));
        auto pending=packet();assert(!d.transmitAWDL(&pending,1003));
        auto calls=d.calls;
        d.onCommand=[&](const std::string &name) {
            if(name!="data-queue")return;
            if(scenario==1)clockUS=end;
            if(scenario==2)end=0;
            if(scenario==3)assert(d.stopAWDLRadio()==EBUSY);
            if(scenario==4)d.com.sc_ic.ic_bss=nullptr;
            if(scenario==5)end=clockUS+20000;
        };
        int error=d.serviceAWDLChannel(149,end,policy);
        assert(error==((scenario==3||scenario==4)?ECANCELED:0));
        if(scenario==0||scenario==5) {
            assert(d.awdlTxWindowOpen() && !pending.frees);
            assert(d.calls==calls+1 && d.awdlRadio.dataQueueConfigured);
            if(scenario==5)assert(d.awdlRadio.dwell.windowEndUS==end-3000);
            assert(!d.serviceAWDLChannel(0,clockUS+65536));
        }
        assert(!d.awdlTxWindowOpen() && pending.frees==1);
        if(scenario==1||scenario==2)assert(d.awdlRadio.ready && d.awdlRadio.binding && d.dmaOwned);
        assert(!d.stopAWDLRadio() && !d.dmaOwned && !d.awdlRadio.binding);
    }
    ItlIwx failed;failed.com.sc_enabled_capa[44/8]|=1U<<(44%8);
    assert(!failed.startAWDLRadio(self));assert(!failed.serviceAWDLChannel(149,clockUS+65536));
    auto p=packet();assert(!failed.transmitAWDL(&p,1004));
    failed.failAt=failed.calls+1;
    assert(failed.serviceAWDLChannel(0,clockUS+65536)==EIO && failed.awdlRadio.fault);
    assert(failed.dmaOwned && failed.stationPresent && !p.frees);
    const auto count=failed.calls;assert(failed.stopAWDLRadio()==EIO && failed.calls==count);
    failed.dmaOwned=failed.stationPresent=false;++failed.dmaEpoch;
    failed.iwx_reset_tx_ring(&failed.com,&failed.com.txq[7]);failed.resetAWDLRadio();
    assert(p.frees==1 && !failed.awdlRadio.listener && !failed.awdlRadio.fault);
}

static void listener_retune_interleaving() {
    for(unsigned rlc:{1u,2u})for(unsigned scenario=0;scenario<4;++scenario)
    for(int call=1;call<=(rlc==2?4:3);++call) {
        ItlIwx d;d.com.sc_enabled_capa[44/8]|=1U<<(44%8);
        d.rlcVersion=rlc;
        assert(!d.startAWDLRadio(self) && !d.serviceAWDLChannel(149,clockUS+65536));
        auto p=packet();assert(!d.transmitAWDL(&p,1005));
        const int at=d.calls+call;
        if(scenario==0)d.failAt=at;
        if(scenario==1)d.resetAt=at;
        d.onCommand=[&](const std::string &) {
            if(d.calls!=at)return;
            if(scenario==2)assert(d.stopAWDLRadio()==EBUSY);
            if(scenario==3)d.infra.in_phyctxt=nullptr;
        };
        int error=d.serviceAWDLChannel(6,clockUS+65536);
        assert(error==(scenario==0?EIO:scenario==1?ENXIO:ECANCELED));
        assert(!d.awdlRadio.ready && !d.awdlRadio.changing && !d.awdlTxWindowOpen());
        if(scenario==1)assert(d.calls==at);
        if(scenario>=2)assert(!d.dmaOwned && !d.awdlRadio.binding && !d.awdlRadio.fault && p.frees==1);
        // A real device reset stops DMA before freeing any uncertain ring.
        d.dmaOwned=d.stationPresent=false;++d.dmaEpoch;
        for(unsigned q:{7u,8u})if(d.com.txq[q].ring_count) {
            d.iwx_reset_tx_ring(&d.com,&d.com.txq[q]);d.iwx_free_tx_ring(&d.com,&d.com.txq[q]);
        }
        d.resetAWDLRadio();assert(p.frees==1);
    }
}

static void listener_aggregate_queue_reuse() {
    ItlIwx d;d.com.sc_enabled_capa[44/8]|=1U<<(44%8);
    setupAggregateRadio(d);
    auto aggregate=aggregatePacket(),management=packet();
    assert(!d.transmitAWDL(&aggregate,1006) && !d.transmitAWDL(&management,1007));
    assert(!d.serviceAWDLChannel(0,clockUS+65536));
    assert(aggregate.frees==1 && management.frees==1);
    assert(d.com.txq[7].cur==1 && d.com.txq[8].cur==1 && d.awdlRadio.dataQueueConfigured);
    const auto calls=d.calls;clockUS+=65536;
    assert(!d.serviceAWDLChannel(6,clockUS+65536) && d.calls==calls);
    auto next=aggregatePacket();assert(!d.transmitAWDL(&next,1008));
    aggregateReport(d);assert(next.frees==1 && d.com.txq[8].cur==2);
    assert(!d.stopAWDLRadio() && !d.com.txq[7].ring_count && !d.com.txq[8].ring_count);
}

static void widePHYConfigurationAndRollback() {
    auto allow=[](ItlIwx &d) {
        for(unsigned c:{149U,153U,157U,161U}) {
            d.com.sc_ic.ic_channels[c]={IEEE80211_CHAN_5GHZ|IEEE80211_CHAN_VHT80,5000+5*c};
            d.com.sc_ic.ic_chan_active[c/8]|=1U<<(c%8);
        }
    };
    ItlIwx d;allow(d);d.rlcVersion=2;
    const unsigned positions[]={IWX_PHY_VHT_CTRL_POS_2_BELOW,IWX_PHY_VHT_CTRL_POS_1_BELOW,
        IWX_PHY_VHT_CTRL_POS_1_ABOVE,IWX_PHY_VHT_CTRL_POS_2_ABOVE};
    for(unsigned c:{149U,153U,157U,161U}) {
        assert(!d.awdlPHY(c,80));const auto &cmd=d.phyCommands.back();
        assert(cmd.ci.channel==c&&cmd.ci.width==IWX_PHY_VHT_CHANNEL_MODE80&&cmd.ci.ctrl_pos==positions[(c-149)/4]);
        assert(d.awdlRadio.widthMHz==80);auto calls=d.calls;assert(!d.awdlPHY(c,80)&&calls==d.calls);
    }
    for(unsigned original:{20U,80U}) {
        assert(!d.awdlPHY(149,original));const unsigned target=original==20?80:20;
        const auto before=d.phyCommands.size();d.failAt=d.calls+2;
        assert(d.awdlPHY(149,target)==EIO);assert(d.phyCommands.size()==before+2);
        assert(d.phyCommands[before].ci.width==(target==80?IWX_PHY_VHT_CHANNEL_MODE80:IWX_PHY_VHT_CHANNEL_MODE20));
        assert(d.phyCommands.back().ci.width==(original==80?IWX_PHY_VHT_CHANNEL_MODE80:IWX_PHY_VHT_CHANNEL_MODE20));
        assert(d.com.sc_phyctxt[1].channel==&d.com.sc_ic.ic_channels[149]&&d.awdlRadio.widthMHz==original);
        d.failAt=0;
    }
    for(unsigned restriction:{unsigned(IEEE80211_CHAN_PASSIVE),unsigned(IEEE80211_CHAN_DFS),unsigned(IEEE80211_CHAN_NOADHOC)}) {
        d.com.sc_ic.ic_channels[157].ic_flags|=restriction;auto calls=d.calls;
        assert(d.awdlPHY(149,80)==EACCES&&calls==d.calls);
        d.com.sc_ic.ic_channels[157].ic_flags&=~restriction;
    }
    ItlIwx live;allow(live);live.rlcVersion=2;live.com.sc_enabled_capa[44/8]|=1U<<(44%8);
    assert(!live.startAWDLRadio(self)&&live.awdlRadio.widthMHz==80);
    RadioState::GrantPolicy policy={};policy.widthMHz=80;
    assert(!live.serviceAWDLChannel(149,clockUS+65536,policy)&&live.awdlTxWindowOpen());
    auto pending=packet();assert(!live.transmitAWDL(&pending,1011));
    policy.widthMHz=20;assert(!live.serviceAWDLChannel(149,clockUS+65536,policy));
    assert(pending.frees==1&&live.awdlRadio.widthMHz==20&&live.awdlTxWindowOpen());
    assert(!live.stopAWDLRadio());
}

// PEER TX SCENARIOS
static void listenerNegotiationAcrossWindows() {
    ItlIwx d;d.com.sc_enabled_capa[44/8]|=1U<<(44%8);
    assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(149,clockUS+65536));
    auto &tx=d.awdlRadio.blockAckTX;
    tx.configured=true;tx.epoch=d.dmaEpoch;tx.cookie=91;
    tx.phase=AirportAWDL::TXBlockAckQueue::Asking;tx.enabled=false;tx.paused=false;
    assert(!d.closeAWDLListenerWindow());
    assert(!tx.paused && !tx.enabled && tx.cookie==91);
    clockUS+=65536;
    assert(!d.serviceAWDLChannel(149,clockUS+65536));
    assert(!tx.paused && !tx.enabled && tx.cookie==91);
    assert(!d.stopAWDLRadio());
}
static void listenerNegotiationAcrossChannels() {
    ItlIwx d;d.com.sc_enabled_capa[44/8]|=1U<<(44%8);
    assert(!d.startAWDLRadio(self));assert(!d.serviceAWDLChannel(149,clockUS+65536));
    auto &tx=d.awdlRadio.blockAckTX;
    tx.configured=true;tx.epoch=d.dmaEpoch;tx.cookie=91;
    tx.phase=AirportAWDL::TXBlockAckQueue::Asking;tx.attempts=2;tx.dialog=9;
    const auto clears=d.baClears;
    for(unsigned channel:{6u,48u,149u}) {
        auto pending=packet();assert(!d.transmitAWDL(&pending,1100+channel));
        clockUS+=65536;
        assert(!d.serviceAWDLChannel(channel,clockUS+65536));
        assert(d.baClears==clears && tx.configured && tx.cookie==91);
        assert(tx.phase==AirportAWDL::TXBlockAckQueue::Asking && tx.attempts==2 && tx.dialog==9);
        assert(pending.frees==1 && !tx.paused);
        assert(d.awdlRadio.binding && d.stationPresent && d.dmaOwned);
        assert(!commandCount(d,"free-ring") && !commandCount(d,"remove-station") && !commandCount(d,"unbind"));
    }
    assert(!d.stopAWDLRadio());assert(d.baClears==clears+1 && !d.dmaOwned);
}
int main() {
#if AWDL_RADIO_CASE == 48
    listenerNegotiationAcrossWindows();
#elif AWDL_RADIO_CASE == 49
    listenerNegotiationAcrossChannels();
#elif AWDL_RADIO_CASE == 47
    widePHYConfigurationAndRollback();
#elif AWDL_RADIO_CASE == 43
    peerTXWireAndCompletion();
#elif AWDL_RADIO_CASE == 44
    peerTXAggregatePermission();
#elif AWDL_RADIO_CASE == 45
    peerTXAdmissionRechecks();
#elif AWDL_RADIO_CASE == 46
    peerTXHeldAcrossWindows();
#elif AWDL_RADIO_CASE == 36 || AWDL_RADIO_CASE == 40
    listener_keeps_receive_context();
#elif AWDL_RADIO_CASE == 37
    listener_retune_and_filter();
#elif AWDL_RADIO_CASE == 38
    listener_start_failures();
#elif AWDL_RADIO_CASE == 39
    listener_queue_and_deadline();
#elif AWDL_RADIO_CASE == 41
    listener_retune_interleaving();
#elif AWDL_RADIO_CASE == 42
    listener_aggregate_queue_reuse();
#elif AWDL_RADIO_CASE == 31
    aggregateTransport();
#elif AWDL_RADIO_CASE == 32
    aggregateTransportLimits();
#elif AWDL_RADIO_CASE == 33
    aggregateQueueLifecycle();
#elif AWDL_RADIO_CASE == 34
    aggregateLateQueue();
#elif AWDL_RADIO_CASE == 35
    aggregateLateQueueFailures();
#elif AWDL_RADIO_CASE == 1
    lifecycle();
#elif AWDL_RADIO_CASE == 2
    failures();
#elif AWDL_RADIO_CASE == 3
    output();
#elif AWDL_RADIO_CASE == 4
    availabilityEvents();
#elif AWDL_RADIO_CASE == 5
    endNotificationIdentity();
#elif AWDL_RADIO_CASE == 6
    transportRoundTrip();
#elif AWDL_RADIO_CASE == 7
    transportFailures();
#elif AWDL_RADIO_CASE == 8
    cancellationWithoutEnd();
#elif AWDL_RADIO_CASE == 9
    cancellationEndOrdering();
#elif AWDL_RADIO_CASE == 10
    cancellationErrorsAndReset();
#elif AWDL_RADIO_CASE == 11
    disableAndRestart();
#elif AWDL_RADIO_CASE == 12 || AWDL_RADIO_CASE == 28
    boundedGrantTiming();
#elif AWDL_RADIO_CASE == 13
    grantGapAndTransitions();
#elif AWDL_RADIO_CASE == 14
    lateEndAndRepeatedCancellation();
#elif AWDL_RADIO_CASE == 15
    cancellationEmptyFlushOwnership();
#elif AWDL_RADIO_CASE == 16
    phyReceiveConfigurationAndCache();
#elif AWDL_RADIO_CASE == 17
    phyReceiveFailuresAndRollback();
#elif AWDL_RADIO_CASE == 18
    phyReceiveResetAndDisable();
#elif AWDL_RADIO_CASE == 19
    broadcastFirmwareReports();
#elif AWDL_RADIO_CASE == 20
    txGrantAtPublication();
#elif AWDL_RADIO_CASE == 21
    btCoexActionAntenna();
#elif AWDL_RADIO_CASE == 22
    managementAntennaFeedback();
#elif AWDL_RADIO_CASE == 23
    compareBroadcastAntennas();
#elif AWDL_RADIO_CASE == 24 || AWDL_RADIO_CASE == 25
    quz0034TransmitAntenna();
#elif AWDL_RADIO_CASE == 26 || AWDL_RADIO_CASE == 27
    txAirtimeAdmission();
#elif AWDL_RADIO_CASE == 29 || AWDL_RADIO_CASE == 30
    adaptiveDataRate();
#endif
}
