"""Verify BlueToolFixup 2.7.3 per-process COW vm_map patching keeps APFS SSV UBC clean."""

import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]

FIXTURE_CPP = r"""
#include "bluetooth/BlueToolFixup/IntelPrivacyInstaller.hpp"
#include "bluetooth/BlueToolFixup/IntelStandardHCI.hpp"
#include "bluetooth/BlueToolFixup/IntelAudioClock.hpp"
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

static const uint8_t kVendorCheckOriginal[] = {0x81, 0xFA, 0x5C, 0x0A, 0x00, 0x00, 0x74};
static const uint8_t kVendorCheckPatched[]  = {0x81, 0xFA, 0x5C, 0x0A, 0x00, 0x00, 0xEB};
static const uint8_t kBadChipset154Orig[]   = {0x81, 0xF9, 0x6F, 0x17, 0x00, 0x00, 0x77, 0x30};
static const uint8_t kBadChipset154Patch[]  = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

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

static bool findAndReplace(uint8_t *buf, size_t len, const uint8_t *orig, const uint8_t *repl, size_t n) {
    for (size_t i = 0; i + n <= len; ++i) {
        if (std::memcmp(buf + i, orig, n) == 0) {
            std::memcpy(buf + i, repl, n);
            return true;
        }
    }
    return false;
}

// Simulates APFS SSV decmpfs 64 KB chunks (fat_offset=0x4000) with shared UBC cache + 16 KB Merkle verification.
struct SealedDiskAndUBC {
    static constexpr uint32_t FatOffset = 0x4000;
    std::map<uint32_t, Page> diskPages;     // Clean sealed APFS disk pages keyed by slice RVA
    std::map<uint32_t, Page> ubcPages;      // Resident shared vnode UBC pages keyed by slice RVA
    std::map<uint32_t, uint64_t> ssvHashes; // Expected 16 KB SSV block hashes keyed by vnode 16 KB block base

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

    // Mirrors decmpfs_pagein_compressed + apfs_verify_uncompressed_data:
    // skips resident UBC pages in the 64 KB chunk, fills missing pages from disk, and verifies every 16 KB SSV block.
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
            if (fnv1a(block, sizeof(block)) != hit->second) {
                return false; // EBADMSG (94)
            }
        }
        return true;
    }

    void evictPage(uint32_t slicePageRva) { ubcPages.erase(slicePageRva); }
};

struct ProcessMap {
    SealedDiskAndUBC &storage;
    std::map<uint32_t, Page> privateCow;
    std::map<uint32_t, unsigned> pageProt; // 5 = RX, 7 = RWX
    uint32_t cowCopyCount = 0;
    uint32_t privateRewriteCount = 0;
    bool csFlagsUntouched = true;

    explicit ProcessMap(SealedDiskAndUBC &s) : storage(s) {}

    bool readUser(uint32_t rva, uint8_t *dst, size_t len) {
        size_t done = 0;
        while (done < len) {
            uint32_t addr = rva + static_cast<uint32_t>(done);
            uint32_t pageRva = addr & ~0xfffU;
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
        if (alreadyPrivate) {
            assert(privateCow.count(pageRva) == 1);
            ++privateRewriteCount;
        } else {
            ++cowCopyCount;
        }
        Page p;
        std::memcpy(p.data(), src, PageSize);
        privateCow[pageRva] = p;
        pageProt[pageRva] = 5; // Restored strictly to VM_PROT_READ | VM_PROT_EXECUTE
        return true;
    }
};

static SealedDiskAndUBC build24H32Image() {
    SealedDiskAndUBC s;
    auto hdr = makeHeader(true);
    for (uint32_t off = 0; off < hdr.size(); off += PageSize) {
        Page p{};
        size_t n = (hdr.size() - off < PageSize) ? (hdr.size() - off) : PageSize;
        std::memcpy(p.data(), hdr.data() + off, n);
        s.diskPages[off] = p;
    }
    // HelperPage (0x9da000 -> vnode 0x9de000) and sibling __DATA_CONST page 0 (0x9db000 -> vnode 0x9df000)
    s.diskPages[HelperPage] = Page{};
    Page dataConst0{};
    dataConst0[0xfe0] = 0x42; // Simulates pointer read by initializer bluetoothd+0x334da8
    s.diskPages[0x9db000] = dataConst0;

    // Populate all 12 privacy hooks + guards
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

    // Dual-patched page 0x143000: EventMaskGetter sits alongside PrivacyHook reset_start/reset_recovery!
    std::memcpy(s.diskPages[0x143000].data() + IntelStandardHCI::EventMaskGetterPageOffset,
                IntelStandardHCI::EventMaskGetterOriginal, sizeof(IntelStandardHCI::EventMaskGetterOriginal));
    // Other 24H32 IntelStandardHCI / IntelAudioClock / base BlueToolFixup sites
    std::memcpy(s.diskPages[0x2e6000].data() + IntelStandardHCI::PageOffset,
                IntelStandardHCI::Original, sizeof(IntelStandardHCI::Original));
    std::memcpy(s.diskPages[0x618000].data() + IntelStandardHCI::PowerQueryPageOffset,
                IntelStandardHCI::PowerQueryOriginal, sizeof(IntelStandardHCI::PowerQueryOriginal));
    std::memcpy(s.diskPages[0x3ea000].data() + IntelAudioClock::PageOffset,
                IntelAudioClock::Original, sizeof(IntelAudioClock::Original));
    std::memcpy(s.diskPages[0x845000].data() + IntelStandardHCI::EventMasksPageOffset,
                IntelStandardHCI::EventMasksOriginal, sizeof(IntelStandardHCI::EventMasksOriginal));
    std::memcpy(s.diskPages[0x49d000].data() + IntelStandardHCI::MultiAdvRandomAddressPageOffset,
                IntelStandardHCI::MultiAdvRandomAddressOriginal, sizeof(IntelStandardHCI::MultiAdvRandomAddressOriginal));
    std::memcpy(s.diskPages[0x1ca000].data() + IntelStandardHCI::AdvSetRandomAddressPageOffset,
                IntelStandardHCI::AdvSetRandomAddressOriginal, sizeof(IntelStandardHCI::AdvSetRandomAddressOriginal));
    std::memcpy(s.diskPages[0x1a2000].data() + IntelStandardHCI::ThirdPartyForce1MPageOffset,
                IntelStandardHCI::ThirdPartyForce1MOriginal, sizeof(IntelStandardHCI::ThirdPartyForce1MOriginal));
    std::memcpy(s.diskPages[0x0cd000].data() + 0x7f4, kVendorCheckOriginal, sizeof(kVendorCheckOriginal));
    std::memcpy(s.diskPages[0x1b3000].data() + 0xa59, kBadChipset154Orig, sizeof(kBadChipset154Orig));

    // Seal all 16 KB SSV blocks covering every populated page + 0x9dc000..0x9dffff
    std::set<uint32_t> blocks;
    for (const auto &kv : s.diskPages) {
        uint32_t voff = SealedDiskAndUBC::FatOffset + kv.first;
        uint32_t chunk64 = voff & ~0xffffU;
        for (uint32_t b = 0; b < 0x10000; b += 0x4000) blocks.insert(chunk64 + b);
    }
    for (uint32_t b : blocks) s.sealBlock(b);
    return s;
}

static bool patch24H32TextPage(uint8_t *data, size_t size) {
    bool patched = false;
    if (IntelStandardHCI::apply(data, size)) patched = true;
    if (IntelStandardHCI::applyPowerQuery(data, size)) patched = true;
    if (IntelAudioClock::apply(data, size)) patched = true;
    if (IntelStandardHCI::applyEventMasks(data, size)) patched = true;
    if (IntelStandardHCI::applyEventMaskGetter(data, size)) patched = true;
    if (IntelStandardHCI::applyMultiAdvRandomAddress(data, size)) patched = true;
    if (IntelStandardHCI::applyAdvSetRandomAddress(data, size)) patched = true;
    if (IntelStandardHCI::applyThirdPartyForce1M(data, size)) patched = true;
    if (findAndReplace(data, size, kVendorCheckOriginal, kVendorCheckPatched, sizeof(kVendorCheckOriginal))) patched = true;
    if (findAndReplace(data, size, kBadChipset154Orig, kBadChipset154Patch, sizeof(kBadChipset154Orig))) patched = true;
    return patched;
}

int main() {
    // 1. Negative control: prove the old in-place UBC mutation reproduces EBADMSG (94) after 0x9db000 eviction.
    {
        auto s = build24H32Image();
        assert(s.pagein(HelperPage)); // Loads clean 0x9d0000..0x9dffff including 0x9db000
        assert(image.apply(HelperPage, s.ubcPages[HelperPage].data(), PageSize, true, false, true, false, false));
        s.evictPage(0x9db000);        // Overnight VM reclamation of COW'd __DATA_CONST page 0
        assert(!s.pagein(0x9db000));  // Reproduces apfs_verify_uncompressed_data EBADMSG (94)
    }

    // 2. BT-FIX-A: Boot PID #1 + 10 post-sleep/wake respawns with aggressive overnight eviction of 0x9db000
    //    and cold sibling __TEXT pages inside every patched 64 KB decmpfs chunk.
    {
        auto s = build24H32Image();
        std::array<uint8_t, HeaderSize> hdrBuf {};
        Page pageBuf {}, verifyBuf {};

        for (int launch = 1; launch <= 10; ++launch) {
            if (launch > 1) {
                // Evict __DATA_CONST page 0 (vnode 0x9df000) and sibling cold pages in patched chunks.
                s.evictPage(0x9db000);
                s.evictPage(0x140000);
                s.evictPage(0x2e0000);
                s.evictPage(0x690000);
            }
            ProcessMap proc(s);
            auto report = installCow(
                image, true,
                [&](uint32_t addr, uint8_t *dst, size_t n) { return proc.readUser(addr, dst, n); },
                [&](uint32_t addr, const uint8_t *src, bool alreadyPriv) { return proc.writeCow(addr, src, alreadyPriv); },
                patch24H32TextPage,
                hdrBuf.data(), pageBuf.data(), verifyBuf.data());

            assert(report.privacyResult == Result::Complete);
            assert(report.textSize == TextEnd);
            assert(report.privacyPagesWritten == 10); // HelperPage + 9 unique hook pages
            assert(report.textPagesPatched == 10);    // 8 IntelStandardHCI/AudioClock + Vendor + BadChipset15_4
            assert(report.readFailures == 0 && report.writeFailures == 0);
            assert(proc.privateRewriteCount == 1);    // Dual-patched page 0x143000 reused private COW without 2nd remap
            assert(proc.csFlagsUntouched);

            // Verify every private COW page is strictly RX and UBC remains 100% pristine.
            for (const auto &kv : proc.pageProt) assert(kv.second == 5);
            for (const auto &kv : s.ubcPages) {
                auto dit = s.diskPages.find(kv.first);
                Page expected = (dit != s.diskPages.end()) ? dit->second : Page{};
                assert(kv.second == expected);
            }

            // Simulate dyld initializer faulting __DATA_CONST page 0 (+0x9dbfe0) after overnight eviction!
            uint8_t byteAtFe0 = 0;
            assert(proc.readUser(0x9dbfe0, &byteAtFe0, 1) && byteAtFe0 == 0x42);

            // Verify dual-patched page 0x143000 retains BOTH privacy hooks AND EventMaskGetter 0x21a5f!
            Page p143 {};
            assert(proc.readUser(0x143000, p143.data(), PageSize));
            assert(image.pageMatches(0x143000, p143.data(), true));
            uint32_t mask = read32(p143.data() + IntelStandardHCI::EventMaskGetterPageOffset +
                                   IntelStandardHCI::EventMaskImmediateOffset);
            assert(mask == IntelStandardHCI::StandardEventMask);

            // Verify HelperPage is patched privately while UBC HelperPage stays all-zero in slack.
            Page helper {};
            assert(proc.readUser(HelperPage, helper.data(), PageSize));
            assert(image.helperMatches(helper.data()));
            assert(zero(s.ubcPages[HelperPage].data() + (HelperOffset & 0xfffU), TextEnd - HelperOffset));
        }
    }

    // 3. Atomic phase-1a abort: corrupted guard on last hook page writes 0 privacy pages.
    {
        auto s = build24H32Image();
        const auto &g = image.guards[image.guardCount - 1];
        s.diskPages[g.fileOffset & ~0xfffU][g.fileOffset & 0xfffU] ^= 1;
        uint32_t voff = SealedDiskAndUBC::FatOffset + (g.fileOffset & ~0xfffU);
        s.sealBlock(voff & ~0x3fffU);
        ProcessMap proc(s);
        std::array<uint8_t, HeaderSize> hdrBuf {};
        Page pageBuf {}, verifyBuf {};
        auto report = installCow(
            image, true,
            [&](uint32_t addr, uint8_t *dst, size_t n) { return proc.readUser(addr, dst, n); },
            [&](uint32_t addr, const uint8_t *src, bool alreadyPriv) { return proc.writeCow(addr, src, alreadyPriv); },
            [](uint8_t *, size_t) { return false; },
            hdrBuf.data(), pageBuf.data(), verifyBuf.data());
        assert(report.privacyResult == Result::Hook);
        assert(report.privacyPagesWritten == 0 && proc.cowCopyCount == 0);
    }

    // 4. Non-24H32 Sequoia build: skips privacy profile cleanly, still COW-patches base BlueToolFixup with 0 UBC writes.
    {
        SealedDiskAndUBC s;
        auto hdr = makeHeader(false, 0x20000);
        for (uint32_t off = 0; off < hdr.size(); off += PageSize) {
            Page p{};
            size_t n = (hdr.size() - off < PageSize) ? (hdr.size() - off) : PageSize;
            std::memcpy(p.data(), hdr.data() + off, n);
            s.diskPages[off] = p;
        }
        std::memcpy(s.diskPages[0x10000].data() + 0x100, kVendorCheckOriginal, sizeof(kVendorCheckOriginal));
        s.sealBlock((SealedDiskAndUBC::FatOffset + 0x10000) & ~0x3fffU);

        ProcessMap proc(s);
        std::array<uint8_t, HeaderSize> hdrBuf {};
        Page pageBuf {}, verifyBuf {};
        auto report = installCow(
            image, true,
            [&](uint32_t addr, uint8_t *dst, size_t n) { return proc.readUser(addr, dst, n); },
            [&](uint32_t addr, const uint8_t *src, bool alreadyPriv) { return proc.writeCow(addr, src, alreadyPriv); },
            patch24H32TextPage,
            hdrBuf.data(), pageBuf.data(), verifyBuf.data());
        assert(report.privacyResult == Result::Header);
        assert(report.privacyPagesWritten == 0);
        assert(report.textSize == 0x20000);
        assert(report.textPagesPatched == 1);
        assert(std::memcmp(s.ubcPages[0x10000].data() + 0x100, kVendorCheckOriginal, sizeof(kVendorCheckOriginal)) == 0);
        assert(std::memcmp(proc.privateCow[0x10000].data() + 0x100, kVendorCheckPatched, sizeof(kVendorCheckPatched)) == 0);
    }

    std::puts("BlueToolFixup SSV COW multi-respawn and isolation tests passed");
    return 0;
}
"""


class IntelBTSSVCowTests(unittest.TestCase):
    def test_cow_installer_preserves_apfs_ssv_across_respawns(self):
        with tempfile.TemporaryDirectory(prefix="intel-bt-cow-") as directory:
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
                    exe = target / ("cow_fixture" + opt)
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
                    self.assertIn("BlueToolFixup SSV COW multi-respawn and isolation tests passed", res.stdout)

    def test_bluetool_patch_eliminates_bluetoothd_ubc_mutation_when_cow_active(self):
        patch_text = (ROOT / "bluetooth/patches/bluetool-intel-standard-hci.patch").read_text()
        self.assertNotIn("IntelPrivacyInstaller::onPage", patch_text)
        self.assertIn("if (IntelPrivacyInstaller::cowActive())", patch_text)
        self.assertIn("IntelPrivacyInstaller::start(patcher, patchBluetoothdCowPage, enablePrivacy)", patch_text)


if __name__ == "__main__":
    unittest.main()
