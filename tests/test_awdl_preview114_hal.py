"""Permanent QuZ-68 HAL firmware-safety invariants (post-Preview 114 post-mortem):
1. MacID 1 (AWDL LISTENER + TVQM Station 3 / Stations 4..7) MUST NEVER touch sc_phyctxt[0] or
   Binding 0 (IWX_BINDING_CONTEXT_CMD ACTION_MODIFY sharing Binding 0 with MacID 0 BSS_STA
   returns synchronous status == 0 on QuZ-68.ucode and then triggers an asynchronous UMAC
   SYSASSERT -> awdlChipReset -> en1 linkState:DOWN). Every AWDL binding and PHY command MUST
   target exclusively Binding 1 / PHY 1 even when serviceChannel == infraNum (Ch 48).
2. RadioState::wide80OverlapsInfra(48, 48, true) (and 44/52/64 under 160 MHz AP) clamps PHY 1
   on Ch 48 to 20 MHz while keeping disjoint Ch 149 at 80 MHz.
3. Every 80 -> 20 MHz shrink on PHY 1 MUST strictly execute:
     downgradeAWDLTXRates(20) -> configureAWDLPeerWidth(20) [STA FAT_EN=20] -> awdlPHY(..., 20)
   on BOTH Ch 48 (5 GHz VHT 0x01ff) and Ch 6 (2.4 GHz HT 0x00ff isolated onto non-shared
   Antenna B chains == 2, NSS2 == 0), preserving both 0x0034 and 0x20A2 firmware invariants.
"""
import unittest

import test_awdl_preview91 as p91
import test_regressions as support
from iwx_firmware import declarations as firmware_declarations
from test_offline import definition


