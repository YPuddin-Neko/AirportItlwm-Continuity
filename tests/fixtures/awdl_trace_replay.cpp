    struct HardwareEntry {uint64_t token,doneUS;uint32_t budgetUS;bool failed;unsigned queue;};
    std::deque<HardwareEntry> hardware;
    uint64_t startAtUS=0,endAtUS=0,startDelayUS=0,completionExtraUS=80;
    unsigned failEvery=0,attempts=0,budgetRejected=0;
    bool aggregateProfile=false;
    void flushHardware() {
        auto cancelled=hardware;hardware.clear();com.txq[1].queued=com.txq[2].queued=0;
        for(const auto &entry:cancelled)dataCompletion(entry.token,ECANCELED);
    }
    uint64_t nextHardwareEvent() const {
        uint64_t next=std::numeric_limits<uint64_t>::max();
        if(startAtUS)next=std::min(next,startAtUS);
        if(endAtUS)next=std::min(next,endAtUS);
        if(!hardware.empty())next=std::min(next,hardware.front().doneUS);
        return next;
    }
    void pump() {
        if(startAtUS && clockUS>=startAtUS) {startAtUS=0;notify(1);}
        while(!hardware.empty() && hardware.front().doneUS<=clockUS &&
              (!endAtUS || hardware.front().doneUS<=endAtUS)) {
            const auto entry=hardware.front();hardware.pop_front();--com.txq[entry.queue].queued;
            dataCompletion(entry.token,entry.failed?EIO:0);
        }
        if(endAtUS && clockUS>=endAtUS) {endAtUS=0;notify(0);flushHardware();}
    }
