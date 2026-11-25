// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_DATA_FRAME_HPP
#define AIRPORT_AWDL_DATA_FRAME_HPP

#include "PeerState.hpp"

namespace AirportAWDL {

// Wire-format reference: seemoo-lab/owl revision
// 0ee3263f944ea721c355f826aa90d636d012a512, src/frame.h and src/tx.c.
static const uint8_t BSSID[6] = {0, 0x25, 0, 0xff, 0x94, 0x73};
static const uint8_t SNAP[8] = {0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0};
static const uint8_t LLW_SNAP[8] = {0xaa, 0xaa, 3, 0, 0, 0, 0x86, 0xdd};
static const uint8_t LLW_APPLE_SNAP[8] = {0xaa, 0xaa, 3, 0, 0x17, 0xf2, 0x86, 0xdd};

static inline uint16_t be16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }
static inline void putLE16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void putBE16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }

// Views borrow the complete RX buffer and must not outlive it.
struct DataView {
    uint8_t destination[6], source[6];
    uint16_t sequence;
    uint16_t flags;
    uint8_t tid;
    bool lowLatency;
    bool retry;
    const uint8_t *payload;
    size_t size;
};

struct DataBatch {
    enum { Capacity = 32 };
    unsigned count;
    DataView frames[Capacity];
};

static inline bool ipv6Payload(const uint8_t *p, size_t size)
{
    return size >= 40 && size <= 1500 && (p[0] >> 4) == 6 && be16(p + 4) == size - 40;
}

static inline bool isLinkLocalMDNS(const uint8_t *ethernet, size_t size)
{
    static const uint8_t groupMAC[6] = {0x33, 0x33, 0, 0, 0, 0xfb};
    static const uint8_t groupIP[16] = {0xff, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xfb};
    if (size < 14 + 40 + 8 + 12 || !sameAddress(ethernet, groupMAC) ||
        be16(ethernet + 12) != 0x86dd) return false;
    const uint8_t *ip = ethernet + 14;
    if (!ipv6Payload(ip, size - 14) || ip[6] != 17 || ip[7] != 255) return false;
    for (unsigned i = 0; i < sizeof(groupIP); ++i)
        if (ip[24 + i] != groupIP[i]) return false;
    const uint8_t *udp = ip + 40;
    return be16(udp + 2) == 5353 && be16(udp + 4) == size - 14 - 40;
}

static inline bool matchesBytes8(const uint8_t *p, const uint8_t expected[8])
{
    for (unsigned i = 0; i < 8; ++i)
        if (p[i] != expected[i]) return false;
    return true;
}

static inline bool isPTPPort(uint16_t port)
{
    return port == 319 || port == 320;
}

static inline bool isPTPPayload(uint16_t etherType, const uint8_t *payload,
                                size_t available, size_t totalSize)
{
    if (!payload || available > totalSize || totalSize > 1500)
        return false;
    if (etherType == 0x88f7)
        return totalSize >= 34;
    if (etherType == 0x0800) {
        if (available < 20 || totalSize < 28 || (payload[0] >> 4) != 4)
            return false;
        const size_t ihl = size_t(payload[0] & 0x0f) * 4;
        if (ihl < 20 || available < ihl + 8 || totalSize < ihl + 8)
            return false;
        const uint16_t ipLen = be16(payload + 2);
        if (ipLen < ihl + 8 || ipLen > totalSize)
            return false;
        if ((be16(payload + 6) & 0x1fff) != 0 || payload[9] != 17)
            return false;
        const uint8_t *udp = payload + ihl;
        const uint16_t srcPort = be16(udp), dstPort = be16(udp + 2), udpLen = be16(udp + 4);
        return (isPTPPort(dstPort) || isPTPPort(srcPort)) &&
            udpLen >= 8 && udpLen <= ipLen - ihl;
    }
    if (etherType == 0x86dd) {
        if (available < 40 || totalSize < 48 || (payload[0] >> 4) != 6 ||
            be16(payload + 4) != totalSize - 40)
            return false;
        uint8_t nextHdr = payload[6];
        size_t udpOff = 40;
        if (nextHdr == 0 || nextHdr == 43 || nextHdr == 60) {
            if (available < 42)
                return false;
            const size_t extLen = (size_t(payload[41]) + 1) * 8;
            if (40 + extLen + 8 > totalSize)
                return false;
            nextHdr = payload[40];
            udpOff = 40 + extLen;
        } else if (nextHdr == 44) {
            if (available < 48 || totalSize < 56 || (be16(payload + 42) & 0xfff8) != 0)
                return false;
            nextHdr = payload[40];
            udpOff = 48;
        }
        if (nextHdr != 17 || available < udpOff + 8 || totalSize < udpOff + 8)
            return false;
        const uint8_t *udp = payload + udpOff;
        const uint16_t srcPort = be16(udp), dstPort = be16(udp + 2), udpLen = be16(udp + 4);
        return (isPTPPort(dstPort) || isPTPPort(srcPort)) && udpLen == totalSize - udpOff;
    }
    return false;
}

