#include "wiimote.h"

#include <string.h>

// Output reports (to the Wiimote)
#define OUT_LED 0x11
#define OUT_REPORT_MODE 0x12
#define OUT_STATUS_REQUEST 0x15
#define OUT_WRITE_MEMORY 0x16
#define OUT_READ_MEMORY 0x17

// Input reports (from the Wiimote)
#define IN_STATUS 0x20
#define IN_READ_DATA 0x21
#define IN_ACK 0x22
#define IN_BUTTONS_ACCEL 0x31
#define IN_BUTTONS_ACCEL_EXT16 0x35

#define STATUS_FLAG_EXTENSION 0x02
#define REPORT_MODE_CONTINUOUS 0x04

// Extension registers, in the Wiimote's 0xA4xxxx control register space.
#define REG_EXT_INIT_1 0xF0
#define REG_EXT_INIT_2 0xFB
#define REG_EXT_ID 0xFA

#define MAX_STALLED_TICKS 2

// Measured on a Les Paul: the bar rests at 15-16 and reached 26. Other guitars may
// go further, which wm_guitar_to_packet adapts to.
#define WHAMMY_REST 16
#define WHAMMY_MIN_FULL_TRAVEL 26

// Measured on a Les Paul: the neck axis reads 102 pointing up and 155 pointing down,
// so about 128 when level and 26.5 per g. The trigger is about 55 degrees up and the
// release about 30. The neck must stay raised for about 130ms, so that a hard strum
// doesn't set it off.
#define TILT_TRIGGER 107.0f
#define TILT_RELEASE 115.0f
#define TILT_SMOOTHING 0.2f
#define TILT_HOLD_REPORTS 12

static void send_report_mode(wiimote *w, uint8_t mode) {
    const uint8_t report[] = {OUT_REPORT_MODE, REPORT_MODE_CONTINUOUS, mode};
    w->send(w->send_ctx, report, sizeof(report));
}

static void send_status_request(wiimote *w) {
    const uint8_t report[] = {OUT_STATUS_REQUEST, 0x00};
    w->send(w->send_ctx, report, sizeof(report));
}

static void send_ext_register_write(wiimote *w, uint8_t reg, uint8_t value) {
    uint8_t report[22] = {OUT_WRITE_MEMORY, 0x04, 0xA4, 0x00, reg, 0x01, value};
    w->send(w->send_ctx, report, sizeof(report));
}

static void send_ext_id_read(wiimote *w) {
    const uint8_t report[] = {OUT_READ_MEMORY, 0x04, 0xA4, 0x00, REG_EXT_ID, 0x00, 0x06};
    w->send(w->send_ctx, report, sizeof(report));
}

static wm_ext_type identify_extension(const uint8_t id[6]) {
    static const struct {
        uint8_t id[6];
        wm_ext_type type;
    } known[] = {
        {{0x00, 0x00, 0xA4, 0x20, 0x01, 0x03}, WM_EXT_GUITAR},
        {{0x01, 0x00, 0xA4, 0x20, 0x01, 0x03}, WM_EXT_DRUMS},
        {{0x03, 0x00, 0xA4, 0x20, 0x01, 0x03}, WM_EXT_TURNTABLE},
        {{0x00, 0x00, 0xA4, 0x20, 0x00, 0x00}, WM_EXT_NUNCHUK},
        {{0x00, 0x00, 0xA4, 0x20, 0x01, 0x01}, WM_EXT_CLASSIC},
        {{0x01, 0x00, 0xA4, 0x20, 0x01, 0x01}, WM_EXT_CLASSIC_PRO},
        {{0x00, 0x00, 0xA4, 0x20, 0x04, 0x05}, WM_EXT_MOTION_PLUS},
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (memcmp(id, known[i].id, 6) == 0) {
            return known[i].type;
        }
    }
    return WM_EXT_UNKNOWN;
}

// These bits are always high in real guitar data. A frame without them is a glitch,
// such as the zeros sent while the cable is being plugged in, which would otherwise
// read as every button held down.
static bool guitar_frame_is_valid(const uint8_t *ext) {
    return (ext[4] & 0xAB) == 0xAB && (ext[5] & 0x06) == 0x06;
}

