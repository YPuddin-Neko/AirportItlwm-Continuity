#include <cassert>
#include <cstring>
#include <vector>
#include "AirportItlwm/AWDL/SyncEngine.hpp"
using namespace AirportAWDL;
static const uint8_t self[6]={2,0,0,0,0,1}, remote[6]={2,0,0,0,0,2};

static void wire() {
    SyncEngine engine;engine.start(self,149,1000000);
    std::vector<uint8_t> output(1600,0xaa);
    const uint8_t arpa[]={16,8,0,3,4,'t','e','s','t',0xc0,0x0c};
    size_t size=engine.action(output.data()+1,1536,1000000,true,arpa,sizeof(arpa));
    assert(size==273 && output[0]==0xaa && output[size+1]==0xaa);
    auto p=output.data()+1;
    assert(p[0]==0xd0 && p[1]==0 && p[4]==0xff && sameAddress(p+10,self));
    assert(sameAddress(p+16,BSSID));
    const uint8_t header[]={127,0,0x17,0xf2,8,0x10,3,0,0x40,0x42,0x0f,0,0x40,0x42,0x0f,0};
    assert(!memcmp(p+24,header,sizeof(header)));
    assert(p[40]==4 && le16(p+41)==73);
    assert(le16(p+44)==64 && le16(p+48)==16);
    Advertisement a={};assert(parseMPDU(p,size,a)==ParseResult::Valid);
    assert(a.subtype==3 && a.fields==(Sync|Channels|ElectionV1|ElectionV2|Version));
    assert(a.sequence.count==16 && a.sequence.encoding==3 && a.sequence.channel[15]==149);
    assert(a.sequence.flags[15]==124 && a.nextWindowTU==64 && a.windowCounter==0);
    assert(a.masterMetric==100 && a.selfMetric==100 && a.distance==0);
    assert(sameAddress(a.electionMaster,self) && sameAddress(a.electionSync,self));
    for(unsigned n=0;n<size;++n) {
        std::vector<uint8_t> shortOut(n,0xa5);
        auto old=shortOut;
        assert(!engine.action(shortOut.data(),n,1000000,true,arpa,sizeof(arpa)));
        assert(shortOut==old);
    }
    auto old=output;const uint8_t bad[]={16,0xff,0xff};
    assert(!engine.action(output.data(),output.size(),1000000,true,bad,sizeof(bad)));
    assert(output==old);
    engine.wifiSequence=4095;
    size=engine.action(p,1536,1065536,false,nullptr,0);
    assert(parseMPDU(p,size,a)==ParseResult::Valid && a.windowCounter==4 && a.nextWindowTU==64);
    assert(le16(p+22)==0xfff0);
    engine.action(p,1536,1065537,false,nullptr,0);assert(le16(p+22)==0);
}

static Advertisement advert(uint32_t counter,uint32_t metric) {
    SyncEngine sender;sender.start(remote,149,1000000);sender.counter=sender.masterCounter=counter;
    sender.metric=sender.masterMetric=metric;
    uint8_t bytes[512];size_t size=sender.action(bytes,sizeof(bytes),1000000,true,nullptr,0);
    Advertisement a={};assert(parseMPDU(bytes,size,a)==ParseResult::Valid);return a;
}

static void election() {
    SyncEngine local;local.start(self,149,1000000);
    PeerTable peers={};auto a=advert(2,500);
    assert(peers.update(remote,self,a,1001000,149,-45));
    local.elect(peers,1010000);
    assert(sameAddress(local.master,remote) && sameAddress(local.sync,remote));
    assert(local.distance==1 && local.masterCounter==2 && local.masterMetric==500);
    Window w;assert(local.window(1010000,w) && w.counter==0 && w.untilNextEAWUS==56536);
    local.elect(peers,4001000); // timing becomes stale even before peer eviction
    assert(sameAddress(local.master,self) && local.distance==0);
    a.phyTxTime++;copyAddress(a.electionSync,self);
    peers.update(remote,self,a,4001000,149,-45);local.elect(peers,4001001);
    assert(sameAddress(local.master,self)); // no two-node cycle
    a.phyTxTime++;copyAddress(a.electionSync,remote);a.distance=10;
    peers.update(remote,self,a,4002000,149,-45);local.elect(peers,4002001);
    assert(sameAddress(local.master,self));
    local.elect(peers,100); // a new local clock epoch preserves our MAC
    assert(sameAddress(local.address,self) && local.epochUS==100);
    local.elect(peers,6280100);assert(local.counter==2);
}

