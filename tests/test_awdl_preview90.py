"""Failing-first regressions for Preview 90:
1. Deferred llw0 Skywalk registration after awdl0 attaches guarantees awdl0_ifindex < llw0_ifindex
   so Rapport RPStreamSession _getSockAddrInterfaceType resolves AWDL (4) instead of non-LLW0 (2),
   with race-free thread_call cancellation/cleanup and legacy fallback.
2. Bounded 2.4 GHz social-slot parking (SocialParkIntervalUS = 750 ms) requires >= 16 acknowledged
   TXBA frames AND fresh peer HTCap/sync/sequence (< 1.0 s) so AWDLHTCapablePeers never starves.
3. ADDBA negotiation priority: negotiatingNow() holds non-BA peer frames during active ADDBA
   due/in-flight/receive-probe windows, caps pre-ADDBA queue 2 depth, extends receiveProbeUntilUS
   for 60 ms after MAC ACK of ADDBA Request, and lets txReady() saturate queueCapacity once Agreed.
4. TXBA transient failure survival (all non-fatal FW statuses keep Agreed up to MaxConsecutiveFailures),
   paused-queue stall watchdog exemption + unpause progress reset, and retire-reason/fail-status telemetry.
"""
import re
import unittest

import test_awdl_preview82 as p82
import test_regressions as support
from iwx_firmware import declarations as firmware_declarations
from test_offline import definition


