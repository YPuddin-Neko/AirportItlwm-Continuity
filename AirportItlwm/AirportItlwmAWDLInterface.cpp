// SPDX-License-Identifier: GPL-2.0-or-later
#include "AirportItlwmAWDLInterface.hpp"
#if defined(AIRPORT_WCL) && __IO80211_TARGET == __MAC_15_2
#include "AirportItlwmV2.hpp"
#include "AirportItlwmSkywalkInterface.hpp"
#include "AirportItlwmEthernetInterface.hpp"
#include "Airport/macOS15/IO80211ManagementBuffer.h"
#include "Airport/macOS15/IOSkywalkSequoiaQueues.h"
#include "AWDL/NativeFrame.hpp"
#include "AWDL/NativePeer.hpp"
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/kpi_mbuf.h>
#include <libkern/c++/OSDictionary.h>
extern IOCommandGate *_fCommandGate;
#if DEBUG
#include "AWDL/NativePeerSnapshot.hpp"
#include <libkern/c++/OSBoolean.h>
#include <IOKit/IOUserClient.h>
#include <kern/task.h>
#endif
using namespace AirportAWDL;
static mbuf_t awdlPacket(size_t length);
#ifdef KERNEL
static IOSkywalkPacketBufferPool *gLLWTxPool = nullptr;
#endif

OSDefineMetaClassAndStructors(AirportItlwmLLWInterface, IO80211InfraInterface);

bool AirportItlwmLLWInterface::initWithAWDL(IO80211Controller *controller,
                                            AirportItlwmAWDLInterface *parent,
                                            const ether_addr *mac)
{
    if (!controller || !parent || !mac || !unicastAddress(mac->octet)) return false;
    if (!IO80211InfraInterface::init()) return false;
    address = *mac;
    memcpy(llwUniqueID, "llw0", 4);
    memcpy(llwUniqueID + 4, mac->octet, 6);
    awdlParent = parent;
    enabledState = false;
    skywalkRegistered = false;
    skywalkPool = nullptr;
#ifdef KERNEL
    OSSafeReleaseNULL(gLLWTxPool);
#endif
    txSubQueue = nullptr;
    txCompQueue = nullptr;
    rxCompQueue = nullptr;
    queueWorkLoop = nullptr;
    skywalkTxPackets = skywalkTxDropped = skywalkRxPackets = skywalkRxDropped = 0;
    setInterfaceRole(APPLE80211_VIF_LOW_LATENCY);
    setLowLatencyEnabled(true);
    setInitMacAddress(address);
    setParentInterface(parent);
    return true;
}

bool AirportItlwmLLWInterface::start(IOService *provider)
{
    return IO80211InfraInterface::start(provider);
}

void AirportItlwmLLWInterface::stop(IOService *provider)
{
    deregisterSkywalk();
    enabledState = false;
    awdlParent = nullptr;
    IO80211InfraInterface::stop(provider);
}

void AirportItlwmLLWInterface::free()
{
    deregisterSkywalk();
    awdlParent = nullptr;
    IO80211InfraInterface::free();
}

bool AirportItlwmLLWInterface::registerSkywalk(IOWorkLoop *wl)
{
    if (!wl || skywalkRegistered || !unicastAddress(address.octet)) return false;
    IOSkywalkPacketBufferPool::PoolOptions txOpts = {};
    txOpts.packetCount = 512;
    txOpts.bufferCount = 512;
    txOpts.bufferSize = 2048;
    txOpts.maxBuffersPerPacket = 1;
    txOpts.memorySegmentSize = 0;
    txOpts.poolFlags = 0;
    IOSkywalkPacketBufferPool::PoolOptions opts = {};
    opts.packetCount = 2048;
    opts.bufferCount = 2048;
    opts.bufferSize = 2048;
    opts.maxBuffersPerPacket = 1;
    opts.memorySegmentSize = 0;
    opts.poolFlags = 0;
    IOSkywalkPacketBufferPool *txPool = IOSkywalkPacketBufferPool::withName(
        "llw0-tx", this, kIOSkywalkPacketTypeNetwork, &txOpts);
#ifdef KERNEL
    OSSafeReleaseNULL(gLLWTxPool);
    gLLWTxPool = txPool;
#endif
    if (!txPool) goto fail;
    skywalkPool = IOSkywalkPacketBufferPool::withName("llw0", this, kIOSkywalkPacketTypeNetwork, &opts);
    if (!skywalkPool) {
#ifndef KERNEL
        OSSafeReleaseNULL(txPool);
#endif
        goto fail;
    }
    txSubQueue = IOSkywalkTxSubmissionQueue::withPool(txPool, 256, 0, this, txSubmissionAction, this, 0);
    txCompQueue = IOSkywalkTxCompletionQueue::withPool(txPool, 256, 0, this, nullptr, nullptr, 0);
    rxCompQueue = IOSkywalkRxCompletionQueue::withPool(skywalkPool, 512, 0, this, nullptr, nullptr, 0);
    if (!txSubQueue || !txCompQueue || !rxCompQueue) {
#ifndef KERNEL
        OSSafeReleaseNULL(txPool);
#endif
        goto fail;
    }
    if (wl->addEventSource(txSubQueue) != kIOReturnSuccess ||
        wl->addEventSource(txCompQueue) != kIOReturnSuccess ||
        wl->addEventSource(rxCompQueue) != kIOReturnSuccess) {
        queueWorkLoop = wl;
#ifndef KERNEL
        OSSafeReleaseNULL(txPool);
#endif
        goto fail;
    }
    queueWorkLoop = wl;
    {
        IOSkywalkEthernetInterface::RegistrationInfo info = {};
        if (!initRegistrationInfo(&info, 1, sizeof(info))) {
#ifndef KERNEL
            OSSafeReleaseNULL(txPool);
#endif
            goto fail;
        }
        const uint32_t subFamily = 3; // IFNET_SUBFAMILY_WIFI
        memcpy(info.bytes + 0x0c, &subFamily, sizeof(subFamily));
        const int32_t unit = 0;
        memcpy(info.bytes + 0x38, &unit, sizeof(unit));
        memcpy(info.bytes + 0x108, address.octet, sizeof(address.octet));
        IOSkywalkPacketQueue *queues[3] = {txSubQueue, txCompQueue, rxCompQueue};
        if (registerEthernetInterface(&info, queues, 3, txPool, skywalkPool, 0) != kIOReturnSuccess) {
#ifndef KERNEL
            OSSafeReleaseNULL(txPool);
#endif
            goto fail;
        }
    }
    skywalkRegistered = true;
    setProperty("HiddenInterface", kOSBooleanTrue);
    return true;
fail:
    deregisterSkywalk();
    return false;
}

void AirportItlwmLLWInterface::deregisterSkywalk()
{
    if (txSubQueue) txSubQueue->disable();
    if (txCompQueue) txCompQueue->disable();
    if (rxCompQueue) rxCompQueue->disable();
    if (skywalkRegistered) {
        deregisterEthernetInterface(0);
        skywalkRegistered = false;
    }
    if (queueWorkLoop) {
        if (txSubQueue) queueWorkLoop->removeEventSource(txSubQueue);
        if (txCompQueue) queueWorkLoop->removeEventSource(txCompQueue);
        if (rxCompQueue) queueWorkLoop->removeEventSource(rxCompQueue);
        queueWorkLoop = nullptr;
    }
    OSSafeReleaseNULL(txSubQueue);
    OSSafeReleaseNULL(txCompQueue);
    OSSafeReleaseNULL(rxCompQueue);
    OSSafeReleaseNULL(skywalkPool);
#ifdef KERNEL
    OSSafeReleaseNULL(gLLWTxPool);
#endif
}

SInt32 AirportItlwmLLWInterface::initBSDInterfaceParameters(ifnet_init_eparams *eparams,
                                                            sockaddr_dl **lladdr)
{
    SInt32 ret = IO80211InfraInterface::initBSDInterfaceParameters(eparams, lladdr);
    if (ret == 0 && eparams) {
        auto *raw = reinterpret_cast<uint8_t *>(eparams);
        const void *uid = llwUniqueID;
        const uint32_t uidLen = sizeof(llwUniqueID);
        memcpy(raw + 0x10, &uid, sizeof(uid));
        memcpy(raw + 0x18, &uidLen, sizeof(uidLen));
    }
    return ret;
}

unsigned int AirportItlwmLLWInterface::txSubmissionAction(OSObject *owner,
                                                          IOSkywalkTxSubmissionQueue *queue,
                                                          IOSkywalkPacket * const *packets,
                                                          unsigned int packetCount,
                                                          void *)
{
    auto *self = static_cast<AirportItlwmLLWInterface *>(owner);
    if (!self || !packets || !packetCount) return 0;
    for (unsigned int i = 0; i < packetCount; ++i) {
        IOSkywalkPacket *pkt = packets[i];
        if (!pkt || !self->awdlParent) {
            ++self->skywalkTxDropped;
            continue;
        }
        if (queue && pkt->prepare(queue, 0, 0) != kIOReturnSuccess) {
            ++self->skywalkTxDropped;
            continue;
        }
        const uint32_t len = pkt->getDataLength();
        const uint16_t off = pkt->getDataOffset();
        const uint32_t bufSize = pkt->getBufferSize();
        const auto *base = static_cast<const uint8_t *>(pkt->getDataVirtualAddress());
        if (!base || len < 14 || len > 1514 || uint32_t(off) + len > bufSize) {
            ++self->skywalkTxDropped;
            continue;
        }
        mbuf_t m = awdlPacket(len);
        if (!m) {
            ++self->skywalkTxDropped;
            continue;
        }
        memcpy(mbuf_data(m), base + off, len);
        if (self->awdlParent->enqueueLLWFrame(m) == kIOReturnSuccess)
            ++self->skywalkTxPackets;
        else
            ++self->skywalkTxDropped;
    }
    if (self->txCompQueue)
        self->txCompQueue->enqueuePackets(packets, packetCount, 0);
    return packetCount;
}

bool AirportItlwmLLWInterface::inputSkywalkEthernet(const uint8_t *ethernet, size_t length)
{
    if (!skywalkRegistered || !enabledState || !skywalkPool || !rxCompQueue ||
        !rxCompQueue->isQueueReady() || !ethernet || length < 14 || length > 1514) {
        ++skywalkRxDropped;
        return false;
    }
    IOSkywalkPacket *pkt = nullptr;
    if (skywalkPool->allocatePacket(&pkt, 1) != kIOReturnSuccess || !pkt) {
        ++skywalkRxDropped;
        return false;
    }
    // Pool-allocated RX packets have no submission queue; passing rxCompQueue
    // sets fSourceQueue and triggers IOSkywalkPacketQueue::packetCompletion().
    if (pkt->prepare(nullptr, 0, 0) != kIOReturnSuccess) {
        skywalkPool->deallocatePacket(pkt);
        ++skywalkRxDropped;
        return false;
    }
    void *dst = pkt->getDataVirtualAddress();
    if (!dst || pkt->getBufferSize() < length ||
        pkt->setDataOffsetAndLength(0, uint32_t(length)) != kIOReturnSuccess) {
        skywalkPool->deallocatePacket(pkt);
        ++skywalkRxDropped;
        return false;
    }
    memcpy(dst, ethernet, length);
    static_cast<IOSkywalkNetworkPacket *>(pkt)->setLinkHeaderLength(14);
    IOSkywalkPacket *arr[1] = {pkt};
    if (rxCompQueue->enqueuePackets(arr, 1, 0) != kIOReturnSuccess) {
        skywalkPool->deallocatePacket(pkt);
        ++skywalkRxDropped;
        return false;
    }
    ++skywalkRxPackets;
    return true;
}

