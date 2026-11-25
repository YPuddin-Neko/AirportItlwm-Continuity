// SPDX-License-Identifier: GPL-2.0-or-later
#include "ItlIwx.hpp"

#ifdef AIRPORT_WCL
using AirportAWDL::RadioState;
static uint64_t awdlRadioNow();

int ItlIwx::awdlMAC(uint32_t action, bool receiveAllData)
{
    const uint64_t epoch = dmaEpoch;
#if DEBUG
    if (action == IWX_FW_CTXT_ACTION_ADD && awdlRadio.quz0034) receiveAllData = true;
#else
    receiveAllData = awdlRadio.quz0034;
#endif
    iwx_mac_ctx_cmd cmd = {};
    cmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(RadioState::MacID, 0));
    cmd.action = htole32(action);
    if (action != IWX_FW_CTXT_ACTION_REMOVE) {
        cmd.mac_type = htole32(awdlRadio.listener ? IWX_FW_MAC_TYPE_LISTENER : IWX_FW_MAC_TYPE_P2P_DEVICE);
        cmd.tsf_id = htole32(IWX_TSF_ID_B);
        memcpy(cmd.node_addr, awdlRadio.address, 6);
        memcpy(cmd.bssid_addr, AirportAWDL::BSSID, 6);
        cmd.cck_rates = htole32(0x0f);
        cmd.ofdm_rates = htole32(0x15); // 6, 12 and 24 Mbps basic ACK rates.
        cmd.short_slot = htole32(IWX_MAC_FLG_SHORT_SLOT);
        cmd.filter_flags = htole32(IWX_MAC_FILTER_IN_CONTROL_AND_MGMT |
            IWX_MAC_FILTER_ACCEPT_GRP | IWX_MAC_FILTER_IN_BEACON |
            IWX_MAC_FILTER_IN_PROBE_REQUEST | IWX_MAC_FILTER_DIS_DECRYPT |
            IWX_MAC_FILTER_DIS_GRP_DECRYPT);
        // This board needs MAC1 to pass peer data to the host's AWDL address and BSSID checks.
        if (receiveAllData) cmd.filter_flags |= htole32(IWX_MAC_FILTER_IN_PROMISC);
        if (awdlRadio.listener) {
            // Promiscuous RX on the AP's channel would take its own frames.
            const auto infra = reinterpret_cast<const iwx_node *>(com.sc_ic.ic_bss);
            receiveAllData = awdlRadio.listenerWantsPromisc(awdlHasRXBA(), infra && infra->in_phyctxt &&
                infra->in_phyctxt->channel != &com.sc_ic.ic_channels[awdlRadio.channel], awdlRadioNow());
            cmd.cck_rates = htole32(awdlRadio.channel <= 14 ? 0x0f : 0);
            // BA replies remain host-managed when RX aggregation disables promiscuous data.
            cmd.filter_flags = htole32(IWX_MAC_FILTER_ACCEPT_GRP | IWX_MAC_FILTER_IN_CONTROL_AND_MGMT |
                (receiveAllData ? IWX_MAC_FILTER_IN_PROMISC : 0));
        }
        // Firmware AC order is BK, BE, VI, VO; Gen2 reserves FIFO 0 for commands.
        const uint8_t fifos[IWX_AC_NUM] = {IWX_GEN2_EDCA_TX_FIFO_BK,
            IWX_GEN2_EDCA_TX_FIFO_BE, IWX_GEN2_EDCA_TX_FIFO_VI, IWX_GEN2_EDCA_TX_FIFO_VO};
        for (unsigned i = 0; i < IWX_AC_NUM; ++i) {
            cmd.ac[i].cw_min = htole16(15);
            cmd.ac[i].cw_max = htole16(1023);
            cmd.ac[i].aifsn = 3;
            cmd.ac[i].fifos_mask = 1U << fifos[i];
        }
        cmd.p2p_dev.is_disc_extended = 0;
    }
#if DEBUG
    awdlReceiveFilterKnown = false;
#endif
    if (awdlRadio.listener) awdlRadio.listenerFilterKnown = false;
    int error = iwx_send_cmd_pdu(&com, IWX_MAC_CONTEXT_CMD, 0, sizeof(cmd), &cmd);
    if (epoch != dmaEpoch) return ENXIO;
    if (!error && awdlRadio.listener) {
        awdlRadio.listenerPromisc = action != IWX_FW_CTXT_ACTION_REMOVE && receiveAllData;
        awdlRadio.listenerFilterKnown = action != IWX_FW_CTXT_ACTION_REMOVE;
        awdlRadio.listenerFilterFlags = action != IWX_FW_CTXT_ACTION_REMOVE ? le32toh(cmd.filter_flags) : 0;
    }
#if DEBUG
    if (!error && action != IWX_FW_CTXT_ACTION_REMOVE) {
        awdlReceiveAllData = receiveAllData;
        awdlReceiveFilterKnown = true;
    }
#endif
    if (action != IWX_FW_CTXT_ACTION_MODIFY || awdlRadio.listener)
        awdlRadio.recordFailure(action == IWX_FW_CTXT_ACTION_REMOVE ? RadioState::RemoveMAC :
            (action == IWX_FW_CTXT_ACTION_MODIFY ? RadioState::ModifyMAC : RadioState::AddMAC),
            error, IWX_MAC_CONTEXT_CMD);
    return error;
}

static uint64_t awdlRadioNow()
{
    struct timeval tv;
    getmicrouptime(&tv);
    return uint64_t(tv.tv_sec) * 1000000 + tv.tv_usec;
}

#if DEBUG
uint8_t ItlIwx::awdlCompareArm() const
{
    if (!awdlCompareEndUS) return 0;
    const uint64_t now = awdlRadioNow();
    if (now >= awdlCompareEndUS) return 0;
    return awdlCompareEndUS - now > 15000000 ? 1 : 2;
}
#endif

bool ItlIwx::awdlChannelAllowed(uint32_t channel, uint8_t widthMHz) const
{
    if (!channel || channel > IEEE80211_CHAN_MAX) return false;
    const auto &ic = com.sc_ic;
    const auto &chan = ic.ic_channels[channel];
    if (widthMHz != 20) {
        const unsigned base = (channel >= 36 && channel <= 48 && (channel - 36) % 4 == 0) ? 36U :
            (channel >= 149 && channel <= 161 && (channel - 149) % 4 == 0) ? 149U : 0U;
        if (widthMHz != 80 || !base || !(chan.ic_flags & IEEE80211_CHAN_VHT80)) return false;
        for (unsigned subchannel = base; subchannel <= base + 12; subchannel += 4)
            if (!awdlChannelAllowed(subchannel, 20) ||
                (ic.ic_channels[subchannel].ic_flags & IEEE80211_CHAN_NOADHOC)) return false;
        return true;
    }
    return isset(ic.ic_chan_active, channel) && chan.ic_freq &&
        (chan.ic_flags & (IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_5GHZ)) &&
        !(chan.ic_flags & IEEE80211_CHAN_DFS) &&
        (!(chan.ic_flags & IEEE80211_CHAN_PASSIVE) || isset(ic.ic_beacon_hint, channel));
}

bool ItlIwx::awdlWideChannelAllowed(uint32_t channel) const
{
    if (awdlRadio.wideCircuitTripped() || !awdlChannelAllowed(channel, 80)) return false;
    const auto &ic = com.sc_ic;
    if (ic.ic_state == IEEE80211_S_RUN && ic.ic_bss) {
        const auto infra = reinterpret_cast<const iwx_node *>(ic.ic_bss);
        const auto infraChan = infra->in_phyctxt ? infra->in_phyctxt->channel : nullptr;
        constexpr size_t chanCount = sizeof(ic.ic_channels) / sizeof(ic.ic_channels[0]);
        if (infraChan && infraChan >= ic.ic_channels && infraChan < ic.ic_channels + chanCount) {
            const uint32_t infraNum = uint32_t(infraChan - ic.ic_channels);
            bool infra160 = false;
#ifdef IEEE80211_CHAN_VHT160
            infra160 = (infraChan->ic_flags & IEEE80211_CHAN_VHT160) != 0;
#endif
            if (RadioState::wide80OverlapsInfra(channel, infraNum, infra160)) return false;
        }
    }
    return true;
}

int ItlIwx::awdlPHY(uint16_t channel, uint8_t widthMHz)
{
    if (!awdlChannelAllowed(channel, widthMHz)) return EACCES;
    if (widthMHz == 80 && !awdlWideChannelAllowed(channel)) widthMHz = 20;
    if (awdlRadio.binding || awdlRadio.station) {
        const auto &tx = awdlRadio.blockAckTX;
        if (!awdlRadio.listener || !awdlRadio.changing || awdlRadio.dwell.running ||
            (awdlRadio.queue != IWX_INVALID_QUEUE && com.txq[awdlRadio.queue].queued) ||
            (awdlRadio.dataQueueConfigured && com.txq[awdlRadio.dataQueue].queued) ||
            (awdlRadio.llwQueueConfigured && com.txq[awdlRadio.llwQueue].queued) ||
            (tx.configured && tx.enabled && !tx.paused)) return EBUSY;
    }
    auto &phy = com.sc_phyctxt[RadioState::PhyID];
    const auto previous = phy.channel;
    const uint8_t previousWidth = awdlRadio.widthMHz ? awdlRadio.widthMHz : 20;
    const auto chan = &com.sc_ic.ic_channels[channel];
    const uint64_t epoch = dmaEpoch;
    const bool rlc = iwx_lookup_cmd_ver(&com, IWX_DATA_PATH_GROUP, IWX_RLC_CONFIG_CMD) == 2;
    const uint8_t antennas = iwx_fw_valid_rx_ant(&com);
    if (rlc && antennas && phy.channel == chan && previousWidth == widthMHz && awdlRadio.phyReceiveEpoch == epoch &&
        awdlRadio.phyReceiveAntennas == antennas) return 0;
    awdlRadio.phyReceiveAntennas = 0;
    const unsigned chains = (antennas & (antennas - 1)) ? 2 : 1;
    const uint32_t rxchain = htole32((antennas << IWX_PHY_RX_CHAIN_VALID_POS) |
        (chains << IWX_PHY_RX_CHAIN_CNT_POS) | (chains << IWX_PHY_RX_CHAIN_MIMO_CNT_POS));
    const unsigned bands = IEEE80211_CHAN_2GHZ | IEEE80211_CHAN_5GHZ;
    const bool replace = !awdlRadio.binding &&
        isset(com.sc_enabled_capa, IWX_UCODE_TLV_CAPA_BINDING_CDB_SUPPORT) &&
        (previous->ic_flags & bands) != (chan->ic_flags & bands);
    auto configure = [&](decltype(phy.channel) target, uint8_t width, uint32_t action) {
        iwx_phy_context_cmd cmd = {};
        cmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(phy.id, phy.color));
        cmd.action = htole32(action);
        cmd.lmac_id = htole32(iwx_lmac_id(&com, target));
        if (action != IWX_FW_CTXT_ACTION_REMOVE) {
            cmd.ci.band = IEEE80211_IS_CHAN_2GHZ(target) ? IWX_PHY_BAND_24 : IWX_PHY_BAND_5;
            cmd.ci.channel = htole32(target - com.sc_ic.ic_channels);
            cmd.ci.width = width == 80 ? IWX_PHY_VHT_CHANNEL_MODE80 : IWX_PHY_VHT_CHANNEL_MODE20;
            cmd.ci.ctrl_pos = IWX_PHY_VHT_CTRL_POS_1_BELOW;
            if (width == 80) {
                const uint8_t positions[] = {IWX_PHY_VHT_CTRL_POS_2_BELOW, IWX_PHY_VHT_CTRL_POS_1_BELOW,
                    IWX_PHY_VHT_CTRL_POS_1_ABOVE, IWX_PHY_VHT_CTRL_POS_2_ABOVE};
                const unsigned ch = unsigned(target - com.sc_ic.ic_channels);
                const unsigned base = ch >= 149 ? 149U : 36U;
                cmd.ci.ctrl_pos = positions[(ch - base) / 4];
            }
            if (!rlc) cmd.rxchain_info = rxchain;
        }
        return iwx_send_cmd_pdu(&com, IWX_PHY_CONTEXT_CMD, 0, sizeof(cmd), &cmd);
    };
    auto receiveChains = [&]() {
        struct iwx_rlc_config_cmd cmd = {};
        cmd.phy_id = htole32(RadioState::PhyID);
        cmd.rlc.rx_chain_info = rxchain;
        return iwx_send_cmd_pdu(&com,
            iwx_cmd_id(IWX_RLC_CONFIG_CMD, IWX_DATA_PATH_GROUP, 2), 0, sizeof(cmd), &cmd);
    };
    if (replace) {
        const int error = configure(previous, previousWidth, IWX_FW_CTXT_ACTION_REMOVE);
        if (epoch != dmaEpoch) return ENXIO;
        awdlRadio.recordFailure(RadioState::RemovePHY, error, IWX_PHY_CONTEXT_CMD);
        if (error) return error;
    }
    int error = configure(chan, widthMHz, replace ? IWX_FW_CTXT_ACTION_ADD : IWX_FW_CTXT_ACTION_MODIFY);
    if (epoch != dmaEpoch) return ENXIO;
    awdlRadio.recordFailure(RadioState::ConfigurePHY, error, IWX_PHY_CONTEXT_CMD);
    if (error) return error;
    if (rlc) {
        error = receiveChains();
        if (epoch != dmaEpoch) return ENXIO;
        awdlRadio.recordFailure(RadioState::ConfigurePHY, error,
            iwx_cmd_id(IWX_RLC_CONFIG_CMD, IWX_DATA_PATH_GROUP, 2));
        if (error && (chan != previous || widthMHz != previousWidth)) {
            // The channel change was acknowledged before its RLC failed.
            // Submissions remain stopped and queued peer data remains paused during restoration.
            int restore = 0;
            if (replace) {
                restore = configure(chan, widthMHz, IWX_FW_CTXT_ACTION_REMOVE);
                if (epoch != dmaEpoch) return ENXIO;
            }
            if (!restore) {
                restore = configure(previous, previousWidth,
                    replace ? IWX_FW_CTXT_ACTION_ADD : IWX_FW_CTXT_ACTION_MODIFY);
                if (epoch != dmaEpoch) return ENXIO;
            }
            if (!restore) {
                restore = receiveChains();
                if (epoch != dmaEpoch) return ENXIO;
            }
            // recordFailure keeps the initiating RLC error; a failed restore
            // is returned so normal AWDL cleanup preserves a terminal fault.
            if (restore) return restore;
            awdlRadio.phyReceiveEpoch = epoch;
            awdlRadio.phyReceiveAntennas = antennas;
        }
    }
    if (!error) {
        phy.channel = chan;
        awdlRadio.widthMHz = widthMHz;
        if (rlc) {
            awdlRadio.phyReceiveEpoch = epoch;
            awdlRadio.phyReceiveAntennas = antennas;
        }
    }
    return error;
}

int ItlIwx::awdlBinding(bool add)
{
    const uint64_t epoch = dmaEpoch;
    const auto &phy = com.sc_phyctxt[RadioState::PhyID];
    struct iwx_binding_cmd cmd = {};
    cmd.id_and_color = cmd.phy = htole32(IWX_FW_CMD_ID_AND_COLOR(phy.id, phy.color));
    cmd.action = htole32(add ? IWX_FW_CTXT_ACTION_ADD : IWX_FW_CTXT_ACTION_REMOVE);
    for (auto &mac : cmd.macs) mac = htole32(IWX_FW_CTXT_INVALID);
    if (add) cmd.macs[0] = htole32(IWX_FW_CMD_ID_AND_COLOR(RadioState::MacID, 0));
    cmd.lmac_id = htole32(iwx_lmac_id(&com, phy.channel));
    const size_t size = isset(com.sc_enabled_capa, IWX_UCODE_TLV_CAPA_BINDING_CDB_SUPPORT) ?
        sizeof(cmd) : sizeof(cmd) - sizeof(cmd.lmac_id);
    uint32_t status = 0;
    int error = iwx_send_cmd_pdu_status(&com, IWX_BINDING_CONTEXT_CMD, size, &cmd, &status);
    if (epoch != dmaEpoch) return ENXIO;
    const uint32_t firmwareStatus = error ? UINT32_MAX : status;
    if (!error && status) error = EIO;
    awdlRadio.recordFailure(add ? RadioState::AddBinding : RadioState::RemoveBinding,
        error, IWX_BINDING_CONTEXT_CMD, firmwareStatus);
    return error;
}

