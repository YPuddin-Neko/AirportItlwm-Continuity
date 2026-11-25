#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>
#include <libkern/OSByteOrder.h>
#include "itlwm/hal_iwx/BTCoex.hpp"
#define __packed __attribute__((packed))
#define XYLog(...) ((void)0)
#define htole32 OSSwapHostToLittleInt32
#define htole64 OSSwapHostToLittleInt64
#define le32toh OSSwapLittleToHostInt32
#define le64toh OSSwapLittleToHostInt64
// FIRMWARE DECLARATIONS
enum { IEEE80211_M_STA=1, IEEE80211_M_MONITOR=2, IEEE80211_CHAN_WIDTH_20_NOHT=0,
    IEEE80211_CHAN_WIDTH_20=1, IEEE80211_CHAN_WIDTH_40=2, IEEE80211_CHAN_WIDTH_80=3 };
struct ieee80211_channel { unsigned ic_freq=0,ic_center_freq1=0;bool two=false; };
#define IEEE80211_IS_CHAN_2GHZ(c) ((c)->two)
struct ieee80211_node { unsigned ni_chw=IEEE80211_CHAN_WIDTH_20; };
struct iwx_phy_ctxt { unsigned id=0;ieee80211_channel *channel=nullptr; };
struct iwx_node { ieee80211_node in_ni;iwx_phy_ctxt *in_phyctxt=nullptr; };
struct ieee80211com { unsigned ic_opmode=IEEE80211_M_STA;ieee80211_node *ic_bss=nullptr;ieee80211_channel ic_channels[256]; };
struct task {unsigned queued=0;};
static void *systq=nullptr;
static void task_add(void *,task *t){++t->queued;}
struct iwx_softc {
    task bt_coex_task;
    unsigned sc_device_family=IWX_DEVICE_FAMILY_22000,sc_flags=IWX_FLAG_BINDING_ACTIVE;
    struct { uint8_t valid_rx_ant=3,valid_tx_ant=2;bool sku_cap_mimo_disable=true; } sc_nvm;
    bool sc_tx_with_siso_diversity=false;
    uint8_t sc_enabled_capa[16]={};ieee80211com sc_ic;
};
struct iwx_rx_packet { size_t length=0;uint8_t data[64]={}; };
static size_t iwx_rx_packet_payload_len(const iwx_rx_packet *p) { return p->length; }
static bool isset(const uint8_t *p,unsigned bit) { return p[bit/8]&(1U<<(bit%8)); }
static unsigned ieee80211_chan2ieee(ieee80211com *ic,const ieee80211_channel *c) { return c-ic->ic_channels; }
struct ItlIwx {
    iwx_softc com;IwxBTCoexState btCoex={};iwx_node ap;iwx_phy_ctxt phy,awdlPhy;
    unsigned rxAnt=2,configVersion=6,ciVersion=2,calls=0,failAt=0,resetAt=0;
    std::vector<unsigned> commands;std::vector<std::vector<uint8_t>> payloads;
    std::function<void(unsigned)> onCommand;
    ItlIwx() {
        com.sc_ic.ic_bss=&ap.in_ni;ap.in_phyctxt=&phy;phy.id=0;
        com.sc_ic.ic_channels[6]={2437,2437,true};com.sc_ic.ic_channels[48]={5240,5240,false};
        phy.channel=&com.sc_ic.ic_channels[6];awdlPhy={1,&com.sc_ic.ic_channels[48]};
        com.sc_enabled_capa[67/8]|=1U<<(67%8);
    }
    unsigned iwx_num_of_ant(unsigned n) {return __builtin_popcount(n);}
    unsigned iwx_fw_valid_rx_ant(iwx_softc *) {return rxAnt;}
    unsigned iwx_fw_valid_tx_ant(iwx_softc *) {return rxAnt;}
    unsigned iwx_lookup_cmd_ver(iwx_softc *,unsigned group,unsigned id) {
        if(group==IWX_DATA_PATH_GROUP || id==IWX_PHY_CONTEXT_CMD)return IWX_FW_CMD_VER_UNKNOWN;
        assert(group==IWX_LONG_GROUP);return id==IWX_BT_CONFIG?configVersion:ciVersion;
    }
    int iwx_send_cmd_pdu(iwx_softc *,unsigned id,unsigned flags,size_t n,const void *p) {
        assert(!flags && (id==IWX_BT_CONFIG||id==IWX_BT_COEX_CI));
        assert(n==(id==IWX_BT_CONFIG?8u:24u));
        const unsigned call=++calls;commands.push_back(id);auto b=static_cast<const uint8_t *>(p);payloads.emplace_back(b,b+n);
        if(onCommand)onCommand(id);
        if(call==resetAt){btCoex.reset();return ENXIO;}
        return call==failAt?EIO:0;
    }
    iwx_bt_coex_ci_cmd lastCI() {iwx_bt_coex_ci_cmd c;memcpy(&c,payloads.back().data(),sizeof(c));return c;}
    void iwx_bt_coex_link(iwx_softc *,bool active){btCoex.policy.resetLink(active,false,1);}
    int iwx_send_bt_init_conf(iwx_softc *);
    int iwx_bt_coex_update_ci(iwx_softc *,bool);
    void iwx_rx_bt_coex_profile(iwx_softc *,const iwx_rx_packet *);
};
// PRODUCTION
static void configuration() {
    static_assert(sizeof(iwx_bt_coex_profile_notif)==40,"profile ABI");
    static_assert(sizeof(iwx_bt_coex_ci_cmd)==24,"CI ABI");
    for(unsigned variant=0;variant<6;++variant) {
        ItlIwx d;
        if(variant==1)d.com.sc_enabled_capa[67/8]=0;
        if(variant==2)d.rxAnt=3;
        if(variant==3)d.configVersion=5;
        if(variant==4)d.ciVersion=1;
        if(variant==5)d.com.sc_device_family=22500;
        assert(!d.iwx_send_bt_init_conf(&d.com));
        iwx_bt_coex_cmd c;memcpy(&c,d.payloads.back().data(),sizeof(c));
        assert(le32toh(c.mode)==(variant<2?IWX_BT_COEX_NW:IWX_BT_COEX_WIFI));
        assert(le32toh(c.enabled_modules)==(variant==0?0x15u:variant==1?0x14u:0u));
        assert(d.btCoex.enabled==(variant<2));
        assert(d.btCoex.rxMask==d.rxAnt && d.btCoex.txMask==d.rxAnt);
        assert(d.btCoex.nvmRxMask==3 && d.btCoex.nvmTxMask==2);
        assert(d.btCoex.configVersion==d.configVersion && d.btCoex.ciVersion==d.ciVersion);
        assert(d.btCoex.mimoDisabled && !d.btCoex.txSisoDiversity);
        if(variant>=2){assert(!d.iwx_bt_coex_update_ci(&d.com,true));assert(d.calls==1);}
    }
}
static void channelLifecycle() {
    ItlIwx d;assert(!d.iwx_send_bt_init_conf(&d.com));
    assert(!d.iwx_bt_coex_update_ci(&d.com,false));assert(!le64toh(d.lastCI().bt_primary_ci));
    assert(!d.iwx_bt_coex_update_ci(&d.com,true));
    auto c=d.lastCI();assert(le64toh(c.bt_primary_ci)==0x0003fffc00ULL && !c.primary_ch_phy_id);
    assert(!c.bt_secondary_ci && !c.secondary_ch_phy_id);
    unsigned calls=d.calls;
    for(unsigned n=0;n<100;++n) {
        d.awdlPhy.channel=&d.com.sc_ic.ic_channels[n%2?6:48];
        assert(!d.iwx_bt_coex_update_ci(&d.com,true));
    }
    assert(d.calls==calls);
    d.ap.in_ni.ni_chw=IEEE80211_CHAN_WIDTH_40;d.phy.channel->ic_center_freq1=2447;
    assert(!d.iwx_bt_coex_update_ci(&d.com,true));assert(le64toh(d.lastCI().bt_primary_ci)==0x0ffffffc00ULL);
    d.phy.channel->ic_center_freq1=2427;
    assert(!d.iwx_bt_coex_update_ci(&d.com,true));assert(le64toh(d.lastCI().bt_primary_ci)==0x0003ffffffULL);
    d.phy.channel=&d.com.sc_ic.ic_channels[48];d.ap.in_ni.ni_chw=IEEE80211_CHAN_WIDTH_80;
    assert(!d.iwx_bt_coex_update_ci(&d.com,true));assert(!le64toh(d.lastCI().bt_primary_ci));
    d.phy.channel=&d.com.sc_ic.ic_channels[6];d.ap.in_ni.ni_chw=IEEE80211_CHAN_WIDTH_20;
    assert(!d.iwx_bt_coex_update_ci(&d.com,true));assert(!d.iwx_bt_coex_update_ci(&d.com,false));
    assert(!le64toh(d.lastCI().bt_primary_ci));calls=d.calls;
    assert(!d.iwx_bt_coex_update_ci(&d.com,false) && d.calls==calls);
    d.btCoex.reset();assert(!d.btCoex.enabled && !d.btCoex.ciValid);
    assert(!d.iwx_send_bt_init_conf(&d.com));assert(!d.iwx_bt_coex_update_ci(&d.com,true));
    assert(le64toh(d.lastCI().bt_primary_ci)==0x0003fffc00ULL);
}
static void failuresAndProfile() {
    for(bool reset:{false,true}) {
        ItlIwx d;if(reset)d.resetAt=1;else d.failAt=1;
        assert(d.iwx_send_bt_init_conf(&d.com)==(reset?ENXIO:EIO));assert(!d.btCoex.enabled);
        if(reset) {
            assert(!d.btCoex.rxMask && !d.btCoex.txMask && !d.btCoex.nvmRxMask && !d.btCoex.nvmTxMask);
            assert(!d.btCoex.configVersion && !d.btCoex.ciVersion && !d.btCoex.mimoDisabled);
        } else assert(d.btCoex.rxMask==2 && d.btCoex.configVersion==6 && d.btCoex.mimoDisabled);
        ItlIwx ci;assert(!ci.iwx_send_bt_init_conf(&ci.com));
        if(reset)ci.resetAt=ci.calls+1;else ci.failAt=ci.calls+1;
        assert(ci.iwx_bt_coex_update_ci(&ci.com,true)==(reset?ENXIO:EIO));assert(!ci.btCoex.ciValid);
        if(reset)assert(!ci.btCoex.enabled && !ci.btCoex.lastError);
        else {assert(ci.btCoex.lastError==EIO);ci.failAt=0;assert(!ci.iwx_bt_coex_update_ci(&ci.com,true));}
    }
    ItlIwx d;assert(!d.iwx_send_bt_init_conf(&d.com));
    iwx_bt_coex_profile_notif n={};n.bt_activity_grading=htole32(3);n.bt_ci_compliance=htole32(1);
    n.primary_ch_lut=htole32(2);n.ttc_status=1;n.rrc_status=2;
    iwx_rx_packet p;p.length=sizeof(n);memcpy(p.data,&n,sizeof(n));
    const unsigned calls=d.calls;d.iwx_rx_bt_coex_profile(&d.com,&p);
    assert(d.btCoex.notifications==1 && d.btCoex.activity==3 && d.btCoex.compliance==1);
    assert(d.btCoex.primaryLUT==2 && d.btCoex.ttc==1 && d.btCoex.rrc==2 && d.calls==calls);
    for(unsigned size:{0u,39u,41u}){p.length=size;d.iwx_rx_bt_coex_profile(&d.com,&p);}
    assert(d.btCoex.malformed==3 && d.btCoex.notifications==1 && d.btCoex.activity==3);
    d.btCoex.reset();p.length=40;d.iwx_rx_bt_coex_profile(&d.com,&p);
    assert(d.btCoex.notifications==1 && !d.btCoex.activity);
    ItlIwx invalid;assert(!invalid.iwx_send_bt_init_conf(&invalid.com));
    invalid.ap.in_ni.ni_chw=IEEE80211_CHAN_WIDTH_40;
    assert(invalid.iwx_bt_coex_update_ci(&invalid.com,true)==EINVAL && invalid.calls==1);
}
static void reentrantAndUncertainCI() {
    for(bool failNew:{false,true}) {
        ItlIwx d;assert(!d.iwx_send_bt_init_conf(&d.com));
        assert(!d.iwx_bt_coex_update_ci(&d.com,true));
        d.ap.in_ni.ni_chw=IEEE80211_CHAN_WIDTH_40;d.phy.channel->ic_center_freq1=2447;
        bool entered=false;
        d.onCommand=[&](unsigned id) {
            if(id!=IWX_BT_COEX_CI || entered)return;
            entered=true;
            // While HT40 waits for ACK, a later update restores the old HT20.
            d.ap.in_ni.ni_chw=IEEE80211_CHAN_WIDTH_20;
            if(failNew)d.failAt=d.calls+1;
            assert(d.iwx_bt_coex_update_ci(&d.com,true)==(failNew?EIO:0));
        };
        assert(!d.iwx_bt_coex_update_ci(&d.com,true));
        assert(d.calls==4 && d.btCoex.ciValid==!failNew);
        assert(d.btCoex.lastError==(failNew?EIO:0));
        d.onCommand=nullptr;d.failAt=0;
        assert(!d.iwx_bt_coex_update_ci(&d.com,true));
        assert(d.calls==(failNew?5u:4u));
        iwx_bt_coex_ci_cmd c;memcpy(&c,d.btCoex.ci,sizeof(c));
        assert(le64toh(c.bt_primary_ci)==0x0003fffc00ULL);
    }
    ItlIwx d;assert(!d.iwx_send_bt_init_conf(&d.com));
    assert(!d.iwx_bt_coex_update_ci(&d.com,true));
    d.failAt=d.calls+1;assert(d.iwx_bt_coex_update_ci(&d.com,false)==EIO);
    assert(!d.btCoex.ciValid); // A failed ACK does not prove the old mask survived in firmware.
    const unsigned calls=d.calls;d.failAt=0;
    assert(!d.iwx_bt_coex_update_ci(&d.com,true) && d.calls==calls+1);
}

