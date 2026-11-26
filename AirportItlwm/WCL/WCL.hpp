//
//  WCL.hpp
//  AirportItlwm
//
//  Created by laobamac on 2026/10/1.
//  Copyright © 2026 laobamac. All rights reserved.
//

#ifndef AirportWCL_hpp
#define AirportWCL_hpp

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace AirportWCL {

enum class MacUpdate { Invalid, Unchanged, Busy, Change };
inline MacUpdate planMacUpdate(const uint8_t *current, const uint8_t *requested,
                               size_t length, bool initializing) {
    if (!current || !requested || length != 6 || (requested[0] & 1))
        return MacUpdate::Invalid;
    uint8_t bits = 0;
    for (unsigned i = 0; i < 6; ++i) bits |= requested[i];
    if (!bits) return MacUpdate::Invalid;
    if (memcmp(current, requested, 6) == 0) return MacUpdate::Unchanged;
    return initializing ? MacUpdate::Busy : MacUpdate::Change;
}

// Returns true when this is an internal MAC operation, not an adapter reset.
inline bool consumeMacReconfiguration(bool &pending, bool ready) {
    if (!pending) return false;
    if (ready) pending = false;
    return true;
}

struct HardwareAddress {
    uint32_t version;
    uint8_t address[6];
    uint8_t padding[2];
};
static_assert(sizeof(HardwareAddress) == 12, "HW_ADDR native wire size");
static_assert(offsetof(HardwareAddress, address) == 4, "HW_ADDR native address offset");
inline bool copyHardwareAddress(const uint8_t *address, HardwareAddress &out) {
    out = {};
    if (planMacUpdate(address, address, 6, false) == MacUpdate::Invalid) return false;
    out.version = 1;
    memcpy(out.address, address, 6);
    return true;
}

enum BeaconFlags : uint32_t {
    ProbeResponse = 1U << 0,
    SSIDPresent = 1U << 1,
    RSSIValid = 1U << 14
};

enum Message : uint32_t {
    ResetInterface = 49,
    DriverAvailable = 55,
    ScanResult = 201,
    Deauth = 203,
    AssocComplete = 211,
    FirstBeacon = 212,
    ConnectComplete = 213,
    JoinAbortComplete = 214,
    LinkStatus = 216,
    ChannelSpecQueryCompleted = 224,
    LqmUpdate = 39,
    ScanDone = 237
};

struct SequoiaLqmUpdate {
    uint8_t rssiValid;
    uint8_t reserved0[3];
    int32_t rssi;
    uint8_t reserved1[0x150 - 8];
};
static_assert(sizeof(SequoiaLqmUpdate) == 0x150, "Sequoia LQM size");
static_assert(offsetof(SequoiaLqmUpdate, rssi) == 4, "Sequoia LQM RSSI offset");

// WCLNetManager::handleLqmUpdate consumes this 0x1dc snapshot.
// Only beacon reception is valid; unavailable radio measurements stay invalid.
struct LqmBeaconUpdate {
    uint8_t prefix[0x28];
    uint32_t receivedDelta;
    uint32_t scheduledDelta;
    uint8_t beaconValid;
    uint8_t reserved[0x1d8 - 0x31];
    uint8_t snapshotValid;
    uint8_t countersValid;
    uint8_t tail[2];
};
static_assert(sizeof(LqmBeaconUpdate) == 0x1dc, "WCL LQM update size");
static_assert(offsetof(LqmBeaconUpdate, receivedDelta) == 0x28, "WCL beacon delta offset");
static_assert(offsetof(LqmBeaconUpdate, beaconValid) == 0x30, "WCL beacon valid offset");
static_assert(offsetof(LqmBeaconUpdate, snapshotValid) == 0x1d8, "WCL snapshot valid offset");
inline bool makeBeaconUpdate(uint32_t receivedDelta, LqmBeaconUpdate &out)
{
    out = {};
    if (!receivedDelta)
        return false;
    out.receivedDelta = receivedDelta;
    out.beaconValid = out.snapshotValid = out.countersValid = 1;
    return true;
}

struct DriverAvailability {
    uint32_t version;
    uint32_t reserved0;
    uint32_t available;
    uint32_t reserved1;
    uint32_t reason;
    uint32_t subReason;
    uint8_t reserved2[0xf8 - 24];
};

static_assert(sizeof(DriverAvailability) == 0xf8, "DriverAvailability size");

struct BeaconMetadata {
    uint32_t ieLength;
    uint16_t channelSpec;
    uint8_t ssid[32];
    uint8_t ssidLength;
    uint8_t channel;
    uint8_t reserved0;
    uint8_t bssid[6];
    uint8_t reserved1;
    int32_t rssi;
    int16_t noise;
    int16_t snr;
    uint16_t interval;
    uint16_t capability;
#if !defined(__IO80211_TARGET) || __IO80211_TARGET != __MAC_15_2
    uint32_t reserved2;
#endif
    uint32_t flags;
};

struct AssociationEvent {
    uint16_t status;
    uint16_t reason;
    uint8_t mlo;
    uint8_t bssid[6];
    uint8_t reserved;
    uint32_t mloData[2];
    int32_t driverStatus;
    int32_t driverReason;
};

struct ConnectionPeer {
    uint8_t bssid[6];
    uint16_t reserved;
    int32_t driverStatus;
    int32_t driverReason;
};

struct FirstBeaconEvent {
    uint16_t status;
    uint16_t reason;
    ConnectionPeer peer;
};

struct ConnectionEvent {
    uint16_t status;
    uint16_t reason;
    ConnectionPeer peers[10];
};

struct LinkStatusEvent {
    uint8_t bssid[6];
    uint8_t up;
    uint8_t inSleep;
    uint32_t reason;
    uint32_t reserved;
};
static_assert(sizeof(LinkStatusEvent) == 16, "LinkStatusEvent size");
static_assert(offsetof(LinkStatusEvent, up) == 6, "LinkStatusEvent up offset");

struct DeauthEvent {
    int32_t driverStatus;
    int32_t driverReason;
    uint16_t reason;
    uint8_t bssid[6];
    uint8_t disassociation;
    uint8_t reserved[3];
};

struct __attribute__((packed)) ChannelEntry {
    uint16_t channelSpec;
    uint8_t reserved[4];
    uint8_t channel;
    uint8_t flags;
};

struct __attribute__((packed)) ChannelInfo {
    ChannelEntry channels[400];
    uint16_t count;
    uint8_t country[3];
    uint8_t supports6GHz;
};

#if defined(__IO80211_TARGET) && __IO80211_TARGET == __MAC_15_2
static_assert(sizeof(BeaconMetadata) == 0x40, "Sequoia BeaconMetadata size");
static_assert(offsetof(BeaconMetadata, flags) == 0x3c, "Sequoia BeaconMetadata flags");
#else
static_assert(sizeof(BeaconMetadata) == 0x44, "BeaconMetadata size");
static_assert(offsetof(BeaconMetadata, flags) == 0x40, "BeaconMetadata flags");
#endif
static_assert(offsetof(BeaconMetadata, bssid) == 0x29, "BeaconMetadata BSSID");
static_assert(offsetof(BeaconMetadata, rssi) == 0x30, "BeaconMetadata RSSI");
static_assert(sizeof(AssociationEvent) == 0x1c, "AssociationEvent size");
static_assert(sizeof(FirstBeaconEvent) == 0x14, "FirstBeaconEvent size");
static_assert(sizeof(ConnectionEvent) == 0xa4, "ConnectionEvent size");
static_assert(sizeof(DeauthEvent) == 0x14, "DeauthEvent size");
static_assert(sizeof(ChannelInfo) == 0xc86, "ChannelInfo size");

struct NativeChannelInfo {
    uint8_t reserved0[8];
    uint8_t country[4];
    uint16_t count;
    uint16_t channelSpec[400];
    uint8_t channel[400];
    uint8_t indoor[400];
    uint8_t dfs[400];
    uint8_t passive[400];
    uint8_t supports40MHz[400];
    uint8_t supports80MHz[400];
    uint8_t reserved1[2];
    uint32_t firmwareChannelSpec[400];
    uint32_t channelBitmap[400];
    uint8_t reserved2[0x2720 - 0x1910];
};
static_assert(sizeof(NativeChannelInfo) == 0x2720, "CHANNELS_INFO native size");
static_assert(offsetof(NativeChannelInfo, country) == 8, "CHANNELS_INFO country");
static_assert(offsetof(NativeChannelInfo, count) == 0xc, "CHANNELS_INFO count");
static_assert(offsetof(NativeChannelInfo, channelSpec) == 0xe, "CHANNELS_INFO chanspec");
static_assert(offsetof(NativeChannelInfo, channel) == 0x32e, "CHANNELS_INFO channel");
static_assert(offsetof(NativeChannelInfo, indoor) == 0x4be, "CHANNELS_INFO indoor");
static_assert(offsetof(NativeChannelInfo, dfs) == 0x64e, "CHANNELS_INFO DFS");
static_assert(offsetof(NativeChannelInfo, passive) == 0x7de, "CHANNELS_INFO passive");
static_assert(offsetof(NativeChannelInfo, supports40MHz) == 0x96e, "CHANNELS_INFO 40 MHz");
static_assert(offsetof(NativeChannelInfo, supports80MHz) == 0xafe, "CHANNELS_INFO 80 MHz");
static_assert(offsetof(NativeChannelInfo, firmwareChannelSpec) == 0xc90, "CHANNELS_INFO firmware chanspec");
static_assert(offsetof(NativeChannelInfo, channelBitmap) == 0x12d0, "CHANNELS_INFO bitmap");

inline bool appendChannel(NativeChannelInfo &info, unsigned channel, bool fiveGHz,
                          bool dfs, bool passive, bool supports40MHz, bool supports80MHz)
{
    if (!channel || channel > 255 || info.count >= 400)
        return false;
    unsigned index = info.count++;
    uint16_t spec = uint16_t(channel | (fiveGHz ? 0xd000 : 0x1000));
    info.channelSpec[index] = spec;
    info.firmwareChannelSpec[index] = spec;
    info.channel[index] = channel;
    info.dfs[index] = dfs;
    info.passive[index] = passive;
    info.supports40MHz[index] = supports40MHz;
    info.supports80MHz[index] = supports80MHz;
    info.channelBitmap[index] = 3U | (fiveGHz ? 4U : 0U) |
        (dfs ? 8U : 0U) | (passive ? 32U : 0U);
    return true;
}

struct TrafficCounters {
    uint64_t realtimeInfraTx;
    uint64_t realtimeInfraRx;
    uint64_t infraTx;
    uint64_t infraRx;
    uint64_t realtimeAwdlTx;
    uint64_t realtimeNanTx;
    uint64_t timestampNanoseconds;
};
static_assert(sizeof(TrafficCounters) == 0x38, "WCL_TRAFFIC_COUNTERS native size");
static_assert(offsetof(TrafficCounters, timestampNanoseconds) == 0x30, "WCL traffic timestamp");

inline TrafficCounters makeTrafficCounters(const uint64_t *tx, const uint64_t *rx,
                                           uint64_t timestampNanoseconds)
{
    TrafficCounters out = {};
    out.realtimeInfraTx = tx[2] + tx[3];
    out.realtimeInfraRx = rx[2] + rx[3];
    for (unsigned ac = 0; ac < 4; ++ac) {
        out.infraTx += tx[ac];
        out.infraRx += rx[ac];
    }
    out.timestampNanoseconds = timestampNanoseconds;
    return out;
}

struct ScanTiming {
    uint32_t maxOutTU;
    bool fragmented;
    uint32_t suspendTU;
};

inline ScanTiming scanTiming(uint32_t awayMilliseconds, bool associated,
                             bool supportsFragmentation)
{
    if (!associated)
        return {0, false, 0};
    uint64_t requested = uint64_t(awayMilliseconds) * 1000 / 1024;
    // A normal 110 ms request must fit a full 110 TU passive dwell.
    // Reserve fragmentation for explicit low-latency scans.
    if (awayMilliseconds && requested < 100 && supportsFragmentation)
        return {requested < 44 ? 44 : uint32_t(requested), true, 95};
    return {120, false, 120};
}

inline uint16_t read16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}

