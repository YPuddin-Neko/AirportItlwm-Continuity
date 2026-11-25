#include <cassert>
#include <cstring>
#include <vector>
#include "AirportItlwm/AWDL/NativeFrame.hpp"
#include "AirportItlwm/AWDL/ServiceMerge.hpp"
using namespace AirportAWDL;
// NATIVE CHANNEL FLAGS

struct IO80211Buffer {
    uint8_t bytes[1536];
    size_t capacity=sizeof(bytes),length=0;
    unsigned returns=0;
    bool nullBytes=false;
    void *getBytesNoCopy() { return nullBytes ? nullptr : bytes; }
    size_t getCapacity() const { return capacity; }
    void setLength(size_t size) { assert(size<=capacity);length=size; }
    void returnBuffer() { assert(returns++==0); }
};
struct Pool {
    IO80211Buffer buffer;
    bool exhausted=false;
    IO80211Buffer *getBuffer() {
        if(exhausted)return nullptr;
        buffer.returns=0;buffer.length=0;
        return &buffer;
    }
};
struct BSD { bool attached=true;void *getIfnet() { return attached ? this : nullptr; } };
struct AirportItlwmAWDLInterface {
    bool stopping=false,noPool=false;
    ServiceMergeCache serviceMerge={};
#ifdef AWDL_SERVICE_MERGE_TEST
    bool serviceMergeEnabled=true;
#else
    bool serviceMergeEnabled=false;
#endif
    BSD storage,*bsd=&storage;
    Pool pool;
    unsigned actionDropped=0,actionDelivered=0,taps=0;
    unsigned references=1;
    void retain() { ++references; }
    void release() { assert(references>1);--references; }
    NativeActionMetadata last;
    std::vector<uint8_t> payload;
    Pool *getMgmtFramePool() { return noPool ? nullptr : &pool; }
    void bpfTapInput(IO80211Buffer *buffer,unsigned type,void *metadata,unsigned long length) {
        assert(references==2);
        assert(type==12 && length==72 && !buffer->returns);
        last=*static_cast<NativeActionMetadata *>(metadata);
        payload.assign(buffer->bytes,buffer->bytes+buffer->length);
        ++taps;
        buffer->returnBuffer();
    }
    static void receiveAction(void *,const uint8_t *,size_t,uint16_t,int16_t,uint64_t);
};

// PRODUCTION

