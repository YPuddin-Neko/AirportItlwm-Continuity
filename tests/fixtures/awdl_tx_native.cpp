static void setupNativeTX(AirportItlwmAWDLInterface &i) {
    clockUS=10000000;const uint8_t peer[]={2,0,0,0,0,2};
    dataPeer(i,peer);i.nextActionUS=i.nextMIFUS=clockUS+2000000;i.tick();
    auto &tx=i.device.awdlRadio.blockAckTX;
    tx.phase=TXBlockAckQueue::Asking;tx.configured=true;tx.cookie=55;tx.epoch=i.device.dmaEpoch;
    copyAddress(tx.peer,peer);tx.dialog=3;tx.tid=0;tx.sequence=4095;tx.window=64;tx.paused=true;
}
static void nativeTXNegotiation() {
    AirportItlwmAWDLInterface i;setupNativeTX(i);auto &tx=i.device.awdlRadio.blockAckTX;
    i.device.awdlRadio.dataQueueConfigured=true;i.device.awdlRadio.dataQueue=2;
    i.device.com.txq[2].queued=1;i.serviceTXBlockAck();assert(!tx.attempts);
    i.device.com.txq[2].queued=0;
    for(unsigned attempt=1;attempt<=TXBlockAckQueue::MaxAttempts;++attempt) {
        tx.nextAskUS=0;i.serviceTXBlockAck();assert(tx.attempts==attempt && tx.managementToken);
        assert(tx.nextAskUS==clockUS+(attempt<=2?200000u:500000u));
        assert(i.device.awdlRadio.blockAckTXStats.requests==attempt);
        assert(i.device.filterWindowEnd>clockUS&&i.device.filterWindowEnd<=i.device.awdlRadio.dwell.windowEndUS);
        BlockAckRequest request={};assert(blockAckRequest(i.device.lastUnicast.data(),i.device.lastUnicast.size(),tx.peer,request));
        assert(request.dialog==3 && request.ssn==4095 && request.window==64 && request.immediate && !request.amsdu);
        assert(i.device.transmitDeadlines.back()<i.device.awdlRadio.dwell.windowEndUS);
        const auto token=tx.managementToken;i.serviceTXBlockAck();assert(tx.managementToken==token && tx.attempts==attempt);
        i.completeUnicast(token,false);assert(!tx.managementToken);
    }
    tx.nextAskUS=0;i.serviceTXBlockAck();assert(tx.dialog==4 && !tx.attempts && tx.nextAskUS==clockUS+20000000);
    tx.retire(clockUS);i.serviceTXBlockAck();assert(tx.managementToken && tx.needDelba);
    BlockAckRequest end={};assert(blockAckEnd(i.device.lastUnicast.data(),i.device.lastUnicast.size(),tx.peer,end));
    const auto token=tx.managementToken;i.completeUnicast(token,true);assert(!tx.needDelba && !tx.managementToken);
    i.completeUnicast(token,false);assert(!tx.needDelba);i.drain();
    assert(i.device.awdlRadio.blockAckTXStats.requests==TXBlockAckQueue::MaxAttempts);
}
static void nativeTXAdmissionAndDeadline() {
    for(unsigned failure=0;failure<6;++failure) {
        AirportItlwmAWDLInterface i;setupNativeTX(i);auto &tx=i.device.awdlRadio.blockAckTX;
        const auto before=i.device.tokens.size();
        if(failure==0)allocationDelayUS=65536;
        if(failure==1)failedAllocations=1;
        if(failure==2)i.device.transmitError=ENOBUFS;
        if(failure==3)i.regulatoryPlanBlocked=true;
        if(failure==4)i.requested=false;
        if(failure==5)clockUS=i.device.awdlRadio.dwell.windowEndUS-5000;
        i.serviceTXBlockAck();allocationDelayUS=0;
        assert(!tx.attempts && !tx.managementToken && i.device.tokens.size()==before);
        assert(!i.device.awdlRadio.blockAckTXStats.requests);
        if(failure>=3)assert(!i.device.filterWindowEnd);
        if(failure==2)assert(tx.nextAskUS==clockUS+20000);
        i.drain();
    }
    AirportItlwmAWDLInterface i;setupNativeTX(i);auto &tx=i.device.awdlRadio.blockAckTX;
    tx.phase=TXBlockAckQueue::Agreed;tx.enabled=true;i.device.awdlRadio.listener=true;
    i.serviceTXBlockAck();assert(!tx.paused && tx.windowEndUS);
    const auto end=tx.windowEndUS;clockUS=end-1800;i.tick();assert(i.scheduler.delay==1);
    clockUS=end;i.serviceTXBlockAck();assert(tx.paused && !tx.windowEndUS);i.drain();
}

