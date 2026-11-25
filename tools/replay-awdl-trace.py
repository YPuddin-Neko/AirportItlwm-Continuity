#!/usr/bin/env python3
"""Replay retained AWDL header timing through the production timer and HAL."""

import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import test_awdl_drain
import test_offline
import iwx_firmware


def replace_once(source, before, after):
    if source.count(before) != 1:
        raise ValueError('Replay assembly boundary changed: ' + before[:80])
    return source.replace(before, after, 1)


def input_trace(path, output):
    with path.open() as stream:
        frames = [json.loads(line) for line in stream]
    frames = [item for item in frames if isinstance(item.get('tsft'), int)]
    origin = min(item['tsft'] for item in frames)
    labels = sorted({item.get('ta') for item in frames
                     if item.get('event') == 'awdl_action' and item.get('ta') != 'target'})
    peers = {label: index + 1 for index, label in enumerate(labels)}
    assert len(peers) < 32 and 'peer5' in peers
    acknowledged = set()
    for index, item in enumerate(frames):
        if index and item.get('event') == 'ack' and item.get('ra') == 'peer5':
            prior = frames[index - 1]
            if prior.get('event') == 'data' and prior.get('ta') == 'peer5' and \
                    prior.get('ra') == 'target' and 20 <= item['tsft'] - prior['tsft'] <= 3000:
                acknowledged.add(index - 1)
    rows = []
    outgoing_seen = {}
    outgoing_sizes = []
    for index, item in enumerate(frames):
        at = item['tsft'] - origin + 10000000
        if item.get('event') == 'awdl_action' and len(item.get('channels', [])) == 16:
            who = 0 if item['ta'] == 'target' else peers[item['ta']]
            period, presence = item['windowPeriodTU'], item['presence']
            delay = (item['phyTxTime'] - item['targetTxTime']) & 0xffffffff
            if not item['phyTxTime'] or not item['targetTxTime'] or delay > 65535:
                delay = 0
            assert period and presence and item['nextWindowTU'] <= period * presence
            phase = (period * presence - item['nextWindowTU']) * 1024 + delay
            rows.append([at, 1, who, 0, 0, 0, 0, phase, item['windowCounter'],
                         period, presence, *item['channels']])
        elif item.get('event') == 'data' and item.get('ta') == 'peer5' and item.get('ra') == 'target':
            rows.append([at, 2, peers['peer5'], item['wireBytes'], item['sequence'],
                         int(item['retry']), int(index in acknowledged), 0, 0, 0, 0, *([0] * 16)])
        elif item.get('event') == 'data' and item.get('ta') == 'target' and item.get('ra') == 'peer5':
            key = (item['sequence'], item['fragment'])
            previous = outgoing_seen.get(key)
            outgoing_seen[key] = at
            if previous is not None and at - previous < 3000000:
                continue
            # The retained length includes the monitor header; use it as a
            # conservative Ethernet size, without reconstructing any payload.
            size = item['wireBytes']
            assert 54 <= size <= 1514
            outgoing_sizes.append(size)
            rows.append([at, 3, peers['peer5'], size, item['sequence'],
                         int(item['retry']), 0, 0, 0, 0, 0, *([0] * 16)])
    rows.sort(key=lambda row: row[0])
    output.write_text('\n'.join(' '.join(map(str, row)) for row in rows) + '\n')
    return {'events': len(rows), 'incomingFrames': sum(row[1] == 2 for row in rows),
            'observedAdjacentACKs': len(acknowledged), 'peer': peers['peer5'],
            'outgoingUniqueFrames': len(outgoing_sizes),
            'outgoingWireBytes': sum(outgoing_sizes),
            'durationUS': rows[-1][0] - rows[0][0], 'source': str(path)}


