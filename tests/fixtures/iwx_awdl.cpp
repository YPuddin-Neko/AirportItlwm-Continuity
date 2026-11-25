#include <cstdio>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <sys/time.h>
#include <libkern/OSByteOrder.h>
#include "AirportItlwm/AWDL/DataFrame.hpp"
#include "AirportItlwm/AWDL/BlockAck.hpp"
#include "AirportItlwm/AWDL/RadioState.hpp"
#define htole16 OSSwapHostToLittleInt16
#define le16toh OSSwapLittleToHostInt16
#define le32toh OSSwapLittleToHostInt32
#define le64toh OSSwapLittleToHostInt64
#define AIRPORT_WCL
#define MIN(a,b) std::min((a),(b))
#define MAX(a,b) std::max((a),(b))
#define IC2IFP(ic) (&(ic)->ifp)
#define mtod(m,t) reinterpret_cast<t>((m)->data)
#define IEEE80211_ADDR_EQ(a,b) (std::memcmp((a),(b),6)==0)

struct Packet { uint8_t *data; size_t length, total; unsigned frees; };
using mbuf_t = Packet *;
static size_t mbuf_len(mbuf_t m) { return m->length; }
static size_t mbuf_pkthdr_len(mbuf_t m) { return m->total; }
static void *mbuf_data(mbuf_t m) { return m->data; }
static void mbuf_freem(mbuf_t m) { assert(m->frees++==0); }
static void mbuf_setdata(mbuf_t m,void *p,size_t n) { m->data=(uint8_t*)p;m->length=n; }
static void mbuf_pkthdr_setlen(mbuf_t m,size_t n) { m->total=n; }
static void mbuf_setlen(mbuf_t m,size_t n) { m->length=n; }
static void mbuf_adj(mbuf_t m,size_t n) { m->data+=n;m->length-=n;m->total-=n; }

enum { IEEE80211_M_STA=1, IEEE80211_M_MONITOR=2,
       IEEE80211_FC0_TYPE_MASK=0x0c, IEEE80211_FC0_TYPE_CTL=4,
       IEEE80211_FC0_SUBTYPE_MASK=0xf0, IEEE80211_FC0_SUBTYPE_CTS=0xc0,
       IEEE80211_FC0_SUBTYPE_ACK=0xd0, IEEE80211_CCMP_HDRLEN=8,
       IEEE80211_FC0_SUBTYPE_DEAUTH=0xc0, IEEE80211_FC0_SUBTYPE_DISASSOC=0xa0,
       IEEE80211_RXI_HWDEC_SAME_PN=1, IEEE80211_QOS_AMSDU=0x80,
       IEEE80211_ADDR_LEN=6, IEEE80211_FC1_DIR_MASK=3, IEEE80211_FC1_DIR_DSTODS=3,
       IEEE80211_FC1_DIR_FROMDS=2,
       IEEE80211_FC0_SUBTYPE_QOS=0x80, IEEE80211_FC0_TYPE_DATA=8,
       IEEE80211_FC0_TYPE_MGT=0, IEEE80211_FC1_ORDER=0x80, IEEE80211_CATEG_BA=3 };
