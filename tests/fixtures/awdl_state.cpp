#include <cassert>
#include <cstring>
#include <vector>
#include "AirportItlwm/AWDL/DataFrame.hpp"

using namespace AirportAWDL;
using Bytes = std::vector<uint8_t>;
static const uint8_t self[6] = {2,0,0,0,0,1}, remote[6] = {2,0,0,0,0,2};

static Advertisement advertisement() {
    Advertisement a = {};
    a.fields = Sync | Channels | Version | ElectionV2;
    a.subtype = 3; a.version = 0x31; a.deviceClass = 2;
    a.phyTxTime = 100; a.windowCounter = 8; a.presenceMode = 4;
    a.haveActionSequence = true; a.actionSequence = 1;
    a.nextWindowTU = 16; a.windowPeriodTU = 16;
    a.masterMetric = 1234; a.distance = 1;
    a.electionID = 0x89abcdef;
    copyAddress(a.syncMaster, remote); copyAddress(a.electionMaster, remote);
    a.sequence.count = 16; a.sequence.step = 3; a.sequence.fill = 0xffff;
    a.sequence.encoding = 3;
    for(unsigned i=0;i<16;++i) { a.sequence.channel[i]=6+i; a.sequence.flags[i]=81; }
    return a;
}

static void peers() {
    PeerTable table = {}; auto a=advertisement();
    assert(!table.update(self,self,a,10,44,-50));
    uint8_t address[6]={};assert(!table.update(address,self,a,10,44,-50));
    address[0]=1;assert(!table.update(address,self,a,10,44,-50));
    assert(table.update(remote,self,a,10,44,-50));
    assert(table.expire(10)==1 && table.added==1 && table.rejected==3);
    const Peer *peer=table.find(remote,10);
    assert(peer && peer->receivedMIF && peer->metadata.masterMetric==1234);
    assert(peer->metadata.electionID==0x89abcdef);
    a.fields=Version;a.phyTxTime++;a.version=0x32;a.subtype=0;++a.actionSequence;
    assert(table.update(remote,self,a,20,6,-60));
    peer=table.find(remote,20);
    assert(peer->metadata.version==0x32 && peer->haveSequence && peer->haveSync);
    assert(peer->syncReceivedUS==10 && peer->sequenceReceivedUS==10);
    assert(peer->metadata.masterMetric==1234 && peer->receivedMIF);
    a.retry=true;
    assert(!table.update(remote,self,a,30,149,-90)); // duplicate retry
    a.retry=false;++a.actionSequence;
    a.phyTxTime=99;assert(!table.update(remote,self,a,30,149,-90,15)); // older local RX
    assert(peer->lastSeenUS==20 && peer->rxChannel==6 && table.stale==2);
    a.phyTxTime=102;a.fields=ElectionV1;a.masterMetric=1;a.electionID=3;
    assert(table.update(remote,self,a,40,6,-60));
    assert(peer->metadata.masterMetric==1234); // v1 cannot regress v2 across frames
    assert(peer->metadata.electionID==0x89abcdef);
    a=advertisement();a.phyTxTime=103;a.fields=Sync;a.syncMaster[5]=5;
    assert(table.update(remote,self,a,50,6,-60));
    assert(!peer->haveSequence); // another master requires a fresh schedule
    assert(table.find(remote,50+PeerTable::LifetimeUS-1));
    assert(!table.find(remote,50+PeerTable::LifetimeUS));
    assert(table.expire(50+PeerTable::LifetimeUS)==0 && table.expired==1);

    table={};a=advertisement();a.phyTxTime=0xfffffff0;
    assert(table.update(remote,self,a,100,44,-50));
    a.phyTxTime=0x10;assert(table.update(remote,self,a,200,44,-50));
    a.phyTxTime=0xfffffff8;assert(!table.update(remote,self,a,300,44,-50,150));
    assert(table.find(remote,200)->phyTime==0x10);
    assert(table.expire(50)==0); // a new local clock epoch invalidates old observations

    table={};a=advertisement();address[0]=2;
    for(unsigned i=0;i<PeerTable::Capacity;++i) {
        address[5]=i+10; assert(table.update(address,self,a,100+i,6,-50));
    }
    address[5]=100; assert(table.update(address,self,a,200,6,-50));
    assert(table.evicted==1 && table.expire(200)==PeerTable::Capacity);
    address[5]=10;assert(!table.find(address,200));
    address[5]=11;assert(table.find(address,200));
    a.phyTxTime++;
    assert(table.update(address,self,a,300,6,-50) && table.evicted==1);
}

