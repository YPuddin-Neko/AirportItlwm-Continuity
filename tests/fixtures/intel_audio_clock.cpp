#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <sys/mman.h>
#include "bluetooth/BlueToolFixup/IntelAudioClock.hpp"
#include "intel_audio_clock_native.hpp"

static uint8_t *mapped;
static uint64_t replyTime = 1000000000;
static unsigned delivered;

template <typename T> static void put(void *p, size_t offset, T value) {
    std::memcpy(static_cast<uint8_t *>(p) + offset, &value, sizeof(value));
}
template <typename T> static T get(void *p, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<uint8_t *>(p) + offset, sizeof(value));
    return value;
}

static void deliver(uint32_t status, void *link, uint32_t clock, uint16_t accuracy) {
    ++delivered;
    alignas(16) uint8_t block[0x40] = {};
    put(block, 0x20, link);
    put(block, 0x28, replyTime);
    put(block, 0x30, status);
    put(block, 0x34, clock);
    put(block, 0x38, accuracy);
    reinterpret_cast<void (*)(void *)>(mapped + 0x3ea04d)(block);
}

int main() {
    constexpr size_t imageSize = 0xa6b000, textSize = 0x9db000;
    mapped = static_cast<uint8_t *>(mmap(nullptr, imageSize, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANON, -1, 0));
    assert(mapped != MAP_FAILED);
    std::memset(mapped, 0xcc, textSize);
    for (const auto &block : nativeClockBlocks)
        std::memcpy(mapped + block.offset, block.bytes, block.size);
    auto page = mapped + 0x3ea000;
    std::memcpy(page + IntelAudioClock::PageOffset, IntelAudioClock::Original,
                sizeof(IntelAudioClock::Original));
    assert(page[0x94] == 0x74);
    const uint8_t noLog[] = {0x31, 0xc0, 0xc3};
    std::memcpy(mapped + 0x7e55de, noLog, sizeof(noLog));
    for (size_t offset : {0x27f14e, 0x27f15c, 0x27f16a, 0x27f32c, 0x27f384})
        mapped[offset] = 0xc3;
    uint64_t guard = 0x1438;
    put(mapped, 0x9dc508, &guard);
    put(mapped, 0xa41f98, int64_t(-1));
    alignas(16) uint8_t manager[0x320] = {}, link[0x140] = {};
    put(mapped, 0xa450e0, static_cast<void *>(manager));
    put(link, 0, uint16_t(0x100));
    put(mapped, 0xa4fca0, static_cast<void *>(link));
    put(manager, 0x26c, uint32_t(1));
    put(manager, 0x270, uint32_t(1));

    // Reject a different callback or a short page without changing its bytes.
    uint8_t pristine[4096];
    std::memcpy(pristine, page, sizeof(pristine));
    assert(!IntelAudioClock::apply(page, 0xf8));
    assert(!IntelAudioClock::apply(nullptr, 4096));
    for (size_t i = 0; i < sizeof(IntelAudioClock::Original); ++i) {
        page[IntelAudioClock::PageOffset + i] ^= 1;
        uint8_t mismatched[4096];
        std::memcpy(mismatched, page, sizeof(mismatched));
        assert(!IntelAudioClock::apply(page, 4096));
        assert(std::memcmp(mismatched, page, sizeof(mismatched)) == 0);
        page[IntelAudioClock::PageOffset + i] ^= 1;
    }
    auto decode = reinterpret_cast<void (*)(uint32_t, decltype(&deliver), void *)>(mapped + 0x150d60);
    unsigned cases = 0;
    for (unsigned patched = 0; patched != 2; ++patched) {
        if (patched) {
            assert(IntelAudioClock::apply(page, 4096));
            for (size_t i = 0; i < sizeof(pristine); ++i)
                assert(page[i] == (i == 0x94 ? 0xeb : pristine[i]));
            assert(!IntelAudioClock::apply(page, 4096));
        }
        assert(mprotect(mapped, textSize, PROT_READ | PROT_EXEC) == 0);
        for (uint16_t handle : {uint16_t(0), uint16_t(0x100)}) {
            for (uint32_t status : {0U, 718U}) {
                for (bool truncated : {false, true}) {
                    uint8_t response[8] = {}, stream[16] = {};
                    put(response, 0, handle);
                    put(response, 2, uint32_t(3200000));
                    put(response, 6, uint16_t(0));
                    put(stream, 0, static_cast<void *>(response));
                    put(stream, 0xa, uint16_t(truncated ? 7 : 8));
                    stream[0xe] = 1;
                    put(manager, 0x2e8, replyTime);
                    put(manager, 0x2f0, uint64_t(0));
                    put(manager, 0x2f8, uint64_t(1));
                    put(manager, 0x308, uint32_t(0));
                    delivered = 0;
                    decode(status, deliver, stream);
                    const bool updated = !status && !truncated && (patched || handle);
                    assert(delivered == 1);
                    assert(get<uint64_t>(manager, 0x2f0) == (updated ? 1000000000ULL : 0));
                    assert(get<uint32_t>(manager, 0x308) == (updated ? 3200000U : 0));
                    assert(get<uint64_t>(manager, 0x2f8) == (updated ? 0U : 1U));
                    ++cases;
                }
            }
        }
        assert(mprotect(mapped, textSize, PROT_READ | PROT_WRITE) == 0);
    }
    assert(munmap(mapped, imageSize) == 0);
    std::printf("native local-clock decoder and updater passed: %u cases; original missing-link failure reproduced\n", cases);
}
