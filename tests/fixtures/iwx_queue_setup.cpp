#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include "itlwm/hal_iwx/TxQueueSetup.hpp"
#include "AirportItlwm/AWDL/RadioState.hpp"
#define AIRPORT_VTD 1
#define AIRPORT_WCL 1
#define htole16(x) (x)
#define htole32(x) (x)
#define htole64(x) (x)
#define le16toh(x) (x)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))
#define XYLog(...) ((void)0)
#define IWX_TFD_QUEUE_CB_SIZE(x) (__builtin_ctz(x)-3)
#define IWX_AGG_SSN_TO_TXQ_IDX(x,n) ((x)&((n)-1))
#define __packed __attribute__((packed))
// FIRMWARE DECLARATIONS
enum { IWX_INVALID_QUEUE=0xffff, IWX_MIN_256_BA_QUEUE_SIZE_GEN3=1024,
    IWX_DEVICE_FAMILY_AX210=22500, IWX_FIRST_TB_SIZE_ALIGN=64,
    IWX_TX_QUEUE_CFG_ENABLE_QUEUE=1, IWX_SCD_QUEUE_CFG=29,
    IWX_CMD_WANT_RESP=1, IWX_CMD_FAILED_MSK=0x40,
    IWX_TFH_NUM_TBS=25, MCLBYTES=2048, BUS_DMA_NOWAIT=0 };
