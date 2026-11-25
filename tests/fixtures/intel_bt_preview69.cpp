#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <sys/mman.h>
#include "bluetooth/BlueToolFixup/IntelStandardHCI.hpp"

// Layout of the simulated 24H32 bluetoothd image (base = 0x0, corresponding to VM 0x100000000).
// Patch 1: 0x1ca579 (page 0x1ca000, offset 0x579, 106 bytes)
//   - [0x0a57a00]: current handle (uint8_t)
//   - [0x0a579f1]: enableBTSpecAdvertisingSets (uint8_t)
//   - [0x0a57a10]: savedAddr[slot * 7]
//   - [0x0a510b0 + handle * 0xd28 + 0xd0e]: set[handle].addr (7 bytes: type + 6B BD_ADDR)
//   - [0x0a44de0]: controller-ready guard byte tested at 0x1ca5bc (0x1ca5c3 + 0x87a81d = 0xa44de0)
//   - 0x1c71ff: LE_UpdateMultiAdvertisingSetAddress(addrPtr, handle, cb) -> returns 0
//   - 0x1ca651: enableAdvertisingSetCb()
//   - 0x1ca574: epilogue (pop rbx; pop r14; pop rbp; ret)
//
// Patch 2: 0x1a251a (page 0x1a2000, offset 0x51a, 84 bytes)
//   - 0x0a42d50 + 0x50 (= 0x0a42da0): lookupExternalDongle(&dongleType) callback pointer
//   - 0x021bf03: logExternalDongle(...)
//   - 0x0134705: setStackChipset(uint32_t)
//   - 0x0143d86: setForce1M()
//   - 0x01a256e: internalControllerFallback()
//   - 0x01a25ee: externalDongleContinuation()

struct Patch1Trace {
    uint32_t updateCalls;
    uint32_t enableCalls;
    uint64_t lastAddrPtr;
    uint32_t lastHandle;
    uint64_t lastCbPtr;
};

struct Patch2Trace {
    uint32_t lookupCalls;
    uint32_t lookupReturn;
    uint32_t dongleTypeOut;
    uint32_t logCalls;
    uint32_t setChipsetCalls;
    uint32_t lastChipset;
    uint32_t chipsetHistory[4];
    uint32_t force1MCalls;
    uint32_t fallbackCalls;
    uint32_t contCalls;
    uint32_t contR15;
    uint64_t contR14;
};

static Patch1Trace gP1 = {};
static Patch2Trace gP2 = {};

extern "C" uint32_t p1UpdateStub(uint64_t addrPtr, uint32_t handle, uint64_t cbPtr) {
    ++gP1.updateCalls;
    gP1.lastAddrPtr = addrPtr;
    gP1.lastHandle = handle;
    gP1.lastCbPtr = cbPtr;
    return 0;
}

extern "C" void p1EnableStub() {
    ++gP1.enableCalls;
}

extern "C" uint8_t p2LookupStub(uint32_t *outType) {
    ++gP2.lookupCalls;
    *outType = gP2.dongleTypeOut;
    return static_cast<uint8_t>(gP2.lookupReturn);
}

extern "C" void p2LogStub() {
    ++gP2.logCalls;
}

extern "C" void p2SetChipsetStub(uint32_t chip) {
    if (gP2.setChipsetCalls < 4)
        gP2.chipsetHistory[gP2.setChipsetCalls] = chip;
    ++gP2.setChipsetCalls;
    gP2.lastChipset = chip;
}

extern "C" void p2SetForce1MStub() {
    ++gP2.force1MCalls;
}

extern "C" void p2FallbackStub() {
    ++gP2.fallbackCalls;
}

extern "C" void p2ContStub(uint32_t r15Val, uint64_t r14Val) {
    ++gP2.contCalls;
    gP2.contR15 = r15Val;
    gP2.contR14 = r14Val;
}

extern "C" void invokePatch1(void *entry);
extern "C" void invokePatch2(void *entry, uint8_t hasExternal, uint64_t savedR14);

asm(".text\n"
    "_invokePatch1:\n"
    "pushq %rbp\nmovq %rsp, %rbp\n"
    "pushq %r14\npushq %rbx\n"
    "jmpq *%rdi\n"
    "\n"
    "_invokePatch2:\n"
    "pushq %rbp\nmovq %rsp, %rbp\n"
    "subq $0x180, %rsp\n"
    "pushq %rbx\npushq %r14\npushq %r15\n"
    "movq %rdx, -0x158(%rbp)\n"
    "movzbl %sil, %ebx\n"
    "movl $0xdeadbeef, %r15d\n"
    "callq *%rdi\n"
    "popq %r15\npopq %r14\npopq %rbx\n"
    "addq $0x180, %rsp\n"
    "popq %rbp\nretq\n");