static void timing() {
    PeerTable table={};auto a=advertisement();
    assert(table.update(remote,self,a,100000,44,-50));
    Peer &peer=*table.find(remote,100000);Window w={};
    assert(peer.window(100000,w));
    assert(w.counter==11 && w.channel==8 && w.encoding==3 && w.channelFlags==81);
    assert(w.untilNextAWUS==16384 && w.untilNextEAWUS==16384);
    assert(peer.window(116383,w) && w.counter==11 && w.untilNextAWUS==1);
    assert(peer.window(116384,w) && w.counter==12 && w.channel==9);
    assert(w.untilNextEAWUS==65536);
    assert(peer.window(116385,w) && w.untilNextAWUS==16383);
    assert(!peer.window(99999,w));
    assert(!peer.window(3100000,w));
    peer.sync.windowCounter=65532;
    assert(peer.window(116384,w) && w.counter==0 && w.channel==6);
    peer.sync.nextWindowTU=0;
    assert(peer.window(100000,w) && w.counter==0 && w.untilNextEAWUS==65536);
    peer.sync.nextWindowTU=65;assert(!peer.window(100000,w));
    peer.sync.nextWindowTU=16;peer.sync.presenceMode=3;assert(!peer.window(100000,w));
    peer.sync.presenceMode=4;peer.sequence.duplicates=1;assert(!peer.window(100000,w));
    peer.sequence.duplicates=0;peer.sequence.step=1;assert(!peer.window(100000,w));
    peer.sequence.step=3;peer.sequence.fill=6;assert(!peer.window(100000,w));
    peer.sequence.fill=0xffff;peer.sequence.channel[15]=0;
    assert(peer.window(100000,w) && w.channel==0); // a zero slot is unavailable

    PeerTable delayed={};a=advertisement();
    uint8_t other[6]={2,0,0,0,0,3};
    assert(delayed.update(other,self,a,108000,44,-50));
    assert(delayed.update(remote,self,a,120000,44,-50,20000));
    auto p=delayed.find(remote,120000);
    assert(p && p->syncReceivedUS==100000 && p->sequenceReceivedUS==100000);
    assert(p->lastSeenUS==120000 && delayed.expire(120000)==2);
    assert(p->window(120000,w) && w.counter==12 && w.channel==9);
    assert(w.untilNextEAWUS==61920); // RX processing crossed the EAW boundary.
    assert(delayed.find(other,120000)); // An older air timestamp must not expire other peers.
    assert(!delayed.update(remote,self,a,10,44,-50,11));
    assert(delayed.find(other,120000));

    // 24H32 awdlLllcInput: word[11:0] sequence, word[13:12] class,
    // independent unicast/group state; only zero modular delta is a duplicate.
    DataSequenceState seq={};
    for(bool group:{false,true})for(unsigned ac=0;ac<4;++ac) {
        const uint16_t prefix=ac<<12;
        assert(seq.accept(prefix|54,group));
        assert(!seq.accept(prefix|54,group));
        assert(seq.accept(prefix,group)); // Captured peer counter restart, 54 -> 0.
        assert(!seq.accept(prefix,group));
        assert(seq.accept(prefix|4094,group) && seq.accept(prefix|4095,group));
        assert(seq.accept(prefix,group) && seq.accept(prefix|1,group));
        assert(!seq.accept(prefix|1,group));
        assert(!seq.accept(0xc000|prefix|1,group)); // Upper two bits are not sequence.
        assert(seq.accept(prefix|128,group) && seq.accept(prefix|127,group));
        assert(seq.accept(prefix|128,group)); // Reordered nonconsecutive arrivals stay eligible.
    }
    assert(seq.initialized==0xff);
    seq={};
    for(bool group:{false,true})for(unsigned ac=0;ac<4;++ac)
        assert(seq.accept((ac<<12)|7,group));
    for(bool group:{false,true})for(unsigned ac=0;ac<4;++ac)
        assert(!seq.accept((ac<<12)|7,group));
    assert(seq.accept(0x2008,false));
    assert(!seq.accept(0x2007,true) && !seq.accept(0x1007,false));
}