static void notificationDuringConfiguration() {
    for (unsigned outcome=0;outcome<5;++outcome) {
        ItlIwx d;
        if(outcome==1)d.failAt=1;
        if(outcome==2)d.resetAt=1;
        if(outcome==3)d.configVersion=5;
        d.onCommand=[&](unsigned id) {
            assert(id==IWX_BT_CONFIG && !d.btCoex.enabled);
            iwx_bt_coex_profile_notif n={};
            n.bt_activity_grading=htole32(3);n.bt_ci_compliance=htole32(1);
            n.primary_ch_lut=htole32(2);n.ttc_status=1;n.rrc_status=2;
            iwx_rx_packet p;p.length=sizeof(n);memcpy(p.data,&n,sizeof(n));
            d.iwx_rx_bt_coex_profile(&d.com,&p);
            // The synchronous command has released the gate. Its notification
            // must survive, but cannot enable policy before completion.
            assert(!d.btCoex.enabled && !d.com.bt_coex_task.queued);
            if(outcome==4) {
                n.bt_activity_grading=htole32(2);memcpy(p.data,&n,sizeof(n));
                d.iwx_rx_bt_coex_profile(&d.com,&p);
            }
            p.length=39;d.iwx_rx_bt_coex_profile(&d.com,&p);
            p.length=sizeof(n);n.bt_activity_grading=htole32(5);
            memcpy(p.data,&n,sizeof(n));d.iwx_rx_bt_coex_profile(&d.com,&p);
        };
        assert(d.iwx_send_bt_init_conf(&d.com)==(outcome==1?EIO:outcome==2?ENXIO:0));
        const bool enabled=outcome==0||outcome==4;
        assert(d.btCoex.enabled==enabled && d.calls==1 && !d.com.bt_coex_task.queued);
        assert(d.btCoex.activity==(enabled?(outcome==4?2u:3u):0u));
        assert(d.btCoex.compliance==(enabled?1u:0u));
        assert(d.btCoex.primaryLUT==(enabled?2u:0u));
        assert(d.btCoex.ttc==(enabled?1u:0u) && d.btCoex.rrc==(enabled?2u:0u));
        assert(d.btCoex.notifications==(outcome==3?0u:outcome==4?2u:1u));
        assert(d.btCoex.malformed==(outcome==3?0u:2u));
    }
}
int main(){configuration();channelLifecycle();failuresAndProfile();reentrantAndUncertainCI();notificationDuringConfiguration();}