int ItlIwx::startAWDLRadio(const uint8_t address[6])
{
    if (!getMainWorkLoop()->inGate())
        return getMainCommandGate()->runAction(
            [](OSObject *, void *self, void *addr, void *, void *) -> IOReturn {
                return static_cast<ItlIwx *>(self)->startAWDLRadio(static_cast<uint8_t *>(addr));
            }, this, const_cast<uint8_t *>(address));
    if (!AirportAWDL::unicastAddress(address)) return EINVAL;
    if (com.sc_device_family != IWX_DEVICE_FAMILY_22000 ||
        (!isset(com.sc_enabled_capa, IWX_UCODE_TLV_CAPA_DYNAMIC_QUOTA) &&
         !isset(com.sc_enabled_capa, IWX_UCODE_TLV_CAPA_SESSION_PROT_CMD)) ||
        iwx_lookup_cmd_ver(&com, IWX_LONG_GROUP, IWX_PHY_CONTEXT_CMD) != 3)
        return ENOTSUP;
    if (awdlRadio.changing) return EBUSY;
    if (awdlRadio.fault) return EIO;
    if (awdlRadio.stopPending) return EINPROGRESS;
    if (awdlRadio.ready)
        return AirportAWDL::sameAddress(address, awdlRadio.address) ? 0 : EBUSY;
    auto &ic = com.sc_ic;
    if (ic.ic_opmode != IEEE80211_M_STA || ic.ic_state != IEEE80211_S_RUN ||
        !(com.sc_flags & IWX_FLAG_BINDING_ACTIVE) || !ic.ic_bss) return ENETDOWN;
    auto infra = reinterpret_cast<iwx_node *>(ic.ic_bss);
    if (!infra->in_phyctxt || infra->in_id == RadioState::MacID ||
        infra->in_phyctxt->id == RadioState::PhyID) return EBUSY;
    const uint8_t validTx = awdlRadio.quz0034 ? iwx_fw_valid_tx_ant(&com) : 0;
    if (awdlRadio.quz0034 && !validTx) return ENOTSUP;
    awdlRadio.channel = awdlChannelAllowed(149) ? 149 :
        (awdlChannelAllowed(44) ? 44 : (awdlChannelAllowed(6) ? 6 : 0));
    if (!awdlRadio.channel) return EACCES;

    awdlRadio.changing = true;
    awdlRadio.listener = isset(com.sc_enabled_capa, IWX_UCODE_TLV_CAPA_DYNAMIC_QUOTA);
    awdlRadio.listenerFilterKnown = false;
    awdlRadio.failure = {};
    awdlRadio.queueSetup = {};
    awdlRadio.dataQueueSetup = {};
    awdlRadio.llwQueueSetup = {};
    awdlRadio.session = {};
    // This subsystem's AWDL frames reached the peer on A. Fall back only when firmware/NVM exclude A.
    awdlRadio.txAntennaMask = validTx & -validTx;
    awdlRadio.queue = IWX_INVALID_QUEUE;
    awdlRadio.dataQueue = IWX_INVALID_QUEUE;
    awdlRadio.dataQueueConfigured = false;
    awdlRadio.llwQueue = IWX_INVALID_QUEUE;
    awdlRadio.llwQueueConfigured = false;
    awdlRadio.llwQueueFailed = false;
    awdlRadio.llwRealtime = false;
    awdlRadio.idleUnbound = false;
    awdlRadio.lastWideSocialVisitUS = 0;
    awdlRadio.parkStartUS = 0;
    awdlRadio.idleEligibleSinceUS = 0;
    awdlRadio.lastIdleProbeUS = 0;
    memcpy(awdlRadio.address, address, 6);
    const uint64_t epoch = dmaEpoch;
    const auto bss = ic.ic_bss;
    const auto infraPHY = infra->in_phyctxt;
    const auto infraChannel = infraPHY->channel;
    const uint32_t infraMAC = IWX_FW_CMD_ID_AND_COLOR(infra->in_id, infra->in_color);
    auto wanted = [&]() {
        if (awdlRadio.stopPending || ic.ic_bss != bss || ic.ic_state != IEEE80211_S_RUN ||
            com.ns_nstate != IEEE80211_S_RUN || !(com.sc_flags & IWX_FLAG_BINDING_ACTIVE)) return false;
        const auto current = reinterpret_cast<const iwx_node *>(ic.ic_bss);
        return current->in_phyctxt == infraPHY && infraPHY->channel == infraChannel &&
            IWX_FW_CMD_ID_AND_COLOR(current->in_id, current->in_color) == infraMAC;
    };
    iwx_device_power_cmd power = {};
    awdlRadio.awake = true;
    int error = iwx_send_cmd_pdu(&com, IWX_POWER_TABLE_CMD, 0, sizeof(power), &power);
    if (epoch != dmaEpoch) return ENXIO;
    awdlRadio.recordFailure(RadioState::PowerAwake, error, IWX_POWER_TABLE_CMD);
    if (!error && awdlRadio.listener && !wanted()) error = ECANCELED;
    if (!error) {
        awdlRadio.mac = true;
#if DEBUG
        error = awdlMAC(IWX_FW_CTXT_ACTION_ADD, awdlReceiveAllData);
#else
        error = awdlMAC(IWX_FW_CTXT_ACTION_ADD);
#endif
        if (epoch != dmaEpoch) return ENXIO;
    }
    if (!error && awdlRadio.listener && !wanted()) error = ECANCELED;
    if (!error && awdlRadio.listener) {
        error = prepareAWDLListener(awdlRadio.channel, awdlWideChannelAllowed(awdlRadio.channel) ? 80 : 20);
        if (epoch != dmaEpoch) return ENXIO;
    }
    awdlRadio.changing = false;
    if (error) {
        awdlRadio.fault = error != ECANCELED;
        stopAWDLRadio();
        if (epoch != dmaEpoch) return ENXIO;
        awdlRadio.lastError = error;
        return error;
    }
    awdlRadio.ready = true;
    awdlRadio.lastError = 0;
    return 0;
}

void ItlIwx::receiveAWDLSession(const struct iwx_rx_packet *pkt)
{
    auto &report = awdlRadio.session;
    ++report.received;
    report.lastLength = iwx_rx_packet_payload_len(pkt);
    if (report.lastLength != sizeof(iwx_session_prot_notif)) {
        ++report.malformed;
        return;
    }
    const auto n = reinterpret_cast<const iwx_session_prot_notif *>(pkt->data);
    report.lastMAC = le32toh(n->mac_id);
    report.lastConfig = le32toh(n->conf_id);
    report.lastStart = le32toh(n->start);
    report.lastStatus = le32toh(n->status);
    auto &d = awdlRadio.dwell;
    if (awdlRadio.listener) {
        ++report.inactive;
        return;
    }
    if (report.lastMAC != RadioState::MacID) {
        ++report.otherMAC;
        return;
    }
    if (!d.requested || d.done) {
        ++report.inactive;
        return;
    }
    // Intel's P2P handler checks the configuration on START only. END has no
    // session cookie, so an active MAC's END conservatively stops transmission.
    if (report.lastStart && report.lastConfig != d.confID) {
        ++report.unexpectedStart;
        return;
    }
    if (report.lastStart && !report.startUS) report.startUS = awdlRadioNow();
    if (!report.lastStart) {
        report.lastEndConfig = report.lastConfig;
        report.endUS = awdlRadioNow();
        awdlRadio.reserveAPTime(report.endUS, false);
        d.running = false;
        d.done = 1;
        ++awdlRadio.dwellEnds;
        getMainCommandGate()->commandWakeup(&d.done);
    } else if (!report.lastStatus) {
        // Firmware sends END even when START fails; consume it before reusing this MAC.
        d.failed = true;
        d.running = false;
        ++awdlRadio.dwellFailures;
    } else if (!d.cancelling && !d.failed && !d.running) {
        d.running = true;
        ++awdlRadio.dwellStarts;
    }
    if (awdlAvailabilityChanged) awdlAvailabilityChanged(awdlReceiver);
}

int ItlIwx::finishAWDLDwell()
{
    if (awdlRadio.cleanupError) return awdlRadio.cleanupError;
    const uint64_t epoch = dmaEpoch;
    auto &d = awdlRadio.dwell;
    awdlRadio.llwRealtime = false;
    if (awdlRadio.listener && d.running) ++awdlRadio.dwellEnds;
    d.running = false;
    if (awdlAvailabilityChanged) awdlAvailabilityChanged(awdlReceiver);
    const int baError = clearAWDLBlockAck();
    if (epoch != dmaEpoch) return ENXIO;
    if (baError) { awdlRadio.cleanupError = baError; return baError; }
    if (d.requested && !d.done && !d.cancelAcknowledged) {
        d.cancelling = true;
        iwx_session_prot_cmd cmd = {};
        cmd.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(RadioState::MacID, 0));
        cmd.action = htole32(IWX_FW_CTXT_ACTION_REMOVE);
        cmd.conf_id = htole32(d.confID);
        ++awdlRadio.session.cancels;
        int error = iwx_send_cmd_pdu(&com,
            iwx_cmd_id(IWX_SESSION_PROTECTION_CMD, IWX_MAC_CONF_GROUP, 0), 0, sizeof(cmd), &cmd);
        if (epoch != dmaEpoch) return ENXIO;
        awdlRadio.recordFailure(RadioState::CancelSession, error,
            iwx_cmd_id(IWX_SESSION_PROTECTION_CMD, IWX_MAC_CONF_GROUP, 0));
        if (error) { awdlRadio.cleanupError = error; return error; }
        d.cancelAcknowledged = true;
        ++awdlRadio.session.cancelAcknowledged;
        awdlRadio.session.cancelAckUS = awdlRadioNow();
    }
    if (awdlRadio.queue != IWX_INVALID_QUEUE && !d.flushed) {
        int error = iwx_flush_sta_tids(&com, RadioState::StationID, 0xffff);
        if (epoch != dmaEpoch) return ENXIO;
        awdlRadio.recordFailure(RadioState::FlushStation, error, IWX_TXPATH_FLUSH);
        if (error) { awdlRadio.cleanupError = error; return error; }
        d.flushed = true;
        awdlRadio.session.flushAckUS = awdlRadioNow();
    }
    if (d.cancelAcknowledged && !d.cancelCompleted) {
        // Intel's cancellation path completes after REMOVE and the station
        // flush. END remains a notification, not a second cancellation ACK.
        d.cancelCompleted = true;
        ++awdlRadio.session.cancelCompletions;
        awdlRadio.session.cancelCompletedUS = MAX(awdlRadio.session.cancelAckUS,
            awdlRadio.session.flushAckUS);
        awdlRadio.reserveAPTime(awdlRadio.session.cancelCompletedUS, true);
    }
    if (awdlRadio.station) {
        struct iwx_rm_sta_cmd cmd = {};
        cmd.sta_id = RadioState::StationID;
        int error = iwx_send_cmd_pdu(&com, IWX_REMOVE_STA, 0, sizeof(cmd), &cmd);
        if (epoch != dmaEpoch) return ENXIO;
        awdlRadio.recordFailure(RadioState::RemoveStation, error, IWX_REMOVE_STA);
        if (error) { awdlRadio.cleanupError = error; return error; }
        awdlRadio.station = false;
    }
    int error = awdlRemoveQueue();
    awdlRadio.recordFailure(RadioState::FreeQueue, error);
    if (error) { awdlRadio.cleanupError = error; return error; }
    if (awdlRadio.binding) {
        error = awdlBinding(false);
        if (epoch != dmaEpoch) return ENXIO;
        if (error) { awdlRadio.cleanupError = error; return error; }
        awdlRadio.binding = false;
    }
    d = {};
    return 0;
}

// Adapted from ForbiddenItlwm 59ff7bff, awdlCoexListener/awdlCoexTxSetup.
// Own the binding, station and DMA queues until stop, retune or firmware reset.
int ItlIwx::prepareAWDLListener(uint16_t channel, uint8_t widthMHz)
{
    const uint64_t epoch = dmaEpoch;
    const auto bss = com.sc_ic.ic_bss;
    const auto infra = reinterpret_cast<const iwx_node *>(bss);
    const auto phy = infra ? infra->in_phyctxt : nullptr;
    const auto infraChannel = phy ? phy->channel : nullptr;
    const uint32_t infraMAC = infra ? IWX_FW_CMD_ID_AND_COLOR(infra->in_id, infra->in_color) : 0;
    auto wanted = [&]() {
        if (epoch != dmaEpoch || awdlRadio.stopPending || !bss || !phy || com.sc_ic.ic_bss != bss ||
            com.sc_ic.ic_opmode != IEEE80211_M_STA || com.sc_ic.ic_state != IEEE80211_S_RUN ||
            com.ns_nstate != IEEE80211_S_RUN || !(com.sc_flags & IWX_FLAG_BINDING_ACTIVE)) return false;
        const auto current = reinterpret_cast<const iwx_node *>(com.sc_ic.ic_bss);
        return current->in_phyctxt == phy && phy->channel == infraChannel &&
            IWX_FW_CMD_ID_AND_COLOR(current->in_id, current->in_color) == infraMAC;
    };
    if (!wanted()) return ECANCELED;
    int error = awdlPHY(channel, widthMHz);
    if (epoch != dmaEpoch) return ENXIO;
    if (error || !wanted()) return error ? error : ECANCELED;
    awdlRadio.binding = true;
    error = awdlBinding(true);
    if (epoch != dmaEpoch) return ENXIO;
    if (error || !wanted()) return error ? error : ECANCELED;
    struct iwx_add_sta_cmd station = {};
    station.sta_id = RadioState::StationID;
    station.station_type = IWX_STA_GENERAL_PURPOSE;
    station.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(RadioState::MacID, 0));
    station.tid_disable_tx = htole16(0xffff);
    memset(station.addr, 0xff, sizeof(station.addr));
    uint32_t status = IWX_ADD_STA_SUCCESS;
    error = iwx_send_cmd_pdu_status(&com, IWX_ADD_STA, sizeof(station), &station, &status);
    if (epoch != dmaEpoch) return ENXIO;
    const uint32_t firmwareStatus = error ? UINT32_MAX : status;
    awdlRadio.station = error || (status & IWX_ADD_STA_STATUS_MASK) == IWX_ADD_STA_SUCCESS;
    if (!error && !awdlRadio.station) error = EIO;
    awdlRadio.recordFailure(RadioState::AddStation, error, IWX_ADD_STA, firmwareStatus);
    if (error || !wanted()) return error ? error : ECANCELED;
    const int queue = iwx_tvqm_enable_txq(&com, IWX_MGMT_TID, 0, 64, RadioState::StationID,
        &awdlRadio.queueSetup);
    if (epoch != dmaEpoch) return ENXIO;
    if (queue < 0) {
        awdlRadio.recordFailure(RadioState::AllocateQueue, -queue, IWX_SCD_QUEUE_CFG,
            awdlRadio.queueSetup.responseFlags);
        return -queue;
    }
    awdlRadio.queue = queue;
    return wanted() ? 0 : ECANCELED;
}

int ItlIwx::closeAWDLListenerWindow()
{
    auto &d = awdlRadio.dwell;
    auto &tx = awdlRadio.blockAckTX;
    if (tx.configured && !tx.paused &&
        !(tx.phase == AirportAWDL::TXBlockAckQueue::Asking && !tx.enabled)) {
        const uint64_t epoch = dmaEpoch;
        const int error = configureAWDLTXStation(tx.enabled, true);
        if (epoch != dmaEpoch) return ENXIO;
        if (error) { awdlRadio.cleanupError = error; return error; }
    }
    if (!d.running && d.flushed) return 0;
    const bool open = d.running;
    d.running = false;
    if (open && awdlAvailabilityChanged) awdlAvailabilityChanged(awdlReceiver);
    const bool queued = (awdlRadio.queue != IWX_INVALID_QUEUE && com.txq[awdlRadio.queue].queued) ||
        (awdlRadio.dataQueueConfigured && com.txq[awdlRadio.dataQueue].queued) ||
        (awdlRadio.llwQueueConfigured && com.txq[awdlRadio.llwQueue].queued);
    if (queued) {
        const uint64_t epoch = dmaEpoch;
        const int error = iwx_flush_sta_tids(&com, RadioState::StationID, 0xffff);
        if (epoch != dmaEpoch) return ENXIO;
        awdlRadio.recordFailure(RadioState::FlushStation, error, IWX_TXPATH_FLUSH);
        if (error) { awdlRadio.cleanupError = error; return error; }
    }
    if (open) ++awdlRadio.dwellEnds;
    d = {};
    d.flushed = true;
    return 0;
}

