#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "AirportItlwm/AWDL/RadioState.hpp"
#include "AirportItlwm/AWDL/DataFrame.hpp"
using AirportAWDL::RadioState;
using IOReturn = int;
enum { kIOReturnSuccess, kIOReturnUnsupported, kIOReturnNotReady, kIOReturnBadArgument,
       kIOReturnNotPrivileged, kIOReturnBusy, kIOReturnIOError };
struct OSObject { virtual ~OSObject() = default; };
struct OSBoolean : OSObject { bool value; explicit OSBoolean(bool v):value(v) {} bool getValue() {return value;} };
struct OSNumber : OSObject {};
struct OSDictionary : OSObject {
    std::map<std::string,OSObject *> values;
    OSObject *getObject(const char *key) {auto p=values.find(key);return p==values.end()?nullptr:p->second;}
    unsigned getCount() {return values.size();}
};
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
static void *current_task() {return reinterpret_cast<void *>(uintptr_t(0x1234));}
#define kIOClientPrivilegeAdministrator "root"
static uint64_t clockUS = 1000000;
static uint64_t awdlNow() {return clockUS;}
static uint64_t awdlRadioNow() {return clockUS;}
struct IOUserClient {
    static IOReturn privilege;
    static unsigned checks;
    static IOReturn clientHasPrivilege(void *task,const char *name) {
        assert(task==current_task() && !strcmp(name,"root"));++checks;return privilege;
    }
};
IOReturn IOUserClient::privilege=kIOReturnSuccess;
unsigned IOUserClient::checks=0;
struct WorkLoop {bool gated=false;bool inGate() {return gated;}};
struct Gate {
    WorkLoop *loop;unsigned entries=0;
    IOReturn runAction(IOReturn (*action)(OSObject *,void *,void *,void *,void *),void *self,void *arg=nullptr) {
        bool saved=loop->gated;++entries;loop->gated=true;
        auto result=action(nullptr,self,arg,nullptr,nullptr);
        loop->gated=saved;return result;
    }
    void commandWakeup(void *) {}
};
struct IO80211AWDLProtocol : OSObject {
    unsigned delegated=0;
    IOReturn setProperties(OSObject *) {++delegated;return 99;}
};
#define super IO80211AWDLProtocol
// FIRMWARE DECLARATIONS
static uint16_t htole16(uint16_t v) {return v;}
static uint32_t htole32(uint32_t v) {return v;}
static uint32_t le32toh(uint32_t v) {return v;}
static bool isset(const uint8_t *bits,unsigned bit) {return bits[bit/8]&(1U<<(bit%8));}
enum {IEEE80211_M_STA=1,IEEE80211_S_RUN=4,IFF_UP=1,IFF_RUNNING=0x40};
struct iwx_phyctxt {unsigned id=0;unsigned *channel=nullptr;};
struct iwx_node {unsigned in_id=0,in_color=0;iwx_phyctxt phy; iwx_phyctxt *in_phyctxt=&phy;};
struct iwx_softc {
    unsigned sc_device_family=IWX_DEVICE_FAMILY_22000,sc_flags=IWX_FLAG_BINDING_ACTIVE;
    int ns_nstate=IEEE80211_S_RUN;
    uint8_t sc_enabled_capa[64]={};
    struct {int ic_opmode=IEEE80211_M_STA,ic_state=IEEE80211_S_RUN;iwx_node *ic_bss=nullptr;unsigned ic_channels[256]={};
        struct {struct {unsigned if_flags=IFF_UP|IFF_RUNNING;} ac_if;} ic_ac;} sc_ic;
};
struct ItlIwx {
    bool receiveBA=false;
    bool awdlHasRXBA() const { return receiveBA; }
    int clearAWDLBlockAck() { return 0; }
    void resetAWDLBlockAck() {}

