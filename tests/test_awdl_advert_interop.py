"""Inspect actual TX bytes independently of AirportAWDL's parsers.

Wire offsets come from seemoo-lab/owl 0ee3263f/src/frame.h and the pinned
Owly capture corpus. Public iPhone captures constrain the independent decoder;
the producer executes the production native template handler and SyncEngine.
This does not execute macOS's peer manager or demonstrate RF interoperability.
"""

import collections
import gzip
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

import test_awdl
import test_regressions as support


def tlvs(packet):
    if len(packet) < 40 or packet[:2] != b"\xd0\x00" or packet[24:30] != bytes.fromhex("7f0017f20810"):
        raise ValueError("not an AWDL action frame")
    at = 40
    result = []
    while at < len(packet):
        if len(packet) - at < 3:
            if any(packet[at:]):
                raise ValueError("truncated TLV header")
            break
        kind, length = struct.unpack_from("<BH", packet, at)
        end = at + 3 + length
        if end > len(packet):
            raise ValueError("truncated TLV value")
        result.append((kind, packet[at + 3:end]))
        at = end
    return result


def standalone_channels(fields):
    # Owl RX and Owly Advert::parse take peer sequences from TLV18, not TLV4.
    values = [value for kind, value in fields if kind == 18]
    if len(values) != 1:
        raise ValueError("expected exactly one standalone channel sequence")
    value = values[0]
    if len(value) != 41 or value[:6] != bytes.fromhex("0f030003ffff"):
        raise ValueError("unsupported channel sequence layout")
    return list(value[6:38:2])


def captures():
    for path in sorted((support.ROOT / "tests/fixtures/captures").glob("*.pcap.gz")):
        data = gzip.decompress(path.read_bytes())
        if data[:24] != bytes.fromhex("d4c3b2a1020004000000000000000000ffff00007f000000"):
            raise ValueError("unexpected capture format")
        at = 24
        while at < len(data):
            _, _, length, original = struct.unpack_from("<IIII", data, at)
            at += 16
            packet = data[at:at + length]
            at += length
            if length != original or len(packet) != length:
                raise ValueError("truncated capture")
            raw = packet[struct.unpack_from("<H", packet, 2)[0]:]
            if len(raw) >= 40 and raw[:2] == b"\xd0\x00" and raw[24:30] == bytes.fromhex("7f0017f20810"):
                yield raw