int ItlIwx::serviceAWDLListener(uint16_t channel, uint64_t windowEndUS, RadioState::GrantPolicy policy)
{
    const uint64_t epoch = dmaEpoch;
    const unsigned station3QoSQueued =
        (awdlRadio.dataQueueConfigured ? unsigned(com.txq[awdlRadio.dataQueue].queued) : 0U) +
        (awdlRadio.llwQueueConfigured ? unsigned(com.txq[awdlRadio.llwQueue].queued) : 0U);
    const bool heavyStation3Queued =
        (awdlRadio.queue != IWX_INVALID_QUEUE && com.txq[awdlRadio.queue].queued != 0) ||
        station3QoSQueued > 2;
    const uint64_t closeLeadUS = policy.llwRealtime ? (heavyStation3Queued ? 1500ULL : 400ULL) : 3000ULL;
    awdlRadio.llwRealtime = policy.llwRealtime;
    auto &d = awdlRadio.dwell;
    awdlRadio.changing = true;
    int error = 0;
    uint16_t tuneChannel = 0;
    uint8_t tuneWidth = 20;
    const auto bss = com.sc_ic.ic_bss;
    const auto infra = reinterpret_cast<const iwx_node *>(bss);
    const auto phy = infra ? infra->in_phyctxt : nullptr;
    const auto infraChannel = phy ? phy->channel : nullptr;
    const uint32_t infraMAC = infra ? IWX_FW_CMD_ID_AND_COLOR(infra->in_id, infra->in_color) : 0;
    auto wanted = [&]() {
        if (epoch != dmaEpoch || awdlRadio.stopPending || !bss || !phy || com.sc_ic.ic_bss != bss ||
            com.sc_ic.ic_opmode != IEEE80211_M_STA || com.sc_ic.ic_state != IEEE80211_S_RUN ||
            com.ns_nstate != IEEE80211_S_RUN || !(com.sc_flags & IWX_FLAG_BINDING_ACTIVE)) return false;
        const auto current = reinterpret_cast<const iwx_node *>(com.sc_ic.ic_bss);
        return current->in_phyctxt == phy && phy->channel == infraChannel &&
            IWX_FW_CMD_ID_AND_COLOR(current->in_id, current->in_color) == infraMAC;
    };
    auto currentEnd = [&]() {
        return policy.windowEnd ? MIN(windowEndUS,
            policy.windowEnd(policy.context, awdlRadioNow(), policy.slot, channel)) : windowEndUS;
    };
    if (!wanted()) { error = ECANCELED; goto fail; }
    if (policy.unbindIdle) {
        if (!awdlRadio.idleUnbound &&
            (awdlRadio.binding || awdlRadio.station || awdlRadio.queue != IWX_INVALID_QUEUE)) {
            error = finishAWDLDwell();
            if (epoch != dmaEpoch) return ENXIO;
            if (error) goto fail;
            awdlRadio.idleUnbound = true;
            ++awdlRadio.idleUnbinds;
        }
        awdlRadio.changing = false;
        awdlRadio.lastError = 0;
        return 0;
    }
    if (awdlRadio.idleUnbound) {
        const uint16_t bindChannel = channel ? channel :
            (awdlRadio.channel ? awdlRadio.channel : 149);
        const uint8_t bindWidth = channel ? policy.widthMHz :
            (awdlWideChannelAllowed(bindChannel) ? 80 : 20);
        error = prepareAWDLListener(bindChannel, bindWidth);
        if (epoch != dmaEpoch) return ENXIO;
        if (error) goto fail;
        if (!wanted()) { error = ECANCELED; goto fail; }
        awdlRadio.channel = bindChannel;
        awdlRadio.listenerFilterKnown = false;
        awdlRadio.idleUnbound = false;
        ++awdlRadio.idleRebinds;
    }
    {
        // Park PHY 1 and TLC on the active 80 MHz channel across <= 2-slot (<= 140 ms)
        // pure-infra blocks ONLY when the schedule immediately returns to the same 80 MHz
        // channel (e.g. Sidecar Slots 14-15 [48, 48] bridging 149x5 -> 149x6 = 852 ms),
        // never parking over 2.4 GHz Ch 6 or dead-end infra slots before Ch 6, and never
        // parking during an active llw0 RealTimeGroup stream.
        const uint64_t parkNow = awdlRadioNow();
        const uint64_t effectiveEndUS = currentEnd();
        const uint64_t parkSpanStartUS = awdlRadio.parkStartUS ? awdlRadio.parkStartUS : parkNow;
        const bool shortInfraParkSpan = effectiveEndUS > parkNow && effectiveEndUS >= parkSpanStartUS &&
            effectiveEndUS - parkSpanStartUS <= RadioState::MaxParkWindowUS;
        const bool activeWideBase = !policy.llwRealtime && policy.bridgeInfraToWide &&
            channel && awdlRadio.channel && channel != awdlRadio.channel &&
            awdlRadio.widthMHz == 80 && awdlRadio.blockAckTX.configured &&
            awdlRadio.blockAckTX.phase == AirportAWDL::TXBlockAckQueue::Agreed &&
            (awdlRadio.heavyTransmitActive(parkNow) || awdlRadio.heavyTransmitActive(parkSpanStartUS));
        if (activeWideBase && shortInfraParkSpan &&
            infraChannel && infraChannel == &com.sc_ic.ic_channels[channel]) {
            if (!awdlRadio.parkStartUS) {
                awdlRadio.parkStartUS = parkNow;
                ++awdlRadio.blockAckTXStats.infraParks;
            }
            channel = 0;
        }
        if (channel)
            awdlRadio.parkStartUS = 0;
        const uint16_t homeChannel = policy.idleHomeChannel ? policy.idleHomeChannel :
            ((policy.parkIdleOnInfra && infraChannel) ? uint16_t(infraChannel - com.sc_ic.ic_channels) : 0);
        tuneChannel = channel ? channel :
            ((!awdlRadio.parkStartUS && homeChannel &&
              awdlChannelAllowed(homeChannel, 20)) ? homeChannel : 0);
        tuneWidth = channel ? policy.widthMHz : 20;
    }
    if (tuneChannel && (tuneChannel != awdlRadio.channel || tuneWidth != awdlRadio.widthMHz)) {
        // A schedule change keeps MAC ownership and BA sequence state. Only shutdown
        // removes stations and queues; a retune drains ordinary TX and pauses QoS TX.
        error = closeAWDLListenerWindow();
        if (epoch != dmaEpoch) return ENXIO;
        if (error) goto fail;
        if (!wanted()) { error = ECANCELED; goto fail; }
        const uint16_t savedChannel = channel;
        channel = tuneChannel;
        const uint16_t previousChannel = awdlRadio.channel;
        const uint8_t previousWidth = awdlRadio.widthMHz;
        if (tuneWidth < previousWidth) {
            if (awdlRadio.blockAckTX.configured && awdlRadio.blockAckTX.epoch == epoch &&
                awdlRadio.blockAckTX.widthMHz > tuneWidth) {
                error = downgradeAWDLTXRates(tuneWidth, channel);
                if (epoch != dmaEpoch) return ENXIO;
                if (error) goto fail;
                if (!wanted()) { error = ECANCELED; goto fail; }
            }
            awdlRadio.widthMHz = tuneWidth;
            error = configureAWDLPeerWidth();
            awdlRadio.widthMHz = previousWidth;
            if (epoch != dmaEpoch) return ENXIO;
            if (error) goto fail;
            if (!wanted()) { error = ECANCELED; goto fail; }
        }
        error = awdlPHY(channel, tuneWidth);
        if (epoch != dmaEpoch) return ENXIO;
        if (error) goto fail;
        if (!wanted()) { error = ECANCELED; goto fail; }
        awdlRadio.channel = channel;
        if (savedChannel && channel <= 14) awdlRadio.lastWideSocialVisitUS = awdlRadioNow();
        if (tuneWidth > previousWidth) {
            error = configureAWDLPeerWidth();
            if (epoch != dmaEpoch) return ENXIO;
            if (error) goto fail;
            if (!wanted()) { error = ECANCELED; goto fail; }
        }
        const bool wantsPromisc = awdlRadio.listenerWantsPromisc(awdlHasRXBA(),
            infraChannel != &com.sc_ic.ic_channels[awdlRadio.channel], awdlRadioNow());
        if (!awdlRadio.listenerFilterKnown || !previousChannel ||
            (previousChannel <= 14) != (channel <= 14) ||
            awdlRadio.listenerPromisc != wantsPromisc) {
            error = awdlMAC(IWX_FW_CTXT_ACTION_MODIFY);
            if (epoch != dmaEpoch) return ENXIO;
            if (error) goto fail;
        }
        channel = savedChannel;
    } else if (!awdlRadio.listenerFilterKnown || awdlRadio.listenerPromisc !=
        awdlRadio.listenerWantsPromisc(awdlHasRXBA(),
            infraChannel != &com.sc_ic.ic_channels[awdlRadio.channel], awdlRadioNow())) {
        error = awdlMAC(IWX_FW_CTXT_ACTION_MODIFY);
        if (epoch != dmaEpoch) return ENXIO;
        if (error) goto fail;
    }
    if (!wanted()) { error = ECANCELED; goto fail; }
    windowEndUS = currentEnd();
    // A parent clock update may relabel the current run of same-channel slots.
    // Retuning above already closed the dwell on a channel or width change.
    if (d.running && (!channel || awdlRadioNow() >= d.windowEndUS)) {
        error = closeAWDLListenerWindow();
        if (epoch != dmaEpoch) return ENXIO;
        if (error) goto fail;
        if (!wanted()) { error = ECANCELED; goto fail; }
    }
    if (channel && policy.llwQueue && !awdlRadio.llwQueueConfigured && !awdlRadio.llwQueueFailed &&
        currentEnd() > awdlRadioNow() + 8000) {
        const int queue = iwx_tvqm_enable_txq(&com, RadioState::LLWTID, 0, 64,
            RadioState::StationID, &awdlRadio.llwQueueSetup);
        if (epoch != dmaEpoch) return ENXIO;
        if (queue >= 0) {
            awdlRadio.llwQueue = queue;
            awdlRadio.llwQueueConfigured = true;
        } else if (queue != -ENOMEM) {
            // IWX_STA_GENERAL_PURPOSE (StationID 3) rejects QoS TID 7 in QuZ firmware;
            // latch per radio lifecycle so tick() never repeats SCD_QUEUE_CFG or deferRecycling().
            awdlRadio.llwQueueFailed = true;
        }
    }
    if (channel && (policy.dataQueue || (policy.llwQueue && awdlRadio.llwQueueFailed)) &&
        !awdlRadio.dataQueueConfigured && currentEnd() > awdlRadioNow() + 8000) {
        const int queue = iwx_tvqm_enable_txq(&com, RadioState::DataTID, 0, 64,
            RadioState::StationID, &awdlRadio.dataQueueSetup);
        if (epoch != dmaEpoch) return ENXIO;
        if (queue < 0) {
            error = -queue;
            awdlRadio.recordFailure(RadioState::AllocateQueue, error, IWX_SCD_QUEUE_CFG,
                awdlRadio.dataQueueSetup.responseFlags);
            goto fail;
        }
        awdlRadio.dataQueue = queue;
        awdlRadio.dataQueueConfigured = true;
    }
    if (!wanted()) { error = ECANCELED; goto fail; }
    windowEndUS = currentEnd();
    if (!channel || windowEndUS <= awdlRadioNow() + closeLeadUS) {
        error = closeAWDLListenerWindow();
        if (epoch != dmaEpoch) return ENXIO;
        if (error) goto fail;
        if (!wanted()) { error = ECANCELED; goto fail; }
    } else if (!d.running) {
        d = {};
        d.eawEndUS = windowEndUS;
        d.windowEndUS = windowEndUS - closeLeadUS;
        d.slot = policy.slot;
        d.running = true;
        ++awdlRadio.dwellStarts;
    } else {
        if (d.slot != policy.slot) {
            ++awdlRadio.llwWindowContinuations;
            d.slot = policy.slot;
        }
        d.eawEndUS = windowEndUS;
        d.windowEndUS = windowEndUS - closeLeadUS;
    }
    awdlRadio.changing = false;
    awdlRadio.lastError = 0;
    return 0;
fail:
    awdlRadio.changing = false;
    awdlRadio.fault = error != ECANCELED;
    stopAWDLRadio();
    if (epoch != dmaEpoch) return ENXIO;
    awdlRadio.lastError = error;
    return error;
}

int ItlIwx::serviceAWDLChannel(uint16_t channel, uint64_t windowEndUS, RadioState::GrantPolicy policy)
{
    if (!getMainWorkLoop()->inGate()) return EBUSY;
    if (awdlRadio.changing) return EBUSY;
    if (!awdlRadio.ready || awdlRadio.fault || awdlRadio.stopPending) return ENETDOWN;
    if (channel && !awdlChannelAllowed(channel, policy.widthMHz)) return EACCES;
    if (channel && policy.widthMHz == 80 && !awdlWideChannelAllowed(channel))
        policy.widthMHz = 20;
    if (awdlRadio.listener) return serviceAWDLListener(channel, windowEndUS, policy);
    awdlRadio.llwRealtime = false;
    if (policy.widthMHz != 20) return EOPNOTSUPP;
    auto &d = awdlRadio.dwell;
    const uint64_t epoch = dmaEpoch;
    int error = 0;
    struct iwx_add_sta_cmd station = {};
    iwx_session_prot_cmd session = {};
    uint32_t status = IWX_ADD_STA_SUCCESS;
    uint64_t now;
    bool longGrant;
    auto associated = [&]() {
        return com.sc_ic.ic_opmode == IEEE80211_M_STA && com.sc_ic.ic_state == IEEE80211_S_RUN &&
            com.ns_nstate == IEEE80211_S_RUN && com.sc_ic.ic_bss && (com.sc_flags & IWX_FLAG_BINDING_ACTIVE);
    };
    auto prepareDataQueues = [&](uint64_t endUS) -> int {
        auto canAllocate = [&]() {
            const uint64_t at = awdlRadioNow();
            return at < endUS && endUS - at > 8000 &&
                (!d.requested || (d.running && !d.done && !d.failed && !d.cancelling));
        };
        if (policy.dataQueue && !awdlRadio.dataQueueConfigured && canAllocate()) {
            const int queue = iwx_tvqm_enable_txq(&com, RadioState::DataTID, 0, 64,
                RadioState::StationID, &awdlRadio.dataQueueSetup);
            if (epoch != dmaEpoch) return ENXIO;
            if (queue < 0) {
                awdlRadio.recordFailure(RadioState::AllocateQueue, -queue, IWX_SCD_QUEUE_CFG,
                    awdlRadio.dataQueueSetup.responseFlags);
                return -queue;
            }
            awdlRadio.dataQueue = queue;
            awdlRadio.dataQueueConfigured = true;
        }
        return 0;
    };
    if (d.requested && !d.done && !d.cancelling && !d.failed && awdlRadio.channel == channel &&
        awdlRadioNow() < d.eawEndUS) {
        // A peer-backed grant keeps its original peer and window across ticks.
        if (d.peer.valid) windowEndUS = MIN(windowEndUS,
            policy.peerEnd ? policy.peerEnd(policy.context, awdlRadioNow(), d.peer, false) : 0);
        const bool shortened = windowEndUS < d.windowEndUS + 3000;
        d.eawEndUS = MIN(d.eawEndUS, windowEndUS);
        d.windowEndUS = MIN(d.windowEndUS, windowEndUS > 3000 ? windowEndUS - 3000 : 0);
        if (d.slot == policy.slot && !shortened &&
            (d.durationTU <= 16 || ((d.peer.valid || policy.isolated) && associated()))) {
            // Traffic can arrive after the grant starts. Add its queues without
            // interrupting the current session or its outstanding packets.
            awdlRadio.changing = true;
            error = prepareDataQueues(d.windowEndUS);
            if (epoch != dmaEpoch) return ENXIO;
            if (error) goto fail;
            awdlRadio.changing = false;
            return 0;
        }
    }
    awdlRadio.changing = true;
    error = finishAWDLDwell();
    if (epoch != dmaEpoch) return ENXIO;
    if (error == EINPROGRESS) {
        awdlRadio.changing = false;
        return 0;
    }
    if (error) goto fail;
    now = awdlRadioNow();
    if (!channel || windowEndUS <= now + 8000 || now < awdlRadio.nextGrantUS) {
        awdlRadio.changing = false;
        return 0;
    }
    error = awdlPHY(channel);
    if (epoch != dmaEpoch) return ENXIO;
    if (error) goto fail;
    awdlRadio.channel = channel;
    awdlRadio.binding = true;
    error = awdlBinding(true);
    if (epoch != dmaEpoch) return ENXIO;
    if (error) goto fail;
    station.sta_id = RadioState::StationID;
    station.station_type = IWX_STA_GENERAL_PURPOSE;
    station.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(RadioState::MacID, 0));
    station.tid_disable_tx = htole16(0xffff);
    memset(station.addr, 0xff, sizeof(station.addr));
    error = iwx_send_cmd_pdu_status(&com, IWX_ADD_STA, sizeof(station), &station, &status);
    if (epoch != dmaEpoch) return ENXIO;
    awdlRadio.recordFailure(RadioState::AddStation, error, IWX_ADD_STA);
    awdlRadio.station = error || (status & IWX_ADD_STA_STATUS_MASK) == IWX_ADD_STA_SUCCESS;
    if (!error && !awdlRadio.station) error = EIO;
    awdlRadio.recordFailure(RadioState::AddStation, error, IWX_ADD_STA, status);
    if (error) goto fail;
    error = iwx_tvqm_enable_txq(&com, IWX_MGMT_TID, 0, 64, RadioState::StationID,
        &awdlRadio.queueSetup);
    if (epoch != dmaEpoch) return ENXIO;
    if (error < 0) {
        error = -error;
        awdlRadio.recordFailure(RadioState::AllocateQueue, error, IWX_SCD_QUEUE_CFG,
            awdlRadio.queueSetup.responseFlags);
        goto fail;
    }
    awdlRadio.queue = error;
    error = prepareDataQueues(windowEndUS);
    if (epoch != dmaEpoch) return ENXIO;
    if (error) goto fail;
    now = awdlRadioNow();
    longGrant = policy.isolated && policy.dataDemand && associated() && policy.isolatedEnd;
    if (longGrant) {
        const uint64_t currentEnd = policy.isolatedEnd(policy.context, now, policy.slot, channel);
        windowEndUS = MIN(windowEndUS, currentEnd);
    }
    if (!longGrant && policy.peer.valid && policy.peerEnd && associated()) {
        longGrant = true;
        windowEndUS = MIN(windowEndUS, policy.peerEnd(policy.context, now, policy.peer, true));
        d.peer = policy.peer;
    }
    if (windowEndUS <= now + 8000) {
        error = finishAWDLDwell();
        if (epoch != dmaEpoch) return ENXIO;
        if (error) goto fail;
        awdlRadio.changing = false;
        return 0;
    }
    d.requested = true;
    d.eawEndUS = windowEndUS;
    d.slot = policy.slot;
    d.durationTU = uint32_t(MIN(uint64_t(longGrant ? 64 : 16), (windowEndUS - now) / 1024));
    d.windowEndUS = MIN(windowEndUS, now + uint64_t(d.durationTU) * 1024) - 3000;
    d.confID = IWX_SESSION_PROTECT_CONF_P2P_GO_NEGOTIATION;
    d.apCreditUS = awdlRadio.availableAPCredit(now);
    awdlRadio.session.addUS = now;
    awdlRadio.session.startUS = awdlRadio.session.cancelAckUS = 0;
    awdlRadio.session.flushAckUS = awdlRadio.session.endUS = 0;
    awdlRadio.session.cancelCompletedUS = 0;
    session.id_and_color = htole32(IWX_FW_CMD_ID_AND_COLOR(RadioState::MacID, 0));
    session.action = htole32(IWX_FW_CTXT_ACTION_ADD);
    session.conf_id = htole32(d.confID);
    session.duration_tu = htole32(d.durationTU);
    error = iwx_send_cmd_pdu(&com,
        iwx_cmd_id(IWX_SESSION_PROTECTION_CMD, IWX_MAC_CONF_GROUP, 0), 0, sizeof(session), &session);
    if (epoch != dmaEpoch) return ENXIO;
    awdlRadio.recordFailure(RadioState::StartSession, error,
        iwx_cmd_id(IWX_SESSION_PROTECTION_CMD, IWX_MAC_CONF_GROUP, 0));
    if (error) goto fail;
    if (d.durationTU > 16 || d.peer.valid) {
        now = awdlRadioNow();
        const uint64_t currentEnd = d.peer.valid ? policy.peerEnd(policy.context, now, d.peer, false) :
            policy.isolatedEnd(policy.context, now, policy.slot, channel);
        if (!associated() || now >= d.eawEndUS || currentEnd < d.windowEndUS + 3000) {
            error = finishAWDLDwell();
            if (epoch != dmaEpoch) return ENXIO;
            if (error) goto fail;
        } else {
            d.eawEndUS = MIN(d.eawEndUS, currentEnd);
        }
    }
    awdlRadio.changing = false;
    awdlRadio.lastError = 0;
    return 0;
fail:
    // A failed command may have reached firmware. Keep owned DMA until cleanup or reset.
    awdlRadio.changing = false;
    awdlRadio.fault = true;
    stopAWDLRadio();
    if (epoch != dmaEpoch) return ENXIO;
    awdlRadio.lastError = error;
    return error;
}

int ItlIwx::awdlRemoveQueue()
{
    if (awdlRadio.queue == IWX_INVALID_QUEUE && !awdlRadio.dataQueueConfigured &&
        !awdlRadio.llwQueueConfigured) return 0;
    // The 22000 ADD_STA/SCD_QUEUE_CFG API releases queues when REMOVE_STA
    // completes. A zero-flags SCD_QUEUE_CFG is not a queue-disable command.
    if (awdlRadio.station) return EBUSY;
    if (awdlRadio.llwQueueConfigured) {
        auto ring = &com.txq[awdlRadio.llwQueue];
        iwx_reset_tx_ring(&com, ring);
        iwx_free_tx_ring(&com, ring);
        awdlRadio.llwQueueConfigured = false;
        awdlRadio.llwQueue = IWX_INVALID_QUEUE;
    }
    awdlRadio.llwQueueFailed = false;
    if (awdlRadio.dataQueueConfigured) {
        auto ring = &com.txq[awdlRadio.dataQueue];
        iwx_reset_tx_ring(&com, ring);
        iwx_free_tx_ring(&com, ring);
        awdlRadio.dataQueueConfigured = false;
        awdlRadio.dataQueue = IWX_INVALID_QUEUE;
    }
    if (awdlRadio.queue != IWX_INVALID_QUEUE) {
        auto ring = &com.txq[awdlRadio.queue];
        iwx_reset_tx_ring(&com, ring);
        iwx_free_tx_ring(&com, ring);
    }
    awdlRadio.queue = IWX_INVALID_QUEUE;
    return 0;
}