static Bytes ethernet(unsigned payload=5) {
    Bytes b(14+40+payload,0);
    copyAddress(b.data(),self);copyAddress(b.data()+6,remote);
    putBE16(b.data()+12,0x86dd);b[14]=0x60;putBE16(b.data()+18,payload);
    b[20]=59;b[21]=64;
    for(unsigned i=0;i<payload;++i)b[54+i]=i;
    return b;
}

static Bytes mpdu(uint16_t seq=7) {
    auto e=ethernet();Bytes b(e.size()+26,0);
    assert(dataFromEthernet(e.data(),e.size(),b.data(),b.size(),0x1234,seq)==b.size());
    return b;
}

static Bytes qos(Bytes b,bool ht=false) {
    b[0]=0x88;b[1]=ht?0x80:0;b.insert(b.begin()+24,ht?6:2,0);return b;
}

static Bytes aggregate(unsigned count=2) {
    auto b=mpdu();Bytes a(b.begin(),b.begin()+24);a[0]=0x88;a.push_back(0x80);a.push_back(0);
    for(unsigned i=0;i<count;++i) {
        auto sub=mpdu(i);
        a.insert(a.end(),sub.begin()+4,sub.begin()+16);
        unsigned n=sub.size()-24;a.push_back(n>>8);a.push_back(n);
        a.insert(a.end(),sub.begin()+24,sub.end());
        if(i+1<count)while((a.size()-26)%4)a.push_back(0);
    }
    return a;
}