using u8=uint8_t;using __le16=uint16_t;using __be16=uint16_t;
using __le32=uint32_t;using __le64=uint64_t;
#define offsetofend(t,m) (offsetof(t,m)+sizeof(((t*)0)->m))
// FIRMWARE DECLARATIONS
static_assert(IWX_RX_DESC_SIZE_V1==48 && sizeof(iwx_rx_mpdu_desc)==64,"RX wire sizes");
struct ieee80211_frame { uint8_t i_fc[2],i_dur[2],i_addr1[6],i_addr2[6],i_addr3[6],i_seq[2]; };
static const uint8_t etherbroadcastaddr[6]={255,255,255,255,255,255};
struct ieee80211_frame_cts { uint8_t bytes[10]; };
struct ieee80211_frame_ack { uint8_t bytes[10]; };
struct ieee80211_frame_min { uint8_t bytes[16]; };
struct ieee80211_qosframe { ieee80211_frame h; uint16_t i_qos[1]; };
struct ieee80211_qosframe_addr4 { ieee80211_frame h;uint8_t addr4[6];uint16_t i_qos[1]; };
#define _KASSERT assert
// HEADER LENGTH
struct Stats { int inputErrors; };
struct Ifnet { Stats stats; Stats *netStat=&stats; };
struct ieee80211_node { uint8_t ni_bssid[6]={2,0xaa,0xbb,0xcc,0xdd,1}; };
struct ieee80211com {
    int ic_opmode=IEEE80211_M_STA,ic_max_rssi=100;
    uint8_t ic_myaddr[6]={2,0,0,0,0,1};
    ieee80211_node bss;
    ieee80211_node *ic_bss=&bss;
    struct { int is_rx_tooshort, is_rx_wrongbss; } ic_stats;
    Ifnet ifp;
};
struct iwx_softc { ieee80211com sc_ic; int sc_device_family=0; };
struct ieee80211_rxinfo { int rxi_flags,rxi_rssi,rxi_chan;uint32_t rxi_tstamp; };
struct mbuf_list {};
static uint64_t fixtureNowUS=42000000;
static void getmicrouptime(timeval *t) {
    t->tv_sec=fixtureNowUS/1000000;t->tv_usec=fixtureNowUS%1000000;
}

// RX DIAGNOSTIC DECLARATIONS
struct ItlIwx {
    bool receiveAWDLTXBlockAck(const uint8_t *,size_t) {return false;}
    iwx_softc com;
    uint64_t dmaEpoch = 7;
    AirportAWDL::ReceiveState awdlReceive={};
    AirportAWDL::PeerTable awdlPeers={};
    AirportAWDL::DataReceiveState awdlData={};
    AirportAWDL::RadioState awdlRadio={};
    uint64_t awdlNonAPDataDropped=0;
    void *awdlReceiver=nullptr;
    void (*awdlInput)(void *, const AirportAWDL::DataView &)=nullptr;
    void (*awdlActionInput)(void *, const uint8_t *, size_t, uint16_t, int16_t, uint64_t)=nullptr;
    void (*awdlBlockAckInput)(void *, const AirportAWDL::BlockAckRequest &)=nullptr;
    uint64_t awdlPublishedSecond=0;
    int publications=0, crypto=0, duplicates=0, forwarded=0;
    uint32_t rxDelay=0;
    unsigned clockCalls=0;
    uint32_t awdlReceiveDelay(const iwx_rx_mpdu_desc *desc,uint64_t nowUS) {
        assert(desc && nowUS==fixtureNowUS);++clockCalls;return rxDelay;
    }
    IwxAWDLReceiveDiagnostics awdlReceiveDiagnostics = {};
    void observeAWDLReceiveDescriptor(const iwx_rx_mpdu_desc *,size_t);
    void formatAWDLReceiveDiagnostics(char *,size_t) const;
    bool receiveAWDL(mbuf_t,iwx_rx_mpdu_desc *);
    void resetAWDLPeers();
    void publishAWDLReceive() { ++publications; }
    int iwx_rxmq_get_signal_strength(iwx_softc *,iwx_rx_mpdu_desc *);
    void iwx_rx_mpdu_mq(iwx_softc *,mbuf_t,void *,size_t,mbuf_list *);
    int iwx_rx_hwdecrypt(iwx_softc *,mbuf_t,uint16_t,ieee80211_rxinfo *) { ++crypto;return 0; }
    int iwx_detect_duplicate(iwx_softc *,mbuf_t,iwx_rx_mpdu_desc *,ieee80211_rxinfo *) { ++duplicates;return 0; }
    bool receiveAWDLReordered(mbuf_t,iwx_rx_mpdu_desc *,mbuf_list *) { return false; }
    bool receiveAWDLBlockAckEnd(const AirportAWDL::BlockAckRequest &,bool=false) {return false;}
    int iwx_rx_reorder(iwx_softc *,mbuf_t,int,iwx_rx_mpdu_desc *,int,int,uint32_t,ieee80211_rxinfo *,mbuf_list *) { return 0; }
    void iwx_rx_frame(iwx_softc *,mbuf_t m,int,uint16_t,int,int,uint32_t,ieee80211_rxinfo *,mbuf_list *) {
        ++forwarded;mbuf_freem(m);
    }
};