using bus_addr_t=uint64_t;
using IOReturn=int;
struct OSObject {};
struct Arena {
    unsigned live=0,allocations=0,failAt=0;
    bool deferred=false;
    void deferRecycling() {deferred=true;}
    void *allocate(unsigned size) {
        if(++allocations==failAt)return nullptr;
        ++live;return calloc(1,size);
    }
    void release(void *p) {if(p){assert(live);--live;free(p);}}
};
struct Mapping {void *vaddr;uint64_t paddr;};
struct iwx_tfh_tfd {uint8_t bytes[256];};
struct iwx_device_cmd {uint8_t bytes[128];};
struct iwx_gen3_bc_tbl {uint16_t bytes[1024];};
struct iwx_agn_scd_bc_tbl {uint16_t bytes[320];};
struct iwx_tx_data {uint64_t cmd_paddr;void *map;};
struct iwx_tx_ring {
    uint32_t qid,ring_count,cur,tail,queued;
    Mapping desc_dma,bc_tbl,cmd_dma;
    iwx_tfh_tfd *desc;iwx_device_cmd *cmd;
    iwx_tx_data data[1024];
};
struct iwx_softc {
    Arena *sc_dmat;
    int sc_device_family=22000,first_data_qid=2;
    iwx_tx_ring txq[16]={};
    struct {int qid;} sc_tid_data[9]={};
};
struct iwx_rx_packet {struct {uint8_t group_id;} hdr;size_t length;alignas(8) uint8_t data[8];};
struct iwx_host_cmd {unsigned id,flags;size_t resp_pkt_len;const void *data[1];size_t len[1];iwx_rx_packet *resp_pkt;};
static size_t iwx_rx_packet_payload_len(const iwx_rx_packet *p) {return p->length;}
static int bus_dmamap_create(Arena *a,unsigned size,int,unsigned,int,int,void **map) {
    *map=a->allocate(size);return *map?0:ENOMEM;
}
struct WorkLoop {bool gated=true;bool inGate(){return gated;}};
struct Gate {
    WorkLoop *loop;
    IOReturn runAction(IOReturn (*f)(OSObject*,void*,void*,void*,void*),void *a,void *b) {
        loop->gated=true;int r=f(nullptr,a,b,nullptr,nullptr);loop->gated=false;return r;
    }
};
struct ItlIwx {
    Arena arena; iwx_softc com{&arena};WorkLoop loop;Gate gate{&loop};
    AirportAWDL::RadioState awdlRadio={};
    bool dmaDetaching=false,missingReply=false,resetDuringSubmit=false;
    uint64_t dmaEpoch=1;
    unsigned sendCalls=0,responseFrees=0,resets=0,queueSize=256;
    int sendError=0;
    iwx_rx_packet packet={};
    iwx_tx_queue_cfg_rsp response{7,0,0,0};
    ItlIwx(){packet.length=sizeof(response);awdlRadio.queue=IWX_INVALID_QUEUE;}
    ~ItlIwx(){for(auto &r:com.txq)iwx_free_tx_ring(&com,&r);assert(!arena.live);}
    WorkLoop *getMainWorkLoop(){return &loop;}
    Gate *getMainCommandGate(){return &gate;}
    unsigned getTxQueueSize(){return queueSize;}
    void iwx_tx_ring_init(iwx_softc*,iwx_tx_ring *r,unsigned size){r->ring_count=size;}
    int iwx_dma_contig_alloc(Arena *a,Mapping *m,unsigned size,unsigned){
        m->vaddr=a->allocate(size);m->paddr=reinterpret_cast<uintptr_t>(m->vaddr);return m->vaddr?0:1;
    }
    void iwx_reset_tx_ring(iwx_softc*,iwx_tx_ring *r){
        assert(r==&com.txq[response.queue_number] && !r->queued);++resets;
    }
    void iwx_free_tx_ring(iwx_softc*,iwx_tx_ring *r){
        arena.release(r->desc_dma.vaddr);arena.release(r->bc_tbl.vaddr);arena.release(r->cmd_dma.vaddr);
        for(unsigned i=0;i<r->ring_count;++i)arena.release(r->data[i].map);
        *r={};
    }
    int iwx_send_cmd(iwx_softc*,iwx_host_cmd *h){
        assert(loop.gated && h->id==29 && h->len[0]==24);++sendCalls;
        auto c=static_cast<const iwx_tx_queue_cfg_cmd*>(h->data[0]);
        assert(c->sta_id==3 && c->tid==15 && c->flags==1);
        assert(c->cb_size==3 && c->byte_cnt_addr && c->tfdq_addr);
        memcpy(packet.data,&response,sizeof(response));
        h->resp_pkt=missingReply?nullptr:&packet;
        if(resetDuringSubmit)++dmaEpoch;
        return sendError;
    }
    void iwx_free_resp(iwx_softc*,iwx_host_cmd*){++responseFrees;}
    int iwx_tvqm_enable_txq(iwx_softc*,int,int,uint32_t,uint8_t,IwxTxQueueSetup* = nullptr);
    int run(IwxTxQueueSetup &result){return iwx_tvqm_enable_txq(&com,15,0,64,3,&result);}
};
// PRODUCTION
int main(){
    for(unsigned counter:{0u,63u,64u,255u,256u,0x1234u,0xffffu}) {
        ItlIwx d;d.response.write_pointer=counter;d.loop.gated=false;
        IwxTxQueueSetup result={};assert(d.run(result)==7 && !d.loop.gated);
        assert(result.stage==IwxTxQueueSetup::Ready && !result.error);
        assert(result.writePointer==counter && result.responseFlags==0 && result.responseLength==8);
        assert(d.com.txq[7].cur==(counter&255) && d.com.txq[7].tail==(counter&63));
        assert(!d.arena.deferred && d.arena.live==67);
    }
    for(unsigned failure=1;failure<=67;++failure){
        ItlIwx d;d.arena.failAt=failure;IwxTxQueueSetup result={};
        assert(d.run(result)==-ENOMEM && result.error==ENOMEM);
        assert(result.stage==(failure==1?IwxTxQueueSetup::AllocateDescriptors:
            failure==2?IwxTxQueueSetup::AllocateByteCounts:
            failure==3?IwxTxQueueSetup::AllocateCommands:IwxTxQueueSetup::AllocatePacketMap));
        if(failure>3)assert(result.packetIndex==failure-4);
        assert(!d.arena.live && !d.arena.deferred && !d.sendCalls);
    }
    {
        ItlIwx d;d.com.sc_device_family=IWX_DEVICE_FAMILY_AX210;d.queueSize=65536;
        d.response.write_pointer=0xffff;IwxTxQueueSetup result={};
        assert(d.run(result)==7 && d.com.txq[7].cur==65535 && d.com.txq[7].tail==63);
    }
    // The 9462 returned queue 2, flags 1, write pointer 0 on Preview 32.
    // ADD_STA v12 leaves the preallocated queue 2 free (management uses 1).
    {
        ItlIwx d;d.com.first_data_qid=1;d.response={2,1,0,0};
        d.com.txq[2].qid=2;d.com.txq[2].ring_count=256;
        d.com.txq[2].desc_dma.vaddr=d.arena.allocate(256);
        d.com.txq[2].bc_tbl.vaddr=d.arena.allocate(256);
        d.com.txq[2].cmd_dma.vaddr=d.arena.allocate(256);
        d.com.txq[2].data[0].map=d.arena.allocate(256);
        IwxTxQueueSetup result={};assert(d.run(result)==2);
        assert(result.stage==IwxTxQueueSetup::Ready && result.responseFlags==1);
        assert(result.firstDataQueue==1 && result.owner==0);
        assert(d.com.txq[2].ring_count==64 && d.com.txq[2].qid==2);
        assert(!d.arena.deferred && d.arena.live==67);
    }
    for(unsigned scenario=0;scenario<12;++scenario){
        ItlIwx d;IwxTxQueueSetup result={};auto expected=IwxTxQueueSetup::Submit;int error=EIO;
        d.response.flags=1;
        switch(scenario){
        case 0:d.sendError=ETIMEDOUT;error=ETIMEDOUT;break;
        case 1:d.resetDuringSubmit=true;error=ENXIO;break;
        case 2:d.missingReply=true;expected=IwxTxQueueSetup::ResponseHeader;break;
        case 3:d.packet.length=4;expected=IwxTxQueueSetup::ResponseLength;break;
        case 4:d.com.sc_tid_data[0].qid=7;expected=IwxTxQueueSetup::QueueBusy;break;
        case 5:d.response.queue_number=16;expected=IwxTxQueueSetup::QueueIndex;break;
        case 6:d.response.queue_number=2;expected=IwxTxQueueSetup::QueueReserved;break;
        case 7:d.com.txq[7].queued=1;expected=IwxTxQueueSetup::QueueBusy;break;
        case 8:d.awdlRadio.queue=7;expected=IwxTxQueueSetup::QueueBusy;break;
        case 9:d.packet.hdr.group_id=IWX_CMD_FAILED_MSK;expected=IwxTxQueueSetup::ResponseHeader;break;
        case 10:d.awdlRadio.dataQueueConfigured=true;d.awdlRadio.dataQueue=7;expected=IwxTxQueueSetup::QueueBusy;break;
        case 11:d.awdlRadio.blockAckTX.configured=true;d.awdlRadio.blockAckTX.queue=7;expected=IwxTxQueueSetup::QueueBusy;break;
        }
        assert(d.run(result)==-error && result.stage==expected && result.error==error);
        assert(!d.arena.live && d.arena.deferred && !d.resets);
        if(scenario==4)assert(d.com.sc_tid_data[0].qid==7);
        if(scenario==7)assert(d.com.txq[7].queued==1);
        const unsigned owners[]={0,0,0,0,IwxTxQueueSetup::Infrastructure,0,
            IwxTxQueueSetup::Reserved,IwxTxQueueSetup::Pending,IwxTxQueueSetup::AWDL,0,IwxTxQueueSetup::AWDL,IwxTxQueueSetup::AWDL};
        assert(result.owner==owners[scenario]);
    }
    ItlIwx d;d.dmaDetaching=true;IwxTxQueueSetup result={};
    assert(d.run(result)==-EINVAL && result.stage==IwxTxQueueSetup::Validate && !d.arena.allocations);
}
