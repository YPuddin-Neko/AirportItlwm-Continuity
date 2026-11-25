// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_SERVICE_MERGE_HPP
#define AIRPORT_AWDL_SERVICE_MERGE_HPP

#include <stddef.h>
#include <stdint.h>

namespace AirportAWDL {

// Multi-peer cache for AWDL Service Response records (TLV 2) scoped by each
// peer's current Service Update Indicator (SUI in TLV 6). Native IO80211Family
// only parses MIF (subtype 3) service payloads for ~4 frames after a peer's SUI
// changes; peers with multiple Bonjour services split TLV 2 entries across
// consecutive MIFs. Appending missing current-SUI TLV 2 entries into incoming
// MIF vendor bodies keeps local discovery complete without holding withdrawn
// services across SUI updates or modifying any transmitted frame.
struct ServiceMergeCache {
    static constexpr size_t BodyHeaderBytes = 16;
    static constexpr size_t MaxMergedBodyBytes = 1476;
    static constexpr unsigned Capacity = 8;
    static constexpr unsigned MaxRecords = 48;
    static constexpr unsigned MaxFrameRecords = 96;
    static constexpr size_t MaxRecordBytes = 1536;
    static constexpr uint64_t LifetimeUS = 5000000ULL;

    struct Slot {
        uint8_t peer[6];
        uint16_t sui;
        bool occupied;
        uint64_t lastSeenUS;
        unsigned records;
        size_t used;
        uint16_t offsets[MaxRecords];
        uint16_t lengths[MaxRecords];
        uint8_t bytes[MaxRecordBytes];

        void clearRecords() {
            records = 0;
            used = 0;
        }

        void clear() {
            occupied = false;
            sui = 0;
            lastSeenUS = 0;
            clearRecords();
        }
    };

    Slot slots[Capacity];
    uint64_t mergedFrames;
    uint64_t mergedRecords;
    uint64_t resets;
    uint64_t evictions;
    uint64_t overflow;

    static bool sameBytes(const uint8_t *a, const uint8_t *b, size_t n) {
        for (size_t i = 0; i < n; ++i)
            if (a[i] != b[i]) return false;
        return true;
    }

    static void copyBytes(uint8_t *dst, const uint8_t *src, size_t n) {
        for (size_t i = 0; i < n; ++i)
            dst[i] = src[i];
    }

    void clear() {
        for (unsigned i = 0; i < Capacity; ++i)
            slots[i].clear();
    }

    static bool parseBodySUI(const uint8_t *body, size_t length, uint16_t &suiOut) {
        if (!body || length < BodyHeaderBytes ||
            body[0] != 127 || body[1] != 0 || body[2] != 0x17 ||
            body[3] != 0xf2 || body[4] != 8 || body[5] != 0x10 ||
            (body[6] != 0 && body[6] != 3))
            return false;
        size_t offset = BodyHeaderBytes;
        bool found = false;
        uint16_t sui = 0;
        while (offset < length) {
            if (length - offset < 3) return false;
            const size_t n = size_t(body[offset + 1]) | (size_t(body[offset + 2]) << 8);
            if (n > length - offset - 3) return false;
            if (body[offset] == 6 && n >= 5) {
                sui = uint16_t(body[offset + 6]) | (uint16_t(body[offset + 7]) << 8);
                found = true;
            }
            offset += 3 + n;
        }
        if (offset != length || !found) return false;
        suiOut = sui;
        return true;
    }

    static bool slotHolds(const Slot &slot, const uint8_t *tlv, size_t total) {
        for (unsigned i = 0; i < slot.records; ++i)
            if (slot.lengths[i] == total && sameBytes(slot.bytes + slot.offsets[i], tlv, total))
                return true;
        return false;
    }

    void expire(uint64_t nowUS) {
        for (unsigned i = 0; i < Capacity; ++i) {
            if (slots[i].occupied && nowUS >= slots[i].lastSeenUS &&
                nowUS - slots[i].lastSeenUS >= LifetimeUS)
                slots[i].clear();
        }
    }

