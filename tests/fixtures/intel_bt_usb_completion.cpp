#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

using std::min;
using IOReturn = int;
using AbsoluteTime = uint64_t;
constexpr int kIOReturnSuccess = 0, kIOReturnTimeout = -1, kIOReturnError = -2;
constexpr int kIOReturnNotResponding = -3, kIOUSBPipeStalled = -4, kIOReturnAborted = -5;
constexpr int THREAD_AWAKENED = 0, THREAD_INTERRUPTIBLE = 1, kMillisecondScale = 1;
constexpr int kIODirectionIn = 1;
using Clock = std::chrono::steady_clock;
struct IOLock { std::mutex mutex; std::condition_variable condition; };
static unsigned sleeps;
void IOLockLock(IOLock *lock) { lock->mutex.lock(); }
void IOLockUnlock(IOLock *lock) { lock->mutex.unlock(); }
void IOLockWakeup(IOLock *lock, void *, bool) { lock->condition.notify_all(); }
void IOLockFree(IOLock *lock) { delete lock; }
void clock_interval_to_deadline(uint32_t ms, int, uint64_t *deadline) {
    *deadline = std::chrono::duration_cast<std::chrono::nanoseconds>(
        (Clock::now() + std::chrono::milliseconds(ms)).time_since_epoch()).count();
}
int IOLockSleepDeadline(IOLock *lock, void *, AbsoluteTime deadline, int) {
    ++sleeps;
    std::unique_lock<std::mutex> held(lock->mutex, std::adopt_lock);
    auto status = lock->condition.wait_until(held, Clock::time_point(std::chrono::nanoseconds(deadline)));
    held.release();
    return status == std::cv_status::timeout ? 1 : THREAD_AWAKENED;
}
struct OSObject { virtual ~OSObject() = default; virtual void free() {} };
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
#define OSSafeReleaseNULL(value) do { if (value) { (value)->release(); (value) = nullptr; } } while (0)
#define XYLog(...) do {} while (0)
#define super OSObject
#include "response.hpp"
struct IOUSBHostCompletion {
    void (*action)(void *, void *, IOReturn, uint32_t);
    void *owner;
    void *parameter;
};
struct IOUSBHostIOSource { enum { kAbortAsynchronous = 0, kAbortSynchronous = 1 }; };
struct Buffer {
    uint8_t bytes[64] = {1, 2, 3, 4};
    std::function<void()> beforeRelease;
    size_t getLength() { return sizeof(bytes); }
    const void *getBytesNoCopy() { return bytes; }
    void complete(int) {}
    void release() { if (beforeRelease) beforeRelease(); delete this; }
};
enum class Mode { Early, Deferred, Error, SubmitError, Timeout, Spurious, Stalled };
struct Pipe {
    Mode mode = Mode::Early;
    IOUSBHostCompletion completion {};
    std::mutex mutex;
    bool pending = false, drained = false;
    unsigned submitted = 0, abortOptions = 0;
    std::thread worker;
    IOLock *lock;
    explicit Pipe(IOLock *value) : lock(value) {}
    void deliver(IOReturn status) {
        IOUSBHostCompletion callback;
        {
            std::lock_guard<std::mutex> held(mutex);
            if (!pending) return;
            pending = false;
            callback = completion;
        }
        callback.action(callback.owner, callback.parameter, status, status ? 0 : 4);
    }
    IOReturn io(Buffer *, uint32_t, IOUSBHostCompletion *callback, uint32_t timeout) {
        assert(timeout == 0);
        ++submitted;
        if (mode == Mode::SubmitError) return kIOReturnError;
        if (mode == Mode::Stalled && submitted == 1) return kIOUSBPipeStalled;
        completion = *callback;
        pending = true;
        if (mode == Mode::Early || mode == Mode::Stalled) deliver(kIOReturnSuccess);
        else if (mode == Mode::Error) deliver(kIOReturnNotResponding);
        else if (mode == Mode::Deferred || mode == Mode::Spurious) {
            worker = std::thread([this] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (mode == Mode::Spurious) {
                    IOLockLock(lock);
                    IOLockWakeup(lock, nullptr, true);
                    IOLockUnlock(lock);
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                deliver(kIOReturnSuccess);
            });
        }
        return kIOReturnSuccess;
    }
    IOReturn abort(unsigned options = 0) {
        abortOptions = options;
        if (options != IOUSBHostIOSource::kAbortSynchronous) return kIOReturnSuccess;
        // Cancellation completes before synchronous abort returns, while the read's
        // completion context and the shared buffer are still owned by the caller.
        deliver(kIOReturnAborted);
        if (worker.joinable()) worker.join();
        drained = true;
        return kIOReturnSuccess;
    }
    void clearStall(bool) {}
    void release() { assert(!pending); if (worker.joinable()) worker.join(); delete this; }
};
struct Interface {
    bool isOpen(void *) { return false; }
    void close(void *) {}
};
struct USBDeviceController : OSObject {
    Pipe *m_pBulkWritePipe = nullptr, *m_pBulkReadPipe = nullptr, *m_pInterruptReadPipe;
    Buffer *mReadBuffer = new Buffer;
    IOLock *_hciLock = new IOLock;
    Interface *m_pInterface = nullptr;
    void *m_pClient = nullptr, *m_pDevice = nullptr;
    USBDeviceController() { m_pInterruptReadPipe = new Pipe(_hciLock); }
    void free() override;
    static void interruptHandler(void *, void *, IOReturn, uint32_t);
    IOReturn interruptPipeRead(void *, uint32_t, uint32_t *, uint32_t);
    const char *stringFromReturn(IOReturn) { return "result"; }
};
#include "production.hpp"

void readCase(Mode mode, IOReturn expected, bool waits) {
    USBDeviceController controller;
    controller.m_pInterruptReadPipe->mode = mode;
    uint8_t bytes[8] = {};
    uint32_t length = 99;
    sleeps = 0;
    auto result = controller.interruptPipeRead(bytes, sizeof(bytes), &length, mode == Mode::Timeout ? 2 : 100);
    assert(result == expected);
    assert((sleeps > 0) == waits);
    if (result == kIOReturnSuccess) {
        assert(length == 4 && bytes[0] == 1 && bytes[3] == 4);
    } else {
        assert(length == 0 && bytes[0] == 0);
    }
    if (mode == Mode::Timeout) {
        assert(controller.m_pInterruptReadPipe->abortOptions == IOUSBHostIOSource::kAbortSynchronous);
        assert(controller.m_pInterruptReadPipe->drained);
        controller.m_pInterruptReadPipe->mode = Mode::Early;
        assert(controller.interruptPipeRead(bytes, sizeof(bytes), &length, 100) == kIOReturnSuccess);
        assert(length == 4);
    }
    controller.free();
}

int main() {
    readCase(Mode::Early, kIOReturnSuccess, false);
    readCase(Mode::Deferred, kIOReturnSuccess, true);
    readCase(Mode::Error, kIOReturnNotResponding, false);
    readCase(Mode::SubmitError, kIOReturnError, false);
    readCase(Mode::Timeout, kIOReturnTimeout, true);
    readCase(Mode::Spurious, kIOReturnSuccess, true);
    readCase(Mode::Stalled, kIOReturnSuccess, false);
    USBDeviceController controller;
    InterruptResp response {};
    controller.m_pInterruptReadPipe->completion = {USBDeviceController::interruptHandler, &controller, &response};
    controller.m_pInterruptReadPipe->pending = true;
    controller.mReadBuffer->beforeRelease = [&] { assert(response.completed && response.status == kIOReturnAborted); };
    controller.free();
    std::puts("8 completion cases passed");
}