    WorkLoop loop;Gate gate{&loop};
    WorkLoop *getMainWorkLoop() {return &loop;}
    Gate *getMainCommandGate() {return &gate;}
    iwx_softc com;iwx_node ap;
    RadioState awdlRadio={};
    uint64_t dmaEpoch=1;
    uint64_t awdlCompareEndUS=0,awdlCompareArmSubmitted[2]={},awdlCompareBTOverrides=0;
    uint8_t txMask=3;
    uint8_t awdlCompareArm() const;
    uint8_t iwx_fw_valid_tx_ant(iwx_softc *) {return txMask;}
    bool awdlReceiveAllData=false,awdlReceiveFilterKnown=false;
    int awdlReceiveFilterCommandError=0;
    IOReturn awdlReceiveFilterCleanupResult=kIOReturnSuccess;
    void *awdlReceiver=nullptr;
    void (*awdlAvailabilityChanged)(void *)=nullptr;
    struct Command {unsigned id;iwx_mac_ctx_cmd mac;};
    std::vector<Command> commands;
    std::function<void(uint32_t)> duringCommand;
    int modifyError=0,removeError=0,finishError=0,powerError=0;
    bool deferFinish=false;
    unsigned finishCalls=0,powerCalls=0;
    ItlIwx() {
        com.sc_ic.ic_bss=&ap;
        com.sc_enabled_capa[IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD/8]|=1U<<(IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD%8);
        awdlRadio.queue=IWX_INVALID_QUEUE;
    }
    int iwx_send_cmd_pdu(iwx_softc *sc,unsigned id,unsigned flags,size_t size,const void *bytes) {
        assert(sc==&com && !flags && loop.gated);
        iwx_mac_ctx_cmd cmd={};
        if(id==IWX_MAC_CONTEXT_CMD) {
            assert(size==sizeof(cmd));memcpy(&cmd,bytes,size);
            assert(cmd.id_and_color==IWX_FW_CMD_ID_AND_COLOR(RadioState::MacID,0));
            commands.push_back({id,cmd});
            if(duringCommand)duringCommand(cmd.action);
            return cmd.action==IWX_FW_CTXT_ACTION_MODIFY?modifyError:
                (cmd.action==IWX_FW_CTXT_ACTION_REMOVE?removeError:0);
        }
        assert(id==IWX_POWER_TABLE_CMD && size==sizeof(iwx_device_power_cmd));
        commands.push_back({id,cmd});return 0;
    }
    unsigned iwx_lookup_cmd_ver(iwx_softc *,unsigned,unsigned) {return 3;}
    bool awdlChannelAllowed(unsigned channel,uint8_t width=20) const {return width==20 && (channel==6||channel==44||channel==149);}
    bool awdlWideChannelAllowed(uint32_t channel) const {return awdlChannelAllowed(channel,80);}
    int finishAWDLDwell() {
        ++finishCalls;
        if(finishError)return finishError;
        if(deferFinish && awdlRadio.dwell.requested && !awdlRadio.dwell.done) {
            awdlRadio.dwell.running=false;
            awdlRadio.dwell.cancelling=awdlRadio.dwell.cancelAcknowledged=true;
            return EINPROGRESS;
        }
        awdlRadio.dwell={};awdlRadio.station=awdlRadio.binding=false;awdlRadio.queue=IWX_INVALID_QUEUE;
        return 0;
    }
    int iwx_power_update_device(iwx_softc *sc) {assert(sc==&com);++powerCalls;return powerError;}
    int awdlMAC(uint32_t,bool=false);
    int serviceAWDLReceiveFilter(uint64_t (*)(const void *,uint64_t),const void *);
    int prepareAWDLListener(uint16_t,uint8_t=20) {assert(false);return ENOTSUP;}
    int startAWDLRadio(const uint8_t *);
    int stopAWDLRadio();
    void resetAWDLRadio();
};
struct Timer {unsigned calls=0,delay=0;void setTimeoutMS(unsigned ms) {++calls;delay=ms;} void setTimeoutUS(unsigned us) {++calls;delay=(us+999)/1000;}};
struct AirportItlwmAWDLInterface : IO80211AWDLProtocol {
    ItlIwx device,*radio=&device;
    Timer scheduler,*timer=&scheduler;
    bool requested=true,running=true,stopping=false,ticking=false,wakeArmed=false,interfaceEnabled=true;
    bool infrastructurePaused=false;
    bool receiveFilterChanging=false;
    bool regulatoryPlanBlocked=false,regulatoryStopRequired=false,regulatoryResumePending=false;
    struct {uint8_t octet[6]={2,0,0,0,0,0x81};} address;
    unsigned references=1,drains=0,availability=0,unicast=0,timerBodies=0;
    uint64_t nextRetryUS=123;
    struct {unsigned size=5;} immediateBroadcast;
    struct {bool sent=true;} broadcast;
    std::map<std::string,uint64_t> properties;
    iwx_mac_ctx_cmd initialMAC={};
    void retain() {++references;}
    void release() {assert(references>1);--references;}
    void setProperty(const char *key,bool value) {properties[key]=value;}
    void setProperty(const char *key,uint64_t value,unsigned bits) {assert(bits==32 || bits==64);properties[key]=value;}
    void updateAvailability() {++availability;}
    void serviceUnicast() {++unicast;}
    void drain() {++drains;}
    IOReturn setProperties(OSObject *);
    void publishReceiveFilter();
    IOReturn setCompareTxAntennas(bool);
    IOReturn setReceiveAllData(bool);
    IOReturn setEnabled(bool);
    void tick() {
        // TIMER ENTRY
        ++timerBodies;
        ticking=false;release();
    }
    void boot() {
        const uint8_t mac[]={2,0,0,0,0,0x81};
        assert(!radio->startAWDLRadio(mac));publishReceiveFilter();
        assert(radio->awdlRadio.mac && radio->awdlRadio.awake && radio->awdlRadio.ready);
        initialMAC=radio->commands.back().mac;
        radio->commands.clear();
    }
    IOReturn select(bool enabled,const char *key="AWDLReceiveAllData") {
        OSBoolean value(enabled);OSDictionary d;d.values[key]=&value;return setProperties(&d);
    }
};
// PRODUCTION

