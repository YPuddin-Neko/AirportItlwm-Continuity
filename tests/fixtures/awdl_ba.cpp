#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <deque>
#include <vector>
#include <memory>
#include <functional>
#include <sys/time.h>
#include "AirportItlwm/AWDL/BlockAck.hpp"
#include "AirportItlwm/AWDL/RadioState.hpp"
#include "itlwm/hal_iwx/BTCoex.hpp"
using namespace AirportAWDL;
#define AIRPORT_WCL
#define XYLog(...) ((void)0)
#define htole16(x) (x)
#define htole32(x) (x)
#define le16toh(x) (x)
#define le32toh(x) (x)
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define mtod(m,t) reinterpret_cast<t>((m)->bytes.data())
#define SEQ_LT(a,b) ((((a)-(b)) & 0x800) != 0)
#define IEEE80211_IS_MULTICAST(a) ((a)[0]&1)
#define USEC_TO_TIMEVAL(x,t) do {(t)->tv_sec=(x)/1000000;(t)->tv_usec=(x)%1000000;} while(0)
#define container_of(p,t,m) reinterpret_cast<t *>(reinterpret_cast<uint8_t *>(p)-offsetof(t,m))
using u8=uint8_t;using __le16=uint16_t;using __be16=uint16_t;using __le32=uint32_t;using __le64=uint64_t;
static uint64_t nowUS=10000000;
static uint64_t awdlRadioNow() {return nowUS;}
static void getmicrouptime(timeval *tv) {USEC_TO_TIMEVAL(nowUS,tv);}
static unsigned iwx_cmd_id(unsigned id,unsigned group,unsigned) {return (group<<8)|id;}
static int splnet() {return 1;}
static void splx(int) {}
struct Packet {std::vector<uint8_t> bytes;unsigned frees=0;};
using mbuf_t=Packet *;
static size_t mbuf_len(mbuf_t m) {return m->bytes.size();}
static size_t mbuf_pkthdr_len(mbuf_t m) {return mbuf_len(m);}
static void *mbuf_data(mbuf_t m) {return m->bytes.data();}
static void mbuf_freem(mbuf_t m) {assert(m && !m->frees++);}
struct mbuf_list {std::deque<mbuf_t> frames;};
#define MBUF_LIST_INITIALIZER() {}
static bool ml_empty(mbuf_list *ml) {return ml->frames.empty();}
static void ml_enqueue(mbuf_list *ml,mbuf_t m) {ml->frames.push_back(m);}
static mbuf_t ml_dequeue(mbuf_list *ml) {if(ml_empty(ml))return nullptr;auto m=ml->frames.front();ml->frames.pop_front();return m;}
static void ml_purge(mbuf_list *ml) {while(auto m=ml_dequeue(ml))mbuf_freem(m);}
struct CTimeout {void (*fn)(void *);void *arg;uint64_t deadline=0;};
static void timeout_set(CTimeout **t,void (*fn)(void *),void *arg) {if(!*t)*t=new CTimeout;(*t)->fn=fn;(*t)->arg=arg;(*t)->deadline=0;}
static void timeout_del(CTimeout **t) {if(*t)(*t)->deadline=0;}
static void timeout_free(CTimeout **t) {delete *t;*t=nullptr;}
static void timeout_add_usec(CTimeout **t,int usec) {assert(*t);(*t)->deadline=nowUS+usec;}
static bool timeout_pending(CTimeout **t) {return *t && (*t)->deadline;}
enum {IEEE80211_BA_MAX_WINSZ=64,IEEE80211_M_STA=1,IEEE80211_S_RUN=4,
 IEEE80211_QOS_TID=15,IEEE80211_FC0_TYPE_MASK=12,IEEE80211_FC0_SUBTYPE_MASK=240,
 IEEE80211_FC0_TYPE_CTL=4,IEEE80211_FC0_SUBTYPE_BAR=128,IEEE80211_FC0_SUBTYPE_NODATA=64,
 IEEE80211_RXI_AMPDU_DONE=1,IEEE80211_RXI_SAME_SEQ=2,IEEE80211_RXI_HWDEC_SAME_PN=4,
 IEEE80211_REASON_UNSPECIFIED=1,kIOReturnSuccess=0};
