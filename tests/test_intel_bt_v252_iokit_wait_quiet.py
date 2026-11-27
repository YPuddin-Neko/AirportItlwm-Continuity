"""Verify BlueToolFixup 2.7.4 IOKitWaitQuiet 240s -> 8s COW timeout cap across 24H32 and 24H241."""

import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

FIXTURE_CPP = r"""
#include "bluetooth/BlueToolFixup/IntelPrivacyInstaller.hpp"
#include "bluetooth/BlueToolFixup/IntelStandardHCI.hpp"
#include "IntelPrivacyImage.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

using namespace IntelPrivacyInstaller;
using NativeImage = Image<IntelPrivacyImage::HookPatch, IntelPrivacyImage::Guard>;
using Page = std::array<uint8_t, PageSize>;

static NativeImage image {
    IntelPrivacyImage::ImageBytes, IntelPrivacyImage::ImageSize,
    IntelPrivacyImage::Hooks, IntelPrivacyImage::HookCount,
    IntelPrivacyImage::Guards, IntelPrivacyImage::GuardCount
};

// Exact 28-byte windows from /usr/sbin/bluetoothd (RVA 0x2cf56..0x2cf71):
//   0x10002cf56: leaq -0x470(%rbp), %rsi
//   0x10002cf5d: movq $0xf0, (%rsi)       ; 240s -> patched to 0x08 (8s) at +10 (0xf60)
//   0x10002cf64: movq kIOMainPortDefault(%rip), %rax
//   0x10002cf6b: movl (%rax), %edi
//   0x10002cf6d: callq _IOKitWaitQuiet    ; rel32 = 82 79 7b 00 (24H32) / 92 79 7b 00 (24H241)
static const uint8_t kWindow24H32[28] = {
    0x48, 0x8d, 0xb5, 0x90, 0xfb, 0xff, 0xff,
    0x48, 0xc7, 0x06, 0xf0, 0x00, 0x00, 0x00,
    0x48, 0x8b, 0x05, 0xe5, 0xe9, 0x9a, 0x00,
    0x8b, 0x38,
    0xe8, 0x82, 0x79, 0x7b, 0x00,
};

static const uint8_t kWindow24H241[28] = {
    0x48, 0x8d, 0xb5, 0x90, 0xfb, 0xff, 0xff,
    0x48, 0xc7, 0x06, 0xf0, 0x00, 0x00, 0x00,
    0x48, 0x8b, 0x05, 0xe5, 0xe9, 0x9a, 0x00,
    0x8b, 0x38,
    0xe8, 0x92, 0x79, 0x7b, 0x00,
};

static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (i * 8));
}
static void put64(uint8_t *p, uint64_t v) {
    put32(p, static_cast<uint32_t>(v));
    put32(p + 4, static_cast<uint32_t>(v >> 32));
}

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static std::vector<uint8_t> makeHeader(bool match24H32, uint32_t textSize = TextEnd) {
    std::vector<uint8_t> out(HeaderSize, 0);
    put32(out.data(), 0xfeedfacf);
    put32(out.data() + 4, 0x1000007);
    put32(out.data() + 8, 3);
    put32(out.data() + 12, 2);
    put32(out.data() + 16, 76);
    put32(out.data() + 20, HeaderSize - 32);
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
    segment("__TEXT", 0x100000000ULL, textSize, 0, textSize, 5, 14);
    segment("__DATA_CONST", 0x1009db000ULL, 0x4b000, 0x9db000, 0x4b000, 3, 12);
    segment("__DATA", 0x100a26000ULL, 0x45000, 0xa26000, 0x20000, 3, 8);
    segment("__LINKEDIT", 0x100a6b000ULL, 0x44000, 0xa46000, 0x43bb0, 1, 0);
    put32(out.data() + cursor, 0x1b); put32(out.data() + cursor + 4, 24);
    uint8_t uuid[] = {0x9d,0x47,0x27,0xa2,0x1f,0xc2,0x37,0x77,0xbe,0x26,0x7b,0xad,0x0e,0xd6,0x5e,0xf0};
    if (!match24H32) uuid[0] ^= 0xff;
    std::memcpy(out.data() + cursor + 8, uuid, 16); cursor += 24;
    for (unsigned i = 6; i < 76; ++i) {
        const unsigned size = i == 75 ? static_cast<unsigned>(HeaderSize - cursor) : 8;
        put32(out.data() + cursor, 0x777); put32(out.data() + cursor + 4, size); cursor += size;
    }
    assert(cursor == HeaderSize);
    return out;
}

struct SealedDiskAndUBC {
    static constexpr uint32_t FatOffset = 0x4000;
    std::map<uint32_t, Page> diskPages;
    std::map<uint32_t, Page> ubcPages;
    std::map<uint32_t, uint64_t> ssvHashes;

    void sealBlock(uint32_t vnode16kBase) {
        uint8_t block[0x4000] = {};
        for (uint32_t p = 0; p < 0x4000; p += PageSize) {
            uint32_t voff = vnode16kBase + p;
            if (voff >= FatOffset) {
                auto it = diskPages.find(voff - FatOffset);
                if (it != diskPages.end()) std::memcpy(block + p, it->second.data(), PageSize);
            }
        }
        ssvHashes[vnode16kBase] = fnv1a(block, sizeof(block));
    }

    bool pagein(uint32_t slicePageRva) {
        if (ubcPages.count(slicePageRva)) return true;
        const uint32_t vnodeOff = FatOffset + slicePageRva;
        const uint32_t chunk64Base = vnodeOff & ~0xffffU;
        for (uint32_t p = 0; p < 0x10000; p += PageSize) {
            uint32_t vo = chunk64Base + p;
            if (vo < FatOffset) continue;
            uint32_t rva = vo - FatOffset;
            if (!ubcPages.count(rva)) {
                auto it = diskPages.find(rva);
                ubcPages[rva] = (it != diskPages.end()) ? it->second : Page{};
            }
        }
        for (uint32_t b = 0; b < 0x10000; b += 0x4000) {
            uint32_t b16 = chunk64Base + b;
            auto hit = ssvHashes.find(b16);
            if (hit == ssvHashes.end()) continue;
            uint8_t block[0x4000] = {};
            for (uint32_t p = 0; p < 0x4000; p += PageSize) {
                uint32_t vo = b16 + p;
                if (vo >= FatOffset) {
                    auto it = ubcPages.find(vo - FatOffset);
                    if (it != ubcPages.end()) std::memcpy(block + p, it->second.data(), PageSize);
                }
            }
            if (fnv1a(block, sizeof(block)) != hit->second) return false;
        }
        return true;
    }

    void evictPage(uint32_t slicePageRva) { ubcPages.erase(slicePageRva); }
};

struct ProcessMap {
    SealedDiskAndUBC &storage;
    std::map<uint32_t, Page> privateCow;
    std::map<uint32_t, unsigned> pageProt;
    std::set<uint32_t> visitedPages;

    explicit ProcessMap(SealedDiskAndUBC &s) : storage(s) {}

    bool readUser(uint32_t rva, uint8_t *dst, size_t len) {
        size_t done = 0;
        while (done < len) {
            uint32_t addr = rva + static_cast<uint32_t>(done);
            uint32_t pageRva = addr & ~0xfffU;
            visitedPages.insert(pageRva);
            uint32_t inPage = addr & 0xfffU;
            size_t take = PageSize - inPage;
            if (take > len - done) take = len - done;
            auto pit = privateCow.find(pageRva);
            if (pit != privateCow.end()) {
                std::memcpy(dst + done, pit->second.data() + inPage, take);
            } else {
                if (!storage.pagein(pageRva)) return false;
                std::memcpy(dst + done, storage.ubcPages.at(pageRva).data() + inPage, take);
            }
            done += take;
        }
        return true;
    }

    bool writeCow(uint32_t pageRva, const uint8_t *src, bool alreadyPrivate) {
        if (alreadyPrivate) assert(privateCow.count(pageRva) == 1);
        Page p;
        std::memcpy(p.data(), src, PageSize);
        privateCow[pageRva] = p;
        pageProt[pageRva] = 5;
        return true;
    }
};

static void verifyWindowPatch(const uint8_t (&window)[28]) {
    Page page{};
    std::memcpy(page.data() + IntelStandardHCI::IOKitWaitQuietTimeoutPageOffset, window, sizeof(window));

    // First application patches 0xf0 -> 0x08 and leaves all surrounding bytes identical.
    assert(IntelStandardHCI::applyIOKitWaitQuietTimeout(page.data(), page.size()));
    uint8_t expected[28];
    std::memcpy(expected, window, sizeof(window));
    expected[IntelStandardHCI::IOKitWaitQuietTimeoutImmediateOffset] =
        IntelStandardHCI::IOKitWaitQuietTimeoutSeconds;
    assert(page[0xf60] == 0x08);
    assert(std::memcmp(page.data() + IntelStandardHCI::IOKitWaitQuietTimeoutPageOffset,
                       expected, sizeof(expected)) == 0);

    // Idempotent: second application on already-patched page returns false without mutation.
    assert(!IntelStandardHCI::applyIOKitWaitQuietTimeout(page.data(), page.size()));
    assert(std::memcmp(page.data() + IntelStandardHCI::IOKitWaitQuietTimeoutPageOffset,
                       expected, sizeof(expected)) == 0);

    // Rejects nullptr and short buffers.
    assert(!IntelStandardHCI::applyIOKitWaitQuietTimeout(nullptr, page.size()));
    Page shortCopy{};
    std::memcpy(shortCopy.data() + IntelStandardHCI::IOKitWaitQuietTimeoutPageOffset, window, sizeof(window));
    assert(!IntelStandardHCI::applyIOKitWaitQuietTimeout(
        shortCopy.data(),
        IntelStandardHCI::IOKitWaitQuietTimeoutPageOffset + sizeof(IntelStandardHCI::IOKitWaitQuietTimeoutOriginal) - 1));
    assert(shortCopy[0xf60] == 0xf0);

    // Rejects any single-byte alteration in the 24-byte prefix with zero mutation.
    for (size_t i = 0; i < sizeof(IntelStandardHCI::IOKitWaitQuietTimeoutOriginal); ++i) {
        Page mutated{};
        std::memcpy(mutated.data() + IntelStandardHCI::IOKitWaitQuietTimeoutPageOffset, window, sizeof(window));
        mutated[IntelStandardHCI::IOKitWaitQuietTimeoutPageOffset + i] ^= 0x01;
        Page before = mutated;
        assert(!IntelStandardHCI::applyIOKitWaitQuietTimeout(mutated.data(), mutated.size()));
        assert(mutated == before);
    }
}

int main() {
    // 1. Direct unit checks on both 24H32 (82 79 7b 00) and 24H241 (92 79 7b 00) windows.
    verifyWindowPatch(kWindow24H32);
    verifyWindowPatch(kWindow24H241);

    // 2. Verify page 0x2c000 is strictly disjoint from HelperPage, all 12 privacy hooks,
    //    and all existing IntelStandardHCI / IntelAudioClock / base BlueToolFixup pages.
    std::set<uint32_t> existingPages = {
        HelperPage, 0x0cd000U, 0x143000U, 0x1a2000U, 0x1b3000U,
        0x1ca000U, 0x2e6000U, 0x3ea000U, 0x49d000U, 0x618000U, 0x845000U,
    };
    for (size_t i = 0; i < image.hookCount; ++i)
        existingPages.insert(image.hooks[i].fileOffset & ~0xfffU);
    assert(IntelStandardHCI::IOKitWaitQuietTimeoutPage == 0x2c000U);
    assert(IntelStandardHCI::IOKitWaitQuietTimeoutPage < TextEnd);
    assert(existingPages.count(IntelStandardHCI::IOKitWaitQuietTimeoutPage) == 0);

    // 3. End-to-end installCow() on 24H32 layout: visits page 0x2c000 in Phase 2,
    //    patches 0xf0 -> 0x08 into private COW only, keeps APFS SSV UBC pristine across eviction.
    {
        SealedDiskAndUBC s;
        auto hdr = makeHeader(true, TextEnd);
        for (uint32_t off = 0; off < hdr.size(); off += PageSize) {
            Page p{};
            size_t n = (hdr.size() - off < PageSize) ? (hdr.size() - off) : PageSize;
            std::memcpy(p.data(), hdr.data() + off, n);
            s.diskPages[off] = p;
        }
        s.diskPages[HelperPage] = Page{};
        for (size_t i = 0; i < image.hookCount; ++i) {
            const auto &h = image.hooks[i];
            auto &p = s.diskPages[h.fileOffset & ~0xfffU];
            std::memcpy(p.data() + (h.fileOffset & 0xfffU), h.original, h.length);
        }
        for (size_t i = 0; i < image.guardCount; ++i) {
            const auto &g = image.guards[i];
            auto &p = s.diskPages[g.fileOffset & ~0xfffU];
            std::memcpy(p.data() + (g.fileOffset & 0xfffU), g.bytes, g.length);
        }
        std::memcpy(s.diskPages[0x2c000U].data() + IntelStandardHCI::IOKitWaitQuietTimeoutPageOffset,
                    kWindow24H32, sizeof(kWindow24H32));
        s.diskPages[0x2d000U][0x10] = 0x5a; // Sibling page in same 64 KB decmpfs chunk (vnode 0x30000..0x3ffff)

        std::set<uint32_t> blocks;
        for (const auto &kv : s.diskPages) {
            uint32_t voff = SealedDiskAndUBC::FatOffset + kv.first;
            uint32_t chunk64 = voff & ~0xffffU;
            for (uint32_t b = 0; b < 0x10000; b += 0x4000) blocks.insert(chunk64 + b);
        }
        for (uint32_t b : blocks) s.sealBlock(b);

        ProcessMap proc(s);
        std::array<uint8_t, HeaderSize> hdrBuf{};
        Page pageBuf{}, verifyBuf{};
        auto report = installCow(
            image, true,
            [&](uint32_t addr, uint8_t *dst, size_t n) { return proc.readUser(addr, dst, n); },
            [&](uint32_t addr, const uint8_t *src, bool alreadyPriv) { return proc.writeCow(addr, src, alreadyPriv); },
            [](uint8_t *data, size_t size) { return IntelStandardHCI::applyIOKitWaitQuietTimeout(data, size); },
            hdrBuf.data(), pageBuf.data(), verifyBuf.data());

        assert(report.privacyResult == Result::Complete);
        assert(report.privacyPagesWritten == 10);
        assert(report.textPagesPatched == 1);
        assert(report.readFailures == 0 && report.writeFailures == 0);
        assert(proc.visitedPages.count(0x2c000U) == 1);
        assert(proc.privateCow.count(0x2c000U) == 1);
        assert(proc.pageProt.at(0x2c000U) == 5);
        assert(proc.privateCow.at(0x2c000U)[0xf60] == 0x08);
        assert(s.ubcPages.at(0x2c000U)[0xf60] == 0xf0); // Shared UBC remains 100% untouched.

        // Evict sibling page 0x2d000 and verify clean APFS SSV re-pagein without EBADMSG.
        s.evictPage(0x2d000U);
        uint8_t siblingByte = 0;
        assert(proc.readUser(0x2d010U, &siblingByte, 1) && siblingByte == 0x5a);
    }

    // 4. End-to-end installCow() on 24H241 (non-24H32 UUID): skips 24H32 privacy profile,
    //    still visits page 0x2c000 and COW-caps IOKitWaitQuiet 0xf0 -> 0x08 with 0 UBC writes.
    {
        SealedDiskAndUBC s;
        auto hdr = makeHeader(false, 0x30000U);
        for (uint32_t off = 0; off < hdr.size(); off += PageSize) {
            Page p{};
            size_t n = (hdr.size() - off < PageSize) ? (hdr.size() - off) : PageSize;
            std::memcpy(p.data(), hdr.data() + off, n);
            s.diskPages[off] = p;
        }
        std::memcpy(s.diskPages[0x2c000U].data() + IntelStandardHCI::IOKitWaitQuietTimeoutPageOffset,
                    kWindow24H241, sizeof(kWindow24H241));
        s.sealBlock((SealedDiskAndUBC::FatOffset + 0x2c000U) & ~0x3fffU);

        ProcessMap proc(s);
        std::array<uint8_t, HeaderSize> hdrBuf{};
        Page pageBuf{}, verifyBuf{};
        auto report = installCow(
            image, true,
            [&](uint32_t addr, uint8_t *dst, size_t n) { return proc.readUser(addr, dst, n); },
            [&](uint32_t addr, const uint8_t *src, bool alreadyPriv) { return proc.writeCow(addr, src, alreadyPriv); },
            [](uint8_t *data, size_t size) { return IntelStandardHCI::applyIOKitWaitQuietTimeout(data, size); },
            hdrBuf.data(), pageBuf.data(), verifyBuf.data());

        assert(report.privacyResult == Result::Header);
        assert(report.privacyPagesWritten == 0);
        assert(report.textPagesPatched == 1);
        assert(proc.privateCow.at(0x2c000U)[0xf60] == 0x08);
        assert(s.ubcPages.at(0x2c000U)[0xf60] == 0xf0);
    }

    std::puts("BlueToolFixup 2.7.4 IOKitWaitQuiet 8s COW cap tests passed");
    return 0;
}
"""


