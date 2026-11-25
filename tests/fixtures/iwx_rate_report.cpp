#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <initializer_list>
using u8=uint8_t; using u16=uint16_t; using u32=uint32_t;
using u_int=unsigned int;
using __le16=uint16_t; using __le32=uint32_t;
using IOReturn=int; struct OSObject {};
#define BIT(n) (1U<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define nitems(a) ARRAY_SIZE(a)
#define le32toh(a) (a)
#define WARN_ON(a) ((void)0)
#define XYLog(...) ((void)0)
enum { kIOReturnSuccess=0, APPLE80211_VERSION=1, IEEE80211_NODE_HE=1,
 IEEE80211_MODE_11AC=1, IEEE80211_MODE_11N=2, IEEE80211_MODE_11AX=3,
 IEEE80211_CHAN_WIDTH_20=0, IEEE80211_CHAN_WIDTH_40=1, IEEE80211_CHAN_WIDTH_80=2,
 IEEE80211_CHAN_WIDTH_160=3, IEEE80211_CHAN_WIDTH_80P80=4,
 IEEE80211_HT_RATESET_MIMO4_SGI=7, IEEE80211_RATE_VAL=127 };
enum ieee80211_state { IEEE80211_S_INIT=0, IEEE80211_S_SCAN=1, IEEE80211_S_AUTH=2,
 IEEE80211_S_ASSOC=3, IEEE80211_S_RUN=4 };
#define IFF_DEBUG 0
// WIRE
struct ieee80211_ht_rateset { int nrates,rates[8]; uint32_t mask; int min,max,sgi; };
struct ieee80211_vht_rateset { int nrates,rates[10],nss,sgi; };
// RATE TABLES
struct ieee80211_rateset { int rs_nrates; uint8_t rs_rates[16]; };
struct ieee80211_node {
 uint32_t ni_flags=0, ni_he_txrate_kbps=0, ni_txrate_kbps=0;
 uint8_t ni_he_tx_nss=0, ni_chw=IEEE80211_CHAN_WIDTH_160;
 bool ni_txrate_reported=false;
 int ni_txmcs=0,ni_txrate=0;
 ieee80211_rateset ni_rates{12,{2,4,11,22,12,18,24,36,48,72,96,108}};
};
struct _ifnet { int if_flags=0; };
struct ieee80211com { ieee80211_node *ic_bss; ieee80211_state ic_state=IEEE80211_S_RUN;
 int ic_curmode=IEEE80211_MODE_11AC; _ifnet ic_if; };