def harness(root):
    # Reuse the tested native/HAL assembly, changing only the selected source root.
    previous, previous_firmware = test_offline.ROOT, iwx_firmware.ROOT
    previous_definition = test_awdl_drain.definition
    def historical_definition(path, name):
        if name == 'AirportItlwmAWDLInterface::releaseData' and \
                'AirportItlwmAWDLInterface::releaseData' not in (root / path).read_text():
            return ''  # Older single-packet timers do not call this helper.
        return previous_definition(path, name)
    test_awdl_drain.definition = historical_definition
    test_offline.ROOT = root
    iwx_firmware.ROOT = root
    try:
        case = test_awdl_drain.AWDLDrainRegressions()
        captured = []
        case.compile_run = captured.append
        case.test_full_tick_and_radio_lifecycle_after_cancel_or_natural_end()
        budget = '\n'.join(test_offline.definition('itlwm/hal_iwx/AWDLTxBudget.hpp', name)
                           for name in ['ofdmUS', 'rateCode', 'frameUS'])
        firmware = iwx_firmware.declarations((), budget)
    finally:
        test_awdl_drain.definition = previous_definition
        test_offline.ROOT = previous
        iwx_firmware.ROOT = previous_firmware
    source = captured[0].split('static void attachCallback(', 1)[0]
    radio_state = (root / 'AirportItlwm/AWDL/RadioState.hpp').read_text()
    if 'DataTID' not in radio_state:
        # Keep the historical scheduling/accounting methods. Only unused fields
        # needed by the shared firmware boundary are added to older snapshots.
        radio_state = replace_once(radio_state, '    int queue;',
            '    enum { DataTID = 0 };\n    int dataQueue; bool dataQueueConfigured;\n    int queue;')
        radio_state = radio_state.replace('"../../itlwm/hal_iwx/TxQueueSetup.hpp"',
            '"' + str(root / 'itlwm/hal_iwx/TxQueueSetup.hpp') + '"')
        source = replace_once(source, '#include "AirportItlwm/AWDL/RadioState.hpp"', radio_state)
    source = replace_once(source, '#include <cassert>', '#include <cassert>\n#include <algorithm>\n#include <fstream>\n#include <iostream>\n#include <limits>')
    source = replace_once(source, 'Packet *next=nullptr;', 'Packet *next=nullptr; uint64_t offeredUS=clockUS;')
    source = replace_once(source, 'struct Radio {', '#define BIT(n) (1U << (n))\n' + firmware +
                          '\nstruct ReplayBudget {\n' + budget + '\n};\nstruct Radio {')
    members = (ROOT / 'tests/fixtures/awdl_trace_replay.cpp').read_text()
    radio_members, scenarios = members.split('// REPLAY SCENARIOS', 1)
    source = replace_once(source, '    bool failRemove=false,autoStart=true;',
                            '    bool failRemove=false,autoStart=false;\n' + radio_members)
    source = replace_once(source, '++grants;if(autoStart)notify(1);if(onGrant)onGrant();return 0;',
                            '++grants;startAtUS=clockUS+startDelayUS;endAtUS=startAtUS+uint64_t(command->duration_tu)*1024;'
                            'if(autoStart)notify(1);if(onGrant)onGrant();return 0;')
    source = replace_once(source, '++cancels;return 0;', '++cancels;startAtUS=endAtUS=0;return 0;')
    source = replace_once(source, '!awdlRadio.dwell.running);++flushes;return 0;',
                            '!awdlRadio.dwell.running);++flushes;flushHardware();return 0;')
    start = source.index('    int transmitAWDL(')
    end = source.index('    void notify(', start)
    source = source[:start] + '''    int transmitAWDL(mbuf_t packet,uint64_t token=0,uint64_t deadlineUS=0) {
        assert(awdlRadio.dwell.running && clockUS<awdlRadio.dwell.windowEndUS);
        if (packet->bytes[0]==0xd0) { mbuf_freem(packet); return 0; }
        const uint32_t budget=ReplayBudget::frameUS(unsigned(packet->length),0x4207,true,true);
        assert(budget!=UINT32_MAX);
        const uint64_t cutoff=deadlineUS ? std::min(deadlineUS,awdlRadio.dwell.windowEndUS) : awdlRadio.dwell.windowEndUS;
        uint64_t pending=0;for(const auto &entry:hardware)pending+=entry.budgetUS;
        if (clockUS>=cutoff || pending+budget>cutoff-clockUS || hardware.size()>=48) {
            ++budgetRejected;mbuf_freem(packet);return ENOBUFS;
        }
        const uint64_t done=std::max(clockUS,hardware.empty()?clockUS:hardware.back().doneUS)+budget+completionExtraUS;
        const bool failed=failEvery && (++attempts%failEvery)==0;
        const unsigned queue=packet->bytes[0]==0x88?2:1;
        if(queue==2)assert(awdlRadio.dataQueueConfigured && com.txq[queue].allocated);
        hardware.push_back({token,done,budget,failed,queue});++com.txq[queue].queued;++tx;
        mbuf_freem(packet);return 0;
    }
''' + source[end:]
    return source + '\n' + scenarios


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--sessions', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--candidate', type=Path,
                        help='Also build and replay production code from this source tree')
    parser.add_argument('--baseline', type=Path,
                        help='Compare this saved source tree instead of the historical Preview60/61 pair')
    parser.add_argument('--aggregation', action='store_true',
                        help='Supply fresh HT capability and an established MCS7 rate to each source version')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    trace = args.output / 'timing.tsv'
    metadata = input_trace(args.capture.resolve(), trace)
    samples = json.loads(args.sessions.read_text())['samples']
    start_delays = [sample['tx']['SessionStartUS'] - sample['tx']['SessionAddUS']
                    for sample in samples
                    if sample['tx']['SessionStartUS'] >= sample['tx']['SessionAddUS'] > 0]
    start_delay = round(statistics.median(start_delays))
    metadata['firmwareStartDelay'] = {'source': str(args.sessions.resolve()),
                                    'samples': len(start_delays), 'medianUS': start_delay,
                                    'minimumUS': min(start_delays), 'maximumUS': max(start_delays)}
    versions = {'preview60': ROOT / 'build/diagnostics/preview59-boot-20261107/source-snapshot',
                'preview61': ROOT / 'build/diagnostics/preview60-boot-20261107/source-snapshot'}
    if args.baseline:
        versions = {'baseline': args.baseline.resolve()}
    if args.candidate:
        versions['candidate'] = args.candidate.resolve()
    profiles = [
        {'name': 'receive-only', 'loadMbps': 0, 'startDelayUS': start_delay, 'completionExtraUS': 80, 'failEvery': 0},
        {'name': 'recorded-outgoing', 'loadMbps': 0, 'startDelayUS': start_delay, 'completionExtraUS': 80, 'failEvery': 0, 'recordedOffers': True},
        {'name': 'observed-start-2mbps', 'loadMbps': 2, 'startDelayUS': start_delay, 'completionExtraUS': 80, 'failEvery': 0},
        {'name': 'ideal-8mbps', 'loadMbps': 8, 'startDelayUS': 0, 'completionExtraUS': 80, 'failEvery': 0},
        {'name': 'observed-start-8mbps', 'loadMbps': 8, 'startDelayUS': start_delay, 'completionExtraUS': 80, 'failEvery': 0},
        {'name': 'observed-start-16mbps', 'loadMbps': 16, 'startDelayUS': start_delay, 'completionExtraUS': 80, 'failEvery': 0},
        {'name': 'delayed-loss-16mbps', 'loadMbps': 16, 'startDelayUS': start_delay, 'completionExtraUS': 300, 'failEvery': 20},
        {'name': 'observed-start-16mbps-cap1024', 'loadMbps': 16, 'startDelayUS': start_delay, 'completionExtraUS': 80, 'failEvery': 0, 'backlogPackets': 1024},
        {'name': 'observed-start-16mbps-unbounded', 'loadMbps': 16, 'startDelayUS': start_delay, 'completionExtraUS': 80, 'failEvery': 0, 'backlogPackets': 0},
    ]
    for profile in profiles:
        profile.setdefault('backlogPackets', 128)
        profile.setdefault('recordedOffers', False)
    reports = []
    for version, root in versions.items():
        cpp, binary = args.output / (version + '.cpp'), args.output / version
        cpp.write_text(harness(root))
        command = ['xcrun', 'clang++', '-std=c++14', '-O1', '-g', '-fsanitize=address,undefined',
                   '-fno-sanitize-recover=all', '-I', str(root), '-I', str(ROOT),
                   '-idirafter', str(ROOT / 'itl80211/openbsd'), str(cpp), '-o', str(binary)]
        with (args.output / (version + '-build.log')).open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        for profile in profiles:
            command = [str(binary), str(trace), str(metadata['peer']),
                       *[str(profile[key]) for key in ['loadMbps','startDelayUS','completionExtraUS','failEvery','backlogPackets']],
                       str(int(profile['recordedOffers'])), str(int(args.aggregation))]
            run = subprocess.run(command, capture_output=True, text=True, timeout=45)
            (args.output / (version + '-' + profile['name'] + '.log')).write_text(run.stdout + run.stderr)
            run.check_returncode()
            report = {'version': version, 'profile': profile, **json.loads(run.stdout)}
            reports.append(report)
            print(json.dumps(report), flush=True)
    result = {'trace': metadata, 'sources': {name: str(root) for name, root in versions.items()},
              'aggregationProfile': args.aggregation, 'runs': reports,
              'scope': 'Production tick, peer-window decisions, queue ownership and HAL session lifecycle; recorded native schedule/peer advertisement inputs. Firmware and BSD are explicit host models.',
              'assumptions': ['The 2/8/16 Mbps loads are controlled stress inputs, not measured Sidecar input bitrates.',
                              'All observed advertisement updates are supplied even when the model is off-channel; management TX airtime is omitted.',
                              'TX stays at HT20 MCS7 (65 Mbps); rate adaptation, contention and receive/ACK firmware are not emulated.',
                              'The optional aggregation profile supplies fresh HT capabilities and an already-trained MCS7 rate. It exercises production pairing, not firmware A-MSDU support or a measured peer state.',
                              'Queue command round trips are instantaneous in the host model; additional data-queue setup latency is not measured.',
                              'BSD is a FIFO sensitivity model, not macOS FQ-CoDel. The 128-packet case uses the audited on-disk initial maxlen, not a measured live cap. 1024/unbounded are sensitivity controls.',
                              'Recorded outgoing offers use first observed air times, not original application enqueue times. Retained wire sizes include monitor headers and overestimate synthetic Ethernet lengths.',
                              f'{start_delay}us START delay is the rounded median of retained Preview60 session samples.',
                              'Completion overhead and periodic failures are sensitivity inputs, not measured per-packet firmware outcomes.',
                              'Receive opportunities are not actual firmware reception or RTCP delivery.',
                              'The header-only capture has no RTCP packet identity; an eight-second RTCP timeout cannot be inferred from generic input frames.']}
    (args.output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