static void data() {
    DataBatch batch={};auto b=mpdu();auto e=ethernet();
    // Independent literal wire example: mixed 802.11/AWDL little endian and IPv6 big endian.
    const Bytes literal={
        0x08,0,0,0, 2,0,0,0,0,1, 2,0,0,0,0,2, 0,0x25,0,0xff,0x94,0x73, 0x40,0x23,
        0xaa,0xaa,3,0,0x17,0xf2,8,0, 3,4,7,0,0,0,0x86,0xdd,
        0x60,0,0,0,0,5,59,64,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,1,2,3,4
    };
    assert(b==literal);
    assert(parseDataMPDU(literal.data(),literal.size(),batch)==ParseResult::Valid);
    assert(batch.frames[0].sequence==7 && batch.frames[0].size==45);
    assert(b[22]==0x40 && b[23]==0x23 && b[34]==7);
    for(auto wire:{b,qos(b),qos(b,true)}) {
        assert(parseDataMPDU(wire.data(),wire.size(),batch)==ParseResult::Valid && batch.count==1);
        auto &f=batch.frames[0];assert(f.sequence==7 && f.size==45);
        Bytes out(e.size(),0xa5);
        assert(!ethernetFromData(f,out.data(),out.size()-1) && out[0]==0xa5);
        assert(ethernetFromData(f,out.data(),out.size())==e.size() && out==e);
    }
    auto a=aggregate();assert(parseDataMPDU(a.data(),a.size(),batch)==ParseResult::Valid);
    assert(batch.count==2 && batch.frames[0].sequence==0 && batch.frames[1].sequence==1);
    auto bad=a;bad[26+6]^=2;
    assert(parseDataMPDU(bad.data(),bad.size(),batch)==ParseResult::Malformed);
    // The final malformed subframe must not publish the earlier valid subframe.
    DataBatch saved=batch;
    bad=a;bad.pop_back();assert(parseDataMPDU(bad.data(),bad.size(),batch)==ParseResult::Malformed);
    assert(!std::memcmp(&batch,&saved,sizeof(batch)));
    a=aggregate(DataBatch::Capacity+1);
    assert(parseDataMPDU(a.data(),a.size(),batch)==ParseResult::Unsupported);
    b=qos(mpdu());b[24]=0x80;
    assert(parseDataMPDU(b.data(),b.size(),batch)==ParseResult::Malformed);
    assert(parseDataMPDU(b.data(),b.size(),batch,true)==ParseResult::Valid);
    for(auto bit:{1,2,0x40}) { b=mpdu();b[1]=bit;assert(parseDataMPDU(b.data(),b.size(),batch)==ParseResult::Other); }
    b=mpdu();b[16]^=2;assert(parseDataMPDU(b.data(),b.size(),batch)==ParseResult::Other);
    b=mpdu();b[22]|=1;assert(parseDataMPDU(b.data(),b.size(),batch)==ParseResult::Malformed);
    b=mpdu();b[10]|=1;assert(parseDataMPDU(b.data(),b.size(),batch)==ParseResult::Malformed);
    b=mpdu();b[40]=0x40;assert(parseDataMPDU(b.data(),b.size(),batch)==ParseResult::Malformed);
    b=mpdu();b[39]=0;assert(parseDataMPDU(b.data(),b.size(),batch)==ParseResult::Unsupported);
    e=ethernet(1460);b.resize(e.size()+26);
    assert(dataFromEthernet(e.data(),e.size(),b.data(),b.size(),0,65535)==b.size());
    assert(parseDataMPDU(b.data(),b.size(),batch)==ParseResult::Valid);
    e=ethernet(1461);b.assign(e.size()+26,0xa5);
    assert(!dataFromEthernet(e.data(),e.size(),b.data(),b.size(),0,0) && b[0]==0xa5);
    b=mpdu();b.insert(b.begin(),0);assert(parseDataMPDU(b.data()+1,b.size()-1,batch)==ParseResult::Valid);
    b=mpdu();
    for(size_t n=0;n<b.size();++n)assert(parseDataMPDU(b.data(),n,batch)!=ParseResult::Valid);
    uint32_t seed=98765;
    for(unsigned i=0;i<25000;++i) {
        b=(i&1)?aggregate():qos(mpdu(),true);
        seed=1664525*seed+1013904223;b[seed%b.size()]^=uint8_t(seed>>24);
        seed=1664525*seed+1013904223;b.resize(seed%(b.size()+1));
        auto r=parseDataMPDU(b.data(),b.size(),batch);
        if(r==ParseResult::Valid)for(unsigned j=0;j<batch.count;++j) {
            assert(batch.frames[j].payload>=b.data());
            assert(batch.frames[j].payload+batch.frames[j].size<=b.data()+b.size());
        }
    }
}

