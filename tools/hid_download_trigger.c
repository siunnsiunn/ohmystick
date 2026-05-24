// Send voxstick's HID recovery sequence.
//
// Build:
//   clang -Wall -Wextra -O2 tools/hid_download_trigger.c \
//     -framework IOKit -framework CoreFoundation -o /tmp/voxstick-hid-download
//
// Run while the normal UAC/HID firmware is enumerated:
//   /tmp/voxstick-hid-download

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define VOX_VID 0x303a
#define VOX_PID 0x8001
#define HID_USAGE_PAGE_DESKTOP 0x01
#define HID_USAGE_KEYBOARD     0x06
#define HID_REPORT_ID_KBD      1
#define HID_LED_MAGIC          0x1f

static CFNumberRef cfnum(int value)
{
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value);
}

static void match_number(CFMutableDictionaryRef dict, CFStringRef key, int value)
{
    CFNumberRef num = cfnum(value);
    CFDictionarySetValue(dict, key, num);
    CFRelease(num);
}

static const char *ioret_name(IOReturn ret)
{
    switch (ret) {
        case kIOReturnSuccess:      return "success";
        case kIOReturnNotFound:     return "not found";
        case kIOReturnNotOpen:      return "not open";
        case kIOReturnNoDevice:     return "no device";
        case kIOReturnNotPermitted: return "not permitted";
        case kIOReturnExclusiveAccess: return "exclusive access";
        default:                    return "IOKit error";
    }
}

static IOReturn send_magic_reports(IOHIDDeviceRef device,
                                   uint8_t report_id,
                                   const uint8_t *report,
                                   CFIndex report_len)
{
    IOReturn ret = kIOReturnSuccess;

    for (int i = 0; i < 3; i++) {
        ret = IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput,
                                   report_id, report, report_len);
        if (ret != kIOReturnSuccess) {
            return ret;
        }
        usleep(150000);
    }

    return ret;
}

static IOReturn send_magic_variants(IOHIDDeviceRef device)
{
    uint8_t payload_only[1] = { HID_LED_MAGIC };
    uint8_t with_report_id[2] = { HID_REPORT_ID_KBD, HID_LED_MAGIC };
    uint8_t padded_payload[8] = { HID_LED_MAGIC };
    uint8_t padded_with_id[8] = { HID_REPORT_ID_KBD, HID_LED_MAGIC };
    IOReturn last_ret = kIOReturnNotFound;

    struct {
        uint8_t report_id;
        uint8_t *report;
        CFIndex report_len;
    } variants[] = {
        { HID_REPORT_ID_KBD, payload_only, sizeof(payload_only) },
        { HID_REPORT_ID_KBD, with_report_id, sizeof(with_report_id) },
        { 0, payload_only, sizeof(payload_only) },
        { 0, with_report_id, sizeof(with_report_id) },
        { HID_REPORT_ID_KBD, padded_payload, sizeof(padded_payload) },
        { 0, padded_with_id, sizeof(padded_with_id) },
    };

    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
        last_ret = send_magic_reports(device,
                                      variants[i].report_id,
                                      variants[i].report,
                                      variants[i].report_len);
        if (last_ret == kIOReturnSuccess) {
            return last_ret;
        }
    }

    return last_ret;
}

int main(void)
{
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault,
                                                 kIOHIDOptionsTypeNone);
    if (!manager) {
        fprintf(stderr, "failed to create IOHIDManager\n");
        return 1;
    }

    CFMutableDictionaryRef match = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    match_number(match, CFSTR(kIOHIDVendorIDKey), VOX_VID);
    match_number(match, CFSTR(kIOHIDProductIDKey), VOX_PID);
    match_number(match, CFSTR(kIOHIDPrimaryUsagePageKey), HID_USAGE_PAGE_DESKTOP);
    match_number(match, CFSTR(kIOHIDPrimaryUsageKey), HID_USAGE_KEYBOARD);
    IOHIDManagerSetDeviceMatching(manager, match);
    CFRelease(match);

    IOReturn ret = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    if (ret != kIOReturnSuccess) {
        fprintf(stderr, "IOHIDManagerOpen failed: 0x%08x (%s)\n",
                ret, ioret_name(ret));
        CFRelease(manager);
        return 1;
    }

    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    if (!devices || CFSetGetCount(devices) == 0) {
        fprintf(stderr, "StickS3-Mic HID keyboard not found\n");
        if (devices) CFRelease(devices);
        IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
        CFRelease(manager);
        return 1;
    }

    CFIndex device_count = CFSetGetCount(devices);
    IOHIDDeviceRef *values = calloc((size_t)device_count, sizeof(*values));
    if (!values) {
        fprintf(stderr, "calloc failed\n");
        CFRelease(devices);
        IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
        CFRelease(manager);
        return 1;
    }
    CFSetGetValues(devices, (const void **)values);

    IOReturn last_ret = kIOReturnNotFound;
    for (CFIndex idx = 0; idx < device_count; idx++) {
        IOHIDDeviceRef device = values[idx];

        ret = send_magic_variants(device);
        if (ret == kIOReturnSuccess) {
            printf("sent HID download trigger to StickS3-Mic\n");
            free(values);
            CFRelease(devices);
            IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
            CFRelease(manager);
            return 0;
        }
        last_ret = ret;

        ret = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
        if (ret == kIOReturnSuccess || ret == kIOReturnExclusiveAccess) {
            IOReturn send_ret = send_magic_variants(device);
            if (send_ret == kIOReturnSuccess) {
                printf("sent HID download trigger to StickS3-Mic\n");
                free(values);
                CFRelease(devices);
                IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
                CFRelease(manager);
                return 0;
            }
            last_ret = send_ret;
        } else {
            last_ret = ret;
        }

        ret = IOHIDDeviceOpen(device, kIOHIDOptionsTypeSeizeDevice);
        if (ret == kIOReturnSuccess || ret == kIOReturnExclusiveAccess) {
            IOReturn send_ret = send_magic_variants(device);
            if (send_ret == kIOReturnSuccess) {
                printf("sent HID download trigger to StickS3-Mic\n");
                free(values);
                CFRelease(devices);
                IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
                CFRelease(manager);
                return 0;
            }
            last_ret = send_ret;
        } else {
            last_ret = ret;
        }
    }

    fprintf(stderr, "SetReport failed on all matches: 0x%08x (%s)\n",
            last_ret, ioret_name(last_ret));
    free(values);
    CFRelease(devices);
    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);
    return 1;
}
