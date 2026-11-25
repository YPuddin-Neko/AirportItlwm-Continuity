// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef IWX_AWDL_TX_BUDGET_HPP
#define IWX_AWDL_TX_BUDGET_HPP
#include "../../AirportItlwm/AWDL/TxRate.hpp"

struct IwxAWDLTxBudget {
    static uint32_t ofdmUS(unsigned bytes, unsigned rate500Kbps, bool twoGHz) {
        // Legacy OFDM SERVICE, MPDU/FCS and tail, rounded to whole symbols.
        // As in ieee80211_mira_legacy_txtime; ACK/IFS/contention are excluded.
        const unsigned symbols = (8 + 4 * (bytes + 4) + 3 + rate500Kbps - 1) / rate500Kbps;
        return 20 + 4 * symbols + (twoGHz ? 6 : 0);
    }

    static uint8_t rateCode(uint32_t flags, bool v2) {
        static const uint8_t rates[] = {12,18,24,36,48,72,96,108};
        if (v2) {
            if ((flags & IWX_RATE_MCS_MOD_TYPE_MSK) == IWX_RATE_MCS_HT_MSK) {
                if (flags & (IWX_RATE_MCS_CHAN_WIDTH_MSK | IWX_RATE_MCS_SGI_MSK |
                    IWX_RATE_MCS_NSS_MSK | IWX_RATE_MCS_STBC_MSK | 0x08)) return 0;
                return 0x80 | (flags & 7);
            }
            if ((flags & IWX_RATE_MCS_MOD_TYPE_MSK) != IWX_RATE_MCS_LEGACY_OFDM_MSK) return 0;
            return rates[flags & IWX_RATE_LEGACY_RATE_MSK] / 2;
        }
        if (flags & IWX_RATE_MCS_HT_MSK_V1) {
            if (flags & (IWX_RATE_MCS_VHT_MSK_V1 | IWX_RATE_MCS_HE_MSK_V1 |
                IWX_RATE_MCS_CCK_MSK_V1 | IWX_RATE_MCS_CHAN_WIDTH_MSK_V1 |
                IWX_RATE_MCS_SGI_MSK_V1 | IWX_RATE_HT_MCS_GF_MSK |
                IWX_RATE_MCS_STBC_MSK | 0xf8)) return 0;
            return 0x80 | (flags & 7);
        }
        if (flags & (IWX_RATE_MCS_HT_MSK_V1 | IWX_RATE_MCS_VHT_MSK_V1 |
                     IWX_RATE_MCS_HE_MSK_V1 | IWX_RATE_MCS_CCK_MSK_V1)) return 0;
        static const uint8_t plcp[] = {IWX_RATE_6M_PLCP,IWX_RATE_9M_PLCP,IWX_RATE_12M_PLCP,
            IWX_RATE_18M_PLCP,IWX_RATE_24M_PLCP,IWX_RATE_36M_PLCP,IWX_RATE_48M_PLCP,IWX_RATE_54M_PLCP};
        for (unsigned i = 0; i < sizeof(rates); ++i)
            if ((flags & IWX_RATE_LEGACY_RATE_MSK_V1) == plcp[i]) return rates[i] / 2;
        return 0;
    }

    static unsigned rate500Kbps(uint32_t flags, bool v2) {
        return AirportAWDL::TxRateControl::halfMbps(rateCode(flags, v2));
    }

    static uint32_t frameUS(unsigned bytes, uint32_t flags, bool v2, bool twoGHz) {
        const uint8_t code = rateCode(flags, v2);
        const unsigned rate = AirportAWDL::TxRateControl::halfMbps(code);
        if (!rate) return UINT32_MAX;
        if (!(code & 0x80)) return ofdmUS(bytes, rate, twoGHz);
        // HT mixed format, one spatial stream, 20 MHz and long guard interval.
        const unsigned symbols = (16 + 8 * (bytes + 4) + 6 + rate * 2 - 1) / (rate * 2);
        return 36 + 4 * symbols + (twoGHz ? 6 : 0);
    }

    static uint64_t queuedUS(const iwx_tx_ring &ring, bool gen3, bool v2, bool twoGHz) {
        uint64_t total = 0;
        // Unreclaimed descriptors may already have transmitted. Charging their
        // full cost is conservative until the normal completion clears m.
        for (unsigned i = 0; i < ring.ring_count; ++i) {
            if (!ring.data[i].m) continue;
            if (!(ring.data[i].flags & IWX_TXDATA_FLAG_AWDL)) return UINT64_MAX;
            unsigned bytes;
            uint32_t flags;
            if (gen3) {
                const auto tx = reinterpret_cast<const iwx_tx_cmd_gen3 *>(ring.cmd[i].data);
                bytes = le16toh(tx->len); flags = le32toh(tx->rate_n_flags);
            } else {
                const auto tx = reinterpret_cast<const iwx_tx_cmd_gen2 *>(ring.cmd[i].data);
                bytes = le16toh(tx->len); flags = le32toh(tx->rate_n_flags);
            }
            const uint32_t time = frameUS(bytes, flags, v2, twoGHz);
            if (time == UINT32_MAX) return UINT64_MAX;
            total += time;
        }
        return total;
    }
};
#endif
