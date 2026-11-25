#include "../../bluetooth/BlueToolFixup/IntelPrivacyController.hpp"

#include <assert.h>
#include <atomic>
#include <condition_variable>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <mutex>
#include <pthread.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

namespace {
struct USBContext {
    uint8_t prefix[0x14];
    uint16_t vendor;
    uint16_t product;
};
struct NativeTable {
    uintptr_t thread;
    uintptr_t devices[10];
    int32_t count;
};
static_assert(offsetof(USBContext, vendor) == 0x14, "Native USB vendor offset");
static_assert(offsetof(USBContext, product) == 0x16, "Native USB product offset");
static_assert(offsetof(NativeTable, devices) == 8, "Native table first slot");
static_assert(offsetof(NativeTable, count) == 0x58, "Native table count offset");

NativeTable table {};
alignas(uintptr_t) uintptr_t active;
pthread_mutex_t nativeMutex = PTHREAD_MUTEX_INITIALIZER;
std::atomic<unsigned> attempts {0}, acquisitions {0}, releases {0};
std::mutex pauseMutex;
std::condition_variable pauseChanged;
bool pauseOnAcquisition = false, acquiredAndPaused = false, continueQuery = false;

int tryLock(void *mutex) {
    assert(mutex == &nativeMutex);
    ++attempts;
    const int result = pthread_mutex_trylock(&nativeMutex);
    if (!result) {
        ++acquisitions;
        std::unique_lock<std::mutex> guard(pauseMutex);
        if (pauseOnAcquisition) {
            acquiredAndPaused = true;
            pauseChanged.notify_all();
            pauseChanged.wait(guard, [] { return continueQuery; });
        }
    }
    return result;
}

int unlock(void *mutex) {
    assert(mutex == &nativeMutex);
    ++releases;
    return pthread_mutex_unlock(&nativeMutex);
}

template <typename T> int64_t relative(T pointer) {
    return reinterpret_cast<intptr_t>(pointer) -
           reinterpret_cast<intptr_t>(&intelPrivacyControllerConfig);
}

IntelPrivacy::ControllerConfig configuration() {
    return {relative(tryLock), relative(unlock), relative(&nativeMutex),
            relative(&active), relative(table.devices), relative(&table.count)};
}

void reset() {
    assert(acquisitions == releases);
    table = {};
    __atomic_store_n(&active, 0, __ATOMIC_RELAXED);
    attempts = acquisitions = releases = 0;
}

void select(uintptr_t address, unsigned slot = 0) {
    assert(slot < 10);
    table.devices[slot] = address;
    table.count = static_cast<int32_t>(slot + 1);
    __atomic_store_n(&active, address, __ATOMIC_RELAXED);
}

USBContext *allocateContext() {
    auto *context = static_cast<USBContext *>(calloc(1, sizeof(USBContext)));
    assert(context);
    context->vendor = 0x8087;
    context->product = 0x0aaa;
    return context;
}

void concurrentRemoval() {
    reset();
    USBContext *context = allocateContext();
    select(reinterpret_cast<uintptr_t>(context));
    {
        std::lock_guard<std::mutex> guard(pauseMutex);
        pauseOnAcquisition = true;
        acquiredAndPaused = continueQuery = false;
    }
    uint32_t answer = 0;
    std::thread query([&] { answer = intelPrivacyActiveController(); });
    {
        std::unique_lock<std::mutex> guard(pauseMutex);
        pauseChanged.wait(guard, [] { return acquiredAndPaused; });
    }
    std::atomic<bool> removing {false}, removed {false};
    std::thread removal([&] {
        removing.store(true, std::memory_order_release);
        assert(!pthread_mutex_lock(&nativeMutex));
        table.devices[0] = 0;
        table.count = 0;
        assert(!pthread_mutex_unlock(&nativeMutex));
        // The native remover may clear active outside the table mutex.
        __atomic_store_n(&active, 0, __ATOMIC_RELAXED);
        free(context);
        removed.store(true, std::memory_order_release);
    });
    while (!removing.load(std::memory_order_acquire))
        std::this_thread::yield();
    assert(!removed.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> guard(pauseMutex);
        continueQuery = true;
        pauseOnAcquisition = false;
    }
    pauseChanged.notify_all();
    query.join();
    removal.join();
    assert(answer == 1 && removed && intelPrivacyActiveController() == 0);

    std::atomic<bool> finished {false};
    std::atomic<unsigned> calls {0};
    std::thread reader([&] {
        do {
            assert(intelPrivacyActiveController() <= 1);
            ++calls;
        } while (!finished.load(std::memory_order_acquire));
    });
    for (unsigned i = 0; i != 1000; ++i) {
        context = allocateContext();
        assert(!pthread_mutex_lock(&nativeMutex));
        select(reinterpret_cast<uintptr_t>(context));
        assert(!pthread_mutex_unlock(&nativeMutex));
        std::this_thread::yield();
        assert(!pthread_mutex_lock(&nativeMutex));
        table.devices[0] = 0;
        table.count = 0;
        assert(!pthread_mutex_unlock(&nativeMutex));
        __atomic_store_n(&active, 0, __ATOMIC_RELAXED);
        free(context);
    }
    finished.store(true, std::memory_order_release);
    reader.join();
    assert(calls && acquisitions == releases);
}
} // namespace

