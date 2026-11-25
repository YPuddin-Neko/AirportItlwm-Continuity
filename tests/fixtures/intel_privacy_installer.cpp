#include "../../bluetooth/BlueToolFixup/IntelPrivacyInstaller.hpp"
#include "IntelPrivacyImage.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

using namespace IntelPrivacyInstaller;
using NativeImage = Image<IntelPrivacyImage::HookPatch, IntelPrivacyImage::Guard>;
static NativeImage image {IntelPrivacyImage::ImageBytes, IntelPrivacyImage::ImageSize,
    IntelPrivacyImage::Hooks, IntelPrivacyImage::HookCount,
    IntelPrivacyImage::Guards, IntelPrivacyImage::GuardCount};
using Page = std::array<uint8_t, PageSize>;
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
}
static void put64(uint8_t *p, uint64_t v) {
    put32(p, static_cast<uint32_t>(v)); put32(p + 4, static_cast<uint32_t>(v >> 32));
}

// A structural fixture uses no Apple executable bytes. The real generated
// helper, original instructions and replacements are exercised below.
static std::vector<uint8_t> header() {
    std::vector<uint8_t> out(HeaderSize);
    put32(out.data(), 0xfeedfacf); put32(out.data() + 4, 0x1000007);
    put32(out.data() + 8, 3); put32(out.data() + 12, 2);
    put32(out.data() + 16, 76); put32(out.data() + 20, HeaderSize - 32);
    size_t cursor = 32;
    auto segment = [&](const char *n, uint64_t addr, uint64_t size, uint64_t off,
                       uint64_t file, unsigned prot, unsigned sections) {
        auto c = out.data() + cursor;
        unsigned extent = 72 + sections * 80;
        put32(c, 0x19); put32(c + 4, extent); std::strcpy(reinterpret_cast<char *>(c + 8), n);
        put64(c + 24, addr); put64(c + 32, size); put64(c + 40, off); put64(c + 48, file);
        put32(c + 56, prot); put32(c + 60, prot); put32(c + 64, sections);
        for (unsigned i = 0; i < sections; ++i) put64(c + 72 + i * 80 + 32, addr);
        if (sections == 14) {
            auto s = c + 72 + 13 * 80;
            std::strcpy(reinterpret_cast<char *>(s), "__eh_frame");
            put64(s + 32, 0x1009da318ULL); put64(s + 40, 0x58);
        }
        if (sections == 8) {
            auto s = c + 72 + 7 * 80;
            std::strcpy(reinterpret_cast<char *>(s), "__common");
            put64(s + 32, 0x100a649d0ULL); put64(s + 40, 0x6528); put32(s + 64, 1);
        }
        cursor += extent;
    };
    segment("__PAGEZERO", 0, 0x100000000ULL, 0, 0, 0, 0);
    segment("__TEXT", 0x100000000ULL, TextEnd, 0, TextEnd, 5, 14);
    segment("__DATA_CONST", 0x1009db000ULL, 0x4b000, 0x9db000, 0x4b000, 3, 12);
    segment("__DATA", 0x100a26000ULL, 0x45000, 0xa26000, 0x20000, 3, 8);
    segment("__LINKEDIT", 0x100a6b000ULL, 0x44000, 0xa46000, 0x43bb0, 1, 0);
    put32(out.data() + cursor, 0x1b); put32(out.data() + cursor + 4, 24);
    const uint8_t uuid[] = {0x9d,0x47,0x27,0xa2,0x1f,0xc2,0x37,0x77,0xbe,0x26,0x7b,0xad,0x0e,0xd6,0x5e,0xf0};
    std::memcpy(out.data() + cursor + 8, uuid, 16); cursor += 24;
    for (unsigned i = 6; i < 76; ++i) {
        const unsigned size = i == 75 ? static_cast<unsigned>(HeaderSize - cursor) : 8;
        put32(out.data() + cursor, 0x777); put32(out.data() + cursor + 4, size); cursor += size;
    }
    assert(cursor == HeaderSize && layout(out.data(), out.size()));
    return out;
}

struct Process {
    std::vector<uint8_t> mach = header();
    std::array<uint8_t, RuntimeSize> runtime {};
    std::map<uint32_t, Page> pages;
    bool allow = false, bound = false, rejectBind = false;
    uint32_t noFault = UINT32_MAX, failedRead = UINT32_MAX;
    std::vector<uint32_t> reads;

    Process() {
        pages[HelperPage] = {};
        for (size_t i = 0; i < image.hookCount; ++i) {
            auto &h = image.hooks[i];
            auto &p = pages[h.fileOffset & ~0xfffU];
            std::memcpy(p.data() + (h.fileOffset & 0xfff), h.original, h.length);
        }
        for (size_t i = 0; i < image.guardCount; ++i) {
            auto &g = image.guards[i];
            std::memcpy(pages[g.fileOffset & ~0xfffU].data() + (g.fileOffset & 0xfff), g.bytes, g.length);
        }
    }
    bool allMarkers() const {
        for (const auto &page : pages)
            if (page.first != HelperPage && !image.pageMatches(page.first, page.second.data(), true)) return false;
        return true;
    }
    Result run() {
        std::array<uint8_t, HeaderSize> buffer {};
        return prefault(image, [&](uint32_t offset, uint8_t *dst, size_t n) {
            reads.push_back(offset);
            if (offset == failedRead) return false;
            if (!offset) { std::memcpy(dst, mach.data(), n); return true; }
            if (offset == RuntimeOffset) { std::memcpy(dst, runtime.data(), n); return true; }
            auto &page = pages.at(offset);
            if (offset != HelperPage) assert(bound);
            if (offset != noFault) image.apply(offset, page.data(), page.size(), allow || bound, bound, true, false, false);
            std::memcpy(dst, page.data(), n);
            return true;
        }, [&]() { allow = true; }, [&]() {
            assert(image.helperMatches(pages.at(HelperPage).data()));
            if (rejectBind) return false;
            bound = true; return true;
        }, buffer.data());
    }
};

