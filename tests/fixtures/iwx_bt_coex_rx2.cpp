#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>
#include <cstdio>
#include <libkern/OSByteOrder.h>
#include "itlwm/hal_iwx/BTCoex.hpp"
#define XYLog(...) ((void)0)
#define htole32 OSSwapHostToLittleInt32
#define htole64 OSSwapHostToLittleInt64
#define le32toh OSSwapLittleToHostInt32
#define le64toh OSSwapLittleToHostInt64
using u8=uint8_t; using __le16=uint16_t; using __le32=uint32_t; using __le64=uint64_t;
// WIRE
enum { IEEE80211_M_STA=1, IEEE80211_S_RUN=5, IEEE80211_NODE_HT=1,
    IEEE80211_NODE_VHT=2, IEEE80211_NODE_HE=4, IEEE80211_CHAN_WIDTH_20=1,
    IEEE80211_CHAN_WIDTH_20_NOHT=0, IEEE80211_CHAN_WIDTH_40=2, EDCA_AC_BE=0, MBUF_DONTWAIT=0 };
struct Packet { uint8_t data[27]={};unsigned freed=0; };
using mbuf_t=Packet *;
static void mbuf_freem(mbuf_t m){assert(!m->freed++);}
static int mbuf_copyback(mbuf_t m,size_t o,size_t n,const void *p,int){assert(o+n<=27);memcpy(m->data+o,p,n);return 0;}
struct ieee80211_channel { unsigned ic_freq=2437,ic_center_freq1=2437;bool two=true; };
#define IEEE80211_IS_CHAN_2GHZ(c) ((c)->two)
struct ieee80211_node {
    unsigned ni_flags=IEEE80211_NODE_HT,ni_chw=IEEE80211_CHAN_WIDTH_20,ni_txseq=0,refs=1;
    uint8_t ni_macaddr[6]={2,1,2,3,4,5},ni_bssid[6]={2,1,2,3,4,5};
};
struct iwx_phy_ctxt { unsigned id=0,color=0;ieee80211_channel *channel=nullptr; };
struct iwx_node { ieee80211_node in_ni;iwx_phy_ctxt *in_phyctxt=nullptr;unsigned in_id=0; };
struct ieee80211com { unsigned ic_opmode=IEEE80211_M_STA,ic_state=IEEE80211_S_RUN;
    ieee80211_node *ic_bss=nullptr;uint8_t ic_myaddr[6]={2,6,7,8,9,10};ieee80211_channel ic_channels[256]; };
struct task { unsigned queued=0; };
static void *systq=nullptr;
static void task_add(void *,task *t){++t->queued;}
static void task_del(void *,task *t){t->queued=0;}
struct iwx_softc { unsigned sc_device_family=IWX_DEVICE_FAMILY_22000,sc_flags=IWX_FLAG_BINDING_ACTIVE;
    struct { uint8_t valid_rx_ant=3,valid_tx_ant=3;bool sku_cap_mimo_disable=false; } sc_nvm;
    bool sc_tx_with_siso_diversity=false;uint8_t sc_enabled_capa[16]={},sc_ucode_api[16]={};
    ieee80211com sc_ic;task bt_coex_task;unsigned sc_tx_timer=0; };