class Preview90Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p90_llw_deferred_registration_guarantees_awdl_ifindex_before_llw(self):
        # 1. AirportItlwm::start still calls startLLWInterface before awdlBSD->registerService (P82 contract).
        v2_src = (support.ROOT / "AirportItlwm/AirportItlwmV2.cpp").read_text()
        start_ctrl = v2_src.split("bool AirportItlwm::start(", 1)[1].split("void AirportItlwm::stop(", 1)[0]
        llw_pos = start_ctrl.find("startLLWInterface()")
        awdl_reg_pos = start_ctrl.find("awdlBSD->registerService()")
        self.assertGreater(llw_pos, 0)
        self.assertGreater(awdl_reg_pos, llw_pos)

        # 2. startLLWInterface must NOT call registerSkywalk() synchronously before awdl0 attaches!
        start_llw = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwm::startLLWInterface")
        self.assertNotIn("registerSkywalk(", start_llw)

        # 3. Runtime C++ ASan+UBSan verification of real production lifecycle functions:
        #    startLLWInterface, scheduleLLWRegistration, scheduleLLWRegistrationGated,
        #    completeLLWRegistrationCall, completeLLWRegistration, stopLLWInterface,
        #    and AirportItlwmEthernetInterface::attachToDataLinkLayer.
        llw_routines = "\n".join(
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwm::" + n)
            for n in (
                "startLLWInterface",
                "scheduleLLWRegistration",
                "scheduleLLWRegistrationGated",
                "completeLLWRegistrationCall",
                "completeLLWRegistration",
                "stopLLWInterface",
            )
        )
        eth_attach = definition(
            "AirportItlwm/AirportItlwmEthernetInterface.cpp",
            "AirportItlwmEthernetInterface::attachToDataLinkLayer",
        )
        src = f"""
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define AIRPORT_WCL 1
#define __IO80211_TARGET 150200
#define __MAC_15_2 150200
#define IFNAMSIZ 16
#define kIOEthernetAddressSize 6
#define kIOInterfaceUnit "IOInterfaceUnit"
#define kIOInterfaceNamePrefix "IOInterfaceNamePrefix"
#define kIOMACAddress "IOMACAddress"
#define XYLog(...) ((void)0)
#define OSDynamicCast(cls, ptr) dynamic_cast<cls *>(ptr)
#define OSSafeReleaseNULL(ptr) do {{ if (ptr) {{ (ptr)->release(); (ptr) = nullptr; }} }} while (0)

using IOReturn = int;
using IOOptionBits = unsigned int;
using UInt8 = uint8_t;
using boolean_t = bool;
enum {{
    kIOReturnSuccess = 0,
    kIOReturnError = -1,
    kIOReturnNotReady = -2,
    APPLE80211_VIF_AWDL = 6,
    APPLE80211_VIF_LOW_LATENCY = 7
}};
struct ether_addr {{ uint8_t octet[6]; }};
using IOEthernetAddress = ether_addr;

static uint32_t gNextIfindex = 5;
static std::vector<std::string> gAttachOrder;

struct Ifnet {{
    std::string name;
    unsigned unit = 0;
    uint32_t index = 0;
    uint32_t mtu = 1500;
}};
using ifnet_t = Ifnet *;
static const char *ifnet_name(ifnet_t i) {{ return i->name.c_str(); }}
static unsigned ifnet_unit(ifnet_t i) {{ return i->unit; }}
static int ifnet_set_lladdr(ifnet_t, const void *, size_t) {{ return 0; }}
static int ifnet_set_mtu(ifnet_t i, uint32_t m) {{ i->mtu = m; return 0; }}

struct OSObject {{
    int refcount = 1;
    virtual ~OSObject() = default;
    void retain() {{ ++refcount; }}
    void release() {{ if (--refcount == 0) delete this; }}
}};
struct OSData : OSObject {{ static OSData *withBytes(const void *, size_t) {{ return nullptr; }} }};
struct OSString : OSObject {{ static OSString *withCString(const char *) {{ return nullptr; }} }};
struct OSNumber : OSObject {{ static OSNumber *withNumber(unsigned long long, unsigned) {{ return nullptr; }} }};
struct IOService : OSObject {{
    IOService *provider = nullptr;
    bool published = false;
    bool started = false;
    bool attached = false;
    virtual bool attach(IOService *p) {{ provider = p; attached = true; return true; }}
    virtual void detach(IOService *p) {{ if (provider == p) {{ provider = nullptr; attached = false; }} }}
    virtual IOService *getProvider() const {{ return provider; }}
    virtual bool start(IOService *) {{ started = true; return true; }}
    virtual void stop(IOService *) {{ started = false; }}
    virtual void registerService() {{ published = true; }}
    void setProperty(const char *, const void *) {{}}
    void setProperty(const char *, void *, unsigned) {{}}
    void setProperty(const char *, bool) {{}}
}};

struct IOWorkLoop : OSObject {{}};
struct IOCommandGate : OSObject {{
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    bool inGate = false;
    IOReturn runAction(Action act, void *a0 = nullptr, void *a1 = nullptr, void *a2 = nullptr, void *a3 = nullptr) {{
        assert(!inGate);
        inGate = true;
        IOReturn r = act(nullptr, a0, a1, a2, a3);
        inGate = false;
        return r;
    }}
}};
IOCommandGate *_fCommandGate = nullptr;

using thread_call_param_t = void *;
using thread_call_func_t = void (*)(thread_call_param_t, thread_call_param_t);
struct ThreadCall {{
    thread_call_func_t fn = nullptr;
    thread_call_param_t p0 = nullptr;
    bool pending = false;
    bool running = false;
    bool freed = false;
}};
using thread_call_t = ThreadCall *;
static std::vector<ThreadCall *> gLiveCalls;
static unsigned gCallsFreed = 0;

static thread_call_t thread_call_allocate(thread_call_func_t fn, thread_call_param_t p0) {{
    auto *c = new ThreadCall{{fn, p0, false, false, false}};
    gLiveCalls.push_back(c);
    return c;
}}
static boolean_t thread_call_enter(thread_call_t c) {{
    assert(c && !c->freed);
    bool was = c->pending;
    c->pending = true;
    return was;
}}
static boolean_t thread_call_cancel_wait(thread_call_t c) {{
    assert(c && !c->freed);
    if (c->pending) {{
        c->pending = false;
        return true;
    }}
    return false;
}}
static boolean_t thread_call_free(thread_call_t c) {{
    assert(c && !c->freed && !c->pending && !c->running);
    c->freed = true;
    ++gCallsFreed;
    delete c;
    return true;
}}
static void *IOMalloc(size_t n) {{ return calloc(1, n); }}

struct IOSkywalkNetworkInterface {{ struct RegistrationInfo {{ uint8_t bytes[0x130]; }}; }};
struct IOSkywalkEthernetInterface {{ struct RegistrationInfo {{ uint8_t bytes[0x130]; }}; }};

struct IO80211SkywalkInterface : IOService {{
    unsigned role = 0;
    ether_addr mac = {{{{0x02, 0x11, 0x22, 0x33, 0x44, 0x55}}}};
    unsigned getInterfaceRole() const {{ return role; }}
    virtual IOReturn getHardwareAddress(ether_addr *out) {{ *out = mac; return kIOReturnSuccess; }}
    virtual IOReturn prepareBSDInterface(ifnet_t, int) {{ return kIOReturnSuccess; }}
}};

struct AirportItlwmEthernetInterface;
struct AirportItlwmLLWInterface;

struct AirportItlwmAWDLInterface : IO80211SkywalkInterface {{
    AirportItlwmLLWInterface *companion = nullptr;
    AirportItlwmLLWInterface *boundLLW = nullptr;
    AirportItlwmEthernetInterface *boundLLWBSD = nullptr;
    AirportItlwmAWDLInterface() {{ role = APPLE80211_VIF_AWDL; }}
    void setCompanionLowLatencySkywalkInterface(AirportItlwmLLWInterface *c) {{ companion = c; }}
    void bindLLW(AirportItlwmLLWInterface *l, AirportItlwmEthernetInterface *b) {{ boundLLW = l; boundLLWBSD = b; }}
}};

struct AirportItlwmLLWInterface : IO80211SkywalkInterface {{
    struct Exp1 {{ IOSkywalkNetworkInterface::RegistrationInfo *fRegistrationInfo = nullptr; }} e1, *mExpansionData = &e1;
    struct Exp2 {{ IOSkywalkEthernetInterface::RegistrationInfo *fRegistrationInfo = nullptr; }} e2, *mExpansionData2 = &e2;
    bool skywalkRegistered = false;
    bool failSkywalk = false;
    Ifnet ifnet;
    ~AirportItlwmLLWInterface() override {{
        free(e1.fRegistrationInfo);
        free(e2.fRegistrationInfo);
    }}
    bool initWithAWDL(IOService *, AirportItlwmAWDLInterface *, const ether_addr *m) {{
        role = APPLE80211_VIF_LOW_LATENCY;
        mac = *m;
        return true;
    }}
    void setInterfaceId(int) {{}}
    void setCompanionInterfaceId(int) {{}}
    void deferBSDAttach(bool) {{}}
    bool initRegistrationInfo(IOSkywalkEthernetInterface::RegistrationInfo *info, int, size_t) {{
        memset(info, 0, sizeof(*info));
        return true;
    }}
    bool registerSkywalk(IOWorkLoop *wl) {{
        assert(wl);
        assert(!_fCommandGate || !_fCommandGate->inGate); // Must NEVER run under command gate!
        if (failSkywalk) return false;
        skywalkRegistered = true;
        ifnet.name = "llw";
        ifnet.unit = 0;
        ifnet.index = gNextIfindex++;
        gAttachOrder.push_back("llw0:" + std::to_string(ifnet.index));
        return true;
    }}
    void deregisterSkywalk() {{ skywalkRegistered = false; }}
}};

struct IONetworkInterface : IOService {{
    IOService *ctrl = nullptr;
    Ifnet ifnet;
    bool attachedToDLL = false;
    IOService *getController() const {{ return ctrl; }}
    ifnet_t getIfnet() {{ return attachedToDLL ? &ifnet : nullptr; }}
    bool setInterfaceSubType(uint32_t) {{ return true; }}
    virtual IOReturn attachToDataLinkLayer(IOOptionBits, void *) {{
        attachedToDLL = true;
        ifnet.name = "awdl";
        ifnet.unit = 0;
        ifnet.index = gNextIfindex++;
        gAttachOrder.push_back(ifnet.name + std::to_string(ifnet.unit) + ":" + std::to_string(ifnet.index));
        return kIOReturnSuccess;
    }}
    virtual void detachFromDataLinkLayer(IOOptionBits, void *) {{ attachedToDLL = false; }}
    void stopOutputThread() {{}}
}};
struct IOEthernetInterface : IONetworkInterface {{}};

struct AirportItlwmEthernetInterface : IOEthernetInterface {{
    IO80211SkywalkInterface *interface = nullptr;
    bool isAttach = false;
    static IOReturn prepareBSDGated(OSObject *, void *arg0, void *, void *, void *) {{
        auto *self = static_cast<AirportItlwmEthernetInterface *>(arg0);
        return self->interface->prepareBSDInterface(self->getIfnet(), 0);
    }}
    IOReturn attachToDataLinkLayer(IOOptionBits options, void *parameter) override;
}};

struct IONetworkController : IOService {{
    bool attachInterface(IONetworkInterface **out, bool publish) {{
        assert(!publish);
        auto *eth = new AirportItlwmEthernetInterface;
        eth->ctrl = this;
        *out = eth;
        return true;
    }}
    void detachInterface(IONetworkInterface *) {{}}
}};

struct AirportItlwm : IONetworkController {{
    IOWorkLoop wl;
    AirportItlwmAWDLInterface *awdlInterface = nullptr;
    AirportItlwmEthernetInterface *awdlBSD = nullptr;
    AirportItlwmLLWInterface *llwInterface = nullptr;
    AirportItlwmEthernetInterface *llwBSD = nullptr;
    bool creatingAWDL = false, awdlStarted = false, awdlAttached = false;
    bool creatingLLW = false, llwStarted = false, llwAttached = false;
    bool llwRegDeferred = false, llwRegScheduled = false, llwStopping = false;
    thread_call_t llwRegThreadCall = nullptr;
    bool nextLLWFailSkywalk = false;

    IOWorkLoop *getWorkLoop() const {{ return const_cast<IOWorkLoop *>(&wl); }}
    bool attachInterface(IO80211SkywalkInterface *iface, IOService *) {{
        if (auto *l = dynamic_cast<AirportItlwmLLWInterface *>(iface)) l->failSkywalk = nextLLWFailSkywalk;
        return true;
    }}
    void detachInterface(IO80211SkywalkInterface *, bool) {{}}
    bool startLLWInterface();
    bool scheduleLLWRegistration();
    static IOReturn scheduleLLWRegistrationGated(OSObject *, void *, void *, void *, void *);
    static void completeLLWRegistrationCall(thread_call_param_t, thread_call_param_t);
    void completeLLWRegistration();
    void stopLLWInterface();
}};

#define super IOEthernetInterface
{eth_attach}
#undef super
{llw_routines}

int main() {{
    IOCommandGate gate;
    _fCommandGate = &gate;

    // Scenario 1: Normal boot order -> awdl0 attaches FIRST (ifindex 5), then deferred LLW thread
    // registers llw0 via Skywalk (ifindex 6), then publishes llw0 and awdl0!
    {{
        gNextIfindex = 5; gAttachOrder.clear();
        auto *ctrl = new AirportItlwm;
        ctrl->awdlInterface = new AirportItlwmAWDLInterface;
        ctrl->awdlBSD = new AirportItlwmEthernetInterface;
        ctrl->awdlBSD->ctrl = ctrl;
        ctrl->awdlBSD->interface = ctrl->awdlInterface;

        assert(ctrl->startLLWInterface());
        assert(ctrl->llwInterface != nullptr);
        assert(!ctrl->llwInterface->skywalkRegistered); // Deferred!
        assert(!ctrl->llwInterface->published);
        assert(!ctrl->awdlInterface->published);
        assert(ctrl->awdlInterface->companion == ctrl->llwInterface);
        assert(gAttachOrder.empty());

        // IONetworkStack attaches awdl0 first.
        assert(ctrl->awdlBSD->attachToDataLinkLayer(0, nullptr) == kIOReturnSuccess);
        assert(ctrl->awdlBSD->isAttach);
        assert(ctrl->awdlBSD->ifnet.index == 5);
        assert(!ctrl->awdlInterface->published); // Still waiting for LLW registration!
        assert(ctrl->refcount == 2); // Retained for thread_call!
        assert(ctrl->llwRegThreadCall && ctrl->llwRegThreadCall->pending);

        // Execute deferred kernel thread_call outside any gate/stack lock.
        ThreadCall *call = ctrl->llwRegThreadCall;
        call->pending = false; call->running = true;
        call->fn(call->p0, nullptr);
        call->running = false;

        assert(ctrl->refcount == 1); // Balanced release!
        assert(ctrl->llwInterface->skywalkRegistered);
        assert(ctrl->llwInterface->published);
        assert(ctrl->awdlInterface->published);
        assert(ctrl->awdlBSD->ifnet.index == 5 && ctrl->llwInterface->ifnet.index == 6);
        assert(ctrl->awdlBSD->ifnet.index < ctrl->llwInterface->ifnet.index);
        assert(gAttachOrder.size() == 2 && gAttachOrder[0] == "awdl0:5" && gAttachOrder[1] == "llw0:6");

        ctrl->stopLLWInterface();
        assert(!ctrl->llwInterface && !ctrl->llwRegThreadCall);
        ctrl->awdlBSD->release();
        ctrl->awdlInterface->release();
        ctrl->release();
    }}

    // Scenario 2: stopLLWInterface() cancels scheduled-but-unrun thread_call cleanly (zero leak, no publish).
    {{
        gNextIfindex = 5; gAttachOrder.clear();
        auto *ctrl = new AirportItlwm;
        ctrl->awdlInterface = new AirportItlwmAWDLInterface;
        ctrl->awdlBSD = new AirportItlwmEthernetInterface;
        ctrl->awdlBSD->ctrl = ctrl;
        ctrl->awdlBSD->interface = ctrl->awdlInterface;

        assert(ctrl->startLLWInterface());
        assert(ctrl->awdlBSD->attachToDataLinkLayer(0, nullptr) == kIOReturnSuccess);
        assert(ctrl->refcount == 2);
        ctrl->stopLLWInterface();
        assert(ctrl->refcount == 1); // Cancelled call released retain!
        assert(!ctrl->awdlInterface->published);
        assert(!ctrl->llwInterface && !ctrl->llwRegThreadCall);
        ctrl->awdlBSD->release();
        ctrl->awdlInterface->release();
        ctrl->release();
    }}

    // Scenario 3: Native Skywalk registration failure falls back to legacy llwBSD AFTER awdl0 and still publishes awdl0.
    {{
        gNextIfindex = 5; gAttachOrder.clear();
        auto *ctrl = new AirportItlwm;
        ctrl->nextLLWFailSkywalk = true;
        ctrl->awdlInterface = new AirportItlwmAWDLInterface;
        ctrl->awdlBSD = new AirportItlwmEthernetInterface;
        ctrl->awdlBSD->ctrl = ctrl;
        ctrl->awdlBSD->interface = ctrl->awdlInterface;

        assert(ctrl->startLLWInterface());
        assert(ctrl->awdlBSD->attachToDataLinkLayer(0, nullptr) == kIOReturnSuccess);
        ThreadCall *call = ctrl->llwRegThreadCall;
        call->pending = false; call->running = true;
        call->fn(call->p0, nullptr);
        call->running = false;

        assert(ctrl->refcount == 1);
        assert(!ctrl->llwInterface->skywalkRegistered);
        assert(ctrl->llwBSD != nullptr && ctrl->llwBSD->published);
        assert(ctrl->awdlInterface->boundLLWBSD == ctrl->llwBSD);
        assert(ctrl->awdlInterface->published);
        ctrl->stopLLWInterface();
        ctrl->awdlBSD->release();
        ctrl->awdlInterface->release();
        ctrl->release();
    }}

    return 0;
}}
"""
        self.compile_run(src)

    def test_p90_social_park_bounded_750ms_with_ack_and_peer_freshness_guards(self):
        radio_src = (support.ROOT / "tests/fixtures/awdl_radio.cpp").read_text().rsplit("int main()", 1)[0]
        routines = (
            "awdlMAC", "awdlChannelAllowed", "awdlWideChannelAllowed", "awdlPHY", "awdlBinding",
            "receiveAWDLSession", "finishAWDLDwell", "serviceAWDLChannel",
            "prepareAWDLListener", "closeAWDLListenerWindow", "serviceAWDLListener",
            "configureAWDLPeerWidth", "startAWDLRadio", "awdlRemoveQueue",
            "stopAWDLRadio", "resetAWDLRadio", "awdlTxWindowOpen", "transmitAWDL",
            "completeAWDLTx", "sampleAWDLRate", "noteAWDLTXBlockAck",
            "awdlTXAntenna", "awdlTXMcs", "awdlTXWidth",
        )
        radio_src = radio_src.replace(
            "// PEER TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_transport.cpp").read_text(),
        )
        radio_src = radio_src.replace(
            "// PRODUCTION",
            "\n".join(definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::" + n) for n in routines),
        )
        radio_src = radio_src.replace(
            '#include "tests/fixtures/awdl_transport.hpp"',
            (support.ROOT / "tests/fixtures/awdl_transport.hpp").read_text(),
        )
        radio_src = radio_src.replace(
            '#include "itlwm/hal_iwx/AWDLTxBudget.hpp"',
            (support.ROOT / "itlwm/hal_iwx/AWDLTxBudget.hpp").read_text(),
        )
        transport = "\n".join(
            definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name)
            for name in (
                "iwx_rval2ridx", "iwx_toggle_tx_ant", "iwx_tx_fill_awdl_cmd", "iwx_tx_update_byte_tbl",
                "iwx_tx", "iwx_rx_tx_cmd_single", "iwx_rx_tx_cmd", "iwx_txd_done",
                "iwx_ampdu_txq_advance", "iwx_reset_tx_ring",
            )
        )
        transport += "\n" + definition("itlwm/hal_iwx/ItlIwx.cpp", "iwx_clear_tx_desc")
        transport += "\n" + definition("itlwm/hal_iwx/ItlIwx.cpp", "iwx_awdl_is_quz_0034")
        driver = (support.ROOT / "itlwm/hal_iwx/ItlIwx.cpp").read_text()
        table = re.search(r"const struct iwx_rate \{.*?\} iwx_rates\[\] = \{.*?^\};", driver, re.S | re.M)[0]
        net = (support.ROOT / "itl80211/openbsd/net80211/ieee80211.c").read_text()
        rates = re.search(r"const struct ieee80211_rateset ieee80211_std_rateset_11a =.*?;", net, re.S)[0]
        radio_src = radio_src.replace("struct ItlIwx {", table + rates + "\nstruct ItlIwx {")
        radio_src = radio_src.replace("static const uint8_t self", transport + "\nstatic const uint8_t self")
        radio_src = radio_src.replace(
            "// TRANSPORT TYPES",
            "void iwx_clear_tx_desc(struct iwx_softc *,struct iwx_tx_ring *,int);",
        )
        radio_src = radio_src.replace(
            "// FIRMWARE DECLARATIONS",
            firmware_declarations(
                (
                    "iwx_mac_data_ap", "iwx_mac_data_ibss", "iwx_mac_data_sta", "iwx_mac_data_go",
                    "iwx_mac_data_p2p_sta", "iwx_mac_data_pibss", "iwx_mac_data_p2p_dev", "iwx_ac_qos",
                    "iwx_mac_ctx_cmd", "iwx_fw_channel_info", "iwx_phy_context_cmd", "iwx_binding_cmd",
                    "iwx_rlc_properties", "iwx_sad_properties", "iwx_rlc_config_cmd",
                    "iwx_device_power_cmd", "iwx_add_sta_cmd", "iwx_rm_sta_cmd",
                    "iwx_session_prot_cmd", "iwx_session_prot_notif", "iwx_tx_data", "iwx_tfh_tb", "iwx_tfh_tfd",
                    "iwx_cmd_header", "iwx_cmd_header_wide", "iwx_device_cmd", "iwx_dram_sec_info",
                    "iwx_tx_cmd_gen2", "iwx_tx_cmd_gen3", "iwx_agn_scd_bc_tbl", "iwx_gen3_bc_tbl",
                    "iwx_agg_tx_status", "iwx_tx_resp",
                ),
                radio_src,
                ("iwx_gen2_tx_fifo",),
            ),
        )
        extra = r"""
static void preview90SocialParkGuards() {
    static_assert(RadioState::MaxParkWindowUS == 140000ULL, "140 ms bounded infra bridge park");

    ItlIwx d;
    for (unsigned c : {6U, 36U, 40U, 44U, 48U, 149U, 153U, 157U, 161U}) {
        const unsigned band = c == 6 ? IEEE80211_CHAN_2GHZ : (IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_VHT80);
        d.com.sc_ic.ic_channels[c] = {band, uint16_t(c == 6 ? 2437 : 5000 + 5 * c)};
        d.com.sc_ic.ic_chan_active[c / 8] |= 1U << (c % 8);
    }
    d.rlcVersion = 2;
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    d.com.sc_phyctxt[0].channel = &d.com.sc_ic.ic_channels[48];
    assert(!d.startAWDLRadio(self));

    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 9};
    auto &peer = d.awdlPeers.peers[0];
    peer = {};
    peer.occupied = true;
    AirportAWDL::copyAddress(peer.address, peerAddr);
    peer.haveSync = true;
    peer.haveSequence = true;
    peer.metadata.fields = AirportAWDL::HTCapabilities;
    peer.metadata.rxHTMcs = 0xff;
    auto refreshPeer = [&](uint64_t t) {
        peer.lastSeenUS = peer.syncReceivedUS = peer.sequenceReceivedUS = peer.htCapabilitiesReceivedUS = t;
    };
    refreshPeer(clockUS);

    auto &tx = d.awdlRadio.blockAckTX;
    tx.configured = true; tx.epoch = d.dmaEpoch;
    tx.phase = AirportAWDL::TXBlockAckQueue::Agreed;
    AirportAWDL::copyAddress(tx.peer, peerAddr);
    tx.enabled = true; tx.paused = false; tx.widthMHz = 80;
    tx.lastHeavyDataUS = clockUS;

    RadioState::GrantPolicy p80 = {}; p80.widthMHz = 80; p80.slot = 1;
    RadioState::GrantPolicy p20 = {}; p20.widthMHz = 20; p20.slot = 2; p20.bridgeInfraToWide = true;
    assert(!d.serviceAWDLChannel(149, clockUS + 96000, p80));
    assert(!d.serviceAWDLChannel(6, clockUS + 16000, p20)); // Ch 6 never parks even when bridgeInfraToWide is set!
    assert(d.awdlRadio.channel == 6 && d.awdlRadio.blockAckTXStats.socialParks == 0);
    assert(d.awdlRadio.lastWideSocialVisitUS == clockUS);

    // Bridged 2-slot infra [48, 48] parks on 149@80, while Ch 6 still tunes immediately!
    clockUS += 16000; refreshPeer(clockUS); tx.lastHeavyDataUS = clockUS; tx.widthMHz = 80;
    tx.acknowledged = 64;
    p80.slot = 5; assert(!d.serviceAWDLChannel(149, clockUS + 96000, p80));
    const size_t phyBefore = d.phyCommands.size();
    p20.slot = 14; assert(!d.serviceAWDLChannel(48, clockUS + 131072, p20));
    assert(!d.awdlRadio.dwell.running && d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);
    assert(d.phyCommands.size() == phyBefore && d.awdlRadio.blockAckTXStats.infraParks == 1);

    clockUS += 131072; refreshPeer(clockUS); tx.lastHeavyDataUS = clockUS;
    p80.slot = 0; assert(!d.serviceAWDLChannel(149, clockUS + 96000, p80));
    p20.slot = 8; assert(!d.serviceAWDLChannel(6, clockUS + 16000, p20));
    assert(d.awdlRadio.channel == 6 && d.awdlRadio.blockAckTXStats.socialParks == 0);

    assert(!d.stopAWDLRadio());
}

int main() {
    preview90SocialParkGuards();
    return 0;
}
"""
        self.compile_run(radio_src + extra)

    def test_p90_addba_negotiation_priority_and_probe_extension_after_ack(self):
        # Part A: TXBlockAckQueue::negotiatingNow() & allowsSoftwareAggregate() + completeAWDLTXManagement probe extension.
        hal_complete = definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::completeAWDLTXManagement")
        src_a = f"""
#include <cassert>
#include <cstdint>
#include "AirportItlwm/AWDL/RadioState.hpp"

using namespace AirportAWDL;
static uint64_t nowUS = 10000000;
static uint64_t awdlRadioNow() {{ return nowUS; }}

struct ItlIwx {{
    RadioState awdlRadio = {{}};
    bool completeAWDLTXManagement(uint64_t token, bool acknowledged);
}};
{hal_complete}

int main() {{
    TXBlockAckQueue tx = {{}};
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Asking;
    tx.nextAskUS = nowUS + 200000;
    assert(!tx.negotiatingNow(nowUS, nowUS + 64000));
    assert(tx.allowsSoftwareAggregate(nowUS));

    // Pre-ADDBA 15ms window: allowsSoftwareAggregate false, negotiatingNow still false until nextAskUS.
    assert(!tx.allowsSoftwareAggregate(tx.nextAskUS - 10000));
    assert(!tx.negotiatingNow(tx.nextAskUS - 10000, tx.nextAskUS + 64000));

    // At nextAskUS on a long dwell (> 19ms): negotiatingNow true!
    assert(tx.negotiatingNow(tx.nextAskUS, tx.nextAskUS + 64000));
    // On a short 16ms ch 6 slot (<= 19ms) with nothing in flight: negotiatingNow false!
    assert(!tx.negotiatingNow(tx.nextAskUS, tx.nextAskUS + 16000));

    // Active receiveProbeUntilUS: both negotiatingNow true AND allowsSoftwareAggregate false even if nextAskUS far!
    tx.receiveProbeUntilUS = nowUS + 60000;
    assert(tx.negotiatingNow(nowUS, nowUS + 16000));
    assert(!tx.allowsSoftwareAggregate(nowUS));

    // completeAWDLTXManagement(token, true) extends open receiveProbeUntilUS by full 60ms AFTER MAC ACK!
    ItlIwx d;
    auto &owner = d.awdlRadio.blockAckTX;
    owner.configured = true;
    owner.phase = TXBlockAckQueue::Asking;
    owner.managementToken = 77;
    owner.receiveProbeUntilUS = nowUS + 60000;
    nowUS += 45000; // ADDBA Request waited 45ms in queue before MAC ACK!
    assert(d.completeAWDLTXManagement(77, true));
    assert(owner.managementToken == 0 && owner.lastAskAcked);
    assert(owner.receiveProbeUntilUS == nowUS + TXBlockAckQueue::ReceiveProbeDurationUS);
    assert(d.awdlRadio.blockAckTXStats.acknowledged == 1);

    // If receiveProbeUntilUS was NOT open (same-channel / no RXBA), ACK does NOT open a spurious probe.
    owner.managementToken = 78;
    owner.receiveProbeUntilUS = 0;
    assert(d.completeAWDLTXManagement(78, true));
    assert(owner.receiveProbeUntilUS == 0);
    return 0;
}}
"""
        self.compile_run(src_a)

        # Part B: Live tick() / txReady() / completeUnicast() Sidecar flood yields to ADDBA negotiation.
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        extra_routines = "\n".join(
            definition("AirportItlwm/AirportItlwmAWDLInterface.cpp", "AirportItlwmAWDLInterface::" + n)
            for n in ("submitOutputPackets", "drainPendingData", "txReady")
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines() + "\n" + extra_routines)
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src = tick_src.replace("} pending[64]={};", "} pending[96]={};")
        tick_src = tick_src.replace("inFlight[64]={};", "inFlight[96]={};")
        tick_src += r"""
int main() {
    clockUS = 10000000;
    const uint8_t peerAddr[] = {2, 0, 0, 0, 0, 9};
    AirportItlwmAWDLInterface i;
    dataPeer(i, peerAddr);
    i.nextActionUS = i.nextMIFUS = clockUS + 5000000;
    i.device.acknowledgeData = false;
    i.device.loop.gated = true;
    i.device.awdlRadio.listener = true;

    i.tick();
    assert(i.device.awdlTxWindowOpen());

    auto &tx = i.device.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.phase = TXBlockAckQueue::Asking;
    tx.cookie = 90;
    tx.epoch = i.device.dmaEpoch;
    copyAddress(tx.peer, peerAddr);
    tx.dialog = 1;
    tx.tid = 0;
    tx.nextAskUS = clockUS; // ADDBA due right now!

    // Flood 40 Sidecar packets into network.output + 16 into pending[].
    for (unsigned n = 0; n < 16; ++n) i.enqueue(ethernet(i.address.octet, peerAddr));
    for (unsigned n = 0; n < 40; ++n) i.network.output.push_back(ethernet(i.address.octet, peerAddr));

    // 1. txReady() during active ADDBA negotiation MUST NOT pull or stuff non-BA frames into queue 2,
    //    and MUST arm 1 ms timer so tick()->serviceTXBlockAck() fires immediately!
    const size_t outBefore = i.network.output.size();
    const unsigned dataBefore = i.device.data;
    i.scheduler.delay = 5;
    AirportItlwmAWDLInterface::txReady(&i);
    assert(i.device.data == dataBefore);
    assert(i.network.output.size() == outBefore);
    assert(i.scheduler.delay == 1);

    // 2. tick() sends the ADDBA Request (attempts == 1, managementToken != 0) while holding Sidecar
    //    non-BA video frames cleanly in pending[] (ZERO non-BA frames stuffed behind ADDBA!).
    i.tick();
    assert(tx.attempts == 1 && tx.managementToken != 0);
    assert(i.device.data == dataBefore);
    assert(i.pendingCount == 16);
    assert(i.scheduler.delay == 1);

    // 3. completeUnicast() on ADDBA Request ACK arms 1 ms timer!
    i.scheduler.delay = 5;
    tx.receiveProbeUntilUS = clockUS + 60000;
    i.completeUnicast(tx.managementToken, true);
    assert(tx.managementToken == 0);
    assert(i.scheduler.delay == 1);

    // 4. Once Agreed, txReady() refills across multiple passes past 32 frames!
    tx.receiveProbeUntilUS = 0;
    tx.phase = TXBlockAckQueue::Agreed;
    tx.enabled = true; tx.paused = false; tx.window = 64; tx.queue = 2;
    tx.windowEndUS = i.device.awdlRadio.dwell.windowEndUS;
    AirportItlwmAWDLInterface::txReady(&i);
    assert(i.device.data > dataBefore);
    assert(i.network.output.size() < outBefore);

    i.drain();
    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)

    def test_p90_txba_transient_failure_survival_and_paused_watchdog_and_telemetry(self):
        # Verify fatal status constants match if_iwxreg.h verbatim.
        reg_text = (support.ROOT / "itlwm/hal_iwx/if_iwxreg.h").read_text()
        expected_codes = {
            "IWX_TX_STATUS_FAIL_INTERNAL_CROSSED_RETRY": 0x81,
            "IWX_TX_STATUS_FAIL_SHORT_LIMIT": 0x82,
            "IWX_TX_STATUS_FAIL_LONG_LIMIT": 0x83,
            "IWX_TX_STATUS_FAIL_RFKILL_FLUSH": 0x86,
            "IWX_TX_STATUS_FAIL_LIFE_EXPIRE": 0x87,
            "IWX_TX_STATUS_FAIL_DEST_PS": 0x88,
            "IWX_TX_STATUS_FAIL_HOST_ABORTED": 0x89,
            "IWX_TX_STATUS_FAIL_BT_RETRY": 0x8a,
            "IWX_TX_STATUS_FAIL_STA_INVALID": 0x8b,
            "IWX_TX_STATUS_FAIL_TID_DISABLE": 0x8d,
            "IWX_TX_STATUS_FAIL_FW_DROP": 0x90,
            "IWX_TX_STATUS_FAIL_STA_COLOR_MISMATCH": 0x91,
            "IWX_TX_STATUS_INTERNAL_ABORT": 0x92,
        }
        for sym, val in expected_codes.items():
            m = re.search(rf"^#define\s+{sym}\s+0x([0-9a-fA-F]+)", reg_text, re.M)
            self.assertIsNotNone(m, f"Missing {sym}")
            self.assertEqual(int(m.group(1), 16), val)

        # Part A: Transport-level transient vs fatal failure survival & retireFailures/lastFailStatus telemetry.
        radio_src = (support.ROOT / "tests/fixtures/awdl_radio.cpp").read_text().rsplit("int main()", 1)[0]
        routines = (
            "awdlMAC", "awdlChannelAllowed", "awdlWideChannelAllowed", "awdlPHY", "awdlBinding",
            "receiveAWDLSession", "finishAWDLDwell", "serviceAWDLChannel",
            "prepareAWDLListener", "closeAWDLListenerWindow", "serviceAWDLListener",
            "configureAWDLPeerWidth", "startAWDLRadio", "awdlRemoveQueue",
            "stopAWDLRadio", "resetAWDLRadio", "awdlTxWindowOpen", "transmitAWDL",
            "completeAWDLTx", "sampleAWDLRate", "noteAWDLTXBlockAck",
            "awdlTXAntenna", "awdlTXMcs", "awdlTXWidth",
        )
        radio_src = radio_src.replace(
            "// PEER TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_transport.cpp").read_text(),
        )
        radio_src = radio_src.replace(
            "// PRODUCTION",
            "\n".join(definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::" + n) for n in routines),
        )
        radio_src = radio_src.replace(
            '#include "tests/fixtures/awdl_transport.hpp"',
            (support.ROOT / "tests/fixtures/awdl_transport.hpp").read_text(),
        )
        radio_src = radio_src.replace(
            '#include "itlwm/hal_iwx/AWDLTxBudget.hpp"',
            (support.ROOT / "itlwm/hal_iwx/AWDLTxBudget.hpp").read_text(),
        )
        transport = "\n".join(
            definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name)
            for name in (
                "iwx_rval2ridx", "iwx_toggle_tx_ant", "iwx_tx_fill_awdl_cmd", "iwx_tx_update_byte_tbl",
                "iwx_tx", "iwx_rx_tx_cmd_single", "iwx_rx_tx_cmd", "iwx_txd_done",
                "iwx_ampdu_txq_advance", "iwx_reset_tx_ring",
            )
        )
        transport += "\n" + definition("itlwm/hal_iwx/ItlIwx.cpp", "iwx_clear_tx_desc")
        transport += "\n" + definition("itlwm/hal_iwx/ItlIwx.cpp", "iwx_awdl_is_quz_0034")
        driver = (support.ROOT / "itlwm/hal_iwx/ItlIwx.cpp").read_text()
        table = re.search(r"const struct iwx_rate \{.*?\} iwx_rates\[\] = \{.*?^\};", driver, re.S | re.M)[0]
        net = (support.ROOT / "itl80211/openbsd/net80211/ieee80211.c").read_text()
        rates = re.search(r"const struct ieee80211_rateset ieee80211_std_rateset_11a =.*?;", net, re.S)[0]
        radio_src = radio_src.replace("struct ItlIwx {", table + rates + "\nstruct ItlIwx {")
        radio_src = radio_src.replace("static const uint8_t self", transport + "\nstatic const uint8_t self")
        radio_src = radio_src.replace(
            "// TRANSPORT TYPES",
            "void iwx_clear_tx_desc(struct iwx_softc *,struct iwx_tx_ring *,int);",
        )
        radio_src = radio_src.replace(
            "// FIRMWARE DECLARATIONS",
            firmware_declarations(
                (
                    "iwx_mac_data_ap", "iwx_mac_data_ibss", "iwx_mac_data_sta", "iwx_mac_data_go",
                    "iwx_mac_data_p2p_sta", "iwx_mac_data_pibss", "iwx_mac_data_p2p_dev", "iwx_ac_qos",
                    "iwx_mac_ctx_cmd", "iwx_fw_channel_info", "iwx_phy_context_cmd", "iwx_binding_cmd",
                    "iwx_rlc_properties", "iwx_sad_properties", "iwx_rlc_config_cmd",
                    "iwx_device_power_cmd", "iwx_add_sta_cmd", "iwx_rm_sta_cmd",
                    "iwx_session_prot_cmd", "iwx_session_prot_notif", "iwx_tx_data", "iwx_tfh_tb", "iwx_tfh_tfd",
                    "iwx_cmd_header", "iwx_cmd_header_wide", "iwx_device_cmd", "iwx_dram_sec_info",
                    "iwx_tx_cmd_gen2", "iwx_tx_cmd_gen3", "iwx_agn_scd_bc_tbl", "iwx_gen3_bc_tbl",
                    "iwx_agg_tx_status", "iwx_tx_resp",
                ),
                radio_src,
                ("iwx_gen2_tx_fifo",),
            ),
        )
        extra_transport = r"""