struct ieee80211_rxinfo {uint32_t rxi_flags=0;};
// FIRMWARE
// FROM REORDER BUFFER
struct ieee80211_frame {uint8_t i_fc[2],dur[2],i_addr1[6],i_addr2[6],i_addr3[6],seq[2];};
static bool ieee80211_has_qos(ieee80211_frame *wh) {return (wh->i_fc[0]&0x8c)==0x88;}
static uint8_t ieee80211_get_qos(ieee80211_frame *wh) {return reinterpret_cast<uint8_t *>(wh)[24];}
struct ieee80211_rx_ba {int ba_timeout_val=0;};
struct ieee80211_node {unsigned refs=0;ieee80211_rx_ba ni_rx_ba[16];};
struct ieee80211_channel {int channel;};
struct phy_context {ieee80211_channel *channel;};
struct iwx_node:ieee80211_node {phy_context *in_phyctxt;uint32_t in_id=0,in_color=1;};
struct ieee80211com {
 int ic_opmode=IEEE80211_M_STA,ic_state=IEEE80211_S_RUN,ic_if=0;
 ieee80211_node *ic_bss;
 struct {unsigned is_ht_rx_frame_above_ba_winend=0,is_ht_rx_ba_window_jump=0,
 is_ht_rx_frame_below_ba_winstart=0,is_ht_rx_ba_no_buf=0,is_ht_rx_ba_window_gap_timeout=0;} ic_stats;
};
static unsigned apFinds=0,apDelbas=0,apAccepted=0,apRefused=0;
static void ieee80211_addba_req_refuse(ieee80211com *,ieee80211_node *,int){++apRefused;}
static void ieee80211_addba_req_accept(ieee80211com *,ieee80211_node *,int){++apAccepted;}
static ieee80211_node *ieee80211_find_rxnode(ieee80211com *ic,ieee80211_frame *) {++apFinds;++ic->ic_bss->refs;return ic->ic_bss;}
static void ieee80211_release_node(ieee80211com *,ieee80211_node *ni) {assert(ni && ni->refs);--ni->refs;}
static void ieee80211_delba_request(ieee80211com *,ieee80211_node *,int,int,int) {++apDelbas;}
static void if_input(int *,mbuf_list *ml) {assert(ml_empty(ml));}
struct iwx_tx_data {mbuf_t m=nullptr;};
struct iwx_tx_ring {unsigned cur=0,queued=0,ring_count=0;bool allocated=false;iwx_tx_data data[64];};
struct iwx_softc {ieee80211com sc_ic;int ns_nstate=IEEE80211_S_RUN,sc_flags=IWX_FLAG_BINDING_ACTIVE,sc_device_family=IWX_DEVICE_FAMILY_22000,sc_generation=1;
 struct Config {uint8_t non_shared_ant=2;} config;Config *sc_cfg=&config;uint8_t sc_mgmt_last_antenna_idx=0;
 iwx_tx_ring txq[16];
 unsigned sc_rx_ba_sessions=0;struct iwx_rxba_data sc_rxba_data[IWX_MAX_BAID]={};};
using IOReturn=int;struct OSObject {};
struct Loop {bool gated=true;bool inGate() const {return gated;}};
struct Gate {Loop *loop;unsigned entries=0;IOReturn runAction(IOReturn (*fn)(OSObject *,void *,void *,void *,void *),void *arg) {
 ++entries;const bool old=loop->gated;loop->gated=true;int r=fn(nullptr,arg,nullptr,nullptr,nullptr);loop->gated=old;return r;}};
