"""Exercise the production data RX bridge and native peer notification boundary."""

import unittest

import test_regressions as support
from test_offline import definition


class NativeDataPeerRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def source(self, case, debug):
        fixture = (support.ROOT / "tests/fixtures/awdl_native_rx.cpp").read_text()
        routines = "\n".join(definition(
            "AirportItlwm/AirportItlwmAWDLInterface.cpp", name)
            for name in ("awdlPacket", "AirportItlwmAWDLInterface::recordNativeDataActivity",
                         "AirportItlwmAWDLInterface::notifyNativeDataPeer",
                         "AirportItlwmAWDLInterface::receive"))
        return (f"#define DEBUG {int(debug)}\n#define NATIVE_RX_CASE {case}\n" +
                fixture.replace("// PRODUCTION", routines))

    def run_case(self, case):
        for debug in (False, True):
            with self.subTest(debug=debug):
                self.compile_run(self.source(case, debug))

    def test_directed_rx_notifies_before_bsd_without_changing_native_policy(self):
        self.run_case(1)

    def test_software_peer_destination_and_duplicate_filtering(self):
        self.run_case(2)

    def test_native_manager_peer_types_and_contended_lock(self):
        self.run_case(3)

    def test_native_notification_requires_active_workloop_context(self):
        self.run_case(4)

    def test_receive_allocation_conversion_and_packet_ownership(self):
        self.run_case(5)

    def test_stopped_or_detached_receive_does_not_allocate_or_notify(self):
        self.run_case(6)

    def test_activity_requires_shared_gate_but_accepts_timer_thread(self):
        self.run_case(7)

    def test_native_counters_receive_ethernet_deltas_after_deduplication(self):
        self.run_case(8)

    def test_native_reachability_counters_survive_rx_and_monitor_cycles(self):
        self.run_case(9)


if __name__ == "__main__":
    unittest.main()