class AdvertInteroperability(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="awdl-advert-interop-")
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        # Reuse declarations/mocks; run the entire production native handler.
        collected = []
        collector = type("Collector", (), {"compile_run": lambda self, source: collected.append(source)})()
        test_awdl.AWDLRegressions.run_control_case(collector, 0)
        source = collected[0].split("static void syncParameters()", 1)[0]
        source += (support.ROOT / "tests/fixtures/awdl_advert_interop.cpp").read_text()
        source_path = directory / "producer.cpp"
        source_path.write_text(source)
        cls.producer = directory / "producer"
        subprocess.run(["xcrun", "clang++", "-std=c++14", "-O1", "-g",
                        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                        "-I", str(support.ROOT), "-idirafter", str(support.ROOT / "itl80211/openbsd"),
                        str(source_path), "-o", str(cls.producer)], check=True)
        cls.capture = next(raw for raw in captures()
                           if raw[15] == 2 and raw[30] == 0 and
                           any(kind == 6 and len(value) == 13 for kind, value in tlvs(raw)))
        # Build a native-style template from real captured TLVs. Change only
        # election identity to the fixture interface; preserve service/DP/version.
        cls.native_fields = []
        for kind, value in tlvs(cls.capture):
            if kind in (4, 5):
                continue
            if kind == 24:
                value = bytearray(value)
                value[:12] = bytes.fromhex("020000000001") * 2
                value[16:20] = bytes(4)
                value = bytes(value)
            cls.native_fields.append((kind, value))
        cls.payload = directory / "native-template.bin"
        cls.payload.write_bytes(b"".join(struct.pack("<BH", kind, len(value)) + value
                                        for kind, value in cls.native_fields))

    def produce(self, mode, now=None):
        command = [str(self.producer), mode, str(self.payload)]
        if now is not None:
            command.append(str(now))
        return subprocess.run(command,
                              check=True, capture_output=True, timeout=20).stdout

    def test_capture_local_remaining_aw_matches_advertised_countdown(self):
        count = positive = 0
        for raw in captures():
            fields = tlvs(raw)
            service = next(value for kind, value in fields if kind == 6)
            # The manifests identify the local Linux sender; it has the
            # 9-byte zero service TLV. iPhone advertises a different policy.
            if raw[30] != 0 or service != bytes(9):
                continue
            sync = next(value for kind, value in fields if kind == 4)
            countdown = struct.unpack_from("<H", sync, 1)[0]
            period = struct.unpack_from("<H", sync, 5)[0]
            common = struct.unpack_from("<H", sync, 13)[0]
            remaining = struct.unpack_from("<H", sync, 15)[0]
            self.assertEqual(common, period)
            self.assertEqual(remaining, max(0, common - (period * sync[27] - countdown)))
            count += 1
            positive += remaining != 0
        self.assertEqual((count, positive), (1263, 315))

    def test_remaining_aw_shares_countdown_tu_rounding_and_window_boundaries(self):
        # TU values are explicit at both sides of the first/common AW edge,
        # in extended AWs, in the final TU and at the next EAW start.
        cases = ((0, 64, 16), (1, 64, 16), (1023, 64, 16), (1024, 63, 15),
                 (15359, 50, 2), (15360, 49, 1), (16383, 49, 1), (16384, 48, 0),
                 (16385, 48, 0), (32768, 32, 0), (64512, 1, 0), (65535, 1, 0),
                 (65536, 64, 16), (65537, 64, 16), (81920, 48, 0))
        for mode in ("psf", "mif", "native-mif", "umi"):
            for elapsed, countdown, remaining in cases:
                with self.subTest(mode=mode, elapsed=elapsed):
                    raw = self.produce(mode, 1000000 + elapsed)
                    sync = next(value for kind, value in tlvs(raw) if kind == 4)
                    self.assertEqual(struct.unpack_from("<H", sync, 1)[0], countdown)
                    self.assertEqual(struct.unpack_from("<H", sync, 15)[0], remaining)
                    self.assertEqual(struct.unpack_from("<H", sync, 29)[0], elapsed // 16384)
                    self.assertEqual(struct.unpack_from("<I", raw, 32)[0], 1000000 + elapsed)

    def test_capture_oracle_has_one_sequence_and_one_service(self):
        count = 0
        service_ids = set()
        for raw in captures():
            fields = tlvs(raw)
            self.assertEqual(len(standalone_channels(fields)), 16)
            service = [value for kind, value in fields if kind == 6]
            self.assertEqual(len(service), 1)
            service_ids.add(struct.unpack_from("<H", service[0], 3)[0])
            count += 1
        self.assertEqual(count, 4893)
        self.assertEqual(service_ids, {0, 215, 218})

    def test_psf_and_automatic_mif_preserve_single_native_service(self):
        expected = [value for kind, value in self.native_fields if kind == 6]
        self.assertEqual(len(expected), 1)
        self.assertEqual(struct.unpack_from("<H", expected[0], 3)[0], 215)
        for mode in ("psf", "mif", "native-mif", "umi"):
            with self.subTest(mode=mode):
                fields = tlvs(self.produce(mode))
                self.assertEqual([value for kind, value in fields if kind == 6], expected)
                for kind in (7, 12, 17, 21, 24):
                    self.assertEqual([v for k, v in fields if k == kind],
                                     [v for k, v in self.native_fields if k == kind])

    def test_all_frame_modes_publish_the_current_schedule_once(self):
        schedule = [44, 0, 0, 149, 0, 44, 0, 149, 6, 0, 44, 149, 0, 44, 0, 149]
        for mode in ("psf", "mif", "native-mif", "umi", "fallback-psf", "fallback-mif"):
            with self.subTest(mode=mode):
                raw = self.produce(mode)
                fields = tlvs(raw)
                self.assertEqual(standalone_channels(fields), schedule)
                counts = collections.Counter(kind for kind, _ in fields)
                for kind in (4, 5, 18, 21, 24):
                    self.assertEqual(counts[kind], 1)
                sync = next(v for k, v in fields if k == 4)
                sequence = next(v for k, v in fields if k == 18)
                self.assertEqual(sequence[:38], sync[33:71])
                self.assertEqual(sequence[38:], bytes(3))
                self.assertEqual(sync[71:], bytes(2))
                self.assertEqual(raw[10:16], bytes.fromhex("020000000001"))
                self.assertEqual(raw[4:10], bytes.fromhex("020000000002") if mode == "umi" else b"\xff" * 6)
                self.assertEqual(raw[16:22], bytes.fromhex("002500ff9473"))
                self.assertEqual(raw[30], 0 if mode in ("psf", "fallback-psf") else 3)

    def test_fallback_service_and_awdl_address_are_single_and_consistent(self):
        for mode in ("fallback-psf", "fallback-mif"):
            with self.subTest(mode=mode):
                raw = self.produce(mode)
                fields = tlvs(raw)
                self.assertEqual([v for k, v in fields if k == 6], [bytes(9)])
                data_path = [v for k, v in fields if k == 12]
                self.assertEqual(len(data_path), 1)
                self.assertEqual(struct.unpack_from("<H", data_path[0])[0], 0x8f24)
                self.assertEqual(data_path[0][7:13], raw[10:16])


if __name__ == "__main__":
    unittest.main()
