"""Exercise native capability responses with the production 802.11 IE writers."""

import re
import unittest

import test_regressions as support
from test_offline import definition


class PHYCapabilityRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_native_ht_vht_responses_preserve_real_mcs_and_wire_layout(self):
        native = (support.ROOT / "include/Airport/apple80211_ioctl.h").read_text()
        ieee = (support.ROOT / "itl80211/openbsd/net80211/ieee80211.h").read_text()
        var = (support.ROOT / "itl80211/openbsd/net80211/ieee80211_var.h").read_text()
        declarations = []
        for name, source in (("apple80211_ht_capability", native),
                             ("apple80211_vht_capability", native),
                             ("ieee80211_ie_htcap", ieee),
                             ("ieee80211_vht_mcs_info", ieee),
                             ("ieee80211_ie_vhtcap", ieee)):
            declarations.append(re.search(r"struct " + name +
                r"\s*\{.*?\}\s*(?:__packed|__attribute__\(\(packed\)\));", source, re.S)[0])
        declarations.append("enum {" + ",".join(re.findall(
            r"IEEE80211_MODE_11(?:N|AC)\s*=\s*\d+", var)) + "};")
        declarations.append("enum {" + ",".join(re.search(name + r"\s*=\s*\d+", ieee)[0]
            for name in ("IEEE80211_ELEMID_HTCAPS", "IEEE80211_ELEMID_VHT_CAP")) + "};")
        declarations.append(re.search(r"^#define\s+IEEE80211_MCS_RX_RATE_HIGH\s+[^\n]+",
            ieee, re.M)[0])
        routines = "\n".join(definition("itl80211/openbsd/net80211/ieee80211_output.c", name)
            for name in ("ieee80211_add_htcaps", "ieee80211_add_vhtcaps"))
        routines += "\n" + "\n".join(definition(
            "AirportItlwm/AirportItlwmSkywalkInterface.cpp", "AirportItlwmSkywalkInterface::" + name)
            for name in ("getHT_CAPABILITY", "getVHT_CAPABILITY"))
        source = (support.ROOT / "tests/fixtures/phy_capabilities.cpp").read_text()
        self.compile_run(source.replace("// DECLARATIONS", "\n".join(declarations)).replace(
            "// PRODUCTION", routines))


if __name__ == "__main__":
    unittest.main()
