"""Execute 24H32 x86_64 BT-Spec RPA and third-party 1M PHY patches."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class IntelBTPreview69Tests(unittest.TestCase):
    def test_native_adv_set_random_address_and_third_party_force_1m(self):
        with tempfile.TemporaryDirectory(prefix="intel-bt-p69-") as directory:
            executable = pathlib.Path(directory) / "preview69"
            subprocess.run(
                [
                    "clang++", "-arch", "x86_64", "-std=c++17", "-O1",
                    "-I", str(ROOT), str(ROOT / "tests/fixtures/intel_bt_preview69.cpp"),
                    "-o", str(executable),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            result = subprocess.run(
                [str(executable)],
                check=True,
                capture_output=True,
                text=True,
                timeout=20,
            )
            self.assertIn("native x86_64 AdvSetRandomAddress patch passed", result.stdout)
            self.assertIn("native x86_64 ThirdPartyForce1M patch passed", result.stdout)

    def test_bluetool_patch_wires_both_preview69_hooks(self):
        patch_text = (ROOT / "bluetooth/patches/bluetool-intel-standard-hci.patch").read_text()
        self.assertIn("IntelStandardHCI::applyAdvSetRandomAddress", patch_text)
        self.assertIn("IntelStandardHCI::applyThirdPartyForce1M", patch_text)


if __name__ == "__main__":
    unittest.main()