def _build_p114_hal_safety_harness(extra_cpp: str) -> str:
    src = p91._build_radio_harness("\n// P114_SAFETY_EXTRA\n")
    src = src.replace(
        "struct Phy {int id=0,color=0;Channel *channel=nullptr;};",
        "struct iwx_phy_ctxt {int id=0,color=0;Channel *channel=nullptr;};\nusing Phy = iwx_phy_ctxt;",
    )
    extra_routines = "\n".join(
        definition("itlwm/hal_iwx/ItlIwxAWDL.cpp", "ItlIwx::" + fn)
        for fn in ("configureAWDLTXRates", "downgradeAWDLTXRates", "setAWDLTXWindow", "addAWDLBlockAckPeer")
    )
    tlc_decls = firmware_declarations(
        ("iwx_tlc_config_cmd_v4", "iwl_tlc_config_cmd_v3"),
        extra_routines,
        ("iwx_tlc_mng_cfg_mode", "iwx_tlc_mng_cfg_cw"),
    )
    src = src.replace("struct ItlIwx {", tlc_decls + "\nstruct ItlIwx {")
    old_downgrade = (
        "    int downgradeAWDLTXRates(uint8_t width, uint16_t = 0) {\n"
        "        if (awdlRadio.blockAckTX.configured && awdlRadio.blockAckTX.widthMHz > width)\n"
        "            awdlRadio.blockAckTX.widthMHz = width;\n"
        "        return 0;\n"
        "    }"
    )
    assert old_downgrade in src
    new_members = """    int configureAWDLTXRates();
    int downgradeAWDLTXRates(uint8_t, uint16_t = 0);
    int setAWDLTXWindow(uint64_t (*)(const void *, uint64_t), const void *);
    int addAWDLBlockAckPeer(unsigned, const uint8_t *);
    unsigned tlcVersion = 4;
    std::vector<std::pair<struct ::iwx_binding_cmd, size_t>> bindingLog;
    std::vector<iwx_add_sta_cmd> staLog;
    std::vector<iwx_tlc_config_cmd_v4> tlcLog;
    std::vector<uint32_t> orderLog;
    unsigned sc_rx_ba_sessions = 0;"""
    src = src.replace(old_downgrade, new_members)
    src = src.replace(
        "if(g==IWX_DATA_PATH_GROUP){assert(c==IWX_RLC_CONFIG_CMD);return rlcVersion;}",
        "if(g==IWX_DATA_PATH_GROUP){if(c==IWX_TLC_MNG_CONFIG_CMD)return tlcVersion;assert(c==IWX_RLC_CONFIG_CMD);return rlcVersion;}",
    )
    # Record PHY width commands in orderLog (tag 3) and assert PHY 0 is NEVER touched by AWDL.
    src = src.replace(
        "phyCommands.push_back(*c);",
        "assert(c->id_and_color == IWX_FW_CMD_ID_AND_COLOR(1, com.sc_phyctxt[1].color));"
        "phyCommands.push_back(*c);"
        "orderLog.push_back((3U << 16) | uint32_t(c->ci.width));",
    )
    # Capture IWX_TLC_MNG_CONFIG_CMD in orderLog (tag 1).
    src = src.replace(
        'assert(id==IWX_REMOVE_STA && static_cast<const iwx_rm_sta_cmd*>(data)->sta_id==3);',
        'if(id==iwx_cmd_id(IWX_TLC_MNG_CONFIG_CMD,IWX_DATA_PATH_GROUP,0)) {\n'
        '            assert(size==sizeof(iwx_tlc_config_cmd_v4));\n'
        '            const auto &cfg = *static_cast<const iwx_tlc_config_cmd_v4*>(data);\n'
        '            tlcLog.push_back(cfg);\n'
        '            orderLog.push_back((1U << 16) | uint32_t(cfg.max_ch_width));\n'
        '            return command("tlc-config");\n'
        '        }\n'
        '        assert(id==IWX_REMOVE_STA && static_cast<const iwx_rm_sta_cmd*>(data)->sta_id==3);',
    )
    # Strictly assert every AWDL IWX_BINDING_CONTEXT_CMD targets ONLY Binding 1 / PHY 1 (never Binding 0 / PHY 0)
    # and record peer STA FAT_EN modifications in orderLog (tag 2).
    old_status_fn = (
        '        if(id==IWX_BINDING_CONTEXT_CMD) {\n'
        '            auto c=static_cast<const iwx_binding_cmd*>(data);\n'
        '            assert(size == (isset(com.sc_enabled_capa,39) ? 28 : 24));\n'
        '            assert(c->phy==1 && c->id_and_color==1 && c->macs[0]==(c->action==1?1:0xffffffffU) &&\n'
        '                c->macs[1]==0xffffffffU && c->macs[2]==0xffffffffU);\n'
        '            assert(c->action==1||c->action==3);*status=c->action==1?bindingStatus:0;\n'
        '            return command(c->action==1?"bind":"unbind");\n'
        '        }\n'
        '        assert(id==IWX_ADD_STA);auto c=static_cast<const iwx_add_sta_cmd*>(data);\n'
        '        assert(size==sizeof(*c) && size==48);\n'
        '        assert(c->sta_id==3 && c->station_type==IWX_STA_GENERAL_PURPOSE && c->mac_id_n_color==1 && c->tid_disable_tx==0xffff);\n'
        '        for(auto b:c->addr)assert(b==255);\n'
        '        *status=stationStatus;int error=command("add-station");\n'
        '        if(!error && stationStatus==IWX_ADD_STA_SUCCESS)stationPresent=true;return error;'
    )
    assert old_status_fn in src
    new_status_fn = """        if(id==IWX_BINDING_CONTEXT_CMD) {
            auto c=static_cast<const struct ::iwx_binding_cmd*>(data);
            bindingLog.push_back({*c, size});
            assert(size == (isset(com.sc_enabled_capa,39) ? 28 : 24));
            // Permanent QuZ-68 safety invariant: AWDL MacID 1 MUST NEVER touch Binding 0 / PHY 0!
            assert(c->phy == 1 && c->id_and_color == 1);
            assert(c->action == IWX_FW_CTXT_ACTION_ADD || c->action == IWX_FW_CTXT_ACTION_REMOVE);
            assert(c->macs[0] == (c->action == IWX_FW_CTXT_ACTION_ADD ? 1U : 0xffffffffU));
            assert(c->macs[1] == 0xffffffffU && c->macs[2] == 0xffffffffU);
            *status = c->action == IWX_FW_CTXT_ACTION_ADD ? bindingStatus : 0;
            return command(c->action == IWX_FW_CTXT_ACTION_ADD ? "bind" : "unbind");
        }
        assert(id==IWX_ADD_STA);auto c=static_cast<const iwx_add_sta_cmd*>(data);
        assert(size==sizeof(*c) && size==48);
        staLog.push_back(*c);
        if (c->sta_id >= 4 && c->sta_id < 8) {
            assert(c->mac_id_n_color == 1);
            if (c->add_modify == IWX_STA_MODE_MODIFY && (c->station_flags_msk & IWX_STA_FLG_FAT_EN_MSK)) {
                const uint32_t fat80 = (c->station_flags & IWX_STA_FLG_FAT_EN_MSK) ? 2U : 0U;
                orderLog.push_back((2U << 16) | fat80);
            }
            *status = IWX_ADD_STA_SUCCESS;
            return command(c->add_modify ? "modify-peer-sta" : "add-peer-sta");
        }
        assert(c->sta_id==3 && c->station_type==IWX_STA_GENERAL_PURPOSE && c->mac_id_n_color==1 && c->tid_disable_tx==0xffff);
        for(auto b:c->addr)assert(b==255);
        *status=stationStatus;int error=command("add-station");
        if(!error && stationStatus==IWX_ADD_STA_SUCCESS)stationPresent=true;return error;"""
    src = src.replace(old_status_fn, new_status_fn)
    return src.replace("// P114_SAFETY_EXTRA", extra_routines + "\n" + extra_cpp)


