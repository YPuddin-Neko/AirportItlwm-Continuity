"""Exercise production Intel coex commands and notification handling."""
import unittest
import struct
import test_regressions as support
from test_offline import definition
from iwx_firmware import declarations

class BTCoexRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_quz_firmware_68_advertises_config6_ci2_and_mplut(self):
        data = (support.ROOT / 'itlwm/firmware/iwlwifi-QuZ-a0-jf-b0-68.ucode').read_bytes()
        self.assertEqual(struct.unpack_from('<II', data), (0, 0x0a4c5749))
        offset, capabilities, versions = 88, set(), {}
        while offset + 8 <= len(data):
            kind, length = struct.unpack_from('<II', data, offset)
            offset += 8
            self.assertLessEqual(offset + length, len(data))
            if kind == 30:
                index, bits = struct.unpack_from('<II', data, offset)
                capabilities.update(index * 32 + bit for bit in range(32) if bits & (1 << bit))
            elif kind == 48:
                self.assertEqual(length % 4, 0)
                for pos in range(offset, offset + length, 4):
                    command, group, version, notification = data[pos:pos + 4]
                    versions[(group, command)] = version
            offset += (length + 3) & ~3
        self.assertEqual(offset, len(data))
        self.assertEqual(versions[(1, 0x9b)], 6)
        self.assertEqual(versions[(1, 0x5d)], 2)
        self.assertIn(67, capabilities)
        self.assertEqual(versions[(1, 0x08)], 3)  # PHY_CONTEXT v3
        self.assertEqual(versions[(5, 0x0f)], 3)  # TLC_CONFIG v3 rate/antenna offload
        self.assertEqual(versions[(5, 0x08)], 2)  # RLC v2 owns receive chain counts
        self.assertIn(43, capabilities)
        self.assertIn(45, capabilities)

    def test_single_chain_config_ci_lifecycle_reset_and_profile(self):
        source = (support.ROOT / 'tests/fixtures/iwx_bt_coex.cpp').read_text()
        source = source.replace('// PRODUCTION', '\n'.join(definition(
            'itlwm/hal_iwx/ItlIwx.cpp', 'ItlIwx::' + name) for name in
            ('iwx_send_bt_init_conf', 'iwx_bt_coex_update_ci', 'iwx_rx_bt_coex_profile')))
        source = source.replace('// FIRMWARE DECLARATIONS', declarations(
            ('iwx_bt_coex_cmd', 'iwx_bt_coex_ci_cmd', 'iwx_bt_coex_profile_notif'), source))
        self.compile_run(source)

if __name__ == '__main__':
    unittest.main()
