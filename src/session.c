// SPDX-License-Identifier: GPL-3.0-or-later
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

typedef struct {
    int player;
    IOHIDDeviceRef device;
    wiimote wm;
    uint8_t input_buffer[INPUT_BUFFER_SIZE];
    bool extension_announced;
    wm_ext_type announced_extension;
    uint8_t address[6];
    uint8_t highest_whammy;
    wm_tilt tilt;

    char address_text[18];
    _Atomic bool connected;
    _Atomic int extension;
} slot;

static struct {
    session_callbacks callbacks;
    slot slots[SESSION_MAX_WIIMOTES];
    int max_wiimotes;
    int bridge_socket;
    double last_refused_at;
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
    slot *s = ctx;
    if (session.callbacks.report_seen) {
        session.callbacks.report_seen(session.callbacks.ctx, s->player, true, report, len);
    }
    IOReturn result = IOHIDDeviceSetReport(s->device, kIOHIDReportTypeOutput, report[0], report, (CFIndex)len);
    if (result != kIOReturnSuccess) {
        char message[80];
        snprintf(message, sizeof(message), "Could not send report 0x%02X to Wiimote %d (error 0x%08X)", report[0],
                 s->player + 1, result);
        report_problem(message);
    }
}

static void send_to_bridge(slot *s, bool tilted) {
    wm_bridge_packet packet = wm_guitar_to_packet(&s->wm.guitar, &s->highest_whammy);
    packet.tilt = tilted ? 1.0f : 0.0f;
    memcpy(packet.address, s->address, sizeof(packet.address));

    double now = now_seconds();
    if (send(session.bridge_socket, &packet, sizeof(packet), 0) < 0 && errno == ECONNREFUSED) {
        session.last_refused_at = now;
    }
    session.yarg_listening = session.last_refused_at == 0 || now - session.last_refused_at > YARG_SILENCE_SECONDS;
}

static void announce_extension(slot *s) {
    if (s->extension_announced && s->announced_extension == s->wm.ext) {
        return;
    }
    s->extension_announced = true;
    s->announced_extension = s->wm.ext;
    s->extension = s->wm.ext;
    if (session.callbacks.extension_changed) {
        session.callbacks.extension_changed(session.callbacks.ctx, s->player, &s->wm);
    }
}

