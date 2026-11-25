#include <cassert>
#include <cstring>
#include <cstdint>
#include "itl80211/openbsd/sys/tree.h"
#define IEEE80211_ADDR_LEN 6
#define IEEE80211_ADDR_COPY(dst, src) memcpy(dst, src, 6)
struct ieee80211_node { RB_ENTRY(ieee80211_node) ni_node; uint8_t ni_macaddr[6]; };
RB_HEAD(ieee80211_tree, ieee80211_node);
struct ieee80211com { ieee80211_tree ic_tree; };
struct ieee80211_scan_cursor { uint8_t last_mac[6]; bool started; };
// PRODUCTION: cursor
RB_GENERATE(ieee80211_tree, ieee80211_node, ni_node, ieee80211_node_cmp)
static ieee80211_node *insert(ieee80211com &ic, unsigned n) {
    auto p = new ieee80211_node{};
    p->ni_macaddr[4] = n >> 8; p->ni_macaddr[5] = n;
    assert(!RB_INSERT(ieee80211_tree, &ic.ic_tree, p));
    return p;
}
static void erase(ieee80211com &ic, ieee80211_node *p) {
    RB_REMOVE(ieee80211_tree, &ic.ic_tree, p); delete p;
}
int main() {
    ieee80211com ic = {}; ieee80211_scan_cursor a = {}, b = {};
    assert(!ieee80211_scan_next(&ic, &a));
    auto p20 = insert(ic, 20), p10 = insert(ic, 10), p30 = insert(ic, 30);
    assert(ieee80211_scan_next(&ic, &a) == p10);
    assert(ieee80211_scan_next(&ic, &b) == p10);
    erase(ic, p10); erase(ic, p20); // Both last-returned and would-be-next nodes disappear.
    auto p25 = insert(ic, 25);
    assert(ieee80211_scan_next(&ic, &a) == p25);
    assert(ieee80211_scan_next(&ic, &a) == p30);
    assert(!ieee80211_scan_next(&ic, &a));
    assert(ieee80211_scan_next(&ic, &b) == p25); // Independent client.
    erase(ic, p25); erase(ic, p30);
    assert(!ieee80211_scan_next(&ic, &b));
    for (unsigned i = 0; i != 1000; ++i) insert(ic, i);
    a = {};
    for (unsigned i = 0; i != 1000; ++i) {
        auto n = ieee80211_scan_next(&ic, &a);
        assert(n && (unsigned(n->ni_macaddr[4]) << 8 | n->ni_macaddr[5]) == i);
        erase(ic, n);
    }
    assert(!ieee80211_scan_next(&ic, &a));
}