int ItlIwx::stopAWDLRadio()
{
    if (!getMainWorkLoop()->inGate())
        return getMainCommandGate()->runAction(
            [](OSObject *, void *self, void *, void *, void *) -> IOReturn {
                return static_cast<ItlIwx *>(self)->stopAWDLRadio();
            }, this);
#if DEBUG
    awdlCompareEndUS = awdlCompareArmSubmitted[0] = awdlCompareArmSubmitted[1] = awdlCompareBTOverrides = 0;
#endif
    if (awdlRadio.changing) {
        if (awdlRadio.listener) awdlRadio.stopPending = true;
        return EBUSY;
    }
    awdlRadio.ready = false;
    if (awdlRadio.cleanupError) {
        awdlRadio.stopPending = false;
        awdlRadio.fault = true;
        return awdlRadio.cleanupError;
    }
    awdlRadio.changing = true;
    awdlRadio.stopPending = true;
    const uint64_t epoch = dmaEpoch;
    int error = 0;
    // No firmware command was submitted until awake was set.
    if (!awdlRadio.awake) {
        awdlRadio.queue = IWX_INVALID_QUEUE;
        awdlRadio.dataQueue = IWX_INVALID_QUEUE;
        awdlRadio.dataQueueConfigured = false;
        awdlRadio.llwQueue = IWX_INVALID_QUEUE;
        awdlRadio.llwQueueConfigured = false;
        awdlRadio.llwQueueFailed = false;
        awdlRadio.llwRealtime = false;
        awdlRadio.changing = false;
        awdlRadio.stopPending = false;
        return 0;
    }
    error = finishAWDLDwell();
    if (epoch != dmaEpoch) return ENXIO;
    if (error == EINPROGRESS) {
        awdlRadio.changing = false;
        return error;
    }
    if (error) goto fail;
    if (awdlRadio.mac) {
        error = awdlMAC(IWX_FW_CTXT_ACTION_REMOVE);
        if (epoch != dmaEpoch) return ENXIO;
        if (error) goto fail;
        awdlRadio.mac = false;
    }
    error = iwx_power_update_device(&com);
    if (epoch != dmaEpoch) return ENXIO;
    awdlRadio.recordFailure(RadioState::RestorePower, error, IWX_POWER_TABLE_CMD);
    if (error) goto fail;
    awdlRadio.awake = false;
    awdlRadio.idleUnbound = false;
    awdlRadio.stopPending = false;
    awdlRadio.changing = false;
    return 0;
fail:
    // Keep uncertain firmware-owned buffers until the device reset stops DMA.
    awdlRadio.fault = true;
    awdlRadio.stopPending = false;
    awdlRadio.cleanupError = error;
    awdlRadio.changing = false;
    awdlRadio.lastError = error;
    return error;
}

void ItlIwx::resetAWDLRadio()
{
    resetAWDLBlockAck();
#if DEBUG
    awdlCompareEndUS = awdlCompareArmSubmitted[0] = awdlCompareArmSubmitted[1] = awdlCompareBTOverrides = 0;
    awdlReceiveAllData = awdlReceiveFilterKnown = false;
    awdlReceiveFilterCommandError = 0;
    awdlReceiveFilterCleanupResult = kIOReturnSuccess;
#endif
    awdlRadio.ready = awdlRadio.changing = awdlRadio.fault = awdlRadio.stopPending = false;
    awdlRadio.cleanupError = 0;
    awdlRadio.mac = awdlRadio.binding = awdlRadio.station = awdlRadio.awake = false;
    awdlRadio.queue = IWX_INVALID_QUEUE;
    awdlRadio.dataQueue = IWX_INVALID_QUEUE;
    awdlRadio.dataQueueConfigured = false;
    awdlRadio.llwQueue = IWX_INVALID_QUEUE;
    awdlRadio.llwQueueConfigured = false;
    awdlRadio.llwQueueFailed = false;
    awdlRadio.llwRealtime = false;
    awdlRadio.idleUnbound = false;
    awdlRadio.lastWideSocialVisitUS = 0;
    awdlRadio.parkStartUS = 0;
    awdlRadio.idleEligibleSinceUS = 0;
    awdlRadio.lastIdleProbeUS = 0;
    awdlRadio.channel = 0;
    awdlRadio.widthMHz = 0;
    awdlRadio.listener = awdlRadio.listenerPromisc = awdlRadio.listenerFilterKnown = false;
    awdlRadio.listenerFilterFlags = 0;
    awdlRadio.lastRXPayloadUS = 0;
    awdlRadio.lastHeavyRXPayloadUS = 0;
    awdlRadio.phyReceiveEpoch = 0;
    awdlRadio.phyReceiveAntennas = 0;
    awdlRadio.txAntennaMask = 0;
    awdlRadio.dwell = {};
    awdlRadio.nextGrantUS = 0;
    awdlRadio.apTime = {};
    if (awdlAvailabilityChanged) awdlAvailabilityChanged(awdlReceiver);
    getMainCommandGate()->commandWakeup(&awdlRadio.dwell.done);
}

int ItlIwx::addAWDLBlockAckPeer(unsigned index, const uint8_t *address)
{
    using namespace AirportAWDL;
    if (index >= BlockAckState::PeerCount) return EINVAL;
    auto &peer = awdlBA.peers[index];
    if (peer.station) return peer.epoch == dmaEpoch && sameAddress(peer.address, address) ? 0 : EBUSY;
    const uint64_t epoch = dmaEpoch;
    struct iwx_add_sta_cmd cmd = {};
    cmd.sta_id = BlockAckState::FirstStation + index;
    cmd.station_type = IWX_STA_LINK;
    cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(RadioState::MacID, 0));
    copyAddress(cmd.addr, address);
    cmd.tid_disable_tx = htole16(0xffff);
    const bool mimo = iwx_mimo_enabled(&com) && (iwx_fw_valid_rx_ant(&com) & 3) == 3 &&
        (iwx_fw_valid_tx_ant(&com) & 3) == 3;
    cmd.station_flags = htole32((awdlRadio.widthMHz == 80 ? IWX_STA_FLG_FAT_EN_80MHZ : IWX_STA_FLG_FAT_EN_20MHZ) |
        (mimo ? IWX_STA_FLG_MIMO_EN_MIMO2 : IWX_STA_FLG_MIMO_EN_SISO) |
        IWX_STA_FLG_MAX_AGG_SIZE_256K);
    cmd.station_flags_msk = htole32(IWX_STA_FLG_FAT_EN_MSK | IWX_STA_FLG_MIMO_EN_MSK |
        IWX_STA_FLG_MAX_AGG_SIZE_MSK | IWX_STA_FLG_AGG_MPDU_DENS_MSK);
    peer.station = true;
    peer.epoch = epoch;
    copyAddress(peer.address, address);
    uint32_t status = IWX_ADD_STA_SUCCESS;
    const int error = iwx_send_cmd_pdu_status(&com, IWX_ADD_STA, sizeof(cmd), &cmd, &status);
    if (epoch != dmaEpoch) return ENXIO;
    if (!error && (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS) peer = {};
    return error ? error : (peer.station ? 0 : EIO);
}

void ItlIwx::noteAWDLTXBlockAck(const uint8_t *address, size_t bytes, bool acceptedRX)
{
    if (!awdlRadio.listener || !awdlRadio.ready || awdlRadio.stopPending || awdlRadio.fault) return;
    const uint64_t now = awdlRadioNow();
    const auto *peer = awdlPeers.find(address, now);
    const bool queueEmpty = !awdlRadio.blockAckTX.configured || !com.txq[awdlRadio.blockAckTX.queue].queued;
    if (peer && peer->txHTMcs(now)) awdlRadio.blockAckTX.demand(address, bytes, now, acceptedRX, queueEmpty);
}

uint8_t ItlIwx::awdlTXAntenna()
{
    uint8_t mask = awdlRadio.txAntennaMask ? awdlRadio.txAntennaMask : 1U << com.sc_mgmt_last_antenna_idx;
    const auto *peer = awdlPeers.find(awdlRadio.blockAckTX.peer, awdlRadioNow());
    if (iwx_mimo_enabled(&com) && peer && peer->txHTMcs2(awdlRadioNow()) &&
        (iwx_fw_valid_tx_ant(&com) & 3) == 3 && (iwx_fw_valid_rx_ant(&com) & 3) == 3) mask = 3;
    if (awdlRadio.channel && awdlRadio.channel <= 14 && btCoex.enabled && com.sc_cfg) {
        const uint8_t available = com.sc_cfg->non_shared_ant & btCoex.txMask;
        if (available) mask = available;
    }
    mask &= iwx_fw_valid_tx_ant(&com) & 3;
    return mask;
}

int ItlIwx::configureAWDLPeerWidth()
{
    const uint64_t epoch = dmaEpoch;
    for (unsigned index = 0; index < AirportAWDL::BlockAckState::PeerCount; ++index) {
        const auto &peer = awdlBA.peers[index];
        if (!peer.station || peer.epoch != epoch) continue;
        struct iwx_add_sta_cmd cmd = {};
        cmd.sta_id = AirportAWDL::BlockAckState::FirstStation + index;
        cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(RadioState::MacID, 0));
        cmd.add_modify = IWX_STA_MODE_MODIFY;
        cmd.station_flags_msk = htole32(IWX_STA_FLG_FAT_EN_MSK);
        cmd.station_flags = htole32(
            awdlRadio.widthMHz == 80 ? IWX_STA_FLG_FAT_EN_80MHZ : IWX_STA_FLG_FAT_EN_20MHZ);
        uint32_t status = IWX_ADD_STA_SUCCESS;
        const int error = iwx_send_cmd_pdu_status(&com, IWX_ADD_STA, sizeof(cmd), &cmd, &status);
        if (epoch != dmaEpoch) return ENXIO;
        if (error || (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS)
            return error ? error : EIO;
        if (awdlRadio.stopPending) return ECANCELED;
    }
    return 0;
}

int ItlIwx::configureAWDLTXStation(bool enabled, bool paused)
{
    auto &owner = awdlRadio.blockAckTX;
    if (!owner.configured || owner.epoch != dmaEpoch) return ENXIO;
    const uint64_t epoch = dmaEpoch, cookie = owner.cookie;
    struct iwx_add_sta_cmd cmd = {};
    cmd.sta_id = owner.station;
    cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(RadioState::MacID, 0));
    cmd.add_modify = IWX_STA_MODE_MODIFY;
    cmd.modify_mask = IWX_STA_MODIFY_TID_DISABLE_TX;
    cmd.tid_disable_tx = htole16(enabled ? uint16_t(0xffff & ~(1U << owner.tid)) : 0xffff);
    cmd.station_flags_msk = htole32(IWX_STA_FLG_DISABLE_TX);
    cmd.station_flags = htole32(paused ? IWX_STA_FLG_DISABLE_TX : 0);
    uint32_t status = IWX_ADD_STA_SUCCESS;
    const int error = iwx_send_cmd_pdu_status(&com, IWX_ADD_STA, sizeof(cmd), &cmd, &status);
    if (epoch != dmaEpoch || cookie != owner.cookie || !owner.configured) return ENXIO;
    if (error || (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS) return error ? error : EIO;
    if (owner.paused != paused) {
        if (paused) ++owner.pauses;
        else { ++owner.resumes; owner.lastProgressUS = awdlRadioNow(); }
    }
    owner.enabled = enabled;
    owner.paused = paused;
    if (paused) owner.windowEndUS = 0;
    return 0;
}

int ItlIwx::configureAWDLTXRates()
{
    auto &owner = awdlRadio.blockAckTX;
    const uint64_t epoch = dmaEpoch, cookie = owner.cookie, now = awdlRadioNow();
    const uint8_t mcs = awdlTXMcs(now);
    const uint8_t antenna = awdlTXAntenna();
    const uint8_t mcs2 = antenna == 3 ? awdlTXMcs(now, 1) : 0;
    const uint8_t width = awdlTXWidth(now);
    const auto *peer = awdlPeers.find(owner.peer, now);
    const uint16_t htCap = peer ? peer->txHTCap(now) : 0;
    const uint8_t floorMbps = awdlMinRateMbps;
    if (!owner.configured || (owner.enabled && !owner.paused) || !mcs || !antenna) return EAGAIN;
    struct iwx_tlc_config_cmd_v4 cfg = {};
    const bool useVHT = width == 80 || awdlRadio.channel > 14;
    cfg.sta_id = owner.station;
    cfg.mode = useVHT ? IWX_TLC_MNG_MODE_VHT : IWX_TLC_MNG_MODE_HT;
    cfg.max_ch_width = width == 80 ? IWX_TLC_MNG_CH_WIDTH_80MHZ : IWX_TLC_MNG_CH_WIDTH_20MHZ;
    cfg.chains = antenna;
    if (htCap & AirportAWDL::HTCapSGI20)
        cfg.sgi_ch_width_supp |= (1U << IWX_TLC_MNG_CH_WIDTH_20MHZ);
    if (width == 80 && (htCap & AirportAWDL::HTCapSGI40))
        cfg.sgi_ch_width_supp |= (1U << IWX_TLC_MNG_CH_WIDTH_40MHZ) |
            (1U << IWX_TLC_MNG_CH_WIDTH_80MHZ);
    uint16_t tlcFlags = 0;
    if (htCap & AirportAWDL::HTCapLDPC)
        tlcFlags |= IWX_TLC_MNG_CFG_FLAGS_LDPC_MSK;
    if (antenna == 3 && (htCap & AirportAWDL::HTCapRxSTBCMask))
        tlcFlags |= IWX_TLC_MNG_CFG_FLAGS_STBC_MSK;
    cfg.flags = htole16(tlcFlags);
    const uint8_t rates[] = {6, 9, 12, 18, 24, 36, 48, 54};
    uint16_t legacy = 0;
    uint8_t minimum = 0;
    for (unsigned i = 0; i < sizeof(rates); ++i) if (rates[i] >= floorMbps) {
        legacy |= 1U << (i + 4);
        if (!minimum) minimum = rates[i];
    }
    if (!minimum) return EINVAL;
    cfg.non_ht_rates = htole16(legacy);
    const uint16_t fullMask = width == 80 ? 0x03ff : (useVHT ? 0x01ff : 0x00ff);
    const uint16_t rate1 = (mcs == 0xf0) ? fullMask : uint16_t(mcs);
    const uint16_t rate2 = (mcs2 == 0xf0) ? fullMask : uint16_t(mcs2);
    cfg.ht_rates[IWX_TLC_NSS_1][IWX_TLC_MCS_PER_BW_80] = htole16(rate1);
    cfg.ht_rates[IWX_TLC_NSS_2][IWX_TLC_MCS_PER_BW_80] = htole16(rate2);
    cfg.max_mpdu_len = htole16(3839);
    cfg.max_tx_op = htole16(4000);
    const uint32_t command = iwx_cmd_id(IWX_TLC_MNG_CONFIG_CMD, IWX_DATA_PATH_GROUP, 0);
    const uint8_t version = iwx_lookup_cmd_ver(&com, IWX_DATA_PATH_GROUP, IWX_TLC_MNG_CONFIG_CMD);
    int error;
    if (version == 4) error = iwx_send_cmd_pdu(&com, command, 0, sizeof(cfg), &cfg);
    else if (version < 4 || version == IWX_FW_CMD_VER_UNKNOWN) {
        struct iwl_tlc_config_cmd_v3 old = {};
        old.sta_id = cfg.sta_id; old.mode = cfg.mode; old.max_ch_width = cfg.max_ch_width;
        old.chains = cfg.chains; old.flags = cfg.flags; old.non_ht_rates = cfg.non_ht_rates;
        for (unsigned nss = 0; nss < IWX_TLC_NSS_MAX; ++nss)
            memcpy(old.ht_rates[nss], cfg.ht_rates[nss], sizeof(old.ht_rates[nss]));
        old.max_mpdu_len = cfg.max_mpdu_len;
        old.sgi_ch_width_supp = cfg.sgi_ch_width_supp;
        old.max_tx_op = htole32(4000);
        const size_t size = version < 3 || version == IWX_FW_CMD_VER_UNKNOWN ? sizeof(old) - 4 : sizeof(old);
        error = iwx_send_cmd_pdu(&com, command, 0, size, &old);
    } else return EOPNOTSUPP;
    if (epoch != dmaEpoch || cookie != owner.cookie || !owner.configured) return ENXIO;
    if (!error) {
        owner.mcs = mcs; owner.antenna = antenna;
        owner.mcs2 = mcs2; owner.widthMHz = width; owner.tlcMode = cfg.mode;
        owner.htCap = htCap; owner.sgiMask = cfg.sgi_ch_width_supp; owner.tlcFlags = tlcFlags;
        owner.floorMbps = floorMbps; owner.legacyMbps = minimum;
        ++awdlRadio.blockAckTXStats.tlcConfigs;
    }
    return error;
}

