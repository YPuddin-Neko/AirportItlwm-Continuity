"""Exercise the native-callsite privacy payload with synthetic addresses."""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'bluetooth/BlueToolFixup/IntelPrivacy.cpp'
FIXTURE = ROOT / 'tests/fixtures/intel_privacy.cpp'


class IntelPrivacyTests(unittest.TestCase):
    def test_native_abi_admission_and_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix='intel-privacy-') as directory:
            for optimization in ('-O0', '-O2'):
                with self.subTest(optimization=optimization):
                    executable = pathlib.Path(directory) / ('fixture' + optimization)
                    subprocess.run([
                        'clang++', '-arch', 'x86_64', '-std=c++17', optimization,
                        '-Wall', '-Wextra', '-Werror', str(SOURCE), str(FIXTURE),
                        '-o', str(executable),
                    ], check=True, capture_output=True, text=True)
                    result = subprocess.run([str(executable)], check=True,
                                            capture_output=True, text=True, timeout=20)
                    self.assertIn('17 synthetic cases passed', result.stdout)

    def test_freestanding_size_and_dependencies(self):
        with tempfile.TemporaryDirectory(prefix='intel-privacy-payload-') as directory:
            target = pathlib.Path(directory) / 'payload.o'
            subprocess.run([
                'clang++', '-arch', 'x86_64', '-std=c++17', '-Os', '-ffreestanding',
                '-fno-exceptions', '-fno-rtti', '-fno-stack-protector', '-fno-builtin',
                '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
                '-fvisibility=hidden', '-c', str(SOURCE), '-o', str(target),
            ], check=True, capture_output=True, text=True)
            undefined = subprocess.check_output(['nm', '-u', str(target)], text=True)
            self.assertEqual(undefined.strip(), '_intelPrivacyConfig')
            sizes = subprocess.check_output(['size', '-m', str(target)], text=True)
            code = re.search(r'Section \(__TEXT, __text\): (\d+)', sizes)
            state = re.search(r'Section \(__DATA, __common\): (\d+)', sizes)
            self.assertIsNotNone(code)
            self.assertIsNotNone(state)
            self.assertLessEqual(int(code.group(1)), 3216)
            self.assertEqual(int(state.group(1)), 24)


if __name__ == '__main__':
    unittest.main()