static void emitAbsJump(uint8_t *at, void *target) {
    // movabsq $target, %r11; jmpq *%r11
    at[0] = 0x49;
    at[1] = 0xbb;
    uint64_t addr = reinterpret_cast<uint64_t>(target);
    memcpy(at + 2, &addr, sizeof(addr));
    at[10] = 0x41;
    at[11] = 0xff;
    at[12] = 0xe3;
}

static void testAdvSetRandomAddress() {
    constexpr size_t imageSize = 0xa60000;
    auto image = static_cast<uint8_t *>(mmap(nullptr, imageSize, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANON, -1, 0));
    assert(image != MAP_FAILED);

    auto page = image + 0x1ca000;
    auto region = page + IntelStandardHCI::AdvSetRandomAddressPageOffset;
    memcpy(region, IntelStandardHCI::AdvSetRandomAddressOriginal,
           sizeof(IntelStandardHCI::AdvSetRandomAddressOriginal));

    // Place the exact function epilogue at 0x1ca574 (pop rbx; pop r14; pop rbp; ret)
    // and the tail-call trampoline at 0x1ca63b (pop rbx; pop r14; pop rbp; jmp 0x1ca651).
    const uint8_t epilogue[] = {0x5b, 0x41, 0x5e, 0x5d, 0xc3};
    memcpy(image + 0x1ca574, epilogue, sizeof(epilogue));
    const uint8_t skipTail[] = {0x5b, 0x41, 0x5e, 0x5d, 0xeb, 0x10};
    memcpy(image + 0x1ca63b, skipTail, sizeof(skipTail));

    emitAbsJump(image + 0x1c71ff, reinterpret_cast<void *>(p1UpdateStub));
    emitAbsJump(image + 0x1ca651, reinterpret_cast<void *>(p1EnableStub));

    // Guard check byte at 0x1ca5c3 + 0x87a81d = 0xa44de0 must be non-zero to allow HCI 0x2035 call.
    image[0xa44de0] = 1;

    uint8_t pristine[4096];
    memcpy(pristine, page, sizeof(pristine));

    // Short size rejected cleanly.
    assert(!IntelStandardHCI::applyAdvSetRandomAddress(page, IntelStandardHCI::AdvSetRandomAddressPageOffset + 10));
    assert(memcmp(pristine, page, sizeof(pristine)) == 0);

    // Every single byte of the 106-byte anchor must match.
    for (size_t i = 0; i < sizeof(IntelStandardHCI::AdvSetRandomAddressOriginal); ++i) {
        region[i] ^= 1;
        uint8_t mutated[4096];
        memcpy(mutated, page, sizeof(mutated));
        assert(!IntelStandardHCI::applyAdvSetRandomAddress(page, 4096));
        assert(memcmp(mutated, page, sizeof(mutated)) == 0);
        region[i] ^= 1;
    }

    auto setEntry = [&](uint8_t handle) -> uint8_t * {
        return image + 0xa510b0 + size_t(handle) * 0xd28 + 0xd0e;
    };
    auto savedEntry = [&](uint8_t handle) -> uint8_t * {
        return image + 0xa57a10 + size_t(handle) * 7;
    };

    for (unsigned patched = 0; patched != 2; ++patched) {
        if (patched) {
            assert(IntelStandardHCI::applyAdvSetRandomAddress(page, 4096));
            // Only bytes inside [PageOffset + PatchOffset, PageOffset + PatchOffset + sizeof(Patched)) change.
            for (size_t i = 0; i < 4096; ++i) {
                const size_t begin = IntelStandardHCI::AdvSetRandomAddressPageOffset +
                                     IntelStandardHCI::AdvSetRandomAddressPatchOffset;
                const size_t end = begin + sizeof(IntelStandardHCI::AdvSetRandomAddressPatched);
                if (i < begin || i >= end)
                    assert(page[i] == pristine[i]);
            }
            // Re-applying to an already patched page must be a no-op returning false.
            uint8_t oncePatched[4096];
            memcpy(oncePatched, page, sizeof(oncePatched));
            assert(!IntelStandardHCI::applyAdvSetRandomAddress(page, 4096));
            assert(memcmp(oncePatched, page, sizeof(oncePatched)) == 0);
        }

        assert(mprotect(image, 0xa00000, PROT_READ | PROT_EXEC) == 0);

        for (uint8_t handle : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(5)}) {
            image[0xa57a00] = handle;
            uint8_t *cur = setEntry(handle);
            uint8_t *sav = savedEntry(handle);

            // Case 1: BT-Spec enabled + Random RPA (type 1) where savedAddr == set[handle].addr.
            // Unpatched bluetoothd skips HCI 0x2035 (AdvA stays 00:00:00:00:00:00!).
            // Patched bluetoothd MUST call LE_UpdateMultiAdvertisingSetAddress(cur, handle, 0x1ca651).
            image[0xa579f1] = 1;
            const uint8_t rpa[7] = {0x01, 0x4e, 0x22, 0x33, 0x44, 0x55, 0x66};
            memcpy(cur, rpa, 7);
            memcpy(sav, rpa, 7);
            gP1 = {};
            invokePatch1(region);
            if (!patched) {
                assert(gP1.updateCalls == 0 && gP1.enableCalls == 1);
            } else {
                assert(gP1.updateCalls == 1 && gP1.enableCalls == 0);
                assert(gP1.lastAddrPtr == reinterpret_cast<uint64_t>(cur));
                assert(gP1.lastHandle == handle);
                assert(gP1.lastCbPtr == reinterpret_cast<uint64_t>(image + 0x1ca651));
            }

            // Case 2: BT-Spec enabled + Public address (type 0) -> must skip HCI 0x2035 and go straight to Enable.
            const uint8_t pubAddr[7] = {0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
            memcpy(cur, pubAddr, 7);
            memset(sav, 0x5a, 7);
            gP1 = {};
            invokePatch1(region);
            if (patched) {
                assert(gP1.updateCalls == 0 && gP1.enableCalls == 1);
            }

            // Case 3: Legacy VSC (enableBTSpecAdvertisingSets == 0) + identical 7 bytes (differing 8th byte!) -> skip.
            image[0xa579f1] = 0;
            memcpy(cur, rpa, 7);
            memcpy(sav, rpa, 7);
            cur[7] = 0x11;
            sav[7] = 0xee;
            gP1 = {};
            invokePatch1(region);
            assert(gP1.updateCalls == 0 && gP1.enableCalls == 1);

            // Case 4: Legacy VSC + any single byte 0..6 differs -> call LE_UpdateMultiAdvertisingSetAddress.
            for (unsigned idx = 0; idx < 7; ++idx) {
                memcpy(cur, rpa, 7);
                memcpy(sav, rpa, 7);
                cur[idx] ^= 0x80;
                gP1 = {};
                invokePatch1(region);
                assert(gP1.updateCalls == 1 && gP1.enableCalls == 0);
                assert(gP1.lastAddrPtr == reinterpret_cast<uint64_t>(cur));
                assert(gP1.lastHandle == handle);
                assert(gP1.lastCbPtr == reinterpret_cast<uint64_t>(image + 0x1ca651));
            }
        }

        assert(mprotect(image, 0xa00000, PROT_READ | PROT_WRITE) == 0);
    }

    assert(munmap(image, imageSize) == 0);
    std::puts("native x86_64 AdvSetRandomAddress patch passed");
}

