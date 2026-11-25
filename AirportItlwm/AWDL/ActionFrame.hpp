// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_ACTION_FRAME_HPP
#define AIRPORT_AWDL_ACTION_FRAME_HPP

#include <stddef.h>
#include <stdint.h>

// Wire-format reference: seemoo-lab/owl, revision
// 0ee3263f944ea721c355f826aa90d636d012a512, src/{frame,channel}.h.
// Read bytes explicitly: AWDL's little-endian TLVs need not be aligned.
namespace AirportAWDL {

enum class ParseResult { Other, Valid, Malformed, Unsupported };
enum Field : uint32_t {
    Sync = 1, Channels = 2, ElectionV1 = 4, ElectionV2 = 8, Version = 16, HTCapabilities = 32
};
enum HTCapBit : uint16_t {
    HTCapLDPC = 0x0001,
    HTCapSGI20 = 0x0020,
    HTCapSGI40 = 0x0040,
    HTCapRxSTBCMask = 0x0300,
};

struct ChannelSequence {
    uint8_t count, encoding, duplicates, step;
    uint16_t fill;
    uint8_t channel[16];
    uint8_t flags[16]; // Operating class for encoding 3; flags for encoding 1.
};

struct Advertisement {
    uint32_t fields;
    uint8_t subtype, version, deviceClass;
    uint8_t rxHTMcs, rxHTMcs2;
    uint16_t htCapabilities;
    bool haveActionSequence, retry;
    uint16_t actionSequence;
    uint32_t phyTxTime, targetTxTime;
    uint16_t nextWindowTU, windowPeriodTU, actionPeriodTU, windowCounter;
    uint8_t presenceMode, masterChannel, syncMaster[6];
    uint8_t electionMaster[6], electionSync[6];
    uint32_t distance, masterMetric, selfMetric, masterCounter, selfCounter, electionID;
    ChannelSequence sequence;
};

static inline uint16_t le16(const uint8_t *p)
{
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

static inline uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
        (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static inline void copyAddress(uint8_t *to, const uint8_t *from)
{
    for (unsigned i = 0; i < 6; ++i)
        to[i] = from[i];
}

static inline ParseResult parseSequence(const uint8_t *p, size_t size,
                                       ChannelSequence &out)
{
    if (size < 6)
        return ParseResult::Malformed;
    unsigned count = unsigned(p[0]) + 1;
    unsigned stride;
    switch (p[1]) {
        case 0: stride = 1; break;
        case 1: case 3: stride = 2; break;
        default: return ParseResult::Unsupported;
    }
    if (size - 6 < count * stride)
        return ParseResult::Malformed;
    if (count > 16)
        return ParseResult::Unsupported;
    ChannelSequence sequence = {};
    sequence.count = count;
    sequence.encoding = p[1];
    sequence.duplicates = p[2];
    sequence.step = p[3];
    sequence.fill = le16(p + 4);
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *entry = p + 6 + i * stride;
        sequence.channel[i] = entry[p[1] == 1 ? 1 : 0];
        sequence.flags[i] = p[1] == 1 ? entry[0] : (p[1] == 3 ? entry[1] : 0);
    }
    out = sequence;
    return ParseResult::Valid;
}

static inline uint8_t normalizeExtPrimaryChannel(uint16_t opClass, uint8_t chan)
{
    if (opClass == 128 || opClass == 130) {
        switch (chan) {
            case 42: return 44;
            case 58: return 52;
            case 106: return 100;
            case 122: return 116;
            case 138: return 132;
            case 155: return 149;
            default: return chan;
        }
    }
    if (opClass == 129) {
        switch (chan) {
            case 50: return 48;
            case 114: return 100;
            default: return chan;
        }
    }
    return chan;
}

static inline uint8_t wide80PrimaryBase(uint8_t chan)
{
    if (chan >= 36 && chan <= 48 && ((chan - 36) % 4) == 0) return 36;
    if (chan >= 52 && chan <= 64 && ((chan - 52) % 4) == 0) return 52;
    if (chan >= 100 && chan <= 112 && ((chan - 100) % 4) == 0) return 100;
    if (chan >= 116 && chan <= 128 && ((chan - 116) % 4) == 0) return 116;
    if (chan >= 132 && chan <= 144 && ((chan - 132) % 4) == 0) return 132;
    if (chan >= 149 && chan <= 161 && ((chan - 149) % 4) == 0) return 149;
    return 0;
}

static inline bool sameWideChannelBlock(uint8_t opClass, uint8_t extPrimary, uint8_t basePrimary)
{
    const uint8_t extBase = wide80PrimaryBase(extPrimary);
    const uint8_t baseBase = wide80PrimaryBase(basePrimary);
    if (!extBase || !baseBase) return false;
    if (opClass == 128 || opClass == 130)
        return extBase == baseBase;
    if (opClass == 129) {
        const uint8_t ext160 = (extBase == 36 || extBase == 52) ? 36 :
            ((extBase == 100 || extBase == 116) ? 100 : 0);
        const uint8_t base160 = (baseBase == 36 || baseBase == 52) ? 36 :
            ((baseBase == 100 || baseBase == 116) ? 100 : 0);
        return ext160 && ext160 == base160;
    }
    return false;
}

static inline ParseResult parseSequenceExt(const uint8_t *p, size_t size,
                                           ChannelSequence &out)
{
    if (size < 9)
        return ParseResult::Malformed;
    ChannelSequence sequence = {};
    sequence.count = 16;
    sequence.encoding = 3;
    sequence.duplicates = 0;
    sequence.step = 3;
    sequence.fill = 0xffff;
    while (size > 0) {
        if (size < 9)
            return ParseResult::Malformed;
        const uint16_t opClass = le16(p + 2);
        const uint16_t chan = le16(p + 4);
        const uint8_t bitmapBytes = p[8];
        if (size - 9 < bitmapBytes)
            return ParseResult::Malformed;
        if (chan > 255 || (opClass >= 131 && opClass <= 137))
            return ParseResult::Unsupported;
        if (chan && !(opClass == 0 || (opClass >= 81 && opClass <= 84) ||
                      (opClass >= 115 && opClass <= 130)))
            return ParseResult::Malformed;
        const uint8_t normChan = !chan ? 0 : normalizeExtPrimaryChannel(opClass, uint8_t(chan));
        const uint8_t normClass = !normChan ? 0 :
            uint8_t(opClass ? opClass : (normChan <= 13 ? 81 : (normChan == 14 ? 82 : 115)));
        for (unsigned b = 0; b < bitmapBytes; ++b) {
            const uint8_t bits = p[9 + b];
            for (unsigned bit = 0; bit < 8; ++bit) {
                const unsigned slot = b * 8 + bit;
                if (slot < 16 && ((bits >> bit) & 1U)) {
                    sequence.channel[slot] = normChan;
                    sequence.flags[slot] = normClass;
                }
            }
        }
        p += 9 + bitmapBytes;
        size -= 9 + bitmapBytes;
    }
    out = sequence;
    return ParseResult::Valid;
}

// The caller owns the output. A rejected frame never partially replaces it.
static inline ParseResult parseAction(const uint8_t *p, size_t size,
                                     Advertisement &out)
{
    if (size < 5 || p[0] != 127 || p[1] != 0 || p[2] != 0x17 ||
        p[3] != 0xf2 || p[4] != 8)
        return ParseResult::Other;
    if (size < 16)
        return ParseResult::Malformed;
    if (p[5] != 0x10 || (p[6] != 0 && p[6] != 3))
        return ParseResult::Unsupported;

    Advertisement next = {};
    next.subtype = p[6];
    next.phyTxTime = le32(p + 8);
    next.targetTxTime = le32(p + 12);
    ChannelSequence baseSequence = {};
    ChannelSequence extSequence = {};
    bool haveBaseSequence = false;
    bool explicitSequence = false;
    bool haveChanSeqExt = false;
    for (size_t offset = 16; offset < size;) {
        if (size - offset < 3)
            return ParseResult::Malformed;
        uint8_t type = p[offset];
        size_t length = le16(p + offset + 1);
        offset += 3;
        if (length > size - offset)
            return ParseResult::Malformed;
        const uint8_t *value = p + offset;
        switch (type) {
            case 4: { // Synchronization parameters, optionally followed by a sequence.
                if (length < 33 || le16(value + 5) == 0 || value[27] == 0)
                    return ParseResult::Malformed;
                next.nextWindowTU = le16(value + 1);
                next.masterChannel = value[3];
                next.windowPeriodTU = le16(value + 5);
                next.actionPeriodTU = le16(value + 7);
                copyAddress(next.syncMaster, value + 21);
                next.presenceMode = value[27];
                next.windowCounter = le16(value + 29);
                next.fields |= Sync;
                if (length > 33) {
                    ChannelSequence sequence = {};
                    ParseResult result = parseSequence(value + 33, length - 33, sequence);
                    if (result != ParseResult::Valid)
                        return result;
                    if (!explicitSequence) {
                        baseSequence = sequence;
                        haveBaseSequence = true;
                    }
                    next.fields |= Channels;
                }
                break;
            }
            case 5:
                if (length < 21)
                    return ParseResult::Malformed;
                // Election v2 wins regardless of TLV ordering.
                if (!(next.fields & ElectionV2)) {
                    next.electionID = le16(value + 1);
                    next.distance = value[3];
                    copyAddress(next.electionMaster, value + 5);
                    next.masterMetric = le32(value + 11);
                    next.selfMetric = le32(value + 15);
                }
                next.fields |= ElectionV1;
                break;
            case 7:
                // Two AWDL bytes precede the IEEE HT capability body. MCS0..7
                // are the first RX mask byte, after capabilities and A-MPDU parameters.
                if (length < 3 || length > 28) return ParseResult::Malformed;
                next.htCapabilities = length >= 4 ? le16(value + 2) : 0;
                next.rxHTMcs = length >= 6 ? value[5] : 0;
                next.rxHTMcs2 = length >= 7 ? value[6] : 0;
                next.fields |= HTCapabilities;
                break;
            case 18: {
                ChannelSequence sequence = {};
                ParseResult result = parseSequence(value, length, sequence);
                if (result != ParseResult::Valid)
                    return result;
                baseSequence = sequence;
                haveBaseSequence = true;
                explicitSequence = true;
                next.fields |= Channels;
                break;
            }
            case 21:
                if (length < 2)
                    return ParseResult::Malformed;
                next.version = value[0];
                next.deviceClass = value[1];
                next.fields |= Version;
                break;
            case 24:
                if (length < 40)
                    return ParseResult::Malformed;
                copyAddress(next.electionMaster, value);
                copyAddress(next.electionSync, value + 6);
                next.masterCounter = le32(value + 12);
                next.distance = le32(value + 16);
                next.masterMetric = le32(value + 20);
                next.selfMetric = le32(value + 24);
                next.electionID = le32(value + 28);
                next.selfCounter = le32(value + 36);
                next.fields |= ElectionV2;
                break;
            case 32: {
                ChannelSequence sequence = {};
                if (parseSequenceExt(value, length, sequence) == ParseResult::Valid) {
                    extSequence = sequence;
                    haveChanSeqExt = true;
                    next.fields |= Channels;
                }
                break;
            }
            default:
                break;
        }
        offset += length;
    }
    if (haveChanSeqExt) {
        if (haveBaseSequence) {
            for (unsigned s = 0; s < 16; ++s) {
                const uint8_t baseChan = s < baseSequence.count ? baseSequence.channel[s] : 0;
                const uint8_t baseFlags = s < baseSequence.count ? baseSequence.flags[s] : 0;
                const uint8_t extChan = extSequence.channel[s];
                const uint8_t extClass = extSequence.flags[s];
                if (!extChan && baseChan) {
                    extSequence.channel[s] = baseChan;
                    extSequence.flags[s] = baseSequence.encoding == 3 ? baseFlags :
                        uint8_t(baseChan <= 13 ? 81 : (baseChan == 14 ? 82 : 115));
                } else if (extChan && baseChan && sameWideChannelBlock(extClass, extChan, baseChan)) {
                    extSequence.channel[s] = baseChan;
                }
            }
        }
        if ((next.fields & Sync) && next.presenceMode > 0)
            extSequence.step = next.presenceMode - 1;
        next.sequence = extSequence;
    } else if (haveBaseSequence) {
        next.sequence = baseSequence;
    }
    out = next;
    return ParseResult::Valid;
}

// Input is a complete, FCS-checked MPDU with firmware padding already removed.
static inline ParseResult parseMPDU(const uint8_t *p, size_t size,
                                   Advertisement &out)
{
    if (size < 24 || p[0] != 0xd0 || (p[1] & 0x40))
        return ParseResult::Other;
    const size_t header = (p[1] & 0x80) ? 28 : 24; // Optional HT control.
    if (size < header + 5)
        return ParseResult::Other;
    const uint8_t *body = p + header;
    if (body[0] != 127 || body[1] != 0 || body[2] != 0x17 ||
        body[3] != 0xf2 || body[4] != 8)
        return ParseResult::Other;
    if ((p[1] & 0x07) || (p[22] & 0x0f) || (p[10] & 1))
        return ParseResult::Malformed; // No DS addressing, fragmentation or group sender.
    const auto result = parseAction(body, size - header, out);
    if (result == ParseResult::Valid) {
        out.haveActionSequence = true;
        out.actionSequence = le16(p + 22) >> 4;
        out.retry = p[1] & 0x08;
    }
    return result;
}

// Intel LISTENER RX can append trailing zero padding after the final AWDL TLV on 5 GHz / HE frames.
// Strip trailing zero bytes only when they follow a valid non-empty TLV sequence.
static inline size_t trimActionMPDU(const uint8_t *p, size_t size)
{
    if (!p || size < 24 || p[0] != 0xd0 || (p[1] & 0x47) ||
        (p[22] & 0x0f) || (p[10] & 1))
        return size;
    const size_t header = (p[1] & 0x80) ? 28 : 24;
    if (size <= header + 16)
        return size;
    const uint8_t *body = p + header;
    if (body[0] != 127 || body[1] != 0 || body[2] != 0x17 ||
        body[3] != 0xf2 || body[4] != 8 || body[5] != 0x10 ||
        (body[6] != 0 && body[6] != 3))
        return size;
    size_t lastNonZeroEnd = 16;
    for (size_t offset = 16; offset + 3 <= size - header;) {
        const uint8_t type = body[offset];
        const size_t length = le16(body + offset + 1);
        if (length > (size - header) - (offset + 3))
            break;
        offset += 3 + length;
        if (type != 0 || length != 0)
            lastNonZeroEnd = offset;
    }
    const size_t trimmed = header + lastNonZeroEnd;
    if (trimmed <= header + 16 || trimmed >= size)
        return size;
    for (size_t i = trimmed; i < size; ++i)
        if (p[i] != 0)
            return size;
    Advertisement probe = {};
    return parseMPDU(p, trimmed, probe) == ParseResult::Valid ? trimmed : size;
}

struct ReceiveState {
    uint64_t valid, malformed, unsupported, psf, mif, otherDestination, zeroPadTrimmed;
    int16_t lastRSSI;
    uint16_t lastChannel;
    uint8_t lastSender[6];
    Advertisement last;

    bool receive(const uint8_t *frame, size_t size, uint16_t channel, int16_t rssi)
    {
        Advertisement parsed = {};
        switch (parseMPDU(frame, size, parsed)) {
            case ParseResult::Other:
                return false;
            case ParseResult::Malformed:
                ++malformed;
                break;
            case ParseResult::Unsupported:
                ++unsupported;
                break;
            case ParseResult::Valid:
                ++valid;
                if (parsed.subtype == 0) ++psf; else ++mif;
                last = parsed;
                lastChannel = channel;
                lastRSSI = rssi;
                copyAddress(lastSender, frame + 10);
                break;
        }
        return true;
    }
};

} // namespace AirportAWDL
#endif
