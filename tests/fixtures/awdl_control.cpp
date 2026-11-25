#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
#include <sys/types.h>
#include "AirportItlwm/AWDL/SyncEngine.hpp"
using namespace AirportAWDL;
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
#define APPLE80211_VERSION 1
#define APPLE80211_MAX_SSID_LEN 32
#define APPLE80211_ADDR_LEN 6
enum { APPLE80211_C_FLAG_20MHZ=2, APPLE80211_C_FLAG_40MHZ=4,
       APPLE80211_C_FLAG_2GHZ=8, APPLE80211_C_FLAG_5GHZ=16,
       APPLE80211_C_FLAG_ACTIVE=0x80, APPLE80211_C_FLAG_DFS=0x100,
       APPLE80211_C_FLAG_EXT_ABV=0x200, APPLE80211_C_FLAG_80MHZ=0x400,
       APPLE80211_C_FLAG_160MHZ=0x800 };
enum { IEEE80211_S_INIT, IEEE80211_S_RUN };
using IOReturn=int;
enum { kIOReturnSuccess, kIOReturnUnsupported, kIOReturnNotReady, kIOReturnBadArgument,
       kIOReturnNoMemory, kIOReturnBusy };
struct ether_addr { uint8_t octet[6]; };
struct apple80211_companion_skywalk_if_create;
struct apple80211_llw_params;
struct ieee80211_channel { unsigned channel=48; };
struct ieee80211_node { ieee80211_channel *ni_chan=nullptr; };
struct ieee80211com { unsigned ic_state=IEEE80211_S_INIT;ieee80211_node *ic_bss=nullptr; };
static unsigned ieee80211_chan2ieee(ieee80211com *,ieee80211_channel *channel) {return channel->channel;}
// NATIVE DECLARATIONS
struct Radio {
    struct {
        bool ready=false;unsigned channel=44;uint8_t widthMHz=20;uint64_t dwellStarts=0;
        struct { uint32_t wideFaultCount=0; } fwHealth;
        bool wideCircuitTripped() const { return fwHealth.wideFaultCount >= 2; }
    } awdlRadio;
    PeerTable awdlPeers={};
    uint8_t awdlMinRateMbps=12;
    ieee80211com ic={};ieee80211_channel apChannel={};ieee80211_node ap={&apChannel};
    bool allow149=true,allow48=true,allow80=false;
    bool awdlChannelAllowed(unsigned channel,uint8_t width=20) {
        return (width==20 || (width==80&&channel==149&&allow80)) &&
            (channel==6||channel==44||(channel==48&&allow48)||(channel==149&&allow149));
    }
    ieee80211com *get80211Controller() {return &ic;}
};
static void IOLog(const char *,unsigned) {}
static uint64_t awdlNow() { return 1010000; }
struct Controller {
    std::vector<apple80211_peer_cache_control> messages;
    IOReturn result=kIOReturnSuccess;
    PeerTable *table=nullptr;
    unsigned calls=0;
    IOReturn postMessage(void *target,unsigned type,void *bytes,size_t size,bool route) {
        ++calls;
        assert(target && type==51 && size==464 && route);
        const auto &message=*static_cast<const apple80211_peer_cache_control *>(bytes);
        assert(message.version==1 && (message.operation==1 || message.operation==3));
        assert(bool(table->findCached(message.address))==(message.operation==1));
        if(result!=kIOReturnSuccess) return result;
        // Match the native PostOffice's copy-on-enqueue contract; no inline callback.
        messages.push_back(message);
        return kIOReturnSuccess;
    }
};
struct AirportItlwmAWDLInterface {
    // NATIVE REQUESTS
    Radio device,*radio=&device;
    Controller native;bool haveController=true,stopping=false;
    ether_addr address={{2,0,0,0,0,1}};
    SyncEngine sync={};
    bool sequenceConfigured=false,requested=false;
    bool regulatoryPlanBlocked=false,regulatoryStopRequired=false;
    bool nativeSequenceValid=false;
    bool llwParamsConfigured=false,llwParamsEnabled=false;
    apple80211_llw_params llwParams={};
    uint64_t llwParamsSetCalls=0;
    apple80211_awdl_sync_channel_sequence nativeSequence={};
    uint32_t secondaryMasterChannel=0;
    apple80211_awdl_extension_state_machine_parameter extensionParams={};
    uint32_t channelSequenceError=0;std::string channelSequenceLastReject;
    uint32_t afTxMode=0;
    uint64_t nextActionUS=123,nextMIFUS=456,nextRetryUS=0,unsupportedRequests=0;
    uint8_t frameTemplate[1024]={};size_t templateSize=0;
    BroadcastTemplate broadcast={};BroadcastRequest immediateBroadcast={};
    struct Timer {void setTimeoutMS(unsigned) {} void setTimeoutUS(unsigned) {}} scheduler,*timer=&scheduler;
    AirportItlwmAWDLInterface() {
        const uint8_t self[]={2,0,0,0,0,1};sync.start(self,0,1000000);
        native.table=&device.awdlPeers;
    }
    template<typename T>void setProperty(const char *key,T value,unsigned) {
        if(!strcmp(key,"ChannelSequenceError"))channelSequenceError=uint32_t(value);
    }
    void setProperty(const char *key,const char *value) {
        assert(!strcmp(key,"ChannelSequenceLastReject"));channelSequenceLastReject=value;
    }
    IOReturn setEnabled(bool enabled) { requested=enabled;return kIOReturnSuccess; }
    IOReturn handle(Request,void *);
    IOReturn handleCompanionSkywalkCreate(apple80211_companion_skywalk_if_create *) { return kIOReturnSuccess; }
    IOReturn handleGetLLWParams(apple80211_llw_params *) { return kIOReturnSuccess; }
    IOReturn handleSetLLWParams(const apple80211_llw_params *) { return kIOReturnSuccess; }
    IOReturn queueUnicast(const apple80211_awdl_oob_request &) {return kIOReturnNotReady;}
    IOReturn configureBroadcast(const apple80211_awdl_oob_request &,bool);
    bool broadcastDue(uint64_t) const;
    IOReturn updatePeerCache(const apple80211_peer_cache_control &);
    Controller *getController() { return haveController ? &native : nullptr; }
};
// PRODUCTION
using Request=AirportItlwmAWDLInterface::Request;

