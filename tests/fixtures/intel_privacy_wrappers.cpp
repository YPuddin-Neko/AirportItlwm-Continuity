#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>

namespace {
std::vector<unsigned> events;
void *const instance = reinterpret_cast<void *>(0x12345678);
void *const callback = reinterpret_cast<void *>(0xabcdef00);
unsigned controllerCalls;
bool supported;
bool retryAllowed;
}

extern "C" {
uint16_t intelPrivacyNativeIRKCount = 7;
void *intelPrivacyNativeIRKTable = reinterpret_cast<void *>(0x77773333);
extern uint8_t testMarker[8];
void intelPrivacySleep(void *);
void intelPrivacyWake(void *);
void intelPrivacyStart(void *);
void intelPrivacyStop(void *);
uint32_t intelPrivacyRemove(const void *);
void intelPrivacyClear();
uint32_t intelPrivacyReset(uint32_t, void *);
uint32_t intelPrivacyInstalledControllerGate();
void intelPrivacyRetry(void *);
uint32_t intelPrivacyRefreshRetry(void *self) {
    assert(self == instance); events.push_back(11); return retryAllowed;
}
void intelPrivacyNativeRetry(void *self) {
    assert(self == instance); events.push_back(12);
}

void intelPrivacySetEnabled(uint32_t enable) { events.push_back(enable ? 2 : 1); }
void intelPrivacyInvalidate() { events.push_back(3); }
uint32_t intelPrivacyActiveController() { ++controllerCalls; return supported; }
void testSleepBody(void *self) { assert(self==instance); events.push_back(4); }
void testWakeBody(void *self) { assert(self==instance); events.push_back(5); }
void testStartBody(void *self) { assert(self==instance); events.push_back(6); }
void testStopBody(void *self) { assert(self==instance); events.push_back(7); }
uint32_t testRemoveBody(const void *identity, uint32_t count) {
    assert(identity==instance); assert(count==intelPrivacyNativeIRKCount);
    events.push_back(8); return count ? 0 : 0x78;
}
void testClearBody(void *table) {
    assert(table==intelPrivacyNativeIRKTable); events.push_back(9);
}
uint32_t testResetBody(uint32_t descriptor, void *completion, uint32_t vectorCount) {
    assert(descriptor==0x37 && completion==callback && vectorCount==0);
    events.push_back(10); return 0x91;
}
}

int main() {
    intelPrivacySleep(instance); assert((events==std::vector<unsigned>{1,4})); events.clear();
    intelPrivacyWake(instance); assert((events==std::vector<unsigned>{5,2})); events.clear();
    intelPrivacyStart(instance); assert((events==std::vector<unsigned>{6,2})); events.clear();
    intelPrivacyStop(instance); assert((events==std::vector<unsigned>{1,7})); events.clear();
    assert(intelPrivacyRemove(instance)==0);
    assert((events==std::vector<unsigned>{3,8})); events.clear();
    intelPrivacyNativeIRKCount=0;
    assert(intelPrivacyRemove(instance)==0x78);
    assert((events==std::vector<unsigned>{3,8})); events.clear();
    intelPrivacyClear(); assert((events==std::vector<unsigned>{3,9})); events.clear();
    assert(intelPrivacyReset(0x37,callback)==0x91);
    assert((events==std::vector<unsigned>{3,10})); events.clear();
    retryAllowed=true; intelPrivacyRetry(instance);
    assert((events==std::vector<unsigned>{11,12})); events.clear();
    retryAllowed=false; intelPrivacyRetry(instance);
    assert((events==std::vector<unsigned>{11})); events.clear();
    supported=true;
    assert(intelPrivacyInstalledControllerGate()==1 && controllerCalls==1);
    testMarker[7]^=1;
    assert(intelPrivacyInstalledControllerGate()==0 && controllerCalls==1);
    testMarker[7]^=1;
    testMarker[0]^=1;
    assert(intelPrivacyInstalledControllerGate()==0 && controllerCalls==1);
    testMarker[0]^=1;
    supported=false;
    assert(intelPrivacyInstalledControllerGate()==0 && controllerCalls==2);
    puts("IntelPrivacy wrapper ABI, lifecycle order and all-hook markers passed");
}