int ItlIwx::downgradeAWDLTXRates(uint8_t targetWidthMHz, uint16_t targetChannel)
{
    auto &owner = awdlRadio.blockAckTX;
    if (!owner.configured || owner.epoch != dmaEpoch || owner.widthMHz <= targetWidthMHz) return 0;
    if (owner.enabled && !owner.paused) return EAGAIN;
    const uint64_t epoch = dmaEpoch, cookie = owner.cookie;
    const uint16_t chan = targetChannel ? targetChannel : awdlRadio.channel;
    const uint8_t width = targetWidthMHz == 80 ? 80 : 20;
    const bool useVHT = width == 80 || chan > 14;
    uint8_t antenna = owner.antenna ? owner.antenna :
        (awdlRadio.txAntennaMask ? awdlRadio.txAntennaMask : 1);
    if (chan && chan <= 14 && btCoex.enabled && com.sc_cfg) {
        const uint8_t available = com.sc_cfg->non_shared_ant & btCoex.txMask &
            (iwx_fw_valid_tx_ant(&com) & 3);
        if (available) antenna = available;
    }
    const uint8_t mcs = owner.mcs ? owner.mcs : 0xf0;
    const uint8_t mcs2 = antenna == 3 ? (owner.mcs2 ? owner.mcs2 : 0xf0) : 0;
    const uint8_t floorMbps = owner.floorMbps ? owner.floorMbps : awdlMinRateMbps;
    struct iwx_tlc_config_cmd_v4 cfg = {};
    cfg.sta_id = owner.station;
    cfg.mode = useVHT ? IWX_TLC_MNG_MODE_VHT : IWX_TLC_MNG_MODE_HT;
    cfg.max_ch_width = width == 80 ? IWX_TLC_MNG_CH_WIDTH_80MHZ : IWX_TLC_MNG_CH_WIDTH_20MHZ;
    cfg.chains = antenna;
    cfg.sgi_ch_width_supp = width == 80 ? owner.sgiMask :
        uint8_t(owner.sgiMask & ~((1U << IWX_TLC_MNG_CH_WIDTH_40MHZ) |
                                  (1U << IWX_TLC_MNG_CH_WIDTH_80MHZ)));
    const uint16_t tlcFlags = antenna == 3 ? owner.tlcFlags :
        uint16_t(owner.tlcFlags & ~IWX_TLC_MNG_CFG_FLAGS_STBC_MSK);
    cfg.flags = htole16(tlcFlags);
    const uint8_t rates[] = {6, 9, 12, 18, 24, 36, 48, 54};
    uint16_t legacy = 0;
    for (unsigned i = 0; i < sizeof(rates); ++i)
        if (rates[i] >= floorMbps) legacy |= 1U << (i + 4);
    if (!legacy) legacy = 1U << 4;
    cfg.non_ht_rates = htole16(legacy);
    const uint16_t fullMask = width == 80 ? 0x03ff : (useVHT ? 0x01ff : 0x00ff);
    const uint16_t rate1 = (mcs == 0xf0) ? fullMask : uint16_t(mcs);
    const uint16_t rate2 = (mcs2 == 0xf0) ? fullMask : uint16_t(mcs2);
    cfg.ht_rates[IWX_TLC_NSS_1][IWX_TLC_MCS_PER_BW_80] = htole16(rate1);
    cfg.ht_rates[IWX_TLC_NSS_2][IWX_TLC_MCS_PER_BW_80] = htole16(rate2);
    cfg.max_mpdu_len = htole16(3839);
    cfg.max_tx_op = htole16(4000);
    const uint32_t command = iwx_cmd_id(IWX_TLC_MNG_CONFIG_CMD, IWX_DATA_PATH_GROUP, 0);
    const uint8_t version = iwx_lookup_cmd_ver(&com, IWX_DATA_PATH_GROUP, IWX_TLC_MNG_CONFIG_CMD);
    int error;
    if (version == 4) error = iwx_send_cmd_pdu(&com, command, 0, sizeof(cfg), &cfg);
    else if (version < 4 || version == IWX_FW_CMD_VER_UNKNOWN) {
        struct iwl_tlc_config_cmd_v3 old = {};
        old.sta_id = cfg.sta_id; old.mode = cfg.mode; old.max_ch_width = cfg.max_ch_width;
        old.chains = cfg.chains; old.flags = cfg.flags; old.non_ht_rates = cfg.non_ht_rates;
        for (unsigned nss = 0; nss < IWX_TLC_NSS_MAX; ++nss)
            memcpy(old.ht_rates[nss], cfg.ht_rates[nss], sizeof(old.ht_rates[nss]));
        old.max_mpdu_len = cfg.max_mpdu_len;
        old.sgi_ch_width_supp = cfg.sgi_ch_width_supp;
        old.max_tx_op = htole32(4000);
        const size_t size = version < 3 || version == IWX_FW_CMD_VER_UNKNOWN ? sizeof(old) - 4 : sizeof(old);
        error = iwx_send_cmd_pdu(&com, command, 0, size, &old);
    } else return EOPNOTSUPP;
    if (epoch != dmaEpoch || cookie != owner.cookie || !owner.configured) return ENXIO;
    if (!error) {
        owner.widthMHz = width;
        owner.antenna = antenna;
        owner.mcs2 = mcs2;
        owner.tlcMode = cfg.mode;
        owner.sgiMask = cfg.sgi_ch_width_supp;
        owner.tlcFlags = tlcFlags;
        ++awdlRadio.blockAckTXStats.tlcConfigs;
    }
    return error;
}

uint8_t ItlIwx::awdlTXMcs(uint64_t now, unsigned stream) const
{
    const auto *peer = awdlPeers.find(awdlRadio.blockAckTX.peer, now);
    uint8_t mask = peer ? (stream ? peer->txHTMcs2(now) : peer->txHTMcs(now)) : 0;
    // Peers leaving the channel must not push TLC into the lowest HT rates.
    mask &= 0xf0;
    for (unsigned i = 0; i < 8; ++i)
        if (AirportAWDL::TxRateControl::halfMbps(0x80 | i) < unsigned(awdlMinRateMbps) * 2)
            mask &= ~(1U << i);
    return mask;
}

uint8_t ItlIwx::awdlTXWidth(uint64_t now) const
{
    const auto *peer = awdlPeers.find(awdlRadio.blockAckTX.peer, now);
    AirportAWDL::Window window = {};
    if (awdlRadio.widthMHz != 80 || !peer || !peer->window(now, window) ||
        window.channel != awdlRadio.channel)
        return 20;
    if (window.widthMHz() == 80)
        return 80;
    const unsigned presence = peer->sync.presenceMode;
    const unsigned slot = presence ? (window.counter / presence) % 16 : 0;
    const bool multiSlotRun = presence && peer->sequence.count == 16 &&
        (peer->sequence.channel[(slot + 15) % 16] == window.channel ||
         peer->sequence.channel[(slot + 1) % 16] == window.channel);
    const bool socialClass = window.encoding == 0 || window.encoding == 1 ||
        (window.encoding == 3 &&
         (window.channelFlags == 0 ||
          (window.channel <= 48 ? window.channelFlags == 115 :
           (window.channel <= 64 ? window.channelFlags == 118 :
            (window.channelFlags >= 124 && window.channelFlags <= 130)))));
    return (window.channel == 149 || window.channel == 44) && multiSlotRun && socialClass &&
        (peer->txHTCap(now) & AirportAWDL::HTCapSGI40) ? 80 : 20;
}

bool ItlIwx::receiveAWDLTXBlockAck(const uint8_t *frame, size_t length)
{
    using namespace AirportAWDL;
    auto &owner = awdlRadio.blockAckTX;
    if (!owner.configured || owner.epoch != dmaEpoch || !awdlRadio.ready || awdlRadio.stopPending ||
        length < 24 || !sameAddress(frame + 10, owner.peer)) return false;
    BlockAckRequest end = {};
    if (blockAckTXEnd(frame, length, awdlRadio.address, end) &&
        sameAddress(end.peer, owner.peer) && end.tid == owner.tid) {
        ++awdlRadio.blockAckTXStats.retireDelba;
        owner.retire(awdlRadioNow(), true);
        return true;
    }
    BlockAckAnswer answer = {};
    if (!blockAckAnswer(frame, length, awdlRadio.address, answer)) return false;
    auto &stats = awdlRadio.blockAckTXStats;
    ++stats.responses;
    if (!sameAddress(frame + 16, BSSID)) ++stats.responsesOtherBSSID;
    if (owner.phase != TXBlockAckQueue::Asking || !owner.attempts ||
        !sameAddress(answer.peer, owner.peer) || answer.dialog != owner.dialog || answer.tid != owner.tid) {
        ++stats.unmatched;
        return true;
    }
    stats.lastStatus = answer.status;
    owner.managementToken = 0;
    owner.lastAskAcked = false;
    if (answer.status || !answer.immediate) {
        ++stats.refusals;
        owner.retryDialog(awdlRadioNow());
        return true;
    }
    owner.window = !answer.window || answer.window > 64 ? 64 : answer.window;
    owner.amsdu = false;
    owner.receiveProbeUntilUS = 0;
    owner.phase = TXBlockAckQueue::Enabling;
    return true;
}

bool ItlIwx::completeAWDLTXManagement(uint64_t token, bool acknowledged)
{
    auto &owner = awdlRadio.blockAckTX;
    if (!token || token != owner.managementToken) return false;
    if (owner.phase != AirportAWDL::TXBlockAckQueue::Retiring) {
        owner.lastAskAcked = acknowledged;
        if (acknowledged) ++awdlRadio.blockAckTXStats.acknowledged;
        else ++awdlRadio.blockAckTXStats.notAcknowledged;
        if (owner.phase == AirportAWDL::TXBlockAckQueue::Asking && acknowledged &&
            owner.receiveProbeUntilUS) {
            const uint64_t probeEnd = awdlRadioNow() + AirportAWDL::TXBlockAckQueue::ReceiveProbeDurationUS;
            if (probeEnd > owner.receiveProbeUntilUS)
                owner.receiveProbeUntilUS = probeEnd;
        }
    }
    owner.managementToken = 0;
    if (owner.phase == AirportAWDL::TXBlockAckQueue::Retiring && acknowledged) owner.needDelba = false;
    return true;
}

void ItlIwx::serviceAWDLTXBlockAck()
{
    using namespace AirportAWDL;
    auto &owner = awdlRadio.blockAckTX;
    if (!getMainWorkLoop()->inGate() || awdlRadio.changing || !awdlRadio.ready ||
        !awdlRadio.listener || awdlRadio.stopPending || awdlRadio.fault || owner.phase == TXBlockAckQueue::Idle)
        return;
    const uint64_t epoch = dmaEpoch;
    const auto bss = com.sc_ic.ic_bss;
    const auto infra = reinterpret_cast<const iwx_node *>(bss);
    const auto phy = infra ? infra->in_phyctxt : nullptr;
    const auto channel = phy ? phy->channel : nullptr;
    const uint32_t mac = infra ? IWX_FW_CMD_ID_AND_COLOR(infra->in_id, infra->in_color) : 0;
    auto wanted = [&]() {
        if (epoch != dmaEpoch || !awdlRadio.ready || !awdlRadio.binding || awdlRadio.stopPending ||
            awdlRadio.fault || !bss || !phy || com.sc_ic.ic_bss != bss ||
            com.sc_ic.ic_state != IEEE80211_S_RUN || com.ns_nstate != IEEE80211_S_RUN) return false;
        const auto current = reinterpret_cast<const iwx_node *>(com.sc_ic.ic_bss);
        return current->in_phyctxt == phy && phy->channel == channel &&
            IWX_FW_CMD_ID_AND_COLOR(current->in_id, current->in_color) == mac &&
            ((owner.phase == TXBlockAckQueue::Retiring && owner.releasePeer) ||
             awdlPeers.find(owner.peer, awdlRadioNow()));
    };
    if (!wanted()) {
        if (!owner.configured && owner.phase == TXBlockAckQueue::Requested) owner = {};
        return;
    }
    awdlRadio.changing = true;
    int error = 0;
    if (owner.phase == TXBlockAckQueue::Requested) {
        unsigned index = BlockAckState::PeerCount, free = index, evict = index;
        for (unsigned i = 0; i < BlockAckState::PeerCount; ++i) {
            const auto &peer = awdlBA.peers[i];
            if (peer.station && sameAddress(peer.address, owner.peer)) { index = i; break; }
            if (!peer.station && free == BlockAckState::PeerCount) free = i;
            else if (peer.station && evict == BlockAckState::PeerCount) {
                bool hasBA = false;
                for (const auto &session : peer.sessions) hasBA |= session.firmware;
                if (!hasBA) evict = i;
            }
        }
        if (index == BlockAckState::PeerCount) index = free < BlockAckState::PeerCount ? free : evict;
        if (index < BlockAckState::PeerCount && awdlBA.nextCookie != ~uint64_t(0)) {
            if (awdlBA.peers[index].station && !sameAddress(awdlBA.peers[index].address, owner.peer)) {
                error = removeAWDLBlockAckPeer(index);
                if (epoch != dmaEpoch) return;
                if (!error && !wanted()) error = ECANCELED;
            }
        }
        if (!error && index < BlockAckState::PeerCount && awdlBA.nextCookie != ~uint64_t(0)) {
            owner.phase = TXBlockAckQueue::Preparing;
            owner.epoch = epoch; owner.cookie = ++awdlBA.nextCookie;
            owner.station = BlockAckState::FirstStation + index; owner.tid = 0;
            error = addAWDLBlockAckPeer(index, owner.peer);
            if (epoch != dmaEpoch) return;
            if (!error && !wanted()) error = ECANCELED;
            if (!error) {
                IwxTxQueueSetup setup = {};
                const int queue = iwx_tvqm_enable_txq(&com, owner.tid, 0, 64, owner.station, &setup);
                if (epoch != dmaEpoch) return;
                if (queue < 0) error = -queue;
                else {
                    owner.queue = queue; owner.configured = true;
                    owner.sequence = com.txq[queue].cur & 0xfff;
                }
            }
            if (!error && !wanted()) error = ECANCELED;
            if (!error) error = configureAWDLTXStation(false, false);
            if (epoch != dmaEpoch) return;
            if (!error && !wanted()) error = ECANCELED;
            if (!error) error = configureAWDLTXRates();
            if (epoch != dmaEpoch) return;
            if (!error && !wanted()) error = ECANCELED;
            if (!error) {
                owner.phase = TXBlockAckQueue::Asking;
                owner.dialog = uint8_t(owner.cookie);
                if (!owner.dialog) owner.dialog = 1;
                owner.attempts = 0; owner.nextAskUS = 0; owner.lastAskAcked = false;
                ++owner.negotiations;
            }
        }
    } else if (owner.phase == TXBlockAckQueue::Enabling) {
        error = configureAWDLTXStation(true, true);
        if (epoch != dmaEpoch) return;
        if (!error && !wanted()) error = ECANCELED;
        if (!error && owner.phase == TXBlockAckQueue::Enabling) {
            owner.phase = TXBlockAckQueue::Agreed;
            owner.consecutiveFailures = 0;
            owner.keepAgreedOnFailure = false;
            owner.wasAgreed = false;
            owner.lastDataUS = owner.lastProgressUS = awdlRadioNow();
            ++owner.agreements;
            ++awdlRadio.blockAckTXStats.agreements;
        }
    }
    if (!error && owner.phase == TXBlockAckQueue::Agreed && !owner.paused &&
        com.txq[owner.queue].queued && awdlRadioNow() - owner.lastProgressUS >= 2000000) {
        ++awdlRadio.blockAckTXStats.retireStalls;
        owner.retire(awdlRadioNow());
    }
    if (!error && owner.phase == TXBlockAckQueue::Retiring) {
        if (owner.releasePeer && !awdlPeers.find(owner.peer, awdlRadioNow())) owner.needDelba = false;
        if (!owner.paused) error = configureAWDLTXStation(owner.enabled, true);
        if (epoch != dmaEpoch) return;
        if (!error && !owner.retireFlushed) {
            error = iwx_flush_sta_tids(&com, owner.station, 0xffff);
            if (epoch != dmaEpoch) return;
            if (!error) owner.retireFlushed = true;
        }
        if (!error && (!owner.needDelba || awdlRadioNow() >= owner.retireDeadlineUS)) {
            if (!com.txq[owner.queue].queued) {
                if (owner.releasePeer) {
                    const unsigned index = owner.station - BlockAckState::FirstStation;
                    uint8_t nextPeer[6];
                    copyAddress(nextPeer, owner.handoverPeer);
                    const bool nextHeavy = owner.handoverFrames >= 16;
                    bool active = false;
                    for (unsigned tid = 0; tid < BlockAckState::TIDCount; ++tid)
                        if (awdlBA.peers[index].sessions[tid].firmware) {
                            active = true; requestAWDLBlockAckEnd(owner.station, tid);
                        }
                    if (!active) {
                        error = removeAWDLBlockAckPeer(index);
                        if (epoch != dmaEpoch) return;
                        if (!error) {
                            ++awdlRadio.blockAckTXStats.handovers;
                            if (unicastAddress(nextPeer))
                                owner.demand(nextPeer, nextHeavy ? TXBlockAckQueue::HeavyFrameBytes : 0, awdlRadioNow(), true, true);
                        }
                    }
                } else {
                    error = configureAWDLTXStation(false, false);
                    if (epoch != dmaEpoch) return;
                    if (!error) {
                        const uint64_t retryDelay = owner.wasAgreed ?
                            TXBlockAckQueue::RecoveryDelayUS : TXBlockAckQueue::RetryDelayUS;
                        owner.phase = TXBlockAckQueue::Asking;
                        owner.managementToken = 0;
                        owner.retryDialog(awdlRadioNow(), retryDelay);
                    }
                }
            } else if (awdlRadioNow() >= owner.retireDeadlineUS) {
                // A flush without reclaim pointers still owns DMA. End RX first,
                // then REMOVE_STA releases the shared station and its TX queue.
                const unsigned index = owner.station - BlockAckState::FirstStation;
                bool active = false;
                for (unsigned tid = 0; tid < BlockAckState::TIDCount; ++tid)
                    if (awdlBA.peers[index].sessions[tid].firmware) {
                        active = true; requestAWDLBlockAckEnd(owner.station, tid);
                    }
                if (!active) error = removeAWDLBlockAckPeer(index);
                if (epoch != dmaEpoch) return;
            }
        }
    }
    if (error && owner.station >= BlockAckState::FirstStation &&
        owner.station < BlockAckState::FirstStation + BlockAckState::PeerCount) {
        const int cleanup = removeAWDLBlockAckPeer(owner.station - BlockAckState::FirstStation);
        if (epoch != dmaEpoch) return;
        if (cleanup) { awdlRadio.ready = false; awdlRadio.fault = true; awdlRadio.cleanupError = cleanup; }
        else if (!owner.configured) owner = {};
    }
    awdlRadio.changing = false;
    if (awdlRadio.stopPending) stopAWDLRadio();
}

int ItlIwx::serviceAWDLReceiveFilter(uint64_t (*windowEnd)(const void *, uint64_t), const void *context)
{
    using AirportAWDL::TXBlockAckQueue;
    if (!getMainWorkLoop()->inGate() || awdlRadio.changing) return EBUSY;
    if (!awdlRadio.listener || !awdlRadio.ready || !awdlRadio.mac ||
        AirportAWDL::idleUnboundOf(awdlRadio) ||
        awdlRadio.stopPending || awdlRadio.fault) return 0;
    auto &tx = awdlRadio.blockAckTX;
    const uint64_t epoch = dmaEpoch, now = awdlRadioNow();
    auto separateChannel = [&]() {
        const auto infra = reinterpret_cast<const iwx_node *>(com.sc_ic.ic_bss);
        return infra && infra->in_phyctxt &&
            infra->in_phyctxt->channel != &com.sc_ic.ic_channels[awdlRadio.channel];
    };
    const bool receiveSession = awdlHasRXBA();
    const bool deferProbe = awdlRadio.deferReceiveProbe(now);
    const bool asking = receiveSession && separateChannel() && tx.configured &&
        tx.epoch == epoch && tx.phase == TXBlockAckQueue::Asking &&
        (tx.attempts < TXBlockAckQueue::MaxAttempts || tx.receiveProbeUntilUS > now);
    const uint64_t end = asking && windowEnd ? windowEnd(context, now) : 0;
    if (!asking || !end || deferProbe) tx.receiveProbeUntilUS = 0;
    else if (tx.receiveProbeUntilUS > end) tx.receiveProbeUntilUS = end;
    if (asking && tx.attempts < TXBlockAckQueue::MaxAttempts && end > now &&
        awdlRadio.listenerFilterKnown && !awdlRadio.listenerPromisc &&
        !tx.managementToken && now >= tx.nextAskUS && tx.receiveProbeUntilUS <= now) {
        if (deferProbe) {
            // Keep hardware RX BlockAck active during bulk inbound streams when no heavy TX is active.
            ++awdlRadio.blockAckTXStats.receiveProbesDeferred;
        } else {
            // Open a bounded promiscuous window for the outgoing ADDBA Request so LISTENER
            // delivers the peer's unicast ADDBA Response alongside light inbound RX BA traffic.
            tx.receiveProbeTried = true;
            tx.receiveProbeUntilUS = end - now < TXBlockAckQueue::ReceiveProbeDurationUS ?
                end : now + TXBlockAckQueue::ReceiveProbeDurationUS;
            ++awdlRadio.blockAckTXStats.receiveProbes;
        }
    }
    auto wantsPromisc = [&]() {
        return awdlRadio.listenerWantsPromisc(awdlHasRXBA(), separateChannel(), awdlRadioNow());
    };
    if (awdlRadio.listenerFilterKnown && awdlRadio.listenerPromisc == wantsPromisc()) return 0;
    awdlRadio.changing = true;
    int error = awdlMAC(IWX_FW_CTXT_ACTION_MODIFY);
    if (epoch != dmaEpoch) return ENXIO;
    // A reply or payload can arrive while the command sleeps. Close the probe immediately in that case.
    if (!error && awdlRadio.ready && !awdlRadio.stopPending &&
        awdlRadio.listenerPromisc != wantsPromisc()) {
        error = awdlMAC(IWX_FW_CTXT_ACTION_MODIFY);
        if (epoch != dmaEpoch) return ENXIO;
    }
    awdlRadio.changing = false;
    if (error) {
        awdlRadio.fault = true;
        stopAWDLRadio();
        if (epoch != dmaEpoch) return ENXIO;
        awdlRadio.lastError = error;
    } else if (awdlRadio.stopPending) stopAWDLRadio();
    return error;
}