#if DEBUG
static void antennaComparisonControl() {
    AirportItlwmAWDLInterface i;
    i.boot();
    auto &d=i.device;
    const char *key="AWDLCompareTxAntennas";
    assert(i.properties.count(key) && !i.properties[key]);
    OSBoolean yes(true);
    OSNumber invalid;
    OSDictionary mixed;
    mixed.values[key]=&invalid;
    assert(i.setProperties(&mixed)==kIOReturnBadArgument);
    mixed.values[key]=&yes;
    mixed.values["AWDLReceiveAllData"]=&yes;
    assert(i.setProperties(&mixed)==kIOReturnBadArgument && !d.awdlCompareEndUS);
    IOUserClient::privilege=kIOReturnNotPrivileged;
    const auto before=d.gate.entries;
    assert(i.select(true,key)==kIOReturnNotPrivileged && d.gate.entries==before);
    IOUserClient::privilege=kIOReturnSuccess;
    for (uint8_t mask : {0,1,2,4}) {
        d.txMask=mask;
        assert(i.select(true,key)==kIOReturnUnsupported && !d.awdlCompareEndUS);
    }
    d.txMask=3;
    d.awdlRadio.stopPending=true;
    assert(i.select(true,key)==kIOReturnNotReady);
    d.awdlRadio.stopPending=false;
    i.regulatoryPlanBlocked=true;
    assert(i.select(true,key)==kIOReturnNotReady);
    i.regulatoryPlanBlocked=false;
    assert(i.select(true,key)==kIOReturnSuccess && !d.commands.size());
    const auto end=d.awdlCompareEndUS;
    assert(end==clockUS+30000000 && i.properties[key] && i.properties["AWDLCompareEndUptimeUS"]==end);
    clockUS+=15000000;
    assert(d.awdlCompareArm()==2 && i.select(true,key)==kIOReturnBusy && d.awdlCompareEndUS==end);
    d.awdlCompareArmSubmitted[0]=1;
    d.awdlCompareArmSubmitted[1]=2;
    d.awdlCompareBTOverrides=1;
    clockUS=end;
    i.publishReceiveFilter();
    assert(!i.properties[key] && i.properties["AWDLCompareSubmittedA"]==1 &&
        i.properties["AWDLCompareSubmittedB"]==2 && i.properties["AWDLCompareBTOverrides"]==1);
    assert(i.select(false,key)==kIOReturnSuccess && !d.awdlCompareEndUS && i.properties["AWDLCompareSubmittedB"]==2);
    assert(i.select(true,key)==kIOReturnSuccess && !i.properties["AWDLCompareSubmittedB"]);
    d.loop.gated=true;
    assert(!d.stopAWDLRadio() && !d.awdlCompareEndUS);
    i.boot();
    assert(i.select(true,key)==kIOReturnSuccess);
    d.awdlCompareArmSubmitted[1]=2;
    d.resetAWDLRadio();
    assert(!d.awdlCompareEndUS && !d.awdlCompareArmSubmitted[1]);
}

