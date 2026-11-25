#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include "bluetooth/BlueToolFixup/IntelStandardHCI.hpp"
#include "bluetooth/BlueToolFixup/policy.cpp"

extern "C" void invokePowerBranch(void *, void *, void *);
asm(".text\n"
    "_invokePowerBranch:\n"
    "pushq %rbp\nmovq %rsp, %rbp\n"
    "pushq %rbx\npushq %r12\npushq %r14\npushq %r15\nsubq $64, %rsp\n"
    "movq %rdi, %rbx\nmovq %rsi, %r12\ncallq *%rdx\n"
    "addq $64, %rsp\npopq %r15\npopq %r14\npopq %r12\npopq %rbx\n"
    "popq %rbp\nretq\n");

static void powerQuery() {
    constexpr size_t size = 0xa59000;
    auto image = static_cast<uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANON, -1, 0));
    assert(image != MAP_FAILED);
    auto page = image + 0x618000;
    auto region = page + IntelStandardHCI::PowerQueryPageOffset;
    memcpy(region, IntelStandardHCI::PowerQueryOriginal, sizeof(IntelStandardHCI::PowerQueryOriginal));
    region[sizeof(IntelStandardHCI::PowerQueryOriginal)] = 0xc3;
    // Only the native logging functions and HCI query are stubbed.
    const uint8_t logging[] = {0xb0, 0x00, 0xc3};
    memcpy(image + 0x7e55de, logging, sizeof(logging));
    image[0x7e4ed6] = 0xc3;
    uint8_t hci[] = {0xff, 0x05, 0, 0, 0, 0, 0xc3};
    int32_t displacement = 0xa57b00 - (0x142dfc + 6);
    memcpy(hci + 2, &displacement, sizeof(displacement));
    memcpy(image + 0x142dfc, hci, sizeof(hci));
    auto count = reinterpret_cast<uint32_t *>(image + 0xa57b00);
    uint8_t controller[0x6a00] = {};
    void *logObject = nullptr;
    uint8_t pristine[4096];
    memcpy(pristine, page, sizeof(pristine));
    assert(!IntelStandardHCI::applyPowerQuery(page, 0x730));
    assert(memcmp(pristine, page, sizeof(pristine)) == 0);
    for (size_t i = 0; i < sizeof(IntelStandardHCI::PowerQueryOriginal); ++i) {
        region[i] ^= 1;
        uint8_t invalid[4096];
        memcpy(invalid, page, sizeof(invalid));
        assert(!IntelStandardHCI::applyPowerQuery(page, 4096));
        assert(memcmp(invalid, page, sizeof(invalid)) == 0);
        region[i] ^= 1;
    }
    for (unsigned patched = 0; patched != 2; ++patched) {
        if (patched) {
            assert(IntelStandardHCI::applyPowerQuery(page, 4096));
            for (size_t i = 0; i < sizeof(pristine); ++i) {
                const auto begin = IntelStandardHCI::PowerQueryPageOffset + IntelStandardHCI::PowerQueryLoadOffset;
                if (i < begin || i >= begin + sizeof(IntelStandardHCI::PowerQueryPatched))
                    assert(page[i] == pristine[i]);
            }
            uint8_t changed[4096];
            memcpy(changed, page, sizeof(changed));
            assert(!IntelStandardHCI::applyPowerQuery(page, 4096));
            assert(memcmp(changed, page, sizeof(changed)) == 0);
        }
        for (unsigned logEnabled = 0; logEnabled != 2; ++logEnabled) {
            image[0x7e55df] = logEnabled;
            assert(mprotect(image, 0xa57000, PROT_READ | PROT_EXEC) == 0);
            for (unsigned extended = 0; extended != 2; ++extended) {
                controller[0x6901] = extended;
                for (unsigned standard = 0; standard != 2; ++standard) {
                    image[0xa579f1] = standard;
                    *count = 0;
                    invokePowerBranch(controller, &logObject, region);
                    assert(*count == (patched ? !standard : !extended));
                    assert(controller[0x6901] == extended);
                    assert(image[0xa579f1] == standard);
                }
            }
            assert(mprotect(image, 0xa57000, PROT_READ | PROT_WRITE) == 0);
        }
    }
    assert(munmap(image, size) == 0);
    std::puts("native TX-power command-family branches passed");
}

