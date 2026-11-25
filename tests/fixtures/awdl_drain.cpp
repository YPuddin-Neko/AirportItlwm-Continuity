// FIRMWARE DECLARATIONS
static uint16_t htole16(uint16_t value) {return value;}
static uint32_t htole32(uint32_t value) {return value;}
static uint32_t le32toh(uint32_t value) {return value;}
static uint64_t awdlRadioNow() {return clockUS;}
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
struct iwx_rx_packet {unsigned length;uint8_t data[16];};
static unsigned iwx_rx_packet_payload_len(const iwx_rx_packet *packet) {return packet->length;}
struct TinyRing {unsigned queued=0;bool allocated=false;};
enum {IEEE80211_M_STA=1};
struct TinyDevice {TinyRing txq[4];unsigned sc_flags=IWX_FLAG_BINDING_ACTIVE;int ns_nstate=IEEE80211_S_RUN;
    struct {int ic_opmode=IEEE80211_M_STA,ic_state=IEEE80211_S_RUN;void *ic_bss=this;
        struct {struct {unsigned if_flags=IFF_UP|IFF_RUNNING;} ac_if;} ic_ac;} sc_ic;};
struct Radio {
    int clearAWDLBlockAck() { return 0; }
    void serviceAWDLBlockAck() {}
    void serviceAWDLTXBlockAck() {}
    int serviceAWDLReceiveFilter(uint64_t (*end)(const void *,uint64_t),const void *context) {
        filterWindowEnd=end(context,clockUS);return 0;
    }
    uint64_t filterWindowEnd=0;
    int setAWDLTXWindow(uint64_t (*end)(const void *,uint64_t),const void *context) {
        auto &tx=awdlRadio.blockAckTX;if(!tx.configured)return 0;
        tx.windowEndUS=end(context,clockUS);tx.paused=!tx.windowEndUS;return 0;
    }
    bool completeAWDLTXManagement(uint64_t token,bool ack) {
        auto &tx=awdlRadio.blockAckTX;if(!token || token!=tx.managementToken)return false;
        tx.managementToken=0;if(ack && tx.phase==TXBlockAckQueue::Retiring)tx.needDelba=false;return true;
    }
    void noteAWDLTXBlockAck(const uint8_t *peer,size_t bytes,bool accepted=false) {
        awdlRadio.blockAckTX.demand(peer,bytes,clockUS,accepted);
    }
    bool awdlTxWindowOpen() {return awdlRadio.ready && awdlRadio.dwell.running && clockUS<awdlRadio.dwell.windowEndUS;}

    int prepareAWDLBlockAck(const BlockAckRequest &,uint64_t &cookie) {cookie=0;return EOPNOTSUPP;}
    bool awdlBlockAckLive(const BlockAckRequest &,uint64_t) {return false;}
    void completeAWDLBlockAckEnd(const BlockAckRequest &,uint64_t) {}

