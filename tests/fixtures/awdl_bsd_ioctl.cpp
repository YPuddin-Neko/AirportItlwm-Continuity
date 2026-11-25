#include <cassert>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <initializer_list>

using SInt32 = int32_t;
using IOReturn = int32_t;
struct OSObject {};
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnNotReady = -1001;
constexpr unsigned long SIOCSIFFLAGS = 0x80206910UL;
constexpr unsigned long SIOCSIFLLADDR = 0x8020693cUL;
constexpr unsigned long SIOCADDMULTI = 0x80206931UL, SIOCDELMULTI = 0x80206932UL;
constexpr unsigned IFF_UP = 1, IFF_RUNNING = 0x40;
constexpr unsigned ETHER_ADDR_LEN = 6;
struct ether_addr { uint8_t octet[ETHER_ADDR_LEN]; };
struct sockaddr { uint8_t sa_len; uint8_t sa_family; char sa_data[14]; };
struct ifreq { char ifr_name[16]; sockaddr ifr_addr; };
constexpr unsigned APPLE80211_VIF_LOW_LATENCY = 2;
constexpr unsigned APPLE80211_VIF_AWDL = 6;
struct Ifnet { unsigned flags = 0; };
unsigned ifnet_flags(Ifnet *interface) { return interface->flags; }
struct Gate {
    bool inside = false;
    unsigned calls = 0;
    IOReturn failure = 0;
    IOReturn runAction(IOReturn (*action)(OSObject *, void *, void *, void *, void *),
                      void *arg, void *arg1 = nullptr, void *arg2 = nullptr) {
        ++calls;
        if (failure) return failure;
        inside = true;
        auto result = action(nullptr, arg, arg1, arg2, nullptr);
        inside = false;
        return result;
    }
} gate;
Gate *_fCommandGate = &gate;
struct IONetworkController {};
struct Payload { unsigned version = 1, length = 0, result = 0; };
struct Skywalk {
    unsigned role = APPLE80211_VIF_AWDL, calls = 0, command = 0;
    Ifnet *lastIfnet = nullptr;
    void *lastPayload = nullptr;
    int result = 0;
    unsigned enableCalls = 0;
    bool requested = false;
    IOReturn enableResult = 0;
    IOReturn setInterfaceEnable(bool enabled) {
        assert(gate.inside);
        ++enableCalls;
        if (!enableResult) requested = enabled;
        return enableResult;
    }
    unsigned getInterfaceRole() { return role; }
    ether_addr mac = {{2, 0, 0, 0, 0, 1}};
    IOReturn getHardwareAddress(ether_addr *out) { if (!out) return kIOReturnNotReady; *out = mac; return kIOReturnSuccess; }
    int processBSDCommand(Ifnet *ifnet, unsigned request, void *data) {
        if (request == SIOCADDMULTI || request == SIOCDELMULTI) assert(gate.inside);
        else assert(data);
        ++calls; command = request; lastIfnet = ifnet; lastPayload = data;
        if (data) {
            auto payload = static_cast<Payload *>(data);
            payload->length = 4;
            payload->result = 42;
        }
        return result;
    }
};
struct IOEthernetInterface {
    unsigned genericCalls = 0;
    int performCommand(IONetworkController *, unsigned long, void *, void *) {
        ++genericCalls;
        return EOPNOTSUPP;
    }
};
struct AirportItlwmEthernetInterface : IOEthernetInterface {
    Skywalk *interface = nullptr;
    Ifnet *ifnet = nullptr;
    Ifnet *getIfnet() { return ifnet; }
    SInt32 performCommand(IONetworkController *, unsigned long, void *, void *);
    static IOReturn syncAWDLFlagsGated(OSObject *, void *, void *, void *, void *);
    static IOReturn syncAWDLMulticastGated(OSObject *, void *, void *, void *, void *);
    int errnoFromReturn(IOReturn result) { return result == kIOReturnNotReady ? ENXIO : EIO; }
};
#define super IOEthernetInterface
// PRODUCTION: ioctl

