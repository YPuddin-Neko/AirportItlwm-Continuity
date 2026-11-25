// Kernel allocation and DMA boundary for the radio/transport integration tests.
struct Channel {unsigned ic_flags=0,ic_freq=0;};
struct Phy {int id=0,color=0;Channel *channel=nullptr;};
struct ieee80211_frame_min {uint8_t bytes[10];};
struct ieee80211_tx_ba {int ba_state=0;};
struct ieee80211_node {ieee80211_tx_ba ni_tx_ba[16];unsigned releases=0;};
struct iwx_node {ieee80211_node in_ni;Phy *in_phyctxt=nullptr;int in_id=0,in_color=0;};
struct ieee80211_key {int k_cipher;uint64_t k_tsc;};
enum {IEEE80211_FC0_TYPE_MASK=12,IEEE80211_FC0_SUBTYPE_MASK=240,
    IEEE80211_FC0_TYPE_MGT=0,IEEE80211_FC0_TYPE_CTL=4,IEEE80211_FC0_TYPE_DATA=8,
    IEEE80211_FC0_SUBTYPE_ACTION=0xd0,IEEE80211_FC0_SUBTYPE_NODATA=64,IEEE80211_FC1_RETRY=8,IEEE80211_FC1_PROTECTED=64,
    IEEE80211_QOS_TID=15,IEEE80211_QOS_AMSDU=128,IEEE80211_BA_AGREED=1,IEEE80211_CIPHER_CCMP=3,EDCA_AC_BE=0};
#define IEEE80211_IS_MULTICAST(p) ((p)[0]&1)
static bool ieee80211_has_qos(const ieee80211_frame *p) {return (p->i_fc[0]&0x8c)==0x88;}
static uint16_t ieee80211_get_qos(const ieee80211_frame *p) {assert(ieee80211_has_qos(p));const auto q=reinterpret_cast<const uint8_t *>(p)+24;return q[0]|(uint16_t(q[1])<<8);}
static unsigned ieee80211_get_hdrlen(const ieee80211_frame *p) {return ieee80211_has_qos(p)?26:24;}
static int ieee80211_wcl_ac(unsigned) {return EDCA_AC_BE;}
struct Statistics {unsigned outputErrors=0;};
struct _ifnet {Statistics stats;Statistics *netStat=&stats;};
struct ieee80211com {
    int ic_opmode=1,ic_state=4;ieee80211_node *ic_bss=nullptr;
    Channel ic_channels[256];uint8_t ic_chan_active[32]={},ic_beacon_hint[32]={};uint16_t ic_beacon_hint_mcc=0;_ifnet ic_if;
    uint64_t ic_wcl_tx_packets[4]={};
};
static void ieee80211_release_node(ieee80211com *,ieee80211_node *n) {++n->releases;}
static ieee80211_key *ieee80211_get_txkey(ieee80211com *,ieee80211_frame *,ieee80211_node *) {
    assert(false);return nullptr;
}
static mbuf_t ieee80211_encrypt(ieee80211com *,mbuf_t,ieee80211_key *) {assert(false);return nullptr;}
struct Memory {void *vaddr=nullptr;size_t size=0;};
struct iwx_tx_ring {
    unsigned ring_count=0;int tail=0,cur=0,queued=0,qid=IWX_INVALID_QUEUE,hi_mark=56;
    std::vector<iwx_tx_data> data;
    std::vector<DMAMap> maps;
    std::vector<iwx_tfh_tfd> descriptors;
    std::vector<iwx_device_cmd> commands;
    std::vector<uint16_t> counts;
    iwx_tfh_tfd *desc=nullptr;iwx_device_cmd *cmd=nullptr;Memory desc_dma,bc_tbl;
    void allocate(unsigned id) {
        assert(!queued);ring_count=64;qid=id;tail=cur=0;
        data.assign(64,{});maps.assign(64,{});descriptors.assign(64,{});commands.assign(64,{});
        counts.assign(sizeof(iwx_agn_scd_bc_tbl)/2,0);
        desc=descriptors.data();cmd=commands.data();
        desc_dma={desc,descriptors.size()*sizeof(*desc)};bc_tbl={counts.data(),counts.size()*2};
        for(unsigned i=0;i<64;++i) {data[i].map=&maps[i];data[i].cmd_paddr=reinterpret_cast<uint64_t>(&cmd[i]);}
    }
};
struct iwl_cfg { uint8_t non_shared_ant; };
struct iwx_softc {
    const iwl_cfg *sc_cfg=nullptr;
    ieee80211com sc_ic={};unsigned sc_flags=IWX_FLAG_BINDING_ACTIVE;int ns_nstate=IEEE80211_S_RUN;
    int sc_device_family=IWX_DEVICE_FAMILY_22000;int first_data_qid=1;
    iwx_tx_ring txq[16];Phy sc_phyctxt[3];uint8_t sc_enabled_capa[8]={};
    struct {int qid=IWX_INVALID_QUEUE;} sc_tid_data[16];
    unsigned sc_tx_timer=0;uint8_t sc_mgmt_last_antenna_idx=0;
    void *sc_cmd_resp_pkt[64]={};size_t sc_cmd_resp_len[64]={};ItlQueueMask<16> qfullmsk={};
    std::vector<unsigned> doorbells;
};
struct iwx_rx_packet {size_t length=0;alignas(8) uint8_t data[128]={};};
struct iwx_rx_data {void *map=nullptr;};
static size_t iwx_rx_packet_payload_len(const iwx_rx_packet *p) {return p->length;}
struct ieee80211_rateset {uint8_t rs_nrates,rs_rates[16];};
