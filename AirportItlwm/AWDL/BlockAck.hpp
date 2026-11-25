// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_BLOCK_ACK_HPP
#define AIRPORT_AWDL_BLOCK_ACK_HPP

#include "DataFrame.hpp"

namespace AirportAWDL {

struct BlockAckRequest {
    uint8_t peer[6], dialog, tid;
    uint16_t ssn, window, timeout;
    bool immediate, amsdu, teardown;
};

inline bool sameBlockAck(const BlockAckRequest &a, const BlockAckRequest &b)
{
    return sameAddress(a.peer, b.peer) && a.dialog == b.dialog && a.tid == b.tid &&
        a.ssn == b.ssn && a.window == b.window && a.timeout == b.timeout &&
        a.immediate == b.immediate && a.amsdu == b.amsdu && a.teardown == b.teardown;
}

inline bool blockAckRequest(const uint8_t *frame, size_t size, const uint8_t *self,
                            BlockAckRequest &out)
{
    if (size < 24 || frame[0] != 0xd0 || (frame[1] & 0x47) ||
        !sameAddress(frame + 4, self) || !unicastAddress(frame + 10) ||
        sameAddress(frame + 10, self) || !sameAddress(frame + 16, BSSID) ||
        (le16(frame + 22) & 15)) return false;
    const size_t header = (frame[1] & 0x80) ? 28 : 24;
    if (size < header + 9 || frame[header] != 3 || frame[header + 1] != 0 ||
        (le16(frame + header + 7) & 15)) return false;
    copyAddress(out.peer, frame + 10);
    out.dialog = frame[header + 2];
    const uint16_t parameters = le16(frame + header + 3);
    out.tid = uint8_t((parameters >> 2) & 15);
    out.window = parameters >> 6;
    out.timeout = le16(frame + header + 5);
    out.ssn = le16(frame + header + 7) >> 4;
    out.immediate = parameters & 2;
    out.amsdu = parameters & 1;
    out.teardown = false;
    return true;
}

inline bool blockAckEnd(const uint8_t *frame, size_t size, const uint8_t *self,
                        BlockAckRequest &out)
{
    if (size < 24 || frame[0] != 0xd0 || (frame[1] & 0x47) ||
        !sameAddress(frame + 4, self) || !unicastAddress(frame + 10) ||
        sameAddress(frame + 10, self) ||
        (le16(frame + 22) & 15)) return false;
    const size_t header = (frame[1] & 0x80) ? 28 : 24;
    if (size < header + 6 || frame[header] != 3 || frame[header + 1] != 2)
        return false;
    const uint16_t parameters = le16(frame + header + 2);
    // An initiator's DELBA closes our receive agreement.
    if (!(parameters & 0x800) || (parameters & 0x7ff)) return false;
    out = {};
    copyAddress(out.peer, frame + 10);
    out.tid = parameters >> 12;
    out.teardown = true;
    return true;
}

inline uint16_t blockAckWindow(const BlockAckRequest &request)
{
    const uint16_t limit = !request.window || request.window > 64 ? 64 : request.window;
    // IWX indexes by sequence modulo window; the window must divide the 12-bit sequence space.
    uint16_t window = 1;
    while (window * 2 <= limit) window *= 2;
    return window;
}

inline void blockAckResponse(uint8_t (&out)[33], const uint8_t *self,
                            const BlockAckRequest &request, uint16_t sequence, bool accepted)
{
    for (auto &byte : out) byte = 0;
    out[0] = 0xd0;
    copyAddress(out + 4, request.peer);
    copyAddress(out + 10, self);
    copyAddress(out + 16, BSSID);
    putLE16(out + 22, uint16_t(sequence << 4));
    out[24] = 3; out[25] = 1; out[26] = request.dialog;
    putLE16(out + 27, accepted ? 0 : 37);
    putLE16(out + 29, uint16_t(request.tid << 2) |
        (accepted ? uint16_t((blockAckWindow(request) << 6) | 2 | (request.amsdu ? 1 : 0)) : 0));
    // Zero timeout keeps the agreement until DELBA, peer expiry or radio teardown.
}

inline void blockAckDecline(uint8_t (&out)[33], const uint8_t *self,
                            const BlockAckRequest &request, uint16_t sequence)
{
    blockAckResponse(out, self, request, sequence, false);
}

inline void blockAckTeardown(uint8_t (&out)[30], const uint8_t *self,
                            const BlockAckRequest &request, uint16_t sequence)
{
    for (auto &byte : out) byte = 0;
    out[0] = 0xd0;
    copyAddress(out + 4, request.peer);
    copyAddress(out + 10, self);
    copyAddress(out + 16, BSSID);
    putLE16(out + 22, uint16_t(sequence << 4));
    out[24] = 3; out[25] = 2;
    putLE16(out + 26, uint16_t(request.tid << 12));
    putLE16(out + 28, 39);
}

struct BlockAckReply {
    enum State : uint8_t { Free, Queued, Submitted } state;
    BlockAckRequest request;
    uint64_t queuedUS, epoch, token, cookie;
    bool prepared, accepted;
    uint8_t attempts;
    static constexpr uint64_t LifetimeUS = 1000000;
};

struct BlockAckState {
    enum { FirstStation = 4, PeerCount = 4, TIDCount = 8, MaxSessions = 16 };
    struct Session {
        uint8_t baidPlusOne;
        BlockAckRequest request;
        uint64_t cookie, endDeadlineUS;
        bool firmware, endQueued;
    };
    struct Peer {
        bool station;
        uint8_t address[6], removeTids;
        uint64_t epoch;
        Session sessions[TIDCount];
    } peers[PeerCount];
    bool preparing;
    uint64_t llwGapExpiries, llwBelowWindow;
    uint64_t nextCookie, accepted, declined, removed, reordered, rejected, teardownSent, barFrames;
};

}
#endif