int ItlIwx::setAWDLTXWindow(uint64_t (*windowEnd)(const void *, uint64_t), const void *context)
{
    using AirportAWDL::TXBlockAckQueue;
    auto &owner = awdlRadio.blockAckTX;
    if (!owner.configured || awdlRadio.changing) return 0;
    // TID disable prevents data submission while negotiation uses the management queue.
    if (owner.phase == TXBlockAckQueue::Asking && !owner.enabled) {
        owner.windowEndUS = 0;
        return 0;
    }
    const uint64_t epoch = dmaEpoch, cookie = owner.cookie;
    const auto bss = com.sc_ic.ic_bss;
    const auto infra = reinterpret_cast<const iwx_node *>(bss);
    const auto phy = infra ? infra->in_phyctxt : nullptr;
    const auto channel = phy ? phy->channel : nullptr;
    const uint32_t mac = infra ? IWX_FW_CMD_ID_AND_COLOR(infra->in_id, infra->in_color) : 0;
    auto deadline = [&]() -> uint64_t {
        if (epoch != dmaEpoch || owner.epoch != epoch || cookie != owner.cookie ||
            !bss || !phy || com.sc_ic.ic_bss != bss || com.sc_ic.ic_state != IEEE80211_S_RUN ||
            com.ns_nstate != IEEE80211_S_RUN || infra->in_phyctxt != phy || phy->channel != channel ||
            IWX_FW_CMD_ID_AND_COLOR(infra->in_id, infra->in_color) != mac) return 0;
        if (awdlRadio.channel && awdlRadio.channel <= 14) {
            const auto *txPeer = awdlPeers.find(owner.peer, awdlRadioNow());
            if (txPeer && txPeer->haveSequence) {
                for (unsigned s = 0; s < txPeer->sequence.count && s < 16; ++s)
                    if (txPeer->sequence.channel[s] > 14) return 0;
            }
        }
        return owner.phase == TXBlockAckQueue::Agreed && windowEnd && awdlRadio.ready &&
            !awdlRadio.stopPending && !awdlRadio.fault ? windowEnd(context, awdlRadioNow()) : 0;
    };
    const uint64_t rateNow = awdlRadioNow();
    const uint8_t mcs = awdlTXMcs(rateNow);
    const uint8_t antenna = awdlTXAntenna();
    const uint8_t mcs2 = antenna == 3 ? awdlTXMcs(rateNow, 1) : 0;
    const uint8_t targetWidth = awdlTXWidth(rateNow);
    const uint8_t expectedMode = (targetWidth == 80 || awdlRadio.channel > 14) ?
        IWX_TLC_MNG_MODE_VHT : IWX_TLC_MNG_MODE_HT;
    const auto *ratePeer = awdlPeers.find(owner.peer, rateNow);
    const uint16_t htCap = ratePeer ? ratePeer->txHTCap(rateNow) : 0;
    auto effectiveSGI = [](uint16_t cap, uint8_t width) -> uint8_t {
        uint8_t sgi = 0;
        if (cap & AirportAWDL::HTCapSGI20)
            sgi |= uint8_t(1U << IWX_TLC_MNG_CH_WIDTH_20MHZ);
        if (width == 80 && (cap & AirportAWDL::HTCapSGI40))
            sgi |= uint8_t((1U << IWX_TLC_MNG_CH_WIDTH_40MHZ) |
                           (1U << IWX_TLC_MNG_CH_WIDTH_80MHZ));
        return sgi;
    };
    auto effectiveFlags = [](uint16_t cap, uint8_t chains) -> uint16_t {
        uint16_t flags = 0;
        if (cap & AirportAWDL::HTCapLDPC)
            flags |= IWX_TLC_MNG_CFG_FLAGS_LDPC_MSK;
        if (chains == 3 && (cap & AirportAWDL::HTCapRxSTBCMask))
            flags |= IWX_TLC_MNG_CFG_FLAGS_STBC_MSK;
        return flags;
    };
    const uint8_t expectedSGI = effectiveSGI(htCap, targetWidth);
    const uint16_t expectedFlags = effectiveFlags(htCap, antenna);
    const bool ratesChanged = mcs != owner.mcs || mcs2 != owner.mcs2 || antenna != owner.antenna ||
        owner.widthMHz != targetWidth || owner.tlcMode != expectedMode ||
        owner.sgiMask != expectedSGI || owner.tlcFlags != expectedFlags ||
        owner.floorMbps != awdlMinRateMbps;
    uint64_t end = mcs ? deadline() : 0;
    if (owner.paused && !end) {
        owner.windowEndUS = 0;
        return 0;
    }
    awdlRadio.changing = true;
    int error = 0;
    if (!owner.paused && (!end || ratesChanged)) error = configureAWDLTXStation(owner.enabled, true);
    if (epoch != dmaEpoch || cookie != owner.cookie) return ENXIO;
    if (!error) end = mcs ? deadline() : 0;
    if (!error && end && ratesChanged) error = configureAWDLTXRates();
    if (epoch != dmaEpoch || cookie != owner.cookie) return ENXIO;
    auto ratesCurrent = [&]() {
        const uint64_t checkNow = awdlRadioNow();
        const uint8_t checkWidth = awdlTXWidth(checkNow);
        const uint8_t checkAntenna = awdlTXAntenna();
        const uint8_t checkMode = (checkWidth == 80 || awdlRadio.channel > 14) ?
            IWX_TLC_MNG_MODE_VHT : IWX_TLC_MNG_MODE_HT;
        const auto *currentPeer = awdlPeers.find(owner.peer, checkNow);
        const uint16_t currentCap = currentPeer ? currentPeer->txHTCap(checkNow) : 0;
        return owner.mcs && (awdlTXMcs(checkNow) & owner.mcs) == owner.mcs &&
            (awdlTXMcs(checkNow, 1) & owner.mcs2) == owner.mcs2 &&
            checkAntenna == owner.antenna && owner.floorMbps == awdlMinRateMbps &&
            owner.widthMHz == checkWidth && owner.tlcMode == checkMode &&
            owner.sgiMask == effectiveSGI(currentCap, checkWidth) &&
            owner.tlcFlags == effectiveFlags(currentCap, checkAntenna);
    };
    if (!error && mcs) end = ratesCurrent() ? deadline() : 0;
    if (!error && end && owner.paused) {
        error = configureAWDLTXStation(true, false);
        if (epoch != dmaEpoch || cookie != owner.cookie) return ENXIO;
        if (!error && !owner.paused) owner.lastProgressUS = awdlRadioNow();
    }
    if (!error) {
        end = mcs && ratesCurrent() ? deadline() : 0;
        if (!end && !owner.paused) error = configureAWDLTXStation(owner.enabled, true);
        if (epoch != dmaEpoch || cookie != owner.cookie) return ENXIO;
        owner.windowEndUS = owner.paused ? 0 : end;
    }
    awdlRadio.changing = false;
    if (error) {
        ++awdlRadio.blockAckTXStats.retireErrors;
        owner.retire(awdlRadioNow());
        awdlRadio.lastError = error;
    }
    if (awdlRadio.stopPending) stopAWDLRadio();
    return error;
}

bool ItlIwx::awdlHasRXBA() const
{
    if (awdlBA.preparing) return true;
    for (const auto &peer : awdlBA.peers)
        for (const auto &session : peer.sessions)
            if (session.firmware) return true;
    return false;
}

bool ItlIwx::awdlRXBAOwned(const struct iwx_rxba_data *rxba) const
{
    using AirportAWDL::BlockAckState;
    if (!awdlRadio.ready || awdlRadio.stopPending || awdlRadio.fault ||
        rxba->sta_id < BlockAckState::FirstStation ||
        rxba->sta_id >= BlockAckState::FirstStation + BlockAckState::PeerCount ||
        rxba->tid >= BlockAckState::TIDCount || rxba->baid >= IWX_MAX_BAID ||
        rxba->reorder_buf.removed) return false;
    const auto &peer = awdlBA.peers[rxba->sta_id - BlockAckState::FirstStation];
    const auto &session = peer.sessions[rxba->tid];
    return peer.station && peer.epoch == dmaEpoch && !(peer.removeTids & (1U << rxba->tid)) &&
        session.firmware && session.baidPlusOne == rxba->baid + 1 && !session.endDeadlineUS;
}

bool ItlIwx::awdlBlockAckLive(const AirportAWDL::BlockAckRequest &request, uint64_t cookie) const
{
    if (!cookie || request.tid >= AirportAWDL::BlockAckState::TIDCount ||
        !awdlRadio.ready || awdlRadio.stopPending || awdlRadio.fault) return false;
    for (const auto &peer : awdlBA.peers) {
        if (!peer.station || peer.epoch != dmaEpoch ||
            !AirportAWDL::sameAddress(peer.address, request.peer)) continue;
        const auto &session = peer.sessions[request.tid];
        if (!session.firmware || !session.baidPlusOne || session.cookie != cookie ||
            (peer.removeTids & (1U << request.tid))) return false;
        return request.teardown ? session.endDeadlineUS != 0 :
            !session.endDeadlineUS && AirportAWDL::sameBlockAck(session.request, request);
    }
    return false;
}

int ItlIwx::removeAWDLBlockAckTID(unsigned index, unsigned tid)
{
    auto &peer = awdlBA.peers[index];
    auto &session = peer.sessions[tid];
    if (!session.firmware) { peer.removeTids &= ~(1U << tid); return 0; }
    const uint64_t epoch = dmaEpoch;
    struct iwx_add_sta_cmd cmd = {};
    cmd.sta_id = AirportAWDL::BlockAckState::FirstStation + index;
    cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(RadioState::MacID, 0));
    cmd.add_modify = IWX_STA_MODE_MODIFY;
    cmd.modify_mask = IWX_STA_MODIFY_REMOVE_BA_TID;
    cmd.remove_immediate_ba_tid = tid;
    uint32_t status = IWX_ADD_STA_SUCCESS;
    const int error = iwx_send_cmd_pdu_status(&com, IWX_ADD_STA, sizeof(cmd), &cmd, &status);
    if (epoch != dmaEpoch) return ENXIO;
    if (error || (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS)
        return error ? error : EIO;
    if (session.baidPlusOne && session.baidPlusOne <= IWX_MAX_BAID) {
        auto &rxba = com.sc_rxba_data[session.baidPlusOne - 1];
        if (rxba.baid == session.baidPlusOne - 1 && rxba.sta_id == cmd.sta_id && rxba.tid == tid)
            iwx_clear_reorder_buffer(&com, &rxba);
    }
    session = {};
    peer.removeTids &= ~(1U << tid);
    if (com.sc_rx_ba_sessions) --com.sc_rx_ba_sessions;
    ++awdlBA.removed;
    return 0;
}

int ItlIwx::removeAWDLBlockAckPeer(unsigned index)
{
    auto &peer = awdlBA.peers[index];
    if (!peer.station) return 0;
    const uint64_t epoch = dmaEpoch;
    struct iwx_rm_sta_cmd cmd = {};
    cmd.sta_id = AirportAWDL::BlockAckState::FirstStation + index;
    auto &tx = awdlRadio.blockAckTX;
    const bool ownsTX = tx.configured && tx.station == cmd.sta_id;
    const int queue = ownsTX ? tx.queue : IWX_INVALID_QUEUE;
    if (ownsTX) {
        tx.phase = AirportAWDL::TXBlockAckQueue::Retiring;
        tx.windowEndUS = 0;
        const int error = iwx_flush_sta_tids(&com, cmd.sta_id, 0xffff);
        if (epoch != dmaEpoch) return ENXIO;
        if (error) return error;
    }
    const int error = iwx_send_cmd_pdu(&com, IWX_REMOVE_STA, 0, sizeof(cmd), &cmd);
    if (epoch != dmaEpoch) return ENXIO;
    if (error) return error;
    if (ownsTX) {
        auto &ring = com.txq[queue];
        for (unsigned i = 0; i < ring.ring_count; ++i) {
            if (ring.data[i].m) completeAWDLTx(&ring.data[i], false, true);
            if (epoch != dmaEpoch) return ENXIO;
        }
        iwx_reset_tx_ring(&com, &ring);
        iwx_free_tx_ring(&com, &ring);
        tx = {};
    } else if (tx.phase == AirportAWDL::TXBlockAckQueue::Preparing && tx.station == cmd.sta_id) tx = {};
    for (unsigned tid = 0; tid < AirportAWDL::BlockAckState::TIDCount; ++tid) {
        const auto &session = peer.sessions[tid];
        if (!session.firmware) continue;
        if (session.baidPlusOne && session.baidPlusOne <= IWX_MAX_BAID) {
            auto &rxba = com.sc_rxba_data[session.baidPlusOne - 1];
            if (rxba.baid == session.baidPlusOne - 1 && rxba.sta_id == cmd.sta_id && rxba.tid == tid)
                iwx_clear_reorder_buffer(&com, &rxba);
        }
        if (com.sc_rx_ba_sessions) --com.sc_rx_ba_sessions;
        ++awdlBA.removed;
    }
    peer = {};
    return 0;
}

int ItlIwx::clearAWDLBlockAck()
{
    for (unsigned i = 0; i < AirportAWDL::BlockAckState::PeerCount; ++i) {
        const int error = removeAWDLBlockAckPeer(i);
        if (error) return error;
    }
    awdlRadio.blockAckTX = {};
    return 0;
}

void ItlIwx::resetAWDLBlockAck()
{
    using AirportAWDL::BlockAckState;
    for (auto &rxba : com.sc_rxba_data) {
        if (rxba.baid == IWX_RX_REORDER_DATA_INVALID_BAID ||
            rxba.sta_id < BlockAckState::FirstStation ||
            rxba.sta_id >= BlockAckState::FirstStation + BlockAckState::PeerCount) continue;
        iwx_clear_reorder_buffer(&com, &rxba);
    }
    for (auto &peer : awdlBA.peers) {
        for (const auto &session : peer.sessions)
            if (session.firmware && com.sc_rx_ba_sessions) --com.sc_rx_ba_sessions;
        peer = {};
    }
    awdlBA.preparing = false;
    awdlRadio.blockAckTX = {};
}

