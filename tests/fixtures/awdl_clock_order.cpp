#include <cassert>
#include <cstring>
#include "AirportItlwm/AWDL/NativeFrame.hpp"
using namespace AirportAWDL;
static const uint8_t self[]={2,0,0,0,0,1}, remote[]={2,0,0,0,0,2};
static void stamp(uint8_t *p,uint64_t t) {
    for(unsigned i=0;i<4;++i)p[i]=uint32_t(t)>>(24-8*i);
}
static Advertisement action(SyncEngine &sender,uint64_t now,uint32_t phy,
                            uint16_t seq,bool retry=false) {
    uint8_t frame[512];size_t n=sender.action(frame,sizeof(frame),now,true,nullptr,0);
    putLE32(frame+32,phy);putLE32(frame+36,phy);
    putLE16(frame+22,seq<<4);frame[1]|=retry?8:0;
    Advertisement a={};assert(parseMPDU(frame,n,a)==ParseResult::Valid);
    return a;
}
int main() {
    const uint64_t base=10000000;
    SyncEngine sender;sender.start(remote,6,base);
    PeerTable peers={};auto a=action(sender,base,1000000000,4095);
    assert(peers.update(remote,self,a,base,6,-38));
    auto peer=peers.find(remote,base);assert(peer);peer->nativeCached=true;
    assert(peer->dataSequence.accept(100,false));
    SyncEngine local;local.start(self,6,base);local.configureSyncState(1,0,remote);
    local.elect(peers,base);assert(local.transmitReady(base));
    uint8_t record[44]={};
    putLE16(record+6,110);putLE16(record+8,16);putLE16(record+10,16);
    putLE16(record+12,16);record[16]=4;
    copyAddress(record+32,remote);copyAddress(record+38,remote);
    for(unsigned step=1;step<=48;++step) {
        const uint64_t t=base+step*250000;
        a=action(sender,t,1000+step*250000,uint16_t((step-1)&4095));
        assert(peers.update(remote,self,a,t,6,-38));
        putLE16(record,a.windowCounter);putLE16(record+2,a.nextWindowTU);stamp(record+20,t);
        assert(local.nativeParent.configure(record,peers,t)==AdvertiserResult::Applied);
        local.elect(peers,t);assert(local.transmitReady(t));
        assert(peers.find(remote,t)==peer && peer->nativeCached);
        assert(!peer->dataSequence.accept(100,false));
    }
    assert(!peers.expired && !peers.stale && peers.remoteClockBacksteps==1);
    const uint64_t t=base+12000000;
    auto saved=*peer;
    a=action(sender,t+1000,20000000,47,true);
    assert(!peers.update(remote,self,a,t+1000,6,-38));
    assert(!memcmp(peer,&saved,sizeof(saved)));
    a=action(sender,t+2000,30000000,48);
    assert(!peers.update(remote,self,a,t+2000,6,-38,3000));
    assert(!memcmp(peer,&saved,sizeof(saved)));
    // A retry that was not received previously is useful, even after a PHY reset.
    a=action(sender,t+3000,1,48,true);
    assert(peers.update(remote,self,a,t+3000,6,-38));
    assert(peers.remoteClockBacksteps==2 && peers.stale==2);
    // Reused/zero PHY timestamps do not suppress fresh 802.11 observations.
    a=action(sender,t+4000,1,49);assert(peers.update(remote,self,a,t+4000,6,-38));
    a=action(sender,t+5000,0,50);assert(peers.update(remote,self,a,t+5000,6,-38));
    local.elect(peers,t+5000);assert(local.transmitReady(t+5000));
    local.elect(peers,t+3005000);assert(!local.transmitReady(t+3005000));
}
