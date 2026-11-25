#include "AirportItlwm/AWDL/BlockAck.hpp"
#include <cassert>
#include <algorithm>
#define MIN(a,b) std::min((a),(b))
#include <cstdio>
#include <map>
#include <string>
#include <sys/time.h>
#include "AirportItlwm/AWDL/DataFrame.hpp"
#include "itlwm/hal_iwx/ReceiveClock.hpp"

struct OSObject {
    int refs=1;
    virtual ~OSObject()=default;
    void retain() { ++refs; }
    void release() { if(--refs==0)delete this; }
};
struct OSNumber : OSObject {
    uint64_t value;
    static OSNumber *withNumber(uint64_t v,unsigned) { auto p=new OSNumber;p->value=v;return p; }
};
struct OSString : OSObject {
    std::string value;
    static OSString *withCString(const char *v) { auto p=new OSString;p->value=v;return p; }
};
struct OSDictionary : OSObject {
    std::map<std::string,OSObject *> values;
    static OSDictionary *withCapacity(unsigned) { return new OSDictionary; }
    bool setObject(const char *key,OSObject *p) {
        p->retain();if(values.count(key))values[key]->release();values[key]=p;return true;
    }
    ~OSDictionary() { for(auto &p:values)p.second->release(); }
};
struct Controller {
    OSDictionary *snapshot=nullptr;
    bool fail=false;
    unsigned publications=0;
    bool setProperty(const char *key,OSDictionary *p) {
        assert(std::string(key)=="AWDLReceive");
        if(fail)return false;
        ++publications;
        p->retain();if(snapshot)snapshot->release();snapshot=p;return true;
    }
    ~Controller() { if(snapshot)snapshot->release(); }
    uint64_t number(const char *key) { return dynamic_cast<OSNumber *>(snapshot->values.at(key))->value; }
    std::string string(const char *key) { return dynamic_cast<OSString *>(snapshot->values.at(key))->value; }
};
static unsigned uptime=42;
static void getmicrouptime(timeval *t) { t->tv_sec=uptime;t->tv_usec=0; }
// RX DIAGNOSTIC DECLARATIONS
// FIRMWARE DECLARATIONS
struct ItlIwx {
    AirportAWDL::BlockAckState awdlBA={};
    struct {
        uint64_t llwWindowContinuations = 0;
        bool llwRealtime = false, heavyRX = false, heavyTX = false;
        bool heavyReceiveActive(uint64_t) const { return heavyRX; }
        bool heavyTransmitActive(uint64_t) const { return heavyTX; }
    } awdlRadio;
    AirportAWDL::ReceiveState awdlReceive={};
    AirportAWDL::PeerTable awdlPeers={};
    AirportAWDL::DataReceiveState awdlData={};
    uint64_t awdlNonAPDataDropped=0;
    IwxReceiveClock awdlReceiveClock={};
    uint64_t awdlPublishedSecond=~uint64_t(0);
    Controller controller;
    Controller *getController() { return &controller; }
    IwxAWDLReceiveDiagnostics awdlReceiveDiagnostics = {};
    void formatAWDLReceiveDiagnostics(char *,size_t) const;
    void publishAWDLReceive();
};

// PRODUCTION