static void nativeTXSplitRetriedPair() {
    clockUS=10000000;const uint8_t destination[]={2,0,0,0,0,2};
    AirportItlwmAWDLInterface i;aggregatePeer(i,destination);
    i.sync.dataSequence=65535;for(unsigned n=0;n<2;++n)i.enqueue(ethernet(i.address.octet,destination));
    i.tick();assert(i.inFlightCount==1 && i.aggregateExtraOwned==1);
    i.completeData(i.device.dataTokens.back(),ECANCELED);assert(i.pendingCount==1 && i.aggregateExtraOwned==1);
    auto &tx=i.device.awdlRadio.blockAckTX;tx.configured=true;tx.phase=TXBlockAckQueue::Agreed;
    tx.enabled=true;tx.amsdu=false;tx.cookie=44;tx.epoch=i.device.dmaEpoch;copyAddress(tx.peer,destination);
    ++i.device.awdlRadio.dwellStarts;i.tick();i.tick();
    assert(!i.pendingCount && i.inFlightCount==2 && !i.aggregateExtraOwned && i.device.dataFrames.size()==3);
    for(unsigned n=1;n<=2;++n) {
        DataBatch batch={};const auto &frame=i.device.dataFrames[n];
        assert(parseDataMPDU(frame.data(),frame.size(),batch)==ParseResult::Valid && batch.count==1);
        assert(batch.frames[0].sequence==(n==1?65535:0));
        i.completeData(i.device.dataTokens[n],0);
    }
    assert(!i.inFlightCount && !allocations && i.sync.dataSequence==1);i.drain();
}

static void sidecarLatencyAndLinkQuality() {
    clockUS=10000000;const uint8_t destination[]={2,0,0,0,0,2};
    AirportItlwmAWDLInterface i;dataPeer(i,destination);
    assert(i.lastLinkQuality==100 && i.linkQualityUpdates>=1);
    const unsigned upUpdates=i.linkQualityUpdates;
    // Transient AP reassociation keeps native AWDL link & LQM active.
    i.device.com.ns_nstate=0;i.device.stopAWDLRadio();i.tick();
    assert(i.infrastructurePaused && i.lastLinkQuality==100);
    i.device.com.ns_nstate=IEEE80211_S_RUN;clockUS+=1000000;i.tick();
    assert(i.running && i.lastLinkQuality==100);
    dataPeer(i,destination);
    // Administrative disable reports LQM off (-2), then re-enable restores 100.
    const unsigned beforeDisable=i.linkQualityUpdates;
    assert(!i.setInterfaceEnable(false));
    assert(i.lastLinkQuality==-2 && i.linkQualityUpdates==beforeDisable+1);
    i.interfaceEnabled=true;assert(!i.setEnabled(true));clockUS+=1000000;i.tick();
    assert(i.running && i.lastLinkQuality==100);
    dataPeer(i,destination);

    // pullOutputPackets() arms immediate 1 ms timer wakeup outside tick().
    i.nextActionUS=i.nextMIFUS=clockUS+2000000;i.tick();
    assert(i.scheduler.delay==5);
    i.network.output.push_back(ethernet(i.address.octet,destination));
    assert(i.pullOutputPackets()==kIOReturnSuccess && i.pendingCount==1);
    assert(i.scheduler.delay==1);

    // completeData() arms immediate 1 ms wakeup both on ACK capacity release and ECANCELED retry.
    i.device.acknowledgeData=false;
    i.tick();assert(i.inFlightCount==1 && !i.pendingCount);
    i.scheduler.delay=5;
    i.completeData(i.device.dataTokens.back(),0);
    assert(!i.inFlightCount && i.scheduler.delay==1);
    i.enqueue(ethernet(i.address.octet,destination));i.tick();
    assert(i.inFlightCount==1);
    i.scheduler.delay=5;
    i.completeData(i.device.dataTokens.back(),ECANCELED);
    assert(i.pendingCount==1 && i.scheduler.delay==1);
    ++i.device.awdlRadio.dwellStarts;i.tick();
    i.completeData(i.device.dataTokens.back(),0);
    assert(!i.inFlightCount && !i.pendingCount);

    // Mid-burst PSF due transmits announcement inline without starving remaining queued frames.
    const unsigned dataBefore=i.device.data,psfBefore=i.device.psf;
    for(unsigned n=0;n<4;++n)i.enqueue(ethernet(i.address.octet,destination));
    i.nextMIFUS=clockUS+2000000;
    i.nextActionUS=clockUS+200;
    i.device.onTransmit=[&]{clockUS+=150;};
    i.tick();
    i.device.onTransmit={};
    assert(i.device.data==dataBefore+4 && i.device.psf==psfBefore+1 && !i.pendingCount);
    for(size_t k=i.device.dataTokens.size()-4;k<i.device.dataTokens.size();++k)
        i.completeData(i.device.dataTokens[k],0);
    i.drain();
}

