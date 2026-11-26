// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace IntelPrivacyInstaller {
static constexpr uint32_t PageSize = 0x1000;
static constexpr uint32_t HeaderSize = 0x2440;
static constexpr uint32_t HelperPage = 0x9da000;
static constexpr uint32_t HelperOffset = 0x9da370;
static constexpr uint32_t TextEnd = 0x9db000;
static constexpr uint32_t RuntimeOffset = 0xa6aef8;
static constexpr uint32_t RuntimeSize = 24;
static constexpr size_t RequiredHooks = 12;

inline uint32_t read32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
        static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}
inline uint64_t read64(const uint8_t *p) {
    return read32(p) | static_cast<uint64_t>(read32(p + 4)) << 32;
}
inline bool equal(const uint8_t *a, const uint8_t *b, size_t size) {
    for (size_t i = 0; i < size; ++i)
        if (a[i] != b[i]) return false;
    return true;
}
inline bool zero(const uint8_t *p, size_t size) {
    for (size_t i = 0; i < size; ++i)
        if (p[i]) return false;
    return true;
}
inline bool name(const uint8_t *p, const char *s) {
    size_t i = 0;
    for (; i < 16 && s[i]; ++i)
        if (p[i] != static_cast<uint8_t>(s[i])) return false;
    return i == 16 || zero(p + i, 16 - i);
}

// The two unused ranges are established from the executable's load commands.
inline bool layout(const uint8_t *header, size_t size) {
    static constexpr uint8_t uuid[] = {
        0x9d, 0x47, 0x27, 0xa2, 0x1f, 0xc2, 0x37, 0x77,
        0xbe, 0x26, 0x7b, 0xad, 0x0e, 0xd6, 0x5e, 0xf0
    };
    if (!header || size < HeaderSize || read32(header) != 0xfeedfacf ||
        read32(header + 4) != 0x1000007 || read32(header + 8) != 3 ||
        read32(header + 12) != 2 || read32(header + 16) != 76 ||
        read32(header + 20) != HeaderSize - 32)
        return false;
    unsigned seen = 0;
    size_t cursor = 32;
    for (unsigned i = 0; i < 76; ++i) {
        if (cursor > HeaderSize - 8) return false;
        auto c = header + cursor;
        const uint32_t command = read32(c), length = read32(c + 4);
        if (length < 8 || (length & 7) || length > HeaderSize - cursor) return false;
        if (command == 0x1b) {
            if (length != 24 || (seen & 1) || !equal(c + 8, uuid, sizeof(uuid))) return false;
            seen |= 1;
        } else if (command == 0x19) {
            if (length < 72) return false;
            uint32_t sections = read32(c + 64);
            if (sections > 32 || length != 72 + sections * 80) return false;
            const uint64_t address = read64(c + 24), vmSize = read64(c + 32);
            const uint64_t fileOffset = read64(c + 40), fileSize = read64(c + 48);
            unsigned bit = 0;
            if (name(c + 8, "__PAGEZERO")) {
                bit = 2;
                if (address || vmSize != 0x100000000ULL || fileOffset || fileSize ||
                    read32(c + 56) || read32(c + 60) || sections) return false;
            } else if (name(c + 8, "__TEXT")) {
                bit = 4;
                if (address != 0x100000000ULL || vmSize != TextEnd || fileOffset ||
                    fileSize != TextEnd || read32(c + 56) != 5 ||
                    read32(c + 60) != 5 || sections != 14) return false;
            } else if (name(c + 8, "__DATA_CONST")) {
                bit = 8;
                if (address != 0x1009db000ULL || vmSize != 0x4b000 ||
                    fileOffset != 0x9db000 || fileSize != 0x4b000 ||
                    read32(c + 56) != 3 || read32(c + 60) != 3 || sections != 12) return false;
            } else if (name(c + 8, "__DATA")) {
                bit = 16;
                if (address != 0x100a26000ULL || vmSize != 0x45000 ||
                    fileOffset != 0xa26000 || fileSize != 0x20000 ||
                    read32(c + 56) != 3 || read32(c + 60) != 3 || sections != 8) return false;
            } else if (name(c + 8, "__LINKEDIT")) {
                bit = 32;
                if (address != 0x100a6b000ULL || vmSize != 0x44000 ||
                    fileOffset != 0xa46000 || fileSize != 0x43bb0 ||
                    read32(c + 56) != 1 || read32(c + 60) != 1 || sections) return false;
            } else return false;
            if (seen & bit) return false;
            seen |= bit;
            for (uint32_t j = 0; j < sections; ++j) {
                auto section = c + 72 + j * 80;
                const auto start = read64(section + 32), extent = read64(section + 40);
                if (start < address || start - address > vmSize ||
                    extent > vmSize - (start - address)) return false;
                if (bit == 4 && start + extent > 0x100000000ULL + HelperOffset) return false;
                if (bit == 16 && start + extent > 0x100000000ULL + RuntimeOffset) return false;
                if (bit == 4 && j == 13 &&
                    (!name(section, "__eh_frame") || start != 0x1009da318ULL || extent != 0x58))
                    return false;
                if (bit == 16 && j == 7 &&
                    (!name(section, "__common") || start != 0x100a649d0ULL ||
                     extent != 0x6528 || (read32(section + 64) & 0xff) != 1)) return false;
            }
        }
        cursor += length;
    }
    return seen == 63 && cursor == HeaderSize;
}