struct ItlIwx {
 iwx_softc com;Loop loop;Gate gate{&loop};uint64_t dmaEpoch=5;
 RadioState awdlRadio={};BlockAckState awdlBA={};PeerTable awdlPeers={};DataReceiveState awdlData={};
 IwxBTCoexState btCoex={};uint8_t awdlMinRateMbps=12,tlcVersion=4;
 bool queueOwned=false,flushClears=true;unsigned txStation=0,flushes=0,resets=0,frees=0,cancelled=0;
 std::vector<std::vector<uint8_t>> tlcCommands;
 ieee80211_channel channel{6};phy_context phy{&channel};iwx_node bss;
 std::vector<uint16_t> delivered;std::vector<BlockAckRequest> ends;
 void *awdlReceiver=this;
 void (*awdlAvailabilityChanged)(void *)=nullptr;
 void (*awdlInput)(void *,const DataView &)=[](void *p,const DataView &v){static_cast<ItlIwx *>(p)->delivered.push_back(v.sequence);};
 void (*awdlBlockAckInput)(void *,const BlockAckRequest &)=[](void *p,const BlockAckRequest &r){static_cast<ItlIwx *>(p)->ends.push_back(r);};
 void (*awdlBlockAckEndInput)(void *,const BlockAckRequest &)=nullptr;
 int calls=0,failCall=0,failError=EIO;bool failCleanup=false,missingBAID=false;int forcedBAID=-1;
 unsigned nextBAID=0,apFrames=0,macChanges=0;bool filterPromisc=true;
 std::vector<iwx_add_sta_cmd> adds;std::vector<unsigned> removes;
 std::function<void(int)> onCommand;
 ItlIwx() {
  bss.in_phyctxt=&phy;com.sc_ic.ic_bss=&bss;
  awdlRadio.ready=awdlRadio.binding=awdlRadio.listener=true;
  const uint8_t address[]={2,0,0,0,0,1};copyAddress(awdlRadio.address,address);
  for(auto &r:com.sc_rxba_data){r.sc=&com;r.baid=IWX_RX_REORDER_DATA_INVALID_BAID;}
 }
 ~ItlIwx() {resetAWDLBlockAck();for(auto &r:com.sc_rxba_data){iwx_clear_reorder_buffer(&com,&r);}}
 Loop *getMainWorkLoop(){return &loop;}Gate *getMainCommandGate(){return &gate;}
 int command(){++calls;if(onCommand)onCommand(calls);return calls==failCall?failError:0;}
 int iwx_send_cmd_pdu_status(iwx_softc *,int cmd,size_t len,const void *p,uint32_t *status) {
  assert(cmd==IWX_ADD_STA && len==sizeof(iwx_add_sta_cmd));const auto c=*static_cast<const iwx_add_sta_cmd *>(p);adds.push_back(c);
  assert(c.sta_id==0 || (c.sta_id>=4 && c.sta_id<8));int error=command();if(error)return error;
  *status=IWX_ADD_STA_SUCCESS;
  if(c.add_modify==IWX_STA_MODE_MODIFY && c.modify_mask==IWX_STA_MODIFY_ADD_BA_TID && !missingBAID)
   *status|=IWX_ADD_STA_BAID_VALID_MASK|((forcedBAID>=0?forcedBAID:nextBAID++)<<IWX_ADD_STA_BAID_SHIFT);
  return 0;
 }
 int iwx_send_cmd_pdu(iwx_softc *,int cmd,int,size_t len,const void *p) {
  if(cmd==IWX_REMOVE_STA) {
   assert(len==sizeof(iwx_rm_sta_cmd));const unsigned sta=static_cast<const iwx_rm_sta_cmd *>(p)->sta_id;removes.push_back(sta);
   int e=command();if(failCleanup)e=EIO;if(!e && sta==txStation)queueOwned=false;return e;
  }
  auto bytes=static_cast<const uint8_t *>(p);tlcCommands.push_back({bytes,bytes+len});return command();
 }
 int iwx_tvqm_enable_txq(iwx_softc *,int tid,int,unsigned size,uint8_t station,IwxTxQueueSetup *) {
  assert(tid==0 && size==64 && station>=4 && station<8);const auto epoch=dmaEpoch;int e=command();if(e)return -e;if(epoch!=dmaEpoch)return -ENXIO;
  com.txq[8].ring_count=64;com.txq[8].cur=254;com.txq[8].allocated=true;txStation=station;queueOwned=true;return 8;
 }
 int iwx_flush_sta_tids(iwx_softc *,int station,uint16_t tids) {
  assert(station==txStation && tids==0xffff);++flushes;int e=command();if(e)return e;
  if(flushClears){for(auto &d:com.txq[8].data)if(d.m){mbuf_freem(d.m);d.m=nullptr;}com.txq[8].queued=0;}return 0;
 }
 void completeAWDLTx(iwx_tx_data *,bool,bool) {++cancelled;}
 void iwx_reset_tx_ring(iwx_softc *,iwx_tx_ring *q) {assert(!queueOwned);++resets;for(auto &d:q->data)if(d.m){mbuf_freem(d.m);d.m=nullptr;}q->queued=0;}
 void iwx_free_tx_ring(iwx_softc *,iwx_tx_ring *q) {assert(!queueOwned && !q->queued);++frees;q->allocated=false;q->ring_count=0;}
 bool mimo=true;uint8_t txAnt=3,rxAnt=3;
 bool iwx_mimo_enabled(iwx_softc *) {return mimo;}
 uint8_t iwx_fw_valid_rx_ant(iwx_softc *) {return rxAnt;}
 uint8_t iwx_fw_valid_tx_ant(iwx_softc *) {return txAnt;}
 uint8_t iwx_lookup_cmd_ver(iwx_softc *,int,int) {return tlcVersion;}
 int addAWDLBlockAckPeer(unsigned,const uint8_t *);
 void noteAWDLTXBlockAck(const uint8_t *,size_t,bool=false);bool receiveAWDLTXBlockAck(const uint8_t *,size_t);
 void serviceAWDLTXBlockAck();int configureAWDLTXStation(bool,bool);uint8_t awdlTXAntenna();uint8_t awdlTXMcs(uint64_t,unsigned=0) const;uint8_t awdlTXWidth(uint64_t) const;
 int configureAWDLTXRates();int downgradeAWDLTXRates(uint8_t,uint16_t=0);int setAWDLTXWindow(uint64_t (*)(const void *,uint64_t),const void *);
 int closeAWDLListenerWindow();int configureAWDLPeerWidth();
 bool completeAWDLTXManagement(uint64_t,bool);
 int awdlMAC(uint32_t action,bool=false){assert(action==IWX_FW_CTXT_ACTION_MODIFY);++macChanges;filterPromisc=!awdlHasRXBA();return command();}
 int stopAWDLRadio(){int e=clearAWDLBlockAck();awdlRadio.ready=false;awdlRadio.stopPending=false;return e;}
 void publishAWDLReceive(){}
 bool awdlHasRXBA() const;bool awdlRXBAOwned(const iwx_rxba_data *) const;
 bool awdlBlockAckLive(const BlockAckRequest &,uint64_t) const;
 int removeAWDLBlockAckTID(unsigned,unsigned);int removeAWDLBlockAckPeer(unsigned);int clearAWDLBlockAck();void resetAWDLBlockAck();
 int prepareAWDLBlockAck(const BlockAckRequest &,uint64_t &);bool receiveAWDLBlockAckEnd(const BlockAckRequest &,bool=false);
 void requestAWDLBlockAckEnd(uint8_t,uint8_t);void completeAWDLBlockAckEnd(const BlockAckRequest &,uint64_t);void serviceAWDLBlockAck();
 void deliverAWDLReordered(mbuf_t,bool);bool receiveAWDLReordered(mbuf_t,iwx_rx_mpdu_desc *,mbuf_list *);
 void iwx_init_reorder_buffer(iwx_reorder_buffer *,uint16_t,uint16_t);void iwx_clear_reorder_buffer(iwx_softc *,iwx_rxba_data *);
 int iwx_is_sn_less(uint16_t,uint16_t,uint16_t);void iwx_release_frames(iwx_softc *,ieee80211_node *,iwx_rxba_data *,iwx_reorder_buffer *,uint16_t,mbuf_list *);
 int iwx_oldsn_workaround(iwx_softc *,ieee80211_node *,int,iwx_reorder_buffer *,uint32_t,uint32_t);
 int iwx_rx_reorder(iwx_softc *,mbuf_t,int,iwx_rx_mpdu_desc *,int,int,uint32_t,ieee80211_rxinfo *,mbuf_list *);
 static void iwx_reorder_timer_expired(void *);
 static void iwx_rx_ba_session_expired(void *){}
 int iwx_sta_rx_agg(iwx_softc *,ieee80211_node *,uint8_t,uint16_t,uint16_t,int,int);
 void iwx_rx_frame(iwx_softc *,mbuf_t m,int,uint32_t,int,uint32_t,uint32_t,ieee80211_rxinfo *,mbuf_list *){++apFrames;mbuf_freem(m);}
};
#define RX_REORDER_BUF_TIMEOUT_MQ_USEC (100000ULL)
#define IWX_MAX_RX_BA_SESSIONS 16
// PRODUCTION
static BlockAckRequest request(ItlIwx &d,unsigned peer=0,unsigned tid=0,unsigned ssn=100) {
 BlockAckRequest r={};r.peer[0]=2;r.peer[5]=peer+2;r.dialog=7;r.tid=tid;r.ssn=ssn;r.window=64;r.immediate=true;r.amsdu=true;
 auto &p=d.awdlPeers.peers[peer];p.occupied=true;p.lastSeenUS=nowUS;copyAddress(p.address,r.peer);return r;
}
static unsigned start(ItlIwx &d,const BlockAckRequest &r) {uint64_t cookie=0;assert(!d.prepareAWDLBlockAck(r,cookie));assert(cookie && d.awdlBlockAckLive(r,cookie));
 for(auto &p:d.awdlBA.peers)if(p.station && sameAddress(p.address,r.peer))return p.sessions[r.tid].baidPlusOne-1;assert(false);return 0;}