static void preview90TransientSurvival() {
    ItlIwx d;
    setupPeerTX(d);
    auto &owner = d.awdlRadio.blockAckTX;
    const uint8_t transientStatuses[] = {
        0x81, // INTERNAL_CROSSED_RETRY
        0x87, // LIFE_EXPIRE
        0x88, // DEST_PS
        0x8d, // TID_DISABLE
        0x90, // FW_DROP
        0x92, // INTERNAL_ABORT
    };
    for (uint8_t st : transientStatuses) {
        auto frame = peerTXPacket();
        assert(!d.transmitAWDL(&frame, 700 + st));
        peerTXReport(d, st);
        assert(owner.phase == AirportAWDL::TXBlockAckQueue::Agreed);
        assert(d.awdlRadio.blockAckTXStats.lastFailStatus == st);
        assert(d.awdlRadio.blockAckTXStats.retireFailures == 0);
        // Success resets consecutiveFailures so mixed transient events never kill Agreed.
        auto ok = peerTXPacket();
        assert(!d.transmitAWDL(&ok, 800 + st));
        peerTXReport(d, IWX_TX_STATUS_SUCCESS);
        assert(owner.consecutiveFailures == 0);
    }

    // 8 consecutive FW_DROP failures DO retire Agreed -> Retiring and increment retireFailures.
    for (unsigned n = 0; n < AirportAWDL::TXBlockAckQueue::MaxConsecutiveFailures; ++n) {
        auto frame = peerTXPacket();
        assert(!d.transmitAWDL(&frame, 900 + n));
        peerTXReport(d, 0x90);
        if (n + 1 < AirportAWDL::TXBlockAckQueue::MaxConsecutiveFailures)
            assert(owner.phase == AirportAWDL::TXBlockAckQueue::Agreed);
    }
    assert(owner.phase == AirportAWDL::TXBlockAckQueue::Retiring);
    assert(d.awdlRadio.blockAckTXStats.retireFailures == 1);
    finishPeerTX(d);

    // Fatal station error (0x91 STA_COLOR_MISMATCH) retires on 1st failure.
    ItlIwx fatal;
    setupPeerTX(fatal);
    auto &fo = fatal.awdlRadio.blockAckTX;
    auto ff = peerTXPacket();
    assert(!fatal.transmitAWDL(&ff, 999));
    peerTXReport(fatal, 0x91);
    assert(fo.phase == AirportAWDL::TXBlockAckQueue::Retiring);
    assert(fatal.awdlRadio.blockAckTXStats.lastFailStatus == 0x91);
    assert(fatal.awdlRadio.blockAckTXStats.retireFailures == 1);
    finishPeerTX(fatal);
}
int main() {
    preview90TransientSurvival();
    return 0;
}
"""
        self.compile_run(radio_src + extra_transport)

        # Part B: Paused-queue stall watchdog exemption + unpause lastProgressUS reset + retireDelba/retireErrors.
        ba_src = (support.ROOT / "tests/fixtures/awdl_ba.cpp").read_text().rsplit("int main()", 1)[0]
        ba_src += (support.ROOT / "tests/fixtures/awdl_tx_live.cpp").read_text().rsplit("int main()", 1)[0]
        hal = (
            "addAWDLBlockAckPeer", "awdlHasRXBA", "awdlRXBAOwned", "awdlBlockAckLive",
            "removeAWDLBlockAckTID", "removeAWDLBlockAckPeer", "clearAWDLBlockAck",
            "resetAWDLBlockAck", "prepareAWDLBlockAck", "receiveAWDLBlockAckEnd",
            "requestAWDLBlockAckEnd", "completeAWDLBlockAckEnd", "serviceAWDLBlockAck",
            "deliverAWDLReordered", "receiveAWDLReordered", "noteAWDLTXBlockAck",
            "receiveAWDLTXBlockAck", "serviceAWDLTXBlockAck", "configureAWDLTXStation",
            "awdlTXAntenna", "awdlTXMcs", "awdlTXWidth", "configureAWDLTXRates", "setAWDLTXWindow",
            "completeAWDLTXManagement", "closeAWDLListenerWindow", "configureAWDLPeerWidth",
        )
        rx = (
            "iwx_init_reorder_buffer", "iwx_clear_reorder_buffer", "iwx_is_sn_less",
            "iwx_release_frames", "iwx_oldsn_workaround", "iwx_rx_reorder",
            "iwx_reorder_timer_expired", "iwx_sta_rx_agg",
        )
        ba_src = ba_src.replace(
            "// PRODUCTION",
            "\n".join(definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::" + name) for name in hal)
            + "\n"
            + "\n".join(definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name) for name in rx),
        )
        ba_src = ba_src.replace(
            "// FIRMWARE",
            firmware_declarations(
                (
                    "iwx_rx_mpdu_desc_v1", "iwx_rx_mpdu_desc_v3", "iwx_rx_mpdu_desc",
                    "iwx_add_sta_cmd", "iwx_rm_sta_cmd", "iwx_reorder_buffer",
                    "iwx_reorder_buf_entry", "iwx_rxba_data", "iwx_tlc_config_cmd_v4",
                    "iwl_tlc_config_cmd_v3",
                ),
                ba_src,
                ("iwx_tlc_mng_cfg_mode", "iwx_tlc_mng_cfg_cw"),
            ),
        )
        ba_src = ba_src.replace(
            "// FROM REORDER BUFFER",
            definition("itlwm/hal_iwx/if_iwxvar.h", "iwx_rxba_data_from_reorder_buf"),
        )
        ba_src += r"""
