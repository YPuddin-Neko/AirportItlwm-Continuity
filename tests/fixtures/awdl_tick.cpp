#include <cassert>
#include <cerrno>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>
#include "AirportItlwm/AWDL/SyncEngine.hpp"
#include "AirportItlwm/AWDL/RadioState.hpp"
#include "AirportItlwm/AWDL/BlockAck.hpp"
#include "AirportItlwm/AWDL/ServiceMerge.hpp"
#include "AirportItlwm/AWDL/NativePeer.hpp"
#include "AirportItlwm/AWDL/LLWFlow.hpp"
#include "itlwm/hal_iwx/BTCoex.hpp"
#define MIN(a,b) ((a)<(b)?(a):(b))
using namespace AirportAWDL;
#define __MAC_15_2 150200
#define __IO80211_TARGET __MAC_15_2
// NATIVE DECLARATIONS
static uint32_t be32(const uint8_t *p) {return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
static uint32_t OSSwapHostToBigInt32(uint32_t v) {return __builtin_bswap32(v);}
enum {APPLE80211_VERSION=1,APPLE80211_C_FLAG_20MHZ=2,APPLE80211_C_FLAG_80MHZ=0x400,
      APPLE80211_C_FLAG_2GHZ=8,APPLE80211_C_FLAG_5GHZ=16};
struct WorkLoop {
    bool gated=false,thread=false;
    bool inGate() {return gated;}
    bool onThread() {return thread;}
};
struct Controller {
    struct Message {unsigned type;std::vector<uint8_t> bytes;};
    std::vector<Message> messages;
    std::function<void()> onEnd;
    unsigned failures=0;
    WorkLoop *queue=nullptr;
    WorkLoop *getWorkQueue() {return queue;}
    int postMessage(void *,unsigned type,void *data,size_t size,bool async) {
        assert(async && ((type==41 && size==36) || (type==43 && size==40) || (type==69 && size==7)));
        if(failures) {--failures;return 4;}
        auto p=static_cast<uint8_t *>(data);messages.push_back({type,{p,p+size}});
        if(type==43 && onEnd)onEnd();
        return 0;
    }
};
static uint64_t clockUS=1000000;
static uint64_t awdlNow() { return clockUS; }
using AbsoluteTime=uint64_t;
static void clock_get_uptime(AbsoluteTime *time) { *time=clockUS*7+0x100000000ULL; }
static unsigned allocations=0;
static unsigned failedAllocations=0;
static unsigned failedDuplicates=0;
static uint64_t allocationDelayUS=0;
struct Packet { std::vector<uint8_t> bytes; size_t length; Packet *next=nullptr; };
using mbuf_t=Packet *;
static mbuf_t awdlPacket(size_t size) {
    clockUS+=allocationDelayUS;
    if(failedAllocations) {--failedAllocations;return nullptr;}
    ++allocations;return new Packet{std::vector<uint8_t>(size),size};
}
static void mbuf_freem(mbuf_t p) { assert(p && allocations && !p->next);--allocations;delete p; }
static mbuf_t mbuf_nextpkt(mbuf_t p) {return p->next;}
static void mbuf_setnextpkt(mbuf_t p,mbuf_t next) {p->next=next;}
static void *mbuf_data(mbuf_t p) { return p->bytes.data(); }
static size_t mbuf_pkthdr_len(mbuf_t p) { return p->length; }
static void mbuf_setlen(mbuf_t p,size_t size) { assert(size<=p->bytes.size());p->length=size; }
static void mbuf_pkthdr_setlen(mbuf_t p,size_t size) { mbuf_setlen(p,size); }
static int mbuf_copydata(mbuf_t p,size_t offset,size_t size,void *out) {
    assert(offset+size<=p->length);std::memcpy(out,p->bytes.data()+offset,size);return 0;
}
enum { MBUF_DONTWAIT=0 };
static int mbuf_dup(mbuf_t source,int,mbuf_t *copy) {
    assert(!source->next);*copy=nullptr;
    if(failedDuplicates) {--failedDuplicates;return ENOMEM;}
    *copy=awdlPacket(source->length);
    if(!*copy)return ENOMEM;
    (*copy)->bytes=source->bytes;return 0;
}
enum { kIO80211NetworkLinkDown=0,kIO80211NetworkLinkUp=1 };
enum { IEEE80211_S_RUN=4, IFF_UP=1, IFF_RUNNING=0x40, IWX_FLAG_RFKILL=2,
       IWX_FLAG_HW_ERR=0x80, IWX_FLAG_SHUTDOWN=0x100 };
using IOReturn=int;
using SInt32=int;
enum {kIOReturnSuccess=0,kIOReturnNoResources=1,kIOReturnNoFrames=2,
      kIOReturnNotReady=3,kIOReturnIOError=4,kIOReturnBadArgument=5,kIOReturnUnsupported=6,kIOReturnBusy=7,IWX_INVALID_QUEUE=0xffff};
struct OSObject {
    bool managerClass=true;
    void *owner=nullptr;
    OSObject *metaCast(const char *name) {
        assert(!strcmp(name,"IO80211AWDLPeerManager"));
        return managerClass?this:nullptr;
    }
};
struct Gate {
    WorkLoop *loop;unsigned entries=0;
    IOReturn runAction(IOReturn (*action)(OSObject *,void *,void *,void *,void *),void *self,void *arg=nullptr) {
        assert(!loop->gated);++entries;loop->gated=true;
        auto result=action(nullptr,self,arg,nullptr,nullptr);
        loop->gated=false;return result;
    }
};
struct IO80211VirtualInterface {
    bool nativeUp=false;
    unsigned nativeTransitions=0;
    int lastLinkQuality=-2;
    unsigned linkQualityUpdates=0;
    std::function<void()> onLinkUp;
    std::function<void()> onLinkDown;
    int enableError=0;
    SInt32 setInterfaceEnable(bool) {return enableError;}
    IOReturn reportLinkQuality(unsigned q) {
        lastLinkQuality=int(int32_t(q));
        ++linkQualityUpdates;
        return kIOReturnSuccess;
    }
    void setLinkState(int state,int) {
        if (nativeUp==bool(state)) return;
        nativeUp=state;++nativeTransitions;
        if (state && onLinkUp) onLinkUp();
        if (!state && onLinkDown) onLinkDown();
    }
};
struct BSD {
    bool up=false,attached=true;unsigned starts=0,signals=0,dequeues=0,lastLimit=0;
    int dequeueResult=kIOReturnSuccess;
    std::deque<mbuf_t> output;
    std::function<void()> onDequeue;
    ~BSD() {for(auto p:output)mbuf_freem(p);}
    void *getIfnet() { return attached?this:nullptr; }
    void setLinkState(int state) { up=state; }
    void startOutputThread() { assert(up);++starts; }
    void signalOutputThread() {assert(up);++signals;}
    IOReturn dequeueOutputPackets(unsigned limit,mbuf_t *packets) {
        assert(limit && limit<=32);lastLimit=limit;++dequeues;
        if(onDequeue)onDequeue();
        if(dequeueResult)return dequeueResult;
        if(output.empty())return kIOReturnNoFrames;
        mbuf_t *next=packets;
        while(limit-- && !output.empty()) {
            *next=output.front();output.pop_front();next=&(*next)->next;
        }
        *next=nullptr;return kIOReturnSuccess;
    }
};
struct Timer {
    unsigned delay=0,delayUS=0,calls=0;
    void setTimeoutMS(unsigned ms) { delay=ms;delayUS=ms*1000;++calls; }
    void setTimeoutUS(unsigned us) { delay=(us+999)/1000;delayUS=us;++calls; }
};
struct Radio {
    Radio() {awdlRadio.widthMHz=20;}
    void serviceAWDLBlockAck() {}
    void serviceAWDLTXBlockAck() {}
    int serviceAWDLReceiveFilter(uint64_t (*end)(const void *,uint64_t),const void *context) {
        filterWindowEnd=end(context,clockUS);return 0;
    }
    uint64_t filterWindowEnd=0;
    int setAWDLTXWindow(uint64_t (*end)(const void *,uint64_t),const void *context) {
        auto &tx=awdlRadio.blockAckTX;if(!tx.configured)return 0;
        tx.windowEndUS=end(context,clockUS);tx.paused=!tx.windowEndUS;return 0;
    }
    bool completeAWDLTXManagement(uint64_t token,bool ack) {
        auto &tx=awdlRadio.blockAckTX;if(!token || token!=tx.managementToken)return false;
        tx.managementToken=0;if(ack && tx.phase==TXBlockAckQueue::Retiring)tx.needDelba=false;return true;
    }
    void noteAWDLTXBlockAck(const uint8_t *peer,size_t bytes,bool accepted=false) {
        awdlRadio.blockAckTX.demand(peer,bytes,clockUS,accepted);
    }
    bool awdlTxWindowOpen() {return awdlRadio.ready && awdlRadio.dwell.running && clockUS<awdlRadio.dwell.windowEndUS;}

    int baResult=EOPNOTSUPP;uint64_t baCookie=1;bool baLive=true;
    unsigned baPreparations=0,baEnds=0;
    std::function<void()> onBAPrepare;
    int prepareAWDLBlockAck(const BlockAckRequest &,uint64_t &cookie) {
        ++baPreparations;cookie=baResult?0:baCookie;if(onBAPrepare)onBAPrepare();return baResult;
    }
    bool awdlBlockAckLive(const BlockAckRequest &,uint64_t cookie) {return baLive && cookie==baCookie;}
    void completeAWDLBlockAckEnd(const BlockAckRequest &,uint64_t cookie) {assert(cookie==baCookie);++baEnds;}

    uint64_t dmaEpoch = 1;
    uint8_t awdlMinRateMbps=12,awdlRateLastRetries=0;
    uint64_t awdlRateSamples=0,awdlRateIgnored=0,awdlRateProbes=0,awdlRateRaised=0,awdlRateLowered=0;
    uint32_t awdlRateLastFlags=0;
    uint16_t awdlRateLastMediaUS=0;
    int awdlReceiveFilterCleanupResult=0;
    struct Reports {template<class T>void publish(T &) {}} awdlBroadcastReports;
    WorkLoop loop;Gate gate{&loop};
    WorkLoop *getMainWorkLoop() {return &loop;}
    Gate *getMainCommandGate() {return &gate;}
    RadioState awdlRadio={};PeerTable awdlPeers={};
    IwxBTCoexState btCoex={};
    struct {
        struct { unsigned queued=0; } txq[4];
        struct { struct { struct { unsigned if_flags=IFF_UP|IFF_RUNNING; } ac_if; } ic_ac; } sc_ic;
        unsigned sc_flags=0;
        int ns_nstate=IEEE80211_S_RUN;
    } com;
    unsigned starts=0,stops=0,psf=0,mif=0,data=0;
    unsigned transmitAttempts=0;
    int transmitError=0;
    std::vector<char> transmissions;
    std::vector<uint64_t> tokens, dataTokens, transmitDeadlines;
    std::function<void(uint64_t,int)> dataCompletion;
    bool acknowledgeData=true;
    std::vector<uint8_t> lastUnicast;
    std::vector<std::vector<uint8_t>> broadcastFrames;
    std::vector<std::vector<uint8_t>> dataFrames;
    bool deferStop=false,deferChannel=false;
    int stopError=0,startError=0;
    std::function<void()> onStart,onService,onTransmit;
    std::function<void(uint64_t)> onReject;
    int startAWDLRadio(const uint8_t *) {
        ++starts;if(onStart)onStart();
        if(startError)return startError;
        awdlRadio.ready=true;awdlRadio.queue=1;awdlRadio.channel=44;awdlRadio.widthMHz=20;return 0;
    }
    bool allow149=true;
    bool awdlChannelAllowed(unsigned c,uint8_t width=20) {return width==20 && (c==6||c==44||(c==149&&allow149));}
    int stopAWDLRadio() {
        ++stops;awdlRadio.ready=false;awdlRadio.dwell.running=false;
        if(stopError) {
            awdlRadio.stopPending=false;awdlRadio.fault=true;awdlRadio.cleanupError=stopError;return stopError;
        }
        if(deferStop && awdlRadio.dwell.requested && !awdlRadio.dwell.done) {
            awdlRadio.stopPending=true;awdlRadio.dwell.cancelling=true;
            awdlRadio.dwell.cancelAcknowledged=true;return EINPROGRESS;
        }
        awdlRadio.stopPending=false;awdlRadio.dwell={};return 0;
    }
    RadioState::GrantPolicy lastPolicy={};
    int serviceAWDLChannel(unsigned c,uint64_t endUS,RadioState::GrantPolicy policy = {}) {
        lastPolicy=policy;
        if(onService)onService();
        if(deferChannel && awdlRadio.dwell.cancelling) {
            if(!awdlRadio.dwell.done)return 0;
            awdlRadio.dwell={};
        }
        if(!c) {awdlRadio.dwell.running=false;awdlRadio.queue=0xffff;return 0;}
        if(!awdlRadio.dwell.running || awdlRadio.channel!=c || clockUS>=awdlRadio.dwell.windowEndUS) {
            ++awdlRadio.dwellStarts;
            awdlRadio.dwell.windowEndUS=endUS;
            awdlRadio.dataQueueConfigured=policy.dataQueue;awdlRadio.dataQueue=policy.dataQueue?2:IWX_INVALID_QUEUE;
        }
        awdlRadio.channel=c;awdlRadio.widthMHz=policy.widthMHz;awdlRadio.queue=1;
        awdlRadio.dwell.requested=awdlRadio.dwell.running=true;return 0;
    }
    int transmitAWDL(mbuf_t packet,uint64_t token=0,uint64_t deadlineUS=0) {
        transmitDeadlines.push_back(deadlineUS);
        ++transmitAttempts;
        if (transmitError) {
            const int error = transmitError;
            mbuf_freem(packet);
            if (onReject) onReject(token);
            return error;
        }
        if(token && packet->bytes[0]==0xd0) {
            assert(unicastAddress(packet->bytes.data()+4));tokens.push_back(token);
            lastUnicast.assign(packet->bytes.begin(),packet->bytes.begin()+packet->length);
            transmissions.push_back('U');
        } else if(packet->bytes[0]==0xd0) {
            broadcastFrames.emplace_back(packet->bytes.begin(),packet->bytes.begin()+packet->length);
            Advertisement a={};
            assert(parseMPDU(packet->bytes.data(),packet->length,a)==ParseResult::Valid);
            assert(a.phyTxTime==uint32_t(clockUS));
            if(a.subtype==3) {++mif;transmissions.push_back('M');}
            else {assert(a.subtype==0);++psf;transmissions.push_back('P');}
        } else {
            dataFrames.emplace_back(packet->bytes.begin(),packet->bytes.begin()+packet->length);
            assert(token);dataTokens.push_back(token);
            ++data;transmissions.push_back('D');
            if(acknowledgeData)dataCompletion(token,0);
        }
        ++awdlRadio.submitted;mbuf_freem(packet);if(onTransmit)onTransmit();return 0;
    }
};
struct AirportItlwmAWDLInterface:IO80211VirtualInterface {
    void publishReceiveFilter() {}
    Radio device,*radio=&device;Timer scheduler,*timer=&scheduler;BSD network,*bsd=&network;
    Controller controller;
    bool haveController=true,haveManager=true;
    OSObject nativeManager;
    unsigned nativeManagerReads=0;
    std::vector<bool> nativeSyncStates;
    std::vector<uint64_t> nativeRxActivity,nativeTxActivity;
    std::vector<AirportAWDLNativeDataCountersDelta> nativeDataDeltas;
    Controller *getController() {return haveController?&controller:nullptr;}
    enum class Request {getAWDL_SYNC_ENABLED};
    IOReturn getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *);
    IOReturn request(Request,void *);
    IOReturn handle(Request,void *);
    void publishNativeSyncState();
    void recordNativeDataActivity(bool,uint32_t);
    bool nativeSyncReady=false;
    uint64_t nativeSyncStateCalls=0;
    uint64_t nativeDataPeerNotifications=0,nativeDataPeerLockMisses=0;
    ServiceMergeCache serviceMerge={};
    bool serviceMergeEnabled=true;
    int32_t reportedLinkQuality=-2;
    bool receiveFilterChanging=false;
    uint64_t relayRecoveryWindowStarts=0;
    uint64_t parentRecoveryWindowStarts=0;
    bool nativeWindowOpen=false,nativeWindowClosing=false;
    uint64_t nativeWindowSerial=0,nativeWindowEndUS=0;
    uint16_t nativeWindowCounter=0,nativeWindowChannel=0,nativeWindowPeriodTU=0,nativeWindowPresence=0;
    uint64_t nativeWindowStarts=0,nativeWindowEnds=0,nativeWindowErrors=0;
    apple80211_availability_window_extensions_end nativeWindowEnd={};
    void updateAvailability();
    UnicastRequest unicast[16]={};
    BroadcastTemplate broadcast={};BroadcastRequest immediateBroadcast={};
    uint64_t broadcastSubmitted=0;
    uint64_t unicastToken=0,unicastSubmitted=0,unicastAcknowledged=0,unicastFailed=0,unicastNotifyErrors=0;
    IOReturn queueUnicast(const apple80211_awdl_oob_request &);
    IOReturn configureBroadcast(const apple80211_awdl_oob_request &,bool);
    bool broadcastDue(uint64_t) const;
    bool psfDue(uint64_t) const;
    uint64_t peerGrantEnd(uint64_t, const RadioState::PeerGrant &, bool) const;
    void serviceUnicast();void completeUnicast(uint64_t,bool);
    static void receiveBlockAck(void *,const BlockAckRequest &);
    static void receiveBlockAckEnd(void *,const BlockAckRequest &);
    uint64_t nextManagementToken();void serviceBlockAck();void serviceTXBlockAck();
    BlockAckReply blockAckReplies[8]={};
    uint64_t blockAckReceived=0,blockAckQueued=0,blockAckCoalesced=0,blockAckRejected=0;
    uint64_t blockAckSubmitted=0,blockAckAcknowledged=0,blockAckFailed=0,blockAckExpired=0;
    SyncEngine sync={};struct { uint8_t octet[6]={2,0,0,0,0,1}; } address;
    bool stopping=false,ticking=false,drainingData=false,wakeArmed=false,requested=true,running=false,interfaceEnabled=true;
    bool infrastructurePaused=false;
    bool sequenceConfigured=false;
    bool regulatoryPlanBlocked=false,regulatoryStopRequired=false,regulatoryResumePending=false;
    void regulatoryMapChanged();
    uint32_t afTxMode=0;
    uint64_t nextRetryUS=0,nextActionUS=0,nextMIFUS=0,lastSnapshotUS=0;
    uint64_t rxDelivered=0,rxDropped=0,actionDelivered=0,actionDropped=0,txDropped=0;
    struct MockLLWIf {
        bool isSkywalkNative() const { return false; }
        void *getBSDInterface() const { return nullptr; }
        uint64_t skywalkTxCount() const { return 0; }
        uint64_t skywalkTxDropCount() const { return 0; }
        uint64_t skywalkRxCount() const { return 0; }
        uint64_t skywalkRxDropCount() const { return 0; }
    };
    MockLLWIf *llwInterface=nullptr;
    BSD *llwBSD=nullptr;
    bool llwActive=false,llwLinkUp=false,llwParamsConfigured=false,llwParamsEnabled=false;
    struct { uint8_t peer_mac[6]={}; } llwParams={};
    void syncLLWLinkState() {}
    LLWFlowRouter llwFlows={};
    uint64_t llwCreateRequests=0,llwDestroyRequests=0,llwParamsGetCalls=0,llwParamsSetCalls=0;
    uint64_t llwTxQueued=0,llwRxDelivered=0,llwRxDropped=0,llwNDPMirrored=0;
    uint8_t frameTemplate[1024]={};size_t templateSize=0;
    static constexpr uint64_t DataLifetimeUS = 17ULL * 4 * 16 * 1024;
    static constexpr uint8_t MaxDataFlushes = 8;
    struct Pending {
        enum : uint8_t { MulticastUnpaired, MulticastSocial, MulticastSlot8 };
        mbuf_t packet;
        uint64_t queuedUS, epoch, grant;
        uint16_t wifiSequence, dataSequence;
        bool retry, group, lowLatency;
        uint8_t multicastQueue, flushes;
        mbuf_t second;
        uint64_t secondQueuedUS;
        uint16_t secondSequence;
        unsigned count() const { return second ? 2 : 1; }
    } pending[64]={};
    void releaseData(Pending &);
    struct Flight { Pending entry;uint64_t token; } inFlight[64]={};
    unsigned inFlightCount=0,aggregateExtraOwned=0;bool outputCapacityReleased=false;
    uint64_t dataToken=0,dataSubmitted=0,dataRetried=0,dataFlushed=0,dataTxExpired=0;
    uint64_t dataUnicastAcknowledged=0,dataGroupCompleted=0,dataAbandoned=0;
    uint64_t multicastCopiesQueued=0,multicastCopiesDropped=0,dataAggregatesSubmitted=0;
    void completeData(uint64_t,int);
    unsigned pendingCount=0,pendingHead=0,references=1;
    bool publishedRadioReady=false;
    AirportItlwmAWDLInterface() {
        sync.start(address.octet,0,clockUS);
        controller.queue=&device.loop;nativeManager.owner=this;
        device.dataCompletion=[this](uint64_t token,int result){completeData(token,result);};
    }
    void retain() { ++references; }
    void release() { assert(references>1);--references; }
    template<typename T>void setProperty(const char *name,T value,unsigned=0) {
        if(!strcmp(name,"RadioReady"))publishedRadioReady=bool(value);
    }
    void drain();void tick();IOReturn pullOutputPackets();
    IOReturn pullLLWOutputPackets() { return kIOReturnNoResources; }
    IOReturn submitOutputPackets(bool);
    void drainPendingData();
    static void txReady(void *);
    IOReturn setEnabled(bool);SInt32 setInterfaceEnable(bool);
    void enqueue(mbuf_t p) { assert(pendingCount+inFlightCount+aggregateExtraOwned<64);pending[(pendingHead+pendingCount++)%64]={p,clockUS,device.dmaEpoch}; }
};