inline uint32_t read32(const uint8_t *p) {
    return uint32_t(read16(p)) | uint32_t(read16(p + 2)) << 16;
}

inline bool validIEs(const uint8_t *data, size_t length) {
    if (!data || length > 2048)
        return false;
    while (length) {
        if (length < 2 || size_t(data[1]) + 2 > length)
            return false;
        size_t next = size_t(data[1]) + 2;
        data += next;
        length -= next;
    }
    return true;
}

inline bool scanUsesProbes(bool requested, bool activeRequest) {
    return requested ? activeRequest : true;
}

// Present the supported PSK option of a PSK/SAE transition network to WCL.
// This operates on the scan-message copy, never the received beacon or the
// net80211 RSN IE used to validate the four-way handshake. SAE-only, mandatory
// PMF and other AKM combinations must not be converted into WPA2 networks.
inline size_t selectWPA2TransitionMode(uint8_t *ies, size_t length) {
    if (!validIEs(ies, length))
        return length;
    for (size_t offset = 0; offset < length; ) {
        uint8_t *ie = ies + offset;
        size_t size = size_t(ie[1]) + 2;
        if (ie[0] != 48 || size < 10 || read16(ie + 2) != 1) {
            offset += size;
            continue;
        }
        size_t pairwiseCount = read16(ie + 8);
        if (!pairwiseCount || pairwiseCount > (size - 10) / 4) {
            offset += size;
            continue;
        }
        size_t countOffset = 10 + 4 * pairwiseCount;
        if (size - countOffset < 2) {
            offset += size;
            continue;
        }
        size_t count = read16(ie + countOffset);
        size_t suitesOffset = countOffset + 2;
        if (count < 2 || count > (size - suitesOffset) / 4) {
            offset += size;
            continue;
        }
        size_t tailOffset = suitesOffset + 4 * count;
        // PSK/SAE transition mode advertises PMF capable, but not required.
        if (size - tailOffset < 2 || (read16(ie + tailOffset) & 0xc0) != 0x80) {
            offset += size;
            continue;
        }
        bool psk = false, sae = false, other = false;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *suite = ie + suitesOffset + 4 * i;
            if (suite[0] != 0 || suite[1] != 0x0f || suite[2] != 0xac)
                other = true;
            else if (suite[3] == 2)
                psk = true;
            else if (suite[3] == 8)
                sae = true;
            else
                other = true;
        }
        if (!psk || !sae || other) {
            offset += size;
            continue;
        }
        const uint8_t pskSuite[] = {0, 0x0f, 0xac, 2};
        memcpy(ie + suitesOffset, pskSuite, sizeof(pskSuite));
        ie[countOffset] = 1;
        ie[countOffset + 1] = 0;
        size_t removed = 4 * (count - 1);
        // Keep RSN capabilities, PMKIDs, group-management cipher and all
        // following information elements byte-for-byte intact.
        memmove(ie + suitesOffset + 4, ie + tailOffset,
                length - offset - tailOffset);
        ie[1] -= removed;
        length -= removed;
        offset += size - removed;
    }
    return length;
}
inline bool isHiddenSSID(const uint8_t *ssid, size_t length) {
    if (!ssid || !length || length > 32)
        return true;
    uint8_t any = 0;
    for (size_t i = 0; i < length; ++i)
        any |= ssid[i];
    return any == 0;
}

