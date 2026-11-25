// SPDX-License-Identifier: GPL-2.0-or-later
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_error.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static CFMutableDictionaryRef properties(io_registry_entry_t service)
{
    CFMutableDictionaryRef values = NULL;
    kern_return_t result = IORegistryEntryCreateCFProperties(service, &values,
                                                            kCFAllocatorDefault, 0);
    if (result != KERN_SUCCESS)
        fprintf(stderr, "Cannot read AWDL properties: %s (0x%x)\n",
                mach_error_string(result), (unsigned)result);
    return values;
}

static bool enabled(CFDictionaryRef values, CFStringRef key)
{
    CFTypeRef value = CFDictionaryGetValue(values, key);
    return value && CFGetTypeID(value) == CFBooleanGetTypeID() &&
           CFBooleanGetValue((CFBooleanRef)value);
}

static void print_status(CFDictionaryRef values, bool changed, kern_return_t result)
{
    static const char *keys[] = {
        "AWDLReceiveAllDataSupported", "AWDLReceiveAllData", "AWDLReceiveFilterKnown",
        "AWDLReceiveFilterCommandError", "AWDLReceiveFilterCleanupResult",
        "RadioReady", "RadioError", "RadioFailureStage", "RadioFailureError",
        "DwellStarts", "DwellEnds", "TxSubmitted", "TxCompleted", "TxFailed",
        "RxDelivered", "ActionFramesToSystem"
    };
    printf("{\n  \"command_return\": ");
    if (changed) printf("%u", (unsigned)result);
    else printf("null");
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        CFStringRef key = CFStringCreateWithCString(kCFAllocatorDefault, keys[i], kCFStringEncodingUTF8);
        CFTypeRef value = key ? CFDictionaryGetValue(values, key) : NULL;
        printf(",\n  \"%s\": ", keys[i]);
        if (value && CFGetTypeID(value) == CFBooleanGetTypeID()) {
            printf("%s", CFBooleanGetValue((CFBooleanRef)value) ? "true" : "false");
        } else if (value && CFGetTypeID(value) == CFNumberGetTypeID()) {
            int64_t number;
            if (CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &number))
                printf("%lld", (long long)number);
            else printf("null");
        } else printf("null");
        if (key) CFRelease(key);
    }
    puts("\n}");
}

int main(int argc, char **argv)
{
    const char *action = argc == 1 ? "--status" : argv[1];
    bool change = !strcmp(action, "--enable") || !strcmp(action, "--disable");
    if (argc > 2 || (!change && strcmp(action, "--status"))) {
        fprintf(stderr, "Usage: awdl-receive-filter [--status | --enable | --disable]\n");
        return 64;
    }
    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t result = IOServiceGetMatchingServices(kIOMainPortDefault,
        IOServiceMatching("AirportItlwmAWDLInterface"), &iterator);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "Cannot find AWDL interface: %s\n", mach_error_string(result));
        return 1;
    }
    io_service_t service = IOIteratorNext(iterator);
    io_service_t another = IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if (!service || another) {
        fprintf(stderr, "%s\n", another ? "More than one AirportItlwm AWDL interface is present" :
                                          "AirportItlwm AWDL interface is not loaded");
        if (service) IOObjectRelease(service);
        if (another) IOObjectRelease(another);
        return 2;
    }
    CFMutableDictionaryRef values = properties(service);
    if (!values) { IOObjectRelease(service); return 1; }
    if (change && !enabled(values, CFSTR("AWDLReceiveAllDataSupported"))) {
        print_status(values, false, KERN_SUCCESS);
        fprintf(stderr, "This driver does not support the Debug receive-filter control\n");
        CFRelease(values); IOObjectRelease(service); return 3;
    }
    if (change) {
        const void *key = CFSTR("AWDLReceiveAllData");
        const void *value = !strcmp(action, "--enable") ? kCFBooleanTrue : kCFBooleanFalse;
        CFDictionaryRef request = CFDictionaryCreate(kCFAllocatorDefault, &key, &value, 1,
                                                     &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks);
        if (!request) { CFRelease(values); IOObjectRelease(service); return 1; }
        result = IORegistryEntrySetCFProperties(service, request);
        CFRelease(request);
        CFRelease(values);
        values = properties(service);
        if (result != KERN_SUCCESS)
            fprintf(stderr, "AWDL filter command failed: %s (0x%x)\n",
                    mach_error_string(result), (unsigned)result);
    }
    if (values) { print_status(values, change, result); CFRelease(values); }
    IOObjectRelease(service);
    return result != KERN_SUCCESS || !values;
}