extern "C" OSObject *AirportAWDLGetNativePeerManager(void *object) {
    auto interface=static_cast<AirportItlwmAWDLInterface *>(object);
    ++interface->nativeManagerReads;
    return interface->haveManager?&interface->nativeManager:nullptr;
}
extern "C" void AirportAWDLNativeSyncStateChanged(void *object) {
    auto manager=static_cast<OSObject *>(object);
    auto interface=static_cast<AirportItlwmAWDLInterface *>(manager->owner);
    assert(manager==&interface->nativeManager && manager->managerClass);
    assert(interface->device.loop.inGate());
    apple80211_awdl_sync_enabled state={};
    assert(!interface->getAWDL_SYNC_ENABLED(&state));
    assert(state.version==APPLE80211_VERSION && !state.unk1);
    assert(interface->nativeSyncReady==bool(state.enabled));
    interface->nativeSyncStates.push_back(state.enabled!=0);
}
extern "C" void AirportAWDLSetNativeRxActivity(void *object,uint64_t time) {
    auto manager=static_cast<OSObject *>(object);
    auto interface=static_cast<AirportItlwmAWDLInterface *>(manager->owner);
    assert(manager==&interface->nativeManager && manager->managerClass && interface->device.loop.inGate());
    assert(time==clockUS*7+0x100000000ULL);
    interface->nativeRxActivity.push_back(time);
}
extern "C" void AirportAWDLSetNativeTxActivity(void *object,uint64_t time) {
    auto manager=static_cast<OSObject *>(object);
    auto interface=static_cast<AirportItlwmAWDLInterface *>(manager->owner);
    assert(manager==&interface->nativeManager && manager->managerClass && interface->device.loop.inGate());
    assert(time==clockUS*7+0x100000000ULL);
    interface->nativeTxActivity.push_back(time);
}
extern "C" void AirportAWDLUpdateNativeDataCounters(void *object,const AirportAWDLNativeDataCountersDelta *delta) {
    auto manager=static_cast<OSObject *>(object);
    auto interface=static_cast<AirportItlwmAWDLInterface *>(manager->owner);
    assert(manager==&interface->nativeManager && manager->managerClass && interface->device.loop.inGate());
    assert(delta->txPackets==1 && delta->txBytes && !delta->rxPackets && !delta->rxBytes);
    assert(!delta->reservedHeader);
    for(auto value:delta->reserved)assert(!value);
    interface->nativeDataDeltas.push_back(*delta);
}

// PRODUCTION

static void timers() {
    AirportItlwmAWDLInterface i;
    // A sleeping firmware command must not leave the first action timestamp stale.
    i.device.onStart=[&] { clockUS+=300000; i.tick();assert(i.device.starts==1); };
    i.tick();
    assert(i.references==1 && !i.ticking && i.running && i.nativeUp && i.network.up);
    assert(i.sync.epochUS==clockUS && i.device.mif==1 && i.device.psf==1);
    for(auto &c:i.sync.channels)c=44;
    i.tick();assert(i.device.mif==1 && i.device.psf==1);
    for(unsigned n=0;n<200;++n) { clockUS+=5000;i.tick(); }
    assert(i.device.mif>=15 && i.device.psf>=8); // MIF must not postpone PSF indefinitely.
    assert(i.network.starts==1 && i.scheduler.delay==5 && !allocations);
    i.requested=false;i.device.stopAWDLRadio();i.tick();
    assert(!i.running && i.nativeUp && i.network.up && i.scheduler.delay==100);
    unsigned transmitted=i.device.mif+i.device.psf;
    clockUS+=1000000;i.tick();assert(i.device.mif+i.device.psf==transmitted);
}
static void cancellation() {
    for(bool stop:{false,true}) {
        AirportItlwmAWDLInterface i;
        i.device.onStart=[&] {
            assert(i.ticking && i.references==2);
            clockUS+=500000;i.requested=false;
            if(stop) { i.stopping=true;i.timer=nullptr; }
        };
        i.tick();
        assert(i.device.starts==1 && i.device.stops==1 && !i.device.awdlRadio.ready);
        assert(!i.device.mif && !i.device.psf && !i.running && !i.nativeUp);
        assert(!i.ticking && i.references==1);
        assert(i.scheduler.calls==(stop?0:1));
        i.stopping=true;i.tick();assert(i.references==1);
    }
    AirportItlwmAWDLInterface i;i.tick();
    const auto mif=i.device.mif,psf=i.device.psf;
    for(auto &c:i.sync.channels)c=44;
    i.device.onService=[&] {clockUS+=20000;i.requested=false;};
    i.tick();assert(i.device.stops==1 && i.device.mif==mif && i.device.psf==psf);
    assert(!i.ticking && i.references==1);
}
static mbuf_t ethernet(const uint8_t *self,const uint8_t *dest) {
    auto p=awdlPacket(54);auto b=p->bytes.data();
    copyAddress(b,dest);copyAddress(b+6,self);b[12]=0x86;b[13]=0xdd;b[14]=0x60;
    return p;
}
static void dataPeer(AirportItlwmAWDLInterface &i,const uint8_t *dest) {
    i.tick();
    for(auto &c:i.sync.channels)c=44;
    i.tick();
    SyncEngine sender;sender.start(dest,44,clockUS);
    uint8_t bytes[512];Advertisement a={};
    auto size=sender.action(bytes,sizeof(bytes),clockUS,true,nullptr,0);
    assert(parseMPDU(bytes,size,a)==ParseResult::Valid);
    assert(i.device.awdlPeers.update(dest,i.address.octet,a,clockUS,44,-40));
    clockUS+=5000;
}
static void dataWindowRecheck() {
    const uint8_t dest[]={2,0,0,0,0,2};
    for(unsigned delay:{58000,60536,65536}) {
        AirportItlwmAWDLInterface i;dataPeer(i,dest);
        auto packet=ethernet(i.address.octet,dest);i.enqueue(packet);
        const auto queued=clockUS;const auto sequence=i.sync.dataSequence;
        allocationDelayUS=delay;i.tick();allocationDelayUS=0;
        assert(!i.device.data && i.pendingCount==1 && !i.txDropped);
        assert(i.pending[i.pendingHead].packet==packet && i.pending[i.pendingHead].queuedUS==queued);
        assert(i.sync.dataSequence==sequence && allocations==1);
        clockUS=i.sync.epochUS+65536+5000;i.tick();
        assert(i.device.data==1 && !i.pendingCount && !i.txDropped && !allocations);
    }
}
static void dataRecipientCutoff() {
    const uint8_t destination[] = {2,0,0,0,0,2};
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        clockUS = 10000000;
        AirportItlwmAWDLInterface i;dataPeer(i, destination);
        i.nextActionUS = i.nextMIFUS = clockUS + 1000000;
        auto peer = i.device.awdlPeers.find(destination, clockUS);assert(peer);
        // The remote current EAW ends before the dense local EAW.
        peer->sync.nextWindowTU = 32;
        if (scenario == 1) peer->haveSequence = false;
        if (scenario == 2) {
            // Exactly the existing edge guard leaves no usable TX time.
            peer->syncReceivedUS = clockUS - (32768 - 3000);
        }
        Window remote = {};
        const bool known = peer->window(clockUS, remote);
        if (scenario == 0 || scenario == 3) assert(known && remote.untilNextEAWUS > 3000);
        if (scenario == 2) assert(known && remote.untilNextEAWUS == 3000);
        const uint64_t endUS = known ? clockUS + remote.untilNextEAWUS - 3000 : 0;
        i.enqueue(ethernet(i.address.octet, destination));
        const auto attempts = i.device.transmitAttempts;
        if (scenario == 3) allocationDelayUS = remote.untilNextEAWUS - 2999;
        i.tick();allocationDelayUS = 0;
        if (scenario == 0) {
            assert(i.device.data == 1 && !i.pendingCount);
            assert(i.device.transmitDeadlines.back() == endUS && endUS > clockUS);
        } else {
            assert(!i.device.data && i.pendingCount == 1 && i.device.transmitAttempts == attempts);
        }
        i.drain();assert(!allocations);
    }
}

static void multicastDiscoveryWindows() {
    const uint8_t group[]={0x33,0x33,0,0,0,0xfb};
    for(unsigned slot:{0,8,10}) {
        AirportItlwmAWDLInterface i;i.tick();
        const auto epoch=i.sync.epochUS;
        for(auto &channel:i.sync.channels)channel=0;
        i.sync.channels[slot]=44;
        auto packet=ethernet(i.address.octet,group);const auto original=packet->bytes;
        i.network.output.push_back(packet);assert(!i.pullOutputPackets());
        assert(i.pendingCount==2 && !i.device.data);
        for(unsigned before=0;before<slot;++before) {
            clockUS=epoch+before*65536+5000;i.tick();
            assert(i.pendingCount==2 && !i.device.data && !i.txDropped);
        }
        clockUS=epoch+slot*65536+5000;i.tick();
        assert(!i.device.transmitDeadlines.back());
        assert(i.device.data==1 && i.pendingCount==1 && !i.txDropped && allocations==1);
        DataBatch decoded={};const auto &frame=i.device.dataFrames.back();
        assert(parseDataMPDU(frame.data(),frame.size(),decoded)==ParseResult::Valid && decoded.count==1);
        uint8_t restored[54];assert(ethernetFromData(decoded.frames[0],restored,sizeof(restored))==54);
        assert(!memcmp(restored,original.data(),54) && i.network.signals==1);
        i.drain();
    }
}
static mbuf_t bonjourPacket(const uint8_t *self,bool answer) {
    // PTR query or announcement for test._airdrop._tcp.local., fe80::1 -> ff02::fb.
    const uint8_t group[]={0x33,0x33,0,0,0,0xfb};
    const uint8_t name[]={8,'_','a','i','r','d','r','o','p',4,'_','t','c','p',5,'l','o','c','a','l',0};
    const uint8_t record[]={0,0,0,120,0,7,4,'t','e','s','t',0xc0,0x0c};
    const unsigned udpLength=8+12+sizeof(name)+4+(answer?sizeof(record):0);
    auto packet=awdlPacket(14+40+udpLength);
    auto eth=packet->bytes.data(),ip=eth+14,udp=ip+40,dns=udp+8;
    copyAddress(eth,group);copyAddress(eth+6,self);putBE16(eth+12,0x86dd);
    ip[0]=0x60;putBE16(ip+4,udpLength);ip[6]=17;ip[7]=255;
    ip[8]=0xfe;ip[9]=0x80;ip[23]=1;ip[24]=0xff;ip[25]=2;ip[39]=0xfb;
    putBE16(udp,5353);putBE16(udp+2,5353);putBE16(udp+4,udpLength);
    putBE16(udp+6,answer?0xc77a:0x7280);
    putBE16(dns+2,answer?0x8400:0);putBE16(dns+(answer?6:4),1);
    auto rr=dns+12;memcpy(rr,name,sizeof(name));rr+=sizeof(name);
    putBE16(rr,12);putBE16(rr+2,1);
    if(answer)memcpy(rr+4,record,sizeof(record));
    return packet;
}
static void bonjourBootstrap() {
    const uint8_t destination[]={2,0,0,0,0,2};
    enum PeerCase { Absent, WrongChannel, StaleSchedule };
    for(auto peerCase:{Absent,WrongChannel,StaleSchedule}) {
        AirportItlwmAWDLInterface i;
        if(peerCase==Absent) {i.tick();clockUS+=5000;}
        else {
            dataPeer(i,destination);
            auto peer=i.device.awdlPeers.find(destination,clockUS);assert(peer);
            if(peerCase==WrongChannel) {
                for(auto &channel:peer->sequence.channel)channel=6;
            } else clockUS+=3*16*65536; // Still in the peer table, past the schedule freshness limit.
            assert(!i.sync.canSendTo(*peer,clockUS,true));
        }
        auto packet=bonjourPacket(i.address.octet,peerCase!=WrongChannel);
        const auto original=packet->bytes;
        i.network.output.push_back(packet);assert(i.pullOutputPackets()==kIOReturnSuccess);
        assert(i.network.output.empty() && i.pendingCount==2);
        i.tick();
        assert(i.dataGroupCompleted==1 && i.pendingCount==1 && i.device.dataFrames.size()==1);
        DataBatch decoded={};const auto &frame=i.device.dataFrames.front();
        assert(parseDataMPDU(frame.data(),frame.size(),decoded)==ParseResult::Valid && decoded.count==1);
        std::vector<uint8_t> restored(original.size());
        assert(ethernetFromData(decoded.frames[0],restored.data(),restored.size())==original.size());
        assert(restored==original && !i.txDropped && !i.inFlightCount && allocations==1);
        i.drain();
    }
    AirportItlwmAWDLInterface i;i.tick();
    for(auto &channel:i.sync.channels)channel=149;
    const auto epoch=i.sync.epochUS;
    auto packet=bonjourPacket(i.address.octet,true);i.network.output.push_back(packet);
    assert(i.pullOutputPackets()==kIOReturnSuccess);
    auto held=[&]() {
        i.tick();assert(i.pendingCount==2 && i.pending[i.pendingHead].packet==packet &&
                        i.device.dataFrames.empty() && !i.txDropped);
    };
    clockUS=epoch+2999;held(); // Beginning and end guard in the local social EAW.
    clockUS=epoch+65536-2999;held();
    clockUS=epoch+65536+5000;held(); // Valid channel and grant, non-social slot.
    clockUS=epoch+8*65536+5000;
    i.device.deferChannel=true;i.device.awdlRadio.dwell.cancelling=true;held();
    i.device.awdlRadio.dwell={};i.device.deferChannel=false;
    i.device.allow149=false;held();i.device.allow149=true;
    i.sync.configureSyncState(1,0,destination);held(); // No clock for this fixed parent.
    i.sync.configureSyncState(0,0,i.address.octet);
    i.nextActionUS=i.nextMIFUS=clockUS+1000000;
    clockUS=epoch+8*65536+60000;allocationDelayUS=3000;held();allocationDelayUS=0;
    clockUS=epoch+10*65536+3000;i.tick();
    assert(i.dataGroupCompleted==1 && i.pendingCount==1 && allocations==1);
    i.drain();
    const auto dropped=i.txDropped;

    auto unicast=bonjourPacket(i.address.octet,false);unicast->bytes[0]^=1;
    i.network.output.push_back(unicast);
    auto other=bonjourPacket(i.address.octet,false);
    putBE16(other->bytes.data()+14+40+2,5354);putBE16(other->bytes.data()+14+40+6,0x727f);
    i.network.output.push_back(other);
    auto invalid=bonjourPacket(i.address.octet,false);invalid->bytes[14]=0x50;
    i.network.output.push_back(invalid);
    assert(i.pullOutputPackets()==kIOReturnSuccess);i.tick();
    assert(i.pendingCount==2 && i.device.dataFrames.size()==2 && i.txDropped==dropped+2);
    clockUS=epoch+24*65536+5000;i.tick();
    assert(i.pendingCount==1 && i.device.dataFrames.size()==3 && i.txDropped==dropped+2);
    i.drain();
}

