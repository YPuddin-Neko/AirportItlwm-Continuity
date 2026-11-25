// Replay the public, anonymized iPhone sessions through production IWX RX.
#include <fstream>
#include <iostream>
#include <string>

struct CaptureDelivery {
    ItlIwx *driver;
    AirportItlwmAWDLInterface native;
    const std::vector<uint8_t> *frame=nullptr;
    unsigned data=0;
    uint64_t bytes=0;
};

static void capturedAction(void *context,const uint8_t *p,size_t n,
                           uint16_t channel,int16_t rssi,uint64_t receivedUS) {
    auto &delivery=*static_cast<CaptureDelivery *>(context);
    assert(std::vector<uint8_t>(p,p+n)==*delivery.frame);
    const unsigned before=delivery.native.actionDelivered;
    AirportItlwmAWDLInterface::receiveAction(&delivery.native,p,n,channel,rssi,receivedUS);
    assert(delivery.native.actionDelivered==before+1 && !delivery.native.actionDropped);
    const size_t header=ieee80211_get_hdrlen(reinterpret_cast<const ieee80211_frame *>(p));
    assert(delivery.native.payload==std::vector<uint8_t>(p+header,p+n));
    assert(delivery.native.pool.buffer.returns==1);
    const uint8_t *meta=delivery.native.last.bytes;
    assert(sameAddress(meta,p+10) && le32(meta+12)==1 && le32(meta+16)==channel);
    assert(int32_t(le32(meta+44))==rssi);
    const uint32_t stamp=(uint32_t(meta[48])<<24)|(uint32_t(meta[49])<<16)|
        (uint32_t(meta[50])<<8)|meta[51];
    assert(stamp==uint32_t(receivedUS));
    const auto *peer=delivery.driver->awdlPeers.find(p+10,fixtureNowUS);
    assert(peer && peer->haveSync && peer->haveSequence);
}

static void capturedData(void *context,const DataView &view) {
    auto &delivery=*static_cast<CaptureDelivery *>(context);
    const auto &raw=*delivery.frame;
    assert(sameAddress(view.source,raw.data()+10));
    assert(sameAddress(view.destination,raw.data()+4));
    std::vector<uint8_t> ethernet(view.size+14);
    assert(ethernetFromData(view,ethernet.data(),ethernet.size())==ethernet.size());
    assert(sameAddress(ethernet.data(),view.destination));
    assert(sameAddress(ethernet.data()+6,view.source) && be16(ethernet.data()+12)==0x86dd);
    assert(!std::memcmp(ethernet.data()+14,view.payload,view.size));
    ++delivery.data;delivery.bytes+=view.size;
}

struct CaptureCase {
    const char *name;
    uint8_t local,peer;
    unsigned frames,actions,data,nativeActions,deliveredData,duplicates,blockAck;
};

struct CapturedSequenceReset {
    std::vector<uint8_t> first[55];
    unsigned resumed=0,refreshes=0;
    uint64_t lastFirstUS=0,lastAFUS=0,maxAFGapUS=0;

    bool inspect(unsigned number,uint64_t us,const std::vector<uint8_t> &raw) {
        static const uint8_t phone[6]={2,0,0,0,0,1},local[6]={2,0,0,0,0,2};
        if(number>370 && number<518 && raw.size()>=40 && raw[0]==0xd0 && raw[24]==127 &&
           sameAddress(raw.data()+10,phone)) {
            if(lastAFUS)maxAFGapUS=std::max(maxAFGapUS,us-lastAFUS);
            lastAFUS=us;++refreshes;
        }
        if(raw[0]!=0x88 || !sameAddress(raw.data()+10,phone) ||
           !sameAddress(raw.data()+4,local))return false;
        // These corpus records have a 26-byte QoS header and a 16-byte AWDL body header.
        assert(raw.size()>=102 && !(raw[24]&0x80) && !(raw[1]&0x80));
        const uint16_t word=le16(raw.data()+36);
        const uint8_t *ip=raw.data()+42,*tcp=ip+40;
        assert(ip[6]==6 && be16(ip+4)+82==raw.size());
        assert(be16(tcp)==53804 && be16(tcp+2)==8771);
        if(word>=55)return false;
        if(number<=370) {
            assert(first[word].empty());first[word]=raw;lastFirstUS=us;return false;
        }
        const auto &old=first[word];assert(!old.empty());
        const uint8_t *oldIP=old.data()+42,*oldTCP=oldIP+40;
        assert(!std::memcmp(ip+8,oldIP+8,32)); // Same IPv6 connection, not another peer.
        assert(std::memcmp(tcp+4,oldTCP+4,4)); // New TCP sequence, not a link retry.
        assert(word==resumed++);
        if(number==518) {
            assert(word==0 && us==10831243 && lastFirstUS==7129487);
            assert(us-lastFirstUS==3701756 && refreshes==46 && maxAFGapUS==230858);
            assert(!(raw[1]&8) && le16(raw.data()+22)==0);
            assert(oldTCP[13]==0xc2 && tcp[13]==0x10);
        }
        return true;
    }
};