static void syncParameters() {
    static_assert(sizeof(apple80211_awdl_sync_params)==24,"24H32 request size");
    static_assert(offsetof(apple80211_awdl_sync_params,non_election_master_sync_frame_period)==20,"24H32 last period");
    AirportItlwmAWDLInterface i;
    // Surround the actual native request with canaries, including its sixth word.
    struct { uint32_t before;apple80211_awdl_sync_params value;uint32_t after; } out;
    memset(&out,0xa5,sizeof(out));
    assert(i.handle(Request::getAWDL_SYNC_PARAMS,&out.value)==kIOReturnSuccess);
    assert(out.before==0xa5a5a5a5 && out.after==0xa5a5a5a5);
    assert(out.value.version==1 && out.value.availability_window_length==16);
    assert(out.value.availability_window_period==16 && out.value.extension_length==16);
    assert(out.value.synchronization_frame_period==110 && !out.value.non_election_master_sync_frame_period);
    apple80211_awdl_sync_params input={1,16,16,16,220,128};
    assert(!i.device.awdlRadio.ready);
    assert(i.handle(Request::setAWDL_SYNC_PARAMS,&input)==kIOReturnSuccess);
    assert(!i.nextActionUS && !i.nextMIFUS);
    i.handle(Request::getAWDL_SYNC_PARAMS,&out.value);
    assert(!memcmp(&input,&out.value,sizeof(input)));
    for(unsigned field=0;field<6;++field) {
        auto invalid=input;IOReturn expected=kIOReturnBadArgument;
        switch(field) {
        case 0:invalid.availability_window_length=0;break;
        case 1:invalid.availability_window_period=0;break;
        case 2:invalid.extension_length=32;expected=kIOReturnUnsupported;break;
        case 3:invalid.synchronization_frame_period=0;break;
        case 4:invalid.synchronization_frame_period=65536;break;
        case 5:invalid.non_election_master_sync_frame_period=65536;break;
        }
        i.nextActionUS=123;i.nextMIFUS=456;
        assert(i.handle(Request::setAWDL_SYNC_PARAMS,&invalid)==expected);
        assert(i.nextActionUS==123 && i.nextMIFUS==456);
        i.handle(Request::getAWDL_SYNC_PARAMS,&out.value);
        assert(!memcmp(&input,&out.value,sizeof(input)));
    }
}
static void electionWire() {
    AirportItlwmAWDLInterface i;
    apple80211_awdl_election_id id={1,0x12345678},read={};
    assert(i.handle(Request::setAWDL_ELECTION_ID,&id)==kIOReturnSuccess);
    assert(i.handle(Request::getAWDL_ELECTION_ID,&read)==kIOReturnSuccess);
    assert(read.version==1 && read.election_id==id.election_id);
    assert(!i.nextActionUS && !i.nextMIFUS);
    apple80211_awdl_sync_params params={1,16,16,16,220,0};
    assert(i.handle(Request::setAWDL_SYNC_PARAMS,&params)==kIOReturnSuccess);
    apple80211_channel_data channel={1,{1,44,APPLE80211_C_FLAG_5GHZ}};
    assert(i.handle(Request::setCHANNEL,&channel)==kIOReturnSuccess);
    assert(i.sequenceConfigured);
    i.sync.restart(44,1000000);
    i.sync.counter=17;i.sync.masterCounter=42;
    uint8_t bytes[512];auto size=i.sync.action(bytes,sizeof(bytes),1005000,true,nullptr,0);
    assert(size>40);
    unsigned found=0;
    // Check raw TLV offsets independently of our advertisement decoder.
    for(size_t at=40;at<size;) {
        assert(size-at>=3);
        const uint8_t type=bytes[at];const unsigned length=le16(bytes+at+1);
        assert(length<=size-at-3);const auto value=bytes+at+3;
        if(type==4) { assert(length==73 && le16(value+7)==220);found|=1; }
        if(type==5) { assert(length==21 && le16(value+1)==0x5678);found|=2; }
        if(type==24) {
            assert(length==40 && le32(value+28)==0x12345678);
            assert(le32(value+12)==42 && le32(value+36)==17);found|=4;
        }
        at+=3+length;
    }
    assert(found==7 && !i.unsupportedRequests);
    id.election_id=0;i.handle(Request::setAWDL_ELECTION_ID,&id);
    i.handle(Request::getAWDL_ELECTION_ID,&read);assert(!read.election_id);
}
static void channelConfiguration() {
    AirportItlwmAWDLInterface i;
    apple80211_awdl_sync_channel_sequence input={};input.version=1;
    input.length=16;input.encoding=1;input.step_count=3;input.fill_channel=0xff;
    for(unsigned n=0;n<16;++n)
        input.seqs[n]={1,n%3==0?0U:44U,n%3==0?0U:unsigned(APPLE80211_C_FLAG_5GHZ)};
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&input)==kIOReturnSuccess);
    assert(i.sequenceConfigured);
    for(unsigned n=0;n<16;++n)assert(i.sync.channels[n]==input.seqs[n].channel);
    auto bad=input;bad.seqs[15]={1,44,APPLE80211_C_FLAG_2GHZ};
    bad.seqs[0]={1,149,APPLE80211_C_FLAG_5GHZ};
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&bad)==kIOReturnUnsupported);
    assert(!i.sync.channels[0]); // A bad last entry must not partially install earlier ones.
    bad=input;bad.seqs[15]={1,165,APPLE80211_C_FLAG_5GHZ};
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&bad)==kIOReturnUnsupported);
    bad=input;bad.length=33;
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&bad)==kIOReturnBadArgument);
    i.device.awdlRadio.ready=true;apple80211_awdl_sync_channel_sequence out={};
    assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&out)==kIOReturnSuccess);
    assert(out.length==16 && out.step_count==3 && out.encoding==1 && out.fill_channel==0xff);
    for(unsigned n=0;n<16;++n)assert(!memcmp(&input.seqs[n],&out.seqs[n],sizeof(out.seqs[n])));
    // 24H32's _channel6Info and setChannelSequenceByState use flags 0x0a.
    auto native=input;
    native.seqs[0]={1,6,APPLE80211_C_FLAG_2GHZ|APPLE80211_C_FLAG_20MHZ};
    native.seqs[1]={1,149,APPLE80211_C_FLAG_5GHZ|APPLE80211_C_FLAG_20MHZ};
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&native)==kIOReturnSuccess);
    for(unsigned n=0;n<16;++n)assert(i.sync.channels[n]==native.seqs[n].channel);
    for(unsigned invalidFlags : {1U,4U,0x100U,0x200U,0x400U,0x800U}) {
        bad=native;bad.seqs[0].channel=44;bad.seqs[0].flags=APPLE80211_C_FLAG_5GHZ;
        bad.seqs[15].flags=APPLE80211_C_FLAG_5GHZ|invalidFlags;
        assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&bad)==kIOReturnUnsupported);
        assert(i.sync.channels[0]==6);
    }
    bad=native;bad.seqs[3]={1,0,APPLE80211_C_FLAG_20MHZ};
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&bad)==kIOReturnUnsupported);
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&input)==kIOReturnSuccess);
    apple80211_channel_data channel={1,{1,256+44,APPLE80211_C_FLAG_5GHZ}};
    assert(i.handle(Request::setCHANNEL,&channel)==kIOReturnUnsupported);
    for(unsigned n=0;n<16;++n)assert(i.sync.channels[n]==input.seqs[n].channel);
}
static void nativeChannelMetadata() {
    AirportItlwmAWDLInterface i;
    i.device.ic.ic_state=IEEE80211_S_RUN;i.device.ic.ic_bss=&i.device.ap;
    i.device.awdlRadio.ready=true;
    apple80211_awdl_sync_channel_sequence input={};input.version=1;
    input.pad1=2;input.length=16;input.encoding=1;input.step_count=3;input.fill_channel=0xff;
    const unsigned channels[]={149,149,149,149,0,0,48,48,6,149,149,149,0,0,48,48};
    for(unsigned n=0;n<16;++n) {
        const unsigned c=channels[n];
        const unsigned flags=!c?0:c==48?0x294:c==6?0x8a:0x92;
        input.seqs[n]={1,c,flags};
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&input)==kIOReturnSuccess);
    assert(!i.channelSequenceError && i.channelSequenceLastReject.empty());
    apple80211_awdl_sync_channel_sequence read={};
    assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&read)==kIOReturnSuccess);
    assert(!memcmp(&input,&read,sizeof(read)));
    const uint64_t duration=16*4*1024;
    i.sync.restart(149,1000000);
    for(unsigned n=0;n<16;++n) {
        Window window={};const uint64_t now=1000000+n*duration+5000;
        assert(i.sync.window(now,window) && window.channel==channels[n]);
        if(!channels[n])continue;
        uint8_t frame[512]={};const auto size=i.sync.action(frame,sizeof(frame),now,false,nullptr,0);
        assert(size);
        unsigned sequences=0;
        for(size_t at=40;at<size;at+=3+le16(frame+at+1)) {
            const auto type=frame[at];const auto value=frame+at+3;
            const uint8_t *sequence=nullptr;
            if(type==4) {assert(value[0]==channels[n]);sequence=value+33;}
            if(type==18)sequence=value;
            if(!sequence)continue;
            ++sequences;
            assert(sequence[0]==15 && sequence[1]==3 && sequence[3]==3);
            for(unsigned j=0;j<16;++j) {
                assert(sequence[6+2*j]==channels[j]);
                const unsigned opclass=!channels[j]?0:channels[j]==6?81:channels[j]==48?115:124;
                assert(sequence[7+2*j]==opclass);
            }
        }
        assert(sequences==2);
    }
    assert(i.device.ic.ic_state==IEEE80211_S_RUN && i.device.apChannel.channel==48);
    assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&read)==kIOReturnSuccess);
    assert(!memcmp(&input,&read,sizeof(read)));
    // Unavailable radio state hides the retained host schedule until restart completes.
    i.device.awdlRadio.ready=false;
    assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&read)==kIOReturnNotReady);
    i.sync.restart(44,3000000);i.device.awdlRadio.ready=true;
    assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&read)==kIOReturnSuccess);
    assert(!memcmp(&input,&read,sizeof(read)));
    for(unsigned j=0;j<16;++j)assert(i.sync.channels[j]==channels[j]);
    for(unsigned flags:{0x14U,0x294U,0x490U,0x890U}) {
        auto wide=input;wide.seqs[15].flags=flags;
        assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&wide)==kIOReturnSuccess);
        assert(i.sync.channels[15]==48);
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&input)==kIOReturnSuccess);
    auto reject=[&](apple80211_awdl_sync_channel_sequence bad,const char *stage) {
        assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&bad)!=kIOReturnSuccess);
        assert(i.channelSequenceError && i.channelSequenceLastReject.find(stage)!=std::string::npos);
        assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&read)==kIOReturnSuccess);
        assert(!memcmp(&input,&read,sizeof(read)));
        for(unsigned j=0;j<16;++j)assert(i.sync.channels[j]==channels[j]);
    };
    for(unsigned flags:{0x95U,0x296U,0x494U,0x290U,0x190U,0x2090U,0x294U|0x10000U}) {
        auto bad=input;bad.seqs[15].flags=flags;reject(bad,"stage=flags");
        assert(i.channelSequenceLastReject.find("index=15 channel=48")!=std::string::npos);
    }
    auto bad=input;bad.seqs[15].channel=44;reject(bad,"stage=flags");
    i.device.ic.ic_state=IEEE80211_S_INIT;reject(input,"stage=flags");
    i.device.ic.ic_state=IEEE80211_S_RUN;i.device.ic.ic_bss=nullptr;reject(input,"stage=flags");
    i.device.ic.ic_bss=&i.device.ap;i.device.ap.ni_chan=nullptr;reject(input,"stage=flags");
    i.device.ap.ni_chan=&i.device.apChannel;
    i.device.allow48=false;reject(input,"stage=channel");
    assert(i.channelSequenceLastReject.find("hal_allowed=0")!=std::string::npos);
    i.device.allow48=true;i.device.allow149=false;reject(input,"stage=channel");
    assert(i.channelSequenceLastReject.find("index=0 channel=149")!=std::string::npos);
    i.device.allow149=true;
    bad=input;bad.seqs[14]={1,0,APPLE80211_C_FLAG_20MHZ};reject(bad,"stage=flags");
    for(unsigned n=0;n<5;++n) {
        bad=input;
        switch(n) {
        case 0:bad.length=0;break;
        case 1:bad.encoding=2;break;
        case 2:bad.step_count=2;break;
        case 3:bad.duplicate_count=1;break;
        case 4:bad.fill_channel=6;break;
        }
        reject(bad,n<2?"stage=format":"stage=layout");
        assert(i.channelSequenceLastReject.find("presence=4")!=std::string::npos);
    }
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&input)==kIOReturnSuccess);
    assert(!i.channelSequenceError && !i.channelSequenceLastReject.empty());
    apple80211_channel_data fixed={1,{1,44,APPLE80211_C_FLAG_5GHZ}};
    assert(i.handle(Request::setCHANNEL,&fixed)==kIOReturnSuccess && !i.nativeSequenceValid);
    assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&read)==kIOReturnSuccess);
    for(unsigned n=0;n<16;++n)assert(read.seqs[n].channel==44 && read.seqs[n].flags==APPLE80211_C_FLAG_5GHZ);
    i.regulatoryPlanBlocked=i.regulatoryStopRequired=true;
    assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&read)==kIOReturnNotReady);
    assert(i.handle(Request::setCHANNEL,&fixed)==kIOReturnSuccess && i.regulatoryPlanBlocked);
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&bad)!=kIOReturnSuccess);
    assert(i.regulatoryPlanBlocked && i.regulatoryStopRequired);
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&input)==kIOReturnSuccess);
    assert(!i.regulatoryPlanBlocked && i.regulatoryStopRequired);
    assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&read)==kIOReturnNotReady);
    i.regulatoryStopRequired=false;
    assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&read)==kIOReturnSuccess);
    assert(!memcmp(&input,&read,sizeof(read)));
}
static void wideChannelMetadata() {
    AirportItlwmAWDLInterface i;i.device.allow80=true;i.device.awdlRadio.ready=true;
    apple80211_awdl_sync_channel_sequence input={};input.version=1;input.length=16;
    input.encoding=1;input.step_count=3;input.fill_channel=0xff;
    for(unsigned n=0;n<16;++n)input.seqs[n]={1,n%2?6U:149U,unsigned(n%2?
        APPLE80211_C_FLAG_2GHZ|APPLE80211_C_FLAG_20MHZ:APPLE80211_C_FLAG_5GHZ|APPLE80211_C_FLAG_80MHZ)};
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&input)==kIOReturnSuccess);
    i.sync.restart(149,1000000);
    for(unsigned n=0;n<16;++n) {
        const uint64_t now=1000000+n*65536+5000;Window w={};assert(i.sync.window(now,w));
        assert(w.channel==input.seqs[n].channel&&w.widthMHz()==(n%2?20:80));
        uint8_t frame[512]={};auto size=i.sync.action(frame,sizeof(frame),now,false,nullptr,0);assert(size);
        unsigned sequences=0;
        for(size_t at=40;at<size;at+=3+le16(frame+at+1)) {
            const auto type=frame[at];const uint8_t *s=type==4?frame+at+36:type==18?frame+at+3:nullptr;
            if(!s)continue;++sequences;assert(s[1]==3);
            for(unsigned j=0;j<16;++j){assert(s[6+2*j]==(j%2?6:149));assert(s[7+2*j]==(j%2?81:128));}
        }
        assert(sequences==2);
    }
    apple80211_awdl_sync_channel_sequence read={};
    assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&read)==kIOReturnSuccess);
    assert(!memcmp(&input,&read,sizeof(input)));
    i.device.allow80=false;
    assert(i.handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&input)==kIOReturnUnsupported);
    for(unsigned j=0;j<16;++j)assert(i.sync.channelClasses[j]==(j%2?0:128));
    i.device.allow80=true;
    apple80211_channel_data fixed={1,{1,149,APPLE80211_C_FLAG_5GHZ|APPLE80211_C_FLAG_80MHZ}};
    assert(i.handle(Request::setCHANNEL,&fixed)==kIOReturnSuccess);
    assert(i.handle(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,&read)==kIOReturnSuccess);
    for(unsigned n=0;n<read.length;++n)assert(read.seqs[n].channel==149&&(read.seqs[n].flags&APPLE80211_C_FLAG_80MHZ));
    i.device.awdlRadio.channel=149;i.device.awdlRadio.widthMHz=80;
    assert(i.handle(Request::getCHANNEL,&fixed)==kIOReturnSuccess&&(fixed.channel.flags&APPLE80211_C_FLAG_80MHZ));
}