inline uint32_t parseTextSegmentSize(const uint8_t *header, size_t size) {
    if (!header || size < 32 || read32(header) != 0xfeedfacf ||
        read32(header + 4) != 0x1000007 || read32(header + 12) != 2)
        return 0;
    const uint32_t ncmds = read32(header + 16);
    const uint32_t sizeofcmds = read32(header + 20);
    const size_t cmdEnd = 32 + static_cast<size_t>(sizeofcmds);
    const size_t limit = size < cmdEnd ? size : cmdEnd;
    size_t cursor = 32;
    for (uint32_t i = 0; i < ncmds && cursor + 8 <= limit; ++i) {
        auto c = header + cursor;
        const uint32_t command = read32(c), length = read32(c + 4);
        if (length < 8 || (length & 7) || length > limit - cursor)
            return 0;
        if (command == 0x19 && length >= 72 && name(c + 8, "__TEXT")) {
            const uint64_t vmSize = read64(c + 32);
            const uint64_t fileOffset = read64(c + 40);
            const uint64_t fileSize = read64(c + 48);
            if (fileOffset != 0 || vmSize == 0 || vmSize != fileSize ||
                (vmSize & (PageSize - 1)) || vmSize > 64U * 1024U * 1024U)
                return 0;
            return static_cast<uint32_t>(vmSize);
        }
        cursor += length;
    }
    return 0;
}

template <typename Hook, typename Guard>
struct Image {
    const uint8_t *bytes;
    size_t size;
    const Hook *hooks;
    size_t hookCount;
    const Guard *guards;
    size_t guardCount;

    bool valid() const {
        if (!bytes || !size || size > TextEnd - HelperOffset || !hooks ||
            hookCount != RequiredHooks || !guards || !guardCount) return false;
        for (size_t i = 0; i < hookCount; ++i) {
            const auto &h = hooks[i];
            if (!h.length || h.length > 16 || h.fileOffset < 0x2550 ||
                h.fileOffset >= 0x7e4372 || h.length > 0x7e4372 - h.fileOffset ||
                (h.fileOffset & 0xfff) + h.length > PageSize) return false;
            for (size_t j = 0; j < i; ++j)
                if (h.fileOffset < hooks[j].fileOffset + hooks[j].length &&
                    hooks[j].fileOffset < h.fileOffset + h.length) return false;
        }
        for (size_t i = 0; i < guardCount; ++i) {
            const auto &g = guards[i];
            if (!g.length || g.length > 32 || g.fileOffset >= HelperOffset ||
                (g.fileOffset & 0xfff) + g.length > PageSize) return false;
            for (size_t j = 0; j < hookCount; ++j)
                if (g.fileOffset < hooks[j].fileOffset + hooks[j].length &&
                    hooks[j].fileOffset < g.fileOffset + g.length) return false;
        }
        return true;
    }

    bool helperMatches(const uint8_t *page) const {
        return equal(page + (HelperOffset & 0xfff), bytes, size) &&
            zero(page + (HelperOffset & 0xfff) + size, TextEnd - HelperOffset - size);
    }

    bool pageMatches(uint32_t offset, const uint8_t *page, bool patched) const {
        bool found = false;
        for (size_t i = 0; i < guardCount; ++i) {
            const auto &g = guards[i];
            if ((g.fileOffset & ~0xfffU) == offset &&
                !equal(page + (g.fileOffset & 0xfff), g.bytes, g.length)) return false;
        }
        for (size_t i = 0; i < hookCount; ++i) {
            const auto &h = hooks[i];
            if ((h.fileOffset & ~0xfffU) != offset) continue;
            found = true;
            auto p = page + (h.fileOffset & 0xfff);
            if (!equal(p, h.replacement, h.length) &&
                (patched || !equal(p, h.original, h.length))) return false;
        }
        return found;
    }