int main() {
    // 1. Paused TXBA queue with queued frames does NOT trip the 2-second stall watchdog,
    //    and unpausing resets lastProgressUS so immediate serviceAWDLTXBlockAck stays Agreed!
    ItlIwx d;
    auto r = txPeer(d);
    prepareTX(d, r);
    agreeTX(d);
    auto &tx = d.awdlRadio.blockAckTX;
    assert(tx.paused);
    Packet pkt;
    d.com.txq[tx.queue].queued = 1;
    d.com.txq[tx.queue].data[0].m = &pkt;

    // Advance 3 seconds while paused (refreshing peer so wanted() stays true).
    nowUS += 3000000;
    d.awdlPeers.peers[0].lastSeenUS = nowUS;
    d.awdlPeers.peers[0].htCapabilitiesReceivedUS = nowUS;
    d.serviceAWDLTXBlockAck();
    assert(tx.phase == TXBlockAckQueue::Agreed);
    assert(d.awdlRadio.blockAckTXStats.retireStalls == 0);

    // Unpause via setAWDLTXWindow(): resets lastProgressUS to nowUS -> serviceAWDLTXBlockAck stays Agreed!
    uint64_t end = nowUS + 60000;
    assert(!d.setAWDLTXWindow(grant, &end));
    assert(!tx.paused && tx.lastProgressUS == nowUS);
    nowUS += 1000;
    d.serviceAWDLTXBlockAck();
    assert(tx.phase == TXBlockAckQueue::Agreed && d.awdlRadio.blockAckTXStats.retireStalls == 0);

    // Genuine 2.0 s unpaused stall DOES retire and increment retireStalls!
    nowUS += 2000000;
    d.awdlPeers.peers[0].lastSeenUS = nowUS;
    d.awdlPeers.peers[0].htCapabilitiesReceivedUS = nowUS;
    d.serviceAWDLTXBlockAck();
    assert(tx.phase == TXBlockAckQueue::Retiring && d.awdlRadio.blockAckTXStats.retireStalls == 1);
    d.com.txq[tx.queue].queued = 0;
    d.com.txq[tx.queue].data[0].m = nullptr;
    assert(!d.clearAWDLBlockAck());

    // 2. Remote DELBA increments retireDelba.
    ItlIwx d2;
    auto r2 = txPeer(d2);
    prepareTX(d2, r2);
    agreeTX(d2);
    BlockAckRequest delba = {};
    copyAddress(delba.peer, d2.awdlRadio.address);
    delba.tid = 0;
    uint8_t frame[30];
    blockAckTeardown(frame, r2.peer, delba, 0);
    assert(d2.receiveAWDLTXBlockAck(frame, sizeof(frame)));
    assert(d2.awdlRadio.blockAckTX.phase == TXBlockAckQueue::Retiring);
    assert(d2.awdlRadio.blockAckTXStats.retireDelba == 1);
    assert(!d2.clearAWDLBlockAck());
    return 0;
}
"""
        self.compile_run(ba_src)

        # Part C: Verify all 5 new telemetry keys are published to ioreg.
        awdl_if = (support.ROOT / "AirportItlwm/AirportItlwmAWDLInterface.cpp").read_text()
        for key in (
            '"TXBlockAckRetireStalls"',
            '"TXBlockAckRetireFailures"',
            '"TXBlockAckRetireDelba"',
            '"TXBlockAckRetireErrors"',
            '"TXBlockAckLastFailStatus"',
        ):
            self.assertIn(key, awdl_if)


if __name__ == "__main__":
    unittest.main()
