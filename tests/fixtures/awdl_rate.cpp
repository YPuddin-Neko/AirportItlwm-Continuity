#include <cassert>
#include <cstdint>
#include <cstddef>
#include <initializer_list>
#include "itlwm/hal_iwx/BTCoex.hpp"
#include "AirportItlwm/AWDL/PeerState.hpp"
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define AIRPORT_WCL 1
enum { IWX_LONG_GROUP=1,IWX_TX_CMD=0x1c };
struct ieee80211_rateset { uint8_t rs_nrates,rs_rates[16]; };
// RATE DECLARATIONS
struct iwl_cfg { uint8_t non_shared_ant; };
struct iwx_softc {
    uint8_t sc_mgmt_last_antenna_idx=0;
    uint8_t version=7;
    const iwl_cfg *sc_cfg=nullptr;
};
struct ItlIwx {
    IwxBTCoexState btCoex{};
    struct { uint16_t channel=0; uint8_t txAntennaMask=0; } awdlRadio;
    uint8_t awdlMinRateMbps=12;
    int iwx_rval2ridx(int);
    const iwx_rate *iwx_tx_fill_awdl_cmd(iwx_softc *,uint32_t *,uint8_t compareAntenna=0,uint8_t selectedRate=0);
    uint8_t iwx_lookup_cmd_ver(iwx_softc *sc,unsigned group,unsigned command) {
        assert(group==1 && command==0x1c);return sc->version;
    }
};
// PRODUCTION
static void adaptation() {
    using AirportAWDL::TxRateControl;
    uint64_t serial=0;
    TxRateControl r{};
    r.prepare(1,6,12,serial);
    const auto initial=r.epoch;
    for(unsigned i=0;i<7;++i) {
        assert(r.select()==12);
        assert(r.sample(i+1,r.epoch,12,true,0,serial));
    }
    assert(r.select()==12);
    r.sample(8,r.epoch,12,true,1,serial);assert(r.clean==0);
    for(unsigned i=0;i<8;++i)r.sample(i+1,r.epoch,12,true,0,serial);
    assert(r.select()==24);
    r.submitted(50,24);assert(r.select()==12 && r.probeToken==50);
    r.sample(50,r.epoch,24,true,0,serial);
    assert(r.current==24 && r.select()==24 && r.epoch!=initial && !r.probeToken);
    assert(!r.sample(49,initial,12,true,0,serial));
    auto fast=r.epoch;
    r.sample(51,r.epoch,24,true,2,serial);assert(r.current==12 && r.epoch!=fast);
    for(unsigned i=0;i<8;++i)r.sample(i+1,r.epoch,12,true,0,serial);
    r.submitted(60,24);r.release(60,r.epoch);assert(!r.probeToken && !r.clean && r.select()==12);
    for(unsigned i=0;i<8;++i)r.sample(i+1,r.epoch,12,true,0,serial);
    r.submitted(61,24);r.sample(61,r.epoch,24,false,4,serial);
    assert(r.current==12 && !r.probeToken && !r.clean);
    auto old=r.epoch;
    for(unsigned floor=0;floor<=54;++floor) {
        r.prepare(1,6,floor,serial);
        assert(r.current==TxRateControl::roundedFloor(floor));
        for(unsigned i=0;i<8;++i)r.sample(i+1,r.epoch,r.current,true,0,serial);
        assert(r.select()>=r.current && (r.select()<=24 || r.select()==r.current));
    }
    r.prepare(1,6,12,serial);assert(r.epoch!=old);
    old=r.epoch;r.prepare(1,48,12,serial);assert(r.epoch!=old);
    old=r.epoch;r.prepare(2,48,12,serial);assert(r.epoch!=old);
    AirportAWDL::Peer p{};p.txRate=r;p.resetObservation();assert(!p.txRate.epoch);
    p.txRate.prepare(2,48,12,serial);assert(p.txRate.epoch!=r.epoch);
    assert(!p.txRate.sample(1,r.epoch,12,true,0,serial));
    AirportAWDL::PeerTable table{};
    auto &observed=table.peers[0];observed.occupied=true;observed.lastSeenUS=100;
    const uint8_t remote[6]={2,0,0,0,0,2},self[6]={2,0,0,0,0,1};
    AirportAWDL::copyAddress(observed.address,remote);observed.txRate=r;
    AirportAWDL::Advertisement advertisement{};
    assert(table.update(remote,self,advertisement,100+table.LifetimeUS,6,-40));
    assert(!observed.txRate.epoch);
    observed.txRate.prepare(2,6,0,serial);old=observed.txRate.epoch;
    observed.txRate.prepare(2,6,6,serial);assert(observed.txRate.epoch!=old);
}
static void htAdaptation() {
    using AirportAWDL::TxRateControl;
    TxRateControl r{};uint64_t serial=0,token=10;
    auto train=[&]() {for(unsigned i=0;i<8;++i)assert(r.sample(++token,r.epoch,r.current,true,0,serial));};
    r.prepare(1,6,24,serial,0xff);
    for(uint8_t mcs=3;mcs<8;++mcs) {
        train();auto selected=r.select();assert(selected==(0x80|mcs));
        auto old=r.epoch;r.submitted(++token,selected);
        assert(r.select()==r.current && r.probeToken==token);
        assert(r.sample(token,old,selected,true,0,serial));
        assert(r.current==selected && r.epoch!=old);
        assert(!r.sample(token-1,old,24,true,0,serial));
    }
    train();assert(r.select()==0x87 && TxRateControl::halfMbps(r.current)==130);
    assert(r.sample(++token,r.epoch,0x87,false,3,serial) && r.current==0x86);
    assert(r.sample(++token,r.epoch,0x86,true,2,serial) && r.current==0x85);
    const auto old=r.epoch;r.prepare(1,6,24,serial,0x88);
    assert(r.epoch!=old && r.current==24);
    train();assert(r.select()==0x83);r.submitted(++token,0x83);
    r.sample(token,r.epoch,0x83,false,3,serial);assert(r.current==24 && !r.probeToken);
    train();r.submitted(++token,0x83);r.sample(token,r.epoch,0x83,true,0,serial);
    train();assert(r.select()==0x87);r.submitted(++token,0x87);r.sample(token,r.epoch,0x87,true,0,serial);
    r.sample(++token,r.epoch,0x87,false,3,serial);assert(r.current==0x83);
    r.sample(++token,r.epoch,0x83,false,3,serial);assert(r.current==24);
    r.prepare(1,6,54,serial,0x7f);train();assert(r.select()==0x86);
    r.prepare(1,6,54,serial,0x3f);train();assert(r.select()==54);
    r.prepare(1,6,24,serial,0);train();assert(r.select()==24);
}

