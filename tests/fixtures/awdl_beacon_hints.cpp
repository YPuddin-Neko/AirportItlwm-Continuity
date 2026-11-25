#define IEEE80211_CHAN_VHT80 0x10000000
#define IEEE80211_CHAN_NOADHOC 0x20000000
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#define __packed __attribute__((packed))
enum {IEEE80211_CHAN_MAX=255,IEEE80211_CHAN_2GHZ=1,IEEE80211_CHAN_5GHZ=2,
    IEEE80211_CHAN_PASSIVE=4,IEEE80211_CHAN_DFS=8,IEEE80211_CAPINFO_ESS=1,
    IEEE80211_EVT_COUNTRY_CODE_UPDATE=7};
static bool isset(const uint8_t *p,unsigned c){return p[c/8]&(1U<<(c%8));}
static void setbit(uint8_t *p,unsigned c){p[c/8]|=1U<<(c%8);}
struct Channel {unsigned ic_freq=0,ic_flags=0;};
struct ieee80211com {uint8_t ic_chan_active[32]={},ic_beacon_hint[32]={};Channel ic_channels[256];
    unsigned events=0;void (*ic_event_handler)(ieee80211com *,int,void *)=nullptr;};
struct ItlIwx {struct {ieee80211com sc_ic;} com;bool awdlChannelAllowed(uint32_t,uint8_t=20) const;};
// VHT DECLARATION
// PRODUCTION
static void setup(ItlIwx &d) {
    auto &ic=d.com.sc_ic;
    for(unsigned c:{6U,44U,149U,153U,157U,161U}) {
        setbit(ic.ic_chan_active,c);
        ic.ic_channels[c]={c==6?2437:5000+5*c,unsigned((c==6?IEEE80211_CHAN_2GHZ:IEEE80211_CHAN_5GHZ)|IEEE80211_CHAN_PASSIVE)};
    }
    ic.ic_event_handler=[](ieee80211com *ic,int event,void *){assert(event==IEEE80211_EVT_COUNTRY_CODE_UPDATE);++ic->events;};
}
int main() {
    for(unsigned fault=0;fault<7;++fault) {
        ItlIwx d;setup(d);auto &ic=d.com.sc_ic;
        if(fault==4)ic.ic_channels[149].ic_flags|=IEEE80211_CHAN_DFS;
        if(fault==5)ic.ic_chan_active[149/8]&=~(1U<<(149%8));
        if(fault==6)ic.ic_channels[149].ic_freq=0;
        ieee80211_note_beacon_hint(&ic,fault==0?256:149,fault==1?153:149,fault==2, fault==3?0:IEEE80211_CAPINFO_ESS,nullptr);
        assert(!isset(ic.ic_beacon_hint,149)&&!ic.events&&!d.awdlChannelAllowed(149));
    }
    ItlIwx d;setup(d);auto &ic=d.com.sc_ic;
    ieee80211_note_beacon_hint(&ic,149,149,0,IEEE80211_CAPINFO_ESS,nullptr);
    assert(d.awdlChannelAllowed(149)&&!d.awdlChannelAllowed(153)&&ic.events==1);
    ieee80211_note_beacon_hint(&ic,149,149,0,IEEE80211_CAPINFO_ESS,nullptr);
    assert(ic.events==1&&(ic.ic_channels[149].ic_flags&IEEE80211_CHAN_PASSIVE));
    ieee80211_ie_vht_operation vht={};vht.chan_width=1;vht.center_freq_seg1_idx=155;
    ieee80211_note_beacon_hint(&ic,157,157,0,IEEE80211_CAPINFO_ESS,&vht);
    for(unsigned c:{149U,153U,157U,161U})assert(d.awdlChannelAllowed(c));
    assert(ic.events==2 && !d.awdlChannelAllowed(149,80));
    ic.ic_channels[149].ic_flags|=IEEE80211_CHAN_VHT80;
    assert(d.awdlChannelAllowed(149,80) && !d.awdlChannelAllowed(149,40));
    ic.ic_channels[161].ic_flags|=IEEE80211_CHAN_NOADHOC;
    assert(!d.awdlChannelAllowed(149,80));
    ic.ic_channels[161].ic_flags&=~IEEE80211_CHAN_NOADHOC;
    ic.ic_beacon_hint[161/8]&=~(1U<<(161%8));assert(!d.awdlChannelAllowed(149,80));
    setbit(ic.ic_beacon_hint,161);assert(d.awdlChannelAllowed(149,80));
    ic.ic_channels[153].ic_flags|=IEEE80211_CHAN_DFS;
    assert(!d.awdlChannelAllowed(153));
    memset(ic.ic_beacon_hint,0,sizeof(ic.ic_beacon_hint));
    ieee80211_note_beacon_hint(&ic,149,149,0,IEEE80211_CAPINFO_ESS,&vht);
    assert(!isset(ic.ic_beacon_hint,153)&&d.awdlChannelAllowed(157));
    memset(ic.ic_beacon_hint,0,sizeof(ic.ic_beacon_hint));vht.center_freq_seg1_idx=42;
    ieee80211_note_beacon_hint(&ic,149,149,0,IEEE80211_CAPINFO_ESS,&vht);
    assert(d.awdlChannelAllowed(149)&&!d.awdlChannelAllowed(157));
}