int main() {
    const CaptureCase cases[]={
        {"discovery-ios26.x-mt7925e-ch44-1",3,2,1229,1219,1,397,0,0,9},
        {"receive-ios26.x-mt7925e-ch44-1",2,1,5413,1499,3896,502,1986,0,18},
        {"send-ios26.x-mt7925e-ch44-1",4,3,2731,2175,527,563,137,0,29}
    };
    // Check both descriptor layouts, then firmware's two-byte header padding.
    for(const auto &capture:cases) for(bool v3:{false,true}) for(bool padded:{false,true}) {
        ItlIwx driver;
        driver.awdlRadio.ready=true;
        driver.awdlRadio.mac=true;
        uint8_t address[6]={2,0,0,0,0,capture.local};
        copyAddress(driver.awdlRadio.address,address);
        CaptureDelivery delivery;delivery.driver=&driver;
        CapturedSequenceReset reset;
        driver.awdlReceiver=&delivery;driver.awdlActionInput=capturedAction;driver.awdlInput=capturedData;
        std::ifstream in(std::string(CAPTURE_DIRECTORY)+"/"+capture.name,std::ios::binary);
        assert(in.is_open());unsigned packets=0,blockAck=0;
        while(in) {
            uint64_t us;uint32_t length;int16_t channel,rssi;
            in.read(reinterpret_cast<char *>(&us),8);if(in.eof())break;
            in.read(reinterpret_cast<char *>(&length),4);
            in.read(reinterpret_cast<char *>(&channel),2);
            in.read(reinterpret_cast<char *>(&rssi),2);
            assert(in && length>=24 && length<=4096 && channel==44);
            std::vector<uint8_t> raw(length);in.read(reinterpret_cast<char *>(raw.data()),length);assert(in);
            fixtureNowUS=42000000+us;delivery.frame=&raw;
            const bool resumed=capture.local==2 && reset.inspect(packets+1,us,raw);
            const unsigned deliveredBefore=delivery.data;
            // Only the descriptor is manufactured. The captured MPDU stays byte-for-byte intact.
            auto bytes=raw;
            if(padded)bytes.insert(bytes.begin()+ieee80211_get_hdrlen(
                reinterpret_cast<const ieee80211_frame *>(bytes.data())),2,0);
            const size_t descSize=v3?sizeof(iwx_rx_mpdu_desc):IWX_RX_DESC_SIZE_V1;
            std::vector<uint8_t> storage(descSize+bytes.size());
            auto *desc=reinterpret_cast<iwx_rx_mpdu_desc *>(storage.data());
            desc->status=IWX_RX_MPDU_RES_STATUS_CRC_OK|IWX_RX_MPDU_RES_STATUS_OVERRUN_OK;
            desc->mpdu_len=bytes.size();desc->mac_flags2=padded?IWX_RX_MPDU_MFLG2_PAD:0;
            if(v3) {desc->v3.energy_a=-rssi;desc->v3.energy_b=-rssi;desc->v3.channel=channel;}
            else {desc->v1.energy_a=-rssi;desc->v1.energy_b=-rssi;desc->v1.channel=channel;}
            driver.com.sc_device_family=v3?IWX_DEVICE_FAMILY_AX210:IWX_DEVICE_FAMILY_22000;
            std::memcpy(storage.data()+descSize,bytes.data(),bytes.size());
            Packet packet={storage.data(),storage.size(),storage.size(),0};
            const int forwarded=driver.forwarded;
            driver.iwx_rx_mpdu_mq(&driver.com,&packet,desc,storage.size(),nullptr);
            if(resumed)assert(delivery.data==deliveredBefore+1);
            assert(packet.frees==1);++packets;
            if(raw.size()>=33 && raw[0]==0xd0 && raw[24]==3 && raw[25]==0 &&
               sameAddress(raw.data()+16,BSSID)) {
                assert(driver.forwarded==forwarded);
                ++blockAck;
            }
        }
        assert(packets==capture.frames);
        assert(driver.awdlReceive.valid==capture.actions);
        assert(driver.awdlData.valid==capture.data && driver.awdlData.subframes==capture.data);
        assert(!driver.awdlReceive.malformed && !driver.awdlReceive.unsupported);
        assert(!driver.awdlData.malformed && !driver.awdlData.unsupported);
        assert(blockAck==capture.blockAck);
        assert(driver.forwarded==int(capture.frames-capture.actions-capture.data-blockAck));
        assert(driver.crypto==driver.forwarded && driver.duplicates==driver.forwarded);
        address[5]=capture.peer;
        const auto *peer=driver.awdlPeers.find(address,fixtureNowUS);
        assert(peer && peer->metadata.deviceClass==2 && peer->receivedMIF && peer->haveSequence);
        Window window={};assert(peer->window(fixtureNowUS,window));
        assert(delivery.native.taps==capture.nativeActions && !delivery.native.actionDropped);
        assert(delivery.data==capture.deliveredData && delivery.data==driver.awdlData.observed);
        assert(driver.awdlData.duplicate==capture.duplicates);
        if(capture.local==2)assert(reset.resumed==55);
        if(capture.data>1)assert(delivery.data>0 && delivery.bytes>10000);
        if(!v3 && !padded)std::cout<<capture.name<<": "<<packets<<" frames, "
            <<delivery.native.taps<<" native actions, "<<delivery.data<<" IPv6 deliveries, "
            <<driver.awdlData.duplicate<<" duplicates\n";
    }
}
