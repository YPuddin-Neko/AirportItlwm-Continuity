// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef IWX_TX_QUEUE_SETUP_HPP
#define IWX_TX_QUEUE_SETUP_HPP
#include <stdint.h>

struct IwxTxQueueSetup {
    enum Stage : uint32_t {
        None, Validate, AllocateRing, AllocateDescriptors, AllocateByteCounts,
        AllocateCommands, AllocatePacketMap, Submit, ResponseHeader,
        ResponseLength, ResponseFlags, QueueIndex, QueueReserved, QueueBusy, Ready
    };
    enum Owner : uint32_t { Reserved = 1, Infrastructure = 2, AWDL = 4, Pending = 8 };
    Stage stage;
    int error;
    uint32_t size, packetIndex, responseLength, responseFlags, queueID, writePointer;
    uint32_t firstDataQueue, owner;
};
#endif
