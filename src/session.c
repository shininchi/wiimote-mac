#include "session.h"

#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NINTENDO_VENDOR_ID 0x057E
#define WIIMOTE_PRODUCT_ID 0x0306
#define WIIMOTE_PLUS_PRODUCT_ID 0x0330

#define INPUT_BUFFER_SIZE 64
#define YARG_SILENCE_SECONDS 1.0

static struct {
    session_callbacks callbacks;
    IOHIDDeviceRef device;
    wiimote wm;
    uint8_t input_buffer[INPUT_BUFFER_SIZE];
    bool extension_announced;
    wm_ext_type announced_extension;

    int bridge_socket;
    uint8_t highest_whammy;
    wm_tilt tilt;
    double last_refused_at;

    _Atomic bool connected;
    _Atomic int extension;
    _Atomic uint64_t reports;
    _Atomic bool yarg_listening;
} session = {.bridge_socket = -1};

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void report_problem(const char *message) {
    if (session.callbacks.problem) {
        session.callbacks.problem(session.callbacks.ctx, message);
    }
}

static void send_report(void *ctx, const uint8_t *report, size_t len) {
    (void)ctx;
    if (session.callbacks.report_seen) {
        session.callbacks.report_seen(session.callbacks.ctx, true, report, len);
    }
    IOReturn result = IOHIDDeviceSetReport(session.device, kIOHIDReportTypeOutput, report[0], report, (CFIndex)len);
    if (result != kIOReturnSuccess) {
        char message[80];
        snprintf(message, sizeof(message), "Could not send report 0x%02X to the Wiimote (error 0x%08X)", report[0],
                 result);
        report_problem(message);
    }
}

static void send_to_bridge(bool tilted) {
    wm_bridge_packet packet = wm_guitar_to_packet(&session.wm.guitar, &session.highest_whammy);
    packet.tilt = tilted ? 1.0f : 0.0f;

    double now = now_seconds();
    if (send(session.bridge_socket, &packet, sizeof(packet), 0) < 0 && errno == ECONNREFUSED) {
        session.last_refused_at = now;
    }
    session.yarg_listening = session.last_refused_at == 0 || now - session.last_refused_at > YARG_SILENCE_SECONDS;
}

static void announce_extension(void) {
    if (session.extension_announced && session.announced_extension == session.wm.ext) {
        return;
    }
    session.extension_announced = true;
    session.announced_extension = session.wm.ext;
    session.extension = session.wm.ext;
    if (session.callbacks.extension_changed) {
        session.callbacks.extension_changed(session.callbacks.ctx, &session.wm);
    }
}

static void input_report_callback(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id,
                                  uint8_t *report, CFIndex length) {
    (void)ctx, (void)sender, (void)type;
    if (result != kIOReturnSuccess || length <= 0) {
        return;
    }

    // Keep the report ID as the first byte, in case macOS has stripped it.
    uint8_t buffer[INPUT_BUFFER_SIZE + 1];
    size_t len = (size_t)length;
    if (report[0] == report_id) {
        memcpy(buffer, report, len);
    } else {
        buffer[0] = (uint8_t)report_id;
        memcpy(buffer + 1, report, len);
        len++;
    }

    bool is_input = wm_handle_report(&session.wm, buffer, len);
    if (!is_input && session.callbacks.report_seen) {
        session.callbacks.report_seen(session.callbacks.ctx, false, buffer, len);
    }
    if (session.wm.stage == WM_STAGE_RUNNING) {
        announce_extension();
    }
    if (!is_input) {
        return;
    }

    session.reports++;
    bool tilted = false;
    if (session.wm.ext == WM_EXT_GUITAR) {
        tilted = wm_tilt_update(&session.tilt, session.wm.accel_y);
        if (session.bridge_socket >= 0) {
            send_to_bridge(tilted);
        }
    }
    if (session.callbacks.input) {
        session.callbacks.input(session.callbacks.ctx, &session.wm, tilted);
    }
}

static bool is_wiimote(IOHIDDeviceRef device) {
    CFNumberRef product = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductIDKey));
    int product_id = 0;
    if (product) {
        CFNumberGetValue(product, kCFNumberIntType, &product_id);
    }
    if (product_id == WIIMOTE_PRODUCT_ID || product_id == WIIMOTE_PLUS_PRODUCT_ID) {
        return true;
    }
    CFStringRef name = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
    return name && CFStringFind(name, CFSTR("RVL-CNT"), 0).location != kCFNotFound;
}

static void device_matched(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)ctx, (void)result, (void)sender;
    if (session.device || !is_wiimote(device)) {
        return;
    }
    IOReturn opened = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (opened != kIOReturnSuccess) {
        char message[80];
        snprintf(message, sizeof(message), "Found a Wiimote but could not open it (error 0x%08X)", opened);
        report_problem(message);
        return;
    }
    session.device = device;
    session.extension_announced = false;
    session.tilt = (wm_tilt){0};
    session.connected = true;
    if (session.callbacks.connected) {
        session.callbacks.connected(session.callbacks.ctx);
    }
    IOHIDDeviceRegisterInputReportCallback(device, session.input_buffer, INPUT_BUFFER_SIZE, input_report_callback,
                                           NULL);
    wm_init(&session.wm, send_report, NULL);
    wm_start(&session.wm);
}

static void device_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)ctx, (void)result, (void)sender;
    if (device != session.device) {
        return;
    }
    session.device = NULL;
    session.connected = false;
    session.extension = WM_EXT_NONE;
    if (session.callbacks.disconnected) {
        session.callbacks.disconnected(session.callbacks.ctx);
    }
}

static void protocol_timer(CFRunLoopTimerRef timer, void *info) {
    (void)timer, (void)info;
    if (session.device) {
        wm_tick(&session.wm);
    }
}

// Opens the local connection to the YARG add-on. Sends don't wait for a reply, so
// this works whether or not YARG is running.
static int open_bridge_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(WM_BRIDGE_PORT)};
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

IOReturn session_start(const session_callbacks *callbacks, bool send_to_yarg, CFRunLoopRef run_loop) {
    session.callbacks = *callbacks;
    if (send_to_yarg) {
        session.bridge_socket = open_bridge_socket();
        if (session.bridge_socket < 0) {
            report_problem("Could not open the connection to YARG.");
        }
    }

    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    int vendor = NINTENDO_VENDOR_ID;
    CFNumberRef vendor_number = CFNumberCreate(NULL, kCFNumberIntType, &vendor);
    const void *keys[] = {CFSTR(kIOHIDVendorIDKey)};
    const void *values[] = {vendor_number};
    CFDictionaryRef matching = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    IOHIDManagerSetDeviceMatching(manager, matching);
    CFRelease(matching);
    CFRelease(vendor_number);

    IOHIDManagerRegisterDeviceMatchingCallback(manager, device_matched, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(manager, device_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(manager, run_loop, kCFRunLoopDefaultMode);

    IOReturn opened = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
    if (opened != kIOReturnSuccess) {
        return opened;
    }

    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1.0, 1.0, 0, 0, protocol_timer,
                                                   NULL);
    CFRunLoopAddTimer(run_loop, timer, kCFRunLoopDefaultMode);
    CFRelease(timer);
    return kIOReturnSuccess;
}

bool session_is_connected(void) {
    return session.connected;
}

wm_ext_type session_extension(void) {
    return (wm_ext_type)session.extension;
}

uint64_t session_input_reports(void) {
    return session.reports;
}

bool session_yarg_listening(void) {
    return session.yarg_listening;
}