// The listener and peer station setup follow ForbiddenItlwm 59ff7bff.
// Host reorder ownership is shared with the IWX RX path below.
int ItlIwx::prepareAWDLBlockAck(const AirportAWDL::BlockAckRequest &request, uint64_t &cookie)
{
    using namespace AirportAWDL;
    cookie = 0;
    if (!getMainWorkLoop()->inGate() || awdlRadio.changing) return EBUSY;
    if (request.teardown && request.tid < BlockAckState::TIDCount) {
        for (const auto &peer : awdlBA.peers) {
            if (!peer.station || !sameAddress(peer.address, request.peer)) continue;
            const auto candidate = peer.sessions[request.tid].cookie;
            if (awdlBlockAckLive(request, candidate)) { cookie = candidate; return 0; }
        }
        return ENOENT;
    }
    if (!request.immediate || request.teardown || request.tid >= BlockAckState::TIDCount ||
        request.ssn > 4095 || !unicastAddress(request.peer) ||
        sameAddress(request.peer, awdlRadio.address) || !awdlRadio.listener)
        return EOPNOTSUPP;
    const uint64_t epoch = dmaEpoch;
    const auto bss = com.sc_ic.ic_bss;
    const auto infra = reinterpret_cast<const iwx_node *>(bss);
    const auto phy = infra ? infra->in_phyctxt : nullptr;
    const auto channel = phy ? phy->channel : nullptr;
    const uint32_t mac = infra ? IWX_FW_CMD_ID_AND_COLOR(infra->in_id, infra->in_color) : 0;
    auto wanted = [&]() {
        if (epoch != dmaEpoch || !awdlRadio.ready || !awdlRadio.binding ||
            awdlRadio.fault || awdlRadio.stopPending || !bss || !phy || com.sc_ic.ic_bss != bss ||
            com.sc_ic.ic_state != IEEE80211_S_RUN || com.ns_nstate != IEEE80211_S_RUN ||
            com.sc_ic.ic_opmode != IEEE80211_M_STA || !(com.sc_flags & IWX_FLAG_BINDING_ACTIVE)) return false;
        const auto current = reinterpret_cast<const iwx_node *>(com.sc_ic.ic_bss);
        return current->in_phyctxt == phy && phy->channel == channel &&
            IWX_FW_CMD_ID_AND_COLOR(current->in_id, current->in_color) == mac &&
            awdlPeers.find(request.peer, awdlRadioNow());
    };
    if (!wanted()) return ENETDOWN;
    unsigned index = BlockAckState::PeerCount, freeIndex = index, evictIndex = index;
    for (unsigned i = 0; i < BlockAckState::PeerCount; ++i) {
        const auto &peer = awdlBA.peers[i];
        if (peer.station && sameAddress(peer.address, request.peer)) { index = i; break; }
        if (!peer.station && freeIndex == BlockAckState::PeerCount) freeIndex = i;
        else if (peer.station && evictIndex == BlockAckState::PeerCount) {
            bool busy = (awdlRadio.blockAckTX.configured &&
                awdlRadio.blockAckTX.station == BlockAckState::FirstStation + i) ||
                (awdlRadio.blockAckTX.phase == TXBlockAckQueue::Preparing &&
                 awdlRadio.blockAckTX.station == BlockAckState::FirstStation + i);
            for (const auto &session : peer.sessions) busy |= session.firmware;
            if (!busy) evictIndex = i;
        }
    }
    if (index == BlockAckState::PeerCount) index = freeIndex < BlockAckState::PeerCount ? freeIndex : evictIndex;
    if (index == BlockAckState::PeerCount || awdlBA.nextCookie == ~uint64_t(0)) return ENOSPC;
    const bool evictCached = awdlBA.peers[index].station &&
        !sameAddress(awdlBA.peers[index].address, request.peer);
    auto &peer = awdlBA.peers[index];
    auto &session = peer.sessions[request.tid];
    if (!evictCached) {
        if ((peer.removeTids & (1U << request.tid)) || session.endDeadlineUS) return EBUSY;
        if (awdlBlockAckLive(request, session.cookie)) { cookie = session.cookie; return 0; }
        if (!session.firmware && com.sc_rx_ba_sessions >= BlockAckState::MaxSessions) return ENOSPC;
    } else if (com.sc_rx_ba_sessions >= BlockAckState::MaxSessions) return ENOSPC;
    awdlRadio.changing = true;
    awdlBA.preparing = true;
    const int error = [&]() -> int {
        if (evictCached) {
            const int err = removeAWDLBlockAckPeer(index);
            if (err) return err;
            if (!wanted()) return ECANCELED;
        }
        if (!peer.station) {
            const int err = addAWDLBlockAckPeer(index, request.peer);
            if (err) return err;
            if (!wanted()) return ECANCELED;
        }
        if (session.firmware) {
            const int err = removeAWDLBlockAckTID(index, request.tid);
            if (err) return err;
            if (!wanted()) return ECANCELED;
        }
        if (peer.removeTids & (1U << request.tid)) return ECANCELED;
        if (com.sc_rx_ba_sessions >= BlockAckState::MaxSessions) return ENOSPC;
        // A matched peer station must receive ACKs, rather than monitor-only delivery.
        const int filterError = awdlMAC(IWX_FW_CTXT_ACTION_MODIFY);
        if (epoch != dmaEpoch) return ENXIO;
        if (filterError || !wanted()) return filterError ? filterError : ECANCELED;
        if (peer.removeTids & (1U << request.tid)) return ECANCELED;
        if (com.sc_rx_ba_sessions >= BlockAckState::MaxSessions) return ENOSPC;
        struct iwx_add_sta_cmd cmd = {};
        cmd.sta_id = BlockAckState::FirstStation + index;
        cmd.mac_id_n_color = htole32(IWX_FW_CMD_ID_AND_COLOR(RadioState::MacID, 0));
        cmd.add_modify = IWX_STA_MODE_MODIFY;
        cmd.modify_mask = IWX_STA_MODIFY_ADD_BA_TID;
        cmd.add_immediate_ba_tid = request.tid;
        cmd.add_immediate_ba_ssn = htole16(request.ssn);
        cmd.rx_ba_window = htole16(blockAckWindow(request));
        // Reserve capacity before the command sleeps and AP negotiation can run.
        ++com.sc_rx_ba_sessions;
        session.firmware = true;
        session.request = request;
        session.cookie = ++awdlBA.nextCookie;
        uint32_t status = IWX_ADD_STA_SUCCESS;
        const int err = iwx_send_cmd_pdu_status(&com, IWX_ADD_STA, sizeof(cmd), &cmd, &status);
        if (epoch != dmaEpoch) return ENXIO;
        if (!err && (status & IWX_ADD_STA_STATUS_MASK) != IWX_ADD_STA_SUCCESS) {
            session = {};
            --com.sc_rx_ba_sessions;
            return EIO;
        }
        if (err) return err;
        const unsigned baid = (status & IWX_ADD_STA_BAID_MASK) >> IWX_ADD_STA_BAID_SHIFT;
        if (!(status & IWX_ADD_STA_BAID_VALID_MASK) || baid >= IWX_MAX_BAID ||
            com.sc_rxba_data[baid].baid != IWX_RX_REORDER_DATA_INVALID_BAID) return EIO;
        auto &rxba = com.sc_rxba_data[baid];
        rxba.sc = &com;
        rxba.sta_id = cmd.sta_id;
        rxba.tid = request.tid;
        rxba.timeout = 0;
        timeout_set(&rxba.reorder_buf.reorder_timer, iwx_reorder_timer_expired, &rxba.reorder_buf);
        if (!rxba.reorder_buf.reorder_timer) return ENOMEM;
        iwx_init_reorder_buffer(&rxba.reorder_buf, request.ssn, blockAckWindow(request));
        rxba.baid = baid;
        session.baidPlusOne = baid + 1;
        if (!wanted() || (peer.removeTids & (1U << request.tid))) return ECANCELED;
        cookie = session.cookie;
        ++awdlBA.accepted;
        return 0;
    }();
    if (epoch != dmaEpoch) return ENXIO;
    if (error) {
        ++awdlBA.declined;
        bool otherSession = false;
        for (const auto &active : peer.sessions) otherSession |= active.firmware;
        const int cleanup = error == ENOSPC && otherSession ? 0 : removeAWDLBlockAckPeer(index);
        if (epoch != dmaEpoch) return ENXIO;
        if (cleanup) {
            awdlRadio.ready = false;
            awdlRadio.fault = true;
            awdlRadio.cleanupError = cleanup;
        }
    }
    awdlBA.preparing = false;
    awdlRadio.changing = false;
    if (awdlRadio.stopPending) { stopAWDLRadio(); return ECANCELED; }
    return error;
}

bool ItlIwx::receiveAWDLBlockAckEnd(const AirportAWDL::BlockAckRequest &request, bool requireSession)
{
    if (request.tid >= AirportAWDL::BlockAckState::TIDCount) return false;
    bool matched = false;
    for (auto &peer : awdlBA.peers)
        if (peer.station && peer.epoch == dmaEpoch && AirportAWDL::sameAddress(peer.address, request.peer) &&
            (!requireSession || peer.sessions[request.tid].firmware)) {
            peer.removeTids |= 1U << request.tid;
            matched = true;
        }
    if ((matched || !requireSession) && awdlBlockAckEndInput) awdlBlockAckEndInput(awdlReceiver, request);
    return matched;
}

void ItlIwx::requestAWDLBlockAckEnd(uint8_t station, uint8_t tid)
{
    using AirportAWDL::BlockAckState;
    if (station < BlockAckState::FirstStation || station >= BlockAckState::FirstStation + BlockAckState::PeerCount ||
        tid >= BlockAckState::TIDCount) return;
    auto &peer = awdlBA.peers[station - BlockAckState::FirstStation];
    auto &session = peer.sessions[tid];
    const bool handover = awdlRadio.blockAckTX.configured &&
        awdlRadio.blockAckTX.phase == AirportAWDL::TXBlockAckQueue::Retiring &&
        awdlRadio.blockAckTX.releasePeer && awdlRadio.blockAckTX.station == station;
    const uint64_t deadlineUS = awdlRadioNow() + (handover ? 250000ULL : 3000000ULL);
    if (peer.station && session.firmware && (!session.endDeadlineUS || deadlineUS < session.endDeadlineUS))
        session.endDeadlineUS = deadlineUS;
}

void ItlIwx::completeAWDLBlockAckEnd(const AirportAWDL::BlockAckRequest &request, uint64_t cookie)
{
    if (!request.teardown || !awdlBlockAckLive(request, cookie)) return;
    for (auto &peer : awdlBA.peers)
        if (peer.station && AirportAWDL::sameAddress(peer.address, request.peer))
            peer.removeTids |= 1U << request.tid;
}

void ItlIwx::serviceAWDLBlockAck()
{
    using namespace AirportAWDL;
    if (awdlRadio.changing || !awdlRadio.ready || awdlRadio.stopPending || awdlRadio.fault) return;
    const uint64_t epoch = dmaEpoch;
    const auto bss = com.sc_ic.ic_bss;
    const auto infra = reinterpret_cast<const iwx_node *>(bss);
    const auto phy = infra ? infra->in_phyctxt : nullptr;
    const auto channel = phy ? phy->channel : nullptr;
    const uint32_t mac = infra ? IWX_FW_CMD_ID_AND_COLOR(infra->in_id, infra->in_color) : 0;
    auto canProvision = [&]() {
        if (epoch != dmaEpoch || !awdlRadio.ready || !awdlRadio.listener || !awdlRadio.binding ||
            awdlRadio.fault || awdlRadio.stopPending || !bss || !phy || com.sc_ic.ic_bss != bss ||
            com.sc_ic.ic_state != IEEE80211_S_RUN || com.ns_nstate != IEEE80211_S_RUN ||
            com.sc_ic.ic_opmode != IEEE80211_M_STA || !(com.sc_flags & IWX_FLAG_BINDING_ACTIVE))
            return false;
        const auto current = reinterpret_cast<const iwx_node *>(com.sc_ic.ic_bss);
        return current->in_phyctxt == phy && phy->channel == channel &&
            IWX_FW_CMD_ID_AND_COLOR(current->in_id, current->in_color) == mac;
    };
    auto activeUnicastPeer = [](const Peer *p, uint64_t at) {
        return p && p->lastUnicastDataUS && at >= p->lastUnicastDataUS &&
            at - p->lastUnicastDataUS < 5000000ULL;
    };
    awdlRadio.changing = true;
    int error = 0;
    for (unsigned i = 0; i < BlockAckState::PeerCount && !error; ++i) {
        auto &peer = awdlBA.peers[i];
        if (!peer.station) continue;
        const uint64_t now = awdlRadioNow();
        const auto *observed = awdlPeers.find(peer.address, now);
        const auto *cached = awdlPeers.findCached(peer.address);
        const bool keepRealtime = observed && AirportAWDL::hasRealtimePeerOn(awdlRadio, peer.address);
        const bool keepCached = cached && canProvision();
        const bool keepStation = (cached || keepRealtime || activeUnicastPeer(observed, now)) && canProvision();
        if (!observed && !keepCached) {
            error = removeAWDLBlockAckPeer(i);
            if (epoch != dmaEpoch) return;
            continue;
        }
        for (unsigned tid = 0; tid < BlockAckState::TIDCount && !error; ++tid) {
            auto &session = peer.sessions[tid];
            if (session.endDeadlineUS && awdlRadioNow() >= session.endDeadlineUS)
                peer.removeTids |= 1U << tid;
            if (peer.removeTids & (1U << tid)) {
                error = removeAWDLBlockAckTID(i, tid);
                if (epoch != dmaEpoch) return;
            } else if (session.endDeadlineUS && !session.endQueued && awdlBlockAckInput) {
                BlockAckRequest request = session.request;
                request.teardown = true;
                session.endQueued = true;
                awdlBlockAckInput(awdlReceiver, request);
            }
        }
        bool active = awdlRadio.blockAckTX.configured &&
            awdlRadio.blockAckTX.station == BlockAckState::FirstStation + i;
        for (const auto &session : peer.sessions) active |= session.firmware;
        if (!active && !keepStation && !error) {
            error = removeAWDLBlockAckPeer(i);
            if (epoch != dmaEpoch) return;
        }
    }
    if (!error && canProvision()) {
        // Proactively install IWX_STA_LINK for nativeCached or active unicast peers so
        // MacID 1 on infraChannel (where IN_PROMISC is disabled) hardware-matches and ACKs
        // unicast PTP, NDP, LLW and data frames across DELBA/ADDBA transitions.
        for (unsigned p = 0; p < PeerTable::Capacity && !error && canProvision(); ++p) {
            const auto &candidate = awdlPeers.peers[p];
            const uint64_t now = awdlRadioNow();
            const bool liveCandidate = candidate.occupied && now >= candidate.lastSeenUS &&
                now - candidate.lastSeenUS < PeerTable::LifetimeUS;
            const bool activeData = liveCandidate &&
                (activeUnicastPeer(&candidate, now) ||
                 AirportAWDL::hasRealtimePeerOn(awdlRadio, candidate.address));
            if ((!candidate.nativeCached && !activeData) || !unicastAddress(candidate.address))
                continue;
            uint8_t addr[6];
            copyAddress(addr, candidate.address);
            if (!awdlPeers.find(addr, now) && !awdlPeers.findCached(addr)) continue;
            bool installed = false;
            unsigned freeSlot = BlockAckState::PeerCount;
            for (unsigned i = 0; i < BlockAckState::PeerCount; ++i) {
                if (awdlBA.peers[i].station && sameAddress(awdlBA.peers[i].address, addr)) {
                    installed = true;
                    break;
                }
                if (!awdlBA.peers[i].station && freeSlot == BlockAckState::PeerCount)
                    freeSlot = i;
            }
            if (installed) continue;
            if (freeSlot == BlockAckState::PeerCount) break;
            error = addAWDLBlockAckPeer(freeSlot, addr);
            if (epoch != dmaEpoch) return;
            const uint64_t afterNow = awdlRadioNow();
            const auto *afterObs = awdlPeers.find(addr, afterNow);
            if (!error && (!canProvision() ||
                (!awdlPeers.findCached(addr) &&
                 !(afterObs && AirportAWDL::hasRealtimePeerOn(awdlRadio, addr)) &&
                 !activeUnicastPeer(afterObs, afterNow)))) {
                error = removeAWDLBlockAckPeer(freeSlot);
                if (epoch != dmaEpoch) return;
            }
        }
    }
    awdlRadio.changing = false;
    if (error) {
        awdlRadio.ready = false;
        awdlRadio.fault = true;
        awdlRadio.cleanupError = error;
    } else if (awdlRadio.stopPending) stopAWDLRadio();
}

void ItlIwx::deliverAWDLReordered(mbuf_t m, bool deaggregated)
{
    const auto frame = static_cast<const uint8_t *>(mbuf_data(m));
    const uint64_t rxNow = awdlRadioNow();
    awdlData.receive(frame, mbuf_len(m), awdlPeers, awdlRadio.address, rxNow,
        deaggregated, awdlRadio.ready ? awdlInput : nullptr, awdlReceiver, true);
    ++awdlBA.reordered;
    if (!awdlRadio.llwRealtime && !awdlRadio.heavyReceiveActive(rxNow) &&
        !awdlRadio.heavyTransmitActive(rxNow))
        publishAWDLReceive();
    mbuf_freem(m);
}

bool ItlIwx::receiveAWDLReordered(mbuf_t m, struct iwx_rx_mpdu_desc *desc, struct mbuf_list *ml)
{
    using namespace AirportAWDL;
    if (com.sc_ic.ic_opmode != IEEE80211_M_STA || mbuf_len(m) < 20 ||
        mbuf_len(m) != mbuf_pkthdr_len(m)) return false;
    const auto frame = static_cast<const uint8_t *>(mbuf_data(m));
    auto drop = [&]() { ++awdlBA.rejected; mbuf_freem(m); return true; };
    if ((frame[0] & 0xfcU) == 0x84U) {
        if (!sameAddress(frame + 4, awdlRadio.address)) return false;
        if ((frame[1] & 0x43) || (frame[16] & 0x0e) == 0x02 ||
            !unicastAddress(frame + 10) || sameAddress(frame + 10, awdlRadio.address))
            return drop();
        const unsigned tid = (frame[17] >> 4) & 0x0f;
        const uint16_t barSSN = le16(frame + 18) >> 4;
        if (tid >= BlockAckState::TIDCount || !awdlRadio.ready ||
            awdlRadio.stopPending || awdlRadio.fault) return drop();
        const BlockAckState::Peer *matchedPeer = nullptr;
        for (const auto &peer : awdlBA.peers) {
            if (peer.station && peer.epoch == dmaEpoch && sameAddress(peer.address, frame + 10)) {
                matchedPeer = &peer;
                break;
            }
        }
        if (!matchedPeer) return drop();
        const auto &session = matchedPeer->sessions[tid];
        if (!session.firmware || !session.baidPlusOne ||
            (matchedPeer->removeTids & (1U << tid)) || session.endDeadlineUS) {
            mbuf_freem(m);
            return true;
        }
        const unsigned baid = session.baidPlusOne - 1;
        if (baid >= IWX_MAX_BAID) return drop();
        auto &rxba = com.sc_rxba_data[baid];
        if (rxba.baid != baid || rxba.tid != tid || !awdlRXBAOwned(&rxba) ||
            rxba.reorder_buf.removed || !rxba.reorder_buf.buf_size)
            return drop();
        auto seqLess = [](uint16_t a, uint16_t b) {
            return ((uint16_t(a - b) & 0xfffU) > 2048U);
        };
        const uint32_t reorder = le32toh(desc->reorder_data);
        const unsigned descBaid = (reorder & IWX_RX_MPDU_REORDER_BAID_MASK) >>
            IWX_RX_MPDU_REORDER_BAID_SHIFT;
        uint16_t targetSSN = barSSN;
        if (descBaid == baid) {
            const uint16_t fwNSSN = reorder & IWX_RX_MPDU_REORDER_NSSN_MASK;
            if (seqLess(targetSSN, fwNSSN)) targetSSN = fwNSSN;
        }
        auto &buf = rxba.reorder_buf;
        if (!buf.valid) buf.valid = 1;
        if (seqLess(buf.head_sn, targetSSN)) {
            iwx_release_frames(&com, nullptr, &rxba, &buf, targetSSN, ml);
        } else if (buf.num_stored) {
            iwx_release_frames(&com, nullptr, &rxba, &buf, buf.head_sn, ml);
        }
        ++awdlBA.barFrames;
        const uint64_t barNow = awdlRadioNow();
        if (!awdlRadio.llwRealtime && !awdlRadio.heavyReceiveActive(barNow) &&
            !awdlRadio.heavyTransmitActive(barNow))
            publishAWDLReceive();
        mbuf_freem(m);
        return true;
    }
    if (mbuf_len(m) < 26 || frame[0] != 0x88 || (frame[1] & 0x43) || !sameAddress(frame + 16, BSSID)) return false;
    const uint32_t reorder = le32toh(desc->reorder_data);
    const unsigned baid = (reorder & IWX_RX_MPDU_REORDER_BAID_MASK) >> IWX_RX_MPDU_REORDER_BAID_SHIFT;
    if (baid == IWX_RX_REORDER_DATA_INVALID_BAID) return false;
    const uint32_t status = le32toh(desc->status);
    const unsigned station = (status & IWX_RX_MPDU_RES_STATUS_STA_ID_MSK) >> 24;
    const unsigned tid = frame[24] & 15;
    if ((frame[1] & 0x47) || (le16(frame + 22) & 15) ||
        ((frame[1] & 0x80) && mbuf_len(m) < 30) || !sameAddress(frame + 4, awdlRadio.address) ||
        !(status & IWX_RX_MPDU_RES_STATUS_SRC_STA_FOUND) || baid >= IWX_MAX_BAID ||
        station < BlockAckState::FirstStation || station >= BlockAckState::FirstStation + BlockAckState::PeerCount)
        return drop();
    auto &rxba = com.sc_rxba_data[baid];
    const auto &peer = awdlBA.peers[station - BlockAckState::FirstStation];
    if (rxba.baid != baid || rxba.sta_id != station || rxba.tid != tid || !awdlRXBAOwned(&rxba) ||
        !sameAddress(frame + 10, peer.address) || (!rxba.reorder_buf.valid && (reorder & IWX_RX_MPDU_REORDER_BA_OLD_SN)))
        return drop();
    DataBatch payload = {};
    if (parseDataMPDU(frame, mbuf_len(m), payload, desc->mac_flags2 & IWX_RX_MPDU_MFLG2_AMSDU) ==
        ParseResult::Valid) {
        const uint64_t rxNow = awdlRadioNow();
        awdlRadio.lastRXPayloadUS = rxNow;
        if (mbuf_len(m) >= 512) awdlRadio.lastHeavyRXPayloadUS = rxNow;
    }
    ieee80211_rxinfo rxi = {};
    const uint32_t gp2 = com.sc_device_family >= IWX_DEVICE_FAMILY_AX210 ?
        le32toh(desc->v3.gp2_on_air_rise) : le32toh(desc->v1.gp2_on_air_rise);
    if (!iwx_rx_reorder(&com, m, 0, desc, 0, 0, gp2, &rxi, ml))
        deliverAWDLReordered(m, desc->mac_flags2 & IWX_RX_MPDU_MFLG2_AMSDU);
    return true;
}