static void transmitPair() {
    auto first=ethernet(),second=ethernet();Bytes out(3100,0xa5);
    auto n=dataPairFromEthernet(first.data(),first.size(),second.data(),second.size(),
        out.data(),out.size(),0x1234,0,1);
    assert(n==aggregate().size() && Bytes(out.begin(),out.begin()+n)==aggregate());
    for(unsigned a:{0u,1u,2u,3u,1460u})for(unsigned b:{0u,3u,1460u}) {
        first=ethernet(a);second=ethernet(b);
        if(b)second.back()^=0x55;
        out.assign(3100,0xa5);
        n=dataPairFromEthernet(first.data(),first.size(),second.data(),second.size(),
            out.data()+1,out.size()-2,0xffff,0xffff,0);
        assert(n && n<=3088 && out.front()==0xa5 && out[n+1]==0xa5);
        DataBatch batch={};
        assert(parseDataMPDU(out.data()+1,n,batch)==ParseResult::Valid && batch.count==2);
        assert(le16(out.data()+23)==0xfff0 && batch.frames[0].sequence==0xffff && !batch.frames[1].sequence);
        for(unsigned i=0;i<2;++i) {
            Bytes restored(i?second.size():first.size());
            assert(ethernetFromData(batch.frames[i],restored.data(),restored.size())==restored.size());
            assert(restored==(i?second:first));
        }
        Bytes small(n+2,0xa5);
        assert(!dataPairFromEthernet(first.data(),first.size(),second.data(),second.size(),
            small.data()+1,n-1,0,0,1));
        assert(small==Bytes(n+2,0xa5));
    }
    for(unsigned bad=0;bad<9;++bad) {
        first=ethernet();second=ethernet();
        if(bad==0)first[0]|=1;
        if(bad==1)first[6]|=1;
        if(bad==2)second[5]^=2;
        if(bad==3)second[11]^=2;
        if(bad==4)second[13]=0;
        if(bad==5)second[18]++;
        if(bad==6)second=ethernet(1461);
        if(bad==7)first.resize(13);
        if(bad==8)second.resize(13);
        out.assign(3100,0xa5);
        assert(!dataPairFromEthernet(first.data(),first.size(),second.data(),second.size(),
            out.data(),out.size(),0,0,1));
        assert(out==Bytes(3100,0xa5));
    }
}

static void receive() {
    PeerTable peers={};DataReceiveState state={};auto b=mpdu();
    assert(state.receive(b.data(),b.size(),peers,self,100) && state.unknownPeer==1);
    auto a=advertisement();assert(peers.update(remote,self,a,100,6,-50));
    assert(state.receive(b.data(),b.size(),peers,self,200) && state.observed==1);
    assert(state.receive(b.data(),b.size(),peers,self,300) && state.duplicate==1);
    b=mpdu(8);b[9]=9;
    assert(state.receive(b.data(),b.size(),peers,self,400) && state.otherDestination==1);
    b[9]=1;assert(state.receive(b.data(),b.size(),peers,self,500) && state.observed==2);
    b=mpdu(9);b[4]=0x33;b[5]=0x33;
    assert(state.receive(b.data(),b.size(),peers,self,600) && state.observed==3);
    assert(peers.find(remote,600)->lastSeenUS==100); // data cannot extend discovery lifetime
    b=aggregate();b.pop_back();auto seq=peers.find(remote,600)->dataSequence;
    assert(state.receive(b.data(),b.size(),peers,self,600) && state.malformed==1);
    assert(!std::memcmp(&seq,&peers.find(remote,600)->dataSequence,sizeof(seq)));
    b=mpdu(10);
    assert(state.receive(b.data(),b.size(),peers,self,100+PeerTable::LifetimeUS) && state.unknownPeer==2);
    assert(!peers.find(remote,100+PeerTable::LifetimeUS));

    peers={};state={};assert(peers.update(remote,self,a,100,6,-50));
    for(bool group:{false,true})for(unsigned ac=0;ac<4;++ac) {
        b=mpdu((ac<<12)|7);
        if(group) {b[4]=0x33;b[5]=0x33;}
        const auto observed=state.observed;
        assert(state.receive(b.data(),b.size(),peers,self,200));
        assert(state.observed==observed+1 && !state.duplicate);
    }
    for(bool group:{false,true})for(unsigned ac=0;ac<4;++ac) {
        b=mpdu((ac<<12)|7);
        if(group) {b[4]=0x33;b[5]=0x33;}
        const auto duplicate=state.duplicate;
        assert(state.receive(b.data(),b.size(),peers,self,300));
        assert(state.observed==8 && state.duplicate==duplicate+1);
    }
    b=mpdu(0);assert(state.receive(b.data(),b.size(),peers,self,400) && state.observed==9);
}

