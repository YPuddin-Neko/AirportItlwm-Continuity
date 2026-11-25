// SPDX-License-Identifier: GPL-2.0-only
// macOS 15.8.1 (24H32): layout and method order verified against IO80211Family.
// Sonoma declarations consulted from maddog860/Sonoma-ReAWDL-Project,
// revision e5a4e793db6e281437db875252707b58147673e0.
#ifndef AIRPORT_MACOS15_VIRTUAL_INTERFACE_H
#define AIRPORT_MACOS15_VIRTUAL_INTERFACE_H
#include "IO80211SkywalkInterface.h"
class IO80211Controller;
class IO80211Buffer;
class IO80211BufferPool;
class IO80211VirtualInterface : public IO80211SkywalkInterface {
    OSDeclareAbstractStructors(IO80211VirtualInterface)
public:
    IO80211BufferPool *getMgmtFramePool();
    void bpfTapInput(IO80211Buffer *, unsigned int, void *, unsigned long);
    void setCompanionLowLatencySkywalkInterface(IO80211SkywalkInterface *);
    virtual void free() override;
    virtual IOReturn configureReport(IOReportChannelList*, unsigned int, void*, void*) override;
    virtual IOReturn updateReport(IOReportChannelList*, unsigned int, void*, void*) override;
    virtual bool start(IOService*) override;
    virtual void stop(IOService*) override;
    virtual bool terminate( IOOptionBits options = 0 ) override;
    virtual bool attach( IOService * provider ) override;
    virtual void detach( IOService * provider ) override;
    virtual IOReturn newUserClient(task*, void*, unsigned int, OSDictionary*, IOUserClient**) override;
    virtual const char* stringFromReturn(int) override;
    virtual IOReturn enable(unsigned int) override;
    virtual IOReturn prepareBSDInterface(struct __ifnet*, unsigned int) override;
    virtual IOReturn processBSDCommand(struct __ifnet*, unsigned int, void*) override;
    virtual SInt32 setInterfaceEnable(bool) override;
    virtual UInt32 getFeatureFlags() override;
    virtual bool bpfTap(unsigned int, unsigned int) override;
    virtual IOReturn setMulticastAddresses(ether_addr const*, unsigned int) override;
    virtual void * createPeerManager() override;
    virtual void postMessage(unsigned int, void*, unsigned long, bool) override;
    virtual void logTxPacket(IO80211NetworkPacket*, PacketSkywalkScratch*, apple80211_wme_ac, bool) override;
    virtual void logTxCompletionPacket(IO80211NetworkPacket*, PacketSkywalkScratch*, unsigned char*, apple80211_wme_ac, int, unsigned int, bool, bool) override;
    virtual IOReturn inputPacket(IO80211NetworkPacket*, packet_info_tag*, ether_header*, bool*, bool) override;
    virtual void logSkywalkTxReqPacket(IO80211NetworkPacket*, PacketSkywalkScratch*, unsigned char*, apple80211_wme_ac, bool) override;
    virtual SInt64 pendingPackets(unsigned char) override;
    virtual SInt64 packetSpace(unsigned char) override;
    virtual IO80211LinkState linkState() override;
    virtual void setScanningState(unsigned int, bool, apple80211_scan_data*, int) override;
    virtual void updateLinkParameters(apple80211_interface_availability*) override;
    virtual void updateInterfaceCoexRiskPct(unsigned long long) override;
    virtual void setInterfaceNF(apple80211_channel, long long) override;
    virtual void setDebugFlags(unsigned long long, unsigned int) override;
    virtual SInt64 debugFlags() override;
    virtual void setInterfaceChipCounters(apple80211_stat_report*, apple80211_chip_counters_tx*, apple80211_chip_error_counters_tx*, apple80211_chip_counters_rx*) override;
    virtual void setInterfaceMIBdot11(apple80211_stat_report*, apple80211_ManagementInformationBasedot11_counters*) override;
    virtual void setFrameStats(apple80211_stat_report*, apple80211_frame_counters*) override;
    virtual void setInfraSpecificFrameStats(apple80211_stat_report*, apple80211_infra_specific_stats*) override;
    virtual SInt64 getWmeTxCounters(unsigned long long*) override;
    virtual void setWoWEnabled(bool) override;
    virtual bool wowEnabled() override;
    virtual void printDataPath(userPrintCtx*) override;
private: virtual void _abi_0c68() __asm__("__ZN23IO80211VirtualInterface21findOrCreateFlowQueueE20IO80211FlowQueueHash"); public:
private: virtual void _abi_0c70() __asm__("__ZN23IO80211VirtualInterface30findOrCreateFlowQueueWithCacheE20IO80211FlowQueueHashPb"); public:
private: virtual void _abi_0c78() __asm__("__ZN23IO80211VirtualInterface21findExistingFlowQueueE20IO80211FlowQueueHash"); public:
private: virtual void _abi_0c80() __asm__("__ZN23IO80211VirtualInterface17removePacketQueueEPK20IO80211FlowQueueHash"); public:
    virtual void flushPacketQueues() override;
    virtual void cachePeer(ether_addr*, unsigned int*) override;
    virtual bool shouldLog(unsigned long long) override;
    virtual void vlogDebug(unsigned long long, char const*, va_list) override;
    virtual void vlogDebugBPF(unsigned long long, char const*, va_list) override;
    virtual void * getP2PSkywalkPeerMgr() override;
private: virtual void _abi_0cd0() __asm__("__ZN23IO80211VirtualInterface8findPeerER10ether_addr"); public:
    virtual int getEventPipeSize() override;
    virtual UInt64 createEventPipe(IO80211APIUserClient*) override;
    virtual bool init( IO80211Controller *, ether_addr *, UInt, char const * );
    virtual void setLinkState( IO80211LinkState, UInt );
    virtual void forwardPacket( IO80211NetworkPacket * );
    virtual void handleIoctl( ifnet_t, unsigned long, void * );
    virtual void addPeerToCache( unsigned char * );
    virtual void deletePeerFromCache( unsigned char * );
    virtual bool handleDebugCmd( apple80211_debug_command * );
private: virtual void _abi_0e98() __asm__("__ZN23IO80211VirtualInterface20getPeerSlotDataStatsEP10ether_addrbP17peerSlotDataStats"); public:
private: virtual void _abi_0ea0() __asm__("__ZN23IO80211VirtualInterface16getPeerDataStatsEP10ether_addrbP16packetCounters_t"); public:
private: virtual void _abi_0ea8() __asm__("__ZN23IO80211VirtualInterface18clearPeerDataStatsEP10ether_addrb"); public:
private: virtual void _abi_0eb0() __asm__("__ZN23IO80211VirtualInterface34setAwdlCurrentChannelSequenceIndexEj"); public:
    virtual bool isOutputFlowControlled( void );
    virtual void setOutputFlowControlled( void );
    virtual void clearOutputFlowControlled( void );
    virtual UInt32 configureAQMOutput( void );
    virtual bool attachToBpf( void );
    virtual bool configureIfnet( void );
    virtual void reset( void );
    virtual void dupAndTransmitMcastPacket( IO80211NetworkPacket *, unsigned char *, unsigned char * );
    virtual void freeMulticastPacket( IO80211NetworkPacket * );
    virtual void triggerAWDLMulticastTx( UInt, unsigned char );
    virtual void p2pMulticastTx( IO80211NetworkPacket **, UInt );
    virtual void sendPendingPacketsToStack( IO80211NetworkPacket **, UInt );
    virtual void freePendingPackets( IO80211NetworkPacket **, UInt );
    virtual void _RESERVEDIO80211VirtualInterface0();
    virtual void _RESERVEDIO80211VirtualInterface1();
    virtual void _RESERVEDIO80211VirtualInterface2();
    virtual void _RESERVEDIO80211VirtualInterface3();
    virtual void _RESERVEDIO80211VirtualInterface4();
    virtual void _RESERVEDIO80211VirtualInterface5();
    virtual void _RESERVEDIO80211VirtualInterface6();
    virtual void _RESERVEDIO80211VirtualInterface7();
    virtual void _RESERVEDIO80211VirtualInterface8();
    virtual void _RESERVEDIO80211VirtualInterface9();
    virtual void _RESERVEDIO80211VirtualInterface10();
    virtual void _RESERVEDIO80211VirtualInterface11();
    virtual void _RESERVEDIO80211VirtualInterface12();
    virtual void _RESERVEDIO80211VirtualInterface13();
    virtual void _RESERVEDIO80211VirtualInterface14();
    virtual void _RESERVEDIO80211VirtualInterface15();
private:
    uint8_t _virtualLayout[0x120 - sizeof(IO80211SkywalkInterface)];
};
static_assert(sizeof(IO80211VirtualInterface) == 0x120, "Sequoia virtual interface ABI");
#endif
