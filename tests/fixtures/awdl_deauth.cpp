#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#define AIRPORT_WCL 1
#define XYLog(...) ((void)0)
#define DEVNAME(sc) "iwx-test"
#define splassert(...) ((void)0)
#define container_of(p, type, member) reinterpret_cast<type *>(reinterpret_cast<char *>(p) - offsetof(type, member))
// PRODUCTION CONSTANTS

enum ieee80211_state { IEEE80211_S_INIT, IEEE80211_S_SCAN, IEEE80211_S_AUTH,
    IEEE80211_S_ASSOC, IEEE80211_S_RUN };
enum { IEEE80211_CHAN_WIDTH_20_NOHT = 0, IWX_FW_CTXT_ACTION_REMOVE = 2,
    IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD = 54, kIOReturnSuccess = 0 };
using IOReturn = int;
struct OSObject {};
struct ieee80211_channel {};
struct ieee80211_node { int ni_chw = 80; uint32_t ni_flags = UINT32_MAX; };
struct iwx_node { ieee80211_node in_ni; int in_id = 0, station_id = 0; };
struct iwx_phy_ctxt { int id; };
struct ieee80211com {
    ieee80211_node *ic_bss = nullptr;
    ieee80211_state ic_state = IEEE80211_S_RUN;
    ieee80211_channel ic_channels[2];
    unsigned transitions = 0;
};
struct Task { unsigned queued = 0; };
struct iwx_softc {
    ieee80211com sc_ic;
    int sc_generation = 1, ns_arg = 7;
    ieee80211_state ns_nstate = IEEE80211_S_INIT;
    unsigned sc_flags = IWX_FLAG_STA_ACTIVE | IWX_FLAG_BINDING_ACTIVE | IWX_FLAG_MAC_ACTIVE;
    uint64_t sc_enabled_capa = uint64_t(1) << IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD;
    iwx_phy_ctxt sc_phyctxt[2] = {{0}, {1}};
    Task init_task;
    void (*sc_newstate)(ieee80211com *, ieee80211_state, int) =
        [](ieee80211com *ic, ieee80211_state state, int arg) {
            assert(arg == 7); ++ic->transitions; ic->ic_state = state;
        };
};
static void *systq;
static void task_add(void *, Task *task) { ++task->queued; }
static bool isset(uint64_t flags, unsigned bit) { return flags & (uint64_t(1) << bit); }
static int splnet() { return 0; }
static void splx(int) {}
struct WorkLoop { bool inGate() const { return true; } };
struct Gate {
    IOReturn runAction(IOReturn (*action)(OSObject *, void *, void *, void *, void *), void *arg) {
        return action(nullptr, arg, nullptr, nullptr, nullptr);
    }
};

struct ItlIwx {
    iwx_softc com;
    iwx_node ap;
    WorkLoop loop;
    Gate gate;
    int stopError = EINPROGRESS, apCancelError = 0, removeStationError = 0;
    bool resetOnStop = false, resetOnAPCancel = false;
    std::vector<std::string> calls;

    ItlIwx() { com.sc_ic.ic_bss = &ap.in_ni; }
    WorkLoop *getMainWorkLoop() { return &loop; }
    Gate *getMainCommandGate() { return &gate; }
    int stopAWDLRadio() {
        calls.emplace_back("awdl-stop");
        if (resetOnStop) ++com.sc_generation;
        return stopError;
    }
    int iwx_bt_coex_update_ci(iwx_softc *, bool) { return 0; }
    void checkAP(iwx_softc *sc, iwx_node *node) {
        assert(sc == &com && node == &ap && node->in_id == 0 && node->station_id == 0);
    }
    void iwx_unprotect_session(iwx_softc *sc, iwx_node *node) {
        checkAP(sc, node); calls.emplace_back("ap-unprotect");
    }
    int iwx_cancel_session_protection(iwx_softc *sc, iwx_node *node) {
        checkAP(sc, node); calls.emplace_back("ap-cancel");
        if (resetOnAPCancel) ++com.sc_generation;
        return apCancelError;
    }
    int iwx_rm_sta(iwx_softc *sc, iwx_node *node) {
        checkAP(sc, node); calls.emplace_back("ap-remove-station");
        if (!removeStationError) sc->sc_flags &= ~IWX_FLAG_STA_ACTIVE;
        return removeStationError;
    }
    int iwx_binding_cmd(iwx_softc *sc, iwx_node *node, int action) {
        checkAP(sc, node); assert(action == IWX_FW_CTXT_ACTION_REMOVE);
        calls.emplace_back("ap-remove-binding"); return 0;
    }
    int iwx_mac_ctxt_cmd(iwx_softc *sc, iwx_node *node, int action, int flags) {
        checkAP(sc, node); assert(action == IWX_FW_CTXT_ACTION_REMOVE && flags == 0);
        calls.emplace_back("ap-remove-mac"); return 0;
    }
    int iwx_phy_ctxt_update(iwx_softc *sc, iwx_phy_ctxt *phy, ieee80211_channel *channel,
                           int chains, int active, int flags) {
        assert(sc == &com && phy == &com.sc_phyctxt[0] && phy->id == 0);
        assert(channel == &com.sc_ic.ic_channels[1] && chains == 1 && active == 1 && flags == 0);
        calls.emplace_back("ap-reset-phy"); return 0;
    }
    int iwx_run_stop(iwx_softc *sc) {
        assert(sc == &com); calls.emplace_back("ap-stop-running"); return 0;
    }
    int iwx_scan(iwx_softc *) { assert(false); return 0; }
    int iwx_auth(iwx_softc *) { assert(false); return 0; }
    int iwx_rs_init(iwx_softc *, iwx_node *, bool) { assert(false); return 0; }
    int iwx_run(iwx_softc *) { assert(false); return 0; }
    int iwx_deauth(iwx_softc *);
    void iwx_bt_coex_link(iwx_softc *,bool){}
    static void iwx_newstate_task(void *);
};

