#include <cassert>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include "itlwm/hal_iwx/TimeEvent.hpp"
#define htole16(x) (x)
#define htole32(x) (x)
#define le32toh(x) (x)
#define DELAY(x) ((void)0)
#define DPRINTFN(n,x) ((void)0)
#define XYLog(...) ((void)0)
#define isset(p,b) ((p)[(b)/8] & (1U << ((b)%8)))
#define IWX_FW_CMD_ID_AND_COLOR(id,color) ((id)|((color)<<8))
enum { IWX_TIME_EVENT_CMD = 0x29, IWX_CMD_WANT_RESP = 1,
    IWX_FLAG_TE_ACTIVE = 8, IWX_FW_CTXT_ACTION_ADD = 1,
    IWX_FW_CTXT_ACTION_REMOVE = 3, IWX_TE_BSS_STA_AGGRESSIVE_ASSOC = 0,
    IWX_TE_V2_FRAG_NONE = 0, IWX_TE_V2_NOTIF_HOST_EVENT_START = 1,
    IWX_TE_V2_NOTIF_HOST_EVENT_END = 2, IWX_T2_V2_START_IMMEDIATELY = 0x800,
    IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD=54, IWX_SESSION_PROTECT_CONF_ASSOC=0,
    IWX_SESSION_PROTECTION_CMD=5, IWX_MAC_CONF_GROUP=3 };
struct iwx_node { uint32_t in_id=0, in_color=2; };
struct iwx_softc {
    uint32_t sc_flags=0, sc_time_event_uid=0;
    uint8_t sc_enabled_capa[8]={};
    struct { iwx_node *ic_bss=nullptr; } sc_ic;
};
struct iwx_session_prot_cmd {
    uint32_t id_and_color,action,conf_id,duration_tu,repetition_count,interval;
};
struct iwx_session_prot_notif { uint32_t mac_id,status,start,conf_id; };
struct iwx_time_event_cmd {
    uint32_t id_and_color,action,id,apply_time,max_delay,depends_on,interval,duration;
    uint8_t repeat,max_frags;
    uint16_t policy;
};
struct iwx_time_event_resp { uint32_t status,id,unique_id,id_and_color; };
struct iwx_time_event_notif { uint32_t timestamp,session_id,unique_id,id_and_color,action,status; };
struct iwx_rx_packet { size_t length; alignas(8) uint8_t data[64]; };
static size_t iwx_rx_packet_payload_len(const iwx_rx_packet *pkt) { return pkt->length; }
struct iwx_host_cmd {
    uint32_t id,flags; size_t resp_pkt_len;
    const void *data[1]; size_t len[1]; iwx_rx_packet *resp_pkt;
};
struct ItlIwx {
    IwxTimeEventState infraTimeEvent={};
    iwx_softc com;
    iwx_node infra;
    int sends=0,frees=0;
    std::function<int(const iwx_time_event_cmd&)> transport;
    std::function<int(const iwx_session_prot_cmd&)> sessionTransport;
    void iwx_sync_time_event_flags(iwx_softc *);
    void iwx_rx_time_event_response(iwx_softc *,const iwx_rx_packet *);
    void iwx_rx_time_event_notification(iwx_softc *,const iwx_rx_packet *);
    void iwx_rx_session_protection(iwx_softc *,const iwx_rx_packet *);
    int iwx_schedule_protect_session(iwx_softc *,iwx_node *,uint32_t);
    int iwx_cancel_session_protection(iwx_softc *,iwx_node *);
    int iwx_send_time_event_cmd(iwx_softc *,const iwx_time_event_cmd *);
    void iwx_protect_session(iwx_softc *,iwx_node *,uint32_t,uint32_t);
    void iwx_unprotect_session(iwx_softc *,iwx_node *);
    static uint32_t iwx_cmd_id(uint8_t id,uint8_t group,uint8_t version) {
        return id|(group<<8)|(version<<16);
    }
    int iwx_send_cmd_pdu(iwx_softc*,uint32_t id,uint32_t flags,size_t size,const void *data) {
        assert(id==0x305 && !flags && size==sizeof(iwx_session_prot_cmd));
        return sessionTransport(*static_cast<const iwx_session_prot_cmd *>(data));
    }
    int iwx_send_cmd(iwx_softc*,iwx_host_cmd *cmd) {
        assert(cmd->id==IWX_TIME_EVENT_CMD && cmd->flags==IWX_CMD_WANT_RESP);
        assert(cmd->len[0]==sizeof(iwx_time_event_cmd));
        assert(infraTimeEvent.pending);
        ++sends;
        cmd->resp_pkt=static_cast<iwx_rx_packet *>(calloc(1,sizeof(iwx_rx_packet)));
        return transport(*static_cast<const iwx_time_event_cmd *>(cmd->data[0]));
    }
    void iwx_free_resp(iwx_softc*,iwx_host_cmd *cmd) {
        ++frees;free(cmd->resp_pkt);cmd->resp_pkt=nullptr;
    }
    void reply(uint32_t status=1,uint32_t uid=42,uint32_t mac=0x200,uint32_t type=0,
               size_t length=sizeof(iwx_time_event_resp)) {
        iwx_rx_packet pkt={};pkt.length=length;
        iwx_time_event_resp response{status,type,uid,mac};memcpy(pkt.data,&response,sizeof(response));
        iwx_rx_time_event_response(&com,&pkt);
    }
    void notify(uint32_t action,uint32_t status=1,uint32_t uid=42,uint32_t mac=0x200,
                size_t length=sizeof(iwx_time_event_notif)) {
        iwx_rx_packet pkt={};pkt.length=length;
        iwx_time_event_notif n{100,1,uid,mac,action,status};memcpy(pkt.data,&n,sizeof(n));
        iwx_rx_time_event_notification(&com,&pkt);
    }
    iwx_time_event_cmd add() { iwx_time_event_cmd c={};c.action=1;c.id_and_color=0x200;return c; }
    void sessionNotify(uint32_t start,uint32_t status=1,uint32_t owner=0,
                       uint32_t config=0,size_t length=16) {
        iwx_rx_packet pkt={};pkt.length=length;
        iwx_session_prot_notif n{owner,status,start,config};memcpy(pkt.data,&n,sizeof(n));
        iwx_rx_session_protection(&com,&pkt);
    }
};
// PRODUCTION: events

