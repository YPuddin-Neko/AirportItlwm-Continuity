"""Drive native ADDBA/DELBA frames and recipient-window scheduling."""
import unittest
import test_awdl as awdl

class TXNativeRegressions(unittest.TestCase):
    compile_run=awdl.AWDLRegressions.compile_run
    run_tick_case=awdl.AWDLRegressions.run_tick_case
    def test_negotiation_tokens_retries_and_wire_frames(self): self.run_tick_case(39)
    def test_admission_failure_and_early_pause_deadline(self): self.run_tick_case(40)

    def test_retry_pair_split_preserves_each_awdl_sequence(self): self.run_tick_case(41)
    def test_tx_wakeup_refill_mid_burst_announcements_and_link_quality(self): self.run_tick_case(42)
    def test_multi_window_flush_survival_and_fifo_head_ordering(self): self.run_tick_case(43)
    def test_timer_wake_coalesces_across_packet_and_completion_bursts(self): self.run_tick_case(44)