// PRODUCTION FUNCTIONS

static void pendingAPCleanup()
{
    for (const int stopResult : {0, EINPROGRESS}) {
        for (const bool sessionAPI : {false, true}) {
            ItlIwx driver;
            driver.stopError = stopResult;
            if (!sessionAPI) driver.com.sc_enabled_capa = 0;
            ItlIwx::iwx_newstate_task(&driver.com);
            const std::vector<std::string> expected = {"ap-stop-running", "awdl-stop",
                sessionAPI ? "ap-cancel" : "ap-unprotect", "ap-remove-station",
                "ap-remove-binding", "ap-remove-mac", "ap-reset-phy"};
            assert(driver.calls == expected);
            assert(driver.com.init_task.queued == 0 && driver.com.sc_generation == 1);
            assert(driver.com.sc_ic.transitions == 1 && driver.com.sc_ic.ic_state == IEEE80211_S_INIT);
            assert(!(driver.com.sc_flags & (IWX_FLAG_STA_ACTIVE | IWX_FLAG_BINDING_ACTIVE | IWX_FLAG_MAC_ACTIVE)));
            assert(driver.ap.in_ni.ni_chw == IEEE80211_CHAN_WIDTH_20_NOHT);
            const uint32_t cleared = IEEE80211_NODE_HT | IEEE80211_NODE_QOS | IEEE80211_NODE_HT_SGI20 |
                IEEE80211_NODE_HT_SGI40 | IEEE80211_NODE_VHT | IEEE80211_NODE_VHT_SGI80 |
                IEEE80211_NODE_VHT_SGI160 | IEEE80211_NODE_HE;
            assert(driver.ap.in_ni.ni_flags == (UINT32_MAX & ~cleared));
        }
    }
}

static void realErrors()
{
    for (const int error : {EIO, ENXIO}) {
        ItlIwx direct;
        direct.stopError = error;
        assert(direct.iwx_deauth(&direct.com) == error);
        assert(direct.calls == std::vector<std::string>{"awdl-stop"});
        assert(direct.com.sc_flags & IWX_FLAG_STA_ACTIVE);
        ItlIwx transition;
        transition.stopError = error;
        ItlIwx::iwx_newstate_task(&transition.com);
        assert(transition.com.init_task.queued == 1 && transition.com.sc_ic.transitions == 0);
        assert((transition.calls == std::vector<std::string>{"ap-stop-running", "awdl-stop"}));
    }
    ItlIwx apFailure;
    apFailure.removeStationError = EIO;
    assert(apFailure.iwx_deauth(&apFailure.com) == EIO);
    assert((apFailure.calls == std::vector<std::string>{"awdl-stop", "ap-cancel", "ap-remove-station"}));
    assert(apFailure.com.sc_flags & IWX_FLAG_BINDING_ACTIVE);
}

static void resetDuringCommands()
{
    ItlIwx resetInAWDL;
    resetInAWDL.stopError = ENXIO;
    resetInAWDL.resetOnStop = true;
    ItlIwx::iwx_newstate_task(&resetInAWDL.com);
    assert(resetInAWDL.com.sc_generation == 2 && resetInAWDL.com.init_task.queued == 0);
    assert(resetInAWDL.com.sc_ic.transitions == 0);
    assert((resetInAWDL.calls == std::vector<std::string>{"ap-stop-running", "awdl-stop"}));

    ItlIwx direct;
    direct.resetOnAPCancel = true;
    assert(direct.iwx_deauth(&direct.com) == ENXIO);
    assert((direct.calls == std::vector<std::string>{"awdl-stop", "ap-cancel"}));
    assert(direct.com.sc_flags & IWX_FLAG_STA_ACTIVE);
    ItlIwx transition;
    transition.resetOnAPCancel = true;
    ItlIwx::iwx_newstate_task(&transition.com);
    assert(transition.com.sc_generation == 2 && transition.com.init_task.queued == 0);
    assert(transition.com.sc_ic.transitions == 0);
}

int main()
{
    // TEST ENTRY
}
