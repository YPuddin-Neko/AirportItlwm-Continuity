"""Verify that temporary controller unavailability does not latch privacy off."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'bluetooth/BlueToolFixup/IntelPrivacy.cpp'
HEADER = ROOT / 'bluetooth/BlueToolFixup/IntelPrivacy.hpp'
SUPPORT = ROOT / 'tests/fixtures/intel_privacy.cpp'
FIXTURE = ROOT / 'tests/fixtures/intel_privacy_recovery.cpp'


def fixture_source():
    support = SUPPORT.read_text().split('\nint main() {', 1)[0]
    support = support.replace('../../bluetooth/BlueToolFixup/IntelPrivacy.hpp', str(HEADER))
    return support + '\n' + FIXTURE.read_text()


class IntelPrivacyRecoveryTests(unittest.TestCase):
    def test_busy_start_and_controller_change_recover_without_extra_wake(self):
        with tempfile.TemporaryDirectory(prefix='intel-privacy-recovery-') as directory:
            fixture = pathlib.Path(directory) / 'fixture.cpp'
            fixture.write_text(fixture_source())
            for optimization in ('-O0', '-O2'):
                with self.subTest(optimization=optimization):
                    executable = pathlib.Path(directory) / ('fixture' + optimization)
                    subprocess.run([
                        'clang++', '-arch', 'x86_64', '-std=c++17', optimization,
                        '-Wall', '-Wextra', '-Werror', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(SOURCE), str(fixture), '-o', str(executable),
                    ], check=True, capture_output=True, text=True)
                    result = subprocess.run([str(executable)], check=True,
                                            capture_output=True, text=True, timeout=20)
                    self.assertIn('4 lifecycle cases passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