static void dataResourceRetries() {
    const uint8_t peer[]={2,0,0,0,0,2},group[]={0x33,0x33,0,0,0,0xfb};
    for(bool multicast:{false,true}) for(int error:{0,ENOBUFS,ENOMEM,ENETDOWN}) {
        AirportItlwmAWDLInterface i;dataPeer(i,peer);
        if(multicast) {clockUS=i.sync.epochUS+8*65536+5000;i.tick();}
        auto packet=ethernet(i.address.octet,multicast?group:peer);
        packet->bytes[21]=0xff; // IPv6 hop limit must survive a retry.
        const auto original=packet->bytes;
        i.enqueue(packet);const auto queued=clockUS;const auto sequence=i.sync.dataSequence;
        if(error)i.device.transmitError=error;else failedAllocations=1;
        i.tick();
        assert(!i.device.data && i.pendingCount==1 && !i.txDropped && allocations==1);
        assert(i.pending[i.pendingHead].packet==packet && i.pending[i.pendingHead].queuedUS==queued);
        assert(i.sync.dataSequence==sequence && !i.network.signals);
        i.device.transmitError=0;clockUS+=5000;i.tick();
        assert(i.device.data==1 && !i.pendingCount && !i.txDropped && !allocations);
        DataBatch decoded={};const auto &frame=i.device.dataFrames.back();
        assert(parseDataMPDU(frame.data(),frame.size(),decoded)==ParseResult::Valid && decoded.count==1);
        uint8_t restored[54];assert(ethernetFromData(decoded.frames[0],restored,sizeof(restored))==54);
        assert(!memcmp(restored,original.data(),54) && decoded.frames[0].sequence==sequence);
        assert(i.network.signals==1);
    }
    AirportItlwmAWDLInterface i;dataPeer(i,peer);
    i.enqueue(ethernet(i.address.octet,peer));i.device.transmitError=ENOMEM;i.tick();
    assert(i.pendingCount==1 && !i.txDropped);
    // Retries retain the original deadline, including while no resources return.
    clockUS+=1114113;i.device.transmitError=0;i.tick();
    assert(!i.pendingCount && i.txDropped==1 && !i.device.data && !allocations);
    // A non-transient framing error must not circulate in the pending queue.
    AirportItlwmAWDLInterface invalid;dataPeer(invalid,peer);
    invalid.enqueue(ethernet(invalid.address.octet,peer));invalid.device.transmitError=EINVAL;
    invalid.tick();assert(!invalid.pendingCount && invalid.txDropped==1 && !allocations);
}
static void pending() {
    AirportItlwmAWDLInterface i;i.tick();
    for(auto &c:i.sync.channels)c=44;
    i.tick();
    const uint8_t dest[]={2,0,0,0,0,2};
    i.enqueue(ethernet(i.address.octet,dest));
    clockUS+=5000;i.tick();assert(i.pendingCount==1 && !i.txDropped); // no peer schedule
    clockUS+=1114113;i.tick();assert(!i.pendingCount && i.txDropped==1 && i.dataTxExpired==1 && !allocations);
    auto bad=ethernet(i.address.octet,dest);bad->bytes[6]^=4;i.enqueue(bad);i.tick();
    assert(!i.pendingCount && i.txDropped==2 && i.dataTxExpired==1);
    // Match a peer, then exhaust the firmware queue: retain until it has room.
    SyncEngine sender;sender.start(dest,44,clockUS);
    uint8_t bytes[512];Advertisement a={};
    auto size=sender.action(bytes,sizeof(bytes),clockUS,true,nullptr,0);
    assert(parseMPDU(bytes,size,a)==ParseResult::Valid);
    i.device.awdlPeers.update(dest,i.address.octet,a,clockUS,44,-40);
    i.enqueue(ethernet(i.address.octet,dest));clockUS+=5000;
    i.device.com.txq[1].queued=48;i.tick();assert(i.pendingCount==1 && !i.device.data);
    i.device.com.txq[1].queued=0;i.tick();assert(!i.pendingCount && i.device.data==1 && !allocations);
    // Wrapped pending indices drain exactly once when the radio is lost.
    for(unsigned n=0;n<64;++n)i.enqueue(ethernet(i.address.octet,dest));
    i.requested=false;i.device.stopAWDLRadio();i.tick();
    assert(!i.pendingCount && i.txDropped==66 && i.dataTxExpired==1 && !allocations);
    // The allocation recheck is the second pending-TTL expiration site.
    AirportItlwmAWDLInterface delayed;dataPeer(delayed,dest);
    delayed.nextActionUS=delayed.nextMIFUS=clockUS+10000000;
    delayed.enqueue(ethernet(delayed.address.octet,dest));
    delayed.pending[delayed.pendingHead].queuedUS=clockUS-delayed.DataLifetimeUS+1;
    allocationDelayUS=2;delayed.tick();allocationDelayUS=0;
    assert(delayed.dataTxExpired==1 && delayed.txDropped==1 && !delayed.pendingCount && !allocations);
}
static void elapsedDuringTX() {
    const uint8_t dest[]={2,0,0,0,0,2};
    for(unsigned delay:{5000,6000}) {
        AirportItlwmAWDLInterface i;i.tick();
        for(auto &c:i.sync.channels)c=44;
        i.tick();
        SyncEngine sender;sender.start(dest,44,clockUS);
        uint8_t bytes[512];Advertisement a={};
        auto size=sender.action(bytes,sizeof(bytes),clockUS,true,nullptr,0);
        assert(parseMPDU(bytes,size,a)==ParseResult::Valid);
        assert(i.device.awdlPeers.update(dest,i.address.octet,a,clockUS,44,-40));
        i.nextActionUS=i.nextMIFUS=clockUS+1000000;
        i.enqueue(ethernet(i.address.octet,dest));i.enqueue(ethernet(i.address.octet,dest));
        clockUS+=60000;
        i.device.onTransmit=[&] {clockUS+=delay;};
        i.tick();
        // The second packet sees either the peer edge guard or the ended local dwell.
        assert(i.device.data==1 && i.pendingCount==1 && !i.txDropped);
        i.drain();assert(!allocations);
    }
    AirportItlwmAWDLInterface i;i.tick();
    for(auto &c:i.sync.channels)c=44;
    const unsigned before=i.device.mif;
    i.nextMIFUS=0;
    allocationDelayUS=500;i.tick(); // Action timestamp includes time spent allocating.
    assert(i.device.mif==before+1);
    i.nextMIFUS=0;
    allocationDelayUS=65536;i.tick(); // Allocation crosses the entire firmware dwell.
    assert(i.device.mif==before+1 && !allocations);
    allocationDelayUS=0;
}
static void configuration() {
    AirportItlwmAWDLInterface i;
    i.sync.actionPeriodTU=220;i.sync.nonElectionMasterPeriodTU=128;
    i.sync.electionID=0x12345678;i.sync.metric=321;
    i.sync.setMasterChannel(6);i.sync.electionRSSI=-60;
    i.sync.wifiSequence=100;i.sync.dataSequence=200;
    const uint8_t sequence[]={44,0,6,149,44,0,6,149,44,0,6,149,44,0,6,149};
    memcpy(i.sync.channels,sequence,sizeof(sequence));i.sequenceConfigured=true;
    for(unsigned restart=0;restart<3;++restart) {
        i.requested=true;i.tick();
        assert(i.running && i.sync.actionPeriodTU==220 && i.sync.nonElectionMasterPeriodTU==128);
        assert(i.sync.electionID==0x12345678 && i.sync.metric==321);
        assert(i.sync.channel==6 && i.sync.configuredMasterChannel==6 && i.sync.electionRSSI==-60);
        assert(!memcmp(i.sync.channels,sequence,sizeof(sequence)));
        assert(i.sync.wifiSequence==102+2*restart && i.sync.dataSequence==200);
        assert(i.device.mif==restart+1);
        i.tick();assert(i.nextActionUS==clockUS+220*1024);
        assert(i.sync.wifiSequence==102+2*restart);
        i.requested=false;i.device.stopAWDLRadio();i.tick();
        assert(!i.running && i.network.up && i.nativeUp);
        clockUS+=1000000;
    }
    // A backwards clock resets only the timing/election epoch, not native configuration.
    i.sync.elect(i.device.awdlPeers,100);
    assert(i.sync.epochUS==100 && i.sync.metric==321 && i.sync.electionID==0x12345678);
    assert(i.sync.channel==6 && i.sync.electionRSSI==-60);
    assert(i.sync.actionPeriodTU==220 && !memcmp(i.sync.channels,sequence,sizeof(sequence)));
    assert(i.sync.wifiSequence==106 && i.sync.dataSequence==200);
}
static void actionRetries() {
    // Begin with an explicitly absent sequence so failures apply to the first action.
    const uint8_t dest[]={2,0,0,0,0,2};
    for(bool allocationFailure:{false,true}) {
        AirportItlwmAWDLInterface i;i.sequenceConfigured=true;i.tick();
        for(auto &c:i.sync.channels)c=44;
        SyncEngine sender;sender.start(dest,44,clockUS);
        uint8_t bytes[512];Advertisement a={};
        auto size=sender.action(bytes,sizeof(bytes),clockUS,true,nullptr,0);
        assert(parseMPDU(bytes,size,a)==ParseResult::Valid);
        assert(i.device.awdlPeers.update(dest,i.address.octet,a,clockUS,44,-40));
        i.enqueue(ethernet(i.address.octet,dest));
        clockUS+=5000;
        if(allocationFailure)failedAllocations=1;else i.device.transmitError=55;
        i.tick();
        assert(!i.nextMIFUS && !i.nextActionUS && i.pendingCount==1);
        assert(!i.device.mif && !i.device.psf && !i.device.data && !i.txDropped);
        assert(i.device.transmitAttempts==(allocationFailure?0:1) && allocations==1);
        // Retry on the next tick, without waiting 64/110 TU for an unsent frame.
        i.device.transmitError=0;clockUS+=5000;i.tick();
        assert(i.device.transmissions==std::vector<char>({'M','P','D'}));
        assert(i.nextMIFUS==clockUS+64*1024 && i.nextActionUS==clockUS+110*1024);
        assert(!i.pendingCount && !allocations);
        // A failed PSF must not reset the successful MIF or admit more data.
        i.enqueue(ethernet(i.address.octet,dest));
        i.nextActionUS=0;i.device.transmitError=55;
        const auto mifDeadline=i.nextMIFUS;i.tick();
        assert(i.device.mif==1 && i.device.psf==1 && i.device.data==1);
        assert(!i.nextActionUS && i.nextMIFUS==mifDeadline && i.pendingCount==1);
        i.device.transmitError=0;clockUS+=5000;i.tick();
        assert(i.device.transmissions==std::vector<char>({'M','P','D','P','D'}));
        assert(!allocations && !i.pendingCount);
    }
    AirportItlwmAWDLInterface i;i.sequenceConfigured=true;i.tick();
    for(auto &c:i.sync.channels)c=44;
    // Losing the dwell during allocation must leave both announcements due.
    allocationDelayUS=65536;i.tick();allocationDelayUS=0;
    assert(!i.nextActionUS && !i.nextMIFUS && !i.device.transmitAttempts && !allocations);
    i.tick();assert(i.device.mif==1 && i.device.psf==1);
    AirportItlwmAWDLInterface partial;partial.sequenceConfigured=true;partial.tick();
    for(auto &c:partial.sync.channels)c=44;
    partial.device.onTransmit=[&] {partial.device.transmitError=55;};
    partial.tick();
    assert(partial.device.mif==1 && !partial.device.psf && !partial.nextActionUS);
    assert(partial.nextMIFUS==clockUS+64*1024 && partial.device.transmitAttempts==2);
    partial.device.onTransmit={};partial.device.transmitError=0;
    clockUS+=5000;partial.tick();
    assert(partial.device.transmissions==std::vector<char>({'M','P'}));
    assert(partial.device.transmitAttempts==3 && partial.nextActionUS==clockUS+110*1024);
}
static void outputFlow() {
    const uint8_t dest[]={2,0,0,0,0,2};
    AirportItlwmAWDLInterface i;i.tick();
    for(auto &c:i.sync.channels)c=44;
    i.network.onDequeue=[&] {assert(i.device.loop.inGate());};
    for(unsigned n=0;n<96;++n)i.network.output.push_back(ethernet(i.address.octet,dest));
    assert(i.pullOutputPackets()==kIOReturnSuccess && i.pendingCount==32);
    assert(i.pullOutputPackets()==kIOReturnNoResources && i.pendingCount==64);
    assert(i.network.output.size()==32 && !i.txDropped && allocations==96);
    assert(i.pullOutputPackets()==kIOReturnNoResources && i.network.dequeues==2);
    // Without a peer window, keep the 64 packets and leave the system queue alone.
    clockUS+=5000;i.tick();assert(i.pendingCount==64 && !i.network.signals && !i.device.data);
    SyncEngine sender;sender.start(dest,44,clockUS);
    uint8_t bytes[512];Advertisement a={};
    auto size=sender.action(bytes,sizeof(bytes),clockUS,true,nullptr,0);
    assert(parseMPDU(bytes,size,a)==ParseResult::Valid);
    assert(i.device.awdlPeers.update(dest,i.address.octet,a,clockUS,44,-40));
    i.device.com.txq[1].queued=48;i.tick();
    assert(i.pendingCount==64 && !i.network.signals && !i.device.data);
    i.device.com.txq[1].queued=0;
    clockUS+=5000;i.tick();
    assert(i.device.data==64 && !i.pendingCount && i.network.signals==1 && !i.txDropped);
    assert(i.pullOutputPackets()==kIOReturnSuccess && i.pendingCount==32 && i.network.output.empty());
    i.tick();assert(i.device.data==96 && i.network.signals==2 && !allocations);
    assert(i.pullOutputPackets()==kIOReturnSuccess && !i.pendingCount); // empty/rate-limited
    // A partial slot allowance must not pull a complete batch.
    for(unsigned n=0;n<63;++n)i.enqueue(ethernet(i.address.octet,dest));
    for(unsigned n=0;n<3;++n)i.network.output.push_back(ethernet(i.address.octet,dest));
    assert(i.pullOutputPackets()==kIOReturnNoResources && i.network.lastLimit==1);
    assert(i.pendingCount==64 && i.network.output.size()==2 && !i.txDropped);
    // Expiry also frees capacity and wakes a producer waiting for queue space.
    clockUS+=1114113;i.tick();
    assert(!i.pendingCount && i.txDropped==64 && i.network.signals==3);
    assert(i.pullOutputPackets()==kIOReturnSuccess && i.pendingCount==2);
    i.drain();assert(!allocations);
    // Radio loss drains only owned packets, keeping untouched output in the OS queue.
    i.network.output.push_back(ethernet(i.address.octet,dest));
    i.enqueue(ethernet(i.address.octet,dest));
    i.requested=false;i.device.stopAWDLRadio();i.tick();
    unsigned calls=i.network.dequeues;
    assert(i.pullOutputPackets()==kIOReturnNoResources && i.network.dequeues==calls);
    assert(i.network.output.size()==1 && i.network.signals==3 && !i.pendingCount);
    i.requested=true;i.tick();assert(i.network.starts==2);
    assert(i.pullOutputPackets()==kIOReturnSuccess && i.pendingCount==1);
    i.drain();assert(!allocations);
    // Detached/stopping/not-running interfaces must not consume new packets.
    i.network.output.push_back(ethernet(i.address.octet,dest));calls=i.network.dequeues;
    i.network.attached=false;assert(i.pullOutputPackets()==kIOReturnNoResources);
    i.network.attached=true;i.stopping=true;assert(i.pullOutputPackets()==kIOReturnNoResources);
    i.stopping=false;i.running=false;assert(i.pullOutputPackets()==kIOReturnNoResources);
    i.running=true;i.device.awdlRadio.ready=false;assert(i.pullOutputPackets()==kIOReturnNoResources);
    i.device.awdlRadio.ready=true;
    assert(i.network.dequeues==calls && i.network.output.size()==1);
    i.running=true;i.network.dequeueResult=99;
    assert(i.pullOutputPackets()==99 && !i.pendingCount && i.network.output.size()==1);
    i.network.dequeueResult=0;
    i.network.output.push_back(awdlPacket(1515));
    auto drops=i.txDropped;
    assert(i.pullOutputPackets()==kIOReturnSuccess && i.pendingCount==1 && i.txDropped==drops+1);
    i.drain();assert(!allocations);
}
static void cacheLifecycle() {
    AirportItlwmAWDLInterface i;i.tick();
    const uint8_t dest[]={2,0,0,0,0,2};Advertisement a={};
    assert(i.device.awdlPeers.update(dest,i.address.octet,a,clockUS,44,-40));
    auto peer=i.device.awdlPeers.find(dest,clockUS);peer->nativeCached=true;
    i.enqueue(ethernet(i.address.octet,dest));
    assert(i.setEnabled(false)==kIOReturnSuccess); // synchronization pause during reconfiguration
    assert(!i.requested && !i.running && i.nativeUp && i.network.up && !i.pendingCount);
    assert(peer->nativeCached && i.device.gate.entries==1 && !allocations);
    assert(i.setEnabled(true)==kIOReturnSuccess && i.requested && i.scheduler.delay==1);
    i.tick();assert(i.running && peer->nativeCached);
    i.enableError=99;assert(i.setInterfaceEnable(false)==99);
    assert(i.running && i.requested && peer->nativeCached);
    i.enableError=0;assert(i.setInterfaceEnable(false)==kIOReturnSuccess);
    assert(!i.running && !i.requested && !peer->nativeCached && !i.nativeUp && !i.network.up);
    assert(i.device.awdlPeers.find(dest,clockUS)==peer);
    assert(i.setInterfaceEnable(true)==kIOReturnSuccess);i.tick();
    assert(i.running && !peer->nativeCached); // only a new registration can reserve it
    peer->nativeCached=true;i.stopping=true;
    assert(i.setInterfaceEnable(false)==kIOReturnNotReady && peer->nativeCached);
    assert(i.setEnabled(false)==kIOReturnNotReady && peer->nativeCached);
}

