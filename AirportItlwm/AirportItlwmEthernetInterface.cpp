//
//  AirportItlwmEthernetInterface.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "AirportItlwmEthernetInterface.hpp"

#include <sys/_if_ether.h>
#include <sys/sockio.h>
#include <net80211/ieee80211_var.h>
#ifdef AIRPORT_WCL
#include <IOKit/IOCommandGate.h>
#if __IO80211_TARGET == __MAC_15_2
#include "AirportItlwmV2.hpp"
#endif
#ifdef KERNEL
extern "C" errno_t ifnet_set_sndq_maxlen(ifnet_t interface, u_int32_t maxqlen);
#endif
extern IOCommandGate *_fCommandGate;
#endif

#define super IOEthernetInterface
OSDefineMetaClassAndStructors(AirportItlwmEthernetInterface, IOEthernetInterface);

const char *AirportItlwmEthernetInterface::getNamePrefix() const
{
#ifdef AIRPORT_WCL
    if (interface && interface->getInterfaceRole() == APPLE80211_VIF_AWDL) return "awdl";
#if __IO80211_TARGET == __MAC_15_2
    if (interface && interface->getInterfaceRole() == APPLE80211_VIF_LOW_LATENCY) return "llw";
#endif
#endif
    return super::getNamePrefix();
}

bool AirportItlwmEthernetInterface::
initWithSkywalkInterfaceAndProvider(IONetworkController *controller, IO80211SkywalkInterface *interface)
{
    // IOEthernetInterface::init queries getNamePrefix before publishing it.
    this->interface = interface;
    this->isAttach = false;
    bool ret = super::init(controller);
#ifdef AIRPORT_WCL
    const bool hiddenVirtual = interface &&
        (interface->getInterfaceRole() == APPLE80211_VIF_AWDL
#if __IO80211_TARGET == __MAC_15_2
         || interface->getInterfaceRole() == APPLE80211_VIF_LOW_LATENCY
#endif
        );
    if (ret && hiddenVirtual)
        ret = setProperty("HiddenInterface", kOSBooleanTrue);
#endif
    return ret;
}

#if defined(AIRPORT_WCL) && __IO80211_TARGET == __MAC_15_2
SInt32 AirportItlwmEthernetInterface::performCommand(IONetworkController *controller,
                                                  unsigned long command,
                                                  void *arg0, void *arg1)
{
    const int role = interface ? interface->getInterfaceRole() : 0;
    if (role == APPLE80211_VIF_AWDL || role == APPLE80211_VIF_LOW_LATENCY) {
        switch (command) {
        case SIOCSIFFLAGS: {
            if (!getIfnet()) return ENXIO;
            // Protocol attachment passes NULL after updating the ifnet flags.
            if (arg0 != getIfnet()) return EINVAL;
            if (!_fCommandGate) return ENXIO;
            const IOReturn result = _fCommandGate->runAction(syncAWDLFlagsGated, this);
            return result == kIOReturnSuccess ? 0 : errnoFromReturn(result);
        }
        // Keep virtual-interface MAC requests isolated from the primary infrastructure adapter.
        case SIOCSIFLLADDR: {
            if (!getIfnet()) return ENXIO;
            if (arg0 != getIfnet() || !arg1) return EINVAL;
            const auto *ifr = static_cast<const struct ifreq *>(arg1);
            if (ifr->ifr_addr.sa_len != ETHER_ADDR_LEN) return EINVAL;
            ether_addr current = {};
            if (interface->getHardwareAddress(&current) != kIOReturnSuccess) return ENXIO;
            return memcmp(current.octet, ifr->ifr_addr.sa_data, ETHER_ADDR_LEN) == 0 ? 0 : EOPNOTSUPP;
        }
        // AWDL/LLW group reception belongs to MAC1, not the infrastructure filter.
        case SIOCADDMULTI:
        case SIOCDELMULTI: {
            if (!getIfnet()) return ENXIO;
            // XNU group-change notifications pass NULL; the native handler reads the ifnet list.
            if (arg0 != getIfnet()) return EINVAL;
            if (role == APPLE80211_VIF_LOW_LATENCY) return 0;
            if (!_fCommandGate) return ENXIO;
            const IOReturn result = _fCommandGate->runAction(syncAWDLMulticastGated, this,
                reinterpret_cast<void *>(command), arg1);
            return result < 0 ? errnoFromReturn(result) : result;
        }
        // Sequoia's virtual-interface handler accepts 32- and 64-bit requests.
        case 0x802069c8UL:
        case 0x802869c8UL:
        case 0xc02069c9UL:
        case 0xc02869c9UL:
            if (!getIfnet()) return ENXIO;
            if (arg0 != getIfnet() || !arg1) return EINVAL;
            return interface->processBSDCommand(getIfnet(), static_cast<unsigned int>(command), arg1);
        }
    }
    return super::performCommand(controller, command, arg0, arg1);
}

