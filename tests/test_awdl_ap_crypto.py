"""Exercise the STA crypto/duplicate boundary while the AWDL MAC is active."""

import re
import unittest

import test_regressions as support
from test_offline import definition
from iwx_firmware import receive_diagnostics
from iwx_firmware import declarations


class AWDLAPCryptoRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def fixture(self, entry):
        source = (support.ROOT / "tests/fixtures/awdl_ap_crypto.cpp").read_text()
        source = source.replace("// RX DIAGNOSTIC DECLARATIONS", receive_diagnostics())
        header = "itl80211/openbsd/net80211/ieee80211.h"
        input_path = "itl80211/openbsd/net80211/ieee80211_input.c"
        driver = "itlwm/hal_iwx/ItlIwx.cpp"
        routines = "\n".join(definition(header, name) for name in (
            "ieee80211_has_seq", "ieee80211_has_addr4", "ieee80211_has_qos",
            "ieee80211_has_htc", "ieee80211_get_qos"))
        routines += "\n" + definition(input_path, "ieee80211_get_hdrlen")
        source = source.replace("// HEADER ROUTINES", routines)
        routines = definition("itl80211/openbsd/net80211/ieee80211_crypto.c", "ieee80211_get_rxkey")
        routines += "\n" + definition("itl80211/openbsd/net80211/ieee80211_crypto_ccmp.c",
                                      "ieee80211_ccmp_get_pn")
        routines += "\n" + definition(input_path, "ieee80211_input_hwdecrypt")
        routines += "\n" + definition(input_path, "ieee80211_sta_foreign_disconnect")
        source = source.replace("// NET80211 CRYPTO", routines)
        routines = "\n".join(definition(driver, "ItlIwx::" + name) for name in (
            "observeAWDLReceiveDescriptor", "formatAWDLReceiveDiagnostics", "iwx_rx_mpdu_mq", "receiveAWDL", "iwx_rxmq_get_signal_strength",
            "iwx_rx_hwdecrypt", "iwx_detect_duplicate", "iwx_ccmp_decap", "iwx_rx_frame"))
        source = source.replace("// DRIVER ROUTINES", routines)
        active = re.sub(r"#if NBPFILTER > 0\n.*?#endif", "", source, flags=re.S)
        source = source.replace("// FIRMWARE DECLARATIONS", declarations(
            ("iwx_rx_mpdu_desc_v1", "iwx_rx_mpdu_desc_v3", "iwx_rx_mpdu_desc", "iwx_rxq_dup_data"), active))
        self.compile_run(source.replace("// TEST ENTRY", entry))

    def test_raw_copies_cannot_poison_later_valid_ap_retry(self):
        self.fixture("rawCopyBeforeValid();")

    def test_ap_packet_number_and_duplicate_ownership(self):
        self.fixture("replayAndSubframes();")

    def test_awdl_blockack_and_foreign_data_avoid_ap_crypto(self):
        self.fixture("receiveOwnership();")

    def test_foreign_disconnects_do_not_poison_ap_duplicate_state(self):
        self.fixture("disconnectOwnership();")


if __name__ == "__main__":
    unittest.main()
