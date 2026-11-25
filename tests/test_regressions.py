#!/usr/bin/env python3
"""Exercise production bit/channel helpers with memory and UB sanitizers."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(path, name):
    text = (ROOT / path).read_text()
    match = re.search(r"(?:static (?:inline )?)?(?:int|void)\s+" + name + r"\s*\(", text)
    if not match:
        raise ValueError("Missing production function: " + name)
    start = text.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


class Regressions(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory(prefix="airport-regression-") as directory:
            source_path = Path(directory) / "test.cpp"
            binary = Path(directory) / "test"
            source_path.write_text(source)
            subprocess.run(["xcrun", "clang++", "-std=c++14", "-O1", "-g",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-I", str(ROOT),
                            "-idirafter", str(ROOT / "itl80211/openbsd"),
                            str(source_path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_rate_bitmap_bounds_and_values(self):
        helper = function("itl80211/linux/bitfield.h", "find_last_bit")
        self.compile_run("""
#include <cassert>
#include <cstdint>
#include <cstdlib>
typedef unsigned int u_int;
""" + helper + """
int main() {
    assert(find_last_bit(nullptr, 0) == 0);
    uint64_t rate_mask = 0xff0;
    assert(find_last_bit(&rate_mask, 64) == 11);
    rate_mask = 0x7fd0;
    assert(find_last_bit(&rate_mask, 64) == 14);
    rate_mask = uint64_t(1) << 63;
    assert(find_last_bit(&rate_mask, 64) == 63);
    rate_mask = 1;
    assert(find_last_bit(&rate_mask, 64) == 0);
    uint32_t seed = 1234567;
    for (int count = 1; count <= 128; ++count) {
        auto words = static_cast<uint32_t *>(calloc((count + 31) / 32, 4));
        assert(words);
        assert(find_last_bit(words, count) == count);
        for (int trial = 0; trial < 100; ++trial) {
            for (int word = 0; word < (count + 31) / 32; ++word) {
                seed = seed * 1664525U + 1013904223U;
                words[word] = seed;
            }
            int expected = count;
            for (int bit = 0; bit < count; ++bit)
                if (words[bit / 32] & (1U << (bit % 32))) expected = bit;
            assert(find_last_bit(words, count) == expected);
        }
        free(words);
    }
}
""")

    def test_vht_hardware_limits_and_channel_geometry(self):
        path = "itl80211/openbsd/net80211/ieee80211_proto.c"
        helpers = function(path, "ieee80211_vht_chandef_usable") + "\n"
        helpers += function(path, "ieee80211_vht_limit_width")
        self.compile_run("""
#include <cassert>
#include <cstdint>
typedef unsigned int u_int;
enum { IEEE80211_CHAN_WIDTH_20, IEEE80211_CHAN_WIDTH_80,
       IEEE80211_CHAN_WIDTH_160, IEEE80211_CHAN_WIDTH_80P80 };
enum { IEEE80211_CHAN_MAX = 255,
       IEEE80211_VHTCAP_SUPP_CHAN_WIDTH_MASK = 12,
       IEEE80211_VHTCAP_SUPP_CHAN_WIDTH_MASK_S = 2 };
struct ieee80211_channel { int ic_freq, ic_flags, ic_center_freq1, ic_center_freq2; };
struct ieee80211com { unsigned ic_vhtcaps; ieee80211_channel ic_channels[256]; };
struct ieee80211_node { int ni_chw; ieee80211_channel *ni_chan; };
static unsigned ieee80211_mhz2ieee(int freq, int) { return (freq - 5000) / 5; }
static void ieee80211_ht_negotiate_chw(ieee80211com *, ieee80211_node *n) {
    n->ni_chw = IEEE80211_CHAN_WIDTH_20;
    n->ni_chan->ic_center_freq1 = n->ni_chan->ic_freq;
}
""" + helpers + """
static ieee80211com hardware(unsigned width) {
    ieee80211com ic = {};
    ic.ic_vhtcaps = width << 2;
    for (int channel = 36; channel <= 64; channel += 4)
        ic.ic_channels[channel] = {5000 + 5 * channel, 1, 0, 0};
    return ic;
}
static void check(unsigned own, int primary, int width, int center, int secondary,
                  int expectedWidth, int expectedCenter, int expectedSecondary) {
    auto ic = hardware(own);
    auto c = ic.ic_channels[primary];
    c.ic_center_freq1 = 5000 + 5 * center;
    c.ic_center_freq2 = secondary ? 5000 + 5 * secondary : 0;
    ieee80211_node n = {width, &c};
    ieee80211_vht_limit_width(&ic, &n);
    assert(n.ni_chw == expectedWidth);
    assert(c.ic_center_freq1 == 5000 + 5 * expectedCenter);
    assert(c.ic_center_freq2 == (expectedSecondary ? 5000 + 5 * expectedSecondary : 0));
}
int main() {
    // Both halves of a 160 MHz AP must work with an 80 MHz adapter.
    check(0, 36, IEEE80211_CHAN_WIDTH_160, 50, 0, IEEE80211_CHAN_WIDTH_80, 42, 0);
    check(0, 60, IEEE80211_CHAN_WIDTH_160, 50, 0, IEEE80211_CHAN_WIDTH_80, 58, 0);
    // Preserve a valid 160 MHz setup on capable hardware.
    check(1, 36, IEEE80211_CHAN_WIDTH_160, 50, 0, IEEE80211_CHAN_WIDTH_160, 50, 0);
    check(2, 60, IEEE80211_CHAN_WIDTH_160, 50, 0, IEEE80211_CHAN_WIDTH_160, 50, 0);
    // Non-contiguous 80+80 must never become an unrelated 160 MHz block.
    check(0, 36, IEEE80211_CHAN_WIDTH_80P80, 42, 106, IEEE80211_CHAN_WIDTH_80, 42, 0);
    check(1, 36, IEEE80211_CHAN_WIDTH_80P80, 42, 106, IEEE80211_CHAN_WIDTH_80, 42, 0);
    // AP advertises channels 52..80; only 52..64 is usable locally.
    check(1, 52, IEEE80211_CHAN_WIDTH_160, 66, 0, IEEE80211_CHAN_WIDTH_80, 58, 0);
    // Neither the advertised wide channel nor its primary segment is usable.
    check(1, 52, IEEE80211_CHAN_WIDTH_80, 66, 0, IEEE80211_CHAN_WIDTH_20, 52, 0);
    auto ic = hardware(1);
    auto c = ic.ic_channels[36];
    assert(!ieee80211_vht_chandef_usable(&ic, &c, 5215, 80));
    assert(!ieee80211_vht_chandef_usable(&ic, &c, 0, 80));
    assert(ieee80211_vht_chandef_usable(&ic, &c, 5210, 80));
    ic.ic_channels[44].ic_flags = 0;
    assert(!ieee80211_vht_chandef_usable(&ic, &c, 5210, 80));
}
""")


if __name__ == "__main__":
    unittest.main()