IOReturn AirportItlwmEthernetInterface::syncAWDLFlagsGated(OSObject *, void *arg0,
                                                        void *, void *, void *)
{
    auto self = static_cast<AirportItlwmEthernetInterface *>(arg0);
    if (!self->getIfnet()) return kIOReturnNotReady;
    // The Ethernet superclass enables the shared controller, not the AWDL interface.
    // The radio timer reports initial link readiness to the native peer manager.
    return self->interface->setInterfaceEnable((ifnet_flags(self->getIfnet()) & IFF_UP) != 0);
}

IOReturn AirportItlwmEthernetInterface::syncAWDLMulticastGated(OSObject *, void *arg0,
                                                            void *arg1, void *arg2, void *)
{
    auto self = static_cast<AirportItlwmEthernetInterface *>(arg0);
    if (!self->getIfnet()) return ENXIO;
    return self->interface->processBSDCommand(self->getIfnet(),
        static_cast<unsigned int>(reinterpret_cast<uintptr_t>(arg1)), arg2);
}
#endif

#ifdef AIRPORT_WCL
bool AirportItlwmEthernetInterface::initIfnetParams(struct ifnet_init_params *params)
{
    if (!super::initIfnetParams(params))
        return false;
    if (interface && interface->getInterfaceRole() == APPLE80211_VIF_AWDL) {
        ether_addr address;
        if (interface->getHardwareAddress(&address) != kIOReturnSuccess)
            return false;
        // The superclass uses the controller MAC for every child's ifnet ID.
        // Give AWDL a separate identity before BSD allocates either interface.
        memcpy(awdlIfnetIdentity, "awdl", 4);
        memcpy(awdlIfnetIdentity + 4, &address, kIOEthernetAddressSize);
        params->uniqueid = awdlIfnetIdentity;
        params->uniqueid_len = sizeof(awdlIfnetIdentity);
    }
#if __IO80211_TARGET == __MAC_15_2
    else if (interface && interface->getInterfaceRole() == APPLE80211_VIF_LOW_LATENCY) {
        ether_addr address;
        if (interface->getHardwareAddress(&address) != kIOReturnSuccess)
            return false;
        memcpy(llwIfnetIdentity, "llw0", 4);
        memcpy(llwIfnetIdentity + 4, &address, kIOEthernetAddressSize);
        params->uniqueid = llwIfnetIdentity;
        params->uniqueid_len = sizeof(llwIfnetIdentity);
    }
#endif
    return true;
}

IOReturn AirportItlwmEthernetInterface::prepareBSDGated(OSObject *, void *arg0, void *, void *, void *)
{
    auto self = static_cast<AirportItlwmEthernetInterface *>(arg0);
    auto controller = OSDynamicCast(IO80211Controller, self->getController());
    auto queue = controller ? controller->getWorkQueue() : nullptr;
    if (!queue || !queue->inGate() || queue->onThread()) {
        return kIOReturnNotPermitted;
    }
    return self->interface->prepareBSDInterface(self->getIfnet(), 0);
}
#endif