    Slot &selectSlot(const uint8_t *peer) {
        unsigned freeIdx = Capacity;
        unsigned oldestIdx = 0;
        for (unsigned i = 0; i < Capacity; ++i) {
            if (slots[i].occupied) {
                if (sameBytes(slots[i].peer, peer, 6))
                    return slots[i];
                if (slots[i].lastSeenUS < slots[oldestIdx].lastSeenUS)
                    oldestIdx = i;
            } else if (freeIdx == Capacity) {
                freeIdx = i;
            }
        }
        if (freeIdx < Capacity)
            return slots[freeIdx];
        ++evictions;
        slots[oldestIdx].clear();
        return slots[oldestIdx];
    }

    size_t process(const uint8_t *peer, const uint8_t *body, size_t bodyLength,
                   uint8_t *out, size_t capacity, uint64_t nowUS) {
        if (!peer || !body || !out || bodyLength > capacity ||
            (peer[0] & 1))
            return 0;
        uint16_t frameSui = 0;
        if (!parseBodySUI(body, bodyLength, frameSui)) {
            copyBytes(out, body, bodyLength);
            return bodyLength;
        }
        expire(nowUS);
        Slot &slot = selectSlot(peer);
        if (!slot.occupied) {
            slot.clear();
            copyBytes(slot.peer, peer, 6);
            slot.sui = frameSui;
            slot.occupied = true;
        } else if (slot.sui != frameSui) {
            slot.clearRecords();
            slot.sui = frameSui;
            ++resets;
        }
        slot.lastSeenUS = nowUS;

        // Only MIFs (subtype 3) carry TLV 2 service payloads; PSFs still update SUI above.
        if (body[6] != 3) {
            copyBytes(out, body, bodyLength);
            return bodyLength;
        }

        for (size_t offset = BodyHeaderBytes; offset < bodyLength;) {
            const size_t n = size_t(body[offset + 1]) | (size_t(body[offset + 2]) << 8);
            const size_t total = 3 + n;
            if (body[offset] == 2 && !slotHolds(slot, body + offset, total)) {
                if (slot.records >= MaxRecords || slot.used + total > MaxRecordBytes) {
                    ++overflow;
                } else {
                    slot.offsets[slot.records] = uint16_t(slot.used);
                    slot.lengths[slot.records] = uint16_t(total);
                    copyBytes(slot.bytes + slot.used, body + offset, total);
                    slot.used += total;
                    ++slot.records;
                }
            }
            offset += total;
        }

        bool have[MaxRecords] = {};
        unsigned present = 0;
        for (size_t offset = BodyHeaderBytes; offset < bodyLength;) {
            const size_t n = size_t(body[offset + 1]) | (size_t(body[offset + 2]) << 8);
            const size_t total = 3 + n;
            if (body[offset] == 2) {
                ++present;
                for (unsigned i = 0; i < slot.records; ++i) {
                    if (slot.lengths[i] == total &&
                        sameBytes(slot.bytes + slot.offsets[i], body + offset, total))
                        have[i] = true;
                }
            }
            offset += total;
        }

        size_t extra = 0;
        unsigned added = 0;
        for (unsigned i = 0; i < slot.records; ++i) {
            if (!have[i]) {
                extra += slot.lengths[i];
                ++added;
            }
        }

        copyBytes(out, body, bodyLength);
        const size_t limit = capacity < MaxMergedBodyBytes ? capacity : MaxMergedBodyBytes;
        if (!added || present + added > MaxFrameRecords || bodyLength + extra > limit)
            return bodyLength;

        size_t w = bodyLength;
        for (unsigned i = 0; i < slot.records; ++i) {
            if (!have[i]) {
                copyBytes(out + w, slot.bytes + slot.offsets[i], slot.lengths[i]);
                w += slot.lengths[i];
            }
        }
        ++mergedFrames;
        mergedRecords += added;
        return w;
    }
};

} // namespace AirportAWDL

#endif
