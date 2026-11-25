// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_AWDL_RADIO_TRAITS_HPP
#define AIRPORT_AWDL_RADIO_TRAITS_HPP

#include <stddef.h>
#include <stdint.h>

namespace AirportAWDL {
namespace Detail {
template <typename R>
inline auto clearRealtime(R &r, int) -> decltype(r.clearRealtimeState(), void()) {
    r.clearRealtimeState();
}
template <typename R>
inline void clearRealtime(R &, long) {}

template <typename R>
inline auto updateRealtime(R &r, const void *raw, size_t cap, int)
    -> decltype(r.updateRealtimeRegistration(raw, cap)) {
    return r.updateRealtimeRegistration(raw, cap);
}
template <typename R>
inline bool updateRealtime(R &, const void *, size_t, long) { return false; }

template <typename R>
inline auto setLLWPeer(R &r, const uint8_t *peer, bool active, int)
    -> decltype(r.setLLWRealtimePeer(peer, active), void()) {
    r.setLLWRealtimePeer(peer, active);
}
template <typename R>
inline void setLLWPeer(R &, const uint8_t *, bool, long) {}

template <typename R>
inline auto hasRealtimePeer(const R &r, const uint8_t *addr, int)
    -> decltype(r.hasRealtimePeer(addr)) {
    return r.hasRealtimePeer(addr);
}
template <typename R>
inline bool hasRealtimePeer(const R &, const uint8_t *, long) { return false; }

template <typename R>
inline auto realtimeMask(const R &r, int) -> decltype(r.realtimeSessionMask) {
    return r.realtimeSessionMask;
}
template <typename R>
inline uint32_t realtimeMask(const R &, long) { return 0U; }

template <typename R>
inline auto idleUnboundFlag(const R &r, int) -> decltype(r.idleUnbound) {
    return r.idleUnbound;
}
template <typename R>
inline bool idleUnboundFlag(const R &, long) { return false; }

template <typename R>
inline auto idleUnbindsCount(const R &r, int) -> decltype(r.idleUnbinds) {
    return r.idleUnbinds;
}
template <typename R>
inline uint64_t idleUnbindsCount(const R &, long) { return 0ULL; }

template <typename R>
inline auto idleRebindsCount(const R &r, int) -> decltype(r.idleRebinds) {
    return r.idleRebinds;
}
template <typename R>
inline uint64_t idleRebindsCount(const R &, long) { return 0ULL; }

template <typename R, typename P>
inline auto evalIdleUnbind(R &r, uint64_t nowUS, uint16_t winChan, bool baseIdle,
                           P &policy, uint16_t &svcChan, int)
    -> decltype(r.evaluateIdleUnbind(nowUS, winChan, baseIdle, policy, svcChan)) {
    return r.evaluateIdleUnbind(nowUS, winChan, baseIdle, policy, svcChan);
}
template <typename R, typename P>
inline bool evalIdleUnbind(R &, uint64_t, uint16_t, bool, P &, uint16_t &, long) {
    return false;
}

template <typename R>
inline auto setTxFlushed(R &r, void (*fn)(void *, uint64_t), int)
    -> decltype(r.awdlTxFlushed = fn, void()) {
    r.awdlTxFlushed = fn;
}
template <typename R>
inline void setTxFlushed(R &, void (*)(void *, uint64_t), long) {}

template <typename R>
inline auto notifyTxFlushed(const R &r, void *receiver, uint64_t token, int)
    -> decltype(r.awdlTxFlushed ? (r.awdlTxFlushed(receiver, token), true) : false) {
    if (!r.awdlTxFlushed) return false;
    r.awdlTxFlushed(receiver, token);
    return true;
}
template <typename R>
inline bool notifyTxFlushed(const R &, void *, uint64_t, long) {
    return false;
}
}

template <typename R>
inline void clearRealtimeSessionsOn(R &r) { Detail::clearRealtime(r, 0); }
template <typename R>
inline bool updateRealtimeRegistrationOn(R &r, const void *raw, size_t cap) {
    return Detail::updateRealtime(r, raw, cap, 0);
}
template <typename R>
inline void setLLWRealtimePeerOn(R &r, const uint8_t *peer, bool active) {
    Detail::setLLWPeer(r, peer, active, 0);
}
template <typename R>
inline bool hasRealtimePeerOn(const R &r, const uint8_t *addr) {
    return Detail::hasRealtimePeer(r, addr, 0);
}
template <typename R>
inline uint32_t realtimeSessionMaskOf(const R &r) { return Detail::realtimeMask(r, 0); }
template <typename R>
inline bool idleUnboundOf(const R &r) { return Detail::idleUnboundFlag(r, 0); }
template <typename R>
inline uint64_t idleUnbindsOf(const R &r) { return Detail::idleUnbindsCount(r, 0); }
template <typename R>
inline uint64_t idleRebindsOf(const R &r) { return Detail::idleRebindsCount(r, 0); }
template <typename R, typename P>
inline bool evaluateIdleUnbindOn(R &r, uint64_t nowUS, uint16_t winChan, bool baseIdle,
                                 P &policy, uint16_t &svcChan) {
    return Detail::evalIdleUnbind(r, nowUS, winChan, baseIdle, policy, svcChan, 0);
}
template <typename R>
inline void setTxFlushedCallbackOn(R &r, void (*fn)(void *, uint64_t)) {
    Detail::setTxFlushed(r, fn, 0);
}
template <typename R>
inline bool notifyTxFlushedOn(const R &r, void *receiver, uint64_t token) {
    return Detail::notifyTxFlushed(r, receiver, token, 0);
}
}

#endif
