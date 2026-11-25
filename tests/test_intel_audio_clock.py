"""Execute the 24H32 clock response decoder and audio clock update callback."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class IntelAudioClockTests(unittest.TestCase):
    def test_native_clock_response_and_error_paths(self):
        with tempfile.TemporaryDirectory(prefix='intel-audio-clock-') as directory:
            executable = pathlib.Path(directory) / 'clock'
            subprocess.run([
                'clang++', '-arch', 'x86_64', '-std=c++17', '-O1', '-I', str(ROOT),
                str(ROOT / 'tests/fixtures/intel_audio_clock.cpp'), '-o', str(executable),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(executable)], check=True, capture_output=True,
                                    text=True, timeout=20)
            self.assertIn('passed: 16 cases; original missing-link failure reproduced', result.stdout)


if __name__ == '__main__':
    unittest.main()