extern "C" const IntelPrivacy::ControllerConfig intelPrivacyControllerConfig = configuration();

int main() {
    unsigned cases = 0;
    const auto pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void *inaccessible = mmap(nullptr, pageSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(inaccessible != MAP_FAILED);
    const auto invalid = reinterpret_cast<uintptr_t>(inaccessible);
    USBContext valid {};
    valid.vendor = 0x8087;
    valid.product = 0x0aaa;

    reset();
    assert(intelPrivacyActiveController() == 0 && attempts == 1 && releases == 1);
    ++cases;

    reset(); select(reinterpret_cast<uintptr_t>(&valid));
    assert(intelPrivacyActiveController() == 1 && acquisitions == releases);
    ++cases;

    reset(); select(invalid);
    assert(!pthread_mutex_lock(&nativeMutex));
    assert(intelPrivacyActiveController() == 0 && attempts == 1 && releases == 0);
    assert(!pthread_mutex_unlock(&nativeMutex));
    ++cases;

    reset(); select(reinterpret_cast<uintptr_t>(&valid));
    __atomic_store_n(&active, invalid, __ATOMIC_RELAXED);
    assert(intelPrivacyActiveController() == 0 && acquisitions == releases);
    ++cases;

    reset(); select(reinterpret_cast<uintptr_t>(&valid));
    valid.vendor = 0x0a5c;
    assert(intelPrivacyActiveController() == 0);
    valid.vendor = 0x8087; valid.product = 0x0026;
    assert(intelPrivacyActiveController() == 0);
    valid.product = 0x0aaa;
    ++cases;

    for (int32_t count : {-1, 0, 11, INT32_MAX}) {
        reset(); select(invalid); table.count = count;
        assert(intelPrivacyActiveController() == 0 && acquisitions == releases);
    }
    ++cases;

    reset(); select(reinterpret_cast<uintptr_t>(&valid), 9);
    assert(intelPrivacyActiveController() == 1);
    table.count = 9;
    assert(intelPrivacyActiveController() == 0);
    ++cases;

    reset();
    USBContext *removed = allocateContext();
    select(reinterpret_cast<uintptr_t>(removed));
    table.devices[0] = 0; table.count = 0;
    free(removed);
    // A stale active token must never authorize a dereference after unlink/free.
    assert(intelPrivacyActiveController() == 0);
    ++cases;

    reset(); select(invalid);
    __atomic_store_n(&active, 0, __ATOMIC_RELAXED);
    assert(intelPrivacyActiveController() == 0);
    ++cases;

    concurrentRemoval();
    ++cases;
    reset();
    assert(!munmap(inaccessible, pageSize));
    assert(!pthread_mutex_destroy(&nativeMutex));
    printf("IntelPrivacyController: %u lifecycle cases passed\n", cases);
}