// Splice a known non-NUL SSID into Tag 0 of a hidden Beacon's IE buffer copy
// (when Tag 0 is missing, empty, or all-NUL) so IO80211BSSBeacon preserves its
// directed-network state (+0x52c == 1) and Probe Response RSN/SSID cache.
inline size_t spliceHiddenBeaconSSID(const uint8_t *ies, size_t length,
                                     const uint8_t *knownSsid, size_t knownLen,
                                     uint8_t *out, size_t outCapacity) {
    if (!ies || !out || length > outCapacity)
        return 0;
    if (!validIEs(ies, length) || isHiddenSSID(knownSsid, knownLen)) {
        memcpy(out, ies, length);
        return length;
    }
    for (size_t offset = 0; offset < length; ) {
        const uint8_t *ie = ies + offset;
        const size_t bodyLen = ie[1];
        const size_t ieSize = bodyLen + 2;
        if (ie[0] == 0) {
            if (!isHiddenSSID(ie + 2, bodyLen)) {
                memcpy(out, ies, length);
                return length;
            }
            const size_t splicedLen = length - bodyLen + knownLen;
            if (splicedLen > 2048 || splicedLen > outCapacity) {
                memcpy(out, ies, length);
                return length;
            }
            if (offset)
                memcpy(out, ies, offset);
            out[offset] = 0;
            out[offset + 1] = uint8_t(knownLen);
            memcpy(out + offset + 2, knownSsid, knownLen);
            const size_t tailOffset = offset + ieSize;
            if (tailOffset < length)
                memcpy(out + offset + 2 + knownLen, ies + tailOffset, length - tailOffset);
            return splicedLen;
        }
        offset += ieSize;
    }
    const size_t prependedLen = 2 + knownLen + length;
    if (prependedLen > 2048 || prependedLen > outCapacity) {
        memcpy(out, ies, length);
        return length;
    }
    out[0] = 0;
    out[1] = uint8_t(knownLen);
    memcpy(out + 2, knownSsid, knownLen);
    if (length)
        memcpy(out + 2 + knownLen, ies, length);
    return prependedLen;
}