static void testThirdPartyForce1M() {
    constexpr size_t imageSize = 0xa60000;
    auto image = static_cast<uint8_t *>(mmap(nullptr, imageSize, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANON, -1, 0));
    assert(image != MAP_FAILED);

    auto page = image + 0x1a2000;
    auto region = page + IntelStandardHCI::ThirdPartyForce1MPageOffset;
    memcpy(region, IntelStandardHCI::ThirdPartyForce1MOriginal,
           sizeof(IntelStandardHCI::ThirdPartyForce1MOriginal));

    // Stub lookup function pointer at [0x1a2525 + 0x8a082b + 0x50] = [0xa42da0].
    void *lookupPtr = reinterpret_cast<void *>(p2LookupStub);
    memcpy(image + 0xa42da0, &lookupPtr, sizeof(lookupPtr));

    emitAbsJump(image + 0x21bf03, reinterpret_cast<void *>(p2LogStub));
    emitAbsJump(image + 0x134705, reinterpret_cast<void *>(p2SetChipsetStub));
    emitAbsJump(image + 0x143d86, reinterpret_cast<void *>(p2SetForce1MStub));
    // Fallback at 0x1a256e -> call p2FallbackStub; ret
    {
        uint8_t *fb = image + 0x1a256e;
        // movabsq $p2FallbackStub, %r11; jmpq *%r11
        emitAbsJump(fb, reinterpret_cast<void *>(p2FallbackStub));
    }
    // Continuation at 0x1a25ee -> movl %r15d, %edi; movq %r14, %rsi; jmp p2ContStub
    {
        uint8_t *ct = image + 0x1a25ee;
        const uint8_t moveArgs[] = {
            0x44, 0x89, 0xff,       // movl %r15d, %edi
            0x4c, 0x89, 0xf6,       // movq %r14, %rsi
        };
        memcpy(ct, moveArgs, sizeof(moveArgs));
        emitAbsJump(ct + sizeof(moveArgs), reinterpret_cast<void *>(p2ContStub));
    }

    uint8_t pristine[4096];
    memcpy(pristine, page, sizeof(pristine));

    assert(!IntelStandardHCI::applyThirdPartyForce1M(page, IntelStandardHCI::ThirdPartyForce1MPageOffset + 10));
    assert(memcmp(pristine, page, sizeof(pristine)) == 0);

    for (size_t i = 0; i < sizeof(IntelStandardHCI::ThirdPartyForce1MOriginal); ++i) {
        region[i] ^= 1;
        uint8_t mutated[4096];
        memcpy(mutated, page, sizeof(mutated));
        assert(!IntelStandardHCI::applyThirdPartyForce1M(page, 4096));
        assert(memcmp(mutated, page, sizeof(mutated)) == 0);
        region[i] ^= 1;
    }

    for (unsigned patched = 0; patched != 2; ++patched) {
        if (patched) {
            assert(IntelStandardHCI::applyThirdPartyForce1M(page, 4096));
            for (size_t i = 0; i < 4096; ++i) {
                const size_t begin = IntelStandardHCI::ThirdPartyForce1MPageOffset +
                                     IntelStandardHCI::ThirdPartyForce1MPatchOffset;
                const size_t end = begin + sizeof(IntelStandardHCI::ThirdPartyForce1MPatched);
                if (i < begin || i >= end)
                    assert(page[i] == pristine[i]);
            }
            uint8_t oncePatched[4096];
            memcpy(oncePatched, page, sizeof(oncePatched));
            assert(!IntelStandardHCI::applyThirdPartyForce1M(page, 4096));
            assert(memcmp(oncePatched, page, sizeof(oncePatched)) == 0);
        }

        assert(mprotect(image, 0xa00000, PROT_READ | PROT_EXEC) == 0);

        // Case 1: Internal Apple controller (%bl == 0) -> fallback only, zero force1M.
        gP2 = {};
        invokePatch2(region, 0, 0x1122334455667788ULL);
        assert(gP2.fallbackCalls == 1 && gP2.lookupCalls == 0 && gP2.setChipsetCalls == 0 && gP2.force1MCalls == 0);

        // Case 2: External third-party USB dongle (Intel 8087:0aaa, dongleType == 0).
        // Unpatched sets chipset=0 only (force1MCalls == 0 -> hangs in wait-for-phy!).
        // Patched sets chipset=0 AND calls setForce1M() (force1MCalls == 1), preserving r15==0 and r14.
        gP2 = {};
        gP2.lookupReturn = 1;
        gP2.dongleTypeOut = 0;
        invokePatch2(region, 1, 0xabcdef0123456789ULL);
        assert(gP2.lookupCalls == 1 && gP2.fallbackCalls == 0 && gP2.contCalls == 1);
        assert(gP2.setChipsetCalls == 1 && gP2.chipsetHistory[0] == 0);
        assert(gP2.contR15 == 0 && gP2.contR14 == 0xabcdef0123456789ULL);
        assert(gP2.force1MCalls == (patched ? 1u : 0u));

        // Case 3: Apple external USB dongle (dongleType == 0xb) -> MUST NOT call setForce1M(),
        //         sets chipset=0 then 0xfa0, contR15 == 0xb.
        gP2 = {};
        gP2.lookupReturn = 1;
        gP2.dongleTypeOut = 0xb;
        invokePatch2(region, 1, 0xfeedfacecafebeefULL);
        assert(gP2.lookupCalls == 1 && gP2.fallbackCalls == 0 && gP2.contCalls == 1);
        assert(gP2.setChipsetCalls == 2 && gP2.chipsetHistory[0] == 0 && gP2.chipsetHistory[1] == 0xfa0);
        assert(gP2.contR15 == 0xb && gP2.contR14 == 0xfeedfacecafebeefULL);
        assert(gP2.force1MCalls == 0);

        assert(mprotect(image, 0xa00000, PROT_READ | PROT_WRITE) == 0);
    }

    assert(munmap(image, imageSize) == 0);
    std::puts("native x86_64 ThirdPartyForce1M patch passed");
}

int main() {
    testAdvSetRandomAddress();
    testThirdPartyForce1M();
    return 0;
}