struct iwx_tx_data { unsigned flags=0;uint64_t coexToken=0;ieee80211_node *node=nullptr;Packet *m=nullptr; };
struct iwx_rx_packet { size_t length=0;uint8_t data[4096]={}; };
static size_t iwx_rx_packet_payload_len(const iwx_rx_packet *p){return p->length;}
static bool isset(const uint8_t *p,unsigned bit){return p[bit/8]&(1U<<(bit%8));}
static void setbit(uint8_t *p,unsigned bit){p[bit/8]|=1U<<(bit%8);}
static unsigned ieee80211_chan2ieee(ieee80211com *ic,const ieee80211_channel *c){return c-ic->ic_channels;}
static void ieee80211_ref_node(ieee80211_node *n){++n->refs;}
static void ieee80211_release_node(ieee80211com *,ieee80211_node *n){assert(n->refs>1);--n->refs;}
struct OSObject {};
using IOReturn=int;
static const IOReturn kIOReturnSuccess=0;
struct WorkLoop {bool gated=true;bool inGate(){return gated;}};
struct Gate {WorkLoop *loop;unsigned calls=0;
    IOReturn runAction(IOReturn (*f)(OSObject *,void *,void *,void *,void *),void *arg){
        ++calls;bool old=loop->gated;loop->gated=true;auto ret=f(nullptr,arg,nullptr,nullptr,nullptr);loop->gated=old;return ret;
    }
};
#define container_of(ptr,type,member) reinterpret_cast<type *>(reinterpret_cast<char *>(ptr)-offsetof(type,member))
struct ItlIwx {
    iwx_softc com;IwxBTCoexState btCoex={};iwx_node ap;iwx_phy_ctxt phy,awdlPhy;
    WorkLoop loop;Gate gate{&loop};
    WorkLoop *getMainWorkLoop(){return &loop;}
    Gate *getMainCommandGate(){return &gate;}
    static void iwx_bt_coex_task(void *);
    unsigned calls=0,failAt=0,rxAnt=3,configVersion=6,ciVersion=2,phyVersion=3,tlcVersion=3,rlcVersion=2;
    unsigned txError=0;std::vector<unsigned> commands;
    std::vector<std::vector<uint8_t>> payloads;std::vector<std::unique_ptr<Packet>> packets;
    std::vector<iwx_tx_data> frames;std::function<void(unsigned)> onCommand;
    ItlIwx(){com.sc_ic.ic_bss=&ap.in_ni;ap.in_phyctxt=&phy;phy.channel=&com.sc_ic.ic_channels[6];
        awdlPhy={1,0,&com.sc_ic.ic_channels[149]};
        setbit(com.sc_enabled_capa,IWX_UCODE_TLV_CAPA_BT_MPLUT_SUPPORT);
        setbit(com.sc_enabled_capa,IWX_UCODE_TLV_CAPA_COEX_SCHEMA_2);
        setbit(com.sc_enabled_capa,IWX_UCODE_TLV_CAPA_TLC_OFFLOAD);
        setbit(com.sc_ucode_api,IWX_UCODE_TLV_API_NEW_RX_STATS);
    }
    ItlIwx *getController(){return this;}
    mbuf_t allocatePacket(size_t n){assert(n==27);packets.emplace_back(new Packet);return packets.back().get();}
    unsigned iwx_num_of_ant(unsigned n){return __builtin_popcount(n);}
    unsigned iwx_fw_valid_rx_ant(iwx_softc *){return rxAnt;}
    unsigned iwx_fw_valid_tx_ant(iwx_softc *){return 3;}
    bool iwx_mimo_enabled(iwx_softc *s){return !s->sc_nvm.sku_cap_mimo_disable;}
    unsigned iwx_lookup_cmd_ver(iwx_softc *,unsigned group,unsigned id){
        if(group==IWX_DATA_PATH_GROUP)return id==IWX_TLC_MNG_CONFIG_CMD?tlcVersion:rlcVersion;
        return id==IWX_BT_CONFIG?configVersion:id==IWX_BT_COEX_CI?ciVersion:phyVersion;
    }
    static unsigned iwx_cmd_id(unsigned id,unsigned group,unsigned version){return id|(group<<8)|(version<<16);}
    int iwx_send_cmd_pdu(iwx_softc *,unsigned id,unsigned flags,size_t n,const void *p){
        assert(loop.gated && !flags);unsigned call=++calls;commands.push_back(id);auto b=static_cast<const uint8_t *>(p);payloads.emplace_back(b,b+n);
        if(onCommand)onCommand(id);return call==failAt?EIO:0;
    }
    unsigned iwx_lmac_id(iwx_softc *,ieee80211_channel *){return 0;}
    unsigned iwx_get_channel_width(ieee80211com *,ieee80211_channel *){return 0;}
    unsigned iwx_get_ctrl_pos(ieee80211com *,ieee80211_channel *){return 0;}
    int iwx_phy_ctxt_cmd(iwx_softc *s,iwx_phy_ctxt *p,uint8_t a,uint8_t b,uint32_t action,uint32_t time){return iwx_phy_ctxt_cmd_v3(s,p,a,b,action,time);}
    int iwx_tx(iwx_softc *,mbuf_t m,ieee80211_node *n,int,int raw,uint64_t awdl,uint64_t coex){
        assert(raw==-1&&!awdl&&coex);if(txError){mbuf_freem(m);return txError;}
        frames.push_back({IWX_TXDATA_FLAG_COEX,coex,n,m});return 0;
    }
    void finish(size_t i,bool ack){auto &f=frames.at(i);iwx_bt_coex_complete_tx(&com,&f,ack);
        iwx_bt_coex_complete_tx(&com,&f,false);if(f.m){mbuf_freem(f.m);ieee80211_release_node(&com.sc_ic,f.node);f.m=nullptr;}}
    void begin(){assert(!iwx_send_bt_init_conf(&com));assert(btCoex.enabled);assert(!iwx_phy_ctxt_cmd_v3(&com,&phy,2,2,IWX_FW_CTXT_ACTION_ADD,0));assert(!iwx_bt_coex_update_ci(&com,true));iwx_bt_coex_link(&com,true);}
    void profile(unsigned activity,unsigned rrc=0,unsigned lut=0){iwx_rx_packet p;p.length=sizeof(iwx_bt_coex_profile_notif);
        auto n=reinterpret_cast<iwx_bt_coex_profile_notif *>(p.data);n->bt_activity_grading=htole32(activity);n->rrc_status=rrc;n->primary_ch_lut=htole32(lut);iwx_rx_bt_coex_profile(&com,&p);}
    void statistics(unsigned energy,unsigned mac=0,int delta=0,bool old=false){iwx_rx_packet p;
        if(old){com.sc_ucode_api[IWX_UCODE_TLV_API_NEW_RX_STATS/8]&=~(1U<<(IWX_UCODE_TLV_API_NEW_RX_STATS%8));
            auto n=reinterpret_cast<iwx_notif_statistics_v11 *>(p.data);n->rx.general.mac_id=htole32(mac);n->general.common.beacon_filter_average_energy=energy;p.length=sizeof(*n)+delta;
        }else{setbit(com.sc_ucode_api,IWX_UCODE_TLV_API_NEW_RX_STATS);auto n=reinterpret_cast<iwx_notif_statistics *>(p.data);n->rx.general.mac_id=htole32(mac);n->general.common.beacon_filter_average_energy=energy;p.length=sizeof(*n)+delta;}
        iwx_bt_coex_statistics(&com,&p);
    }
    int iwx_send_bt_init_conf(iwx_softc *);
    int iwx_bt_coex_update_ci(iwx_softc *,bool);
    void iwx_rx_bt_coex_profile(iwx_softc *,const iwx_rx_packet *);
    void iwx_bt_coex_link(iwx_softc *,bool);
    void iwx_bt_coex_request(iwx_softc *);
    bool iwx_bt_coex_phy_chains(iwx_softc *,const iwx_phy_ctxt *,uint8_t *,uint8_t *);
    int iwx_bt_coex_send_smps(iwx_softc *,ieee80211_node *,IwxBTPolicy::SMPS,uint64_t);
    void iwx_bt_coex_complete_tx(iwx_softc *,iwx_tx_data *,bool);
    void iwx_bt_coex_service(iwx_softc *);
    void iwx_bt_coex_statistics(iwx_softc *,const iwx_rx_packet *);
    int iwx_phy_ctxt_cmd_v3(iwx_softc *,iwx_phy_ctxt *,uint8_t,uint8_t,uint32_t,uint32_t);
};
// ROUTINES