    // A page is checked as a group before any byte is changed. Revalidation
    // accepts the same completed image, never a partially overwritten hook.
    bool apply(uint64_t offset, uint8_t *page, size_t pageSize,
               bool profile, bool helperReady, bool validated, bool tainted, bool nx) const {
        if (!profile || !validated || tainted || nx || !page || pageSize != PageSize ||
            (offset & 0xfff) || offset >= TextEnd || !valid()) return false;
        if (offset == HelperPage) {
            if (helperMatches(page)) return true;
            if (!zero(page + (HelperOffset & 0xfff), TextEnd - HelperOffset)) return false;
            for (size_t i = 0; i < size; ++i) page[(HelperOffset & 0xfff) + i] = bytes[i];
            return true;
        }
        if (!helperReady || !pageMatches(static_cast<uint32_t>(offset), page, false)) return false;
        for (size_t i = 0; i < hookCount; ++i) {
            const auto &h = hooks[i];
            if ((h.fileOffset & ~0xfffU) != offset) continue;
            for (size_t j = 0; j < h.length; ++j)
                page[(h.fileOffset & 0xfff) + j] = h.replacement[j];
        }
        return true;
    }
};

enum class Result : unsigned {
    Complete, Image, Header, Runtime, Helper, Profile, Hook
};

struct CowReport {
    Result privacyResult;
    uint32_t textSize;
    uint32_t privacyPagesWritten;
    uint32_t textPagesPatched;
    uint32_t readFailures;
    uint32_t writeFailures;
};

template <typename Hook, typename Guard, typename Read, typename AllowHelper, typename Bind>
Result prefault(const Image<Hook, Guard> &image, Read read, AllowHelper allowHelper,
                Bind bind, uint8_t *buffer) {
    if (!buffer || !image.valid()) return Result::Image;
    if (!read(0, buffer, HeaderSize) || !layout(buffer, HeaderSize)) return Result::Header;
    if (!read(RuntimeOffset, buffer, RuntimeSize) || !zero(buffer, RuntimeSize)) return Result::Runtime;
    allowHelper();
    if (!read(HelperPage, buffer, PageSize) || !image.helperMatches(buffer)) return Result::Helper;
    // Publishing keeps helper replay available after this exec callback returns.
    if (!bind()) return Result::Profile;
    for (size_t i = 0; i < image.hookCount; ++i) {
        const uint32_t page = image.hooks[i].fileOffset & ~0xfffU;
        bool seen = false;
        for (size_t j = 0; j < i; ++j)
            if ((image.hooks[j].fileOffset & ~0xfffU) == page) seen = true;
        if (!seen && (!read(page, buffer, PageSize) || !image.pageMatches(page, buffer, true)))
            return Result::Hook;
    }
    return Result::Complete;
}

