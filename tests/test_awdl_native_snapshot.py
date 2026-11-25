"""Exercise the production native peer snapshot with synchronous native call boundaries."""

import unittest

import test_regressions as support
from test_offline import definition


class NativePeerSnapshotRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_native_snapshot_class_bounds_failures_and_gate_schedule(self):
        source = (support.ROOT / "tests/fixtures/awdl_native_snapshot.cpp").read_text()
        routine = definition("AirportItlwm/AirportItlwmAWDLInterface.cpp",
                             "AirportItlwmAWDLInterface::publishNativePeers")
        routine += "\n" + definition("AirportItlwm/AirportItlwmAWDLInterface.cpp",
                                     "AirportItlwmAWDLInterface::receiveAction")
        self.compile_run(source.replace("// PRODUCTION", routine))

    def test_release_header_has_no_native_import_dependency(self):
        self.compile_run('''
#define DEBUG 0
#include "AirportItlwm/AWDL/NativePeerSnapshot.hpp"
int main() { return 0; }
''')


if __name__ == "__main__":
    unittest.main()
