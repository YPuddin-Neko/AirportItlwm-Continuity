"""Replay firmware regulatory profiles through production channel mapping."""
import re
import unittest
import test_regressions as support
from test_offline import definition
from iwx_firmware import declarations


class RegulatoryRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def source(self, mcc=False):
        source = (support.ROOT / 'tests/fixtures/iwx_regulatory.cpp').read_text()
        driver = (support.ROOT / 'itlwm/hal_iwx/ItlIwx.cpp').read_text()
        tables = '\n'.join(re.search(r'(?:static )?const uint8_t ' + name +
            r'\[\] = \{.*?\};', driver, re.S)[0] for name in
            ('iwx_nvm_channels_8000', 'iwx_nvm_channels_uhb'))
        local_macros = '\n'.join(re.findall(r'^#define IWX_(?:LAST_2GHZ_HT_PLUS|FIRST_2GHZ_HT_MINUS|NUM_2GHZ_CHANNELS)\b.*', driver, re.M))
        source = source.replace('// PRODUCTION', local_macros + '\n' + tables + '\n' + definition(
            'itlwm/hal_iwx/ItlIwx.cpp', 'ItlIwx::iwx_init_channel_map'))
        source = source.replace('// FIRMWARE DECLARATIONS', declarations((), source))
        if mcc:
            extra = (support.ROOT / 'tests/fixtures/iwx_regulatory_lifecycle.cpp').read_text()
            source = source.replace('// MCC MOCKS', extra.split('// MCC TESTS')[0])
            source = source.replace('// MCC TESTS', extra.split('// MCC TESTS')[1])
            source = source.replace('// MCC PRODUCTION', '\n'.join(definition(
                'itlwm/hal_iwx/ItlIwx.cpp', 'ItlIwx::' + name) for name in
                ('iwx_mcc_update', 'iwx_mcc_task', 'iwx_send_update_mcc_cmd')))
            source = source.replace('// MCC DECLARATIONS', declarations(
                ('iwx_mcc_update_cmd', 'iwx_mcc_chub_notif'), source,
                enums=('IWX_CMD_MODE',)))
            source = '#define MCC_TEST 1\n#define AIRPORT_WCL 1\n' + source
        return source

    def test_firmware_radar_and_uniform_bits_are_distinct(self):
        source = self.source()
        self.compile_run(source)

    def test_production_mcc_command_notification_and_commit_lifecycle(self):
        self.compile_run(self.source(True))


if __name__ == '__main__':
    unittest.main()
