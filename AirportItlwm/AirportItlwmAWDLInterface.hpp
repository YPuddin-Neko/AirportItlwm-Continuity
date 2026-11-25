// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AIRPORT_ITLWM_AWDL_INTERFACE_HPP
#define AIRPORT_ITLWM_AWDL_INTERFACE_HPP
#include "Airport/Apple80211.h"
#if defined(AIRPORT_WCL) && __IO80211_TARGET == __MAC_15_2
#include "Airport/macOS15/IO80211AWDLProtocol.h"
#include "Airport/macOS15/IO80211InfraInterface.h"
#include "AWDL/SyncEngine.hpp"
#include "AWDL/RadioState.hpp"
#include "AWDL/BlockAck.hpp"
#include "AWDL/ServiceMerge.hpp"
#include "AWDL/LLWFlow.hpp"
#include <IOKit/IOTimerEventSource.h>
class ItlIwx;
class AirportItlwmEthernetInterface;
class AirportItlwmAWDLInterface;
class IOSkywalkPacket;
class IOSkywalkPacketBufferPool;
class IOSkywalkTxSubmissionQueue;
class IOSkywalkTxCompletionQueue;
class IOSkywalkRxCompletionQueue;

class AirportItlwmLLWInterface : public IO80211InfraInterface {
    OSDeclareDefaultStructors(AirportItlwmLLWInterface)
public:
    bool initWithAWDL(IO80211Controller *, AirportItlwmAWDLInterface *, const ether_addr *);
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    const char *getBSDNamePrefix() override { return "llw"; }
    UInt getBSDUnitNumber() override { return 0; }
    SInt32 initBSDInterfaceParameters(ifnet_init_eparams *, sockaddr_dl **) override;
    IOReturn getHardwareAddress(ether_addr *) override;
    IOReturn setMacAddress(ether_addr &) override;
    SInt32 setInterfaceEnable(bool) override;
    IOReturn configureMulticastFilter(unsigned int, ether_addr const *, unsigned int) override { return kIOReturnSuccess; }
    IOReturn setAllMulticastModeEnable(bool) override { return kIOReturnSuccess; }
    IOReturn setPromiscuousModeEnable(bool, unsigned int) override { return kIOReturnSuccess; }
    bool isEnabled() const { return enabledState; }
    bool isSkywalkNative() const { return skywalkRegistered; }
    bool registerSkywalk(IOWorkLoop *);
    void deregisterSkywalk();
    bool inputSkywalkEthernet(const uint8_t *ethernet, size_t length);
    uint64_t skywalkTxCount() const { return skywalkTxPackets; }
    uint64_t skywalkTxDropCount() const { return skywalkTxDropped; }
    uint64_t skywalkRxCount() const { return skywalkRxPackets; }
    uint64_t skywalkRxDropCount() const { return skywalkRxDropped; }
    IOReturn processBSDCommand(struct __ifnet *, unsigned int, void *) override;
private:
    static unsigned int txSubmissionAction(OSObject *owner, IOSkywalkTxSubmissionQueue *queue,
                                           IOSkywalkPacket * const *packets, unsigned int packetCount,
                                           void *refCon);
    ether_addr address;
    uint8_t llwUniqueID[10];
    AirportItlwmAWDLInterface *awdlParent;
    bool enabledState;
    bool skywalkRegistered;
    IOSkywalkPacketBufferPool *skywalkPool;
    IOSkywalkTxSubmissionQueue *txSubQueue;
    IOSkywalkTxCompletionQueue *txCompQueue;
    IOSkywalkRxCompletionQueue *rxCompQueue;
    IOWorkLoop *queueWorkLoop;
    uint64_t skywalkTxPackets, skywalkTxDropped, skywalkRxPackets, skywalkRxDropped;
};

