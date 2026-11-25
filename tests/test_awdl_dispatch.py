"""Drive complete production RX batches with firmware wire layouts and host DMA."""

import re
import unittest

import test_regressions as support
from test_offline import definition
from iwx_firmware import receive_diagnostics


class AWDLDispatchRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_firmware_batch_dispatch_and_ownership(self):
        self.fixture("completeBatch();cancellationAndIsolation();receiveOwnershipAndFailures();"
                     "failedStartAndMalformed();ringWrapAndNotificationOrigin();concurrentAPData();coexistenceBatch();")

    def test_clone_allocation_failure_keeps_control_notifications(self):
        self.fixture("cloneAllocationFailure();")

    def test_refill_allocation_failure_keeps_control_notifications(self):
        self.fixture("refillAllocationFailure();")

    def test_refill_recovery_within_same_batch(self):
        self.fixture("refillAllocationRecovery();")

    def test_final_mpdu_recovery_and_ax210_batch_boundary(self):
        self.fixture("finalMPDURecoveryAndAX210Boundary();")

    def test_truncated_mpdu_descriptor_keeps_following_notifications(self):
        self.fixture("truncatedMPDUDescriptor();")

    def test_mpdu_envelope_prevents_overwriting_following_end(self):
        self.fixture("mpduEnvelopeOverlap();")

    def test_bar_release_requires_matching_live_session(self):
        self.fixture("barReleaseOwnership();")

    def test_peer_reply_matching_and_ap_isolation_across_bssids(self):
        self.fixture("peerManagementBSSID();")

    def test_delba_across_bssids_requires_owned_receive_session(self):
        self.fixture("peerDELBAAcrossBSSID();")

    def test_listener_control_frames_do_not_enter_ap_state(self):
        self.fixture("controlFrameIsolation();")

    def fixture(self, entry):
        root = support.ROOT
        source = (root / "tests/fixtures/awdl_dispatch.cpp").read_text()
        source = source.replace("// RX DIAGNOSTIC DECLARATIONS", receive_diagnostics())
        driver = "itlwm/hal_iwx/ItlIwx.cpp"
        radio = "itlwm/hal_iwx/ItlIwxAWDL.cpp"
        routines = ["iwx_rx_pkt_valid", "iwx_rx_pkt", "iwx_notif_intr",
                    "iwx_cmd_done", "iwx_rx_session_protection", "iwx_rx_tx_cmd",
                    "iwx_rx_tx_cmd_single", "iwx_txd_done", "iwx_ampdu_txq_advance",
                    "observeAWDLReceiveDescriptor", "formatAWDLReceiveDiagnostics", "iwx_rx_mpdu_mq", "receiveAWDL", "iwx_rxmq_get_signal_strength"]
        routines.append("iwx_rx_bt_coex_profile")
        production = definition("itl80211/openbsd/net80211/ieee80211_input.c",
                                "ieee80211_sta_foreign_disconnect") + "\n"
        production += definition(driver, "iwx_clear_tx_desc") + "\n"
        production += definition(radio, "awdlRadioNow") + "\n"
        production += "\n".join(definition(driver, "ItlIwx::" + name) for name in routines)
        production += "\n" + "\n".join(definition(radio, "ItlIwx::" + name)
                                      for name in ("receiveAWDLSession", "completeAWDLTx", "receiveAWDLTXBlockAck", "receiveAWDLBlockAckEnd"))
        source = source.replace("// PRODUCTION ROUTINES", production)
        var = (root / "itlwm/hal_iwx/if_iwxvar.h").read_text()
        structs = []
        for name in ("iwx_tx_data", "iwx_rx_data"):
            structs.append(re.search(r"struct " + name + r" \{.*?\n\};", var, re.S).group())
        constants = ("IWX_RX_MQ_RING_COUNT", "IWX_RBUF_SIZE", "IWX_FLAG_HW_ERR",
                     "IWX_FLAG_TE_ACTIVE", "IWX_DEVICE_FAMILY_22000", "IWX_DEVICE_FAMILY_AX210",
                     "IWX_STATION_ID", "IWX_INIT_COMPLETE", "IWX_PNVM_COMPLETE", "IWX_MAX_BAID")
        for name in constants:
            structs.append(re.search(r"^#define\s+" + name + r"\s+.*$", var, re.M).group())
        source = source.replace("// DRIVER DECLARATIONS", "\n".join(structs))
        raw = (root / driver).read_text()
        macro = re.search(r"^#define SYNC_RESP_STRUCT.*?while \(/\*CONSTCOND\*/0\)", raw, re.S | re.M).group()
        pnvm = re.search(r"struct iwl_pnvm_init_complete_ntfy \{.*?\} __packed;", raw, re.S).group()
        source = source.replace("// DISPATCH DECLARATIONS", macro + "\n" + pnvm)
        source = source.replace("// TEST ENTRY", entry)
        self.compile_run(source)


if __name__ == "__main__":
    unittest.main()
