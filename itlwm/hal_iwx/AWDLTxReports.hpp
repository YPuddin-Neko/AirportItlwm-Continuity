// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef IWX_AWDL_TX_REPORTS_HPP
#define IWX_AWDL_TX_REPORTS_HPP

// Gen2 keeps the submitted MAC header in the command ring until reclaim.
struct IwxAWDLBroadcastReports {
    uint64_t submitted, single, withAirtime, success, directDone;
    uint64_t retries, btKills, implicit, flushed, unmatched;
    uint32_t lastRate;
    uint8_t lastStatus;
    struct {
        uint64_t reports, success, withMediaTime, mediaTimeUS;
    } queueAssociated;
    struct {
        uint64_t length, station, frameControl, byteCount, sequence;
        struct {
            size_t payload;
            uint32_t rate;
            uint16_t fc, bytes, expectedBytes, seq, expectedSeq, airtime, rawStatus;
            uint16_t commandSeq, heldCommandSeq;
            uint8_t raTid;
        } last;
    } mismatch;

    static const iwx_tx_cmd_gen2 *broadcast(const iwx_tx_ring &ring, unsigned slot) {
        const auto &data = ring.data[slot];
        if (!data.m || !(data.flags & IWX_TXDATA_FLAG_AWDL) || !ring.cmd)
            return nullptr;
        const auto &command = ring.cmd[slot];
        if (command.hdr.cmd != IWX_TX_CMD || command.hdr.qid != ring.qid || command.hdr.idx != slot)
            return nullptr;
        const auto *tx = reinterpret_cast<const iwx_tx_cmd_gen2 *>(command.data);
        const auto *header = reinterpret_cast<const ieee80211_frame *>(tx + 1);
        return le16toh(tx->len) >= sizeof(*header) && header->i_fc[0] == IEEE80211_FC0_SUBTYPE_ACTION &&
            IEEE80211_IS_MULTICAST(header->i_addr1) ? tx : nullptr;
    }

    void response(const iwx_tx_ring &ring, unsigned next, int ownedQueue,
                  const iwx_tx_resp &report, size_t length) {
        constexpr size_t singleLength = sizeof(report) + sizeof(uint32_t);
        if (report.frame_count != 1 || length < singleLength ||
            ring.qid != ownedQueue || le16toh(report.tx_queue) != ownedQueue) return;
        const bool badLength = length != singleLength;
        const bool badStation = (report.ra_tid >> 4) != AirportAWDL::RadioState::StationID;
        const auto *tx = broadcast(ring, ring.tail);
        if (tx) {
            const uint16_t rawStatus = le16toh(report.status.status);
            const uint8_t status = rawStatus & IWX_TX_STATUS_MSK;
            const auto *header = reinterpret_cast<const ieee80211_frame *>(tx + 1);
            const uint16_t fc = le16toh(report.frame_ctrl);
            const uint16_t bytes = le16toh(report.byte_cnt), expectedBytes = le16toh(tx->len);
            const uint16_t seq = le16toh(report.seq_ctl), expectedSeq = AirportAWDL::le16(header->i_seq);
            const bool badFC = (fc & 0xff) != header->i_fc[0];
            const bool badBytes = bytes != expectedBytes;
            // iwx_rx_tx_cmd validated the scheduler SSN against the owned queue.
            // The first reclaimed descriptor carries the individual TX status.
            // Firmware can manage the non-QoS sequence independently of the host.
            if (!badLength && !badStation && (report.ra_tid & 0xf) == IWX_MGMT_TID &&
                fc == AirportAWDL::le16(header->i_fc) && !badBytes &&
                !memcmp(header->i_addr1, "\xff\xff\xff\xff\xff\xff", 6)) {
                const uint16_t mediaTime = le16toh(report.wireless_media_time);
                ++queueAssociated.reports;
                queueAssociated.success += status == IWX_TX_STATUS_SUCCESS;
                queueAssociated.withMediaTime += mediaTime != 0;
                queueAssociated.mediaTimeUS += mediaTime;
            }
            // A different Wi-Fi sequence on a success-class response stays
            // unmatched even if the status includes burst or queue bits.
            const bool badSeq = status == IWX_TX_STATUS_SUCCESS && seq != expectedSeq;
            if (badLength || badStation || badFC || badBytes || badSeq) {
                ++unmatched;
                mismatch.length += badLength;
                mismatch.station += badStation;
                mismatch.frameControl += badFC;
                mismatch.byteCount += badBytes;
                mismatch.sequence += badSeq;
                const auto &held = ring.cmd[ring.tail].hdr;
                mismatch.last = {length, le32toh(report.initial_rate), fc, bytes, expectedBytes,
                    seq, expectedSeq, le16toh(report.wireless_media_time), rawStatus,
                    le16toh(report.status.sequence), uint16_t(held.qid << 8 | held.idx), report.ra_tid};
            } else {
                ++single;
                withAirtime += report.wireless_media_time != 0;
                success += status == IWX_TX_STATUS_SUCCESS;
                directDone += status == IWX_TX_STATUS_DIRECT_DONE;
                retries += report.failure_frame;
                btKills += report.bt_kill_count;
                lastStatus = status;
                lastRate = le32toh(report.initial_rate);
            }
        }
        // Only the first reclaimed slot has an individual firmware report.
        if (!badLength && !badStation)
            for (unsigned at = (ring.tail + 1) % ring.ring_count; at != next; at = (at + 1) % ring.ring_count)
                implicit += broadcast(ring, at) != nullptr;
    }