int main() {
    uint8_t address[]={2,3,4,5,6,7};
    SyncEngine sender;sender.start(address,44,1000000);
    std::vector<uint8_t> frame(1536);
    frame.resize(sender.action(frame.data(),frame.size(),1004000,true,nullptr,0));
    assert(frame.size()>40);
    AirportItlwmAWDLInterface interface;
    auto deliver=[&](const std::vector<uint8_t> &p) {
        interface.receiveAction(&interface,p.data(),p.size(),44,-47,0x123456789ULL);
    };
    deliver(frame);
    assert(interface.taps==1 && interface.actionDelivered==1 && !interface.actionDropped);
    assert(interface.pool.buffer.returns==1);
    assert(interface.payload==std::vector<uint8_t>(frame.begin()+24,frame.end()));
    uint8_t expected[72]={};
    std::memcpy(expected,address,6);
    expected[12]=1;expected[16]=44;expected[20]=0x10;
    expected[44]=0xd1;expected[45]=expected[46]=expected[47]=0xff;
    expected[48]=0x23;expected[49]=0x45;expected[50]=0x67;expected[51]=0x89;
    assert(!std::memcmp(interface.last.bytes,expected,72));
    auto htc=frame;htc[1]=0x80;htc.insert(htc.begin()+24,{1,2,3,4});
    deliver(htc);
    assert(interface.payload==std::vector<uint8_t>(frame.begin()+24,frame.end()));
    assert(interface.taps==2 && interface.pool.buffer.returns==1);
    for(size_t size=0;size<frame.size();++size) {
        NativeActionMetadata data;std::memset(&data,0xa5,sizeof(data));
        Advertisement a;
        bool valid=parseMPDU(frame.data(),size,a)==ParseResult::Valid;
        assert(data.fill(frame.data(),size,44,-47,APPLE80211_C_FLAG_5GHZ,0x123456789ULL)==valid);
        if(!valid)for(uint8_t byte:data.bytes)assert(byte==0xa5);
    }
    interface.pool.buffer.capacity=1;deliver(frame);
    assert(interface.taps==2 && interface.actionDropped==1 && interface.pool.buffer.returns==1);
    interface.pool.buffer.capacity=1536;interface.pool.buffer.nullBytes=true;deliver(frame);
    assert(interface.actionDropped==2 && interface.pool.buffer.returns==1);
    interface.pool.buffer.nullBytes=false;interface.pool.exhausted=true;deliver(frame);
    assert(interface.actionDropped==3 && interface.taps==2);
    interface.pool.exhausted=false;interface.noPool=true;deliver(frame);
    assert(interface.actionDropped==4);
    interface.noPool=false;interface.stopping=true;deliver(frame);
    interface.stopping=false;interface.bsd=nullptr;deliver(frame);
    assert(interface.actionDropped==4 && interface.taps==2);
    interface.bsd=&interface.storage;
    auto malformed=frame;malformed[10]|=1;deliver(malformed);
    assert(interface.actionDropped==5 && interface.taps==2);
    deliver(frame);assert(interface.taps==3 && interface.pool.buffer.returns==1);
    interface.receiveAction(&interface,frame.data(),frame.size(),6,-47,0x123456789ULL);
    expected[16]=6;expected[20]=0x08;
    assert(!std::memcmp(interface.last.bytes,expected,72));
    assert(interface.taps==4 && interface.pool.buffer.returns==1);
#ifdef AWDL_SERVICE_MERGE_TEST
    auto makeServiceFrame=[&](SyncEngine &peer,uint64_t at,bool mif,uint16_t sui,const std::vector<uint8_t> &svcValues) {
        std::vector<uint8_t> extra={6,9,0,0,0,0,uint8_t(sui&0xff),uint8_t(sui>>8),0,0,0,0};
        for(uint8_t id:svcValues) {
            extra.insert(extra.end(),{2,6,0,'s','v','c',id,uint8_t(id^0x5a),1});
        }
        std::vector<uint8_t> out(1536);
        out.resize(peer.action(out.data(),out.size(),at,mif,extra.data(),extra.size()));
        assert(out.size()>40);
        return out;
    };
    auto countTlv2=[](const std::vector<uint8_t> &body,std::vector<uint8_t> &ids) {
        ids.clear();
        size_t off=16;
        while(off+3<=body.size()) {
            size_t len=size_t(body[off+1])|(size_t(body[off+2])<<8);
            assert(off+3+len<=body.size());
            if(body[off]==2 && len==6)ids.push_back(body[off+6]);
            off+=3+len;
        }
        assert(off==body.size());
    };
    AirportItlwmAWDLInterface merged;
    SyncEngine peers[9];
    for(unsigned k=0;k<9;++k) {
        uint8_t mac[6]={2,9,8,7,6,uint8_t(k+1)};
        peers[k].start(mac,44,2000000);
    }
    // 8 peers interleave split TLV 2 records without thrashing each other.
    for(unsigned k=0;k<8;++k) {
        auto f1=makeServiceFrame(peers[k],2004000+k*100,true,uint16_t(215+k),{uint8_t(10+k)});
        merged.receiveAction(&merged,f1.data(),f1.size(),44,-45,2004000+k*100);
    }
    for(unsigned k=0;k<8;++k) {
        auto f2=makeServiceFrame(peers[k],2010000+k*100,true,uint16_t(215+k),{uint8_t(30+k)});
        merged.receiveAction(&merged,f2.data(),f2.size(),44,-45,2010000+k*100);
        std::vector<uint8_t> ids;countTlv2(merged.payload,ids);
        assert((ids==std::vector<uint8_t>{uint8_t(30+k),uint8_t(10+k)}));
    }
    // PSF (subtype 0) carries TLV 6 without receiving merged TLV 2 entries.
    auto psfSame=makeServiceFrame(peers[0],2020000,false,215,{});
    merged.receiveAction(&merged,psfSame.data(),psfSame.size(),44,-45,2020000);
    std::vector<uint8_t> psfIds;countTlv2(merged.payload,psfIds);
    assert(psfIds.empty());
    // A PSF bumping SUI (215 -> 218) immediately invalidates withdrawn SUI 215 records.
    auto psfNewSui=makeServiceFrame(peers[0],2025000,false,218,{});
    merged.receiveAction(&merged,psfNewSui.data(),psfNewSui.size(),44,-45,2025000);
    auto mifNewSui=makeServiceFrame(peers[0],2030000,true,218,{99});
    merged.receiveAction(&merged,mifNewSui.data(),mifNewSui.size(),44,-45,2030000);
    countTlv2(merged.payload,psfIds);
    assert((psfIds==std::vector<uint8_t>{99}));
    // 9th peer evicts only the oldest LRU peer (peer 1 at 2010100), keeping peer 0 (seen at 2030000).
    auto peer9=makeServiceFrame(peers[8],2035000,true,500,{77});
    merged.receiveAction(&merged,peer9.data(),peer9.size(),44,-45,2035000);
    auto mifPeer0Again=makeServiceFrame(peers[0],2040000,true,218,{100});
    merged.receiveAction(&merged,mifPeer0Again.data(),mifPeer0Again.size(),44,-45,2040000);
    countTlv2(merged.payload,psfIds);
    assert((psfIds==std::vector<uint8_t>{100,99}));
#endif
}