static void earlyProfileDrivesPolicyAfterAssociation() {
    ItlIwx d;
    d.onCommand=[&](unsigned id) {
        if(id!=IWX_BT_CONFIG)return;
        d.profile(4);
        assert(!d.btCoex.enabled && d.btCoex.configuring);
        d.iwx_bt_coex_service(&d.com);
        assert(d.calls==1 && d.frames.empty() && !d.com.bt_coex_task.queued);
    };
    d.begin();
    assert(d.btCoex.enabled && !d.btCoex.configuring && d.btCoex.activity==4);
    assert(d.btCoex.notifications==1 && d.btCoex.configNotifications==1);
    d.iwx_bt_coex_service(&d.com);
    assert(d.frames.size()==1 && d.frames[0].m->data[26]==1);
    assert((d.btCoex.policy.phyApplied==IwxBTPolicy::Chains{2,2}));
    d.finish(0,true);d.iwx_bt_coex_service(&d.com);
    assert((d.btCoex.policy.phyApplied==IwxBTPolicy::Chains{1,1}));
    assert(d.ap.in_ni.refs==1);
}

static void actualQualificationAndACK() {
    {
        ItlIwx siso;siso.com.sc_nvm.sku_cap_mimo_disable=true;siso.begin();
        siso.profile(2);siso.iwx_bt_coex_service(&siso.com);assert(siso.frames.size()==1);
        siso.finish(0,true);siso.iwx_bt_coex_service(&siso.com);
        assert((siso.btCoex.policy.phyApplied==IwxBTPolicy::Chains{1,1}));
    }
    ItlIwx d;d.begin();assert(d.btCoex.rxMask==3&&!d.btCoex.mimoDisabled&&!d.btCoex.txSisoDiversity);
    d.profile(2);d.iwx_bt_coex_service(&d.com);assert(d.frames.size()==1&&d.frames[0].m->data[26]==3);
    assert(d.frames[0].m->data[0]==0xd0&&!d.frames[0].m->data[1]);
    unsigned calls=d.calls;for(unsigned i=0;i<1000;++i)d.iwx_bt_coex_service(&d.com);
    assert(d.frames.size()==1&&d.calls==calls);
    assert((d.btCoex.policy.wantedChains()==IwxBTPolicy::Chains{2,2}));
    d.finish(0,true);d.iwx_bt_coex_service(&d.com);
    assert((d.btCoex.policy.phyApplied==IwxBTPolicy::Chains{1,2}));
    auto p=reinterpret_cast<const iwx_rlc_config_cmd *>(d.payloads.back().data());
    const unsigned rx=le32toh(p->rlc.rx_chain_info);
    assert(((rx>>IWX_PHY_RX_CHAIN_CNT_POS)&3)==1&&((rx>>IWX_PHY_RX_CHAIN_MIMO_CNT_POS)&3)==2);
    d.profile(4);d.iwx_bt_coex_service(&d.com);d.finish(1,true);d.iwx_bt_coex_service(&d.com);
    assert((d.btCoex.policy.phyApplied==IwxBTPolicy::Chains{1,1}));
    assert(!d.iwx_phy_ctxt_cmd_v3(&d.com,&d.phy,2,2,IWX_FW_CTXT_ACTION_MODIFY,0));
    assert((d.btCoex.policy.phyApplied==IwxBTPolicy::Chains{1,1}));
    uint8_t idle=2,active=2;assert(!d.iwx_bt_coex_phy_chains(&d.com,&d.awdlPhy,&idle,&active)&&idle==2&&active==2);
    d.profile(4,1);d.iwx_bt_coex_service(&d.com);assert(d.frames.back().m->data[26]==0);
    d.finish(2,true);d.iwx_bt_coex_service(&d.com);assert((d.btCoex.policy.phyApplied==IwxBTPolicy::Chains{2,2}));
    assert(d.ap.in_ni.refs==1);
}