    uint8_t awdlMinRateMbps=12,awdlRateLastRetries=0;
    uint64_t awdlRateSamples=0,awdlRateIgnored=0,awdlRateProbes=0,awdlRateRaised=0,awdlRateLowered=0;
    uint32_t awdlRateLastFlags=0;
    uint16_t awdlRateLastMediaUS=0;
    IwxBTCoexState btCoex={};
    WorkLoop loop;Gate gate{&loop};
    WorkLoop *getMainWorkLoop() {return &loop;}
    Gate *getMainCommandGate() {return &gate;}
    RadioState awdlRadio={};PeerTable awdlPeers={};
    std::function<void(uint64_t,int)> dataCompletion;
    TinyDevice com;
    uint64_t dmaEpoch=1;
    void *awdlReceiver=nullptr;
    void (*awdlAvailabilityChanged)(void *)=nullptr;
    unsigned starts=0,cancels=0,flushes=0,removed=0,freed=0,unbound=0,macRemoved=0,powerRestored=0,tx=0;
    unsigned psf=0,mif=0,umi=0,grants=0;
    std::function<void()> onQueue,onGrant;
    bool failRemove=false,autoStart=true;
    Radio() {awdlRadio.widthMHz=20;loop.gated=true;awdlRadio.queue=IWX_INVALID_QUEUE;}
    int startAWDLRadio(const uint8_t *address) {
        assert(!awdlRadio.stopPending && !awdlRadio.fault && !awdlRadio.awake);
        ++starts;copyAddress(awdlRadio.address,address);
        awdlRadio.ready=awdlRadio.awake=awdlRadio.mac=true;awdlRadio.channel=149;return 0;
    }
    bool awdlChannelAllowed(unsigned channel,uint8_t width=20) const {return width==20 && (channel==6||channel==44||channel==149);}
    bool awdlWideChannelAllowed(uint32_t channel) const {return awdlChannelAllowed(channel,80);}
    int awdlPHY(uint16_t) {assert(!awdlRadio.binding && !awdlRadio.station);return 0;}
    int awdlBinding(bool add) {
        if(!add) {assert(!awdlRadio.station && !com.txq[1].allocated);++unbound;}
        return 0;
    }
    static unsigned iwx_cmd_id(unsigned id,unsigned group,unsigned version) {return id|(group<<8)|(version<<16);}
    int iwx_send_cmd_pdu(TinyDevice *device,unsigned id,unsigned flags,size_t size,const void *data) {
        assert(device==&com && !flags && loop.gated);
        if(id==iwx_cmd_id(IWX_SESSION_PROTECTION_CMD,IWX_MAC_CONF_GROUP,0)) {
            auto command=static_cast<const iwx_session_prot_cmd *>(data);
            assert(size==sizeof(*command) && command->id_and_color==1 && command->conf_id==3);
            if(command->action==IWX_FW_CTXT_ACTION_ADD) {
                assert(command->duration_tu<=64);++grants;if(autoStart)notify(1);if(onGrant)onGrant();return 0;
            }
            assert(command->action==IWX_FW_CTXT_ACTION_REMOVE && !awdlRadio.dwell.running);
            ++cancels;return 0;
        }
        assert(id==IWX_REMOVE_STA && size==sizeof(iwx_rm_sta_cmd));
        assert(static_cast<const iwx_rm_sta_cmd *>(data)->sta_id==3);
        assert(!awdlRadio.dwell.requested || awdlRadio.dwell.done ||
            (awdlRadio.dwell.cancelCompleted && awdlRadio.dwell.flushed));
        ++removed;return failRemove?EIO:0;
    }
    int iwx_send_cmd_pdu_status(TinyDevice *device,unsigned id,size_t size,const void *data,uint32_t *status) {
        assert(device==&com && id==IWX_ADD_STA && size==sizeof(iwx_add_sta_cmd));
        auto command=static_cast<const iwx_add_sta_cmd *>(data);
        assert(command->sta_id==3 && command->mac_id_n_color==1);
        *status=IWX_ADD_STA_SUCCESS;return 0;
    }
    int iwx_tvqm_enable_txq(TinyDevice *,unsigned tid,unsigned,unsigned size,unsigned station,IwxTxQueueSetup *) {
        assert((tid==IWX_MGMT_TID || tid==RadioState::DataTID) && size==64 && station==3);
        const unsigned qid=tid==IWX_MGMT_TID?1:2;assert(!com.txq[qid].allocated);
        com.txq[qid].allocated=true;if(onQueue)onQueue();return qid;
    }
    int iwx_flush_sta_tids(TinyDevice *,unsigned station,uint16_t tids) {
        assert(station==3 && tids==0xffff && !awdlRadio.dwell.running);++flushes;return 0;
    }
    void iwx_reset_tx_ring(TinyDevice *,TinyRing *ring) {
        assert(!awdlRadio.station && (!awdlRadio.dwell.requested || awdlRadio.dwell.done ||
            (awdlRadio.dwell.cancelCompleted && awdlRadio.dwell.flushed)));ring->queued=0;
    }
    void iwx_free_tx_ring(TinyDevice *,TinyRing *ring) {assert(!ring->queued && ring->allocated);ring->allocated=false;++freed;}
    int awdlMAC(uint32_t action) {assert(action==IWX_FW_CTXT_ACTION_REMOVE);++macRemoved;return 0;}
    int iwx_power_update_device(TinyDevice *) {++powerRestored;return 0;}
    int transmitAWDL(mbuf_t packet,uint64_t token=0,uint64_t deadlineUS=0) {
        assert(!deadlineUS || clockUS < deadlineUS);
        assert(awdlRadio.ready && !awdlRadio.stopPending && !awdlRadio.dwell.cancelling && awdlRadio.dwell.running);
        assert(clockUS<awdlRadio.dwell.windowEndUS);
        if(token && packet->bytes[0]==8) dataCompletion(token,0);
        else if(token) ++umi;
        else if(packet->bytes[0]==0xd0) {
            if(packet->bytes[30]==3)++mif;else ++psf;
        }
        ++tx;mbuf_freem(packet);return 0;
    }
    void notify(unsigned start) {
        iwx_rx_packet packet={16,{}};iwx_session_prot_notif body={1,1,start,3};
        memcpy(packet.data,&body,sizeof(body));receiveAWDLSession(&packet);
    }
    void receiveAWDLSession(const iwx_rx_packet *);
    int finishAWDLDwell();int serviceAWDLChannel(uint16_t,uint64_t,RadioState::GrantPolicy = {});int awdlRemoveQueue();int stopAWDLRadio();
    int serviceAWDLListener(uint16_t,uint64_t,RadioState::GrantPolicy) {assert(false);return ENOTSUP;}
};
// SCENARIOS
static void attachCallback(AirportItlwmAWDLInterface &interface) {
    interface.device.awdlReceiver=&interface;
    interface.device.awdlAvailabilityChanged=[](void *owner) {
        static_cast<AirportItlwmAWDLInterface *>(owner)->updateAvailability();
    };
}
static Peer &social6Peer(AirportItlwmAWDLInterface &i) {
    clockUS=10000000;
    attachCallback(i);
    i.sync.start(i.address.octet,6,clockUS);i.sync.epochCounter=32;
    for(auto &channel:i.sync.channels)channel=6;
    i.sequenceConfigured=i.running=true;i.network.up=true;
    assert(!i.device.startAWDLRadio(i.address.octet));
    i.nextActionUS=i.nextMIFUS=clockUS+10000000;
    auto &p=i.device.awdlPeers.peers[0];
    p.occupied=p.haveSync=p.haveSequence=true;p.address[0]=2;p.address[5]=2;
    p.lastSeenUS=p.syncReceivedUS=p.sequenceReceivedUS=clockUS;
    p.sync.windowPeriodTU=16;p.sync.presenceMode=4;p.sync.nextWindowTU=64;p.sync.windowCounter=32;
    p.sequence.count=16;p.sequence.step=3;p.sequence.fill=0xffff;
    for(auto &channel:p.sequence.channel)channel=149;
    p.sequence.channel[8]=p.sequence.channel[10]=6;
    return p;
}
static mbuf_t peerPacket(AirportItlwmAWDLInterface &i,const uint8_t *destination) {
    auto packet=awdlPacket(54);auto b=packet->bytes.data();
    copyAddress(b,destination);copyAddress(b+6,i.address.octet);b[12]=0x86;b[13]=0xdd;b[14]=0x60;
    return packet;
}
static Peer &observedIsolatedPeer11(AirportItlwmAWDLInterface &i) {
    auto &p=social6Peer(i);
    const uint8_t observed[]={149,149,149,149,48,48,48,48,6,6,149,6,48,48,48,48};
    for(unsigned n=0;n<16;++n) {
        p.sequence.channel[n]=observed[n];
        i.sync.channels[n]=n%8<6?6:0;
    }
    i.sync.epochCounter=p.sync.windowCounter=44;
    return p;
}
static void isolatedPeerGrantsAtAnySlot() {
    for(unsigned demand=0;demand<3;++demand) {
        AirportItlwmAWDLInterface i;auto &p=observedIsolatedPeer11(i);
        const uint64_t begin=clockUS;
        if(demand==0)p.lastUnicastDataUS=clockUS;
        if(demand==1)i.enqueue(peerPacket(i,p.address));
        if(demand==2) {
            i.inFlight[0]={{peerPacket(i,p.address),clockUS,i.device.dmaEpoch},9};i.inFlightCount=1;
        }
        i.tick();auto &s=i.device.awdlRadio;
        assert(s.dwell.durationTU==64 && s.dwell.peer.valid && !s.dwell.peer.dense &&
            "The observed isolated peer slot 11 must retain a demand-backed long grant");
        assert(s.dwell.peer.localCounter==44 && s.dwell.peer.remoteCounter==44);
        assert(s.dwell.windowEndUS==begin+62536 && s.dwell.eawEndUS==begin+65536);
        const auto committed=s.dwell.peer;
        clockUS=begin+4096;
        p.syncReceivedUS=p.sequenceReceivedUS=clockUS;p.sync.nextWindowTU=60;
        i.sync.epochUS+=1024;
        i.tick();
        assert(i.device.grants==1 && !i.device.cancels && s.dwell.windowEndUS==begin+62536);
        assert(s.dwell.peer.endUS==committed.endUS && sameAddress(s.dwell.peer.address,committed.address));
        clockUS=begin+62536;i.enqueue(peerPacket(i,p.address));
        const auto transmitted=i.device.tx;i.tick();assert(i.device.tx==transmitted);
        clockUS=begin+65536;i.device.notify(0);i.tick();
        assert(!s.dwell.running && s.apTime.creditUS==-65536 && s.nextGrantUS==begin+131072);
        clockUS=s.nextGrantUS-1;i.tick();assert(i.device.grants==1);
        i.drain();assert(!allocations);
    }
    // Unicast peer grants use advertised channels; multicast retains its separate social-slot rule.
    for(unsigned slot=0;slot<16;++slot) {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);
        for(unsigned n=0;n<16;++n) {
            p.sequence.channel[n]=n==slot?6:149;
            i.sync.channels[n]=(n+16-slot)%16<12?6:0;
        }
        i.sync.epochCounter=p.sync.windowCounter=slot*4;
        p.lastUnicastDataUS=clockUS;i.tick();
        const auto &d=i.device.awdlRadio.dwell;
        assert(d.durationTU==64 && d.peer.valid && !d.peer.dense);
        assert(d.peer.localCounter==slot*4 && d.peer.remoteCounter==slot*4);
        i.drain();assert(!allocations);
    }
    for(unsigned invalid=0;invalid<5;++invalid) {
        AirportItlwmAWDLInterface i;auto &p=observedIsolatedPeer11(i);
        if(invalid>0 && invalid<4)p.lastUnicastDataUS=clockUS;
        if(invalid==1)p.syncReceivedUS-=Peer::ScheduleFreshnessUS;
        if(invalid==2)p.sequenceReceivedUS-=Peer::ScheduleFreshnessUS;
        if(invalid==3)p.sequence.channel[11]=149;
        if(invalid==4) {i.enqueue(peerPacket(i,p.address));++i.device.dmaEpoch;}
        i.tick();
        assert(i.device.grants==1 && i.device.awdlRadio.dwell.durationTU==16 &&
            !i.device.awdlRadio.dwell.peer.valid);
        i.drain();assert(!allocations);
    }
    for(unsigned change=0;change<3;++change) {
        AirportItlwmAWDLInterface i;auto &p=observedIsolatedPeer11(i);
        p.lastUnicastDataUS=clockUS;i.tick();const auto committed=i.device.awdlRadio.dwell.peer;
        if(change==0)++i.device.dmaEpoch;
        if(change==1)p.sequence.channel[11]=149;
        if(change==2)p.sequence.channel[12]=6;
        assert(!i.peerGrantEnd(clockUS,committed,false));
        i.drain();assert(!allocations);
    }
}
static void peerGrants() {
    // The existing dense-slot policy still caps the identical window at 16 TU.
    {
        AirportItlwmAWDLInterface i;social6Peer(i);
        assert(!i.device.serviceAWDLChannel(6,clockUS+65536,{8,false,true}));
        assert(i.device.awdlRadio.dwell.durationTU==16);
    }
    for(unsigned demand=0;demand<3;++demand) {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);const auto begin=clockUS;
        if(demand==0)p.lastUnicastDataUS=clockUS;
        if(demand==1)i.enqueue(peerPacket(i,p.address));
        if(demand==2) {
            i.inFlight[0]={{peerPacket(i,p.address),clockUS,i.device.dmaEpoch},9};i.inFlightCount=1;
        }
        i.tick();auto &s=i.device.awdlRadio;
        assert(s.dwell.durationTU==64 && s.dwell.peer.valid && s.dwell.windowEndUS==begin+62536);
        assert(s.dwell.eawEndUS==begin+65536 && i.device.grants==1);
        clockUS=begin+25000;
        if(demand!=1)i.enqueue(peerPacket(i,p.address));
        const auto tx=i.device.tx;i.tick();
        assert(i.device.grants==1 && i.device.tx==tx+1); // Beyond the old ADD+16TU deadline.
        if(demand==1) {
            // One outgoing packet is MAC-ACKed before the peer's response arrives.
            assert(!i.pendingCount && !i.inFlightCount && !p.lastUnicastDataUS);
            clockUS=begin+30000;i.tick();clockUS=begin+35000;i.tick();
            assert(s.dwell.running && !i.device.cancels && i.device.grants==1);
            auto reply=peerPacket(i,i.address.octet);copyAddress(reply->bytes.data()+6,p.address);
            uint8_t frame[128];const auto length=dataFromEthernet(reply->bytes.data(),54,frame,sizeof(frame),1,1);
            mbuf_freem(reply);unsigned received=0;DataReceiveState rx={};
            assert(length && rx.receive(frame,length,i.device.awdlPeers,i.address.octet,clockUS,false,
                [](void *ctx,const DataView &){++*static_cast<unsigned *>(ctx);},&received));
            assert(received==1 && p.lastUnicastDataUS==clockUS);
            i.enqueue(peerPacket(i,p.address));i.tick();assert(i.device.tx==tx+2);
            p.lastUnicastDataUS=0;
            const auto frozen=s.dwell.peer;
            assert(!i.peerGrantEnd(clockUS,frozen,true) && i.peerGrantEnd(clockUS,frozen,false));
            clockUS=begin+62536;const auto endTX=i.device.tx;i.tick();
            assert(i.device.tx==endTX && s.dwell.windowEndUS==clockUS);
            clockUS=begin+65536;i.device.notify(0);i.tick();
            assert(!s.dwell.running && s.nextGrantUS>=begin+131072);
        }
        if(demand==0) {
            clockUS=s.dwell.windowEndUS;i.enqueue(peerPacket(i,p.address));
            const auto txAtEnd=i.device.tx;i.tick();assert(i.device.tx==txAtEnd);
            clockUS=begin+65536;i.device.notify(0);i.tick();
            assert(!s.dwell.running && s.nextGrantUS>=begin+131072 && i.device.grants==1);
            clockUS=s.nextGrantUS-1;i.tick();assert(i.device.grants==1);
        }
        i.drain();assert(!allocations);
    }
    // No cache-only, unrelated, invalid, expired or stale demand.
    for(unsigned kind:{0u,1u,2u,3u,6u,8u,9u,10u,11u}) {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);p.nativeCached=true;
        if(kind>=4)p.lastUnicastDataUS=clockUS;
        if(kind==1) {uint8_t other[]={2,0,0,0,0,3};i.enqueue(peerPacket(i,other));}
        if(kind==2) {auto packet=peerPacket(i,p.address);packet->bytes[14]=0;i.enqueue(packet);}
        if(kind==3) {i.enqueue(peerPacket(i,p.address));i.pending[0].queuedUS-=i.DataLifetimeUS+1;}
        if(kind==6)p.syncReceivedUS-=Peer::ScheduleFreshnessUS;
        if(kind==8)p.lastUnicastDataUS-=i.DataLifetimeUS+1;
        if(kind==9)p.lastUnicastDataUS=clockUS+1;
        if(kind==10)p.haveSequence=false;
        if(kind==11)p.sequence.channel[8]=44;
        i.tick();assert(i.device.grants==1 && i.device.awdlRadio.dwell.durationTU==16);
        if(kind==3)assert(i.dataTxExpired==1);
        i.drain();assert(!allocations);
    }
    // Both EAW clocks bound the grant, including peer slot 10 with nonzero neighbors.
    for(bool remoteEarlier:{false,true}) {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);p.lastUnicastDataUS=clockUS;
        p.sync.windowCounter=40;p.sync.nextWindowTU=remoteEarlier?48:64;
        if(!remoteEarlier)i.sync.epochUS-=16384;
        const auto end=clockUS+49152;i.tick();
        assert(i.device.awdlRadio.dwell.durationTU==48 && i.device.awdlRadio.dwell.eawEndUS==end);
        assert(i.device.awdlRadio.dwell.windowEndUS==end-3000);i.drain();
    }
    // Refreshing AF timestamps in the same physical EAW is valid; extending it is not.
    {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);p.lastUnicastDataUS=clockUS;
        i.tick();auto &s=i.device.awdlRadio;const auto deadline=s.dwell.windowEndUS;
        clockUS+=4096;p.syncReceivedUS=p.sequenceReceivedUS=clockUS;p.sync.nextWindowTU=60;
        i.sync.epochUS+=1024; // A later clock estimate cannot increase the original end.
        i.tick();assert(i.device.grants==1 && !i.device.cancels && s.dwell.windowEndUS==deadline);
        p.sync.nextWindowTU=40;i.tick(); // Shorter peer EAW withdraws the outstanding grant.
        assert(i.device.cancels==1 && !s.dwell.running && s.nextGrantUS>=clockUS+65536);
        i.drain();
    }
    // Revalidation runs after setup sleeps, after ADD, and before reuse.
    for(unsigned when=0;when<3;++when)for(unsigned change=0;change<5;++change) {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);p.lastUnicastDataUS=clockUS;
        auto mutate=[&] {
            if(change==0) {auto &other=i.device.awdlPeers.peers[1];other=p;other.address[5]=3;p.occupied=false;}
            if(change==1)p.sequence.channel[8]=44;
            if(change==2)p.sequence.channel[9]=6;
            if(change==3)p.sync.windowCounter+=4;
            if(change==4)p.syncReceivedUS-=Peer::ScheduleFreshnessUS;
        };
        if(when==0)i.device.onQueue=mutate;
        if(when==1)i.device.onGrant=mutate;
        i.tick();
        if(when==2) {mutate();i.tick();}
        auto &s=i.device.awdlRadio;
        assert(!s.dwell.running && !s.binding && !i.device.tx);
        assert(i.device.grants==(when==0?0u:1u) && i.device.cancels==(when==0?0u:1u));
        if(when)assert(s.nextGrantUS>=clockUS+65536);
        i.drain();assert(!allocations);
    }
    {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);p.lastUnicastDataUS=clockUS;
        i.tick();const auto frozen=i.device.awdlRadio.dwell.peer;
        ++i.device.dmaEpoch;assert(!i.peerGrantEnd(clockUS,frozen,false));
    }
}
static void adjacentPeerWindowsAfterAPIdle() {
    AirportItlwmAWDLInterface i;
    auto &p = social6Peer(i);
    const uint64_t begin = clockUS;
    const uint8_t observed[] = {149,149,149,149,149,149,48,48,6,6,149,6,6,6,48,48};
    for (unsigned n = 0; n < 16; ++n) {
        p.sequence.channel[n] = observed[n];
        i.sync.channels[n] = n % 8 < 6 ? 6 : 0;
    }
    auto &s = i.device.awdlRadio;
    // A previous grant's debt has already been repaid during infrastructure use.
    s.apTime.settledUS = begin - 200000;
    s.apTime.creditUS = -65536;
    p.lastUnicastDataUS = clockUS;
    i.tick();
    assert(s.dwell.durationTU == 64);
    assert(s.dwell.peer.valid && s.dwell.peer.dense && i.device.grants == 1);
    clockUS = begin + 65536;
    i.device.notify(0);
    i.tick();
    assert(!s.dwell.running && s.nextGrantUS == clockUS + 8000);
    clockUS = s.nextGrantUS;
    i.enqueue(peerPacket(i,p.address));
    i.tick();
    assert(i.device.grants == 2 && s.dwell.running && s.dwell.durationTU == 56);
    assert(s.dwell.peer.localCounter == 36 && s.dwell.peer.remoteCounter == 36);
    assert(s.dwell.apCreditUS == 8000);
    clockUS = s.session.addUS + uint64_t(s.dwell.durationTU) * 1024;
    i.device.notify(0);
    i.tick();
    assert(s.apTime.creditUS == -49344 && s.nextGrantUS == clockUS + 49344);
    i.drain();assert(!allocations);
}

