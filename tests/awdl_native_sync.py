"""Production GET117 and sync-notification functions for the timer fixtures."""

import re

import test_regressions as support
from test_offline import definition


def declaration():
    headers = (support.ROOT / "include/Airport/apple80211_ioctl.h").read_text()
    return re.search(r"struct apple80211_awdl_sync_enabled\s*\{.*?\}\s*__attribute__\(\(packed\)\);",
                     headers, re.S)[0]


def routines():
    path = "AirportItlwm/AirportItlwmAWDLInterface.cpp"
    source = "\n".join(definition(path, "AirportItlwmAWDLInterface::" + name)
                       for name in ("getAWDL_SYNC_ENABLED", "request", "publishNativeSyncState"))
    handle = definition(path, "AirportItlwmAWDLInterface::handle")
    start = handle.index("    case Request::getAWDL_SYNC_ENABLED:")
    end = handle.index("    case Request::", start + 5)
    # Keep the production prelude and the complete GET case; other ioctl cases
    # are independently exercised by the control fixture.
    return source + "\n" + handle[:handle.index("    case Request::")] + handle[start:end] + """
    default: return kIOReturnUnsupported;
    }
}
"""
