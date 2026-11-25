// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#ifndef IWX_REGULATORY_HPP
#define IWX_REGULATORY_HPP
#include <stddef.h>
#include <stdint.h>

struct IwxRegulatoryProfile {
    uint32_t status = UINT32_MAX, capabilities = 0, count = 0;
    uint16_t mcc = 0, geo = 0;
    uint8_t source = 0;
    uint32_t channels[256] = {};
    static uint16_t read16(const uint8_t *p) { return p[0] | uint16_t(p[1]) << 8; }
    static uint32_t read32(const uint8_t *p) { return read16(p) | uint32_t(read16(p + 2)) << 16; }
    bool parse(const uint8_t *bytes, size_t length, bool axLayout, uint8_t version, unsigned maximum) {
        // Intel nvm.c: response >=8, else 11AX-capable v4 (including v5), else v3.
        const unsigned header = version >= 8 && version != 0xff ? 24 : axLayout ? 20 : 16;
        if (length < header) return false;
        status = read32(bytes);
        count = read32(bytes + header - 4);
        if (!count || count > maximum || count > 256 || length != header + size_t(count) * 4)
            return false;
        // ILLEGAL and LOW_PRIORITY report the profile firmware actually retained.
        if (status != 0 && status != 1 && status != 4 && status != 5) return false;
        mcc = read16(bytes + 4);
        if (!mcc) mcc = 0x3030; // Firmware's world-domain representation.
        if (header == 24) {
            capabilities = read32(bytes + 8); geo = read16(bytes + 14); source = bytes[16];
        } else if (header == 20) {
            capabilities = read16(bytes + 6); geo = read16(bytes + 10); source = bytes[12];
        } else {
            capabilities = bytes[6]; geo = read16(bytes + 10); source = bytes[7];
        }
        for (unsigned i = 0; i < count; ++i) channels[i] = read32(bytes + header + i * 4);
        return true;
    }
    uint32_t channelFlags(unsigned index, uint8_t version) const {
        uint32_t flags = channels[index];
        if (index < 14) return flags;
        const bool v2 = version != 0xff && version >= 6;
        const bool allow40 = v2 ? capabilities & (1U << 8) : !(capabilities & (1U << 7));
        const bool allow80 = capabilities & (1U << (v2 ? 4 : 3));
        const bool allow160 = capabilities & (1U << (v2 ? 3 : 2));
        if (!allow40) flags &= ~(1U << 9);
        if (!allow80) flags &= ~(1U << 10);
        if (!allow160) flags &= ~(1U << 11);
        return flags;
    }
};

struct IwxRegulatoryState {
    uint64_t epoch = 0, serial = 0, refreshes = 0, notifications = 0;
    bool pending = false, running = false, valid = false;
    uint16_t requestedMCC = 0, mappedMCC = 0;
    uint8_t requestedSource = 0, mappedSource = 0, responseVersion = 0;
    uint32_t status = UINT32_MAX, capabilities = 0, rawFlags[256] = {};
    int lastError = 0;
    void reset() {
        ++epoch; ++serial;
        pending = running = valid = false;
        mappedMCC = 0; mappedSource = 0; lastError = 0;
    }
};
#endif