static bool expected(uint32_t chip, bool sets) {
    return (chip >= 19 && chip <= 25) || (chip >= 2001 && chip <= 3999) ||
        (chip >= 5000 && (sets || chip <= 6000));
}

int main() {
    constexpr size_t pageSize = 4096;
    auto page = static_cast<uint8_t *>(mmap(nullptr, pageSize, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANON, -1, 0));
    assert(page != MAP_FAILED);
    memset(page, 0xcc, pageSize);
    memcpy(page + IntelStandardHCI::PageOffset, IntelStandardHCI::Original,
           sizeof(IntelStandardHCI::Original));
    uint8_t pristine[pageSize];
    memcpy(pristine, page, pageSize);
    assert(!IntelStandardHCI::apply(page, IntelStandardHCI::PageOffset));
    assert(memcmp(pristine, page, pageSize) == 0);
    // A changed selector or intervening V2 function rejects the entire group.
    for (size_t i = 0; i < sizeof(IntelStandardHCI::Original); ++i) {
        page[IntelStandardHCI::PageOffset + i] ^= 1;
        uint8_t invalid[pageSize];
        memcpy(invalid, page, pageSize);
        assert(!IntelStandardHCI::apply(page, pageSize));
        assert(memcmp(invalid, page, pageSize) == 0);
        page[IntelStandardHCI::PageOffset + i] ^= 1;
    }
    alignas(uint32_t) uint8_t controller[0x300] = {};
    using Predicate = bool (*)(void *);
    const size_t offsets[] = {0, 0x52, 0xbe};
    assert(mprotect(page, pageSize, PROT_READ | PROT_EXEC) == 0);
    for (uint32_t chip = 0; chip <= 10000; ++chip) {
        memcpy(controller + 0x2f8, &chip, sizeof(chip));
        for (unsigned fn = 0; fn < 3; ++fn) {
            auto predicate = reinterpret_cast<Predicate>(page + IntelStandardHCI::PageOffset + offsets[fn]);
            assert(predicate(controller) == expected(chip, fn == 2));
        }
    }
    assert(mprotect(page, pageSize, PROT_READ | PROT_WRITE) == 0);
    assert(IntelStandardHCI::apply(page, pageSize));
    for (size_t i = 0; i < pageSize; ++i) {
        bool selected = false;
        for (unsigned fn = 0; fn < 3; ++fn) {
            const auto start = IntelStandardHCI::PageOffset + offsets[fn];
            const auto length = fn == 2 ? sizeof(IntelStandardHCI::StandardSets) : sizeof(IntelStandardHCI::StandardScan);
            selected |= i >= start && i < start + length;
        }
        if (!selected) assert(page[i] == pristine[i]);
    }
    // Revalidating an already changed page leaves it intact.
    uint8_t patched[pageSize];
    memcpy(patched, page, pageSize);
    assert(!IntelStandardHCI::apply(page, pageSize));
    assert(memcmp(patched, page, pageSize) == 0);
    assert(mprotect(page, pageSize, PROT_READ | PROT_EXEC) == 0);
    for (uint32_t chip = 0; chip <= 10000; ++chip) {
        memcpy(controller + 0x2f8, &chip, sizeof(chip));
        for (unsigned fn = 0; fn < 3; ++fn) {
            auto predicate = reinterpret_cast<Predicate>(page + IntelStandardHCI::PageOffset + offsets[fn]);
            assert(predicate(controller) == (chip <= 1 || expected(chip, fn == 2)));
            assert(predicate(controller) == (fn == 2 ? intelStandardSets(controller) : intelStandardScan(controller)));
        }
    }
    uint32_t chip = UINT32_MAX;
    memcpy(controller + 0x2f8, &chip, sizeof(chip));
    for (unsigned fn = 0; fn < 3; ++fn) {
        auto predicate = reinterpret_cast<Predicate>(page + IntelStandardHCI::PageOffset + offsets[fn]);
        assert(predicate(controller) == expected(chip, fn == 2));
    }
    munmap(page, pageSize);
    std::puts("native x86_64 policy execution and whole-group matching passed");
    powerQuery();
}
