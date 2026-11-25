"""Run the native timer and HAL cancellation/END lifecycle together."""

import gzip
import re
import struct
import unittest

import test_regressions as support
from test_offline import definition
from iwx_firmware import declarations
import awdl_native_sync


class AWDLDrainRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_full_tick_and_radio_lifecycle_after_cancel_or_natural_end(self):
        root = support.ROOT
        source = (root / "tests/fixtures/awdl_tick.cpp").read_text().split("\n// PRODUCTION", 1)[0]
        radio = (root / "tests/fixtures/awdl_drain.cpp").read_text()
        capture = gzip.decompress((root / "tests/fixtures/captures/receive-ios26.x-mt7925e-ch44-1.pcap.gz").read_bytes())
        offset = 24
        while offset < len(capture):
            length = struct.unpack_from("<I", capture, offset + 8)[0]
            packet = capture[offset + 16:offset + 16 + length]
            offset += 16 + length
            frame = packet[struct.unpack_from("<H", packet, 2)[0]:]
            if frame[0] == 0x88 and frame[10:16] == bytes.fromhex("020000000001"):
                break
        else:
            self.fail("Missing iPhone-origin QoS data in receive capture")
        radio = radio.replace("/* CAPTURED IPHONE DATA */", ",".join(map(str, frame)))
        begin, end = source.index("struct Radio {"), source.index("struct AirportItlwmAWDLInterface:")
        source = source[:begin] + radio.split("// SCENARIOS", 1)[0] + source[end:]
        source = source.replace(",IWX_INVALID_QUEUE=0xffff", "")
        source = source.replace("struct Gate {", "struct Gate {\n    void commandWakeup(void *) {}")
        native = "AirportItlwm/AirportItlwmAWDLInterface.cpp"
        source += "\n" + awdl_native_sync.routines() + "\n" + "\n".join(definition(native, "AirportItlwmAWDLInterface::" + name)
            for name in ("drain", "releaseData", "peerGrantEnd", "recordNativeDataActivity", "tick", "pullOutputPackets", "setEnabled", "setInterfaceEnable",
                         "updateAvailability", "queueUnicast", "serviceUnicast", "completeUnicast", "completeData",
                         "configureBroadcast", "broadcastDue", "psfDue", "nextManagementToken", "receiveBlockAck", "serviceBlockAck", "serviceTXBlockAck"))
        source += "\n" + "\n".join(definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::" + name)
            .replace("ItlIwx::", "Radio::").replace("static_cast<ItlIwx *>", "static_cast<Radio *>")
            for name in ("receiveAWDLSession", "finishAWDLDwell", "serviceAWDLChannel", "awdlRemoveQueue", "stopAWDLRadio"))
        source += "\n" + radio.split("// SCENARIOS", 1)[1]
        source = source.replace("// FIRMWARE DECLARATIONS", declarations(
            ("iwx_add_sta_cmd", "iwx_rm_sta_cmd", "iwx_session_prot_cmd", "iwx_session_prot_notif"), source))
        headers = (root / "include/Airport/apple80211_var.h").read_text() + "\n" + (
            root / "include/Airport/apple80211_ioctl.h").read_text()
        native_declarations = [re.search(r"struct " + name + r"\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;",
            headers, re.S)[0] for name in ("apple80211_channel", "apple80211_availability_window_start",
            "apple80211_availability_window_extensions_end", "apple80211_awdl_oob_request")]
        native_declarations.append(awdl_native_sync.declaration())
        for name in ("APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START",
                     "APPLE80211_M_AWDL_AVAILABILITY_WINDOW_EXTENSIONS_END", "APPLE80211_M_AWDL_UNICAST_AF_STATUS"):
            native_declarations.append(re.search(r"^#define " + name + r"\s+\d+", headers, re.M)[0])
        source = source.replace("// NATIVE DECLARATIONS", "\n".join(native_declarations))
        self.compile_run(source)


if __name__ == "__main__":
    unittest.main()