static void configureDuringLinkUp() {
    AirportItlwmAWDLInterface i;
    // Native configuration may pause synchronization while programming election state.
    i.onLinkUp=[&] { assert(i.network.up);assert(!i.setEnabled(false)); };
    i.tick();
    assert(i.nativeUp && i.network.up && i.nativeTransitions==1);
    assert(!i.running && !i.requested && !i.device.awdlRadio.ready && !i.network.starts);
    assert(!i.device.psf && !i.device.mif);
    i.onLinkUp=nullptr;
    assert(!i.setEnabled(true));i.tick();
    assert(i.running && i.nativeUp && i.nativeTransitions==1 && i.network.starts==1);
    assert(!i.setEnabled(false));i.tick();
    assert(i.nativeUp && i.network.up && i.nativeTransitions==1);
    assert(!i.setInterfaceEnable(false));
    assert(!i.nativeUp && !i.network.up && !i.interfaceEnabled && i.nativeTransitions==2);
}
static void associationLossRecovery() {
    AirportItlwmAWDLInterface i;
    i.tick();assert(i.running && i.nativeUp && i.network.up);
    // Sequoia disables synchronization synchronously when the AWDL link goes down.
    i.onLinkDown=[&] { assert(!i.setEnabled(false)); };
    i.enqueue(ethernet(i.address.octet,i.address.octet));
    i.device.startError=ENETDOWN;i.device.stopAWDLRadio();
    clockUS+=1000000;i.tick();
    assert(!i.running && !i.device.awdlRadio.ready && !i.network.up);
    assert(i.nativeUp && i.requested && !i.pendingCount && !allocations);
    const auto sent=i.device.transmissions.size();
    clockUS+=1000000;i.tick();
    assert(i.device.transmissions.size()==sent && !i.device.awdlRadio.ready);
    i.device.startError=0;clockUS+=1000000;i.tick();
    assert(i.running && i.requested && i.nativeUp && i.network.up);
    assert(i.device.awdlRadio.ready && i.nativeTransitions==1 && i.network.starts==2);
    assert(!i.setInterfaceEnable(false));

    // A user/native sync pause during a disconnected interval cancels auto-resume.
    AirportItlwmAWDLInterface paused;paused.tick();
    paused.device.startError=ENETDOWN;paused.device.stopAWDLRadio();
    clockUS+=1000000;paused.tick();assert(!paused.setEnabled(false));
    auto starts=paused.device.starts;
    paused.device.startError=0;clockUS+=1000000;paused.tick();
    assert(!paused.requested && !paused.running && paused.device.starts==starts);
    assert(!paused.setInterfaceEnable(false));

    // A queued AP state change takes effect before the old RUN binding is torn down.
    AirportItlwmAWDLInterface switching;switching.tick();
    switching.onLinkDown=[&] { assert(!switching.setEnabled(false)); };
    switching.device.stopAWDLRadio();switching.device.com.ns_nstate=0;
    starts=switching.device.starts;clockUS+=1000000;switching.tick();
    assert(switching.requested && switching.nativeUp && !switching.network.up);
    assert(!switching.running && !switching.device.awdlRadio.ready && switching.device.starts==starts);
    switching.device.com.ns_nstate=IEEE80211_S_RUN;clockUS+=1000000;switching.tick();
    assert(switching.running && switching.device.starts==starts+1 && switching.network.up);
    assert(!switching.setInterfaceEnable(false));

    // An AP transition queued while firmware setup sleeps must stop that setup.
    for(bool resumed:{false,true}) {
        AirportItlwmAWDLInterface sleeping;
        if(resumed) {sleeping.tick();sleeping.device.stopAWDLRadio();}
        sleeping.onLinkDown=[&] { assert(!sleeping.setEnabled(false)); };
        sleeping.device.onStart=[&] {sleeping.device.com.ns_nstate=0;};
        clockUS+=1000000;sleeping.tick();
        assert(sleeping.requested && !sleeping.running && !sleeping.device.awdlRadio.ready);
        assert(!sleeping.network.up && sleeping.nativeUp==resumed);
        const auto sent=sleeping.device.transmissions.size();
        starts=sleeping.device.starts;clockUS+=1000000;sleeping.tick();
        assert(sleeping.device.starts==starts && sleeping.device.transmissions.size()==sent);
        sleeping.device.onStart=nullptr;sleeping.device.com.ns_nstate=IEEE80211_S_RUN;
        clockUS+=1000000;sleeping.tick();
        assert(sleeping.running && sleeping.network.up && sleeping.nativeUp);
        assert(sleeping.device.starts==starts+1);
        assert(!sleeping.setInterfaceEnable(false));
    }

    // A full power-off or hardware fault after a transient pause closes the native link.
    for(unsigned cause=0;cause<6;++cause) {
        AirportItlwmAWDLInterface stopped;stopped.tick();
        stopped.onLinkDown=[&] { assert(!stopped.setEnabled(false)); };
        stopped.device.startError=ENETDOWN;stopped.device.stopAWDLRadio();
        clockUS+=1000000;stopped.tick();assert(stopped.requested && stopped.nativeUp);
        if(cause==0)stopped.device.com.sc_ic.ic_ac.ac_if.if_flags &= ~IFF_UP;
        if(cause==1)stopped.device.com.sc_ic.ic_ac.ac_if.if_flags &= ~IFF_RUNNING;
        if(cause==2)stopped.device.com.sc_flags|=IWX_FLAG_SHUTDOWN;
        if(cause==3)stopped.device.com.sc_flags|=IWX_FLAG_HW_ERR;
        if(cause==4)stopped.device.com.sc_flags|=IWX_FLAG_RFKILL;
        if(cause==5)stopped.device.awdlRadio.fault=true;
        stopped.device.startError=0;starts=stopped.device.starts;
        clockUS+=1000000;stopped.tick();
        assert(!stopped.requested && !stopped.running && !stopped.nativeUp && !stopped.network.up);
        assert(stopped.device.starts==starts && !stopped.device.awdlRadio.ready);
    }
}
static void availability() {
    static_assert(sizeof(apple80211_availability_window_start)==36, "start size");
    static_assert(sizeof(apple80211_availability_window_extensions_end)==40, "end size");
    AirportItlwmAWDLInterface i;
    auto &r=i.device.awdlRadio;auto &messages=i.controller.messages;
    i.updateAvailability();assert(messages.empty());
    i.sequenceConfigured=true; // Explicitly absent until the simulated firmware START.
    i.tick();assert(messages.empty());
    for(auto &channel:i.sync.channels)channel=44;
    r.ready=true;r.channel=44;r.dwell.windowEndUS=clockUS+65536;r.dwellStarts=1;
    r.dwell.requested=true;
    i.updateAvailability();assert(messages.empty()); // command submitted, no START
    r.dwell.running=true;i.updateAvailability();
    assert(messages.size()==1 && messages[0].type==41 && i.nativeWindowOpen);
    const auto &bytes=messages[0].bytes;
    assert(le32(bytes.data())==0 && le32(bytes.data()+4)==1);
    assert(sameAddress(bytes.data()+8,i.sync.sync) && sameAddress(bytes.data()+14,i.sync.master));
    assert(be32(bytes.data()+20)==uint32_t(clockUS));
    assert(le32(bytes.data()+24)==1 && le32(bytes.data()+28)==44 && le32(bytes.data()+32)==18);
    clockUS+=17000;i.updateAvailability();assert(messages.size()==1); // same EAW, next AW
    r.dwell.running=false;i.updateAvailability();assert(messages.size()==1 && i.nativeWindowOpen);
    r.dwell.running=true;++r.dwellStarts;i.updateAvailability();assert(messages.size()==1);
    i.controller.failures=1;r.ready=false;i.updateAvailability();
    assert(i.nativeWindowOpen && i.nativeWindowClosing && i.nativeWindowErrors==1 && messages.size()==1);
    const uint32_t ended=uint32_t(clockUS);
    clockUS+=1000;r.ready=true;++r.dwellStarts;
    i.updateAvailability();assert(messages.size()==2 && messages[1].type==43 && !i.nativeWindowOpen);
    assert(be32(messages[1].bytes.data()+4)==ended); // preserve first END time on retry
    for(unsigned n=0;n<40;++n)if(n<4 || n>=8)assert(messages[1].bytes[n]==0);
    clockUS=r.dwell.windowEndUS;i.updateAvailability();assert(messages.size()==2 && !i.nativeWindowOpen);
    i.controller.failures=1;
    r.dwell.windowEndUS=clockUS+65536;++r.dwellStarts;i.updateAvailability();
    assert(!i.nativeWindowOpen && messages.size()==2);
    clockUS+=1000;i.updateAvailability();assert(i.nativeWindowOpen && messages.size()==3);
    assert(le32(messages.back().bytes.data())==4);
    assert(i.setEnabled(false)==kIOReturnSuccess && !i.nativeWindowOpen && messages.back().type==43);
    assert(i.nativeWindowStarts==2 && i.nativeWindowEnds==2 && i.nativeWindowErrors==2);
    // Wrong channel, expired parent and cancellation cannot open a window.
    clockUS+=65536;
    i.requested=true;r.ready=true;r.dwell.running=true;r.dwell.windowEndUS=clockUS+1000;
    r.channel=6;i.updateAvailability();assert(messages.size()==4);
    Window w={};assert(i.sync.window(clockUS,w));r.dwell.windowEndUS=clockUS+w.untilNextEAWUS;
    r.channel=6;i.updateAvailability();assert(messages.size()==4);
    r.channel=44;r.dwell.cancelling=true;i.updateAvailability();assert(messages.size()==4);
    r.dwell.cancelling=false;r.dwell.failed=true;i.updateAvailability();assert(messages.size()==4);
    r.dwell.failed=false;i.sync.syncMode=1;i.sync.fixedParent[5]=2;
    i.updateAvailability();assert(messages.size()==4);
    i.sync.syncMode=0;i.updateAvailability();assert(messages.size()==5);
    i.stopping=true;i.updateAvailability();assert(messages.size()==6 && !i.nativeWindowOpen);
    // Both timestamp and AW counter wrap; 2.4 GHz uses the matching flags.
    AirportItlwmAWDLInterface wrap;
    clockUS=0x100000100ULL;wrap.sync.start(wrap.address.octet,6,clockUS);
    wrap.sync.epochCounter=65532;
    auto &q=wrap.device.awdlRadio;q.ready=q.dwell.running=true;q.channel=6;q.dwellStarts=1;
    q.dwell.windowEndUS=clockUS+65536;wrap.updateAvailability();
    assert(le32(wrap.controller.messages[0].bytes.data())==65532);
    assert(be32(wrap.controller.messages[0].bytes.data()+20)==256);
    assert(le32(wrap.controller.messages[0].bytes.data()+32)==10);
    clockUS+=65536;++q.dwellStarts;q.dwell.windowEndUS=clockUS+65536;wrap.updateAvailability();
    assert(wrap.controller.messages.size()==3 && le32(wrap.controller.messages[2].bytes.data())==0);
}
static std::vector<uint8_t> umiPayload() {
    std::vector<uint8_t> p(55);p[0]=24;putLE16(p.data()+1,40);
    p[3]=2;p[8]=1;p[9]=2;p[14]=1;
    p[43]=21;p[44]=2;p[46]=0x93;p[47]=2;
    p[48]=0x23;p[49]=4;p[51]=0x41;p[52]=0x42;p[53]=0x43;p[54]=0x44;
    return p;
}
static std::vector<uint8_t> nativeUmiPayload() {
    // 24H32 buildMasterIndicationTemplate(UMI=true): Election24 + DataPath12.
    // Both UMI exits precede buildAWDLVersionTlv; DataPath12 has 47 value bytes.
    auto p=umiPayload();p.resize(43+50);
    std::fill(p.begin()+43,p.end(),0);
    p[43]=12;putLE16(p.data()+44,47);
    putLE16(p.data()+46,0x9b03);p[48]='C';p[49]='N';
    p[53]=2;p[58]=3;putLE16(p.data()+59,44);
    p[61]=2;p[66]=1;putLE16(p.data()+67,4);
    putLE32(p.data()+69,0x4000);
    return p;
}
static void setupUnicast(AirportItlwmAWDLInterface &i) {
    i.sequenceConfigured=true;for(auto &c:i.sync.channels)c=44;i.tick();
    for(unsigned n=2;n<20;++n) {
        uint8_t dest[]={2,0,0,0,0,uint8_t(n)};
        SyncEngine sender;sender.start(dest,44,clockUS);
        uint8_t bytes[512];Advertisement a={};
        auto size=sender.action(bytes,sizeof(bytes),clockUS,true,nullptr,0);
        assert(parseMPDU(bytes,size,a)==ParseResult::Valid);
        assert(i.device.awdlPeers.update(dest,i.address.octet,a,clockUS,44,-40));
        i.device.awdlPeers.find(dest,clockUS)->nativeCached=true;
    }
    clockUS+=5000;i.tick();assert(i.nativeWindowOpen);
    i.controller.messages.clear();
}
static apple80211_awdl_oob_request umiRequest(const std::vector<uint8_t> &p,unsigned last=2,unsigned flags=6) {
    apple80211_awdl_oob_request r={};r.destination[0]=2;r.destination[5]=last;
    copyAddress(r.bssid,BSSID);r.flags=flags;r.data_len=p.size();r.data=p.data();return r;
}
static void lostRelayElection() {
    const uint8_t relay[]={2,0,0,0,0,2},root[]={2,0,0,0,0,3};
    const uint64_t epoch=clockUS,cycle=16*65536;
    AirportItlwmAWDLInterface i;
    i.sequenceConfigured=true;
    i.sync.channels[0]=i.sync.channels[2]=i.sync.channels[8]=i.sync.channels[10]=6;
    i.tick();
    SyncEngine relayTX,rootTX;
    relayTX.start(relay,6,epoch);rootTX.start(root,6,epoch);
    copyAddress(relayTX.master,root);copyAddress(relayTX.sync,root);relayTX.distance=1;
    uint8_t profile[48]={24,40,0};
    auto select=[&](const uint8_t *parent,unsigned hops) {
        copyAddress(profile+3,root);copyAddress(profile+9,parent);
        putLE32(profile+19,hops);putLE32(profile+23,rootTX.masterMetric);putLE32(profile+27,100);
        profile[43]=21;profile[44]=2;profile[46]=0x93;profile[47]=2;
        assert(i.sync.configureTemplate(profile,sizeof(profile)));
        i.sync.configureSyncState(1,0,parent);
    };
    auto receive=[&](SyncEngine &source) {
        uint8_t bytes[512];
        const auto length=source.action(bytes,sizeof(bytes),clockUS,false,nullptr,0);
        Advertisement a={};assert(parseMPDU(bytes,length,a)==ParseResult::Valid);
        assert(i.device.awdlPeers.update(source.address,i.address.octet,a,clockUS,6,-40));
    };
    select(relay,2);
    clockUS=epoch+2*65536+8192;receive(rootTX);i.tick();
    assert(!i.sync.transmitReady(clockUS) && !i.nativeWindowOpen); // Root alone at cold start.
    receive(relayTX);i.tick();
    assert(i.sync.transmitReady(clockUS) && i.nativeWindowOpen);
    const auto tx=i.device.awdlRadio.submitted;
    clockUS=epoch+4*cycle+8192;receive(rootTX);
    i.network.output.push_back(bonjourPacket(i.address.octet,true));assert(!i.pullOutputPackets());
    auto &r=i.device.awdlRadio;
    i.device.deferChannel=true;r.dwell.cancelling=true;i.tick();
    assert(!i.sync.transmitReady(clockUS) && !i.nativeWindowOpen && i.pendingCount==2);
    r.dwell={};i.device.deferChannel=false;
    i.device.serviceAWDLChannel(6,clockUS+30000);
    auto rootPeer=i.device.awdlPeers.find(root,clockUS);
    auto relayPeer=i.device.awdlPeers.find(relay,clockUS);
    assert(rootPeer && relayPeer);
    const Peer observed=*rootPeer;
    auto held=[&]() {
        i.updateAvailability();
        assert(!i.nativeWindowOpen && !i.sync.transmitReady(clockUS) && i.device.awdlRadio.submitted==tx);
    };
    rootPeer->metadata.electionID++;held();*rootPeer=observed;
    rootPeer->metadata.distance=1;held();*rootPeer=observed;
    copyAddress(rootPeer->metadata.electionSync,relay);held();*rootPeer=observed;
    rootPeer->electionV2ReceivedUS-=3000000;held();*rootPeer=observed;
    rootPeer->sequenceReceivedUS-=3000000;held();*rootPeer=observed;
    rootPeer->sync.windowCounter+=4;held();*rootPeer=observed;
    rootPeer->sync.nextWindowTU-=4;held();*rootPeer=observed;
    rootPeer->sequence.channel[0]=44;held();*rootPeer=observed;
    const auto lastRelay=relayPeer->lastSeenUS;
    relayPeer->lastSeenUS=clockUS;held();relayPeer->lastSeenUS=lastRelay;
    i.sync.metric++;held();i.sync.metric--;
    const auto locked=i.sync;
    i.sync.configureSyncState(2,0,relay);i.sync.configureSyncState(1,0,relay);held();i.sync=locked;
    select(root,1);select(relay,2);held();i.sync=locked;
    profile[31]=1;assert(i.sync.configureTemplate(profile,sizeof(profile)));
    profile[31]=0;assert(i.sync.configureTemplate(profile,sizeof(profile)));held();i.sync=locked;
    i.sync.restart(6,epoch);held();i.sync=locked;
    r.changing=true;held();r.changing=false;
    r.stopPending=true;held();r.stopPending=false;
    r.dwell.running=false;held();r.dwell.running=true;
    const auto grantEnd=r.dwell.windowEndUS;r.dwell.windowEndUS=clockUS;held();r.dwell.windowEndUS=grantEnd;
    r.channel=44;held();r.channel=6;
    i.regulatoryPlanBlocked=true;held();i.regulatoryPlanBlocked=false;
    i.requested=false;held();assert(!i.pendingCount);i.requested=true;
    i.network.output.push_back(bonjourPacket(i.address.octet,true));
    i.network.output.push_back(ethernet(i.address.octet,root));assert(!i.pullOutputPackets());
    assert(i.psfDue(clockUS) && i.broadcastDue(clockUS));
    i.tick();
    assert(i.nativeWindowOpen && i.controller.messages.back().type==41);
    const auto &event=i.controller.messages.back().bytes;
    assert(le32(event.data())==256 && sameAddress(event.data()+8,relay) && sameAddress(event.data()+14,root));
    assert(!i.sync.transmitReady(clockUS) && i.device.awdlRadio.submitted==tx && i.pendingCount==3);
    const auto payload=umiPayload();const auto request=umiRequest(payload,3);
    rootPeer->nativeCached=true;
    assert(i.queueUnicast(request)==kIOReturnNotReady);
    i.nextMIFUS=clockUS+100000;i.tick(); // Only PSF due.
    i.nextActionUS=clockUS+100000;i.tick(); // Neither announcement is blocking data.
    assert(i.device.awdlRadio.submitted==tx && !i.unicastSubmitted && !i.dataSubmitted && i.pendingCount==3);
    i.controller.failures=1;
    clockUS=epoch+4*cycle+65536+8192;i.updateAvailability();
    assert(i.nativeWindowClosing);i.tick();assert(!i.nativeWindowOpen && i.controller.messages.back().type==43);
    // The real manager evaluates its election at END of slot zero. Supply the
    // corresponding host reconfiguration and a current AF from the selected root.
    select(root,1);clockUS=epoch+4*cycle+8*65536+8192;receive(rootTX);i.tick();
    assert(i.sync.transmitReady(clockUS) && i.nativeWindowOpen && i.pendingCount==1 &&
           i.dataGroupCompleted==1 && i.dataUnicastAcknowledged==1 && i.device.awdlRadio.submitted>tx && allocations==1);
    i.drain();
    assert(i.queueUnicast(request)==kIOReturnSuccess);

    // Recheck the candidate START if choosing a parent changed while posting END.
    clockUS=epoch+4*cycle+10*65536+8192;
    assert(!i.device.serviceAWDLChannel(6,clockUS+30000));
    i.controller.onEnd=[&]{select(relay,2);};
    i.updateAvailability();
    assert(!i.nativeWindowOpen && i.controller.messages.back().type==43);
    select(root,1);receive(rootTX);i.controller.onEnd={};i.tick();assert(i.nativeWindowOpen);
    clockUS=epoch+5*cycle+8192;
    assert(!i.device.serviceAWDLChannel(6,clockUS+500));
    i.controller.onEnd=[&]{clockUS+=501;};
    i.updateAvailability();assert(!i.nativeWindowOpen && i.controller.messages.back().type==43);
}
static void lostDirectParentElection() {
    const uint8_t parent[]={2,0,0,0,0,2};
    const uint64_t epoch=clockUS,cycle=16*65536;
    AirportItlwmAWDLInterface i;
    i.sequenceConfigured=true;
    i.sync.setMasterChannel(6);
    for(auto &channel:i.sync.channels)channel=6;
    i.tick();
    SyncEngine sender;sender.start(parent,6,epoch);
    uint8_t profile[48]={24,40,0};
    copyAddress(profile+3,parent);copyAddress(profile+9,parent);
    putLE32(profile+19,1);putLE32(profile+23,sender.masterMetric);putLE32(profile+27,100);
    profile[43]=21;profile[44]=2;profile[46]=0x93;profile[47]=2;
    assert(i.sync.configureTemplate(profile,sizeof(profile)));
    i.sync.configureSyncState(1,0,parent);
    clockUS=epoch+8192;i.tick();
    assert(!i.sync.transmitReady(clockUS) && !i.nativeWindowOpen);
    clockUS=epoch+2*65536+8192;
    uint8_t frame[512];Advertisement advert={};
    auto size=sender.action(frame,sizeof(frame),clockUS,false,nullptr,0);
    assert(parseMPDU(frame,size,advert)==ParseResult::Valid);
    assert(i.device.awdlPeers.update(parent,i.address.octet,advert,clockUS,6,-40));
    i.tick();assert(i.sync.transmitReady(clockUS) && i.nativeWindowOpen);
    const auto submitted=i.device.awdlRadio.submitted;
    clockUS=epoch+4*cycle+8192;
    i.sync.elect(i.device.awdlPeers,clockUS);
    auto &radio=i.device.awdlRadio;
    radio.dwell.running=false;i.updateAvailability();assert(!i.nativeWindowOpen);
    assert(!i.device.serviceAWDLChannel(6,clockUS+40000));
    const auto locked=i.sync;
    auto peer=i.device.awdlPeers.find(parent,clockUS);assert(peer);
    const Peer original=*peer;
    auto held=[&] {
        i.updateAvailability();
        assert(!i.nativeWindowOpen && !i.sync.transmitReady(clockUS));
        assert(i.device.awdlRadio.submitted==submitted);
    };
    peer->lastSeenUS=clockUS;held();*peer=original;
    i.sync.configureSyncState(2,0,parent);held();i.sync=locked;
    i.sync.configureSyncState(1,0x20,parent);held();i.sync=locked;
    i.sync.configureSyncState(0,0,parent);held();i.sync=locked;
    i.sync.restart(6,epoch);held();i.sync=locked;
    profile[31]=1;assert(i.sync.configureTemplate(profile,sizeof(profile)));
    profile[31]=0;assert(i.sync.configureTemplate(profile,sizeof(profile)));held();i.sync=locked;
    i.sync.metric++;held();i.sync=locked;
    i.sync.nativeSyncUS=clockUS;held();i.sync=locked;
    i.sync.nativeSequenceUS=clockUS;held();i.sync=locked;
    i.sync.nativeSyncUS=clockUS+1;held();i.sync=locked;
    radio.changing=true;held();radio.changing=false;
    radio.stopPending=true;held();radio.stopPending=false;
    radio.dwell.running=false;held();radio.dwell.running=true;
    radio.dwell.cancelling=true;held();radio.dwell.cancelling=false;
    radio.dwell.failed=true;held();radio.dwell.failed=false;
    const auto end=radio.dwell.windowEndUS;
    radio.dwell.windowEndUS=clockUS;held();radio.dwell.windowEndUS=end;
    radio.channel=44;held();radio.channel=6;
    i.regulatoryPlanBlocked=true;held();i.regulatoryPlanBlocked=false;
    i.requested=false;held();i.requested=true;
    i.stopping=true;held();i.stopping=false;
    i.updateAvailability();assert(i.nativeWindowOpen);
    assert(!i.sync.transmitReady(clockUS) && !sender.nativeElection);
    assert(!i.sync.action(frame,sizeof(frame),clockUS,false,nullptr,0));
    assert(i.device.awdlRadio.submitted==submitted);
    const auto starts=i.nativeWindowStarts;
    i.updateAvailability();assert(i.nativeWindowStarts==starts);
    clockUS=epoch+4*cycle+65536+8192;
    i.controller.failures=1;i.updateAvailability();assert(i.nativeWindowClosing);
    i.updateAvailability();assert(!i.nativeWindowOpen && !i.nativeWindowClosing);
    assert(i.nativeWindowStarts==starts && !i.sync.transmitReady(clockUS));
    i.drain();
}
static void availabilityClockCorrections() {
    AirportItlwmAWDLInterface i;setupUnicast(i);
    const uint8_t destination[]={2,0,0,0,0,2};
    auto peer=i.device.awdlPeers.find(destination,clockUS);assert(peer);
    i.sync.syncMode=2;copyAddress(i.sync.fixedParent,destination);
    i.sync.elect(i.device.awdlPeers,clockUS);
    const auto starts=i.nativeWindowStarts,ends=i.nativeWindowEnds,serial=i.nativeWindowSerial;
    uint8_t record[44]={};putLE16(record+2,64);putLE16(record+6,110);
    putLE16(record+8,16);putLE16(record+10,16);putLE16(record+12,16);record[16]=4;
    uint32_t received=OSSwapHostToBigInt32(uint32_t(peer->syncReceivedUS));memcpy(record+20,&received,4);
    copyAddress(record+32,peer->sync.syncMaster);copyAddress(record+38,destination);
    auto payload=umiPayload();auto request=umiRequest(payload);
    for(unsigned correction:{250,0,750,100}) {
        putLE16(record+4,correction);
        assert(i.sync.nativeParent.configure(record,i.device.awdlPeers,clockUS)==AdvertiserResult::Applied);
        i.sync.elect(i.device.awdlPeers,clockUS);i.updateAvailability();
        assert(i.nativeWindowOpen && !i.nativeWindowClosing);
        assert(i.nativeWindowStarts==starts && i.nativeWindowEnds==ends && i.nativeWindowSerial==serial);
        assert(i.queueUnicast(request)==kIOReturnSuccess);
        const auto token=i.unicast[0].token;i.serviceUnicast();
        assert(i.unicast[0].state==UnicastRequest::Submitted);
        i.completeUnicast(token,true);i.serviceUnicast();
        assert(i.unicast[0].state==UnicastRequest::Free);
    }
    assert(i.unicastAcknowledged==4 && !i.unicastFailed && !allocations);
    // Only the logical EAW closes the native window; a short TX deadline does not.
    i.device.awdlRadio.dwell.windowEndUS=clockUS+1000;
    clockUS+=1000;i.updateAvailability();assert(i.nativeWindowOpen);
    Window next={};assert(i.sync.window(clockUS,next));clockUS+=next.untilNextEAWUS;i.updateAvailability();
    assert(!i.nativeWindowOpen && i.nativeWindowEnds==ends+1);
}
static void availabilityOverlappingWindows() {
    AirportItlwmAWDLInterface i;i.sync.start(i.address.octet,44,clockUS);
    auto &r=i.device.awdlRadio;r.ready=r.dwell.running=true;r.channel=44;r.dwellStarts=1;
    r.dwell.windowEndUS=clockUS+65536;i.updateAvailability();
    auto &messages=i.controller.messages;assert(messages.size()==1 && i.nativeWindowOpen);
    // A clock correction moves into another EAW while firmware remains on this channel.
    clockUS+=60000;i.sync.epochUS-=8000;i.controller.failures=1;i.updateAvailability();
    assert(i.nativeWindowClosing && messages.size()==1);
    i.updateAvailability();assert(messages.size()==3 && messages[1].type==43 && messages[2].type==41);
    assert(le32(messages[2].bytes.data())==4 && r.dwellStarts==1);
    i.sync.channels[1]=6;i.updateAvailability();
    assert(!i.nativeWindowOpen && messages.size()==4 && messages.back().type==43);
    i.sync.channels[1]=44;i.updateAvailability();assert(!i.nativeWindowOpen && messages.size()==4);
    clockUS=r.dwell.windowEndUS;i.updateAvailability();
    assert(!i.nativeWindowOpen && messages.size()==4);
    // A short confirmed dwell is usable even when it ends before the software EAW.
    Window next={};assert(i.sync.window(clockUS,next));clockUS+=next.untilNextEAWUS;
    ++r.dwellStarts;r.dwell.windowEndUS=clockUS+1000;i.updateAvailability();
    assert(i.nativeWindowOpen && messages.size()==5);
    clockUS+=1000;i.updateAvailability();assert(i.nativeWindowOpen && messages.size()==5);
    r.dwell.running=false;i.updateAvailability();assert(i.nativeWindowOpen && messages.size()==5);
    assert(i.sync.window(clockUS,next));clockUS+=next.untilNextEAWUS;i.updateAvailability();
    assert(!i.nativeWindowOpen && messages.size()==6);
}
static void unicastOwnership() {
    static_assert(sizeof(apple80211_awdl_oob_request)==56,"native OOB size");
    static_assert(offsetof(apple80211_awdl_oob_request,flags)==20,"OOB flags");
    static_assert(offsetof(apple80211_awdl_oob_request,bssid)==22,"OOB BSSID");
    static_assert(offsetof(apple80211_awdl_oob_request,data_len)==40,"OOB size field");
    static_assert(offsetof(apple80211_awdl_oob_request,data)==48,"OOB pointer");
    AirportItlwmAWDLInterface i;setupUnicast(i);auto p=nativeUmiPayload(),saved=p;
    auto r=umiRequest(p);assert(i.queueUnicast(r)==kIOReturnSuccess);
    const uint64_t token=i.unicast[0].token;assert(token && i.controller.messages.empty());
    assert(i.queueUnicast(r)==kIOReturnBusy);
    std::fill(p.begin(),p.end(),0xaa);i.tick();
    assert(i.device.tokens==std::vector<uint64_t>{token} && i.unicastSubmitted==1);
    assert(i.controller.messages.empty() && !i.unicastAcknowledged);
    assert(i.device.transmitDeadlines.back()==i.unicast[0].deadlineUS);
    assert(i.device.lastUnicast.size()==184+saved.size());
    assert(!memcmp(i.device.lastUnicast.data()+184,saved.data(),saved.size()));
    assert(sameAddress(i.device.lastUnicast.data()+4,r.destination));
    unsigned types[256]={};
    for(size_t at=40;at<i.device.lastUnicast.size();at+=3+le16(i.device.lastUnicast.data()+at+1))
        ++types[i.device.lastUnicast[at]];
    for(unsigned type:{4,5,18,24,12})assert(types[type]==1);
    assert(!types[21]); // Do not invent a version TLV absent from the native UMI.
    i.completeUnicast(token+1,true);assert(!i.unicastAcknowledged);
    i.completeUnicast(token,true);assert(i.unicastAcknowledged==1 && i.controller.messages.empty());
    i.controller.failures=1;i.serviceUnicast();assert(i.unicast[0].state==UnicastRequest::Complete);
    i.completeUnicast(token,false);assert(!i.unicastFailed); // duplicate completion ignored
    i.serviceUnicast();assert(i.unicast[0].state==UnicastRequest::Free && i.unicastNotifyErrors==1);
    assert(i.controller.messages.size()==1 && i.controller.messages[0].type==69);
    assert(i.controller.messages[0].bytes[6]==0);
    assert(sameAddress(i.controller.messages[0].bytes.data(),r.destination));
    p=saved;r=umiRequest(p);
    auto broadcast=r;memset(broadcast.destination,0xff,6);
    assert(i.configureBroadcast(broadcast,true)==kIOReturnBadArgument);
    assert(i.configureBroadcast(broadcast,false)==kIOReturnBadArgument);
    assert(!i.broadcast.configured && !i.immediateBroadcast.size);
    for(unsigned variant=0;variant<6;++variant) {
        auto invalid=saved;
        switch(variant) {
        case 0:invalid[1]=39;break;
        case 1:invalid[44]=48;break;
        case 2:invalid.insert(invalid.end(),saved.begin(),saved.begin()+43);break;
        case 3:invalid.insert(invalid.end(),{21,1,0,0x93});break;
        case 4:invalid.insert(invalid.end(),{21,2,0,0x93,2,21,2,0,0x93,2});break;
        case 5:invalid.insert(invalid.end(),{4,0,0});break;
        }
        assert(i.queueUnicast(umiRequest(invalid))==kIOReturnBadArgument);
        for(const auto &entry:i.unicast)assert(entry.state==UnicastRequest::Free);
    }
    for(unsigned n=0;n<9;++n) {
        auto bad=r;int expected=kIOReturnBadArgument;
        switch(n) {
        case 0:bad.version=1;break;
        case 1:bad.destination[0]=1;break;
        case 2:bad.destination[5]=1;break;
        case 3:bad.bssid[0]=9;break;
        case 4:bad.data=nullptr;break;
        case 5:bad.data_len=1201;break;
        case 6:bad.flags=7;expected=kIOReturnUnsupported;break;
        case 7:bad.destination[5]=99;expected=kIOReturnNotReady;break;
        case 8:bad.data_len=2;break;
        }
        assert(i.queueUnicast(bad)==expected);
    }
    for(unsigned n=2;n<18;++n)assert(i.queueUnicast(umiRequest(p,n,n&1?14:6))==kIOReturnSuccess);
    assert(i.queueUnicast(umiRequest(p,18))==kIOReturnNoResources);
    i.requested=false;i.serviceUnicast();assert(i.unicastFailed==16);
    for(const auto &entry:i.unicast)assert(entry.state==UnicastRequest::Free);
}
static void unicastLifecycle() {
    for(unsigned scenario=0;scenario<8;++scenario) {
        AirportItlwmAWDLInterface i;setupUnicast(i);auto p=nativeUmiPayload();auto r=umiRequest(p);
        assert(!i.queueUnicast(r));uint64_t token=i.unicast[0].token;
        if(scenario==0) {
            i.device.com.txq[1].queued=48;i.tick();assert(i.device.tokens.empty());
            clockUS=i.unicast[0].deadlineUS;i.serviceUnicast();
        } else if(scenario==1) {
            failedAllocations=1;i.serviceUnicast();assert(i.device.tokens.empty());
            i.device.transmitError=1;i.serviceUnicast();i.serviceUnicast();
        } else if(scenario==2) {
            i.tick();i.device.stopAWDLRadio();i.serviceUnicast();
        } else if(scenario==3) {
            i.tick();clockUS+=1000000;i.serviceUnicast();
        } else if(scenario==4) {
            i.tick();assert(!i.setEnabled(false));
        } else if(scenario==5) {
            i.device.awdlPeers.find(r.destination,clockUS)->nativeCached=false;i.serviceUnicast();
        } else if(scenario==6) {
            allocationDelayUS=70000;i.serviceUnicast();allocationDelayUS=0;
            assert(i.device.tokens.empty());i.serviceUnicast();
        } else {
            i.tick();i.completeUnicast(token,false);i.serviceUnicast();
        }
        assert(!i.unicastAcknowledged && i.unicastFailed==1);
        bool reported=false;for(const auto &m:i.controller.messages)if(m.type==69) {
            assert(m.bytes[6]==0xda);reported=true;
        }
        assert(reported && i.unicast[0].state==UnicastRequest::Free);
        i.completeUnicast(token,true);assert(!i.unicastAcknowledged && i.unicastFailed==1);
    }
    AirportItlwmAWDLInterface i;setupUnicast(i);auto p=umiPayload();auto r=umiRequest(p);
    assert(!i.queueUnicast(r));auto old=i.unicast[0].token;i.tick();i.completeUnicast(old,false);i.serviceUnicast();
    assert(!i.queueUnicast(r));auto fresh=i.unicast[0].token;assert(fresh!=old);i.tick();
    i.completeUnicast(old,true);assert(!i.unicastAcknowledged);
    i.completeUnicast(fresh,true);assert(i.unicastAcknowledged==1);
}
static apple80211_awdl_oob_request broadcastRequest(const std::vector<uint8_t> &p,
                                                   uint32_t map=0xffffffff,unsigned offset=10000) {
    auto r=umiRequest(p);memset(r.destination,0xff,6);r.tx_channel_map=map;r.tx_offset_us=offset;return r;
}
static void broadcastClockCorrections() {
    AirportItlwmAWDLInterface i;setupUnicast(i);
    const uint8_t destination[]={2,0,0,0,0,2};
    auto peer=i.device.awdlPeers.find(destination,clockUS);assert(peer);
    i.sync.syncMode=2;copyAddress(i.sync.fixedParent,destination);
    auto payload=umiPayload();auto request=broadcastRequest(payload,0xffffffff,0);
    assert(i.configureBroadcast(request,true)==kIOReturnSuccess);i.tick();
    assert(i.broadcastSubmitted==1);
    const auto physicalEnd=i.device.awdlRadio.dwell.windowEndUS,serial=i.device.awdlRadio.dwellStarts;
    const auto starts=i.nativeWindowStarts,ends=i.nativeWindowEnds;
    uint8_t record[44]={};putLE16(record+2,64);putLE16(record+6,110);
    putLE16(record+8,16);putLE16(record+10,16);putLE16(record+12,16);record[16]=4;
    uint32_t received=OSSwapHostToBigInt32(uint32_t(peer->syncReceivedUS));memcpy(record+20,&received,4);
    copyAddress(record+32,peer->sync.syncMaster);copyAddress(record+38,destination);
    unsigned packets=0;
    for(unsigned correction:{250,0,750,100}) {
        putLE16(record+4,correction);
        assert(i.sync.nativeParent.configure(record,i.device.awdlPeers,clockUS)==AdvertiserResult::Applied);
        i.enqueue(ethernet(i.address.octet,destination));clockUS+=500;i.tick();
        assert(i.broadcastSubmitted==1 && i.device.data==++packets && !i.pendingCount);
        assert(i.device.awdlRadio.dwell.windowEndUS==physicalEnd && i.device.awdlRadio.dwellStarts==serial);
        assert(i.nativeWindowStarts==starts && i.nativeWindowEnds==ends);
    }
    // A logical EAW change, including counter wrap, still needs a new announcement.
    unsigned submitted=1;
    for(unsigned counter:{4,65532,0}) {
        putLE16(record,counter);
        assert(i.sync.nativeParent.configure(record,i.device.awdlPeers,clockUS)==AdvertiserResult::Applied);
        i.tick();assert(i.broadcastSubmitted==++submitted);
        i.tick();assert(i.broadcastSubmitted==submitted);
    }
    // A short grant reentry does not repeat the same logical EAW's MIF.
    i.device.awdlRadio.dwell.running=false;i.tick();
    assert(i.device.awdlRadio.dwellStarts==serial+1 && i.broadcastSubmitted==submitted);
    i.tick();assert(i.broadcastSubmitted==submitted && !allocations);
}
static void broadcastSchedule() {
    AirportItlwmAWDLInterface i;i.sequenceConfigured=true;for(auto &c:i.sync.channels)c=44;
    auto p=umiPayload();p[48]=16;auto saved=p; // Opaque service payload distinct from PSF.
    auto r=broadcastRequest(p,(1U<<1)|(1U<<15));
    assert(!i.configureBroadcast(r,true));
    std::fill(p.begin(),p.end(),0xaa);
    i.tick();const auto epoch=i.sync.epochUS;
    assert(i.device.psf==1 && !i.device.mif);
    clockUS=epoch+65536+9999;i.tick();assert(!i.device.mif);
    clockUS=epoch+65536+10000;i.tick();assert(i.device.mif==1);
    const auto &frame=i.device.broadcastFrames.back();
    assert(frame[30]==3 && frame.size()==184+saved.size());
    assert(!memcmp(frame.data()+184,saved.data(),saved.size()));
    clockUS+=5000;i.tick();assert(i.device.mif==1 && !i.broadcastDue(clockUS));
    for(unsigned index:{2u,14u,15u,16u,17u}) {
        clockUS=epoch+uint64_t(index)*65536+10000;i.tick();
        const unsigned count=index<15?1:(index<17?2:3);
        assert(i.device.mif==count);
    }
    // Bitmap indexes wrap with the 16-entry sequence and the 16-bit AW counter.
    i.sync.epochUS=clockUS;i.sync.epochCounter=65532;i.broadcast.sent=false;
    clockUS+=10000;i.tick();assert(i.device.mif==4);
    clockUS=i.sync.epochUS+65536+10000;i.tick();assert(i.device.mif==4);
    clockUS=i.sync.epochUS+2*65536+10000;i.tick();assert(i.device.mif==5);
    for(const auto &f:i.device.broadcastFrames)if(f[30]==0) {
        for(size_t at=40;at<f.size();at+=3+le16(f.data()+at+1))assert(f[at]!=16);
    }
    assert(i.broadcastSubmitted==5 && !allocations);
}
static void broadcastLifecycle() {
    AirportItlwmAWDLInterface i;setupUnicast(i);auto p=umiPayload();
    auto periodic=broadcastRequest(p,0xffffffff,10000);
    assert(!i.configureBroadcast(periodic,true));
    auto epoch=i.sync.epochUS;clockUS=epoch+10000;
    const auto before=i.device.mif;
    failedAllocations=1;i.tick();assert(i.device.mif==before && i.broadcastDue(clockUS));
    i.device.transmitError=5;i.tick();assert(i.device.mif==before && i.broadcastDue(clockUS));
    i.device.transmitError=0;i.tick();assert(i.device.mif==before+1 && !i.broadcastDue(clockUS));
    // Immediate replacement does not overwrite the scheduled copy or wait for its map.
    auto once=p;once.back()=0x99;auto r=broadcastRequest(once);
    assert(!i.configureBroadcast(r,false));i.tick();
    assert(i.device.mif==before+2 && !i.immediateBroadcast.size);
    assert(i.device.broadcastFrames.back().back()==0x99 && i.broadcast.payload[p.size()-1]==p.back());
    // A disabled periodic map must not hold up native unicast or IPv6 traffic.
    periodic.tx_channel_map=0;assert(!i.configureBroadcast(periodic,true));
    i.nextMIFUS=0;assert(!i.broadcastDue(clockUS));
    assert(!i.queueUnicast(umiRequest(p)));i.tick();assert(i.unicastSubmitted==1);
    i.completeUnicast(i.unicast[0].token,true);i.serviceUnicast();
    const uint8_t destination[]={2,0,0,0,0,2};
    i.enqueue(ethernet(i.address.octet,destination));i.tick();
    assert(i.device.data==1 && !i.pendingCount);
    assert(!i.configureBroadcast(r,false));
    for(auto &c:i.sync.channels)c=0;
    clockUS+=1000000;i.tick();assert(!i.immediateBroadcast.size);
    for(auto &c:i.sync.channels)c=44;
    assert(!i.configureBroadcast(r,false));
    assert(!i.setEnabled(false) && !i.immediateBroadcast.size);
    assert(i.broadcast.configured && !i.broadcast.channelMap);
    assert(!i.setEnabled(true));i.tick();assert(i.device.mif==before+2);
    // Allocation delay may cross into an unselected slot; retry there must not transmit.
    periodic.tx_channel_map=1;periodic.tx_offset_us=0;
    assert(!i.configureBroadcast(periodic,true));
    i.sync.epochUS=clockUS;i.sync.epochCounter=0;i.broadcast.sent=false;
    allocationDelayUS=65536;i.tick();allocationDelayUS=0;
    assert(i.device.mif==before+2 && !i.broadcast.sent && !allocations);
}
static void psfSuppression() {
    AirportItlwmAWDLInterface i;setupUnicast(i);auto p=umiPayload();
    i.sequenceConfigured=true; // Retain the configured channel-44 sequence across restart.
    const unsigned originalPSF=i.device.psf,originalMIF=i.device.mif;
    i.afTxMode=3;i.nextActionUS=0;
    auto periodic=broadcastRequest(p,0,0);assert(!i.configureBroadcast(periodic,true));
    const uint8_t destination[]={2,0,0,0,0,2};
    assert(!i.queueUnicast(umiRequest(p)));
    i.enqueue(ethernet(i.address.octet,destination));
    i.tick();
    assert(i.device.psf==originalPSF && i.device.mif==originalMIF);
    assert(i.unicastSubmitted==1 && i.device.data==1 && !i.pendingCount);
    assert(i.running && i.nativeWindowOpen && i.device.awdlRadio.ready);
    i.completeUnicast(i.unicast[0].token,true);i.serviceUnicast();
    // Both kinds of native MIF continue while periodic PSF is suppressed.
    periodic.tx_channel_map=0xffffffff;
    assert(!i.configureBroadcast(periodic,true));i.tick();
    assert(i.device.mif==originalMIF+1 && i.device.psf==originalPSF);
    assert(!i.configureBroadcast(periodic,false));i.tick();
    assert(i.device.mif==originalMIF+2 && i.device.psf==originalPSF);
    clockUS+=65536;i.tick();
    assert(i.device.mif==originalMIF+3 && i.device.psf==originalPSF);
    // Sync disable/enable retains native policy, but still restores the radio and MIF.
    assert(!i.setEnabled(false) && !i.setEnabled(true));i.tick();
    assert(i.afTxMode==3 && i.running && i.device.mif==originalMIF+4 && i.device.psf==originalPSF);
    periodic.tx_channel_map=0;assert(!i.configureBroadcast(periodic,true));
    i.afTxMode=0;i.nextActionUS=0;
    failedAllocations=1;i.tick();assert(i.device.psf==originalPSF && i.psfDue(clockUS));
    i.device.transmitError=5;i.tick();assert(i.device.psf==originalPSF && i.psfDue(clockUS));
    i.device.transmitError=0;i.tick();assert(i.device.psf==originalPSF+1 && !i.psfDue(clockUS));
    i.tick();assert(i.device.psf==originalPSF+1);
    // A policy change while channel setup releases the gate applies to this tick.
    i.nextActionUS=0;i.device.onService=[&] {i.afTxMode=3;};i.tick();
    assert(i.device.psf==originalPSF+1 && i.running && !allocations);
}
static void nonblockingStop() {
    AirportItlwmAWDLInterface i;i.tick();
    auto &r=i.device.awdlRadio;i.device.deferStop=true;
    const unsigned starts=i.device.starts,tx=i.device.transmitAttempts;
    assert(i.setEnabled(false)==kIOReturnSuccess);
    assert(!i.requested && !i.running && r.stopPending && !r.ready && !r.fault);
    assert(i.scheduler.delay==1);
    i.tick();i.tick();
    assert(r.stopPending && i.device.starts==starts && i.device.transmitAttempts==tx && i.scheduler.delay==5);
    r.dwell.done=1;i.tick();
    assert(!r.stopPending && !i.requested && !i.running && i.device.starts==starts);
    assert(i.scheduler.delay==100);
    assert(i.setEnabled(true)==kIOReturnSuccess);i.tick();
    assert(i.device.starts==starts+1 && i.running && !r.fault);

    AirportItlwmAWDLInterface resumed;resumed.tick();resumed.device.deferStop=true;
    auto &rr=resumed.device.awdlRadio;
    assert(resumed.setEnabled(false)==kIOReturnSuccess);
    assert(resumed.setEnabled(true)==kIOReturnSuccess);
    resumed.tick();assert(rr.stopPending && resumed.device.starts==1 && !resumed.running);
    rr.dwell.done=1;resumed.tick();
    assert(!rr.stopPending && resumed.device.starts==2 && resumed.running);

    AirportItlwmAWDLInterface failed;failed.tick();failed.device.stopError=EIO;
    assert(failed.setEnabled(false)==kIOReturnIOError);
    failed.setEnabled(true);const unsigned stops=failed.device.stops;
    for(unsigned n=0;n<5;++n){clockUS+=1000000;failed.tick();}
    assert(failed.device.stops==stops && failed.device.starts==1 && failed.device.awdlRadio.fault);

    AirportItlwmAWDLInterface setupFault;setupFault.tick();setupFault.device.deferStop=true;
    assert(setupFault.setEnabled(false)==kIOReturnSuccess);
    setupFault.device.awdlRadio.fault=true;setupFault.setEnabled(true);
    setupFault.device.awdlRadio.dwell.done=1;setupFault.tick();
    assert(!setupFault.device.awdlRadio.stopPending && setupFault.device.awdlRadio.fault);
    assert(setupFault.device.starts==1 && !setupFault.running);

    AirportItlwmAWDLInterface service;service.sequenceConfigured=true;
    for(auto &channel:service.sync.channels)channel=44;
    service.tick();service.device.deferChannel=true;
    auto &sr=service.device.awdlRadio;
    sr.dwell.running=false;sr.dwell.cancelling=sr.dwell.cancelAcknowledged=true;
    const unsigned submitted=service.device.transmitAttempts;service.tick();
    assert(service.running && sr.ready && !sr.fault && service.device.transmitAttempts==submitted);
    sr.dwell.done=1;service.tick();
    assert(sr.dwell.running && !sr.dwell.cancelling && service.device.transmitAttempts==submitted);
    clockUS+=65536;service.tick();assert(service.device.transmitAttempts>submitted);

    AirportItlwmAWDLInterface removed;removed.tick();removed.device.deferStop=true;
    removed.setEnabled(false);removed.stopping=true;removed.device.awdlRadio.dwell.done=1;
    const unsigned before=removed.device.stops;removed.tick();
    assert(removed.device.stops==before && removed.device.awdlRadio.stopPending);
}
static void logicalWindowResume() {
    AirportItlwmAWDLInterface i;i.sequenceConfigured=true;for(auto &c:i.sync.channels)c=44;i.tick();
    const uint64_t epoch=i.sync.epochUS;assert(i.nativeWindowStarts==1);
    clockUS+=5000;assert(!i.setEnabled(false));assert(i.nativeWindowEnds==1);
    assert(!i.setEnabled(true));i.tick();
    assert(i.sync.epochUS==epoch && !i.nativeWindowOpen && i.nativeWindowStarts==1);
    clockUS=epoch+65536;i.tick();
    assert(i.nativeWindowOpen && i.nativeWindowCounter==4 && i.nativeWindowStarts==2);
    clockUS+=3000;i.setEnabled(false);i.setEnabled(true);i.tick();
    assert(i.sync.epochUS==epoch && !i.nativeWindowOpen && i.nativeWindowStarts==2);
    clockUS=epoch+2*65536;i.tick();assert(i.nativeWindowCounter==8 && i.nativeWindowStarts==3);
    // A device restart with an intact host clock does not roll the AW counter back.
    i.device.awdlRadio.ready=false;i.tick();i.running=false;
    clockUS+=1000;i.tick();assert(i.sync.epochUS==epoch && i.nativeWindowCounter==8);
    // A backwards host clock cannot reuse a future epoch.
    i.setEnabled(false);clockUS=epoch-1000;i.setEnabled(true);i.tick();
    assert(i.sync.epochUS==clockUS && i.sync.epochCounter==0);
    assert(i.nativeWindowOpen && i.nativeWindowCounter==0);
    i.setEnabled(false);clockUS+=2000;i.sync.periodTU=32;i.setEnabled(true);i.tick();
    assert(i.sync.epochUS==clockUS && i.sync.epochCounter==0);
}
static void unicastAcrossGrantGap() {
    for(unsigned scenario=0;scenario<5;++scenario) {
        AirportItlwmAWDLInterface i;setupUnicast(i);
        auto payload=umiPayload(),saved=payload;auto request=umiRequest(payload);
        auto &r=i.device.awdlRadio;
        r.dwell.running=false;r.dwell.windowEndUS=clockUS;i.updateAvailability();
        assert(i.nativeWindowOpen && !i.queueUnicast(request));
        const auto token=i.unicast[0].token;
        std::fill(payload.begin(),payload.end(),0xaa);i.serviceUnicast();
        assert(i.device.tokens.empty() && i.unicast[0].state==UnicastRequest::Queued);
        if(scenario==0) {
            clockUS+=8000;r.dwell.running=true;r.dwell.windowEndUS=clockUS+13000;++r.dwellStarts;
            i.tick();assert(i.device.tokens==std::vector<uint64_t>{token});
            assert(!memcmp(i.device.lastUnicast.data()+184,saved.data(),saved.size()));
            assert(i.nativeWindowStarts==1 && !i.nativeWindowEnds);
            i.completeUnicast(token,true);i.serviceUnicast();assert(i.unicastAcknowledged==1);
        } else if(scenario==1) {
            clockUS=i.unicast[0].deadlineUS;i.updateAvailability();i.serviceUnicast();
        } else if(scenario==2) {
            // The parent advances to the next EAW before the stored deadline.
            i.sync.epochUS-=65536;r.dwell.running=true;r.dwell.windowEndUS=clockUS+13000;
            i.updateAvailability();i.serviceUnicast();
        } else if(scenario==3) {
            i.setEnabled(false);i.setEnabled(true);i.tick();
            payload=saved;request=umiRequest(payload);
            assert(!i.nativeWindowOpen && i.queueUnicast(request)==kIOReturnNotReady);
        } else {
            const auto deadline=i.unicast[0].deadlineUS;
            clockUS=deadline-20000;i.sync.epochUS-=15000;
            r.dwell.running=true;r.dwell.windowEndUS=clockUS+13384;
            i.nextActionUS=i.nextMIFUS=deadline+1000;
            allocationDelayUS=10000;i.serviceUnicast();allocationDelayUS=0;
            assert(clockUS<deadline && i.device.tokens.empty());
            i.serviceUnicast();
        }
        if(scenario) {
            assert(i.device.tokens.empty() && i.unicastFailed==1);
            i.serviceUnicast();assert(i.unicastFailed==1);
        }
        assert(i.unicast[0].state==UnicastRequest::Free);
    }
}

