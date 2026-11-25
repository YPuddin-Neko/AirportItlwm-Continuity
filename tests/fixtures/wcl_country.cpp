#include <cassert>
#include <cstring>
#include <functional>
#include <vector>
#include "AirportItlwm/WCL/WCL.hpp"
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
using IOReturn=int;
enum {kIOReturnSuccess,kIOReturnBadArgument,kIOReturnNotReady,kIOReturnNotPermitted};
enum {APPLE80211_M_COUNTRY_CODE_CHANGED=11,IEEE80211_CHAN_MAX=255,
      IEEE80211_CHAN_DFS=1,IEEE80211_CHAN_PASSIVE=2};
struct OSObject {};
struct apple80211ChannelInfo {};
struct apple80211_country_code_data {unsigned version;char cc[3];};
struct ieee80211_channel {unsigned ic_flags=0;bool five=false;};
struct ieee80211com {bool ic_chan_active[256]={};ieee80211_channel ic_channels[256]={};};
static bool isset(const bool *p,unsigned i) {return p[i];}
static bool IEEE80211_IS_CHAN_5GHZ(const ieee80211_channel *c) {return c->five;}
struct Hal {ieee80211com ic;ieee80211com *get80211Controller() {return &ic;}};
struct NativeAWDL {
    unsigned references=1;
    std::vector<unsigned> events;
    std::function<void()> afterCountry;
    void retain() {++references;}
    void release() {assert(references>1);--references;}
    void postMessage(unsigned code,void *data,size_t size,bool sendToWcl) {
        assert(!data&&!size&&!sendToWcl&&(code==11||code==224));events.push_back(code);
        if(code==11&&afterCountry)afterCountry();
    }
};
struct Instance {
    NativeAWDL native;NativeAWDL *awdlInterface=&native;
    IOReturn getCOUNTRY_CODE(OSObject *,apple80211_country_code_data *out) {
        memcpy(out->cc,"CN",3);return kIOReturnSuccess;
    }
};
struct Queue {bool gated=true,on=false;bool inGate() {return gated;}bool onThread() {return on;}};
struct AirportItlwmSkywalkInterface:OSObject {
    struct CountryUpdate {
        AirportItlwmSkywalkInterface *interface;Queue *queue;Instance *controller;
        uint64_t generation,readSerial;bool channelComplete;
    };
    bool wclStopping=false;
    uint64_t wclCountryGeneration=1,wclChannelReadSerial=0,wclCountryReloadPending=0;
    Hal device;Hal *fHalService=&device;Instance owner;Instance *instance=&owner;
    Queue queue;
    std::vector<unsigned> events;
    std::vector<CountryUpdate> completions;
    static IOReturn wclCountryUpdateGated(OSObject *,void *,void *,void *,void *);
    IOReturn getWCL_CHANNELS_INFO(apple80211ChannelInfo *);
    void deferWCLCountryUpdate(bool completion) {
        assert(completion);
        completions.push_back({this,&queue,&owner,wclCountryReloadPending,wclChannelReadSerial,true});
    }
    void postMessage(unsigned code,void *data,size_t size,bool sendToWcl) {
        assert(!data&&!size);events.push_back(code);
        if(code==11)assert(sendToWcl);
        else assert(code==224&&!sendToWcl);
    }
    IOReturn reload() {
        AirportWCL::ChannelInfo info={};
        return getWCL_CHANNELS_INFO(reinterpret_cast<apple80211ChannelInfo *>(&info));
    }
    void complete() {
        auto pending=completions;completions.clear();
        for(auto &update:pending)wclCountryUpdateGated(nullptr,&update,nullptr,nullptr,nullptr);
    }
};
// PRODUCTION
int main() {
    AirportItlwmSkywalkInterface s;
    AirportItlwmSkywalkInterface::CountryUpdate update{&s,&s.queue,&s.owner,1,0,false};
    auto run=[&] {return s.wclCountryUpdateGated(nullptr,&update,nullptr,nullptr,nullptr);};
    s.device.ic.ic_chan_active[48]=true;s.device.ic.ic_channels[48].five=true;
    assert(!run());assert((s.events==std::vector<unsigned>{11})&&s.wclCountryReloadPending==1);
    assert(!s.reload());assert(s.completions.size()==1&&s.events.size()==1);
    s.complete();assert((s.events==std::vector<unsigned>{11})&&!s.wclCountryReloadPending);
    assert((s.owner.native.events==std::vector<unsigned>{11,224}));
    ++s.wclCountryGeneration;update.generation=s.wclCountryGeneration;
    assert(!run());assert(!s.reload());s.complete();
    assert((s.events==std::vector<unsigned>{11,11})&&s.wclChannelReadSerial==2);
    assert((s.owner.native.events==std::vector<unsigned>{11,224,11,224}));
    s.device.ic.ic_chan_active[48]=false;s.events.clear();
    assert(!run());assert(s.reload()==kIOReturnNotReady);s.complete();
    assert((s.events==std::vector<unsigned>{11})&&s.wclChannelReadSerial==2);
    s.device.ic.ic_chan_active[48]=true;s.events.clear();
    assert(!run());assert(!s.reload());++s.wclCountryGeneration;s.complete();
    assert((s.events==std::vector<unsigned>{11}));
    s.events.clear();assert(run()==kIOReturnNotReady&&s.events.empty());
    update.generation=s.wclCountryGeneration;
    assert(!run());assert(!s.reload());assert(!s.reload());
    assert(s.completions.size()==2);s.complete();
    assert((s.events==std::vector<unsigned>{11}));
    assert(s.owner.native.events.size()==6);
    // A newer country task must send its own 11 before any read can complete it.
    s.events.clear();assert(!run());++s.wclCountryGeneration;
    assert(!s.reload()&&s.completions.empty());
    update.generation=s.wclCountryGeneration;assert(!run());assert(!s.reload());s.complete();
    assert(s.owner.native.events.size()==8);
    s.events.clear();assert(!run());assert(!s.reload());s.wclStopping=true;s.complete();
    assert((s.events==std::vector<unsigned>{11}));
    s.events.clear();assert(run()==kIOReturnNotReady&&s.events.empty());
    s.wclStopping=false;s.events.clear();assert(!run());assert(!s.reload());
    s.owner.native.afterCountry=[&] {s.owner.awdlInterface=nullptr;};s.complete();
    assert(s.owner.native.events.back()==11&&s.owner.native.events.size()==9);
    assert(s.owner.native.references==1);
    s.events.clear();s.queue.on=true;
    assert(run()==kIOReturnNotPermitted&&s.events.empty());
    s.queue.on=false;s.queue.gated=false;assert(run()==kIOReturnNotPermitted&&s.events.empty());
    assert(s.getWCL_CHANNELS_INFO(nullptr)==kIOReturnBadArgument);
}
