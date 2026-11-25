// SPDX-License-Identifier: GPL-2.0-or-later
// Non-owning declarations for the 24H32 management-frame pool.
#ifndef AIRPORT_MACOS15_MANAGEMENT_BUFFER_H
#define AIRPORT_MACOS15_MANAGEMENT_BUFFER_H
#include <libkern/c++/OSObject.h>

class IO80211Buffer {
public:
    void *getBytesNoCopy();
    size_t getCapacity() const;
    void setLength(size_t);
    void returnBuffer();
};

class IO80211BufferPool : public OSObject {
    OSDeclareAbstractStructors(IO80211BufferPool)
public:
    // First slot after OSObject, at 0x118. The concrete pool owns this buffer.
    virtual IO80211Buffer *getBuffer();
};
#endif