// PRODUCTION

static std::vector<uint8_t> frame() {
    std::vector<uint8_t> b(24,0);
    b[0]=0xd0;b[10]=2;
    std::fill(b.begin()+4,b.begin()+10,0xff);
    const uint8_t action[]={127,0,0x17,0xf2,8,0x10,0,0,0,0,0,0,0,0,0,0,21,2,0,0x31,2};
    b.insert(b.end(),std::begin(action),std::end(action));
    return b;
}

static void run(ItlIwx &driver,std::vector<uint8_t> b,unsigned flags=0,
                unsigned status=3, bool v3=false,unsigned macMask=0,unsigned macPhy=0) {
    if(flags & IWX_RX_MPDU_MFLG2_PAD)
        b.insert(b.begin()+ieee80211_get_hdrlen(reinterpret_cast<const ieee80211_frame *>(b.data())),2,0);
    const size_t descSize=v3?sizeof(iwx_rx_mpdu_desc):IWX_RX_DESC_SIZE_V1;
    size_t len=b.size();
    std::vector<uint8_t> storage(descSize+len);
    auto *desc=reinterpret_cast<iwx_rx_mpdu_desc *>(storage.data());
    desc->status=status;desc->mpdu_len=len;desc->mac_flags2=flags;desc->mac_phy_idx=macPhy;
    if(v3) {desc->v3.energy_a=70;desc->v3.energy_b=50;desc->v3.channel=149;desc->v3.mac_context=macMask;}
    else {desc->v1.energy_a=45;desc->v1.energy_b=60;desc->v1.channel=44;desc->v1.mac_context=macMask;}
    driver.com.sc_device_family=v3 ? IWX_DEVICE_FAMILY_AX210 : IWX_DEVICE_FAMILY_22000;
    std::memcpy(storage.data()+descSize,b.data(),len);
    Packet m={storage.data(),storage.size(),storage.size(),0};
    driver.iwx_rx_mpdu_mq(&driver.com,&m,desc,storage.size(),nullptr);
    assert(m.frees==1);
}

static void bounds(ItlIwx &driver) {
    // A short reported descriptor must be rejected before subtracting its size.
    auto b=frame();
    std::vector<uint8_t> storage(IWX_RX_DESC_SIZE_V1+b.size());
    auto *desc=reinterpret_cast<iwx_rx_mpdu_desc *>(storage.data());
    desc->status=3;desc->mpdu_len=b.size();
    std::memcpy(storage.data()+IWX_RX_DESC_SIZE_V1,b.data(),b.size());
    unsigned observed=driver.awdlReceive.valid;
    Packet packet={storage.data(),storage.size(),storage.size(),0};
    driver.iwx_rx_mpdu_mq(&driver.com,&packet,desc,IWX_RX_DESC_SIZE_V1-1,nullptr);
    assert(packet.frees==1 && driver.awdlReceive.valid==observed);

    // The firmware padding flag requires two bytes beyond the full header/IV.
    for(unsigned size : {24U,25U,32U,33U}) {
        std::vector<uint8_t> shortPacket(IWX_RX_DESC_SIZE_V1+size);
        desc=reinterpret_cast<iwx_rx_mpdu_desc *>(shortPacket.data());
        desc->status=3 | (size>=32 ? IWX_RX_MPDU_RES_STATUS_SEC_CCM_ENC : 0);
        desc->mpdu_len=size;desc->mac_flags2=IWX_RX_MPDU_MFLG2_PAD;
        unsigned forwarded=driver.forwarded;
        Packet m={shortPacket.data(),shortPacket.size(),shortPacket.size(),0};
        driver.iwx_rx_mpdu_mq(&driver.com,&m,desc,shortPacket.size(),nullptr);
        assert(m.frees==1 && driver.forwarded==int(forwarded));
    }
}

