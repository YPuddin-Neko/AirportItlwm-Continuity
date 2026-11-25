"""Run native MCC notification and AWDL plan invalidation through production routines."""

import re
import unittest

import test_regressions as support
from test_offline import definition
import awdl_native_sync


class AWDLRegulatoryRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_native_clm_restrictions_follow_hal_without_hiding_sta_channels(self):
        source = (support.ROOT / "tests/fixtures/wcl_regulatory.cpp").read_text()
        routines = definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::awdlChannelAllowed")
        routines += "\n" + "\n".join(definition("AirportItlwm/WCL/WCL.cpp",
            "AirportItlwmSkywalkInterface::" + name) for name in
            ("copyWCLChannels", "getWCL_CHANNELS_INFO"))
        self.compile_run(source.replace("// PRODUCTION", routines))

    def test_country_completion_requires_successful_current_channel_reload(self):
        source = (support.ROOT / "tests/fixtures/wcl_country.cpp").read_text()
        routines = "\n".join(definition("AirportItlwm/WCL/WCL.cpp",
            "AirportItlwmSkywalkInterface::" + name) for name in
            ("wclCountryUpdateGated", "getWCL_CHANNELS_INFO"))
        self.compile_run(source.replace("// PRODUCTION", routines))

    def test_regulatory_restriction_holds_whole_plan_until_complete_native_set(self):
        source = (support.ROOT / "tests/fixtures/awdl_tick.cpp").read_text().split("\nint main()", 1)[0]
        source = source.replace("// NATIVE DECLARATIONS", self.native_declarations())
        source = source.replace("    bool sequenceConfigured=false;", """
    bool nativeSequenceValid=false;
    apple80211_awdl_sync_channel_sequence nativeSequence={};
    IOReturn setSequence(void *);
    void setProperty(const char *, const char *) {}
    bool sequenceConfigured=false;""")
        source = source.replace("    bool allow149=true;", """
    struct Channel { unsigned value=44; } apChannel;
    struct Node { Channel *ni_chan=nullptr; } ap;
    struct IC { unsigned ic_state=0; Node *ic_bss=nullptr; } ic;
    IC *get80211Controller() {return &ic;}
    bool allow149=true;""")
        source = source.replace("struct AirportItlwmAWDLInterface:IO80211VirtualInterface", """
static unsigned ieee80211_chan2ieee(Radio::IC *, Radio::Channel *c) {return c->value;}
enum {APPLE80211_C_FLAG_ACTIVE=0x80,
      APPLE80211_C_FLAG_40MHZ=4,
      APPLE80211_C_FLAG_160MHZ=0x800,APPLE80211_C_FLAG_EXT_ABV=0x200};
struct AirportItlwmAWDLInterface:IO80211VirtualInterface""")
        names = ("drain", "releaseData", "peerGrantEnd", "recordNativeDataActivity", "tick", "pullOutputPackets", "setEnabled", "setInterfaceEnable",
            "updateAvailability", "queueUnicast", "serviceUnicast", "completeUnicast", "completeData",
            "configureBroadcast", "broadcastDue", "psfDue", "regulatoryMapChanged", "nextManagementToken", "receiveBlockAck", "serviceBlockAck", "serviceTXBlockAck")
        path = "AirportItlwm/AirportItlwmAWDLInterface.cpp"
        routines = awdl_native_sync.routines() + "\n" + "\n".join(
            definition(path, "AirportItlwmAWDLInterface::" + name) for name in names)
        # Execute the exact production SET case with the same native request layout.
        handle = definition(path, "AirportItlwmAWDLInterface::handle")
        start = handle.index("case Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE: {")
        end = handle.index("    case Request::", start + 5)
        routines += "\nIOReturn AirportItlwmAWDLInterface::setSequence(void *data) " + handle[
            handle.index("{", start):end]
        source = source.replace("// PRODUCTION", routines)
        source = "#include <cstdio>\n" + source
        source += (support.ROOT / "tests/fixtures/awdl_regulatory.cpp").read_text()
        self.compile_run(source)

    @staticmethod
    def native_declarations():
        headers = (support.ROOT / "include/Airport/apple80211_var.h").read_text() + "\n" + (
            support.ROOT / "include/Airport/apple80211_ioctl.h").read_text()
        result = [re.search(r"struct " + name + r"\s*\{.*?\}\s*(?:__attribute__\(\(packed\)\))?;",
            headers, re.S)[0] for name in ("apple80211_channel", "apple80211_awdl_sync_channel_sequence",
            "apple80211_availability_window_start", "apple80211_availability_window_extensions_end",
            "apple80211_awdl_oob_request")]
        result.append(awdl_native_sync.declaration())
        for name in ("APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START",
            "APPLE80211_M_AWDL_AVAILABILITY_WINDOW_EXTENSIONS_END", "APPLE80211_M_AWDL_UNICAST_AF_STATUS"):
            result.append(re.search(r"^#define " + name + r"\s+\d+", headers, re.M)[0])
        return "\n".join(result)


if __name__ == "__main__":
    unittest.main()
