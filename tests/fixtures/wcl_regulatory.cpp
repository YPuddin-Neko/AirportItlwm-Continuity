#define IEEE80211_CHAN_VHT80 0x10000000
#define IEEE80211_CHAN_NOADHOC 0x20000000
#include <cassert>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "AirportItlwm/WCL/WCL.hpp"
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
using IOReturn = int;
enum {kIOReturnSuccess, kIOReturnBadArgument, kIOReturnNotReady};
enum {IEEE80211_CHAN_MAX=255, IEEE80211_CHAN_2GHZ=1, IEEE80211_CHAN_5GHZ=2,
      IEEE80211_CHAN_DFS=4, IEEE80211_CHAN_PASSIVE=8, HT40=16, VHT80=32};
struct OSObject {};
struct apple80211ChannelInfo {};
struct apple80211_country_code_data {unsigned version; char cc[3];};
struct ieee80211_channel {unsigned ic_freq=0, ic_flags=0;};
struct ieee80211com {bool ic_chan_active[256]={},ic_beacon_hint[256]={}; ieee80211_channel ic_channels[256]={};};
static bool isset(const bool *p, unsigned i) {return p[i];}
static bool IEEE80211_IS_CHAN_5GHZ(const ieee80211_channel *c) {return c->ic_flags & IEEE80211_CHAN_5GHZ;}
static bool IEEE80211_IS_CHAN_HT40(const ieee80211_channel *c) {return c->ic_flags & HT40;}
static bool IEEE80211_IS_CHAN_VHT40(const ieee80211_channel *) {return false;}
static bool IEEE80211_IS_CHAN_VHT80(const ieee80211_channel *c) {return c->ic_flags & VHT80;}
struct Hal {
    virtual ~Hal() = default;
    virtual ieee80211com *get80211Controller() = 0;
};
struct ItlIwx:Hal {
    struct {ieee80211com sc_ic;} com;
    ieee80211com *get80211Controller() override {return &com.sc_ic;}
    bool awdlChannelAllowed(uint32_t,uint8_t=20) const;
};
struct Instance {
    IOReturn getCOUNTRY_CODE(OSObject *, apple80211_country_code_data *out) {
        memcpy(out->cc, "CN", 3);return kIOReturnSuccess;
    }
};
struct AirportItlwmSkywalkInterface:OSObject {
    ItlIwx device;Hal *fHalService=&device;Instance owner,*instance=&owner;
    uint64_t wclChannelReadSerial=0,wclCountryReloadPending=0,wclCountryGeneration=1;
    IOReturn copyWCLChannels(void *);
    IOReturn getWCL_CHANNELS_INFO(apple80211ChannelInfo *);
    void deferWCLCountryUpdate(bool) {assert(false);}
};
// PRODUCTION
int main() {
    AirportItlwmSkywalkInterface s;
    auto &ic=s.device.com.sc_ic;
    for(unsigned c:{6U,44U,48U,149U}) {
        ic.ic_chan_active[c]=true;
        ic.ic_channels[c]={c==6 ? 2437U:5000+5*c, unsigned(c==6 ? IEEE80211_CHAN_2GHZ:
            IEEE80211_CHAN_5GHZ|IEEE80211_CHAN_PASSIVE|HT40|VHT80)};
    }
    AirportWCL::NativeChannelInfo info={};AirportWCL::ChannelInfo wcl={};
    auto read=[&] {
        assert(s.copyWCLChannels(&info)==kIOReturnSuccess);
        assert(s.getWCL_CHANNELS_INFO(reinterpret_cast<apple80211ChannelInfo *>(&wcl))==kIOReturnSuccess);
        assert(info.count==4&&wcl.count==4&&!memcmp(info.country,wcl.country,3));
        for(unsigned i=0;i<info.count;++i) {
            const unsigned c=info.channel[i];assert(c==wcl.channels[i].channel);
            assert(bool(info.passive[i])==bool(wcl.channels[i].flags&4));
            assert(bool(info.dfs[i])==bool(wcl.channels[i].flags&2));
            assert(bool(info.channelBitmap[i]&0x100)==!s.device.awdlChannelAllowed(c));
        }
    };
    read();assert(!(info.channelBitmap[0]&0x100));
    assert((info.channelBitmap[1]&0x100)&&(info.channelBitmap[2]&0x100)&&(info.channelBitmap[3]&0x100));
    // The current AP remains a visible STA channel while AWDL cannot initiate there.
    assert(info.channel[2]==48&&info.supports80MHz[2]&&wcl.channels[2].flags==4);
    ic.ic_beacon_hint[149]=true;
    read();assert(!(info.channelBitmap[3]&0x100)&&s.device.awdlChannelAllowed(149));
    assert(ic.ic_channels[149].ic_flags&IEEE80211_CHAN_PASSIVE);
    ic.ic_channels[149].ic_flags|=IEEE80211_CHAN_DFS;
    read();assert(info.channelBitmap[3]&0x100);
    ic.ic_channels[149].ic_flags&=~IEEE80211_CHAN_DFS;
    ic.ic_beacon_hint[149]=false;
    // Same-country profile changes must be reflected by the next read in both directions.
    ic.ic_channels[44].ic_flags &= ~IEEE80211_CHAN_PASSIVE;
    read();assert(!(info.channelBitmap[1]&0x100)&&!info.passive[1]);
    ic.ic_channels[44].ic_flags |= IEEE80211_CHAN_DFS;
    read();assert((info.channelBitmap[1]&0x100)&&info.dfs[1]);
    ic.ic_channels[44].ic_flags &= ~IEEE80211_CHAN_DFS;
    ic.ic_channels[6].ic_flags |= IEEE80211_CHAN_PASSIVE;
    read();assert((info.channelBitmap[0]&0x100)&&!(info.channelBitmap[1]&0x100));
    ic.ic_chan_active[149]=false;
    assert(!s.copyWCLChannels(&info)&&info.count==3);
    assert(!s.getWCL_CHANNELS_INFO(reinterpret_cast<apple80211ChannelInfo *>(&wcl))&&wcl.count==3);
    for(unsigned i=0;i<info.count;++i)assert(info.channel[i]!=149);
}
