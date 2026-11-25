"""AWDL RX aggregation through production firmware ownership and IWX reorder code."""
import unittest
import test_regressions as support
from test_offline import definition
from iwx_firmware import declarations


class BlockAckRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def run_case(self, case):
        source = (support.ROOT / 'tests/fixtures/awdl_ba.cpp').read_text()
        hal = ('addAWDLBlockAckPeer', 'awdlHasRXBA', 'awdlRXBAOwned', 'awdlBlockAckLive',
               'removeAWDLBlockAckTID', 'removeAWDLBlockAckPeer', 'clearAWDLBlockAck',
               'resetAWDLBlockAck', 'prepareAWDLBlockAck', 'receiveAWDLBlockAckEnd',
               'requestAWDLBlockAckEnd', 'completeAWDLBlockAckEnd', 'serviceAWDLBlockAck',
               'deliverAWDLReordered', 'receiveAWDLReordered')
        rx = ('iwx_init_reorder_buffer', 'iwx_clear_reorder_buffer', 'iwx_is_sn_less',
              'iwx_release_frames', 'iwx_oldsn_workaround', 'iwx_rx_reorder',
              'iwx_reorder_timer_expired', 'iwx_sta_rx_agg')
        source = source.replace('// PRODUCTION', '\n'.join(
            definition('itlwm/hal_iwx/ItlIwxAWDL.cpp', 'ItlIwx::' + name) for name in hal)
            + '\n' + '\n'.join(definition('itlwm/hal_iwx/ItlIwx.cpp', 'ItlIwx::' + name) for name in rx))
        source = source.replace('// FIRMWARE', declarations((
            'iwx_rx_mpdu_desc_v1', 'iwx_rx_mpdu_desc_v3', 'iwx_rx_mpdu_desc',
            'iwx_add_sta_cmd', 'iwx_rm_sta_cmd', 'iwx_reorder_buffer',
            'iwx_reorder_buf_entry', 'iwx_rxba_data'), source))
        source = source.replace('// FROM REORDER BUFFER', definition(
            'itlwm/hal_iwx/if_iwxvar.h', 'iwx_rxba_data_from_reorder_buf'))
        self.compile_run('#define CASE ' + str(case) + '\n' + source)

    def test_wire_parameters_response_and_delba(self): self.run_case(1)
    def test_firmware_ownership_reuse_and_replacement(self): self.run_case(2)
    def test_capacity_collision_and_command_failures(self): self.run_case(3)
    def test_reset_stop_and_delba_during_sleeping_commands(self): self.run_case(4)
    def test_reorder_gaps_duplicates_wrap_and_expiry(self): self.run_case(5)
    def test_hardware_amsdu_subframes_and_ownership(self): self.run_case(6)
    def test_delba_expiry_and_ap_isolation(self): self.run_case(7)
    def test_old_sequence_teardown_and_stale_baid(self): self.run_case(8)

    def test_ap_session_capacity_and_baid_reuse(self): self.run_case(9)