    void flush(const iwx_tx_ring &ring, unsigned first, unsigned end) {
        // The firmware consumed the prefix before the explicit flush range.
        for (unsigned offset = 0; offset < end; ++offset) {
            if (broadcast(ring, (ring.tail + offset) % ring.ring_count)) {
                if (offset < first) ++implicit;
                else ++flushed;
            }
        }
    }

    template <class RegistryEntry>
    void publish(RegistryEntry &entry) const {
        entry.setProperty("BroadcastAFSubmitted", submitted, 64);
        entry.setProperty("BroadcastAFSingleResponses", single, 64);
        entry.setProperty("BroadcastAFWithAirtime", withAirtime, 64);
        entry.setProperty("BroadcastAFSuccess", success, 64);
        entry.setProperty("BroadcastAFDirectDone", directDone, 64);
        entry.setProperty("BroadcastAFFrameRetries", retries, 64);
        entry.setProperty("BroadcastAFBluetoothKills", btKills, 64);
        entry.setProperty("BroadcastAFImplicitReclaims", implicit, 64);
        entry.setProperty("BroadcastAFFlushDiscards", flushed, 64);
        entry.setProperty("BroadcastAFUnmatchedResponses", unmatched, 64);
        entry.setProperty("BroadcastAFQueueAssociatedReports", queueAssociated.reports, 64);
        entry.setProperty("BroadcastAFQueueAssociatedSuccess", queueAssociated.success, 64);
        entry.setProperty("BroadcastAFQueueAssociatedWithMediaTime", queueAssociated.withMediaTime, 64);
        entry.setProperty("BroadcastAFQueueAssociatedMediaTimeUS", queueAssociated.mediaTimeUS, 64);
        entry.setProperty("BroadcastAFLastStatus", uint64_t(lastStatus), 8);
        entry.setProperty("BroadcastAFLastRate", uint64_t(lastRate), 32);
        if (unmatched) {
            entry.setProperty("BroadcastAFMismatchLength", mismatch.length, 64);
            entry.setProperty("BroadcastAFMismatchStation", mismatch.station, 64);
            entry.setProperty("BroadcastAFMismatchFC", mismatch.frameControl, 64);
            entry.setProperty("BroadcastAFMismatchBytes", mismatch.byteCount, 64);
            entry.setProperty("BroadcastAFMismatchSeq", mismatch.sequence, 64);
            const auto &sample = mismatch.last;
            char fields[192];
            snprintf(fields, sizeof(fields),
                "payload=%zu ra_tid=0x%02x fc=0x%04x bytes=%u/%u seq=0x%04x/0x%04x "
                "cmd=0x%04x/0x%04x status=0x%04x rate=0x%x airtime_us=%u",
                sample.payload, sample.raTid, sample.fc, sample.bytes, sample.expectedBytes,
                sample.seq, sample.expectedSeq, sample.commandSeq, sample.heldCommandSeq,
                sample.rawStatus, sample.rate, sample.airtime);
            entry.setProperty("BroadcastAFLastUnmatchedReport", fields);
        }
    }
};

#endif