static void infrastructureOwnership() {
    for(bool v3:{false,true}) for(unsigned mask:{0U,1U,2U,3U,255U}) {
        ItlIwx driver;driver.awdlRadio.mac=true;
        std::vector<uint8_t> ap(40,0);
        ap[0]=8;ap[1]=2;
        AirportAWDL::copyAddress(ap.data()+4,driver.com.sc_ic.ic_myaddr);
        AirportAWDL::copyAddress(ap.data()+10,driver.com.sc_ic.ic_bss->ni_bssid);
        auto receive=[&](const std::vector<uint8_t> &p) {run(driver,p,0,3,v3,mask);};
        receive(ap);
        auto encrypted=ap;encrypted[1]|=0x40;receive(encrypted);
        auto group=ap;group[4]=1;receive(group);
        assert(driver.forwarded==3 && driver.duplicates==3 && driver.crypto==3);
        auto foreign=ap;foreign[10]^=4;receive(foreign);
        foreign=ap;foreign[9]^=4;receive(foreign);
        for(unsigned direction:{0U,1U,3U}) {foreign=ap;foreign[1]=direction;receive(foreign);}
        assert(driver.awdlNonAPDataDropped==5);
        assert(driver.forwarded==3 && driver.duplicates==3 && driver.crypto==3);
        driver.com.sc_ic.ic_bss=nullptr;receive(ap);
        assert(driver.awdlNonAPDataDropped==6 && driver.forwarded==3);
        driver.com.sc_ic.ic_bss=&driver.com.sc_ic.bss;
        // Management/scan input remains available regardless of the context mask.
        auto beacon=ap;beacon[0]=0x80;beacon[1]=0;beacon[10]^=4;receive(beacon);
        assert(driver.forwarded==4 && driver.awdlNonAPDataDropped==6);
        // With no AWDL MAC, retain the existing infrastructure receive behavior.
        driver.awdlRadio.mac=false;receive(foreign);
        assert(driver.forwarded==5);
    }
}

static void blockAckOwnership() {
    ItlIwx driver;
    auto &ap=driver.com.sc_ic.bss;
    const uint8_t request[]={3,0,0x48,3,0x10,0,0,0,0};
    for(bool htc:{false,true}) {
        auto b=frame();b.resize(htc?28:24);
        b[1]=htc?0x80:0;
        if(htc) b[24]=0x7f;
        AirportAWDL::copyAddress(b.data()+16,AirportAWDL::BSSID);
        AirportAWDL::copyAddress(b.data()+4,driver.com.sc_ic.ic_myaddr);
        const size_t header=b.size();
        b.insert(b.end(),std::begin(request),std::end(request));
        const int before=driver.forwarded;
        for(uint8_t action:{0,1,2}) {
            b[header+1]=action;
            run(driver,b,IWX_RX_MPDU_MFLG2_PAD);
        }
        b[9]^=2;run(driver,b); // A negotiation addressed to another AWDL peer.
        auto shortBA=b;shortBA.resize(header+1);run(driver,shortBA);
        assert(driver.forwarded==before && driver.crypto==before && driver.duplicates==before);
        assert(!driver.awdlReceive.valid && !driver.awdlReceive.malformed && !driver.awdlReceive.unsupported);

        b[header]=4;run(driver,b); // Unrelated category follows existing handling.
        shortBA.resize(header);run(driver,shortBA); // No category byte available.
        b[header]=3;
        AirportAWDL::copyAddress(b.data()+10,ap.ni_bssid);
        AirportAWDL::copyAddress(b.data()+16,ap.ni_bssid);
        run(driver,b); // Negotiation with the associated AP.
        auto original=ap;
        AirportAWDL::copyAddress(ap.ni_bssid,AirportAWDL::BSSID);
        AirportAWDL::copyAddress(b.data()+10,ap.ni_bssid);
        AirportAWDL::copyAddress(b.data()+16,ap.ni_bssid);
        run(driver,b); // The AP can itself use the same BSSID.
        ap=original;
        assert(driver.forwarded==before+4 && driver.crypto==before+4 && driver.duplicates==before+4);
    }
}

