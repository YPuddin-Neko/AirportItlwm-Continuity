#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <sys/mman.h>
#include "bluetooth/BlueToolFixup/IntelStandardHCI.hpp"

static constexpr uint8_t NativeInitializer[] = {
    0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0xe8, 0xc2, 0xfc, 0xfe, 0xff,
    0x85, 0xc0, 0x48, 0x8d, 0x05, 0x41, 0x11, 0x70, 0x00, 0x48, 0x8d, 0x15,
    0x42, 0x11, 0x70, 0x00, 0x48, 0x0f, 0x44, 0xd0, 0x48, 0x8d, 0x35, 0xcf,
    0x16, 0x00, 0x00, 0xbf, 0xa2, 0x00, 0x00, 0x00, 0x31, 0xc0, 0xe8, 0xb6,
    0x1d, 0x00, 0x00, 0x85, 0xc0, 0x74, 0x4f, 0x89, 0xc3, 0xbf, 0x02, 0x00,
    0x00, 0x00, 0xbe, 0x2e, 0x00, 0x00, 0x00, 0xe8, 0x60, 0x72, 0xf8, 0xff,
    0x84, 0xc0, 0x74, 0x2c, 0x48, 0x8d, 0x3d, 0x26, 0xe3, 0x73, 0x00, 0x31,
    0xc0, 0xe8, 0xc9, 0x75, 0x0d, 0x00, 0xbf, 0x2e, 0x00, 0x00, 0x00, 0xe8,
    0x56, 0x52, 0xf6, 0xff, 0x49, 0x89, 0xc6, 0x48, 0x89, 0xc7, 0xbe, 0x10,
    0x00, 0x00, 0x00, 0xe8, 0x3f, 0x0b, 0x6a, 0x00, 0x84, 0xc0, 0x75, 0x1f,
    0x48, 0x8b, 0x3d, 0xce, 0xae, 0x90, 0x00, 0x89, 0xde, 0xe8, 0x8b, 0x6e,
    0x0d, 0x00, 0x5b, 0x41, 0x5e, 0x5d, 0xc3, 0x48, 0x8b, 0x3d, 0xbb, 0xae,
    0x90, 0x00, 0xe9, 0x2a, 0x6e, 0x0d, 0x00, 0x89, 0xdf, 0x4c, 0x89, 0xf6,
    0xe8, 0x2f, 0x31, 0x60, 0x00, 0xeb, 0xd5,
};

static constexpr uint8_t NativeChipGetter[] = {
    0x55, 0x48, 0x89, 0xe5, 0x8b, 0x05, 0x05, 0x5b, 0x93, 0x00, 0x5d, 0xc3,
};

static void validateWrites(uint8_t *page, size_t offset, const uint8_t *original,
                           size_t length, size_t immediate, bool (*patch)(void *, size_t)) {
    uint8_t pristine[4096];
    memcpy(pristine, page, sizeof(pristine));
    assert(!patch(page, offset + length - 1));
    assert(memcmp(pristine, page, sizeof(pristine)) == 0);
    for (size_t i = 0; i < length; ++i) {
        page[offset + i] ^= 1;
        uint8_t invalid[4096];
        memcpy(invalid, page, sizeof(invalid));
        assert(!patch(page, 4096));
        assert(memcmp(invalid, page, sizeof(invalid)) == 0);
        page[offset + i] ^= 1;
    }
    assert(patch(page, 4096));
    for (size_t i = 0; i < 4096; ++i)
        if (i < offset + immediate || i >= offset + immediate + 4)
            assert(page[i] == pristine[i]);
    uint8_t changed[4096];
    memcpy(changed, page, sizeof(changed));
    assert(!patch(page, 4096));
    assert(memcmp(changed, page, sizeof(changed)) == 0);
}

int main() {
    constexpr size_t size = 0xa6b000;
    auto image = static_cast<uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANON, -1, 0));
    assert(image != MAP_FAILED);
    memcpy(image + 0x1346f9, NativeChipGetter, sizeof(NativeChipGetter));
    memcpy(image + 0x144a2b, NativeInitializer, sizeof(NativeInitializer));
    memcpy(image + 0x143dcb, IntelStandardHCI::EventMaskGetterOriginal,
           sizeof(IntelStandardHCI::EventMaskGetterOriginal));
    memcpy(image + 0x845b81, IntelStandardHCI::EventMasksOriginal,
           sizeof(IntelStandardHCI::EventMasksOriginal));
    // Record the actual 2001 descriptor and eight-byte parameter at dispatch.
    uint8_t dispatch[] = {0x89, 0x3d, 0, 0, 0, 0, 0x48, 0x8b, 0x02,
                          0x48, 0x89, 0x05, 0, 0, 0, 0, 0x31, 0xc0, 0xc3};
    int32_t displacement = 0xa6a300 - (0x146814 + 6);
    memcpy(dispatch + 2, &displacement, 4);
    displacement = 0xa6a308 - (0x146814 + 16);
    memcpy(dispatch + 12, &displacement, 4);
    memcpy(image + 0x146814, dispatch, sizeof(dispatch));
    using Init = void (*)();
    using Getter = uint64_t (*)();
    auto init = reinterpret_cast<Init>(image + 0x144a2b);
    auto getter = reinterpret_cast<Getter>(image + 0x143dcb);
    auto chip = reinterpret_cast<uint32_t *>(image + 0xa6a208);
    auto command = reinterpret_cast<uint32_t *>(image + 0xa6a300);
    auto wireMask = reinterpret_cast<uint64_t *>(image + 0xa6a308);
    constexpr uint64_t readyEvents = (1ULL << (0x07 - 1)) | (1ULL << (0x0c - 1));
    constexpr uint64_t added = readyEvents | (1ULL << (0x0a - 1)) |
        (1ULL << (0x0d - 1)) | (1ULL << (0x12 - 1));
    static_assert((0x2121fULL & readyEvents) == 0);
    static_assert((IntelStandardHCI::StandardEventMask ^ 0x1f) == added);
    for (unsigned patched = 0; patched < 2; ++patched) {
        if (patched) {
            validateWrites(image + 0x143000, IntelStandardHCI::EventMaskGetterPageOffset,
                           IntelStandardHCI::EventMaskGetterOriginal, sizeof(IntelStandardHCI::EventMaskGetterOriginal),
                           IntelStandardHCI::EventMaskImmediateOffset, IntelStandardHCI::applyEventMaskGetter);
            validateWrites(image + 0x845000, IntelStandardHCI::EventMasksPageOffset,
                           IntelStandardHCI::EventMasksOriginal, sizeof(IntelStandardHCI::EventMasksOriginal),
                           0, IntelStandardHCI::applyEventMasks);
        }
        assert(mprotect(image, 0xa6a000, PROT_READ | PROT_EXEC) == 0);
        for (uint32_t value : {0U, 1U, 19U, 25U, 2001U, 5000U, UINT32_MAX}) {
            *chip = value;
            *command = 0;
            *wireMask = 0;
            init();
            auto expected = value ? 0x000bf3ff3f86fa7fULL : (patched ? 0x21a5fULL : 0x1fULL);
            assert(*command == 0xa2);
            assert(*wireMask == expected);
            assert(getter() == expected);
            if (!value && patched) {
                assert((*wireMask & readyEvents) == readyEvents);
                assert((getter() & readyEvents) == readyEvents);
            }
        }
        assert(mprotect(image, 0xa6a000, PROT_READ | PROT_WRITE) == 0);
    }
    assert(munmap(image, size) == 0);
    std::puts("native cold-start and resume masks passed");
}