static void unicastRejectedBeforeSubmission() {
    for (const int error : {ENETDOWN, ENOBUFS, ENOMEM}) {
        AirportItlwmAWDLInterface i;
        setupUnicast(i);
        const auto payload = nativeUmiPayload();
        assert(!i.queueUnicast(umiRequest(payload)));
        auto &entry = i.unicast[0];
        const uint64_t token = entry.token, deadline = entry.deadlineUS;
        auto &r = i.device.awdlRadio;
        const uint64_t firstGrant = r.dwellStarts;
        r.dwell.windowEndUS = clockUS + 100;
        i.device.transmitError = error;
        i.device.onReject = [&](uint64_t rejected) {
            assert(rejected == token && !allocations);
            clockUS = r.dwell.windowEndUS;
            r.dwell.running = false;
        };
        i.serviceUnicast();
        assert(entry.state == UnicastRequest::Queued && entry.token == token && entry.deadlineUS == deadline);
        assert(!i.unicastSubmitted && !i.unicastFailed && i.controller.messages.empty());
        const unsigned attempts = i.device.transmitAttempts;
        i.serviceUnicast();
        assert(i.device.transmitAttempts == attempts);
        i.device.transmitError = 0;
        clockUS += 8000;
        assert(clockUS < deadline && !i.device.serviceAWDLChannel(44, clockUS + 13384));
        assert(r.dwellStarts == firstGrant + 1);
        i.device.com.txq[1].queued = 48;
        i.serviceUnicast();
        assert(i.device.transmitAttempts == attempts);
        i.device.com.txq[1].queued = 0;
        failedAllocations = 1;
        i.serviceUnicast();
        assert(!failedAllocations && i.device.transmitAttempts == attempts && !allocations);
        i.serviceUnicast();
        assert(i.device.tokens == std::vector<uint64_t>{token} && i.unicastSubmitted == 1);
        assert(!memcmp(i.device.lastUnicast.data() + 184, payload.data(), payload.size()));
        i.completeUnicast(token, true);
        i.serviceUnicast();
        assert(entry.state == UnicastRequest::Free && i.unicastAcknowledged == 1 && !i.unicastFailed);
        assert(i.controller.messages.size() == 1 && i.controller.messages[0].bytes[6] == 0);
    }

    // Purging a peer can complete and recycle this slot while the HAL is returning.
    AirportItlwmAWDLInterface i;
    setupUnicast(i);
    const auto payload = nativeUmiPayload();
    const auto request = umiRequest(payload);
    assert(!i.queueUnicast(request));
    const uint64_t first = i.unicast[0].token;
    i.device.transmitError = ENETDOWN;
    i.device.onReject = [&](uint64_t rejected) {
        assert(rejected == first);
        auto *peer = i.device.awdlPeers.find(request.destination, clockUS);
        peer->nativeCached = false;
        i.serviceUnicast();
        peer->nativeCached = true;
        assert(!i.queueUnicast(request));
        i.device.transmitError = 0;
        i.serviceUnicast();
    };
    i.serviceUnicast();
    auto &entry = i.unicast[0];
    assert(entry.token != first && entry.state == UnicastRequest::Submitted);
    assert(i.unicastFailed == 1 && i.unicastSubmitted == 1 && !allocations);
    i.completeUnicast(first, true);
    i.completeUnicast(entry.token, false);
    i.serviceUnicast();
    assert(entry.state == UnicastRequest::Free && i.unicastFailed == 2 && !i.unicastAcknowledged);
    assert(i.controller.messages.size() == 2 && i.device.tokens.size() == 1);

    for (const int error : {EIO, ENETDOWN}) {
        AirportItlwmAWDLInterface expired;
        setupUnicast(expired);
        assert(!expired.queueUnicast(request));
        expired.device.transmitError = error;
        if (error == ENETDOWN)
            expired.device.onReject = [&](uint64_t) {clockUS = expired.unicast[0].deadlineUS;};
        const unsigned attempts = expired.device.transmitAttempts;
        expired.serviceUnicast();
        expired.device.transmitError = 0;
        expired.serviceUnicast();
        expired.serviceUnicast();
        assert(expired.device.transmitAttempts == attempts + 1 && !expired.unicastSubmitted);
        assert(expired.unicastFailed == 1 && expired.unicast[0].state == UnicastRequest::Free);
        assert(expired.controller.messages.size() == 1 && expired.controller.messages[0].bytes[6] == 0xda);
    }
}
static void dataFlushRetry() {
    const uint8_t peer[]={2,0,0,0,0,2},group[]={0x33,0x33,0,0,0,0xfb};
    for(bool multicast:{false,true}) {
        AirportItlwmAWDLInterface i;dataPeer(i,peer);i.device.acknowledgeData=false;
        i.sync.dataSequence=65535;i.sync.wifiSequence=4095;
        i.enqueue(ethernet(i.address.octet,multicast?group:peer));i.tick();
        assert(i.inFlightCount==1 && !i.pendingCount && i.dataSubmitted==1 && !i.network.signals);
        const auto a=i.device.dataTokens.back();
        const auto first=i.device.dataFrames.back();
        i.completeData(a,ECANCELED);i.completeData(a,0);
        assert(i.pendingCount==1 && !i.inFlightCount && i.dataFlushed==1 && !i.txDropped);
        i.tick();assert(i.device.data==1); // old grant cannot retry a flushed frame
        ++i.device.awdlRadio.dwellStarts;clockUS+=1000;i.tick();
        assert(!i.pendingCount && i.inFlightCount==1 && i.dataRetried==1 && i.dataSubmitted==2);
        auto second=i.device.dataFrames.back();
        assert(second[1]==(multicast?0:8));second[1]=0;assert(first==second);
        const auto b=i.device.dataTokens.back();assert(a!=b);
        i.completeData(a,EIO);assert(i.inFlightCount==1);
        i.completeData(b,0);i.completeData(b,ECANCELED);
        assert(!allocations && !i.inFlightCount && !i.dataAbandoned && !i.txDropped);
        assert(i.dataGroupCompleted==unsigned(multicast) && i.dataUnicastAcknowledged==unsigned(!multicast));
        i.tick();assert(i.network.signals==1 && i.sync.dataSequence==0 && i.sync.wifiSequence==4096);
    }
    for(unsigned outcome=0;outcome<8;++outcome) {
        AirportItlwmAWDLInterface i;dataPeer(i,peer);i.device.acknowledgeData=false;
        i.enqueue(ethernet(i.address.octet,peer));const auto queued=clockUS;i.tick();
        const auto token=i.device.dataTokens.back();
        switch(outcome) {
        case 0: i.completeData(token,EIO);break;
        case 1: clockUS=queued+1114113;i.completeData(token,ECANCELED);break;
        case 2: ++i.device.dmaEpoch;i.completeData(token,ECANCELED);break;
        case 3: i.setEnabled(false);i.setEnabled(true);i.completeData(token,ECANCELED);break;
        case 4: i.completeData(token,ECANCELED);++i.device.dmaEpoch;i.tick();break;
        case 5: i.completeData(token,ECANCELED);clockUS=queued+1114113;i.tick();break;
        case 6: i.completeData(token,ECANCELED);i.device.awdlRadio.ready=false;i.updateAvailability();
                i.device.awdlRadio.ready=true;break;
        case 7:
            for(unsigned flush=1;flush<=AirportItlwmAWDLInterface::MaxDataFlushes;++flush) {
                i.completeData(i.device.dataTokens.back(),ECANCELED);
                ++i.device.awdlRadio.dwellStarts;i.tick();
                assert(i.dataRetried==flush);
            }
            i.completeData(i.device.dataTokens.back(),ECANCELED);
            break;
        }
        i.completeData(token,0);assert(!i.pendingCount && !i.inFlightCount && !allocations);
        assert(i.dataAbandoned==1 && i.txDropped==1 &&
            i.dataSubmitted==(outcome==7?AirportItlwmAWDLInterface::MaxDataFlushes+1u:1u));
    }
    // Only slot 0 overlaps. Missing its physical grant may require a full cycle.
    {
        AirportItlwmAWDLInterface i;dataPeer(i,peer);
        auto remote=i.device.awdlPeers.find(peer,clockUS);assert(remote);
        for(auto &c:remote->sequence.channel)c=0;
        remote->sequence.channel[0]=44;
        const auto queued=clockUS;
        i.enqueue(ethernet(i.address.octet,peer));i.device.com.txq[1].queued=48;i.tick();
        i.device.com.txq[1].queued=0;clockUS=queued+1000001;i.tick();
        assert(i.pendingCount==1 && !i.txDropped && !i.dataSubmitted);
        clockUS=queued+16*65536;i.tick();assert(i.dataSubmitted==1 && !allocations && !i.txDropped);
    }
    // Retained originals consume the same capacity as waiting packets. ACK frees a single slot.
    {
        AirportItlwmAWDLInterface i;dataPeer(i,peer);i.device.acknowledgeData=false;
        for(unsigned n=0;n<64;++n)i.enqueue(ethernet(i.address.octet,peer));
        i.network.output.push_back(ethernet(i.address.octet,peer));i.tick();
        assert(i.inFlightCount==64 && !i.pendingCount && !i.network.signals);
        assert(i.pullOutputPackets()==kIOReturnNoResources && i.network.output.size()==1);
        i.completeData(i.device.dataTokens[0],0);i.tick();assert(i.network.signals==1);
        assert(i.pullOutputPackets()==kIOReturnNoResources && !i.network.output.size());
        i.completeData(i.device.dataTokens[1],ECANCELED);
        assert(i.inFlightCount==62 && i.pendingCount==2);
        i.drain();assert(!allocations && !i.pendingCount && !i.inFlightCount);
        for(auto token:i.device.dataTokens)i.completeData(token,ECANCELED);
        assert(!i.pendingCount && !i.inFlightCount && i.txDropped==64);
    }
}
static bool syncEnabled(AirportItlwmAWDLInterface &interface) {
    static_assert(sizeof(apple80211_awdl_sync_enabled)==12,"24H32 GET117 size");
    struct { uint32_t before;apple80211_awdl_sync_enabled value;uint32_t after; } output;
    memset(&output,0xa5,sizeof(output));
    assert(!interface.getAWDL_SYNC_ENABLED(&output.value));
    assert(output.before==0xa5a5a5a5 && output.after==0xa5a5a5a5);
    assert(output.value.version==APPLE80211_VERSION && !output.value.unk1);
    assert(output.value.enabled<=1);
    return output.value.enabled!=0;
}
static void syncStateReadiness() {
    AirportItlwmAWDLInterface i;
    // IOTimerEventSource's default thread_call closes the gate before its action.
    i.device.loop.gated=true;i.requested=false;
    assert(i.device.loop.inGate() && !i.device.loop.onThread());
    assert(!syncEnabled(i) && !i.nativeSyncStateCalls);
    i.publishNativeSyncState();assert(i.nativeSyncStates.empty());
    i.device.onStart=[&] {
        // Firmware startup has not yet returned a usable radio.
        assert(i.requested && !syncEnabled(i));
        assert(i.nativeSyncStates.empty());
        i.publishNativeSyncState();assert(i.nativeSyncStates.empty());
    };
    assert(!i.setEnabled(true));
    assert(i.requested && !syncEnabled(i) && i.nativeSyncStates.empty());
    i.tick();
    assert(i.running && syncEnabled(i) && i.nativeSyncReady);
    assert(i.nativeSyncStateCalls==1 && i.nativeSyncStates==std::vector<bool>{true});
    i.device.onStart=nullptr;
    for(unsigned n=0;n<10;++n) {clockUS+=1000;i.tick();}
    assert(i.nativeSyncStateCalls==1);

    // setEnabled clears running before the next timer callback.
    assert(!i.setEnabled(false));
    assert(!i.running && !syncEnabled(i) && i.nativeSyncStateCalls==1);
    i.tick();
    assert(!i.nativeSyncReady && i.nativeSyncStateCalls==2);
    assert((i.nativeSyncStates==std::vector<bool>{true,false}));
    i.tick();assert(i.nativeSyncStateCalls==2);
    assert(!i.setEnabled(true));i.tick();
    assert(syncEnabled(i) && i.nativeSyncStateCalls==3);

    // AP reassociation keeps the native interface while real radio state changes.
    i.device.stopAWDLRadio();i.device.com.ns_nstate=0;
    clockUS+=1000000;i.tick();
    assert(i.requested && !i.running && i.nativeUp && !i.network.up);
    assert(!syncEnabled(i) && i.nativeSyncStateCalls==4);
    i.tick();assert(i.nativeSyncStateCalls==4);
    i.device.com.ns_nstate=IEEE80211_S_RUN;
    clockUS+=1000000;i.tick();
    assert(i.requested && i.running && syncEnabled(i) && i.nativeSyncStateCalls==5);
    assert((i.nativeSyncStates==std::vector<bool>{true,false,true,false,true}));
    assert(!i.setInterfaceEnable(false));i.tick();
    assert(!syncEnabled(i) && i.nativeSyncStateCalls==6);

    for(int error:{ENETDOWN,EIO}) {
        AirportItlwmAWDLInterface failed;
        failed.device.loop.gated=true;failed.device.startError=error;
        for(unsigned n=0;n<3;++n) {clockUS+=1000000;failed.tick();}
        assert(failed.device.starts==3 && !syncEnabled(failed));
        assert(!failed.running && !failed.nativeSyncReady && failed.nativeSyncStates.empty());
        failed.device.startError=0;clockUS+=1000000;failed.tick();
        assert(syncEnabled(failed) && failed.nativeSyncStateCalls==1);
        assert(!failed.setInterfaceEnable(false));failed.tick();
        assert(!syncEnabled(failed) && failed.nativeSyncStateCalls==2);
    }
    // A native disable while startup sleeps must never publish the transient ready.
    for(bool stopping:{false,true}) {
        AirportItlwmAWDLInterface cancelled;cancelled.device.loop.gated=true;
        cancelled.device.onStart=[&] {
            assert(!cancelled.setEnabled(false));
            cancelled.stopping=stopping;
        };
        cancelled.tick();
        assert(!cancelled.running && !cancelled.device.awdlRadio.ready);
        assert(!cancelled.nativeSyncReady && cancelled.nativeSyncStates.empty());
        assert(!cancelled.nativeSyncStateCalls);
    }
    for(unsigned blocked=0;blocked<8;++blocked) {
        AirportItlwmAWDLInterface guarded;
        guarded.device.loop.gated=true;guarded.device.awdlRadio.ready=true;
        if(blocked==0)guarded.stopping=true;
        if(blocked==1)guarded.radio=nullptr;
        if(blocked==2)guarded.haveController=false;
        if(blocked==3)guarded.controller.queue=nullptr;
        if(blocked==4)guarded.device.loop.gated=false;
        if(blocked==5)guarded.haveManager=false;
        if(blocked==6)guarded.nativeManager.managerClass=false;
        if(blocked==7)guarded.regulatoryPlanBlocked=true;
        guarded.publishNativeSyncState();
        assert(!guarded.nativeSyncReady && !guarded.nativeSyncStateCalls && guarded.nativeSyncStates.empty());
        assert(guarded.nativeManagerReads==unsigned(blocked==5 || blocked==6));
        guarded.stopping=false;guarded.radio=&guarded.device;
        guarded.haveController=true;guarded.controller.queue=&guarded.device.loop;
        guarded.device.loop.gated=true;
        assert(!guarded.device.loop.onThread());
        guarded.haveManager=guarded.nativeManager.managerClass=true;
        guarded.regulatoryPlanBlocked=false;
        guarded.publishNativeSyncState();
        assert(guarded.nativeSyncReady && guarded.nativeSyncStateCalls==1);
        guarded.publishNativeSyncState();assert(guarded.nativeSyncStateCalls==1);
        guarded.regulatoryStopRequired=true;guarded.publishNativeSyncState();
        assert(!guarded.nativeSyncReady && !syncEnabled(guarded));
        assert((guarded.nativeSyncStates==std::vector<bool>{true,false}));
    }
}
static void multicastPair() {
    for(unsigned first:{0,8,10}) {
        clockUS=10000000;
        AirportItlwmAWDLInterface i;i.tick();
        for(auto &channel:i.sync.channels)channel=6;
        const auto epoch=i.sync.epochUS;
        clockUS=epoch+first*65536+5000;
        auto packet=bonjourPacket(i.address.octet,false);const auto original=packet->bytes;
        i.network.output.push_back(packet);assert(!i.pullOutputPackets());
        assert(i.pendingCount==2 && i.multicastCopiesQueued==1 && !i.multicastCopiesDropped);
        i.device.acknowledgeData=false;i.tick();
        assert(i.pendingCount==1 && i.inFlightCount==1 && i.device.dataFrames.size()==1);
        const auto second=first==8?10:8+(first==10?16:0);
        clockUS=epoch+second*65536+5000;i.tick();
        assert(!i.pendingCount && i.inFlightCount==2 && i.device.dataFrames.size()==2);
        const auto &a=i.device.dataFrames[0],&b=i.device.dataFrames[1];
        assert(le16(a.data()+34)!=le16(b.data()+34));
        assert(le16(a.data()+22)!=le16(b.data()+22));
        for(const auto &frame:i.device.dataFrames) {
            DataBatch decoded={};assert(parseDataMPDU(frame.data(),frame.size(),decoded)==ParseResult::Valid);
            std::vector<uint8_t> restored(original.size());
            assert(ethernetFromData(decoded.frames[0],restored.data(),restored.size())==original.size());
            assert(restored==original);
        }
        for(auto token:i.device.dataTokens)i.completeData(token,0);
        assert(!i.pendingCount && !i.inFlightCount && i.dataGroupCompleted==2 && !allocations);
        i.tick();assert(i.device.dataFrames.size()==2);
    }
    {
        clockUS=10000000;
        AirportItlwmAWDLInterface i;i.tick();for(auto &channel:i.sync.channels)channel=6;
        const auto epoch=i.sync.epochUS;clockUS=epoch+5000;
        i.device.acknowledgeData=false;
        i.network.output.push_back(bonjourPacket(i.address.octet,false));assert(!i.pullOutputPackets());
        i.tick();const auto first=i.device.dataFrames[0];
        const auto oldToken=i.device.dataTokens[0];i.completeData(oldToken,ECANCELED);
        assert(i.pendingCount==2 && !i.inFlightCount);
        clockUS=epoch+8*65536+5000;i.tick();
        assert(i.pendingCount==1 && i.inFlightCount==1);
        i.completeData(i.device.dataTokens[1],0);
        clockUS=epoch+10*65536+5000;i.tick();
        assert(!i.pendingCount && i.inFlightCount==1 && i.dataRetried==1);
        assert(i.device.dataFrames[2]==first);
        assert(le16(i.device.dataFrames[1].data()+34)!=le16(first.data()+34));
        i.completeData(i.device.dataTokens[2],ECANCELED);
        i.completeData(oldToken,0);
        assert(!i.pendingCount && !i.inFlightCount && i.dataAbandoned==1 && i.dataGroupCompleted==1 && !allocations);
    }
    {
        clockUS=10000000;
        AirportItlwmAWDLInterface i;i.tick();for(auto &channel:i.sync.channels)channel=6;
        const auto epoch=i.sync.epochUS;clockUS=epoch+8*65536+5000;
        i.network.output.push_back(bonjourPacket(i.address.octet,false));failedDuplicates=1;
        assert(!i.pullOutputPackets());assert(!failedDuplicates && i.multicastCopiesDropped==1);
        i.tick();assert(i.pendingCount==1 && !i.device.dataFrames.size());
        clockUS=epoch+10*65536+5000;i.tick();
        assert(!i.pendingCount && i.dataGroupCompleted==1 && !allocations);
    }
    {
        clockUS=10000000;
        AirportItlwmAWDLInterface i;i.tick();
        const uint8_t destination[]={2,0,0,0,0,2};
        i.enqueue(ethernet(i.address.octet,destination));
        for(unsigned n=0;n<32;++n)i.network.output.push_back(bonjourPacket(i.address.octet,false));
        assert(i.pullOutputPackets()==kIOReturnNoResources && i.pendingCount==64);
        assert(i.multicastCopiesQueued==31 && i.multicastCopiesDropped==1 && i.network.output.empty());
        unsigned originals=0,copies=0;
        for(const auto &entry:i.pending) {
            originals+=entry.multicastQueue==AirportItlwmAWDLInterface::Pending::MulticastSocial;
            copies+=entry.multicastQueue==AirportItlwmAWDLInterface::Pending::MulticastSlot8;
        }
        assert(originals==32 && copies==31);
        i.drain();assert(!allocations);
    }
    {
        clockUS=10000000;
        AirportItlwmAWDLInterface i;i.tick();
        i.network.output.push_back(bonjourPacket(i.address.octet,false));assert(!i.pullOutputPackets());
        const auto queued=i.pending[i.pendingHead].queuedUS;
        clockUS=queued+i.DataLifetimeUS+1;i.tick();
        assert(!i.pendingCount && !i.inFlightCount && i.txDropped==2 && !allocations);
    }
}