static void authorization() {
    AirportItlwmAWDLInterface i;i.boot();
    OSBoolean yes(true);OSNumber number;OSDictionary d;
    assert(i.setProperties(&number)==99 && i.delegated==1);
    d.values["Unrelated"]=&yes;
    assert(i.setProperties(&d)==99 && i.delegated==2);
    d.values["AWDLReceiveAllData"]=&yes;
    assert(i.setProperties(&d)==kIOReturnBadArgument);
    d.values.erase("Unrelated");d.values["AWDLReceiveAllData"]=&number;
    assert(i.setProperties(&d)==kIOReturnBadArgument);
    IOUserClient::privilege=kIOReturnNotPrivileged;
    const unsigned before=i.device.gate.entries;
    assert(i.select(true)==kIOReturnNotPrivileged && i.device.gate.entries==before);
    IOUserClient::privilege=kIOReturnSuccess;
    assert(i.device.commands.empty() && !i.device.awdlReceiveAllData);
    assert(i.properties["AWDLReceiveAllDataSupported"] && i.properties["AWDLReceiveFilterKnown"]);
}
static void preconditions() {
    for(unsigned kind=0;kind<10;++kind) {
        AirportItlwmAWDLInterface i;i.boot();auto &r=i.device.awdlRadio;
        switch(kind) {
        case 0:i.stopping=true;break;
        case 1:i.requested=false;break;
        case 2:i.running=false;break;
        case 3:i.interfaceEnabled=false;break;
        case 4:r.ready=false;break;
        case 5:r.fault=true;break;
        case 6:r.mac=false;break;
        case 7:r.awake=false;break;
        case 8:r.changing=true;break;
        case 9:i.ticking=true;break;
        }
        assert(i.select(true)==(kind>=8?kIOReturnBusy:kIOReturnNotReady));
        assert(i.device.commands.empty() && !i.drains && !i.receiveFilterChanging);
    }
    AirportItlwmAWDLInterface i;i.boot();i.receiveFilterChanging=true;
    assert(i.select(true)==kIOReturnBusy && i.device.commands.empty());
}
static void toggle() {
    AirportItlwmAWDLInterface i;i.boot();
    i.device.duringCommand=[&](uint32_t action) {
        assert(action==IWX_FW_CTXT_ACTION_MODIFY && i.receiveFilterChanging && i.device.awdlRadio.changing);
        assert(!i.properties["AWDLReceiveFilterKnown"]);
        i.tick();assert(!i.timerBodies && i.scheduler.delay==5);
        assert(i.select(false)==kIOReturnBusy);
    };
    assert(i.select(false)==kIOReturnSuccess && i.device.commands.empty());
    assert(i.select(true)==kIOReturnSuccess);
    assert(i.select(false)==kIOReturnSuccess);
    assert(i.device.commands.size()==2 && !i.drains && !i.device.finishCalls && !i.device.powerCalls);
    const auto on=i.device.commands[0].mac;auto off=i.device.commands[1].mac;
    assert(on.action==IWX_FW_CTXT_ACTION_MODIFY && off.action==on.action);
    auto expected=i.initialMAC;expected.action=IWX_FW_CTXT_ACTION_MODIFY;
    assert(!memcmp(&expected,&off,sizeof(off)));
    assert((on.filter_flags^off.filter_flags)==IWX_MAC_FILTER_IN_PROMISC);
    assert(on.filter_flags&IWX_MAC_FILTER_IN_PROMISC);
    off.filter_flags=on.filter_flags;assert(!memcmp(&on,&off,sizeof(on)));
    assert(on.id_and_color==IWX_FW_CMD_ID_AND_COLOR(1,0));
    assert(on.mac_type==IWX_FW_MAC_TYPE_P2P_DEVICE && on.tsf_id==IWX_TSF_ID_B);
    assert(!i.device.awdlReceiveAllData && i.device.awdlReceiveFilterKnown);
    assert(i.requested && i.running && i.interfaceEnabled && i.device.awdlRadio.ready);
    assert(i.references==1 && !i.receiveFilterChanging && !i.device.awdlRadio.changing && i.scheduler.delay==1);
    i.tick();assert(i.timerBodies==1);
    assert(!i.properties["AWDLReceiveFilterCommandError"] && !i.properties["AWDLReceiveFilterCleanupResult"]);
}
static void commandFailure() {
    for(bool brokenCleanup:{false,true}) {
        AirportItlwmAWDLInterface i;i.boot();assert(i.select(true)==kIOReturnSuccess);
        i.device.commands.clear();i.device.modifyError=ETIMEDOUT;
        if(brokenCleanup)i.device.finishError=EIO;
        assert(i.select(false)==kIOReturnIOError);
        assert(i.device.awdlReceiveAllData && !i.device.awdlReceiveFilterKnown);
        assert(!i.requested && !i.running && i.drains==1 && i.device.finishCalls==1);
        assert(i.properties["AWDLReceiveFilterCommandError"]==ETIMEDOUT);
        assert(i.properties["AWDLReceiveFilterCleanupResult"]==unsigned(brokenCleanup?kIOReturnIOError:kIOReturnSuccess));
        assert(!i.device.awdlRadio.failure.stage); // MODIFY must not become an AddMAC failure.
        assert(i.device.awdlRadio.mac==brokenCleanup && i.device.awdlRadio.fault==brokenCleanup);
        assert(i.select(true)==kIOReturnNotReady);
        if(!brokenCleanup) {
            i.device.modifyError=0;i.device.duringCommand={};
            const uint8_t mac[]={2,0,0,0,0,0x81};assert(!i.device.startAWDLRadio(mac));
            const auto &add=i.device.commands.back().mac;
            assert(add.action==IWX_FW_CTXT_ACTION_ADD && (add.filter_flags&IWX_MAC_FILTER_IN_PROMISC));
            assert(i.device.awdlReceiveFilterKnown);
        }
    }
}
static void interleaving() {
    for(unsigned scenario=0;scenario<4;++scenario) {
        AirportItlwmAWDLInterface i;i.boot();
        bool fired=false;
        i.device.duringCommand=[&](uint32_t action) {
            if(fired || action!=IWX_FW_CTXT_ACTION_MODIFY)return;
            fired=true;
            if(scenario==0) assert(i.setEnabled(false)==kIOReturnIOError);
            if(scenario==1) {
                i.stopping=true;i.requested=false;i.timer=nullptr;
                assert(i.device.stopAWDLRadio()==EBUSY);
            }
            if(scenario==2) {++i.device.dmaEpoch;i.device.resetAWDLRadio();}
            if(scenario==3) {i.device.modifyError=EIO;i.stopping=true;i.requested=false;i.timer=nullptr;}
        };
        auto result=i.select(true);
        assert(fired && i.references==1 && !i.receiveFilterChanging);
        if(scenario==2) {
            assert(result==kIOReturnNotReady && !i.device.awdlReceiveAllData && !i.device.awdlReceiveFilterKnown);
            assert(!i.device.awdlReceiveFilterCommandError && !i.device.finishCalls);
        } else {
            assert(result==(scenario==3?kIOReturnIOError:kIOReturnSuccess));
            assert(!i.requested && !i.device.awdlRadio.mac && !i.device.awdlRadio.ready);
            assert(i.device.finishCalls==1 && !i.device.awdlRadio.changing);
        }
    }
    AirportItlwmAWDLInterface i;i.boot();i.device.modifyError=EIO;
    i.device.duringCommand=[&](uint32_t action) {
        if(action==IWX_FW_CTXT_ACTION_REMOVE) {++i.device.dmaEpoch;i.device.resetAWDLRadio();}
    };
    assert(i.select(true)==kIOReturnNotReady);
    assert(!i.device.awdlReceiveFilterCommandError && !i.device.awdlReceiveFilterCleanupResult);
    assert(!i.device.awdlReceiveAllData && !i.device.awdlRadio.fault);
}
static void pendingCleanup() {
    for(unsigned scenario=0;scenario<3;++scenario) {
        AirportItlwmAWDLInterface i;i.boot();auto &r=i.device.awdlRadio;
        r.dwell.requested=r.dwell.running=true;i.device.deferFinish=true;
        if(scenario<2)i.device.modifyError=ETIMEDOUT;
        else i.device.duringCommand=[&](uint32_t action) {
            if(action==IWX_FW_CTXT_ACTION_MODIFY)assert(i.setEnabled(false)==kIOReturnIOError);
        };
        assert(i.select(true)==(scenario<2?kIOReturnIOError:kIOReturnSuccess));
        assert(r.stopPending && !r.ready && !i.requested && !r.fault);
        assert(i.device.awdlReceiveFilterCleanupResult==kIOReturnBusy);
        assert(i.properties["AWDLReceiveFilterCleanupResult"]==kIOReturnBusy);
        const unsigned commands=i.device.commands.size();
        i.tick();assert(r.stopPending && i.device.commands.size()==commands);
        assert(i.device.awdlReceiveFilterCleanupResult==kIOReturnBusy);
        if(scenario==1)i.device.finishError=EIO;
        r.dwell.done=1;i.tick();
        assert(!r.stopPending && !r.ready && !i.requested);
        assert(i.device.awdlReceiveFilterCleanupResult==(scenario==1?kIOReturnIOError:kIOReturnSuccess));
        assert(i.properties["AWDLReceiveFilterCleanupResult"]==unsigned(scenario==1?kIOReturnIOError:kIOReturnSuccess));
        const unsigned completeCommands=i.device.commands.size();
        const unsigned finishes=i.device.finishCalls;i.tick();i.tick();
        assert(i.device.commands.size()==completeCommands && i.device.finishCalls==finishes);
        assert(r.fault==(scenario==1));
    }
    AirportItlwmAWDLInterface i;i.boot();i.device.deferFinish=true;
    i.device.awdlRadio.dwell.requested=true;i.device.modifyError=EIO;
    i.select(true);i.stopping=true;i.device.awdlRadio.dwell.done=1;
    const unsigned before=i.device.finishCalls;i.tick();assert(i.device.finishCalls==before);
    assert(i.device.awdlRadio.stopPending && i.device.awdlReceiveFilterCleanupResult==kIOReturnBusy);
}
#else
static void releaseDefaults() {
    AirportItlwmAWDLInterface i;i.boot();
    assert(!i.properties["AWDLReceiveAllDataSupported"] && !i.properties["AWDLReceiveAllData"]);
    assert(i.select(true)==kIOReturnUnsupported && i.device.commands.empty());
    assert(i.select(true,"AWDLCompareTxAntennas")==kIOReturnUnsupported && !i.delegated);
    assert(!i.properties.count("AWDLCompareTxAntennas") && !i.properties.count("AWDLCompareSubmittedA"));
    assert(!IOUserClient::checks);
    i.device.loop.gated=true;
    assert(!i.device.awdlMAC(IWX_FW_CTXT_ACTION_MODIFY,true));
    assert(!(i.device.commands.back().mac.filter_flags&IWX_MAC_FILTER_IN_PROMISC));
    i.device.loop.gated=false;
    OSDictionary other;OSBoolean yes(true);other.values["Unrelated"]=&yes;
    assert(i.setProperties(&other)==99 && i.delegated==1);
}
#endif
static void quz0034ReceiveData() {
    AirportItlwmAWDLInterface board, other;
    board.device.awdlRadio.quz0034 = true;
    board.boot();
    other.boot();
    auto expected = other.initialMAC;
    expected.filter_flags |= IWX_MAC_FILTER_IN_PROMISC;
    assert(!memcmp(&board.initialMAC, &expected, sizeof(expected)));
    assert(board.properties["AWDLReceiveAllData"] && board.properties["AWDLReceiveFilterKnown"]);
    assert(!(other.initialMAC.filter_flags & IWX_MAC_FILTER_IN_PROMISC));
#if DEBUG
    assert(board.select(false) == kIOReturnSuccess);
    assert(!(board.device.commands.back().mac.filter_flags & IWX_MAC_FILTER_IN_PROMISC));
#else
    assert(board.select(false) == kIOReturnUnsupported && board.device.commands.empty());
#endif
    board.device.resetAWDLRadio();
    board.boot();
    assert(board.initialMAC.filter_flags & IWX_MAC_FILTER_IN_PROMISC);
}
static void listenerFilterAutomatic() {
    AirportItlwmAWDLInterface i;i.device.awdlRadio.quz0034=true;i.boot();
    i.device.awdlRadio.listener=true;
    i.device.awdlRadio.listenerFilterKnown=i.device.awdlReceiveFilterKnown=true;
    for(bool promisc:{false,true}) {
        i.device.awdlRadio.listenerPromisc=i.device.awdlReceiveAllData=promisc;
        i.publishReceiveFilter();
        assert(!i.properties["AWDLReceiveAllDataSupported"]);
        assert(i.properties["AWDLReceiveAllData"]==promisc && i.properties["AWDLReceiveFilterKnown"]);
        assert(i.select(!promisc)==kIOReturnUnsupported && i.device.commands.empty());
    }
}
static void listenerManagementDuringBA() {
    for(bool receivingBA:{true,false}) for(bool sharedChannel:{true,false}) {
        AirportItlwmAWDLInterface i;i.boot();
        i.device.awdlRadio.listener=true;i.device.awdlRadio.channel=6;
        i.device.receiveBA=receivingBA;
        i.device.ap.phy.channel=&i.device.com.sc_ic.ic_channels[sharedChannel?6:48];
        i.device.loop.gated=true;
        assert(!i.device.awdlMAC(IWX_FW_CTXT_ACTION_MODIFY));
        const auto flags=i.device.commands.back().mac.filter_flags;
        assert(flags&IWX_MAC_FILTER_IN_CONTROL_AND_MGMT);
        assert(flags&IWX_MAC_FILTER_ACCEPT_GRP);
        assert(bool(flags&IWX_MAC_FILTER_IN_PROMISC)==(!receivingBA&&!sharedChannel));
        assert(i.device.awdlRadio.listenerFilterFlags==flags);
    }
}