static void failuresResetAndProfiles() {
    ItlIwx d;d.begin();d.profile(4);d.iwx_bt_coex_service(&d.com);d.finish(0,false);
    for(unsigned i=0;i<1000;++i){d.profile(4);d.iwx_bt_coex_service(&d.com);}
    assert(d.frames.size()==1&&(d.btCoex.policy.phyApplied==IwxBTPolicy::Chains{2,2}));
    d.profile(2);d.iwx_bt_coex_service(&d.com);d.finish(1,true);
    d.failAt=d.calls+1;d.iwx_bt_coex_service(&d.com);assert(d.btCoex.phyError==EIO&&!d.btCoex.policy.phyValid);
    unsigned calls=d.calls;for(unsigned i=0;i<1000;++i)d.iwx_bt_coex_service(&d.com);assert(calls==d.calls);
    d.profile(0);d.iwx_bt_coex_service(&d.com);d.finish(2,true);d.failAt=0;d.iwx_bt_coex_service(&d.com);
    assert(d.btCoex.policy.phyValid&&(d.btCoex.policy.phyApplied==IwxBTPolicy::Chains{2,2}));
    d.profile(4);d.iwx_bt_coex_service(&d.com);auto stale=d.frames.back().coexToken;
    d.iwx_bt_coex_link(&d.com,false);d.finish(3,true);assert(!d.btCoex.policy.associated&&!d.btCoex.policy.actionToken);
    d.iwx_bt_coex_link(&d.com,true);d.iwx_bt_coex_service(&d.com);assert(d.frames.back().coexToken>stale);
    d.finish(4,true);d.onCommand=[&](unsigned id){if(id==IWX_PHY_CONTEXT_CMD)d.btCoex.reset();};
    d.iwx_bt_coex_service(&d.com);assert(!d.btCoex.enabled&&!d.btCoex.policy.associated);
    assert(d.ap.in_ni.refs==1);
}