static inline bool isPTPDatagram(const uint8_t *ip, size_t available, size_t size)
{
    if (!ip || available < 1 || available > size)
        return false;
    const uint8_t ver = ip[0] >> 4;
    if (ver == 6)
        return isPTPPayload(0x86dd, ip, available, size);
    if (ver == 4)
        return isPTPPayload(0x0800, ip, available, size);
    return false;
}

static inline bool isUnicastPTPHeader(const uint8_t *header, size_t copyLen, size_t totalSize)
{
    if (!header || copyLen < 14 || totalSize < copyLen || totalSize > 1514 ||
        !unicastAddress(header))
        return false;
    return isPTPPayload(be16(header + 12), header + 14, copyLen - 14, totalSize - 14);
}

static inline bool isUnicastPTPMPDU(const uint8_t *mpdu, size_t size)
{
    if (!mpdu || size < 24 || (mpdu[0] != 0x08 && mpdu[0] != 0x88) ||
        (mpdu[1] & 0x47) || (mpdu[22] & 15) ||
        !sameAddress(mpdu + 16, BSSID) ||
        !unicastAddress(mpdu + 4) || !unicastAddress(mpdu + 10))
        return false;
    const bool qos = mpdu[0] == 0x88;
    const size_t hdrLen = 24 + (qos ? 2 : 0) + ((qos && (mpdu[1] & 0x80)) ? 4 : 0);
    if (size <= hdrLen || (qos && (mpdu[24] & 0x80)))
        return false;
    const uint8_t *body = mpdu + hdrLen;
    const size_t bodyLen = size - hdrLen;
    if (bodyLen >= 16 && matchesBytes8(body, SNAP) && le16(body + 8) == 0x0403)
        return isPTPPayload(be16(body + 14), body + 16, bodyLen - 16, bodyLen - 16);
    if (bodyLen >= 8 && (matchesBytes8(body, LLW_SNAP) || matchesBytes8(body, LLW_APPLE_SNAP)))
        return isPTPPayload(0x86dd, body + 8, bodyLen - 8, bodyLen - 8);
    if (bodyLen >= 8 && body[0] == 0xaa && body[1] == 0xaa && body[2] == 3 &&
        body[3] == 0 && body[4] == 0 && body[5] == 0)
        return isPTPPayload(be16(body + 6), body + 8, bodyLen - 8, bodyLen - 8);
    return false;
}

static inline bool isUnicastPTP(const uint8_t *frame, size_t size)
{
    if (!frame || size < 14)
        return false;
    if (size >= 32 && (frame[0] == 0x08 || frame[0] == 0x88) &&
        !(frame[1] & 0x43) && sameAddress(frame + 16, BSSID))
        return isUnicastPTPMPDU(frame, size);
    return isUnicastPTPHeader(frame, size, size);
}

static inline bool isUnicastPTP(const DataView &view)
{
    return unicastAddress(view.destination) &&
        isPTPDatagram(view.payload, view.size, view.size);
}

static inline ParseResult parseDataBody(const uint8_t *p, size_t size,
                                       const uint8_t *source, const uint8_t *destination,
                                       DataView &out)
{
    if (size < 8) return ParseResult::Malformed;
    if (matchesBytes8(p, SNAP)) {
        if (size < 16) return ParseResult::Malformed;
        // AWDL data headers carry optional metadata bits in bytes 12..13 alongside
        // the 0x0403 header marker and 0x86dd IPv6 EtherType.
        if (le16(p + 8) != 0x0403 || be16(p + 14) != 0x86dd)
            return ParseResult::Unsupported;
        if (!ipv6Payload(p + 16, size - 16)) return ParseResult::Malformed;
        DataView next = {};
        copyAddress(next.source, source);
        copyAddress(next.destination, destination);
        next.sequence = le16(p + 10);
        next.flags = le16(p + 12);
        next.payload = p + 16;
        next.size = size - 16;
        out = next;
        return ParseResult::Valid;
    }
    if (matchesBytes8(p, LLW_SNAP) || matchesBytes8(p, LLW_APPLE_SNAP)) {
        if (!ipv6Payload(p + 8, size - 8)) return ParseResult::Malformed;
        DataView next = {};
        copyAddress(next.source, source);
        copyAddress(next.destination, destination);
        next.lowLatency = true;
        next.payload = p + 8;
        next.size = size - 8;
        out = next;
        return ParseResult::Valid;
    }
    if (size < 16) return ParseResult::Malformed;
    return ParseResult::Unsupported;
}