static void blockAckRequests() {
    ItlIwx driver;driver.awdlRadio.ready=true;
    AirportAWDL::copyAddress(driver.awdlRadio.address,driver.com.sc_ic.ic_myaddr);
    std::vector<AirportAWDL::BlockAckRequest> requests;
    driver.awdlReceiver=&requests;
    driver.awdlBlockAckInput=[](void *context,const AirportAWDL::BlockAckRequest &r) {
        static_cast<std::vector<AirportAWDL::BlockAckRequest> *>(context)->push_back(r);
    };
    for(bool htc:{false,true}) for(unsigned tid=0;tid<16;++tid) {
        auto b=frame();b.resize(htc?37:33);b[1]=htc?0x80:0;
        AirportAWDL::copyAddress(b.data()+4,driver.awdlRadio.address);
        AirportAWDL::copyAddress(b.data()+16,AirportAWDL::BSSID);
        const unsigned header=htc?28:24;
        std::fill(b.begin()+24,b.end(),0);
        b[header]=3;b[header+2]=100;
        AirportAWDL::putLE16(b.data()+header+3,uint16_t(3|(tid<<2)|(64<<6)));
        const size_t previous=requests.size();
        run(driver,b,IWX_RX_MPDU_MFLG2_PAD);
        assert(requests.size()==previous+1);
        assert(requests.back().tid==tid && requests.back().dialog==100);
        const size_t accepted=requests.size();
        b[1]|=8;run(driver,b);assert(requests.size()==accepted+1);b[1]&=~8;
        const size_t count=requests.size();
        for(unsigned n=24;n<b.size();++n) {
            auto shortFrame=b;shortFrame.resize(n);run(driver,shortFrame);
            assert(requests.size()==count);
        }
        for(unsigned variant=0;variant<12;++variant) {
            auto bad=b;
            if(variant==0)bad[4]^=2;
            if(variant==1)bad[10]|=1;
            if(variant==2)std::fill(bad.begin()+10,bad.begin()+16,0);
            if(variant==3)AirportAWDL::copyAddress(bad.data()+10,driver.awdlRadio.address);
            if(variant==4)bad[16]^=2;
            if(variant==5)bad[1]|=4;
            if(variant==6)bad[1]|=0x40;
            if(variant==7)bad[22]|=1;
            if(variant==8)bad[header+1]=1;
            if(variant==9)bad[header+1]=2;
            if(variant==10)bad[header+7]|=1;
            if(variant==11)bad[header]=4;
            run(driver,bad);assert(requests.size()==count);
        }
        driver.awdlRadio.ready=false;run(driver,b);assert(requests.size()==count);
        driver.awdlRadio.ready=true;
        auto ap=b;
        AirportAWDL::copyAddress(ap.data()+10,driver.com.sc_ic.bss.ni_bssid);
        AirportAWDL::copyAddress(ap.data()+16,driver.com.sc_ic.bss.ni_bssid);
        const int forwarded=driver.forwarded;run(driver,ap);
        assert(requests.size()==count && driver.forwarded==forwarded+1);
    }
    assert(requests.size()==64);
}


