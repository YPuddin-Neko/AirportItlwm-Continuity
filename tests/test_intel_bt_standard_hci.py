"""Execute original and patched 24H32 x86_64 policy predicates."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class IntelStandardHCITests(unittest.TestCase):
    def test_native_startup_and_resume_event_masks(self):
        with tempfile.TemporaryDirectory(prefix='intel-bt-events-') as directory:
            executable = pathlib.Path(directory) / 'events'
            subprocess.run([
                'clang++', '-arch', 'x86_64', '-std=c++17', '-O1',
                '-I', str(ROOT), str(ROOT / 'tests/fixtures/intel_bt_event_masks.cpp'),
                '-o', str(executable),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True,
                                    timeout=20)
            self.assertIn('native cold-start and resume masks passed', result.stdout)

    def test_real_x86_predicates_and_atomic_group(self):
        with tempfile.TemporaryDirectory(prefix='intel-bt-policy-') as directory:
            executable = pathlib.Path(directory) / 'policy'
            subprocess.run([
                'clang++', '-arch', 'x86_64', '-std=c++17', '-O1',
                '-I', str(ROOT), str(ROOT / 'tests/fixtures/intel_bt_standard_hci.cpp'),
                '-o', str(executable),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True,
                                    timeout=20)
            self.assertIn('whole-group matching passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