static uint64_t probeWindow(const void *p,uint64_t now) {
    const auto end=*static_cast<const uint64_t *>(p);return end>now?end:0;
}
static void prepareReceiveProbe(AirportItlwmAWDLInterface &i) {
    clockUS=1000000;i.boot();auto &d=i.device;auto &r=d.awdlRadio;
    d.loop.gated=true;d.receiveBA=true;r.listener=true;r.channel=149;
    d.ap.phy.channel=&d.com.sc_ic.ic_channels[48];
    auto &tx=r.blockAckTX;tx.configured=true;tx.epoch=d.dmaEpoch;tx.cookie=9;
    tx.phase=AirportAWDL::TXBlockAckQueue::Asking;tx.attempts=0;tx.nextAskUS=clockUS;
    assert(!d.awdlMAC(IWX_FW_CTXT_ACTION_MODIFY));assert(!r.listenerPromisc);
    d.commands.clear();
}
static void receiveProbeLifecycle() {
    AirportItlwmAWDLInterface i;prepareReceiveProbe(i);auto &d=i.device;auto &r=d.awdlRadio;
    auto &tx=r.blockAckTX;uint64_t end=clockUS+400000;
    // Active inbound RX BA traffic (20 ms Sidecar cadence) must NOT block the ADDBA probe on Attempt #1.
    r.lastRXPayloadUS=clockUS-20000;
    assert(!d.serviceAWDLReceiveFilter(probeWindow,&end));
    assert(tx.receiveProbeUntilUS==clockUS+60000 && r.listenerPromisc);
    assert(r.blockAckTXStats.receiveProbes==1 && d.commands.size()==1);
    // Simulate ADDBA Request in flight + inbound RX BA packets arriving during the 60 ms probe window.
    tx.managementToken=1;tx.attempts=1;tx.lastAskUS=clockUS;tx.nextAskUS=clockUS+200000;
    clockUS+=10000;r.lastRXPayloadUS=clockUS;
    assert(!d.serviceAWDLReceiveFilter(probeWindow,&end) && r.listenerPromisc && d.commands.size()==1);
    tx.managementToken=0;
    clockUS=tx.receiveProbeUntilUS-1;
    assert(!d.serviceAWDLReceiveFilter(probeWindow,&end) && r.listenerPromisc && d.commands.size()==1);
    // Once the 60 ms probe window expires, IN_PROMISC turns back off until nextAskUS.
    ++clockUS;
    assert(!d.serviceAWDLReceiveFilter(probeWindow,&end) && !r.listenerPromisc && d.commands.size()==2);
    assert(!d.serviceAWDLReceiveFilter(probeWindow,&end) && d.commands.size()==2);
    // At nextAskUS, Attempt #2 opens another short bounded probe (bounded by EAW end if shorter than 60 ms).
    clockUS=tx.nextAskUS;end=clockUS+35000;r.lastRXPayloadUS=clockUS-5000;
    assert(!d.serviceAWDLReceiveFilter(probeWindow,&end) && r.listenerPromisc && r.blockAckTXStats.receiveProbes==2);
    assert(tx.receiveProbeUntilUS==end);
    // Immediate ADDBA Response transitions to Enabling and closes IN_PROMISC immediately (~3 ms).
    clockUS+=3000;tx.receiveProbeUntilUS=0;tx.phase=AirportAWDL::TXBlockAckQueue::Enabling;
    assert(!d.serviceAWDLReceiveFilter(probeWindow,&end) && !r.listenerPromisc && !tx.receiveProbeUntilUS);
    d.receiveBA=false;
    assert(!d.serviceAWDLReceiveFilter(probeWindow,&end) && r.listenerPromisc);
}
static void receiveProbeAdmission() {
    for(unsigned condition=0;condition<9;++condition) {
        AirportItlwmAWDLInterface i;prepareReceiveProbe(i);auto &d=i.device;auto &r=d.awdlRadio;
        auto &tx=r.blockAckTX;uint64_t end=clockUS+100000;
        switch(condition) {
        case 0:tx.managementToken=77;break;
        case 1:tx.attempts=AirportAWDL::TXBlockAckQueue::MaxAttempts;break;
        case 2:tx.nextAskUS=clockUS+1;break;
        case 3:tx.phase=AirportAWDL::TXBlockAckQueue::Agreed;break;
        case 4:tx.attempts=0;tx.nextAskUS=clockUS+20000000;break;
        case 5:tx.epoch=d.dmaEpoch-1;break;
        case 6:end=clockUS;break;
        case 7:d.ap.phy.channel=&d.com.sc_ic.ic_channels[149];break;
        case 8:tx.configured=false;break;
        }
        assert(!d.serviceAWDLReceiveFilter(probeWindow,&end));
        assert(!r.listenerPromisc && !r.blockAckTXStats.receiveProbes && d.commands.empty());
    }
}
static void receiveProbeCommandLifetime() {
    for(unsigned scenario=0;scenario<5;++scenario) {
        AirportItlwmAWDLInterface i;prepareReceiveProbe(i);auto &d=i.device;auto &r=d.awdlRadio;
        uint64_t end=clockUS+100000;bool fired=false;
        d.duringCommand=[&](uint32_t action) {
            if(fired || action!=IWX_FW_CTXT_ACTION_MODIFY)return;fired=true;
            assert(r.changing);
            if(scenario==0)clockUS=r.blockAckTX.receiveProbeUntilUS;
            if(scenario==1)r.blockAckTX.phase=AirportAWDL::TXBlockAckQueue::Enabling;
            if(scenario==2) {++d.dmaEpoch;d.resetAWDLRadio();}
            if(scenario==3) {assert(d.stopAWDLRadio()==EBUSY);}
            if(scenario==4)d.modifyError=ETIMEDOUT;
        };
        const int error=d.serviceAWDLReceiveFilter(probeWindow,&end);
        assert(fired);
        if(scenario<2) assert(!error && d.commands.size()==2 && !r.listenerPromisc && r.ready);
        else if(scenario==2) assert(error==ENXIO && !r.ready && !r.listenerFilterKnown && !r.lastRXPayloadUS);
        else if(scenario==3) assert(!error && !r.ready && !r.mac && !r.stopPending);
        else assert(error==ETIMEDOUT && !r.ready && r.fault && !r.mac);
    }
}

int main() {
    // TEST ENTRY
}