static void multiWindowFlushSurvivalAndFIFOOrder() {
    clockUS=10000000;const uint8_t destination[]={2,0,0,0,0,2};
    // 1. A unicast packet survives up to 8 window-close ECANCELED flushes within DataLifetimeUS.
    {
        AirportItlwmAWDLInterface i;dataPeer(i,destination);i.device.acknowledgeData=false;
        i.nextActionUS=i.nextMIFUS=clockUS+2000000;
        i.sync.dataSequence=400;i.sync.wifiSequence=900;
        auto pkt=ethernet(i.address.octet,destination);pkt->bytes.back()=0x42;
        i.enqueue(pkt);i.tick();
        assert(i.inFlightCount==1 && i.dataSubmitted==1);
        for(unsigned flush=1;flush<=8;++flush) {
            i.completeData(i.device.dataTokens.back(),ECANCELED);
            assert(i.pendingCount==1 && !i.inFlightCount && i.dataFlushed==flush && !i.dataAbandoned);
            ++i.device.awdlRadio.dwellStarts;clockUS+=20000;
            dataPeer(i,destination);
            i.nextActionUS=i.nextMIFUS=clockUS+2000000;
            i.tick();
            assert(i.inFlightCount==1 && !i.pendingCount && i.dataSubmitted==flush+1 && i.dataRetried==flush);
        }
        i.completeData(i.device.dataTokens.back(),0);
        assert(!i.inFlightCount && !i.pendingCount && i.dataUnicastAcknowledged==1 && !i.dataAbandoned);
        assert(i.sync.dataSequence==401 && i.sync.wifiSequence==901);
        i.drain();
    }
    // 2. Exceeding MaxDataFlushes (9th ECANCELED) abandons cleanly without leaking mbufs.
    {
        AirportItlwmAWDLInterface i;dataPeer(i,destination);i.device.acknowledgeData=false;
        i.enqueue(ethernet(i.address.octet,destination));i.tick();
        for(unsigned flush=1;flush<=8;++flush) {
            i.completeData(i.device.dataTokens.back(),ECANCELED);
            ++i.device.awdlRadio.dwellStarts;clockUS+=20000;
            dataPeer(i,destination);i.tick();
        }
        i.completeData(i.device.dataTokens.back(),ECANCELED);
        assert(!i.pendingCount && !i.inFlightCount && i.dataAbandoned==1 && i.txDropped==1);
        i.drain();
    }
    // 3. Flushed packets requeue at the HEAD in original sequence order ahead of newly queued packets.
    {
        AirportItlwmAWDLInterface i;dataPeer(i,destination);i.device.acknowledgeData=false;
        i.sync.dataSequence=10;i.sync.wifiSequence=50;
        for(uint8_t tag:{1,2}) {
            auto p=ethernet(i.address.octet,destination);p->bytes.back()=tag;i.enqueue(p);
        }
        i.tick();
        assert(i.inFlightCount==2 && !i.pendingCount && i.device.dataFrames.size()==2);
        const auto t1=i.device.dataTokens[0],t2=i.device.dataTokens[1];
        // New packets P3, P4 arrive in pending[] before Window 1 flushes P1, P2.
        for(uint8_t tag:{3,4}) {
            auto p=ethernet(i.address.octet,destination);p->bytes.back()=tag;
            i.network.output.push_back(p);
        }
        assert(i.pullOutputPackets()==kIOReturnSuccess && i.pendingCount==2);
        // Complete flushed descriptors even in reverse order (t2 then t1) to verify sequence ordering!
        i.completeData(t2,ECANCELED);
        i.completeData(t1,ECANCELED);
        assert(i.pendingCount==4 && !i.inFlightCount);
        i.device.dataFrames.clear();i.device.dataTokens.clear();
        ++i.device.awdlRadio.dwellStarts;clockUS+=20000;
        dataPeer(i,destination);
        i.tick();
        assert(i.inFlightCount==4 && !i.pendingCount && i.device.dataFrames.size()==4);
        for(unsigned idx=0;idx<4;++idx) {
            DataBatch batch={};
            const auto &f=i.device.dataFrames[idx];
            assert(parseDataMPDU(f.data(),f.size(),batch)==ParseResult::Valid && batch.count==1);
            assert(batch.frames[0].sequence==10+idx);
            assert(batch.frames[0].payload[batch.frames[0].size-1]==idx+1);
            i.completeData(i.device.dataTokens[idx],0);
        }
        assert(i.dataUnicastAcknowledged==4 && !i.dataAbandoned);
        i.drain();
    }
}