static void dataDemand() {
    const uint8_t destination[]={2,0,0,0,0,2};
    enum Scenario { Empty, InFlight, Receiving, ExactExpiry, Expired, Future, OtherChannel, Stale, Dense };
    for(auto scenario:{Empty,InFlight,Receiving,ExactExpiry,Expired,Future,OtherChannel,Stale,Dense}) {
        clockUS=10000000;
        AirportItlwmAWDLInterface i;dataPeer(i,destination);
        auto peer=i.device.awdlPeers.find(destination,clockUS);assert(peer && !peer->nativeCached);
        Window window={};assert(i.sync.window(clockUS,window));
        if(scenario!=Dense) {
            for(auto &channel:i.sync.channels)channel=0;
            i.sync.channels[(window.counter/i.sync.presence)%16]=44;
        }
        if(scenario==InFlight) {
            i.device.acknowledgeData=false;
            i.enqueue(ethernet(i.address.octet,destination));i.tick();
            assert(!i.pendingCount && i.inFlightCount==1);
        } else if(scenario!=Empty) {
            peer->lastUnicastDataUS=clockUS;
            if(scenario==ExactExpiry)peer->lastUnicastDataUS=clockUS-i.DataLifetimeUS;
            if(scenario==Expired)peer->lastUnicastDataUS=clockUS-i.DataLifetimeUS-1;
            if(scenario==Future)peer->lastUnicastDataUS=clockUS+1;
            if(scenario==OtherChannel)for(auto &channel:peer->sequence.channel)channel=6;
            if(scenario==Stale)peer->syncReceivedUS=clockUS-Peer::ScheduleFreshnessUS;
        }
        i.tick();
        const bool expected=scenario==InFlight||scenario==Receiving||scenario==ExactExpiry;
        assert(i.device.lastPolicy.dataDemand==expected);
        assert(i.device.lastPolicy.isolated==(scenario!=Dense));
        i.drain();
        assert(!peer->lastUnicastDataUS && !i.inFlightCount && !i.pendingCount);
    }
}