IOReturn AirportItlwmEthernetInterface::
attachToDataLinkLayer( IOOptionBits options, void *parameter )
{
    XYLog("%s\n", __FUNCTION__);
    char infName[IFNAMSIZ];
#ifdef AIRPORT_WCL
    if (!setInterfaceSubType(3)) {
        return kIOReturnNotReady;
    }
#endif
    IOReturn ret = super::attachToDataLinkLayer(options, parameter);
    if (ret == kIOReturnSuccess && interface) {
        UInt8 builtIn = 0;
        IOEthernetAddress addr;
        interface->setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
        snprintf(infName, sizeof(infName), "%s%u", ifnet_name(getIfnet()), ifnet_unit(getIfnet()));
        interface->setProperty("IOInterfaceName", OSString::withCString(infName));
        interface->setProperty(kIOInterfaceUnit, OSNumber::withNumber(ifnet_unit(getIfnet()), 8));
        interface->setProperty(kIOInterfaceNamePrefix, OSString::withCString(ifnet_name(getIfnet())));
#ifdef AIRPORT_WCL
        IOReturn addressResult = interface->getHardwareAddress(reinterpret_cast<ether_addr *>(&addr));
#else
        IOReturn addressResult = OSDynamicCast(IOEthernetController, getController())->getHardwareAddress(&addr);
#endif
        if (addressResult == kIOReturnSuccess)
            setProperty(kIOMACAddress,  (void *) &addr,
                        kIOEthernetAddressSize);
#ifdef AIRPORT_WCL
        const bool virtualLink = interface->getInterfaceRole() == APPLE80211_VIF_AWDL
#if __IO80211_TARGET == __MAC_15_2
            || interface->getInterfaceRole() == APPLE80211_VIF_LOW_LATENCY
#endif
            ;
        if (virtualLink &&
            (addressResult != kIOReturnSuccess || ifnet_set_lladdr(getIfnet(), &addr, sizeof(addr)))) {
            super::detachFromDataLinkLayer(options, parameter);
            return kIOReturnError;
        }
#if __IO80211_TARGET == __MAC_15_2
        if (interface->getInterfaceRole() == APPLE80211_VIF_LOW_LATENCY) {
            ifnet_set_mtu(getIfnet(), 1500);
        }
#endif
#endif
#ifdef AIRPORT_WCL
        ret = _fCommandGate ? _fCommandGate->runAction(prepareBSDGated, this) : kIOReturnNotReady;
        if (ret != kIOReturnSuccess) {
            super::detachFromDataLinkLayer(options, parameter);
            isAttach = false;
            return ret;
        }
        isAttach = true;
#if defined(KERNEL) && __IO80211_TARGET == __MAC_15_2
        if (ifnet_t ifp = getIfnet()) {
            const int role = interface->getInterfaceRole();
            if (role == APPLE80211_VIF_AWDL)
                ifnet_set_sndq_maxlen(ifp, 512);
            else if (role != APPLE80211_VIF_LOW_LATENCY)
                ifnet_set_sndq_maxlen(ifp, 2048);
        }
#endif
#if __IO80211_TARGET == __MAC_15_2
        if (interface->getInterfaceRole() == APPLE80211_VIF_AWDL) {
            auto owner = OSDynamicCast(AirportItlwm, getController());
            if (owner && owner->scheduleLLWRegistration())
                return ret;
        }
#endif
        interface->registerService();
#else
        interface->registerService();
        interface->prepareBSDInterface(getIfnet(), 0);
#endif
//        ret = bpf_attach(getIfnet(), DLT_RAW, 0x48, &AirportItlwmEthernetInterface::bpfOutputPacket, &AirportItlwmEthernetInterface::bpfTap);
    }
#ifdef AIRPORT_WCL
    isAttach = ret == kIOReturnSuccess;
#else
    isAttach = true;
#endif
    return ret;
}

void AirportItlwmEthernetInterface::
detachFromDataLinkLayer(IOOptionBits options, void *parameter)
{
    super::detachFromDataLinkLayer(options, parameter);
    isAttach = false;
}

/**
 Add another hack to fake that the provider is IOSkywalkNetworkInterface, to avoid skywalkfamily instance cast panic.
 */