class AirportItlwmAWDLInterface : public IO80211AWDLProtocol {
    OSDeclareDefaultStructors(AirportItlwmAWDLInterface)
public:
    bool init(IO80211Controller *, ether_addr *, unsigned, const char *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    IOReturn setProperties(OSObject *) override;
    IOReturn setMacAddress(ether_addr &) override;
    IOReturn getHardwareAddress(ether_addr *) override;
    const char *getBSDNamePrefix() override { return "awdl"; }
    SInt32 setInterfaceEnable(bool) override;
    IOReturn setMulticastAddresses(const ether_addr *, unsigned) override { return kIOReturnSuccess; }
    void bindBSD(AirportItlwmEthernetInterface *interface) { bsd = interface; }
    void bindLLW(AirportItlwmLLWInterface *interface, AirportItlwmEthernetInterface *bsdIf)
    {
        llwInterface = interface;
        llwBSD = bsdIf;
    }
    IOReturn pullOutputPackets();
    IOReturn pullLLWOutputPackets();
    IOReturn submitOutputPackets(bool fromLLW);
    IOReturn enqueueLLWFrame(mbuf_t);
    void drainPendingData();
    static void txReady(void *);
    void tick();
    bool continuityMediaActive() const;
    void notifyInfrastructureReady();
    IOReturn getSSID(apple80211_ssid_data*) override;
    IOReturn getCHANNEL(apple80211_channel_data*) override;
    IOReturn getBSSID(apple80211_bssid_data*) override;
    IOReturn getSTATE(apple80211_state_data*) override;
    IOReturn getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*) override;
    IOReturn getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*) override;
    IOReturn getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter*) override;
    IOReturn getAWDL_ELECTION_ID(apple80211_awdl_election_id*) override;
    IOReturn getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*) override;
    IOReturn getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*) override;
    IOReturn getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size*) override;
    IOReturn getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*) override;
    IOReturn getAWDL_SYNC_STATE(apple80211_awdl_sync_state*) override;
    IOReturn getAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds*) override;
    IOReturn getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*) override;
    IOReturn getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*) override;
    IOReturn getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities*) override;
    IOReturn getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*) override;
    IOReturn getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*) override;
    IOReturn getAWDL_MIN_RATE(apple80211_awdl_min_rate*) override;
    IOReturn getAWDL_ENCRYPTION_TYPE(apple80211_encrypt*) override;
    IOReturn getAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*) override;
    IOReturn getAWDL_DEV_STATS(apple80211_awdl_dev_stats*) override;
    IOReturn getAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*) override;
    IOReturn getAWDL_CCA(apple80211_awdl_cca*) override;
    IOReturn getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap*) override;
    IOReturn getLLW_PARAMS(apple80211_llw_params*) override;
    IOReturn setSSID(apple80211_ssid_data*) override;
    IOReturn setCHANNEL(apple80211_channel_data*) override;
    IOReturn setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params*) override;
    IOReturn setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled*) override;
    IOReturn setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter*) override;
    IOReturn setAWDL_ELECTION_ID(apple80211_awdl_election_id*) override;
    IOReturn setAWDL_BSSID(apple80211_awdl_bssid*) override;
    IOReturn setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric*) override;
    IOReturn setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence*) override;
    IOReturn setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel*) override;
    IOReturn setAWDL_SYNC_STATE(apple80211_awdl_sync_state*) override;
    IOReturn setAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds*) override;
    IOReturn setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode*) override;
    IOReturn setPEER_CACHE_CONTROL(apple80211_peer_cache_control*) override;
    IOReturn setAWDL_OOB_REQUEST(apple80211_awdl_oob_request*) override;
    IOReturn setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template*) override;
    IOReturn setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params*) override;
    IOReturn setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration*) override;
    IOReturn setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel*) override;
    IOReturn setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode*) override;
    IOReturn setAWDL_MIN_RATE(apple80211_awdl_min_rate*) override;
    IOReturn setAWDL_ENCRYPTION_KEYS(apple80211_key*) override;
    IOReturn setAWDL_ENCRYPTION_TYPE(apple80211_encrypt*) override;
    IOReturn setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request*) override;
    IOReturn setAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period*) override;
    IOReturn setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add*) override;
    IOReturn setAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config*) override;
    IOReturn setAWDL_DFSP_UCSA_CONFIG(apple80211_awdl_dfsp_ucsa_config*) override;
    IOReturn setCOMPANION_SKYWALK_IF_CREATE(apple80211_companion_skywalk_if_create*) override;
    IOReturn setAWDL_OOB_SEC_PAYLOAD_REQUEST(apple80211_awdl_af_sec_payload_request*) override;
    IOReturn setLLW_PARAMS(apple80211_llw_params*) override;
    IOReturn setTX_RATE(apple80211_tx_rate*) override;
    IOReturn setP2P_COEX_PROTECTION(apple80211_p2p_coex_protection*) override;
    IOReturn setPSF_DWELL(apple80211_awdl_psf_dwell_params*) override;
    IOReturn setAWDL_CONFIG(apple80211_awdl_config_request*) override;
    IOReturn setAWDL_RTG_PEER_STATS_CONFIG(apple80211_rtg_peer_stats_cfg*) override;