inline bool probeChannel(bool active, bool passiveOnly, bool dfs) {
    return active && !passiveOnly && !dfs;
}

struct ScanRequest {
    uint32_t identifier;
    uint32_t type;
    uint8_t ssid[32];
    uint32_t ssidLength;
    uint8_t bssid[6];
    uint32_t channelCount;
    uint8_t channels[32];
    uint32_t ssidCount;
    uint8_t ssidLengths[10];
    uint8_t ssids[10][32];
};

inline bool hasDirectedSSID(const ScanRequest &scan) {
    if (!isHiddenSSID(scan.ssid, scan.ssidLength))
        return true;
    const unsigned count = scan.ssidCount <= 10 ? scan.ssidCount : 10;
    for (unsigned i = 0; i < count; ++i) {
        if (!isHiddenSSID(scan.ssids[i], scan.ssidLengths[i]))
            return true;
    }
    return false;
}

template <typename Entry>
inline void fillScanSSIDs(Entry *entries, unsigned count, const uint8_t *lengths,
                         const uint8_t (*names)[32]) {
    for (unsigned i = 0; i < count; ++i) {
        entries[i].id = 0;
        entries[i].len = lengths[i];
        memcpy(entries[i].ssid, names[i], lengths[i]);
    }
}

inline bool scanChannelSelected(const uint8_t *channels, bool restricted, unsigned channel,
                                unsigned joinChannel = 0) {
    return (!joinChannel || channel == joinChannel) &&
        (!restricted || (channel < 256 && (channels[channel / 8] & (1U << (channel % 8)))));
}

inline unsigned joinChannelHint(uint16_t spec) {
    // These are the 20 MHz encodings emitted in our native scan results.
    // Unknown or wider encodings use the full scan instead of guessing a primary.
    unsigned channel = spec & 0xff;
    if ((spec & 0xff00) == 0x1000 && channel >= 1 && channel <= 14)
        return channel;
    if ((spec & 0xff00) == 0xd000 && channel > 14 && channel <= 196)
        return channel;
    return 0;
}

inline bool matchesScan(const ScanRequest &scan, const uint8_t *bssid,
                        const uint8_t *ssid, size_t ssidLength, unsigned channel) {
    uint8_t any = 0, all = 0xff;
    for (unsigned i = 0; i < 6; ++i) {
        any |= scan.bssid[i];
        all &= scan.bssid[i];
    }
    if (any && all != 0xff && memcmp(scan.bssid, bssid, 6))
        return false;
    if (scan.ssidCount) {
        bool matched = false;
        for (unsigned i = 0; i < scan.ssidCount; ++i)
            if (!scan.ssidLengths[i] || (scan.ssidLengths[i] == ssidLength &&
                !memcmp(scan.ssids[i], ssid, ssidLength))) matched = true;
        if (!matched) return false;
    } else if (scan.ssidLength && (scan.ssidLength != ssidLength ||
               memcmp(scan.ssid, ssid, ssidLength))) return false;
    return scanChannelSelected(scan.channels, scan.channelCount != 0, channel);
}

