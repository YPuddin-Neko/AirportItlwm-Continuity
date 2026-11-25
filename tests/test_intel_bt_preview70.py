"""Execute 24H32 x86_64 MultiAdvRandomAddress retention patch for third-party controllers."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class IntelBTPreview70Tests(unittest.TestCase):
    def test_native_multi_adv_random_address_retention(self):
        with tempfile.TemporaryDirectory(prefix="intel-bt-p70-") as directory:
            executable = pathlib.Path(directory) / "preview70"
            subprocess.run(
                [
                    "clang++", "-arch", "x86_64", "-std=c++17", "-O1",
                    "-I", str(ROOT), str(ROOT / "tests/fixtures/intel_bt_preview70.cpp"),
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
            self.assertIn("native x86_64 MultiAdvRandomAddress patch passed", result.stdout)

    def test_bluetool_patch_wires_multi_adv_random_address_hook(self):
        patch_text = (ROOT / "bluetooth/patches/bluetool-intel-standard-hci.patch").read_text()
        self.assertIn("IntelStandardHCI::applyMultiAdvRandomAddress", patch_text)


if __name__ == "__main__":
    unittest.main()