static void receiveDemand() {
    PeerTable peers={};DataReceiveState state={};auto a=advertisement();
    assert(peers.update(remote,self,a,100,6,-50));
    auto peer=peers.find(remote,100);assert(peer && !peer->lastUnicastDataUS);
    auto deliver=[](void *context,const DataView &) {++*static_cast<unsigned *>(context);};
    unsigned delivered=0;
    auto b=mpdu(1);
    state.receive(b.data(),b.size(),peers,self,200,false,deliver,&delivered);
    assert(delivered==1 && peer->lastUnicastDataUS==200 && peer->lastSeenUS==100);
    state.receive(b.data(),b.size(),peers,self,300,false,deliver,&delivered);
    assert(delivered==1 && peer->lastUnicastDataUS==200);
    b=mpdu(2);b[4]=0x33;b[5]=0x33;
    state.receive(b.data(),b.size(),peers,self,400,false,deliver,&delivered);
    assert(delivered==2 && peer->lastUnicastDataUS==200);
    b=mpdu(3);b[9]=9;
    state.receive(b.data(),b.size(),peers,self,500,false,deliver,&delivered);
    assert(delivered==2 && peer->lastUnicastDataUS==200);
    b=mpdu(4);b[15]=9;
    state.receive(b.data(),b.size(),peers,self,600,false,deliver,&delivered);
    assert(delivered==2 && peer->lastUnicastDataUS==200);
    b=mpdu(5);
    state.receive(b.data(),b.size(),peers,self,700);
    assert(peer->lastUnicastDataUS==200);
    b=mpdu(6);
    state.receive(b.data(),b.size(),peers,self,800,false,deliver,&delivered);
    assert(delivered==3 && peer->lastUnicastDataUS==800);
    peer->nativeCached=true;peer->resetObservation();
    assert(!peer->lastUnicastDataUS && peer->nativeCached);
    assert(peers.update(remote,self,a,900,6,-50));
    b=mpdu(7);state.receive(b.data(),b.size(),peers,self,1000,false,deliver,&delivered);
    assert(peer->lastUnicastDataUS==1000);
    peers.expire(900+PeerTable::LifetimeUS);
    assert(!peer->lastUnicastDataUS && !peer->occupied);
    assert(peers.update(remote,self,a,20000000,6,-50));
    b=mpdu(8);
    auto reset=[](void *context,const DataView &) {static_cast<Peer *>(context)->resetObservation();};
    state.receive(b.data(),b.size(),peers,self,20000100,false,reset,peer);
    assert(!peer->lastUnicastDataUS && !peer->occupied);
}

