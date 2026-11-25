"""Feed real TLC notifications into both native RATE getters."""
import re
import unittest
import test_regressions as support
from test_offline import definition
from iwx_firmware import declarations


class IWXRateReportRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_firmware_selected_rate_reaches_native_getters(self):
        driver = 'itlwm/hal_iwx/ItlIwx.cpp'
        source = (support.ROOT / 'tests/fixtures/iwx_rate_report.cpp').read_text()
        names = ['iwx_rs_extract_rate', 'iwx_hwrate_to_plcp_idx',
                 'iwx_new_rate_from_v1', 'iwx_he_rate_kbps']
        if 'iwx_tx_rate_kbps(' in (support.ROOT / driver).read_text():
            names.append('iwx_tx_rate_kbps')
        names.append('ItlIwx::iwx_rs_update')
        code = '\n'.join(definition(driver, name) for name in names)
        code += '\n' + definition('AirportItlwm/AirportItlwmSkywalkInterface.cpp',
                                  'AirportItlwmSkywalkInterface::getRATE')
        code += '\n' + definition('AirportItlwm/AirportSTAIOCTL.cpp', 'AirportItlwm::getRATE')
        # Execute the real state-entry prelude; management teardown below this
        # boundary is covered by the radio lifecycle fixtures.
        proto = (support.ROOT / 'itl80211/openbsd/net80211/ieee80211_proto.c').read_text()
        start = proto.index('int\nieee80211_newstate(')
        code += '\n' + proto[start:proto.index('\tieee80211_set_link_state(', start)] + '\nreturn 0;\n}'
        source = source.replace('// PRODUCTION', code)
        rates = re.search(r'const struct iwx_rate \{.*?\} iwx_rates\[\] = \{.*?^\};',
                          (support.ROOT / driver).read_text(), re.S | re.M)[0]
        net = (support.ROOT / 'itl80211/openbsd/net80211/ieee80211.c').read_text()
        for kind, suffix in (('ht', '11n'), ('vht', '11ac')):
            rates += '\n' + re.search(r'const struct ieee80211_' + kind + r'_rateset '
                r'ieee80211_std_ratesets_' + suffix + r'\[\] = \{.*?^\};', net, re.S | re.M)[0]
        source = source.replace('// RATE TABLES', rates)
        reg = (support.ROOT / 'itlwm/hal_iwx/if_iwxreg.h').read_text()
        indices = re.search(r'enum \{\s*IWX_RATE_1M_INDEX.*?\};', reg, re.S)[0]
        indices += '\n#define IWX_LAST_HE_RATE IWX_RATE_MCS_11_INDEX\n#define IWX_RATE_INVALID IWX_RATE_COUNT\n'
        known = set(re.findall(r'\bIWX_\w+', indices))
        request = re.sub(r'\bIWX_\w+', lambda m: '0' if m[0] in known else m[0], source)
        source = source.replace('// WIRE', indices + declarations(('iwx_tlc_update_notif',), request))
        self.compile_run(source)


if __name__ == '__main__':
    unittest.main()
