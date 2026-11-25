"""Exercise the production page installer with the real linked privacy image."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class IntelPrivacyInstallerTests(unittest.TestCase):
    def test_exact_profile_page_groups_and_revalidation(self):
        with tempfile.TemporaryDirectory(prefix='intel-privacy-installer-') as directory:
            target = pathlib.Path(directory)
            subprocess.run(['python3', str(ROOT / 'scripts/build-intel-privacy.py'),
                            '--output-dir', str(target)], check=True,
                           capture_output=True, text=True)
            for optimization in ('-O0', '-O2'):
                with self.subTest(optimization=optimization):
                    executable = target / ('fixture' + optimization)
                    subprocess.run([
                        'clang++', '-arch', 'x86_64', '-std=c++17', optimization,
                        '-Wall', '-Wextra', '-Werror', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-I', str(target),
                        str(ROOT / 'tests/fixtures/intel_privacy_installer.cpp'),
                        '-o', str(executable),
                    ], check=True, capture_output=True, text=True)
                    args = [str(executable)]
                    native = pathlib.Path('/private/tmp/bluetoothd24H32-x86_64')
                    if native.exists():
                        args.append(str(native))
                    result = subprocess.run(args, check=True, capture_output=True,
                                            text=True, timeout=20)
                    self.assertIn('partial and replay cases passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