static inline ParseResult parseDataMPDU(const uint8_t *p, size_t size, DataBatch &out,
                                       bool hardwareDeaggregated = false)
{
    if (size < 24 || (p[0] != 0x08 && p[0] != 0x88) || (p[1] & 0x43) ||
        !sameAddress(p + 16, BSSID))
        return ParseResult::Other;
    if ((p[1] & 4) || (p[22] & 15) || !unicastAddress(p + 10))
        return ParseResult::Malformed;
    const bool qos = p[0] == 0x88;
    const bool retry = (p[1] & 0x08) != 0;
    const uint16_t wifiSeq = uint16_t(le16(p + 22) >> 4);
    const size_t header = 24 + (qos ? 2 : 0) + ((qos && (p[1] & 0x80)) ? 4 : 0);
    if (size < header) return ParseResult::Malformed;
    const uint8_t tid = qos ? (p[24] & 15) : 0;
    DataBatch next = {};
    auto parseAMSDU = [&](bool allowTrailingPad) -> ParseResult {
        size_t offset = header;
        while (offset < size) {
            if (size - offset < 14) return ParseResult::Malformed;
            size_t length = be16(p + offset + 12);
            if (length > size - offset - 14) return ParseResult::Malformed;
            // AWDL is a direct link: aggregated frames cannot introduce another sender.
            if (!sameAddress(p + offset + 6, p + 10) || !sameAddress(p + offset, p + 4))
                return ParseResult::Malformed;
            if (next.count == DataBatch::Capacity) return ParseResult::Unsupported;
            ParseResult result = parseDataBody(p + offset + 14, length, p + offset + 6,
                                               p + offset, next.frames[next.count]);
            if (result != ParseResult::Valid) return result;
            next.frames[next.count].tid = tid;
            next.frames[next.count].retry = retry;
            if (next.frames[next.count].lowLatency) next.frames[next.count].sequence = wifiSeq;
            ++next.count;
            offset += 14 + length;
            if (offset < size) {
                size_t padding = (4 - ((14 + length) % 4)) % 4;
                if (allowTrailingPad && size - offset == padding) break;
                if (size - offset <= padding) return ParseResult::Malformed;
                offset += padding;
            }
        }
        return next.count ? ParseResult::Valid : ParseResult::Malformed;
    };
    if (!qos || !(p[24] & 0x80) || hardwareDeaggregated) {
        ParseResult result = parseDataBody(p + header, size - header, p + 10, p + 4, next.frames[0]);
        if (result == ParseResult::Valid) {
            next.frames[0].tid = tid;
            next.frames[0].retry = retry;
            if (next.frames[0].lowLatency) next.frames[0].sequence = wifiSeq;
            next.count = 1;
        } else if (qos && (p[24] & 0x80) && hardwareDeaggregated &&
                   result == ParseResult::Unsupported && size - header >= 14 &&
                   sameAddress(p + header, p + 4) && sameAddress(p + header + 6, p + 10)) {
            // Direct-link hardware A-MSDU deaggregation may preserve the 14-byte
            // subframe header and non-last alignment padding when Addr3 holds the BSSID.
            result = parseAMSDU(true);
            if (result != ParseResult::Valid) return result;
        } else {
            return result;
        }
    } else {
        ParseResult result = parseAMSDU(false);
        if (result != ParseResult::Valid) return result;
    }
    out = next;
    return ParseResult::Valid;
}

// Caller supplies separate input/output buffers. Failure leaves output unchanged.
static inline size_t ethernetFromData(const DataView &view, uint8_t *out, size_t capacity)
{
    if (!ipv6Payload(view.payload, view.size) || capacity < 14 + view.size) return 0;
    copyAddress(out, view.destination);
    copyAddress(out + 6, view.source);
    putBE16(out + 12, 0x86dd);
    for (size_t i = 0; i < view.size; ++i) out[14 + i] = view.payload[i];
    return 14 + view.size;
}

