#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <vector>
#define AIRPORT_VTD 1
#define MIN(a,b) std::min((a),(b))
#define MAX(a,b) std::max((a),(b))
using UInt32=uint32_t;
using bus_size_t=uint32_t;
struct Arena;
struct Buffer {uint32_t size=0;Arena *arena=nullptr;void *vaddr=nullptr;uint64_t address=0;};
struct Arena {
    std::vector<uint8_t> bytes;
    bool allocate(uint32_t size,uint32_t alignment,Buffer &buffer) {
        assert(alignment==4096);bytes.assign(size,0);
        buffer={size,this,bytes.data(),0x100000000ULL};return true;
    }
};
using bus_dma_tag_t=Arena *;
struct bus_dmamap {Buffer dma;uint32_t maxSegmentSize=0,maxSegments=0;};
using bus_dmamap_t=bus_dmamap *;
struct Packet {std::vector<uint8_t> bytes;};
using mbuf_t=Packet *;
static size_t mbuf_pkthdr_len(mbuf_t p){return p->bytes.size();}
static size_t mbuf_len(mbuf_t p){return p->bytes.size();}
static mbuf_t mbuf_next(mbuf_t){return nullptr;}
static int mbuf_copydata(mbuf_t p,size_t offset,size_t size,void *out) {
    assert(offset+size<=p->bytes.size());memcpy(out,p->bytes.data()+offset,size);return 0;
}
struct IOPhysicalSegment {uint64_t location,length;};
// PRODUCTION
int main() {
    Arena arena;bus_dmamap_t map=nullptr;
    assert(!bus_dmamap_create(&arena,2048,23,2048,0,0,&map));
    assert(map->dma.size==4096 && map->maxSegments==23 && map->maxSegmentSize==2048);
    for(unsigned size:{140u,3062u}) {
        Packet p;p.bytes.resize(size);for(unsigned i=0;i<size;++i)p.bytes[i]=uint8_t(i);
        IOPhysicalSegment segments[23]={};
        unsigned count=bus_dmamap_packet(map,&p,segments,23,false);
        assert(count==(size>2048?2u:1u));
        std::vector<uint8_t> reconstructed;
        for(unsigned i=0;i<count;++i) {
            assert(segments[i].length<=2048);
            const auto offset=segments[i].location-map->dma.address;
            assert(offset+segments[i].length<=arena.bytes.size());
            reconstructed.insert(reconstructed.end(),arena.bytes.begin()+offset,
                arena.bytes.begin()+offset+segments[i].length);
        }
        assert(reconstructed==p.bytes);
        if(size>2048) {
            assert(segments[0].length==2048 && segments[1].length==1014);
            IOPhysicalSegment shortList[2]={};
            // The real TX descriptor supplies all 23 slots; a two-slot map also fits.
            assert(bus_dmamap_packet(map,&p,shortList,2,false)==2);
        }
    }
    Packet tooLarge;tooLarge.bytes.resize(4097);IOPhysicalSegment segments[23]={};
    assert(!bus_dmamap_packet(map,&tooLarge,segments,23,false));
    delete map;
}