// All guitar buttons are active low. Layout per Linux's hid-wiimote driver.
static void decode_guitar(wm_guitar *g, const uint8_t *ext) {
    g->stick_x = ext[0] & 0x3F;
    g->stick_y = ext[1] & 0x3F;
    g->touch_bar = ext[2] & 0x1F;
    g->whammy = ext[3] & 0x1F;
    g->strum_down = !(ext[4] & 0x40);
    g->minus = !(ext[4] & 0x10);
    g->plus = !(ext[4] & 0x04);
    g->orange = !(ext[5] & 0x80);
    g->red = !(ext[5] & 0x40);
    g->blue = !(ext[5] & 0x20);
    g->green = !(ext[5] & 0x10);
    g->yellow = !(ext[5] & 0x08);
    g->strum_up = !(ext[5] & 0x01);
}

static void handle_status(wiimote *w, const uint8_t *report, size_t len) {
    if (len < 4) {
        return;
    }
    // The Wiimote stops streaming after a status report, including the one it
    // sends by itself when an extension is plugged in or out, so always redo
    // the setup from here.
    w->guitar = (wm_guitar){0};
    memset(w->ext_raw, 0, sizeof(w->ext_raw));
    if (report[3] & STATUS_FLAG_EXTENSION) {
        w->stage = WM_STAGE_EXT_INIT_1;
        send_ext_register_write(w, REG_EXT_INIT_1, 0x55);
    } else {
        w->ext = WM_EXT_NONE;
        memset(w->ext_id, 0, sizeof(w->ext_id));
        w->stage = WM_STAGE_RUNNING;
        send_report_mode(w, IN_BUTTONS_ACCEL);
    }
}

static void handle_ack(wiimote *w, const uint8_t *report, size_t len) {
    if (len < 5 || report[3] != OUT_WRITE_MEMORY) {
        return;
    }
    if (report[4] != 0) {
        // The extension is still settling or was just pulled out. wm_tick retries.
        return;
    }
    if (w->stage == WM_STAGE_EXT_INIT_1) {
        w->stage = WM_STAGE_EXT_INIT_2;
        send_ext_register_write(w, REG_EXT_INIT_2, 0x00);
    } else if (w->stage == WM_STAGE_EXT_INIT_2) {
        w->stage = WM_STAGE_EXT_READ_ID;
        send_ext_id_read(w);
    }
}

static void handle_read_data(wiimote *w, const uint8_t *report, size_t len) {
    if (w->stage != WM_STAGE_EXT_READ_ID || len < 12) {
        return;
    }
    uint8_t error = report[3] & 0x0F;
    uint8_t address_low = report[5];
    if (error != 0 || address_low != REG_EXT_ID) {
        return;
    }
    memcpy(w->ext_id, &report[6], 6);
    w->ext = identify_extension(w->ext_id);
    w->stage = WM_STAGE_RUNNING;
    send_report_mode(w, IN_BUTTONS_ACCEL_EXT16);
}

void wm_init(wiimote *w, wm_send_fn send, void *send_ctx) {
    memset(w, 0, sizeof(*w));
    w->send = send;
    w->send_ctx = send_ctx;
}

void wm_start(wiimote *w) {
    const uint8_t led[] = {OUT_LED, 0x10};
    w->send(w->send_ctx, led, sizeof(led));
    w->stage = WM_STAGE_STATUS;
    send_status_request(w);
}

bool wm_handle_report(wiimote *w, const uint8_t *report, size_t len) {
    if (len < 3) {
        return false;
    }
    w->buttons = (uint16_t)((report[1] << 8) | report[2]) & 0x1F9F;

    switch (report[0]) {
    case IN_STATUS:
        handle_status(w, report, len);
        return false;
    case IN_READ_DATA:
        handle_read_data(w, report, len);
        return false;
    case IN_ACK:
        handle_ack(w, report, len);
        return false;
    case IN_BUTTONS_ACCEL:
    case IN_BUTTONS_ACCEL_EXT16:
        break;
    default:
        return false;
    }

    if (len < 6) {
        return false;
    }
    w->accel_x = report[3];
    w->accel_y = report[4];
    w->accel_z = report[5];
    if (report[0] == IN_BUTTONS_ACCEL_EXT16 && len >= 22) {
        memcpy(w->ext_raw, &report[6], sizeof(w->ext_raw));
        if (w->ext == WM_EXT_GUITAR) {
            if (guitar_frame_is_valid(w->ext_raw)) {
                decode_guitar(&w->guitar, w->ext_raw);
            } else {
                w->invalid_ext_frames++;
            }
        }
    }
    w->input_reports++;
    return true;
}

