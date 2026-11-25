"""Exercise asynchronous trace replay against the current production routines."""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_regressions import ROOT

SPEC = importlib.util.spec_from_file_location('awdl_trace_replay', ROOT / 'tools/replay-awdl-trace.py')
REPLAY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPLAY)


class AWDLTraceReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='awdl-trace-regression-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        source = cls.directory / 'replay.cpp'
        source.write_text(REPLAY.harness(ROOT))
        cls.binary = cls.directory / 'replay'
        subprocess.run(['xcrun', 'clang++', '-std=c++14', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-I', str(ROOT), '-idirafter', str(ROOT / 'itl80211/openbsd'),
                        str(source), '-o', str(cls.binary)], check=True)
        local = [6 if slot % 8 < 6 else 0 for slot in range(16)]
        peer = [149, 149, 149, 149, 149, 149, 48, 48, 6, 6, 149, 6, 6, 6, 48, 48]
        rows = []
        for elapsed in range(0, 10_000_001, 4000):
            at = 10_000_000 + elapsed
            counter, phase = (32 + elapsed // 16384) & 65535, elapsed % 65536
            if elapsed % 16000 == 0:
                for who, channels in [(0, local), (1, peer)]:
                    rows.append([at, 1, who, 0, 0, 0, 0, phase, counter, 16, 4, *channels])
            slot = (counter // 4) % 16
            if local[slot] == peer[slot] == 6:
                rows.append([at, 2, 1, 150, elapsed // 4000, 0, 0, 0, 0, 0, 0, *([0] * 16)])
        cls.trace = cls.directory / 'synthetic.tsv'
        cls.trace.write_text('\n'.join(' '.join(map(str, row)) for row in rows) + '\n')

    def replay(self, load, overhead=80, failures=0, capacity=128, aggregation=False):
        result = subprocess.run([str(self.binary), str(self.trace), '1', str(load),
                                 '4539', str(overhead), str(failures), str(capacity), '0', str(int(aggregation))],
                                capture_output=True, text=True, check=True, timeout=20)
        return json.loads(result.stdout)

    def test_receive_only_and_low_load_do_not_invent_backlog_or_loss(self):
        receive = self.replay(0)
        self.assertEqual(receive['offeredBytes'], 0)
        self.assertEqual(receive['maxBSDBacklogPackets'], 0)
        self.assertGreater(receive['receiveOpportunities'], 0)
        light = self.replay(2)
        self.assertGreater(light['goodputMbps'], 1.7)
        self.assertEqual(light['upstreamDroppedBytes'], 0)

    def test_sustained_load_has_bounded_backlog_and_real_completion_delay(self):
        loaded = self.replay(16)
        self.assertEqual(loaded['maxBSDBacklogPackets'], 128)
        self.assertGreater(loaded['upstreamDroppedBytes'], 0)
        self.assertGreater(loaded['completionP95US'], 100000)
        self.assertGreater(loaded['fullQueueTicks'], loaded['ticks'] // 2)
        self.assertLess(loaded['goodputMbps'], 16)

    def test_late_failed_completions_reduce_capacity_and_release_ownership(self):
        clean = self.replay(16)
        delayed = self.replay(16, overhead=300, failures=20)
        self.assertLess(delayed['goodputMbps'], clean['goodputMbps'])
        self.assertGreater(delayed['driverFlushed'], clean['driverFlushed'])
        self.assertGreater(delayed['driverRetried'], 0)
        self.assertGreater(delayed['abandoned'], clean['abandoned'])

    def test_larger_upstream_queue_trades_drops_for_delay(self):
        small, large = self.replay(16), self.replay(16, capacity=1024)
        self.assertGreater(large['completionP95US'], small['completionP95US'])
        self.assertLess(large['upstreamDroppedBytes'], small['upstreamDroppedBytes'])
        self.assertAlmostEqual(large['goodputMbps'], small['goodputMbps'], delta=0.2)

    def test_pairs_reduce_mpdu_work_without_changing_advertised_windows(self):
        single = self.replay(16, overhead=300)
        paired = self.replay(16, overhead=300, aggregation=True)
        self.assertGreater(paired['aggregatedMPDUs'], 0)
        self.assertLess(paired['submittedMPDUs'], paired['submitted'])
        self.assertLessEqual(paired['maxBSDBacklogPackets'], 128)
        self.assertGreater(paired['goodputMbps'], single['goodputMbps'])
        self.assertEqual(paired['advertisedUS'], single['advertisedUS'])
        self.assertEqual(paired['maxReservationStreakUS'], single['maxReservationStreakUS'])
        delayed = self.replay(16, overhead=300, failures=20, aggregation=True)
        self.assertGreater(delayed['driverFlushed'], 0)
        self.assertGreater(delayed['driverRetried'], 0)
        self.assertLessEqual(delayed['acknowledgedBytes'], delayed['offeredBytes'])

    def test_advertised_time_integrates_between_packet_and_timer_boundaries(self):
        duration = 16 * 65536 + 12345
        for channels, expected in [([0] * 16, 0), ([6] * 16, duration),
                                   ([6, 0] * 8, 8 * 65536 + 12345)]:
            rows = []
            for elapsed in (0, duration):
                for who, sequence in [(0, channels), (1, [149] * 16)]:
                    rows.append([10_000_000 + elapsed, 1, who, 0, 0, 0, 0,
                                 elapsed % 65536, elapsed // 16384, 16, 4, *sequence])
            trace = self.directory / 'availability.tsv'
            trace.write_text('\n'.join(' '.join(map(str, row)) for row in rows) + '\n')
            result = subprocess.run([str(self.binary), str(trace), '1', '0',
                                     '4539', '80', '0', '128', '0'],
                                    capture_output=True, text=True, check=True, timeout=20)
            data = json.loads(result.stdout)
            self.assertEqual(data['durationUS'], duration)
            self.assertEqual(data['advertisedUS'], expected)
            self.assertLessEqual(data['advertisedReceiveUS'], expected)
            self.assertLessEqual(data['advertisedReceiveUS'], data['runningUS'])
            self.assertLessEqual(data['runningUS'], data['reservedUS'])
            self.assertLessEqual(data['reservedUS'], duration)
            if not expected:
                self.assertEqual(data['reservedUS'], 0)
            else:
                self.assertGreater(data['advertisedReceiveUS'], 0)
                self.assertLess(data['advertisedReceiveUS'], expected)
                self.assertLess(data['runningUS'], data['reservedUS'])

    def test_trace_keeps_retry_attempts_and_uses_ack_receiver_direction(self):
        action = dict(event='awdl_action', tsft=1000, ta='peer5', channels=[6] * 16,
                      windowPeriodTU=16, presence=4, phyTxTime=2049,
                      targetTxTime=1, nextWindowTU=0, windowCounter=32)
        outgoing = dict(event='data', ta='target', ra='peer5', wireBytes=1200,
                        sequence=17, fragment=0, retry=False)
        incoming = dict(event='data', ta='peer5', ra='target', wireBytes=150,
                        sequence=18, fragment=0, retry=False)
        frames = [action, {**action, 'ta': 'target', 'tsft': 1001},
                  {**outgoing, 'tsft': 1100}, {**outgoing, 'tsft': 1200, 'retry': True},
                  {**incoming, 'tsft': 1300}, dict(event='ack', ra='target', tsft=1400),
                  {**incoming, 'tsft': 1500, 'retry': True},
                  dict(event='ack', ra='peer5', tsft=1600),
                  {**outgoing, 'tsft': 4_001_100}]
        capture, trace = self.directory / 'headers.ndjson', self.directory / 'converted.tsv'
        capture.write_text(''.join(json.dumps(frame) + '\n' for frame in frames))
        metadata = REPLAY.input_trace(capture, trace)
        self.assertEqual(metadata['incomingFrames'], 2)
        self.assertEqual(metadata['outgoingUniqueFrames'], 2)
        self.assertEqual(metadata['observedAdjacentACKs'], 1)
        rows = [list(map(int, line.split())) for line in trace.read_text().splitlines()]
        self.assertEqual([row[6] for row in rows if row[1] == 2], [0, 1])
        # A delayed PHY transmission can cross an EAW; the replay must retain it.
        self.assertEqual(rows[0][7], 67584)


if __name__ == '__main__':
    unittest.main()