// REPLAY SCENARIOS
struct ReplayEvent {
    uint64_t at,phase;
    unsigned kind,who,length,sequence,retry,ack,counter,period,presence,channels[16];
};
static void addressFor(unsigned who,uint8_t *address) {
    memset(address,0,6);address[0]=2;address[5]=uint8_t(who+2);
}
static void applyAdvertisement(AirportItlwmAWDLInterface &i,const ReplayEvent &e) {
    if(!e.who) {
        i.sync.periodTU=e.period;i.sync.presence=e.presence;
        i.sync.epochUS=e.at-e.phase;i.sync.epochCounter=e.counter-e.counter%e.presence;
        for(unsigned n=0;n<16;++n)i.sync.channels[n]=e.channels[n];
        return;
    }
    auto &p=i.device.awdlPeers.peers[e.who-1];
    p.occupied=p.haveSync=p.haveSequence=true;addressFor(e.who,p.address);
    p.lastSeenUS=p.syncReceivedUS=p.sequenceReceivedUS=e.at;
    if(i.device.aggregateProfile) {
        p.metadata.fields|=HTCapabilities;p.metadata.rxHTMcs=0xff;p.htCapabilitiesReceivedUS=e.at;
        p.txRate.prepare(i.device.dmaEpoch,6,12,i.device.awdlRateSamples,0xff);
        p.txRate.current=0x87;
    }
    const uint64_t cycle=uint64_t(e.period)*e.presence*1024;
    const uint64_t phase=e.phase%cycle;
    p.sync.windowCounter=uint16_t(e.counter+(e.phase/cycle)*e.presence);
    p.sync.windowPeriodTU=e.period;p.sync.presenceMode=e.presence;
    const auto whole=phase/1024;
    p.sync.nextWindowTU=e.period*e.presence-whole;
    p.sync.targetTxTime=1;p.sync.phyTxTime=1+phase%1024;
    p.sequence.count=16;p.sequence.step=e.presence-1;p.sequence.fill=0xffff;
    for(unsigned n=0;n<16;++n)p.sequence.channel[n]=e.channels[n];
}
static mbuf_t offeredPacket(AirportItlwmAWDLInterface &i,unsigned peer,unsigned bytes=1414) {
    assert(bytes>=54 && bytes<=1514);
    auto p=awdlPacket(bytes);auto b=p->bytes.data();
    addressFor(peer,b);copyAddress(b+6,i.address.octet);b[12]=0x86;b[13]=0xdd;b[14]=0x60;
    const unsigned payload=bytes-14-40;b[18]=uint8_t(payload>>8);b[19]=uint8_t(payload);b[20]=17;b[21]=64;
    return p;
}
int main(int argc,char **argv) {
    assert(argc==9 || argc==10);
    assert(ReplayBudget::rateCode(0x4207,true)==0x87);
    std::ifstream input(argv[1]);assert(input.good());
    std::vector<ReplayEvent> events;ReplayEvent e={};
    while(input>>e.at>>e.kind>>e.who>>e.length>>e.sequence>>e.retry>>e.ack>>e.phase>>e.counter>>e.period>>e.presence) {
        for(auto &channel:e.channels)input>>channel;
        assert(input.good());events.push_back(e);
    }
    assert(!events.empty());
    const unsigned peer=unsigned(std::stoul(argv[2])),load=unsigned(std::stoul(argv[3]));
    const uint64_t begin=events.front().at,end=events.back().at;
    const unsigned backlogLimit=unsigned(std::stoul(argv[7]));
    const bool recordedOffers=bool(std::stoul(argv[8]));
    uint64_t acknowledgedBytes=0,offeredBytes=0,upstreamDroppedBytes=0,maxBacklog=0,fullTicks=0,ticks=0;
    uint64_t lastOpportunity=begin,maxOpportunityGap=0;
    uint64_t advertisedUS=0,advertisedReceiveUS=0,reservedUS=0,runningUS=0;
    uint64_t reservationStreakUS=0,maxReservationStreakUS=0;
    uint64_t incoming=0,opportunities=0,observedACK=0,missedObservedACK=0;
    uint64_t slotIncoming[16]={},slotOpportunity[16]={},slotObservedACK[16]={};
    std::vector<uint64_t> delays;
    unsigned aggregates=0,mpdus=0,grants=0,flushes=0,rejected=0;uint64_t abandoned=0,expired=0,driverFlushed=0,driverRetried=0,submitted=0;
    {
        clockUS=begin;AirportItlwmAWDLInterface i;
        i.device.aggregateProfile=argc==10 && bool(std::stoul(argv[9]));
        i.device.startDelayUS=std::stoull(argv[4]);i.device.completionExtraUS=std::stoull(argv[5]);i.device.failEvery=unsigned(std::stoul(argv[6]));
        i.sync.start(i.address.octet,6,clockUS);i.sequenceConfigured=i.running=true;i.network.up=true;
        i.nextActionUS=i.nextMIFUS=end+10000000;
        i.device.awdlReceiver=&i;i.device.awdlAvailabilityChanged=[](void *p){static_cast<AirportItlwmAWDLInterface *>(p)->updateAvailability();};
        assert(!i.device.startAWDLRadio(i.address.octet));
        i.device.dataCompletion=[&](uint64_t token,int result) {
            if(!result)for(const auto &flight:i.inFlight)if(flight.token==token) {
                acknowledgedBytes+=flight.entry.packet->length;
                delays.push_back(clockUS-flight.entry.packet->offeredUS);
                if(flight.entry.second) {
                    acknowledgedBytes+=flight.entry.second->length;
                    delays.push_back(clockUS-flight.entry.second->offeredUS);
                }
                break;
            }
            i.completeData(token,result);
        };
        // The retained trace starts during an established connection. Seed each
        // participant from its first recorded advertisement, then replay updates.
        bool seeded[33]={};
        for(const auto &record:events)if(record.kind==1 && !seeded[record.who]) {
            auto seed=record;
            const uint64_t cycle=uint64_t(seed.period)*seed.presence*1024;
            const uint64_t back=seed.at-begin;
            const uint64_t eaws=(back+cycle-1)/cycle;
            seed.phase+=eaws*cycle-back;seed.counter=uint16_t(seed.counter-eaws*seed.presence);
            seed.counter=uint16_t(seed.counter+(seed.phase/cycle)*seed.presence);seed.phase%=cycle;seed.at=begin;
            applyAdvertisement(i,seed);seeded[record.who]=true;
        }
        assert(seeded[0] && seeded[peer]);
        auto offer=[&](unsigned bytes) {
            offeredBytes+=bytes;
            if(backlogLimit && i.network.output.size()>=backlogLimit)upstreamDroppedBytes+=bytes;
            else i.network.output.push_back(offeredPacket(i,peer,bytes));
        };
        uint64_t nextTick=begin,nextOffer=load?begin:UINT64_MAX;
        const uint64_t offerStep=load?1414ULL*8/load:0;
        size_t index=0;
        while(clockUS<end) {
            const uint64_t eventAt=index<events.size()?events[index].at:end;
            uint64_t next=std::min({nextTick,nextOffer,eventAt,i.device.nextHardwareEvent(),end});
            // Integrate before processing the next event. Split at advertised
            // EAW boundaries, which need not coincide with a timer or packet.
            for(uint64_t at=clockUS;at<next;) {
                Window local={};assert(i.sync.window(at,local));
                const uint64_t step=std::min(next-at,local.untilNextEAWUS);
                assert(step);
                const auto &d=i.device.awdlRadio.dwell;
                const bool reserved=bool(i.device.endAtUS);
                const bool ready=d.running && !d.cancelling && reserved;
                if(reserved) {
                    reservedUS+=step;reservationStreakUS+=step;
                    maxReservationStreakUS=std::max(maxReservationStreakUS,reservationStreakUS);
                } else reservationStreakUS=0;
                if(ready)runningUS+=step;
                if(local.channel) {
                    advertisedUS+=step;
                    if(ready && i.device.awdlRadio.channel==local.channel)advertisedReceiveUS+=step;
                }
                at+=step;
            }
            assert(next>=clockUS);clockUS=next;i.device.pump();
            while(index<events.size() && events[index].at<=clockUS) {
                const auto &record=events[index++];
                if(record.kind==1) {
                    // Recorded schedules are an external native-controller input;
                    // all AF updates are supplied, an optimistic receive assumption.
                    applyAdvertisement(i,record);
                } else if(record.kind==3) {
                    if(recordedOffers)offer(record.length);
                } else {
                    ++incoming;Window window={};i.sync.window(clockUS,window);
                    const unsigned slot=(window.counter/i.sync.presence)%16;++slotIncoming[slot];
                    const auto &d=i.device.awdlRadio.dwell;
                    const bool ready=d.running && !d.cancelling && i.device.awdlRadio.channel==6 &&
                                     i.device.endAtUS && clockUS<i.device.endAtUS;
                    if(ready) {
                        ++opportunities;++slotOpportunity[slot];
                        maxOpportunityGap=std::max(maxOpportunityGap,clockUS-lastOpportunity);lastOpportunity=clockUS;
                        i.device.awdlPeers.peers[record.who-1].lastUnicastDataUS=clockUS;
                    }
                    if(record.ack) {++observedACK;++slotObservedACK[slot];if(!ready)++missedObservedACK;}
                }
            }
            if(clockUS==nextOffer) {
                offer(1414);nextOffer+=offerStep;
            }
            if(clockUS==nextTick) {
                i.tick();++ticks;if(i.pendingCount+i.inFlightCount+i.aggregateExtraOwned==64)++fullTicks;
                assert(i.pendingCount+i.inFlightCount+i.aggregateExtraOwned<=64);
                nextTick=clockUS+uint64_t(i.scheduler.delay?i.scheduler.delay:5)*1000;
            }
            while(i.pendingCount+i.inFlightCount+i.aggregateExtraOwned<64 && !i.network.output.empty()) {
                const auto before=i.network.output.size();i.pullOutputPackets();
                if(i.network.output.size()==before)break;
            }
            maxBacklog=std::max(maxBacklog,uint64_t(i.network.output.size()));
        }
        aggregates=i.dataAggregatesSubmitted;mpdus=i.device.tx;
        grants=i.device.grants;flushes=i.device.flushes;rejected=i.device.budgetRejected;
        abandoned=i.dataAbandoned;expired=i.dataTxExpired;
        driverFlushed=i.dataFlushed;driverRetried=i.dataRetried;submitted=i.dataSubmitted;
        i.stopping=true;i.device.stopAWDLRadio();i.drain();
    }
    assert(!allocations);
    assert(acknowledgedBytes<=offeredBytes && upstreamDroppedBytes<=offeredBytes);
    assert(!backlogLimit || maxBacklog<=backlogLimit);
    assert(!offeredBytes || acknowledgedBytes);
    maxOpportunityGap=std::max(maxOpportunityGap,end-lastOpportunity);
    std::sort(delays.begin(),delays.end());
    const uint64_t p95=delays.empty()?0:delays[(delays.size()-1)*95/100];
    std::cout<<"{\"durationUS\":"<<end-begin<<",\"offeredBytes\":"<<offeredBytes
      <<",\"acknowledgedBytes\":"<<acknowledgedBytes<<",\"goodputMbps\":"<<double(acknowledgedBytes)*8/(end-begin)
      <<",\"maxBSDBacklogPackets\":"<<maxBacklog<<",\"fullQueueTicks\":"<<fullTicks<<",\"ticks\":"<<ticks
      <<",\"upstreamDroppedBytes\":"<<upstreamDroppedBytes<<",\"maxReceiveOpportunityGapUS\":"<<maxOpportunityGap
      <<",\"advertisedUS\":"<<advertisedUS<<",\"advertisedReceiveUS\":"<<advertisedReceiveUS
      <<",\"reservedUS\":"<<reservedUS<<",\"runningUS\":"<<runningUS
      <<",\"maxReservationStreakUS\":"<<maxReservationStreakUS
      <<",\"completionP95US\":"<<p95<<",\"grants\":"<<grants<<",\"flushes\":"<<flushes
      <<",\"budgetRejected\":"<<rejected<<",\"abandoned\":"<<abandoned<<",\"expired\":"<<expired
      <<",\"aggregatedMPDUs\":"<<aggregates<<",\"submittedMPDUs\":"<<mpdus
      <<",\"submitted\":"<<submitted<<",\"driverFlushed\":"<<driverFlushed<<",\"driverRetried\":"<<driverRetried
      <<",\"incomingFrames\":"<<incoming<<",\"receiveOpportunities\":"<<opportunities
      <<",\"observedACKFrames\":"<<observedACK<<",\"observedACKOutsideModelGrant\":"<<missedObservedACK<<",\"slots\":[";
    for(unsigned slot=0;slot<16;++slot) {
        if(slot)std::cout<<",";
        std::cout<<"{\"slot\":"<<slot<<",\"incoming\":"<<slotIncoming[slot]<<",\"opportunities\":"<<slotOpportunity[slot]<<",\"observedACK\":"<<slotObservedACK[slot]<<"}";
    }
    std::cout<<"]}\n";
}
