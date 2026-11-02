// SPDX-License-Identifier: GPL-3.0-or-later
// Wiimote protocol and extension decoding. No macOS dependencies, so it can be
// tested against a simulated Wiimote.
#ifndef WIIMOTE_H
#define WIIMOTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WM_EXT_NONE,
    WM_EXT_UNKNOWN,
    WM_EXT_GUITAR,
    WM_EXT_DRUMS,
    WM_EXT_TURNTABLE,
    WM_EXT_NUNCHUK,
    WM_EXT_CLASSIC,
    WM_EXT_CLASSIC_PRO,
    WM_EXT_MOTION_PLUS,
} wm_ext_type;

typedef enum {
    WM_STAGE_IDLE,
    WM_STAGE_STATUS,
    WM_STAGE_EXT_INIT_1,
    WM_STAGE_EXT_INIT_2,
    WM_STAGE_EXT_READ_ID,
    WM_STAGE_RUNNING,
} wm_stage;

// Wiimote core buttons, as laid out in the two button bytes of every report.
enum {
    WM_BTN_LEFT  = 0x0100,
    WM_BTN_RIGHT = 0x0200,
    WM_BTN_DOWN  = 0x0400,
    WM_BTN_UP    = 0x0800,
    WM_BTN_PLUS  = 0x1000,
    WM_BTN_TWO   = 0x0001,
    WM_BTN_ONE   = 0x0002,
    WM_BTN_B     = 0x0004,
    WM_BTN_A     = 0x0008,
    WM_BTN_MINUS = 0x0010,
    WM_BTN_HOME  = 0x0080,
};

typedef struct {
    bool green, red, yellow, blue, orange;
    bool strum_up, strum_down;
    bool plus, minus;
    uint8_t stick_x, stick_y; // 0-63
    uint8_t touch_bar;        // 0-31, World Tour guitars and newer
    uint8_t whammy;           // 0-31
} wm_guitar;

// What the YARG add-on receives for each guitar update. The button bits and field
// order must match WiiGuitarState in yarg-addon/Bridge.cs.
#define WM_BRIDGE_PORT 47283
#define WM_BRIDGE_MAGIC 0x32544757u // "WGT2"

typedef struct {
    uint32_t magic;
    uint32_t buttons;
    float whammy; // 0-1
    float tilt;   // 0-1
    uint8_t address[6]; // the Wiimote's Bluetooth address, which identifies this guitar
    uint8_t reserved[2];
} wm_bridge_packet;

// Detects the neck being raised for star power, from the Wiimote's motion sensor.
typedef struct {
    float smoothed;
    int reports_past_trigger;
    int press_reports_left;
    bool active;
    bool started;
} wm_tilt;

typedef void (*wm_send_fn)(void *ctx, const uint8_t *report, size_t len);

typedef struct {
    wm_send_fn send;
    void *send_ctx;

    wm_stage stage;
    wm_stage last_tick_stage;
    int stalled_ticks;

    wm_ext_type ext;
    uint8_t ext_id[6];
    uint8_t ext_raw[16];

    uint16_t buttons;
    uint8_t accel_x, accel_y, accel_z;
    wm_guitar guitar;

    uint64_t input_reports;
    uint64_t invalid_ext_frames;
} wiimote;

void wm_init(wiimote *w, wm_send_fn send, void *send_ctx);

// Lights this player's LED, counting from 0, and starts detecting what is plugged in.
void wm_start(wiimote *w, int player);

// Feeds one report from the Wiimote, with its report ID as the first byte.
// Returns true if it carried new input data.
bool wm_handle_report(wiimote *w, const uint8_t *report, size_t len);

// Call about once a second. Restarts detection if the Wiimote stopped answering.
void wm_tick(wiimote *w);

const char *wm_ext_name(wm_ext_type ext);

// Whether this add-on's input is available as a guitar, in wiimote.guitar.
bool wm_ext_plays_as_guitar(wm_ext_type ext);

// Converts a guitar's state for the YARG add-on. highest_whammy remembers the
// furthest the bar has gone, since its full travel varies; start it at 0.
wm_bridge_packet wm_guitar_to_packet(const wm_guitar *guitar, uint8_t *highest_whammy);

// Call for each input report, with the motion reading along the guitar's neck.
// Returns whether the neck is raised. Start with a zeroed wm_tilt.
bool wm_tilt_update(wm_tilt *tilt, uint8_t neck_axis);

// Whether tilting should be pressing star power right now, which only lasts briefly.
bool wm_tilt_pressing(const wm_tilt *tilt);

// The PIN for pairing with the SYNC button is this computer's Bluetooth address
// in reverse. Returns it packed into the integer that macOS's pairing call takes.
uint64_t wm_sync_pairing_key(const uint8_t host_address[6]);

#endif