class IntelBTIOKitWaitQuietTests(unittest.TestCase):
    def test_iokit_wait_quiet_timeout_cap_24h32_and_24h241_cow(self):
        with tempfile.TemporaryDirectory(prefix="intel-bt-iokit-wait-") as directory:
            target = pathlib.Path(directory)
            subprocess.run(
                ["python3", str(ROOT / "scripts/build-intel-privacy.py"), "--output-dir", str(target)],
                check=True,
                capture_output=True,
                text=True,
            )
            src = target / "fixture.cpp"
            src.write_text(FIXTURE_CPP)
            for opt in ("-O0", "-O2"):
                with self.subTest(optimization=opt):
                    exe = target / ("iokit_wait_fixture" + opt)
                    subprocess.run(
                        [
                            "clang++", "-arch", "x86_64", "-std=c++17", opt,
                            "-Wall", "-Wextra", "-Werror", "-g",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I", str(ROOT), "-I", str(target),
                            str(src), "-o", str(exe),
                        ],
                        check=True,
                        capture_output=True,
                        text=True,
                    )
                    res = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=30)
                    self.assertIn("BlueToolFixup 2.7.4 IOKitWaitQuiet 8s COW cap tests passed", res.stdout)

    def test_bluetool_patch_and_version_wiring(self):
        patch_text = (ROOT / "bluetooth/patches/bluetool-intel-standard-hci.patch").read_text()
        self.assertIn("IntelStandardHCI::applyIOKitWaitQuietTimeout(data, size)", patch_text)
        build_text = (ROOT / "scripts/build-bluetooth.py").read_text()
        self.assertIn("version = '2.7.4' if args.component == 'bluetool'", build_text)


if __name__ == "__main__":
    unittest.main()
