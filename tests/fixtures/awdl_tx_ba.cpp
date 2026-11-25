
static void ownedQueue(ItlIwx &d, Packet (&packets)[3]) {
    auto &o=d.awdlRadio.blockAckTX;
    o.configured=true;o.queue=1;o.station=4;o.tid=0;o.epoch=d.dmaEpoch;o.cookie=17;
    auto &r=d.com.txq[1];r.tail=7;r.cur=2;r.queued=3;
    d.com.sc_tx_timer=9;
    for(unsigned n=0;n<3;++n) {
        auto &s=r.data[(7+n)%8];s.m=&packets[n];s.flags=IWX_TXDATA_FLAG_AWDL|IWX_TXDATA_FLAG_AWDL_BA;
        s.awdlToken=10+n;s.awdlBACookie=o.cookie;s.type=IEEE80211_FC0_TYPE_DATA;
    }
}
static iwx_compressed_ba_notif &baPacket(iwx_rx_packet &p) {
    p.length=sizeof(iwx_compressed_ba_notif)+sizeof(iwx_compressed_ba_tfd);
    auto &b=*reinterpret_cast<iwx_compressed_ba_notif *>(p.data);
    b.sta_id=4;b.tfd_cnt=1;b.tfd[0].q_num=1;b.tfd[0].tid=0;b.tfd[0].tfd_index=2;
    // The aggregate counters do not identify individual reclaimed descriptors.
    b.txed=2;b.done=1;
    return b;
}
static void baCompletion() {
    ItlIwx d;Packet packets[3];ownedQueue(d,packets);
    iwx_rx_packet p;auto &b=baPacket(p);iwx_rx_data rx;
    b.tfd[0].tfd_index=1;
    d.iwx_rx_tx_ba_notif(&d.com,&p,&rx);
    assert((d.dataResults==std::vector<std::pair<uint64_t,int>>{{10,0},{11,0}}));
    assert(d.com.txq[1].tail==1&&d.com.txq[1].queued==1&&packets[2].frees==0);
    b.flags=IWX_BA_RESP_TX_BAR;b.tfd[0].tfd_index=2;
    d.iwx_rx_tx_ba_notif(&d.com,&p,&rx);
    d.iwx_rx_tx_ba_notif(&d.com,&p,&rx);
    assert(d.dataResults.size()==3&&d.awdlRadio.blockAckTX.acknowledged==3);
    assert(d.awdlRadio.completed==3&&d.com.sc_tx_timer==9&&d.com.txq[1].queued==0);
    for(auto &p:packets)assert(p.frees==1);
}
static void aggregateReport() {
    ItlIwx d;Packet packets[3];ownedQueue(d,packets);
    iwx_rx_packet p;iwx_rx_data rx;
    auto &t=*reinterpret_cast<iwx_tx_resp *>(p.data);
    t.frame_count=3;t.tx_queue=1;t.ra_tid=0x40;
    p.length=offsetof(iwx_tx_resp,status)+3*sizeof(t.status)+4;
    d.iwx_rx_tx_cmd(&d.com,&p,&rx);
    assert(d.dataResults.empty()&&d.com.txq[1].queued==3&&d.com.sc_tx_timer==9);
    assert(d.awdlRadio.blockAckTX.transmittedReports==1);
    for(auto &packet:packets)assert(!packet.frees);
    iwx_rx_packet ba;baPacket(ba);d.iwx_rx_tx_ba_notif(&d.com,&ba,&rx);
    assert(d.dataResults.size()==3&&d.awdlRadio.completed==3);
}
static void rejectedBA() {
    for(unsigned variant=0;variant<17;++variant) {
        ItlIwx d;Packet packets[3];ownedQueue(d,packets);
        iwx_rx_packet p;auto &b=baPacket(p);iwx_rx_data rx;auto &o=d.awdlRadio.blockAckTX;
        switch(variant) {
        case 0:b.sta_id=5;break;
        case 1:b.sta_id=0;break;
        case 2:b.tfd[0].tid=1;break;
        case 3:b.tfd[0].q_num=257;break;
        case 4:b.tfd[0].tfd_index=258;break;
        case 5:b.tfd[0].tfd_index=66;break;
        case 6:o.configured=false;break;
        case 7:++d.dmaEpoch;break;
        case 8:d.com.txq[1].data[0].awdlBACookie=19;break;
        case 9:d.com.txq[1].data[0].flags=IWX_TXDATA_FLAG_AWDL;break;
        case 10:--p.length;break;
        case 11:b.tfd_cnt=65535;break;
        case 12:p.length=sizeof(b)-1;break;
        case 13:b.tfd[0].tfd_index=3;break;
        case 14:b.tfd_cnt=0;break;
        case 15:o.cookie=0;break;
        case 16:b.tfd[0].q_num=2;break;
        }
        d.iwx_rx_tx_ba_notif(&d.com,&p,&rx);
        assert(d.dataResults.empty()&&d.com.txq[1].queued==3);
        assert(d.com.sc_tx_timer==9);
        for(auto &packet:packets)assert(!packet.frees);
        d.iwx_reset_tx_ring(&d.com,&d.com.txq[1]);
        assert(d.dataResults.size()==3);
        for(auto &packet:packets)assert(packet.frees==1);
    }
}
static void singleFinal() {
    for(unsigned status:{1,2,0x82,0x8e,0x85}) {
        ItlIwx d;Packet packets[3];ownedQueue(d,packets);
        iwx_rx_packet p;auto &t=*reinterpret_cast<iwx_tx_resp *>(p.data);
        t.frame_count=1;t.tx_queue=1;t.ra_tid=0x40;t.status.status=status;
        p.length=sizeof(t)+4;uint32_t next=2;memcpy(&t.status+1,&next,4);iwx_rx_data rx;
        d.iwx_rx_tx_cmd(&d.com,&p,&rx);
        const int result=status<=2?0:status==0x82?EIO:ECANCELED;
        assert((d.dataResults==std::vector<std::pair<uint64_t,int>>{{10,result},{11,0},{12,0}}));
        assert(d.awdlRadio.blockAckTX.acknowledged==(status<=2?3:2));
        assert(d.com.sc_tx_timer==9);
        for(auto &packet:packets)assert(packet.frees==1);
    }
    for(unsigned variant=0;variant<6;++variant) {
        ItlIwx d;Packet packets[3];ownedQueue(d,packets);iwx_rx_packet p;iwx_rx_data rx;
        auto &t=*reinterpret_cast<iwx_tx_resp *>(p.data);
        t.frame_count=1;t.tx_queue=1;t.ra_tid=0x40;t.status.status=1;p.length=sizeof(t)+4;
        uint32_t next=2;
        switch(variant) {
        case 0:next=258;break;
        case 1:next=66;break;
        case 2:t.ra_tid=0;break;
        case 3:t.ra_tid=0x41;break;
        case 4:++d.dmaEpoch;break;
        case 5:d.com.txq[1].data[0].awdlBACookie=23;break;
        }
        memcpy(&t.status+1,&next,4);d.iwx_rx_tx_cmd(&d.com,&p,&rx);
        assert(d.dataResults.empty()&&d.com.txq[1].queued==3);
        d.iwx_reset_tx_ring(&d.com,&d.com.txq[1]);
        for(auto &packet:packets)assert(packet.frees==1);
    }
}
static iwx_flush_queue_info &flushPacket(ItlIwx &d) {
    d.flushStation=4;d.flushResponse.length=sizeof(iwx_tx_path_flush_cmd_rsp);
    auto &r=*reinterpret_cast<iwx_tx_path_flush_cmd_rsp *>(d.flushResponse.data);
    r.sta_id=4;r.num_flushed_queues=1;
    auto &q=r.queues[0];q.queue_num=1;q.tid=0;q.read_before_flush=0;q.read_after_flush=2;
    return q;
}
static void peerFlush() {
    for(unsigned variant=0;variant<10;++variant) {
        ItlIwx d;Packet packets[3];ownedQueue(d,packets);auto &q=flushPacket(d);
        switch(variant) {
        case 0:break;
        case 1:q.read_before_flush=2;q.read_after_flush=0;break;
        case 2:q.read_before_flush=256;break;
        case 3:q.read_after_flush=258;break;
        case 4:d.resetDuringFlush=true;break;
        case 5:d.flushResponse.length=0;break;
        case 6:q.tid=1;break;
        case 7:d.com.txq[1].data[0].awdlBACookie=23;break;
        case 8:d.awdlRadio.blockAckTX.configured=false;break;
        case 9:q.queue_num=2;break;
        }
        assert(!d.iwx_flush_sta_tids(&d.com,4,0xffff));
        if(!variant) {
            assert((d.dataResults==std::vector<std::pair<uint64_t,int>>{{10,0},{11,ECANCELED},{12,ECANCELED}}));
            assert(d.awdlRadio.blockAckTX.acknowledged==1&&d.awdlRadio.blockAckTX.flushed==2);
            assert(!d.iwx_flush_sta_tids(&d.com,4,0xffff));
            assert(d.dataResults.size()==3);
        } else assert(d.dataResults.empty()&&d.com.txq[1].queued==3);
        d.iwx_reset_tx_ring(&d.com,&d.com.txq[1]);
        for(auto &packet:packets)assert(packet.frees==1);
    }
}
static void callbackReset() {
    for(unsigned mode=0;mode<3;++mode) {
        ItlIwx d;Packet packets[3];ownedQueue(d,packets);
        d.awdlDataComplete=[](void *ctx,uint64_t token,int result) {
            auto &d=*static_cast<ItlIwx *>(ctx);d.dataResults.push_back({token,result});
            if(d.dmaEpoch==1) {
                ++d.dmaEpoch;d.awdlRadio.blockAckTX={};
                d.iwx_reset_tx_ring(&d.com,&d.com.txq[1]);
            }
        };
        iwx_rx_packet p;iwx_rx_data rx;
        if(mode==0) {baPacket(p);d.iwx_rx_tx_ba_notif(&d.com,&p,&rx);}
        if(mode==1) {
            auto &t=*reinterpret_cast<iwx_tx_resp *>(p.data);
            t.frame_count=1;t.tx_queue=1;t.ra_tid=0x40;t.status.status=1;p.length=sizeof(t)+4;
            uint32_t next=2;memcpy(&t.status+1,&next,4);d.iwx_rx_tx_cmd(&d.com,&p,&rx);
        }
        if(mode==2) {flushPacket(d);assert(!d.iwx_flush_sta_tids(&d.com,4,0xffff));}
        assert(d.dmaEpoch==2&&d.dataResults.size()==3&&d.com.txq[1].queued==0);
        for(auto &packet:packets)assert(packet.frees==1);
    }
}
static void wireProtocol() {
    using namespace AirportAWDL;
    const uint8_t a[6]={2,1,2,3,4,5},b[6]={2,6,7,8,9,10};
    BlockAckRequest request={};copyAddress(request.peer,b);
    request.dialog=19;request.tid=7;request.ssn=4095;request.window=64;request.immediate=true;request.amsdu=true;
    uint8_t frame[33];blockAckSolicit(frame,a,request,4095);
    BlockAckRequest received={};assert(blockAckRequest(frame,sizeof(frame),b,received));
    assert(sameAddress(received.peer,a)&&received.ssn==4095&&received.tid==7&&received.window==64);
    assert(received.dialog==19&&received.immediate&&received.amsdu);
    uint8_t reply[33];blockAckResponse(reply,b,received,0,true);
    BlockAckAnswer answer={};assert(blockAckAnswer(reply,sizeof(reply),a,answer));
    assert(sameAddress(answer.peer,b)&&answer.dialog==19&&answer.tid==7&&answer.status==0&&answer.window==64);
    for(size_t length=0;length<sizeof(reply);++length)assert(!blockAckAnswer(reply,length,a,answer));
    for(unsigned byte:{0,1,4,10,22,24,25}) {
        auto saved=reply[byte];reply[byte]^=1;
        assert(!blockAckAnswer(reply,sizeof(reply),a,answer));reply[byte]=saved;
    }
    uint8_t ordered[37]={};memcpy(ordered,reply,24);ordered[1]|=0x80;memcpy(ordered+28,reply+24,9);
    assert(blockAckAnswer(ordered,sizeof(ordered),a,answer));
    blockAckResponse(reply,b,received,1,false);
    assert(blockAckAnswer(reply,sizeof(reply),a,answer)&&answer.status==37&&!answer.immediate);
    uint8_t end[30];blockAckTeardown(end,b,received,2);
    assert(blockAckTXEnd(end,sizeof(end),a,received)&&received.tid==7);
    assert(!blockAckEnd(end,sizeof(end),a,received));
    blockAckTXTeardown(end,a,request,3);
    assert(blockAckEnd(end,sizeof(end),b,received)&&!blockAckTXEnd(end,sizeof(end),b,received));
}
int main() {
    switch(CASE) {
    case 1:baCompletion();break;case 2:aggregateReport();break;case 3:rejectedBA();break;
    case 4:singleFinal();break;case 5:peerFlush();break;case 6:callbackReset();break;case 7:wireProtocol();break;
    }
}
