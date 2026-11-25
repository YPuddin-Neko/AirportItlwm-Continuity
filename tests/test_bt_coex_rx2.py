"""Exercise real two-RX coex policy, PHY commands and AP management transport."""
import re
import unittest
import test_regressions as support
from test_offline import definition
from iwx_firmware import declarations

DRIVER = 'itlwm/hal_iwx/ItlIwx.cpp'


def ordered_structs(names):
    headers = '\n'.join((support.ROOT / 'itlwm/hal_iwx' / name).read_text()
                        for name in ('if_iwxreg.h', 'if_iwxvar.h'))
    ordered = []
    def add(name):
        if name in ordered:
            return
        match = re.search(r'\bstruct ' + name + r'\s*\{', headers)
        if not match:
            raise ValueError('Missing wire declaration: ' + name)
        end, depth = headers.index('{', match.start()) + 1, 1
        while depth:
            depth += (headers[end] == '{') - (headers[end] == '}')
            end += 1
        body = re.sub(r'/\*.*?\*/|//[^\n]*', '', headers[match.end():end], flags=re.S)
        for dependency in re.findall(r'\bstruct (\w+)', body):
            add(dependency)
        ordered.append(name)
    for name in names:
        add(name)
    return ordered


class BTCoexRX2Regressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_actual_policy_ack_phy_statistics_and_epochs(self):
        source = (support.ROOT / 'tests/fixtures/iwx_bt_coex_rx2.cpp').read_text()
        names = ('iwx_send_bt_init_conf', 'iwx_bt_coex_update_ci', 'iwx_rx_bt_coex_profile',
                 'iwx_bt_coex_link', 'iwx_bt_coex_request', 'iwx_bt_coex_phy_chains',
                 'iwx_bt_coex_send_smps', 'iwx_bt_coex_complete_tx', 'iwx_bt_coex_service',
                 'iwx_bt_coex_statistics', 'iwx_phy_ctxt_cmd_v3', 'iwx_bt_coex_task')
        source = source.replace('// ROUTINES', '\n'.join(definition(DRIVER, 'ItlIwx::' + name) for name in names))
        names = ordered_structs(('iwx_bt_coex_cmd', 'iwx_bt_coex_ci_cmd', 'iwx_bt_coex_profile_notif',
                                 'iwx_bt_coex_reduced_txp_update_cmd', 'iwx_phy_context_cmd', 'iwx_rlc_config_cmd',
                                 'iwx_notif_statistics', 'iwx_notif_statistics_v11'))
        self.compile_run(source.replace('// WIRE', declarations(names, source)))

    def test_actual_completion_reclaim_reset_and_reference_ownership(self):
        source = (support.ROOT / 'tests/fixtures/iwx_bt_coex_completion.cpp').read_text()
        names = ('iwx_rx_tx_cmd_single', 'iwx_rx_tx_cmd', 'iwx_txd_done', 'iwx_ampdu_txq_advance',
                 'iwx_reset_tx_ring', 'iwx_free_tx_ring', 'iwx_bt_coex_complete_tx', 'iwx_rx_tx_ba_notif')
        routines = definition('itlwm/hal_iwx/ItlIwxAWDL.cpp', 'ItlIwx::completeAWDLTx') + '\n'
        routines += '\n'.join(definition(DRIVER, 'ItlIwx::' + name) for name in names)
        source = source.replace('// PRODUCTION', routines)
        wire = declarations(('iwx_agg_tx_status', 'iwx_tx_resp', 'iwx_tx_data', 'iwx_compressed_ba_ratid',
                             'iwx_compressed_ba_tfd', 'iwx_compressed_ba_notif'), source, ('iwx_ba_resp_flags',))
        self.compile_run(source.replace('// NATIVE DECLARATIONS', 'using u8=uint8_t;using __le16=uint16_t;using __le32=uint32_t;\n' + wire))

    def test_actual_ap_action_dma_submission_and_ack(self):
        source = (support.ROOT / 'tests/fixtures/iwx_bt_coex_transport.cpp').read_text()
        source = source.replace('#include "itlwm/hal_iwx/AWDLTxBudget.hpp"',
                                (support.ROOT / 'itlwm/hal_iwx/AWDLTxBudget.hpp').read_text())
        names = ('iwx_rval2ridx', 'iwx_tx_fill_awdl_cmd', 'iwx_tx_update_byte_tbl', 'iwx_tx',
                 'iwx_rx_tx_cmd_single', 'iwx_rx_tx_cmd', 'iwx_txd_done', 'iwx_ampdu_txq_advance', 'iwx_reset_tx_ring')
        routines = '\n'.join(definition(DRIVER, 'ItlIwx::' + name) for name in names)
        routines += '\n' + definition(DRIVER, 'iwx_clear_tx_desc')
        routines += '\n' + definition('itlwm/hal_iwx/ItlIwxAWDL.cpp', 'ItlIwx::completeAWDLTx')
        source = source.replace('// PRODUCTION', routines)
        driver = (support.ROOT / DRIVER).read_text()
        table = re.search(r'const struct iwx_rate \{.*?\} iwx_rates\[\] = \{.*?^\};', driver, re.S | re.M)[0]
        net = (support.ROOT / 'itl80211/openbsd/net80211/ieee80211.c').read_text()
        rates = re.search(r'const struct ieee80211_rateset ieee80211_std_rateset_11a =.*?;', net, re.S)[0]
        source = source.replace('// RATE DECLARATIONS', table + rates)
        names = ('iwx_mac_data_ap', 'iwx_mac_data_ibss', 'iwx_mac_data_sta', 'iwx_mac_data_go',
                 'iwx_mac_data_p2p_sta', 'iwx_mac_data_pibss', 'iwx_mac_data_p2p_dev', 'iwx_ac_qos',
                 'iwx_mac_ctx_cmd', 'iwx_fw_channel_info', 'iwx_phy_context_cmd', 'iwx_binding_cmd',
                 'iwx_device_power_cmd', 'iwx_add_sta_cmd', 'iwx_rm_sta_cmd', 'iwx_session_prot_cmd',
                 'iwx_session_prot_notif', 'iwx_tx_data', 'iwx_tfh_tb', 'iwx_tfh_tfd', 'iwx_cmd_header',
                 'iwx_cmd_header_wide', 'iwx_device_cmd', 'iwx_dram_sec_info', 'iwx_tx_cmd_gen2',
                 'iwx_tx_cmd_gen3', 'iwx_agn_scd_bc_tbl', 'iwx_gen3_bc_tbl', 'iwx_agg_tx_status', 'iwx_tx_resp')
        self.compile_run(source.replace('// FIRMWARE DECLARATIONS', declarations(names, source, ('iwx_gen2_tx_fifo',))))


if __name__ == '__main__':
    unittest.main()
