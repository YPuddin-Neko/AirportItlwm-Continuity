
static BlockAckRequest txPeer(ItlIwx &d,bool rx=false) {
 auto r=request(d);auto &p=d.awdlPeers.peers[0];p.metadata.fields|=HTCapabilities;p.metadata.rxHTMcs=0xff;p.htCapabilitiesReceivedUS=nowUS;
 d.awdlRadio.channel=6;if(rx)start(d,r);return r;
}
static void prepareTX(ItlIwx &d,const BlockAckRequest &r) {
 d.noteAWDLTXBlockAck(r.peer,0,true);assert(d.awdlRadio.blockAckTX.phase==TXBlockAckQueue::Requested);
 d.serviceAWDLTXBlockAck();auto &tx=d.awdlRadio.blockAckTX;
 assert(tx.configured&&tx.phase==TXBlockAckQueue::Asking&&tx.queue==8&&tx.station==4&&!tx.paused&&!tx.enabled);
 assert(tx.sequence==254&&tx.cookie&&d.queueOwned&&d.tlcCommands.size()==1);
}
static void answerTX(ItlIwx &d,bool accepted=true,uint8_t dialog=0) {
 auto &tx=d.awdlRadio.blockAckTX;BlockAckRequest r={};copyAddress(r.peer,d.awdlRadio.address);
 r.dialog=dialog?dialog:tx.dialog;r.tid=tx.tid;r.window=32;r.immediate=r.amsdu=true;
 uint8_t frame[33];blockAckResponse(frame,tx.peer,r,0,accepted);tx.attempts=1;
 d.receiveAWDLTXBlockAck(frame,sizeof(frame));
}
static void agreeTX(ItlIwx &d) {
 answerTX(d);assert(d.awdlRadio.blockAckTX.phase==TXBlockAckQueue::Enabling);
 d.serviceAWDLTXBlockAck();auto &tx=d.awdlRadio.blockAckTX;
 assert(tx.phase==TXBlockAckQueue::Agreed&&tx.enabled&&tx.paused&&tx.window==32&&!tx.amsdu);
}
static uint64_t grant(const void *p,uint64_t now) {auto end=*static_cast<const uint64_t *>(p);return end>now?end:0;}
static void bulkAndNegotiation() {
 ItlIwx d;auto r=txPeer(d);
 for(unsigned n=0;n<100;++n)d.noteAWDLTXBlockAck(r.peer,100);
 assert(d.awdlRadio.blockAckTX.phase==TXBlockAckQueue::Idle);
 for(unsigned n=0;n<15;++n)d.noteAWDLTXBlockAck(r.peer,1500);
 assert(d.awdlRadio.blockAckTX.phase==TXBlockAckQueue::Idle);
 d.noteAWDLTXBlockAck(r.peer,1500);d.serviceAWDLTXBlockAck();auto &tx=d.awdlRadio.blockAckTX;
 assert(tx.phase==TXBlockAckQueue::Asking&&tx.configured&&d.com.sc_rx_ba_sessions==0);
 auto &tlc=*reinterpret_cast<const iwx_tlc_config_cmd_v4 *>(d.tlcCommands[0].data());
 assert(tlc.sta_id==4&&tlc.mode==IWX_TLC_MNG_MODE_HT&&tlc.chains==1&&tlc.max_ch_width==0);
 assert(tlc.ht_rates[0][0]==0xff&&tlc.ht_rates[1][0]==0&&tlc.non_ht_rates==0xfc0&&tlc.max_tx_op==4000);
 agreeTX(d);uint64_t end=nowUS+30000;assert(!d.setAWDLTXWindow(grant,&end));
 assert(!tx.paused&&tx.windowEndUS==end&&tx.resumes==1);
 d.serviceAWDLBlockAck();assert(tx.configured&&d.awdlBA.peers[0].station);
 end=0;assert(!d.setAWDLTXWindow(grant,&end));assert(tx.paused&&tx.pauses==2);
 assert(!d.clearAWDLBlockAck());assert(d.frees==1&&d.resets==1&&!d.queueOwned&&!tx.configured);
}
static void recipientEndKeepsRX() {
 ItlIwx d;auto r=txPeer(d,true);const auto baid=d.awdlBA.peers[0].sessions[0].baidPlusOne;
 prepareTX(d,r);agreeTX(d);auto &tx=d.awdlRadio.blockAckTX;const uint64_t cookie=tx.cookie;
 Packet packet;d.com.txq[8].queued=1;d.com.txq[8].data[0].m=&packet;
 BlockAckRequest end={};copyAddress(end.peer,d.awdlRadio.address);end.tid=0;
 uint8_t frame[30];blockAckTeardown(frame,r.peer,end,0);d.receiveAWDLTXBlockAck(frame,sizeof(frame));
 assert(tx.phase==TXBlockAckQueue::Retiring&&!tx.needDelba);
 d.serviceAWDLTXBlockAck();d.serviceAWDLBlockAck();
 assert(tx.phase==TXBlockAckQueue::Asking&&tx.configured&&tx.cookie==cookie&&!tx.paused&&!tx.enabled);
 assert(d.removes.empty()&&d.frees==0&&packet.frees==1);
 assert(d.awdlBA.peers[0].sessions[0].baidPlusOne==baid&&d.com.sc_rx_ba_sessions==1);
 assert(!d.clearAWDLBlockAck());assert(d.frees==1&&d.com.sc_rx_ba_sessions==0);
}
static void missingFlushPointers() {
 ItlIwx d;auto r=txPeer(d,true);prepareTX(d,r);agreeTX(d);d.flushClears=false;
 Packet packet;d.com.txq[8].queued=1;d.com.txq[8].data[0].m=&packet;
 auto &tx=d.awdlRadio.blockAckTX;tx.retire(nowUS,true);d.serviceAWDLTXBlockAck();
 assert(d.queueOwned&&!packet.frees&&d.frees==0);
 nowUS+=1000001;d.awdlPeers.peers[0].lastSeenUS=nowUS;d.serviceAWDLTXBlockAck();d.serviceAWDLBlockAck();
 assert(d.ends.size()==1&&d.queueOwned&&!packet.frees);
 auto end=d.ends[0];auto cookie=d.awdlBA.peers[0].sessions[0].cookie;
 d.completeAWDLBlockAckEnd(end,cookie);d.serviceAWDLBlockAck();d.serviceAWDLTXBlockAck();
 assert(!d.queueOwned&&!tx.configured&&packet.frees==1&&d.frees==1);
}
static void commandFailureOwnership() {
 for(unsigned offset=1;offset<=4;++offset) {
  ItlIwx d;auto r=txPeer(d);d.failCall=offset;d.noteAWDLTXBlockAck(r.peer,0,true);d.serviceAWDLTXBlockAck();
  assert(!d.queueOwned&&!d.awdlRadio.blockAckTX.configured&&!d.awdlRadio.fault);
  assert(d.awdlRadio.blockAckTX.phase==TXBlockAckQueue::Idle);
 }
 for(unsigned failure:{1u,2u}) {
  ItlIwx d;auto r=txPeer(d);prepareTX(d,r);agreeTX(d);Packet packet;
  d.flushClears=false;d.com.txq[8].queued=1;d.com.txq[8].data[0].m=&packet;
  d.failCall=d.calls+failure;assert(d.clearAWDLBlockAck()==EIO);
  assert(d.queueOwned&&d.frees==0&&d.resets==0&&!packet.frees&&d.awdlRadio.blockAckTX.configured);
  d.failCall=0;assert(!d.clearAWDLBlockAck());assert(packet.frees==1&&d.frees==1);
 }
}
static void interruptedSetup() {
 for(unsigned stage=1;stage<=4;++stage)for(bool reset:{false,true}) {
  ItlIwx d;auto r=txPeer(d);
  d.onCommand=[&](int n){if(unsigned(n)!=stage)return;if(reset){++d.dmaEpoch;d.resetAWDLBlockAck();d.awdlRadio.ready=false;d.queueOwned=false;}else d.awdlRadio.stopPending=true;};
  d.noteAWDLTXBlockAck(r.peer,0,true);d.serviceAWDLTXBlockAck();
  assert(!d.awdlRadio.blockAckTX.configured&&!d.awdlRadio.ready&&!d.queueOwned);
 }
}
static void resumeBoundary() {
 for(unsigned kind=0;kind<3;++kind) {
  ItlIwx d;auto r=txPeer(d);prepareTX(d,r);agreeTX(d);uint64_t end=nowUS+20000;
  const int at=d.calls+1;
  d.onCommand=[&](int n){if(n!=at)return;if(kind==0)nowUS=end;else if(kind==1)++d.bss.in_color;else d.awdlRadio.stopPending=true;};
  d.setAWDLTXWindow(grant,&end);
  assert(!d.awdlRadio.blockAckTX.configured||d.awdlRadio.blockAckTX.paused);
  assert(!d.awdlRadio.blockAckTX.windowEndUS);
  d.onCommand={};if(d.awdlRadio.blockAckTX.configured)assert(!d.clearAWDLBlockAck());
 }
}
static void ratesAndAntenna() {
 for(unsigned version:{2u,3u,4u}) {
  ItlIwx d;auto r=txPeer(d);d.tlcVersion=version;d.awdlMinRateMbps=24;
  prepareTX(d,r);agreeTX(d);assert(d.awdlRadio.blockAckTX.mcs==0xf0&&d.awdlRadio.blockAckTX.legacyMbps==24);
  assert(d.tlcCommands[0].size()==(version==4?sizeof(iwx_tlc_config_cmd_v4):sizeof(iwl_tlc_config_cmd_v3)-(version<3?4:0)));
  uint64_t end=nowUS+30000;assert(!d.setAWDLTXWindow(grant,&end));
  d.btCoex.enabled=true;d.btCoex.activity=IwxBTCoexState::HighTraffic;d.btCoex.txMask=2;
  assert(!d.setAWDLTXWindow(grant,&end));assert(d.awdlRadio.blockAckTX.antenna==2&&!d.awdlRadio.blockAckTX.paused);
  d.awdlPeers.peers[0].htCapabilitiesReceivedUS=nowUS-Peer::ScheduleFreshnessUS;
  assert(!d.setAWDLTXWindow(grant,&end));assert(d.awdlRadio.blockAckTX.paused);
  assert(!d.clearAWDLBlockAck());
 }
}
static void negotiationRaces() {
 ItlIwx d;auto r=txPeer(d);prepareTX(d,r);auto &tx=d.awdlRadio.blockAckTX;
 auto &stats=d.awdlRadio.blockAckTXStats;
 tx.managementToken=90;assert(d.completeAWDLTXManagement(90,true));
 assert(!d.completeAWDLTXManagement(90,false));
 tx.managementToken=91;assert(d.completeAWDLTXManagement(91,false));
 assert(stats.acknowledged==1&&stats.notAcknowledged==1);
 answerTX(d,true,uint8_t(tx.dialog+1));assert(tx.phase==TXBlockAckQueue::Asking);
 assert(stats.responses==1&&stats.unmatched==1&&!stats.refusals);
 const uint8_t before=tx.dialog;answerTX(d,false);assert(tx.dialog!=before&&tx.nextAskUS==nowUS+20000000&&!tx.attempts);
 assert(stats.responses==2&&stats.unmatched==1&&stats.refusals==1&&stats.lastStatus==37);
 answerTX(d,true,before);assert(tx.phase==TXBlockAckQueue::Asking);
 answerTX(d);assert(tx.phase==TXBlockAckQueue::Enabling);
 assert(stats.responses==4&&stats.unmatched==2&&stats.refusals==1&&!stats.lastStatus);
 const int at=d.calls+1;
 d.onCommand=[&](int n){if(n!=at)return;BlockAckRequest end={};copyAddress(end.peer,d.awdlRadio.address);
  uint8_t f[30];blockAckTeardown(f,r.peer,end,0);d.receiveAWDLTXBlockAck(f,sizeof(f));};
 d.serviceAWDLTXBlockAck();assert(tx.phase==TXBlockAckQueue::Asking&&!tx.enabled&&tx.agreements==0);
 assert(!d.clearAWDLBlockAck());
 assert(stats.responses==4&&stats.acknowledged==1&&!tx.configured);
}
static void ratesChangeDuringCommand() {
 for(unsigned change=0;change<3;++change)for(bool duringTLC:{false,true}) {
  ItlIwx d;auto r=txPeer(d);prepareTX(d,r);agreeTX(d);auto &tx=d.awdlRadio.blockAckTX;
  if(duringTLC)d.awdlMinRateMbps=18;
  uint64_t end=nowUS+30000;const int at=d.calls+1;
  d.onCommand=[&](int n){if(n!=at)return;
   if(change==0){d.btCoex.enabled=true;d.btCoex.activity=IwxBTCoexState::HighTraffic;d.btCoex.txMask=2;}
   if(change==1)d.awdlPeers.peers[0].metadata.rxHTMcs=2;
   if(change==2)d.awdlMinRateMbps=24;
  };
  assert(!d.setAWDLTXWindow(grant,&end));assert(tx.paused&&!tx.windowEndUS);
  d.onCommand={};assert(!d.clearAWDLBlockAck());
 }
}
static void wideRatesAndNSS() {
 for(unsigned version:{2U,3U,4U})for(unsigned variant=0;variant<7;++variant) {
  ItlIwx d;auto r=txPeer(d);d.tlcVersion=version;d.awdlRadio.channel=149;d.awdlRadio.widthMHz=80;
  auto &p=d.awdlPeers.peers[0];p.metadata.rxHTMcs2=0xff;p.haveSync=p.haveSequence=true;
  p.syncReceivedUS=p.sequenceReceivedUS=nowUS;p.sync.presenceMode=4;p.sync.windowPeriodTU=16;p.sync.nextWindowTU=64;
  p.sequence.count=16;p.sequence.fill=0xffff;p.sequence.step=3;p.sequence.encoding=3;
  for(unsigned n=0;n<16;++n){p.sequence.channel[n]=149;p.sequence.flags[n]=128;}
  if(variant==1)d.mimo=false;
  if(variant==2)d.txAnt=1;
  if(variant==3)d.rxAnt=1;
  if(variant==4)p.metadata.rxHTMcs2=0;
  if(variant==5)for(auto &flag:p.sequence.flags)flag=124;
  if(variant==6)d.awdlRadio.widthMHz=20;
  prepareTX(d,r);auto &tx=d.awdlRadio.blockAckTX;
  const bool two=variant==0||variant>=5,wide=variant<5;
  assert(tx.antenna==(two?3:1)&&tx.mcs==0xf0&&tx.mcs2==(two?0xf0:0)&&tx.widthMHz==(wide?80:20));
  auto flags=d.adds[0].station_flags;
  assert((flags&IWX_STA_FLG_MIMO_EN_MSK)==((variant>=1&&variant<=3)?IWX_STA_FLG_MIMO_EN_SISO:IWX_STA_FLG_MIMO_EN_MIMO2));
  assert((flags&IWX_STA_FLG_FAT_EN_MSK)==(variant==6?IWX_STA_FLG_FAT_EN_20MHZ:IWX_STA_FLG_FAT_EN_80MHZ));
  auto check=[&](const auto &cfg) {
   assert(cfg.mode==IWX_TLC_MNG_MODE_VHT);
   assert(cfg.max_ch_width==(wide?IWX_TLC_MNG_CH_WIDTH_80MHZ:IWX_TLC_MNG_CH_WIDTH_20MHZ));
   const uint16_t expMcs = wide ? 0x03ff : 0x01ff;
   assert(cfg.chains==(two?3:1)&&cfg.ht_rates[0][0]==expMcs&&cfg.ht_rates[1][0]==(two?expMcs:0));
  };
  if(version==4)check(*reinterpret_cast<const iwx_tlc_config_cmd_v4 *>(d.tlcCommands[0].data()));
  else {auto &cfg=*reinterpret_cast<const iwl_tlc_config_cmd_v3 *>(d.tlcCommands[0].data());check(cfg);assert(!cfg.amsdu);}
  agreeTX(d);uint64_t end=nowUS+30000;assert(!d.setAWDLTXWindow(grant,&end)&&!tx.paused);
  d.onCommand=[&](int){for(auto &flag:p.sequence.flags)flag=124;p.metadata.rxHTMcs2=0;p.metadata.rxHTMcs=0;};
  d.awdlMinRateMbps=24;auto result=d.setAWDLTXWindow(grant,&end);
  assert((!result||result==EAGAIN)&&tx.paused&&!tx.windowEndUS);
  d.onCommand={};assert(!d.clearAWDLBlockAck());
 }
}