template <typename Hook, typename Guard, typename Read, typename WriteCow, typename PatchTextPage>
CowReport installCow(const Image<Hook, Guard> &image, bool allowPrivacy,
                     Read read, WriteCow writeCow, PatchTextPage patchTextPage,
                     uint8_t *headerBuffer, uint8_t *pageBuffer, uint8_t *verifyBuffer) {
    CowReport report {Result::Header, 0, 0, 0, 0, 0};
    if (!headerBuffer || !pageBuffer || !verifyBuffer) {
        report.privacyResult = Result::Image;
        return report;
    }
    if (!read(0, headerBuffer, PageSize)) {
        ++report.readFailures;
        return report;
    }
    const uint32_t textSize = parseTextSegmentSize(headerBuffer, PageSize);
    if (!textSize) return report;
    report.textSize = textSize;

    // Track pages made private in phase 1 so phase 2 avoids redundant VM_PROT_COPY remaps.
    uint32_t privatePages[RequiredHooks + 1];
    size_t privateCount = 0;
    auto isPrivatePage = [&](uint32_t offset) {
        for (size_t i = 0; i < privateCount; ++i)
            if (privatePages[i] == offset) return true;
        return false;
    };

    if (allowPrivacy) {
        if (!image.valid()) {
            report.privacyResult = Result::Image;
        } else if (textSize != TextEnd || !read(0, headerBuffer, HeaderSize) ||
                   !layout(headerBuffer, HeaderSize)) {
            report.privacyResult = Result::Header;
        } else if (!read(RuntimeOffset, pageBuffer, RuntimeSize) ||
                   !zero(pageBuffer, RuntimeSize)) {
            report.privacyResult = Result::Runtime;
        } else if (!read(HelperPage, pageBuffer, PageSize) ||
                   !zero(pageBuffer + (HelperOffset & 0xfff), TextEnd - HelperOffset)) {
            report.privacyResult = Result::Helper;
        } else {
            // Phase 1a: validate every hook page before writing any private page.
            bool hooksValid = true;
            for (size_t i = 0; i < image.hookCount; ++i) {
                const uint32_t page = image.hooks[i].fileOffset & ~0xfffU;
                bool seen = false;
                for (size_t j = 0; j < i; ++j)
                    if ((image.hooks[j].fileOffset & ~0xfffU) == page) seen = true;
                if (seen) continue;
                if (!read(page, pageBuffer, PageSize) || !image.pageMatches(page, pageBuffer, false)) {
                    hooksValid = false;
                    break;
                }
            }
            if (!hooksValid) {
                report.privacyResult = Result::Hook;
            } else {
                // Phase 1b: commit HelperPage first, then all hook pages into private COW pages.
                if (!read(HelperPage, pageBuffer, PageSize) ||
                    !image.apply(HelperPage, pageBuffer, PageSize, true, false, true, false, false) ||
                    !writeCow(HelperPage, pageBuffer, false) ||
                    !read(HelperPage, verifyBuffer, PageSize) ||
                    !image.helperMatches(verifyBuffer)) {
                    ++report.writeFailures;
                    report.privacyResult = Result::Helper;
                } else {
                    privatePages[privateCount++] = HelperPage;
                    ++report.privacyPagesWritten;
                    bool committed = true;
                    for (size_t i = 0; i < image.hookCount; ++i) {
                        const uint32_t page = image.hooks[i].fileOffset & ~0xfffU;
                        bool seen = false;
                        for (size_t j = 0; j < i; ++j)
                            if ((image.hooks[j].fileOffset & ~0xfffU) == page) seen = true;
                        if (seen) continue;
                        if (!read(page, pageBuffer, PageSize) ||
                            !image.apply(page, pageBuffer, PageSize, true, true, true, false, false) ||
                            !writeCow(page, pageBuffer, false) ||
                            !read(page, verifyBuffer, PageSize) ||
                            !image.pageMatches(page, verifyBuffer, true)) {
                            ++report.writeFailures;
                            committed = false;
                            break;
                        }
                        if (privateCount < RequiredHooks + 1)
                            privatePages[privateCount++] = page;
                        ++report.privacyPagesWritten;
                    }
                    report.privacyResult = committed ? Result::Complete : Result::Hook;
                }
            }
        }
    }

    // Phase 2: apply standard/base bluetoothd __TEXT patches into private COW pages only.
    for (uint32_t offset = 0; offset < textSize; offset += PageSize) {
        if (!read(offset, pageBuffer, PageSize)) {
            ++report.readFailures;
            continue;
        }
        if (patchTextPage(pageBuffer, PageSize)) {
            if (writeCow(offset, pageBuffer, isPrivatePage(offset))) {
                ++report.textPagesPatched;
            } else {
                ++report.writeFailures;
            }
        }
    }
    return report;
}
} // namespace IntelPrivacyInstaller

#ifdef KERNEL
#include "IntelPrivacyImage.hpp"
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <mach/vm_prot.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/vnode.h>

