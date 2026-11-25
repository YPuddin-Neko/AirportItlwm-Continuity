// MCC DECLARATIONS
#define PAGE_SIZE 4096
using IOReturn=int;
const int kIOReturnSuccess=0;
enum {IEEE80211_S_RUN=4,IEEE80211_EVT_COUNTRY_CODE_UPDATE=7};
struct OSObject {};
struct Loop {bool inGate(){return true;}};
struct Gate {
    using Action=IOReturn(*)(OSObject *,void *,void *,void *,void *);
    int runAction(Action f,void *a=nullptr,void *b=nullptr,void *c=nullptr,void *d=nullptr){return f(nullptr,a,b,c,d);}
};
#define container_of(p,t,m) reinterpret_cast<t *>(p)
static void *systq=nullptr;
static bool isset(const uint8_t *p,unsigned b){return p[b/8]&(1U<<(b%8));}
static void setbit(uint8_t *p,unsigned b){p[b/8]|=1U<<(b%8);}
struct iwx_rx_packet {struct {uint8_t group_id=0;}hdr;uint8_t data[1024]={};size_t length=0;};
struct iwx_host_cmd {uint32_t id=0,flags=0;size_t len[1]={};const void *data[1]={};size_t resp_pkt_len=0;iwx_rx_packet *resp_pkt=nullptr;};
static size_t iwx_rx_packet_payload_len(const iwx_rx_packet *p){return p->length;}
struct ItlIwx {
    iwx_softc com;IwxRegulatoryState regulatory;Loop loop;Gate gate;
    iwx_rx_packet reply;unsigned version=5,sends=0,frees=0,events=0,callbacks=0,publishes=0;
    int error=0;std::function<void()> onSend;
    std::vector<iwx_mcc_update_cmd> commands;
    void *awdlReceiver=this;
    void (*awdlRegulatoryChanged)(void *)=[](void *p){auto d=static_cast<ItlIwx *>(p);assert(d->regulatory.valid);assert(d->regulatory.refreshes);++d->callbacks;};
    ItlIwx(){
        setbit(com.sc_enabled_capa,IWX_UCODE_TLV_CAPA_MCC_UPDATE_11AX_SUPPORT);
        setbit(com.sc_ucode_api,IWX_UCODE_TLV_API_WIFI_MCC_UPDATE);
        com.sc_ic.ic_event_handler=[](ieee80211com *ic,int event,void *){
            auto d=reinterpret_cast<ItlIwx *>(ic);assert(event==IEEE80211_EVT_COUNTRY_CODE_UPDATE);
            assert(d->regulatory.valid);assert(d->callbacks==d->events+1);
            assert(uint8_t(d->com.sc_fw_mcc[0])==d->regulatory.mappedMCC>>8);++d->events;
        };
    }
    Loop *getMainWorkLoop(){return &loop;}Gate *getMainCommandGate(){return &gate;}
    void iwx_add_task(iwx_softc *,void *,task *t){t->queued=true;}
    void iwx_publish_regulatory(iwx_softc *){++publishes;}
    unsigned iwx_lookup_notif_ver(iwx_softc *,unsigned g,unsigned c){assert(g==1&&c==0xc8);return version;}
    int iwx_send_cmd(iwx_softc *,iwx_host_cmd *cmd){
        assert(cmd->id==IWX_MCC_UPDATE_CMD&&cmd->flags==IWX_CMD_WANT_RESP);
        assert(cmd->len[0]==sizeof(iwx_mcc_update_cmd));
        ++sends;commands.push_back(*static_cast<const iwx_mcc_update_cmd *>(cmd->data[0]));
        cmd->resp_pkt=&reply;if(onSend)onSend();return error;
    }
    void iwx_free_resp(iwx_softc *,iwx_host_cmd *c){++frees;c->resp_pkt=nullptr;}
    void iwx_init_channel_map(iwx_softc *,uint16_t *,uint32_t *,int);
    int iwx_send_update_mcc_cmd(iwx_softc *,const char *,uint8_t=0xff);
    void iwx_mcc_update(iwx_softc *,iwx_mcc_chub_notif *);
    static void iwx_mcc_task(void *);
    void worker(){assert(com.mcc_task.queued);com.mcc_task.queued=false;iwx_mcc_task(&com);}
    void notify(uint16_t mcc,uint8_t src){iwx_mcc_chub_notif n={htole16(mcc),src,0};iwx_mcc_update(&com,&n);}
};
// MCC TESTS
static void put16(uint8_t *p,uint16_t x){p[0]=x;p[1]=x>>8;}
static void put32(uint8_t *p,uint32_t x){put16(p,x);put16(p+2,x>>16);}
static void response(ItlIwx &d,unsigned count=51,uint16_t country=0x434e,uint32_t status=0) {
    d.reply={};const unsigned header=d.version>=8&&d.version!=IWX_FW_CMD_VER_UNKNOWN?24:
        isset(d.com.sc_enabled_capa,IWX_UCODE_TLV_CAPA_MCC_UPDATE_11AX_SUPPORT)?20:16;
    d.reply.length=header+4*count;put32(d.reply.data,status);put16(d.reply.data+4,country);
    if(header==24){put32(d.reply.data+8,0x118);d.reply.data[16]=2;}
    else if(header==20){put16(d.reply.data+6,0x0c);d.reply.data[12]=2;}
    else {d.reply.data[6]=0x0c;d.reply.data[7]=2;}
    put32(d.reply.data+header-4,count);
    for(unsigned i=0;i<count;++i)put32(d.reply.data+header+i*4,1u|8u|128u|256u|512u|1024u|2048u);
}
static void bootAndNotifications(){
    ItlIwx d;response(d);d.com.sc_ic.ic_channels[48].ic_center_freq1=5250;
    assert(!d.iwx_send_update_mcc_cmd(&d.com,"ZZ"));
    assert(d.commands.back().source_id==0x10&&le16toh(d.commands.back().mcc)==0x5a5a);
    assert(d.regulatory.mappedMCC==0x434e&&d.events==1&&d.callbacks==1);
    assert(isset(d.com.sc_ic.ic_chan_active,48));assert(!(d.com.sc_ic.ic_channels[48].ic_flags&IEEE80211_CHAN_DFS));
    assert(d.com.sc_ic.ic_channels[48].ic_center_freq1==5250);
    const auto sends=d.sends;d.notify(0x5553,1);d.notify(0x434e,2);assert(d.sends==sends);
    d.worker();assert(d.sends==sends+1&&d.commands.back().source_id==2);
    assert(le16toh(d.commands.back().mcc)==0x434e&&d.events==2); // Same country still publishes new profile.
    d.com.sc_ic.ic_state=IEEE80211_S_RUN;d.notify(0x5553,5);assert(!d.com.mcc_task.queued);
    d.com.sc_flags=IWX_FLAG_SHUTDOWN;d.notify(0x5553,2);assert(!d.com.mcc_task.queued);
}
static void failedRepliesAndReset(){
    for(unsigned fault=0;fault<7;++fault){
        ItlIwx d;response(d);assert(!d.iwx_send_update_mcc_cmd(&d.com,"ZZ"));
        auto old=d.com.sc_ic;const auto count=d.regulatory.refreshes;response(d,51,0x5553);
        if(fault==0)d.error=EWOULDBLOCK;
        if(fault==1)d.reply.hdr.group_id=IWX_CMD_FAILED_MSK;
        if(fault==2)--d.reply.length;
        if(fault==3)++d.reply.length;
        if(fault==4)put32(d.reply.data+16,0xffffffff);
        if(fault==5)put32(d.reply.data,2);
        if(fault==6){d.onSend=[&]{d.regulatory.reset();++d.com.sc_generation;};}
        assert(d.iwx_send_update_mcc_cmd(&d.com,"US",2)!=0);
        assert(!memcmp(&old,&d.com.sc_ic,sizeof(old)));
        assert(d.events==1&&d.regulatory.refreshes==count);
        if(fault==6)assert(!d.regulatory.valid);
    }
}
static void newerNotificationAndTail(){
    ItlIwx d;response(d);d.notify(0x5553,1);
    d.onSend=[&]{d.notify(0x434e,2);};d.worker();
    assert(!d.events&&d.com.mcc_task.queued&&d.regulatory.pending&&!d.regulatory.running);
    d.onSend={};response(d);d.worker();assert(d.events==1&&d.regulatory.mappedMCC==0x434e);
    assert(le16toh(d.commands.back().mcc)==0x434e);
    response(d,14);assert(!d.iwx_send_update_mcc_cmd(&d.com,"CN",2));
    assert(!d.com.sc_ic.ic_channels[48].ic_flags&&!isset(d.com.sc_ic.ic_chan_active,48));
    assert(isset(d.com.sc_ic.ic_chan_active,6));
}
static void layoutsAndCapabilities(){
    for(unsigned variant=0;variant<4;++variant){
        ItlIwx d;d.version=variant==0?IWX_FW_CMD_VER_UNKNOWN:variant==1?5:variant==2?6:8;
        if(variant==0)memset(d.com.sc_enabled_capa,0,sizeof(d.com.sc_enabled_capa));
        if(variant==3)d.com.sc_uhb_supported=true;
        response(d,variant==3?110:51,0,variant==0?4:5);
        if(variant==2)put16(d.reply.data+6,0x118);
        assert(!d.iwx_send_update_mcc_cmd(&d.com,"ZZ"));
        assert(d.regulatory.mappedMCC==0x3030&&d.events==1);
        assert(d.com.sc_ic.ic_channels[48].ic_flags&IEEE80211_CHAN_VHT160);
    }
    ItlIwx d;response(d);put16(d.reply.data+6,1u<<7);
    assert(!d.iwx_send_update_mcc_cmd(&d.com,"ZZ"));
    assert(!(d.com.sc_ic.ic_channels[48].ic_flags&(IEEE80211_CHAN_HT40U|IEEE80211_CHAN_HT40D|IEEE80211_CHAN_VHT80|IEEE80211_CHAN_VHT160)));
    assert(d.com.sc_ic.ic_channels[6].ic_flags&IEEE80211_CHAN_HT40U);
}
static void countryBeaconHints(){
    ItlIwx d;auto &ic=d.com.sc_ic;
    setbit(ic.ic_beacon_hint,149);response(d);
    assert(!d.iwx_send_update_mcc_cmd(&d.com,"CN",2));
    assert(ic.ic_beacon_hint_mcc==0x434e&&!isset(ic.ic_beacon_hint,149));
    setbit(ic.ic_beacon_hint,149);
    response(d);assert(!d.iwx_send_update_mcc_cmd(&d.com,"CN",2));
    assert(isset(ic.ic_beacon_hint,149));
    response(d,51,0);assert(!d.iwx_send_update_mcc_cmd(&d.com,"ZZ",2));
    assert(ic.ic_beacon_hint_mcc==0x434e&&isset(ic.ic_beacon_hint,149));
    response(d,51,0x5553);d.error=EIO;
    assert(d.iwx_send_update_mcc_cmd(&d.com,"US",2)==EIO);
    assert(ic.ic_beacon_hint_mcc==0x434e&&isset(ic.ic_beacon_hint,149));
    d.error=0;assert(!d.iwx_send_update_mcc_cmd(&d.com,"US",2));
    assert(ic.ic_beacon_hint_mcc==0x5553&&!isset(ic.ic_beacon_hint,149));
}
int main(){rawMapping();bootAndNotifications();failedRepliesAndReset();newerNotificationAndTail();layoutsAndCapabilities();countryBeaconHints();}
