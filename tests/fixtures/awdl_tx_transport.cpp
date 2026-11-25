static void setupPeerTX(ItlIwx &d) {
    using namespace AirportAWDL;
    clockUS=60000000;setupAggregateRadio(d);d.awdlRadio.listener=true;
    auto &tx=d.awdlRadio.blockAckTX;tx.configured=true;tx.phase=TXBlockAckQueue::Agreed;
    tx.queue=9;tx.station=4;tx.tid=0;tx.epoch=d.dmaEpoch;tx.cookie=91;
    copyAddress(tx.peer,d.awdlPeers.peers[0].address);tx.window=32;tx.sequence=4095;
    tx.enabled=true;tx.paused=false;tx.amsdu=true;tx.windowEndUS=clockUS+55000;
    tx.widthMHz=20;tx.floorMbps=tx.legacyMbps=12;tx.mcs=d.awdlTXMcs(clockUS);tx.antenna=d.awdlTXAntenna();
    assert(tx.mcs==0xf0 && tx.antenna);d.com.txq[9].allocate(9);
}
static Packet peerTXPacket() {
    uint8_t ethernet[1514]={};memcpy(ethernet,self,6);ethernet[5]=2;
    memcpy(ethernet+6,self,6);ethernet[12]=0x86;ethernet[13]=0xdd;ethernet[14]=0x60;
    AirportAWDL::putBE16(ethernet+18,1460);ethernet[1513]=0xab;
    Packet p;p.size=p.total=AirportAWDL::dataFromEthernet(ethernet,sizeof(ethernet),p.bytes,sizeof(p.bytes),100,321);
    assert(p.size>1500);return p;
}
static void peerTXReport(ItlIwx &d,unsigned status=IWX_TX_STATUS_SUCCESS) {
    iwx_rx_packet p={};p.length=sizeof(iwx_tx_resp)+4;
    auto &report=*reinterpret_cast<iwx_tx_resp *>(p.data);
    report.frame_count=1;report.tx_queue=9;report.ra_tid=0x40;report.status.status=status;
    unsigned next=d.com.txq[9].cur;memcpy(&report.status+1,&next,4);iwx_rx_data rx;
    d.iwx_rx_tx_cmd(&d.com,&p,&rx);
}
static void finishPeerTX(ItlIwx &d) {
    d.awdlRadio.blockAckTX={};d.iwx_reset_tx_ring(&d.com,&d.com.txq[9]);
    assert(!d.stopAWDLRadio());
}
static void peerTXWireAndCompletion() {
    for(bool gen3:{false,true}) {
        ItlIwx d;setupPeerTX(d);if(gen3)d.com.sc_device_family=IWX_DEVICE_FAMILY_AX210;
        auto &owner=d.awdlRadio.blockAckTX;auto &ring=d.com.txq[9];auto p=peerTXPacket();
        p.bytes[1]=IEEE80211_FC1_RETRY;
        const std::vector<uint8_t> payload(p.bytes+24,p.bytes+p.size);
        const size_t length=p.size;const auto allocationsBefore=allocatedPackets.size();
        assert(!d.transmitAWDL(&p,501));
        assert(p.frees==1 && allocatedPackets.size()==allocationsBefore+1 && ring.queued==1 && !owner.sequence);
        auto &data=ring.data[0];auto qos=data.m;assert(qos && !qos->frees && data.awdlBACookie==owner.cookie);
        assert(data.flags==(IWX_TXDATA_FLAG_AWDL|IWX_TXDATA_FLAG_AWDL_BA) && !data.awdlRatePeer);
        auto tx=reinterpret_cast<const iwx_tx_cmd_gen2 *>(ring.cmd[0].data);
        const auto flags=gen3?reinterpret_cast<const iwx_tx_cmd_gen3 *>(tx)->flags:tx->flags;
        assert(!(flags&IWX_TX_FLAGS_CMD_RATE) && tx->len==length+2);
        const auto header=ring.cmd[0].data+(gen3?sizeof(iwx_tx_cmd_gen3):sizeof(*tx));
        assert(header[0]==0x88 && !header[1] && !header[24] && !header[25]);
        assert((AirportAWDL::le16(header+22)>>4)==4095);
        assert(ring.maps[0].bytes==payload && d.com.doorbells.back()==((9u<<16)|1));
        assert(!d.com.txq[7].queued && !d.com.txq[8].queued);
        peerTXReport(d);assert(qos->frees==1 && !ring.queued && owner.phase==AirportAWDL::TXBlockAckQueue::Agreed);
        auto second=peerTXPacket();assert(!d.transmitAWDL(&second,502));
        assert((AirportAWDL::le16(ring.cmd[1].data+(gen3?sizeof(iwx_tx_cmd_gen3):sizeof(*tx))+22)>>4)==0);
        assert(owner.sequence==1);peerTXReport(d,IWX_TX_STATUS_FAIL_LONG_LIMIT);
        assert(owner.phase==AirportAWDL::TXBlockAckQueue::Agreed && !owner.needDelba && !ring.queued);
        auto third=peerTXPacket();assert(!d.transmitAWDL(&third,503));
        assert((AirportAWDL::le16(ring.cmd[2].data+(gen3?sizeof(iwx_tx_cmd_gen3):sizeof(*tx))+22)>>4)==1);
        assert(owner.sequence==2);peerTXReport(d,IWX_TX_STATUS_FAIL_SHORT_LIMIT);
        assert(owner.phase==AirportAWDL::TXBlockAckQueue::Agreed && !ring.queued);
        auto fourth=peerTXPacket();assert(!d.transmitAWDL(&fourth,504));
        peerTXReport(d,IWX_TX_STATUS_FAIL_BT_RETRY);
        assert(owner.phase==AirportAWDL::TXBlockAckQueue::Agreed && !ring.queued);
        auto fifth=peerTXPacket();assert(!d.transmitAWDL(&fifth,505));
        peerTXReport(d,IWX_TX_STATUS_SUCCESS);
        assert(owner.phase==AirportAWDL::TXBlockAckQueue::Agreed && !ring.queued);
        for(unsigned loss=0;loss<8;++loss) {
            auto f=peerTXPacket();assert(!d.transmitAWDL(&f,510+loss));
            peerTXReport(d,IWX_TX_STATUS_FAIL_LONG_LIMIT);
            if(loss<7)assert(owner.phase==AirportAWDL::TXBlockAckQueue::Agreed);
        }
        assert(owner.phase==AirportAWDL::TXBlockAckQueue::Retiring && owner.needDelba && !ring.queued);
        auto waiting=peerTXPacket();assert(d.transmitAWDL(&waiting,530)==EAGAIN);
        finishPeerTX(d);
        ItlIwx fatal;setupPeerTX(fatal);if(gen3)fatal.com.sc_device_family=IWX_DEVICE_FAMILY_AX210;
        auto fp=peerTXPacket();assert(!fatal.transmitAWDL(&fp,540));
        peerTXReport(fatal,IWX_TX_STATUS_FAIL_STA_INVALID);
        assert(fatal.awdlRadio.blockAckTX.phase==AirportAWDL::TXBlockAckQueue::Retiring && fatal.awdlRadio.blockAckTX.needDelba);
        finishPeerTX(fatal);
    }
}
static void peerTXAggregatePermission() {
    for(bool allowed:{false,true}) {
        ItlIwx d;setupPeerTX(d);auto &tx=d.awdlRadio.blockAckTX;tx.amsdu=allowed;
        auto p=aggregatePacket();const auto seq=tx.sequence;
        const int result=d.transmitAWDL(&p,701);
        if(!allowed)assert(result==EAGAIN && p.frees==1 && tx.sequence==seq && !d.com.txq[9].queued);
        else {
            assert(!result && !p.frees && d.com.txq[9].queued==1);
            const auto header=d.com.txq[9].cmd[0].data+sizeof(iwx_tx_cmd_gen2);
            assert(header[24]==0x80 && (AirportAWDL::le16(header+22)>>4)==seq);
            peerTXReport(d);assert(p.frees==1);
        }
        finishPeerTX(d);
    }
}
static void peerTXAdmissionRechecks() {
    for(unsigned change=0;change<11;++change) {
        ItlIwx d;setupPeerTX(d);auto &tx=d.awdlRadio.blockAckTX;auto &ring=d.com.txq[9];
        const auto seq=tx.sequence;const auto bells=d.com.doorbells.size();
        if(change==10) {
            d.txAnt=d.rxAnt=3;
            d.awdlPeers.peers[0].metadata.rxHTMcs2=0xf0;
            tx.mcs2=0xf0;tx.antenna=d.awdlTXAntenna();assert(tx.antenna==3);
        }
        ring.maps[0].onMap=[&] {
            if(change==0)tx.paused=true;
            if(change==1)tx.windowEndUS=clockUS;
            if(change==2)++tx.cookie;
            if(change==3)++d.dmaEpoch;
            if(change==4)tx.phase=AirportAWDL::TXBlockAckQueue::Retiring;
            if(change==5)d.awdlMinRateMbps=24;
            if(change==6)d.txAnt=tx.antenna==1?2:1;
            if(change==7)d.awdlPeers.peers[0].metadata.rxHTMcs=2;
            if(change==8)tx.windowEndUS=clockUS+1;
            if(change==10)d.awdlPeers.peers[0].metadata.rxHTMcs2=0x80;
        };
        failAllocation=change==9;auto p=peerTXPacket();assert(d.transmitAWDL(&p,801));failAllocation=false;
        assert(p.frees==1 && !ring.queued && tx.sequence==seq && d.com.doorbells.size()==bells);
        assert(!ring.desc[0].num_tbs && !ring.data[0].m);ring.maps[0].onMap={};
        d.awdlMinRateMbps=12;finishPeerTX(d);
    }
}
static void peerTXHeldAcrossWindows() {
    ItlIwx d;setupPeerTX(d);auto &tx=d.awdlRadio.blockAckTX;auto &ring=d.com.txq[9];
    for(unsigned n=0;n<20;++n){auto p=peerTXPacket();assert(!d.transmitAWDL(&p,900+n));}
    assert(ring.queued==20);std::vector<mbuf_t> held;for(unsigned n=0;n<20;++n)held.push_back(ring.data[n].m);
    const auto seq=tx.sequence;assert(!d.closeAWDLListenerWindow());
    assert(tx.paused && ring.queued==20 && tx.sequence==seq);for(auto p:held)assert(!p->frees);
    const auto cookie=tx.cookie;
    clockUS+=65536;
    assert(!d.serviceAWDLChannel(149,clockUS+60000));
    assert(tx.cookie==cookie && tx.paused && ring.queued==20 && tx.sequence==seq);
    for(auto packet:held)assert(!packet->frees);
    clockUS+=65536;RadioState::GrantPolicy policy={};policy.dataQueue=true;
    assert(!d.serviceAWDLChannel(6,clockUS+60000,policy));
    tx.paused=false;tx.windowEndUS=clockUS+4000;
    auto p=peerTXPacket();assert(!d.transmitAWDL(&p,930));
    assert(ring.queued==21);peerTXReport(d);assert(!ring.queued);
    for(auto packet:held)assert(packet->frees==1);finishPeerTX(d);
}