static void timerWakeCoalescing() {
    clockUS=10000000;const uint8_t destination[]={2,0,0,0,0,2};
    AirportItlwmAWDLInterface i;dataPeer(i,destination);i.device.acknowledgeData=false;
    i.nextActionUS=i.nextMIFUS=clockUS+2000000;
    i.enqueue(ethernet(i.address.octet,destination));
    i.enqueue(ethernet(i.address.octet,destination));
    i.tick();
    assert(i.inFlightCount==2 && !i.pendingCount);
    // Simulate idle 5 ms timer armed, then a burst of completions + new output packets before tick() runs.
    i.scheduler.delay=5;
    const unsigned callsBefore=i.scheduler.calls;
    for(unsigned n=0;n<4;++n) {
        i.network.output.push_back(ethernet(i.address.octet,destination));
        assert(i.pullOutputPackets()==kIOReturnSuccess);
    }
    i.completeData(i.device.dataTokens[0],0);
    i.completeData(i.device.dataTokens[1],ECANCELED);
    assert(i.scheduler.delay==1 && i.scheduler.calls==callsBefore+1);
    // Running tick() clears wakeArmed so the next post-tick burst arms setTimeoutMS(1) once again.
    ++i.device.awdlRadio.dwellStarts;clockUS+=20000;
    dataPeer(i,destination);
    i.tick();
    for(auto tok:i.device.dataTokens) i.completeData(tok,0);
    i.nextActionUS=i.nextMIFUS=clockUS+2000000;
    i.tick();
    assert(i.scheduler.delay==5);
    const unsigned callsAfterTick=i.scheduler.calls;
    i.network.output.push_back(ethernet(i.address.octet,destination));
    assert(i.pullOutputPackets()==kIOReturnSuccess);
    i.network.output.push_back(ethernet(i.address.octet,destination));
    assert(i.pullOutputPackets()==kIOReturnSuccess);
    assert(i.scheduler.delay==1 && i.scheduler.calls==callsAfterTick+1);
    i.drain();
}