inline bool decodeScan(const void *buffer, size_t length, ScanRequest &out) {
    if (!buffer || length < 0x58)
        return false;
    const uint8_t *p = static_cast<const uint8_t *>(buffer);
    uint32_t count = read32(p + 0x54), ssidLength = read32(p + 0x1c);
    uint32_t type = read32(p + 0x40);
    if (count > 400 || ssidLength > 32 || type < 1 || type > 4 ||
        length < 0x58 + size_t(count) * 12)
        return false;
    out = {};
    out.identifier = read32(p);
    out.type = type;
    out.ssidLength = ssidLength;
    out.channelCount = count;
    memcpy(out.ssid, p + 0x20, ssidLength);
    memcpy(out.bssid, p + 0x14, 6);
    // WCL carries up to ten SSIDs, including the native wildcard entry.
    if (length >= 0x131c) {
        out.ssidCount = read32(p + 0x1318);
        if (out.ssidCount > 10 || length < 0x131c + size_t(out.ssidCount) * 56)
            return false;
        for (unsigned i = 0; i < out.ssidCount; ++i) {
            const uint8_t *entry = p + 0x131c + i * 56;
            const uint32_t size = read32(entry + 4);
            if (size > 32) return false;
            out.ssidLengths[i] = size;
            memcpy(out.ssids[i], entry + 8, size);
        }
    }
    // Native entries are apple80211_channel: version, channel, flags (12 bytes).
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = p + 0x58 + size_t(i) * 12;
        uint32_t channel = read32(entry + 4), flags = read32(entry + 8);
        // Do not confuse 6 GHz channel numbers with overlapping 2.4 GHz ones.
        if (!channel || channel > 196 || (flags & 0x2000))
            continue;
        if (((flags & 0x8) && channel > 14) || ((flags & 0x10) && channel <= 14))
            continue;
        out.channels[channel / 8] |= 1U << (channel % 8);
    }
    return true;
}

struct JoinRequest {
    uint32_t lowerAuth;
    uint32_t upperAuth;
    uint32_t ssidLength;
    uint8_t ssid[32];
    uint32_t keyLength;
    uint32_t keyCipher;
    uint16_t keyIndex;
    uint8_t key[32];
    uint8_t bssid[6];
    uint16_t channelSpec;
    uint16_t rsnLength;
    uint8_t rsn[257];
};

inline bool validCipherKey(uint32_t cipher, uint32_t length, uint16_t flags,
                           uint16_t index, uint32_t rscLength) {
    // Native WCL accepts PMK/MSK for enterprise, not legacy PTK/GTK.
    (void)flags; // PTK/GTK selectors do not apply to PMK material.
    (void)index;
    return (cipher == 6 || cipher == 7 || cipher == 9) && length == 32 && rscLength <= 8;
}

inline bool validRSNOverride(const uint8_t *ie, size_t length) {
    if (!ie || length > 257) return false;
    if (!length) return true;
    if (length < 4 || size_t(ie[1]) + 2 != length) return false;
    if (ie[0] == 48) return ie[2] == 1 && ie[3] == 0;
    return length >= 8 && ie[0] == 221 && ie[2] == 0 && ie[3] == 0x50 &&
        ie[4] == 0xf2 && ie[5] == 1 && ie[6] == 1 && ie[7] == 0;
}

enum class LegacyAuth { Invalid, Open, Wep, Personal, Enterprise };
inline LegacyAuth classifyLegacyAuth(const JoinRequest &j) {
    // Values from apple80211_authtype/cipher_type, used by the WCL association request.
    if (!validRSNOverride(j.rsn, j.rsnLength)) return LegacyAuth::Invalid;
    if (j.lowerAuth != 1) return LegacyAuth::Invalid;
    if (!j.upperAuth) {
        if (!j.keyLength) return LegacyAuth::Open;
        if (j.keyIndex < 4 && ((j.keyLength == 5 && j.keyCipher == 1) ||
                              (j.keyLength == 13 && j.keyCipher == 2)))
            return LegacyAuth::Wep;
        return LegacyAuth::Invalid;
    }
    const uint32_t personal = (1u << 1) | (1u << 3) | (1u << 10);
    const uint32_t personalTransition = (1u << 4) | (1u << 12) | (1u << 13);
    const uint32_t enterprise = (1u << 0) | (1u << 2) | (1u << 11);
    if (j.upperAuth & personal) {
        if (j.upperAuth & ~(personal | personalTransition)) return LegacyAuth::Invalid;
        return j.keyLength == 32 && (j.keyCipher == 6 || j.keyCipher == 7) ?
            LegacyAuth::Personal : LegacyAuth::Invalid;
    }
    if (j.upperAuth & ~enterprise) return LegacyAuth::Invalid;
    if (j.upperAuth & enterprise)
        return (!j.keyLength || (j.keyLength == 32 && (j.keyCipher == 6 || j.keyCipher == 7))) ?
            LegacyAuth::Enterprise : LegacyAuth::Invalid;
    return LegacyAuth::Invalid;
}