int main() {
    adaptation();
    htAdaptation();
    ItlIwx driver;iwx_softc sc;
    const uint8_t mbps[]={6,9,12,18,24,36,48,54};
    const uint8_t plcp[]={0xd,0xf,0x5,0x7,0x9,0xb,0x1,0x3};
    for(unsigned request=0;request<=54;++request) {
        unsigned selected=0;while(mbps[selected]<request)++selected;
        driver.awdlMinRateMbps=request;
        for(unsigned version:{7u,8u,9u,unsigned(IWX_FW_CMD_VER_UNKNOWN)}) {
            sc.version=version;
            for(unsigned antenna=0;antenna<3;++antenna) {
                sc.sc_mgmt_last_antenna_idx=antenna;
                uint32_t flags=0xdeadbeef;
                const auto *rate=driver.iwx_tx_fill_awdl_cmd(&sc,&flags);
                assert(rate->rate==2*mbps[selected] && rate->plcp==plcp[selected]);
                const uint32_t encoding=(version==9)?0x100U|selected:plcp[selected];
                assert(flags==(encoding|((1U<<antenna)<<14)));
            }
        }
    }
    // Above 12 Mbps must skip the table's interleaved HT-only 13-Mbps entry.
    driver.awdlMinRateMbps=13;uint32_t flags=0;
    assert(driver.iwx_tx_fill_awdl_cmd(&sc,&flags)->rate==36);

    const iwl_cfg quz{2}, other{};
    sc.sc_cfg=&quz;sc.version=9;sc.sc_mgmt_last_antenna_idx=0;
    driver.awdlMinRateMbps=12;
    driver.btCoex.enabled=true;driver.btCoex.txMask=3;
    const auto check=[&](unsigned channel,unsigned grade,uint32_t expected) {
        driver.awdlRadio.channel=channel;driver.btCoex.activity=grade;
        assert(driver.iwx_tx_fill_awdl_cmd(&sc,&flags)->rate==24);
        assert(flags==(0x102u | (expected<<14)));
    };
    for (unsigned grade=0;grade<=4;++grade) {
        check(6,grade,grade>=3?2:1);
        check(48,grade,1);check(149,grade,1);check(0,grade,1);
    }
    driver.btCoex.txMask=1;check(6,3,1);
    driver.btCoex.txMask=3;sc.sc_cfg=&other;check(6,3,1);
    sc.sc_cfg=&quz;driver.btCoex.enabled=false;check(6,4,1);
    driver.btCoex.enabled=true;check(6,3,2);
    driver.btCoex.reset();assert(!driver.btCoex.activity && !driver.btCoex.enabled);
    driver.iwx_tx_fill_awdl_cmd(&sc,&flags);assert(flags==0x4102u);
    assert(driver.iwx_tx_fill_awdl_cmd(&sc,&flags,0,24)->rate==48 && flags==0x4104);
    driver.awdlMinRateMbps=36;
    assert(driver.iwx_tx_fill_awdl_cmd(&sc,&flags,0,24)->rate==72 && flags==0x4105);
    for(unsigned version:{7u,8u,9u,unsigned(IWX_FW_CMD_VER_UNKNOWN)}) {
        sc.version=version;driver.awdlMinRateMbps=6;driver.awdlRadio.txAntennaMask=3;
        for(unsigned mcs=0;mcs<8;++mcs) {
            auto rate=driver.iwx_tx_fill_awdl_cmd(&sc,&flags,0,0x80|mcs);
            assert(rate->ht_plcp==mcs && flags==((version==9?0x4200u:0x4100u)|mcs));
        }
        driver.btCoex.enabled=true;driver.btCoex.activity=3;driver.btCoex.txMask=3;
        driver.iwx_tx_fill_awdl_cmd(&sc,&flags,0,0x87);
        assert(flags==(version==9?0x8207u:0x8107u));
        driver.btCoex.reset();driver.awdlMinRateMbps=36;
        assert(driver.iwx_tx_fill_awdl_cmd(&sc,&flags,0,0x83)->rate==72);
        assert(flags==(version==9?0xc105u:0xc00bu));
    }
}
