"""Exercise production QoS conversion, DMA admission and final confirmations."""
import unittest
import test_awdl as awdl

class TXTransportRegressions(unittest.TestCase):
    compile_run=awdl.AWDLRegressions.compile_run
    run_radio_case=awdl.AWDLRegressions.run_radio_case
    def test_qos_sequence_firmware_rates_and_final_failure(self): self.run_radio_case(43)
    def test_amsdu_requires_peer_permission(self): self.run_radio_case(44)
    def test_admission_rechecks_after_dma_mapping(self): self.run_radio_case(45)
    def test_peer_dma_survives_window_pause(self): self.run_radio_case(46)