static std::unique_ptr<Packet> packet(ItlIwx &d,const BlockAckRequest &r,unsigned sn,unsigned dataSequence) {
 auto m=std::unique_ptr<Packet>(new Packet);m->bytes.resize(82);auto p=m->bytes.data();p[0]=0x88;copyAddress(p+4,d.awdlRadio.address);copyAddress(p+10,r.peer);copyAddress(p+16,BSSID);
 putLE16(p+22,sn<<4);p[24]=r.tid;memcpy(p+26,SNAP,8);putLE16(p+34,0x0403);putLE16(p+36,dataSequence);putBE16(p+40,0x86dd);p[42]=0x60;return m;
}
static void receive(ItlIwx &d,Packet *m,unsigned baid,unsigned sn,unsigned nssn,int sub=-1,bool last=true,bool old=false,unsigned gp2=0) {
 const auto &r=d.com.sc_rxba_data[baid];iwx_rx_mpdu_desc desc={};desc.status=IWX_RX_MPDU_RES_STATUS_SRC_STA_FOUND|(r.sta_id<<24);
 desc.reorder_data=(baid<<24)|(sn<<12)|nssn|(old?IWX_RX_MPDU_REORDER_BA_OLD_SN:0);desc.v1.gp2_on_air_rise=gp2;
 if(sub>=0){desc.mac_flags2=IWX_RX_MPDU_MFLG2_AMSDU;desc.amsdu_info=sub|(last?IWX_RX_MPDU_AMSDU_LAST_SUBFRAME:0);m->bytes[24]|=128;}
 mbuf_list ml;assert(d.receiveAWDLReordered(m,&desc,&ml));assert(ml_empty(&ml));
}
static void wire() {
 const uint8_t self[]={2,0,0,0,0,1};std::vector<uint8_t> b(33);b[0]=0xd0;copyAddress(b.data()+4,self);b[10]=2;b[15]=2;copyAddress(b.data()+16,BSSID);
 b[24]=3;b[26]=99;putLE16(b.data()+27,3|(7<<2)|(512<<6));putLE16(b.data()+29,88);putLE16(b.data()+31,4095<<4);
 BlockAckRequest r={};assert(blockAckRequest(b.data(),b.size(),self,r));assert(r.window==512 && r.tid==7 && r.amsdu && r.immediate && r.ssn==4095 && r.timeout==88);
 r.window=63;assert(blockAckWindow(r)==32);r.window=33;assert(blockAckWindow(r)==32);r.window=1;assert(blockAckWindow(r)==1);r.window=512;
 uint8_t answer[33];blockAckResponse(answer,self,r,44,true);assert(le16(answer+27)==0 && le16(answer+29)==(3|(7<<2)|(64<<6)) && !le16(answer+31));
 uint8_t end[30];blockAckTeardown(end,self,r,45);BlockAckRequest got={};assert(!blockAckEnd(end,sizeof(end),r.peer,got));
 putLE16(end+26,(7<<12)|0x800);assert(blockAckEnd(end,sizeof(end),r.peer,got) && got.tid==7 && got.teardown);
 for(unsigned n=0;n<30;++n)assert(!blockAckEnd(end,n,r.peer,got));
 end[26]=1;assert(!blockAckEnd(end,sizeof(end),r.peer,got));
}
static void ownership() {
 ItlIwx d;auto r=request(d);unsigned b=start(d,r);assert(b==0 && d.com.sc_rx_ba_sessions==1 && d.adds[0].sta_id==4 && !d.filterPromisc);
 assert(d.com.sc_rxba_data[b].reorder_buf.buf_size==64 && d.com.sc_rxba_data[b].reorder_buf.head_sn==100);
 int calls=d.calls;uint64_t cookie=0;assert(!d.prepareAWDLBlockAck(r,cookie) && d.calls==calls);
 auto old=cookie;++r.dialog;r.ssn=200;r.window=8;b=start(d,r);assert(b==1 && d.com.sc_rx_ba_sessions==1 && !d.awdlBlockAckLive(r,old));
 assert(d.com.sc_rxba_data[0].baid==IWX_RX_REORDER_DATA_INVALID_BAID && d.com.sc_rxba_data[0].reorder_buf.reorder_timer==nullptr);
 auto r2=request(d,1,0);start(d,r2);assert(d.com.sc_rx_ba_sessions==2 && d.awdlBA.peers[1].station);
 assert(!d.clearAWDLBlockAck() && d.com.sc_rx_ba_sessions==0 && !d.awdlHasRXBA());
}
static void failures() {
 for(int fail=1;fail<=3;++fail){ItlIwx d;auto r=request(d);d.failCall=fail;uint64_t cookie=77;assert(d.prepareAWDLBlockAck(r,cookie)!=0 && !cookie);assert(!d.com.sc_rx_ba_sessions && !d.awdlHasRXBA());}
 {ItlIwx d;auto r=request(d);d.missingBAID=true;uint64_t c;assert(d.prepareAWDLBlockAck(r,c)==EIO && !d.com.sc_rx_ba_sessions);}
 {ItlIwx d;auto r=request(d);auto &ap=d.com.sc_rxba_data[0];ap.baid=0;ap.sta_id=0;ap.tid=0;d.iwx_init_reorder_buffer(&ap.reorder_buf,500,16);d.com.sc_rx_ba_sessions=1;
 d.forcedBAID=0;uint64_t c;assert(d.prepareAWDLBlockAck(r,c)==EIO);assert(ap.baid==0 && ap.reorder_buf.head_sn==500 && d.com.sc_rx_ba_sessions==1);}
 {ItlIwx d;auto r=request(d);d.com.sc_rx_ba_sessions=16;uint64_t c;assert(d.prepareAWDLBlockAck(r,c)==ENOSPC && d.com.sc_rx_ba_sessions==16);}
 {ItlIwx d;auto r=request(d);d.failCall=3;d.failCleanup=true;uint64_t c;assert(d.prepareAWDLBlockAck(r,c)!=0 && d.awdlRadio.fault && !d.awdlRadio.ready);assert(d.com.sc_rx_ba_sessions==1);d.resetAWDLBlockAck();assert(!d.com.sc_rx_ba_sessions);}
}
static void interrupted() {
 for(int at=1;at<=3;++at) for(int kind=0;kind<4;++kind){ItlIwx d;auto r=request(d);d.onCommand=[&](int n){if(n!=at)return;
 if(kind==0){d.resetAWDLBlockAck();++d.dmaEpoch;d.awdlRadio.changing=false;d.awdlRadio.ready=false;}
 if(kind==1)d.awdlRadio.stopPending=true;
 if(kind==2){auto e=r;e.teardown=true;d.receiveAWDLBlockAckEnd(e);}
 if(kind==3)++d.bss.in_color;
 };uint64_t c=99;assert(d.prepareAWDLBlockAck(r,c)!=0 && !c && !d.awdlRadio.changing);assert(!d.com.sc_rx_ba_sessions && !d.awdlHasRXBA());}
}
static void reordered() {
 ItlIwx d;auto r=request(d);unsigned b=start(d,r);
 auto p2=packet(d,r,102,2),p0=packet(d,r,100,0),p1=packet(d,r,101,1),dup=packet(d,r,102,2);
 receive(d,p2.get(),b,102,100);assert(!p2->frees && d.delivered.empty());receive(d,dup.get(),b,102,100);assert(dup->frees);
 receive(d,p0.get(),b,100,101);assert((d.delivered==std::vector<uint16_t>{0}));receive(d,p1.get(),b,101,103);assert((d.delivered==std::vector<uint16_t>{0,1,2}));
 auto gap=packet(d,r,105,5);receive(d,gap.get(),b,105,103);nowUS+=100001;d.loop.gated=false;ItlIwx::iwx_reorder_timer_expired(&d.com.sc_rxba_data[b].reorder_buf);
 assert(d.delivered.back()==5 && gap->frees && d.gate.entries==1);d.loop.gated=true;
 ++r.dialog;r.ssn=4095;b=start(d,r);auto wrap=packet(d,r,0,7),last=packet(d,r,4095,6);receive(d,wrap.get(),b,0,4095);receive(d,last.get(),b,4095,1);
 assert(d.delivered[d.delivered.size()-2]==6 && d.delivered.back()==7 && !apFinds && !d.apFrames);
}
static void amsdu() {
 ItlIwx d;auto r=request(d);unsigned b=start(d,r);auto future=packet(d,r,101,3),a=packet(d,r,100,1),bframe=packet(d,r,100,2),duplicate=packet(d,r,100,1);
 receive(d,future.get(),b,101,100);receive(d,a.get(),b,100,102,0,false);assert(d.delivered.empty());receive(d,duplicate.get(),b,100,102,0,false);assert(duplicate->frees);
 receive(d,bframe.get(),b,100,102,1,true);assert((d.delivered==std::vector<uint16_t>{1,2,3}));assert(a->frees && bframe->frees && future->frees);
 auto wrong=packet(d,r,103,4);wrong->bytes[15]^=8;receive(d,wrong.get(),b,103,104);assert(wrong->frees && d.awdlBA.rejected==1);
 auto pending=packet(d,r,105,5);receive(d,pending.get(),b,105,102);assert(!pending->frees);d.resetAWDLBlockAck();assert(pending->frees && !d.com.sc_rx_ba_sessions);
}
static void teardown() {
 ItlIwx d;auto r=request(d);unsigned b=start(d,r);auto p=packet(d,r,102,2);receive(d,p.get(),b,102,100);
 auto end=r;end.teardown=true;d.receiveAWDLBlockAckEnd(end);d.serviceAWDLBlockAck();assert(p->frees && !d.awdlBA.peers[0].station && !d.com.sc_rx_ba_sessions);
 b=start(d,r);d.requestAWDLBlockAckEnd(4,0);d.serviceAWDLBlockAck();assert(d.ends.size()==1 && d.ends[0].teardown);uint64_t cookie=0;
 assert(!d.prepareAWDLBlockAck(d.ends[0],cookie));d.completeAWDLBlockAckEnd(d.ends[0],cookie);d.serviceAWDLBlockAck();assert(!d.com.sc_rx_ba_sessions);
 b=start(d,r);d.requestAWDLBlockAckEnd(4,0);nowUS+=3000001;d.serviceAWDLBlockAck();assert(!d.com.sc_rx_ba_sessions && !apDelbas);
}
static void oldSequence() {
 ItlIwx d;auto r=request(d);unsigned b=start(d,r);d.com.sc_rxba_data[b].reorder_buf.valid=1;
 std::vector<std::unique_ptr<Packet>> packets;
 for(unsigned i=1;i<=20;++i){packets.push_back(packet(d,r,99,i));receive(d,packets.back().get(),b,99,100,-1,true,true,i);}
 assert(d.awdlBA.peers[0].sessions[0].endDeadlineUS && !apDelbas);d.serviceAWDLBlockAck();assert(d.ends.size()==1);
 const auto before=d.delivered.size();auto stale=packet(d,r,100,21);receive(d,stale.get(),b,100,101);assert(stale->frees && d.delivered.size()==before);
}
static void apCoexistence() {
 {ItlIwx d;auto r=request(d);start(d,r);auto m=packet(d,r,10,1);m->bytes[1]=2;
  iwx_rx_mpdu_desc desc={};desc.reorder_data=0;mbuf_list ml;
  assert(!d.receiveAWDLReordered(m.get(),&desc,&ml) && !m->frees);
 }

 {ItlIwx d;auto r=request(d);auto baid=start(d,r);const auto cookie=d.awdlBA.peers[0].sessions[0].cookie;
  assert(!d.iwx_sta_rx_agg(&d.com,&d.bss,0,200,64,0,1));assert(d.com.sc_rx_ba_sessions==2 && apAccepted==1);
  assert(!d.iwx_sta_rx_agg(&d.com,&d.bss,0,0,0,0,0));assert(d.com.sc_rx_ba_sessions==1 && d.awdlBlockAckLive(r,cookie));
  const auto calls=d.calls;assert(!d.iwx_sta_rx_agg(&d.com,&d.bss,0,0,0,0,0));assert(d.calls==calls && d.com.sc_rx_ba_sessions==1);
  assert(!d.clearAWDLBlockAck());d.forcedBAID=baid;
  assert(!d.iwx_sta_rx_agg(&d.com,&d.bss,0,200,64,0,1));assert(d.com.sc_rxba_data[baid].reorder_buf.reorder_timer && d.com.sc_rxba_data[baid].session_timer);
 }
 {ItlIwx d;auto r=request(d);d.com.sc_rx_ba_sessions=15;int nested=0;
  d.onCommand=[&](int n){if(n!=1)return;uint64_t c=0;nested=d.prepareAWDLBlockAck(r,c);};
  assert(!d.iwx_sta_rx_agg(&d.com,&d.bss,0,200,64,0,1));assert(nested==ENOSPC && d.com.sc_rx_ba_sessions==16);
 }
 {ItlIwx d;auto r=request(d);d.com.sc_rx_ba_sessions=15;int nested=-1;
  d.onCommand=[&](int n){if(n==2)nested=d.iwx_sta_rx_agg(&d.com,&d.bss,0,200,64,0,1);};uint64_t c=0;
  assert(d.prepareAWDLBlockAck(r,c)==ENOSPC && !nested && d.com.sc_rx_ba_sessions==16);
 }
 {ItlIwx d;d.com.sc_rx_ba_sessions=15;d.failCall=1;
  assert(d.iwx_sta_rx_agg(&d.com,&d.bss,0,200,64,0,1)==EIO && d.com.sc_rx_ba_sessions==15);
 }
}

int main(){
 if(CASE==9)apCoexistence();if(CASE==1)wire();if(CASE==2)ownership();if(CASE==3)failures();if(CASE==4)interrupted();
 if(CASE==5)reordered();if(CASE==6)amsdu();if(CASE==7)teardown();if(CASE==8)oldSequence();
}