int main() {
    ItlIwx driver;
    driver.publishAWDLReceive();
    assert(driver.controller.number("ValidActionFrames")==0);
    assert(driver.controller.string("Mode")=="passive");
    assert(!driver.controller.snapshot->values.count("LastRXChannel"));
    const char *diagnosticKeys[]={"RXDescriptorSampled","RXDescriptorCRCOK","RXDescriptorCRCBad",
        "RXDescriptorOverrunOK","RXDescriptorOverrunBad","RXDescriptorSourceFound","RXDescriptorSourceMissing"};
    for(auto key:diagnosticKeys) assert(driver.controller.number(key)==0);
    assert(driver.controller.string("RXDescriptorSummary").find("ACK=unknown")!=std::string::npos);
    assert(driver.controller.string("RXDescriptorSummary").find('#')==std::string::npos);
    auto &diagnostics=driver.awdlReceiveDiagnostics;
    diagnostics.sampled=diagnostics.crcOK=diagnostics.overrunOK=diagnostics.sourceFound=1;
    auto &first=diagnostics.samples[0];
    first.ordinal=1;first.uptimeUS=42000000;first.epoch=7;first.status=0x1b000007;
    first.macPhy=0xa5;first.macContext=2;first.channel=6;first.fc0=0x88;
    first.sequenceControl=0x1230;first.tid=13;
    first.receiver[0]=first.transmitter[0]=2;first.receiver[5]=1;first.transmitter[5]=2;
    driver.awdlReceive.valid=1;driver.awdlReceive.psf=1;
    driver.awdlReceive.lastChannel=44;driver.awdlReceive.lastRSSI=-48;
    driver.awdlReceive.last.fields=AirportAWDL::Version;
    driver.awdlReceive.last.version=0x31;
    driver.publishAWDLReceive(); // Same second as attach: first frame stays visible.
    assert(driver.controller.number("ValidActionFrames")==1);
    assert(driver.controller.number("LastRXChannel")==44);
    assert(driver.controller.number("LastPeerVersion")==0x31);
    assert(driver.controller.string("LastRSSI")=="-48 dBm");
    assert(driver.controller.number("RXDescriptorSampled")==1);
    assert(driver.controller.number("RXDescriptorCRCOK")==1 && driver.controller.number("RXDescriptorSourceFound")==1);
    const auto firstSummary=driver.controller.string("RXDescriptorSummary");
    assert(firstSummary.find("#1 t=42000000 epoch=7 status=1b000007 found=1 sta=27")!=std::string::npos);
    assert(firstSummary.find("macphy=a5 macmask=02 ch=6 fc=8800 seq=291 frag=0 tid=13")!=std::string::npos);
    assert(firstSummary.find("ra=02:00:00:00:00:01 ta=02:00:00:00:00:02")!=std::string::npos);
    diagnostics.sampled=2;diagnostics.crcBad=diagnostics.overrunBad=diagnostics.sourceMissing=1;
    diagnostics.samples[1]=first;
    diagnostics.samples[1].ordinal=2;diagnostics.samples[1].status=0x03000000;
    driver.awdlReceive.valid=2;driver.awdlReceive.mif=1;driver.awdlReceive.otherDestination=1;
    driver.awdlNonAPDataDropped=9;
    driver.awdlReceiveClock={2,3,4,500,1500};
    driver.publishAWDLReceive();
    assert(driver.controller.number("ValidActionFrames")==1);
    assert(driver.controller.number("RXDescriptorSampled")==1);
    assert(driver.controller.string("RXDescriptorSummary")==firstSummary);
    ++uptime;
    driver.controller.fail=true;driver.publishAWDLReceive();
    assert(driver.awdlPublishedSecond==42);
    assert(driver.controller.number("RXDescriptorSampled")==1);
    driver.controller.fail=false;driver.publishAWDLReceive();
    assert(driver.controller.number("ValidActionFrames")==2);
    assert(driver.controller.number("RXDescriptorSampled")==2);
    for(unsigned i=1;i<7;++i) assert(driver.controller.number(diagnosticKeys[i])==1);
    const auto refreshedSummary=driver.controller.string("RXDescriptorSummary");
    assert(refreshedSummary.find("#2 t=42000000 epoch=7 status=03000000 found=0 sta=3")!=std::string::npos);
    assert(refreshedSummary.find("bad CRC/overflow headers=unverified")!=std::string::npos);
    assert(driver.controller.number("ValidDataFrames")==0);
    assert(driver.controller.number("ActionFramesForOtherDestinations")==1);
    assert(driver.controller.number("NonAPDataDropped")==9);
    assert(driver.controller.number("SnapshotUptimeSeconds")==43);
    assert(driver.controller.number("PSF")==1 && driver.controller.number("MIF")==1);
    assert(driver.controller.number("HardwareTimedActionFrames")==2);
    assert(driver.controller.number("RXClockUnavailable")==3);
    assert(driver.controller.number("RXClockRejectedSamples")==4);
    assert(driver.controller.number("LastRXProcessingDelayUS")==500);
    assert(driver.controller.number("MaxRXProcessingDelayUS")==1500);
    assert(driver.controller.number("ObservedPeers")==0);
    uint8_t remote[6]={2,0,0,0,0,2},self[6]={2,0,0,0,0,1};
    AirportAWDL::Advertisement a={};
    assert(driver.awdlPeers.update(remote,self,a,43000000,44,-48));
    driver.awdlPeers.find(remote,43000000)->nativeCached=true;
    driver.awdlRadio.llwWindowContinuations = 3;
    driver.awdlBA.llwGapExpiries = 4;
    driver.awdlBA.llwBelowWindow = 5;
    driver.awdlData.valid=1;driver.awdlData.subframes=2;driver.awdlData.observed=1;driver.awdlData.duplicate=1;
    ++uptime;driver.publishAWDLReceive();
    assert(driver.controller.number("ObservedPeers")==1);
    assert(driver.controller.number("NativeCachedPeers")==1);
    assert(driver.controller.number("PeersWithSchedule")==0);
    assert(driver.controller.number("ValidDataFrames")==1);
    assert(driver.controller.number("DataSubframes")==2);
    assert(driver.controller.number("LLWWindowContinuations") == 3);
    assert(driver.controller.number("LLWReorderTimeoutReleases") == 4);
    assert(driver.controller.number("LLWReorderBelowWindow") == 5);
    assert(driver.controller.number("ObservedDataSubframes")==1);
    uptime=53;driver.publishAWDLReceive();
    assert(driver.controller.number("ObservedPeers")==0 && driver.controller.number("ExpiredPeers")==1);
    assert(driver.controller.number("NativeCachedPeers")==1);
    driver.awdlPeers.clearCache();++uptime;driver.publishAWDLReceive();
    assert(driver.controller.number("NativeCachedPeers")==0);
    ItlIwx dataOnly;
    dataOnly.publishAWDLReceive();
    dataOnly.awdlData.malformed=1;dataOnly.publishAWDLReceive();
    assert(dataOnly.controller.number("MalformedDataFrames")==1);
    ItlIwx foreignOnly;
    foreignOnly.publishAWDLReceive();
    foreignOnly.awdlNonAPDataDropped=1;foreignOnly.publishAWDLReceive();
    assert(foreignOnly.controller.number("NonAPDataDropped")==1);
    assert(foreignOnly.controller.publications==2);
    for(unsigned i=0;i<100;++i) {
        ++foreignOnly.awdlNonAPDataDropped;foreignOnly.publishAWDLReceive();
    }
    assert(foreignOnly.controller.number("NonAPDataDropped")==1);
    assert(foreignOnly.controller.publications==2);
    ++uptime;foreignOnly.publishAWDLReceive();
    assert(foreignOnly.controller.number("NonAPDataDropped")==101);
    assert(foreignOnly.controller.publications==3);
}