IOReturn AirportItlwmLLWInterface::getHardwareAddress(ether_addr *out)
{
    if (!out) return kIOReturnBadArgument;
    *out = address;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmLLWInterface::setMacAddress(ether_addr &mac)
{
    return sameAddress(address.octet, mac.octet) ? kIOReturnSuccess : kIOReturnUnsupported;
}

SInt32 AirportItlwmLLWInterface::setInterfaceEnable(bool enabled)
{
    if (enabled != enabledState) {
        SInt32 result = IO80211InfraInterface::setInterfaceEnable(enabled);
        if (result) return result;
        enabledState = enabled;
    }
    if (enabled) {
        if (txSubQueue) txSubQueue->enable();
        if (txCompQueue) txCompQueue->enable();
        if (rxCompQueue) rxCompQueue->enable();
        reportLinkStatus(3, 0x80);
        reportLinkQuality(100);
        IO80211InfraInterface::setLinkState(kIO80211NetworkLinkUp, 1, false, 0);
#ifdef KERNEL
        reportDataBandwidths(866700000ULL, 866700000ULL, 433300000ULL, 433300000ULL);
#endif
    } else {
        if (txSubQueue) txSubQueue->disable();
        if (txCompQueue) txCompQueue->disable();
        if (rxCompQueue) rxCompQueue->disable();
        reportLinkStatus(1, 0);
        reportLinkQuality(uint32_t(-2));
        IO80211InfraInterface::setLinkState(kIO80211NetworkLinkDown, 0, false, 0);
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwmLLWInterface::processBSDCommand(struct __ifnet *ifp, unsigned int cmd, void *data)
{
    // Companion low-latency interfaces reject Apple80211 control ioctls with EINVAL.
    switch (cmd) {
    case 0x802069c8U:
    case 0x802869c8U:
    case 0xc02069c9U:
    case 0xc02869c9U:
        return EINVAL;
    default:
        return IO80211InfraInterface::processBSDCommand(ifp, cmd, data);
    }
}

#define super IO80211AWDLProtocol
OSDefineMetaClassAndStructors(AirportItlwmAWDLInterface, IO80211AWDLProtocol);

IOReturn AirportItlwmAWDLInterface::getSSID(apple80211_ssid_data* data) { return request(Request::getSSID, data); }
IOReturn AirportItlwmAWDLInterface::getCHANNEL(apple80211_channel_data* data) { return request(Request::getCHANNEL, data); }
IOReturn AirportItlwmAWDLInterface::getBSSID(apple80211_bssid_data* data) { return request(Request::getBSSID, data); }
IOReturn AirportItlwmAWDLInterface::getSTATE(apple80211_state_data* data) { return request(Request::getSTATE, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params* data) { return request(Request::getAWDL_SYNC_PARAMS, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled* data) { return request(Request::getAWDL_SYNC_ENABLED, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter* data) { return request(Request::getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_ELECTION_ID(apple80211_awdl_election_id* data) { return request(Request::getAWDL_ELECTION_ID, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric* data) { return request(Request::getAWDL_ELECTION_METRIC, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence* data) { return request(Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, data); }
IOReturn AirportItlwmAWDLInterface::getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size* data) { return request(Request::getPEER_CACHE_MAXIMUM_SIZE, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel* data) { return request(Request::getAWDL_MASTER_CHANNEL, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_SYNC_STATE(apple80211_awdl_sync_state* data) { return request(Request::getAWDL_SYNC_STATE, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds* data) { return request(Request::getAWDL_ELECTION_RSSI_THRESHOLDS, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode* data) { return request(Request::getAWDL_PRESENCE_MODE, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params* data) { return request(Request::getAWDL_ELECTION_PARAMS, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities* data) { return request(Request::getAWDL_DEVICE_CAPABILITIES, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel* data) { return request(Request::getAWDL_SECONDARY_MASTER_CHANNEL, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode* data) { return request(Request::getAWDL_AF_TX_MODE, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_MIN_RATE(apple80211_awdl_min_rate* data) { return request(Request::getAWDL_MIN_RATE, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_ENCRYPTION_TYPE(apple80211_encrypt* data) { return request(Request::getAWDL_ENCRYPTION_TYPE, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period* data) { return request(Request::getAWDL_TXCAL_PERIOD, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_DEV_STATS(apple80211_awdl_dev_stats* data) { return request(Request::getAWDL_DEV_STATS, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config* data) { return request(Request::getAWDL_DFSP_CONFIG, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_CCA(apple80211_awdl_cca* data) { return request(Request::getAWDL_CCA, data); }
IOReturn AirportItlwmAWDLInterface::getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap* data) { return request(Request::getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES, data); }
IOReturn AirportItlwmAWDLInterface::getLLW_PARAMS(apple80211_llw_params* data) { return request(Request::getLLW_PARAMS, data); }
IOReturn AirportItlwmAWDLInterface::setSSID(apple80211_ssid_data* data) { return request(Request::setSSID, data); }
IOReturn AirportItlwmAWDLInterface::setCHANNEL(apple80211_channel_data* data) { return request(Request::setCHANNEL, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params* data) { return request(Request::setAWDL_SYNC_PARAMS, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled* data) { return request(Request::setAWDL_SYNC_ENABLED, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter* data) { return request(Request::setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_ELECTION_ID(apple80211_awdl_election_id* data) { return request(Request::setAWDL_ELECTION_ID, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_BSSID(apple80211_awdl_bssid* data) { return request(Request::setAWDL_BSSID, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric* data) { return request(Request::setAWDL_ELECTION_METRIC, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence* data) { return request(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel* data) { return request(Request::setAWDL_MASTER_CHANNEL, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_SYNC_STATE(apple80211_awdl_sync_state* data) { return request(Request::setAWDL_SYNC_STATE, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds* data) { return request(Request::setAWDL_ELECTION_RSSI_THRESHOLDS, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode* data) { return request(Request::setAWDL_PRESENCE_MODE, data); }
IOReturn AirportItlwmAWDLInterface::setPEER_CACHE_CONTROL(apple80211_peer_cache_control* data) { return request(Request::setPEER_CACHE_CONTROL, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_OOB_REQUEST(apple80211_awdl_oob_request* data) { return request(Request::setAWDL_OOB_REQUEST, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template* data) { return request(Request::setAWDL_SYNC_FRAME_TEMPLATE, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params* data) { return request(Request::setAWDL_ELECTION_PARAMS, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration* data) { return request(Request::setAWDL_PEER_TRAFFIC_REGISTRATION, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel* data) { return request(Request::setAWDL_SECONDARY_MASTER_CHANNEL, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode* data) { return request(Request::setAWDL_AF_TX_MODE, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_MIN_RATE(apple80211_awdl_min_rate* data) { return request(Request::setAWDL_MIN_RATE, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_ENCRYPTION_KEYS(apple80211_key* data) { return request(Request::setAWDL_ENCRYPTION_KEYS, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_ENCRYPTION_TYPE(apple80211_encrypt* data) { return request(Request::setAWDL_ENCRYPTION_TYPE, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request* data) { return request(Request::setAWDL_OOB_AUTO_REQUEST, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period* data) { return request(Request::setAWDL_TXCAL_PERIOD, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add* data) { return request(Request::setAWDL_AWDL_ADVERTISERS, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config* data) { return request(Request::setAWDL_DFSP_CONFIG, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_DFSP_UCSA_CONFIG(apple80211_awdl_dfsp_ucsa_config* data) { return request(Request::setAWDL_DFSP_UCSA_CONFIG, data); }
IOReturn AirportItlwmAWDLInterface::setCOMPANION_SKYWALK_IF_CREATE(apple80211_companion_skywalk_if_create* data) { return request(Request::setCOMPANION_SKYWALK_IF_CREATE, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_OOB_SEC_PAYLOAD_REQUEST(apple80211_awdl_af_sec_payload_request* data) { return request(Request::setAWDL_OOB_SEC_PAYLOAD_REQUEST, data); }
IOReturn AirportItlwmAWDLInterface::setLLW_PARAMS(apple80211_llw_params* data) { return request(Request::setLLW_PARAMS, data); }
IOReturn AirportItlwmAWDLInterface::setTX_RATE(apple80211_tx_rate* data) { return request(Request::setTX_RATE, data); }
IOReturn AirportItlwmAWDLInterface::setP2P_COEX_PROTECTION(apple80211_p2p_coex_protection* data) { return request(Request::setP2P_COEX_PROTECTION, data); }
IOReturn AirportItlwmAWDLInterface::setPSF_DWELL(apple80211_awdl_psf_dwell_params* data) { return request(Request::setPSF_DWELL, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_CONFIG(apple80211_awdl_config_request* data) { return request(Request::setAWDL_CONFIG, data); }
IOReturn AirportItlwmAWDLInterface::setAWDL_RTG_PEER_STATS_CONFIG(apple80211_rtg_peer_stats_cfg* data) { return request(Request::setAWDL_RTG_PEER_STATS_CONFIG, data); }

static uint64_t awdlNow()
{
    timeval now; getmicrouptime(&now);
    return uint64_t(now.tv_sec) * 1000000 + now.tv_usec;
}

static mbuf_t awdlPacket(size_t length)
{
    mbuf_t packet = nullptr;
    unsigned chunks = 1;
    if (mbuf_allocpacket(MBUF_DONTWAIT, length, &chunks, &packet)) return nullptr;
    mbuf_setlen(packet, length); mbuf_pkthdr_setlen(packet, length);
    return packet;
}

bool AirportItlwmAWDLInterface::init(IO80211Controller *controller, ether_addr *mac,
                                    unsigned role, const char *prefix)
{
    if (!mac || role != APPLE80211_VIF_AWDL || !unicastAddress(mac->octet)) return false;
    auto owner = OSDynamicCast(AirportItlwm, controller);
    radio = owner ? OSDynamicCast(ItlIwx, owner->fHalService) : nullptr;
    if (!radio) return false;
    radio->retain();
    address = *mac;
    interfaceEnabled = false;
    infrastructurePaused = false;
    nativeSequenceValid = false;
    nativeDataPeerNotifications = 0;
    nativeDataPeerLockMisses = 0;
    serviceMerge = {};
    serviceMergeEnabled = true;
    reportedLinkQuality = -2;
    nativeSyncReady = false;
    drainingData = false;
    nativeSyncStateCalls = 0;
    llwInterface = nullptr;
    llwBSD = nullptr;
    llwActive = false;
    llwLinkUp = false;
    llwParamsConfigured = false;
    llwParamsEnabled = false;
    llwParams = {};
    llwFlows.clear();
    llwCreateRequests = 0;
    llwDestroyRequests = 0;
    llwParamsGetCalls = 0;
    llwParamsSetCalls = 0;
    llwTxQueued = 0;
    llwRxDelivered = 0;
    llwRxDropped = 0;
    llwNDPMirrored = 0;
#if DEBUG
    lastNativePeerSnapshotUS = 0;
    relayRecoveryWindowStarts = 0;
    parentRecoveryWindowStarts = 0;
    nativePeerSampled = false;
#endif
    if (!super::init(controller, mac, role, prefix)) return false;
    sync.start(mac->octet, 0, awdlNow());
    return true;
}

bool AirportItlwmAWDLInterface::start(IOService *provider)
{
    if (!super::start(provider)) return false;
    IOTimerEventSource::Action tickAction = [](OSObject *owner, IOTimerEventSource *) {
        static_cast<AirportItlwmAWDLInterface *>(owner)->tick();
    };
    timer = IOTimerEventSource::timerEventSource(
        kIOTimerEventSourceOptionsPriorityHigh, this, tickAction);
    if (!timer)
        timer = IOTimerEventSource::timerEventSource(this, tickAction);
    if (!timer || radio->getMainWorkLoop()->addEventSource(timer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(timer);
        super::stop(provider);
        return false;
    }
    radio->getMainCommandGate()->runAction(
        [](OSObject *, void *arg, void *, void *, void *) -> IOReturn {
            auto self = static_cast<AirportItlwmAWDLInterface *>(arg);
            self->radio->awdlReceiver = self;
            self->radio->awdlInput = receive;
            self->radio->awdlActionInput = receiveAction;
            self->radio->awdlBlockAckInput = receiveBlockAck;
            self->radio->awdlBlockAckEndInput = receiveBlockAckEnd;
            self->radio->awdlAvailabilityChanged = [](void *owner) {
                static_cast<AirportItlwmAWDLInterface *>(owner)->updateAvailability();
            };
            self->radio->awdlRegulatoryChanged = [](void *owner) {
                static_cast<AirportItlwmAWDLInterface *>(owner)->regulatoryMapChanged();
            };
            self->radio->awdlTxComplete = [](void *owner, uint64_t token, bool acknowledged) {
                static_cast<AirportItlwmAWDLInterface *>(owner)->completeUnicast(token, acknowledged);
            };
            AirportAWDL::setTxFlushedCallbackOn(*self->radio, [](void *owner, uint64_t token) {
                static_cast<AirportItlwmAWDLInterface *>(owner)->completeManagement(token, false, true);
            });
            self->radio->awdlDataComplete = [](void *owner, uint64_t token, int result) {
                static_cast<AirportItlwmAWDLInterface *>(owner)->completeData(token, result);
            };
            self->radio->awdlRadio.txReady = txReady;
            self->publishReceiveFilter();
            return kIOReturnSuccess;
        }, this);
    timer->setTimeoutMS(100);
    return true;
}

void AirportItlwmAWDLInterface::releaseData(Pending &entry)
{
    if (entry.second) {
        mbuf_freem(entry.second);
        entry.second = nullptr;
        --aggregateExtraOwned;
    }
    mbuf_freem(entry.packet);
    entry.packet = nullptr;
}

void AirportItlwmAWDLInterface::drain()
{
    serviceMerge.clear();
    llwFlows.clear();
    for (auto &reply : blockAckReplies) {
        if (reply.state != BlockAckReply::Free) ++blockAckFailed;
        reply = {};
    }
    for (auto &peer : radio->awdlPeers.peers) peer.lastUnicastDataUS = 0;
    for (auto &flight : inFlight) {
        if (!flight.entry.packet) continue;
        txDropped += flight.entry.count();
        dataAbandoned += flight.entry.count();
        releaseData(flight.entry);
        flight = {};
    }
    inFlightCount = 0;
    outputCapacityReleased = false;
    constexpr unsigned queueCapacity = sizeof(pending) / sizeof(pending[0]);
    while (pendingCount) {
        auto &entry = pending[pendingHead];
        if (entry.retry) dataAbandoned += entry.count();
        txDropped += entry.count();
        releaseData(entry); entry = {};
        pendingHead = (pendingHead + 1) % queueCapacity; --pendingCount;
    }
}

void AirportItlwmAWDLInterface::stop(IOService *provider)
{
    radio->getMainCommandGate()->runAction(
        [](OSObject *, void *arg, void *, void *, void *) -> IOReturn {
            auto self = static_cast<AirportItlwmAWDLInterface *>(arg);
            self->stopping = true;
            self->requested = false;
            if (self->bsd && self->bsd->getIfnet() && self->reportedLinkQuality != -2) {
                self->reportLinkQuality(uint32_t(-2));
                self->reportedLinkQuality = -2;
            }
            self->llwActive = false;
            self->llwLinkUp = false;
            self->llwParamsEnabled = false;
            if (self->llwInterface)
                self->llwInterface->setInterfaceEnable(false);
            if (self->llwBSD) {
                self->llwBSD->stopOutputThread();
                if (self->llwBSD->getIfnet())
                    self->llwBSD->setLinkState(kIO80211NetworkLinkDown);
            }
            self->updateAvailability();
            self->serviceUnicast();
            self->wakeArmed = false;
            if (self->timer) self->timer->cancelTimeout();
            self->radio->awdlInput = nullptr;
            self->radio->awdlActionInput = nullptr;
            self->radio->awdlBlockAckInput = nullptr;
            self->radio->awdlBlockAckEndInput = nullptr;
            self->radio->awdlAvailabilityChanged = nullptr;
            self->radio->awdlRegulatoryChanged = nullptr;
            self->radio->awdlTxComplete = nullptr;
            AirportAWDL::setTxFlushedCallbackOn(*self->radio, nullptr);
            self->radio->awdlDataComplete = nullptr;
            self->radio->awdlRadio.txReady = nullptr;
            self->radio->awdlReceiver = nullptr;
            self->radio->awdlPeers.clearCache();
            self->radio->stopAWDLRadio();
            self->drain();
            return kIOReturnSuccess;
        }, this);
    if (timer) {
        radio->getMainWorkLoop()->removeEventSource(timer);
        OSSafeReleaseNULL(timer);
    }
    super::stop(provider);
}

void AirportItlwmAWDLInterface::free()
{
    OSSafeReleaseNULL(radio);
    super::free();
}

IOReturn AirportItlwmAWDLInterface::setProperties(OSObject *properties)
{
    auto dictionary = OSDynamicCast(OSDictionary, properties);
    auto value = dictionary ? dictionary->getObject("AWDLReceiveAllData") : nullptr;
    auto compare = dictionary ? dictionary->getObject("AWDLCompareTxAntennas") : nullptr;
    if (compare) value = compare;
    if (!value) return super::setProperties(properties);
#if DEBUG
    auto enabled = OSDynamicCast(OSBoolean, value);
    if (dictionary->getCount() != 1 || !enabled) return kIOReturnBadArgument;
    const IOReturn privilege = IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator);
    if (privilege != kIOReturnSuccess) return privilege;
    if (!radio || stopping) return kIOReturnNotReady;
    struct Change { bool compare, enabled; } change = {compare != nullptr, enabled->getValue()};
    auto apply = [](OSObject *, void *self, void *request, void *, void *) -> IOReturn {
        auto interface = static_cast<AirportItlwmAWDLInterface *>(self);
        auto input = static_cast<const Change *>(request);
        return input->compare ? interface->setCompareTxAntennas(input->enabled) :
            interface->setReceiveAllData(input->enabled);
    };
    if (radio->getMainWorkLoop()->inGate()) return apply(nullptr, this, &change, nullptr, nullptr);
    return radio->getMainCommandGate()->runAction(apply, this, &change);
#else
    return kIOReturnUnsupported;
#endif
}

void AirportItlwmAWDLInterface::publishReceiveFilter()
{
#if DEBUG
    setProperty("AWDLReceiveAllDataSupported", !radio->awdlRadio.listener);
    setProperty("AWDLReceiveAllData", radio->awdlReceiveAllData);
    setProperty("AWDLReceiveFilterKnown", radio->awdlReceiveFilterKnown);
    setProperty("AWDLReceiveFilterCommandError", uint64_t(radio->awdlReceiveFilterCommandError), 32);
    setProperty("AWDLReceiveFilterCleanupResult", uint64_t(radio->awdlReceiveFilterCleanupResult), 32);
    setProperty("AWDLCompareTxAntennas", radio->awdlCompareArm() != 0);
    setProperty("AWDLCompareEndUptimeUS", radio->awdlCompareEndUS, 64);
    setProperty("AWDLCompareSubmittedA", radio->awdlCompareArmSubmitted[0], 64);
    setProperty("AWDLCompareSubmittedB", radio->awdlCompareArmSubmitted[1], 64);
    setProperty("AWDLCompareBTOverrides", radio->awdlCompareBTOverrides, 64);
#else
    setProperty("AWDLReceiveAllDataSupported", false);
    const bool applied = radio->awdlRadio.ready && radio->awdlRadio.mac && !radio->awdlRadio.fault;
    setProperty("AWDLReceiveAllData", applied && (radio->awdlRadio.listener ?
        radio->awdlRadio.listenerFilterKnown && radio->awdlRadio.listenerPromisc : radio->awdlRadio.quz0034));
    setProperty("AWDLReceiveFilterKnown", applied &&
        (!radio->awdlRadio.listener || radio->awdlRadio.listenerFilterKnown));
    setProperty("AWDLReceiveFilterCommandError", uint64_t(0), 32);
    setProperty("AWDLReceiveFilterCleanupResult", uint64_t(kIOReturnUnsupported), 32);
#endif
}

#if DEBUG
IOReturn AirportItlwmAWDLInterface::setCompareTxAntennas(bool enabled)
{
    if (!radio || stopping) return kIOReturnNotReady;
    if (enabled) {
        const auto &state = radio->awdlRadio;
        if (!requested || !running || !interfaceEnabled || regulatoryPlanBlocked || regulatoryStopRequired ||
            state.stopPending || state.fault || !state.ready || !state.awake || !state.mac)
            return kIOReturnNotReady;
        if (state.changing || receiveFilterChanging || radio->awdlCompareArm()) return kIOReturnBusy;
        if ((radio->iwx_fw_valid_tx_ant(&radio->com) & 3) != 3) return kIOReturnUnsupported;
        radio->awdlCompareArmSubmitted[0] = radio->awdlCompareArmSubmitted[1] = radio->awdlCompareBTOverrides = 0;
        radio->awdlCompareEndUS = awdlNow() + 30000000;
    } else {
        radio->awdlCompareEndUS = 0;
    }
    publishReceiveFilter();
    return kIOReturnSuccess;
}

void AirportItlwmAWDLInterface::publishNativePeers(uint64_t now)
{
    if (!requested || stopping || !radio || radio->awdlRadio.changing ||
        receiveFilterChanging) return;
    auto controller = getController();
    auto queue = controller ? controller->getWorkQueue() : nullptr;
    if (!queue || !queue->inGate() || !queue->onThread()) return;
    if (nativePeerSampled && now >= lastNativePeerSnapshotUS &&
        now - lastNativePeerSnapshotUS < 10000000) return;
    nativePeerSampled = true;
    lastNativePeerSnapshotUS = now;

    int error = ENXIO;
    uint32_t used = 0;
    NativePeerSnapshotControl *control = nullptr;
    auto manager = AirportAWDLGetNativePeerManager(this);
    if (manager) {
        error = EINVAL;
        if (manager->metaCast("IO80211AWDLPeerManager")) {
            control = static_cast<NativePeerSnapshotControl *>(IOMallocZero(sizeof(*control)));
            error = ENOMEM;
            if (control) {
                control->version = 1;
                control->capacity = NativePeerSnapshotCapacity;
                // On the workloop thread, the native data-path lock fails on contention without sleeping the gate.
                error = AirportAWDLPrintNativePeers(manager, control);
                if (!error && control->used > NativePeerSnapshotCapacity) error = EOVERFLOW;
                if (!error) {
                    used = control->used;
                    control->text[used] = 0;
                }
            }
        }
    }
    setProperty("NativePeerSummary", !error ? control->text : "");
    setProperty("NativePeerSummaryError", uint64_t(uint32_t(error)), 32);
    setProperty("NativePeerSummaryBytes", uint64_t(used), 32);
    setProperty("NativePeerSummaryTimeUS", now, 64);
    if (control) IOFree(control, sizeof(*control));
}

IOReturn AirportItlwmAWDLInterface::setReceiveAllData(bool enabled)
{
    if (!radio || stopping || !requested || !running || !interfaceEnabled ||
        regulatoryPlanBlocked || regulatoryStopRequired)
        return kIOReturnNotReady;
    auto &state = radio->awdlRadio;
    if (state.fault || state.stopPending || !state.ready || !state.mac || !state.awake)
        return kIOReturnNotReady;
    if (state.listener) return kIOReturnUnsupported;
    if (state.changing || ticking || receiveFilterChanging) return kIOReturnBusy;
    if (radio->awdlReceiveFilterKnown && radio->awdlReceiveAllData == enabled)
        return kIOReturnSuccess;

    retain();
    receiveFilterChanging = state.changing = true;
    const uint64_t epoch = radio->dmaEpoch;
    radio->awdlReceiveFilterKnown = false;
    publishReceiveFilter();
    const int error = radio->awdlMAC(IWX_FW_CTXT_ACTION_MODIFY, enabled);
    IOReturn result = error ? kIOReturnIOError : kIOReturnSuccess;
    if (epoch == radio->dmaEpoch) {
        state.changing = false;
        radio->awdlReceiveFilterCommandError = error;
        radio->awdlReceiveFilterCleanupResult = kIOReturnSuccess;
        IOReturn cleanupResult = kIOReturnSuccess;
        if (error) {
            // A timed-out command may have changed the filter; halt AWDL before further TX.
            if (stopping) {
                const int cleanup = radio->stopAWDLRadio();
                cleanupResult = cleanup == EINPROGRESS ? kIOReturnBusy :
                    (cleanup ? kIOReturnIOError : kIOReturnSuccess);
            } else {
                cleanupResult = setEnabled(false);
                if (epoch == radio->dmaEpoch && state.stopPending) cleanupResult = kIOReturnBusy;
            }
        } else if (!requested || stopping) {
            // A native disable can arrive while the command releases the gate.
            const int cleanup = radio->stopAWDLRadio();
            cleanupResult = cleanup == EINPROGRESS ? kIOReturnBusy :
                (cleanup ? kIOReturnIOError : kIOReturnSuccess);
            if (cleanup && cleanup != EINPROGRESS) result = kIOReturnIOError;
        }
        if (epoch == radio->dmaEpoch) radio->awdlReceiveFilterCleanupResult = cleanupResult;
    }
    if (epoch != radio->dmaEpoch) result = kIOReturnNotReady;
    receiveFilterChanging = false;
    publishReceiveFilter();
    if (!stopping && timer) timer->setTimeoutMS(1);
    release();
    return result;
}
#endif

IOReturn AirportItlwmAWDLInterface::getHardwareAddress(ether_addr *out)
{
    if (!out) return kIOReturnBadArgument;
    *out = address;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::setMacAddress(ether_addr &mac)
{
    // Changing an active direct-link address also changes IPv6 link-local identity.
    return sameAddress(address.octet, mac.octet) ? kIOReturnSuccess : kIOReturnUnsupported;
}

SInt32 AirportItlwmAWDLInterface::setInterfaceEnable(bool enabled)
{
    if (!radio || stopping) return kIOReturnNotReady;
    if (!radio->getMainWorkLoop()->inGate())
        return radio->getMainCommandGate()->runAction(
            [](OSObject *, void *self, void *on, void *, void *) -> IOReturn {
                return static_cast<AirportItlwmAWDLInterface *>(self)->setInterfaceEnable(on != nullptr);
            }, this, enabled ? this : nullptr);
    SInt32 result = IO80211VirtualInterface::setInterfaceEnable(enabled);
    if (result) return result;
    interfaceEnabled = enabled;
    if (!enabled) {
        radio->awdlPeers.clearCache();
        serviceMerge.clear();
        IO80211VirtualInterface::setLinkState(kIO80211NetworkLinkDown, 0);
        if (bsd && bsd->getIfnet()) {
            bsd->setLinkState(kIO80211NetworkLinkDown);
            if (reportedLinkQuality != -2) {
                reportLinkQuality(uint32_t(-2));
                reportedLinkQuality = -2;
            }
        }
    }
    return setEnabled(enabled);
}

IOReturn AirportItlwmAWDLInterface::setEnabled(bool enabled)
{
    if (!radio || stopping) return kIOReturnNotReady;
    if (!radio->getMainWorkLoop()->inGate())
        return radio->getMainCommandGate()->runAction(
            [](OSObject *, void *arg, void *on, void *, void *) -> IOReturn {
                return static_cast<AirportItlwmAWDLInterface *>(arg)->setEnabled(on != nullptr);
            }, this, enabled ? this : nullptr);
    requested = enabled;
    if (!enabled) {
        infrastructurePaused = false;
        immediateBroadcast.size = 0;
        AirportAWDL::clearRealtimeSessionsOn(radio->awdlRadio);
        broadcast.sent = false;
        updateAvailability();
        serviceUnicast();
        const int error = radio->stopAWDLRadio();
        drain();
        // Sync pauses preserve the configured peer manager and the BSD UP state.
        // Administrative shutdown is handled by setInterfaceEnable(false).
        running = false;
        if (error == EINPROGRESS) {
            if (timer) timer->setTimeoutMS(1);
            return kIOReturnSuccess;
        }
        return error ? kIOReturnIOError : kIOReturnSuccess;
    }
    // Activation is asynchronous, as the infrastructure radio may still be joining.
    nextRetryUS = 0;
    if (timer) timer->setTimeoutMS(1);
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::request(Request operation, void *data)
{
    if (!radio || stopping) return kIOReturnNotReady;
    if (!data) return kIOReturnBadArgument;
    struct Args { Request operation; void *data; } args = {operation, data};
    if (radio->getMainWorkLoop()->inGate()) return handle(operation, data);
    return radio->getMainCommandGate()->runAction(
        [](OSObject *, void *self, void *arg, void *, void *) -> IOReturn {
            auto args = static_cast<Args *>(arg);
            return static_cast<AirportItlwmAWDLInterface *>(self)->handle(args->operation, args->data);
        }, this, &args);
}

void AirportItlwmAWDLInterface::publishNativeSyncState()
{
    if (stopping || !radio) return;
    auto controller = getController();
    auto queue = controller ? controller->getWorkQueue() : nullptr;
    // The default timer action holds the workloop gate on a thread_call thread.
    if (!queue || !queue->inGate()) return;
    apple80211_awdl_sync_enabled state = {};
    if (getAWDL_SYNC_ENABLED(&state) != kIOReturnSuccess) return;
    const bool ready = state.enabled != 0;
    if (ready == nativeSyncReady) return;
    auto manager = AirportAWDLGetNativePeerManager(this);
    if (!manager || !manager->metaCast("IO80211AWDLPeerManager")) return;
    nativeSyncReady = ready;
    // Radio startup completes after the native enable request returns. Event 46
    // makes wifip2pd read GET117 again; LinkUp does not refresh that state.
    // The manager retains its suspended/link-state notification policy.
    AirportAWDLNativeSyncStateChanged(manager);
    ++nativeSyncStateCalls;
}

IOReturn AirportItlwmAWDLInterface::handle(Request operation, void *data)
{
    const bool active = radio->awdlRadio.ready && !regulatoryPlanBlocked && !regulatoryStopRequired;
    switch (operation) {
    case Request::getSSID: {
        auto out = static_cast<apple80211_ssid_data *>(data); *out = {};
        out->version = APPLE80211_VERSION;
        return kIOReturnSuccess;
    }
    case Request::setSSID:
        return static_cast<apple80211_ssid_data *>(data)->ssid_len ?
            kIOReturnUnsupported : kIOReturnSuccess;
    case Request::getBSSID: {
        auto out = static_cast<apple80211_bssid_data *>(data); *out = {};
        out->version = APPLE80211_VERSION; copyAddress(out->bssid.octet, BSSID);
        return kIOReturnSuccess;
    }
    case Request::setAWDL_BSSID:
        return sameAddress(static_cast<apple80211_awdl_bssid *>(data)->bssid, BSSID) ?
            kIOReturnSuccess : kIOReturnUnsupported;
    case Request::getSTATE: {
        auto out = static_cast<apple80211_state_data *>(data); *out = {};
        out->version = APPLE80211_VERSION; out->state = active ? IEEE80211_S_RUN : IEEE80211_S_INIT;
        return kIOReturnSuccess;
    }
    case Request::getCHANNEL: {
        if (!active) return kIOReturnNotReady;
        auto out = static_cast<apple80211_channel_data *>(data); *out = {};
        out->version = out->channel.version = APPLE80211_VERSION;
        out->channel.channel = radio->awdlRadio.channel;
        out->channel.flags = out->channel.channel <= 14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ;
        if (radio->awdlRadio.widthMHz == 80) out->channel.flags |= APPLE80211_C_FLAG_80MHZ;
        return kIOReturnSuccess;
    }
    case Request::setCHANNEL: {
        const auto &input = static_cast<apple80211_channel_data *>(data)->channel;
        const auto channel = input.channel;
        const uint8_t width = input.flags & APPLE80211_C_FLAG_80MHZ ? 80 : 20;
        if (!radio->awdlChannelAllowed(channel, width)) return kIOReturnUnsupported;
        for (auto &entry : sync.channels) entry = channel;
        for (auto &entry : sync.channelClasses) entry = width == 80 ? 128 : 0;
        sequenceConfigured = true;
        nativeSequenceValid = false;
        return kIOReturnSuccess;
    }
    case Request::getAWDL_SYNC_PARAMS: {
        auto out = static_cast<apple80211_awdl_sync_params *>(data); *out = {};
        out->version = APPLE80211_VERSION;
        out->availability_window_length = out->availability_window_period =
            out->extension_length = sync.periodTU;
        out->synchronization_frame_period = sync.actionPeriodTU;
        out->non_election_master_sync_frame_period = sync.nonElectionMasterPeriodTU;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_SYNC_PARAMS: {
        const auto in = static_cast<const apple80211_awdl_sync_params *>(data);
        if (!in->availability_window_length || !in->availability_window_period ||
            !in->extension_length || !in->synchronization_frame_period ||
            in->synchronization_frame_period > 0xffff ||
            in->non_election_master_sync_frame_period > 0xffff)
            return kIOReturnBadArgument;
        // The scheduler implements Sequoia's default contiguous 16-TU AWs.
        if (in->availability_window_length != 16 || in->availability_window_period != 16 ||
            in->extension_length != 16) return kIOReturnUnsupported;
        sync.actionPeriodTU = in->synchronization_frame_period;
        sync.nonElectionMasterPeriodTU = in->non_election_master_sync_frame_period;
        nextActionUS = nextMIFUS = 0;
        return kIOReturnSuccess;
    }
    case Request::getAWDL_SYNC_ENABLED: {
        auto out = static_cast<apple80211_awdl_sync_enabled *>(data); *out = {};
        out->version = APPLE80211_VERSION; out->enabled = active;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_SYNC_ENABLED:
        return setEnabled(static_cast<apple80211_awdl_sync_enabled *>(data)->enabled != 0);
    case Request::getAWDL_SYNC_STATE: {
        auto out = static_cast<apple80211_awdl_sync_state *>(data); *out = {};
        out->version = APPLE80211_VERSION;
        out->state = active ? 1 : 0;
        out->mode = sync.syncMode; out->flags = sync.syncFlags;
        copyAddress(out->sync_master_addr, sync.syncMode ? sync.fixedParent : sync.sync);
        return kIOReturnSuccess;
    }
    case Request::setAWDL_SYNC_STATE: {
        const auto in = static_cast<const apple80211_awdl_sync_state *>(data);
        if (in->state < 1 || in->state > 3 || !unicastAddress(in->sync_master_addr))
            return kIOReturnBadArgument;
        // The native host-election path programs the advertising-master role.
        if (in->state != 1 || in->mode > 2 || (in->flags & ~0x20)) return kIOReturnUnsupported;
        if (!in->mode && in->flags) return kIOReturnUnsupported;
        sync.configureSyncState(in->mode, in->flags, in->sync_master_addr);
        nextActionUS = nextMIFUS = 0;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_AWDL_ADVERTISERS: {
        const auto in = static_cast<const awdl_peer_advert_add *>(data);
        const uint64_t now = awdlNow();
        AdvertiserFailure failure;
        const auto result = sync.nativeParent.configure(
            in->bytes, radio->awdlPeers, now, &failure, sync.master, sync.address);
#if DEBUG
        setProperty("NativeAdvertiserLastFailure", uint64_t(failure), 8);
        setProperty("NativeAdvertiserLastRequestUS", now, 64);
        if (failure != AdvertiserFailure::None) {
            setProperty("NativeAdvertiserRejectedFailure", uint64_t(failure), 8);
            setProperty("NativeAdvertiserRejectedUS", now, 64);
        }
#endif
        if (result == AdvertiserResult::Invalid) return kIOReturnBadArgument;
        if (result == AdvertiserResult::Unsupported) return kIOReturnUnsupported;
        if (result == AdvertiserResult::Unavailable) return kIOReturnNotReady;
        nextActionUS = nextMIFUS = 0;
        return kIOReturnSuccess;
    }
    case Request::getAWDL_PRESENCE_MODE: {
        auto out = static_cast<apple80211_awdl_presence_mode *>(data);
        out->version = APPLE80211_VERSION; out->mode = sync.presence;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_PRESENCE_MODE:
        return static_cast<apple80211_awdl_presence_mode *>(data)->mode == 4 ?
            kIOReturnSuccess : kIOReturnUnsupported;
    case Request::getAWDL_AF_TX_MODE: {
        auto out = static_cast<apple80211_awdl_af_tx_mode *>(data); *out = {};
        out->version = APPLE80211_VERSION; out->mode = afTxMode;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_AF_TX_MODE: {
        const auto mode = static_cast<const apple80211_awdl_af_tx_mode *>(data)->mode;
        // Sequoia uses 0/3 to allow/suppress PSF while retaining in-window MIF and data.
        if (mode != 0 && mode != 3) return kIOReturnUnsupported;
        if (afTxMode != mode) {
            afTxMode = mode;
            if (!mode) nextActionUS = 0;
            if (timer) timer->setTimeoutMS(1);
        }
        return kIOReturnSuccess;
    }
    case Request::getAWDL_ELECTION_ID: {
        auto out = static_cast<apple80211_awdl_election_id *>(data); *out = {};
        out->version = APPLE80211_VERSION; out->election_id = sync.electionID;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_ELECTION_ID:
        sync.electionID = static_cast<apple80211_awdl_election_id *>(data)->election_id;
        nextActionUS = nextMIFUS = 0;
        return kIOReturnSuccess;
    case Request::getAWDL_ELECTION_METRIC: {
        auto out = static_cast<apple80211_awdl_election_metric *>(data); *out = {};
        out->version = APPLE80211_VERSION; out->metric = sync.metric;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_ELECTION_METRIC:
        sync.metric = static_cast<apple80211_awdl_election_metric *>(data)->metric;
        return kIOReturnSuccess;
    case Request::getAWDL_ELECTION_PARAMS: {
        auto out = static_cast<apple80211_awdl_election_params *>(data); *out = {};
        out->version = APPLE80211_VERSION; out->format = 1;
        out->flags = sync.electionFlags; out->election_id = uint16_t(sync.electionID);
        out->distance = sync.distance; copyAddress(out->master, sync.master);
        out->master_metric = sync.masterMetric; out->self_metric = sync.metric;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_ELECTION_PARAMS: {
        const auto in = static_cast<const apple80211_awdl_election_params *>(data);
        if (!unicastAddress(in->master)) return kIOReturnBadArgument;
        sync.electionFlags = in->flags;
        // This legacy request carries only the low half of the native election ID.
        sync.electionID = (sync.electionID & 0xffff0000U) | in->election_id;
        sync.metric = in->self_metric; sync.masterMetric = in->master_metric;
        sync.distance = in->distance; copyAddress(sync.master, in->master);
        nextActionUS = nextMIFUS = 0;
        return kIOReturnSuccess;
    }
    case Request::getAWDL_MASTER_CHANNEL: {
        auto out = static_cast<apple80211_awdl_master_channel *>(data);
        out->version = APPLE80211_VERSION; out->master_channel = sync.channel;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_MASTER_CHANNEL: {
        const auto channel = static_cast<apple80211_awdl_master_channel *>(data)->master_channel;
        // Native zero requests retain the previous channel.
        if (!channel) return kIOReturnSuccess;
        if (!radio->awdlChannelAllowed(channel)) return kIOReturnUnsupported;
        sync.setMasterChannel(channel);
        if (!sequenceConfigured)
            sync.discoverySchedule(radio->awdlChannelAllowed(6), radio->awdlChannelAllowed(44),
                radio->awdlChannelAllowed(149), radio->awdlChannelAllowed(149, 80),
                radio->awdlChannelAllowed(44, 80));
        nextActionUS = nextMIFUS = 0;
        return kIOReturnSuccess;
    }
    case Request::getAWDL_SECONDARY_MASTER_CHANNEL: {
        auto out = static_cast<apple80211_awdl_secondary_master_channel *>(data); *out = {};
        out->version = APPLE80211_VERSION;
        out->secondary_master_channel = secondaryMasterChannel;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_SECONDARY_MASTER_CHANNEL: {
        const auto channel = static_cast<const apple80211_awdl_secondary_master_channel *>(data)->secondary_master_channel;
        if (channel && !radio->awdlChannelAllowed(channel)) return kIOReturnUnsupported;
        secondaryMasterChannel = channel;
        return kIOReturnSuccess;
    }
    case Request::getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS: {
        auto out = static_cast<apple80211_awdl_extension_state_machine_parameter *>(data);
        *out = extensionParams;
        out->version = APPLE80211_VERSION;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS: {
        extensionParams = *static_cast<const apple80211_awdl_extension_state_machine_parameter *>(data);
        extensionParams.version = APPLE80211_VERSION;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_PEER_TRAFFIC_REGISTRATION: {
        const auto *raw = static_cast<const uint8_t *>(data);
        if (!raw) return kIOReturnBadArgument;
        const uint32_t nameLen = le32(raw + 0x0c);
        if (nameLen > 152) return kIOReturnBadArgument;
        const uint32_t activeFlag = le32(raw + 0xa8);
        AirportAWDL::updateRealtimeRegistrationOn(
            radio->awdlRadio, raw, sizeof(apple80211_awdl_peer_traffic_registration));
        if (activeFlag) {
            if (llwParamsEnabled && nativeSequenceValid) {
                apple80211_awdl_sync_channel_sequence seq = nativeSequence;
                handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq);
            }
            nextActionUS = nextMIFUS = 0;
            if (timer) timer->setTimeoutMS(1);
        }
        return kIOReturnSuccess;
    }
    case Request::setP2P_COEX_PROTECTION:
        return kIOReturnSuccess;
    case Request::getAWDL_ELECTION_RSSI_THRESHOLDS: {
        auto out = static_cast<apple80211_awdl_election_rssi_thresholds *>(data); *out = {};
        out->version = APPLE80211_VERSION; out->sync_edge = sync.electionRSSI;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_ELECTION_RSSI_THRESHOLDS: {
        const auto in = static_cast<const apple80211_awdl_election_rssi_thresholds *>(data);
        // Sequoia's host election configures only the sync-edge threshold.
        if (in->range_mid || in->range_close || in->sync_close) return kIOReturnUnsupported;
        if (in->sync_edge < -128 || in->sync_edge > 0) return kIOReturnBadArgument;
        sync.electionRSSI = in->sync_edge;
        return kIOReturnSuccess;
    }
    case Request::getPEER_CACHE_MAXIMUM_SIZE: {
        auto out = static_cast<apple80211_peer_cache_maximum_size *>(data);
        out->version = APPLE80211_VERSION; out->max_peers = PeerTable::Capacity;
        return kIOReturnSuccess;
    }
    case Request::setPEER_CACHE_CONTROL:
        return updatePeerCache(*static_cast<const apple80211_peer_cache_control *>(data));
    case Request::getAWDL_MIN_RATE: {
        auto out = static_cast<apple80211_awdl_min_rate *>(data); *out = {};
        out->version = APPLE80211_VERSION; out->min_rate = radio->awdlMinRateMbps;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_MIN_RATE: {
        const auto rate = static_cast<const apple80211_awdl_min_rate *>(data)->min_rate;
        // AWDL raw TX currently supports legacy OFDM, including a zero floor.
        if (rate > 54) return kIOReturnUnsupported;
        radio->awdlMinRateMbps = rate;
        return kIOReturnSuccess;
    }
    case Request::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE: {
        if (!active) return kIOReturnNotReady;
        auto out = static_cast<apple80211_awdl_sync_channel_sequence *>(data); *out = {};
        if (nativeSequenceValid) {
            *out = nativeSequence;
            return kIOReturnSuccess;
        }
        out->version = APPLE80211_VERSION;
        out->length = 16; out->encoding = 1;
        out->step_count = sync.presence - 1; out->fill_channel = 0xff;
        for (unsigned i = 0; i < out->length; ++i) {
            out->seqs[i].version = APPLE80211_VERSION;
            out->seqs[i].channel = sync.channels[i];
            out->seqs[i].flags = !sync.channels[i] ? 0 :
                (sync.channels[i] <= 14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ);
            if (sync.channelClasses[i] == 128) out->seqs[i].flags |= APPLE80211_C_FLAG_80MHZ;
        }
        return kIOReturnSuccess;
    }
    case Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE: {
        auto in = static_cast<apple80211_awdl_sync_channel_sequence *>(data);
        auto reject = [&](IOReturn error, const char *stage, unsigned index,
                          uint32_t channel, uint32_t flags, int allowed) {
            char detail[288];
            snprintf(detail, sizeof(detail),
                "stage=%s version=%u control=0x%x length=%u encoding=%u step=%u duplicate=%u "
                "fill=0x%x presence=%u index=%u channel=%u flags=0x%x hal_allowed=%d",
                stage, in->version, in->pad1, in->length, in->encoding, in->step_count,
                in->duplicate_count, in->fill_channel, sync.presence, index, channel, flags, allowed);
            setProperty("ChannelSequenceError", uint32_t(error), 32);
            setProperty("ChannelSequenceLastReject", detail);
            return error;
        };
        if (!in->length || in->length > 32 || in->encoding > 1)
            return reject(kIOReturnBadArgument, "format", ~0U, 0, 0, -1);
        if (in->length != 16 || in->step_count != sync.presence - 1 ||
            in->duplicate_count || in->fill_channel != 0xff)
            return reject(kIOReturnUnsupported, "layout", ~0U, 0, 0, -1);
        const auto ic = radio->get80211Controller();
        const unsigned infrastructure = ic && ic->ic_state == IEEE80211_S_RUN &&
            ic->ic_bss && ic->ic_bss->ni_chan ?
            ieee80211_chan2ieee(ic, ic->ic_bss->ni_chan) : 0;
        bool infra160 = infrastructure && AirportAWDL::bssNodeIs160MHz(ic->ic_bss);
#ifdef KERNEL
        static_assert(IEEE80211_CHAN_WIDTH_80P80 == 4 && IEEE80211_CHAN_WIDTH_160 == 5,
            "bssNodeIs160MHz width enum mismatch");
        static_assert(IEEE80211_CHAN_WIDTH_80 == 3 && IEEE80211_S_SCAN == 1 &&
            IEEE80211_F_BGSCAN == 0x08000000U,
            "SyncEngine kernel constant mismatch");
#endif
        for (unsigned i = 0; i < in->length; ++i) {
            const uint32_t c = in->seqs[i].channel;
            const uint32_t flags = in->seqs[i].flags;
            const uint32_t band = !c ? 0 : (c <= 14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ);
            const bool allowed = c && radio->awdlChannelAllowed(c);
            if (c && !allowed) return reject(kIOReturnUnsupported, "channel", i, c, flags, 0);
            uint32_t metadata = c ? APPLE80211_C_FLAG_ACTIVE | APPLE80211_C_FLAG_20MHZ : 0;
            const uint32_t wide = flags & (APPLE80211_C_FLAG_40MHZ |
                APPLE80211_C_FLAG_80MHZ | APPLE80211_C_FLAG_160MHZ);
            if (wide == APPLE80211_C_FLAG_80MHZ && !(flags & APPLE80211_C_FLAG_20MHZ) &&
                radio->awdlChannelAllowed(c, 80)) metadata = APPLE80211_C_FLAG_ACTIVE | wide;
            // AP-only widths remain metadata when no dedicated wide context supports them.
            if (c && c == infrastructure && wide && !(wide & (wide - 1)) &&
                !(flags & APPLE80211_C_FLAG_20MHZ)) {
                metadata = APPLE80211_C_FLAG_ACTIVE | wide;
                if (wide == APPLE80211_C_FLAG_40MHZ) metadata |= APPLE80211_C_FLAG_EXT_ABV;
                if (wide == APPLE80211_C_FLAG_160MHZ) infra160 = true;
            }
            if (in->encoding && (flags & ~metadata) != band)
                return reject(kIOReturnUnsupported, "flags", i, c, flags, allowed);
        }
        const uint8_t maxBandwidth = in->pad1 & 0x0f;
        const bool wideSequence = in->encoding &&
            !radio->awdlRadio.wideCircuitTripped() &&
            (maxBandwidth == 0 || maxBandwidth >= 3);
        uint8_t nextChannels[16] = {};
        uint8_t nextClasses[16] = {};
        bool nextDiverged = false;
        for (unsigned i = 0; i < in->length; ++i) {
            const uint32_t ch = in->seqs[i].channel;
            const uint32_t flags = in->seqs[i].flags;
            const bool explicit40 = (flags & APPLE80211_C_FLAG_40MHZ) && !(flags & APPLE80211_C_FLAG_80MHZ);
            const bool explicit80 = (flags & APPLE80211_C_FLAG_80MHZ) != 0;
            const bool flagWide80Plus = (flags & (APPLE80211_C_FLAG_80MHZ | APPLE80211_C_FLAG_160MHZ)) != 0 &&
                !(flags & (APPLE80211_C_FLAG_20MHZ | APPLE80211_C_FLAG_40MHZ));
            const bool multiSlotRun = in->seqs[(i + 15) % 16].channel == ch ||
                in->seqs[(i + 1) % 16].channel == ch;
            const bool socialOrExplicit80 = explicit80 ||
                ((ch == 149 || ch == 44) && multiSlotRun);
            const bool wants80 = wideSequence && !explicit40 && socialOrExplicit80 &&
                (flags & (APPLE80211_C_FLAG_5GHZ | APPLE80211_C_FLAG_80MHZ)) &&
                !AirportAWDL::wide80OverlapsInfra(ch, infrastructure, infra160);
            nextChannels[i] = uint8_t(ch);
            nextClasses[i] = wants80 && radio->awdlChannelAllowed(ch, 80) ? 128 : 0;
            const bool hostExpectedWide = ch > 14 && in->encoding &&
                (flagWide80Plus || explicit40 || ((ch == 149 || ch == 44) && multiSlotRun));
            if (hostExpectedWide && nextClasses[i] != 128)
                nextDiverged = true;
        }
        const uint64_t seqNow = awdlNow();
        bool strictZeroPunchout = false;
        if (sequenceConfigured && AirportAWDL::interfaceContinuityMediaActive(*this, seqNow) &&
            !AirportAWDL::controllerLiveScanActive(ic)) {
            bool subset = true, punchedOut = false, keptNonZero = false;
            for (unsigned i = 0; i < 16; ++i) {
                if (nextChannels[i] == 0) {
                    if (sync.channels[i] != 0) punchedOut = true;
                } else if (nextChannels[i] == sync.channels[i]) {
                    keptNonZero = true;
                } else {
                    subset = false;
                    break;
                }
            }
            strictZeroPunchout = subset && punchedOut && keptNonZero;
        }
        if (strictZeroPunchout) {
            for (unsigned i = 0; i < 16; ++i) {
                sync.punchoutFallbackChannels[i] = nextChannels[i];
                sync.punchoutFallbackClasses[i] = nextClasses[i];
            }
            sync.punchoutFallbackDiverged = nextDiverged;
            sync.sequenceDiverged = true;
            if (!sync.scanPunchoutPreservedUntilUS)
                sync.scanPunchoutPreservedUntilUS = seqNow + 1000000ULL;
        } else {
            for (unsigned i = 0; i < 16; ++i) {
                sync.channels[i] = nextChannels[i];
                sync.channelClasses[i] = nextClasses[i];
            }
            sync.sequenceDiverged = nextDiverged;
            sync.scanPunchoutPreservedUntilUS = 0;
        }
        uint8_t wideSlots = 0;
        for (unsigned i = 0; i < 16; ++i)
            wideSlots += sync.channelClasses[i] == 128;
        // The native monitor compares the acknowledged request, including AP metadata.
        nativeSequence = *in;
        nativeSequenceValid = true;
        sequenceConfigured = true;
        regulatoryPlanBlocked = false;
        nextRetryUS = 0;
        if (timer) timer->setTimeoutMS(1);
        setProperty("ChannelSequenceError", uint32_t(0), 32);
        setProperty("ChannelSequenceControl", uint64_t(in->pad1), 8);
        setProperty("ChannelSequenceWideSlots", uint64_t(wideSlots), 8);
        return kIOReturnSuccess;
    }
    case Request::getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES: {
        auto out = static_cast<apple80211_awdl_cap *>(data); *out = {};
        out->version = APPLE80211_VERSION; out->length = sizeof(*out);
        return kIOReturnSuccess;
    }
    case Request::setAWDL_SYNC_FRAME_TEMPLATE: {
        auto in = static_cast<apple80211_awdl_sync_frame_template *>(data);
        if (in->payload_len > sizeof(frameTemplate) || (!in->payload && in->payload_len))
            return kIOReturnBadArgument;
        const uint8_t *bytes = static_cast<const uint8_t *>(in->payload);
        size_t used = 0;
        if (!sync.configureTemplate(bytes, in->payload_len)) return kIOReturnBadArgument;
        for (size_t at = 0; at < in->payload_len;) {
            const size_t length = 3 + le16(bytes + at + 1);
            const unsigned type = bytes[at];
            if (type != 4 && type != 5 && type != 18 && type != 24 && type != 21) {
                memcpy(frameTemplate + used, bytes + at, length); used += length;
            }
            at += length;
        }
        templateSize = used;
        nextActionUS = nextMIFUS = 0;
        return kIOReturnSuccess;
    }
    case Request::setAWDL_OOB_REQUEST: {
        const auto &in = *static_cast<const apple80211_awdl_oob_request *>(data);
        return (in.destination[0] & 1) ? configureBroadcast(in, false) : queueUnicast(in);
    }
    case Request::setAWDL_OOB_AUTO_REQUEST:
        return configureBroadcast(*static_cast<const apple80211_awdl_oob_request *>(data), true);
    case Request::setCOMPANION_SKYWALK_IF_CREATE:
        return handleCompanionSkywalkCreate(static_cast<apple80211_companion_skywalk_if_create *>(data));
    case Request::getLLW_PARAMS:
        return handleGetLLWParams(static_cast<apple80211_llw_params *>(data));
    case Request::setLLW_PARAMS:
        return handleSetLLWParams(static_cast<const apple80211_llw_params *>(data));
    case Request::getAWDL_TXCAL_PERIOD:
    case Request::setAWDL_TXCAL_PERIOD:
        return data ? kIOReturnSuccess : kIOReturnBadArgument;
    default:
        break;
    }
    const uint64_t bit = uint64_t(1) << unsigned(operation);
    if (!(unsupportedRequests & bit)) {
        unsupportedRequests |= bit;
        IOLog("AirportItlwm AWDL: unsupported request %u\n", unsigned(operation));
        setProperty("UnsupportedRequests", unsupportedRequests, 64);
    }
    return kIOReturnUnsupported;
}

void AirportItlwmAWDLInterface::syncLLWLinkState()
{
    if (!llwActive) return;
    if (llwInterface && llwInterface->isSkywalkNative()) {
        if (!llwInterface->getBSDInterface()) return;
        if (!llwInterface->isEnabled())
            llwInterface->setInterfaceEnable(true);
        llwLinkUp = true;
        return;
    }
    if (!llwBSD || !llwBSD->getIfnet()) return;
    const bool up = running && requested && radio->awdlRadio.ready;
    if (up) {
        if (llwInterface && !llwInterface->isEnabled())
            llwInterface->setInterfaceEnable(true);
        if (!llwLinkUp) {
            llwBSD->setLinkState(kIO80211NetworkLinkUp);
            llwBSD->startOutputThread();
            llwLinkUp = true;
        }
    } else if (llwLinkUp) {
        llwBSD->setLinkState(kIO80211NetworkLinkDown);
        llwLinkUp = false;
    }
}

IOReturn AirportItlwmAWDLInterface::handleCompanionSkywalkCreate(apple80211_companion_skywalk_if_create *data)
{
    if (!data) return kIOReturnBadArgument;
    if (stopping) return kIOReturnNotReady;
    const bool nativeLLW = llwInterface && llwInterface->isSkywalkNative();
    if (data->action == 1) {
        if (!nativeLLW && !llwBSD) return kIOReturnNotReady;
        memset(data->bsd_name, 0, sizeof(data->bsd_name));
        ifnet_t ifp = nativeLLW ? llwInterface->getBSDInterface() : llwBSD->getIfnet();
        if (ifp) {
            snprintf(data->bsd_name, sizeof(data->bsd_name), "%s%u", ifnet_name(ifp), ifnet_unit(ifp));
        } else {
            const char *prefix = nativeLLW ? llwInterface->getBSDNamePrefix() : llwBSD->getNamePrefix();
            snprintf(data->bsd_name, sizeof(data->bsd_name), "%s0", prefix ? prefix : "llw");
        }
        llwActive = true;
        ++llwCreateRequests;
        if (llwInterface && ifp && !llwInterface->isEnabled())
            llwInterface->setInterfaceEnable(true);
        syncLLWLinkState();
        if (timer) timer->setTimeoutMS(1);
        return kIOReturnSuccess;
    }
    if (data->action == 0) {
        const bool wasLLWEnabled = llwParamsEnabled;
        llwActive = false;
        llwLinkUp = false;
        llwParamsEnabled = false;
        if (radio) AirportAWDL::setLLWRealtimePeerOn(radio->awdlRadio, nullptr, false);
        llwFlows.clear();
        ++llwDestroyRequests;
        if (llwInterface) llwInterface->setInterfaceEnable(false);
        if (llwBSD) {
            llwBSD->stopOutputThread();
            llwBSD->flushOutputQueue();
            if (llwBSD->getIfnet())
                llwBSD->setLinkState(kIO80211NetworkLinkDown);
        }
        if (wasLLWEnabled && nativeSequenceValid) {
            apple80211_awdl_sync_channel_sequence seq = nativeSequence;
            handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq);
        }
        return kIOReturnSuccess;
    }
    return kIOReturnBadArgument;
}

IOReturn AirportItlwmAWDLInterface::handleGetLLWParams(apple80211_llw_params *out)
{
    if (!out) return kIOReturnBadArgument;
    ++llwParamsGetCalls;
    *out = llwParams;
    if (!llwParamsConfigured) out->version = APPLE80211_VERSION;
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::handleSetLLWParams(const apple80211_llw_params *in)
{
    if (!in) return kIOReturnBadArgument;
    ++llwParamsSetCalls;
    const bool wasEnabled = llwParamsEnabled;
    llwParams = *in;
    llwParamsConfigured = true;
    llwParamsEnabled = in->enable != 0;
    if (radio)
        AirportAWDL::setLLWRealtimePeerOn(radio->awdlRadio, llwParams.peer_mac, llwParamsEnabled);
    if (wasEnabled != llwParamsEnabled && nativeSequenceValid) {
        apple80211_awdl_sync_channel_sequence seq = nativeSequence;
        handle(Request::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE, &seq);
    }
    if (llwParamsEnabled) {
#ifdef KERNEL
        if (auto *owner = OSDynamicCast(AirportItlwm, getController())) {
            if (auto *skywalkInterface = OSDynamicCast(AirportItlwmSkywalkInterface, owner->fNetIf))
                skywalkInterface->abortScanForContinuityMedia();
        }
#endif
        if (!stopping && timer)
            timer->setTimeoutMS(1);
    }
    return kIOReturnSuccess;
}

bool AirportItlwmAWDLInterface::continuityMediaActive() const
{
    if (!requested || !running || stopping || !radio || !radio->awdlRadio.ready ||
        radio->awdlRadio.stopPending || radio->awdlRadio.fault)
        return false;
    const auto &state = radio->awdlRadio;
    if (AirportAWDL::realtimeSessionMaskOf(state) != 0)
        return true;
    if (llwActive && llwParamsEnabled)
        return true;
    const uint64_t now = awdlNow();
    if (state.heavyReceiveActive(now) || state.heavyTransmitActive(now))
        return true;
    const auto &tx = state.blockAckTX;
    if (tx.configured && tx.phase == AirportAWDL::TXBlockAckQueue::Agreed &&
        tx.lastDataUS && now >= tx.lastDataUS && now - tx.lastDataUS < 2000000ULL)
        return true;
    if (state.lastRXPayloadUS && now >= state.lastRXPayloadUS &&
        now - state.lastRXPayloadUS < 1500000ULL && radio->awdlHasRXBA())
        return true;
    return false;
}

void AirportItlwmAWDLInterface::notifyInfrastructureReady()
{
    if (!requested || stopping || !radio || radio->awdlRadio.ready)
        return;
    nextRetryUS = 0;
    if (!ticking && !wakeArmed && timer) {
        wakeArmed = true;
        timer->setTimeoutMS(1);
    }
}

IOReturn AirportItlwmAWDLInterface::updatePeerCache(const apple80211_peer_cache_control &in)
{
    if (stopping) return kIOReturnNotReady;
    if (in.version != APPLE80211_VERSION || !unicastAddress(in.address) ||
        sameAddress(in.address, address.octet)) return kIOReturnBadArgument;
    const bool remove = in.operation == 2;
    if (!remove && in.operation != 0 && in.operation != 6) return kIOReturnUnsupported;
    auto cached = radio->awdlPeers.findCached(in.address);
    auto peer = remove ? cached : radio->awdlPeers.find(in.address, awdlNow());
    if (!remove && (!requested || !radio->awdlRadio.ready || !peer))
        return kIOReturnNotReady;
    auto controller = getController();
    if (!controller) return kIOReturnNotReady;
    // Native cache bookkeeping can restart independently of the radio's records.
    const bool wasCached = peer && peer->nativeCached;
    if (peer) peer->nativeCached = !remove;
    apple80211_peer_cache_control completion = {};
    completion.version = APPLE80211_VERSION;
    completion.operation = remove ? 3 : 1;
    copyAddress(completion.address, in.address);
    completion.reserved = in.reserved;
    // OFDM TX uses software peer state; timing continues to come from received AFs.
    // The controller copies and queues this indication on our shared workloop.
    const IOReturn result = controller->postMessage(this, APPLE80211_M_PEER_CACHE_CONTROL,
                                                   &completion, sizeof(completion), true);
    if (result != kIOReturnSuccess && peer) peer->nativeCached = wasCached;
    else if (result == kIOReturnSuccess && !stopping && timer) timer->setTimeoutMS(1);
    return result;
}

void AirportItlwmAWDLInterface::recordNativeDataActivity(bool transmit, uint32_t ethernetBytes)
{
    if (stopping || !requested || !running) return;
    auto controller = getController();
    auto queue = controller ? controller->getWorkQueue() : nullptr;
    if (!queue || queue != radio->getMainWorkLoop() || !queue->inGate()) return;
    auto manager = AirportAWDLGetNativePeerManager(this);
    if (!manager || !manager->metaCast("IO80211AWDLPeerManager")) return;
    AirportAWDLNativeDataCountersDelta delta = {};
    if (transmit) { delta.txPackets = 1; delta.txBytes = ethernetBytes; }
    else { delta.rxPackets = 1; delta.rxBytes = ethernetBytes; }
    AirportAWDLUpdateNativeDataCounters(manager, &delta);
    AbsoluteTime now;
    clock_get_uptime(&now);
    if (transmit) AirportAWDLSetNativeTxActivity(manager, now);
    else AirportAWDLSetNativeRxActivity(manager, now);
}

void AirportItlwmAWDLInterface::notifyNativeDataPeer(const DataView &frame)
{
    if (stopping || !requested || !running ||
        !sameAddress(frame.destination, address.octet) ||
        !unicastAddress(frame.source) || sameAddress(frame.source, address.octet)) return;
    auto controller = getController();
    auto queue = controller ? controller->getWorkQueue() : nullptr;
    if (!queue || !queue->inGate() || !queue->onThread()) return;
    auto manager = AirportAWDLGetNativePeerManager(this);
    if (!manager || !manager->metaCast("IO80211AWDLPeerManager")) return;
    // The native receive path holds this lock while using a borrowed peer.
    // On the workloop thread, contention fails without sleeping the gate.
    if (!AirportAWDLLockNativePeers(manager)) { ++nativeDataPeerLockMisses; return; }
    uint8_t source[6];
    copyAddress(source, frame.source);
    auto peer = AirportAWDLFindNativePeer(manager, source);
    if (peer && peer->metaCast("IO80211AWDLPeer")) {
        // Preserve the native RSSI and peer-policy checks before it marks reachability.
        const int previous = AirportAWDLNativePeerReachability(peer);
        AirportAWDLNativePeerReceivedData(peer);
        const int current = AirportAWDLNativePeerReachability(peer);
        // Native AF/monitor callers account for the transition outside the peer method.
        if (current != previous) {
            AirportAWDLDecrementNativeReachability(manager, previous);
            AirportAWDLIncrementNativeReachability(manager, current);
        }
        ++nativeDataPeerNotifications;
    }
    AirportAWDLUnlockNativePeers(manager);
}

bool AirportItlwmAWDLInterface::deliverLLWOrMirror(const DataView &frame, mbuf_t packet, size_t length)
{
    const bool nativeLLW = llwInterface && llwInterface->isSkywalkNative();
    if (!llwActive) return false;
    if (nativeLLW ? !llwInterface->getBSDInterface() : (!llwBSD || !llwBSD->getIfnet())) return false;
    const LLWRoute route = frame.lowLatency ? LLWRoute::LLW : llwFlows.classifyReceive(
        frame.source, frame.payload, frame.size, frame.tid,
        true, llwParamsEnabled, llwParams.peer_mac, awdlNow());
    if (route == LLWRoute::AWDL) return false;
    if (route == LLWRoute::LLW) {
        if (nativeLLW) {
            if (llwInterface->inputSkywalkEthernet(static_cast<const uint8_t *>(mbuf_data(packet)), length))
                ++llwRxDelivered;
            else
                ++llwRxDropped;
            mbuf_freem(packet);
        } else {
            if (llwBSD->inputPacket(packet, unsigned(length))) ++llwRxDelivered;
            else ++llwRxDropped;
        }
        return true;
    }
    // Mirror ICMPv6 Neighbor Discovery so llw0 maintains peer MAC resolution alongside awdl0.
    if (nativeLLW) {
        if (llwInterface->inputSkywalkEthernet(static_cast<const uint8_t *>(mbuf_data(packet)), length))
            ++llwNDPMirrored;
        return false;
    }
    mbuf_t mirror = awdlPacket(frame.size + 14);
    if (mirror) {
        const size_t mirrorLen = ethernetFromData(frame, static_cast<uint8_t *>(mbuf_data(mirror)), frame.size + 14);
        if (mirrorLen && llwBSD->inputPacket(mirror, unsigned(mirrorLen)))
            ++llwNDPMirrored;
        else if (!mirrorLen)
            mbuf_freem(mirror);
    }
    return false;
}

void AirportItlwmAWDLInterface::receive(void *context, const DataView &frame)
{
    auto self = static_cast<AirportItlwmAWDLInterface *>(context);
    if (self->stopping || !self->bsd || !self->running || !self->bsd->getIfnet()) {
        ++self->rxDropped; return;
    }
    mbuf_t packet = awdlPacket(frame.size + 14);
    if (!packet) { ++self->rxDropped; return; }
    const size_t length = ethernetFromData(frame, static_cast<uint8_t *>(mbuf_data(packet)), frame.size + 14);
    if (!length) { mbuf_freem(packet); ++self->rxDropped; return; }
    if (sameAddress(frame.destination, self->address.octet) &&
        unicastAddress(frame.source) && !sameAddress(frame.source, self->address.octet))
        self->recordNativeDataActivity(false, unsigned(length));
    self->notifyNativeDataPeer(frame);
    if (self->deliverLLWOrMirror(frame, packet, length)) return;
    if (self->bsd->inputPacket(packet, unsigned(length))) ++self->rxDelivered;
    else ++self->rxDropped;
}

void AirportItlwmAWDLInterface::receiveAction(void *context, const uint8_t *frame,
                                             size_t size, uint16_t channel, int16_t rssi,
                                             uint64_t receivedUS)
{
    auto self = static_cast<AirportItlwmAWDLInterface *>(context);
    if (self->stopping || !self->bsd || !self->bsd->getIfnet()) return;
    NativeActionMetadata metadata;
    const uint32_t band = channel <= 14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ;
    if (!metadata.fill(frame, size, channel, rssi, band, receivedUS)) { ++self->actionDropped; return; }
    auto pool = self->getMgmtFramePool();
    auto buffer = pool ? pool->getBuffer() : nullptr;
    if (!buffer) { ++self->actionDropped; return; }
    void *bytes = buffer->getBytesNoCopy();
    const size_t header = (frame[1] & 0x80) ? 28 : 24;
    const size_t length = size - header;
    if (!bytes || length > buffer->getCapacity()) {
        buffer->returnBuffer(); ++self->actionDropped; return;
    }
    // The native buffer contains the vendor action body, without the 802.11 header.
    const size_t merged = self->serviceMergeEnabled ?
        self->serviceMerge.process(frame + 10, frame + header, length,
            static_cast<uint8_t *>(bytes), buffer->getCapacity(), receivedUS) : 0;
    if (merged) {
        buffer->setLength(merged);
    } else {
        memcpy(bytes, frame + header, length);
        buffer->setLength(length);
    }
    ++self->actionDelivered;
    // bpfTapInput transfers the buffer to the peer manager, or returns it to its pool.
    self->retain();
    self->bpfTapInput(buffer, 12, &metadata, sizeof(metadata));
#if DEBUG
    const uint64_t snapNow = awdlNow();
    if (!(self->llwActive && self->llwParamsEnabled) &&
        !self->radio->awdlRadio.heavyReceiveActive(snapNow) &&
        !self->radio->awdlRadio.heavyTransmitActive(snapNow))
        self->publishNativePeers(snapNow);
#endif
    self->release();
}

uint64_t AirportItlwmAWDLInterface::nextManagementToken()
{
    // Never reuse a token that an old firmware completion could still carry.
    if (unicastToken == ~uint64_t(0)) return 0;
    return ++unicastToken;
}

void AirportItlwmAWDLInterface::receiveBlockAck(void *context, const BlockAckRequest &request)
{
    auto self = static_cast<AirportItlwmAWDLInterface *>(context);
    if (!self->radio->getMainWorkLoop()->inGate()) return;
    ++self->blockAckReceived;
    if (!self->requested || !self->running || self->stopping || self->regulatoryPlanBlocked ||
        self->regulatoryStopRequired || !self->radio->awdlRadio.ready) {
        ++self->blockAckRejected;
        return;
    }
    const uint64_t now = awdlNow();
    BlockAckReply *slot = nullptr;
    for (auto &entry : self->blockAckReplies) {
        if (entry.state != BlockAckReply::Free &&
            (entry.epoch != self->radio->dmaEpoch || now < entry.queuedUS ||
             now - entry.queuedUS >= BlockAckReply::LifetimeUS)) {
            ++self->blockAckExpired;
            entry = {};
        }
        if (entry.state != BlockAckReply::Free && entry.request.tid == request.tid &&
            sameAddress(entry.request.peer, request.peer)) {
            if (sameBlockAck(entry.request, request)) {
                ++self->blockAckCoalesced;
                return;
            }
            if (entry.state == BlockAckReply::Queued) { slot = &entry; break; }
        }
        if (!slot && entry.state == BlockAckReply::Free) slot = &entry;
    }
    const uint64_t token = slot ? self->nextManagementToken() : 0;
    if (!token) { ++self->blockAckRejected; return; }
    *slot = {};
    slot->state = BlockAckReply::Queued;
    slot->request = request;
    slot->queuedUS = now;
    slot->epoch = self->radio->dmaEpoch;
    slot->token = token;
    ++self->blockAckQueued;
    if (!self->wakeArmed && self->timer) {
        if (!self->ticking) self->wakeArmed = true;
        self->timer->setTimeoutMS(1);
    }
}

void AirportItlwmAWDLInterface::serviceBlockAck()
{
    radio->serviceAWDLBlockAck();
    serviceTXBlockAck();
    for (auto &entry : blockAckReplies) {
        if (entry.state == BlockAckReply::Free) continue;
        uint64_t now = awdlNow();
        const uint64_t token = entry.token;
        auto live = [&]() {
            return entry.token == token && requested && running && !stopping &&
                !regulatoryPlanBlocked && !regulatoryStopRequired && radio->awdlRadio.ready &&
                !radio->awdlRadio.stopPending && !radio->awdlRadio.fault &&
                entry.epoch == radio->dmaEpoch && now >= entry.queuedUS &&
                now - entry.queuedUS < BlockAckReply::LifetimeUS;
        };
        if (!live()) { ++blockAckExpired; entry = {}; continue; }
        if (entry.state == BlockAckReply::Submitted) continue;
        // Keep successive negotiations for one TID in transmission order.
        bool previous = false;
        for (const auto &other : blockAckReplies)
            if (other.state == BlockAckReply::Submitted && other.request.tid == entry.request.tid &&
                sameAddress(other.request.peer, entry.request.peer)) previous = true;
        if (previous) continue;
        if (!entry.prepared) {
            if (!radio->awdlPeers.find(entry.request.peer, now)) continue;
            const BlockAckRequest request = entry.request;
            uint64_t cookie = 0;
            const int result = radio->prepareAWDLBlockAck(request, cookie);
            now = awdlNow();
            if (!live()) continue;
            if (result == EBUSY) continue;
            entry.prepared = true;
            entry.accepted = result == 0;
            entry.cookie = cookie;
        }
        if ((entry.accepted && !radio->awdlBlockAckLive(entry.request, entry.cookie)) ||
            (entry.request.teardown && !entry.accepted)) {
            ++blockAckExpired;
            entry = {};
            continue;
        }
        uint64_t deadline = 0;
        auto eligible = [&]() {
            const auto &state = radio->awdlRadio;
            const bool prioritizeReply = (llwActive && llwParamsEnabled) ||
                state.heavyReceiveActive(now);
            Window local = {}, remote = {};
            auto peer = radio->awdlPeers.find(entry.request.peer, now);
            if (!live() || entry.state != BlockAckReply::Queued || !peer ||
                (entry.accepted && !radio->awdlBlockAckLive(entry.request, entry.cookie)) ||
                !sync.canSendTo(*peer, now, false, state.listener) || !sync.window(now, local) ||
                !peer->window(now, remote) || !radio->awdlChannelAllowed(local.channel) ||
                state.channel != local.channel || state.changing || !state.dwell.running ||
                state.dwell.cancelling || now >= state.dwell.windowEndUS ||
                state.queue == IWX_INVALID_QUEUE || radio->com.txq[state.queue].queued >= 48 ||
                (!prioritizeReply && (psfDue(now) || broadcastDue(now)))) return false;
            deadline = entry.queuedUS + BlockAckReply::LifetimeUS;
            const uint64_t localRemain = state.listener ? local.untilChannelChangeUS : local.untilNextEAWUS;
            const uint64_t peerRemain = state.listener ? remote.untilChannelChangeUS : remote.untilNextEAWUS;
            const uint64_t localEnd = now + (localRemain > 3000 ? localRemain - 3000 : 0);
            const uint64_t peerEnd = now + (peerRemain > 3000 ? peerRemain - 3000 : 0);
            if (localEnd < deadline) deadline = localEnd;
            if (peerEnd < deadline) deadline = peerEnd;
            if (state.dwell.windowEndUS < deadline) deadline = state.dwell.windowEndUS;
            return deadline > now;
        };
        if (!eligible()) continue;
        const size_t length = entry.request.teardown ? 30 : 33;
        mbuf_t packet = awdlPacket(length);
        if (!packet) continue;
        now = awdlNow();
        if (!eligible()) { mbuf_freem(packet); continue; }
        if (entry.request.teardown) {
            uint8_t frame[30];
            blockAckTeardown(frame, address.octet, entry.request, sync.wifiSequence++);
            memcpy(mbuf_data(packet), frame, sizeof(frame));
        } else {
            uint8_t frame[33];
            blockAckResponse(frame, address.octet, entry.request, sync.wifiSequence++, entry.accepted);
            memcpy(mbuf_data(packet), frame, sizeof(frame));
        }
        entry.state = BlockAckReply::Submitted;
        ++entry.attempts;
        const int error = radio->transmitAWDL(packet, token, deadline);
        if (!error) ++blockAckSubmitted;
        else if (entry.token == token && entry.state == BlockAckReply::Submitted) {
            // No descriptor was admitted. Firmware handles retries after admission.
            if (error == ENETDOWN || error == ENOBUFS || error == ENOMEM)
                entry.state = BlockAckReply::Queued;
            else { ++blockAckFailed; entry = {}; }
        }
    }
}

void AirportItlwmAWDLInterface::serviceTXBlockAck()
{
    using AirportAWDL::TXBlockAckQueue;
    const bool wasAgreed = radio->awdlRadio.blockAckTX.phase == TXBlockAckQueue::Agreed;
    radio->serviceAWDLTXBlockAck();
    auto &tx = radio->awdlRadio.blockAckTX;
    if (!wasAgreed && tx.phase == TXBlockAckQueue::Agreed) {
#ifdef KERNEL
        if (auto *owner = OSDynamicCast(AirportItlwm, getController())) {
            if (auto *skywalkInterface = OSDynamicCast(AirportItlwmSkywalkInterface, owner->fNetIf))
                skywalkInterface->abortScanForContinuityMedia();
        }
#endif
    }
    struct WindowCheck {
        static uint64_t end(const void *context, uint64_t now) {
            const auto self = static_cast<const AirportItlwmAWDLInterface *>(context);
            const auto &state = self->radio->awdlRadio;
            const auto *peer = self->radio->awdlPeers.find(state.blockAckTX.peer, now);
            Window local = {}, remote = {};
            if (!self->requested || !self->running || self->stopping || self->regulatoryPlanBlocked ||
                self->regulatoryStopRequired || !state.ready || state.stopPending || state.fault ||
                !state.dwell.running || state.dwell.cancelling || !peer ||
                !self->sync.window(now, local) || !peer->window(now, remote) ||
                !self->sync.canSendTo(*peer, now, false, state.listener) || state.channel != local.channel ||
                !self->radio->awdlChannelAllowed(local.channel, local.widthMHz()) ||
                state.widthMHz != local.widthMHz()) return 0;
            auto hasPeerTraffic = [&]() -> bool {
                constexpr unsigned cap = sizeof(self->pending) / sizeof(self->pending[0]);
                for (unsigned i = 0; i < self->pendingCount; ++i) {
                    const auto &item = self->pending[(self->pendingHead + i) % cap];
                    if (item.lowLatency || item.multicastQueue || !item.packet ||
                        item.epoch != self->radio->dmaEpoch) continue;
                    uint8_t dst[6];
                    if (mbuf_pkthdr_len(item.packet) >= 14 &&
                        !mbuf_copydata(item.packet, 0, sizeof(dst), dst) &&
                        sameAddress(dst, state.blockAckTX.peer))
                        return true;
                }
                for (const auto &flight : self->inFlight) {
                    if (!flight.token || flight.entry.lowLatency || flight.entry.group ||
                        !flight.entry.packet || flight.entry.epoch != self->radio->dmaEpoch) continue;
                    uint8_t dst[6];
                    if (mbuf_pkthdr_len(flight.entry.packet) >= 14 &&
                        !mbuf_copydata(flight.entry.packet, 0, sizeof(dst), dst) &&
                        sameAddress(dst, state.blockAckTX.peer))
                        return true;
                }
                return false;
            };
            const bool txIdle = state.blockAckTX.phase == TXBlockAckQueue::Agreed &&
                !(state.blockAckTX.configured && self->radio->com.txq[state.blockAckTX.queue].queued) &&
                state.blockAckTX.lastDataUS && now >= state.blockAckTX.lastDataUS &&
                now - state.blockAckTX.lastDataUS > 1500000ULL &&
                !hasPeerTraffic();
            if (txIdle) return 0;
            const uint64_t localRemain = state.listener ? local.untilChannelChangeUS : local.untilNextEAWUS;
            const uint64_t remoteRemain = state.listener ? remote.untilChannelChangeUS : remote.untilNextEAWUS;
            if (localRemain <= 6000 || remoteRemain <= 6000 || now >= state.dwell.windowEndUS) return 0;
            // Older TLC APIs cannot limit TXOP. Pause ahead of the 5.484 ms HT PPDU limit.
            return MIN(state.dwell.windowEndUS, now + MIN(localRemain, remoteRemain) - 6000);
        }
        static uint64_t probeEnd(const void *context, uint64_t now) {
            const auto self = static_cast<const AirportItlwmAWDLInterface *>(context);
            const auto &state = self->radio->awdlRadio;
            const uint64_t deadline = end(context, now);
            if (!deadline) return 0;
            if (state.blockAckTX.receiveProbeUntilUS > now) return deadline;
            if (self->psfDue(now) || self->broadcastDue(now) ||
                (state.dataQueueConfigured && self->radio->com.txq[state.dataQueue].queued))
                return 0;
            return deadline > now + 19000 ? deadline : 0;
        }
    };
    if (radio->serviceAWDLReceiveFilter(WindowCheck::probeEnd, this) ||
        radio->setAWDLTXWindow(WindowCheck::end, this) || !tx.configured) return;
    uint64_t now = awdlNow();
    const uint64_t cookie = tx.cookie, epoch = radio->dmaEpoch;
    const bool ending = tx.phase == TXBlockAckQueue::Retiring;
    if ((!ending && tx.phase != TXBlockAckQueue::Asking) || (ending && !tx.needDelba) ||
        tx.managementToken || now < tx.nextAskUS || psfDue(now) || broadcastDue(now) ||
        (!ending && radio->awdlRadio.deferReceiveProbe(now))) return;
    if (tx.attempts >= (ending ? 3 : TXBlockAckQueue::MaxAttempts)) {
        if (!ending) tx.retryDialog(now);
        return;
    }
    // Old QoS single-frame traffic must drain before starting a new TID sequence.
    if (!ending && radio->awdlRadio.dataQueueConfigured &&
        radio->com.txq[radio->awdlRadio.dataQueue].queued) return;
    const size_t length = ending ? 30 : 33;
    uint64_t deadline = WindowCheck::end(this, now);
    if (!deadline || (!ending && deadline <= now + 19000) || !radio->awdlTxWindowOpen()) return;
    mbuf_t packet = awdlPacket(length);
    if (!packet) return;
    now = awdlNow(); deadline = WindowCheck::end(this, now);
    if (!deadline || (!ending && deadline <= now + 19000) || !radio->awdlTxWindowOpen() ||
        tx.cookie != cookie || radio->dmaEpoch != epoch ||
        (ending ? tx.phase != TXBlockAckQueue::Retiring || !tx.needDelba : tx.phase != TXBlockAckQueue::Asking)) {
        mbuf_freem(packet); return;
    }
    const uint64_t token = nextManagementToken();
    if (!token) { mbuf_freem(packet); return; }
    const uint8_t previousDialog = tx.dialog;
    const bool hadAckedAsk = !ending && tx.lastAskAcked;
    if (hadAckedAsk) {
        if (!++tx.dialog) ++tx.dialog;
        tx.lastAskAcked = false;
    }
    BlockAckRequest request = {};
    copyAddress(request.peer, tx.peer);
    request.dialog = tx.dialog; request.tid = tx.tid;
    request.ssn = tx.sequence; request.window = 64; request.immediate = true;
    if (ending) {
        uint8_t frame[30]; blockAckTXTeardown(frame, address.octet, request, sync.wifiSequence++);
        memcpy(mbuf_data(packet), frame, sizeof(frame));
    } else {
        uint8_t frame[33]; blockAckSolicit(frame, address.octet, request, sync.wifiSequence++);
        memcpy(mbuf_data(packet), frame, sizeof(frame));
    }
    tx.managementToken = token; ++tx.attempts;
    tx.nextAskUS = now + (ending || tx.attempts <= 2 ? 200000 : 500000);
    const int error = radio->transmitAWDL(packet, token, deadline);
    if (!error && !ending) {
        ++radio->awdlRadio.blockAckTXStats.requests;
        if (tx.cookie == cookie && radio->dmaEpoch == epoch) {
            tx.lastAskUS = now;
            if (tx.receiveProbeUntilUS > now)
                tx.receiveProbeUntilUS = deadline - now < TXBlockAckQueue::ReceiveProbeDurationUS ?
                    deadline : now + TXBlockAckQueue::ReceiveProbeDurationUS;
        }
    }
    if (error && tx.cookie == cookie && radio->dmaEpoch == epoch && tx.managementToken == token) {
        tx.managementToken = 0; --tx.attempts; tx.nextAskUS = now + 20000;
        if (hadAckedAsk) { tx.dialog = previousDialog; tx.lastAskAcked = true; }
    }
}

void AirportItlwmAWDLInterface::receiveBlockAckEnd(void *context, const BlockAckRequest &request)
{
    auto self = static_cast<AirportItlwmAWDLInterface *>(context);
    if (!self->radio->getMainWorkLoop()->inGate()) return;
    for (auto &entry : self->blockAckReplies)
        if (entry.state != BlockAckReply::Free && entry.request.tid == request.tid &&
            sameAddress(entry.request.peer, request.peer)) entry = {};
    if (!self->wakeArmed && self->timer) {
        if (!self->ticking) self->wakeArmed = true;
        self->timer->setTimeoutMS(1);
    }
}

IOReturn AirportItlwmAWDLInterface::pullOutputPackets()
{
    if (!radio->getMainWorkLoop()->inGate()) {
        return radio->getMainCommandGate()->runAction(
            [](OSObject *, void *self, void *, void *, void *) -> IOReturn {
                return static_cast<AirportItlwmAWDLInterface *>(self)->pullOutputPackets();
            }, this);
    }
    constexpr unsigned arrayCapacity = sizeof(pending) / sizeof(pending[0]);
    const bool baAgreed = radio->awdlRadio.blockAckTX.configured &&
        radio->awdlRadio.blockAckTX.phase == TXBlockAckQueue::Agreed;
    const unsigned queueCapacity = (!baAgreed && arrayCapacity > 96U) ? 96U : arrayCapacity;
    const unsigned maxBatch = queueCapacity / 2 < 32 ? queueCapacity / 2 : 32;
    // Reserve 16 software-queue slots for native Skywalk llw0 frames only when an LLW
    // session is enabled or queued, so idle-attached llw0 never shrinks Sidecar/AirDrop staging.
    bool hasPendingLLW = false;
    for (unsigned i = 0; i < pendingCount; ++i) {
        if (pending[(pendingHead + i) % arrayCapacity].lowLatency) {
            hasPendingLLW = true;
            break;
        }
    }
    const bool reserveLLW = llwActive && (llwParamsEnabled || hasPendingLLW);
    const unsigned awdlLimit = reserveLLW && queueCapacity > 16 ? queueCapacity - 16 : queueCapacity;
    const unsigned used = pendingCount + inFlightCount + aggregateExtraOwned;
    if (!bsd || !bsd->getIfnet() || !requested || !running || !radio->awdlRadio.ready ||
        regulatoryPlanBlocked || regulatoryStopRequired ||
        stopping || used >= awdlLimit)
        return kIOReturnNoResources;
    // Reserve software queue space under the same gate as tick's dequeues.
    const unsigned capacity = awdlLimit - used;
    mbuf_t packets = nullptr;
    IOReturn result = bsd->dequeueOutputPackets(capacity < maxBatch ? capacity : maxBatch, &packets);
    if (result != kIOReturnSuccess)
        return result == kIOReturnNoFrames ? kIOReturnSuccess : result;
    auto isUrgentEntry = [](const Pending &p) {
        if (p.retry || p.lowLatency) return true;
        if (p.multicastQueue || !p.packet) return false;
        const size_t len = mbuf_pkthdr_len(p.packet);
        uint8_t hdr[96];
        const size_t copy = len < sizeof(hdr) ? len : sizeof(hdr);
        return copy >= 14 && len <= 1514 &&
            !mbuf_copydata(p.packet, 0, copy, hdr) &&
            isUnicastPTPHeader(hdr, copy, len);
    };
    auto insertUrgent = [&](const Pending &item) {
        unsigned pos = 0;
        while (pos < pendingCount && isUrgentEntry(pending[(pendingHead + pos) % arrayCapacity]))
            ++pos;
        if (pos == pendingCount) {
            pending[(pendingHead + pendingCount) % arrayCapacity] = item;
        } else if (pos == 0) {
            pendingHead = (pendingHead + arrayCapacity - 1) % arrayCapacity;
            pending[pendingHead] = item;
        } else {
            for (unsigned k = pendingCount; k > pos; --k)
                pending[(pendingHead + k) % arrayCapacity] =
                    pending[(pendingHead + k - 1) % arrayCapacity];
            pending[(pendingHead + pos) % arrayCapacity] = item;
        }
        ++pendingCount;
    };
    unsigned remaining = 0, enqueued = 0;
    for (auto packet = packets; packet; packet = mbuf_nextpkt(packet)) ++remaining;
    while (packets) {
        mbuf_t packet = packets;
        packets = mbuf_nextpkt(packet); mbuf_setnextpkt(packet, nullptr);
        --remaining;
        if (mbuf_pkthdr_len(packet) > 1514) { mbuf_freem(packet); ++txDropped; continue; }
        Pending entry = {packet, awdlNow(), radio->dmaEpoch};
        mbuf_t duplicate = nullptr;
        bool urgent = false;
        uint8_t header[96];
        const size_t pktLen = mbuf_pkthdr_len(packet);
        const size_t copyLen = pktLen < sizeof(header) ? pktLen : sizeof(header);
        if (copyLen >= 14 && !mbuf_copydata(packet, 0, copyLen, header)) {
            if ((header[0] & 1) && be16(header + 12) == 0x86dd) {
                entry.multicastQueue = Pending::MulticastSocial;
                if (pendingCount + inFlightCount + aggregateExtraOwned + remaining + 2 <= queueCapacity &&
                    !mbuf_dup(packet, MBUF_DONTWAIT, &duplicate)) ++multicastCopiesQueued;
                else ++multicastCopiesDropped;
            } else if (unicastAddress(header)) {
                urgent = isUnicastPTPHeader(header, copyLen, pktLen);
                if (be16(header + 12) == 0x86dd) {
                    if (llwActive && copyLen >= 62)
                        llwFlows.noteTransmit(header, header + 14, copyLen - 14, false, entry.queuedUS);
                    if (!urgent) {
                        auto &txBA = radio->awdlRadio.blockAckTX;
                        if (txBA.configured && txBA.forPeer(header) && txBA.phase != TXBlockAckQueue::Retiring) {
                            txBA.lastDataUS = entry.queuedUS;
                            if (pktLen >= TXBlockAckQueue::HeavyFrameBytes)
                                txBA.lastHeavyDataUS = entry.queuedUS;
                        }
                        if (radio->awdlRadio.listener && pktLen >= TXBlockAckQueue::HeavyFrameBytes && pktLen <= 1514)
                            radio->noteAWDLTXBlockAck(header, pktLen);
                    }
                }
            }
        }
        if (urgent) insertUrgent(entry);
        else {
            pending[(pendingHead + pendingCount) % arrayCapacity] = entry;
            ++pendingCount;
        }
        ++enqueued;
        if (duplicate) {
            entry.packet = duplicate;
            entry.multicastQueue = Pending::MulticastSlot8;
            pending[(pendingHead + pendingCount) % arrayCapacity] = entry;
            ++pendingCount;
            ++enqueued;
        }
    }
    if (enqueued && !stopping && !ticking && !wakeArmed && timer) {
        wakeArmed = true;
        timer->setTimeoutMS(1);
    }
    return pendingCount + inFlightCount + aggregateExtraOwned >= awdlLimit ? kIOReturnNoResources : kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::pullLLWOutputPackets()
{
    if (!radio->getMainWorkLoop()->inGate()) {
        return radio->getMainCommandGate()->runAction(
            [](OSObject *, void *self, void *, void *, void *) -> IOReturn {
                return static_cast<AirportItlwmAWDLInterface *>(self)->pullLLWOutputPackets();
            }, this);
    }
    constexpr unsigned arrayCapacity = sizeof(pending) / sizeof(pending[0]);
    const bool baAgreed = radio->awdlRadio.blockAckTX.configured &&
        radio->awdlRadio.blockAckTX.phase == TXBlockAckQueue::Agreed;
    const unsigned queueCapacity = (!baAgreed && arrayCapacity > 96U) ? 96U : arrayCapacity;
    const unsigned maxBatch = queueCapacity / 2 < 32 ? queueCapacity / 2 : 32;
    const unsigned used = pendingCount + inFlightCount + aggregateExtraOwned;
    if (!llwActive || !llwBSD || !llwBSD->getIfnet() || !requested || !running ||
        !radio->awdlRadio.ready || regulatoryPlanBlocked || regulatoryStopRequired ||
        stopping || used >= queueCapacity)
        return kIOReturnNoResources;
    const unsigned capacity = queueCapacity - used;
    mbuf_t packets = nullptr;
    IOReturn result = llwBSD->dequeueOutputPackets(capacity < maxBatch ? capacity : maxBatch, &packets);
    if (result != kIOReturnSuccess)
        return result == kIOReturnNoFrames ? kIOReturnSuccess : result;
    auto isUrgentEntry = [](const Pending &p) {
        if (p.retry || p.lowLatency) return true;
        if (p.multicastQueue || !p.packet) return false;
        const size_t len = mbuf_pkthdr_len(p.packet);
        uint8_t hdr[96];
        const size_t copy = len < sizeof(hdr) ? len : sizeof(hdr);
        return copy >= 14 && len <= 1514 &&
            !mbuf_copydata(p.packet, 0, copy, hdr) &&
            isUnicastPTPHeader(hdr, copy, len);
    };
    auto insertUrgent = [&](const Pending &item) {
        unsigned pos = 0;
        while (pos < pendingCount && isUrgentEntry(pending[(pendingHead + pos) % arrayCapacity]))
            ++pos;
        if (pos == pendingCount) {
            pending[(pendingHead + pendingCount) % arrayCapacity] = item;
        } else if (pos == 0) {
            pendingHead = (pendingHead + arrayCapacity - 1) % arrayCapacity;
            pending[pendingHead] = item;
        } else {
            for (unsigned k = pendingCount; k > pos; --k)
                pending[(pendingHead + k) % arrayCapacity] =
                    pending[(pendingHead + k - 1) % arrayCapacity];
            pending[(pendingHead + pos) % arrayCapacity] = item;
        }
        ++pendingCount;
    };
    unsigned remaining = 0, enqueued = 0;
    for (auto packet = packets; packet; packet = mbuf_nextpkt(packet)) ++remaining;
    while (packets) {
        mbuf_t packet = packets;
        packets = mbuf_nextpkt(packet); mbuf_setnextpkt(packet, nullptr);
        --remaining;
        if (mbuf_pkthdr_len(packet) > 1514) { mbuf_freem(packet); ++txDropped; continue; }
        Pending entry = {packet, awdlNow(), radio->dmaEpoch};
        mbuf_t duplicate = nullptr;
        uint8_t header[62];
        const size_t pktLen = mbuf_pkthdr_len(packet);
        const size_t copyLen = pktLen < sizeof(header) ? pktLen : sizeof(header);
        if (copyLen >= 14 && !mbuf_copydata(packet, 0, copyLen, header)) {
            if ((header[0] & 1) && be16(header + 12) == 0x86dd) {
                entry.multicastQueue = Pending::MulticastSocial;
                if (pendingCount + inFlightCount + aggregateExtraOwned + remaining + 2 <= queueCapacity &&
                    !mbuf_dup(packet, MBUF_DONTWAIT, &duplicate)) ++multicastCopiesQueued;
                else ++multicastCopiesDropped;
            } else if (be16(header + 12) == 0x86dd) {
                entry.lowLatency = true;
                if (copyLen >= 62)
                    llwFlows.noteTransmit(header, header + 14, copyLen - 14, true, entry.queuedUS);
            }
        }
        if (entry.lowLatency) insertUrgent(entry);
        else {
            pending[(pendingHead + pendingCount) % arrayCapacity] = entry;
            ++pendingCount;
        }
        ++enqueued;
        ++llwTxQueued;
        if (duplicate) {
            entry.packet = duplicate;
            entry.multicastQueue = Pending::MulticastSlot8;
            pending[(pendingHead + pendingCount) % arrayCapacity] = entry;
            ++pendingCount;
            ++enqueued;
        }
    }
    if (enqueued && !stopping && !ticking && !wakeArmed && timer) {
        wakeArmed = true;
        timer->setTimeoutMS(1);
    }
    return pendingCount + inFlightCount + aggregateExtraOwned >= queueCapacity ? kIOReturnNoResources : kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::enqueueLLWFrame(mbuf_t packet)
{
    if (!packet) return kIOReturnBadArgument;
    constexpr unsigned arrayCapacity = sizeof(pending) / sizeof(pending[0]);
    const bool baAgreed = radio->awdlRadio.blockAckTX.configured &&
        radio->awdlRadio.blockAckTX.phase == TXBlockAckQueue::Agreed;
    const unsigned queueCapacity = (!baAgreed && arrayCapacity > 96U) ? 96U : arrayCapacity;
    if (!llwActive || !requested || !running || !radio->awdlRadio.ready ||
        regulatoryPlanBlocked || regulatoryStopRequired || stopping ||
        pendingCount + inFlightCount + aggregateExtraOwned >= queueCapacity ||
        mbuf_pkthdr_len(packet) > 1514) {
        mbuf_freem(packet);
        ++txDropped;
        return kIOReturnNoResources;
    }
    Pending entry = {packet, awdlNow(), radio->dmaEpoch};
    mbuf_t duplicate = nullptr;
    uint8_t header[62];
    const size_t pktLen = mbuf_pkthdr_len(packet);
    const size_t copyLen = pktLen < sizeof(header) ? pktLen : sizeof(header);
    if (copyLen >= 14 && !mbuf_copydata(packet, 0, copyLen, header)) {
        if ((header[0] & 1) && be16(header + 12) == 0x86dd) {
            entry.multicastQueue = Pending::MulticastSocial;
            if (pendingCount + inFlightCount + aggregateExtraOwned + 2 <= queueCapacity &&
                !mbuf_dup(packet, MBUF_DONTWAIT, &duplicate)) ++multicastCopiesQueued;
            else ++multicastCopiesDropped;
        } else if (be16(header + 12) == 0x86dd) {
            entry.lowLatency = true;
            if (copyLen >= 62)
                llwFlows.noteTransmit(header, header + 14, copyLen - 14, true, entry.queuedUS);
        }
    }
    if (entry.lowLatency) {
        auto isUrgentEntry = [](const Pending &p) {
            if (p.retry || p.lowLatency) return true;
            if (p.multicastQueue || !p.packet) return false;
            const size_t len = mbuf_pkthdr_len(p.packet);
            uint8_t hdr[96];
            const size_t copy = len < sizeof(hdr) ? len : sizeof(hdr);
            return copy >= 14 && len <= 1514 &&
                !mbuf_copydata(p.packet, 0, copy, hdr) &&
                isUnicastPTPHeader(hdr, copy, len);
        };
        unsigned pos = 0;
        while (pos < pendingCount && isUrgentEntry(pending[(pendingHead + pos) % arrayCapacity]))
            ++pos;
        if (pos == pendingCount) {
            pending[(pendingHead + pendingCount) % arrayCapacity] = entry;
        } else if (pos == 0) {
            pendingHead = (pendingHead + arrayCapacity - 1) % arrayCapacity;
            pending[pendingHead] = entry;
        } else {
            for (unsigned k = pendingCount; k > pos; --k)
                pending[(pendingHead + k) % arrayCapacity] =
                    pending[(pendingHead + k - 1) % arrayCapacity];
            pending[(pendingHead + pos) % arrayCapacity] = entry;
        }
        ++pendingCount;
    } else {
        pending[(pendingHead + pendingCount) % arrayCapacity] = entry;
        ++pendingCount;
    }
    ++llwTxQueued;
    if (duplicate) {
        entry.packet = duplicate;
        entry.multicastQueue = Pending::MulticastSlot8;
        pending[(pendingHead + pendingCount) % arrayCapacity] = entry;
        ++pendingCount;
    }
    if (!stopping && !ticking && !drainingData && requested && running &&
        !regulatoryPlanBlocked && !regulatoryStopRequired && radio->awdlRadio.ready &&
        !radio->awdlRadio.changing && radio->awdlTxWindowOpen()) {
        drainPendingData();
    }
    if (pendingCount && !stopping && !ticking && !wakeArmed && timer) {
        wakeArmed = true;
        timer->setTimeoutMS(1);
    }
    return kIOReturnSuccess;
}

IOReturn AirportItlwmAWDLInterface::submitOutputPackets(bool fromLLW)
{
    if (!radio->getMainWorkLoop()->inGate()) {
        return radio->getMainCommandGate()->runAction(
            [](OSObject *, void *self, void *llw, void *, void *) -> IOReturn {
                return static_cast<AirportItlwmAWDLInterface *>(self)->submitOutputPackets(llw != nullptr);
            }, this, fromLLW ? this : nullptr);
    }
    IOReturn result = kIOReturnSuccess;
    if (fromLLW) {
        result = pullLLWOutputPackets();
    } else {
        const auto &tx = radio->awdlRadio.blockAckTX;
        const unsigned maxPasses = tx.configured && tx.phase == TXBlockAckQueue::Agreed ? 4U : 1U;
        // Stage a burst even if a channel switch has stopped TX completions.
        for (unsigned pass = 0; pass < maxPasses; ++pass) {
            const unsigned before = pendingCount;
            result = pullOutputPackets();
            if (result != kIOReturnSuccess || pendingCount == before) break;
        }
    }
    if (pendingCount && !stopping && !ticking && !drainingData && requested && running &&
        !regulatoryPlanBlocked && !regulatoryStopRequired && radio->awdlRadio.ready &&
        !radio->awdlRadio.changing && radio->awdlTxWindowOpen()) {
        drainPendingData();
        constexpr unsigned arrayCapacity = sizeof(pending) / sizeof(pending[0]);
        const bool baAgreed = radio->awdlRadio.blockAckTX.configured &&
            radio->awdlRadio.blockAckTX.phase == TXBlockAckQueue::Agreed;
        const unsigned queueCapacity = (!baAgreed && arrayCapacity > 96U) ? 96U : arrayCapacity;
        if (result == kIOReturnNoResources &&
            pendingCount + inFlightCount + aggregateExtraOwned < queueCapacity)
            result = kIOReturnSuccess;
    }
    return result;
}

void AirportItlwmAWDLInterface::txReady(void *context)
{
    auto self = static_cast<AirportItlwmAWDLInterface *>(context);
    if (!self || !self->radio || !self->radio->getMainWorkLoop()->inGate() ||
        self->stopping || self->ticking || self->drainingData ||
        !self->requested || !self->running || self->regulatoryPlanBlocked ||
        self->regulatoryStopRequired || !self->radio->awdlRadio.ready ||
        self->radio->awdlRadio.changing || !self->radio->awdlTxWindowOpen())
        return;
    const auto &state = self->radio->awdlRadio;
    const uint64_t now = awdlNow();
    uint64_t solicitDeadline = 1;
    if (!self->psfDue(now) && !self->broadcastDue(now)) {
        const auto *askPeer = self->radio->awdlPeers.find(state.blockAckTX.peer, now);
        Window local = {}, remote = {};
        if (askPeer && self->sync.window(now, local) && askPeer->window(now, remote) &&
            self->sync.canSendTo(*askPeer, now, false, state.listener) &&
            state.channel == local.channel && state.widthMHz == local.widthMHz()) {
            const uint64_t localRemain = state.listener ? local.untilChannelChangeUS : local.untilNextEAWUS;
            const uint64_t remoteRemain = state.listener ? remote.untilChannelChangeUS : remote.untilNextEAWUS;
            if (localRemain > 6000 && remoteRemain > 6000 && now < state.dwell.windowEndUS) {
                const uint64_t end = now + (localRemain < remoteRemain ? localRemain : remoteRemain) - 6000;
                solicitDeadline = state.dwell.windowEndUS < end ? state.dwell.windowEndUS : end;
            }
        }
    }
    const bool negotiating = state.blockAckTX.negotiatingNow(now, solicitDeadline) &&
        !state.deferReceiveProbe(now);
    // Drain pre-staged pending frames into newly freed hardware slots first,
    // then pull fresh frames up to queueCapacity so FQ_CODEL stays moving.
    if (self->pendingCount) self->drainPendingData();
    if (negotiating) {
        if (!self->wakeArmed && self->timer) { self->wakeArmed = true; self->timer->setTimeoutMS(1); }
        return;
    }
    constexpr unsigned arrayCapacity = sizeof(self->pending) / sizeof(self->pending[0]);
    const bool baAgreed = state.blockAckTX.configured &&
        state.blockAckTX.phase == TXBlockAckQueue::Agreed;
    const unsigned queueCapacity = (!baAgreed && arrayCapacity > 96U) ? 96U : arrayCapacity;
    const unsigned maxPasses = baAgreed ? 4U : 1U;
    bool pulled = false;
    for (unsigned pass = 0; pass < maxPasses &&
         self->pendingCount + self->inFlightCount + self->aggregateExtraOwned < queueCapacity; ++pass) {
        const unsigned before = self->pendingCount;
        if (self->bsd && self->bsd->getIfnet()) self->pullOutputPackets();
        if (self->llwActive && self->llwBSD && self->llwBSD->getIfnet()) self->pullLLWOutputPackets();
        if (self->pendingCount == before) break;
        pulled = true;
    }
    if (pulled && self->pendingCount) self->drainPendingData();
}

void AirportItlwmAWDLInterface::drainPendingData()
{
    if (stopping || ticking || drainingData || !requested || !running ||
        regulatoryPlanBlocked || regulatoryStopRequired || !pendingCount ||
        !radio->awdlRadio.ready || radio->awdlRadio.changing || !radio->awdlTxWindowOpen())
        return;
    drainingData = true;
    constexpr unsigned queueCapacity = sizeof(pending) / sizeof(pending[0]);
    const unsigned owned = pendingCount + inFlightCount + aggregateExtraOwned;
    unsigned remaining = pendingCount;
    unsigned keptCount = 0;
    auto pushHead = [&](const Pending &item) {
        pendingHead = (pendingHead + queueCapacity - 1) % queueCapacity;
        pending[pendingHead] = item;
        ++pendingCount;
    };
    auto reversePending = [&](unsigned startOffset, unsigned endOffset) {
        while (startOffset + 1 < endOffset) {
            --endOffset;
            const unsigned a = (pendingHead + startOffset) % queueCapacity;
            const unsigned b = (pendingHead + endOffset) % queueCapacity;
            const Pending tmp = pending[a];
            pending[a] = pending[b];
            pending[b] = tmp;
            ++startOffset;
        }
    };
    auto restoreFIFO = [&]() {
        if (!keptCount || keptCount >= pendingCount) return;
        const unsigned unvisited = pendingCount - keptCount;
        reversePending(0, unvisited);
        reversePending(unvisited, pendingCount);
        reversePending(0, pendingCount);
        keptCount = 0;
    };
    uint8_t blockedPeers[8][6] = {};
    unsigned blockedPeerCount = 0;
    auto isPeerBlocked = [&](const uint8_t *addr) {
        for (unsigned b = 0; b < blockedPeerCount; ++b)
            if (sameAddress(blockedPeers[b], addr)) return true;
        return false;
    };
    auto markPeerBlocked = [&](const uint8_t *addr) {
        if (isPeerBlocked(addr)) return true;
        if (blockedPeerCount >= 8) return false;
        copyAddress(blockedPeers[blockedPeerCount++], addr);
        return true;
    };
    while (remaining--) {
        uint64_t now = awdlNow();
        auto onChannel = [&]() {
            Window current = {};
            return requested && !stopping && !regulatoryPlanBlocked && !regulatoryStopRequired &&
                radio->awdlRadio.ready && !radio->awdlRadio.changing &&
                radio->awdlRadio.dwell.running && !radio->awdlRadio.dwell.cancelling &&
                now < radio->awdlRadio.dwell.windowEndUS && sync.window(now, current) &&
                current.channel && current.channel == radio->awdlRadio.channel &&
                current.widthMHz() == radio->awdlRadio.widthMHz;
        };
        if (!onChannel()) break;
        auto headUrgent = [&]() {
            const auto &head = pending[pendingHead];
            if (head.retry || head.lowLatency) return true;
            if (!head.packet || head.second) return false;
            const size_t headSize = mbuf_pkthdr_len(head.packet);
            uint8_t headBytes[1514];
            return headSize >= 14 && headSize <= sizeof(headBytes) &&
                !mbuf_copydata(head.packet, 0, headSize, headBytes) &&
                isUnicastPTP(headBytes, headSize);
        };
        const bool drainMediaActive = (llwActive && llwParamsEnabled) ||
            radio->awdlRadio.heavyReceiveActive(now) || radio->awdlRadio.heavyTransmitActive(now);
        const uint64_t drainLeadUS = drainMediaActive ? 12000ULL : 2000ULL;
        const bool drainMgmtBusy = drainMediaActive && radio->awdlRadio.queue != IWX_INVALID_QUEUE &&
            radio->com.txq[radio->awdlRadio.queue].queued != 0;
        if (!headUrgent() && !drainMgmtBusy &&
            now + drainLeadUS < radio->awdlRadio.dwell.windowEndUS && (psfDue(now) || broadcastDue(now))) {
            if (!wakeArmed && timer) { wakeArmed = true; timer->setTimeoutMS(1); }
            break;
        }
        Pending entry = pending[pendingHead]; pending[pendingHead] = {};
        pendingHead = (pendingHead + 1) % queueCapacity; --pendingCount;
        uint8_t ethernet[1514], secondEthernet[1514];
        const size_t size = mbuf_pkthdr_len(entry.packet);
        const bool expired = now >= entry.queuedUS && now - entry.queuedUS > DataLifetimeUS;
        const bool secondExpired = entry.second && (now < entry.secondQueuedUS ||
            now - entry.secondQueuedUS > DataLifetimeUS);
        const bool copiedEthernet = entry.epoch == radio->dmaEpoch && now >= entry.queuedUS &&
            !expired && !secondExpired && size >= 14 && size <= sizeof(ethernet) &&
            !mbuf_copydata(entry.packet, 0, size, ethernet) &&
            sameAddress(ethernet + 6, address.octet);
        const bool multicast = copiedEthernet && (ethernet[0] & 1);
        const bool ptp = copiedEthernet && !multicast && isUnicastPTP(ethernet, size);
        const bool validIPv6 = copiedEthernet && be16(ethernet + 12) == 0x86dd &&
            ipv6Payload(ethernet + 14, size - 14);
        if (!validIPv6 && !ptp) {
            if (entry.epoch == radio->dmaEpoch && (expired || secondExpired)) dataTxExpired += entry.count();
            txDropped += entry.count();
            if (entry.retry) dataAbandoned += entry.count();
            releaseData(entry);
            continue;
        }
        const bool bonjour = multicast && isLinkLocalMDNS(ethernet, size);
        auto retry = [&]() {
            pending[(pendingHead + pendingCount) % queueCapacity] = entry;
            ++pendingCount;
            ++keptCount;
        };
        if (!multicast && !entry.lowLatency && !ptp && isPeerBlocked(ethernet)) {
            retry();
            continue;
        }
        auto canAggregate = [&]() {
            if (entry.lowLatency || ptp) return false;
            const auto *peer = multicast ? nullptr : radio->awdlPeers.find(ethernet, now);
            const auto &tx = radio->awdlRadio.blockAckTX;
            if (!multicast && tx.forPeer(ethernet) && !tx.allowsSoftwareAggregate(now) &&
                !(tx.phase == TXBlockAckQueue::Asking && radio->awdlRadio.deferReceiveProbe(now)))
                return tx.phase == TXBlockAckQueue::Agreed && tx.amsdu && peer && peer->txHTMcs(now);
            return radio->awdlRadio.dataQueueConfigured && peer &&
                peer->aggregateTX(now, radio->dmaEpoch, radio->awdlRadio.channel, radio->awdlMinRateMbps);
        };
        auto splitUnsubmitted = [&]() {
            Pending second = entry;
            second.packet = entry.second; second.queuedUS = entry.secondQueuedUS;
            second.dataSequence = entry.secondSequence;
            second.second = nullptr; second.secondQueuedUS = 0;
            entry.second = nullptr; --aggregateExtraOwned;
            pushHead(second);
            ++remaining;
        };
        auto splitRetry = [&]() {
            const auto &tx = radio->awdlRadio.blockAckTX;
            return tx.forPeer(ethernet) && tx.phase == TXBlockAckQueue::Agreed && !tx.amsdu;
        };
        if (entry.second && (!entry.retry || splitRetry()) && !canAggregate()) splitUnsubmitted();
        uint64_t transmitEndUS = 0;
        auto eligible = [&]() {
            transmitEndUS = 0;
            const auto &state = radio->awdlRadio;
            const auto &tx = state.blockAckTX;
            const bool peerTX = !multicast && !entry.lowLatency && tx.forPeer(ethernet);
            const bool blockAck = peerTX && tx.phase == TXBlockAckQueue::Agreed;
            const bool holdForAnnouncement = !(entry.lowLatency || ptp) && !drainMgmtBusy &&
                now + drainLeadUS < state.dwell.windowEndUS && (psfDue(now) || broadcastDue(now));
            if (!onChannel() || holdForAnnouncement ||
                (entry.retry && entry.grant == state.dwellStarts) ||
                state.queue == IWX_INVALID_QUEUE ||
                (entry.second && !canAggregate())) return false;
            if (peerTX) {
                if (!ptp && (tx.phase == TXBlockAckQueue::Enabling || tx.phase == TXBlockAckQueue::Retiring))
                    return false;
                uint64_t solicitDeadline = 1;
                if (!psfDue(now) && !broadcastDue(now)) {
                    const auto *askPeer = radio->awdlPeers.find(tx.peer, now);
                    Window local = {}, remote = {};
                    if (askPeer && sync.window(now, local) && askPeer->window(now, remote) &&
                        sync.canSendTo(*askPeer, now, false, state.listener) &&
                        state.channel == local.channel && state.widthMHz == local.widthMHz()) {
                        const uint64_t localRemain = state.listener ? local.untilChannelChangeUS : local.untilNextEAWUS;
                        const uint64_t remoteRemain = state.listener ? remote.untilChannelChangeUS : remote.untilNextEAWUS;
                        if (localRemain > 6000 && remoteRemain > 6000 && now < state.dwell.windowEndUS) {
                            const uint64_t end = now + (localRemain < remoteRemain ? localRemain : remoteRemain) - 6000;
                            solicitDeadline = state.dwell.windowEndUS < end ? state.dwell.windowEndUS : end;
                        }
                    }
                }
                if (tx.negotiatingNow(now, solicitDeadline) && !state.deferReceiveProbe(now))
                    return false;
            }
            const bool peerBAOpen = blockAck && state.listener && tx.configured && tx.enabled &&
                !tx.paused && tx.windowEndUS > now && radio->com.txq[tx.queue].queued < tx.window;
            const bool ptpBypassBA = ptp && peerTX && !peerBAOpen;
            const bool useBlockAck = blockAck && !ptpBypassBA;
            if (useBlockAck && state.listener) {
                if (!peerBAOpen) return false;
            } else if ((entry.lowLatency || (ptp && (!peerTX || ptpBypassBA))) && state.llwQueueConfigured) {
                if (radio->com.txq[state.llwQueue].queued >= 48U) return false;
            } else {
                const unsigned limit = bonjour ? 56U :
                    (!ptp && peerTX && tx.configured && tx.phase == TXBlockAckQueue::Asking &&
                     !tx.allowsSoftwareAggregate(now) && !state.deferReceiveProbe(now) ? 4U : 48U);
                if (radio->com.txq[state.queue].queued + (state.dataQueueConfigured ?
                        radio->com.txq[state.dataQueue].queued : 0) >= limit) return false;
            }
            if (entry.multicastQueue) {
                Window local = {};
                if (!sync.localDataWindow(now, true, local) || !radio->awdlChannelAllowed(local.channel))
                    return false;
                const unsigned slot = (local.counter / sync.presence) % 16;
                if ((entry.multicastQueue == Pending::MulticastSlot8) != (slot == 8)) return false;
                return true;
            }
            if (bonjour) {
                Window local;
                return sync.localDataWindow(now, true, local) && radio->awdlChannelAllowed(local.channel);
            }
            const int station3Depth =
                (state.queue != IWX_INVALID_QUEUE ? radio->com.txq[state.queue].queued : 0) +
                (state.dataQueueConfigured ? radio->com.txq[state.dataQueue].queued : 0) +
                (state.llwQueueConfigured ? radio->com.txq[state.llwQueue].queued : 0);
            const bool station3Queued = station3Depth > 2;
            const bool llwRealtimeActive = state.llwRealtime || (llwActive && llwParamsEnabled);
            const uint64_t guardUS = state.listener && llwRealtimeActive && !station3Queued ? 500ULL :
                (((entry.lowLatency || ptp) && state.listener) ? 1500ULL : 3000ULL);
            for (const auto &peer : radio->awdlPeers.peers) {
                if ((multicast || sameAddress(peer.address, ethernet)) &&
                    sync.canSendTo(peer, now, multicast, state.listener, guardUS)) {
                    if (!multicast) {
                        Window local = {}, remote = {};
                        const uint64_t localRemain = !sync.window(now, local) ? 0 :
                            (state.listener ? local.untilChannelChangeUS : local.untilNextEAWUS);
                        const uint64_t remoteRemain = !peer.window(now, remote) ? 0 :
                            (state.listener ? remote.untilChannelChangeUS : remote.untilNextEAWUS);
                        uint64_t remainingUS = localRemain < remoteRemain ? localRemain : remoteRemain;
                        if (state.listener && now < state.dwell.windowEndUS &&
                            state.dwell.windowEndUS - now < remainingUS)
                            remainingUS = state.dwell.windowEndUS - now;
                        const uint64_t margin = useBlockAck && state.listener ? 6000 : guardUS;
                        if (remainingUS <= margin) return false;
                        transmitEndUS = now + remainingUS - margin;
                        if (useBlockAck && state.listener && tx.windowEndUS < transmitEndUS)
                            transmitEndUS = tx.windowEndUS;
                    }
                    return true;
                }
            }
            return false;
        };
        if (!eligible()) {
            if (!multicast && !entry.lowLatency && !ptp && !markPeerBlocked(ethernet)) {
                pushHead(entry);
                break;
            }
            retry();
            continue;
        }
        Pending partner = {};
        size_t secondSize = 0;
        bool joining = false;
        if (!entry.second && !entry.retry && !entry.lowLatency && !ptp && canAggregate() && remaining && pendingCount) {
            partner = pending[pendingHead];
            secondSize = mbuf_pkthdr_len(partner.packet);
            joining = !partner.retry && !partner.second && !partner.lowLatency && !partner.multicastQueue &&
                partner.epoch == entry.epoch && now >= partner.queuedUS &&
                now - partner.queuedUS <= DataLifetimeUS && secondSize >= 54 &&
                secondSize <= sizeof(secondEthernet) &&
                !mbuf_copydata(partner.packet, 0, secondSize, secondEthernet) &&
                sameAddress(ethernet, secondEthernet) && sameAddress(ethernet + 6, secondEthernet + 6) &&
                be16(secondEthernet + 12) == 0x86dd && ipv6Payload(secondEthernet + 14, secondSize - 14) &&
                !isUnicastPTP(secondEthernet, secondSize);
        } else if (entry.second) {
            secondSize = mbuf_pkthdr_len(entry.second);
            if (secondSize < 54 || secondSize > sizeof(secondEthernet) ||
                mbuf_copydata(entry.second, 0, secondSize, secondEthernet)) {
                txDropped += entry.count(); dataAbandoned += entry.count();
                releaseData(entry); continue;
            }
        }
        const bool llwQos = entry.lowLatency && radio->awdlRadio.llwQueueConfigured;
        const size_t capacity = entry.second || joining ?
            26 + size + 16 + ((4 - ((size + 16) & 3)) & 3) + secondSize + 16 :
            (entry.lowLatency ? size + (llwQos ? 20 : 18) : size + 26);
        mbuf_t frame = awdlPacket(capacity);
        if (!frame) { pushHead(entry); break; }
        now = awdlNow();
        if (now < entry.queuedUS || now - entry.queuedUS > DataLifetimeUS ||
            (entry.second && (now < entry.secondQueuedUS || now - entry.secondQueuedUS > DataLifetimeUS))) {
            dataTxExpired += entry.count(); txDropped += entry.count();
            if (entry.retry) dataAbandoned += entry.count();
            mbuf_freem(frame); releaseData(entry);
            continue;
        }
        if (joining && (!canAggregate() || now < partner.queuedUS ||
            now - partner.queuedUS > DataLifetimeUS)) joining = false;
        if (entry.second && (!entry.retry || splitRetry()) && !canAggregate()) splitUnsubmitted();
        if (!eligible()) {
            mbuf_freem(frame);
            if (!multicast && !entry.lowLatency && !ptp && !markPeerBlocked(ethernet)) {
                pushHead(entry);
                break;
            }
            retry();
            continue;
        }
        if (!entry.retry) {
            entry.wifiSequence = sync.wifiSequence;
            entry.dataSequence = sync.dataSequence;
            entry.secondSequence = uint16_t(sync.dataSequence + 1);
        }
        const bool aggregate = entry.second || joining;
        auto bytes = static_cast<uint8_t *>(mbuf_data(frame));
        const bool useLLWQos = entry.lowLatency && radio->awdlRadio.llwQueueConfigured && capacity >= size + 20;
        size_t encoded = aggregate ? dataPairFromEthernet(ethernet, size,
            secondEthernet, secondSize, bytes, capacity, entry.wifiSequence,
            entry.dataSequence, entry.secondSequence) :
            (entry.lowLatency ? dataFromEthernetLLW(ethernet, size, bytes, capacity,
                entry.wifiSequence, useLLWQos, RadioState::LLWTID) :
             dataFromEthernet(ethernet, size, bytes, capacity, entry.wifiSequence, entry.dataSequence));
        if (!encoded && ptp && !aggregate && !entry.lowLatency &&
            unicastAddress(ethernet + 6) && capacity >= size + 26) {
            memset(bytes, 0, 40);
            bytes[0] = 0x08;
            copyAddress(bytes + 4, ethernet);
            copyAddress(bytes + 10, ethernet + 6);
            copyAddress(bytes + 16, BSSID);
            putLE16(bytes + 22, uint16_t((entry.wifiSequence & 0xfff) << 4));
            memcpy(bytes + 24, SNAP, 8);
            putLE16(bytes + 32, 0x0403);
            putLE16(bytes + 34, entry.dataSequence);
            putBE16(bytes + 38, be16(ethernet + 12));
            memcpy(bytes + 40, ethernet + 14, size - 14);
            encoded = size + 26;
        }
        if (!encoded) {
            mbuf_freem(frame);
            txDropped += entry.count();
            if (entry.retry) dataAbandoned += entry.count();
            releaseData(entry); continue;
        }
        mbuf_setlen(frame, encoded); mbuf_pkthdr_setlen(frame, encoded);
        if (entry.retry && !multicast) bytes[1] = 0x08;
        if (joining) {
            entry.second = partner.packet; entry.secondQueuedUS = partner.queuedUS;
            pending[pendingHead] = {}; pendingHead = (pendingHead + 1) % queueCapacity;
            --pendingCount; ++aggregateExtraOwned; --remaining;
        }
        unsigned slot = 0;
        while (inFlight[slot].token) ++slot;
        if (!++dataToken) ++dataToken;
        entry.group = multicast;
        inFlight[slot] = {entry, dataToken};
        inFlight[slot].entry.grant = radio->awdlRadio.dwellStarts;
        ++inFlightCount;
        const int error = radio->transmitAWDL(frame, dataToken, transmitEndUS);
        if (!error) {
            if (!multicast) {
                recordNativeDataActivity(true, unsigned(size));
                if (aggregate) recordNativeDataActivity(true, unsigned(secondSize));
            }
            dataSubmitted += entry.count();
            if (aggregate) ++dataAggregatesSubmitted;
            if (entry.retry) dataRetried += entry.count();
            else { ++sync.wifiSequence; sync.dataSequence += entry.count(); }
            continue;
        }
        inFlight[slot] = {}; --inFlightCount;
        if (error == EAGAIN && entry.second && !entry.retry) {
            Pending second = {entry.second, entry.secondQueuedUS, entry.epoch};
            entry.second = nullptr; --aggregateExtraOwned;
            if (!multicast && !entry.lowLatency && !ptp && markPeerBlocked(ethernet)) {
                retry();
                pending[(pendingHead + pendingCount) % queueCapacity] = second;
                ++pendingCount;
                ++keptCount;
                continue;
            }
            pushHead(second); pushHead(entry);
            break;
        }
        if (error == EAGAIN && !multicast && !entry.lowLatency && !ptp && markPeerBlocked(ethernet)) {
            retry();
            continue;
        }
        if (error == ENOBUFS || error == ENOMEM || error == ENETDOWN || error == EAGAIN) {
            pushHead(entry); break;
        }
        txDropped += entry.count();
        if (entry.retry) dataAbandoned += entry.count();
        releaseData(entry);
    }
    restoreFIFO();
    const bool baAgreedAtEnd = radio->awdlRadio.blockAckTX.configured &&
        radio->awdlRadio.blockAckTX.phase == TXBlockAckQueue::Agreed;
    const unsigned signalCapacity = (!baAgreedAtEnd && queueCapacity > 96U) ? 96U : queueCapacity;
    if ((pendingCount + inFlightCount + aggregateExtraOwned < owned || outputCapacityReleased) &&
        pendingCount + inFlightCount + aggregateExtraOwned < signalCapacity && requested && !stopping &&
        radio->awdlRadio.ready) {
        outputCapacityReleased = false;
        if (bsd && bsd->getIfnet()) bsd->signalOutputThread();
        if (llwActive && llwBSD && llwBSD->getIfnet()) llwBSD->signalOutputThread();
    }
    if (pendingCount && !stopping && !wakeArmed && timer) {
        wakeArmed = true;
        timer->setTimeoutMS(1);
    }
    drainingData = false;
}

IOReturn AirportItlwmAWDLInterface::configureBroadcast(const apple80211_awdl_oob_request &in,
                                                      bool automatic)
{
    if (stopping || (!automatic && (!requested || !radio->awdlRadio.ready ||
        regulatoryPlanBlocked || regulatoryStopRequired)))
        return kIOReturnNotReady;
    const uint8_t destination[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    if (in.version || !sameAddress(in.destination, destination) || !sameAddress(in.bssid, BSSID))
        return kIOReturnBadArgument;
    if (in.flags != 6) return kIOReturnUnsupported;
    if (automatic && in.tx_offset_us >= uint32_t(sync.periodTU) * sync.presence * 1024)
        return kIOReturnUnsupported;
    // A zero map disables automatic broadcasts; its payload may also be empty.
    if ((!automatic || in.tx_channel_map || in.data_len) && !oobPayload(in.data, in.data_len))
        return kIOReturnBadArgument;
    if (automatic) {
        broadcast.configured = true;
        broadcast.channelMap = in.tx_channel_map;
        broadcast.offsetUS = in.tx_offset_us;
        broadcast.sent = false;
        broadcast.size = in.data_len;
        if (in.data_len) memcpy(broadcast.payload, in.data, in.data_len);
    } else {
        const uint64_t now = awdlNow();
        if (immediateBroadcast.size && now >= immediateBroadcast.queuedUS &&
            now - immediateBroadcast.queuedUS < 1000000) return kIOReturnBusy;
        immediateBroadcast.queuedUS = now;
        immediateBroadcast.size = in.data_len;
        memcpy(immediateBroadcast.payload, in.data, in.data_len);
    }
    if (timer) timer->setTimeoutMS(1);
    return kIOReturnSuccess;
}

bool AirportItlwmAWDLInterface::broadcastDue(uint64_t now) const
{
    if (immediateBroadcast.size && now >= immediateBroadcast.queuedUS &&
        now - immediateBroadcast.queuedUS < 1000000) return true;
    if (!broadcast.configured) return now >= nextMIFUS;
    Window window = {};
    if (!broadcast.size || !sync.window(now, window) || !window.channel) return false;
    // tx_chan_map selects entries in the 16-entry channel sequence, one per EAW.
    if (!(broadcast.channelMap & (uint32_t(1) << ((window.counter / sync.presence) % 16))))
        return false;
    const uint64_t duration = uint64_t(sync.periodTU) * sync.presence * 1024;
    const uint16_t counter = window.counter - window.counter % sync.presence;
    return duration - window.untilNextEAWUS >= broadcast.offsetUS &&
        (!broadcast.sent || broadcast.lastWindowCounter != counter);
}

bool AirportItlwmAWDLInterface::psfDue(uint64_t now) const
{
    return afTxMode == 0 && now >= nextActionUS;
}

IOReturn AirportItlwmAWDLInterface::queueUnicast(const apple80211_awdl_oob_request &in)
{
    if (stopping || !requested || !radio->awdlRadio.ready || regulatoryPlanBlocked ||
        regulatoryStopRequired) return kIOReturnNotReady;
    if (in.version || !unicastAddress(in.destination) || sameAddress(in.destination, address.octet) ||
        !sameAddress(in.bssid, BSSID) || !oobPayload(in.data, in.data_len, true)) return kIOReturnBadArgument;
    if (in.flags != 6 && in.flags != 14) return kIOReturnUnsupported;
    const uint64_t now = awdlNow();
    auto peer = radio->awdlPeers.find(in.destination, now);
    Window window = {};
    if (!peer || !peer->nativeCached || !nativeWindowOpen || nativeWindowClosing ||
        radio->awdlRadio.fault || !sync.transmitReady(now) || !sync.window(now, window) ||
        window.channel != nativeWindowChannel ||
        uint16_t(window.counter - window.counter % sync.presence) != nativeWindowCounter)
        return kIOReturnNotReady;
    UnicastRequest *slot = nullptr;
    for (auto &entry : unicast) {
        if (entry.state != UnicastRequest::Free && sameAddress(entry.destination, in.destination))
            return kIOReturnBusy;
        if (!slot && entry.state == UnicastRequest::Free) slot = &entry;
    }
    if (!slot) return kIOReturnNoResources;
    const uint64_t token = nextManagementToken();
    if (!token) return kIOReturnNoResources;
    *slot = {};
    slot->state = UnicastRequest::Queued;
    slot->token = token;
    slot->queuedUS = now;
    slot->deadlineUS = now + window.untilNextEAWUS;
    slot->windowCounter = nativeWindowCounter;
    slot->channel = window.channel;
    copyAddress(slot->destination, in.destination);
    slot->size = in.data_len;
    memcpy(slot->payload, in.data, in.data_len);
    if (timer) timer->setTimeoutMS(1);
    return kIOReturnSuccess;
}

void AirportItlwmAWDLInterface::completeUnicast(uint64_t token, bool acknowledged)
{
    if (!token) return;
    auto armWake = [&]() {
        if (!stopping && !wakeArmed && timer) {
            if (!ticking) wakeArmed = true;
            timer->setTimeoutMS(1);
        }
    };
    if (radio->completeAWDLTXManagement(token, acknowledged)) {
        armWake();
        return;
    }
    for (auto &entry : blockAckReplies) {
        if (entry.token != token || entry.state != BlockAckReply::Submitted) continue;
        if (acknowledged) ++blockAckAcknowledged;
        else ++blockAckFailed;
        const bool isActiveLLWPeer = llwParamsEnabled &&
            sameAddress(entry.request.peer, llwParams.peer_mac);
        if (acknowledged && entry.accepted && !entry.request.teardown &&
            entry.request.tid != RadioState::LLWTID && !isActiveLLWPeer &&
            radio->awdlBlockAckLive(entry.request, entry.cookie) &&
            !radio->awdlRadio.heavyReceiveActive(awdlNow()))
            radio->noteAWDLTXBlockAck(entry.request.peer, 0, true);
        if (entry.epoch == radio->dmaEpoch) {
            if (entry.request.teardown && acknowledged) {
                radio->completeAWDLBlockAckEnd(entry.request, entry.cookie);
            } else if (!acknowledged && entry.attempts < 3 &&
                       (entry.request.teardown || entry.accepted) &&
                       radio->awdlBlockAckLive(entry.request, entry.cookie)) {
                const uint64_t now = awdlNow();
                if (now >= entry.queuedUS && now - entry.queuedUS < BlockAckReply::LifetimeUS) {
                    entry.token = nextManagementToken();
                    if (entry.token) {
                        entry.state = BlockAckReply::Queued;
                        armWake();
                        return;
                    }
                }
            }
        }
        entry = {};
        return;
    }
    for (auto &entry : unicast) {
        if (entry.token != token || entry.state != UnicastRequest::Submitted) continue;
        entry.status = acknowledged ? 0 : 0xda;
        entry.state = UnicastRequest::Complete;
        if (acknowledged) ++unicastAcknowledged;
        else ++unicastFailed;
        armWake();
        return;
    }
}

void AirportItlwmAWDLInterface::completeManagement(uint64_t token, bool acknowledged, bool flushed)
{
    if (!token) return;
    if (!flushed) {
        completeUnicast(token, acknowledged);
        return;
    }
    auto armWake = [&]() {
        if (!stopping && !wakeArmed && timer) {
            if (!ticking) wakeArmed = true;
            timer->setTimeoutMS(1);
        }
    };
    auto &tx = radio->awdlRadio.blockAckTX;
    if (token == tx.managementToken) {
        tx.managementToken = 0;
        armWake();
        return;
    }
    for (auto &entry : blockAckReplies) {
        if (entry.token != token || entry.state != BlockAckReply::Submitted) continue;
        if (entry.attempts) --entry.attempts;
        if (entry.epoch == radio->dmaEpoch && requested && running && !stopping &&
            (entry.request.teardown || entry.accepted) &&
            radio->awdlBlockAckLive(entry.request, entry.cookie)) {
            const uint64_t now = awdlNow();
            if (now >= entry.queuedUS && now - entry.queuedUS < BlockAckReply::LifetimeUS) {
                entry.token = nextManagementToken();
                if (entry.token) {
                    entry.state = BlockAckReply::Queued;
                    armWake();
                    return;
                }
            }
        }
        entry = {};
        return;
    }
    for (auto &entry : unicast) {
        if (entry.token != token || entry.state != UnicastRequest::Submitted) continue;
        const uint64_t now = awdlNow();
        if (requested && running && !stopping && !regulatoryPlanBlocked && !regulatoryStopRequired &&
            radio->awdlRadio.ready && !radio->awdlRadio.fault && !radio->awdlRadio.stopPending &&
            now >= entry.queuedUS && now - entry.queuedUS < 1000000 && now < entry.deadlineUS) {
            const auto *peer = radio->awdlPeers.find(entry.destination, now);
            if (peer && peer->nativeCached) {
                const uint64_t nextToken = nextManagementToken();
                if (nextToken) {
                    entry.token = nextToken;
                    entry.state = UnicastRequest::Queued;
                    armWake();
                    return;
                }
            }
        }
        entry.status = 0xda;
        entry.state = UnicastRequest::Complete;
        ++unicastFailed;
        armWake();
        return;
    }
}

void AirportItlwmAWDLInterface::completeData(uint64_t token, int result)
{
    if (!token) return;
    for (auto &flight : inFlight) {
        if (flight.token != token) continue;
        Pending entry = flight.entry;
        flight = {};
        --inFlightCount;
        if (result == ECANCELED) {
            dataFlushed += entry.count();
            const uint64_t now = awdlNow();
            const uint8_t maxFlushes = entry.group ? 1 : MaxDataFlushes;
            if (entry.flushes < maxFlushes && entry.epoch == radio->dmaEpoch && requested && running &&
                !stopping && !regulatoryPlanBlocked && !regulatoryStopRequired &&
                radio->awdlRadio.ready && !radio->awdlRadio.stopPending && !radio->awdlRadio.fault &&
                now >= entry.queuedUS && now - entry.queuedUS <= DataLifetimeUS &&
                (!entry.second || (now >= entry.secondQueuedUS &&
                    now - entry.secondQueuedUS <= DataLifetimeUS))) {
                ++entry.flushes;
                entry.retry = true;
                constexpr unsigned queueCapacity = sizeof(pending) / sizeof(pending[0]);
                unsigned pos = 0;
                while (pos < pendingCount) {
                    const auto &other = pending[(pendingHead + pos) % queueCapacity];
                    if (!other.retry || int16_t(other.wifiSequence - entry.wifiSequence) > 0)
                        break;
                    ++pos;
                }
                if (pos == 0) {
                    pendingHead = (pendingHead + queueCapacity - 1) % queueCapacity;
                    pending[pendingHead] = entry;
                } else {
                    for (unsigned k = pendingCount; k > pos; --k)
                        pending[(pendingHead + k) % queueCapacity] = pending[(pendingHead + k - 1) % queueCapacity];
                    pending[(pendingHead + pos) % queueCapacity] = entry;
                }
                ++pendingCount;
                if (!stopping && !ticking && !wakeArmed && timer) {
                    wakeArmed = true;
                    timer->setTimeoutMS(1);
                }
                return;
            }
        }
        if (result) { txDropped += entry.count(); dataAbandoned += entry.count(); }
        else if (entry.group) dataGroupCompleted += entry.count();
        else dataUnicastAcknowledged += entry.count();
        releaseData(entry);
        constexpr unsigned arrayCapacity = sizeof(pending) / sizeof(pending[0]);
        const bool baAgreed = radio->awdlRadio.blockAckTX.configured &&
            radio->awdlRadio.blockAckTX.phase == TXBlockAckQueue::Agreed;
        const unsigned queueCapacity = (!baAgreed && arrayCapacity > 96U) ? 96U : arrayCapacity;
        if (!ticking && !drainingData && !stopping && requested && running && !regulatoryPlanBlocked && !regulatoryStopRequired &&
            radio->awdlRadio.ready && pendingCount + inFlightCount + aggregateExtraOwned < queueCapacity) {
            outputCapacityReleased = false;
            if (bsd && bsd->getIfnet()) bsd->signalOutputThread();
            if (llwActive && llwBSD && llwBSD->getIfnet()) llwBSD->signalOutputThread();
        } else {
            outputCapacityReleased = true;
        }
        if (!stopping && !ticking && !wakeArmed && timer) {
            wakeArmed = true;
            timer->setTimeoutMS(1);
        }
        return;
    }
}

void AirportItlwmAWDLInterface::serviceUnicast()
{
    for (auto &entry : unicast) {
        if (entry.state == UnicastRequest::Free) continue;
        const uint64_t token = entry.token;
        uint64_t now = awdlNow();
        const auto &state = radio->awdlRadio;
        auto currentPeer = [&]() -> const Peer * {
            if (!requested || stopping || regulatoryPlanBlocked || regulatoryStopRequired ||
                !state.ready || now < entry.queuedUS ||
                now - entry.queuedUS >= 1000000) return nullptr;
            const auto *peer = radio->awdlPeers.find(entry.destination, now);
            return peer && peer->nativeCached ? peer : nullptr;
        };
        auto inRequestWindow = [&]() {
            Window window = {};
            return !state.fault && !state.stopPending && now < entry.deadlineUS &&
                nativeWindowOpen && !nativeWindowClosing &&
                nativeWindowChannel == entry.channel && nativeWindowCounter == entry.windowCounter &&
                sync.window(now, window) && window.channel == entry.channel &&
                uint16_t(window.counter - window.counter % sync.presence) == entry.windowCounter;
        };
        const auto *peer = currentPeer();
        if (entry.state != UnicastRequest::Complete && (!peer ||
            (entry.state == UnicastRequest::Queued && !inRequestWindow()))) {
            entry.state = UnicastRequest::Complete;
            entry.status = 0xda;
            ++unicastFailed;
        }
        if (entry.state == UnicastRequest::Complete) {
            uint8_t status[7];
            copyAddress(status, entry.destination); status[6] = entry.status;
            if (getController()->postMessage(this, APPLE80211_M_AWDL_UNICAST_AF_STATUS,
                    status, sizeof(status), true) == kIOReturnSuccess) entry = {};
            else ++unicastNotifyErrors;
            continue;
        }
        if (entry.state == UnicastRequest::Submitted) continue;
        auto eligible = [&](const Peer *peer) {
            return entry.token == token && entry.state == UnicastRequest::Queued &&
                peer && inRequestWindow() && state.channel == entry.channel &&
                !state.changing && state.dwell.running && !state.dwell.cancelling &&
                now < state.dwell.windowEndUS &&
                sync.canSendTo(*peer, now, false, state.listener) && !broadcastDue(now) && !psfDue(now) &&
                state.queue != IWX_INVALID_QUEUE && radio->com.txq[state.queue].queued < 48;
        };
        if (!eligible(peer)) continue;
        mbuf_t packet = awdlPacket(1536);
        if (!packet) continue;
        now = awdlNow();
        const bool clampWideRates = true;
        const size_t size = eligible(currentPeer()) ? sync.action(static_cast<uint8_t *>(mbuf_data(packet)),
            1536, now, true, entry.payload, entry.size, entry.destination, false, clampWideRates) : 0;
        if (!size) { mbuf_freem(packet); continue; }
        mbuf_setlen(packet, size); mbuf_pkthdr_setlen(packet, size);
        entry.state = UnicastRequest::Submitted;
        const int error = radio->transmitAWDL(packet, token, entry.deadlineUS);
        if (!error) {
            ++unicastSubmitted;
        } else if (entry.token == token && entry.state == UnicastRequest::Submitted) {
            // HAL consumed the frame but published no descriptor. Retry from the owned native payload.
            now = awdlNow();
            const bool temporary = error == ENETDOWN || error == ENOBUFS || error == ENOMEM;
            if (temporary && currentPeer() && inRequestWindow())
                entry.state = UnicastRequest::Queued;
            else
                completeUnicast(token, false);
        }
    }
}

void AirportItlwmAWDLInterface::regulatoryMapChanged()
{
    if (stopping || regulatoryPlanBlocked) return;
    for (unsigned i = 0; i < 16; ++i) {
        const auto channel = sync.channels[i];
        if (!channel || radio->awdlChannelAllowed(channel, sync.channelClasses[i] == 128 ? 80 : 20)) continue;
        // A legal current slot must not advertise now-forbidden future slots.
        // The MCC commit callback holds the gate and cannot sleep in cleanup.
        regulatoryPlanBlocked = regulatoryStopRequired = regulatoryResumePending = true;
        drain();
        immediateBroadcast.size = 0;
        for (auto &entry : unicast) {
            if (entry.state == UnicastRequest::Free || entry.state == UnicastRequest::Complete) continue;
            entry.state = UnicastRequest::Complete;
            entry.status = 0xda;
            ++unicastFailed;
        }
        if (timer) timer->setTimeoutMS(1);
        return;
    }
}

void AirportItlwmAWDLInterface::updateAvailability()
{
    uint64_t now;
    uint16_t counter;
    Window window = {};
    const auto &state = radio->awdlRadio;
    // HAL stop/reset can disable and restore MAC1 between native timer ticks.
    if ((!state.ready || !requested || stopping) && (pendingCount || inFlightCount)) drain();
    bool transmitClockReady = false;
    auto readWindow = [&]() {
        now = awdlNow();
        if (!requested || stopping || regulatoryPlanBlocked || regulatoryStopRequired || !state.ready ||
            state.fault || !sync.window(now, window) || !window.channel) return false;
        counter = window.counter - window.counter % sync.presence;
        transmitClockReady = sync.transmitReady(now);
        return transmitClockReady ||
            (!state.changing && !state.stopPending && radio->awdlChannelAllowed(window.channel, window.widthMHz()) &&
             (sync.rootConfirmsElectionWindow(radio->awdlPeers, now, window) ||
              sync.parentLostElectionWindow(radio->awdlPeers, now, window)));
    };
    bool scheduled = readWindow();
    // Native AW notifications describe the logical EAW. Short radio grants and
    // their AP gaps do not advance its counter; every TX still checks the grant.
    if (nativeWindowOpen && !nativeWindowClosing && scheduled &&
        nativeWindowCounter == counter && nativeWindowChannel == window.channel)
        return;

    if (nativeWindowOpen) {
        if (!nativeWindowClosing) {
            nativeWindowEnd = {};
            nativeWindowEnd.timestamp_be = OSSwapHostToBigInt32(uint32_t(now));
            nativeWindowClosing = true;
        }
        if (getController()->postMessage(this, APPLE80211_M_AWDL_AVAILABILITY_WINDOW_EXTENSIONS_END,
                &nativeWindowEnd, sizeof(nativeWindowEnd), true) != kIOReturnSuccess) {
            ++nativeWindowErrors;
            return; // A later START must not overtake a failed END.
        }
        nativeWindowOpen = nativeWindowClosing = false;
        ++nativeWindowEnds;
        // Enqueuing END allocates memory. Recheck the window and grant before the next START.
        scheduled = readWindow();
    }
    const bool active = scheduled && state.dwell.running && !state.dwell.cancelling &&
        !state.dwell.failed && now < state.dwell.windowEndUS && window.channel == state.channel &&
        window.widthMHz() == state.widthMHz;
    if (!active || (nativeWindowStarts && nativeWindowCounter == counter)) return;

    apple80211_availability_window_start start = {};
    start.aw_counter = counter;
    start.sync_state = 1; // Advertising-master profile supported by SYNC_STATE.
    copyAddress(start.sync_master, sync.sync);
    copyAddress(start.top_master, sync.master);
    const uint64_t eawDurationUS = uint64_t(sync.periodTU) * sync.presence * 1024ULL;
    const uint64_t elapsedInEAWUS = eawDurationUS > window.untilNextEAWUS ?
        (eawDurationUS - window.untilNextEAWUS) : 0;
    const uint64_t eawStartUS = now >= elapsedInEAWUS ? (now - elapsedInEAWUS) : 0;
    start.timestamp_be = OSSwapHostToBigInt32(uint32_t(eawStartUS));
    start.channel.version = APPLE80211_VERSION;
    start.channel.channel = window.channel;
    start.channel.flags = (window.widthMHz() == 80 ? APPLE80211_C_FLAG_80MHZ : APPLE80211_C_FLAG_20MHZ) |
        (window.channel <= 14 ? APPLE80211_C_FLAG_2GHZ : APPLE80211_C_FLAG_5GHZ);
#if DEBUG
    const bool directParentRecovery = !transmitClockReady && sync.distance == 1;
#endif
    if (getController()->postMessage(this, APPLE80211_M_AWDL_AVAILABILITY_WINDOW_START,
            &start, sizeof(start), true) != kIOReturnSuccess) {
        ++nativeWindowErrors;
        return;
    }
    nativeWindowOpen = true;
    nativeWindowSerial = state.dwellStarts;
    nativeWindowEndUS = now + window.untilNextEAWUS;
    nativeWindowCounter = counter;
    nativeWindowChannel = window.channel;
    nativeWindowPeriodTU = sync.periodTU;
    nativeWindowPresence = sync.presence;
    ++nativeWindowStarts;
#if DEBUG
    if (!transmitClockReady) {
        if (directParentRecovery) ++parentRecoveryWindowStarts;
        else ++relayRecoveryWindowStarts;
    }
#endif
}

uint64_t AirportItlwmAWDLInterface::peerGrantEnd(uint64_t now, const RadioState::PeerGrant &grant,
                                               bool requireDemand) const
{
    Window local = {}, remote = {};
    if (!grant.valid || grant.epoch != radio->dmaEpoch || now >= grant.endUS ||
        !requested || stopping || regulatoryPlanBlocked || regulatoryStopRequired ||
        !radio->awdlChannelAllowed(6) || !sync.transmitReady(now) || !sync.window(now, local) ||
        sync.periodTU != grant.localPeriod || sync.presence != grant.localPresence ||
        local.channel != 6 || local.counter - local.counter % sync.presence != grant.localCounter)
        return 0;
    const auto peer = radio->awdlPeers.find(grant.address, now);
    if (!peer || !peer->window(now, remote) || peer->sync.windowPeriodTU != grant.remotePeriod ||
        peer->sync.presenceMode != grant.remotePresence || remote.channel != 6 ||
        remote.counter - remote.counter % peer->sync.presenceMode != grant.remoteCounter)
        return 0;
    const unsigned slot = (remote.counter / peer->sync.presenceMode) % 16;
    const bool remoteDense = peer->sequence.channel[(slot + 15) % 16] == 6 ||
        peer->sequence.channel[(slot + 1) % 16] == 6;
    if (grant.dense) {
        const unsigned localSlot = (local.counter / sync.presence) % 16;
        if (!remoteDense || (sync.channels[(localSlot + 15) % 16] != 6 &&
            sync.channels[(localSlot + 1) % 16] != 6)) return 0;
    } else if (remoteDense) return 0;
    auto queuedForPeer = [&](const Pending &entry, const uint8_t *peerAddress) {
        if (!entry.packet || entry.epoch != radio->dmaEpoch || now < entry.queuedUS ||
            now - entry.queuedUS > DataLifetimeUS) return false;
        const size_t size = mbuf_pkthdr_len(entry.packet);
        uint8_t header[54];
        return size >= sizeof(header) && size <= 1514 &&
            !mbuf_copydata(entry.packet, 0, sizeof(header), header) &&
            sameAddress(header, peerAddress) && sameAddress(header + 6, address.octet) &&
            be16(header + 12) == 0x86dd && ipv6Payload(header + 14, size - 14);
    };
    auto demandForPeer = [&](const Peer &candidate) {
        if (candidate.lastUnicastDataUS && now >= candidate.lastUnicastDataUS &&
            now - candidate.lastUnicastDataUS <= DataLifetimeUS) return true;
        constexpr unsigned queueCapacity = sizeof(pending) / sizeof(pending[0]);
        for (unsigned i = 0; i < pendingCount; ++i)
            if (queuedForPeer(pending[(pendingHead + i) % queueCapacity], candidate.address)) return true;
        for (const auto &flight : inFlight)
            if (flight.token && queuedForPeer(flight.entry, candidate.address)) return true;
        return false;
    };
    // A running grant can wait for its peer after the last local TX is acknowledged.
    // Other peers on this channel share the grant; each TX checks its recipient's window.
    if (requireDemand && !demandForPeer(*peer)) return 0;
    uint64_t end = now + local.untilNextEAWUS;
    const uint64_t remoteEnd = now + remote.untilNextEAWUS;
    if (remoteEnd < end) end = remoteEnd;
    return end < grant.endUS ? end : grant.endUS;
}

void AirportItlwmAWDLInterface::tick()
{
    if (stopping || ticking) return;
    wakeArmed = false;
#if DEBUG
    if (receiveFilterChanging) {
        if (timer) timer->setTimeoutMS(5);
        return;
    }
#endif
    retain();
    ticking = true;
    uint64_t now = awdlNow();
    updateAvailability();
    bool stoppedForRegulatory = false;
    if ((regulatoryStopRequired || radio->awdlRadio.stopPending) && !radio->awdlRadio.changing &&
        !radio->awdlRadio.cleanupError) {
#if DEBUG
        const uint64_t epoch = radio->dmaEpoch;
#endif
        stoppedForRegulatory = regulatoryStopRequired;
        const int error = radio->stopAWDLRadio();
        if (error != EINPROGRESS) regulatoryStopRequired = false;
        if (error && error != EINPROGRESS) setProperty("RadioError", uint64_t(error), 32);
#if DEBUG
        if (epoch == radio->dmaEpoch && radio->awdlReceiveFilterCleanupResult == kIOReturnBusy) {
            radio->awdlReceiveFilterCleanupResult = error == EINPROGRESS ? kIOReturnBusy :
                (error ? kIOReturnIOError : kIOReturnSuccess);
            if (error != EINPROGRESS) publishReceiveFilter();
        }
#endif
        now = awdlNow();
    }
    auto infrastructurePowered = [&]() {
        const auto &device = radio->com;
        const unsigned flags = device.sc_ic.ic_ac.ac_if.if_flags;
        return (flags & (IFF_UP | IFF_RUNNING)) == (IFF_UP | IFF_RUNNING) &&
            !(device.sc_flags & (IWX_FLAG_SHUTDOWN | IWX_FLAG_HW_ERR | IWX_FLAG_RFKILL));
    };
    if (requested && !stopping && infrastructurePowered() && !regulatoryPlanBlocked && !regulatoryStopRequired &&
        !stoppedForRegulatory &&
        radio->com.ns_nstate == IEEE80211_S_RUN &&
        !radio->awdlRadio.ready && !radio->awdlRadio.stopPending &&
        !radio->awdlRadio.fault && now >= nextRetryUS) {
        nextRetryUS = now + 1000000;
        int error = radio->startAWDLRadio(address.octet);
        setProperty("RadioError", uint64_t(error), 32);
        now = awdlNow();
        // Firmware command sleeps release the gate; disable/stop may run meanwhile.
        if (!requested || stopping || !infrastructurePowered() ||
            radio->com.ns_nstate != IEEE80211_S_RUN || regulatoryPlanBlocked || regulatoryStopRequired)
            radio->stopAWDLRadio();
    }
    if (stopping) { ticking = false; release(); return; }
    const bool available = requested && infrastructurePowered() && !regulatoryPlanBlocked && !regulatoryStopRequired &&
        radio->awdlRadio.ready;
    if (!available || (immediateBroadcast.size && (now < immediateBroadcast.queuedUS ||
        now - immediateBroadcast.queuedUS >= 1000000))) immediateBroadcast.size = 0;
    const bool infrastructureReloading = (radio->com.sc_ic.ic_ac.ac_if.if_flags & IFF_UP) &&
        radio->awdlRadio.controllerReinit &&
        !(radio->com.sc_flags & (IWX_FLAG_SHUTDOWN | IWX_FLAG_RFKILL));
    const bool recoverInfrastructure = requested && interfaceEnabled &&
        (infrastructurePowered() || infrastructureReloading) &&
        !regulatoryPlanBlocked && !regulatoryStopRequired && !stoppedForRegulatory && !radio->awdlRadio.fault &&
        !radio->awdlRadio.cleanupError;
    if (infrastructurePaused && !recoverInfrastructure) {
        infrastructurePaused = false;
        IO80211VirtualInterface::setLinkState(kIO80211NetworkLinkDown, 0);
        if (bsd && bsd->getIfnet()) {
            bsd->setLinkState(kIO80211NetworkLinkDown);
            if (reportedLinkQuality != -2) {
                reportLinkQuality(uint32_t(-2));
                reportedLinkQuality = -2;
            }
        }
    }
    if (running != available) {
        running = available;
        if (available) {
            infrastructurePaused = false;
            if (!regulatoryResumePending) broadcast.sent = false;
            regulatoryResumePending = false;
            const uint64_t previousEpochUS = sync.epochUS;
            const uint16_t previousCounter = sync.epochCounter;
            const bool previousLockedParent = sync.lockedFixedParent;
            sync.restart(radio->awdlRadio.channel, now);
            // Native sync pauses retain the previous AW number. Resume that
            // host clock while waiting for any new parent-clock correction.
            if (nativeWindowStarts && now >= previousEpochUS &&
                sync.periodTU == nativeWindowPeriodTU && sync.presence == nativeWindowPresence) {
                sync.epochUS = previousEpochUS;
                sync.epochCounter = previousCounter;
                sync.lockedFixedParent = previousLockedParent;
                const uint64_t periodUS = uint64_t(sync.periodTU) * 1024;
                Window probe = {};
                if (previousLockedParent && periodUS && sync.presence &&
                    now >= sync.nativeSyncUS && now - sync.nativeSyncUS >= Peer::ScheduleFreshnessUS &&
                    now >= sync.nativeSequenceUS && now - sync.nativeSequenceUS >= Peer::ScheduleFreshnessUS &&
                    sync.window(now, probe)) {
                    const uint32_t cycle = uint32_t(16) * sync.presence;
                    probe.counter = uint16_t(probe.counter - probe.counter % cycle);
                    if (sync.parentLostElectionWindow(radio->awdlPeers, now, probe)) {
                        const uint64_t projected = uint64_t(previousCounter) + (now - previousEpochUS) / periodUS;
                        sync.epochUS = now;
                        sync.epochCounter = uint16_t(((projected / cycle) + 1) * cycle);
                    }
                }
            }
            if (!sequenceConfigured)
                sync.discoverySchedule(radio->awdlChannelAllowed(6), radio->awdlChannelAllowed(44),
                    radio->awdlChannelAllowed(149),
                    !radio->awdlRadio.wideCircuitTripped() && radio->awdlChannelAllowed(149, 80),
                    !radio->awdlRadio.wideCircuitTripped() && radio->awdlChannelAllowed(44, 80));
            nextActionUS = nextMIFUS = 0;
            if (bsd && bsd->getIfnet()) {
                bsd->setLinkState(kIO80211NetworkLinkUp);
                IO80211VirtualInterface::setLinkState(kIO80211NetworkLinkUp, 0);
                if (reportedLinkQuality != 100) {
                    reportLinkQuality(100);
                    reportedLinkQuality = 100;
                }
                if (running && requested && radio->awdlRadio.ready)
                    bsd->startOutputThread();
            }
        } else {
            // Keep the peer manager alive across AP reassociation. Its link-down
            // callback disables sync, which would remove the radio restart request.
            infrastructurePaused = recoverInfrastructure;
            if (!interfaceEnabled || requested) {
                if (!infrastructurePaused) {
                    IO80211VirtualInterface::setLinkState(kIO80211NetworkLinkDown, 0);
                    if (bsd && bsd->getIfnet() && reportedLinkQuality != -2) {
                        reportLinkQuality(uint32_t(-2));
                        reportedLinkQuality = -2;
                    }
                }
                if (bsd && bsd->getIfnet()) bsd->setLinkState(kIO80211NetworkLinkDown);
                if (llwBSD && llwBSD->getIfnet()) llwBSD->setLinkState(kIO80211NetworkLinkDown);
                llwLinkUp = false;
            }
            drain();
        }
    }
    syncLLWLinkState();
    if (running && bsd && bsd->getIfnet() && reportedLinkQuality != 100) {
        reportLinkQuality(100);
        reportedLinkQuality = 100;
    }
    if (running) {
        sync.expireScanPunchout(now);
        radio->awdlPeers.expire(now);
        sync.elect(radio->awdlPeers, now);
        Window window = {};
        sync.window(now, window);
        const uint16_t slot = sync.presence ? window.counter / sync.presence : 0;
        // Firmware commands can sleep while native code replaces the schedule.
        auto isolatedEnd = [](const void *context, uint64_t at, uint16_t slot, uint16_t channel) -> uint64_t {
            auto self = static_cast<const AirportItlwmAWDLInterface *>(context);
            Window current = {};
            if (!channel || !self->requested || self->stopping || self->regulatoryPlanBlocked ||
                self->regulatoryStopRequired || !self->sync.window(at, current) ||
                current.counter / self->sync.presence != slot || current.channel != channel ||
                self->sync.channels[(slot + 15) % 16] || self->sync.channels[(slot + 1) % 16])
                return 0;
            return at + current.untilNextEAWUS;
        };
        const bool isolated = isolatedEnd(this, now, slot, window.channel) != 0;
        bool dataDemand = pendingCount != 0 || inFlightCount != 0;
        if (isolated && !dataDemand) {
            for (const auto &peer : radio->awdlPeers.peers) {
                if (peer.nativeCached) { dataDemand = true; break; }
                Window peerWindow = {};
                if (peer.lastUnicastDataUS && now >= peer.lastUnicastDataUS &&
                    now - peer.lastUnicastDataUS <= DataLifetimeUS &&
                    peer.window(now, peerWindow) && peerWindow.channel == window.channel) {
                    dataDemand = true;
                    break;
                }
            }
        }
        const bool listener = radio->awdlRadio.listener;
        const uint16_t policySlot = listener ? window.runStartEAW : slot;
        RadioState::GrantPolicy policy = {policySlot, isolated, dataDemand, this, isolatedEnd};
        policy.widthMHz = window.widthMHz();
        policy.windowEnd = [](const void *context, uint64_t at, uint16_t slot, uint16_t channel) -> uint64_t {
            auto self = static_cast<const AirportItlwmAWDLInterface *>(context);
            Window current = {};
            if (!channel || !self->requested || self->stopping || self->regulatoryPlanBlocked ||
                self->regulatoryStopRequired || !self->sync.window(at, current)) return 0;
            if (self->radio->awdlRadio.listener) {
                const auto &state = self->radio->awdlRadio;
                const auto widthMatchesOrFellBack = [&](const Window &w) {
                    return w.widthMHz() == state.widthMHz ||
                        (state.widthMHz == 20 && w.widthMHz() == 80 && state.wideCircuitTripped());
                };
                if (current.channel != channel) {
                    auto countSlots = [&](uint16_t ch) -> unsigned {
                        unsigned n = 0;
                        for (unsigned s = 0; s < 16; ++s)
                            if (self->sync.channels[s] == ch)
                                ++n;
                        return n;
                    };
                    Window nextCurrent = {};
                    if (self->llwActive && self->llwParamsEnabled && current.channel &&
                        current.untilChannelChangeUS <= 500ULL &&
                        countSlots(channel) > countSlots(current.channel) &&
                        self->sync.window(at + current.untilChannelChangeUS, nextCurrent) &&
                        nextCurrent.channel == channel && nextCurrent.runStartEAW == slot &&
                        (state.channel != channel || widthMatchesOrFellBack(nextCurrent)))
                        return at + current.untilChannelChangeUS + nextCurrent.untilChannelChangeUS;
                    return 0;
                }
                if (current.runStartEAW != slot) return 0;
                if (state.channel == channel && !widthMatchesOrFellBack(current))
                    return 0;
                return at + current.untilChannelChangeUS;
            }
            if (current.channel != channel || current.counter / self->sync.presence != slot) return 0;
            return at + current.untilNextEAWUS;
        };
        policy.peerEnd = [](const void *context, uint64_t at, const RadioState::PeerGrant &grant, bool requireDemand) {
            return static_cast<const AirportItlwmAWDLInterface *>(context)->peerGrantEnd(at, grant, requireDemand);
        };
        if (!isolated && window.channel == 6) {
            // Keep the peer chosen for a running grant; another peer cannot extend it.
            if (radio->awdlRadio.dwell.requested && radio->awdlRadio.dwell.peer.valid)
                policy.peer = radio->awdlRadio.dwell.peer;
            else for (const auto &peer : radio->awdlPeers.peers) {
                Window remote = {};
                if (!peer.window(now, remote) || remote.channel != 6) continue;
                RadioState::PeerGrant grant = {};
                grant.valid = true;
                const unsigned remoteSlot = (remote.counter / peer.sync.presenceMode) % 16;
                grant.dense = peer.sequence.channel[(remoteSlot + 15) % 16] == 6 ||
                    peer.sequence.channel[(remoteSlot + 1) % 16] == 6;
                copyAddress(grant.address, peer.address);
                grant.epoch = radio->dmaEpoch;
                grant.endUS = now + window.untilNextEAWUS;
                grant.localCounter = window.counter - window.counter % sync.presence;
                grant.remoteCounter = remote.counter - remote.counter % peer.sync.presenceMode;
                grant.localPeriod = sync.periodTU; grant.localPresence = sync.presence;
                grant.remotePeriod = peer.sync.windowPeriodTU; grant.remotePresence = peer.sync.presenceMode;
                if (const uint64_t end = peerGrantEnd(now, grant, true)) {
                    grant.endUS = end;
                    policy.peer = grant;
                    break;
                }
            }
        }
        constexpr unsigned queueCapacity = sizeof(pending) / sizeof(pending[0]);
        bool hasPendingLLW = false;
        bool hasPendingPTP = false;
        if (pendingCount) {
            for (unsigned i = 0; i < pendingCount; ++i) {
                const auto &entry = pending[(pendingHead + i) % queueCapacity];
                if (entry.lowLatency) { hasPendingLLW = true; continue; }
                if (!entry.packet || entry.epoch != radio->dmaEpoch || now < entry.queuedUS ||
                    now - entry.queuedUS > DataLifetimeUS) continue;
                const size_t entryLen = mbuf_pkthdr_len(entry.packet);
                if (!hasPendingPTP && !entry.multicastQueue && entryLen >= 14 && entryLen <= 1514) {
                    uint8_t ptpHdr[96];
                    const size_t ptpCopy = entryLen < sizeof(ptpHdr) ? entryLen : sizeof(ptpHdr);
                    if (!mbuf_copydata(entry.packet, 0, ptpCopy, ptpHdr) &&
                        isUnicastPTPHeader(ptpHdr, ptpCopy, entryLen))
                        hasPendingPTP = true;
                }
                if (policy.dataQueue) continue;
                uint8_t destination[6];
                if (entryLen < 54 ||
                    mbuf_copydata(entry.packet, 0, sizeof(destination), destination) || (destination[0] & 1)) continue;
                const auto *peer = radio->awdlPeers.find(destination, now);
                const auto &tx = radio->awdlRadio.blockAckTX;
                const bool allowSoftwareAgg = !tx.forPeer(destination) || tx.allowsSoftwareAggregate(now) ||
                    (tx.phase == TXBlockAckQueue::Asking && radio->awdlRadio.deferReceiveProbe(now));
                policy.dataQueue = allowSoftwareAgg && peer && sync.canSendTo(*peer, now, false, listener) &&
                    peer->aggregateTX(now, radio->dmaEpoch, window.channel, radio->awdlMinRateMbps) &&
                    (entry.second || pendingCount > 1);
            }
        }
        policy.llwQueue = (llwActive && (llwParamsEnabled || hasPendingLLW)) || (listener && hasPendingPTP);
        policy.llwRealtime = llwActive && llwParamsEnabled;
        uint16_t serviceChannel = window.channel;
        uint64_t serviceEndUS = now + (listener ? window.untilChannelChangeUS : window.untilNextEAWUS);
        if (listener) {
            policy.bridgeInfraToWide = false;
            unsigned zeroSlots = 0;
            for (unsigned s = 0; s < 16; ++s) zeroSlots += sync.channels[s] == 0;
            bool hasActivePeer = false;
            for (const auto &p : radio->awdlPeers.peers) {
                if (p.occupied && (p.nativeCached ||
                    (p.lastUnicastDataUS && now >= p.lastUnicastDataUS &&
                     now - p.lastUnicastDataUS < 5000000ULL))) {
                    hasActivePeer = true;
                    break;
                }
            }
            bool hasUnicastOrBA = false;
            for (const auto &u : unicast)
                if (u.state != UnicastRequest::Free) { hasUnicastOrBA = true; break; }
            if (!hasUnicastOrBA) {
                for (const auto &b : blockAckReplies)
                    if (b.state != BlockAckReply::Free) { hasUnicastOrBA = true; break; }
            }
            bool rxBAIdle = true;
            bool infraAssociated = radio->com.ns_nstate == IEEE80211_S_RUN;
#ifdef KERNEL
            rxBAIdle = !radio->awdlHasRXBA();
            const auto assocIC = radio->get80211Controller();
            infraAssociated = infraAssociated && assocIC && assocIC->ic_state == IEEE80211_S_RUN;
#endif
            const bool baseIdleCandidate = !infraAssociated && rxBAIdle && zeroSlots >= 8 && !hasActivePeer &&
                !hasUnicastOrBA && !pendingCount && !inFlightCount && !immediateBroadcast.size &&
                !(llwActive && llwParamsEnabled);
            AirportAWDL::evaluateIdleUnbindOn(
                radio->awdlRadio, now, window.channel, baseIdleCandidate, policy, serviceChannel);
            if (!window.channel && !policy.unbindIdle && infraAssociated)
                policy.parkIdleOnInfra = true;
            if (llwActive && llwParamsEnabled && window.channel && window.untilChannelChangeUS <= 500ULL) {
                const auto &st = radio->awdlRadio;
                const int station3Depth =
                    (st.queue != IWX_INVALID_QUEUE ? radio->com.txq[st.queue].queued : 0) +
                    (st.dataQueueConfigured ? radio->com.txq[st.dataQueue].queued : 0) +
                    (st.llwQueueConfigured ? radio->com.txq[st.llwQueue].queued : 0);
                if (station3Depth <= 2 && sync.presence) {
                    auto countSlots = [&](uint16_t ch) -> unsigned {
                        unsigned n = 0;
                        for (unsigned s = 0; s < 16; ++s)
                            if (sync.channels[s] == ch)
                                ++n;
                        return n;
                    };
                    const unsigned nextSlot = ((window.counter / sync.presence) + 1) % 16;
                    const uint16_t nextChan = sync.channels[nextSlot];
                    if (nextChan && nextChan != window.channel &&
                        countSlots(nextChan) > countSlots(window.channel)) {
                        const uint64_t nextStartUS = now + window.untilChannelChangeUS;
                        Window nextWindow = {};
                        if (sync.window(nextStartUS, nextWindow) && nextWindow.channel == nextChan &&
                            radio->awdlChannelAllowed(nextChan, nextWindow.widthMHz())) {
                            serviceChannel = nextChan;
                            policy.slot = nextWindow.runStartEAW;
                            policy.widthMHz = nextWindow.widthMHz();
                            policy.parkIdleOnInfra = false;
                            policy.bridgeInfraToWide = false;
                            serviceEndUS = nextStartUS + nextWindow.untilChannelChangeUS;
                        }
                    }
                }
            }
        }
        int error = radio->serviceAWDLChannel(serviceChannel, serviceEndUS, policy);
        if (error) setProperty("RadioError", uint64_t(error), 32);
        // If PHY downgraded a wide slot to 20 MHz (infra overlap or wide circuit breaker),
        // reconcile sync.channelClasses so onChannel() and advertised TLVs stay consistent.
        if (!error && radio->awdlRadio.ready && radio->awdlRadio.channel &&
            radio->awdlRadio.widthMHz == 20 && policy.widthMHz == 80 &&
            serviceChannel == radio->awdlRadio.channel) {
            bool demoted = false;
            for (unsigned i = 0; i < 16; ++i)
                if (sync.channels[i] == radio->awdlRadio.channel && sync.channelClasses[i] == 128) {
                    sync.channelClasses[i] = 0;
                    demoted = true;
                }
            if (demoted) sync.sequenceDiverged = true;
        }
        now = awdlNow();
        if (!requested || stopping || regulatoryPlanBlocked || regulatoryStopRequired)
            radio->stopAWDLRadio();
        auto onChannel = [&]() {
            Window current = {};
            return requested && !stopping && !regulatoryPlanBlocked && !regulatoryStopRequired &&
                radio->awdlRadio.ready && !radio->awdlRadio.changing &&
                radio->awdlRadio.dwell.running && !radio->awdlRadio.dwell.cancelling &&
                now < radio->awdlRadio.dwell.windowEndUS && sync.window(now, current) &&
                current.channel && current.channel == radio->awdlRadio.channel &&
                current.widthMHz() == radio->awdlRadio.widthMHz;
        };
        updateAvailability();
        // Service ADDBA Responses and unicast actions first; due announcements queue ahead of data.
        bool announcementFailed = false;
        bool sentAnnouncement = false;
        const auto mediaActive = [&](uint64_t at) {
            return (llwActive && llwParamsEnabled) ||
                radio->awdlRadio.heavyReceiveActive(at) || radio->awdlRadio.heavyTransmitActive(at);
        };
        auto announcementWindowOpen = [&]() {
            const uint64_t leadUS = mediaActive(now) ? 12000ULL : 2000ULL;
            return onChannel() && now + leadUS < radio->awdlRadio.dwell.windowEndUS;
        };
        auto sendDueAnnouncements = [&]() {
            for (unsigned action = 0; action < 3; ++action) {
                now = awdlNow();
                const bool mif = broadcastDue(now);
                if (!psfDue(now) && !mif) break;
                if (!announcementWindowOpen()) { announcementFailed = true; break; }
                const bool once = mif && immediateBroadcast.size && now >= immediateBroadcast.queuedUS &&
                    now - immediateBroadcast.queuedUS < 1000000;
                if (!once && mediaActive(now) && radio->awdlRadio.queue != IWX_INVALID_QUEUE &&
                    radio->com.txq[radio->awdlRadio.queue].queued) {
                    announcementFailed = true;
                    break;
                }
                mbuf_t packet = awdlPacket(1536);
                if (!packet) { announcementFailed = true; break; }
                now = awdlNow();
                const bool onceNow = mif && immediateBroadcast.size && now >= immediateBroadcast.queuedUS &&
                    now - immediateBroadcast.queuedUS < 1000000;
                const bool nativeMIF = mif && (onceNow || broadcast.configured);
                const uint8_t *payload = !nativeMIF ? frameTemplate :
                    (onceNow ? immediateBroadcast.payload : broadcast.payload);
                const size_t payloadSize = !nativeMIF ? templateSize :
                    (onceNow ? immediateBroadcast.size : broadcast.size);
                const bool clampWideRates = true;
                size_t size = announcementWindowOpen() && (mif ? broadcastDue(now) : psfDue(now)) ?
                    sync.action(static_cast<uint8_t *>(mbuf_data(packet)), 1536,
                        now, mif, payload, payloadSize, nullptr, nativeMIF, clampWideRates) : 0;
                if (!size) { mbuf_freem(packet); announcementFailed = true; break; }
                mbuf_setlen(packet, size); mbuf_pkthdr_setlen(packet, size);
                if (radio->transmitAWDL(packet)) { announcementFailed = true; break; }
                sentAnnouncement = true;
                if (nativeMIF) {
                    ++broadcastSubmitted;
                    if (onceNow) immediateBroadcast.size = 0;
                    else {
                        Window sent = {}; sync.window(now, sent);
                        broadcast.sent = true;
                        broadcast.lastWindowCounter = sent.counter - sent.counter % sync.presence;
                    }
                } else if (mif) nextMIFUS = now + 16 * 4 * 1024;
                else nextActionUS = now + uint64_t(sync.actionPeriodTU) * 1024;
            }
        };
        serviceBlockAck();
        serviceUnicast();
        sendDueAnnouncements();
        if (sentAnnouncement) {
            serviceUnicast();
            serviceBlockAck();
        }
        const unsigned owned = pendingCount + inFlightCount + aggregateExtraOwned;
        unsigned remaining = pendingCount;
        unsigned keptCount = 0;
        auto pushHead = [&](const Pending &item) {
            pendingHead = (pendingHead + queueCapacity - 1) % queueCapacity;
            pending[pendingHead] = item;
            ++pendingCount;
        };
        auto reversePending = [&](unsigned startOffset, unsigned endOffset) {
            while (startOffset + 1 < endOffset) {
                --endOffset;
                const unsigned a = (pendingHead + startOffset) % queueCapacity;
                const unsigned b = (pendingHead + endOffset) % queueCapacity;
                const Pending tmp = pending[a];
                pending[a] = pending[b];
                pending[b] = tmp;
                ++startOffset;
            }
        };
        auto restoreFIFO = [&]() {
            if (!keptCount || keptCount >= pendingCount) return;
            const unsigned unvisited = pendingCount - keptCount;
            reversePending(0, unvisited);
            reversePending(unvisited, pendingCount);
            reversePending(0, pendingCount);
            keptCount = 0;
        };
        uint8_t blockedPeers[8][6] = {};
        unsigned blockedPeerCount = 0;
        auto isPeerBlocked = [&](const uint8_t *addr) {
            for (unsigned b = 0; b < blockedPeerCount; ++b)
                if (sameAddress(blockedPeers[b], addr)) return true;
            return false;
        };
        auto markPeerBlocked = [&](const uint8_t *addr) {
            if (isPeerBlocked(addr)) return true;
            if (blockedPeerCount >= 8) return false;
            copyAddress(blockedPeers[blockedPeerCount++], addr);
            return true;
        };
        while (remaining--) {
            now = awdlNow();
            if (!announcementFailed && announcementWindowOpen() && (psfDue(now) || broadcastDue(now))) {
                sendDueAnnouncements();
                now = awdlNow();
            }
            Pending entry = pending[pendingHead]; pending[pendingHead] = {};
            pendingHead = (pendingHead + 1) % queueCapacity; --pendingCount;
            uint8_t ethernet[1514], secondEthernet[1514];
            const size_t size = mbuf_pkthdr_len(entry.packet);
            const bool expired = now >= entry.queuedUS && now - entry.queuedUS > DataLifetimeUS;
            const bool secondExpired = entry.second && (now < entry.secondQueuedUS ||
                now - entry.secondQueuedUS > DataLifetimeUS);
            const bool copiedEthernet = entry.epoch == radio->dmaEpoch && now >= entry.queuedUS &&
                !expired && !secondExpired && size >= 14 && size <= sizeof(ethernet) &&
                !mbuf_copydata(entry.packet, 0, size, ethernet) &&
                sameAddress(ethernet + 6, address.octet);
            const bool multicast = copiedEthernet && (ethernet[0] & 1);
            const bool ptp = copiedEthernet && !multicast && isUnicastPTP(ethernet, size);
            const bool validIPv6 = copiedEthernet && be16(ethernet + 12) == 0x86dd &&
                ipv6Payload(ethernet + 14, size - 14);
            if (!validIPv6 && !ptp) {
                if (entry.epoch == radio->dmaEpoch && (expired || secondExpired)) dataTxExpired += entry.count();
                txDropped += entry.count();
                if (entry.retry) dataAbandoned += entry.count();
                releaseData(entry);
                continue;
            }
            const bool bonjour = multicast && isLinkLocalMDNS(ethernet, size);
            auto retry = [&]() {
                pending[(pendingHead + pendingCount) % queueCapacity] = entry;
                ++pendingCount;
                ++keptCount;
            };
            if (!multicast && !entry.lowLatency && !ptp && isPeerBlocked(ethernet)) {
                retry();
                continue;
            }
            auto canAggregate = [&]() {
                if (entry.lowLatency || ptp) return false;
                const auto *peer = multicast ? nullptr : radio->awdlPeers.find(ethernet, now);
                const auto &tx = radio->awdlRadio.blockAckTX;
                if (!multicast && tx.forPeer(ethernet) && !tx.allowsSoftwareAggregate(now) &&
                    !(tx.phase == TXBlockAckQueue::Asking && radio->awdlRadio.deferReceiveProbe(now)))
                    return tx.phase == TXBlockAckQueue::Agreed && tx.amsdu && peer && peer->txHTMcs(now);
                return radio->awdlRadio.dataQueueConfigured && peer &&
                    peer->aggregateTX(now, radio->dmaEpoch, radio->awdlRadio.channel, radio->awdlMinRateMbps);
            };
            auto splitUnsubmitted = [&]() {
                Pending second = entry;
                second.packet = entry.second; second.queuedUS = entry.secondQueuedUS;
                second.dataSequence = entry.secondSequence;
                second.second = nullptr; second.secondQueuedUS = 0;
                entry.second = nullptr; --aggregateExtraOwned;
                pushHead(second);
                ++remaining;
            };
            auto splitRetry = [&]() {
                const auto &tx = radio->awdlRadio.blockAckTX;
                return tx.forPeer(ethernet) && tx.phase == TXBlockAckQueue::Agreed && !tx.amsdu;
            };
            if (entry.second && (!entry.retry || splitRetry()) && !canAggregate()) splitUnsubmitted();
            uint64_t transmitEndUS = 0;
            auto eligible = [&]() {
                transmitEndUS = 0;
                const auto &state = radio->awdlRadio;
                const auto &tx = state.blockAckTX;
                const bool peerTX = !multicast && !entry.lowLatency && tx.forPeer(ethernet);
                const bool blockAck = peerTX && tx.phase == TXBlockAckQueue::Agreed;
                const bool mgmtBusy = mediaActive(now) && state.queue != IWX_INVALID_QUEUE &&
                    radio->com.txq[state.queue].queued != 0;
                const bool holdForAnnouncement = !(entry.lowLatency || ptp) && !mgmtBusy &&
                    announcementWindowOpen() && (psfDue(now) || broadcastDue(now));
                if (!onChannel() || holdForAnnouncement ||
                    (entry.retry && entry.grant == state.dwellStarts) ||
                    state.queue == IWX_INVALID_QUEUE ||
                    (entry.second && !canAggregate())) return false;
                if (peerTX) {
                    if (!ptp && (tx.phase == TXBlockAckQueue::Enabling || tx.phase == TXBlockAckQueue::Retiring))
                        return false;
                    uint64_t solicitDeadline = 1;
                    if (!psfDue(now) && !broadcastDue(now)) {
                        const auto *askPeer = radio->awdlPeers.find(tx.peer, now);
                        Window local = {}, remote = {};
                        if (askPeer && sync.window(now, local) && askPeer->window(now, remote) &&
                            sync.canSendTo(*askPeer, now, false, state.listener) &&
                            state.channel == local.channel && state.widthMHz == local.widthMHz()) {
                            const uint64_t localRemain = state.listener ? local.untilChannelChangeUS : local.untilNextEAWUS;
                            const uint64_t remoteRemain = state.listener ? remote.untilChannelChangeUS : remote.untilNextEAWUS;
                            if (localRemain > 6000 && remoteRemain > 6000 && now < state.dwell.windowEndUS) {
                                const uint64_t end = now + (localRemain < remoteRemain ? localRemain : remoteRemain) - 6000;
                                solicitDeadline = state.dwell.windowEndUS < end ? state.dwell.windowEndUS : end;
                            }
                        }
                    }
                    if (tx.negotiatingNow(now, solicitDeadline) && !state.deferReceiveProbe(now))
                        return false;
                }
                const bool peerBAOpen = blockAck && state.listener && tx.configured && tx.enabled &&
                    !tx.paused && tx.windowEndUS > now && radio->com.txq[tx.queue].queued < tx.window;
                const bool ptpBypassBA = ptp && peerTX && !peerBAOpen;
                const bool useBlockAck = blockAck && !ptpBypassBA;
                if (useBlockAck && state.listener) {
                    if (!peerBAOpen) return false;
                } else if ((entry.lowLatency || (ptp && (!peerTX || ptpBypassBA))) && state.llwQueueConfigured) {
                    if (radio->com.txq[state.llwQueue].queued >= 48U) return false;
                } else {
                    const unsigned limit = bonjour ? 56U :
                        (!ptp && peerTX && tx.configured && tx.phase == TXBlockAckQueue::Asking &&
                         !tx.allowsSoftwareAggregate(now) && !state.deferReceiveProbe(now) ? 4U : 48U);
                    if (radio->com.txq[state.queue].queued + (state.dataQueueConfigured ?
                            radio->com.txq[state.dataQueue].queued : 0) >= limit) return false;
                }
                if (entry.multicastQueue) {
                    Window local = {};
                    if (!sync.localDataWindow(now, true, local) || !radio->awdlChannelAllowed(local.channel))
                        return false;
                    const unsigned slot = (local.counter / sync.presence) % 16;
                    if ((entry.multicastQueue == Pending::MulticastSlot8) != (slot == 8)) return false;
                    return true;
                }
                if (bonjour) {
                    Window local;
                    return sync.localDataWindow(now, true, local) && radio->awdlChannelAllowed(local.channel);
                }
                const int station3Depth =
                    (state.queue != IWX_INVALID_QUEUE ? radio->com.txq[state.queue].queued : 0) +
                    (state.dataQueueConfigured ? radio->com.txq[state.dataQueue].queued : 0) +
                    (state.llwQueueConfigured ? radio->com.txq[state.llwQueue].queued : 0);
                const bool station3Queued = station3Depth > 2;
                const bool llwRealtimeActive = state.llwRealtime || (llwActive && llwParamsEnabled);
                const uint64_t guardUS = state.listener && llwRealtimeActive && !station3Queued ? 500ULL :
                    (((entry.lowLatency || ptp) && state.listener) ? 1500ULL : 3000ULL);
                for (const auto &peer : radio->awdlPeers.peers) {
                    if ((multicast || sameAddress(peer.address, ethernet)) &&
                        sync.canSendTo(peer, now, multicast, state.listener, guardUS)) {
                        if (!multicast) {
                            Window local = {}, remote = {};
                            const uint64_t localRemain = !sync.window(now, local) ? 0 :
                                (state.listener ? local.untilChannelChangeUS : local.untilNextEAWUS);
                            const uint64_t remoteRemain = !peer.window(now, remote) ? 0 :
                                (state.listener ? remote.untilChannelChangeUS : remote.untilNextEAWUS);
                            uint64_t remainingUS = localRemain < remoteRemain ? localRemain : remoteRemain;
                            if (state.listener && now < state.dwell.windowEndUS &&
                                state.dwell.windowEndUS - now < remainingUS)
                                remainingUS = state.dwell.windowEndUS - now;
                            const uint64_t margin = useBlockAck && state.listener ? 6000 : guardUS;
                            if (remainingUS <= margin) return false;
                            transmitEndUS = now + remainingUS - margin;
                            if (useBlockAck && state.listener && tx.windowEndUS < transmitEndUS)
                                transmitEndUS = tx.windowEndUS;
                        }
                        return true;
                    }
                }
                return false;
            };
            if (!eligible()) {
                if (!multicast && !entry.lowLatency && !ptp && !markPeerBlocked(ethernet)) {
                    pushHead(entry);
                    break;
                }
                retry();
                continue;
            }
            Pending partner = {};
            size_t secondSize = 0;
            bool joining = false;
            if (!entry.second && !entry.retry && !entry.lowLatency && !ptp && canAggregate() && remaining && pendingCount) {
                partner = pending[pendingHead];
                secondSize = mbuf_pkthdr_len(partner.packet);
                joining = !partner.retry && !partner.second && !partner.lowLatency && !partner.multicastQueue &&
                    partner.epoch == entry.epoch && now >= partner.queuedUS &&
                    now - partner.queuedUS <= DataLifetimeUS && secondSize >= 54 &&
                    secondSize <= sizeof(secondEthernet) &&
                    !mbuf_copydata(partner.packet, 0, secondSize, secondEthernet) &&
                    sameAddress(ethernet, secondEthernet) && sameAddress(ethernet + 6, secondEthernet + 6) &&
                    be16(secondEthernet + 12) == 0x86dd && ipv6Payload(secondEthernet + 14, secondSize - 14) &&
                    !isUnicastPTP(secondEthernet, secondSize);
            } else if (entry.second) {
                secondSize = mbuf_pkthdr_len(entry.second);
                if (secondSize < 54 || secondSize > sizeof(secondEthernet) ||
                    mbuf_copydata(entry.second, 0, secondSize, secondEthernet)) {
                    txDropped += entry.count(); dataAbandoned += entry.count();
                    releaseData(entry); continue;
                }
            }
            const bool llwQos = entry.lowLatency && radio->awdlRadio.llwQueueConfigured;
            const size_t capacity = entry.second || joining ?
                26 + size + 16 + ((4 - ((size + 16) & 3)) & 3) + secondSize + 16 :
                (entry.lowLatency ? size + (llwQos ? 20 : 18) : size + 26);
            mbuf_t frame = awdlPacket(capacity);
            if (!frame) { pushHead(entry); break; }
            now = awdlNow();
            if (now < entry.queuedUS || now - entry.queuedUS > DataLifetimeUS ||
                (entry.second && (now < entry.secondQueuedUS || now - entry.secondQueuedUS > DataLifetimeUS))) {
                dataTxExpired += entry.count(); txDropped += entry.count();
                if (entry.retry) dataAbandoned += entry.count();
                mbuf_freem(frame); releaseData(entry);
                continue;
            }
            if (joining && (!canAggregate() || now < partner.queuedUS ||
                now - partner.queuedUS > DataLifetimeUS)) joining = false;
            if (entry.second && (!entry.retry || splitRetry()) && !canAggregate()) splitUnsubmitted();
            if (!eligible()) {
                mbuf_freem(frame);
                if (!multicast && !entry.lowLatency && !ptp && !markPeerBlocked(ethernet)) {
                    pushHead(entry);
                    break;
                }
                retry();
                continue;
            }
            if (!entry.retry) {
                entry.wifiSequence = sync.wifiSequence;
                entry.dataSequence = sync.dataSequence;
                entry.secondSequence = uint16_t(sync.dataSequence + 1);
            }
            const bool aggregate = entry.second || joining;
            auto bytes = static_cast<uint8_t *>(mbuf_data(frame));
            const bool useLLWQos = entry.lowLatency && radio->awdlRadio.llwQueueConfigured && capacity >= size + 20;
            size_t encoded = aggregate ? dataPairFromEthernet(ethernet, size,
                secondEthernet, secondSize, bytes, capacity, entry.wifiSequence,
                entry.dataSequence, entry.secondSequence) :
                (entry.lowLatency ? dataFromEthernetLLW(ethernet, size, bytes, capacity,
                    entry.wifiSequence, useLLWQos, RadioState::LLWTID) :
                 dataFromEthernet(ethernet, size, bytes, capacity, entry.wifiSequence, entry.dataSequence));
            if (!encoded && ptp && !aggregate && !entry.lowLatency &&
                unicastAddress(ethernet + 6) && capacity >= size + 26) {
                memset(bytes, 0, 40);
                bytes[0] = 0x08;
                copyAddress(bytes + 4, ethernet);
                copyAddress(bytes + 10, ethernet + 6);
                copyAddress(bytes + 16, BSSID);
                putLE16(bytes + 22, uint16_t((entry.wifiSequence & 0xfff) << 4));
                memcpy(bytes + 24, SNAP, 8);
                putLE16(bytes + 32, 0x0403);
                putLE16(bytes + 34, entry.dataSequence);
                putBE16(bytes + 38, be16(ethernet + 12));
                memcpy(bytes + 40, ethernet + 14, size - 14);
                encoded = size + 26;
            }
            if (!encoded) {
                mbuf_freem(frame);
                txDropped += entry.count();
                if (entry.retry) dataAbandoned += entry.count();
                releaseData(entry); continue;
            }
            mbuf_setlen(frame, encoded); mbuf_pkthdr_setlen(frame, encoded);
            if (entry.retry && !multicast) bytes[1] = 0x08;
            if (joining) {
                entry.second = partner.packet; entry.secondQueuedUS = partner.queuedUS;
                pending[pendingHead] = {}; pendingHead = (pendingHead + 1) % queueCapacity;
                --pendingCount; ++aggregateExtraOwned; --remaining;
            }
            unsigned slot = 0;
            while (inFlight[slot].token) ++slot;
            if (!++dataToken) ++dataToken;
            entry.group = multicast;
            inFlight[slot] = {entry, dataToken};
            inFlight[slot].entry.grant = radio->awdlRadio.dwellStarts;
            ++inFlightCount;
            const int error = radio->transmitAWDL(frame, dataToken, transmitEndUS);
            if (!error) {
                if (!multicast) {
                    recordNativeDataActivity(true, unsigned(size));
                    if (aggregate) recordNativeDataActivity(true, unsigned(secondSize));
                }
                dataSubmitted += entry.count();
                if (aggregate) ++dataAggregatesSubmitted;
                if (entry.retry) dataRetried += entry.count();
                else { ++sync.wifiSequence; sync.dataSequence += entry.count(); }
                continue;
            }
            inFlight[slot] = {}; --inFlightCount;
            if (error == EAGAIN && entry.second && !entry.retry) {
                Pending second = {entry.second, entry.secondQueuedUS, entry.epoch};
                entry.second = nullptr; --aggregateExtraOwned;
                if (!multicast && !entry.lowLatency && !ptp && markPeerBlocked(ethernet)) {
                    retry();
                    pending[(pendingHead + pendingCount) % queueCapacity] = second;
                    ++pendingCount;
                    ++keptCount;
                    continue;
                }
                pushHead(second); pushHead(entry);
                break;
            }
            if (error == EAGAIN && !multicast && !entry.lowLatency && !ptp && markPeerBlocked(ethernet)) {
                retry();
                continue;
            }
            if (error == ENOBUFS || error == ENOMEM || error == ENETDOWN || error == EAGAIN) {
                pushHead(entry); break;
            }
            txDropped += entry.count();
            if (entry.retry) dataAbandoned += entry.count();
            releaseData(entry);
        }
        restoreFIFO();
        const bool baAgreedAtTickEnd = radio->awdlRadio.blockAckTX.configured &&
            radio->awdlRadio.blockAckTX.phase == TXBlockAckQueue::Agreed;
        const unsigned signalCapacity = (!baAgreedAtTickEnd && queueCapacity > 96U) ? 96U : queueCapacity;
        if ((pendingCount + inFlightCount + aggregateExtraOwned < owned || outputCapacityReleased) &&
            pendingCount + inFlightCount + aggregateExtraOwned < signalCapacity && requested && !stopping && radio->awdlRadio.ready) {
            outputCapacityReleased = false;
            if (bsd && bsd->getIfnet()) bsd->signalOutputThread();
            if (llwActive && llwBSD && llwBSD->getIfnet()) llwBSD->signalOutputThread();
        }
    }
    if (!running) { serviceUnicast(); serviceBlockAck(); }
    publishNativeSyncState();
    const bool snapshotMediaActive = (llwActive && llwParamsEnabled) ||
        radio->awdlRadio.heavyReceiveActive(now) || radio->awdlRadio.heavyTransmitActive(now);
    const uint64_t snapshotIntervalUS = lastSnapshotUS && snapshotMediaActive ? 5000000ULL : 1000000ULL;
    if (now - lastSnapshotUS >= snapshotIntervalUS) {
        publishReceiveFilter();
#ifdef KERNEL
        radio->publishAWDLReceive();
#if DEBUG
        publishNativePeers(now);
#endif
#endif
        setProperty("InterfaceEnabled", interfaceEnabled);
        setProperty("SyncRequested", requested);
        setProperty("RadioReady", requested && !stopping && !regulatoryPlanBlocked &&
            !regulatoryStopRequired && radio->awdlRadio.ready);
        setProperty("RegulatoryPlanBlocked", regulatoryPlanBlocked);
        setProperty("BTCoexEnabled", radio->btCoex.enabled);
        setProperty("BTCoexRXMask", uint64_t(radio->btCoex.rxMask), 8);
        setProperty("BTCoexTXMask", uint64_t(radio->btCoex.txMask), 8);
        setProperty("BTCoexNVMRXMask", uint64_t(radio->btCoex.nvmRxMask), 8);
        setProperty("BTCoexNVMTXMask", uint64_t(radio->btCoex.nvmTxMask), 8);
        setProperty("BTCoexConfigVersion", uint64_t(radio->btCoex.configVersion), 8);
        setProperty("BTCoexCIVersion", uint64_t(radio->btCoex.ciVersion), 8);
        setProperty("BTCoexRLCVersion", uint64_t(radio->btCoex.rlcVersion), 8);
        setProperty("BTCoexMIMODisabled", radio->btCoex.mimoDisabled);
        setProperty("BTCoexTXSISODiversity", radio->btCoex.txSisoDiversity);
        setProperty("BTCoexModules", uint64_t(radio->btCoex.modules), 32);
        setProperty("BTCoexNotifications", radio->btCoex.notifications, 64);
        setProperty("BTCoexNotificationsDuringConfig", radio->btCoex.configNotifications, 64);
        setProperty("BTCoexMalformedNotifications", radio->btCoex.malformed, 64);
        setProperty("BTCoexActivity", uint64_t(radio->btCoex.activity), 32);
        setProperty("BTCoexCIUpdates", radio->btCoex.ciUpdates, 64);
        setProperty("BTCoexLastError", uint64_t(radio->btCoex.lastError), 32);
        const auto &btPolicy = radio->btCoex.policy;
        setProperty("BTCoexAssociated", btPolicy.associated);
        setProperty("BTCoexLinkEpoch", btPolicy.linkEpoch, 64);
        setProperty("BTCoexSMPSDesired", uint64_t(btPolicy.desired), 8);
        setProperty("BTCoexSMPSAcknowledged", uint64_t(btPolicy.acknowledged), 8);
        setProperty("BTCoexSMPSPending", btPolicy.actionToken != 0);
        setProperty("BTCoexSMPSAcks", btPolicy.actionAcks, 64);
        setProperty("BTCoexSMPSFailures", btPolicy.actionFailures, 64);
        setProperty("BTCoexSMPSLastError", uint64_t(radio->btCoex.smpsError), 32);
        setProperty("BTCoexPHYIdleChains", uint64_t(btPolicy.phyApplied.idle), 8);
        setProperty("BTCoexPHYActiveChains", uint64_t(btPolicy.phyApplied.active), 8);
        setProperty("BTCoexPHYValid", btPolicy.phyValid);
        setProperty("BTCoexPHYPending", btPolicy.phyToken != 0);
        setProperty("BTCoexPHYUpdates", btPolicy.phyUpdates, 64);
        setProperty("BTCoexPHYFailures", btPolicy.phyFailures, 64);
        setProperty("BTCoexPHYLastError", uint64_t(radio->btCoex.phyError), 32);
        setProperty("BTCoexReducedTXPower", btPolicy.powerApplied);
        setProperty("BTCoexTXPowerValid", btPolicy.powerValid);
        setProperty("BTCoexTXPowerPending", btPolicy.powerToken != 0);
        setProperty("BTCoexTXPowerUpdates", btPolicy.powerUpdates, 64);
        setProperty("BTCoexTXPowerFailures", btPolicy.powerFailures, 64);
        setProperty("BTCoexTXPowerLastError", uint64_t(radio->btCoex.powerError), 32);
        setProperty("TxSubmitted", radio->awdlRadio.submitted, 64);
        setProperty("TxCompleted", radio->awdlRadio.completed, 64);
        setProperty("TxFailed", radio->awdlRadio.failed, 64);
        setProperty("AWDLMinRateMbps", uint64_t(radio->awdlMinRateMbps), 8);
        uint64_t htPeers = 0;
        uint8_t htMcsMask = 0, htMcs2Mask = 0;
        uint16_t htCapMask = 0;
        for (const auto &peer : radio->awdlPeers.peers) {
            const uint8_t mask = peer.txHTMcs(now);
            htPeers += mask != 0;
            htMcsMask |= mask;
            htMcs2Mask |= peer.txHTMcs2(now);
            htCapMask |= peer.txHTCap(now);
        }
        setProperty("AWDLHTCapablePeers", htPeers, 64);
        setProperty("AWDLPeerHTMcsMask", uint64_t(htMcsMask), 8);
        setProperty("AWDLPeerHTMcs2Mask", uint64_t(htMcs2Mask), 8);
        setProperty("AWDLPeerHTCap", uint64_t(htCapMask), 16);
        setProperty("AWDLRateSamples", radio->awdlRateSamples, 64);
        setProperty("AWDLRateIgnored", radio->awdlRateIgnored, 64);
        setProperty("AWDLRateProbes", radio->awdlRateProbes, 64);
        setProperty("AWDLRateRaised", radio->awdlRateRaised, 64);
        setProperty("AWDLRateLowered", radio->awdlRateLowered, 64);
        setProperty("AWDLRateLastFlags", uint64_t(radio->awdlRateLastFlags), 32);
        setProperty("AWDLRateLastMediaUS", uint64_t(radio->awdlRateLastMediaUS), 16);
        setProperty("AWDLRateLastRetries", uint64_t(radio->awdlRateLastRetries), 8);
#if DEBUG
        radio->awdlBroadcastReports.publish(*this);
#endif
        setProperty("RxDelivered", rxDelivered, 64);
        setProperty("RxDropped", rxDropped, 64);
        setProperty("ActionFramesToSystem", actionDelivered, 64);
        setProperty("NativeDataPeerNotifications", nativeDataPeerNotifications, 64);
        setProperty("NativeDataPeerLockMisses", nativeDataPeerLockMisses, 64);
        setProperty("NativeSyncStateCalls", nativeSyncStateCalls, 64);
        setProperty("ActionFramesDropped", actionDropped, 64);
        setProperty("TxDropped", txDropped, 64);
        constexpr unsigned queueCapacity = sizeof(pending) / sizeof(pending[0]);
        unsigned pendingExtra = 0;
        for (unsigned i = 0; i < pendingCount; ++i)
            pendingExtra += pending[(pendingHead + i) % queueCapacity].second != nullptr;
        setProperty("DataTxPending", uint64_t(pendingCount + pendingExtra), 32);
        setProperty("DataTxInFlight", uint64_t(inFlightCount + aggregateExtraOwned - pendingExtra), 32);
        setProperty("DataTxAggregatesSubmitted", dataAggregatesSubmitted, 64);
        setProperty("RadioDataQueueConfigured", radio->awdlRadio.dataQueueConfigured);
        setProperty("RadioDataQueueStage", uint64_t(radio->awdlRadio.dataQueueSetup.stage), 32);
        setProperty("RadioDataQueueError", uint64_t(radio->awdlRadio.dataQueueSetup.error), 32);
        setProperty("MulticastCopiesQueued", multicastCopiesQueued, 64);
        setProperty("MulticastCopiesDropped", multicastCopiesDropped, 64);
        setProperty("BlockAckRequestsReceived", blockAckReceived, 64);
        setProperty("BlockAckRepliesQueued", blockAckQueued, 64);
        setProperty("BlockAckRequestsCoalesced", blockAckCoalesced, 64);
        setProperty("BlockAckRequestsRejected", blockAckRejected, 64);
        setProperty("BlockAckRepliesSubmitted", blockAckSubmitted, 64);
        setProperty("BlockAckRepliesAcknowledged", blockAckAcknowledged, 64);
        setProperty("BlockAckRepliesFailed", blockAckFailed, 64);
        setProperty("BlockAckRepliesExpired", blockAckExpired, 64);
        const auto &txBA = radio->awdlRadio.blockAckTX;
        setProperty("TXBlockAckPhase", uint64_t(txBA.phase), 32);
        setProperty("RadioWidthMHz", uint64_t(radio->awdlRadio.widthMHz), 32);
        setProperty("TXBlockAckWidthMHz", uint64_t(txBA.widthMHz), 32);
        setProperty("TXBlockAckChains", uint64_t(txBA.antenna), 32);
        setProperty("TXBlockAckMcs1", uint64_t(txBA.mcs), 32);
        setProperty("TXBlockAckMcs2", uint64_t(txBA.mcs2), 32);
        setProperty("TXBlockAckSGI", uint64_t(txBA.sgiMask), 8);
        setProperty("TXBlockAckFlags", uint64_t(txBA.tlcFlags), 16);
        setProperty("TXBlockAckTLCRateFlags", uint64_t(txBA.lastTLCRate), 32);
        setProperty("TXBlockAckTLCRateKbps", uint64_t(txBA.lastTLCRateKbps), 32);
        setProperty("TXBlockAckQueue", uint64_t(txBA.configured ? txBA.queue : IWX_INVALID_QUEUE), 32);
        setProperty("TXBlockAckPaused", txBA.paused);
        setProperty("TXBlockAckAgreements", txBA.agreements, 64);
        setProperty("TXBlockAckAcknowledged", txBA.acknowledged, 64);
        setProperty("TXBlockAckFailed", txBA.failed, 64);
        setProperty("TXBlockAckRecoveries", txBA.recoveries, 64);
        const auto &txBAStats = radio->awdlRadio.blockAckTXStats;
        setProperty("TXBlockAckRequestsSubmitted", txBAStats.requests, 64);
        setProperty("TXBlockAckRequestsAcknowledged", txBAStats.acknowledged, 64);
        setProperty("TXBlockAckRequestsUnacknowledged", txBAStats.notAcknowledged, 64);
        setProperty("TXBlockAckResponsesReceived", txBAStats.responses, 64);
        setProperty("TXBlockAckResponsesUnmatched", txBAStats.unmatched, 64);
        setProperty("TXBlockAckResponsesRefused", txBAStats.refusals, 64);
        setProperty("TXBlockAckTotalAgreements", txBAStats.agreements, 64);
        setProperty("TXBlockAckTotalAcknowledged", txBAStats.framesAcknowledged, 64);
        setProperty("TXBlockAckTotalFailed", txBAStats.framesFailed, 64);
        setProperty("TXBlockAckTotalFlushed", txBAStats.framesFlushed, 64);
        setProperty("TXBlockAckHandovers", txBAStats.handovers, 64);
        setProperty("TXBlockAckReceiveProbes", txBAStats.receiveProbes, 64);
        setProperty("TXBlockAckProbesDeferred", txBAStats.receiveProbesDeferred, 64);
        setProperty("TXBlockAckLastTLCRateFlags", uint64_t(txBAStats.lastTLCRate), 32);
        setProperty("TXBlockAckLastTLCRateKbps", uint64_t(txBAStats.lastTLCRateKbps), 32);
        setProperty("TXBlockAckPeakTLCRateFlags", uint64_t(txBAStats.peakTLCRate), 32);
        setProperty("TXBlockAckPeakTLCRateKbps", uint64_t(txBAStats.peakTLCRateKbps), 32);
        setProperty("TXBlockAckTLCConfigs", txBAStats.tlcConfigs, 64);
        setProperty("TXBlockAckInfraParks", txBAStats.infraParks, 64);
        setProperty("TXBlockAckSocialParks", txBAStats.socialParks, 64);
        setProperty("TXBlockAckRetireStalls", txBAStats.retireStalls, 64);
        setProperty("TXBlockAckRetireFailures", txBAStats.retireFailures, 64);
        setProperty("TXBlockAckRetireDelba", txBAStats.retireDelba, 64);
        setProperty("TXBlockAckRetireErrors", txBAStats.retireErrors, 64);
        setProperty("TXBlockAckLastFailStatus", uint64_t(txBAStats.lastFailStatus), 16);
        setProperty("AWDLDataRXLastRateFlags", uint64_t(radio->awdlRadio.lastRXRateFlags), 32);
        setProperty("AWDLDataRXLastRateKbps", uint64_t(radio->awdlRadio.lastRXRateKbps), 32);
        setProperty("AWDLDataRXPeakRateKbps", uint64_t(radio->awdlRadio.peakRXRateKbps), 32);
        const auto &fwHealth = radio->awdlRadio.fwHealth;
        setProperty("FWResetCount", fwHealth.resetCount, 64);
        setProperty("LastFWAssertID", uint64_t(fwHealth.lastAssertID), 32);
        setProperty("LastFWAssertPC", uint64_t(fwHealth.lastAssertPC), 32);
        setProperty("LastFWLastCmd", uint64_t(fwHealth.lastCmdID), 32);
        setProperty("LastFWUMACAssertID", uint64_t(fwHealth.lastUMACAssertID), 32);
        setProperty("LastFWResetChannel", uint64_t(fwHealth.lastResetChannel), 16);
        setProperty("LastFWResetWidthMHz", uint64_t(fwHealth.lastResetWidthMHz), 8);
        setProperty("AWDLWideFaultCount", uint64_t(fwHealth.wideFaultCount), 32);
        setProperty("AWDLWideCircuitTripped", radio->awdlRadio.wideCircuitTripped());
        setProperty("TXBlockAckResponsesOtherBSSID", txBAStats.responsesOtherBSSID, 64);
        setProperty("TXBlockAckLastResponseStatus", uint64_t(txBAStats.lastStatus), 16);
        setProperty("DataTxSubmitted", dataSubmitted, 64);
        setProperty("DataTxRetried", dataRetried, 64);
        setProperty("DataTxFlushed", dataFlushed, 64);
        setProperty("DataTxUnicastAcknowledged", dataUnicastAcknowledged, 64);
        setProperty("DataTxGroupCompleted", dataGroupCompleted, 64);
        setProperty("DataTxAbandoned", dataAbandoned, 64);
        setProperty("DataTxExpired", dataTxExpired, 64);
        setProperty("RadioChannel", uint64_t(radio->awdlRadio.channel), 16);
        setProperty("AWDLPHYReceiveConfigured", radio->awdlRadio.phyReceiveAntennas != 0 &&
            radio->awdlRadio.phyReceiveEpoch == radio->dmaEpoch);
        setProperty("AWDLPHYReceiveMask", uint64_t(radio->awdlRadio.phyReceiveAntennas), 8);
        setProperty("RadioStopPending", radio->awdlRadio.stopPending);
        setProperty("RadioFault", radio->awdlRadio.fault);
        setProperty("RadioCleanupError", uint64_t(radio->awdlRadio.cleanupError), 32);
        const auto &failure = radio->awdlRadio.failure;
        setProperty("RadioFailureStage", uint64_t(failure.stage), 32);
        setProperty("RadioFailureCommand", uint64_t(failure.command), 32);
        setProperty("RadioFailureStatus", uint64_t(failure.status), 32);
        setProperty("RadioFailureError", uint64_t(failure.error), 32);
        const auto &queue = radio->awdlRadio.queueSetup;
        setProperty("RadioQueueStage", uint64_t(queue.stage), 32);
        setProperty("RadioQueueError", uint64_t(queue.error), 32);
        setProperty("RadioQueueSize", uint64_t(queue.size), 32);
        setProperty("RadioQueuePacketIndex", uint64_t(queue.packetIndex), 32);
        setProperty("RadioQueueResponseLength", uint64_t(queue.responseLength), 32);
        setProperty("RadioQueueFlags", uint64_t(queue.responseFlags), 32);
        setProperty("RadioQueueID", uint64_t(queue.queueID), 32);
        setProperty("RadioQueueWritePointer", uint64_t(queue.writePointer), 32);
        setProperty("RadioQueueFirstDataQueue", uint64_t(queue.firstDataQueue), 32);
        setProperty("RadioQueueOwner", uint64_t(queue.owner), 32);
        const auto &session = radio->awdlRadio.session;
        setProperty("SessionNotifications", session.received, 64);
        setProperty("SessionMalformed", session.malformed, 64);
        setProperty("SessionOtherMAC", session.otherMAC, 64);
        setProperty("SessionUnexpectedStart", session.unexpectedStart, 64);
        setProperty("SessionInactive", session.inactive, 64);
        setProperty("SessionLastLength", uint64_t(session.lastLength), 32);
        setProperty("SessionLastMAC", uint64_t(session.lastMAC), 32);
        setProperty("SessionLastConfig", uint64_t(session.lastConfig), 32);
        setProperty("SessionLastStart", uint64_t(session.lastStart), 32);
        setProperty("SessionLastStatus", uint64_t(session.lastStatus), 32);
        setProperty("SessionLastEndConfig", uint64_t(session.lastEndConfig), 32);
        setProperty("SessionCancels", session.cancels, 64);
        setProperty("SessionCancelAcknowledged", session.cancelAcknowledged, 64);
        setProperty("SessionCancelCompletions", session.cancelCompletions, 64);
        setProperty("SessionWaits", session.waits, 64);
        setProperty("SessionWaitTimeouts", session.waitTimeouts, 64);
        setProperty("SessionDeferred", session.deferred, 64);
        setProperty("SessionAddUS", session.addUS, 64);
        setProperty("SessionStartUS", session.startUS, 64);
        setProperty("SessionCancelAckUS", session.cancelAckUS, 64);
        setProperty("SessionCancelCompletedUS", session.cancelCompletedUS, 64);
        setProperty("SessionFlushAckUS", session.flushAckUS, 64);
        setProperty("SessionEndUS", session.endUS, 64);
        setProperty("DwellRunning", radio->awdlRadio.dwell.running);
        setProperty("RadioPersistentListener", radio->awdlRadio.listener);
        setProperty("RadioListenerBound",
            radio->awdlRadio.listener && radio->awdlRadio.binding &&
            !AirportAWDL::idleUnboundOf(radio->awdlRadio));
        setProperty("RadioIdleUnbinds", AirportAWDL::idleUnbindsOf(radio->awdlRadio), 64);
        setProperty("RadioIdleRebinds", AirportAWDL::idleRebindsOf(radio->awdlRadio), 64);
        setProperty("RealtimeSessionMask",
            uint64_t(AirportAWDL::realtimeSessionMaskOf(radio->awdlRadio)), 32);
        setProperty("RadioListenerPromiscuous", radio->awdlRadio.listenerPromisc);
        setProperty("RadioListenerFilterFlags", uint64_t(radio->awdlRadio.listenerFilterFlags), 32);
        setProperty("DwellCancelling", radio->awdlRadio.dwell.cancelling);
        setProperty("DwellCancelAcknowledged", radio->awdlRadio.dwell.cancelAcknowledged);
        setProperty("DwellCancelCompleted", radio->awdlRadio.dwell.cancelCompleted);
        setProperty("DwellFlushed", radio->awdlRadio.dwell.flushed);
        setProperty("DwellEndReceived", radio->awdlRadio.dwell.done != 0);
        setProperty("DwellConfiguration", uint64_t(radio->awdlRadio.dwell.confID), 32);
        setProperty("DwellDurationTU", uint64_t(radio->awdlRadio.dwell.durationTU), 32);
        setProperty("DwellTXDeadlineUS", radio->awdlRadio.dwell.windowEndUS, 64);
        setProperty("DwellLogicalEndUS", radio->awdlRadio.dwell.eawEndUS, 64);
        const bool apTimeGrantActive = radio->awdlRadio.dwell.requested &&
            !radio->awdlRadio.dwell.done && !radio->awdlRadio.dwell.cancelCompleted;
        setProperty("APTimeCreditUS", apTimeGrantActive ? uint64_t(0) :
            radio->awdlRadio.availableAPCredit(now), 64);
        setProperty("APTimeGrantCreditUS", radio->awdlRadio.dwell.apCreditUS, 64);
        const uint64_t apDebtUS = radio->awdlRadio.apTime.creditUS < 0 ?
            uint64_t(-radio->awdlRadio.apTime.creditUS) : 0;
        const uint64_t apIdleUS = now > radio->awdlRadio.apTime.settledUS ?
            now - radio->awdlRadio.apTime.settledUS : 0;
        setProperty("APTimeDebtUS", apDebtUS > apIdleUS ? apDebtUS - apIdleUS : 0, 64);
        setProperty("APTimeSettledUS", radio->awdlRadio.apTime.settledUS, 64);
        setProperty("NextGrantAfterUS", radio->awdlRadio.nextGrantUS, 64);
        setProperty("DwellStarts", radio->awdlRadio.dwellStarts, 64);
        setProperty("DwellEnds", radio->awdlRadio.dwellEnds, 64);
        setProperty("DwellFailures", radio->awdlRadio.dwellFailures, 64);
        setProperty("NativeWindowOpen", nativeWindowOpen && !nativeWindowClosing);
        setProperty("NativeWindowStarts", nativeWindowStarts, 64);
#if DEBUG
        setProperty("RelayRecoveryWindowStarts", relayRecoveryWindowStarts, 64);
        setProperty("ParentRecoveryWindowStarts", parentRecoveryWindowStarts, 64);
#endif
        setProperty("NativeWindowEnds", nativeWindowEnds, 64);
        setProperty("NativeWindowErrors", nativeWindowErrors, 64);
        setProperty("UnicastSubmitted", unicastSubmitted, 64);
        setProperty("UnicastAcknowledged", unicastAcknowledged, 64);
        setProperty("UnicastFailed", unicastFailed, 64);
        setProperty("UnicastNotifyErrors", unicastNotifyErrors, 64);
        setProperty("BroadcastTemplateConfigured", broadcast.configured);
        setProperty("BroadcastChannelMap", uint64_t(broadcast.channelMap), 32);
        setProperty("BroadcastOffsetUS", uint64_t(broadcast.offsetUS), 32);
        setProperty("BroadcastSubmitted", broadcastSubmitted, 64);
        setProperty("ActionFrameTxMode", uint64_t(afTxMode), 32);
        setProperty("NativeElection", sync.nativeElection);
        setProperty("NativeAdvertiserClock", sync.nativeParent.valid);
        setProperty("SyncMode", uint64_t(sync.syncMode), 32);
        setProperty("SyncFlags", uint64_t(sync.syncFlags), 8);
        setProperty("SyncTransmitReady", sync.transmitReady(now));
        setProperty("LinkQualityGood", reportedLinkQuality == 100);
        setProperty("ServiceMergedFrames", serviceMerge.mergedFrames, 64);
        setProperty("ServiceMergedRecords", serviceMerge.mergedRecords, 64);
        setProperty("ServiceCacheResets", serviceMerge.resets, 64);
        const bool nativeLLW = llwInterface && llwInterface->isSkywalkNative();
        setProperty("LLWAttached", nativeLLW ? (llwInterface->getBSDInterface() != nullptr) :
            (llwBSD != nullptr && llwBSD->getIfnet() != nullptr));
        setProperty("LLWNativeSkywalk", nativeLLW);
        setProperty("LLWQueueConfigured", radio->awdlRadio.llwQueueConfigured);
        setProperty("LLWActive", llwActive);
        setProperty("LLWCreateRequests", llwCreateRequests, 64);
        setProperty("LLWDestroyRequests", llwDestroyRequests, 64);
        setProperty("LLWParamsGetCalls", llwParamsGetCalls, 64);
        setProperty("LLWParamsSetCalls", llwParamsSetCalls, 64);
        setProperty("LLWParamsEnabled", llwParamsEnabled);
        setProperty("LLWTxQueued", llwTxQueued, 64);
        setProperty("LLWRxDelivered", llwRxDelivered, 64);
        setProperty("LLWRxDropped", llwRxDropped, 64);
        setProperty("LLWNDPMirrored", llwNDPMirrored, 64);
        if (nativeLLW) {
            setProperty("LLWSkywalkTx", llwInterface->skywalkTxCount(), 64);
            setProperty("LLWSkywalkTxDropped", llwInterface->skywalkTxDropCount(), 64);
            setProperty("LLWSkywalkRx", llwInterface->skywalkRxCount(), 64);
            setProperty("LLWSkywalkRxDropped", llwInterface->skywalkRxDropCount(), 64);
        }
        lastSnapshotUS = now;
    }
    if (!stopping && timer) {
        unsigned delayMS = (running || radio->awdlRadio.stopPending ||
            (regulatoryResumePending && !regulatoryPlanBlocked)) ? 5 : 100;
        unsigned delayUS = 0;
        const auto &state = radio->awdlRadio;
        if (running && state.listener) {
            const uint64_t at = awdlNow();
            Window current = {};
            const bool haveWindow = sync.window(at, current);
            const int station3Depth =
                (state.queue != IWX_INVALID_QUEUE ? radio->com.txq[state.queue].queued : 0) +
                (state.dataQueueConfigured ? radio->com.txq[state.dataQueue].queued : 0) +
                (state.llwQueueConfigured ? radio->com.txq[state.llwQueue].queued : 0);
            const bool station3Queued = station3Depth > 2;
            const bool llwRealtimeActive = state.llwRealtime || (llwActive && llwParamsEnabled);
            const uint64_t openGuardUS = llwRealtimeActive ? (!station3Queued ? 500ULL : 1500ULL) : 3000ULL;
            if (state.dwell.running) {
                uint64_t deadline = state.dwell.windowEndUS;
                if (nativeWindowOpen && !nativeWindowClosing)
                    deadline = MIN(deadline, nativeWindowEndUS);
                else if (haveWindow && current.untilNextEAWUS)
                    deadline = MIN(deadline, at + current.untilNextEAWUS);
                if (state.blockAckTX.configured && !state.blockAckTX.paused)
                    deadline = MIN(deadline, state.blockAckTX.windowEndUS);
                const uint64_t remaining = deadline > at ? deadline - at : 0;
                delayMS = unsigned(MIN(uint64_t(delayMS), remaining > 1000 ? remaining / 1000 : 1));
                if (llwActive && llwParamsEnabled && remaining < 5000)
                    delayUS = unsigned(remaining > 200 ? remaining + 50 : 250);
                if (haveWindow && current.channel != state.channel &&
                    current.untilChannelChangeUS && current.untilChannelChangeUS <= 500ULL) {
                    const unsigned boundaryUS = unsigned(current.untilChannelChangeUS + 50);
                    delayMS = 1;
                    if (!delayUS || boundaryUS < delayUS)
                        delayUS = boundaryUS;
                }
                const bool txWaiting = pendingCount || inFlightCount ||
                    (state.blockAckTX.configured && state.blockAckTX.phase == TXBlockAckQueue::Agreed);
                if (haveWindow && current.sinceChannelStartUS < openGuardUS && txWaiting) {
                    const uint64_t guardRemain = openGuardUS - current.sinceChannelStartUS;
                    const unsigned guardMS = unsigned((guardRemain + 999) / 1000);
                    delayMS = unsigned(MIN(uint64_t(delayMS), uint64_t(guardMS ? guardMS : 1)));
                    if (!delayUS || guardRemain + 50 < delayUS)
                        delayUS = unsigned(guardRemain + 50);
                } else if (pendingCount && state.blockAckTX.configured &&
                           ((state.blockAckTX.phase == TXBlockAckQueue::Agreed && state.blockAckTX.paused) ||
                            state.blockAckTX.phase == TXBlockAckQueue::Enabling ||
                            (state.blockAckTX.negotiatingNow(at, state.dwell.windowEndUS) &&
                             !state.deferReceiveProbe(at)))) {
                    delayMS = 1;
                    delayUS = 0;
                }
            } else if (haveWindow && current.untilChannelChangeUS) {
                const unsigned remainMS = unsigned((current.untilChannelChangeUS + 999) / 1000);
                delayMS = unsigned(MIN(uint64_t(delayMS), uint64_t(remainMS ? remainMS : 1)));
                if (current.untilChannelChangeUS < 5000)
                    delayUS = unsigned(current.untilChannelChangeUS + 50);
            }
        }
        const bool useSubMS = delayUS && delayUS < delayMS * 1000U + 1000U;
        wakeArmed = useSubMS && delayUS <= 1000U;
        if (useSubMS)
            timer->setTimeoutUS(delayUS);
        else
            timer->setTimeoutMS(delayMS);
    }
    ticking = false;
    release();
}
IO80211VirtualInterface *AirportItlwm::createVirtualInterface(ether_addr *mac, unsigned role)
{
    if (role != APPLE80211_VIF_AWDL) return IO80211Controller::createVirtualInterface(mac, role);
    if (awdlInterface) return nullptr;
    auto interface = new AirportItlwmAWDLInterface;
    if (interface && !interface->init(this, mac, role, "awdl")) {
        interface->release(); return nullptr;
    }
    return interface;
}

SInt32 AirportItlwm::enableVirtualInterface(IO80211VirtualInterface *interface)
{
    auto awdl = OSDynamicCast(AirportItlwmAWDLInterface, interface);
    return awdl ? awdl->setInterfaceEnable(true) : IO80211Controller::enableVirtualInterface(interface);
}

SInt32 AirportItlwm::disableVirtualInterface(IO80211VirtualInterface *interface)
{
    auto awdl = OSDynamicCast(AirportItlwmAWDLInterface, interface);
    return awdl ? awdl->setInterfaceEnable(false) : IO80211Controller::disableVirtualInterface(interface);
}

bool AirportItlwm::startAWDLInterface()
{
    ether_addr mac;
    memcpy(mac.octet, fHalService->get80211Controller()->ic_myaddr, 6);
    mac.octet[0] = (mac.octet[0] & 0xfc) | 2;
    mac.octet[5] ^= 0x80;
    awdlInterface = static_cast<AirportItlwmAWDLInterface *>(createVirtualInterface(&mac, APPLE80211_VIF_AWDL));
    if (!awdlInterface) return false;
    IOSkywalkEthernetInterface::RegistrationInfo info = {};
    if (!awdlInterface->attach(this)) goto fail;
    awdlAttached = attachInterface(awdlInterface, this);
    if (!awdlAttached || !awdlInterface->initRegistrationInfo(&info, 1, sizeof(info))) goto fail;
    awdlInterface->mExpansionData->fRegistrationInfo =
        static_cast<IOSkywalkNetworkInterface::RegistrationInfo *>(IOMalloc(sizeof(info)));
    awdlInterface->mExpansionData2->fRegistrationInfo =
        static_cast<IOSkywalkEthernetInterface::RegistrationInfo *>(IOMalloc(sizeof(info)));
    if (!awdlInterface->mExpansionData->fRegistrationInfo || !awdlInterface->mExpansionData2->fRegistrationInfo)
        goto fail;
    memcpy(awdlInterface->mExpansionData->fRegistrationInfo, &info, sizeof(info));
    memcpy(awdlInterface->mExpansionData2->fRegistrationInfo, &info, sizeof(info));
    awdlInterface->setInterfaceId(2);
    awdlInterface->deferBSDAttach(true);
    awdlStarted = awdlInterface->start(this);
    if (!awdlStarted) goto fail;
    creatingAWDL = true;
    if (!IONetworkController::attachInterface(reinterpret_cast<IONetworkInterface **>(&awdlBSD), false)) {
        creatingAWDL = false;
        goto fail;
    }
    creatingAWDL = false;
    awdlInterface->bindBSD(awdlBSD);
    // Registration is deferred until startLLWInterface binds llw0, preventing
    // wifip2pd from issuing companion creation before llw0 is ready.
    return true;
fail:
    stopAWDLInterface();
    return false;
}

void AirportItlwm::stopAWDLInterface()
{
    if (awdlStarted) {
        awdlInterface->stop(this);
        awdlStarted = false;
    }
    if (awdlBSD) {
        awdlBSD->stopOutputThread();
        if (awdlInterface) awdlInterface->bindBSD(nullptr);
        IONetworkController::detachInterface(awdlBSD);
        OSSafeReleaseNULL(awdlBSD);
    }
    if (awdlInterface) {
        if (awdlAttached) detachInterface(awdlInterface, true);
        else if (awdlInterface->getProvider() == this) awdlInterface->detach(this);
        awdlAttached = false;
        OSSafeReleaseNULL(awdlInterface);
    }
}

bool AirportItlwm::startLLWInterface()
{
    if (!awdlInterface || llwInterface) return false;
    ether_addr mac = {};
    if (awdlInterface->getHardwareAddress(&mac) != kIOReturnSuccess) return false;
    llwInterface = new AirportItlwmLLWInterface;
    if (!llwInterface || !llwInterface->initWithAWDL(this, awdlInterface, &mac)) goto fail;
    if (!llwInterface->attach(this)) goto fail;
    llwAttached = attachInterface(llwInterface, this);
    if (!llwAttached) goto fail;
    llwInterface->setInterfaceId(3);
    llwInterface->setCompanionInterfaceId(2);
    llwInterface->deferBSDAttach(false);
    llwStarted = llwInterface->start(this);
    if (!llwStarted) goto fail;
    llwRegThreadCall = thread_call_allocate(completeLLWRegistrationCall, this);
    if (!llwRegThreadCall) goto fail;
    awdlInterface->setCompanionLowLatencySkywalkInterface(llwInterface);
    awdlInterface->bindLLW(llwInterface, nullptr);
    llwStopping = false;
    llwRegScheduled = false;
    // Defer llw0 Skywalk/BSD registration until awdl0 completes attachToDataLinkLayer
    // so awdl0 always owns the lower ifindex ahead of llw0.
    llwRegDeferred = true;
    return true;
fail:
    stopLLWInterface();
    return false;
}

IOReturn AirportItlwm::scheduleLLWRegistrationGated(OSObject *, void *arg0, void *, void *, void *)
{
    auto *self = static_cast<AirportItlwm *>(arg0);
    if (!self->llwRegDeferred || self->llwStopping || !self->llwRegThreadCall)
        return kIOReturnNotReady;
    self->llwRegDeferred = false;
    self->llwRegScheduled = true;
    self->retain();
    if (thread_call_enter(self->llwRegThreadCall))
        self->release();
    return kIOReturnSuccess;
}

bool AirportItlwm::scheduleLLWRegistration()
{
    if (_fCommandGate)
        return _fCommandGate->runAction(scheduleLLWRegistrationGated, this) == kIOReturnSuccess;
    return scheduleLLWRegistrationGated(nullptr, this, nullptr, nullptr, nullptr) == kIOReturnSuccess;
}

void AirportItlwm::completeLLWRegistrationCall(thread_call_param_t param0, thread_call_param_t)
{
    auto *self = static_cast<AirportItlwm *>(param0);
    if (!self) return;
    self->completeLLWRegistration();
    self->release();
}

void AirportItlwm::completeLLWRegistration()
{
    if (__atomic_load_n(&llwStopping, __ATOMIC_ACQUIRE) || !llwInterface || !awdlInterface)
        return;
    bool attached = false;
    if (llwInterface->registerSkywalk(getWorkLoop())) {
        attached = true;
        if (!__atomic_load_n(&llwStopping, __ATOMIC_ACQUIRE))
            llwInterface->registerService();
    } else if (!__atomic_load_n(&llwStopping, __ATOMIC_ACQUIRE)) {
        // Fallback to legacy Ethernet interface only if native Skywalk registration fails.
        IOSkywalkEthernetInterface::RegistrationInfo info = {};
        if (llwInterface->initRegistrationInfo(&info, 1, sizeof(info))) {
            const uint32_t subFamily = 3; // IFNET_SUBFAMILY_WIFI
            memcpy(info.bytes + 0x0c, &subFamily, sizeof(subFamily));
            llwInterface->mExpansionData->fRegistrationInfo =
                static_cast<IOSkywalkNetworkInterface::RegistrationInfo *>(IOMalloc(sizeof(info)));
            llwInterface->mExpansionData2->fRegistrationInfo =
                static_cast<IOSkywalkEthernetInterface::RegistrationInfo *>(IOMalloc(sizeof(info)));
            if (llwInterface->mExpansionData->fRegistrationInfo &&
                llwInterface->mExpansionData2->fRegistrationInfo) {
                memcpy(llwInterface->mExpansionData->fRegistrationInfo, &info, sizeof(info));
                memcpy(llwInterface->mExpansionData2->fRegistrationInfo, &info, sizeof(info));
                llwInterface->deferBSDAttach(true);
                creatingLLW = true;
                const bool ok = IONetworkController::attachInterface(
                    reinterpret_cast<IONetworkInterface **>(&llwBSD), false);
                creatingLLW = false;
                if (ok && llwBSD) {
                    awdlInterface->bindLLW(llwInterface, llwBSD);
                    if (!__atomic_load_n(&llwStopping, __ATOMIC_ACQUIRE))
                        llwBSD->registerService();
                    attached = true;
                }
            }
        }
    }
    if (!__atomic_load_n(&llwStopping, __ATOMIC_ACQUIRE)) {
        setProperty("LLWInterfaceAttached", attached);
        awdlInterface->registerService();
    }
}

void AirportItlwm::stopLLWInterface()
{
    thread_call_t call = nullptr;
    if (_fCommandGate) {
        _fCommandGate->runAction(
            [](OSObject *, void *arg0, void *arg1, void *, void *) -> IOReturn {
                auto *self = static_cast<AirportItlwm *>(arg0);
                auto *out = static_cast<thread_call_t *>(arg1);
                __atomic_store_n(&self->llwStopping, true, __ATOMIC_RELEASE);
                self->llwRegDeferred = false;
                self->llwRegScheduled = false;
                *out = self->llwRegThreadCall;
                self->llwRegThreadCall = nullptr;
                return kIOReturnSuccess;
            },
            this, &call);
    } else {
        __atomic_store_n(&llwStopping, true, __ATOMIC_RELEASE);
        llwRegDeferred = false;
        llwRegScheduled = false;
        call = llwRegThreadCall;
        llwRegThreadCall = nullptr;
    }
    if (call) {
        if (thread_call_cancel_wait(call))
            release();
        thread_call_free(call);
    }
    if (awdlInterface) {
        awdlInterface->bindLLW(nullptr, nullptr);
        awdlInterface->setCompanionLowLatencySkywalkInterface(nullptr);
    }
    if (llwInterface) {
        llwInterface->deregisterSkywalk();
    }
    if (llwStarted && llwInterface) {
        llwInterface->stop(this);
        llwStarted = false;
    }
    if (llwBSD) {
        llwBSD->stopOutputThread();
        IONetworkController::detachInterface(llwBSD);
        OSSafeReleaseNULL(llwBSD);
    }
    if (llwInterface) {
        if (llwAttached) detachInterface(llwInterface, true);
        else if (llwInterface->getProvider() == this) llwInterface->detach(this);
        llwAttached = false;
        OSSafeReleaseNULL(llwInterface);
    }
}
#endif
