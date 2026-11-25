#include <cassert>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <functional>
#include <string>
#include <vector>
#include <libkern/OSByteOrder.h>
#include "itlwm/hal_iwx/Regulatory.hpp"
#define htole16 OSSwapHostToLittleInt16
#define htole32 OSSwapHostToLittleInt32
#define le16toh OSSwapLittleToHostInt16
#define le32_to_cpup(p) OSSwapLittleToHostInt32(*(p))
#define le16_to_cpup(p) OSSwapLittleToHostInt16(*(p))
#define nitems(a) (sizeof(a)/sizeof((a)[0]))
#define DPRINTFN(...) ((void)0)
#define IEEE80211_CHAN_MAX 255
#define IEEE80211_CHAN_B (IEEE80211_CHAN_2GHZ|IEEE80211_CHAN_CCK)
#define IEEE80211_CHAN_PUREG (IEEE80211_CHAN_2GHZ|IEEE80211_CHAN_OFDM)
#define IEEE80211_CHAN_HT (IEEE80211_CHAN_HT20|IEEE80211_CHAN_HT40U|IEEE80211_CHAN_HT40D)
#define IEEE80211_CHAN_VHT (IEEE80211_CHAN_VHT80|IEEE80211_CHAN_VHT160)
// FIRMWARE DECLARATIONS
enum {IEEE80211_CHAN_2GHZ=1,IEEE80211_CHAN_5GHZ=2,IEEE80211_CHAN_CCK=4,
    IEEE80211_CHAN_OFDM=8,IEEE80211_CHAN_DYN=16,IEEE80211_CHAN_A=2|8,
    IEEE80211_CHAN_PASSIVE=32,IEEE80211_CHAN_HT20=64,IEEE80211_CHAN_HT40U=128,
    IEEE80211_CHAN_HT40D=256,IEEE80211_CHAN_VHT80=512,IEEE80211_CHAN_VHT160=1024,
    IEEE80211_CHAN_DFS=2048};
struct ieee80211_channel {unsigned ic_flags=0,ic_freq=0,ic_center_freq1=0,ic_center_freq2=0;};
struct ieee80211com {ieee80211_channel ic_channels[256];uint8_t ic_chan_active[32]={},ic_chan_avail[32]={},ic_beacon_hint[32]={};
    uint16_t ic_beacon_hint_mcc=0;
    unsigned ic_curmode=0,ic_state=0;void (*ic_event_handler)(ieee80211com *,int,void *)=nullptr;};
struct iwx_nvm_data {bool sku_cap_band_52GHz_enable=true,sku_cap_11n_enable=true,
    sku_cap_11ac_enable=true,vht160_supported=false,lar_enabled=true;};
struct task {bool queued=false;};
struct iwx_softc {ieee80211com sc_ic;iwx_nvm_data sc_nvm;bool sc_uhb_supported=false;
    unsigned sc_flags=0;int sc_generation=0;task mcc_task;
    uint8_t sc_enabled_capa[16]={},sc_ucode_api[16]={};char sc_fw_mcc[3]={};uint16_t sc_fw_mcc_int=0;};
static unsigned ieee80211_ieee2mhz(unsigned channel,unsigned flags) {
    return flags&IEEE80211_CHAN_2GHZ ? 2407+channel*5 : 5000+channel*5;
}
#ifdef MCC_TEST
// MCC MOCKS
#else
struct ItlIwx {void iwx_init_channel_map(iwx_softc *,uint16_t *,uint32_t *,int);};
#endif
// PRODUCTION
static void rawMapping() {
    ItlIwx driver;iwx_softc sc;uint32_t profile[nitems(iwx_nvm_channels_8000)]={};
    unsigned at=0;while(iwx_nvm_channels_8000[at]!=48)++at;
    sc.sc_ic.ic_channels[48].ic_center_freq1=5250;
    sc.sc_ic.ic_channels[48].ic_center_freq2=5570;
    // Intel NVM: bit7 is UNIFORM; bit4 is RADAR. Independent raw wire values.
    profile[at]=OSSwapHostToLittleInt32(1u|8u|128u|256u|1024u);
    driver.iwx_init_channel_map(&sc,nullptr,profile,nitems(profile));
    assert(!(sc.sc_ic.ic_channels[48].ic_flags&IEEE80211_CHAN_DFS));
    assert(!(sc.sc_ic.ic_channels[48].ic_flags&IEEE80211_CHAN_PASSIVE));
    profile[at]=OSSwapHostToLittleInt32(1u|8u|16u|256u);
    driver.iwx_init_channel_map(&sc,nullptr,profile,nitems(profile));
    assert(sc.sc_ic.ic_channels[48].ic_flags&IEEE80211_CHAN_DFS);
    profile[at]=OSSwapHostToLittleInt32(1u|256u);
    driver.iwx_init_channel_map(&sc,nullptr,profile,nitems(profile));
    assert(sc.sc_ic.ic_channels[48].ic_flags&IEEE80211_CHAN_PASSIVE);
    assert(sc.sc_ic.ic_channels[48].ic_center_freq1==5250);
    assert(sc.sc_ic.ic_channels[48].ic_center_freq2==5570);
}
#ifdef MCC_TEST
// MCC PRODUCTION
// MCC TESTS
#else
int main() {rawMapping();}
#endif
