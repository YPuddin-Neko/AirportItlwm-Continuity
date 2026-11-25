#include <cassert>
#include <cstdint>
#include <cstddef>
#include <initializer_list>
using IOReturn = int;
enum { IOCTL_MASK = 0x80000000U, IOCTL_ID_MAX = 13, kIOReturnError = 1 };
struct OSObject {};
struct IOExternalMethodArguments { const void *structureInput; void *structureOutput; };
struct IOExternalMethodDispatch {};
struct IOUserClient : OSObject {
    IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *) { return 77; }
};
#define super IOUserClient
static unsigned calls;
static IOReturn method(OSObject *, void *, bool) { ++calls; return 0; }
struct ItlNetworkUserClient : IOUserClient {
    static constexpr IOReturn (*sMethods[IOCTL_ID_MAX])(OSObject *, void *, bool) =
        {method,method,method,method,method,method,method,method,method,method,method,method,method};
    IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *);
};
constexpr IOReturn (*ItlNetworkUserClient::sMethods[IOCTL_ID_MAX])(OSObject *, void *, bool);
// PRODUCTION: dispatch
int main() {
    ItlNetworkUserClient client; uint8_t buffer[4096] = {};
    IOExternalMethodArguments args = {buffer, buffer};
    for (auto mask : {0U, unsigned(IOCTL_MASK)}) {
        for (unsigned i = 0; i < IOCTL_ID_MAX; ++i)
            assert(client.externalMethod(i | mask, &args, nullptr, nullptr, nullptr) == 0);
        for (unsigned i : {unsigned(IOCTL_ID_MAX), unsigned(IOCTL_ID_MAX + 1), 0x7fffffffU})
            assert(client.externalMethod(i | mask, &args, nullptr, nullptr, nullptr) == 77);
    }
    assert(calls == 2 * IOCTL_ID_MAX);
}