private:
    enum class Request {
        getSSID,
        getCHANNEL,
        getBSSID,
        getSTATE,
        getAWDL_SYNC_PARAMS,
        getAWDL_SYNC_ENABLED,
        getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS,
        getAWDL_ELECTION_ID,
        getAWDL_ELECTION_METRIC,
        getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,
        getPEER_CACHE_MAXIMUM_SIZE,
        getAWDL_MASTER_CHANNEL,
        getAWDL_SYNC_STATE,
        getAWDL_ELECTION_RSSI_THRESHOLDS,
        getAWDL_PRESENCE_MODE,
        getAWDL_ELECTION_PARAMS,
        getAWDL_DEVICE_CAPABILITIES,
        getAWDL_SECONDARY_MASTER_CHANNEL,
        getAWDL_AF_TX_MODE,
        getAWDL_MIN_RATE,
        getAWDL_ENCRYPTION_TYPE,
        getAWDL_TXCAL_PERIOD,
        getAWDL_DEV_STATS,
        getAWDL_DFSP_CONFIG,
        getAWDL_CCA,
        getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES,
        getLLW_PARAMS,
        setSSID,
        setCHANNEL,
        setAWDL_SYNC_PARAMS,
        setAWDL_SYNC_ENABLED,
        setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS,
        setAWDL_ELECTION_ID,
        setAWDL_BSSID,
        setAWDL_ELECTION_METRIC,
        setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE,
        setAWDL_MASTER_CHANNEL,
        setAWDL_SYNC_STATE,
        setAWDL_ELECTION_RSSI_THRESHOLDS,
        setAWDL_PRESENCE_MODE,
        setPEER_CACHE_CONTROL,
        setAWDL_OOB_REQUEST,
        setAWDL_SYNC_FRAME_TEMPLATE,
        setAWDL_ELECTION_PARAMS,
        setAWDL_PEER_TRAFFIC_REGISTRATION,
        setAWDL_SECONDARY_MASTER_CHANNEL,
        setAWDL_AF_TX_MODE,
        setAWDL_MIN_RATE,
        setAWDL_ENCRYPTION_KEYS,
        setAWDL_ENCRYPTION_TYPE,
        setAWDL_OOB_AUTO_REQUEST,
        setAWDL_TXCAL_PERIOD,
        setAWDL_AWDL_ADVERTISERS,
        setAWDL_DFSP_CONFIG,
        setAWDL_DFSP_UCSA_CONFIG,
        setCOMPANION_SKYWALK_IF_CREATE,
        setAWDL_OOB_SEC_PAYLOAD_REQUEST,
        setLLW_PARAMS,
        setTX_RATE,
        setP2P_COEX_PROTECTION,
        setPSF_DWELL,
        setAWDL_CONFIG,
        setAWDL_RTG_PEER_STATS_CONFIG
    };
    IOReturn request(Request, void *);
    IOReturn handle(Request, void *);
    IOReturn handleCompanionSkywalkCreate(apple80211_companion_skywalk_if_create *);
    IOReturn handleGetLLWParams(apple80211_llw_params *);
    IOReturn handleSetLLWParams(const apple80211_llw_params *);
    void syncLLWLinkState();
    IOReturn pullOutputFromInterface(AirportItlwmEthernetInterface *, bool fromLLW);
    void noteOutputFlow(mbuf_t, bool fromLLW, uint64_t nowUS);
    IOReturn updatePeerCache(const apple80211_peer_cache_control &);
    void updateAvailability();
    void regulatoryMapChanged();
    IOReturn queueUnicast(const apple80211_awdl_oob_request &);
    IOReturn configureBroadcast(const apple80211_awdl_oob_request &, bool);
    bool broadcastDue(uint64_t) const;
    bool psfDue(uint64_t) const;
    uint64_t peerGrantEnd(uint64_t, const AirportAWDL::RadioState::PeerGrant &, bool) const;
    void serviceUnicast();
    void completeUnicast(uint64_t, bool);
    void completeManagement(uint64_t, bool, bool = false);
    void completeData(uint64_t, int);
    IOReturn setEnabled(bool);
    void publishReceiveFilter();
    void publishNativeSyncState();
    bool nativeSyncReady;
    uint64_t nativeSyncStateCalls;
#if DEBUG
    IOReturn setCompareTxAntennas(bool);
    IOReturn setReceiveAllData(bool);
    void publishNativePeers(uint64_t);
    uint64_t lastNativePeerSnapshotUS;
    uint64_t relayRecoveryWindowStarts;
    uint64_t parentRecoveryWindowStarts;
    bool nativePeerSampled;
    bool receiveFilterChanging;