IOService *AirportItlwmEthernetInterface::
getProvider() const
{
    return isAttach ? this->interface : super::getProvider();
}

errno_t AirportItlwmEthernetInterface::
bpfOutputPacket(ifnet_t interface, u_int32_t data_link_type, mbuf_t packet)
{
    XYLog("%s data_link_type: %d\n", __FUNCTION__, data_link_type);
    AirportItlwmEthernetInterface *networkInterface = (AirportItlwmEthernetInterface *)ifnet_softc(interface);
    return networkInterface->enqueueOutputPacket(packet);
}

errno_t AirportItlwmEthernetInterface::
bpfTap(ifnet_t interface, u_int32_t data_link_type, bpf_tap_mode direction)
{
    XYLog("%s data_link_type: %d direction: %d\n", __FUNCTION__, data_link_type, direction);
    return 0;
}

bool AirportItlwmEthernetInterface::
setLinkState(IO80211LinkState state)
{
    if (state == kIO80211NetworkLinkUp) {
        ifnet_set_flags(getIfnet(), ifnet_flags(getIfnet()) | (IFF_UP | IFF_RUNNING), (IFF_UP | IFF_RUNNING));
    } else {
        ifnet_set_flags(getIfnet(), 0, IFF_UP | IFF_RUNNING);
    }
    return true;
}

extern const char* hexdump(uint8_t *buf, size_t len);

#ifdef AIRPORT_WCL
static mbuf_t normalizeInputPacket(mbuf_t packet, UInt32 length)
{
    const size_t packetLength = length ? length : mbuf_pkthdr_len(packet);
    if (!(mbuf_flags(packet) & MBUF_PKTHDR) || packetLength < ETHER_HDR_LEN) {
        mbuf_freem(packet);
        return NULL;
    }
    if (!mbuf_next(packet) && mbuf_len(packet) >= packetLength)
        return packet;

    unsigned int maxChunks = 1;
    mbuf_t linear = NULL;
    if (mbuf_allocpacket(MBUF_DONTWAIT, packetLength + ETHER_ALIGN, &maxChunks, &linear)) {
        mbuf_freem(packet);
        return NULL;
    }
    UInt8 *data = static_cast<UInt8 *>(mbuf_data(linear)) + ETHER_ALIGN;
    if (mbuf_copydata(packet, 0, packetLength, data) ||
        mbuf_copy_pkthdr(linear, packet) ||
        mbuf_setdata(linear, data, packetLength)) {
        mbuf_freem(linear);
        mbuf_freem(packet);
        return NULL;
    }
    mbuf_pkthdr_setlen(linear, packetLength);
    mbuf_pkthdr_setheader(linear, NULL);
    mbuf_freem(packet);
    return linear;
}
#endif

UInt32 AirportItlwmEthernetInterface::
inputPacket(mbuf_t packet, UInt32 length, IOOptionBits options, void *param)
{
#ifdef AIRPORT_WCL
    if (!packet)
        return 0;
    if (!getIfnet()) {
        mbuf_freem(packet);
        return 0;
    }
    const bool directInput = param == packet;
    packet = normalizeInputPacket(packet, length);
    if (!packet) {
        IONetworkData *data = getParameter(kIONetworkStatsKey);
        IONetworkStats *stats = data ? (IONetworkStats *)data->getBuffer() : NULL;
        if (stats)
            stats->inputErrors++;
        return 0;
    }
    if (directInput)
        param = packet;
#endif
#ifndef AIRPORT_WCL
    ether_header_t *eh;
    size_t len = mbuf_len(packet);
    
    eh = (ether_header_t *)mbuf_data(packet);

    if (len >= sizeof(ether_header_t) && eh->ether_type == htons(ETHERTYPE_PAE)) { // EAPOL packet
        const char* dump = hexdump((uint8_t*)mbuf_data(packet), len);
        XYLog("input EAPOL packet, len: %zu, data: %s\n", len, dump ? dump : "Failed to allocate memory");
        if (dump)
            IOFree((void*)dump, 3 * len + 1);
    }
#endif
    return IOEthernetInterface::inputPacket(packet, length, options, param);
}