inline bool decodeJoin(const void *buffer, size_t length, JoinRequest &out) {
    if (!buffer || length < 0x218)
        return false;
    const uint8_t *p = static_cast<const uint8_t *>(buffer);
    uint32_t count = read32(p + 0x214), ssidLength = read32(p + 0x1c);
    uint32_t keyLength = read32(p + 0x44);
    uint16_t rsnLength = read16(p + 0xd4);
    if (!count || count > 10 || !ssidLength || ssidLength > 32 ||
        keyLength > 32 || rsnLength > 257 || length < 0x218 + size_t(count) * 18)
        return false;
    if (p[0x21c] & 1)
        return false;
    uint8_t addressBits = 0;
    for (unsigned i = 0; i < 6; ++i)
        addressBits |= p[0x21c + i];
    if (!addressBits)
        return false;
    out = {};
    out.lowerAuth = read32(p + 0x10);
    out.upperAuth = read32(p + 0x14);
    out.ssidLength = ssidLength;
    memcpy(out.ssid, p + 0x20, ssidLength);
    out.keyLength = keyLength;
    out.keyCipher = read32(p + 0x48);
    out.keyIndex = read16(p + 0x4e);
    memcpy(out.key, p + 0x50, keyLength);
    memcpy(out.bssid, p + 0x21c, 6);
    out.channelSpec = read16(p + 0x228);
    out.rsnLength = rsnLength;
    memcpy(out.rsn, p + 0xd6, rsnLength);
    return true;
}

struct InfraBandwidths {
    uint64_t maxBps;
    uint64_t effectiveBps;
};

inline InfraBandwidths computeInfraBandwidths(uint32_t negotiatedTxKbps,
                                              unsigned channelWidthCode,
                                              bool isHE,
                                              bool isVHT,
                                              bool isHT,
                                              unsigned txNss) {
    const unsigned streams = txNss >= 2 ? 2U : 1U;
    uint64_t ceilingBps = 54000000ULL;
    if (isHE) {
        const uint64_t perStream = channelWidthCode >= 3 ? 1201000000ULL :
            (channelWidthCode == 2 ? 600500000ULL :
             (channelWidthCode == 1 ? 286800000ULL : 143400000ULL));
        ceilingBps = perStream * streams;
    } else if (isVHT) {
        const uint64_t perStream = channelWidthCode >= 3 ? 866650000ULL :
            (channelWidthCode == 2 ? 433300000ULL :
             (channelWidthCode == 1 ? 200000000ULL : 86700000ULL));
        ceilingBps = perStream * streams;
    } else if (isHT) {
        const uint64_t perStream = channelWidthCode >= 1 ? 150000000ULL : 72200000ULL;
        ceilingBps = perStream * streams;
    }
    const uint64_t negotiatedBps = uint64_t(negotiatedTxKbps) * 1000ULL;
    uint64_t maxBps = negotiatedBps > ceilingBps ? negotiatedBps : ceilingBps;
    if (maxBps > 4000000000ULL)
        maxBps = 4000000000ULL;
    const uint64_t floorEffBase = (ceilingBps * 80ULL) / 100ULL;
    const uint64_t effBase = negotiatedBps > floorEffBase ? negotiatedBps : floorEffBase;
    uint64_t effectiveBps = (effBase * 85ULL) / 100ULL;
    if (effectiveBps > maxBps)
        effectiveBps = maxBps;
    return {maxBps, effectiveBps};
}

inline uint64_t multiSsidDigest(const ScanRequest &scan) {
    const unsigned count = scan.ssidCount <= 10 ? scan.ssidCount : 10;
    uint64_t hash = 1469598103934665603ULL;
    hash = (hash ^ uint64_t(count)) * 1099511628211ULL;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned len = scan.ssidLengths[i] <= 32 ? scan.ssidLengths[i] : 32;
        hash = (hash ^ uint64_t(len)) * 1099511628211ULL;
        for (unsigned b = 0; b < len; ++b)
            hash = (hash ^ uint64_t(scan.ssids[i][b])) * 1099511628211ULL;
    }
    return hash ? hash : 1ULL;
}

template <typename Node>
inline auto nodeMatchesSSIDImpl(const Node *ni, const uint8_t *ssid, size_t len, int)
    -> decltype(bool(size_t(ni->ni_esslen) == len && memcmp(ni->ni_essid, ssid, len) == 0)) {
    return ni && size_t(ni->ni_esslen) == len && memcmp(ni->ni_essid, ssid, len) == 0;
}
template <typename Node>
inline bool nodeMatchesSSIDImpl(const Node *, const uint8_t *, size_t, long) {
    return false;
}
template <typename Node>
inline bool nodeMatchesSSID(const Node *ni, const uint8_t *ssid, size_t len) {
    return nodeMatchesSSIDImpl(ni, ssid, len, 0);
}

template <typename Ifp>
inline auto readIfpDirectBytes(const Ifp &ifp, int)
    -> decltype(uint64_t(ifp.if_ibytes) + uint64_t(ifp.if_obytes)) {
    return uint64_t(ifp.if_ibytes) + uint64_t(ifp.if_obytes);
}
template <typename Ifp>
inline auto readIfpDirectBytes(const Ifp &ifp, long)
    -> decltype(uint64_t(ifp.if_data.ifi_ibytes) + uint64_t(ifp.if_data.ifi_obytes)) {
    return uint64_t(ifp.if_data.ifi_ibytes) + uint64_t(ifp.if_data.ifi_obytes);
}
template <typename Ifp>
inline uint64_t readIfpDirectBytes(const Ifp &, ...) {
    return 0;
}

