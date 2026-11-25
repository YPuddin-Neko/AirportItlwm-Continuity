"""Exercise the linked native payload layout and real wrapper instructions."""
import importlib.util
import pathlib
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('privacy_builder',ROOT/'scripts/build-intel-privacy.py')
BUILDER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILDER)


class IntelPrivacyBuildTests(unittest.TestCase):
    def test_wrappers_preserve_native_abi_and_gate_all_markers(self):
        with tempfile.TemporaryDirectory(prefix='intel-privacy-wrappers-') as directory:
            for optimization in ('-O0','-O2'):
                with self.subTest(optimization=optimization):
                    exe = pathlib.Path(directory)/('wrappers'+optimization)
                    subprocess.run(['clang++','-arch','x86_64','-std=c++17',optimization,
                                    str(ROOT/'tests/fixtures/intel_privacy_wrappers.cpp'),
                                    str(ROOT/'tests/fixtures/intel_privacy_wrappers.S'),
                                    str(ROOT/'bluetooth/BlueToolFixup/IntelPrivacyWrappers.S'),
                                    '-o',str(exe)],check=True,capture_output=True,text=True)
                    result = subprocess.run([str(exe)],check=True,capture_output=True,text=True,timeout=20)
                    self.assertIn('all-hook markers passed',result.stdout)

    def test_linked_profile_uses_native_addresses_and_zero_fill_runtime(self):
        with tempfile.TemporaryDirectory(prefix='intel-privacy-image-') as directory:
            directory = pathlib.Path(directory)
            report = BUILDER.build(directory)
            self.assertLessEqual(report['image_size'],3216)
            self.assertEqual(report['hooks'],12)
            self.assertEqual(report['runtime_size'],24)
            self.assertTrue(report['two_pass_layout_identical'])
            self.assertFalse(report['undefined_symbols'])
            self.assertEqual(len(report['verified_direct_branches']),23)
            image = BUILDER.MachO(directory/'IntelPrivacy.macho')
            symbols = image.symbols
            markers = symbols['_intelPrivacyHookMarkers']
            for i,hook in enumerate(BUILDER.HOOKS):
                role,offset,original,guard,export = hook
                relative,expected,length = struct.unpack('<qQB',image.read(markers+i*17,17))
                self.assertEqual(markers+relative,BUILDER.BASE+offset)
                patch = expected.to_bytes(8,'little')[:length]
                self.assertEqual(patch,BUILDER.replacement(offset,bytes.fromhex(original),symbols['_'+export]))
                target = BUILDER.BASE+offset+5+struct.unpack_from('<i',patch,1)[0]
                self.assertEqual(target,symbols['_'+export])
                self.assertEqual(patch[0],0xe8 if original.startswith('e8') else 0xe9)
            config = symbols['_intelPrivacyConfig']
            values = struct.unpack('<7q2Q',image.read(config,72))
            self.assertEqual(config+values[0],BUILDER.BASE+0x1f59f3)
            self.assertEqual(config+values[1],BUILDER.BASE+0x68b548)
            self.assertEqual(config+values[5],symbols['_intelPrivacyInstalledControllerGate'])
            self.assertEqual(values[-2:],(0,5000000))
            retry = symbols['_intelPrivacyRetryConfig']
            retry_values = struct.unpack('<11i',image.read(retry,44))
            self.assertEqual([retry+x for x in retry_values],
                [BUILDER.BASE+x for x in (0xa44ff0,0x27e472,0x7e54ee,0x9dc0a8,
                 0x68b608,0x67ef7c,0x64df4c,0x64ddba,0x202fc0,0xcbbb1,0xcbc7e)])
            self.assertNotIn('dyld',','.join(image.symbols))


if __name__=='__main__':
    unittest.main()