int main() {
    Ifnet ifnet, other;
    IONetworkController controller;
    Skywalk peer, station;
    station.role = 1;
    AirportItlwmEthernetInterface bridge;
    bridge.ifnet = &ifnet;
    bridge.interface = &peer;
    for (unsigned long cmd : {0x802069c8UL, 0x802869c8UL, 0xc02069c9UL, 0xc02869c9UL,
                             SIOCADDMULTI, SIOCDELMULTI}) {
        Payload data;
        // Reproduce the old generic BSD path's rejected Apple80211 request.
        assert(bridge.IOEthernetInterface::performCommand(&controller, cmd, &ifnet, &data) == EOPNOTSUPP);
        auto generic = bridge.genericCalls, calls = peer.calls;
        assert(bridge.performCommand(&controller, cmd, &ifnet, &data) == 0);
        assert(peer.calls == calls + 1 && bridge.genericCalls == generic);
        assert(peer.lastIfnet == &ifnet && peer.command == cmd);
        assert(data.length == 4 && data.result == 42);
        peer.result = EACCES;
        assert(bridge.performCommand(&controller, cmd, &ifnet, &data) == EACCES);
        peer.result = 0;
    }
    // XNU ifnet_ioctl_event_callback sends both group notifications with NULL data.
    for (unsigned long cmd : {SIOCADDMULTI, SIOCDELMULTI}) {
        auto generic = bridge.genericCalls, calls = peer.calls, gated = gate.calls;
        assert(bridge.performCommand(&controller, cmd, &ifnet, nullptr) == 0);
        assert(peer.calls == calls + 1 && bridge.genericCalls == generic);
        assert(gate.calls == gated + 1 && !gate.inside);
        assert(peer.lastIfnet == &ifnet && peer.command == cmd && !peer.lastPayload);
        peer.result = EACCES;
        assert(bridge.performCommand(&controller, cmd, &ifnet, nullptr) == EACCES);
        assert(peer.calls == calls + 2 && bridge.genericCalls == generic);
        peer.result = 0;
    }
    for (unsigned long cmd : {0xc0206911UL, 0xc03069c9UL, 0x100c02869c9UL}) {
        Payload data;
        auto generic = bridge.genericCalls, calls = peer.calls;
        assert(bridge.performCommand(&controller, cmd, &ifnet, &data) == EOPNOTSUPP);
        assert(peer.calls == calls && bridge.genericCalls == generic + 1);
        assert(!data.length && !data.result);
    }
    // A BSD UP request must reach the AWDL enable path without enabling en1.
    gate.calls = 0;
    Payload flags;
    auto generic = bridge.genericCalls;
    ifnet.flags = IFF_UP;
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, &flags) == 0);
    assert(peer.requested && peer.enableCalls == 1 && gate.calls == 1);
    assert(bridge.genericCalls == generic && ifnet.flags == IFF_UP);
    // An unrelated flags update preserves UP and does not assert radio readiness.
    ifnet.flags |= 0x100;
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, &flags) == 0);
    assert(peer.requested && !(ifnet.flags & IFF_RUNNING));
    ifnet.flags = 0;
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, &flags) == 0);
    assert(!peer.requested && bridge.genericCalls == generic);
    // dlil_handle_proto_attach sets IFF_UP before its NULL-data notification.
    auto nullEnabled = peer.enableCalls, nullGated = gate.calls;
    ifnet.flags = IFF_UP;
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, nullptr) == 0);
    assert(peer.requested && peer.enableCalls == nullEnabled + 1 && gate.calls == nullGated + 1);
    assert(bridge.genericCalls == generic && !(ifnet.flags & IFF_RUNNING));
    ifnet.flags = 0;
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, nullptr) == 0);
    assert(!peer.requested && peer.enableCalls == nullEnabled + 2 && gate.calls == nullGated + 2);
    assert(bridge.genericCalls == generic);
    peer.enableResult = kIOReturnNotReady;
    ifnet.flags = IFF_UP;
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, &flags) == ENXIO);
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, nullptr) == ENXIO);
    assert(!peer.requested && !(ifnet.flags & IFF_RUNNING));
    peer.enableResult = 0;
    gate.failure = kIOReturnNotReady;
    auto enabled = peer.enableCalls;
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, &flags) == ENXIO);
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, nullptr) == ENXIO);
    assert(peer.enableCalls == enabled);
    auto multicastCalls = peer.calls;
    assert(bridge.performCommand(&controller, SIOCADDMULTI, &ifnet, &flags) == ENXIO);
    assert(bridge.performCommand(&controller, SIOCADDMULTI, &ifnet, nullptr) == ENXIO);
    assert(peer.calls == multicastCalls);
    gate.failure = 0;
    _fCommandGate = nullptr;
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, &flags) == ENXIO);
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, nullptr) == ENXIO);
    assert(peer.enableCalls == enabled);
    assert(bridge.performCommand(&controller, SIOCDELMULTI, &ifnet, &flags) == ENXIO);
    assert(bridge.performCommand(&controller, SIOCDELMULTI, &ifnet, nullptr) == ENXIO);
    assert(peer.calls == multicastCalls);
    _fCommandGate = &gate;
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &other, &flags) == EINVAL);
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &other, nullptr) == EINVAL);
    assert(bridge.performCommand(&controller, SIOCADDMULTI, &other, &flags) == EINVAL);
    assert(bridge.performCommand(&controller, SIOCDELMULTI, &other, nullptr) == EINVAL);
    bridge.ifnet = nullptr;
    assert(bridge.performCommand(&controller, SIOCSIFFLAGS, nullptr, &flags) == ENXIO);
    assert(bridge.syncAWDLFlagsGated(nullptr, &bridge, nullptr, nullptr, nullptr) == kIOReturnNotReady);
    assert(bridge.performCommand(&controller, SIOCADDMULTI, nullptr, &flags) == ENXIO);
    assert(bridge.syncAWDLMulticastGated(nullptr, &bridge, nullptr, nullptr, nullptr) == ENXIO);
    bridge.ifnet = &ifnet;
    Payload data;
    auto calls = peer.calls;
    assert(bridge.performCommand(&controller, 0xc02869c9UL, &other, &data) == EINVAL);
    assert(bridge.performCommand(&controller, 0xc02869c9UL, &ifnet, nullptr) == EINVAL);
    bridge.ifnet = nullptr;
    assert(bridge.performCommand(&controller, 0xc02869c9UL, nullptr, &data) == ENXIO);
    assert(peer.calls == calls);
    bridge.ifnet = &ifnet;
    for (Skywalk *native : {&station, static_cast<Skywalk *>(nullptr)}) {
        bridge.interface = native;
        auto multicastCalls = bridge.genericCalls;
        assert(bridge.performCommand(&controller, SIOCADDMULTI, &ifnet, &data) == EOPNOTSUPP);
        assert(bridge.performCommand(&controller, SIOCDELMULTI, &ifnet, &data) == EOPNOTSUPP);
        assert(bridge.performCommand(&controller, SIOCADDMULTI, &ifnet, nullptr) == EOPNOTSUPP);
        assert(bridge.performCommand(&controller, SIOCDELMULTI, &ifnet, nullptr) == EOPNOTSUPP);
        assert(bridge.genericCalls == multicastCalls + 4);
        assert(bridge.performCommand(&controller, 0xc02869c9UL, &ifnet, &data) == EOPNOTSUPP);
        assert(station.calls == 0 && peer.calls == calls);
        auto enableCalls = peer.enableCalls;
        assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, &flags) == EOPNOTSUPP);
        assert(bridge.performCommand(&controller, SIOCSIFFLAGS, &ifnet, nullptr) == EOPNOTSUPP);
        assert(station.enableCalls == 0 && peer.enableCalls == enableCalls);
    }
}
