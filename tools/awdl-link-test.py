#!/usr/bin/env python3
"""Run bounded synthetic UDP plus feedback over two real awdl0 interfaces."""

import argparse
import base64
import collections
import ctypes
import datetime
import ipaddress
import json
from pathlib import Path
import plistlib
import re
import runpy
import select
import socket
import struct
import subprocess
import threading
import time

HEADER = struct.Struct('!8sB7xQQQQ')
MAGIC = b'AWDLBT01'
HELLO, READY, DATA, REPORT, DONE = range(5)
PACKET_BYTES = 1200
SEND_TAIL_SECONDS = 4
RECEIVE_TAIL_SECONDS = 2


class Feedback:
    def __init__(self):
        self.times = []
        self.ages = []
        self.last_stamp = 0
        self.packets = self.bytes = 0

    def observe(self, elapsed, now_ns, stamp, count, byte_count):
        self.times.append(elapsed)
        self.packets = max(self.packets, count)
        self.bytes = max(self.bytes, byte_count)
        # Repeating the last report during the drain period is not a new RTT.
        if self.last_stamp < stamp <= now_ns:
            self.ages.append((now_ns - stamp) / 1_000_000)
            self.last_stamp = stamp

    def summary(self, duration):
        active = [0, *(value for value in self.times if value < duration), duration]
        ordered = sorted(self.ages)
        return dict(feedbackCount=len(self.times),
            maximumActiveFeedbackGapSeconds=max(b-a for a, b in zip(active, active[1:])),
            firstFeedbackSeconds=self.times[0] if self.times else None,
            freshFeedbackSamples=len(ordered),
            feedbackAgeP50MS=ordered[(len(ordered)-1)//2] if ordered else None,
            feedbackAgeP95MS=ordered[(len(ordered)-1)*95//100] if ordered else None)


class DriverSamples:
    """Read numeric driver counters without storing device or peer identities."""
    def __init__(self):
        self.stop = threading.Event()
        self.samples = []
        self.errors = []
        link = subprocess.check_output(['/sbin/ifconfig', 'awdl0'], text=True, timeout=5)
        own = re.search(r'\bether ([0-9a-f:]{17})', link).group(1)
        self.labels = {own: 'self'}
        self.started = time.monotonic()
        self.thread = threading.Thread(target=self.collect)
        self.thread.start()

    def collect(self):
        def walk(value):
            if isinstance(value, dict):
                yield value
                for item in value.values():
                    yield from walk(item)
            elif isinstance(value, list):
                for item in value:
                    yield from walk(item)
        while True:
            started = time.monotonic()
            sample = dict(elapsed=started-self.started, tx={}, rx={}, peers=[])
            try:
                for cls in ['AirportItlwm', 'AirportItlwmAWDLInterface']:
                    data = plistlib.loads(subprocess.check_output(
                        ['/usr/sbin/ioreg', '-a', '-r', '-c', cls], timeout=5))
                    for node in walk(data):
                        if 'APTimeCreditUS' in node:
                            sample['tx'] = {k: v for k, v in node.items()
                                if type(v) in (int, bool) and re.match(
                                    r'^(?:APTime|AWDL|DataTx|Dwell|Native|Radio|Sync|Output|Pending|Peer|BTCoex|Rate|NextGrant|InterfaceEnabled|Regulatory|Relay|ParentRecovery|Broadcast|ActionFrames|Tx|Rx)', k)}
                            sample['peers'] = self.peer_rows(node.get('NativePeerSummary', ''))
                        if isinstance(node.get('AWDLReceive'), dict):
                            sample['rx'] = {k: v for k, v in node['AWDLReceive'].items()
                                            if type(v) in (int, bool)}
                self.samples.append(sample)
            except Exception as error:
                self.errors.append(type(error).__name__)
            if self.stop.wait(max(0, 1-(time.monotonic()-started))):
                return

    def peer_rows(self, summary):
        def label(address):
            address = address.lower()
            if address not in self.labels:
                self.labels[address] = 'peer' + str(len(self.labels))
            return self.labels[address]
        rows = []
        mac = r'(?:[0-9a-f]{2}:){5}[0-9a-f]{2}'
        for line in summary.splitlines():
            cells = [value.strip() for value in line.split('|')[1:-1]]
            if len(cells) < 17 or not re.fullmatch(mac, cells[1], re.I):
                continue
            roots = re.findall(mac, cells[3], re.I)
            row = dict(peer=label(cells[1]), root=label(roots[0]) if roots else None)
            for key, index in [('distance', 4), ('selfMetric', 5), ('rootMetric', 6),
                               ('psf2G', 7), ('mif2G', 8), ('rssi2G', 11), ('lastAFMS', 14)]:
                row[key] = int(cells[index]) if re.fullmatch(r'-?\d+', cells[index]) else None
            rows.append(row)
        return rows

    def finish(self):
        self.stop.set()
        self.thread.join()
        return dict(samples=self.samples, errors=self.errors)


def interface_address():
    result = subprocess.run(['/sbin/ifconfig', 'awdl0'], capture_output=True,
                            text=True, check=True, timeout=5)
    match = re.search(r'\binet6 (fe80:[0-9a-f:]+)%awdl0\b', result.stdout)
    if not match:
        raise RuntimeError('awdl0 has no link-local IPv6 address')
    return match[1], socket.if_nametoindex('awdl0')


class Discovery:
    """Keep only this test's two services registered/resolving during the run."""
    def __init__(self, index, port, local_name, remote_name):
        self.lib = ctypes.CDLL('/usr/lib/libSystem.B.dylib')
        ref = ctypes.c_void_p
        u32, i32 = ctypes.c_uint32, ctypes.c_int32
        self.RegisterReply = ctypes.CFUNCTYPE(None, ref, u32, i32,
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ref)
        self.BrowseReply = ctypes.CFUNCTYPE(None, ref, u32, u32, i32,
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ref)
        self.ResolveReply = ctypes.CFUNCTYPE(None, ref, u32, u32, i32,
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint16,
            ctypes.c_uint16, ref, ref)
        self.lib.DNSServiceRegister.argtypes = [ctypes.POINTER(ref), u32, u32,
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
            ctypes.c_uint16, ctypes.c_uint16, ref, self.RegisterReply, ref]
        self.lib.DNSServiceBrowse.argtypes = [ctypes.POINTER(ref), u32, u32,
            ctypes.c_char_p, ctypes.c_char_p, self.BrowseReply, ref]
        self.lib.DNSServiceResolve.argtypes = [ctypes.POINTER(ref), u32, u32,
            ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, self.ResolveReply, ref]
        self.lib.DNSServiceRefSockFD.argtypes = [ref]
        self.lib.DNSServiceRefSockFD.restype = ctypes.c_int
        self.lib.DNSServiceProcessResult.argtypes = [ref]
        self.lib.DNSServiceRefDeallocate.argtypes = [ref]
        self.lib.DNSServiceRefDeallocate.restype = None
        self.refs = []
        self.errors = []
        self.registered = self.discovered = self.resolved = False
        self.interfaces = set()

        def register_reply(_ref, _flags, error, _name, _type, _domain, _context):
            if error:
                self.errors.append(error)
            else:
                self.registered = True

        def resolve_reply(_ref, _flags, interface, error, *_unused):
            if error:
                self.errors.append(error)
            else:
                self.resolved = True
                self.interfaces.add(interface)

        def browse_reply(_ref, flags, interface, error, name, kind, domain, _context):
            if error:
                self.errors.append(error)
                return
            if not flags & 2 or name != remote_name or self.discovered:
                return
            self.discovered = True
            self.interfaces.add(interface)
            value = ref()
            error = self.lib.DNSServiceResolve(ctypes.byref(value), 0x100000,
                index, name, kind, domain, self.on_resolve, None)
            self.add(value, error)

        self.on_register = self.RegisterReply(register_reply)
        self.on_browse = self.BrowseReply(browse_reply)
        self.on_resolve = self.ResolveReply(resolve_reply)
        try:
            value = ref()
            self.add(value, self.lib.DNSServiceRegister(ctypes.byref(value),
                0x100000, index, local_name, b'_awdlbench._udp', b'local.', None,
                socket.htons(port), 0, None, self.on_register, None))
            value = ref()
            self.add(value, self.lib.DNSServiceBrowse(ctypes.byref(value),
                0x100000, index, b'_awdlbench._udp', b'local.', self.on_browse, None))
        except Exception:
            self.close()
            raise

    def add(self, value, error):
        if error:
            self.errors.append(error)
        else:
            self.refs.append(value)

    def descriptors(self):
        return {self.lib.DNSServiceRefSockFD(ref): ref for ref in self.refs}

    def process(self, fd, refs):
        error = self.lib.DNSServiceProcessResult(refs[fd])
        if error:
            self.errors.append(error)

    def close(self):
        for ref in self.refs:
            self.lib.DNSServiceRefDeallocate(ref)
        self.refs.clear()


class Endpoint:
    def __init__(self, local=False):
        self.address, self.index = interface_address()
        self.sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        try:
            # XNU 11417.140.69 socket_private.h and the SDK in6.h.
            self.sock.setsockopt(socket.SOL_SOCKET, 0x1104, 1)  # SO_RECV_ANYIF
            self.sock.setsockopt(socket.IPPROTO_IPV6, 125, self.index)  # IPV6_BOUND_IF
            self.sock.setsockopt(socket.IPPROTO_IPV6, 61, 1)  # IPV6_RECVPKTINFO
            self.sock.bind((self.address, 0, 0, self.index))
            self.sock.setblocking(False)
            self.port = self.sock.getsockname()[1]
            self.discovery = Discovery(self.index, self.port,
                b'Continuity bench local' if local else b'Continuity bench target',
                b'Continuity bench target' if local else b'Continuity bench local')
        except Exception:
            self.sock.close()
            raise

    def close(self):
        self.discovery.close()
        self.sock.close()

    def run(self, peer_address, peer_port, sending, duration, mbps):
        expected_peer = ipaddress.IPv6Address(peer_address)
        destination = (peer_address, peer_port, 0, self.index) if peer_port else None
        start = time.monotonic()
        connected = None
        stop = start + 20
        next_hello = next_data = next_report = next_done = start
        interval = PACKET_BYTES * 8 / (mbps * 1_000_000)
        sent = received_bytes = sent_sequence = wrong_interface = unexpected = 0
        seen = set()
        errors = collections.Counter()
        receive_interfaces = collections.Counter()
        samples = collections.defaultdict(collections.Counter)
        feedback = Feedback()
        latest_stamp = latest_sequence = 0
        first_data = last_data = first_done = None
        receive_gaps = []
        expected_packets = None
        paced_skips = 0
        errors_by_kind = collections.defaultdict(collections.Counter)

        def send(kind, seq=0, stamp=0, count=0, byte_count=0):
            packet = HEADER.pack(MAGIC, kind, seq, stamp, count, byte_count)
            if kind == DATA:
                packet += bytes(PACKET_BYTES - HEADER.size)
            try:
                self.sock.sendto(packet, destination)
                return True
            except OSError as error:
                errors[str(error.errno)] += 1
                errors_by_kind[str(kind)][str(error.errno)] += 1
                return False

        try:
            while time.monotonic() < stop:
                now = time.monotonic()
                if connected is None and destination and now >= next_hello:
                    send(HELLO)
                    next_hello = now + 0.25
                if connected is not None:
                    second = int(now - connected)
                    if sending and now - connected < duration and now >= next_data:
                        stamp = time.monotonic_ns()
                        if send(DATA, sent_sequence, stamp):
                            sent += 1
                            samples[second]['sent'] += 1
                        sent_sequence += 1
                        next_data += interval
                        if next_data < now - interval * 4:
                            paced_skips += int((now - next_data) / interval)
                            next_data = now + interval
                    elif sending and now - connected >= duration and now >= next_done:
                        send(DONE, sent_sequence, 0, sent, sent * PACKET_BYTES)
                        next_done = now + 0.1
                    elif not sending and now >= next_report:
                        send(REPORT, latest_sequence, latest_stamp, len(seen), received_bytes)
                        next_report = now + 0.1
                refs = self.discovery.descriptors()
                deadlines = [stop, now + 0.05]
                if connected is None:
                    deadlines.append(next_hello if destination else now + 0.05)
                elif sending and now - connected < duration:
                    deadlines.append(next_data)
                elif sending:
                    deadlines.append(next_done)
                elif not sending:
                    deadlines.append(next_report)
                ready, _, _ = select.select([self.sock, *refs], [], [], max(0, min(deadlines) - now))
                for readable in ready:
                    if readable is not self.sock:
                        self.discovery.process(readable, refs)
                        continue
                    # A bounded batch keeps pacing and feedback alive under load.
                    for _ in range(64):
                        try:
                            packet, ancillary, flags, source = self.sock.recvmsg(2048, 256)
                        except BlockingIOError:
                            break
                        interface = None
                        for level, kind, value in ancillary:
                            if level == socket.IPPROTO_IPV6 and kind == 46 and len(value) == 20:
                                interface = struct.unpack_from('=I', value, 16)[0]
                        if interface != self.index:
                            wrong_interface += 1
                            continue
                        receive_interfaces[str(interface)] += 1
                        if ipaddress.IPv6Address(source[0].split('%')[0]) != expected_peer or \
                                len(packet) < HEADER.size or flags & socket.MSG_TRUNC:
                            unexpected += 1
                            continue
                        magic, kind, sequence, stamp, count, byte_count = HEADER.unpack_from(packet)
                        if magic != MAGIC or kind not in (HELLO, READY, DATA, REPORT, DONE) or \
                                len(packet) != (PACKET_BYTES if kind == DATA else HEADER.size):
                            unexpected += 1
                            continue
                        if destination is None:
                            if kind != HELLO:
                                unexpected += 1
                                continue
                            destination = (peer_address, source[1], 0, self.index)
                        if source[1] != destination[1]:
                            unexpected += 1
                            continue
                        now = time.monotonic()
                        if kind == HELLO:
                            send(READY)
                        if connected is None:
                            connected = now
                            next_data = next_report = now
                            stop = now + duration + (SEND_TAIL_SECONDS if sending else 10)
                        second = int(now - connected)
                        if kind == DATA and not sending and len(packet) == PACKET_BYTES:
                            if first_data is None:
                                first_data = now
                                if first_done is None:
                                    stop = now + duration + 10
                            if last_data is not None:
                                receive_gaps.append(now-last_data)
                            last_data = now
                            if stamp > latest_stamp:
                                latest_sequence, latest_stamp = sequence, stamp
                            if sequence not in seen:
                                seen.add(sequence)
                                received_bytes += len(packet)
                                samples[second]['received'] += 1
                        elif kind == REPORT and sending:
                            feedback.observe(now-connected, time.monotonic_ns(), stamp, count, byte_count)
                            samples[second]['feedback'] += 1
                        elif kind == DONE and not sending:
                            expected_packets = count
                            if first_done is None:
                                first_done = now
                                stop = now + RECEIVE_TAIL_SECONDS
            return dict(connected=connected is not None,
                connectionSeconds=None if connected is None else connected-start,
                elapsedSeconds=time.monotonic()-start,
                durationSeconds=duration, offeredMbps=mbps if sending else 0,
                sending=sending, packetBytes=PACKET_BYTES, sentPackets=sent,
                attemptedPackets=sent_sequence, pacedSkips=paced_skips,
                receivedPackets=len(seen), receivedBytes=received_bytes,
                peerReceivedPackets=feedback.packets, peerReceivedBytes=feedback.bytes,
                peerSentPackets=expected_packets, receivedDone=first_done is not None,
                goodputMbps=None if connected is None else
                    (feedback.bytes if sending else received_bytes)*8/duration/1_000_000,
                receiveSpanSeconds=None if first_data is None else last_data-first_data,
                maximumReceiveGapSeconds=max(receive_gaps, default=0),
                firstDataSeconds=None if first_data is None else first_data-connected,
                feedback=feedback.summary(duration) if sending else None,
                sendErrors=dict(errors), wrongInterfacePackets=wrong_interface,
                sendErrorsByKind={key: dict(value) for key, value in errors_by_kind.items()},
                unexpectedPackets=unexpected, boundInterface='awdl0', interfaceIndex=self.index,
                receivedInterfaceCounts=dict(receive_interfaces),
                socketSendBuffer=self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF),
                socketReceiveBuffer=self.sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF),
                discovery=dict(registered=self.discovery.registered,
                    discovered=self.discovery.discovered, resolved=self.discovery.resolved,
                    interfaces=sorted(self.discovery.interfaces), errors=self.discovery.errors),
                seconds={str(key): dict(value) for key, value in sorted(samples.items())})
        finally:
            self.close()


def remote_run(config):
    boot = subprocess.check_output(['/usr/sbin/sysctl', '-n', 'kern.bootsessionuuid'],
                                   text=True, timeout=5).strip()
    started_at = datetime.datetime.now().astimezone().isoformat()
    endpoint = Endpoint()
    sampler = DriverSamples()
    try:
        result = endpoint.run(config['address'], config['port'], config['sending'],
                              config['duration'], config['mbps'])
    finally:
        driver = sampler.finish()
    result.update(boot=boot, startedAt=started_at, driver=driver)
    print(json.dumps(result))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ssh-helper', type=Path, required=True)
    parser.add_argument('--direction', choices=['target-send', 'target-receive'], required=True)
    parser.add_argument('--mbps', type=float, default=0.2)
    parser.add_argument('--seconds', type=float, default=10)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 0.05 <= args.mbps <= 16 or not 1 <= args.seconds <= 20:
        parser.error('Use 0.05–16 Mbps and 1–20 seconds')
    run = runpy.run_path(str(args.ssh_helper))['run']
    lookup = "import subprocess,re,json; text=subprocess.check_output(['/sbin/ifconfig','awdl0'],text=True); match=re.search(r'\\binet6 (fe80:[0-9a-f:]+)%awdl0\\b',text); assert match; print(json.dumps(match[1]))"
    remote_address = json.loads(run(lookup))
    endpoint = Endpoint(local=True)
    config = dict(address=endpoint.address, port=endpoint.port,
                  sending=args.direction == 'target-send', duration=args.seconds, mbps=args.mbps)
    encoded = base64.b64encode(Path(__file__).read_bytes()).decode('ascii')
    remote_source = 'import base64\nns={"__name__":"awdl_bench"}\nexec(compile(base64.b64decode(' + \
        repr(encoded) + '),"awdl_bench","exec"),ns)\nns["remote_run"](' + repr(config) + ')'
    remote_result = {}

    def worker():
        try:
            remote_result['result'] = json.loads(run(remote_source))
        except Exception as error:
            # The helper's exception may contain SSH command text; retain only its type.
            remote_result['error'] = type(error).__name__

    thread = threading.Thread(target=worker)
    print('Starting bounded AWDL exchange', flush=True)
    thread.start()
    try:
        local = endpoint.run(remote_address, None, not config['sending'], args.seconds, args.mbps)
    finally:
        # The SSH helper bounds its invocation. Keep its terminal result in the
        # report instead of writing while the worker can still mutate it.
        thread.join()
    target = remote_result.get('result', {})
    sender = target if config['sending'] else local
    receiver = local if config['sending'] else target
    connected = bool(sender.get('connected') and receiver.get('connected'))
    complete = connected and receiver.get('receivedDone', False) and \
        sender.get('peerReceivedPackets') == receiver.get('receivedPackets')
    result = dict(direction=args.direction, local=local, target=remote_result,
                  targetWorkerFinished=not thread.is_alive(),
                  outcome='complete' if complete else 'incomplete' if connected else 'connection_failed',
                  deliveryFraction=(receiver['receivedPackets']/sender['sentPackets'])
                    if connected and sender.get('sentPackets') else None,
                  scope='Synthetic 1200-byte UDP datagrams and 100ms cumulative feedback; no media or user data. Both endpoints bound to awdl0, receive interface verified per packet. Goodput includes up to 2 seconds of drain after DONE. Feedback age includes receiver scheduling and the feedback interval; it is not a one-way delay or RTCP.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    compact = {**result, 'target': dict(remote_result)}
    if 'result' in compact['target']:
        compact['target']['result'] = {key: value for key, value in remote_result['result'].items() if key != 'driver'}
    print(json.dumps(compact, indent=2))


if __name__ == '__main__':
    main()
