"""Run the receive-filter C tool with real CF values and a mocked IOKit registry."""

import json
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
STATUS_KEYS = {
    "command_return", "AWDLReceiveAllDataSupported", "AWDLReceiveAllData",
    "AWDLReceiveFilterKnown", "AWDLReceiveFilterCommandError",
    "AWDLReceiveFilterCleanupResult", "RadioReady", "RadioError",
    "RadioFailureStage", "RadioFailureError", "DwellStarts", "DwellEnds",
    "TxSubmitted", "TxCompleted", "TxFailed", "RxDelivered", "ActionFramesToSystem",
}


class AWDLFilterToolRegressions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = tempfile.TemporaryDirectory(prefix="awdl-filter-tool-")
        cls.addClassCleanup(directory.cleanup)
        cls.binary = Path(directory.name) / "test"
        subprocess.run([
            "xcrun", "clang", "-std=c11", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-I", str(ROOT), str(ROOT / "tests/fixtures/awdl_filter_tool.c"),
            "-framework", "CoreFoundation", "-framework", "IOKit", "-o", str(cls.binary),
        ], check=True)

    def invoke(self, scenario="normal", *arguments):
        result = subprocess.run([str(self.binary), scenario, *arguments],
                                text=True, capture_output=True, timeout=10)
        self.assertIn("MOCK_TRACE ", result.stderr, result.stderr)
        errors, trace = result.stderr.rsplit("MOCK_TRACE ", 1)
        observed = json.loads(trace)
        output = json.loads(result.stdout) if result.stdout.strip() else None
        if output is not None:
            self.assertEqual(set(output), STATUS_KEYS)
        for secret in ("PRIVATE_WIFI_NAME", "aa:bb:cc:dd:ee:ff", "PRIVATE_PEER_NAME"):
            self.assertNotIn(secret, result.stdout + errors)
        self.assertEqual(observed["unreleased_objects"], 0)
        return result.returncode, output, observed, errors

    def test_status_and_default_never_write(self):
        for arguments in ((), ("--status",)):
            with self.subTest(arguments=arguments):
                code, status, trace, errors = self.invoke("normal", *arguments)
                self.assertEqual(code, 0)
                self.assertEqual(errors, "")
                self.assertEqual(trace["writes"], 0)
                self.assertEqual(trace["reads"], 1)
                self.assertIsNone(status["command_return"])
                self.assertIs(status["AWDLReceiveAllData"], False)
                self.assertEqual(status["TxSubmitted"], 11)

    def test_enable_and_disable_write_one_boolean_to_unique_service(self):
        for argument, scenario, enabled in (("--enable", "normal", True),
                                             ("--disable", "enabled", False)):
            with self.subTest(argument=argument):
                code, status, trace, errors = self.invoke(scenario, argument)
                self.assertEqual(code, 0)
                self.assertEqual(errors, "")
                self.assertEqual(trace["matching_class"], "AirportItlwmAWDLInterface")
                self.assertEqual(trace["writes"], 1)
                self.assertEqual(trace["write_service"], 501)
                self.assertEqual(trace["write_key_count"], 1)
                self.assertEqual(trace["write_key"], "AWDLReceiveAllData")
                self.assertIs(trace["write_boolean"], enabled)
                self.assertEqual(trace["reads"], 2)
                self.assertEqual(status["command_return"], 0)
                self.assertIs(status["AWDLReceiveAllData"], enabled)
                self.assertEqual(status["TxSubmitted"], 22)

    def test_old_release_and_non_boolean_support_refuse_writes(self):
        for scenario in ("old", "release", "wrong_support_type"):
            for argument in ("--enable", "--disable"):
                with self.subTest(scenario=scenario, argument=argument):
                    code, status, trace, errors = self.invoke(scenario, argument)
                    self.assertEqual(code, 3)
                    self.assertEqual(trace["writes"], 0)
                    self.assertEqual(trace["reads"], 1)
                    self.assertIsNone(status["command_return"])
                    self.assertIn("does not support", errors)

    def test_missing_and_multiple_interfaces_refuse_before_read_or_write(self):
        for scenario in ("missing", "multiple"):
            for argument in ("--status", "--enable"):
                with self.subTest(scenario=scenario, argument=argument):
                    code, status, trace, errors = self.invoke(scenario, argument)
                    self.assertEqual(code, 2)
                    self.assertIsNone(status)
                    self.assertEqual(trace["reads"], 0)
                    self.assertEqual(trace["writes"], 0)
                    self.assertIn("not loaded" if scenario == "missing" else "More than one", errors)

    def test_invalid_arguments_do_not_call_iokit(self):
        for arguments in (("--invalid",), ("--enable", "extra"), ("--status", "--disable")):
            with self.subTest(arguments=arguments):
                code, status, trace, errors = self.invoke("normal", *arguments)
                self.assertEqual(code, 64)
                self.assertIsNone(status)
                self.assertEqual(trace["iokit_calls"], 0)
                self.assertIn("Usage:", errors)

    def test_write_failure_returns_error_and_prints_new_status(self):
        code, status, trace, errors = self.invoke("write_failure", "--enable")
        self.assertEqual(code, 1)
        self.assertEqual(trace["writes"], 1)
        self.assertEqual(trace["reads"], 2)
        self.assertEqual(status["command_return"], 5)
        self.assertEqual(status["AWDLReceiveFilterCommandError"], 5)
        self.assertIs(status["AWDLReceiveFilterKnown"], False)
        self.assertIs(status["AWDLReceiveAllData"], False)
        self.assertEqual(status["TxSubmitted"], 22)
        self.assertIn("command failed", errors)

    def test_registry_failures_propagate_without_unintended_writes(self):
        for scenario, reads, writes in (("matching_failure", 0, 0),
                                         ("read_failure", 1, 0),
                                         ("post_read_failure", 2, 1)):
            with self.subTest(scenario=scenario):
                code, status, trace, errors = self.invoke(scenario, "--enable")
                self.assertEqual(code, 1)
                self.assertIsNone(status)
                self.assertEqual(trace["reads"], reads)
                self.assertEqual(trace["writes"], writes)
                self.assertIn("Cannot", errors)

    def test_status_whitelist_omits_network_identity_and_non_scalar_values(self):
        code, status, trace, errors = self.invoke("types", "--status")
        self.assertEqual(code, 0)
        self.assertEqual(errors, "")
        self.assertEqual(trace["writes"], 0)
        self.assertEqual(status["DwellStarts"], 42)
        self.assertEqual(status["RadioError"], -7)
        self.assertIsNone(status["RadioReady"])
        self.assertIsNone(status["DwellEnds"])
        self.assertIsNone(status["RadioFailureError"])


if __name__ == "__main__":
    unittest.main()
