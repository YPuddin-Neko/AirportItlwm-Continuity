"""Run the USB completion routines from the pinned Intel firmware patch."""

import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def postimage(path):
    patch = (ROOT / 'bluetooth/patches/intel-usb-completion.patch').read_text()
    section = patch.split('+++ b/' + path + '\n', 1)[1].split('\ndiff --git ', 1)[0]
    return '\n'.join(line[1:] for line in section.splitlines()
                     if line.startswith((' ', '+')))


def routine(source, name):
    start = source.index(name)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class IntelUSBCompletionTests(unittest.TestCase):
    def test_production_completion_lifetime(self):
        source = postimage('IntelBluetoothFirmware/USBDeviceController.cpp')
        header = postimage('IntelBluetoothFirmware/USBDeviceController.hpp')
        response = header[header.index('typedef struct {'):header.index('} InterruptResp;') + len('} InterruptResp;')]
        methods = '\n\n'.join(routine(source, name) for name in (
            'void USBDeviceController::\nfree()',
            'void USBDeviceController::\ninterruptHandler(',
            'IOReturn USBDeviceController::\ninterruptPipeRead('))
        with tempfile.TemporaryDirectory(prefix='intel-bt-usb-') as directory:
            temporary = pathlib.Path(directory)
            (temporary / 'response.hpp').write_text(response)
            (temporary / 'production.hpp').write_text(methods)
            executable = temporary / 'completion'
            subprocess.run([
                'clang++', '-std=c++17', '-O1', '-g', '-pthread',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I', str(temporary),
                str(ROOT / 'tests/fixtures/intel_bt_usb_completion.cpp'),
                '-o', str(executable),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(executable)], check=True, capture_output=True, text=True,
                                    timeout=20)
            self.assertIn('8 completion cases passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