#endif
    static void receive(void *, const AirportAWDL::DataView &);
    bool deliverLLWOrMirror(const AirportAWDL::DataView &, mbuf_t, size_t);
    void recordNativeDataActivity(bool transmit, uint32_t ethernetBytes);
    void notifyNativeDataPeer(const AirportAWDL::DataView &);
    static void receiveAction(void *, const uint8_t *, size_t, uint16_t, int16_t, uint64_t);
    static void receiveBlockAck(void *, const AirportAWDL::BlockAckRequest &);
    static void receiveBlockAckEnd(void *, const AirportAWDL::BlockAckRequest &);
    uint64_t nextManagementToken();
    void serviceBlockAck();
    void serviceTXBlockAck();
    AirportAWDL::BlockAckReply blockAckReplies[8];
    uint64_t blockAckReceived, blockAckQueued, blockAckCoalesced, blockAckRejected;
    uint64_t blockAckSubmitted, blockAckAcknowledged, blockAckFailed, blockAckExpired;
    void drain();
    ItlIwx *radio;
    AirportItlwmEthernetInterface *bsd;
    AirportItlwmLLWInterface *llwInterface;
    AirportItlwmEthernetInterface *llwBSD;
    bool llwActive, llwLinkUp, llwParamsConfigured, llwParamsEnabled;
    apple80211_llw_params llwParams;
    AirportAWDL::LLWFlowRouter llwFlows;
    uint64_t llwCreateRequests, llwDestroyRequests;
    uint64_t llwParamsGetCalls, llwParamsSetCalls;
    uint64_t llwTxQueued, llwRxDelivered, llwRxDropped, llwNDPMirrored;
    IOTimerEventSource *timer;
    ether_addr address;
    bool requested, running, stopping, ticking, drainingData, wakeArmed, interfaceEnabled;
    bool infrastructurePaused;
    bool sequenceConfigured;
    bool regulatoryPlanBlocked, regulatoryStopRequired, regulatoryResumePending;
    bool nativeSequenceValid;
    apple80211_awdl_sync_channel_sequence nativeSequence;
    uint32_t secondaryMasterChannel;
    apple80211_awdl_extension_state_machine_parameter extensionParams;
    uint32_t afTxMode;
    uint64_t nextActionUS, nextMIFUS, nextRetryUS, lastSnapshotUS;
    bool nativeWindowOpen, nativeWindowClosing;
    uint64_t nativeWindowSerial, nativeWindowEndUS;
    uint16_t nativeWindowCounter, nativeWindowChannel, nativeWindowPeriodTU, nativeWindowPresence;
    uint64_t nativeWindowStarts, nativeWindowEnds, nativeWindowErrors;
    apple80211_availability_window_extensions_end nativeWindowEnd;
    AirportAWDL::UnicastRequest unicast[16];
    AirportAWDL::BroadcastTemplate broadcast;
    AirportAWDL::BroadcastRequest immediateBroadcast;
    uint64_t broadcastSubmitted;
    uint64_t unicastToken, unicastSubmitted, unicastAcknowledged, unicastFailed, unicastNotifyErrors;
    uint64_t unsupportedRequests;
    uint64_t rxDelivered, rxDropped, txDropped;
    uint64_t actionDelivered, actionDropped;
    uint64_t nativeDataPeerNotifications, nativeDataPeerLockMisses;
    AirportAWDL::ServiceMergeCache serviceMerge;
    bool serviceMergeEnabled;
    int32_t reportedLinkQuality;
    AirportAWDL::SyncEngine sync;
    uint8_t frameTemplate[1024];
    size_t templateSize;
    // One 16-slot channel cycle plus a full slot for the next common grant;
    // agreed hardware TX BlockAck scales staging to absorb multi-slot parked bursts.
    static constexpr unsigned DataQueueCapacity = 96;
    static constexpr unsigned TXBAQueueCapacity = 384;
    static constexpr uint64_t DataLifetimeUS = 17ULL * 4 * 16 * 1024;
    static constexpr uint8_t MaxDataFlushes = 8;
    struct Pending {
        enum : uint8_t { MulticastUnpaired, MulticastSocial, MulticastSlot8 };
        mbuf_t packet;
        uint64_t queuedUS, epoch, grant;
        uint16_t wifiSequence, dataSequence;
        bool retry, group, lowLatency;
        uint8_t multicastQueue, flushes;
        mbuf_t second;
        uint64_t secondQueuedUS;
        uint16_t secondSequence;
        unsigned count() const { return second ? 2 : 1; }
    } pending[TXBAQueueCapacity];
    void releaseData(Pending &);
    struct Flight { Pending entry; uint64_t token; } inFlight[TXBAQueueCapacity];
    unsigned inFlightCount;
    unsigned aggregateExtraOwned;
    bool outputCapacityReleased;
    uint64_t dataToken, dataSubmitted, dataRetried, dataFlushed, dataTxExpired;
    uint64_t dataUnicastAcknowledged, dataGroupCompleted, dataAbandoned;
    uint64_t multicastCopiesQueued, multicastCopiesDropped;
    uint64_t dataAggregatesSubmitted;
    unsigned pendingHead, pendingCount;
};
#endif
#endif
