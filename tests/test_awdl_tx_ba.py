"""Exercise firmware TX aggregation reports through production completion code."""
import re
import unittest
import test_regressions as support
from test_offline import definition


class TXBlockAckRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def run_case(self, case):
        source = (support.ROOT / 'tests/fixtures/awdl_completion.cpp').read_text().rsplit('int main()', 1)[0]
        source += (support.ROOT / 'tests/fixtures/awdl_tx_ba.cpp').read_text()
        wire = ['using u8=uint8_t;using __le16=uint16_t;using __le32=uint32_t;',
                '#define IWX_TX_FLUSH_QUEUE_RSP 16',
                'enum {IWX_BA_RESP_TX_AGG=0,IWX_BA_RESP_TX_BAR=1,IWX_BA_RESP_TX_AGG_FAIL=2};']
        for path, names in (
            ('itlwm/hal_iwx/if_iwxreg.h', ('iwx_agg_tx_status', 'iwx_tx_resp', 'iwx_tx_path_flush_cmd',
                'iwx_flush_queue_info', 'iwx_tx_path_flush_cmd_rsp', 'iwx_compressed_ba_ratid',
                'iwx_compressed_ba_tfd', 'iwx_compressed_ba_notif')),
            ('itlwm/hal_iwx/if_iwxvar.h', ('iwx_tx_data',))):
            header = (support.ROOT / path).read_text()
            wire.extend(re.search(r'struct ' + name + r'\s*\{.*?\}[^;]*;', header, re.S)[0] for name in names)
        source = source.replace('// NATIVE DECLARATIONS', '\n'.join(wire))
        routines = definition('itlwm/hal_iwx/ItlIwxAWDL.cpp', 'ItlIwx::completeAWDLTx') + '\n'
        routines += '\n'.join(definition('itlwm/hal_iwx/ItlIwx.cpp', 'ItlIwx::' + name) for name in (
            'iwx_rx_tx_cmd_single', 'iwx_rx_tx_cmd', 'iwx_rx_tx_ba_notif', 'iwx_txd_done',
            'iwx_ampdu_txq_advance', 'iwx_reset_tx_ring', 'iwx_flush_sta_tids'))
        self.compile_run('#define CASE ' + str(case) + '\n' + source.replace('// PRODUCTION', routines))

    def test_ba_reclaims_confirmed_prefix_across_ring_wrap(self): self.run_case(1)
    def test_transmitted_aggregate_waits_for_block_ack(self): self.run_case(2)
    def test_rejects_foreign_stale_and_malformed_ba(self): self.run_case(3)
    def test_final_single_status_and_full_pointer_validation(self): self.run_case(4)
    def test_flush_preserves_confirmed_prefix_and_cancels_suffix(self): self.run_case(5)
    def test_completion_callback_reset_stops_old_queue_reclaim(self): self.run_case(6)
    def test_request_answer_and_directional_delba_wire(self): self.run_case(7)
