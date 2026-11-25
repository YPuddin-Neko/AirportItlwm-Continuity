"""Exercise ESS beacon evidence, regulatory lifetime and AWDL eligibility."""
import re
import unittest

import test_regressions as support
from test_offline import definition


class BeaconHintRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_received_channel_ess_width_and_notification(self):
        raw = (support.ROOT / 'itl80211/openbsd/net80211/ieee80211.h').read_text()
        declaration = re.search(r'struct ieee80211_ie_vht_operation \{.*?\} __packed;', raw, re.S)[0]
        source = (support.ROOT / 'tests/fixtures/awdl_beacon_hints.cpp').read_text()
        source = source.replace('// VHT DECLARATION', declaration)
        routines = definition('itl80211/openbsd/net80211/ieee80211_input.c', 'ieee80211_note_beacon_hint')
        routines += '\n' + definition('itlwm/hal_iwx/ItlIwxAWDL.cpp', 'ItlIwx::awdlChannelAllowed')
        self.compile_run(source.replace('// PRODUCTION', routines))


if __name__ == '__main__':
    unittest.main()
