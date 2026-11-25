#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <sys/mman.h>
#include "bluetooth/BlueToolFixup/IntelStandardHCI.hpp"

// Simulated 24H32 bluetoothd image for 0x49d499 (page 0x49d000, offset 0x499, 49 bytes):
//   0x10049d499: callq 0x100337d00  -> getController()
//   0x10049d4a9: callq 0x1001c8007  -> setSupportMultiAdvRandomAddress(uint8_t)
//   0x10049d4be: callq 0x1001f2f2e  -> setThirdPartyFlagA(uint8_t)
//   0x10049d4c5: callq 0x1001f2f48  -> setThirdPartyFlagB(uint8_t)
//   0x10049d4ca: continuation (returns cleanly)

struct MockController {
    uint8_t pad[0x2f8];
    uint32_t chipset;
};

struct Trace {
    uint32_t getControllerCalls;
    uint32_t setMultiAdvRandCalls;
    uint8_t lastMultiAdvRandArg;
    uint32_t flagACalls;
    uint8_t lastFlagAArg;
    uint32_t flagBCalls;
    uint8_t lastFlagBArg;
};

static MockController gCtrl = {};
static Trace gTrace = {};

extern "C" void *getControllerStub() {
    ++gTrace.getControllerCalls;
    return &gCtrl;
}

extern "C" void setMultiAdvRandStub(uint8_t enable) {
    ++gTrace.setMultiAdvRandCalls;
    gTrace.lastMultiAdvRandArg = enable;
}

extern "C" void setFlagAStub(uint8_t enable) {
    ++gTrace.flagACalls;
    gTrace.lastFlagAArg = enable;
}

extern "C" void setFlagBStub(uint8_t enable) {
    ++gTrace.flagBCalls;
    gTrace.lastFlagBArg = enable;
}

static void emitAbsJump(uint8_t *at, void *target) {
    at[0] = 0x49;
    at[1] = 0xbb;
    uint64_t addr = reinterpret_cast<uint64_t>(target);
    memcpy(at + 2, &addr, sizeof(addr));
    at[10] = 0x41;
    at[11] = 0xff;
    at[12] = 0xe3;
}

int main() {
    constexpr size_t imageSize = 0x500000;
    auto image = static_cast<uint8_t *>(mmap(nullptr, imageSize, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANON, -1, 0));
    assert(image != MAP_FAILED);

    auto page = image + 0x49d000;
    auto region = page + IntelStandardHCI::MultiAdvRandomAddressPageOffset;
    memcpy(region, IntelStandardHCI::MultiAdvRandomAddressOriginal,
           sizeof(IntelStandardHCI::MultiAdvRandomAddressOriginal));
    // Place retq at continuation 0x49d4ca (offset +49 from 0x49d499).
    region[sizeof(IntelStandardHCI::MultiAdvRandomAddressOriginal)] = 0xc3;

    emitAbsJump(image + 0x337d00, reinterpret_cast<void *>(getControllerStub));
    emitAbsJump(image + 0x1c8007, reinterpret_cast<void *>(setMultiAdvRandStub));
    emitAbsJump(image + 0x1f2f2e, reinterpret_cast<void *>(setFlagAStub));
    emitAbsJump(image + 0x1f2f48, reinterpret_cast<void *>(setFlagBStub));

    uint8_t pristine[4096];
    memcpy(pristine, page, sizeof(pristine));

    // Short size rejected without modification.
    assert(!IntelStandardHCI::applyMultiAdvRandomAddress(
        page, IntelStandardHCI::MultiAdvRandomAddressPageOffset + 10));
    assert(memcmp(pristine, page, sizeof(pristine)) == 0);

    // Every single byte of the 49-byte anchor must match.
    for (size_t i = 0; i < sizeof(IntelStandardHCI::MultiAdvRandomAddressOriginal); ++i) {
        region[i] ^= 1;
        uint8_t mutated[4096];
        memcpy(mutated, page, sizeof(mutated));
        assert(!IntelStandardHCI::applyMultiAdvRandomAddress(page, 4096));
        assert(memcmp(mutated, page, sizeof(mutated)) == 0);
        region[i] ^= 1;
    }

    using Fn = void (*)();
    auto entry = reinterpret_cast<Fn>(region);

    for (unsigned patched = 0; patched != 2; ++patched) {
        if (patched) {
            assert(IntelStandardHCI::applyMultiAdvRandomAddress(page, 4096));
            for (size_t i = 0; i < 4096; ++i) {
                const size_t begin = IntelStandardHCI::MultiAdvRandomAddressPageOffset +
                                     IntelStandardHCI::MultiAdvRandomAddressPatchOffset;
                const size_t end = begin + sizeof(IntelStandardHCI::MultiAdvRandomAddressPatched);
                if (i < begin || i >= end)
                    assert(page[i] == pristine[i]);
            }
            uint8_t oncePatched[4096];
            memcpy(oncePatched, page, sizeof(oncePatched));
            assert(!IntelStandardHCI::applyMultiAdvRandomAddress(page, 4096));
            assert(memcmp(oncePatched, page, sizeof(oncePatched)) == 0);
        }

        assert(mprotect(image, imageSize, PROT_READ | PROT_EXEC) == 0);

        for (uint32_t chip : {0u, 1u, 2u, 3u, 4u, 0xfa0u, 0x1770u}) {
            gCtrl.chipset = chip;
            gTrace = {};
            entry();
            if (chip == 0) {
                // Intel 8087:0aaa (THIRD_PARTY_DONGLE):
                // Before patch: disables MultiAdvRandomAddress (kills HCI 0x2035!) + sets flags A/B=0.
                // After patch:  PRESERVES MultiAdvRandomAddress (0 calls!) + still sets flags A/B=0.
                assert(gTrace.setMultiAdvRandCalls == (patched ? 0u : 1u));
                assert(gTrace.flagACalls == 1 && gTrace.lastFlagAArg == 0);
                assert(gTrace.flagBCalls == 1 && gTrace.lastFlagBArg == 0);
            } else if (chip == 1) {
                assert(gTrace.setMultiAdvRandCalls == (patched ? 0u : 1u));
                assert(gTrace.flagACalls == 0 && gTrace.flagBCalls == 0);
            } else if (chip == 2 || chip == 3) {
                // Legacy Apple CSR stays 100% unchanged.
                assert(gTrace.setMultiAdvRandCalls == 1 && gTrace.lastMultiAdvRandArg == 0);
                assert(gTrace.flagACalls == 0 && gTrace.flagBCalls == 0);
            } else {
                // Apple Broadcom stays 100% unchanged.
                assert(gTrace.setMultiAdvRandCalls == 0);
                assert(gTrace.flagACalls == 0 && gTrace.flagBCalls == 0);
            }
        }

        assert(mprotect(image, imageSize, PROT_READ | PROT_WRITE) == 0);
    }

    assert(munmap(image, imageSize) == 0);
    std::puts("native x86_64 MultiAdvRandomAddress patch passed");
    return 0;
}