static void densePeerGrants() {
    for(unsigned slot:{1u,8u,10u,13u})for(unsigned demand=0;demand<3;++demand) {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);const auto begin=clockUS;
        for(auto &channel:p.sequence.channel)channel=6;
        for(unsigned n=0;n<16;++n)i.sync.channels[n]=(n%8<6)?6:0;
        i.sync.epochCounter=p.sync.windowCounter=slot*4;
        if(demand==0)p.lastUnicastDataUS=clockUS;
        if(demand==1)i.enqueue(peerPacket(i,p.address));
        if(demand==2) {
            i.inFlight[0]={{peerPacket(i,p.address),clockUS,i.device.dmaEpoch},9};i.inFlightCount=1;
        }
        i.tick();auto &s=i.device.awdlRadio;
        assert(s.dwell.durationTU==64 && s.dwell.peer.valid && s.dwell.peer.dense);
        assert(s.dwell.windowEndUS==begin+62536 && s.dwell.eawEndUS==begin+65536);
        const auto selected=s.dwell.peer;
        clockUS=begin+25000;
        if(demand!=1)i.enqueue(peerPacket(i,p.address));
        const auto tx=i.device.tx;i.tick();
        assert(i.device.grants==1 && i.device.tx==tx+1 && s.dwell.running);
        // A new active peer cannot replace or extend the already committed grant.
        auto &other=i.device.awdlPeers.peers[1];other=p;other.address[5]=3;other.lastUnicastDataUS=clockUS;
        clockUS=begin+30000;i.tick();
        assert(i.device.grants==1 && sameAddress(s.dwell.peer.address,selected.address));
        assert(s.dwell.windowEndUS==begin+62536);
        other.lastUnicastDataUS=0;
        clockUS=begin+62536;i.enqueue(peerPacket(i,p.address));const auto cutoffTX=i.device.tx;i.tick();
        assert(i.device.tx==cutoffTX);
        clockUS=begin+65536;i.device.notify(0);i.tick();
        assert(!s.dwell.running && s.nextGrantUS>=begin+131072);
        clockUS=s.nextGrantUS-1;i.tick();assert(i.device.grants==1);
        for(unsigned n=0;n<16;++n)assert(i.sync.channels[n]==((n%8<6)?6:0));
        i.drain();assert(!allocations);
    }
    for(unsigned kind=0;kind<15;++kind) {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);
        for(auto &channel:p.sequence.channel)channel=6;
        if(kind>=4)p.lastUnicastDataUS=clockUS;
        if(kind==0)p.nativeCached=true;
        if(kind==1) {const uint8_t group[]={0x33,0x33,0,0,0,0xfb};i.enqueue(peerPacket(i,group));}
        if(kind==2) {const uint8_t other[]={2,0,0,0,0,3};i.enqueue(peerPacket(i,other));}
        if(kind==3) {i.enqueue(peerPacket(i,p.address));i.pending[0].queuedUS-=i.DataLifetimeUS+1;}
        if(kind==4)p.lastUnicastDataUS-=i.DataLifetimeUS+1;
        if(kind==5)p.lastUnicastDataUS=clockUS+1;
        if(kind==6)p.syncReceivedUS-=Peer::ScheduleFreshnessUS;
        if(kind==7)p.sequenceReceivedUS-=Peer::ScheduleFreshnessUS;
        if(kind==8)p.haveSequence=false;
        if(kind==9)p.sequence.channel[8]=44;
        if(kind==10)i.sync.channels[7]=i.sync.channels[9]=44;
        if(kind==11)i.device.com.sc_ic.ic_state=0;
        if(kind==12) {auto &other=i.device.awdlPeers.peers[1];other=p;other.address[5]=3;}
        if(kind==13) {
            auto &other=i.device.awdlPeers.peers[1];other=p;other.address[5]=3;
            other.lastUnicastDataUS=0;i.enqueue(peerPacket(i,other.address));
        }
        if(kind==14) {
            auto &other=i.device.awdlPeers.peers[1];other=p;other.address[5]=3;
            other.lastUnicastDataUS=0;
            i.inFlight[0]={{peerPacket(i,other.address),clockUS,i.device.dmaEpoch},9};i.inFlightCount=1;
        }
        i.tick();assert(i.device.grants==1);
        const auto &dwell=i.device.awdlRadio.dwell;
        assert(dwell.durationTU==(kind>=12?64:16));
        assert(dwell.peer.valid==(kind>=12));
        if(kind>=12)assert(sameAddress(dwell.peer.address,p.address));
        i.drain();assert(!allocations);
    }
    for(bool remoteEarlier:{false,true}) {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);p.lastUnicastDataUS=clockUS;
        for(auto &channel:p.sequence.channel)channel=6;
        p.sync.nextWindowTU=remoteEarlier?48:64;
        if(!remoteEarlier)i.sync.epochUS-=16384;
        const auto end=clockUS+49152;i.tick();
        assert(i.device.awdlRadio.dwell.durationTU==48);
        assert(i.device.awdlRadio.dwell.eawEndUS==end && i.device.awdlRadio.dwell.windowEndUS==end-3000);
        i.drain();
    }
    for(unsigned when=0;when<3;++when)for(unsigned change=0;change<6;++change) {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);p.lastUnicastDataUS=clockUS;
        for(auto &channel:p.sequence.channel)channel=6;
        auto mutate=[&] {
            if(change==0) {auto &other=i.device.awdlPeers.peers[1];other=p;other.address[5]=3;p.occupied=false;}
            if(change==1)p.sequence.channel[8]=44;
            if(change==2)p.sequence.channel[7]=p.sequence.channel[9]=149;
            if(change==3)p.sync.windowCounter+=4;
            if(change==4)p.syncReceivedUS-=Peer::ScheduleFreshnessUS;
            if(change==5)i.sync.channels[7]=i.sync.channels[9]=44;
        };
        if(when==0)i.device.onQueue=mutate;
        if(when==1)i.device.onGrant=mutate;
        i.tick();if(when==2) {mutate();i.tick();}
        auto &s=i.device.awdlRadio;
        assert(!s.dwell.running && !s.binding && !i.device.tx);
        assert(i.device.grants==(when==0?0u:1u) && i.device.cancels==(when==0?0u:1u));
        if(when)assert(s.nextGrantUS>=clockUS+65536);
        i.drain();assert(!allocations);
    }
    {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);p.lastUnicastDataUS=clockUS;
        for(auto &channel:p.sequence.channel)channel=6;
        const auto end=clockUS+62536;
        i.device.onQueue=[&] {auto &other=i.device.awdlPeers.peers[1];other=p;other.address[5]=3;};
        i.tick();assert(i.device.grants==1 && i.device.awdlRadio.binding);
        assert(i.device.awdlRadio.dwell.running && i.device.awdlRadio.dwell.windowEndUS==end);
        assert(sameAddress(i.device.awdlRadio.dwell.peer.address,p.address));
    }
    {
        AirportItlwmAWDLInterface i;auto &p=social6Peer(i);p.lastUnicastDataUS=clockUS;
        for(auto &channel:p.sequence.channel)channel=6;
        i.tick();const auto frozen=i.device.awdlRadio.dwell.peer;
        ++i.device.dmaEpoch;assert(!i.peerGrantEnd(clockUS,frozen,false));
    }
}
int main() {
    isolatedPeerGrantsAtAnySlot();
    adjacentPeerWindowsAfterAPIdle();
    densePeerGrants();
    peerGrants();
    {
        const uint8_t captured[]={/* CAPTURED IPHONE DATA */};
        AirportItlwmAWDLInterface interface;attachCallback(interface);
        copyAddress(interface.address.octet,captured+4);interface.sync.start(captured+4,44,clockUS);
        interface.sequenceConfigured=true;
        for(auto &ch:interface.sync.channels)ch=0;
        interface.sync.channels[0]=44;
        auto &device=interface.device;
        auto &peer=device.awdlPeers.peers[0];peer.nativeCached=peer.occupied=true;
        peer.lastSeenUS=clockUS;copyAddress(peer.address,captured+10);
        interface.tick();
        auto &state=device.awdlRadio;
        assert(state.dwell.durationTU==64 && state.dwell.running && device.grants==1);
        uint64_t epoch=interface.sync.epochUS;
        clockUS=epoch+20384;interface.tick(); // Previously inside the mandatory 8ms gap.
        assert(state.dwell.running && state.binding && state.channel==44 && device.grants==1);
        DataReceiveState receiver={};unsigned delivered=0;
        assert(receiver.receive(captured,sizeof(captured),device.awdlPeers,interface.address.octet,clockUS,false,
            [](void *ctx,const DataView &view) {
                assert(view.payload[6]==6 && (view.payload[53]&2));++*static_cast<unsigned *>(ctx);
            },&delivered));
        assert(delivered==1 && receiver.observed==1);
        // A newly adjacent slot withdraws the long grant; repeated requests and
        // a native disable/re-enable must both respect its full AP recovery time.
        interface.sync.channels[1]=44;interface.tick();
        assert(device.cancels==1 && !state.dwell.running && device.grants==1);
        const auto resume=clockUS+65536;
        assert(state.nextGrantUS>=resume);
        assert(!interface.setEnabled(false) && !interface.setEnabled(true));
        clockUS=resume-1;interface.tick();assert(device.grants==1);
        clockUS=resume;interface.tick();assert(device.grants==2 && state.dwell.durationTU<=16);
        assert(interface.network.up);
    }
    {
        AirportItlwmAWDLInterface interface;attachCallback(interface);interface.tick();
        auto &device=interface.device;assert(device.awdlRadio.dwell.running && device.tx);
        assert(interface.publishedRadioReady);
        device.com.txq[1].queued=2;
        assert(interface.setInterfaceEnable(false)==kIOReturnSuccess);
        assert(!interface.requested && !device.awdlRadio.stopPending && !device.awdlRadio.fault);
        assert(!device.awdlRadio.ready && !device.awdlRadio.awake);
        const unsigned transmitted=device.tx;
        for(unsigned n=0;n<100;++n) {clockUS+=5000;interface.tick();}
        assert(device.cancels==1 && device.flushes==1 && device.removed==1 && device.freed==1);
        assert(!device.com.txq[1].queued && device.tx==transmitted && device.starts==1);
        assert(device.awdlRadio.session.cancelCompletions==1 && !device.awdlRadio.dwellEnds);
        assert(device.awdlRadio.session.cancelCompletedUS && !device.awdlRadio.session.endUS);
        clockUS+=1000000;interface.tick();assert(!interface.publishedRadioReady);
        device.notify(0);interface.tick();
        assert(!device.awdlRadio.stopPending && !device.awdlRadio.awake && !device.awdlRadio.fault);
        assert(device.removed==1 && device.freed==1 && device.unbound==1 && device.macRemoved==1 && device.powerRestored==1);
        assert(!interface.requested && device.starts==1 && !device.com.txq[1].queued);
        assert(device.awdlRadio.session.inactive==1 && !device.awdlRadio.dwellEnds);
    }
    {
        AirportItlwmAWDLInterface interface;attachCallback(interface);interface.tick();auto &device=interface.device;
        assert(interface.setEnabled(false)==kIOReturnSuccess);assert(interface.setEnabled(true)==kIOReturnSuccess);
        interface.tick();
        assert(device.starts==2 && !device.awdlRadio.stopPending && device.awdlRadio.ready);
        assert(device.freed==1 && device.macRemoved==1 && interface.requested);
        assert(device.awdlRadio.session.cancelCompletions==1 && !device.awdlRadio.dwellEnds);
    }
    {
        AirportItlwmAWDLInterface interface;attachCallback(interface);
        interface.sequenceConfigured=true;for(auto &channel:interface.sync.channels)channel=44;
        interface.tick();auto &device=interface.device;
        const unsigned transmitted=device.tx;
        clockUS=device.awdlRadio.dwell.eawEndUS+1000;interface.tick();
        assert(device.awdlRadio.ready && !device.awdlRadio.stopPending && !device.awdlRadio.fault);
        assert(!device.awdlRadio.dwell.running && device.tx==transmitted);
        assert(device.cancels==1 && device.flushes==1 && device.freed==1);
        assert(device.awdlRadio.session.cancelCompletions==1 && !device.awdlRadio.dwellEnds);
        clockUS+=8000;interface.tick();
        assert(device.grants==2 && device.awdlRadio.dwell.running && device.starts==1 && device.tx>transmitted);
    }
    {
        AirportItlwmAWDLInterface interface;attachCallback(interface);interface.tick();auto &device=interface.device;
        device.failRemove=true;assert(interface.setEnabled(false)==kIOReturnIOError);interface.tick();
        assert(device.awdlRadio.fault && device.awdlRadio.cleanupError==EIO && !device.awdlRadio.stopPending);
        const unsigned removed=device.removed;interface.setEnabled(true);
        for(unsigned n=0;n<20;++n) {clockUS+=1000000;interface.tick();}
        assert(device.removed==removed && !device.freed && device.starts==1);
    }
    for(unsigned offset:{23000U,36800U}) {
        AirportItlwmAWDLInterface interface;attachCallback(interface);
        interface.sequenceConfigured=true;for(auto &channel:interface.sync.channels)channel=44;
        std::vector<uint8_t> payload(55);payload[0]=24;putLE16(payload.data()+1,40);
        payload[3]=2;payload[8]=1;payload[9]=2;payload[14]=1;
        payload[43]=21;payload[44]=2;payload[46]=0x93;payload[47]=2;
        payload[48]=0x20;payload[49]=4;payload[51]=0x41;
        apple80211_awdl_oob_request request={};memset(request.destination,0xff,6);
        copyAddress(request.bssid,BSSID);request.flags=6;request.data=payload.data();request.data_len=payload.size();
        request.tx_channel_map=0xffffffff;request.tx_offset_us=offset;
        assert(!interface.configureBroadcast(request,true));interface.tick();auto &device=interface.device;
        const auto epoch=interface.sync.epochUS;
        assert(device.grants==1 && device.psf==1 && !device.mif && interface.nativeWindowStarts==1);
        const uint8_t peerAddress[]={2,0,0,0,0,2};SyncEngine peer;peer.start(peerAddress,44,clockUS);
        uint8_t bytes[512];Advertisement advertisement={};
        auto size=peer.action(bytes,sizeof(bytes),clockUS,true,nullptr,0);
        assert(parseMPDU(bytes,size,advertisement)==ParseResult::Valid);
        assert(device.awdlPeers.update(peerAddress,interface.address.octet,advertisement,clockUS,44,-40));
        device.awdlPeers.find(peerAddress,clockUS)->nativeCached=true;
        clockUS=epoch+14000;interface.tick();
        assert(!device.cancels && device.grants==1 && device.psf==1 && interface.nativeWindowOpen);
        clockUS=epoch+16384;device.notify(0);interface.tick();
        assert(!device.cancels && device.freed==1 && device.grants==1 && interface.nativeWindowOpen);
        clockUS+=4000;copyAddress(request.destination,peerAddress);
        assert(!interface.queueUnicast(request));interface.tick();assert(!device.umi);
        clockUS=epoch+24384;interface.tick();
        assert(device.grants==2 && device.psf==1 && device.mif==(offset==23000) && device.umi==1);
        assert(interface.nativeWindowStarts==1 && !interface.nativeWindowEnds);
        interface.completeUnicast(interface.unicast[0].token,true);interface.serviceUnicast();
        clockUS=epoch+34384;interface.tick();
        assert(device.mif==(offset==23000));
        clockUS=epoch+39384;interface.tick();
        assert(device.mif==(offset==23000));
        clockUS=epoch+40768;device.notify(0);interface.tick();
        clockUS+=8000;interface.tick();
        assert(device.grants==3 && device.psf==1 && device.mif==1 && device.umi==1);
        assert(interface.nativeWindowStarts==1 && !interface.nativeWindowEnds);
        clockUS=epoch+65152;device.notify(0);interface.tick();
        clockUS=epoch+65536;interface.tick();
        assert(interface.nativeWindowEnds==1 && !interface.nativeWindowOpen && device.grants==3);
        clockUS=epoch+73152;interface.tick();
        assert(device.grants==4 && interface.nativeWindowStarts==2 && interface.nativeWindowCounter==4);
        assert(interface.nativeWindowEnds==1 && !device.cancels && interface.network.up);
    }
    for (unsigned kind=0;kind<9;++kind) {
        AirportItlwmAWDLInterface i;attachCallback(i);i.sequenceConfigured=true;
        for(auto &channel:i.sync.channels)channel=0;
        i.sync.channels[0]=44;
        auto &d=i.device;auto &s=d.awdlRadio;
        if(kind!=0 && kind!=6)d.awdlPeers.peers[0].nativeCached=true;
        if(kind==1)i.sync.channels[15]=6; // Dense across the sequence wrap.
        if(kind==2) {
            assert(!d.startAWDLRadio(i.address.octet));
            d.com.ns_nstate=0; // AP migration begins after radio setup while ic_state remains RUN.
        }
        if(kind==3)d.onQueue=[&] {i.sync.channels[1]=6;}; // Command sleep before ADD.
        if(kind==4)d.onGrant=[&] {i.sync.channels[1]=6;}; // Command sleep after ADD.
        if(kind==5)d.onGrant=[&] {i.sync.epochUS-=8192;}; // Shorter EAW while ADD waits.
        if(kind==8)d.autoStart=false;
        if(kind==6) {
            assert(!d.startAWDLRadio(i.address.octet));
            auto p=awdlPacket(54);auto b=p->bytes.data();
            b[0]=2;b[5]=77;copyAddress(b+6,i.address.octet);b[12]=0x86;b[13]=0xdd;b[14]=0x60;
            i.enqueue(p); // Real pending mbuf, no cached peer.
        }
        i.tick();
        if(kind<3)assert(d.grants==1 && s.dwell.durationTU==16);
        if(kind==3)assert(!d.grants && !s.dwell.running && !s.binding && !d.tx);
        if(kind==4 || kind==5)assert(d.grants==1 && d.cancels==1 && !s.dwell.running &&
            !s.binding && !d.tx && s.nextGrantUS>=clockUS+65536);
        if(kind==6)assert(d.grants==1 && s.dwell.durationTU==64 && i.pendingCount==1);
        if(kind==7) {
            assert(d.grants==1 && s.dwell.durationTU==64);
            uint64_t begin=clockUS;
            clockUS=begin+1000;d.notify(0); // Ambiguous early END with no session identifier.
            for(auto &c:i.sync.channels)c=44;
            i.tick();assert(d.grants==1 && !s.dwell.running && s.nextGrantUS>=begin+131072);
            clockUS=begin+131072-1;i.tick();assert(d.grants==1);
            clockUS=begin+131072;i.tick();assert(d.grants==2 && s.dwell.durationTU==16);
        }
        if(kind==8) {
            uint64_t begin=clockUS;
            assert(d.grants==1 && !s.dwell.running && !d.tx);
            clockUS=begin+20000;d.notify(1);i.tick();
            assert(s.dwell.running && s.dwell.windowEndUS==begin+65536-3000);
            unsigned sent=d.tx;clockUS=s.dwell.windowEndUS;i.tick();assert(d.tx==sent);
            clockUS=begin+20000+65536;d.notify(0);
            assert(!s.dwell.running && s.nextGrantUS>=clockUS+65536);
            for(auto &c:i.sync.channels)c=44;
            i.tick();clockUS=s.nextGrantUS-1;i.tick();assert(d.grants==1);
        }
        i.drain();
    }
    assert(!allocations);
}
