"""Run production scan, connection and crypto routines with kernel boundary fakes."""

import re
import unittest
import test_regressions as support

ROOT = support.ROOT


def definition(path, name):
    source = (ROOT / path).read_text()
    pattern = re.escape(name).replace("::", r"::\s*") + r"\s*\([^;{}]*\)\s*(?:const\s*)?\{"
    match = re.search(pattern, source)
    if not match:
        raise ValueError("Missing definition: " + name)
    start = source.rfind("\n", 0, match.start()) + 1
    if not source[start:match.start()].strip():
        start = source.rfind("\n", 0, start - 1) + 1
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class OfflineRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def fixture(self, name, routines):
        source = (ROOT / "tests/fixtures" / name).read_text()
        for marker, path, names in routines:
            code = "\n\n".join(definition(path, name) for name in names)
            source = source.replace("// PRODUCTION: " + marker + "\n", code + "\n")
        if "// PRODUCTION: scan_done" in source:
            events = (ROOT / "AirportItlwm/WCL/WCL.cpp").read_text()
            start = events.index("        case IEEE80211_EVT_SCAN_DONE:")
            end = events.index("            break;", start)
            source = source.replace("// PRODUCTION: scan_done", events[events.index("\n", start)+1:end])
        self.compile_run(source)

    def test_native_scan_timing_and_filters(self):
        self.fixture("wcl_scan.cpp", [])

    def test_iwx_queue_response_wrap_failures_and_dma_lifetime(self):
        source = (ROOT / "tests/fixtures/iwx_queue_setup.cpp").read_text()
        header = (ROOT / "itlwm/hal_iwx/if_iwxreg.h").read_text()
        declarations = "\n".join(re.search(r"struct " + name + r"\s*\{.*?\}\s*__packed;",
            header, re.S)[0] for name in ("iwx_tx_queue_cfg_cmd", "iwx_tx_queue_cfg_rsp"))
        source = source.replace("// FIRMWARE DECLARATIONS", declarations)
        source = source.replace("// PRODUCTION", definition(
            "itlwm/hal_iwx/ItlIwx.cpp", "ItlIwx::iwx_tvqm_enable_txq"))
        self.compile_run(source)

    def test_card_capabilities_enable_native_dual_band_discovery(self):
        source = (ROOT / "tests/fixtures/card_capabilities.cpp").read_text()
        native = (ROOT / "include/Airport/apple80211_var.h").read_text()
        net = (ROOT / "itl80211/openbsd/net80211/ieee80211_var.h").read_text()
        declarations = [re.search(r"enum apple80211_card_capability\s*\{.*?\};", native, re.S)[0]]
        declarations += re.findall(r"^#define\s+IEEE80211_C_\w+[^\n]*", net, re.M)
        declarations.append("enum {" + ",".join(re.findall(
            r"IEEE80211_MODE_11[ABG]\s*=\s*\d+", net)) + "};")
        source = source.replace("// CAPABILITY DECLARATIONS", "\n".join(declarations))
        source = source.replace("// PRODUCTION", definition(
            "AirportItlwm/AirportItlwmV2.cpp", "AirportItlwm::getCARD_CAPABILITIES"))
        self.compile_run(source)

    def test_awdl_ifnet_allocation_preserves_primary_interface(self):
        self.fixture("ifnet_identity.cpp", [("identity",
            "AirportItlwm/AirportItlwmEthernetInterface.cpp",
            ["AirportItlwmEthernetInterface::initIfnetParams"])])

    def test_awdl_publication_waits_for_bsd_identity(self):
        self.fixture("interface_publication.cpp", [("bridge_init",
            "AirportItlwm/AirportItlwmEthernetInterface.cpp",
            ["AirportItlwmEthernetInterface::getNamePrefix",
             "AirportItlwmEthernetInterface::initWithSkywalkInterfaceAndProvider"]),
            ("start_awdl", "AirportItlwm/AirportItlwmAWDLInterface.cpp",
             ["AirportItlwm::startAWDLInterface"])])

    def test_awdl_bsd_ioctl_reaches_native_handler(self):
        self.fixture("awdl_bsd_ioctl.cpp", [("ioctl",
            "AirportItlwm/AirportItlwmEthernetInterface.cpp",
            ["AirportItlwmEthernetInterface::performCommand",
             "AirportItlwmEthernetInterface::syncAWDLFlagsGated",
             "AirportItlwmEthernetInterface::syncAWDLMulticastGated"])])

    def test_firmware_time_events_reply_notification_and_reset_order(self):
        self.fixture("iwx_time_event.cpp", [("events", "itlwm/hal_iwx/ItlIwx.cpp",
            ["ItlIwx::" + name for name in ("iwx_sync_time_event_flags",
             "iwx_rx_time_event_response", "iwx_rx_time_event_notification",
             "iwx_send_time_event_cmd", "iwx_protect_session", "iwx_unprotect_session",
             "iwx_schedule_protect_session", "iwx_cancel_session_protection",
             "iwx_rx_session_protection")])])

    def test_scan_cursor_survives_deleted_nodes(self):
        self.fixture("scan_cursor.cpp", [("cursor",
            "itl80211/openbsd/net80211/ieee80211_node.c",
            ["ieee80211_node_cmp", "ieee80211_scan_next"])])

    def test_scan_result_rates_and_ie_bounds(self):
        self.fixture("scan_result.cpp", [("convert",
            "AirportItlwm/AirportItlwmSkywalkInterface.cpp", ["convertNodeToScanResult"])])

    def test_join_scan_uses_hint_once_then_all_channels(self):
        self.fixture("join_scan.cpp", [("iwx", "itlwm/hal_iwx/ItlIwx.cpp",
            ["ItlIwx::iwx_umac_scan_fill_channels", "ItlIwx::iwx_scan"])])

    def test_vht_elements_only_on_5ghz(self):
        self.fixture("vht_frames.cpp", [("frames", "itl80211/openbsd/net80211/ieee80211_output.c",
            ["ieee80211_add_ssid", "ieee80211_add_rates", "ieee80211_add_xrates",
             "ieee80211_add_vhtcaps", "ieee80211_get_probe_req", "ieee80211_get_assoc_req"])])

    def test_userclient_selector_boundary(self):
        self.fixture("userclient.cpp", [("dispatch", "itlwm/ItlNetworkUserClient.cpp",
            ["ItlNetworkUserClient::externalMethod"])])

    def test_deferred_scan_lifecycle(self):
        self.fixture("wcl_lifecycle.cpp", [("scan",
            "AirportItlwm/WCL/WCL.cpp", ["AirportItlwmSkywalkInterface::" + name for name in
            ("beginWCLScan", "startWCLScan", "resumeWCLScan", "finishWCLScan",
             "completeWCLScan", "handleWCLAvailability", "resumeWCLJoin")])])

    def test_hw_crypto_replay_counters_and_buffer_ownership(self):
        self.fixture("hw_crypto.cpp", [
            ("keys", "itl80211/openbsd/net80211/ieee80211_crypto.c",
             ["ieee80211_get_txkey", "ieee80211_get_rxkey"]),
            ("pn", "itl80211/openbsd/net80211/ieee80211_crypto_ccmp.c", ["ieee80211_ccmp_get_pn"]),
            ("rx", "itl80211/openbsd/net80211/ieee80211_input.c", ["ieee80211_input_hwdecrypt", "ieee80211_input_mgmt_decrypt"]),
            ("iwx", "itlwm/hal_iwx/ItlIwx.cpp", ["ItlIwx::iwx_ccmp_decap"])])

    def test_early_eapol_and_run_link_state(self):
        path = "itl80211/openbsd/net80211/ieee80211_proto.c"
        source = (ROOT / path).read_text()
        start = source.index("#if defined(AIRPORT_WCL)")
        end = source.index("            ni->ni_fails = 0;", start)
        fixture = (ROOT / "tests/fixtures/link_state.cpp").read_text()
        fixture = fixture.replace("// PRODUCTION: link", definition(path, "ieee80211_set_link_state"))
        fixture = fixture.replace("// PRODUCTION: run", source[start:end])
        self.compile_run(fixture)


if __name__ == "__main__":
    unittest.main()
