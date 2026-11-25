#include <cassert>
#include <cstring>
#include "AirportItlwm/AWDL/NativeFrame.hpp"
using namespace AirportAWDL;
static const uint8_t self[6]={2,0,0,0,0,1},remote[6]={2,0,0,0,0,2};

static void stamp(uint8_t *out,uint64_t time) {
    for(unsigned i=0;i<4;++i)out[i]=uint32_t(time)>>(24-8*i);
}

int main() {
    const uint64_t received=0x100000000ULL-2000,now=received+20000;
    SyncEngine sender;sender.start(remote,44,received-100000);
    sender.counter=sender.masterCounter=4;
    uint8_t frame[512];size_t size=sender.action(frame,sizeof(frame),received,true,nullptr,0);
    Advertisement a={};assert(parseMPDU(frame,size,a)==ParseResult::Valid);
    a.phyTxTime=100000;a.targetTxTime=97000;a.windowCounter=65532;a.nextWindowTU=4;
    PeerTable table={};assert(table.update(remote,self,a,received+5000,44,-40,5000));
    auto peer=table.find(remote,now);assert(peer && peer->lastReceiveUS==received);
    Window observed={};assert(peer->window(received,observed));
    assert(observed.counter==65535 && observed.untilNextEAWUS==1096);
    assert(peer->window(received+1096,observed) && observed.counter==0);
    assert(observed.untilNextEAWUS==65536);

    // Firmware TX delay wraps independently of the receiver's local clock.
    auto wrapped=*peer;wrapped.sync.phyTxTime=2000;wrapped.sync.targetTxTime=0xfffffc18;
    assert(wrapped.syncPhaseUS()==peer->syncPhaseUS());
    wrapped.sync.targetTxTime=3000;assert(wrapped.syncPhaseUS()==60*1024);
    wrapped.sync.targetTxTime=0;assert(wrapped.syncPhaseUS()==60*1024);

    NativeActionMetadata metadata;
    assert(metadata.fill(frame,size,44,-40,16,received));
    uint8_t record[44]={};
    putLE16(record,65532);putLE16(record+2,4);putLE16(record+4,4000);
    putLE16(record+6,110);putLE16(record+8,16);putLE16(record+10,16);putLE16(record+12,16);
    record[14]=uint8_t(-40);record[16]=a.presenceMode;
    memcpy(record+20,metadata.bytes+48,4);
    copyAddress(record+32,remote);copyAddress(record+38,remote);
    SyncEngine local;local.start(self,44,received-100000);
    assert(local.nativeParent.configure(record,table,now)==AdvertiserResult::Applied);
    local.configureSyncState(1,0,remote);local.elect(table,now);
    assert(local.transmitReady(now));
    assert(local.epochUS==received-60*1024-4000 && local.epochCounter==65532);
    Window actual={};assert(local.window(received+96,actual));
    assert(actual.counter==0 && actual.untilNextEAWUS==65536);

    // Invalid/expired/future records never partially replace an accepted clock.
    auto old=local.nativeParent;
    const AdvertiserFailure failures[]={AdvertiserFailure::Record,AdvertiserFailure::Record,
        AdvertiserFailure::Timing,AdvertiserFailure::StampAge,AdvertiserFailure::StampAge,
        AdvertiserFailure::PeerMissing,AdvertiserFailure::Root,AdvertiserFailure::Timing,
        AdvertiserFailure::NewerStamp};
    for(unsigned test=0;test<9;++test) {
        uint8_t bad[44];memcpy(bad,record,44);
        auto expected=AdvertiserResult::Unavailable;
        switch(test) {
        case 0:bad[38]|=1;expected=AdvertiserResult::Invalid;break;
        case 1:putLE16(bad+2,65);expected=AdvertiserResult::Invalid;break;
        case 2:putLE16(bad+10,32);expected=AdvertiserResult::Unsupported;break;
        case 3:stamp(bad+20,now+1);break;
        case 4:stamp(bad+20,now-3000000);break;
        case 5:bad[43]=3;break;
        case 6:bad[37]=3;break;
        case 7:bad[16]=44;expected=AdvertiserResult::Unsupported;break;
        case 8:stamp(bad+20,received+1);break;
        }
        AdvertiserFailure failure;
        assert(local.nativeParent.configure(bad,table,now,&failure)==expected);
        assert(failure==failures[test]);
        assert(!memcmp(&old,&local.nativeParent,sizeof(old)));
    }
    local.elect(table,received+3000000);assert(!local.transmitReady(received+3000000));
    assert(local.nativeParent.configure(record,table,received+3000000)==AdvertiserResult::Unavailable);

    // A newer observed sync frame supersedes an older host request without
    // shifting its clock to request-processing time.
    a.phyTxTime+=2000;a.targetTxTime+=2000;a.windowCounter=0;a.nextWindowTU=64;
    const uint64_t newer=received+4000;
    assert(table.update(remote,self,a,newer+5000,44,-40,5000));
    local.elect(table,now);
    assert(local.epochUS==newer-3000 && local.epochCounter==0);
    assert(local.nativeParent.configure(record,table,now)==AdvertiserResult::Applied);
    assert(local.nativeParent.receivedUS==newer && local.nativeParent.epochUS==newer-3000);

    // Native last-RX timestamps can advance on frames without a sync TLV.
    // Their retained countdown still belongs to the previous sync-bearing RX.
    a.fields=Version;a.phyTxTime+=1000;
    assert(table.update(remote,self,a,newer+6000,44,-40,5000));
    stamp(record+20,newer+1000);
    assert(local.nativeParent.configure(record,table,now)==AdvertiserResult::Applied);
    assert(local.nativeParent.receivedUS==newer && local.nativeParent.epochUS==newer-3000);
    local.elect(table,now);assert(local.epochUS==newer-3000);
    local.elect(table,newer+3000000);assert(!local.transmitReady(newer+3000000));

    // An exact zero in the 32-bit field is a valid wrap, not an unknown time.
    a.fields=Sync|Channels|ElectionV2;a.phyTxTime+=1000;a.targetTxTime=a.phyTxTime;
    const uint64_t zeroStamp=0x200000000ULL;
    assert(table.update(remote,self,a,zeroStamp+5000,44,-40,5000));
    stamp(record+20,zeroStamp);putLE16(record,0);putLE16(record+2,64);putLE16(record+4,0);
    assert(local.nativeParent.configure(record,table,zeroStamp+10000)==AdvertiserResult::Applied);
    assert(local.nativeParent.epochUS==zeroStamp);
    local.restart(44,zeroStamp+10000);assert(!local.nativeParent.valid);
    assert(!local.transmitReady(zeroStamp+10000));
}
