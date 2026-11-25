// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef IOSkywalkSequoiaQueues_h
#define IOSkywalkSequoiaQueues_h

#include "Airport/IOSkywalkPacketBufferPool.h"
#include <IOKit/IOEventSource.h>
#include <IOKit/IOReturn.h>
#include <libkern/c++/OSObject.h>
#include <stdint.h>

class IOSkywalkPacket;
class IOSkywalkNetworkPacket;
class IOSkywalkPacketQueue;
class IOSkywalkTxSubmissionQueue;
class IOSkywalkTxCompletionQueue;
class IOSkywalkRxCompletionQueue;

enum {
    kIOSkywalkPacketTypeGeneric = 0,
    kIOSkywalkPacketTypeNetwork = 1,
    kIOSkywalkPacketTypeCloneable = 2
};

static_assert(sizeof(IOSkywalkPacketBufferPool::PoolOptions) == 32, "Sequoia PoolOptions size");

class IOSkywalkPacket : public OSObject {
    OSDeclareAbstractStructors(IOSkywalkPacket)
public:
    IOReturn prepare(IOSkywalkPacketQueue *queue, uint64_t options = 0, unsigned int flags = 0);
    void *getDataVirtualAddress() const;
    unsigned int getDataLength() const;
    unsigned short getDataOffset() const;
    unsigned int getBufferSize() const;
    IOReturn setDataOffsetAndLength(unsigned short offset, unsigned int length);
};

class IOSkywalkNetworkPacket : public IOSkywalkPacket {
    OSDeclareAbstractStructors(IOSkywalkNetworkPacket)
public:
    IOReturn setLinkHeaderLength(unsigned char length);
};

class IOSkywalkPacketQueue : public IOEventSource {
    OSDeclareAbstractStructors(IOSkywalkPacketQueue)
public:
    bool isQueueReady() const;
};

class IOSkywalkTxSubmissionQueue : public IOSkywalkPacketQueue {
    OSDeclareDefaultStructors(IOSkywalkTxSubmissionQueue)
public:
    typedef unsigned int (*Action)(OSObject *owner, IOSkywalkTxSubmissionQueue *queue,
                                   IOSkywalkPacket * const *packets, unsigned int packetCount,
                                   void *refCon);
    static IOSkywalkTxSubmissionQueue *withPool(IOSkywalkPacketBufferPool *pool,
                                                unsigned int capacity,
                                                unsigned int queueId,
                                                OSObject *owner,
                                                Action action,
                                                void *refCon,
                                                unsigned int options);
};

class IOSkywalkTxCompletionQueue : public IOSkywalkPacketQueue {
    OSDeclareDefaultStructors(IOSkywalkTxCompletionQueue)
public:
    typedef unsigned int (*Action)(OSObject *owner, IOSkywalkTxCompletionQueue *queue,
                                   IOSkywalkPacket **packets, unsigned int packetCount,
                                   void *refCon);
    static IOSkywalkTxCompletionQueue *withPool(IOSkywalkPacketBufferPool *pool,
                                                unsigned int capacity,
                                                unsigned int queueId,
                                                OSObject *owner,
                                                Action action,
                                                void *refCon,
                                                unsigned int options);
    IOReturn enqueuePackets(IOSkywalkPacket * const *packets, unsigned int packetCount,
                            unsigned int options);
};

class IOSkywalkRxCompletionQueue : public IOSkywalkPacketQueue {
    OSDeclareDefaultStructors(IOSkywalkRxCompletionQueue)
public:
    typedef unsigned int (*Action)(OSObject *owner, IOSkywalkRxCompletionQueue *queue,
                                   IOSkywalkPacket **packets, unsigned int packetCount,
                                   void *refCon);
    static IOSkywalkRxCompletionQueue *withPool(IOSkywalkPacketBufferPool *pool,
                                                unsigned int capacity,
                                                unsigned int queueId,
                                                OSObject *owner,
                                                Action action,
                                                void *refCon,
                                                unsigned int options);
    IOReturn enqueuePackets(IOSkywalkPacket * const *packets, unsigned int packetCount,
                            unsigned int options);
};

#endif
