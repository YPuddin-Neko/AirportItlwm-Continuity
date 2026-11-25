"""Exercise the Debug property, MAC command and pause lifecycle with host boundaries."""

import unittest

import test_regressions as support
from test_offline import definition
from iwx_firmware import declarations


class AWDLFilterRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def fixture(self, entry, debug=True):
        source = (support.ROOT / "tests/fixtures/awdl_filter.cpp").read_text()
        interface = "AirportItlwm/AirportItlwmAWDLInterface.cpp"
        radio = "itlwm/hal_iwx/ItlIwxAWDL.cpp"
        routines = "\n".join(definition(interface, "AirportItlwmAWDLInterface::" + name)
                              for name in ("setProperties", "publishReceiveFilter", "setEnabled"))
        if debug:
            routines += "\n" + definition(interface, "AirportItlwmAWDLInterface::setReceiveAllData")
            routines += "\n" + definition(interface, "AirportItlwmAWDLInterface::setCompareTxAntennas")
            routines += "\n" + definition(radio, "ItlIwx::awdlCompareArm")
        routines += "\n" + "\n".join(definition(radio, "ItlIwx::" + name)
                                    for name in ("awdlMAC", "startAWDLRadio", "stopAWDLRadio", "resetAWDLRadio", "serviceAWDLReceiveFilter"))
        source = source.replace("// PRODUCTION", routines)
        # Execute production pending-stop/start scheduling; the tick suite executes its full body.
        tick = definition(interface, "AirportItlwmAWDLInterface::tick")
        source = source.replace("// TIMER ENTRY", tick[tick.index("{") + 1:tick.index("    const bool available")])
        source = source.replace("// FIRMWARE DECLARATIONS", declarations(
            ("iwx_mac_data_ap", "iwx_mac_data_ibss", "iwx_mac_data_sta", "iwx_mac_data_go",
             "iwx_mac_data_p2p_sta", "iwx_mac_data_pibss", "iwx_mac_data_p2p_dev", "iwx_ac_qos",
             "iwx_mac_ctx_cmd", "iwx_device_power_cmd"), source, ("iwx_gen2_tx_fifo",)))
        source = source.replace("// TEST ENTRY", entry)
        self.compile_run(("#define DEBUG 1\n" if debug else "") + source)

    def test_negotiation_probe_timing_traffic_and_dialog_lifetime(self):
        self.fixture("receiveProbeLifecycle();receiveProbeAdmission();receiveProbeCommandLifetime();")

    def test_property_authorization_payload_and_radio_preconditions(self):
        self.fixture("authorization();preconditions();")

    def test_listener_owns_filter_and_reports_applied_state(self):
        for debug in (False, True):
            with self.subTest(debug=debug):
                self.fixture("listenerFilterAutomatic();", debug=debug)

    def test_listener_keeps_management_delivery_when_ba_disables_promiscuous_data(self):
        for debug in (False, True):
            with self.subTest(debug=debug):
                self.fixture("listenerManagementDuringBA();", debug=debug)

    def test_debug_antenna_comparison_authorization_timeout_and_lifecycle(self):
        self.fixture("antennaComparisonControl();")

    def test_same_boot_mac1_filter_toggle_preserves_command_and_lifecycle(self):
        self.fixture("toggle();")

    def test_modify_failure_stops_only_awdl_and_retains_truthful_selection(self):
        self.fixture("commandFailure();")

    def test_command_sleep_serialization_native_disable_stop_and_reset(self):
        self.fixture("interleaving();")

    def test_pending_cleanup_is_completed_by_timer_after_late_end(self):
        self.fixture("pendingCleanup();")

    def test_release_rejects_control_and_keeps_original_mac_filter(self):
        self.fixture("releaseDefaults();", debug=False)

    def test_quz0034_enables_mac1_data_on_start_and_after_reset(self):
        for debug in (False, True):
            with self.subTest(debug=debug):
                self.fixture("quz0034ReceiveData();", debug=debug)


if __name__ == "__main__":
    unittest.main()