static inline size_t dataFromEthernet(const uint8_t *ethernet, size_t size, uint8_t *out,
                                      size_t capacity, uint16_t wifiSequence, uint16_t awdlSequence)
{
    if (size < 14 || be16(ethernet + 12) != 0x86dd || !unicastAddress(ethernet + 6) ||
        !ipv6Payload(ethernet + 14, size - 14) || capacity < size + 26) return 0;
    // Emit a non-QoS, unprotected direct-link MPDU; firmware supplies duration/FCS.
    for (unsigned i = 0; i < 40; ++i) out[i] = 0;
    out[0] = 0x08;
    copyAddress(out + 4, ethernet);
    copyAddress(out + 10, ethernet + 6);
    copyAddress(out + 16, BSSID);
    putLE16(out + 22, (wifiSequence & 0xfff) << 4);
    for (unsigned i = 0; i < 8; ++i) out[24 + i] = SNAP[i];
    putLE16(out + 32, 0x0403);
    putLE16(out + 34, awdlSequence);
    putBE16(out + 38, 0x86dd);
    for (size_t i = 14; i < size; ++i) out[26 + i] = ethernet[i];
    return size + 26;
}

static inline size_t dataFromEthernetLLW(const uint8_t *ethernet, size_t size, uint8_t *out,
                                         size_t capacity, uint16_t wifiSequence,
                                         bool qos = true, uint8_t tid = 7)
{
    const size_t header = qos ? 26 : 24;
    const size_t total = header + 8 + (size > 14 ? size - 14 : 0);
    if (size < 14 || be16(ethernet + 12) != 0x86dd || !unicastAddress(ethernet + 6) ||
        !ipv6Payload(ethernet + 14, size - 14) || capacity < total) return 0;
    for (size_t i = 0; i < header; ++i) out[i] = 0;
    out[0] = qos ? 0x88 : 0x08;
    copyAddress(out + 4, ethernet);
    copyAddress(out + 10, ethernet + 6);
    copyAddress(out + 16, BSSID);
    putLE16(out + 22, (wifiSequence & 0xfff) << 4);
    if (qos) out[24] = tid & 0x0f;
    for (unsigned i = 0; i < 8; ++i) out[header + i] = LLW_SNAP[i];
    for (size_t i = 14; i < size; ++i) out[header + 8 + (i - 14)] = ethernet[i];
    return total;
}

// A pair is one QoS MPDU and one ACK, with an AWDL sequence for each MSDU.
static inline size_t dataPairFromEthernet(const uint8_t *first, size_t firstSize,
                                          const uint8_t *second, size_t secondSize,
                                          uint8_t *out, size_t capacity,
                                          uint16_t wifiSequence, uint16_t firstSequence,
                                          uint16_t secondSequence)
{
    if (firstSize < 14 || secondSize < 14 || be16(first + 12) != 0x86dd ||
        be16(second + 12) != 0x86dd || !unicastAddress(first) ||
        !unicastAddress(first + 6) || !sameAddress(first, second) ||
        !sameAddress(first + 6, second + 6) ||
        !ipv6Payload(first + 14, firstSize - 14) ||
        !ipv6Payload(second + 14, secondSize - 14)) return 0;
    const size_t padding = (4 - ((firstSize + 16) & 3)) & 3;
    const size_t size = 26 + firstSize + 16 + padding + secondSize + 16;
    if (capacity < size) return 0;
    for (unsigned i = 0; i < 26; ++i) out[i] = 0;
    out[0] = 0x88;
    copyAddress(out + 4, first);
    copyAddress(out + 10, first + 6);
    copyAddress(out + 16, BSSID);
    putLE16(out + 22, (wifiSequence & 0xfff) << 4);
    out[24] = 0x80; // A-MSDU present, normal ACK policy, TID 0.
    auto subframe = [&](size_t offset, const uint8_t *ethernet, size_t length, uint16_t sequence) {
        copyAddress(out + offset, ethernet);
        copyAddress(out + offset + 6, ethernet + 6);
        putBE16(out + offset + 12, uint16_t(length + 2));
        uint8_t *body = out + offset + 14;
        for (unsigned i = 0; i < 8; ++i) body[i] = SNAP[i];
        putLE16(body + 8, 0x0403);
        putLE16(body + 10, sequence);
        putLE16(body + 12, 0);
        putBE16(body + 14, 0x86dd);
        for (size_t i = 14; i < length; ++i) body[2 + i] = ethernet[i];
    };
    subframe(26, first, firstSize, firstSequence);
    for (size_t i = 0; i < padding; ++i) out[26 + firstSize + 16 + i] = 0;
    subframe(26 + firstSize + 16 + padding, second, secondSize, secondSequence);
    return size;
}

