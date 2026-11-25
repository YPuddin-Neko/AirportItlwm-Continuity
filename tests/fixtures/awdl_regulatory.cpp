
static apple80211_awdl_sync_channel_sequence plan() {
    apple80211_awdl_sync_channel_sequence p={};p.version=1;
    p.length=16;p.encoding=1;p.step_count=3;p.fill_channel=0xff;
    for(auto &c:p.seqs)c={1,44,APPLE80211_C_FLAG_5GHZ|APPLE80211_C_FLAG_20MHZ};
    return p;
}
int main() {
    for(unsigned interleave=0;interleave<3;++interleave) {
        clockUS=1000000;
        AirportItlwmAWDLInterface i;setupUnicast(i);
        auto p=plan();p.seqs[8]={1,149,APPLE80211_C_FLAG_5GHZ};
        assert(!i.setSequence(&p));i.tick();assert(i.nativeWindowOpen);
        auto payload=umiPayload();auto request=umiRequest(payload);
        assert(!i.queueUnicast(request));
        memset(request.destination,0xff,6);request.flags=6;request.tx_channel_map=0xffff;
        assert(!i.configureBroadcast(request,true));assert(!i.configureBroadcast(request,false));
        const auto oldSequence=i.sync;
        const unsigned attempts=i.device.transmitAttempts;
        auto restrict=[&] {i.device.allow149=false;i.regulatoryMapChanged();};
        if(interleave==1) {i.device.onService=restrict;i.tick();i.device.onService={};}
        else {
            restrict();
            if(interleave==2) {
                // A complete SET may arrive before timer cleanup starts.
                auto good=plan();assert(!i.setSequence(&good));
                assert(!i.regulatoryPlanBlocked && i.regulatoryStopRequired);
            }
            i.updateAvailability();i.serviceUnicast();
            assert(i.device.transmitAttempts==attempts);
            if(interleave==2) {
                i.tick();assert(!i.running && i.regulatoryResumePending);
                assert(!i.device.awdlRadio.ready && !i.regulatoryStopRequired);
            }
        }
        assert(i.requested && !i.immediateBroadcast.size);
        assert(i.unicastFailed==1 && i.device.tokens.empty());
        if(interleave!=2) {
            assert(i.regulatoryPlanBlocked);
            assert(!memcmp(oldSequence.channels,i.sync.channels,sizeof(i.sync.channels)));
            auto bad=plan();bad.seqs[15]={1,149,APPLE80211_C_FLAG_5GHZ};
            assert(i.setSequence(&bad)==kIOReturnUnsupported && i.regulatoryPlanBlocked);
            assert(!memcmp(oldSequence.channels,i.sync.channels,sizeof(i.sync.channels)));
            for(unsigned n=0;n<10;++n) {clockUS+=5000;i.tick();}
            assert(i.device.transmitAttempts==attempts && !i.running && !i.nativeWindowOpen);
            assert(!i.device.awdlRadio.ready && !i.regulatoryStopRequired);
            const unsigned stops=i.device.stops;
            i.device.allow149=true;i.regulatoryMapChanged();i.tick();
            assert(i.regulatoryPlanBlocked && i.device.stops==stops && i.device.transmitAttempts==attempts);
            i.device.allow149=false;auto good=plan();assert(!i.setSequence(&good));
        }
        clockUS+=65536;i.tick();
        assert(!i.regulatoryPlanBlocked && !i.regulatoryStopRequired && !i.regulatoryResumePending &&
            i.running && i.requested);
        assert(i.device.transmitAttempts>attempts && i.device.tokens.empty());
        for(const auto &entry:i.unicast)assert(entry.state==UnicastRequest::Free);
        Advertisement a={};const auto &last=i.device.broadcastFrames.back();
        assert(parseMPDU(last.data(),last.size(),a)==ParseResult::Valid);
        for(auto channel:a.sequence.channel)assert(channel==44);
    }
    {
        clockUS=2000000;
        AirportItlwmAWDLInterface i;setupUnicast(i);
        auto p=plan();p.seqs[8]={1,149,APPLE80211_C_FLAG_5GHZ};assert(!i.setSequence(&p));
        auto payload=umiPayload();auto request=umiRequest(payload);memset(request.destination,0xff,6);
        request.tx_channel_map=0xffff;assert(!i.configureBroadcast(request,true));i.tick();
        assert(i.broadcast.sent);const auto submitted=i.broadcastSubmitted;
        const auto counter=i.broadcast.lastWindowCounter;
        i.device.allow149=false;i.regulatoryMapChanged();auto good=plan();assert(!i.setSequence(&good));
        i.tick();assert(!i.running&&i.regulatoryResumePending);
        clockUS+=5000;i.tick();
        assert(i.running&&!i.regulatoryResumePending&&i.broadcast.sent);
        assert(i.broadcast.lastWindowCounter==counter&&i.broadcastSubmitted==submitted);
        assert(!i.nativeWindowOpen); // An administrative pause cannot reopen the same native EAW.
    }
    {
        AirportItlwmAWDLInterface i;auto p=plan();p.seqs[8]={1,149,APPLE80211_C_FLAG_5GHZ};
        assert(!i.setSequence(&p));i.tick();i.device.allow149=false;i.regulatoryMapChanged();
        i.device.stopError=EIO;i.tick();const unsigned stops=i.device.stops;
        auto good=plan();assert(!i.setSequence(&good));
        for(unsigned n=0;n<10;++n) {clockUS+=1000000;i.tick();}
        assert(!i.running && i.device.stops==stops && i.device.awdlRadio.fault);
    }
    {
        AirportItlwmAWDLInterface i;auto p=plan();p.seqs[8]={1,149,APPLE80211_C_FLAG_5GHZ};
        assert(!i.setSequence(&p));i.tick();i.device.allow149=false;i.regulatoryMapChanged();
        assert(!i.setEnabled(false));auto good=plan();assert(!i.setSequence(&good));
        i.tick();assert(!i.requested && !i.running && !i.device.awdlRadio.ready);
    }
    assert(!allocations);
}
