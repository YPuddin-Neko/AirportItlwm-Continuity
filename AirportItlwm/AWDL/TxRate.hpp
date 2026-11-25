// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_TX_RATE_HPP
#define AIRPORT_AWDL_TX_RATE_HPP

#include <stdint.h>

namespace AirportAWDL {

// Legacy rates use Mbps; 0x80..0x87 identify single-stream HT20 MCS0..7, long GI.
struct TxRateControl {
    uint64_t epoch, dmaEpoch, probeToken;
    uint16_t channel;
    uint8_t floor, requestedFloor, current, clean, htMcsMask;

    static unsigned halfMbps(uint8_t rate) {
        static const uint8_t ht[] = {13,26,39,52,78,104,117,130};
        return (rate & 0xf8) == 0x80 ? ht[rate & 7] : unsigned(rate) * 2;
    }

    static uint8_t roundedFloor(unsigned mbps)
    {
        const uint8_t rates[] = {6, 9, 12, 18, 24, 36, 48, 54};
        for (auto rate : rates) if (rate >= mbps) return rate;
        return 54;
    }

    static uint64_t nextEpoch(uint64_t &serial)
    {
        if (!++serial) ++serial;
        return serial;
    }

    void prepare(uint64_t dma, uint16_t ch, uint8_t minimum, uint64_t &serial,
                 uint8_t mcsMask = 0)
    {
        const uint8_t rounded = roundedFloor(minimum);
        if (epoch && dmaEpoch == dma && channel == ch && requestedFloor == minimum &&
            htMcsMask == mcsMask) return;
        if (epoch && dmaEpoch == dma && channel != ch) {
            uint8_t preserved = current;
            if (preserved & 0x80) {
                uint8_t maxHT = 0;
                for (uint8_t mcs = 0; mcs < 8; ++mcs)
                    if (mcsMask & (1U << mcs)) maxHT = 0x80 | mcs;
                if (!maxHT || halfMbps(maxHT) < halfMbps(rounded)) {
                    preserved = rounded;
                } else if (!(mcsMask & (1U << (preserved & 7)))) {
                    preserved = halfMbps(preserved) > halfMbps(maxHT) ? maxHT : rounded;
                } else if (halfMbps(preserved) < halfMbps(rounded)) {
                    preserved = rounded;
                }
            } else if (preserved < rounded) {
                preserved = rounded;
            }
            epoch = nextEpoch(serial);
            channel = ch;
            floor = rounded;
            requestedFloor = minimum;
            current = preserved;
            probeToken = 0;
            clean = 0;
            htMcsMask = mcsMask;
            return;
        }
        *this = {};
        epoch = nextEpoch(serial); dmaEpoch = dma; channel = ch;
        floor = current = rounded; requestedFloor = minimum;
        htMcsMask = mcsMask;
    }

    uint8_t select() const
    {
        if (probeToken || clean < 8) return current;
        if (halfMbps(current) < 24) return 12;
        if (halfMbps(current) < 48) return 24;
        for (uint8_t mcs = 0; mcs < 8; ++mcs)
            if ((htMcsMask & (1U << mcs)) && halfMbps(0x80 | mcs) > halfMbps(current))
                return 0x80 | mcs;
        return current;
    }

    void submitted(uint64_t token, uint8_t rate)
    {
        if (halfMbps(rate) > halfMbps(current)) { probeToken = token; clean = 0; }
    }

    void release(uint64_t token, uint64_t sampleEpoch)
    {
        if (sampleEpoch == epoch && token == probeToken) {
            probeToken = 0;
            clean = 0;
        }
    }

    bool sample(uint64_t token, uint64_t sampleEpoch, uint8_t rate,
                bool acknowledged, uint8_t retries, uint64_t &serial)
    {
        if (sampleEpoch != epoch) return false;
        if (token == probeToken) {
            probeToken = 0;
            clean = 0;
            if (acknowledged && !retries && halfMbps(rate) > halfMbps(current)) {
                current = rate;
                epoch = nextEpoch(serial);
            }
        } else if (rate == current) {
            if (acknowledged && !retries) {
                if (clean < 8) ++clean;
            } else {
                clean = 0;
                if (!acknowledged || retries >= 2) {
                    uint8_t fallback = floor;
                    if (current & 0x80) {
                        if (floor < 24 && halfMbps(current) > 48) fallback = 24;
                        for (uint8_t mcs = 0; mcs < 8; ++mcs)
                            if ((htMcsMask & (1U << mcs)) &&
                                halfMbps(0x80 | mcs) < halfMbps(current) &&
                                halfMbps(0x80 | mcs) > halfMbps(fallback)) fallback = 0x80 | mcs;
                    } else {
                        const uint8_t lower = current > 12 ? 12 : 6;
                        if (lower > floor) fallback = lower;
                    }
                    if (halfMbps(fallback) < halfMbps(current)) {
                        current = fallback;
                        probeToken = 0;
                        epoch = nextEpoch(serial);
                    }
                }
            }
        } else return false;
        return true;
    }
};

} // namespace AirportAWDL
#endif
