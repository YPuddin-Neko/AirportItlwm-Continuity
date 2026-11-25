"""Failing-first regressions for Preview 97:
1. Sleep/wake PCIe D0 + Command + MSI restoration, deferred child enable() while pmPowerState is
   kPowerStateOff, synchronous firmware bringup in setPowerStateOn() prior to acknowledgeSetPowerState(),
   ItlIwx::enable() IFF_UP rollback on hardware-not-ready, and controllerReinit AWDL link preservation
   across transient iwx_init_task reloads.
2. OOB MIF (afTxMode=3) HT capability freshness continuity and stable 80 MHz Sidecar awdlTXWidth across
   operatingClass(149)=124 multi-slot schedules so Intel QuZ TLC never resets to 26 Mbps on every MIF.
3. Active nativeCached peer IWX_STA_LINK + RXBA retention across >10 s broadcast AF gaps, plus
   AirportItlwmSkywalkInterface setSCAN_CONTROL / setABORT_SCAN support for RealTimeMode transitions.
4. Unicast IPv6 UDP 319/320 (IOTimeSync gPTP) announcement-hold bypass, 1500 us contiguous listener
   guard, and single-MPDU non-aggregation on awdl0.
"""
import unittest

import test_awdl_preview82 as p82
import test_awdl_preview91 as p91
import test_regressions as support
from test_offline import definition


