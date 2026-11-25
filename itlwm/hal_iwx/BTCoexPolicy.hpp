// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
// Intel two-chain coexistence policy; firmware TLC retains TX rate control.
// Policy follows Intel Linux v5.17 mvm/coex.c, mvm/phy-ctxt.c,
// net/mac80211/{ht.c,status.c}. No legacy LQ command is used with TLC offload.
#ifndef IWX_BT_COEX_POLICY_HPP
#define IWX_BT_COEX_POLICY_HPP
#include <stdint.h>
#include <string.h>

namespace IwxBTPolicy {
enum class SMPS : uint8_t { Off = 0, Static = 1, Dynamic = 3 };
enum class LUT : uint8_t { Tight = 0, Loose = 1, TxDis = 2, Invalid = 0xff };
struct Chains {
    uint8_t idle, active;
    bool operator==(const Chains &other) const {
        return idle == other.idle && active == other.active;
    }
    bool operator!=(const Chains &other) const { return !(*this == other); }
};
struct Profile {
    uint8_t activity = 0, rrc = 0;
    bool schema2 = true;
    LUT lut = LUT::Invalid;
};

inline SMPS requestedSMPS(const Profile &profile, bool associated, bool band2,
    uint8_t phy)
{
    if (!associated || !band2 || phy >= 8 || (profile.rrc & (1U << phy)))
        return SMPS::Off;
    if (profile.activity >= (profile.schema2 ? 4 : 3)) return SMPS::Static;
    if (profile.activity >= 2) return SMPS::Dynamic;
    return SMPS::Off;
}

inline bool reducedPower(const Profile &profile, bool associated, bool band2,
    bool singleSharedAntenna, int beaconDBm, bool previous)
{
    if (!associated || !band2 || singleSharedAntenna || !profile.activity ||
        profile.lut == LUT::Loose) return false;
    // Intel uses -100 dBm when a beacon average is not available.
    if (!beaconDBm) beaconDBm = -100;
    if (beaconDBm > -62) return true;
    if (beaconDBm < -65) return false;
    return previous;
}

// HT category action is not a robust management frame. It stays unprotected
// even when PMF is negotiated; ordinary mgmt_output currently protects all
// actions and cannot construct this frame unchanged.
inline void buildAction(uint8_t (&frame)[27], const uint8_t (&ap)[6],
    const uint8_t (&local)[6], const uint8_t (&bssid)[6], uint16_t sequence, SMPS mode)
{
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xd0;
    memcpy(frame + 4, ap, 6);
    memcpy(frame + 10, local, 6);
    memcpy(frame + 16, bssid, 6);
    sequence = uint16_t((sequence & 0xfff) << 4);
    frame[22] = uint8_t(sequence);
    frame[23] = uint8_t(sequence >> 8);
    frame[24] = 7; // WLAN_CATEGORY_HT
    frame[25] = 1; // WLAN_HT_ACTION_SMPS
    frame[26] = uint8_t(mode);
}

struct State {
    uint64_t serial = 0, linkEpoch = 0;
    uint64_t actionToken = 0, phyToken = 0, powerToken = 0;
    uint64_t actionAcks = 0, actionFailures = 0, phyUpdates = 0, powerUpdates = 0;
    uint64_t phyFailures = 0, powerFailures = 0;
    bool associated = false, ht = false;
    uint8_t neededRx = 1;
    bool actionBlocked = false, phyValid = false, phyBlocked = false;
    bool powerValid = false, powerBlocked = false, powerDesired = false;
    bool powerApplied = false, powerSent = false, powerFailed = false;
    SMPS desired = SMPS::Off, acknowledged = SMPS::Off, actionSent = SMPS::Off;
    SMPS actionFailed = SMPS::Off;
    Chains baseline{2, 2}, phyApplied{2, 2}, phySent{2, 2}, phyFailed{2, 2};

    void resetLink(bool active, bool htAssociation, uint8_t receiveChains,
        Chains original = {2, 2})
    {
        const bool knownChains = phyValid && !phyToken && phyApplied == original;
        ++linkEpoch;
        associated = active;
        ht = htAssociation;
        neededRx = receiveChains;
        actionToken = phyToken = powerToken = 0;
        actionBlocked = phyBlocked = powerBlocked = false;
        desired = acknowledged = actionSent = SMPS::Off;
        baseline = phyApplied = original;
        // Only a completed PHY/RLC command establishes the baseline. A link
        // transition that retained the old station may still need restoration.
        phyValid = active && knownChains;
        // RUN -> ASSOC may reuse the firmware station. Synchronize its bit
        // again instead of assuming station modification cleared it.
        powerDesired = powerApplied = powerSent = false;
        powerValid = false;
    }

    void request(SMPS mode, bool reducePower)
    {
        desired = mode;
        powerDesired = reducePower;
        if (desired != actionFailed) actionBlocked = false;
        if (powerDesired != powerFailed) powerBlocked = false;
    }

    uint64_t beginAction(SMPS &mode)
    {
        if (!associated || !ht || baseline.active < 2 || actionToken ||
            desired == acknowledged || (actionBlocked && actionFailed == desired))
            return 0;
        mode = actionSent = desired;
        return actionToken = ++serial;
    }

    bool completeAction(uint64_t token, bool ack)
    {
        if (!token || actionToken != token) return false;
        actionToken = 0;
        if (ack) {
            ++actionAcks;
            acknowledged = actionSent;
            actionBlocked = false;
        } else {
            ++actionFailures;
            actionBlocked = true;
            actionFailed = actionSent;
        }
        return true;
    }

    Chains wantedChains() const
    {
        // Requested SMPS alone must not shrink receive chains before AP ACK.
        if (associated && ht && acknowledged == SMPS::Static) return {1, 1};
        if (associated && ht && acknowledged == SMPS::Dynamic) return {1, neededRx};
        return baseline;
    }

    bool needsPHY() {
        const Chains chains = wantedChains();
        if (chains != phyFailed) phyBlocked = false;
        return associated && !(phyValid && phyApplied == chains) &&
            !(phyBlocked && phyFailed == chains);
    }

    // All infrastructure PHY commands use this serial, including bandwidth
    // updates which can enter while a synchronous command has released the gate.
    uint64_t beginPHY(Chains chains) {
        phySent = chains;
        phyValid = false;
        return phyToken = ++serial;
    }

    bool completePHY(uint64_t token, bool success, bool removed = false)
    {
        if (!token || phyToken != token) return false;
        phyToken = 0;
        phyBlocked = !success;
        if (success && removed) {
            phyValid = false;
            return true;
        }
        if (success) {
            ++phyUpdates;
            phyApplied = phySent;
            phyValid = true;
        } else {
            ++phyFailures;
            phyFailed = phySent;
        }
        return true;
    }

    uint64_t beginPower(bool &reduce)
    {
        if (!associated || powerToken ||
            (powerValid && powerApplied == powerDesired) ||
            (powerBlocked && powerFailed == powerDesired)) return 0;
        reduce = powerSent = powerDesired;
        powerValid = false;
        return powerToken = ++serial;
    }

    bool completePower(uint64_t token, bool success)
    {
        if (!token || powerToken != token) return false;
        powerToken = 0;
        powerBlocked = !success;
        if (success) {
            ++powerUpdates;
            powerApplied = powerSent;
            powerValid = true;
        } else {
            ++powerFailures;
            powerFailed = powerSent;
        }
        return true;
    }
};
}
#endif
