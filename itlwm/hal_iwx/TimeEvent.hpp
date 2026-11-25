// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ITL_IWX_TIME_EVENT_HPP
#define ITL_IWX_TIME_EVENT_HPP
#include <stdint.h>

// Command replies and notifications are consumed in RX order on the HAL gate.
struct IwxTimeEventState {
    uint64_t serial;
    uint32_t mac, type, uid;
    bool pending, removing, responseSeen, responseOK, uidValid, active, running;

    void reset()
    {
        ++serial;
        mac = type = uid = 0;
        pending = removing = responseSeen = responseOK = uidValid = active = running = false;
    }

    bool begin(uint32_t owner, uint32_t eventType, bool remove)
    {
        if (pending || (remove ? (!active || !uidValid || owner != mac || eventType != uid) : active))
            return false;
        ++serial;
        pending = true;
        removing = remove;
        responseSeen = responseOK = false;
        if (!remove) {
            mac = owner;
            type = eventType;
            uid = 0;
            uidValid = running = false;
        }
        return true;
    }

    bool response(uint32_t status, uint32_t eventType, uint32_t eventUID, uint32_t owner)
    {
        if (!pending || responseSeen || owner != mac || eventType != type ||
            (removing && eventUID != uid))
            return false;
        responseSeen = true;
        // TIME_EVENT_CMD uses bit 0 for success and the other bits for errors.
        responseOK = status == 1;
        if (!responseOK) return true;
        uid = eventUID;
        uidValid = active = !removing;
        running = false;
        return true;
    }

    bool notification(uint32_t owner, uint32_t eventUID, uint32_t action, uint32_t status)
    {
        if (!uidValid || owner != mac || eventUID != uid)
            return false;
        // An END may arrive before the command sender resumes, or contain START too.
        if (!status || (action & 2)) {
            active = running = false;
        } else if (active && (action & 1)) {
            running = true;
        }
        return true;
    }
};
#endif
