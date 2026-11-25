#!/usr/bin/env python3
"""Run retained 24H32 election machine code with synthetic peers and clock."""

import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FUNCTIONS = ('shouldRunElection', 'runElection', 'isPeerQualifiedForElection',
             'runParentElection', 'electionParamsChanged', 'ageoutRootNodeList',
             'isRemoteScreenRTGSyncElectionEnabled')
STUBS = {
    0xffffff8000536cc0: 'fixtureClock',
    0xffffff8000536d80: 'fixtureNanoseconds',
    0xffffff8000383150: 'fixtureUnexpected',
    0xffffff8000104420: 'memcmp',
    0xffffff8000a49ab0: 'fixtureCast',
    0xffffff80021da96e: 'fixtureLog',
    0xffffff80022fb863: 'fixtureStateName',
    0xffffff80021e55c4: 'fixtureFindPeer',
    0xffffff800217882a: 'fixtureUpdateBroadcast',
    0xffffff80021783ca: 'fixtureUpdateTemplate',
    0xffffff800218718c: 'fixtureProgramMaster',
    0xffffff8002176d66: 'fixtureServices',
    0xffffff8000a4a150: 'fixtureName',
}


def assembly(image, disassembly):
    data = image.read_bytes()
    assert struct.unpack_from('<I', data)[0] == 0xfeedfacf
    segments, offset = [], 32
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            address, _, file_offset, file_size = struct.unpack_from('<QQQQ', data, offset + 24)
            segments.append((address, file_offset, file_size))
        offset += size

    def read(address, size):
        for base, offset, extent in segments:
            if base <= address and address + size <= base + extent:
                return data[offset + address-base:offset + address-base + size]
        raise ValueError('Address outside retained image: ' + hex(address))

    source = disassembly.read_text()
    bodies, symbols, ranges = {}, {}, {}
    for name in FUNCTIONS:
        match = re.search(r'^([0-9a-f]+) <IO80211AWDLPeerManager::' + name + r'\(.*?\)>:\n', source, re.M)
        assert match, name
        end = re.search(r'^[0-9a-f]+ <.*>:\n', source[match.end():], re.M)
        body = source[match.end():match.end()+end.start()]
        bodies[name] = body
        symbols[int(match[1], 16)] = 'native_' + name
        ranges[name] = (int(match[1], 16), int(end[0].split()[0], 16))

    output = ['.text']
    references, imported, bridges, manifest = set(), {}, set(), []
    for name, body in bodies.items():
        output += ['.p2align 4', '.globl _native_' + name, '_native_' + name + ':']
        relocated, length = 0, 0
        cursor, end = ranges[name]
        for line in body.splitlines():
            match = re.match(r'^([0-9a-f]+): ((?:[0-9a-f]{2}[ \t]+)+)(.*)$', line)
            if not match:
                continue
            address, raw, instruction = int(match[1], 16), bytes.fromhex(match[2]), match[3]
            assert address == cursor, (name, hex(cursor), hex(address))
            assert read(address, len(raw)) == raw, (name, hex(address))
            cursor += len(raw)
            length += len(raw)
            target, displacement = None, None
            if raw[0] == 0xe8:
                target = address + 5 + struct.unpack_from('<i', raw, 1)[0]
                displacement = 1
                symbol = symbols.get(target, STUBS.get(target))
                if symbol is None:
                    symbol = 'unexpected_' + format(target, 'x')
                    imported[target] = symbol
                elif target not in symbols:
                    bridges.add(symbol)
                    symbol = 'bridge_' + symbol
            elif '(%rip)' in instruction:
                assert len(raw) == 7, line
                target = address + 7 + struct.unpack_from('<i', raw, 3)[0]
                displacement = 3
                symbol = 'guardReference' if target == 0xffffff80023bc118 else 'reference_' + format(target, 'x')
                if target != 0xffffff80023bc118:
                    assert instruction.startswith('leaq'), line
                    references.add(symbol)
            if target is None:
                output.append('.byte ' + ','.join(str(value) for value in raw))
            else:
                assert displacement + 4 == len(raw)
                output.append('.byte ' + ','.join(str(value) for value in raw[:displacement]))
                output.append('.long _' + symbol + ' - . - 4')
                relocated += 1
        assert cursor == end, (name, hex(cursor), hex(end))
        manifest.append(dict(function=name, bytes=length, relocatedReferences=relocated))
    for address, symbol in imported.items():
        output += ['_' + symbol + ':', 'movabsq $' + hex(address) + ', %rdi', 'jmp _fixtureUnexpected']
    for symbol in sorted(bridges):
        output += ['_bridge_' + symbol + ':', 'jmp _' + symbol]
    output += ['.section __DATA,__data', '.p2align 3']
    output += ['_guardReference:', '.quad _fixtureGuard']
    for symbol in sorted(references):
        # These are names for logging or a metaclass argument consumed by the cast boundary.
        output += ['_' + symbol + ':', '.quad 0']
    return '\n'.join(output) + '\n', manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    base = ROOT / 'build/diagnostics'
    asm, manifest = assembly(base / '24H32-IO80211Family-analysis.macho',
        base / 'preview58-followup-20261107/channel-plan/io80211-disassembly.asm')
    args.output.mkdir(parents=True, exist_ok=True)
    path = args.output / 'native-election.s'
    path.write_text(asm)
    binary = args.output / 'native-election'
    sys.path.insert(0, str(ROOT / 'tests'))
    import test_regressions as support
    from test_offline import definition
    support.ROOT = args.source.resolve()
    import test_offline
    test_offline.ROOT = support.ROOT
    interface = definition('AirportItlwm/AirportItlwmAWDLInterface.cpp',
                           'AirportItlwmAWDLInterface::updateAvailability')
    headers = (ROOT / 'include/Airport/apple80211_ioctl.h').read_text() + '\n' + (
        ROOT / 'include/Airport/apple80211_var.h').read_text()
    declarations = '\n'.join(re.search(r'struct ' + name + r'\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;',
        headers, re.S)[0] for name in ('apple80211_channel', 'apple80211_availability_window_start',
                                     'apple80211_availability_window_extensions_end'))
    fixture = (ROOT / 'tests/fixtures/awdl_native_election_replay.cpp').read_text()
    fixture = fixture.replace('// NATIVE DECLARATIONS', declarations).replace('// PRODUCTION AVAILABILITY', interface)
    generated = args.output / 'native-election.cpp'
    generated.write_text(fixture)
    subprocess.run(['xcrun', 'clang++', '-arch', 'x86_64', '-std=c++14', '-O1', '-g',
        '-I', str(args.source.resolve()), '-idirafter', str(ROOT / 'itl80211/openbsd'),
        str(generated), str(path),
        '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    if result.returncode:
        raise RuntimeError('Native fixture failed: ' + str(result.returncode) + '\n' + result.stderr)
    report = dict(source=str(args.source.resolve()), nativeBuild='24H32', functions=manifest,
                  result=json.loads(result.stdout),
                  scope='Original x86 election predicates and selection. Synthetic object storage, time, locks, and driver-programming boundaries; not the complete macOS work queue or firmware.')
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