struct UnsupportedDataDiag {
    bool valid;
    bool snapMatch;
    bool amsduHeaderMatch;
    bool hwDeaggregated;
    uint8_t fc0, fc1, qos0;
    uint16_t mpduLen, head, pad, etherType;
    uint8_t snapBytes[8];
};

struct DataReceiveState {
    uint64_t valid, malformed, unsupported, subframes;
    uint64_t unknownPeer, otherDestination, duplicate, observed;
    UnsupportedDataDiag lastUnsupported;

    void recordUnsupported(const uint8_t *p, size_t size, bool hardwareDeaggregated)
    {
        UnsupportedDataDiag d = {};
        d.valid = true;
        d.hwDeaggregated = hardwareDeaggregated;
        d.mpduLen = uint16_t(size > 0xffff ? 0xffff : size);
        if (size >= 2) { d.fc0 = p[0]; d.fc1 = p[1]; }
        const bool qos = size >= 26 && p[0] == 0x88;
        if (qos) d.qos0 = p[24];
        const size_t header = 24 + (qos ? 2 : 0) + ((qos && (p[1] & 0x80)) ? 4 : 0);
        if (size >= header + 14) {
            d.amsduHeaderMatch = sameAddress(p + header, p + 4) && sameAddress(p + header + 6, p + 10);
        }
        const size_t bodyOff = (d.amsduHeaderMatch && size >= header + 14 + 16) ? header + 14 : header;
        if (size >= bodyOff + 8) {
            const uint8_t *body = p + bodyOff;
            d.snapMatch = true;
            for (unsigned i = 0; i < 8; ++i) {
                d.snapBytes[i] = body[i];
                if (body[i] != SNAP[i]) d.snapMatch = false;
            }
            if (size >= bodyOff + 16) {
                d.head = le16(body + 8);
                d.pad = le16(body + 12);
                d.etherType = be16(body + 14);
            }
        }
        lastUnsupported = d;
    }

    bool receive(const uint8_t *p, size_t size, PeerTable &peers, const uint8_t *self,
                 uint64_t nowUS, bool hardwareDeaggregated = false,
                 void (*deliver)(void *, const DataView &) = nullptr, void *context = nullptr,
                 bool blockAckReordered = false)
    {
        DataBatch batch = {};
        switch (parseDataMPDU(p, size, batch, hardwareDeaggregated)) {
            case ParseResult::Other: return false;
            case ParseResult::Malformed: ++malformed; return true;
            case ParseResult::Unsupported:
                ++unsupported;
                recordUnsupported(p, size, hardwareDeaggregated);
                return true;
            case ParseResult::Valid: ++valid; break;
        }
        subframes += batch.count;
        bool checkedLLWRetry = false, duplicateLLWRetry = false;
        for (unsigned i = 0; i < batch.count; ++i) {
            const DataView &frame = batch.frames[i];
            Peer *peer = peers.find(frame.source, nowUS);
            if (!peer) { ++unknownPeer; continue; }
            if (!(frame.destination[0] & 1) && !sameAddress(frame.destination, self)) {
                ++otherDestination; continue;
            }
            if (frame.lowLatency) {
                if (!checkedLLWRetry) {
                    // Subframes share the MPDU sequence. Block Ack handles hardware-split retries.
                    duplicateLLWRetry = !(blockAckReordered && hardwareDeaggregated) &&
                        frame.retry && peer->hasLLWSeq && peer->lastLLWSeq == frame.sequence;
                    peer->hasLLWSeq = true;
                    peer->lastLLWSeq = frame.sequence;
                    checkedLLWRetry = true;
                }
                if (duplicateLLWRetry) {
                    ++duplicate;
                    continue;
                }
            } else if (!peer->dataSequence.accept(frame.sequence, frame.destination[0] & 1)) {
                ++duplicate;
                continue;
            }
            ++observed;
            if (deliver) {
                if (!(frame.destination[0] & 1)) peer->lastUnicastDataUS = nowUS;
                deliver(context, frame);
            }
        }
        return true;
    }
};

} // namespace AirportAWDL
#endif
