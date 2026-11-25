#include <cassert>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>

constexpr unsigned APPLE80211_VIF_AWDL = 6, kIOEthernetAddressSize = 6;
constexpr int kIOReturnSuccess = 0;
struct ether_addr { uint8_t octet[6]; };
struct ifnet_init_params {
    const void *uniqueid = nullptr;
    uint32_t uniqueid_len = 0, family = 0;
    const char *name = nullptr;
};
struct SkywalkInterface {
    unsigned role;
    ether_addr mac;
    int error = 0;
    unsigned getInterfaceRole() { return role; }
    int getHardwareAddress(ether_addr *out) { *out = mac; return error; }
};
static const uint8_t controllerMAC[6] = {0x50, 0xeb, 0x71, 1, 2, 3};
struct IOEthernetInterface {
    bool fail = false;
    bool initIfnetParams(ifnet_init_params *params) {
        if (fail) return false;
        params->uniqueid = controllerMAC;
        params->uniqueid_len = sizeof(controllerMAC);
        params->family = 2;
        return true;
    }
};
struct AirportItlwmEthernetInterface : IOEthernetInterface {
    SkywalkInterface *interface = nullptr;
    uint8_t awdlIfnetIdentity[4 + kIOEthernetAddressSize] = {};
    bool initIfnetParams(ifnet_init_params *);
};
#define super IOEthernetInterface
// PRODUCTION: identity

// BSD allocation keys on family and unique ID, independently of en/awdl names.
struct Allocator {
    std::set<std::pair<uint32_t, std::string>> used;
    bool allocate(const ifnet_init_params &params) {
        return used.emplace(params.family, std::string(
            static_cast<const char *>(params.uniqueid), params.uniqueid_len)).second;
    }
};

int main() {
    SkywalkInterface station{1, {{0x1e, 1, 2, 3, 4, 5}}};
    SkywalkInterface peer{6, {{0x52, 0xeb, 0x71, 1, 2, 0x83}}};
    AirportItlwmEthernetInterface primary, awdl;
    primary.interface = &station;
    awdl.interface = &peer;
    ifnet_init_params en, secondary;
    assert(primary.initIfnetParams(&en));
    assert(awdl.initIfnetParams(&secondary));
    assert(en.uniqueid == controllerMAC && en.uniqueid_len == 6);
    assert(secondary.uniqueid == awdl.awdlIfnetIdentity);
    assert(secondary.uniqueid_len == 10 && secondary.family == en.family);
    assert(!memcmp(static_cast<const uint8_t *>(secondary.uniqueid) + 4, &peer.mac, 6));
    for (bool peerFirst : {false, true}) {
        Allocator allocator;
        assert(allocator.allocate(peerFirst ? secondary : en));
        assert(allocator.allocate(peerFirst ? en : secondary));
        assert(!allocator.allocate(en));
    }
    // Reproduce d5: separate interface names do not prevent duplicate IDs.
    ifnet_init_params oldPeer;
    assert(awdl.IOEthernetInterface::initIfnetParams(&oldPeer));
    oldPeer.name = "awdl";
    en.name = "en";
    Allocator oldAllocator;
    assert(oldAllocator.allocate(oldPeer));
    assert(!oldAllocator.allocate(en));
    // Private AP MAC changes must not change the primary's permanent identity.
    station.mac = peer.mac;
    assert(primary.initIfnetParams(&en));
    assert(en.uniqueid == controllerMAC && en.uniqueid_len == 6);
    assert(awdl.initIfnetParams(&secondary));
    Allocator equalMACs;
    assert(equalMACs.allocate(en) && equalMACs.allocate(secondary));
    peer.error = 1;
    assert(!awdl.initIfnetParams(&secondary));
    peer.error = 0;
    awdl.fail = true;
    assert(!awdl.initIfnetParams(&secondary));
    primary.interface = nullptr;
    assert(primary.initIfnetParams(&en));
    assert(en.uniqueid == controllerMAC);
}
