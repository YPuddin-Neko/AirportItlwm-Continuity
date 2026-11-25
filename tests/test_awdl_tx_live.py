"""Drive the production peer TX BA negotiation and firmware resource lifecycle."""
import unittest
import test_regressions as support
from test_offline import definition
from iwx_firmware import declarations


class TXBlockAckLiveRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def run_case(self, case):
        source = (support.ROOT / 'tests/fixtures/awdl_ba.cpp').read_text().rsplit('int main()', 1)[0]
        source += (support.ROOT / 'tests/fixtures/awdl_tx_live.cpp').read_text()
        hal = ('addAWDLBlockAckPeer', 'awdlHasRXBA', 'awdlRXBAOwned', 'awdlBlockAckLive',
               'removeAWDLBlockAckTID', 'removeAWDLBlockAckPeer', 'clearAWDLBlockAck',
               'resetAWDLBlockAck', 'prepareAWDLBlockAck', 'receiveAWDLBlockAckEnd',
               'requestAWDLBlockAckEnd', 'completeAWDLBlockAckEnd', 'serviceAWDLBlockAck',
               'deliverAWDLReordered', 'receiveAWDLReordered', 'noteAWDLTXBlockAck',
               'receiveAWDLTXBlockAck', 'serviceAWDLTXBlockAck', 'configureAWDLTXStation',
               'awdlTXAntenna', 'awdlTXMcs', 'awdlTXWidth', 'configureAWDLTXRates', 'setAWDLTXWindow',
               'completeAWDLTXManagement', 'closeAWDLListenerWindow', 'configureAWDLPeerWidth')
        rx = ('iwx_init_reorder_buffer', 'iwx_clear_reorder_buffer', 'iwx_is_sn_less',
              'iwx_release_frames', 'iwx_oldsn_workaround', 'iwx_rx_reorder',
              'iwx_reorder_timer_expired', 'iwx_sta_rx_agg')
        source = source.replace('// PRODUCTION', '\n'.join(
            definition('itlwm/hal_iwx/ItlIwxAWDL.cpp', 'ItlIwx::' + name) for name in hal)
            + '\n' + '\n'.join(definition('itlwm/hal_iwx/ItlIwx.cpp', 'ItlIwx::' + name) for name in rx))
        source = source.replace('// FIRMWARE', declarations((
            'iwx_rx_mpdu_desc_v1', 'iwx_rx_mpdu_desc_v3', 'iwx_rx_mpdu_desc',
            'iwx_add_sta_cmd', 'iwx_rm_sta_cmd', 'iwx_reorder_buffer',
            'iwx_reorder_buf_entry', 'iwx_rxba_data', 'iwx_tlc_config_cmd_v4',
            'iwl_tlc_config_cmd_v3'), source, ('iwx_tlc_mng_cfg_mode', 'iwx_tlc_mng_cfg_cw')))
        source = source.replace('// FROM REORDER BUFFER', definition(
            'itlwm/hal_iwx/if_iwxvar.h', 'iwx_rxba_data_from_reorder_buf'))
        self.compile_run('#define CASE ' + str(case) + '\n' + source)

    def test_80mhz_two_stream_tlc_abis_hardware_limits_and_reconfiguration(self): self.run_case(10)
    def test_window_close_and_reopen_keep_negotiation_station_unpaused(self): self.run_case(11)
    def test_channel_width_keeps_rx_reorder_and_queued_tx_agreement(self): self.run_case(12)
    def test_agreed_session_recovery_uses_fast_renegotiation_backoff(self): self.run_case(13)

    def test_bulk_trigger_and_confirmed_peer_negotiation(self): self.run_case(1)
    def test_recipient_delba_keeps_receive_session_and_queue(self): self.run_case(2)
    def test_missing_flush_pointers_end_receive_before_freeing_dma(self): self.run_case(3)
    def test_command_failures_never_release_firmware_owned_dma(self): self.run_case(4)
    def test_reset_and_stop_during_each_setup_command(self): self.run_case(5)
    def test_window_and_ap_identity_rechecked_after_resume(self): self.run_case(6)
    def test_peer_rates_bt_antenna_and_tlc_versions(self): self.run_case(7)
    def test_refusal_stale_dialog_and_delba_during_enable(self): self.run_case(8)

    def test_peer_rates_and_bt_change_during_firmware_commands(self): self.run_case(9)
