"""Exercise the production USB identity gate and native table lifetime."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'bluetooth/BlueToolFixup/IntelPrivacyController.cpp'
FIXTURE = ROOT / 'tests/fixtures/intel_privacy_controller.cpp'


class IntelPrivacyControllerTests(unittest.TestCase):
    def test_active_usb_identity_and_unlink_lifetime(self):
        with tempfile.TemporaryDirectory(prefix='intel-privacy-controller-') as directory:
            for optimization in ('-O0', '-O2'):
                with self.subTest(optimization=optimization):
                    executable = pathlib.Path(directory) / ('fixture' + optimization)
                    subprocess.run([
                        'clang++', '-arch', 'x86_64', '-std=c++17', optimization,
                        '-Wall', '-Wextra', '-Werror', '-pthread', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(SOURCE), str(FIXTURE), '-o', str(executable),
                    ], check=True, capture_output=True, text=True)
                    result = subprocess.run([str(executable)], check=True,
                                            capture_output=True, text=True, timeout=20)
                    self.assertIn('10 lifecycle cases passed', result.stdout)

    def test_freestanding_native_calls_have_no_external_dependencies(self):
        with tempfile.TemporaryDirectory(prefix='intel-privacy-controller-payload-') as directory:
            target = pathlib.Path(directory) / 'controller.o'
            subprocess.run([
                'clang++', '-arch', 'x86_64', '-std=c++17', '-Os', '-ffreestanding',
                '-fno-exceptions', '-fno-rtti', '-fno-stack-protector', '-fno-builtin',
                '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
                '-fvisibility=hidden', '-c', str(SOURCE), '-o', str(target),
            ], check=True, capture_output=True, text=True)
            undefined = subprocess.check_output(['nm', '-u', str(target)], text=True)
            self.assertEqual(undefined.strip(), '_intelPrivacyControllerConfig')


if __name__ == '__main__':
    unittest.main()