static void reducedPowerStatisticsAndEpoch() {
    ItlIwx d;d.begin();d.profile(1);d.statistics(61);d.iwx_bt_coex_service(&d.com);
    assert(d.commands.back()==IWX_BT_COEX_UPDATE_REDUCED_TXP&&d.payloads.back().size()==4);
    uint32_t value;memcpy(&value,d.payloads.back().data(),4);assert(le32toh(value)==(IWX_STATION_ID|0x80));
    unsigned calls=d.calls;for(unsigned energy:{62u,63u,64u,65u}){d.statistics(energy);d.iwx_bt_coex_service(&d.com);}assert(calls==d.calls);
    d.statistics(20,1);d.statistics(20,0,-1);d.statistics(20,0,1);assert(d.btCoex.beaconDBm==-65);
    d.statistics(66,0,0,true);d.iwx_bt_coex_service(&d.com);assert(!d.btCoex.policy.powerApplied);
    d.statistics(50);d.failAt=d.calls+1;d.iwx_bt_coex_service(&d.com);assert(d.btCoex.powerError==EIO&&!d.btCoex.policy.powerValid);
    calls=d.calls;for(unsigned i=0;i<1000;++i)d.iwx_bt_coex_service(&d.com);assert(calls==d.calls);
    d.failAt=0;d.profile(1,0,1);d.iwx_bt_coex_service(&d.com);assert(d.btCoex.policy.powerValid&&!d.btCoex.policy.powerApplied);
    d.profile(1);d.onCommand=[&](unsigned id){if(id==IWX_BT_COEX_UPDATE_REDUCED_TXP)d.iwx_bt_coex_link(&d.com,false);};
    d.iwx_bt_coex_service(&d.com);assert(!d.btCoex.policy.associated&&!d.btCoex.policy.powerValid);
    assert(d.ap.in_ni.refs==1);
}


