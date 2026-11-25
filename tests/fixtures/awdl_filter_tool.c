// Run the complete command-line tool against a local registry model.
// CoreFoundation dictionaries, types and reference counting use the system framework.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *scenario;
static CFMutableDictionaryRef registry;
static unsigned calls, matches, reads, writes, next_index;
static unsigned iterator_refs, service_refs[2];
static unsigned write_service, write_key_count;
static bool write_boolean;
static char matching_class[80], write_key[80];

static bool is(const char *name) { return strcmp(scenario, name) == 0; }

static void number(CFStringRef key, int64_t value)
{
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value);
    assert(n);
    CFDictionarySetValue(registry, key, n);
    CFRelease(n);
}

static CFMutableDictionaryRef mock_matching(const char *name)
{
    ++calls;
    assert(name && strlen(name) < sizeof(matching_class));
    strcpy(matching_class, name);
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef value = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
    assert(match && value);
    CFDictionarySetValue(match, CFSTR("IOProviderClass"), value);
    CFRelease(value);
    return match;
}

static kern_return_t mock_get_matching(mach_port_t port, CFDictionaryRef match, io_iterator_t *iterator)
{
    ++calls; ++matches;
    assert(port == kIOMainPortDefault && match && iterator);
    assert(CFEqual(CFDictionaryGetValue(match, CFSTR("IOProviderClass")),
                   CFSTR("AirportItlwmAWDLInterface")));
    CFRelease(match); // IOServiceGetMatchingServices consumes its matching dictionary.
    *iterator = IO_OBJECT_NULL;
    if (is("matching_failure")) return KERN_FAILURE;
    *iterator = 100;
    ++iterator_refs;
    return KERN_SUCCESS;
}

static io_object_t mock_next(io_iterator_t iterator)
{
    ++calls;
    assert(iterator == 100 && iterator_refs == 1);
    unsigned count = is("missing") ? 0 : is("multiple") ? 2 : 1;
    if (next_index >= count) return IO_OBJECT_NULL;
    ++service_refs[next_index];
    return 501 + next_index++;
}

static kern_return_t mock_release(io_object_t object)
{
    ++calls;
    if (object == 100) { assert(iterator_refs == 1); --iterator_refs; }
    else {
        assert(object >= 501 && object <= 502 && service_refs[object - 501] == 1);
        --service_refs[object - 501];
    }
    return KERN_SUCCESS;
}

static kern_return_t mock_properties(io_registry_entry_t service, CFMutableDictionaryRef *values,
                                     CFAllocatorRef allocator, IOOptionBits options)
{
    ++calls; ++reads;
    assert(service == 501 && service_refs[0] == 1 && allocator == kCFAllocatorDefault && options == 0);
    *values = NULL;
    if (is("read_failure") || (is("post_read_failure") && reads == 2)) return KERN_FAILURE;
    *values = CFDictionaryCreateMutableCopy(allocator, 0, registry);
    assert(*values);
    return KERN_SUCCESS;
}

static kern_return_t mock_set(io_registry_entry_t service, CFTypeRef properties)
{
    ++calls; ++writes;
    assert(service == 501 && service_refs[0] == 1 && properties);
    assert(CFGetTypeID(properties) == CFDictionaryGetTypeID());
    CFDictionaryRef request = (CFDictionaryRef)properties;
    write_service = service;
    write_key_count = (unsigned)CFDictionaryGetCount(request);
    assert(write_key_count == 1);
    const void *key, *value;
    CFDictionaryGetKeysAndValues(request, &key, &value);
    assert(CFGetTypeID(key) == CFStringGetTypeID());
    assert(CFStringGetCString(key, write_key, sizeof(write_key), kCFStringEncodingUTF8));
    assert(CFEqual(key, CFSTR("AWDLReceiveAllData")) && CFGetTypeID(value) == CFBooleanGetTypeID());
    write_boolean = CFBooleanGetValue(value);
    number(CFSTR("TxSubmitted"), 22); // Distinguish the post-command status from the initial snapshot.
    if (is("write_failure")) {
        number(CFSTR("AWDLReceiveFilterCommandError"), KERN_FAILURE);
        CFDictionarySetValue(registry, CFSTR("AWDLReceiveFilterKnown"), kCFBooleanFalse);
        return KERN_FAILURE;
    }
    CFDictionarySetValue(registry, key, value);
    number(CFSTR("AWDLReceiveFilterCommandError"), 0);
    return KERN_SUCCESS;
}

#define IOServiceMatching mock_matching
#define IOServiceGetMatchingServices mock_get_matching
#define IOIteratorNext mock_next
#define IOObjectRelease mock_release
#define IORegistryEntryCreateCFProperties mock_properties
#define IORegistryEntrySetCFProperties mock_set
#define main filter_tool_main
#include "tools/awdl-receive-filter.c"
#undef main

int main(int argc, char **argv)
{
    assert(argc >= 2);
    scenario = argv[1];
    registry = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    assert(registry);
    if (!is("old"))
        CFDictionarySetValue(registry, CFSTR("AWDLReceiveAllDataSupported"),
                             is("release") ? kCFBooleanFalse : kCFBooleanTrue);
    if (is("wrong_support_type")) number(CFSTR("AWDLReceiveAllDataSupported"), 1);
    CFDictionarySetValue(registry, CFSTR("AWDLReceiveAllData"), is("enabled") ? kCFBooleanTrue : kCFBooleanFalse);
    CFDictionarySetValue(registry, CFSTR("AWDLReceiveFilterKnown"), kCFBooleanTrue);
    CFDictionarySetValue(registry, CFSTR("RadioReady"), kCFBooleanTrue);
    CFDictionarySetValue(registry, CFSTR("SSID"), CFSTR("PRIVATE_WIFI_NAME"));
    CFDictionarySetValue(registry, CFSTR("MACAddress"), CFSTR("aa:bb:cc:dd:ee:ff"));
    CFDictionarySetValue(registry, CFSTR("PeerName"), CFSTR("PRIVATE_PEER_NAME"));
    number(CFSTR("DwellStarts"), 42);
    number(CFSTR("TxSubmitted"), 11);
    number(CFSTR("RadioError"), -7);
    if (is("types")) {
        CFDictionarySetValue(registry, CFSTR("RadioReady"), CFSTR("PRIVATE_WIFI_NAME"));
        const void *private_peer = CFSTR("PRIVATE_PEER_NAME");
        CFArrayRef list = CFArrayCreate(kCFAllocatorDefault, &private_peer, 1, &kCFTypeArrayCallBacks);
        assert(list);
        CFDictionarySetValue(registry, CFSTR("DwellEnds"), list);
        CFRelease(list);
    }
    int code = filter_tool_main(argc - 1, argv + 1);
    CFRelease(registry);
    fprintf(stderr, "MOCK_TRACE {\"iokit_calls\":%u,\"matches\":%u,\"reads\":%u,\"writes\":%u,"
        "\"matching_class\":\"%s\",\"write_service\":%u,\"write_key_count\":%u,"
        "\"write_key\":\"%s\",\"write_boolean\":%s,\"unreleased_objects\":%u}\n",
        calls, matches, reads, writes, matching_class, write_service, write_key_count,
        write_key, write_boolean ? "true" : "false", iterator_refs + service_refs[0] + service_refs[1]);
    return code;
}