static void scheduling() {
    SyncEngine local;local.start(self,149,1000000);
    PeerTable peers={};auto a=advert(1,500);
    peers.update(remote,self,a,1000000,149,-45);
    const Peer &peer=*peers.find(remote,1000000);
    assert(!local.canSendTo(peer,1000000,false));
    assert(local.canSendTo(peer,1003000,false));
    assert(!local.canSendTo(peer,1064000,false));
    assert(local.canSendTo(peer,1003000,true));
    assert(!local.canSendTo(peer,1000000+3*65536+3000,true));
    assert(!local.canSendTo(peer,1000000+4*65536+3000,true));
    for(auto &c:local.channels)c=6;
    assert(!local.canSendTo(peer,1003000,false));
    for(auto &c:local.channels)c=149;
    assert(!local.canSendTo(peer,4000000,false));
    auto shifted=peer;shifted.syncReceivedUS-=10000;
    assert(!local.canSendTo(shifted,1000000,false)); // local switch guard despite remote mid-window
}
static void nativeMulticastSlots() {
    SyncEngine local;local.start(self,149,1000000);
    PeerTable peers={};auto a=advert(1,500);
    assert(peers.update(remote,self,a,1000000,149,-45));
    auto &peer=*peers.find(remote,1000000);
    // 24H32 initParamsToDefaults: social multicast slots at ivars+0x2f7 are 0,8,10.
    for(unsigned slot=0;slot<16;++slot) {
        const uint64_t now=1000000+slot*65536+4000;
        assert(local.canSendTo(peer,now,false));
        assert(local.canSendTo(peer,now,true)==(slot==0||slot==8||slot==10));
    }
    for(auto &channel:peer.sequence.channel)channel=0;
    peer.sequence.channel[2]=peer.sequence.channel[10]=149;
    assert(!local.canSendTo(peer,1004000,true)); // absent peer in our multicast slot
    assert(!local.canSendTo(peer,1000000+2*65536+4000,true));
    assert(local.canSendTo(peer,1000000+10*65536+4000,true));
    local.epochCounter=peer.sync.windowCounter=65532;
    for(auto &channel:peer.sequence.channel)channel=149;
    assert(!local.canSendTo(peer,1004000,true)); // slot 15 before AW counter wrap
    assert(local.canSendTo(peer,1000000+65536+4000,true)); // slot 0 after wrap
    assert(!local.canSendTo(peer,1000000+65536+2999,true));
    assert(!local.canSendTo(peer,1000000+2*65536-2999,true));
    local.start(self,48,1000000);local.discoverySchedule(true,true,true);
    assert(local.channels[0]==149 && local.channels[8]==6 && local.channels[10]==149);
    unsigned home=0;for(auto channel:local.channels)home+=!channel;
    assert(home==7); // retain the existing number of AP absence slots
}
static void discovery() {
    SyncEngine local;local.start(self,48,1000000);local.discoverySchedule(true,true,true);
    uint8_t bytes[512];unsigned home=0;
    for(unsigned slot=0;slot<16;++slot) {
        uint64_t now=1000000+slot*65536+4000;Window w={};assert(local.window(now,w));
        auto length=local.action(bytes,sizeof(bytes),now,true,nullptr,0);
        if(!w.channel) {++home;assert(!length);continue;}
        if(slot%4==3)assert(w.channel==149);
        Advertisement a={};assert(parseMPDU(bytes,length,a)==ParseResult::Valid);
        for(unsigned i=0;i<16;++i) {
            assert(a.sequence.channel[i]==local.channels[i]);
            if(!a.sequence.channel[i])assert(!a.sequence.flags[i]);
        }
        PeerTable peers={};assert(peers.update(self,remote,a,now,w.channel,-40));
        Window projected={};assert(peers.find(self,now)->window(now,projected));
        assert(projected.channel==w.channel && projected.counter==w.counter);
    }
    assert(home==7 && local.channels[8]==6 && local.channels[13]==44);
    for(unsigned mask=1;mask<8;++mask) {
        local.discoverySchedule(mask&1,mask&2,mask&4);
        for(auto c:local.channels)assert(!c||(c==6&&(mask&1))||(c==44&&(mask&2))||(c==149&&(mask&4)));
    }
    local.setMasterChannel(6);local.discoverySchedule(true,true,true);
    for(unsigned n=3;n<16;n+=4)assert(local.channels[n]==6);
    local.restart(48,2000000);local.discoverySchedule(true,true,true);
    assert(local.channel==6 && local.configuredMasterChannel==6);
    // Explicit non-social channels need their own operating classes in the wire schedule.
    const uint8_t channels[]={14,52,100,165},classes[]={82,118,121,125};
    for(unsigned n=0;n<4;++n) {
        local.setMasterChannel(channels[n]);
        for(auto &c:local.channels)c=channels[n];
        auto length=local.action(bytes,sizeof(bytes),2004000,true,nullptr,0);
        Advertisement a={};assert(parseMPDU(bytes,length,a)==ParseResult::Valid);
        assert(a.masterChannel==channels[n] && a.sequence.flags[0]==classes[n]);
    }
}
static void qualifications() {
    SyncEngine local;local.start(self,6,1000000);local.setMasterChannel(6);
    local.electionID=0x12345678;
    PeerTable peers={};auto a=advert(2,500);a.electionID=local.electionID;
    assert(peers.update(remote,self,a,1000000,149,-71));
    local.elect(peers,1004000);
    assert(sameAddress(local.master,self) && local.channel==6); // below the default edge
    ++a.phyTxTime;assert(peers.update(remote,self,a,1010000,149,-70));
    local.elect(peers,1014000);
    assert(sameAddress(local.master,remote) && local.channel==149); // inclusive edge
    local.electionRSSI=-69;local.elect(peers,1015000);
    assert(sameAddress(local.master,self) && local.channel==6); // restore our configured channel
    local.electionRSSI=-70;
    a.electionID^=0x10000;++a.phyTxTime;
    assert(peers.update(remote,self,a,1020000,149,-30));
    local.elect(peers,1024000);
    assert(sameAddress(local.master,self)); // all 32 ID bits participate
    a.electionID=local.electionID;++a.phyTxTime;
    assert(peers.update(remote,self,a,1030000,149,-30));
    local.elect(peers,1034000);assert(sameAddress(local.master,remote));
    local.elect(peers,4030000);
    assert(sameAddress(local.master,self) && local.channel==6); // stale peer
    local.restart(44,5000000);
    assert(local.channel==6 && local.electionRSSI==-70 && local.electionID==0x12345678);
}
static void nativeElection() {
    SyncEngine local;local.start(self,149,1000000);
    // Byte layout from 24H32 buildAWDLElectionParamsVer2 and buildAWDLVersionTlv.
    uint8_t input[48]={24,40,0};
    copyAddress(input+3,self);copyAddress(input+9,self);
    putLE32(input+15,71);putLE32(input+23,1200);putLE32(input+27,1200);
    putLE32(input+31,0x12345678);putLE32(input+35,0xdeadbeef);putLE32(input+39,71);
    input[43]=21;input[44]=2;input[46]=0x93;input[47]=2;
    assert(local.configureTemplate(input,sizeof(input)) && local.nativeElection);
    PeerTable peers={};auto a=advert(500,9000);a.electionID=local.electionID;
    peers.update(remote,self,a,1001000,149,-40);
    local.elect(peers,1005000);local.elect(peers,8000000);
    assert(sameAddress(local.master,self) && local.counter==71 && local.masterCounter==71);
    uint8_t bytes[512];auto size=local.action(bytes,sizeof(bytes),8004000,true,nullptr,0);
    assert(size);
    unsigned v2=0,ver=0;
    for(size_t at=40;at<size;at+=3+le16(bytes+at+1)) {
        if(bytes[at]==24) { ++v2;assert(!memcmp(bytes+at,input,43)); }
        if(bytes[at]==21) { ++ver;assert(!memcmp(bytes+at,input+43,5)); }
    }
    assert(v2==1 && ver==1);
    // Malformed trailing TLVs and duplicate owned fields must not replace any state.
    auto before=local;
    std::vector<uint8_t> bad(input,input+sizeof(input));bad.push_back(21);
    assert(!local.configureTemplate(bad.data(),bad.size()));
    assert(!memcmp(&before,&local,sizeof(local)));
    bad.assign(input,input+sizeof(input));bad.insert(bad.end(),input+43,input+48);
    assert(!local.configureTemplate(bad.data(),bad.size()));
    assert(!memcmp(&before,&local,sizeof(local)));
    assert(!local.configureTemplate(input,43));
    assert(!local.configureTemplate(input+43,5));
    assert(!memcmp(&before,&local,sizeof(local)));
    bad.assign(input,input+sizeof(input));bad.insert(bad.end(),input,input+43);
    assert(!local.configureTemplate(bad.data(),bad.size()));
    assert(!memcmp(&before,&local,sizeof(local)));
    // Applying an ID/metric ahead of the replacement template cannot mix two elections.
    local.electionID^=1;assert(!local.action(bytes,sizeof(bytes),8004000,true,nullptr,0));
    local.electionID^=1;local.metric++;
    assert(!local.action(bytes,sizeof(bytes),8004000,true,nullptr,0));
    local=before;
    // The host now selects a remote root/parent, regardless of autonomous ranking.
    copyAddress(input+3,remote);copyAddress(input+9,remote);putLE32(input+19,1);
    assert(local.configureTemplate(input,sizeof(input)));
    assert(!local.transmitReady(8004000));
    a.phyTxTime++;peers.update(remote,self,a,8000000,149,-95);
    local.elect(peers,8005000);assert(local.transmitReady(8005000));
    assert(local.masterCounter==71 && local.counter==71 && local.metric==1200);
    // This fixture's PHY timestamp advanced one microsecond beyond target TX.
    Window w={};assert(local.window(8005000,w) && w.counter==0 && w.untilNextEAWUS==60535);
    assert(local.canSendTo(*peers.find(remote,8005000),8005000,false));
    // A host counter update retains a valid clock when root, parent and ID are unchanged.
    bad.assign(input,input+sizeof(input));putLE32(bad.data()+15,72);putLE32(bad.data()+39,72);
    assert(local.configureTemplate(bad.data(),bad.size()) && local.transmitReady(8005000));
    assert(local.counter==72 && local.masterCounter==72);
    assert(local.configureTemplate(input,sizeof(input)) && local.transmitReady(8005000));
    // Expiry is checked at transmission, even when a firmware sleep delays the next tick.
    assert(!local.action(bytes,sizeof(bytes),11000000,true,nullptr,0));
    assert(!local.canSendTo(*peers.find(remote,11000000),11000000,false));
    local.elect(peers,11000000);assert(!local.nativeClockReady);
    assert(sameAddress(local.master,remote) && local.masterCounter==71);
    // Another available peer must not take over a host-selected parent's clock.
    const uint8_t other[]={2,0,0,0,0,3};a.phyTxTime++;
    peers.update(other,self,a,11001000,149,-30);local.elect(peers,11005000);
    assert(!local.transmitReady(11005000));
    // Wrong root, wrong ID and a two-node cycle cannot supply the clock.
    a.phyTxTime++;a.electionID^=1;peers.update(remote,self,a,11006000,149,-40);
    local.elect(peers,11010000);assert(!local.transmitReady(11010000));
    a.phyTxTime++;a.electionID^=1;copyAddress(a.electionMaster,other);
    peers.update(remote,self,a,11011000,149,-40);local.elect(peers,11015000);
    assert(!local.transmitReady(11015000));
    a.phyTxTime++;copyAddress(a.electionMaster,remote);copyAddress(a.electionSync,self);
    peers.update(remote,self,a,11016000,149,-40);local.elect(peers,11020000);
    assert(!local.transmitReady(11020000));
    a.phyTxTime++;copyAddress(a.electionSync,remote);
    peers.update(remote,self,a,11021000,149,-40);local.elect(peers,11025000);
    assert(local.transmitReady(11025000));
    local.restart(149,12000000);
    assert(local.nativeElection && !local.nativeClockReady && local.counter==71);
    assert(!memcmp(local.nativeElectionValue,input+3,40));
    // Clock reset preserves the host election, but invalidates the old peer timing.
    local.elect(peers,100);assert(local.epochUS==100 && !local.transmitReady(100));
    // Removing the host template restores the autonomous fallback.
    assert(local.configureTemplate(nullptr,0) && !local.nativeElection);
    assert(sameAddress(local.master,self) && sameAddress(local.sync,self) && !local.distance);
    assert(local.transmitReady(100));
}
static void fixedModes() {
    SyncEngine local;local.start(self,149,1000000);
    PeerTable peers={};auto a=advert(2,500);
    peers.update(remote,self,a,1000000,149,-45);
    local.configureSyncState(1,0,self);local.elect(peers,1005000);
    assert(sameAddress(local.master,self) && local.transmitReady(1005000));
    local.configureSyncState(1,0,remote);
    assert(!local.transmitReady(1005000));
    local.elect(peers,1005000);
    assert(sameAddress(local.master,remote) && local.transmitReady(1005000));
    assert(!local.transmitReady(4000000));
    local.elect(peers,4000000);
    assert(sameAddress(local.master,remote) && !local.transmitReady(4000000));
    // Fixed mode ignores a better candidate that the host did not select.
    const uint8_t other[]={2,0,0,0,0,3};a.phyTxTime++;a.masterCounter=500;
    peers.update(other,self,a,4001000,149,-30);local.elect(peers,4005000);
    assert(!local.transmitReady(4005000));
    a.phyTxTime++;a.electionID=0x9876;peers.update(remote,self,a,4006000,149,-40);
    local.elect(peers,4010000);assert(!local.transmitReady(4010000));
    // The native force-master path uses mode 2 and may cross an election-ID boundary.
    local.configureSyncState(2,0,remote);local.elect(peers,4010000);
    assert(local.transmitReady(4010000) && local.electionID==0);
    local.restart(149,4011000);
    assert(local.syncMode==2 && sameAddress(local.fixedParent,remote) && !local.nativeClockReady);
    local.elect(peers,4012000);assert(local.transmitReady(4012000));
    local.configureSyncState(0,0,self);local.elect(peers,4013000);
    assert(sameAddress(local.sync,other)); // automatic election applies its ID check again

    // A native template alone cannot supersede a fixed parent until control catches up.
    uint8_t input[48]={24,40,0};copyAddress(input+3,remote);copyAddress(input+9,remote);
    putLE32(input+15,21);putLE32(input+19,1);putLE32(input+23,500);putLE32(input+27,100);
    putLE32(input+31,0x9876);putLE32(input+39,17);
    input[43]=21;input[44]=2;input[46]=0x93;input[47]=2;
    local.configureSyncState(1,0,self);
    assert(local.configureTemplate(input,sizeof(input)));
    local.elect(peers,4014000);assert(!local.transmitReady(4014000));
    local.configureSyncState(1,0,remote);local.elect(peers,4015000);
    assert(local.transmitReady(4015000) && local.masterCounter==21 && local.counter==17);
    // Warmup adopts the requested peer's AW clock without changing our own election tree.
    copyAddress(input+3,self);copyAddress(input+9,self);putLE32(input+19,0);
    assert(local.configureTemplate(input,sizeof(input)));
    local.configureSyncState(1,0x20,remote);local.elect(peers,4015000);
    assert(local.transmitReady(4015000) && sameAddress(local.master,self));
    assert(sameAddress(local.sync,self) && local.masterCounter==21 && !local.distance);
    // The retained parent frame has a two-microsecond PHY/target TX difference.
    Window w={};assert(local.window(4015000,w) && w.counter==0 && w.untilNextEAWUS==56534);
    uint8_t bytes[512];size_t size=local.action(bytes,sizeof(bytes),4015000,true,nullptr,0);
    Advertisement tx={};assert(parseMPDU(bytes,size,tx)==ParseResult::Valid);
    assert(sameAddress(tx.syncMaster,self) && sameAddress(tx.electionMaster,self) && tx.windowCounter==0);
    local.configureSyncState(1,0,remote);local.elect(peers,4015000);
    assert(!local.transmitReady(4015000)); // root mismatch is disallowed outside warmup
    local.configureSyncState(1,0,self);local.elect(peers,4016000);
    assert(local.transmitReady(4016000));
}
static void unicastWire() {
    SyncEngine engine;engine.start(self,44,1000000);
    // Native UMI omits Version21, even when options request additional PHY TLVs.
    uint8_t minimal[93]={24,40,0};copyAddress(minimal+3,self);copyAddress(minimal+9,self);
    minimal[43]=12;putLE16(minimal+44,47);putLE16(minimal+46,0x9b03);
    minimal[48]='C';minimal[49]='N';copyAddress(minimal+61,self);
    putLE16(minimal+67,4);putLE32(minimal+69,0x4000);
    assert(oobPayload(minimal,sizeof(minimal),true));
    assert(!oobPayload(minimal,sizeof(minimal)));
    for(unsigned n=0;n<sizeof(minimal);++n)
        assert(oobPayload(minimal,n,true)==(n==43));
    for(unsigned variant=0;variant<8;++variant) {
        std::vector<uint8_t> invalid(minimal,minimal+sizeof(minimal));
        switch(variant) {
        case 0:invalid[1]=39;break;
        case 1:invalid[44]=48;break;
        case 2:invalid.insert(invalid.end(),minimal,minimal+43);break;
        case 3:invalid.insert(invalid.end(),{21,1,0,0x93});break;
        case 4:invalid.insert(invalid.end(),{21,2,0,0x93,2,21,2,0,0x93,2});break;
        case 5:invalid.insert(invalid.end(),{4,0,0});break;
        case 6:invalid.insert(invalid.end(),{5,0,0});break;
        case 7:invalid.resize(1201);break;
        }
        for(bool unicast:{false,true})assert(!oobPayload(invalid.data(),invalid.size(),unicast));
    }
    auto withVersion=std::vector<uint8_t>(minimal,minimal+sizeof(minimal));
    withVersion.insert(withVersion.end(),{21,2,0,0x93,2});
    for(bool unicast:{false,true}) {
        assert(oobPayload(withVersion.data(),withVersion.size(),unicast));
        assert(!oobPayload(nullptr,sizeof(minimal),unicast));
        assert(!oobPayload(minimal,0,unicast));
        assert(!oobPayload(withVersion.data()+93,5,unicast));
    }
    uint8_t wire[512]={};
    size_t minimalSize=engine.action(wire,sizeof(wire),1005000,true,minimal,sizeof(minimal),remote);
    assert(minimalSize==184+sizeof(minimal));
    assert(!memcmp(wire+184,minimal,sizeof(minimal)));
    unsigned minimalTypes[256]={};
    for(size_t at=40;at<minimalSize;at+=3+le16(wire+at+1))++minimalTypes[wire[at]];
    for(unsigned type:{4,5,18,24,12})assert(minimalTypes[type]==1);
    assert(!minimalTypes[21]);
    assert(!engine.action(wire,sizeof(wire),1005000,true,minimal,sizeof(minimal),nullptr,true));
    const auto broadcastSize=engine.action(wire,sizeof(wire),1005000,true,
        withVersion.data(),withVersion.size(),nullptr,true);
    assert(broadcastSize==184+withVersion.size());
    assert(!memcmp(wire+184,withVersion.data(),withVersion.size()));
    uint8_t payload[55]={24,40,0};copyAddress(payload+3,self);copyAddress(payload+9,self);
    putLE32(payload+15,1);putLE32(payload+23,100);putLE32(payload+27,100);
    payload[43]=21;payload[44]=2;payload[46]=0x93;payload[47]=2;
    payload[48]=35;payload[49]=4;payload[51]=1;payload[52]=2;payload[53]=3;payload[54]=4;
    assert(oobPayload(payload,sizeof(payload)));
    const size_t required=184+sizeof(payload);std::vector<uint8_t> bytes(required+2,0xa5);
    auto size=engine.action(bytes.data()+1,required,1005000,true,payload,sizeof(payload),remote);
    assert(size==required && bytes.front()==0xa5 && bytes.back()==0xa5);
    auto out=bytes.data()+1;assert(sameAddress(out+4,remote) && sameAddress(out+10,self));
    assert(!memcmp(out+184,payload,sizeof(payload)));
    Advertisement a={};assert(parseMPDU(out,size,a)==ParseResult::Valid && a.subtype==3 && a.version==0x93);
    unsigned counts[256]={};for(size_t at=40;at<size;at+=3+le16(out+at+1))++counts[out[at]];
    for(unsigned t:{4,5,18,24,21,35})assert(counts[t]==1);
    assert(!counts[6] && !counts[12]); // native payload supplies data-path/UMI flags
    for(unsigned cap=0;cap<required;++cap) {
        auto before=bytes;auto sequence=engine.wifiSequence;
        assert(!engine.action(out,cap,1005000,true,payload,sizeof(payload),remote));
        assert(before==bytes && engine.wifiSequence==sequence);
    }
    for(unsigned n=0;n<sizeof(payload);++n)assert(!oobPayload(payload,n) || n==48);
    uint8_t invalid[55];memcpy(invalid,payload,sizeof(invalid));invalid[1]=255;
    assert(!oobPayload(invalid,sizeof(invalid)));
    std::vector<uint8_t> duplicate(payload,payload+sizeof(payload));duplicate.insert(duplicate.end(),payload,payload+43);
    assert(!oobPayload(duplicate.data(),duplicate.size()));
    duplicate.assign(payload,payload+sizeof(payload));duplicate.insert(duplicate.end(),{4,0,0});
    assert(!oobPayload(duplicate.data(),duplicate.size()));
    uint8_t group[6]={255,255,255,255,255,255};
    assert(!engine.action(out,required,1005000,true,payload,sizeof(payload),self));
    assert(!engine.action(out,required,1005000,true,payload,sizeof(payload),group));
    assert(!engine.action(out,required,1005000,false,payload,sizeof(payload),remote));
}
int main() {
#if AWDL_SYNC_CASE == 1
    wire();
#elif AWDL_SYNC_CASE == 2
    election();
#elif AWDL_SYNC_CASE == 3
    scheduling();
#elif AWDL_SYNC_CASE == 4
    discovery();
#elif AWDL_SYNC_CASE == 5
    qualifications();
#elif AWDL_SYNC_CASE == 6
    nativeElection();
#elif AWDL_SYNC_CASE == 7
    fixedModes();
#elif AWDL_SYNC_CASE == 8
    unicastWire();
#else
    nativeMulticastSlots();
#endif
}