static void masterConfiguration() {
    AirportItlwmAWDLInterface i;
    apple80211_awdl_master_channel input={1,6},out={};
    assert(!i.device.awdlRadio.ready);
    assert(i.handle(Request::setAWDL_MASTER_CHANNEL,&input)==kIOReturnSuccess);
    i.handle(Request::getAWDL_MASTER_CHANNEL,&out);
    assert(out.version==1 && out.master_channel==6 && i.sync.configuredMasterChannel==6);
    for(unsigned n=3;n<16;n+=4)assert(i.sync.channels[n]==6);
    assert(!i.nextActionUS && !i.nextMIFUS);
    input.master_channel=0;
    assert(i.handle(Request::setAWDL_MASTER_CHANNEL,&input)==kIOReturnSuccess);
    assert(i.sync.channel==6 && i.sync.configuredMasterChannel==6);
    input.master_channel=300;
    assert(i.handle(Request::setAWDL_MASTER_CHANNEL,&input)==kIOReturnUnsupported);
    assert(i.sync.channel==6);
    // A native channel sequence remains authoritative when the master channel changes.
    apple80211_channel_data fixed={1,{1,44,APPLE80211_C_FLAG_5GHZ}};
    assert(i.handle(Request::setCHANNEL,&fixed)==kIOReturnSuccess);
    input.master_channel=149;
    assert(i.handle(Request::setAWDL_MASTER_CHANNEL,&input)==kIOReturnSuccess);
    for(auto c:i.sync.channels)assert(c==44);
    i.sync.restart(44,2000000);
    assert(i.sync.channel==149 && i.sync.localMasterChannel==149);
}
static void rssiConfiguration() {
    static_assert(sizeof(apple80211_awdl_election_rssi_thresholds)==20,"24H32 RSSI request size");
    static_assert(offsetof(apple80211_awdl_election_rssi_thresholds,sync_edge)==16,"24H32 sync edge");
    AirportItlwmAWDLInterface i;
    struct { uint32_t before;apple80211_awdl_election_rssi_thresholds value;uint32_t after; } out;
    memset(&out,0xa5,sizeof(out));
    assert(i.handle(Request::getAWDL_ELECTION_RSSI_THRESHOLDS,&out.value)==kIOReturnSuccess);
    assert(out.before==0xa5a5a5a5 && out.after==0xa5a5a5a5);
    assert(out.value.version==1 && !out.value.range_mid && !out.value.range_close && !out.value.sync_close);
    assert(out.value.sync_edge==-70);
    apple80211_awdl_election_rssi_thresholds input={1,0,0,0,-60};
    assert(i.handle(Request::setAWDL_ELECTION_RSSI_THRESHOLDS,&input)==kIOReturnSuccess);
    assert(i.sync.electionRSSI==-60);
    for(unsigned n=0;n<5;++n) {
        auto invalid=input;IOReturn expected=kIOReturnUnsupported;
        switch(n) {
        case 0:invalid.range_mid=-50;break;
        case 1:invalid.range_close=-40;break;
        case 2:invalid.sync_close=-30;break;
        case 3:invalid.sync_edge=-129;expected=kIOReturnBadArgument;break;
        case 4:invalid.sync_edge=1;expected=kIOReturnBadArgument;break;
        }
        assert(i.handle(Request::setAWDL_ELECTION_RSSI_THRESHOLDS,&invalid)==expected);
        assert(i.sync.electionRSSI==-60);
    }
    for(int edge:{-128,0,-70}) {
        input.sync_edge=edge;
        assert(i.handle(Request::setAWDL_ELECTION_RSSI_THRESHOLDS,&input)==kIOReturnSuccess);
        i.handle(Request::getAWDL_ELECTION_RSSI_THRESHOLDS,&out.value);
        assert(!memcmp(&input,&out.value,sizeof(input)));
    }
}
static void syncState() {
    AirportItlwmAWDLInterface i;
    apple80211_awdl_sync_state input={};input.version=1;input.state=1;
    copyAddress(input.sync_master_addr,i.sync.address);
    assert(i.handle(Request::setAWDL_SYNC_STATE,&input)==kIOReturnSuccess);
    assert(!i.requested && !i.device.awdlRadio.ready && !i.nextActionUS && !i.nextMIFUS);
    input.mode=1;
    assert(i.handle(Request::setAWDL_SYNC_STATE,&input)==kIOReturnSuccess);
    assert(i.sync.syncMode==1 && sameAddress(i.sync.fixedParent,i.sync.address));
    struct { uint32_t before;apple80211_awdl_sync_state value;uint32_t after; } out;
    memset(&out,0xa5,sizeof(out));
    assert(i.handle(Request::getAWDL_SYNC_STATE,&out.value)==kIOReturnSuccess);
    assert(out.before==0xa5a5a5a5 && out.after==0xa5a5a5a5);
    assert(out.value.version==1 && !out.value.state && out.value.mode==1);
    assert(!out.value.reserved && !out.value.flags && !out.value.reserved2[0]);
    i.device.awdlRadio.ready=true;i.requested=true;
    i.handle(Request::getAWDL_SYNC_STATE,&out.value);assert(out.value.state==1);
    for(unsigned n=0;n<6;++n) {
        auto invalid=input;auto before=i.sync;IOReturn expected=kIOReturnUnsupported;
        switch(n) {
        case 0:invalid.state=0;expected=kIOReturnBadArgument;break;
        case 1:invalid.state=3;break;
        case 2:invalid.mode=3;break;
        case 3:invalid.flags=0x10;break;
        case 4:invalid.mode=0;invalid.flags=0x20;break;
        case 5:invalid.sync_master_addr[0]=1;expected=kIOReturnBadArgument;break;
        }
        i.nextActionUS=123;i.nextMIFUS=456;
        assert(i.handle(Request::setAWDL_SYNC_STATE,&invalid)==expected);
        assert(!memcmp(&before,&i.sync,sizeof(before)) && i.nextActionUS==123 && i.nextMIFUS==456);
        assert(i.requested && i.device.awdlRadio.ready);
    }
    for(unsigned mode:{1U,2U}) {
        input.mode=mode;input.flags=0x20;input.sync_master_addr[5]=2;
        assert(i.handle(Request::setAWDL_SYNC_STATE,&input)==kIOReturnSuccess);
        i.handle(Request::getAWDL_SYNC_STATE,&out.value);
        assert(out.value.state==1 && out.value.mode==mode && out.value.flags==0x20);
        assert(sameAddress(out.value.sync_master_addr,input.sync_master_addr));
    }
}
static void electionParameters() {
    static_assert(sizeof(apple80211_awdl_election_params)==56,"24H32 election ABI");
    static_assert(offsetof(apple80211_awdl_election_params,master)==22,"24H32 master offset");
    AirportItlwmAWDLInterface i;
    apple80211_awdl_election_id id={1,0x12345678};
    i.handle(Request::setAWDL_ELECTION_ID,&id);
    apple80211_awdl_election_params input={};input.format=1;
    input.flags=0x80;input.election_id=0xabcd;input.master_metric=777;input.self_metric=555;
    copyAddress(input.master,i.sync.address);
    assert(i.handle(Request::setAWDL_ELECTION_PARAMS,&input)==kIOReturnSuccess);
    assert(i.sync.electionID==0x1234abcd && i.sync.electionFlags==0x80);
    struct { uint32_t before;apple80211_awdl_election_params value;uint32_t after; } out;
    memset(&out,0xa5,sizeof(out));
    assert(i.handle(Request::getAWDL_ELECTION_PARAMS,&out.value)==kIOReturnSuccess);
    assert(out.before==0xa5a5a5a5 && out.after==0xa5a5a5a5);
    input.version=1;assert(!memcmp(&input,&out.value,sizeof(input)));
    for(auto &c:i.sync.channels)c=44;
    uint8_t bytes[512];auto size=i.sync.action(bytes,sizeof(bytes),1005000,true,nullptr,0);
    bool found=false;
    for(size_t at=40;at<size;at+=3+le16(bytes+at+1))if(bytes[at]==5) {
        assert(bytes[at+3]==0x80 && le16(bytes+at+4)==0xabcd);
        assert(le32(bytes+at+14)==777 && le32(bytes+at+18)==555);found=true;
    }
    assert(found);
    auto old=i.sync;input.master[0]=1;
    assert(i.handle(Request::setAWDL_ELECTION_PARAMS,&input)==kIOReturnBadArgument);
    assert(!memcmp(&old,&i.sync,sizeof(old)));
}
static void frameTemplates() {
    AirportItlwmAWDLInterface i;
    uint8_t payload[53]={24,40,0};
    copyAddress(payload+3,i.sync.address);copyAddress(payload+9,i.sync.address);
    putLE32(payload+15,23);putLE32(payload+23,555);putLE32(payload+27,555);
    putLE32(payload+31,0x12345678);putLE32(payload+39,23);
    const uint8_t tail[]={21,2,0,0x93,2,16,2,0,'o','k'};
    memcpy(payload+43,tail,sizeof(tail));
    apple80211_awdl_sync_frame_template input={1,sizeof(payload),payload};
    assert(i.handle(Request::setAWDL_SYNC_FRAME_TEMPLATE,&input)==kIOReturnSuccess);
    assert(i.sync.nativeElection && i.sync.counter==23 && i.sync.metric==555);
    assert(!i.nextActionUS && !i.nextMIFUS && i.templateSize==5);
    assert(!memcmp(i.frameTemplate,payload+48,5));
    // The caller can reuse its input storage immediately after the ioctl.
    memset(payload,0,sizeof(payload));
    i.sync.restart(44,1000000);
    for(auto &c:i.sync.channels)c=44;
    uint8_t bytes[512];auto size=i.sync.action(bytes,sizeof(bytes),1005000,true,
        i.frameTemplate,i.templateSize);
    Advertisement a={};assert(parseMPDU(bytes,size,a)==ParseResult::Valid);
    assert(a.version==0x93 && a.masterCounter==23 && a.electionID==0x12345678);
    assert(!memcmp(bytes+size-5,tail+5,5));
    // Native startup updates election parameters between sync disable and enable.
    apple80211_awdl_sync_enabled enabled={1,0,0};
    assert(i.handle(Request::setAWDL_SYNC_ENABLED,&enabled)==kIOReturnSuccess);
    apple80211_awdl_sync_state state={};state.state=1;state.mode=1;
    copyAddress(state.sync_master_addr,i.sync.address);
    assert(i.handle(Request::setAWDL_SYNC_STATE,&state)==kIOReturnSuccess);
    apple80211_awdl_election_params params={};params.format=1;params.election_id=0x5678;
    params.master_metric=556;params.self_metric=557;copyAddress(params.master,i.sync.address);
    assert(i.handle(Request::setAWDL_ELECTION_PARAMS,&params)==kIOReturnSuccess);
    assert(i.sync.electionID==0x12345678 && !i.sync.transmitReady(1005000));
    payload[0]=24;putLE16(payload+1,40);memcpy(payload+3,i.sync.nativeElectionValue,40);
    putLE32(payload+23,556);putLE32(payload+27,557);memcpy(payload+43,tail,sizeof(tail));
    assert(i.handle(Request::setAWDL_SYNC_FRAME_TEMPLATE,&input)==kIOReturnSuccess);
    enabled.enabled=1;
    assert(i.handle(Request::setAWDL_SYNC_ENABLED,&enabled)==kIOReturnSuccess && i.requested);
    i.sync.restart(44,2000000);
    assert(i.sync.syncMode==1 && i.sync.nativeElection && i.sync.counter==23);
    assert(i.sync.transmitReady(2005000));
    assert(i.sync.masterMetric==556 && i.sync.metric==557 && i.templateSize==5);
    auto prior=i.sync;const uint8_t broken[]={16,1,0,'x',24,40,0};
    input.payload=const_cast<uint8_t *>(broken);input.payload_len=sizeof(broken);
    i.nextActionUS=123;i.nextMIFUS=456;
    assert(i.handle(Request::setAWDL_SYNC_FRAME_TEMPLATE,&input)==kIOReturnBadArgument);
    assert(!memcmp(&i.sync,&prior,sizeof(prior)) && i.templateSize==5);
    assert(i.nextActionUS==123 && i.nextMIFUS==456 && !memcmp(i.frameTemplate,tail+5,5));
    input.payload=nullptr;input.payload_len=0;
    assert(i.handle(Request::setAWDL_SYNC_FRAME_TEMPLATE,&input)==kIOReturnSuccess);
    assert(!i.sync.nativeElection && !i.templateSize);
}
static void minimumRate() {
    static_assert(sizeof(apple80211_awdl_min_rate)==8,"24H32 minimum rate size");
    static_assert(offsetof(apple80211_awdl_min_rate,min_rate)==4,"24H32 minimum rate field");
    AirportItlwmAWDLInterface i;
    struct { uint8_t before;apple80211_awdl_min_rate value;uint8_t after; } output;
    memset(&output,0xa5,sizeof(output));
    assert(i.handle(Request::getAWDL_MIN_RATE,&output.value)==kIOReturnSuccess);
    assert(output.before==0xa5 && output.after==0xa5 && output.value.version==1);
    assert(output.value.min_rate==12);
    for(auto byte:output.value.reserved)assert(!byte);
    for(unsigned rate=0;rate<256;++rate) {
        const auto saved=i.device.awdlMinRateMbps;
        apple80211_awdl_min_rate input={1,uint8_t(rate),{0,0,0}};
        assert(i.handle(Request::setAWDL_MIN_RATE,&input)==
            (rate<=54?kIOReturnSuccess:kIOReturnUnsupported));
        i.handle(Request::getAWDL_MIN_RATE,&output.value);
        assert(output.value.min_rate==(rate<=54?rate:saved));
        i.sync.restart(44,2000000);
        assert(i.device.awdlMinRateMbps==output.value.min_rate);
    }
}
static void advertisers() {
    static_assert(sizeof(awdl_peer_advert_add)==44,"24H32 advertiser request size");
    AirportItlwmAWDLInterface i;
    const uint8_t parent[]={2,0,0,0,0,2};
    SyncEngine sender;sender.start(parent,44,1000000);sender.counter=sender.masterCounter=1;
    uint8_t frame[512];size_t size=sender.action(frame,sizeof(frame),1000000,true,nullptr,0);
    Advertisement a={};assert(parseMPDU(frame,size,a)==ParseResult::Valid);
    i.device.awdlPeers.update(parent,i.sync.address,a,1000000,44,-40);
    struct { uint8_t before;awdl_peer_advert_add value;uint8_t after; } input={};
    input.before=input.after=0xa5;auto b=input.value.bytes;
    putLE16(b,0);putLE16(b+2,64);putLE16(b+6,110);
    putLE16(b+8,16);putLE16(b+10,16);putLE16(b+12,16);b[16]=a.presenceMode;
    b[20]=0;b[21]=0x0f;b[22]=0x42;b[23]=0x40;
    copyAddress(b+32,parent);copyAddress(b+38,parent);
    assert(i.handle(Request::setAWDL_AWDL_ADVERTISERS,&input.value)==kIOReturnSuccess);
    assert(input.before==0xa5 && input.after==0xa5 && i.sync.nativeParent.valid);
    assert(i.sync.nativeParent.epochUS==1000000 && !i.nextActionUS && !i.nextMIFUS);
    auto saved=i.sync.nativeParent;i.nextActionUS=123;i.nextMIFUS=456;
    b[38]=1;
    assert(i.handle(Request::setAWDL_AWDL_ADVERTISERS,&input.value)==kIOReturnBadArgument);
    b[38]=2;b[16]=48;
    assert(i.handle(Request::setAWDL_AWDL_ADVERTISERS,&input.value)==kIOReturnUnsupported);
    b[16]=a.presenceMode;b[23]++;
    assert(i.handle(Request::setAWDL_AWDL_ADVERTISERS,&input.value)==kIOReturnNotReady);
    assert(!memcmp(&saved,&i.sync.nativeParent,sizeof(saved)));
    assert(i.nextActionUS==123 && i.nextMIFUS==456);
}
static void cacheControl() {
    static_assert(sizeof(apple80211_peer_cache_control)==464,"24H32 cache message");
    static_assert(offsetof(apple80211_peer_cache_control,operation)==4,"24H32 operation");
    static_assert(offsetof(apple80211_peer_cache_control,address)==8,"24H32 address");
    static_assert(offsetof(apple80211_peer_cache_control,reserved)==14,"24H32 reserved");
    AirportItlwmAWDLInterface i;
    const uint8_t parent[]={2,0,0,0,0,2};Advertisement a={};a.phyTxTime=1;
    struct {uint32_t before;apple80211_peer_cache_control value;uint32_t after;} input;
    memset(&input,0xa5,sizeof(input));
    auto &in=input.value;in.version=1;in.operation=0;copyAddress(in.address,parent);
    auto invoke=[&] {return i.handle(Request::setPEER_CACHE_CONTROL,&in);};
    assert(invoke()==kIOReturnNotReady && i.native.messages.empty()); // unknown peer
    assert(i.device.awdlPeers.update(parent,i.address.octet,a,1000000,44,-40));
    auto peer=i.device.awdlPeers.find(parent,awdlNow());assert(peer);
    assert(invoke()==kIOReturnNotReady); // disabled
    i.requested=true;assert(invoke()==kIOReturnNotReady); // radio unavailable
    i.device.awdlRadio.ready=true;i.haveController=false;
    assert(invoke()==kIOReturnNotReady && !peer->nativeCached);
    i.haveController=true;i.stopping=true;
    assert(invoke()==kIOReturnNotReady && !i.native.calls);i.stopping=false;
    i.native.result=kIOReturnNoMemory;
    assert(invoke()==kIOReturnNoMemory && !peer->nativeCached && i.native.messages.empty());
    i.native.result=kIOReturnSuccess;
    assert(invoke()==kIOReturnSuccess && peer->nativeCached && i.native.messages.size()==1);
    assert(input.before==0xa5a5a5a5 && input.after==0xa5a5a5a5);
    const auto &added=i.native.messages[0];
    assert(added.operation==1 && added.reserved==0xa5a5 && sameAddress(added.address,parent));
    for(auto byte:added.radio_parameters) assert(!byte);
    assert(invoke()==kIOReturnSuccess && i.native.messages.size()==2); // host cache may restart
    assert(i.native.messages.back().operation==1 && i.device.awdlPeers.added==1);
    in.operation=6;in.reserved=0x1234;
    assert(invoke()==kIOReturnSuccess && i.native.messages.size()==3);
    assert(i.native.messages[0].reserved==0xa5a5 && i.native.messages.back().reserved==0x1234);
    assert(i.native.messages.back().operation==1 && peer->lastSeenUS==1000000);
    for(auto bad:{1u,3u,4u,5u,7u,0xffffffffu}) {
        in.operation=bad;assert(invoke()==kIOReturnUnsupported && peer->nativeCached);
    }
    in.operation=2;in.version=0;assert(invoke()==kIOReturnBadArgument);
    in.version=1;in.address[0]=1;assert(invoke()==kIOReturnBadArgument);
    copyAddress(in.address,i.address.octet);assert(invoke()==kIOReturnBadArgument);
    copyAddress(in.address,parent);i.native.result=kIOReturnNoMemory;
    assert(invoke()==kIOReturnNoMemory && peer->nativeCached); // failed delete retains reservation
    assert(i.native.messages.size()==3);
    peer->resetObservation(); // firmware reset loses RF timing, not host registration
    assert(peer->nativeCached && !peer->occupied && !peer->haveSync);
    in.operation=6;assert(invoke()==kIOReturnNotReady); // update cannot invent fresh RF state
    in.operation=2;i.requested=false;i.device.awdlRadio.ready=false;
    i.native.result=kIOReturnSuccess;
    assert(invoke()==kIOReturnSuccess && !peer->nativeCached && !peer->occupied);
    assert(i.native.messages.size()==4 && i.native.messages.back().operation==3);
    assert(i.native.messages.back().reserved==0x1234);
    assert(invoke()==kIOReturnSuccess && i.native.messages.size()==5); // deletion is idempotent
    i.device.awdlRadio.ready=i.requested=true;
    assert(i.device.awdlPeers.update(parent,i.address.octet,a,1000000,44,-40));
    in.operation=6;assert(invoke()==kIOReturnSuccess); // update restores driver registration
    assert(i.device.awdlPeers.findCached(parent));
    in.operation=0;assert(invoke()==kIOReturnSuccess);
    in.operation=2;assert(invoke()==kIOReturnSuccess);
    assert(i.device.awdlPeers.find(parent,awdlNow())); // unregister retains discovery
    assert(!i.device.awdlPeers.findCached(parent) && !i.unsupportedRequests);
}
static void dataPathTemplate() {
    AirportItlwmAWDLInterface i;
    // Synthetic native-layout payload: election-v2, 47-byte Data Path, version.
    std::vector<uint8_t> payload(43,0);payload[0]=24;payload[1]=40;
    copyAddress(payload.data()+3,i.sync.address);copyAddress(payload.data()+9,i.sync.address);
    putLE32(payload.data()+15,23);putLE32(payload.data()+23,555);putLE32(payload.data()+27,555);
    putLE32(payload.data()+39,23);
    uint8_t dataPath[50]={12,47,0};
    for(unsigned n=3;n<50;++n)dataPath[n]=uint8_t(n*7);
    putLE16(dataPath+3,0x8b03);putLE32(dataPath+26,0);
    payload.insert(payload.end(),dataPath,dataPath+50);
    payload.insert(payload.end(),{21,2,0,0x93,2,16,2,0,'o','k'});
    apple80211_awdl_sync_frame_template input={1,uint32_t(payload.size()),payload.data()};
    assert(i.handle(Request::setAWDL_SYNC_FRAME_TEMPLATE,&input)==kIOReturnSuccess);
    assert(i.templateSize==55 && !memcmp(i.frameTemplate,dataPath,50));
    auto directedPayload=payload;
    directedPayload.insert(directedPayload.end(),{35,2,0,1,3}); // 24H32 AirDrop TLV layout.
    std::fill(payload.begin(),payload.end(),0);
    i.sync.restart(44,1000000);for(auto &c:i.sync.channels)c=44;
    auto check=[&](bool mif) {
        uint8_t frame[1536];size_t size=i.sync.action(frame,sizeof(frame),1005000,mif,i.frameTemplate,i.templateSize);
        assert(size && frame[30]==(mif?3:0));
        unsigned found=0;
        for(size_t at=40;at<size;at+=3+le16(frame+at+1))if(frame[at]==12) {
            ++found;assert(le16(frame+at+1)==47 && !memcmp(frame+at,dataPath,50));
        }
        assert(found==1);
        Advertisement parsed={};assert(parseMPDU(frame,size,parsed)==ParseResult::Valid && parsed.version==0x93);
        const auto sequence=i.sync.wifiSequence;
        memset(frame,0xa5,sizeof(frame));
        assert(!i.sync.action(frame,size-1,1005000,mif,i.frameTemplate,i.templateSize));
        for(auto b:frame)assert(b==0xa5);
        assert(i.sync.wifiSequence==sequence);
    };
    check(false);check(true);
    uint8_t unicast[1536],destination[6]={2,0,0,0,0,2};
    auto unicastSize=i.sync.action(unicast,sizeof(unicast),1005000,true,
        directedPayload.data(),directedPayload.size(),destination);
    assert(unicastSize==184+directedPayload.size());
    assert(!memcmp(unicast+184,directedPayload.data(),directedPayload.size()));
    unsigned types[256]={};
    for(size_t at=40;at<unicastSize;at+=3+le16(unicast+at+1))++types[unicast[at]];
    for(unsigned type:{4,5,12,18,21,24,35})assert(types[type]==1);
    // A malformed replacement must leave both native election and owned TLVs intact.
    auto old=i.sync;std::vector<uint8_t> saved(i.frameTemplate,i.frameTemplate+i.templateSize);
    input.payload=dataPath;input.payload_len=49;
    assert(i.handle(Request::setAWDL_SYNC_FRAME_TEMPLATE,&input)==kIOReturnBadArgument);
    assert(!memcmp(&old,&i.sync,sizeof(old)) && !memcmp(saved.data(),i.frameTemplate,saved.size()));
    // Replacing or removing the host template replaces or restores the Data Path.
    dataPath[49]^=0x80;input.payload_len=50;
    assert(i.handle(Request::setAWDL_SYNC_FRAME_TEMPLATE,&input)==kIOReturnSuccess);
    assert(i.templateSize==50 && !memcmp(i.frameTemplate,dataPath,50));
    uint8_t frame[1536];size_t size=i.sync.action(frame,sizeof(frame),1005000,true,i.frameTemplate,i.templateSize);
    unsigned found=0;for(size_t at=40;at<size;at+=3+le16(frame+at+1))if(frame[at]==12) {
        ++found;assert(!memcmp(frame+at,dataPath,50));
    }
    assert(found==1);
    input.payload=nullptr;input.payload_len=0;
    assert(i.handle(Request::setAWDL_SYNC_FRAME_TEMPLATE,&input)==kIOReturnSuccess && !i.templateSize);
    size=i.sync.action(frame,sizeof(frame),1005000,true,i.frameTemplate,i.templateSize);found=0;
    for(size_t at=40;at<size;at+=3+le16(frame+at+1))if(frame[at]==12) {
        ++found;assert(le16(frame+at+1)==15 && le16(frame+at+3)==0x8f24);
    }
    assert(found==1);
}
static void broadcastControl() {
    static_assert(sizeof(apple80211_awdl_oob_request)==56,"native OOB size");
    static_assert(offsetof(apple80211_awdl_oob_request,tx_offset_us)==12,"native auto offset");
    static_assert(offsetof(apple80211_awdl_oob_request,tx_channel_map)==16,"native channel map");
    AirportItlwmAWDLInterface i;
    std::vector<uint8_t> p(43,0);p[0]=24;p[1]=40;
    copyAddress(p.data()+3,i.address.octet);copyAddress(p.data()+9,i.address.octet);
    p.insert(p.end(),{21,2,0,0x93,2,16,4,0,3,0x12,0x34,0x56});
    auto saved=p;
    apple80211_awdl_oob_request input={};memset(input.destination,0xff,6);
    copyAddress(input.bssid,BSSID);input.flags=6;input.tx_channel_map=0x05050505;
    input.tx_offset_us=17300;input.data=p.data();input.data_len=p.size();
    // The native configure path installs its automatic template before enabling sync.
    assert(!i.requested && !i.device.awdlRadio.ready);
    auto oldSync=i.sync;
    assert(i.handle(Request::setAWDL_OOB_AUTO_REQUEST,&input)==kIOReturnSuccess);
    assert(i.broadcast.configured && i.broadcast.channelMap==0x05050505 && i.broadcast.offsetUS==17300);
    assert(i.broadcast.size==saved.size() && !memcmp(i.broadcast.payload,saved.data(),saved.size()));
    assert(!memcmp(&i.sync,&oldSync,sizeof(oldSync)) && !i.templateSize);
    memset(p.data(),0xaa,p.size());
    assert(!memcmp(i.broadcast.payload,saved.data(),saved.size()));
    p=saved;input.data=p.data();
    const auto original=i.broadcast;
    for(unsigned n=0;n<9;++n) {
        auto bad=input;int expected=kIOReturnBadArgument;
        switch(n) {
        case 0:bad.version=1;break;
        case 1:bad.destination[5]=0xfe;break;
        case 2:bad.destination[0]=2;break;
        case 3:bad.bssid[5]^=1;break;
        case 4:bad.flags=14;expected=kIOReturnUnsupported;break;
        case 5:bad.tx_offset_us=65536;expected=kIOReturnUnsupported;break;
        case 6:bad.data=nullptr;break;
        case 7:bad.data_len=1201;break;
        case 8:bad.data_len=bad.data_len-1;break;
        }
        assert(i.handle(Request::setAWDL_OOB_AUTO_REQUEST,&bad)==expected);
        assert(!memcmp(&i.broadcast,&original,sizeof(original)));
    }
    // Native immediate MIF and periodic MIF have independently owned payloads.
    assert(i.handle(Request::setAWDL_OOB_REQUEST,&input)==kIOReturnNotReady);
    i.requested=i.device.awdlRadio.ready=true;
    p.back()=0x99;
    assert(i.handle(Request::setAWDL_OOB_REQUEST,&input)==kIOReturnSuccess);
    assert(i.immediateBroadcast.size==p.size() && i.immediateBroadcast.payload[p.size()-1]==0x99);
    assert(i.handle(Request::setAWDL_OOB_REQUEST,&input)==kIOReturnBusy);
    assert(!memcmp(&i.broadcast,&original,sizeof(original)));
    i.sync.restart(44,1000000);for(auto &c:i.sync.channels)c=44;
    uint8_t frame[1536];
    auto size=i.sync.action(frame,sizeof(frame),1010000,true,saved.data(),saved.size(),nullptr,true);
    assert(size==184+saved.size() && !memcmp(frame+184,saved.data(),saved.size()));
    for(unsigned n=0;n<6;++n)assert(frame[4+n]==0xff);
    unsigned types[256]={};
    for(size_t at=40;at<size;at+=3+le16(frame+at+1))++types[frame[at]];
    for(unsigned n:{4,5,16,18,21,24})assert(types[n]==1);
    assert(!types[6] && !types[12]); // No synthesized TLVs mixed into the OOB template.
    auto sequence=i.sync.wifiSequence;memset(frame,0xa5,sizeof(frame));
    assert(!i.sync.action(frame,size-1,1010000,true,saved.data(),saved.size(),nullptr,true));
    assert(!i.sync.action(frame,sizeof(frame),1010000,false,saved.data(),saved.size(),nullptr,true));
    for(auto b:frame)assert(b==0xa5);
    assert(i.sync.wifiSequence==sequence);
    // The 1200-byte native buffer limit includes opaque service TLVs.
    auto large=saved;large.push_back(0xfe);large.push_back(0);large.push_back(0);
    putLE16(large.data()+saved.size()+1,1200-large.size());large.resize(1200,0x5a);
    input.data=large.data();input.data_len=large.size();
    assert(i.handle(Request::setAWDL_OOB_AUTO_REQUEST,&input)==kIOReturnSuccess);
    assert(i.sync.action(frame,sizeof(frame),1010000,true,i.broadcast.payload,i.broadcast.size,nullptr,true)==1384);
    input.tx_channel_map=0;input.data=nullptr;input.data_len=0;
    assert(i.handle(Request::setAWDL_OOB_AUTO_REQUEST,&input)==kIOReturnSuccess);
    assert(i.broadcast.configured && !i.broadcast.size && !i.broadcast.channelMap && !i.unsupportedRequests);
}
static void actionTxMode() {
    static_assert(sizeof(apple80211_awdl_af_tx_mode)==8,"24H32 AF mode request size");
    static_assert(offsetof(apple80211_awdl_af_tx_mode,mode)==4,"24H32 AF mode field");
    AirportItlwmAWDLInterface i;
    struct {uint32_t before;apple80211_awdl_af_tx_mode value;uint32_t after;} read;
    memset(&read,0xa5,sizeof(read));
    assert(i.handle(Request::getAWDL_AF_TX_MODE,&read.value)==kIOReturnSuccess);
    assert(read.before==0xa5a5a5a5 && read.after==0xa5a5a5a5 && read.value.version==1 && !read.value.mode);
    apple80211_awdl_af_tx_mode input={0,3};
    assert(!i.device.awdlRadio.ready && !i.requested);
    assert(i.handle(Request::setAWDL_AF_TX_MODE,&input)==kIOReturnSuccess && i.afTxMode==3);
    i.nextActionUS=123;i.nextMIFUS=456;
    for(uint32_t bad:{1u,2u,4u,5u,0xffffffffu}) {
        input.mode=bad;
        assert(i.handle(Request::setAWDL_AF_TX_MODE,&input)==kIOReturnUnsupported && i.afTxMode==3);
        assert(i.nextActionUS==123 && i.nextMIFUS==456);
    }
    assert(i.handle(Request::getAWDL_AF_TX_MODE,&read.value)==kIOReturnSuccess && read.value.mode==3);
    input.mode=0;
    assert(i.handle(Request::setAWDL_AF_TX_MODE,&input)==kIOReturnSuccess && !i.afTxMode);
    assert(!i.nextActionUS && i.nextMIFUS==456);
    // Repeating the active policy must not cause another immediate PSF.
    i.nextActionUS=321;
    assert(i.handle(Request::setAWDL_AF_TX_MODE,&input)==kIOReturnSuccess && i.nextActionUS==321);
    assert(!i.requested && !i.unsupportedRequests);
    assert(read.before==0xa5a5a5a5 && read.after==0xa5a5a5a5);
}
int main() {
#if AWDL_CONTROL_CASE == 1
    cacheControl();
#elif AWDL_CONTROL_CASE == 2
    dataPathTemplate();
#elif AWDL_CONTROL_CASE == 3
    broadcastControl();
#elif AWDL_CONTROL_CASE == 4
    actionTxMode();
#elif AWDL_CONTROL_CASE == 5
    nativeChannelMetadata();
#elif AWDL_CONTROL_CASE == 6
    wideChannelMetadata();
#else
    syncParameters();electionWire();channelConfiguration();masterConfiguration();rssiConfiguration();
    frameTemplates();syncState();electionParameters();advertisers();minimumRate();
#endif
}