class Preview114HALRegressions(unittest.TestCase):
    compile_run = support.Regressions.compile_run

    def test_p114_hal_mac1_never_touches_phy0_or_binding0_on_infra_ch48(self):
        extra = r"""
int main() {
    for (bool cdb : {false, true}) {
        clockUS = 10000000ULL;
        ItlIwx d;
        if (cdb) d.com.sc_enabled_capa[39 / 8] |= 1U << (39 % 8);
        else d.com.sc_enabled_capa[39 / 8] &= ~(1U << (39 % 8));
        d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
        d.rlcVersion = 2;
        d.infra.in_id = 0;
        d.infra.in_color = 3;
        d.com.sc_phyctxt[0] = {0, 5, &d.com.sc_ic.ic_channels[48]};
        for (unsigned c : {6U, 36U, 40U, 44U, 48U, 52U, 56U, 60U, 64U, 149U, 153U, 157U, 161U}) {
            unsigned flags = c == 6 ? IEEE80211_CHAN_2GHZ : (IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_VHT80);
#ifdef IEEE80211_CHAN_VHT160
            if (c >= 36 && c <= 64) flags |= IEEE80211_CHAN_VHT160;
#endif
            d.com.sc_ic.ic_channels[c] = {flags, c == 6 ? 2437U : 5000U + 5U * c};
            d.com.sc_ic.ic_chan_active[c / 8] |= 1U << (c % 8);
        }

        assert(!d.startAWDLRadio(self));
        assert(d.awdlRadio.binding && d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

        // Traverse Sidecar [149x6, 48x2, 6, 149x5, 48x2] and FaceTime RTG [48x2, 149, 48x7, 149, 48x5]
        // including direct Ch 48 == infraNum requests with widthMHz = 80 and llwRealtime = true:
        const uint16_t sidecar[16] = {149,149,149,149,149,149, 48,48, 6, 149,149,149,149,149, 48,48};
        for (unsigned s = 0; s < 16; ++s) {
            RadioState::GrantPolicy p = {};
            p.slot = s;
            p.widthMHz = sidecar[s] == 149 ? 80 : 20;
            assert(!d.serviceAWDLChannel(sidecar[s], clockUS + 65536ULL, p));
            assert(d.awdlRadio.binding);
            clockUS += 65536ULL;
        }

        const uint16_t rtg[16] = {48,48, 149, 48,48,48,48,48,48,48, 149, 48,48,48,48,48};
        for (unsigned s = 0; s < 16; ++s) {
            RadioState::GrantPolicy p = {};
            p.slot = s;
            p.llwRealtime = true;
            p.widthMHz = 80; // Even if 80 MHz is requested on Ch 48, PHY 1 clamps to 20 MHz and stays on Binding 1!
            assert(!d.serviceAWDLChannel(rtg[s], clockUS + 65536ULL, p));
            assert(d.awdlRadio.binding);
            if (rtg[s] == 48) assert(d.awdlRadio.channel == 48 && d.awdlRadio.widthMHz == 20);
            clockUS += 65536ULL;
        }

        // Idle unbind + rebind directly onto Ch 48 == infraNum: still exclusively Binding 1 / PHY 1!
        RadioState::GrantPolicy pUnbind = {};
        pUnbind.unbindIdle = true;
        assert(!d.serviceAWDLChannel(0, clockUS + 65536ULL, pUnbind));
        assert(d.awdlRadio.idleUnbound && !d.awdlRadio.binding);
        RadioState::GrantPolicy pRebind48 = {};
        pRebind48.widthMHz = 80;
        assert(!d.serviceAWDLChannel(48, clockUS + 65536ULL, pRebind48));
        assert(!d.awdlRadio.idleUnbound && d.awdlRadio.binding);
        assert(d.awdlRadio.channel == 48 && d.awdlRadio.widthMHz == 20);

        assert(!d.stopAWDLRadio());
        assert(!d.awdlRadio.binding);
        assert(!d.bindingLog.empty());
        for (const auto &entry : d.bindingLog) {
            assert(entry.first.phy == 1 && entry.first.id_and_color == 1);
            assert(entry.first.action != IWX_FW_CTXT_ACTION_MODIFY);
        }
    }
    return 0;
}
"""
        self.compile_run(_build_p114_hal_safety_harness(extra))

    def test_p114_hal_wide80_overlaps_infra_clamps_phy1_ch48_to_20mhz(self):
        extra = r"""
int main() {
    // 1. Static overlap matrix for Ch 48 @ 80 MHz and Ch 48 @ 160 MHz:
    assert(RadioState::wide80OverlapsInfra(48, 48, true));
    assert(RadioState::wide80OverlapsInfra(44, 48, true));
    assert(RadioState::wide80OverlapsInfra(36, 48, true));
    assert(RadioState::wide80OverlapsInfra(52, 48, true));
    assert(RadioState::wide80OverlapsInfra(64, 48, true));
    assert(RadioState::wide80OverlapsInfra(48, 48, false));
    assert(RadioState::wide80OverlapsInfra(44, 48, false));
    assert(!RadioState::wide80OverlapsInfra(52, 48, false));
    assert(!RadioState::wide80OverlapsInfra(149, 48, true));
    assert(!RadioState::wide80OverlapsInfra(149, 48, false));

    // 2. Live HAL clamp check on QuZ 0034 with en1 on Ch 48:
    clockUS = 20000000ULL;
    ItlIwx d;
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    d.rlcVersion = 2;
    d.com.sc_cfg = &iwl9560_quz_a0_jf_b0_cfg;
    d.awdlRadio.quz0034 = true;
    d.com.sc_phyctxt[0] = {0, 2, &d.com.sc_ic.ic_channels[48]};
    for (unsigned c : {6U, 36U, 40U, 44U, 48U, 149U, 153U, 157U, 161U}) {
        const unsigned flags = c == 6 ? IEEE80211_CHAN_2GHZ : (IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_VHT80);
        d.com.sc_ic.ic_channels[c] = {flags, c == 6 ? 2437U : 5000U + 5U * c};
        d.com.sc_ic.ic_chan_active[c / 8] |= 1U << (c % 8);
    }
    assert(!d.awdlWideChannelAllowed(48));
    assert(!d.awdlWideChannelAllowed(44));
    assert(d.awdlWideChannelAllowed(149));

    assert(!d.startAWDLRadio(self));
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

    RadioState::GrantPolicy req80 = {};
    req80.widthMHz = 80;
    req80.llwRealtime = true;
    assert(!d.serviceAWDLChannel(48, clockUS + 65536ULL, req80));
    assert(d.awdlRadio.channel == 48 && d.awdlRadio.widthMHz == 20);
    assert(d.phyCommands.back().ci.width == IWX_PHY_VHT_CHANNEL_MODE20);

    assert(!d.serviceAWDLChannel(149, clockUS + 65536ULL, req80));
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);
    assert(d.phyCommands.back().ci.width == IWX_PHY_VHT_CHANNEL_MODE80);

    assert(!d.stopAWDLRadio());
    return 0;
}
"""
        self.compile_run(_build_p114_hal_safety_harness(extra))

    def test_p114_hal_80_to_20_shrink_enforces_tlc_then_sta_faten_then_phy_on_ch48_and_ch6(self):
        extra = r"""
static uint64_t activeDeadline = 0;
static uint64_t queryWindowEnd(const void *, uint64_t) { return activeDeadline; }

int main() {
    clockUS = 30000000ULL;
    ItlIwx d;
    d.com.sc_enabled_capa[44 / 8] |= 1U << (44 % 8);
    d.rlcVersion = 2;
    d.tlcVersion = 4;
    d.com.sc_cfg = &iwl9560_quz_a0_jf_b0_cfg;
    d.awdlRadio.quz0034 = true;
    d.txAnt = 3;
    d.rxAnt = 3;
    d.btCoex.enabled = true;
    d.btCoex.txMask = 3;
    d.com.sc_phyctxt[0] = {0, 1, &d.com.sc_ic.ic_channels[48]};
    for (unsigned c : {6U, 36U, 40U, 44U, 48U, 149U, 153U, 157U, 161U}) {
        const unsigned flags = c == 6 ? IEEE80211_CHAN_2GHZ : (IEEE80211_CHAN_5GHZ | IEEE80211_CHAN_VHT80);
        d.com.sc_ic.ic_channels[c] = {flags, c == 6 ? 2437U : 5000U + 5U * c};
        d.com.sc_ic.ic_chan_active[c / 8] |= 1U << (c % 8);
    }

    assert(!d.startAWDLRadio(self));
    assert(d.awdlRadio.channel == 149 && d.awdlRadio.widthMHz == 80);

    // Configure peer + Station 4 at 80 MHz MIMO2 on Ch 149:
    const uint8_t peerAddr[6] = {2, 0, 0, 0, 0, 0xc4};
    auto &peer = d.awdlPeers.peers[0];
    peer = {};
    peer.occupied = true;
    AirportAWDL::copyAddress(peer.address, peerAddr);
    peer.haveSync = peer.haveSequence = true;
    peer.metadata.fields = AirportAWDL::HTCapabilities;
    peer.metadata.rxHTMcs = 0xff;
    peer.metadata.rxHTMcs2 = 0xff;
    peer.metadata.htCapabilities = AirportAWDL::HTCapLDPC | AirportAWDL::HTCapSGI20 | AirportAWDL::HTCapSGI40;
    peer.sync.presenceMode = 4;
    peer.sync.windowPeriodTU = 16;
    peer.sync.nextWindowTU = 64;
    peer.sequence.count = 16;
    peer.sequence.fill = 0xffff;
    peer.sequence.step = 3;
    peer.sequence.encoding = 3;
    const uint16_t seqCh[16] = {149,149,149,149,149,149, 48,48, 6, 149,149,149,149,149, 48,48};
    for (unsigned s = 0; s < 16; ++s) {
        peer.sequence.channel[s] = seqCh[s];
        peer.sequence.flags[s] = seqCh[s] == 149 ? 128 : (seqCh[s] == 48 ? 115 : 81);
    }
    auto syncPeerSlot = [&](unsigned slot) {
        peer.sync.windowCounter = uint16_t(slot * 4);
        peer.sync.nextWindowTU = 64;
        peer.lastSeenUS = peer.syncReceivedUS = peer.sequenceReceivedUS = peer.htCapabilitiesReceivedUS = clockUS;
    };
    syncPeerSlot(0);

    assert(!d.addAWDLBlockAckPeer(0, peerAddr));
    auto &tx = d.awdlRadio.blockAckTX;
    tx.configured = true;
    tx.epoch = d.dmaEpoch;
    tx.cookie = 1;
    tx.station = 4;
    tx.queue = 9;
    d.com.txq[9].allocate(9);
    tx.phase = AirportAWDL::TXBlockAckQueue::Agreed;
    tx.enabled = true;
    tx.paused = true;
    AirportAWDL::copyAddress(tx.peer, peerAddr);

    RadioState::GrantPolicy p80 = {};
    p80.slot = 0;
    p80.widthMHz = 80;
    assert(!d.serviceAWDLChannel(149, clockUS + 65536ULL, p80));
    activeDeadline = clockUS + 50000ULL;
    assert(!d.setAWDLTXWindow(queryWindowEnd, nullptr));
    assert(!tx.paused && tx.widthMHz == 80 && tx.antenna == 3);

    auto assertShrinkOrder = [&](const std::vector<uint32_t> &log) {
        int tlc20 = -1, sta20 = -1, phy20 = -1;
        for (size_t i = 0; i < log.size(); ++i) {
            if (log[i] == ((1U << 16) | uint32_t(IWX_TLC_MNG_CH_WIDTH_20MHZ)) && tlc20 < 0) tlc20 = int(i);
            if (log[i] == ((2U << 16) | 0U) && sta20 < 0) sta20 = int(i);
            if (log[i] == ((3U << 16) | uint32_t(IWX_PHY_VHT_CHANNEL_MODE20)) && phy20 < 0) phy20 = int(i);
        }
        assert(tlc20 >= 0 && sta20 >= 0 && phy20 >= 0);
        assert(tlc20 < sta20 && sta20 < phy20);
    };

    // 1. Shrink 149@80 -> 48@20: MUST execute TLC(20, VHT 0x01ff) -> STA FAT_EN(20) -> PHY(20)!
    clockUS += 65536ULL;
    syncPeerSlot(6);
    d.orderLog.clear();
    d.tlcLog.clear();
    RadioState::GrantPolicy p48 = {};
    p48.slot = 6;
    p48.widthMHz = 20;
    p48.llwRealtime = true;
    assert(!d.serviceAWDLChannel(48, clockUS + 65536ULL, p48));
    assert(tx.paused && d.awdlRadio.channel == 48 && d.awdlRadio.widthMHz == 20);
    assertShrinkOrder(d.orderLog);
    assert(!d.tlcLog.empty());
    assert(d.tlcLog.back().mode == IWX_TLC_MNG_MODE_VHT);
    assert(d.tlcLog.back().max_ch_width == IWX_TLC_MNG_CH_WIDTH_20MHZ);
    assert(d.tlcLog.back().chains == 3);
    assert(d.tlcLog.back().ht_rates[0][0] == 0x01ff && d.tlcLog.back().ht_rates[1][0] == 0x01ff);

    // 2. Grow 48@20 -> 149@80: MUST execute PHY(80) -> STA FAT_EN(80), then restore TLC(80, VHT 0x03ff)!
    clockUS += 65536ULL;
    syncPeerSlot(9);
    d.orderLog.clear();
    p80.slot = 9;
    assert(!d.serviceAWDLChannel(149, clockUS + 65536ULL, p80));
    int phy80 = -1, sta80 = -1;
    for (size_t i = 0; i < d.orderLog.size(); ++i) {
        if (d.orderLog[i] == ((3U << 16) | uint32_t(IWX_PHY_VHT_CHANNEL_MODE80)) && phy80 < 0) phy80 = int(i);
        if (d.orderLog[i] == ((2U << 16) | 2U) && sta80 < 0) sta80 = int(i);
    }
    assert(phy80 >= 0 && sta80 >= 0 && phy80 < sta80);
    activeDeadline = clockUS + 50000ULL;
    assert(!d.setAWDLTXWindow(queryWindowEnd, nullptr));
    assert(!tx.paused && tx.widthMHz == 80 && tx.antenna == 3);

    // 3. Shrink 149@80 -> 6@20: MUST execute TLC(20, HT 0x00ff, chains=2 non-shared Ant B, NSS2=0)
    //    -> STA FAT_EN(20) -> PHY(20), preventing both 0x0034 and 0x20A2 firmware asserts!
    clockUS += 65536ULL;
    syncPeerSlot(8);
    d.orderLog.clear();
    d.tlcLog.clear();
    RadioState::GrantPolicy p6 = {};
    p6.slot = 8;
    p6.widthMHz = 20;
    assert(!d.serviceAWDLChannel(6, clockUS + 60000ULL, p6));
    assert(tx.paused && d.awdlRadio.channel == 6 && d.awdlRadio.widthMHz == 20);
    assertShrinkOrder(d.orderLog);
    assert(!d.tlcLog.empty());
    assert(d.tlcLog.back().mode == IWX_TLC_MNG_MODE_HT);
    assert(d.tlcLog.back().max_ch_width == IWX_TLC_MNG_CH_WIDTH_20MHZ);
    assert(d.tlcLog.back().chains == 2);
    assert(d.tlcLog.back().ht_rates[0][0] == 0x00ff && d.tlcLog.back().ht_rates[1][0] == 0);

    d.awdlBA.peers[0] = {};
    tx = {};
    assert(!d.stopAWDLRadio());
    return 0;
}
"""
        self.compile_run(_build_p114_hal_safety_harness(extra))


if __name__ == "__main__":
    unittest.main()