int main(int argc, char **argv) {
    assert(image.valid());
    if (argc == 2) {
        std::ifstream f(argv[1], std::ios::binary);
        std::vector<uint8_t> real(HeaderSize);
        f.read(reinterpret_cast<char *>(real.data()), real.size());
        assert(f && layout(real.data(), real.size()));
    }
    Process p;
    assert(p.run() == Result::Complete && p.allMarkers());
    assert(p.reads[0] == 0 && p.reads[1] == RuntimeOffset && p.reads[2] == HelperPage);
    // Revalidation outside the exec probe must restore the helper before any
    // previously redirected caller can execute after a page is evicted.
    p.pages[HelperPage].fill(0);
    assert(image.apply(HelperPage, p.pages[HelperPage].data(), PageSize, true, true, true, false, false));
    assert(image.helperMatches(p.pages[HelperPage].data()));
    for (auto &pair : p.pages) {
        auto before = pair.second;
        assert(image.apply(pair.first, pair.second.data(), PageSize, true, true, true, false, false));
        assert(pair.second == before);
    }
    // A subsequent exec can consume the already resident helper and finish a
    // missing page group; it does not wait for a second header validation.
    Process again;
    again.pages[HelperPage] = p.pages[HelperPage]; again.noFault = HelperPage;
    assert(again.run() == Result::Complete);
    for (size_t bad = 0; bad < image.hookCount; ++bad) {
        Process missing;
        const auto &h = image.hooks[bad];
        auto page = h.fileOffset & ~0xfffU;
        missing.pages[page][h.fileOffset & 0xfff] ^= 1;
        auto originalGroup = missing.pages[page];
        assert(missing.run() == Result::Hook && !missing.allMarkers());
        assert(missing.pages[page] == originalGroup);
    }
    for (size_t bad = 0; bad < image.guardCount; ++bad) {
        Process missing;
        const auto &g = image.guards[bad];
        auto page = g.fileOffset & ~0xfffU;
        missing.pages[page][g.fileOffset & 0xfff] ^= 1;
        auto originalGroup = missing.pages[page];
        assert(missing.run() == Result::Hook && !missing.allMarkers());
        assert(missing.pages[page] == originalGroup);
    }
    for (auto bad : {HelperOffset & 0xfffU, 0xfffU}) {
        Process missing;
        missing.pages[HelperPage][bad] = 1;
        auto before = missing.pages;
        assert(missing.run() == Result::Helper && !missing.bound && missing.pages == before);
    }
    for (auto failed : {0U, RuntimeOffset, HelperPage, image.hooks[0].fileOffset & ~0xfffU}) {
        Process missing; missing.failedRead = failed;
        assert(missing.run() != Result::Complete && !missing.allMarkers());
    }
    Process residentOriginal; residentOriginal.noFault = HelperPage;
    assert(residentOriginal.run() == Result::Helper && !residentOriginal.bound);
    Process nonzero; nonzero.runtime[8] = 1;
    assert(nonzero.run() == Result::Runtime && !nonzero.allow);
    Process rejected; rejected.rejectBind = true;
    assert(rejected.run() == Result::Profile && !rejected.bound && !rejected.allMarkers());
    for (auto offset : {0U, 4U, 8U, 12U, 16U, 20U, 32U + 4}) {
        Process wrong; wrong.mach[offset] ^= 1;
        assert(wrong.run() == Result::Header && !wrong.allow);
    }
    for (unsigned fault = 0; fault < 4; ++fault) {
        Process wrong;
        for (size_t cursor = 32; cursor < HeaderSize; cursor += read32(wrong.mach.data() + cursor + 4)) {
            auto c = wrong.mach.data() + cursor;
            if (fault == 0 && read32(c) == 0x1b) c[8] ^= 1;
            if (read32(c) != 0x19) continue;
            if (fault == 1 && name(c + 8, "__TEXT")) put32(c + 60, 7);
            if (fault == 2 && name(c + 8, "__TEXT")) put64(c + 72 + 13 * 80 + 40, 0x59);
            if (fault == 3 && name(c + 8, "__DATA")) put64(c + 72 + 7 * 80 + 40, 0x6529);
        }
        assert(wrong.run() == Result::Header && !wrong.allow);
    }
    auto incomplete = image;
    --incomplete.hookCount;
    assert(!incomplete.valid());
    auto oversized = image;
    oversized.size = TextEnd - HelperOffset + 1;
    assert(!oversized.valid());
    Process original;
    auto first = image.hooks[0].fileOffset & ~0xfffU;
    for (unsigned deny = 0; deny < 5; ++deny) {
        auto page = original.pages[first]; auto before = page;
        assert(!image.apply(first, page.data(), PageSize, deny != 0, deny != 1, deny != 2, deny == 3, deny == 4));
        assert(page == before);
    }
    auto page = original.pages[first], before = page;
    assert(!image.apply(first + 1, page.data(), PageSize, true, true, true, false, false));
    assert(!image.apply(first, page.data(), PageSize - 1, true, true, true, false, false));
    assert(page == before);
    std::puts("installer profile, first-pass, partial and replay cases passed");
}