void wm_tick(wiimote *w) {
    if (w->stage == WM_STAGE_IDLE || w->stage == WM_STAGE_RUNNING) {
        w->stalled_ticks = 0;
        w->last_tick_stage = w->stage;
        return;
    }
    if (w->stage != w->last_tick_stage) {
        w->stalled_ticks = 0;
        w->last_tick_stage = w->stage;
        return;
    }
    if (++w->stalled_ticks >= MAX_STALLED_TICKS) {
        w->stalled_ticks = 0;
        w->stage = WM_STAGE_STATUS;
        send_status_request(w);
    }
}

wm_bridge_packet wm_guitar_to_packet(const wm_guitar *g, uint8_t *highest_whammy) {
    wm_bridge_packet packet = {.magic = WM_BRIDGE_MAGIC};
    packet.buttons = (g->strum_up ? 1u << 0 : 0) | (g->strum_down ? 1u << 1 : 0) | (g->green ? 1u << 4 : 0) |
                     (g->red ? 1u << 5 : 0) | (g->yellow ? 1u << 6 : 0) | (g->blue ? 1u << 7 : 0) |
                     (g->orange ? 1u << 8 : 0) | (g->plus ? 1u << 9 : 0) | (g->minus ? 1u << 10 : 0);

    if (*highest_whammy < WHAMMY_MIN_FULL_TRAVEL) {
        *highest_whammy = WHAMMY_MIN_FULL_TRAVEL;
    }
    if (g->whammy > *highest_whammy) {
        *highest_whammy = g->whammy;
    }
    if (g->whammy > WHAMMY_REST) {
        packet.whammy = (float)(g->whammy - WHAMMY_REST) / (float)(*highest_whammy - WHAMMY_REST);
    }
    return packet;
}

bool wm_tilt_update(wm_tilt *tilt, uint8_t neck_axis) {
    if (!tilt->started) {
        tilt->smoothed = neck_axis;
        tilt->started = true;
    }
    tilt->smoothed += (neck_axis - tilt->smoothed) * TILT_SMOOTHING;

    if (tilt->smoothed <= TILT_TRIGGER) {
        if (tilt->reports_past_trigger < TILT_HOLD_REPORTS) {
            tilt->reports_past_trigger++;
        }
        if (tilt->reports_past_trigger >= TILT_HOLD_REPORTS) {
            tilt->active = true;
        }
    } else {
        tilt->reports_past_trigger = 0;
        if (tilt->smoothed >= TILT_RELEASE) {
            tilt->active = false;
        }
    }
    return tilt->active;
}

uint64_t wm_sync_pairing_key(const uint8_t host_address[6]) {
    uint64_t key = 0;
    for (int i = 0; i < 6; i++) {
        key |= (uint64_t)host_address[5 - i] << (8 * i);
    }
    return key;
}

const char *wm_ext_name(wm_ext_type ext) {
    switch (ext) {
    case WM_EXT_NONE: return "nothing";
    case WM_EXT_GUITAR: return "Guitar Hero guitar";
    case WM_EXT_DRUMS: return "Guitar Hero drums";
    case WM_EXT_TURNTABLE: return "DJ Hero turntable";
    case WM_EXT_NUNCHUK: return "Nunchuk";
    case WM_EXT_CLASSIC: return "Classic Controller";
    case WM_EXT_CLASSIC_PRO: return "Classic Controller Pro";
    case WM_EXT_MOTION_PLUS: return "MotionPlus";
    case WM_EXT_UNKNOWN: break;
    }
    return "unknown add-on";
}