struct iwx_softc { ieee80211com sc_ic; uint8_t notificationVersion=3; };
static int ieee80211_node_supports_sgi(ieee80211_node *) { return 1; }
static int iwx_rs_pretty_print_rate_v1(char*,int,uint32_t){return 0;}
static int iwx_rs_pretty_print_rate(char*,int,uint32_t){return 0;}
static uint8_t iwx_lookup_notif_ver(iwx_softc *sc,int,int){return sc->notificationVersion;}
struct ItlIwx { void iwx_rs_update(iwx_softc*,iwx_tlc_update_notif*); };
struct HAL {
 ieee80211com *ic;
 ieee80211com *get80211Controller(){return ic;}
 HAL *getDriverInfo(){return this;}
 int getTxNSS(){return 2;}
};
struct apple80211_rate_data { uint32_t version,num_radios,rate[4]; };
struct AirportItlwmSkywalkInterface { HAL *fHalService; IOReturn getRATE(apple80211_rate_data*); };
struct AirportItlwm { HAL *fHalService; IOReturn getRATE(OSObject*,apple80211_rate_data*); };
// PRODUCTION
int main() {
 ieee80211_node node; iwx_softc sc{{&node}}; ItlIwx driver; HAL hal{&sc.sc_ic};
 AirportItlwmSkywalkInterface native{&hal}; AirportItlwm legacy{&hal};
 auto report=[&](uint32_t rate, uint32_t flags=IWX_TLC_NOTIF_FLAG_RATE, uint8_t sta=IWX_STATION_ID) {
  iwx_tlc_update_notif n={}; n.sta_id=sta; n.flags=flags; n.rate=rate; driver.iwx_rs_update(&sc,&n);
 };
 auto check=[&](uint32_t expected) {
  apple80211_rate_data a={},b={};
  assert(native.getRATE(&a)==0 && legacy.getRATE(nullptr,&b)==0);
  if(a.rate[0]!=expected || b.rate[0]!=expected) fprintf(stderr,"rate got=%u/%u expected=%u\n",a.rate[0],b.rate[0],expected);
  assert(a.rate[0]==expected && b.rate[0]==expected);
 };
 // The AP negotiated 160 MHz/2SS, but firmware really sends 20 MHz/1SS MCS8.
 report(IWX_RATE_MCS_VHT_MSK|8); check(78);
 report(IWX_RATE_MCS_VHT_MSK|IWX_RATE_MCS_CHAN_WIDTH_160|IWX_RATE_MCS_NSS_MSK|IWX_RATE_MCS_SGI_MSK|8); check(1560);
 report(IWX_RATE_MCS_VHT_MSK|IWX_RATE_MCS_CHAN_WIDTH_80|IWX_RATE_MCS_SGI_MSK|9); check(433);
 report(IWX_RATE_MCS_HT_MSK|IWX_RATE_MCS_CHAN_WIDTH_40|IWX_RATE_MCS_NSS_MSK|IWX_RATE_MCS_SGI_MSK|7); check(300);
 report(IWX_RATE_MCS_HT_MSK|7); check(65);
 report(IWX_RATE_MCS_LEGACY_OFDM_MSK|2); check(12);
 report(IWX_RATE_MCS_CCK_MSK|2); check(5);
 report(IWX_RATE_MCS_LEGACY_OFDM_MSK|7); check(54);
 report(IWX_RATE_MCS_HT_MSK|7,0); check(54);
 report(IWX_RATE_MCS_HT_MSK|7,IWX_TLC_NOTIF_FLAG_RATE,3); check(54);
 // Invalid width/MCS and unsupported EHT replace the old report with unknown.
 report(IWX_RATE_MCS_VHT_MSK|IWX_RATE_MCS_CHAN_WIDTH_320|8); check(0);
 report(IWX_RATE_MCS_VHT_MSK|9); check(0);
 report(IWX_RATE_MCS_HT_MSK|15); check(0);
 report(IWX_RATE_MCS_EHT_MSK|8); check(0);
 report(IWX_RATE_MCS_HE_MSK|IWX_RATE_MCS_CHAN_WIDTH_80|11); check(600);
 report(IWX_RATE_MCS_HE_MSK|(2U<<IWX_RATE_MCS_HE_TYPE_POS)|11); check(0);
 // Old firmware encodes legacy PLCP, not the local interleaved table index.
 sc.notificationVersion=2;
 const uint8_t plcp[]={13,15,5,7,9,11,1,3};
 const uint32_t mbps[]={6,9,12,18,24,36,48,54};
 for(unsigned i=0;i<8;++i){report(plcp[i]);check(mbps[i]);assert(node.ni_rates.rs_rates[node.ni_txrate]==mbps[i]*2);}
 report(IWX_RATE_MCS_CCK_MSK_V1|55); check(5);
 report(0xff); check(0);
 report(IWX_RATE_MCS_VHT_MSK_V1|IWX_RATE_MCS_CHAN_WIDTH_80|IWX_RATE_MCS_SGI_MSK_V1|9); check(433);
 report(IWX_RATE_MCS_HT_MSK_V1|IWX_RATE_MCS_CHAN_WIDTH_40|IWX_RATE_MCS_SGI_MSK_V1|15); check(300);
 // Scan, reassociation and reinitialization clear the previous AP's sample.
 for(auto state:{IEEE80211_S_INIT,IEEE80211_S_SCAN,IEEE80211_S_AUTH,IEEE80211_S_ASSOC}) {
  ieee80211_newstate(&sc.sc_ic,state,-1);
  assert(!node.ni_txrate_reported && !node.ni_txrate_kbps);
  report(IWX_RATE_MCS_HT_MSK_V1|7);
  assert(!node.ni_txrate_reported && !node.ni_txrate_kbps);
  ieee80211_newstate(&sc.sc_ic,IEEE80211_S_RUN,-1);
  assert(!node.ni_txrate_reported);
  report(IWX_RATE_MCS_HT_MSK_V1|7); check(65);
 }
 // Other HALs retain their existing path until a hardware report is available.
 node.ni_txrate_reported=false; node.ni_txmcs=8; check(1560);
 puts("TLC report -> both native RATE getters: passed");
}