static void qualificationAndAPIsolation() {
    for(unsigned variant=0;variant<6;++variant) {
        ItlIwx d;
        if(variant==0)d.phyVersion=4;
        if(variant==1)d.tlcVersion=2;
        if(variant==2)d.rlcVersion=3;
        if(variant==3)d.com.sc_tx_with_siso_diversity=true;
        if(variant==4)d.rxAnt=7;
        if(variant==5)d.com.sc_enabled_capa[IWX_UCODE_TLV_CAPA_TLC_OFFLOAD/8]&=~(1U<<(IWX_UCODE_TLV_CAPA_TLC_OFFLOAD%8));
        assert(!d.iwx_send_bt_init_conf(&d.com) && !d.btCoex.enabled);
        auto c=reinterpret_cast<const iwx_bt_coex_cmd *>(d.payloads.back().data());
        assert(le32toh(c->mode)==IWX_BT_COEX_WIFI && !c->enabled_modules);
    }
    ItlIwx d;d.begin();d.phy.channel->two=false;d.phy.channel->ic_freq=5240;
    d.profile(4);d.statistics(20);d.loop.gated=false;ItlIwx::iwx_bt_coex_task(&d.com);
    assert(!d.loop.gated && d.gate.calls==1 && d.frames.empty());
    assert(d.btCoex.policy.desired==IwxBTPolicy::SMPS::Off && !d.btCoex.policy.powerApplied);
    assert(d.btCoex.policy.powerValid && d.btCoex.policy.powerUpdates==1);
    auto calls=d.calls;d.loop.gated=true;
    for(unsigned n=0;n<1000;++n){d.profile(4);ItlIwx::iwx_bt_coex_task(&d.com);}
    assert(d.calls==calls && d.frames.empty());
    // Reassociation can retain station0 in firmware: establish the power bit
    // even when the newly desired value is off and RSSI has not arrived yet.
    d.iwx_bt_coex_link(&d.com,false);d.iwx_bt_coex_link(&d.com,true);
    d.iwx_bt_coex_service(&d.com);assert(d.calls==calls+3 && d.btCoex.policy.powerValid);
    assert(d.commands.back()==IWX_BT_COEX_UPDATE_REDUCED_TXP);
}
static void latestPHYCompletionOwnsState() {
    for(bool newerFails:{false,true}) {
        ItlIwx d;d.begin();d.profile(4);d.iwx_bt_coex_service(&d.com);d.finish(0,true);
        bool entered=false;
        d.onCommand=[&](unsigned id){
            if(id!=IWX_PHY_CONTEXT_CMD || entered)return;entered=true;
            if(newerFails)d.failAt=d.calls+1;
            // A concurrent width update can enter while the older command sleeps.
            assert(d.iwx_phy_ctxt_cmd_v3(&d.com,&d.phy,2,2,IWX_FW_CTXT_ACTION_MODIFY,0)==(newerFails?EIO:0));
        };
        d.iwx_bt_coex_service(&d.com);
        assert(d.btCoex.phyError==(newerFails?EIO:0) && d.btCoex.policy.phyValid==!newerFails);
        assert(!d.btCoex.policy.phyToken && d.ap.in_ni.refs==1);
    }
    ItlIwx d;d.begin();d.profile(2);d.iwx_bt_coex_service(&d.com);
    d.profile(4);d.iwx_bt_coex_service(&d.com);assert(d.frames.size()==1);
    d.finish(0,true);d.iwx_bt_coex_service(&d.com);
    assert((d.btCoex.policy.phyApplied==IwxBTPolicy::Chains{1,2}));
    assert(d.frames.size()==2 && d.frames.back().m->data[26]==1);
    d.finish(1,true);d.iwx_bt_coex_service(&d.com);
    assert((d.btCoex.policy.phyApplied==IwxBTPolicy::Chains{1,1}));
}

