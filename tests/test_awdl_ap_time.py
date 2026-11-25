"""Exercise infrastructure-time accounting around actual AWDL grant boundaries."""

import unittest

import test_regressions as support


class AwdlAPTime(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_grant_credit_debt_and_cancel_accounting(self):
        self.compile_run(r'''
#include <cassert>
#include "AirportItlwm/AWDL/RadioState.hpp"
using AirportAWDL::RadioState;

static void begin(RadioState &s, uint64_t now, unsigned tu, uint64_t startDelay = 0) {
    s.dwell = {};
    s.dwell.durationTU = tu;
    s.dwell.apCreditUS = s.availableAPCredit(now);
    s.session.addUS = now;
    s.session.startUS = now + startDelay;
}

int main() {
    RadioState s = {};
    // No assumed infrastructure history at initialization.
    assert(!s.availableAPCredit(1000000));
    begin(s, 1000000, 64);
    s.reserveAPTime(1065536, false);
    assert(s.apTime.creditUS == -65536 && s.nextGrantUS == 1131072);
    assert(!s.availableAPCredit(1100000));
    assert(!s.availableAPCredit(1131072));
    assert(s.availableAPCredit(1139072) == 8000);

    // A 64 ms gap used to repay debt is not reusable credit for the next grant.
    begin(s, 1131072, 64);
    s.reserveAPTime(1196608, false);
    assert(s.apTime.creditUS == -65536 && s.nextGrantUS == 1262144);

    // Sparse peer schedules leave actual infrastructure time before their
    // adjacent channel-6 slots. It can cover one long grant, with an 8 ms gap.
    begin(s, 2000000, 56);
    assert(s.dwell.apCreditUS == 65536);
    s.reserveAPTime(2057344, false);
    assert(s.apTime.creditUS == 8192 && s.nextGrantUS == 2065344);
    assert(s.availableAPCredit(2065344) == 16192);
    begin(s, 2065344, 56);
    s.reserveAPTime(2122688, false);
    assert(s.apTime.creditUS == -41152 && s.nextGrantUS == 2163840);
    assert(!s.availableAPCredit(2163840));

    // Bank size is bounded even across long idle periods and normal radio stop.
    assert(s.availableAPCredit(1000000000000ULL) == 65536);
    assert(!s.availableAPCredit(s.apTime.settledUS - 1));

    // An early END may belong to an older session. Preserve the new grant's
    // latest possible end, including the observed START-notification delay.
    RadioState early = {};
    begin(early, 1000000, 64, 4500);
    early.reserveAPTime(1005000, false);
    assert(early.apTime.settledUS == 1070036);
    assert(early.nextGrantUS == 1140072);
    const auto settled = early.apTime.settledUS;
    const auto debt = early.apTime.creditUS;
    early.reserveAPTime(1005000, false);
    assert(early.apTime.settledUS == settled && early.apTime.creditUS == debt);

    // END and later cancellation completion account from the same ADD snapshot.
    RadioState cancel = {};
    cancel.apTime.settledUS = 1000000;
    begin(cancel, 1200000, 64);
    cancel.reserveAPTime(1265536, false);
    assert(!cancel.apTime.creditUS);
    cancel.reserveAPTime(1270000, true);
    assert(cancel.apTime.creditUS == -4464);
    assert(cancel.nextGrantUS == 1278000);
    cancel.reserveAPTime(1270000, true);
    assert(cancel.apTime.creditUS == -4464 && cancel.nextGrantUS == 1278000);

    // Cancellation retains the full requested charge even before natural END.
    RadioState stopped = {};
    begin(stopped, 1000000, 64);
    stopped.reserveAPTime(1001000, true);
    assert(stopped.apTime.creditUS == -65536 && stopped.nextGrantUS == 1066536);

    // Ordinary grants keep 8 ms spacing but consume previously banked credit.
    RadioState shortGrant = {};
    begin(shortGrant, 1000000, 16);
    shortGrant.reserveAPTime(1016384, false);
    assert(shortGrant.nextGrantUS == 1024384 && !shortGrant.apTime.creditUS);
    begin(shortGrant, 1200000, 16);
    shortGrant.reserveAPTime(1216384, false);
    assert(shortGrant.apTime.creditUS == 49152 && shortGrant.nextGrantUS == 1224384);
}
''')


if __name__ == "__main__":
    unittest.main()
