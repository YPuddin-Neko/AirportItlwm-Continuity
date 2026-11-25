#include <cassert>
#define AIRPORT_WCL 1
#define IEEE80211_STA_ONLY 1
#define XYLog(...) ((void)0)
enum { IEEE80211_M_STA, IEEE80211_M_MONITOR };
enum { IEEE80211_S_INIT, IEEE80211_S_SCAN, IEEE80211_S_AUTH, IEEE80211_S_ASSOC, IEEE80211_S_RUN };
enum { LINK_STATE_DOWN, LINK_STATE_UP };
enum { IEEE80211_F_RSNON = 1, kIONetworkLinkValid = 1, kIONetworkLinkActive = 2 };
struct Controller {
    unsigned calls = 0, status = 0;
    void *getCurrentMedium() { return nullptr; }
    void setLinkStatus(unsigned s, void * = nullptr) { ++calls; status = s; }
};
struct _ifnet { int if_link_state; Controller *controller; };
struct ieee80211_node { bool ni_port_valid; int ni_assoc_fail; };
struct ieee80211com { _ifnet ic_if; int ic_opmode, ic_state; unsigned ic_flags; };
// PRODUCTION: link
static void enterRun(ieee80211com *ic, ieee80211_node *ni) {
    ic->ic_state = IEEE80211_S_RUN;
    // PRODUCTION: run
}
int main() {
    Controller ctl;
    ieee80211com ic = {{LINK_STATE_DOWN, &ctl}, IEEE80211_M_STA, IEEE80211_S_ASSOC, IEEE80211_F_RSNON};
    ieee80211_node ni = {true, 1};
    ieee80211_set_link_state(&ic, LINK_STATE_UP); // M4 completed before RUN.
    assert(ic.ic_if.if_link_state == LINK_STATE_DOWN && ctl.calls == 0);
    enterRun(&ic, &ni);
    assert(ic.ic_if.if_link_state == LINK_STATE_UP && ctl.calls == 1 && ni.ni_assoc_fail == 0);
    ieee80211_set_link_state(&ic, LINK_STATE_UP);
    assert(ctl.calls == 1);
    ic.ic_state = IEEE80211_S_AUTH;
    ieee80211_set_link_state(&ic, LINK_STATE_DOWN);
    assert(ctl.calls == 2 && ic.ic_if.if_link_state == LINK_STATE_DOWN);
    ni.ni_port_valid = false;
    enterRun(&ic, &ni);
    assert(ctl.calls == 2); // Ordinary handshake is still required.
    ni.ni_port_valid = true; ieee80211_set_link_state(&ic, LINK_STATE_UP);
    assert(ctl.calls == 3 && ic.ic_if.if_link_state == LINK_STATE_UP);
    ieee80211_set_link_state(&ic, LINK_STATE_DOWN);
    ni.ni_port_valid = false; ic.ic_flags = 0;
    enterRun(&ic, &ni); // Open networks do not wait for EAPOL.
    assert(ic.ic_if.if_link_state == LINK_STATE_UP);
    ic.ic_opmode = IEEE80211_M_MONITOR;
    ieee80211_set_link_state(&ic, LINK_STATE_UP);
    assert(ic.ic_if.if_link_state == LINK_STATE_DOWN);
}