template <typename Inst>
inline auto readInstanceBsdBytes(Inst *inst, int)
    -> decltype((void)inst->bsdInterface->getIfnet(), uint64_t(0)) {
#if defined(KERNEL) && defined(__KPI_INTERFACE__)
    if (inst && inst->bsdInterface) {
        if (auto bsdIfp = inst->bsdInterface->getIfnet()) {
            struct ifnet_stats_param stats = {};
            if (ifnet_stat(bsdIfp, &stats) == 0)
                return uint64_t(stats.bytes_in) + uint64_t(stats.bytes_out);
        }
    }
#else
    (void)inst;
#endif
    return 0;
}
template <typename Inst>
inline uint64_t readInstanceBsdBytes(Inst *, long) {
    return 0;
}

template <typename Ifp, typename Inst>
inline uint64_t sampleInfraTotalBytes(const Ifp &ifp, Inst *inst) {
    const uint64_t bsdBytes = readInstanceBsdBytes(inst, 0);
    const uint64_t directBytes = readIfpDirectBytes(ifp, 0);
    return bsdBytes >= directBytes ? bsdBytes : directBytes;
}

template <typename Hal>
inline auto abortDriverScanImpl(Hal *hal, int)
    -> decltype((void)hal->getDriverController()->abortScanning()) {
    if (hal && hal->getDriverController())
        hal->getDriverController()->abortScanning();
}
template <typename Hal>
inline auto abortDriverScanImpl(Hal *hal, long)
    -> decltype((void)hal->getDriverController()->clearScanningFlags()) {
    if (hal && hal->getDriverController())
        hal->getDriverController()->clearScanningFlags();
}
template <typename Hal>
inline void abortDriverScanImpl(Hal *, ...) {}

template <typename Hal>
inline void abortDriverScan(Hal *hal) {
    abortDriverScanImpl(hal, 0);
}

struct InfraTrafficTracker {
    uint64_t lastBytes;
    uint64_t lastSampleUS;
    uint64_t activeUntilUS;
    bool initialized;

    void reset() {
        lastBytes = 0;
        lastSampleUS = 0;
        activeUntilUS = 0;
        initialized = false;
    }

    bool sample(uint64_t totalBytes, uint64_t nowUS,
                uint64_t holdWindowUS = 2000000ULL,
                uint64_t minBurstBytes = 262144ULL,
                uint64_t minRateBytesPerSec = 131072ULL) {
        if (!initialized || nowUS < lastSampleUS || totalBytes < lastBytes) {
            lastBytes = totalBytes;
            lastSampleUS = nowUS;
            initialized = true;
            return nowUS < activeUntilUS;
        }
        const uint64_t dtUS = nowUS - lastSampleUS;
        const uint64_t deltaBytes = totalBytes - lastBytes;
        if (dtUS > 0) {
            const bool recentBurst = (dtUS <= holdWindowUS && deltaBytes >= minBurstBytes);
            const bool sustainedRate = (dtUS <= 10000000ULL && deltaBytes >= minBurstBytes &&
                                        (deltaBytes * 1000000ULL) >= (minRateBytesPerSec * dtUS));
            if (recentBurst || sustainedRate)
                activeUntilUS = nowUS + holdWindowUS;
            if (dtUS >= 250000ULL || recentBurst) {
                lastBytes = totalBytes;
                lastSampleUS = nowUS;
            }
        } else if (deltaBytes >= minBurstBytes) {
            activeUntilUS = nowUS + holdWindowUS;
            lastBytes = totalBytes;
        }
        return nowUS < activeUntilUS;
    }
};

struct SplitScanBurst {
    uint64_t startUS;
    uint64_t lastStepUS;
    uint64_t multiSsidDigest;
    uint64_t lastMultiSsidScanUS;
    InfraTrafficTracker traffic;
    bool liveStepInFlight;
    bool abortedScanInScanState;
    uint8_t coveredChannels[32];

    void reset() {
        startUS = 0;
        lastStepUS = 0;
        multiSsidDigest = 0;
        lastMultiSsidScanUS = 0;
        traffic.reset();
        liveStepInFlight = false;
        abortedScanInScanState = false;
        memset(coveredChannels, 0, sizeof(coveredChannels));
    }

    void resetBurstOnly() {
        startUS = 0;
        lastStepUS = 0;
        liveStepInFlight = false;
        memset(coveredChannels, 0, sizeof(coveredChannels));
    }

    bool isContinuation(const ScanRequest &scan, uint64_t nowUS,
                        uint64_t stepWindowUS = 5000000ULL) const {
        if (!startUS || !scan.channelCount)
            return false;
        const uint64_t refUS = lastStepUS ? lastStepUS : startUS;
        if (nowUS < refUS || (nowUS - refUS) > stepWindowUS)
            return false;
        for (unsigned i = 0; i < sizeof(coveredChannels); ++i) {
            if (scan.channels[i] & ~coveredChannels[i])
                return true;
        }
        return false;
    }

    void recordLiveScan(const ScanRequest &scan, uint64_t nowUS,
                        bool continuation) {
        if (!scan.channelCount) {
            resetBurstOnly();
            liveStepInFlight = true;
            return;
        }
        liveStepInFlight = true;
        if (!continuation) {
            startUS = nowUS;
            lastStepUS = nowUS;
            memcpy(coveredChannels, scan.channels, sizeof(coveredChannels));
        } else {
            lastStepUS = nowUS;
            for (unsigned i = 0; i < sizeof(coveredChannels); ++i)
                coveredChannels[i] |= scan.channels[i];
        }
    }

