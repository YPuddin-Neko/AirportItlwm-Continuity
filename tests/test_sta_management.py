"""Exercise STA management admission through the production net80211 input path."""

import unittest

import test_regressions as support
from test_offline import definition


class STAManagementRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def fixture(self, entry):
        source = (support.ROOT / "tests/fixtures/sta_management.cpp").read_text()
        path = "itl80211/openbsd/net80211/ieee80211_input.c"
        routines = "\n".join(definition(path, name) for name in (
            "ieee80211_sta_foreign_disconnect", "ieee80211_input_mgmt_decrypt", "ieee80211_inputm",
            "ieee80211_recv_deauth", "ieee80211_recv_disassoc"))
        source = source.replace("// PRODUCTION INPUT", routines)
        self.compile_run(source.replace("// TEST ENTRY", entry))

    def test_foreign_disconnect_does_not_change_sta_or_ap_receive_state(self):
        self.fixture("foreignDisconnects();")

    def test_current_ap_can_disconnect_local_station_or_broadcast(self):
        self.fixture("currentAPDisconnects();")

    def test_scan_beacons_probe_responses_and_monitor_are_unchanged(self):
        self.fixture("scanAndMonitor();")

    def test_pmf_still_requires_valid_protection_for_current_ap(self):
        self.fixture("managementProtection();")

    def test_short_frames_are_freed_without_management_dispatch(self):
        self.fixture("shortFrames();")


if __name__ == "__main__":
    unittest.main()