int main() {
    ItlIwx d;
    auto add=d.add();
    d.transport=[&](const iwx_time_event_cmd &cmd) {
        assert(cmd.action==1 && cmd.id_and_color==0x200 && cmd.id==0);
        assert(cmd.duration==200 && cmd.max_delay==50);
        assert(cmd.policy==(1|2|0x800));
        d.reply();
        assert(d.com.sc_time_event_uid==42 && (d.com.sc_flags&8));
        d.notify(1);assert(d.infraTimeEvent.running);
        d.notify(2);assert(!(d.com.sc_flags&8));
        return 0;
    };
    // END before the synchronous send wakes must not resurrect the event.
    d.iwx_protect_session(&d.com,&d.infra,200,50);
    assert(!d.infraTimeEvent.pending && !d.infraTimeEvent.active && !(d.com.sc_flags&8));
    d.notify(1);assert(!d.infraTimeEvent.running);

    // A successful response is 1, not 0. Other status bits report failure.
    for (uint32_t status : {0u,2u,3u}) {
        d.transport=[&](const iwx_time_event_cmd&) { d.reply(status);return 0; };
        assert(d.iwx_send_time_event_cmd(&d.com,&add)==EIO);
        assert(!d.infraTimeEvent.active && !(d.com.sc_flags&8));
    }
    d.transport=[&](const iwx_time_event_cmd&) { d.reply();return 0; };
    assert(!d.iwx_send_time_event_cmd(&d.com,&add));
    assert(d.infraTimeEvent.active && !d.infraTimeEvent.running);
    int before=d.sends;
    d.iwx_protect_session(&d.com,&d.infra,200,50);assert(d.sends==before);
    d.notify(2,1,99);d.notify(2,1,42,0x201);d.notify(2,1,42,0x200,8);
    assert(d.infraTimeEvent.active);
    d.notify(1,0);assert(!d.infraTimeEvent.active && !(d.com.sc_flags&8));

    // A failed REMOVE retains ownership, allowing a later cancellation.
    assert(!d.iwx_send_time_event_cmd(&d.com,&add));
    d.transport=[&](const iwx_time_event_cmd &cmd) {
        assert(cmd.action==3 && cmd.id==42 && cmd.id_and_color==0x200);
        d.reply(0);return 0;
    };
    d.iwx_unprotect_session(&d.com,&d.infra);
    assert(d.infraTimeEvent.active && (d.com.sc_flags&8));
    d.transport=[&](const iwx_time_event_cmd &cmd) {
        assert(cmd.action==3 && cmd.id==42);
        d.notify(2);d.reply();return 0;
    };
    d.iwx_unprotect_session(&d.com,&d.infra);
    assert(!d.infraTimeEvent.active && !d.infraTimeEvent.uidValid && !(d.com.sc_flags&8));

    // Ignore malformed replies and replies for another MAC/type.
    for (int malformed=0;malformed<3;++malformed) {
        d.transport=[&](const iwx_time_event_cmd&) {
            d.reply(1,42,malformed==1?0x201:0x200,malformed==2?1:0,malformed==0?8:16);
            return 0;
        };
        assert(d.iwx_send_time_event_cmd(&d.com,&add)==EIO);
        assert(!d.infraTimeEvent.active);
    }
    d.transport=[&](const iwx_time_event_cmd&) {return ETIMEDOUT;};
    assert(d.iwx_send_time_event_cmd(&d.com,&add)==ETIMEDOUT);
    assert(!d.infraTimeEvent.pending);

    // Reset while sleeping invalidates this send without clearing a newer event.
    d.transport=[&](const iwx_time_event_cmd&) {
        d.infraTimeEvent.reset();d.iwx_sync_time_event_flags(&d.com);
        assert(d.infraTimeEvent.begin(0x200,0,false));
        d.reply(1,43);d.notify(1,1,43);
        d.infraTimeEvent.pending=false;
        return ENXIO;
    };
    assert(d.iwx_send_time_event_cmd(&d.com,&add)==ENXIO);
    assert(d.infraTimeEvent.active && d.infraTimeEvent.running && d.com.sc_time_event_uid==43);
    d.notify(2,1,42);assert(d.infraTimeEvent.active);
    d.notify(3,1,43);assert(!d.infraTimeEvent.active);
    assert(d.sends==d.frees);

    // Beacon-derived duration is already in TU; a 200 TU request stays 200 TU.
    d.com.sc_ic.ic_bss=&d.infra;
    d.com.sc_enabled_capa[54/8]=1U<<(54%8);
    d.sessionTransport=[&](const iwx_session_prot_cmd &cmd) {
        assert(cmd.id_and_color==0x200 && cmd.action==1 && !cmd.conf_id);
        assert(cmd.duration_tu==200 && !cmd.repetition_count && !cmd.interval);
        d.sessionNotify(1);assert(d.com.sc_flags&8);
        d.sessionNotify(0);return 0;
    };
    assert(!d.iwx_schedule_protect_session(&d.com,&d.infra,200));
    assert(!(d.com.sc_flags&8));
    d.sessionNotify(1);assert(d.com.sc_flags&8);
    // AWDL's MAC/configuration and truncated packets cannot end AP protection.
    d.sessionNotify(0,1,1);d.sessionNotify(0,0,1);
    d.sessionNotify(0,1,0,2);d.sessionNotify(0,1,0,0,8);
    assert(d.com.sc_flags&8);
    d.sessionNotify(1,0);assert(!(d.com.sc_flags&8));
    d.com.sc_flags|=8;
    d.com.sc_enabled_capa[54/8]=0;
    d.sessionNotify(0);assert(d.com.sc_flags&8);
    d.com.sc_enabled_capa[54/8]=1U<<(54%8);
    d.sessionTransport=[&](const iwx_session_prot_cmd &cmd) {
        assert(cmd.id_and_color==0x200 && cmd.action==3 && !cmd.conf_id);
        assert(!cmd.duration_tu && !cmd.repetition_count && !cmd.interval);
        d.sessionNotify(0);return 0;
    };
    assert(!d.iwx_cancel_session_protection(&d.com,&d.infra));
    assert(!(d.com.sc_flags&8));
    d.sessionTransport=[](const iwx_session_prot_cmd&) {return EIO;};
    assert(d.iwx_schedule_protect_session(&d.com,&d.infra,200)==EIO);
    assert(d.iwx_cancel_session_protection(&d.com,&d.infra)==EIO);
}