static void sharedChannelGrant() {
    const uint8_t destination[]={2,0,0,0,0,2}, otherAddress[]={2,0,0,0,0,3};
    for (unsigned otherChannel : {0U, 6U, 149U}) {
        clockUS=10000000;
        AirportItlwmAWDLInterface i;dataPeer(i,destination);
        auto peer=i.device.awdlPeers.find(destination,clockUS);assert(peer);
        for(auto &channel:i.sync.channels)channel=6;
        for(auto &channel:peer->sequence.channel)channel=6;
        peer->lastUnicastDataUS=clockUS;
        if(otherChannel) {
            auto &other=i.device.awdlPeers.peers[1];assert(&other!=peer);
            other=*peer;copyAddress(other.address,otherAddress);
            for(auto &channel:other.sequence.channel)channel=otherChannel;
        }
        Window local={},remote={};
        assert(i.sync.window(clockUS,local) && peer->window(clockUS,remote));
        RadioState::PeerGrant grant={};grant.valid=grant.dense=true;
        grant.epoch=i.device.dmaEpoch;copyAddress(grant.address,destination);
        grant.endUS=clockUS+local.untilNextEAWUS;
        grant.localCounter=local.counter-local.counter%i.sync.presence;
        grant.remoteCounter=remote.counter-remote.counter%peer->sync.presenceMode;
        grant.localPeriod=i.sync.periodTU;grant.localPresence=i.sync.presence;
        grant.remotePeriod=peer->sync.windowPeriodTU;grant.remotePresence=peer->sync.presenceMode;
        const uint64_t end=clockUS+std::min(local.untilNextEAWUS,remote.untilNextEAWUS);
        assert(i.peerGrantEnd(clockUS,grant,true)==end);
        peer->lastUnicastDataUS=0;
        assert(!i.peerGrantEnd(clockUS,grant,true));
        assert(i.peerGrantEnd(clockUS,grant,false)==end);
        peer->syncReceivedUS=clockUS-Peer::ScheduleFreshnessUS;
        assert(!i.peerGrantEnd(clockUS,grant,false));
    }
}

static void nativeTransmitActivity() {
    const uint8_t peer[]={2,0,0,0,0,2},group[]={0x33,0x33,0,0,0,0xfb};
    for(bool multicast:{false,true}) for(int error:{0,ENOBUFS,ENOMEM,ENETDOWN,EINVAL}) {
        clockUS=10000000;
        AirportItlwmAWDLInterface i;dataPeer(i,peer);
        i.device.loop.gated=true;i.device.loop.thread=false;
        i.device.acknowledgeData=false;
        if(multicast)clockUS=i.sync.epochUS+8*65536+5000;
        i.tick();
        assert(i.nativeTxActivity.empty() && i.nativeRxActivity.empty());
        assert(i.nativeDataDeltas.empty());
        i.device.transmitError=error;
        i.enqueue(ethernet(i.address.octet,multicast?group:peer));
        i.tick();
        const unsigned expected=!multicast && !error;
        assert(i.nativeTxActivity.size()==expected && i.nativeRxActivity.empty());
        assert(i.nativeDataDeltas.size()==expected);
        assert(!i.dataUnicastAcknowledged);
        if(expected) {
            assert(i.nativeTxActivity[0]==clockUS*7+0x100000000ULL);
            assert(i.nativeDataDeltas[0].txPackets==1 && i.nativeDataDeltas[0].txBytes==54);
            assert(i.inFlightCount==1 && !i.pendingCount);
            clockUS+=1000;i.tick();
            assert(i.nativeTxActivity.size()==1 && i.nativeDataDeltas.size()==1);
            const auto first=i.device.dataTokens.back();
            i.completeData(first,ECANCELED);i.tick();
            assert(i.nativeDataDeltas.size()==1 && !i.inFlightCount && i.pendingCount==1);
            ++i.device.awdlRadio.dwellStarts;clockUS+=1000;i.tick();
            assert(i.dataRetried==1 && i.nativeTxActivity.size()==2 && i.nativeDataDeltas.size()==2);
            assert(i.nativeDataDeltas[1].txPackets==1 && i.nativeDataDeltas[1].txBytes==54);
            i.completeData(first,0);i.completeData(i.device.dataTokens.back(),EIO);
            assert(i.nativeTxActivity.size()==2 && i.nativeDataDeltas.size()==2 && !i.dataUnicastAcknowledged);
        }
        i.drain();
    }
}