namespace IntelPrivacyInstaller {
static const Image<IntelPrivacyImage::HookPatch, IntelPrivacyImage::Guard> nativeImage {
    IntelPrivacyImage::ImageBytes, IntelPrivacyImage::ImageSize,
    IntelPrivacyImage::Hooks, IntelPrivacyImage::HookCount,
    IntelPrivacyImage::Guards, IntelPrivacyImage::GuardCount
};
static_assert(IntelPrivacyImage::ImageFileOffset == HelperOffset, "helper location");
static_assert(IntelPrivacyImage::RuntimeVmOffset == RuntimeOffset, "runtime location");

using TextPagePatcher = bool (*)(uint8_t *page, size_t pageSize);
using GetTaskMap = vm_map_t (*)(task_t);
using GetMapMin = vm_map_offset_t (*)(vm_map_t);
using ReadUser = kern_return_t (*)(vm_map_t, vm_map_address_t, const void *, vm_size_t);
using WriteUser = kern_return_t (*)(vm_map_t, const void *, vm_map_address_t, vm_size_t);
using VmProtect = kern_return_t (*)(vm_map_t, vm_offset_t, vm_size_t, boolean_t, vm_prot_t);
using VmMapFlagSetter = void (*)(vm_map_t, boolean_t);
using GetTaskProc = proc_t (*)(task_t);
using GetExecVnode = vnode_t (*)(proc_t);
using GetExecOffset = int64_t (*)(proc_t);

static GetTaskMap getTaskMap;
static GetMapMin getMapMin;
static ReadUser readUser;
static WriteUser writeUser;
static VmProtect vmProtect;
static VmMapFlagSetter vmMapSwitchProtect;
static VmMapFlagSetter vmMapCsEnforcementSet;
static VmMapFlagSetter vmMapCsDebuggedSet;
static GetTaskProc getTaskProc;
static GetExecVnode getExecVnode;
static GetExecOffset getExecOffset;
static TextPagePatcher textPagePatcher;
static bool privacyProfileEnabled;
static _Atomic(bool) cowEnabled;
static mach_vm_address_t originalTaskQos;
static kauth_listener_t execListenerHandle;
static ThreadLocal<vnode_t, 8> pendingExec;

inline bool cowActive() {
    return atomic_load_explicit(&cowEnabled, memory_order_acquire);
}

static int execListener(kauth_cred_t, void *, kauth_action_t action,
                        uintptr_t arg0, uintptr_t arg1, uintptr_t, uintptr_t) {
    if (action != KAUTH_FILEOP_EXEC) return KAUTH_RESULT_DEFER;
    if (auto stale = pendingExec.get()) {
        auto node = *stale;
        pendingExec.erase();
        if (node) vnode_rele(node);
    }
    if (arg0 && arg1 && strcmp(reinterpret_cast<const char *>(arg1), "/usr/sbin/bluetoothd") == 0) {
        auto node = reinterpret_cast<vnode_t>(arg0);
        if (vnode_ref(node) == 0 && !pendingExec.set(node)) vnode_rele(node);
    }
    return KAUTH_RESULT_DEFER;
}

static void taskQos(task_t task, thread_t mainThread) {
    FunctionCast(taskQos, originalTaskQos)(task, mainThread);
    auto pending = pendingExec.get();
    if (!pending) return;
    auto node = *pending;
    pendingExec.erase();
    CowReport report {Result::Profile, 0, 0, 0, 0, 0};
    auto process = getTaskProc(task);
    auto actualNode = process ? getExecVnode(process) : nullptr;
    const int64_t offset = process ? getExecOffset(process) : -1;
    auto map = getTaskMap(task);
    auto base = map ? getMapMin(map) : 0;
    const bool matches = node && actualNode == node && offset >= 0 &&
        !(offset & (PageSize - 1)) && base && !(base & (PageSize - 1)) &&
        base <= UINT64_MAX - RuntimeOffset - RuntimeSize;
    // proc_getexecutablevnode returns an iocount, independent of the KAUTH usecount.
    if (actualNode) vnode_put(actualNode);
    if (matches) {
        auto headerBuffer = static_cast<uint8_t *>(IOMalloc(HeaderSize));
        auto pageBuffer = static_cast<uint8_t *>(IOMalloc(PageSize));
        auto verifyBuffer = static_cast<uint8_t *>(IOMalloc(PageSize));
        if (headerBuffer && pageBuffer && verifyBuffer) {
            bool cowPrepared = false;
            auto prepareCow = [&]() {
                if (cowPrepared) return;
                // Keep proc->p_csflags untouched; adjust only this process's vm_map flags.
                vmMapSwitchProtect(map, FALSE);
                vmMapCsEnforcementSet(map, FALSE);
                vmMapCsDebuggedSet(map, TRUE);
                cowPrepared = true;
            };
            report = installCow(
                nativeImage, privacyProfileEnabled,
                [&](uint32_t address, uint8_t *destination, size_t size) {
                    if (size > UINT64_MAX - base || address > UINT64_MAX - base - size)
                        return false;
                    return readUser(map, base + address, destination, size) == KERN_SUCCESS;
                },
                [&](uint32_t address, const uint8_t *source, bool alreadyPrivate) {
                    if (PageSize > UINT64_MAX - base || address > UINT64_MAX - base - PageSize)
                        return false;
                    prepareCow();
                    const vm_offset_t pageAddr = static_cast<vm_offset_t>(base + address);
                    const vm_prot_t writeProt = alreadyPrivate
                        ? (VM_PROT_READ | VM_PROT_WRITE)
                        : (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE | VM_PROT_COPY);
                    if (vmProtect(map, pageAddr, PageSize, FALSE, writeProt) != KERN_SUCCESS)
                        return false;
                    const kern_return_t wr = writeUser(map, source, pageAddr, PageSize);
                    const kern_return_t rx = vmProtect(map, pageAddr, PageSize, FALSE,
                                                       VM_PROT_READ | VM_PROT_EXECUTE);
                    return wr == KERN_SUCCESS && rx == KERN_SUCCESS;
                },
                [&](uint8_t *page, size_t pageSize) {
                    return textPagePatcher ? textPagePatcher(page, pageSize) : false;
                },
                headerBuffer, pageBuffer, verifyBuffer);
        }
        if (verifyBuffer) IOFree(verifyBuffer, PageSize);
        if (pageBuffer) IOFree(pageBuffer, PageSize);
        if (headerBuffer) IOFree(headerBuffer, HeaderSize);
    }
    if (node) vnode_rele(node);
    // This receipt concerns per-process COW installation without modifying shared UBC pages.
    SYSLOG("btlfx", "bluetoothd cow patch privacy=%s(%u) privPages=%u textPages=%u rdErr=%u wrErr=%u",
        report.privacyResult == Result::Complete ? "complete" : "inactive",
        static_cast<unsigned>(report.privacyResult),
        report.privacyPagesWritten, report.textPagesPatched,
        report.readFailures, report.writeFailures);
}

inline bool start(KernelPatcher &patcher, TextPagePatcher pagePatcher, bool enablePrivacy) {
    if (enablePrivacy && !nativeImage.valid()) return false;
    getTaskMap = reinterpret_cast<GetTaskMap>(patcher.solveSymbol(KernelPatcher::KernelID, "_get_task_map"));
    getMapMin = reinterpret_cast<GetMapMin>(patcher.solveSymbol(KernelPatcher::KernelID, "_get_map_min"));
    readUser = reinterpret_cast<ReadUser>(patcher.solveSymbol(KernelPatcher::KernelID, "_vm_map_read_user"));
    writeUser = reinterpret_cast<WriteUser>(patcher.solveSymbol(KernelPatcher::KernelID, "_vm_map_write_user"));
    vmProtect = reinterpret_cast<VmProtect>(patcher.solveSymbol(KernelPatcher::KernelID, "_vm_protect"));
    vmMapSwitchProtect = reinterpret_cast<VmMapFlagSetter>(patcher.solveSymbol(KernelPatcher::KernelID, "_vm_map_switch_protect"));
    vmMapCsEnforcementSet = reinterpret_cast<VmMapFlagSetter>(patcher.solveSymbol(KernelPatcher::KernelID, "_vm_map_cs_enforcement_set"));
    vmMapCsDebuggedSet = reinterpret_cast<VmMapFlagSetter>(patcher.solveSymbol(KernelPatcher::KernelID, "_vm_map_cs_debugged_set"));
    getTaskProc = reinterpret_cast<GetTaskProc>(patcher.solveSymbol(KernelPatcher::KernelID, "_get_bsdtask_info"));
    getExecVnode = reinterpret_cast<GetExecVnode>(patcher.solveSymbol(KernelPatcher::KernelID, "_proc_getexecutablevnode"));
    getExecOffset = reinterpret_cast<GetExecOffset>(patcher.solveSymbol(KernelPatcher::KernelID, "_proc_getexecutableoffset"));
    if (!getTaskMap || !getMapMin || !readUser || !writeUser || !vmProtect ||
        !vmMapSwitchProtect || !vmMapCsEnforcementSet || !vmMapCsDebuggedSet ||
        !getTaskProc || !getExecVnode || !getExecOffset ||
        patcher.getError() != KernelPatcher::Error::NoError) {
        patcher.clearError();
        return false;
    }
    textPagePatcher = pagePatcher;
    privacyProfileEnabled = enablePrivacy;
    KernelPatcher::RouteRequest route {"_task_set_main_thread_qos", taskQos, originalTaskQos};
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &route, 1)) {
        patcher.clearError();
        return false;
    }
    execListenerHandle = kauth_listen_scope(KAUTH_SCOPE_FILEOP, execListener, nullptr);
    if (!execListenerHandle)
        return false;
    atomic_store_explicit(&cowEnabled, true, memory_order_release);
    return true;
}
} // namespace IntelPrivacyInstaller
#endif