class Preview97Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p97_sleep_wake_pcie_msi_power_sequencing_and_controller_reinit_awdl_pause(self):
        # Part A: Verify AirportItlwm::restorePCIPowerState, enable(), setPowerStateOff(), setPowerStateOn(),
        # and ItlIwx::enable() / iwx_activate() across S3 sleep/wake and hardware-not-ready faults.
        pm_fns = "\n".join(
            definition("AirportItlwm/AirportItlwmV2.cpp", "AirportItlwm::" + name)
            for name in ("restorePCIPowerState", "enable", "enableAdapter", "disableAdapter", "setPowerStateOff", "setPowerStateOn")
        )
        iwx_fns = "\n".join(
            definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name)
            for name in ("enable", "disable", "resumeAdapter", "iwx_activate")
        )
        src = f"""
#include <cassert>
#include <cstdint>
#include <cstring>

using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using IOReturn = int;

enum {{
    kIOReturnSuccess = 0,
    kIOReturnError = -1,
    kIOReturnNotReady = -2,
    kPowerStateOff = 0,
    kPowerStateOn = 1,
    kWatchDogTimerPeriod = 1000,
    kIONetworkLinkValid = 1,
    kIOPCIConfigCommand = 0x04,
    kIOPCICommandIOSpace = 0x0001,
    kIOPCICommandMemorySpace = 0x0002,
    kIOPCICommandBusMaster = 0x0004,
    kIOPCICommandMemWrInvalidate = 0x0010,
    kIOPCICommandInterruptDisable = 0x0400,
    kIOPCIPowerManagementCapability = 0x01,
    PCI_CAP_ID_MSI = 0x05,
    PCI_CAP_ID_MSIX = 0x11,
    PCI_MSI_FLAGS = 2,
    PCI_MSIX_FLAGS = 2,
    PCI_MSI_FLAGS_ENABLE = 0x0001,
    PCI_MSIX_FLAGS_ENABLE = 0x8000,
    IFF_UP = 0x1,
    IFF_RUNNING = 0x40,
    DVACT_QUIESCE = 1,
    DVACT_RESUME = 2,
    DVACT_WAKEUP = 3,
}};

#define XYLog(...) do {{}} while (0)
#define DEVNAME(sc) "iwx0"
static void IOSleep(unsigned) {{}}

struct IOPCIDevice {{
    uint8_t cfg[256] = {{}};
    UInt8 pmCap = 0x50;
    UInt8 msiCap = 0x60;
    UInt8 msixCap = 0x70;
    UInt16 configRead16(unsigned off) const {{
        return UInt16(cfg[off] | (UInt16(cfg[off + 1]) << 8));
    }}
    void configWrite16(unsigned off, UInt16 val) {{
        cfg[off] = UInt8(val & 0xff);
        cfg[off + 1] = UInt8(val >> 8);
    }}
    void configWrite8(unsigned off, UInt8 val) {{ cfg[off] = val; }}
    void findPCICapability(UInt8 id, UInt8 *out) const {{
        if (id == kIOPCIPowerManagementCapability) *out = pmCap;
        else if (id == PCI_CAP_ID_MSI) *out = msiCap;
        else if (id == PCI_CAP_ID_MSIX) *out = msixCap;
        else *out = 0;
    }}
}};

struct IONetworkInterface {{}};
struct IO80211SkywalkInterface : IONetworkInterface {{}};
struct AirportItlwmEthernetInterface : IONetworkInterface {{}};

struct MockGate {{
    bool enabled = false;
    void enable() {{ enabled = true; }}
}} gateObj, *_fCommandGate = &gateObj;

struct MockTimer {{
    unsigned timeoutMS = 0;
    bool enabled = false;
    void setTimeoutMS(unsigned ms) {{ timeoutMS = ms; }}
    void enable() {{ enabled = true; }}
    void cancelTimeout() {{ timeoutMS = 0; }}
    void disable() {{ enabled = false; }}
}};

struct task {{ bool queued = false; }};
static void *systq = nullptr;
static void task_add(void *, task *t) {{ t->queued = true; }}
static bool task_del(void *, task *t) {{ const bool was = t->queued; t->queued = false; return was; }}

struct _ifnet {{ unsigned if_flags = 0; }};
struct iwx_softc {{
    struct {{
        _ifnet ic_if;
        struct {{ _ifnet ac_if; }} ic_ac;
    }} sc_ic;
    task init_task;
}};

struct ItlHalService {{
    virtual ~ItlHalService() = default;
    virtual IOReturn enable(IONetworkInterface *) = 0;
    virtual IOReturn disable(IONetworkInterface *) = 0;
    virtual void resumeAdapter() {{}}
}};

struct ItlIwx;
static ItlIwx *gActiveIwx = nullptr;

struct ItlIwx : ItlHalService {{
    iwx_softc com = {{}};
    int resumeErr = 0;
    int hwReady = 1;
    unsigned stops = 0, resumes = 0, initRuns = 0;
    bool runningAtAck = false;

    void iwx_stop(_ifnet *ifp) {{
        ++stops;
        ifp->if_flags &= ~IFF_RUNNING;
        com.sc_ic.ic_ac.ac_if.if_flags &= ~IFF_RUNNING;
    }}
    int iwx_resume(iwx_softc *) {{
        ++resumes;
        return resumeErr;
    }}
    int iwx_set_hw_ready(iwx_softc *) {{ return hwReady; }}
    static void iwx_init_task(void *arg) {{
        auto *sc = static_cast<iwx_softc *>(arg);
        sc->ic_if_mark();
    }}
    IOReturn enable(IONetworkInterface *) override;
    IOReturn disable(IONetworkInterface *) override;
    void resumeAdapter() override;
    int iwx_activate(iwx_softc *, int);
}};

struct PolicyMaker {{
    unsigned acks = 0;
    bool runningWhenAcked = false;
    UInt16 pmcsrWhenAcked = 0;
    UInt16 cmdWhenAcked = 0;
    UInt16 msiWhenAcked = 0;
    UInt16 msixWhenAcked = 0;
    IOPCIDevice *pci = nullptr;
    void acknowledgeSetPowerState() {{
        ++acks;
        if (gActiveIwx)
            runningWhenAcked = (gActiveIwx->com.sc_ic.ic_if.if_flags & IFF_RUNNING) != 0;
        if (pci) {{
            cmdWhenAcked = pci->configRead16(kIOPCIConfigCommand);
            pmcsrWhenAcked = pci->configRead16(pci->pmCap + 4);
            msiWhenAcked = pci->configRead16(pci->msiCap + PCI_MSI_FLAGS);
            msixWhenAcked = pci->configRead16(pci->msixCap + PCI_MSIX_FLAGS);
        }}
    }}
}};

struct SuperController {{
    unsigned superEnables = 0;
    IOReturn enable(IO80211SkywalkInterface *) {{ ++superEnables; return kIOReturnSuccess; }}
}};

struct AirportItlwm : SuperController {{
    using super = SuperController;
    IOPCIDevice *pciNub = nullptr;
    UInt8 pmPCICapPtr = 0x50;
    UInt32 pmPowerState = kPowerStateOn;
    uint8_t power_state = 1;
    PolicyMaker *pmPolicyMaker = nullptr;
    AirportItlwmEthernetInterface *bsdInterface = nullptr;
    ItlHalService *fHalService = nullptr;
    MockTimer *watchdogTimer = nullptr;

    void restorePCIPowerState(IOPCIDevice *provider);
    IOReturn enable(IO80211SkywalkInterface *netif);
    IOReturn enableAdapter(IONetworkInterface *netif);
    void disableAdapter(IONetworkInterface *netif);
    void setPowerStateOff();
    void setPowerStateOn();
}};
"""
        # Replace sc->ic_if_mark() placeholder by wiring iwx_init_task to mark IFF_RUNNING on gActiveIwx
        src = src.replace(
            "sc->ic_if_mark();",
            "if (gActiveIwx) { ++gActiveIwx->initRuns; "
            "sc->sc_ic.ic_if.if_flags |= IFF_RUNNING; sc->sc_ic.ic_ac.ac_if.if_flags |= IFF_RUNNING; }",
        )
        # Unify ic_if and ic_ac.ac_if sync inside ItlIwx::enable/disable test harness
        src += "\n" + pm_fns + "\n" + iwx_fns + r"""

int main() {
    IOPCIDevice pci = {};
    PolicyMaker pm = {};
    pm.pci = &pci;
    MockTimer watchdog = {};
    AirportItlwmEthernetInterface bsd = {};
    IO80211SkywalkInterface en1 = {}, awdl0 = {}, llw0 = {};
    ItlIwx hal = {};
    gActiveIwx = &hal;

    AirportItlwm ctrl = {};
    ctrl.pciNub = &pci;
    ctrl.pmPolicyMaker = &pm;
    ctrl.bsdInterface = &bsd;
    ctrl.fHalService = &hal;
    ctrl.watchdogTimer = &watchdog;

    // 1. Hardware not ready during enable() MUST roll back IFF_UP instead of wedging future enables!
    hal.hwReady = 0;
    assert(hal.enable(&bsd) != kIOReturnSuccess);
    assert(!(hal.com.sc_ic.ic_ac.ac_if.if_flags & IFF_UP));
    hal.hwReady = 1;
    hal.resumeErr = 5;
    assert(hal.enable(&bsd) != kIOReturnSuccess);
    assert(!(hal.com.sc_ic.ic_ac.ac_if.if_flags & IFF_UP));
    hal.resumeErr = 0;

    // Normal boot enable brings up IFF_UP and schedules init_task.
    assert(ctrl.enable(&en1) == kIOReturnSuccess);
    assert(hal.com.sc_ic.ic_ac.ac_if.if_flags & IFF_UP);
    hal.com.sc_ic.ic_if.if_flags = hal.com.sc_ic.ic_ac.ac_if.if_flags;
    assert(hal.com.init_task.queued);
    task_del(systq, &hal.com.init_task);
    ItlIwx::iwx_init_task(&hal.com);
    assert(hal.com.sc_ic.ic_if.if_flags & IFF_RUNNING);

    // 2. Enter S3 sleep: setPowerStateOff() disables adapter; BIOS leaves card in D3 with MSI-X on & MSI off.
    ctrl.setPowerStateOff();
    assert(ctrl.pmPowerState == kPowerStateOff && pm.acks == 1);
    assert(!(hal.com.sc_ic.ic_ac.ac_if.if_flags & IFF_UP));
    pci.configWrite16(kIOPCIConfigCommand, kIOPCICommandIOSpace | kIOPCICommandInterruptDisable);
    pci.configWrite16(pci.pmCap + 4, 0x0003); // D3hot
    pci.configWrite16(pci.msixCap + PCI_MSIX_FLAGS, PCI_MSIX_FLAGS_ENABLE);
    pci.configWrite16(pci.msiCap + PCI_MSI_FLAGS, 0x0000);

    // 3. On wake, all 3 Skywalk child interfaces call enable() BEFORE setPowerStateOn() runs!
    //    AirportItlwm::enable() MUST defer enableAdapter() while pmPowerState == kPowerStateOff.
    const unsigned resumesBeforeWake = hal.resumes;
    assert(ctrl.enable(&en1) == kIOReturnSuccess);
    assert(ctrl.enable(&awdl0) == kIOReturnSuccess);
    assert(ctrl.enable(&llw0) == kIOReturnSuccess);
    assert(hal.resumes == resumesBeforeWake);
    assert(!(hal.com.sc_ic.ic_ac.ac_if.if_flags & IFF_UP));

    // 4. setPowerStateOn() MUST restore D0 + Command + MSI, bring up firmware synchronously,
    //    and ONLY THEN acknowledgeSetPowerState() with IFF_RUNNING already active!
    hal.iwx_resume(&hal.com); // reset counter baseline
    const unsigned rBase = hal.resumes;
    const unsigned initBase = hal.initRuns;
    ctrl.setPowerStateOn();
    assert(ctrl.pmPowerState == kPowerStateOn && pm.acks == 2);
    assert(hal.resumes == rBase + 1);
    assert(hal.initRuns == initBase + 1);
    assert(!hal.com.init_task.queued);
    assert(pm.runningWhenAcked);
    assert(pm.pmcsrWhenAcked == 0x8000);
    assert((pm.cmdWhenAcked & (kIOPCICommandBusMaster | kIOPCICommandMemorySpace | kIOPCICommandMemWrInvalidate)) ==
           (kIOPCICommandBusMaster | kIOPCICommandMemorySpace | kIOPCICommandMemWrInvalidate));
    assert(!(pm.cmdWhenAcked & (kIOPCICommandIOSpace | kIOPCICommandInterruptDisable)));
    assert(!(pm.msixWhenAcked & PCI_MSIX_FLAGS_ENABLE));
    assert(pm.msiWhenAcked & PCI_MSI_FLAGS_ENABLE);
    assert(watchdog.enabled);
    return 0;
}
"""
        self.compile_run(src)

        # Part B: Transient controller reload (awdlRadio.controllerReinit == true while IFF_RUNNING == 0)
        # MUST keep infrastructurePaused == true & nativeUp == true instead of triggering a 47 s awdl0 blackout!
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines())
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src += r"""
int main() {
    clockUS = 10000000;
    AirportItlwmAWDLInterface i;
    i.onLinkDown = [&] { assert(!i.setEnabled(false)); };
    i.tick();
    assert(i.running && i.requested && i.nativeUp);

    // Simulate iwx_init_task controller reload: IFF_UP stays set, IFF_RUNNING drops for ~180 ms,
    // controllerReinit is true while firmware reloads.
    i.device.awdlRadio.controllerReinit = true;
    i.device.com.sc_ic.ic_ac.ac_if.if_flags &= ~IFF_RUNNING;
    i.device.com.ns_nstate = 0;
    i.device.stopAWDLRadio();
    clockUS += 100000;
    i.tick();
    assert(!i.running && i.infrastructurePaused && i.requested && i.nativeUp);

    // Firmware reload completes (IFF_RUNNING restored, controllerReinit cleared) and STA reassociates:
    // awdl0 resumes immediately without ever having dropped nativeUp or requested!
    i.device.com.sc_ic.ic_ac.ac_if.if_flags |= IFF_RUNNING;
    i.device.awdlRadio.controllerReinit = false;
    i.device.com.ns_nstate = IEEE80211_S_RUN;
    clockUS += 1000000;
    i.tick();
    assert(i.running && i.requested && i.nativeUp && !i.infrastructurePaused);

    // Unrecovered stop (controllerReinit == false && IFF_RUNNING == 0) MUST still close native link!
    i.device.com.sc_ic.ic_ac.ac_if.if_flags &= ~IFF_RUNNING;
    i.device.stopAWDLRadio();
    clockUS += 1000000;
    i.tick();
    assert(!i.running && !i.requested && !i.nativeUp);
    return 0;
}
"""
        self.compile_run(tick_src)

    def test_p97_oob_mif_ht_capability_continuity_and_stable_80mhz_tx_width(self):
        # Part A: PeerTable::update refreshes htCapabilitiesReceivedUS on valid OOB MIF (subtype == 3 with Sync|Channels)
        # while preserving ScheduleFreshnessUS expiry for Version-only frames and LifetimeUS expiry.
        peer_src = r"""
#include <cassert>
#include <cstdint>
#include "AirportItlwm/AWDL/PeerState.hpp"

using namespace AirportAWDL;

int main() {
    const uint8_t self[6] = {2, 0, 0, 0, 0, 1};
    const uint8_t remote[6] = {2, 0, 0, 0, 0, 2};
    PeerTable table = {};

    Advertisement psf = {};
    psf.subtype = 0;
    psf.fields = Sync | Channels | HTCapabilities | Version;
    psf.rxHTMcs = 0xff;
    psf.rxHTMcs2 = 0xff;
    psf.htCapabilities = HTCapLDPC | HTCapSGI20 | HTCapSGI40;
    psf.presenceMode = 4;
    psf.windowPeriodTU = 16;
    psf.nextWindowTU = 64;
    psf.sequence.count = 16;
    psf.sequence.encoding = 3;
    psf.sequence.step = 3;
    psf.sequence.fill = 0xffff;
    for (unsigned s = 0; s < 16; ++s) { psf.sequence.channel[s] = 149; psf.sequence.flags[s] = 128; }

    uint64_t t = 1000000;
    assert(table.update(remote, self, psf, t, 6, -40));
    auto *p = table.find(remote, t);
    assert(p && p->txHTMcs(t) == 0xff && p->txHTCap(t) == psf.htCapabilities);

    // Simulate 8 seconds of afTxMode=3 (Sidecar / Continuity Camera):
    // 5 GHz PSF is suppressed; peer sends OOB MIFs (subtype=3, Sync|Channels, NO HTCapabilities TLV) every 250 ms.
    Advertisement mif = {};
    mif.subtype = 3;
    mif.fields = Sync | Channels;
    mif.presenceMode = 4;
    mif.windowPeriodTU = 16;
    mif.nextWindowTU = 64;
    mif.sequence = psf.sequence;
    for (unsigned step = 0; step < 32; ++step) {
        t += 250000;
        mif.phyTxTime = uint32_t(t);
        assert(table.update(remote, self, mif, t, 149, -45));
        assert(p->txHTMcs(t) == 0xff);
        assert(p->txHTMcs2(t) == 0xff);
        assert(p->txHTCap(t) == psf.htCapabilities);
    }

    // Non-MIF Version-only frame MUST NOT extend htCapabilitiesReceivedUS past ScheduleFreshnessUS!
    PeerTable table2 = {};
    assert(table2.update(remote, self, psf, 1000, 6, -40));
    auto *p2 = table2.find(remote, 1000);
    Advertisement verOnly = {};
    verOnly.subtype = 0;
    verOnly.fields = Version;
    verOnly.phyTxTime = 2000;
    assert(table2.update(remote, self, verOnly, 2000, 6, -40));
    assert(p2->txHTMcs(1000 + Peer::ScheduleFreshnessUS - 1) == 0xff);
    assert(p2->txHTMcs(1000 + Peer::ScheduleFreshnessUS) == 0);
    return 0;
}
"""
        self.compile_run(peer_src)

        # Part B: awdlTXWidth + setAWDLTXWindow keep 80 MHz TLC stable when a wideband-capable peer
        # (HTCapSGI40) alternates between flags=128 (PSF) and flags=124 (operatingClass(149) in OOB MIF)
        # on a multi-slot 149 run, while keeping 20 MHz for non-SGI40 peers and isolated 1-slot RTG schedules!
        extra_ba = r"""
int main() {
    ItlIwx d;
    auto r = txPeer(d);
    d.tlcVersion = 4;
    d.awdlRadio.channel = 149;
    d.awdlRadio.widthMHz = 80;
    d.mimo = true; d.txAnt = 3; d.rxAnt = 3;
    auto &p = d.awdlPeers.peers[0];
    p.metadata.fields |= AirportAWDL::HTCapabilities;
    p.metadata.rxHTMcs = 0xff;
    p.metadata.rxHTMcs2 = 0xff;
    p.metadata.htCapabilities = AirportAWDL::HTCapLDPC | AirportAWDL::HTCapSGI20 | AirportAWDL::HTCapSGI40;
    p.htCapabilitiesReceivedUS = nowUS;
    p.haveSync = p.haveSequence = true;
    p.syncReceivedUS = p.sequenceReceivedUS = nowUS;
    p.sync.presenceMode = 4; p.sync.windowPeriodTU = 16; p.sync.nextWindowTU = 64;
    p.sequence.count = 16; p.sequence.fill = 0xffff; p.sequence.step = 3; p.sequence.encoding = 3;

    const uint8_t sidecarCh[16] = {48,48,149,149,149,149,149,149,6,149,149,149,149,149,149,149};
    for (unsigned s = 0; s < 16; ++s) {
        p.sequence.channel[s] = sidecarCh[s];
        p.sequence.flags[s] = sidecarCh[s] == 149 ? 128 : 0;
    }
    nowUS += 3ULL * 65536; // Inside multi-slot 149 run (Slot 3)
    p.lastSeenUS = p.syncReceivedUS = p.sequenceReceivedUS = p.htCapabilitiesReceivedUS = nowUS;
    p.sync.windowCounter = 12;

    prepareTX(d, r);
    agreeTX(d);
    uint64_t end = nowUS + 30000;
    assert(!d.setAWDLTXWindow(grant, &end));
    assert(d.awdlRadio.blockAckTX.widthMHz == 80 && !d.awdlRadio.blockAckTX.paused);
    const uint64_t tlcInitial = d.awdlRadio.blockAckTXStats.tlcConfigs;
    assert(tlcInitial == 1);

    // Peer sends OOB MIF advertising standard 5 GHz operating class 124 on multi-slot 149 run:
    // awdlTXWidth MUST remain 80 MHz with ZERO TLC resets!
    for (unsigned s = 0; s < 16; ++s)
        p.sequence.flags[s] = sidecarCh[s] == 149 ? 124 : (sidecarCh[s] == 48 ? 115 : 81);
    assert(d.awdlTXWidth(nowUS) == 80);
    assert(!d.setAWDLTXWindow(grant, &end));
    assert(d.awdlRadio.blockAckTX.widthMHz == 80 && !d.awdlRadio.blockAckTX.paused);
    assert(d.awdlRadio.blockAckTXStats.tlcConfigs == tlcInitial);

    // Isolated 1-slot 149 RealTimeGroup schedule [48,48,149,48x7,149,48x5] with class 124 MUST stay 20 MHz!
    const uint8_t rtgCh[16] = {48,48,149,48,48,48,48,48,48,48,149,48,48,48,48,48};
    for (unsigned s = 0; s < 16; ++s) {
        p.sequence.channel[s] = rtgCh[s];
        p.sequence.flags[s] = rtgCh[s] == 149 ? 124 : 115;
    }
    p.sync.windowCounter = 8; // Slot 2 (isolated ch 149)
    p.syncReceivedUS = p.sequenceReceivedUS = nowUS;
    assert(d.awdlTXWidth(nowUS) == 20);

    // 20 MHz-only peer (no HTCapSGI40) with class 124 on multi-slot 149 MUST also stay 20 MHz!
    for (unsigned s = 0; s < 16; ++s) {
        p.sequence.channel[s] = sidecarCh[s];
        p.sequence.flags[s] = sidecarCh[s] == 149 ? 124 : 115;
    }
    p.sync.windowCounter = 12;
    p.metadata.htCapabilities = AirportAWDL::HTCapSGI20;
    assert(d.awdlTXWidth(nowUS) == 20);

    assert(!d.clearAWDLBlockAck());
    return 0;
}
"""
        self.compile_run(p91._build_ba_harness(extra_ba))

    def test_p97_cached_peer_rxba_retention_across_af_gap_and_scan_control_support(self):
        # Part A: nativeCached Continuity peer retains IWX_STA_LINK + active RXBA across >10s AF gap!
        extra_ba = r"""
int main() {
    ItlIwx d;
    d.awdlRadio.listener = true;
    d.awdlRadio.binding = true;
    d.awdlRadio.channel = 48;
    d.awdlRadio.widthMHz = 20;

    auto r0 = request(d, 0, 7, 100);
    d.awdlPeers.peers[0].nativeCached = true;
    start(d, r0);
    assert(d.awdlBA.peers[0].station && d.awdlHasRXBA() && d.com.sc_rx_ba_sessions == 1);
    const size_t removesBefore = d.removes.size();

    // Advance past 10 s PeerTable::LifetimeUS without broadcast AFs while Continuity Camera streams:
    // PeerTable::expire() clears occupied=false while preserving nativeCached=true.
    nowUS += AirportAWDL::PeerTable::LifetimeUS + 500000;
    d.awdlPeers.expire(nowUS);
    assert(!d.awdlPeers.peers[0].occupied && d.awdlPeers.peers[0].nativeCached);

    // serviceAWDLBlockAck MUST retain both IWX_STA_LINK and the active TID 7 RXBA session!
    d.serviceAWDLBlockAck();
    assert(d.awdlBA.peers[0].station);
    assert(d.awdlBA.peers[0].sessions[7].firmware);
    assert(d.com.sc_rx_ba_sessions == 1);
    assert(d.removes.size() == removesBefore);

    // Once nativeCached is cleared by IO80211AWDLPeerManager, expired peer station + RXBA clean up!
    d.awdlPeers.peers[0].nativeCached = false;
    d.serviceAWDLBlockAck();
    assert(!d.awdlBA.peers[0].station && d.com.sc_rx_ba_sessions == 0);
    return 0;
}
"""
        self.compile_run(p91._build_ba_harness(extra_ba))

        # Part B: AirportItlwmSkywalkInterface::setSCAN_CONTROL and setABORT_SCAN succeed and abort
        # in-flight associated WCL scans during Continuity RealTimeMode transitions.
        wcl_src = (support.ROOT / "tests/fixtures/wcl_lifecycle.cpp").read_text().rsplit("int main()", 1)[0]
        scan_fns = "\n".join(
            definition("AirportItlwm/WCL/WCL.cpp", "AirportItlwmSkywalkInterface::" + name)
            for name in (
                "beginWCLScan",
                "completeWCLScanFromCache",
                "startWCLScan",
                "resumeWCLScan",
                "finishWCLScan",
                "completeWCLScan",
                "resumeWCLJoin",
                "handleWCLAvailability",
                "setSCAN_CONTROL",
                "setABORT_SCAN",
            )
        )
        wcl_src = wcl_src.replace(
            "struct apple80211ScanRequest { uint8_t bytes[0x1550] = {}; };",
            "struct apple80211ScanRequest { uint8_t bytes[0x1550] = {}; };\n"
            "struct apple80211_scan_control_params { uint32_t version; };\n"
            "struct apple80211_abort_scan { uint32_t version; };",
        )
        wcl_src = wcl_src.replace(
            "struct Instance {",
            "struct MockAWDL { bool active = false; bool continuityMediaActive() const { return active; } };\n"
            "struct Instance {\n"
            "    MockAWDL awdl;\n"
            "    MockAWDL *awdlInterface = &awdl;\n"
            "    unsigned scanResults = 0;",
        )
        wcl_src = wcl_src.replace(
            "messages.push_back({code, *static_cast<int *>(data)});",
            "if (code == AirportWCL::ScanResult) { ++scanResults; return; }\n"
            "        messages.push_back({code, *static_cast<int *>(data)});",
        )
        wcl_src = wcl_src.replace(
            "bool scanPending = false,",
            "bool wclBssValid = false;\n"
            "    uint8_t wclBssInfo[sizeof(AirportWCL::BeaconMetadata) + 0x800] = {};\n"
            "    bool scanPending = false,",
        )
        wcl_src = wcl_src.replace(
            "bool completeWCLScanFromCache(const AirportWCL::ScanRequest &) { return false; }",
            "bool completeWCLScanFromCache(const AirportWCL::ScanRequest &);\n"
            "    IOReturn setSCAN_CONTROL(apple80211_scan_control_params *);\n"
            "    IOReturn setABORT_SCAN(apple80211_abort_scan *);",
        )
        wcl_src = wcl_src.replace("// PRODUCTION: scan\n", scan_fns + "\n")
        wcl_src += r"""
int main() {
    apple80211ScanRequest req = {};
    req.bytes[0x40] = 1;
    apple80211_scan_control_params ctrl = {1};
    apple80211_abort_scan abortReq = {1};

    AirportItlwmSkywalkInterface s;
    s.portAuthorized = true;
    s.owner.awdl.active = false;
    assert(s.beginWCLScan(&req) == kIOReturnSuccess);
    assert(s.scanPending && (s.hal.ic.ic_flags & IEEE80211_F_BGSCAN));

    // RealTimeMode activates on Continuity Camera start -> notifyRealTimeModeOnOff calls setSCAN_CONTROL:
    s.owner.awdl.active = true;
    assert(s.setSCAN_CONTROL(nullptr) == kIOReturnBadArgument);
    assert(s.setSCAN_CONTROL(&ctrl) == kIOReturnSuccess);
    assert(!s.scanPending && s.owner.scans() == 1);

    // Explicit setABORT_SCAN also aborts any in-flight associated scan cleanly:
    s.owner.awdl.active = false;
    assert(s.beginWCLScan(&req) == kIOReturnSuccess);
    assert(s.scanPending);
    assert(s.setABORT_SCAN(&abortReq) == kIOReturnSuccess);
    assert(!s.scanPending && s.owner.scans() == 2);
    return 0;
}
"""
        self.compile_run(wcl_src)

    def test_p97_awdl_unicast_gptp_bypasses_announcement_hold_and_uses_1500us_guard(self):
        tick_src = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().rsplit("int main()", 1)[0]
        tick_src = tick_src.replace(
            "// NATIVE TX SCENARIOS",
            (support.ROOT / "tests/fixtures/awdl_tx_native.cpp").read_text(),
        )
        extra_routines = definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp",
            "AirportItlwmAWDLInterface::drainPendingData",
        )
        tick_src = tick_src.replace("// PRODUCTION", p82._native_tick_routines() + "\n" + extra_routines)
        tick_src = tick_src.replace("// NATIVE DECLARATIONS", p82._native_tick_decls())
        tick_src += r"""
static mbuf_t udpIPv6Ethernet(const uint8_t *src, const uint8_t *dst, uint16_t dstPort, size_t bodyLen = 44) {
    const size_t udpLen = 8 + bodyLen;
    const size_t total = 14 + 40 + udpLen;
    auto *p = awdlPacket(total);
    assert(p);
    uint8_t *b = p->bytes.data();
    std::memset(b, 0, total);
    copyAddress(b, dst);
    copyAddress(b + 6, src);
    b[12] = 0x86; b[13] = 0xdd;
    uint8_t *ip = b + 14;
    ip[0] = 0x60;
    ip[4] = uint8_t(udpLen >> 8); ip[5] = uint8_t(udpLen & 0xff);
    ip[6] = 17; // UDP
    ip[7] = 255;
    ip[8] = 0xfe; ip[9] = 0x80; ip[23] = 1;
    ip[24] = 0xfe; ip[25] = 0x80; ip[39] = 2;
    uint8_t *udp = ip + 40;
    udp[0] = uint8_t(dstPort >> 8); udp[1] = uint8_t(dstPort & 0xff);
    udp[2] = uint8_t(dstPort >> 8); udp[3] = uint8_t(dstPort & 0xff);
    udp[4] = uint8_t(udpLen >> 8); udp[5] = uint8_t(udpLen & 0xff);
    return p;
}

int main() {
    const uint8_t selfAddr[6] = {2, 0, 0, 0, 0, 1};
    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 9};
    const uint8_t mcastAddr[6] = {0x33, 0x33, 0, 0, 0, 0xfb};

    // 1. Unit-verify isUnicastPTP on UDP 319/320 vs non-PTP UDP and multicast:
    {
        auto *p319 = udpIPv6Ethernet(selfAddr, peerAddr, 319);
        auto *p320 = udpIPv6Ethernet(selfAddr, peerAddr, 320);
        auto *p5000 = udpIPv6Ethernet(selfAddr, peerAddr, 5000);
        auto *pMcast = udpIPv6Ethernet(selfAddr, mcastAddr, 319);
        assert(AirportAWDL::isUnicastPTP(p319->bytes.data(), p319->length));
        assert(AirportAWDL::isUnicastPTP(p320->bytes.data(), p320->length));
        assert(!AirportAWDL::isUnicastPTP(p5000->bytes.data(), p5000->length));
        assert(!AirportAWDL::isUnicastPTP(pMcast->bytes.data(), pMcast->length));
        mbuf_freem(p319); mbuf_freem(p320); mbuf_freem(p5000); mbuf_freem(pMcast);
    }

    // 2. Queued awdl0 unicast gPTP (UDP 319 & 320) bypasses psfDue(now) hold, transmits with 1500 us
    //    contiguous listener guard (at +1800 us into slot where ordinary data is still gated until 3000 us),
    //    and is never software-aggregated into an A-MSDU pair!
    {
        clockUS = 10000000;
        AirportItlwmAWDLInterface i;
        aggregatePeer(i, peerAddr);
        i.device.acknowledgeData = true;
        i.device.loop.gated = true;
        i.device.awdlRadio.listener = true;
        i.device.awdlRadio.ready = true;
        i.device.awdlRadio.channel = 44;
        i.device.awdlRadio.widthMHz = 20;
        i.device.awdlRadio.dwell.running = true;
        i.device.awdlRadio.dwell.windowEndUS = clockUS + 60000;
        i.device.awdlRadio.queue = 1;
        i.device.awdlRadio.dataQueueConfigured = true;
        i.device.awdlRadio.dataQueue = 2;
        i.running = true;
        for (unsigned s = 0; s < 16; ++s) i.sync.channels[s] = 44;
        auto *peer = i.device.awdlPeers.find(peerAddr, clockUS);
        assert(peer);
        for (unsigned s = 0; s < 16; ++s) peer->sequence.channel[s] = 44;
        i.sync.epochCounter = 0;
        i.sync.epochUS = clockUS;
        peer->sync.nextWindowTU = 64;
        peer->syncReceivedUS = clockUS;

        clockUS += 1800; // 1500 <= sinceChannelStartUS (1800) < 3000, AND psfDue(clockUS) == true!
        i.nextActionUS = clockUS;
        assert(i.psfDue(clockUS));

        // Enqueue Sync (UDP 319) + Follow_Up (UDP 320) back-to-back on awdl0:
        i.enqueue(udpIPv6Ethernet(i.address.octet, peerAddr, 319));
        i.enqueue(udpIPv6Ethernet(i.address.octet, peerAddr, 320));
        i.drainPendingData();

        // Both gPTP frames MUST transmit immediately as 2 individual MPDUs (0 A-MSDU aggregates)!
        assert(i.pendingCount == 0 && i.inFlightCount == 0);
        assert(i.device.data == 2 && i.dataAggregatesSubmitted == 0);

        // Ordinary UDP 5000 data at +1800 us with psfDue(clockUS) MUST still wait for announcement/3000 us guard!
        i.enqueue(udpIPv6Ethernet(i.address.octet, peerAddr, 5000));
        i.drainPendingData();
        assert(i.pendingCount == 1 && i.device.data == 2);
        i.drain();
    }

    assert(!allocations);
    return 0;
}
"""
        self.compile_run(tick_src)


if __name__ == "__main__":
    unittest.main()