static void blockAckReplies() {
    const uint8_t peer[]={2,0,0,0,0,2};
    BlockAckRequest request={};copyAddress(request.peer,peer);request.dialog=100;request.tid=1;
    auto receive=[&](AirportItlwmAWDLInterface &i,const BlockAckRequest &r) {
        i.device.loop.gated=true;AirportItlwmAWDLInterface::receiveBlockAck(&i,r);i.device.loop.gated=false;
    };
    auto prepare=[&](AirportItlwmAWDLInterface &i) {
        dataPeer(i,peer);i.nextActionUS=i.nextMIFUS=clockUS+2000000;
    };
    {
        clockUS=10000000;AirportItlwmAWDLInterface i;prepare(i);
        AirportItlwmAWDLInterface::receiveBlockAck(&i,request);assert(!i.blockAckReceived);
        receive(i,request);assert(i.blockAckReceived==1 && i.blockAckQueued==1 && i.scheduler.delay==1);
        const auto queued=i.blockAckReplies[0].queuedUS,token=i.blockAckReplies[0].token;
        clockUS+=1000;receive(i,request);
        assert(i.blockAckCoalesced==1 && i.blockAckReplies[0].queuedUS==queued);
        i.tick();assert(i.blockAckSubmitted==1 && !i.blockAckAcknowledged);
        auto &reply=i.device.lastUnicast;
        assert(reply.size()==33 && reply[0]==0xd0 && reply[1]==0);
        assert(sameAddress(reply.data()+4,peer) && sameAddress(reply.data()+10,i.address.octet));
        assert(sameAddress(reply.data()+16,BSSID) && reply[24]==3 && reply[25]==1);
        assert(reply[26]==100 && le16(reply.data()+27)==37 && le16(reply.data()+29)==4);
        assert(!le16(reply.data()+31) && i.device.transmitDeadlines.back()>clockUS);
        assert(i.device.transmitDeadlines.back()<=i.device.awdlRadio.dwell.windowEndUS);
        Window local={},remote={};assert(i.sync.window(clockUS,local));
        assert(i.device.awdlPeers.find(peer,clockUS)->window(clockUS,remote));
        assert(i.device.transmitDeadlines.back()<=clockUS+local.untilNextEAWUS-3000);
        assert(i.device.transmitDeadlines.back()<=clockUS+remote.untilNextEAWUS-3000);
        assert(i.device.lastPolicy.dataDemand==false && !i.nativeDataDeltas.size());
        receive(i,request);assert(i.blockAckCoalesced==2 && i.blockAckSubmitted==1);
        // Native OOB shares the allocator but keeps a separate completion/status owner.
        auto &native=i.unicast[0];native.state=UnicastRequest::Submitted;
        native.token=i.nextManagementToken();assert(native.token>token);
        native.queuedUS=clockUS;copyAddress(native.destination,peer);
        i.device.awdlPeers.find(peer,clockUS)->nativeCached=true;
        i.completeUnicast(token,false);assert(i.blockAckFailed==1 && native.state==UnicastRequest::Submitted);
        const auto oldSequence=le16(reply.data()+22);
        receive(i,request);i.tick();assert(i.blockAckSubmitted==2);
        assert(le16(i.device.lastUnicast.data()+22)!=oldSequence && !i.device.lastUnicast[1]);
        i.completeUnicast(token,true);assert(!i.blockAckAcknowledged);
        i.completeUnicast(i.blockAckReplies[0].token,true);assert(i.blockAckAcknowledged==1);
        assert(native.state==UnicastRequest::Submitted);
        i.completeUnicast(native.token,true);assert(native.state==UnicastRequest::Complete && !native.status);
        i.drain();
    }
    for(int error:{ENOBUFS,ENOMEM,ENETDOWN,EINVAL}) {
        clockUS=10000000;AirportItlwmAWDLInterface i;prepare(i);receive(i,request);
        const auto token=i.blockAckReplies[0].token;i.device.transmitError=error;i.tick();
        assert(!i.blockAckSubmitted && !i.blockAckAcknowledged);
        if(error==EINVAL)assert(i.blockAckFailed==1 && i.blockAckReplies[0].state==BlockAckReply::Free);
        else {
            assert(i.blockAckReplies[0].state==BlockAckReply::Queued);
            i.device.transmitError=0;i.tick();assert(i.blockAckSubmitted==1);
            i.completeUnicast(token,true);assert(i.blockAckAcknowledged==1);
        }
        i.drain();
    }
    {
        clockUS=10000000;AirportItlwmAWDLInterface i;prepare(i);
        for(unsigned tid=0;tid<8;++tid) {auto r=request;r.tid=tid;receive(i,r);}
        auto ninth=request;ninth.tid=8;receive(i,ninth);
        assert(i.blockAckQueued==8 && i.blockAckRejected==1);
        const auto first=i.blockAckReplies[0].token;
        auto replacement=request;replacement.tid=0;replacement.dialog=101;receive(i,replacement);
        assert(i.blockAckReplies[0].token>first && i.blockAckReplies[0].request.dialog==101);
        const auto age=i.blockAckReplies[0].queuedUS;
        clockUS=age+BlockAckReply::LifetimeUS-1;receive(i,replacement);
        assert(i.blockAckReplies[0].queuedUS==age);
        clockUS=age+BlockAckReply::LifetimeUS;i.tick();
        assert(i.blockAckExpired==8 && !i.blockAckSubmitted);
        receive(i,request);const auto stale=i.blockAckReplies[0].token;i.drain();
        i.completeUnicast(stale,true);assert(!i.blockAckAcknowledged);
        assert(i.nextManagementToken()>stale);
        i.unicastToken=~uint64_t(0)-1;
        assert(i.nextManagementToken()==~uint64_t(0) && !i.nextManagementToken());
        receive(i,request);assert(i.blockAckRejected==2);
    }
    // Run the actual tick/service boundary for invalidated owners and unavailable grants.
    for(unsigned reason=0;reason<13;++reason) {
        clockUS=10000000;AirportItlwmAWDLInterface i;prepare(i);receive(i,request);
        auto p=i.device.awdlPeers.find(peer,clockUS);assert(p && !p->nativeCached);
        if(reason==0)p->occupied=false;
        if(reason==1)for(auto &c:p->sequence.channel)c=6;
        if(reason==2)p->syncReceivedUS=clockUS-Peer::ScheduleFreshnessUS;
        if(reason==3){i.device.deferChannel=true;i.device.awdlRadio.dwell.cancelling=true;}
        if(reason==4)i.device.com.txq[1].queued=48;
        if(reason==5)++i.device.dmaEpoch;
        if(reason==6)i.requested=false;
        if(reason==7)i.stopping=true;
        if(reason==8)clockUS=i.sync.epochUS+65536-3001;
        if(reason==8)allocationDelayUS=2;
        if(reason==9)clockUS+=BlockAckReply::LifetimeUS;
        if(reason==10 || reason==11) {
            p->sync.nextWindowTU=8; // Peer EAW ends earlier than this local EAW.
            if(reason==10)clockUS+=192; // Exactly3000us remains: no positive TX budget.
            else allocationDelayUS=193; // Cross only the peer guard during allocation.
        }
        if(reason==12)clockUS=i.sync.epochUS+65536-3000;
        i.tick();allocationDelayUS=0;
        assert(!i.blockAckSubmitted && !i.blockAckAcknowledged);
        i.drain();
    }
    {
        clockUS=10000000;AirportItlwmAWDLInterface i;prepare(i);receive(i,request);
        const auto old=i.blockAckReplies[0].token;
        i.device.transmitError=ENETDOWN;
        i.device.onReject=[&](uint64_t token) {
            assert(token==old);i.drain();
            AirportItlwmAWDLInterface::receiveBlockAck(&i,request);
        };
        i.device.loop.gated=true;
        i.tick();assert(i.blockAckReplies[0].state==BlockAckReply::Queued);
        assert(i.blockAckReplies[0].token>old && !i.blockAckSubmitted);
        i.completeUnicast(old,true);assert(!i.blockAckAcknowledged);i.drain();
    }
}

static Peer *aggregatePeer(AirportItlwmAWDLInterface &i,const uint8_t *dest) {
    dataPeer(i,dest);
    i.device.loop.gated=true;
    auto peer=i.device.awdlPeers.find(dest,clockUS);assert(peer);
    peer->metadata.fields|=HTCapabilities;peer->metadata.rxHTMcs=0xff;
    peer->htCapabilitiesReceivedUS=clockUS;
    uint64_t serial=0;peer->txRate.prepare(i.device.dmaEpoch,44,12,serial,0xff);
    peer->txRate.current=0x87;
    i.device.awdlRadio.dwell.running=false;
    i.nextActionUS=i.nextMIFUS=clockUS+1000000;
    i.device.acknowledgeData=false;
    return peer;
}
static void aggregateOwnership() {
    const uint8_t destination[]={2,0,0,0,0,2};
    clockUS=10000000;
    AirportItlwmAWDLInterface i;aggregatePeer(i,destination);
    i.sync.wifiSequence=4095;i.sync.dataSequence=65535;
    for(unsigned n=0;n<2;++n)i.enqueue(ethernet(i.address.octet,destination));
    i.tick();
    assert(i.device.data==1 && i.dataSubmitted==2 && i.dataAggregatesSubmitted==1);
    assert(i.inFlightCount==1 && !i.pendingCount && i.aggregateExtraOwned==1);
    assert(i.device.lastPolicy.dataQueue && i.device.awdlRadio.dataQueueConfigured);
    assert(i.sync.wifiSequence==4096 && i.sync.dataSequence==1 && allocations==2);
    assert(i.nativeTxActivity.size()==2);
    const auto first=i.device.dataFrames.back();DataBatch batch={};
    assert(parseDataMPDU(first.data(),first.size(),batch)==ParseResult::Valid && batch.count==2);
    assert(batch.frames[0].sequence==65535 && batch.frames[1].sequence==0);
    const auto token=i.device.dataTokens.back();
    i.completeData(token,ECANCELED);i.completeData(token,0);
    assert(i.pendingCount==1 && !i.inFlightCount && i.dataFlushed==2 && i.aggregateExtraOwned==1);
    i.tick();assert(i.device.data==1);
    ++i.device.awdlRadio.dwellStarts;i.tick();
    assert(i.inFlightCount==1 && !i.pendingCount && i.dataRetried==2);
    auto retried=i.device.dataFrames.back();assert(retried[1]==8);retried[1]=0;
    assert(retried==first && i.sync.wifiSequence==4096 && i.sync.dataSequence==1);
    i.completeData(token,EIO);assert(i.inFlightCount==1);
    i.completeData(i.device.dataTokens.back(),0);i.tick();
    assert(!allocations && !i.aggregateExtraOwned && !i.inFlightCount && i.dataUnicastAcknowledged==2);
    assert(i.network.signals==1 && !i.txDropped);
}
static void aggregateBoundaries() {
    const uint8_t destination[]={2,0,0,0,0,2};
    for(unsigned scenario=0;scenario<10;++scenario) {
        clockUS=10000000;
        AirportItlwmAWDLInterface i;auto peer=aggregatePeer(i,destination);
        const auto queued=clockUS;
        i.enqueue(ethernet(i.address.octet,destination));
        if(scenario!=0)i.enqueue(ethernet(i.address.octet,destination));
        if(scenario==1)peer->metadata.rxHTMcs=0;
        if(scenario==2)peer->txRate.current=24;
        if(scenario==3)peer->htCapabilitiesReceivedUS=clockUS-Peer::ScheduleFreshnessUS;
        if(scenario==4) {
            peer->htCapabilitiesReceivedUS=clockUS-Peer::ScheduleFreshnessUS+10;
            allocationDelayUS=11;
        }
        if(scenario==5)i.device.transmitError=EAGAIN;
        if(scenario==6)i.device.transmitError=ENOBUFS;
        if(scenario==7)failedAllocations=1;
        if(scenario==8)i.device.com.txq[1].queued=24,i.device.com.txq[2].queued=24;
        if(scenario==9)allocationDelayUS=60536;
        i.tick();allocationDelayUS=0;
        if(scenario<5) {
            assert(!i.dataAggregatesSubmitted && i.device.data==(scenario?2u:1u));
            for(const auto &frame:i.device.dataFrames)assert(frame[0]==8);
        } else {
            assert(!i.device.data && !i.inFlightCount && !i.dataSubmitted && !i.txDropped);
            assert(i.pendingCount+i.aggregateExtraOwned==2 && allocations==2);
            assert(i.pending[i.pendingHead].queuedUS==queued);
            i.device.transmitError=0;i.device.com.txq[1].queued=i.device.com.txq[2].queued=0;
            clockUS=i.sync.epochUS+65536+5000;i.tick();
            assert(i.device.data==1 && i.dataSubmitted==2 && i.inFlightCount==1);
        }
        i.drain();assert(!allocations && !i.aggregateExtraOwned && !i.inFlightCount && !i.pendingCount);
    }
    // A submitted pair remains indivisible when HT expires; it cannot become two
    // legacy retry MPDUs with the same already-used sequence identity.
    for(unsigned scenario=0;scenario<5;++scenario) {
        clockUS=20000000;AirportItlwmAWDLInterface i;auto peer=aggregatePeer(i,destination);
        for(unsigned n=0;n<2;++n)i.enqueue(ethernet(i.address.octet,destination));
        i.tick();const auto token=i.device.dataTokens.back();
        if(scenario==0) {i.completeData(token,EIO);}
        if(scenario==1) {i.completeData(token,ECANCELED);++i.device.dmaEpoch;i.tick();}
        if(scenario==2) {i.completeData(token,ECANCELED);peer->metadata.rxHTMcs=0;
            ++i.device.awdlRadio.dwellStarts;i.tick();assert(i.device.data==1 && i.pendingCount==1);
            clockUS+=AirportItlwmAWDLInterface::DataLifetimeUS+1;i.tick();}
        if(scenario==3) {
            for(unsigned flush=1;flush<=AirportItlwmAWDLInterface::MaxDataFlushes;++flush) {
                i.completeData(i.device.dataTokens.back(),ECANCELED);
                ++i.device.awdlRadio.dwellStarts;i.tick();
            }
            i.completeData(i.device.dataTokens.back(),ECANCELED);
        }
        if(scenario==4) {i.setEnabled(false);i.completeData(token,ECANCELED);}
        assert(!allocations && !i.aggregateExtraOwned && !i.pendingCount && !i.inFlightCount);
        assert(i.txDropped==2 && i.dataAbandoned==2);
    }
}
static void aggregateCapacity() {
    const uint8_t destination[]={2,0,0,0,0,2};
    clockUS=30000000;AirportItlwmAWDLInterface i;aggregatePeer(i,destination);
    for(unsigned n=0;n<64;++n)i.enqueue(ethernet(i.address.octet,destination));
    for(unsigned n=0;n<3;++n)i.network.output.push_back(ethernet(i.address.octet,destination));
    i.tick();
    assert(i.device.data==32 && i.dataSubmitted==64 && i.inFlightCount==32 && i.aggregateExtraOwned==32);
    assert(!i.pendingCount && i.pullOutputPackets()==kIOReturnNoResources && i.network.output.size()==3);
    i.completeData(i.device.dataTokens[0],0);i.tick();
    assert(i.network.signals==1 && i.aggregateExtraOwned==31 && i.inFlightCount==31);
    assert(i.pullOutputPackets()==kIOReturnNoResources && i.network.output.size()==1 && i.pendingCount==2);
    i.completeData(i.device.dataTokens[1],ECANCELED);
    assert(i.pendingCount==3 && i.inFlightCount==30 && i.aggregateExtraOwned==31);
    i.drain();
    assert(!i.aggregateExtraOwned && !i.inFlightCount && !i.pendingCount && allocations==1);
    assert(i.txDropped==64 && i.dataUnicastAcknowledged==2);
    for(auto token:i.device.dataTokens)i.completeData(token,ECANCELED);
    assert(i.txDropped==64 && allocations==1);
}

static void listener_deadline_and_native_schedule() {
    for(unsigned remaining:{6000u,2500u,500u}) {
        clockUS=10000000;
        AirportItlwmAWDLInterface i;i.tick();
        i.device.awdlRadio.listener=true;
        i.device.awdlRadio.dwell.windowEndUS=clockUS+remaining;
        i.tick();
        assert(i.timer->delay==(remaining==6000?5u:remaining==2500?2u:1u));
        const auto p=i.device.lastPolicy;assert(p.windowEnd && p.context==&i);
        Window window={};assert(i.sync.window(clockUS,window));
        assert(p.windowEnd(p.context,clockUS,p.slot,window.channel)==clockUS+window.untilNextEAWUS);
        assert(!p.windowEnd(p.context,clockUS,p.slot+1,window.channel));
        assert(!p.windowEnd(p.context,clockUS,p.slot,0));
        assert(!p.windowEnd(p.context,clockUS+window.untilNextEAWUS,p.slot,window.channel));
        for(bool *flag:{&i.stopping,&i.regulatoryPlanBlocked,&i.regulatoryStopRequired}) {
            *flag=true;assert(!p.windowEnd(p.context,clockUS,p.slot,window.channel));*flag=false;
        }
        i.requested=false;assert(!p.windowEnd(p.context,clockUS,p.slot,window.channel));
        i.requested=true;i.sync.channels[p.slot%16]=0;
        assert(!p.windowEnd(p.context,clockUS,p.slot,window.channel));
    }
}

static void blockAckAcceptance() {
    const uint8_t peer[]={2,0,0,0,0,2};
    BlockAckRequest request={};copyAddress(request.peer,peer);request.dialog=17;request.tid=0;
    request.immediate=request.amsdu=true;request.window=64;request.ssn=123;
    auto receive=[&](AirportItlwmAWDLInterface &i,const BlockAckRequest &r) {
        i.device.loop.gated=true;AirportItlwmAWDLInterface::receiveBlockAck(&i,r);i.device.loop.gated=false;
    };
    auto setup=[&](AirportItlwmAWDLInterface &i) {
        dataPeer(i,peer);i.nextActionUS=i.nextMIFUS=clockUS+2000000;i.device.baResult=0;
    };
    {
        clockUS=10000000;AirportItlwmAWDLInterface i;setup(i);receive(i,request);i.tick();
        assert(i.blockAckSubmitted==1 && i.device.baPreparations==1);
        const auto &frame=i.device.lastUnicast;assert(frame.size()==33 && le16(frame.data()+27)==0);
        assert(le16(frame.data()+29)==(3|(64<<6)) && !le16(frame.data()+31));
        const auto oldToken=i.blockAckReplies[0].token;auto next=request;++next.dialog;++next.ssn;
        receive(i,next);i.tick();assert(i.device.baPreparations==1 && i.blockAckSubmitted==1);
        i.completeUnicast(oldToken,true);i.tick();assert(i.device.baPreparations==2 && i.blockAckSubmitted==2);
    }
    for(int kind=0;kind<4;++kind) {
        clockUS=10000000;AirportItlwmAWDLInterface i;setup(i);receive(i,request);
        i.device.onBAPrepare=[&] {
            if(kind==0)i.device.baLive=false;
            if(kind==1)++i.device.dmaEpoch;
            if(kind==2){i.device.loop.gated=true;AirportItlwmAWDLInterface::receiveBlockAckEnd(&i,request);i.device.loop.gated=false;}
            if(kind==3)clockUS+=BlockAckReply::LifetimeUS;
        };
        i.tick();assert(!i.blockAckSubmitted);
    }
    {
        clockUS=10000000;AirportItlwmAWDLInterface i;setup(i);i.device.baResult=EBUSY;
        receive(i,request);i.tick();assert(!i.blockAckSubmitted && !i.blockAckReplies[0].prepared);
        i.device.baResult=0;i.tick();assert(i.blockAckSubmitted==1);
    }
    {
        clockUS=10000000;AirportItlwmAWDLInterface i;setup(i);auto end=request;end.teardown=true;
        receive(i,end);i.tick();auto token=i.blockAckReplies[0].token;
        assert(i.blockAckSubmitted==1 && i.device.lastUnicast.size()==30 && i.device.lastUnicast[25]==2);
        i.completeUnicast(token,false);i.tick();assert(i.blockAckSubmitted==2 && i.blockAckReplies[0].token!=token);
        token=i.blockAckReplies[0].token;i.completeUnicast(token,true);assert(i.device.baEnds==1);
        i.completeUnicast(token,true);assert(i.device.baEnds==1);
    }
}

// NATIVE TX SCENARIOS
int main() {
#if AWDL_TICK_CASE == 44
    timerWakeCoalescing();
#elif AWDL_TICK_CASE == 43
    multiWindowFlushSurvivalAndFIFOOrder();
#elif AWDL_TICK_CASE == 42
    sidecarLatencyAndLinkQuality();
#elif AWDL_TICK_CASE == 41
    nativeTXSplitRetriedPair();
#elif AWDL_TICK_CASE == 39
    nativeTXNegotiation();
#elif AWDL_TICK_CASE == 40
    nativeTXAdmissionAndDeadline();
#elif AWDL_TICK_CASE == 38
    blockAckAcceptance();
#elif AWDL_TICK_CASE == 37
    listener_deadline_and_native_schedule();
#elif AWDL_TICK_CASE == 34
    aggregateOwnership();
#elif AWDL_TICK_CASE == 35
    aggregateBoundaries();
#elif AWDL_TICK_CASE == 36
    aggregateCapacity();
#elif AWDL_TICK_CASE == 1
    timers();cancellation();
#elif AWDL_TICK_CASE == 2
    pending();elapsedDuringTX();
#elif AWDL_TICK_CASE == 3
    configuration();
#elif AWDL_TICK_CASE == 4
    actionRetries();
#elif AWDL_TICK_CASE == 5
    outputFlow();
#elif AWDL_TICK_CASE == 6
    cacheLifecycle();configureDuringLinkUp();
#elif AWDL_TICK_CASE == 7
    availability();
#elif AWDL_TICK_CASE == 8
    unicastOwnership();
#elif AWDL_TICK_CASE == 9
    unicastLifecycle();
#elif AWDL_TICK_CASE == 10
    broadcastSchedule();
#elif AWDL_TICK_CASE == 11
    broadcastLifecycle();
#elif AWDL_TICK_CASE == 12
    psfSuppression();
#elif AWDL_TICK_CASE == 13
    dataWindowRecheck();
#elif AWDL_TICK_CASE == 14
    dataResourceRetries();
#elif AWDL_TICK_CASE == 15
    availabilityClockCorrections();
#elif AWDL_TICK_CASE == 16
    availabilityOverlappingWindows();
#elif AWDL_TICK_CASE == 17
    broadcastClockCorrections();
#elif AWDL_TICK_CASE == 18
    multicastDiscoveryWindows();
#elif AWDL_TICK_CASE == 19
    nonblockingStop();
#elif AWDL_TICK_CASE == 21
    dataFlushRetry();
#elif AWDL_TICK_CASE == 22
    bonjourBootstrap();
#elif AWDL_TICK_CASE == 23
    lostRelayElection();
#elif AWDL_TICK_CASE == 24
    unicastRejectedBeforeSubmission();
#elif AWDL_TICK_CASE == 25
    associationLossRecovery();
#elif AWDL_TICK_CASE == 26
    syncStateReadiness();
#elif AWDL_TICK_CASE == 27
    dataDemand();
#elif AWDL_TICK_CASE == 28
    multicastPair();
#elif AWDL_TICK_CASE == 29
    nativeTransmitActivity();
#elif AWDL_TICK_CASE == 30
    dataRecipientCutoff();
#elif AWDL_TICK_CASE == 31
    blockAckReplies();
#elif AWDL_TICK_CASE == 32
    sharedChannelGrant();
#elif AWDL_TICK_CASE == 33
    lostDirectParentElection();
#else
    logicalWindowResume();unicastAcrossGrantGap();
#endif
    assert(!allocations);
}