static void descriptorDiagnostics() {
    ItlIwx driver;driver.awdlRadio.mac=driver.awdlRadio.ready=true;
    AirportAWDL::copyAddress(driver.awdlRadio.address,driver.com.sc_ic.ic_myaddr);
    std::vector<uint8_t> data(26,0);data[0]=0x88;data[10]=2;data[11]=0x22;
    AirportAWDL::copyAddress(data.data()+4,driver.awdlRadio.address);
    AirportAWDL::copyAddress(data.data()+16,AirportAWDL::BSSID);
    AirportAWDL::putLE16(data.data()+22,0xabcd);data[24]=13;
    run(driver,data,0,0x1b000007,false,2,0xa5);
    auto &d=driver.awdlReceiveDiagnostics;
    assert(d.sampled==1 && d.crcOK==1 && d.overrunOK==1 && d.sourceFound==1);
    const auto first=d.samples[0];
    assert(first.status==0x1b000007 && first.macPhy==0xa5 && first.macContext==2);
    assert(first.channel==44 && first.ordinal==1 && first.uptimeUS==42000000 && first.epoch==7);
    assert(first.sequenceControl==0xabcd && first.tid==13 && first.fc0==0x88);
    assert(AirportAWDL::sameAddress(first.receiver,data.data()+4));
    assert(AirportAWDL::sameAddress(first.transmitter,data.data()+10));
    // The diagnostic reads the full status before existing CRC/overflow drops.
    const uint64_t valid=driver.awdlData.valid;
    for(unsigned status:{0U,1U,2U}) run(driver,data,0,status,false,1,0x30);
    assert(d.sampled==4 && d.crcOK==2 && d.crcBad==2 && d.overrunOK==2 && d.overrunBad==2);
    assert(d.sourceFound==1 && d.sourceMissing==3 && driver.awdlData.valid==valid);
    assert(!driver.forwarded && !driver.duplicates && !driver.crypto);
    // Padding follows the optional HTC; BA TID comes from its own parameter set.
    auto ba=data;ba.resize(28,0);ba[0]=0xd0;ba[1]=0x80;
    ba.insert(ba.end(),{3,0,0x55,0x1e,0,0,0,0,0});
    run(driver,ba,IWX_RX_MPDU_MFLG2_PAD,3);
    assert(d.sampled==5 && d.samples[4].tid==7 && d.samples[4].fc1==0x80);
    for(unsigned n=0;n<10;++n) {
        data[1]=n&1?8:0;AirportAWDL::putLE16(data.data()+22,uint16_t(n<<4));
        run(driver,data,0,3);
    }
    assert(d.sampled==15 && d.crcOK+d.crcBad==15 && d.overrunOK+d.overrunBad==15);
    assert(d.sourceFound+d.sourceMissing==15);
    char summary[2048];driver.formatAWDLReceiveDiagnostics(summary,sizeof(summary));
    assert(std::strstr(summary,"ACK=unknown") && std::strstr(summary,"headers=unverified"));
    assert(!std::strstr(summary,"#7 ") && std::strstr(summary,"#8 ") < std::strstr(summary,"#15 "));
    assert(std::strstr(summary,"seq=9 frag=0 tid=13") && std::strlen(summary)<sizeof(summary)-1);
    char tiny[9];std::memset(tiny,0xcc,sizeof(tiny));
    driver.formatAWDLReceiveDiagnostics(tiny,8);assert(!tiny[7] && uint8_t(tiny[8])==0xcc);
    driver.formatAWDLReceiveDiagnostics(nullptr,0);
    // AP/foreign headers, unsupported descriptor versions and truncation cannot enter the ring.
    auto reject=[&](std::vector<uint8_t> b,unsigned flags=0,bool v3=false) {
        run(driver,b,flags,3,v3);assert(d.sampled==15);
    };
    auto b=data;b[4]^=4;reject(b);
    b=data;b[10]|=1;reject(b);
    b=data;AirportAWDL::copyAddress(b.data()+10,driver.awdlRadio.address);reject(b);
    b=data;AirportAWDL::copyAddress(b.data()+10,driver.com.sc_ic.ic_bss->ni_bssid);reject(b);
    b=data;b[16]^=1;reject(b);
    b=data;b[1]=2;reject(b);
    b=data;b.resize(24);reject(b);
    reject(data,0,true);
    driver.awdlRadio.mac=false;reject(data);driver.awdlRadio.mac=true;
    // Direct malformed envelopes exercise the original caller's length checks.
    std::vector<uint8_t> storage(48+data.size());
    auto *desc=reinterpret_cast<iwx_rx_mpdu_desc *>(storage.data());
    desc->status=3;desc->mpdu_len=uint16_t(data.size()+1);
    std::memcpy(storage.data()+48,data.data(),data.size());
    driver.com.sc_device_family=IWX_DEVICE_FAMILY_22000;
    Packet packet={storage.data(),storage.size(),storage.size(),0};
    driver.iwx_rx_mpdu_mq(&driver.com,&packet,desc,storage.size(),nullptr);
    assert(packet.frees==1 && d.sampled==15);
    bounds(driver);assert(d.sampled==15);
}