static void negotiationWindowLifecycle() {
 ItlIwx d;auto r=txPeer(d,true);prepareTX(d,r);auto &tx=d.awdlRadio.blockAckTX;
 tx.attempts=2;const auto cookie=tx.cookie;uint64_t end=nowUS+30000;
 for(unsigned n=0;n<4;++n) {
  d.awdlRadio.dwell.running=true;
  assert(!d.closeAWDLListenerWindow());
  assert(!tx.paused && !tx.enabled && tx.cookie==cookie && tx.attempts==2);
  nowUS+=65536;end=nowUS+30000;
  assert(!d.setAWDLTXWindow(grant,&end));
  assert(!tx.paused && !tx.enabled && !tx.windowEndUS);
 }
 agreeTX(d);assert(!d.setAWDLTXWindow(grant,&end) && !tx.paused);
 assert(!d.closeAWDLListenerWindow() && tx.paused && tx.enabled);
 assert(!d.clearAWDLBlockAck());
}
static void peerWidthKeepsAgreement() {
 for(unsigned scenario=0;scenario<4;++scenario) {
  ItlIwx d;auto r=txPeer(d,true);prepareTX(d,r);agreeTX(d);auto &tx=d.awdlRadio.blockAckTX;
  const auto cookie=tx.cookie;const auto rxCookie=d.awdlBA.peers[0].sessions[0].cookie;
  const auto baid=d.awdlBA.peers[0].sessions[0].baidPlusOne;
  Packet packet;d.com.txq[8].queued=1;d.com.txq[8].data[0].m=&packet;
  auto count=d.adds.size();d.awdlRadio.widthMHz=80;
  if(scenario==1)d.failCall=d.calls+1;
  if(scenario>=2)d.onCommand=[&](int){if(scenario==2){++d.dmaEpoch;d.resetAWDLBlockAck();}else d.awdlRadio.stopPending=true;};
  const int result=d.configureAWDLPeerWidth();
  assert(result==(scenario==1?EIO:scenario==2?ENXIO:scenario==3?ECANCELED:0));
  const auto &cmd=d.adds[count];
  assert(cmd.add_modify==IWX_STA_MODE_MODIFY && !cmd.modify_mask);
  assert(cmd.station_flags_msk==IWX_STA_FLG_FAT_EN_MSK && cmd.station_flags==IWX_STA_FLG_FAT_EN_80MHZ);
  assert(!packet.frees && d.queueOwned && !d.frees && d.removes.empty());
  if(scenario!=2) {
   assert(tx.cookie==cookie && tx.paused && tx.enabled && tx.phase==TXBlockAckQueue::Agreed);
   assert(d.awdlBA.peers[0].sessions[0].cookie==rxCookie && d.awdlBA.peers[0].sessions[0].baidPlusOne==baid);
   assert(d.com.sc_rx_ba_sessions==1);
   if(!scenario) {d.awdlRadio.widthMHz=20;assert(!d.configureAWDLPeerWidth());assert(d.adds.back().station_flags==IWX_STA_FLG_FAT_EN_20MHZ);}
   d.onCommand={};d.failCall=0;assert(!d.clearAWDLBlockAck());
  } else {d.queueOwned=false;d.iwx_reset_tx_ring(&d.com,&d.com.txq[8]);}
 }
}
static void agreedRecoveryUsesFastBackoff() {
 ItlIwx d;auto r=txPeer(d,true);prepareTX(d,r);agreeTX(d);auto &tx=d.awdlRadio.blockAckTX;
 assert(tx.agreements==1);
 BlockAckRequest end={};copyAddress(end.peer,d.awdlRadio.address);end.tid=0;
 uint8_t frame[30];blockAckTeardown(frame,r.peer,end,0);d.receiveAWDLTXBlockAck(frame,sizeof(frame));
 assert(tx.phase==TXBlockAckQueue::Retiring&&!tx.needDelba);
 d.serviceAWDLTXBlockAck();
 assert(tx.phase==TXBlockAckQueue::Asking&&!tx.attempts&&tx.nextAskUS==nowUS+250000);
 assert(!d.clearAWDLBlockAck());
}
int main() {
 switch(CASE) {case 1:bulkAndNegotiation();break;case 2:recipientEndKeepsRX();break;
 case 3:missingFlushPointers();break;case 4:commandFailureOwnership();break;case 5:interruptedSetup();break;
 case 6:resumeBoundary();break;case 7:ratesAndAntenna();break;case 8:negotiationRaces();break;case 9:ratesChangeDuringCommand();break;case 10:wideRatesAndNSS();break;
 case 11:negotiationWindowLifecycle();break;case 12:peerWidthKeepsAgreement();break;case 13:agreedRecoveryUsesFastBackoff();break;}
}