void ItlIwx::completeAWDLTx(struct iwx_tx_data *data, bool acknowledged, bool flushed)
{
    if (!(data->flags & IWX_TXDATA_FLAG_AWDL) || !data->awdlToken) return;
    const uint64_t token = data->awdlToken;
    data->awdlToken = 0;
    auto &tx = awdlRadio.blockAckTX;
    const bool keepAgreed = tx.keepAgreedOnFailure;
    tx.keepAgreedOnFailure = false;
    if ((data->flags & IWX_TXDATA_FLAG_AWDL_BA) && tx.configured &&
        tx.epoch == dmaEpoch && data->awdlBACookie == tx.cookie) {
        tx.lastProgressUS = awdlRadioNow();
        if (acknowledged) tx.consecutiveFailures = 0;
        else if (!flushed && tx.phase == AirportAWDL::TXBlockAckQueue::Agreed) {
            if (!keepAgreed || ++tx.consecutiveFailures >= AirportAWDL::TXBlockAckQueue::MaxConsecutiveFailures) {
                ++awdlRadio.blockAckTXStats.retireFailures;
                tx.retire(tx.lastProgressUS);
            }
        }
    }
    if (data->awdlRatePeer && data->awdlRatePeer <= AirportAWDL::PeerTable::Capacity)
        awdlPeers.peers[data->awdlRatePeer - 1].txRate.release(token, data->awdlRateEpoch);
    data->awdlRatePeer = 0;
    if (data->type == IEEE80211_FC0_TYPE_DATA) {
        const int result = acknowledged ? 0 : (flushed ? ECANCELED : EIO);
        if (awdlDataComplete) awdlDataComplete(awdlReceiver, token, result);
    } else {
        const uint64_t epoch = dmaEpoch, cookie = tx.cookie;
        const bool flushedManagementAsk = flushed && token == tx.managementToken;
        const bool wasRetiring = tx.phase == AirportAWDL::TXBlockAckQueue::Retiring;
        if (!(flushed && AirportAWDL::notifyTxFlushedOn(*this, awdlReceiver, token)) && awdlTxComplete)
            awdlTxComplete(awdlReceiver, token, acknowledged);
        if (flushedManagementAsk && epoch == dmaEpoch && cookie == tx.cookie) {
            if (!tx.managementToken && !wasRetiring && awdlRadio.blockAckTXStats.notAcknowledged)
                --awdlRadio.blockAckTXStats.notAcknowledged;
            tx.managementToken = 0;
            if (tx.attempts) --tx.attempts;
            tx.nextAskUS = awdlRadioNow();
        }
    }
}

void ItlIwx::sampleAWDLRate(struct iwx_softc *sc, struct iwx_rx_packet *pkt,
                           struct iwx_tx_data *data)
{
    if (!data->awdlRatePeer || data->awdlRatePeer > AirportAWDL::PeerTable::Capacity ||
        !data->awdlToken) return;
    const auto &report = *reinterpret_cast<const iwx_tx_resp *>(pkt->data);
    const unsigned version = iwx_lookup_notif_ver(sc, IWX_LONG_GROUP, IWX_TX_CMD);
    const unsigned queue = le16toh(report.tx_queue);
    auto &peer = awdlPeers.peers[data->awdlRatePeer - 1];
    const uint64_t now = awdlRadioNow();
    const uint8_t status = le16toh(report.status.status) & IWX_TX_STATUS_MSK;
    const bool acknowledged = status == IWX_TX_STATUS_SUCCESS;
    const bool retryLimit = status == IWX_TX_STATUS_FAIL_SHORT_LIMIT ||
        status == IWX_TX_STATUS_FAIL_LONG_LIMIT;
    // Only the first reclaimed descriptor has an individual response. A cancelled
    // dwell, BT arbitration or DIRECT_DONE does not supply an RF-rate sample.
    if (iwx_rx_packet_payload_len(pkt) < sizeof(report) + sizeof(uint32_t) ||
        report.frame_count != 1 || (version != 6 && version != 7) ||
        !awdlRadio.ownsQueue(queue) || queue >= nitems(sc->txq) ||
        &sc->txq[queue].data[sc->txq[queue].tail] != data ||
        (report.ra_tid >> 4) != AirportAWDL::RadioState::StationID ||
        (queue == unsigned(awdlRadio.queue) ?
            ((report.ra_tid & 15) != IWX_MGMT_TID && (report.ra_tid & 15) != IWX_TID_NON_QOS) :
            (report.ra_tid & 15) != RadioState::DataTID) ||
        (le16toh(report.frame_ctrl) & 0x7f) != IEEE80211_FC0_TYPE_DATA ||
        report.bt_kill_count || report.failure_rts ||
        (!acknowledged && (!retryLimit || !report.failure_frame)) ||
        !peer.occupied || now < peer.lastSeenUS ||
        now - peer.lastSeenUS >= AirportAWDL::PeerTable::LifetimeUS ||
        peer.txRate.epoch != data->awdlRateEpoch || peer.txRate.dmaEpoch != dmaEpoch ||
        peer.txRate.requestedFloor != awdlMinRateMbps ||
        peer.txRate.htMcsMask != peer.txHTMcs(now) ||
        peer.txRate.channel != awdlRadio.channel ||
        (!acknowledged && (!awdlTxWindowOpen() ||
            data->awdlRateGrant != awdlRadio.dwellStarts || now >= data->awdlRateDeadlineUS))) {
        ++awdlRateIgnored;
        return;
    }
    const auto &command = sc->txq[queue].cmd[sc->txq[queue].tail];
    const auto *tx = reinterpret_cast<const iwx_tx_cmd_gen2 *>(command.data);
    const auto *header = reinterpret_cast<const ieee80211_frame *>(command.data +
        (sc->sc_device_family >= IWX_DEVICE_FAMILY_AX210 ? sizeof(iwx_tx_cmd_gen3) : sizeof(*tx)));
    const unsigned actual = IwxAWDLTxBudget::rateCode(le32toh(report.initial_rate), version > 6);
    const unsigned commandVersion = iwx_lookup_cmd_ver(sc, IWX_LONG_GROUP, IWX_TX_CMD);
    const unsigned submitted = IwxAWDLTxBudget::rateCode(le32toh(tx->rate_n_flags),
        commandVersion > 8 && commandVersion != IWX_FW_CMD_VER_UNKNOWN);
    // CMD_RATE fixes every attempt; a failed report has no successful-rate requirement.
    if (le16toh(report.byte_cnt) != le16toh(tx->len) ||
        (le16toh(report.frame_ctrl) & 0xff) != header->i_fc[0] ||
        submitted != data->awdlRateCode ||
        (acknowledged && actual != submitted) ||
        !AirportAWDL::sameAddress(header->i_addr1, peer.address)) {
        ++awdlRateIgnored;
        return;
    }
    const uint8_t before = peer.txRate.current;
    if (peer.txRate.sample(data->awdlToken, data->awdlRateEpoch, data->awdlRateCode,
                          acknowledged, report.failure_frame, awdlRateSerial)) {
        ++awdlRateSamples;
        awdlRateRaised += AirportAWDL::TxRateControl::halfMbps(peer.txRate.current) >
            AirportAWDL::TxRateControl::halfMbps(before);
        awdlRateLowered += AirportAWDL::TxRateControl::halfMbps(peer.txRate.current) <
            AirportAWDL::TxRateControl::halfMbps(before);
        awdlRateLastFlags = le32toh(report.initial_rate);
        awdlRateLastMediaUS = le16toh(report.wireless_media_time);
        awdlRateLastRetries = report.failure_frame;
    } else ++awdlRateIgnored;
}

bool ItlIwx::awdlTxWindowOpen() const
{
    return awdlRadio.ready && !awdlRadio.changing && !awdlRadio.stopPending &&
        !awdlRadio.fault && awdlRadio.station && awdlRadio.queue != IWX_INVALID_QUEUE &&
        awdlChannelAllowed(awdlRadio.channel, awdlRadio.widthMHz ? awdlRadio.widthMHz : 20) &&
        awdlRadio.dwell.running && !awdlRadio.dwell.cancelling &&
        com.sc_ic.ic_state == IEEE80211_S_RUN && awdlRadioNow() < awdlRadio.dwell.windowEndUS;
}

int ItlIwx::transmitAWDL(mbuf_t packet, uint64_t token, uint64_t deadlineUS)
{
    // Consumes the packet on every return path.
    if (!packet) return EINVAL;
    if (!getMainWorkLoop()->inGate()) {
        struct Args { mbuf_t packet; uint64_t token, deadlineUS; } args = {packet, token, deadlineUS};
        return getMainCommandGate()->runAction(
            [](OSObject *, void *self, void *input, void *, void *) -> IOReturn {
                auto args = static_cast<Args *>(input);
                return static_cast<ItlIwx *>(self)->transmitAWDL(args->packet, args->token, args->deadlineUS);
            }, this, &args);
    }
    if (!awdlTxWindowOpen()) {
        mbuf_freem(packet);
        return ENETDOWN;
    }
    auto bytes = static_cast<uint8_t *>(mbuf_data(packet));
    const bool qosData = mbuf_len(packet) >= 26 && bytes[0] == 0x88;
    const bool aggregate = qosData && (bytes[24] & 0x80) != 0;
    const bool lowLatencyQoS = qosData && !aggregate &&
        (bytes[24] & 0x0f) == RadioState::LLWTID && bytes[25] == 0;
    const bool lowLatencyNoQos = !qosData && bytes[0] == 8 && mbuf_len(packet) >= 32 &&
        AirportAWDL::matchesBytes8(bytes + 24, AirportAWDL::LLW_SNAP);
    const bool lowLatency = lowLatencyQoS || lowLatencyNoQos;
    const bool ptpQoS = qosData && !aggregate && !lowLatencyQoS && bytes[25] == 0 &&
        ((bytes[24] & 0x0f) == RadioState::LLWTID || (bytes[24] & 0x0f) == RadioState::DataTID) &&
        AirportAWDL::isUnicastPTP(bytes, mbuf_len(packet));
    const bool unicastPTP = token && !aggregate && !lowLatency &&
        (ptpQoS || (bytes[0] == 8 && AirportAWDL::isUnicastPTP(bytes, mbuf_len(packet))));
    if (mbuf_len(packet) != mbuf_pkthdr_len(packet) || mbuf_len(packet) < 24 ||
        (bytes[0] != 0xd0 && bytes[0] != 8 && !aggregate && !lowLatencyQoS && !ptpQoS) ||
        (bytes[1] && !(bytes[1] == IEEE80211_FC1_RETRY &&
                       (bytes[0] == 8 || aggregate || lowLatencyQoS || ptpQoS) &&
                       token && AirportAWDL::unicastAddress(bytes + 4))) ||
        !AirportAWDL::sameAddress(bytes + 10, awdlRadio.address) ||
        !AirportAWDL::sameAddress(bytes + 16, AirportAWDL::BSSID) ||
        (token && bytes[0] == 0xd0 && !AirportAWDL::unicastAddress(bytes + 4))) {
        mbuf_freem(packet);
        return EINVAL;
    }
    auto &tx = awdlRadio.blockAckTX;
    const bool data = bytes[0] == 8 || aggregate || lowLatencyQoS || ptpQoS;
    const bool peerTX = data && !lowLatency && tx.forPeer(bytes + 4);
    const bool peerBAReady = peerTX && tx.phase == AirportAWDL::TXBlockAckQueue::Agreed &&
        token && tx.configured && tx.enabled && !tx.paused &&
        tx.windowEndUS > awdlRadioNow() && com.txq[tx.queue].queued < tx.window &&
        awdlTXMcs(awdlRadioNow()) != 0;
    const bool ptpQoSBypass = unicastPTP && !peerBAReady;
    const bool blockAck = peerTX && tx.phase == AirportAWDL::TXBlockAckQueue::Agreed && !ptpQoSBypass;
    const uint64_t cookie = tx.cookie, epoch = dmaEpoch;
    if (peerTX && !ptpQoSBypass && (tx.phase == AirportAWDL::TXBlockAckQueue::Retiring ||
        tx.phase == AirportAWDL::TXBlockAckQueue::Enabling)) {
        mbuf_freem(packet); return EAGAIN;
    }
    if (lowLatencyQoS) {
        AirportAWDL::DataBatch batch = {};
        if (!token || !AirportAWDL::unicastAddress(bytes + 4) ||
            AirportAWDL::parseDataMPDU(bytes, mbuf_len(packet), batch) != AirportAWDL::ParseResult::Valid ||
            batch.count != 1 || !batch.frames[0].lowLatency) {
            mbuf_freem(packet);
            return EINVAL;
        }
        if (!awdlRadio.llwQueueConfigured) {
            mbuf_freem(packet);
            return EAGAIN;
        }
    }
    if (aggregate) {
        AirportAWDL::DataBatch batch = {};
        if (!token || !AirportAWDL::unicastAddress(bytes + 4) ||
            mbuf_len(packet) < 26 || bytes[24] != 0x80 || bytes[25] ||
            AirportAWDL::parseDataMPDU(bytes, mbuf_len(packet), batch) != AirportAWDL::ParseResult::Valid ||
            batch.count != 2) {
            mbuf_freem(packet);
            return EINVAL;
        }
        const auto *peer = awdlPeers.find(bytes + 4, awdlRadioNow());
        if (blockAck ? !tx.amsdu : (!awdlRadio.dataQueueConfigured || !peer ||
            !peer->aggregateTX(awdlRadioNow(), dmaEpoch, awdlRadio.channel, awdlMinRateMbps) ||
            (peerTX && !tx.allowsSoftwareAggregate(awdlRadioNow())))) {
            mbuf_freem(packet);
            return EAGAIN;
        }
    }
    int ptpQueue = IWX_INVALID_QUEUE;
    if (ptpQoSBypass) {
        ptpQueue = awdlRadio.llwQueueConfigured ? awdlRadio.llwQueue :
            (awdlRadio.dataQueueConfigured ? awdlRadio.dataQueue : IWX_INVALID_QUEUE);
        if (ptpQueue == IWX_INVALID_QUEUE) {
            mbuf_freem(packet);
            return EAGAIN;
        }
        const uint8_t ptpTid = awdlRadio.llwQueueConfigured ?
            RadioState::LLWTID : RadioState::DataTID;
        if (!qosData) {
            const size_t size = mbuf_len(packet);
            mbuf_t qos = nullptr;
            if (mbuf_getpacket(MBUF_DONTWAIT, &qos) || !qos) { mbuf_freem(packet); return ENOMEM; }
            if (mbuf_maxlen(qos) < size + 2) { mbuf_freem(qos); mbuf_freem(packet); return ENOBUFS; }
            auto output = static_cast<uint8_t *>(mbuf_data(qos));
            memcpy(output, bytes, 24);
            output[0] = 0x88;
            output[24] = ptpTid;
            output[25] = 0;
            memcpy(output + 26, bytes + 24, size - 24);
            mbuf_setlen(qos, size + 2); mbuf_pkthdr_setlen(qos, size + 2);
            mbuf_freem(packet); packet = qos; bytes = output;
        } else {
            bytes[24] = ptpTid;
            bytes[25] = 0;
        }
    }
    if (blockAck) {
        if (!token || !tx.configured || !tx.enabled || tx.paused ||
            tx.windowEndUS <= awdlRadioNow() || com.txq[tx.queue].queued >= tx.window ||
            !awdlTXMcs(awdlRadioNow())) { mbuf_freem(packet); return EAGAIN; }
        if (!qosData) {
            const size_t size = mbuf_len(packet);
            mbuf_t qos = nullptr;
            if (mbuf_getpacket(MBUF_DONTWAIT, &qos) || !qos) { mbuf_freem(packet); return ENOMEM; }
            if (mbuf_maxlen(qos) < size + 2) { mbuf_freem(qos); mbuf_freem(packet); return ENOBUFS; }
            auto output = static_cast<uint8_t *>(mbuf_data(qos));
            memcpy(output, bytes, 24); output[24] = output[25] = 0;
            memcpy(output + 26, bytes + 24, size - 24);
            mbuf_setlen(qos, size + 2); mbuf_pkthdr_setlen(qos, size + 2);
            mbuf_freem(packet); packet = qos; bytes = output;
        }
        if (epoch != dmaEpoch || cookie != tx.cookie || tx.phase != AirportAWDL::TXBlockAckQueue::Agreed) {
            mbuf_freem(packet); return ENETDOWN;
        }
        bytes[0] = 0x88; bytes[1] = 0;
        bytes[24] = (aggregate ? 0x80 : 0) | tx.tid; bytes[25] = 0;
        AirportAWDL::putLE16(bytes + 22, uint16_t(tx.sequence << 4));
    }
    uint8_t destination[6]; AirportAWDL::copyAddress(destination, bytes + 4);
    const size_t size = mbuf_len(packet);
    const int queue = blockAck ? tx.queue : ptpQoSBypass ? ptpQueue : aggregate ? awdlRadio.dataQueue :
        lowLatencyQoS ? awdlRadio.llwQueue : awdlRadio.queue;
    int error = iwx_tx(&com, packet, nullptr, 0, queue, token, 0, deadlineUS);
    if (!error) {
        ++awdlRadio.submitted;
        if (data && !lowLatency && !ptpQoSBypass && AirportAWDL::unicastAddress(destination))
            noteAWDLTXBlockAck(destination, size);
    }
    return error;
}
#endif