static void input_report_callback(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id,
                                  uint8_t *report, CFIndex length) {
    (void)sender, (void)type;
    slot *s = ctx;
    if (result != kIOReturnSuccess || length <= 0 || !s->device) {
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

    bool is_input = wm_handle_report(&s->wm, buffer, len);
    if (!is_input && session.callbacks.report_seen) {
        session.callbacks.report_seen(session.callbacks.ctx, s->player, false, buffer, len);
    }
    if (s->wm.stage == WM_STAGE_RUNNING) {
        announce_extension(s);
    }
    if (!is_input) {
        return;
    }

    session.reports++;
    bool tilted = false;
    if (wm_ext_plays_as_guitar(s->wm.ext)) {
        // Tilting only makes sense with a guitar. Moving a Wiimote that's just attached
        // to a Classic Controller shouldn't set off star power.
        tilted = s->wm.ext == WM_EXT_GUITAR && wm_tilt_update(&s->tilt, s->wm.accel_y);
        if (session.bridge_socket >= 0) {
            send_to_bridge(s, tilted);
        }
    }
    if (session.callbacks.input) {
        session.callbacks.input(session.callbacks.ctx, s->player, &s->wm, tilted);
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

// The address is what lets YARG tell Wiimotes apart between sessions.
static void read_address(slot *s, IOHIDDeviceRef device) {
    char text[32] = "";
    CFStringRef serial = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDSerialNumberKey));
    if (serial) {
        CFStringGetCString(serial, text, sizeof(text), kCFStringEncodingUTF8);
    }
    unsigned bytes[6];
    if (sscanf(text, "%x%*1[:-]%x%*1[:-]%x%*1[:-]%x%*1[:-]%x%*1[:-]%x", &bytes[0], &bytes[1], &bytes[2], &bytes[3],
               &bytes[4], &bytes[5]) != 6) {
        // Without an address, fall back on the player number, which still keeps two apart.
        memset(bytes, 0, sizeof(bytes));
        bytes[5] = (unsigned)s->player + 1;
    }
    for (int i = 0; i < 6; i++) {
        s->address[i] = (uint8_t)bytes[i];
    }
    snprintf(s->address_text, sizeof(s->address_text), "%02X:%02X:%02X:%02X:%02X:%02X", s->address[0], s->address[1],
             s->address[2], s->address[3], s->address[4], s->address[5]);
}

static void device_matched(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)ctx, (void)result, (void)sender;
    if (!is_wiimote(device)) {
        return;
    }
    slot *s = NULL;
    for (int i = 0; i < session.max_wiimotes; i++) {
        if (session.slots[i].device == device) {
            return;
        }
        if (!s && !session.slots[i].device) {
            s = &session.slots[i];
        }
    }
    if (!s) {
        return;
    }

    IOReturn opened = IOHIDDeviceOpen(device, kIOHIDOptionsTypeNone);
    if (opened != kIOReturnSuccess) {
        char message[80];
        snprintf(message, sizeof(message), "Found a Wiimote but could not open it (error 0x%08X)", opened);
        report_problem(message);
        return;
    }
    s->device = device;
    s->extension_announced = false;
    s->tilt = (wm_tilt){0};
    s->highest_whammy = 0;
    read_address(s, device);
    s->connected = true;
    if (session.callbacks.connected) {
        session.callbacks.connected(session.callbacks.ctx, s->player);
    }
    IOHIDDeviceRegisterInputReportCallback(device, s->input_buffer, INPUT_BUFFER_SIZE, input_report_callback, s);
    wm_init(&s->wm, send_report, s);
    wm_start(&s->wm, s->player);
}

static void device_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef device) {
    (void)ctx, (void)result, (void)sender;
    for (int i = 0; i < session.max_wiimotes; i++) {
        slot *s = &session.slots[i];
        if (s->device == device) {
            s->device = NULL;
            s->connected = false;
            s->extension = WM_EXT_NONE;
            if (session.callbacks.disconnected) {
                session.callbacks.disconnected(session.callbacks.ctx, s->player);
            }
        }
    }
}

static void protocol_timer(CFRunLoopTimerRef timer, void *info) {
    (void)timer, (void)info;
    for (int i = 0; i < session.max_wiimotes; i++) {
        if (session.slots[i].device) {
            wm_tick(&session.slots[i].wm);
        }
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

IOReturn session_start(const session_callbacks *callbacks, bool send_to_yarg, int max_wiimotes, CFRunLoopRef run_loop) {
    session.callbacks = *callbacks;
    session.max_wiimotes = max_wiimotes < SESSION_MAX_WIIMOTES ? max_wiimotes : SESSION_MAX_WIIMOTES;
    for (int i = 0; i < SESSION_MAX_WIIMOTES; i++) {
        session.slots[i].player = i;
    }
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

session_wiimote session_status(int player) {
    session_wiimote status = {0};
    if (player >= 0 && player < SESSION_MAX_WIIMOTES && session.slots[player].connected) {
        status.connected = true;
        status.extension = (wm_ext_type)session.slots[player].extension;
        memcpy(status.address, session.slots[player].address_text, sizeof(status.address));
    }
    return status;
}

bool session_any_connected(void) {
    for (int i = 0; i < SESSION_MAX_WIIMOTES; i++) {
        if (session.slots[i].connected) {
            return true;
        }
    }
    return false;
}

uint64_t session_input_reports(void) {
    return session.reports;
}

bool session_yarg_listening(void) {
    return session.yarg_listening;
}