int main() {
    descriptorDiagnostics();
    blockAckOwnership();
    blockAckRequests();
    infrastructureOwnership();
    ItlIwx driver;
    run(driver,frame());
    assert(driver.awdlReceive.valid==1 && driver.awdlReceive.lastRSSI==-45);
    assert(driver.awdlReceive.lastChannel==44 && driver.publications==1);
    assert(driver.crypto==0 && driver.duplicates==0 && driver.forwarded==0);
    run(driver,frame(),IWX_RX_MPDU_MFLG2_PAD,3,true);
    assert(driver.awdlReceive.valid==2 && driver.awdlReceive.lastRSSI==-50);
    assert(driver.awdlReceive.lastChannel==149);
    assert(driver.clockCalls==2);
    auto b=frame();b.pop_back();run(driver,b);
    assert(driver.awdlReceive.malformed==1 && driver.crypto==0);
    b=frame();b[29]=0x20;run(driver,b);
    assert(driver.awdlReceive.unsupported==1);
    unsigned events=driver.publications;
    run(driver,frame(),0,0); // CRC failure: never reach the AWDL parser.
    assert(driver.publications==int(events));
    b=frame();b[27]=0xf1;run(driver,b); // Other vendor: original receive path.
    b=frame();b[0]=0x08;run(driver,b);  // Infrastructure data: original path.
    b=frame();b[1]=0x40;run(driver,b);  // Protected management: original crypto path.
    assert(driver.forwarded==3 && driver.crypto==3 && driver.duplicates==3);
    driver.com.sc_ic.ic_opmode=IEEE80211_M_MONITOR;run(driver,frame());
    assert(driver.forwarded==4 && driver.publications==int(events));
    assert(driver.clockCalls==2); // CRC, other traffic and malformed actions do not read GP2.
    b=frame();Packet split={b.data(),b.size()-1,b.size(),0};iwx_rx_mpdu_desc desc={};
    driver.com.sc_ic.ic_opmode=IEEE80211_M_STA;
    assert(!driver.receiveAWDL(&split,&desc));
    assert(split.frees==0); // Non-consumed packets remain owned by the caller.
    bounds(driver);
    // Hardware-deaggregated AWDL data must bypass AP crypto/replay handling too.
    std::vector<uint8_t> eth(54,0);
    AirportAWDL::copyAddress(eth.data(),driver.com.sc_ic.ic_myaddr);
    eth[6]=2;eth[12]=0x86;eth[13]=0xdd;eth[14]=0x60;
    std::vector<uint8_t> data(80,0);
    assert(AirportAWDL::dataFromEthernet(eth.data(),eth.size(),data.data(),data.size(),1,2)==80);
    data[0]=0x88;data.insert(data.begin()+24,{0x80,0});
    unsigned before=driver.forwarded;
    run(driver,data,IWX_RX_MPDU_MFLG2_AMSDU);
    assert(driver.awdlData.valid==1 && driver.awdlData.observed==1);
    assert(driver.forwarded==int(before));
    run(driver,data,IWX_RX_MPDU_MFLG2_AMSDU);
    assert(driver.awdlData.duplicate==1);
    assert(driver.awdlPeers.find(eth.data()+6,42000000));
    driver.awdlPeers.find(eth.data()+6,42000000)->nativeCached=true;
    driver.resetAWDLPeers();
    assert(!driver.awdlPeers.find(eth.data()+6,42000000));
    auto registered=driver.awdlPeers.findCached(eth.data()+6);
    assert(registered && !registered->haveSync && !registered->dataSequence.initialized);
    assert(driver.awdlPublishedSecond==~uint64_t(0));
    ItlIwx active;
    active.awdlRadio.ready=true;
    AirportAWDL::copyAddress(active.awdlRadio.address,active.com.sc_ic.ic_myaddr);
    struct Delivery { unsigned action=0,data=0; } delivery;
    active.awdlReceiver=&delivery;
    active.rxDelay=2500;
    active.awdlActionInput=[](void *ctx,const uint8_t *p,size_t n,uint16_t ch,int16_t rssi,uint64_t receivedUS) {
        assert(n==45 && p[24]==127 && ch==44 && rssi==-45);
        assert(receivedUS==41997500);
        ++static_cast<Delivery *>(ctx)->action;
    };
    active.awdlInput=[](void *ctx,const AirportAWDL::DataView &view) {
        assert(view.size==40 && view.payload[0]==0x60);
        ++static_cast<Delivery *>(ctx)->data;
    };
    run(active,frame());
    assert(delivery.action==1);
    b=frame();AirportAWDL::copyAddress(b.data()+4,active.awdlRadio.address);run(active,b);
    assert(delivery.action==2); // A directed response reaches the native peer manager.
    b=frame();b[4]=2;b[9]=9;run(active,b);
    assert(delivery.action==2); // An overheard handshake belongs to another receiver.
    assert(active.awdlReceive.otherDestination==1);
    assert(active.awdlReceive.valid==3); // Passive timing observations remain available.
    run(active,data,IWX_RX_MPDU_MFLG2_AMSDU);
    assert(delivery.data==1);
    run(active,data,IWX_RX_MPDU_MFLG2_AMSDU);
    assert(delivery.data==1); // Duplicate suppression applies before BSD delivery.
    b=frame();b.pop_back();run(active,b);
    b=frame();AirportAWDL::copyAddress(b.data()+10,active.awdlRadio.address);run(active,b);
    assert(delivery.action==2); // Malformed frames and TX echoes are not delivered.
    active.awdlRadio.ready=false;run(active,frame());
    assert(delivery.action==2);

    // Production RX applies hardware delay to timing, retaining host time for expiry.
    ItlIwx timed;timed.rxDelay=2500;
    b=frame();b.insert(b.end(),{4,33,0});
    size_t sync=b.size();b.resize(sync+33,0);
    b[sync+1]=16;b[sync+3]=44;b[sync+5]=16;b[sync+21]=2;b[sync+27]=4;
    run(timed,b);
    const auto *peer=timed.awdlPeers.find(b.data()+10,42000000);
    assert(peer && peer->haveSync && peer->syncReceivedUS==41997500);
    assert(peer->lastSeenUS==42000000 && timed.clockCalls==1);

    // Both consumers see fresh frames across a remote clock restart, and
    // neither consumes duplicate retries or locally reordered observations.
    ItlIwx ordered;unsigned notifications=0;
    ordered.awdlRadio.ready=true;
    AirportAWDL::copyAddress(ordered.awdlRadio.address,ordered.com.sc_ic.ic_myaddr);
    ordered.awdlReceiver=&notifications;
    ordered.awdlActionInput=[](void *ctx,const uint8_t *,size_t,uint16_t,int16_t,uint64_t receivedUS) {
        assert(receivedUS==fixtureNowUS);++*static_cast<unsigned *>(ctx);
    };
    b=frame();for(unsigned j=0;j<4;++j)b[32+j]=uint32_t(1000000000)>>(8*j);
    AirportAWDL::putLE16(b.data()+22,4095<<4);run(ordered,b);
    fixtureNowUS+=1000;std::memset(b.data()+32,0,4);b[32]=1;
    AirportAWDL::putLE16(b.data()+22,0);run(ordered,b);
    assert(notifications==2 && ordered.awdlPeers.remoteClockBacksteps==1);
    fixtureNowUS+=1000;b[1]|=8;run(ordered,b);
    assert(notifications==2 && ordered.awdlPeers.stale==1);
    fixtureNowUS+=1000;ordered.rxDelay=3000;b[1]&=~8;
    AirportAWDL::putLE16(b.data()+22,1<<4);run(ordered,b);
    assert(notifications==2 && ordered.awdlPeers.stale==2);
    fixtureNowUS+=1000;ordered.rxDelay=0;b[1]|=8;run(ordered,b);
    assert(notifications==3 && ordered.awdlPeers.stale==2);
}
