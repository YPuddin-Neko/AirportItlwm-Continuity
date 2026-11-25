"""Exercise production AP teardown while AWDL finishes cancelling asynchronously."""

import re
import unittest

import test_regressions as support
from test_offline import definition


class AWDLDeauthRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def fixture(self, entry):
        source = (support.ROOT / "tests/fixtures/awdl_deauth.cpp").read_text()
        constants = []
        for path, names in (
            ("itlwm/hal_iwx/if_iwxvar.h", ("IWX_FLAG_SHUTDOWN", "IWX_FLAG_SCANNING",
                "IWX_FLAG_STA_ACTIVE", "IWX_FLAG_BINDING_ACTIVE", "IWX_FLAG_MAC_ACTIVE")),
            ("itl80211/openbsd/net80211/ieee80211_node.h", ("IEEE80211_NODE_HT",
                "IEEE80211_NODE_QOS", "IEEE80211_NODE_HT_SGI20", "IEEE80211_NODE_HT_SGI40",
                "IEEE80211_NODE_VHT", "IEEE80211_NODE_VHT_SGI80", "IEEE80211_NODE_VHT_SGI160",
                "IEEE80211_NODE_HE")),
        ):
            header = (support.ROOT / path).read_text()
            constants.extend(re.search(r"^#define\s+" + name + r"\s+[^\n]*", header, re.M).group()
                             for name in names)
        source = source.replace("// PRODUCTION CONSTANTS", "\n".join(constants))
        source = source.replace("// PRODUCTION FUNCTIONS", "\n".join(
            definition("itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::" + name)
            for name in ("iwx_deauth", "iwx_newstate_task")))
        self.compile_run(source.replace("// TEST ENTRY", entry))

    def test_pending_awdl_stop_preserves_ap_cleanup_without_reset(self):
        self.fixture("pendingAPCleanup();")

    def test_real_errors_propagate_and_preserve_reset_policy(self):
        self.fixture("realErrors();")

    def test_generation_change_does_not_restart_stale_transition(self):
        self.fixture("resetDuringCommands();")


if __name__ == "__main__":
    unittest.main()
