#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#define AIRPORT_WCL 1
constexpr unsigned APPLE80211_VIF_AWDL = 6;
constexpr bool kOSBooleanTrue = true;
struct ether_addr { uint8_t octet[6]; };
struct IONetworkController;
struct IO80211SkywalkInterface {
    unsigned role = 1;
    bool published = false, hasUnit = false, legacyBridge = false;
    unsigned getInterfaceRole() const { return role; }
    void registerService() {
        published = true;
        // 24H32 IOSkywalkLegacyEthernet::probe rejects a provider with a unit.
        if (!hasUnit) legacyBridge = true;
    }
};
struct IOEthernetInterface {
    std::string prefix;
    bool hidden = false, failInit = false;
    virtual ~IOEthernetInterface() = default;
    virtual const char *getNamePrefix() const { return "en"; }
    bool init(IONetworkController *) {
        prefix = getNamePrefix();
        return !failInit;
    }
    bool setProperty(const char *key, bool value) {
        assert(std::string(key) == "HiddenInterface");
        hidden = value;
        return true;
    }
};
struct AirportItlwmEthernetInterface : IOEthernetInterface {
    IO80211SkywalkInterface *interface = nullptr;
    bool isAttach = true;
    const char *getNamePrefix() const override;
    bool initWithSkywalkInterfaceAndProvider(IONetworkController *, IO80211SkywalkInterface *);
    void registerService() {} // IONetworkStack attaches asynchronously.
    void completeBSDAttach() {
        interface->hasUnit = true;
        interface->registerService();
    }
};
#define super IOEthernetInterface
// PRODUCTION: bridge_init
#undef super

struct IOSkywalkNetworkInterface {
    struct RegistrationInfo { uint8_t data[32]; };
};
struct IOSkywalkEthernetInterface {
    using RegistrationInfo = IOSkywalkNetworkInterface::RegistrationInfo;
};
struct AirportItlwmAWDLInterface : IO80211SkywalkInterface {
    struct Expansion { IOSkywalkNetworkInterface::RegistrationInfo *fRegistrationInfo = nullptr; };
    Expansion first, second, *mExpansionData = &first, *mExpansionData2 = &second;
    bool failStart = false, failAttach = false, failInfo = false;
    ~AirportItlwmAWDLInterface() {
        free(first.fRegistrationInfo);
        free(second.fRegistrationInfo);
    }
    bool attach(IONetworkController *) { return !failAttach; }
    bool initRegistrationInfo(void *, int, size_t) { return !failInfo; }
    void setInterfaceId(int) {}
    void deferBSDAttach(bool) {}
    bool start(IONetworkController *) { return !failStart; }
    void bindBSD(AirportItlwmEthernetInterface *) {}
};
using IONetworkInterface = AirportItlwmEthernetInterface;
struct IONetworkController {
    bool failBSD = false;
    IO80211SkywalkInterface *pendingInterface = nullptr;
    bool attachInterface(IONetworkInterface **out, bool publish) {
        assert(!publish);
        if (failBSD) return false;
        *out = new AirportItlwmEthernetInterface;
        return (*out)->initWithSkywalkInterfaceAndProvider(this, pendingInterface);
    }
};
struct HAL {
    struct Controller { uint8_t ic_myaddr[6] = {0x50, 0xeb, 0x71, 1, 2, 3}; } ic;
    Controller *get80211Controller() { return &ic; }
};
struct AirportItlwm : IONetworkController {
    HAL hal, *fHalService = &hal;
    AirportItlwmAWDLInterface *awdlInterface = nullptr;
    AirportItlwmEthernetInterface *awdlBSD = nullptr;
    bool awdlAttached = false, awdlStarted = false, creatingAWDL = false;
    bool failStart = false, failAttach = false, failInfo = false, failCreate = false;
    unsigned cleanups = 0;
    AirportItlwmAWDLInterface *createVirtualInterface(ether_addr *, unsigned role) {
        if (failCreate) return nullptr;
        auto result = new AirportItlwmAWDLInterface;
        result->role = role;
        result->failStart = failStart;
        result->failAttach = failAttach;
        result->failInfo = failInfo;
        pendingInterface = result;
        return result;
    }
    bool attachInterface(AirportItlwmAWDLInterface *, IONetworkController *) { return true; }
    void stopAWDLInterface() {
        ++cleanups;
        delete awdlBSD; awdlBSD = nullptr;
        delete awdlInterface; awdlInterface = nullptr;
        awdlAttached = awdlStarted = creatingAWDL = false;
    }
    ~AirportItlwm() { stopAWDLInterface(); }
    bool startAWDLInterface();
};
static void *IOMalloc(size_t n) { return malloc(n); }
// PRODUCTION: start_awdl

int main() {
    IO80211SkywalkInterface primary, peer;
    peer.role = APPLE80211_VIF_AWDL;
    for (auto native : {&primary, &peer}) {
        AirportItlwmEthernetInterface bridge;
        assert(bridge.initWithSkywalkInterfaceAndProvider(nullptr, native));
        assert(!bridge.isAttach);
        assert(bridge.prefix == (native == &peer ? "awdl" : "en"));
        assert(bridge.hidden == (native == &peer));
    }
    AirportItlwmEthernetInterface failed;
    failed.failInit = true;
    assert(!failed.initWithSkywalkInterfaceAndProvider(nullptr, &peer));
    assert(!failed.hidden);

    AirportItlwm driver;
    assert(driver.startAWDLInterface());
    assert(driver.awdlBSD->hidden && driver.awdlBSD->prefix == "awdl");
    assert(!driver.awdlInterface->published && !driver.awdlInterface->legacyBridge);
    driver.awdlBSD->completeBSDAttach();
    assert(driver.awdlInterface->published && !driver.awdlInterface->legacyBridge);
    assert(!driver.creatingAWDL);

    // The previous publication order admits an extra native Ethernet bridge.
    IO80211SkywalkInterface early;
    early.registerService();
    early.hasUnit = true;
    assert(early.legacyBridge);

    for (int phase = 0; phase < 4; ++phase) {
        AirportItlwm failedDriver;
        failedDriver.failAttach = phase == 0;
        failedDriver.failInfo = phase == 1;
        failedDriver.failStart = phase == 2;
        failedDriver.failBSD = phase == 3;
        assert(!failedDriver.startAWDLInterface());
        assert(failedDriver.cleanups == 1 && !failedDriver.awdlBSD && !failedDriver.awdlInterface);
        assert(!failedDriver.creatingAWDL);
    }
}
