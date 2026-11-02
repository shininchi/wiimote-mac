// SPDX-License-Identifier: GPL-3.0-or-later
// Streams from every connected Wiimote at once and reports how steady each one is, to
// check whether several can be used together. Connect them all before running this.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDManager.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/wiimote.h"

#define MAX_WIIMOTES 4
#define INPUT_BUFFER_SIZE 64
#define NOTICEABLE_GAP 0.050

typedef struct {
    IOHIDDeviceRef device;
    wiimote wm;
    uint8_t input_buffer[INPUT_BUFFER_SIZE];
    char address[32];
    double connected_at, last_report_at, longest_gap, removed_at;
    uint64_t noticeable_gaps, send_failures;
    bool announced;
} slot;

static slot slots[MAX_WIIMOTES];
static int slot_count;

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void send_report(void *ctx, const uint8_t *report, size_t len) {
    slot *s = ctx;
    if (IOHIDDeviceSetReport(s->device, kIOHIDReportTypeOutput, report[0], report, (CFIndex)len) != kIOReturnSuccess) {
        s->send_failures++;
    }
}

static void input_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id,
                         uint8_t *report, CFIndex length) {
    (void)sender, (void)type;
    slot *s = ctx;
    if (result != kIOReturnSuccess || length <= 0 || length > INPUT_BUFFER_SIZE) {
        return;
    }
    uint8_t buffer[INPUT_BUFFER_SIZE + 1];
    size_t len = (size_t)length;
    if (report[0] == report_id) {
        memcpy(buffer, report, len);
    } else {
        buffer[0] = (uint8_t)report_id;
        memcpy(buffer + 1, report, len);
        len++;
    }

    bool is_input = wm_handle_report(&s->wm, buffer, len);
    if (s->wm.stage == WM_STAGE_RUNNING && !s->announced) {
        s->announced = true;
        printf("%s: plugged in: %s\n", s->address, wm_ext_name(s->wm.ext));
        fflush(stdout);
    }
    if (!is_input) {
        return;
    }
    double now = now_seconds();
    if (s->last_report_at > 0) {
        double gap = now - s->last_report_at;
        if (gap > s->longest_gap) s->longest_gap = gap;
        if (gap > NOTICEABLE_GAP) s->noticeable_gaps++;
    }
    s->last_report_at = now;
}

static void device_matched(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)ctx, (void)result, (void)sender;
    if (slot_count == MAX_WIIMOTES || IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        return;
    }
    slot *s = &slots[slot_count++];
    s->device = device;
    s->connected_at = now_seconds();
    CFStringRef serial = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDSerialNumberKey));
    if (!serial || !CFStringGetCString(serial, s->address, sizeof(s->address), kCFStringEncodingUTF8)) {
        snprintf(s->address, sizeof(s->address), "Wiimote %d", slot_count);
    }
    printf("%s: connected\n", s->address);
    fflush(stdout);
    IOHIDDeviceRegisterInputReportCallback(device, s->input_buffer, INPUT_BUFFER_SIZE, input_report, s);
    wm_init(&s->wm, send_report, s);
    wm_start(&s->wm, slot_count - 1);
}

static void device_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)ctx, (void)result, (void)sender;
    for (int i = 0; i < slot_count; i++) {
        if (slots[i].device == device && slots[i].removed_at == 0) {
            slots[i].removed_at = now_seconds();
            printf("%s: disconnected\n", slots[i].address);
            fflush(stdout);
        }
    }
}

static void tick(CFRunLoopTimerRef timer, void *info) {
    (void)timer, (void)info;
    for (int i = 0; i < slot_count; i++) {
        if (slots[i].removed_at == 0) {
            wm_tick(&slots[i].wm);
        }
    }
}

static void finish(CFRunLoopTimerRef timer, void *info) {
    (void)timer, (void)info;
    CFRunLoopStop(CFRunLoopGetMain());
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 20;

    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int vendor = 0x057E;
    CFNumberRef vendor_number = CFNumberCreate(NULL, kCFNumberIntType, &vendor);
    const void *keys[] = {CFSTR(kIOHIDVendorIDKey)};
    const void *values[] = {vendor_number};
    CFDictionaryRef matching = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(manager, matching);
    IOHIDManagerRegisterDeviceMatchingCallback(manager, device_matched, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(manager, device_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOReturn opened = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    if (opened != kIOReturnSuccess) {
        printf("Could not listen for devices (error 0x%08X)\n", opened);
        return 1;
    }

    CFRunLoopAddTimer(CFRunLoopGetMain(), CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1, 1, 0, 0, tick, NULL),
                      kCFRunLoopDefaultMode);
    CFRunLoopAddTimer(CFRunLoopGetMain(), CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + seconds, 0, 0, 0, finish, NULL),
                      kCFRunLoopDefaultMode);
    CFRunLoopRun();

    printf("\nWiimotes found: %d\n", slot_count);
    double now = now_seconds();
    for (int i = 0; i < slot_count; i++) {
        slot *s = &slots[i];
        double ran = (s->removed_at ? s->removed_at : now) - s->connected_at;
        printf("%s: %llu reports (%.0f per second), longest gap %.0f ms, gaps over 50 ms: %llu, send failures: %llu%s\n",
               s->address, (unsigned long long)s->wm.input_reports, ran > 0 ? (double)s->wm.input_reports / ran : 0,
               s->longest_gap * 1000, (unsigned long long)s->noticeable_gaps, (unsigned long long)s->send_failures,
               s->removed_at ? ", DISCONNECTED" : "");
    }
    return 0;
}