    void noteScanFinished(uint64_t nowUS) {
        if (!liveStepInFlight)
            return;
        liveStepInFlight = false;
        if (startUS && nowUS >= lastStepUS)
            lastStepUS = nowUS;
    }

    bool shouldThrottleMultiSsidBatch(uint64_t digest, uint64_t nowUS,
                                      uint64_t cooldownUS = 20000000ULL) const {
        if (!digest || digest != multiSsidDigest || !lastMultiSsidScanUS ||
            nowUS < lastMultiSsidScanUS)
            return false;
        return (nowUS - lastMultiSsidScanUS) < cooldownUS;
    }

    void recordMultiSsidBatch(uint64_t digest, uint64_t nowUS) {
        multiSsidDigest = digest;
        lastMultiSsidScanUS = nowUS;
    }
};

struct CountryDeduplicator {
    char lastEffective[3];
    char lastFirmware[3];
    bool valid;

    void reset() {
        memset(lastEffective, 0, sizeof(lastEffective));
        memset(lastFirmware, 0, sizeof(lastFirmware));
        valid = false;
    }

    void noteDriverReset() {
        // Keep lastEffective across radio toggles/wakes so world '00' scans do not
        // spam COUNTRY_CODE_CHANGED, while clearing lastFirmware so the next real MCC
        // regulatory unlock still notifies WCL and AWDL once.
        lastFirmware[0] = '0';
        lastFirmware[1] = '0';
        lastFirmware[2] = '\0';
    }

    static bool isWorldCode(const char *cc) {
        return !cc || !cc[0] || !cc[1] ||
               (cc[0] == '0' && cc[1] == '0') ||
               (cc[0] == 'Z' && cc[1] == 'Z') ||
               (cc[0] == 'z' && cc[1] == 'z') ||
               (cc[0] == 'X' && cc[1] == '0') ||
               (cc[0] == 'x' && cc[1] == '0');
    }

    bool shouldNotify(const char *effectiveCC, const char *firmwareCC) {
        if (isWorldCode(effectiveCC))
            return false;
        const char eff0 = effectiveCC[0];
        const char eff1 = effectiveCC[1];
        const bool fwWorld = isWorldCode(firmwareCC);
        const char fw0 = fwWorld ? '0' : firmwareCC[0];
        const char fw1 = fwWorld ? '0' : firmwareCC[1];
        const bool effChanged = !valid || lastEffective[0] != eff0 || lastEffective[1] != eff1;
        const bool fwChanged = !fwWorld && (!valid || lastFirmware[0] != fw0 || lastFirmware[1] != fw1);
        if (!effChanged && !fwChanged)
            return false;
        lastEffective[0] = eff0;
        lastEffective[1] = eff1;
        lastEffective[2] = '\0';
        lastFirmware[0] = fw0;
        lastFirmware[1] = fw1;
        lastFirmware[2] = '\0';
        valid = true;
        return true;
    }
};

struct SplitScanSlot {
    const void *owner;
    SplitScanBurst burst;
    CountryDeduplicator country;
};

inline SplitScanSlot *splitScanSlots() {
    static SplitScanSlot slots[4];
    return slots;
}

inline SplitScanSlot &splitScanSlotFor(const void *owner) {
    SplitScanSlot *slots = splitScanSlots();
    for (unsigned i = 0; i < 4; ++i) {
        if (slots[i].owner == owner)
            return slots[i];
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (!slots[i].owner) {
            slots[i].owner = owner;
            slots[i].burst.reset();
            slots[i].country.reset();
            return slots[i];
        }
    }
    slots[0].owner = owner;
    slots[0].burst.reset();
    slots[0].country.reset();
    return slots[0];
}

inline SplitScanBurst &splitScanBurstFor(const void *owner) {
    return splitScanSlotFor(owner).burst;
}

inline CountryDeduplicator &countryDeduplicatorFor(const void *owner) {
    return splitScanSlotFor(owner).country;
}

inline void noteAbortedScanInScanState(const void *owner) {
    splitScanBurstFor(owner).abortedScanInScanState = true;
}

inline bool consumeAbortedScanInScanState(const void *owner) {
    SplitScanBurst &burst = splitScanBurstFor(owner);
    const bool aborted = burst.abortedScanInScanState;
    burst.abortedScanInScanState = false;
    return aborted;
}

inline void resetSplitScanBurst(const void *owner, bool releaseSlot = false) {
    SplitScanSlot *slots = splitScanSlots();
    for (unsigned i = 0; i < 4; ++i) {
        if (slots[i].owner == owner) {
            slots[i].burst.reset();
            if (releaseSlot) {
                slots[i].country.reset();
                slots[i].owner = nullptr;
            }
        }
    }
}

inline bool shouldThrottleAssociatedBgscan(uint64_t lastLiveScanUS,
                                           uint64_t nowUS,
                                           int32_t rssiDbm,
                                           uint64_t cooldownUS = 45000000ULL,
                                           int32_t strongRssiFloorDbm = -68,
                                           bool directedSSID = false,
                                           bool splitContinuation = false) {
    if (directedSSID || splitContinuation)
        return false;
    if (!lastLiveScanUS || nowUS < lastLiveScanUS)
        return false;
    if (rssiDbm < strongRssiFloorDbm)
        return false;
    return (nowUS - lastLiveScanUS) < cooldownUS;
}

}
#endif