static void cacheReservations() {
    PeerTable table={};auto a=advertisement();
    assert(table.update(remote,self,a,100,44,-40));
    auto peer=table.find(remote,100);assert(peer);peer->nativeCached=true;
    assert(table.findCached(remote)==peer);
    Window w;assert(peer->window(100,w));
    assert(!peer->window(3000100,w)); // registration does not extend clock validity
    auto dataFrame=mpdu();DataReceiveState state={};
    assert(state.receive(dataFrame.data(),dataFrame.size(),table,self,100+PeerTable::LifetimeUS));
    assert(state.unknownPeer==1 && !state.observed);
    assert(table.expire(100+PeerTable::LifetimeUS)==0);
    assert(table.findCached(remote)==peer && !peer->occupied && !peer->havePhyTime);
    assert(!peer->haveSync && !peer->haveSequence && !peer->dataSequence.initialized);
    assert(!table.find(remote,100+PeerTable::LifetimeUS));
    a.phyTxTime=1; // a peer restart may reset its PHY clock
    assert(table.update(remote,self,a,20000000,44,-40));
    assert(table.find(remote,20000000)==peer && peer->nativeCached && peer->haveSync);
    assert(peer->dataSequence.accept(100,false));
    peer->resetObservation();assert(!peer->dataSequence.initialized);
    assert(table.findCached(remote)==peer && !table.find(remote,20000000));
    assert(table.update(remote,self,a,20000001,44,-40));
    assert(peer->dataSequence.accept(100,false));
    table={};a=advertisement();uint8_t address[6]={2,0,0,0,0,10};
    for(unsigned j=0;j<PeerTable::Capacity;++j) {
        address[5]=j+10;assert(table.update(address,self,a,100+j,44,-40));
        table.find(address,100+j)->nativeCached=true;
    }
    address[5]=100;
    assert(!table.update(address,self,a,1000,44,-40)); // all slots reserved
    assert(table.rejected==1 && !table.evicted && table.expire(1000)==32);
    address[5]=10;auto reserved=table.findCached(address);assert(reserved);
    a.phyTxTime++;assert(table.update(address,self,a,1000,44,-40));
    assert(table.find(address,1000)==reserved && reserved->nativeCached);
    address[5]=11;table.findCached(address)->nativeCached=false;
    address[5]=100;assert(table.update(address,self,a,1001,44,-40));
    assert(table.evicted==1 && !table.find(address,1001)->nativeCached);
    address[5]=10;assert(table.findCached(address)==reserved);
    assert(table.expire(1001+PeerTable::LifetimeUS)==0);
    assert(table.findCached(address)==reserved && !reserved->occupied);
    address[5]=101;assert(table.update(address,self,a,20000000,44,-40)); // unregistered slot reused
    assert(table.findCached(reserved->address)==reserved);
    table.clearCache();
    for(const auto &p:table.peers) assert(!p.nativeCached);
    address[5]=10;assert(table.update(address,self,a,20000001,44,-40));
    assert(table.find(address,20000001) && !table.findCached(address));
}

static void htLifetime() {
    PeerTable table{};auto a=advertisement();a.fields|=HTCapabilities;a.rxHTMcs=0xff;a.rxHTMcs2=0xf0;
    assert(table.update(remote,self,a,1000,6,-40,100));
    auto p=table.find(remote,1000);assert(p && p->txHTMcs(1000)==0xff);
    assert(!p->txHTMcs(899) && p->txHTMcs(900)==0xff);
    a.fields=Version;a.phyTxTime++;
    assert(table.update(remote,self,a,2000,6,-40));
    assert(p->txHTMcs(2000)==0xff && p->txHTMcs2(2000)==0xf0 && p->htCapabilitiesReceivedUS==900);
    assert(!p->txHTMcs2(899)&&p->txHTMcs2(3000899)==0xf0&&!p->txHTMcs2(3000900));
    assert(p->txHTMcs(3000899)==0xff && !p->txHTMcs(3000900));
    a.fields=HTCapabilities;a.phyTxTime++;a.rxHTMcs=8;a.rxHTMcs2=0;
    assert(table.update(remote,self,a,3001000,6,-40) && p->txHTMcs(3001000)==8 && !p->txHTMcs2(3001000));
    a.rxHTMcs=0;a.retry=true;
    assert(!table.update(remote,self,a,3001001,6,-40) && p->txHTMcs(3001001)==8);
    a.phyTxTime++;a.retry=false;++a.actionSequence;
    assert(table.update(remote,self,a,3001002,6,-40) && !p->txHTMcs(3001002));
    p->resetObservation();assert(!p->txHTMcs(3001002) && !p->htCapabilitiesReceivedUS);
}

int main() {
    switch(AWDL_STATE_CASE) {
        case 1: peers();break;
        case 2: timing();break;
        case 3: data();break;
        case 4: receive();break;
        case 5: cacheReservations();break;
        case 6: receiveDemand();break;
        case 7: htLifetime();break;
        case 8: transmitPair();break;
    }
}
