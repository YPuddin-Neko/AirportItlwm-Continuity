"""Exercise native retry refresh with real manager offsets and command outcomes."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class IntelPrivacyRetryTests(unittest.TestCase):
    def test_cancel_terminal_refresh_and_completion_ownership(self):
        with tempfile.TemporaryDirectory(prefix='intel-privacy-retry-') as directory:
            for optimization in ('-O0', '-Oz'):
                with self.subTest(optimization=optimization):
                    executable = pathlib.Path(directory) / ('fixture' + optimization)
                    subprocess.run([
                        'clang++', '-arch', 'x86_64', '-std=c++17', optimization,
                        '-Wall', '-Wextra', '-Werror', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(ROOT / 'bluetooth/BlueToolFixup/IntelPrivacy.cpp'),
                        str(ROOT / 'bluetooth/BlueToolFixup/IntelPrivacyRetry.cpp'),
                        str(ROOT / 'tests/fixtures/intel_privacy_retry.cpp'),
                        '-o', str(executable),
                    ], check=True, capture_output=True, text=True)
                    result = subprocess.run([str(executable)], check=True,
                        capture_output=True, text=True, timeout=20)
                    self.assertIn('47 retry lifecycle cases passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
