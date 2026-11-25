"""Check the native Sequoia multicast return ABI used by BSD group joins."""

import re
import unittest

import test_regressions as support


class NativeMulticast(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_multicast_callbacks_return_ioreturn_zero_on_success(self):
        root = support.ROOT
        base = (root / "include/Airport/macOS15/IOSkywalkEthernetInterface.h").read_text()
        virtual = (root / "include/Airport/macOS15/IO80211VirtualInterface.h").read_text()
        driver = (root / "AirportItlwm/AirportItlwmAWDLInterface.hpp").read_text()
        declarations = []
        for name in ("configureMulticastFilter", "setMulticastAddresses", "setAllMulticastModeEnable"):
            declaration = re.search(r"virtual [^\n]*\b" + name + r"\([^;]*;", base)[0]
            declarations.append(declaration[:-1] + " = 0;")
        override = re.search(r"virtual [^\n]*\bsetMulticastAddresses\([^;]*;", virtual)[0]
        override = override[:-1] + " = 0;"
        implementation = re.search(r"[^\n]*\bsetMulticastAddresses\([^\n]*\}", driver)[0]
        source = r'''
#include <cassert>
#include <cstdint>
#include <type_traits>
using IOReturn = int32_t;
constexpr IOReturn kIOReturnSuccess = 0;
struct ether_addr { uint8_t bytes[6]; };
struct NativeBase {
BASE
};
struct NativeVirtual : NativeBase {
VIRTUAL
};
struct Driver : NativeVirtual {
IMPLEMENTATION
    IOReturn configureMulticastFilter(unsigned, const ether_addr *, unsigned) override { return 0; }
    IOReturn setAllMulticastModeEnable(bool) override { return 0; }
};
static_assert(std::is_same<decltype(&NativeBase::configureMulticastFilter),
    IOReturn (NativeBase::*)(unsigned, const ether_addr *, unsigned)>::value, "native configure ABI");
static_assert(std::is_same<decltype(&NativeBase::setAllMulticastModeEnable),
    IOReturn (NativeBase::*)(bool)>::value, "native all-multicast ABI");
static_assert(std::is_same<decltype(&Driver::setMulticastAddresses),
    IOReturn (Driver::*)(const ether_addr *, unsigned)>::value, "native address-list ABI");
int main() {
    Driver driver;
    NativeBase *native = &driver;
    const ether_addr groups[] = {{{0x33,0x33,0,0,0,1}}, {{0x33,0x33,0,0,0,0xfb}}};
    // 24H32 configureMulticastFilter tests the full return word for zero.
    assert(native->setMulticastAddresses(groups, 2) == 0);
    assert(native->setMulticastAddresses(nullptr, 0) == 0);
}
'''
        source = source.replace("BASE", "\n".join(declarations))
        source = source.replace("VIRTUAL", override).replace("IMPLEMENTATION", implementation)
        self.compile_run(source)


if __name__ == "__main__":
    unittest.main()