static void rlcWireOrderingAndEpochs() {
    static_assert(sizeof(iwx_rlc_config_cmd)==32,"RLC v2 ABI");
    const unsigned rlc=ItlIwx::iwx_cmd_id(IWX_RLC_CONFIG_CMD,IWX_DATA_PATH_GROUP,2);
    {
        ItlIwx d;assert(!d.iwx_send_bt_init_conf(&d.com));
        assert(!d.iwx_phy_ctxt_cmd_v3(&d.com,&d.phy,1,1,IWX_FW_CTXT_ACTION_ADD,0));
        assert(d.commands[d.calls-2]==IWX_PHY_CONTEXT_CMD && d.commands.back()==rlc);
        const auto &p=d.payloads[d.calls-2];
        assert(!reinterpret_cast<const iwx_phy_context_cmd *>(p.data())->rxchain_info);
        const auto &b=d.payloads.back();assert(b.size()==32);
        auto wire=reinterpret_cast<const iwx_rlc_config_cmd *>(b.data());
        assert(!wire->phy_id && le32toh(wire->rlc.rx_chain_info)==((3U<<1)|(2U<<10)|(2U<<12)));
        for(unsigned i=8;i<32;++i)assert(!b[i]);
        assert(d.btCoex.policy.phyValid && (d.btCoex.policy.phyApplied==IwxBTPolicy::Chains{2,2}));
        d.iwx_bt_coex_link(&d.com,true);assert(d.btCoex.policy.phyValid);
        const auto calls=d.calls;
        assert(!d.iwx_phy_ctxt_cmd_v3(&d.com,&d.awdlPhy,1,1,IWX_FW_CTXT_ACTION_MODIFY,0));
        assert(d.calls==calls+1 && d.commands.back()==IWX_PHY_CONTEXT_CMD);
        assert(reinterpret_cast<const iwx_phy_context_cmd *>(d.payloads.back().data())->rxchain_info);
        assert(!d.iwx_phy_ctxt_cmd_v3(&d.com,&d.phy,1,1,IWX_FW_CTXT_ACTION_REMOVE,0));
        assert(d.commands.back()==IWX_PHY_CONTEXT_CMD && !d.btCoex.policy.phyValid && !d.btCoex.policy.phyBlocked);
    }
    for(unsigned stage:{1u,2u}) {
        ItlIwx d;d.begin();d.profile(4);d.iwx_bt_coex_service(&d.com);d.finish(0,true);
        d.failAt=d.calls+stage;unsigned calls=d.calls;d.iwx_bt_coex_service(&d.com);
        assert(d.calls==calls+stage && !d.btCoex.policy.phyValid && d.btCoex.phyError==EIO);
        calls=d.calls;for(unsigned n=0;n<1000;++n)d.iwx_bt_coex_service(&d.com);assert(d.calls==calls);
    }
    for(unsigned stage:{unsigned(IWX_PHY_CONTEXT_CMD),rlc}) {
        ItlIwx d;d.begin();d.profile(4);d.iwx_bt_coex_service(&d.com);d.finish(0,true);
        bool reset=false;const auto calls=d.calls;
        d.onCommand=[&](unsigned id){if(id==stage&&!reset){reset=true;d.btCoex.reset();}};
        d.iwx_bt_coex_service(&d.com);
        assert(d.calls==calls+(stage==IWX_PHY_CONTEXT_CMD?1:2));
        assert(!d.btCoex.enabled && !d.btCoex.policy.phyValid && !d.btCoex.policy.phyToken);
        assert(d.ap.in_ni.refs==1);
    }
    // Complete newer PHY/RLC inside the older PHY wait. The older continuation
    // must not issue a third RLC, even if its eventual PHY ACK reports success.
    {
        ItlIwx d;d.begin();d.profile(4);d.iwx_bt_coex_service(&d.com);d.finish(0,true);
        unsigned start=d.commands.size();bool entered=false;
        d.onCommand=[&](unsigned id){if(id!=IWX_PHY_CONTEXT_CMD||entered)return;entered=true;
            assert(!d.iwx_phy_ctxt_cmd_v3(&d.com,&d.phy,2,2,IWX_FW_CTXT_ACTION_MODIFY,0));};
        d.iwx_bt_coex_service(&d.com);
        assert(d.commands.size()==start+3 && d.commands.back()==rlc);
        assert(d.btCoex.policy.phyValid && (d.btCoex.policy.phyApplied==IwxBTPolicy::Chains{1,1}));
    }
}

int main(){earlyProfileDrivesPolicyAfterAssociation();actualQualificationAndACK();failuresResetAndProfiles();reducedPowerStatisticsAndEpoch();qualificationAndAPIsolation();latestPHYCompletionOwnsState();rlcWireOrderingAndEpochs();puts("candidate HAL coex/PHY/RLC/statistics: 7 groups passed");}
