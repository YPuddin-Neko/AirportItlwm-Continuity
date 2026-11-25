#include <cassert>
#include <cstdint>
#include <sys/time.h>
#include <libkern/OSByteOrder.h>
#include "itlwm/hal_iwx/ReceiveClock.hpp"
#define le32toh OSSwapLittleToHostInt32
enum { IWX_DEVICE_FAMILY_22000=1, IWX_FLAG_SHUTDOWN=0x100,
       IWX_CSR_GP_CNTRL=0x24, IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ=8 };
struct iwx_softc { unsigned sc_flags=0; int sc_device_family=IWX_DEVICE_FAMILY_22000; };
struct iwx_rx_mpdu_desc { struct { uint32_t gp2_on_air_rise; } v1; };
static uint64_t times[2];
static unsigned timeIndex;
static void getmicrouptime(timeval *t) {
    assert(timeIndex<2);
    auto now=times[timeIndex++];t->tv_sec=now/1000000;t->tv_usec=now%1000000;
}
static unsigned cleared;
static void clear(iwx_softc *,unsigned reg,unsigned bits) {
    assert(reg==IWX_CSR_GP_CNTRL && bits==IWX_CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
    ++cleared;
}
#define IWX_CLRBITS clear
struct ItlIwx {
    iwx_softc com;
    struct { bool ready=true, awake=true; } awdlRadio;
    IwxReceiveClock awdlReceiveClock={};
    bool lockOK=true, locked=false;
    unsigned locks=0, reads=0, unlocks=0;
    uint32_t gp2=0;
    int iwx_nic_lock(iwx_softc *sc,int timeout) {
        assert(sc==&com && timeout==100 && !locked);
        ++locks;locked=lockOK;return lockOK;
    }
    uint32_t iwx_read_prph(iwx_softc *sc,uint32_t reg) {
        assert(sc==&com && locked && reg==0xa02c68);++reads;return gp2;
    }
    void iwx_nic_unlock(iwx_softc *sc) { assert(sc==&com && locked);locked=false;++unlocks; }
    uint32_t awdlReceiveDelay(const iwx_rx_mpdu_desc *,uint64_t);
};

// PRODUCTION

static void mapping() {
    IwxReceiveClock clock={};
    // The same air arrival must yield the same epoch despite different RX delays.
    assert(clock.delay(100000,100000,100020,10010,9000)==1000);
    assert(clock.delay(110000,110000,110020,20010,9000)==11000);
    assert(clock.mapped==2 && clock.lastDelayUS==11000 && clock.maxDelayUS==11000);
    // Both zero and UINT32_MAX are valid counter readings, including across wrap.
    assert(clock.delay(200000,200000,200000,0,0xfffffff0)==16);
    assert(clock.delay(200000,200000,200000,0xffffffff,0xfffffff0)==15);
    assert(clock.delay(200000,200000,200000,7,0)==7);
    assert(clock.delay(200000,200000,200000,123,123)==0);
    // Midpoint uncertainty may place a just-received frame slightly in the future.
    assert(clock.delay(300000,300000,300020,123,123)==0 && clock.mapped==7);
    auto mapped=clock.mapped;
    assert(clock.delay(300000,300000,300020,123,124)==0); // future GP2
    assert(clock.delay(300000,300000,300020,1000001,0)==0); // stale
    assert(clock.delay(300000,300000,300257,1000,0)==0); // wide host bracket
    assert(clock.delay(300000,299999,300010,1000,0)==0); // uptime reversal
    assert(clock.delay(300000,300020,300010,1000,0)==0);
    assert(clock.delay(300000,300100,300120,1000,1000)==0); // impossible arrival
    assert(clock.delay(1,1,1,1000,0)==0); // cannot precede boot
    assert(clock.mapped==mapped && clock.rejected==7);
    // No accumulated offset remains when firmware restarts its timer.
    assert(clock.delay(10000000,10000000,10000000,100,90)==10);
    assert(clock.delay(10000001,10000001,10000001,0,0)==0);
    assert(clock.delay(UINT64_MAX-10,UINT64_MAX-10,UINT64_MAX,1000,990)==5);
    assert(clock.delay(2000000,2000000,2000000,1000000,0)==1000000);
    assert(clock.maxDelayUS==1000000);
}

static uint32_t read(ItlIwx &d,uint32_t rx,uint32_t current,uint64_t span=20) {
    timeIndex=0;times[0]=1000000;times[1]=1000000+span;
    d.gp2=current;iwx_rx_mpdu_desc desc={};desc.v1.gp2_on_air_rise=OSSwapHostToLittleInt32(rx);
    return d.awdlReceiveDelay(&desc,1000000);
}

static void hardware() {
    ItlIwx d;
    assert(read(d,9000,10010)==1000);
    assert(d.locks==1 && d.reads==1 && d.unlocks==1 && !d.locked);
    d.awdlRadio.ready=false;assert(read(d,9000,10010)==0);
    d.awdlRadio.ready=true;d.awdlRadio.awake=false;assert(read(d,9000,10010)==0);
    d.awdlRadio.awake=true;d.com.sc_flags=IWX_FLAG_SHUTDOWN;assert(read(d,9000,10010)==0);
    d.com.sc_flags=0;d.com.sc_device_family=2;assert(read(d,9000,10010)==0);
    assert(d.locks==1 && d.awdlReceiveClock.unavailable==4);
    d.com.sc_device_family=IWX_DEVICE_FAMILY_22000;d.lockOK=false;
    assert(read(d,9000,10010)==0 && cleared==1);
    assert(d.reads==1 && d.unlocks==1 && !d.locked && d.awdlReceiveClock.unavailable==5);
    d.lockOK=true;assert(read(d,9000,10010,257)==0);
    assert(d.reads==2 && d.unlocks==2 && d.awdlReceiveClock.rejected==1);
    assert(read(d,9000,10010)==1000 && d.awdlReceiveClock.mapped==2);
}

int main() { mapping();hardware(); }
