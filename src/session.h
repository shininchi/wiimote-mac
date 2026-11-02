// SPDX-License-Identifier: GPL-3.0-or-later
// Finds paired Wiimotes, keeps them connected and optionally sends their guitars to YARG.
// Shared by the terminal tool and the menu bar app.
#ifndef SESSION_H
#define SESSION_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOReturn.h>

#include "wiimote.h"

#define SESSION_MAX_WIIMOTES 4

// All of these are called on the run loop passed to session_start. A Wiimote's player
// number starts at 0, and it keeps that number until it disconnects.
typedef struct {
    void *ctx;
    void (*connected)(void *ctx, int player);
    void (*disconnected)(void *ctx, int player);
    void (*extension_changed)(void *ctx, int player, const wiimote *wm);
    void (*input)(void *ctx, int player, const wiimote *wm, bool tilted);
    void (*report_seen)(void *ctx, int player, bool outgoing, const uint8_t *report, size_t len); // setup messages only
    void (*problem)(void *ctx, const char *message);
} session_callbacks;

typedef struct {
    bool connected;
    wm_ext_type extension;
    char address[18]; // such as 00:11:22:33:44:55
} session_wiimote;

// Starts listening for up to max_wiimotes Wiimotes. Returns kIOReturnNotPermitted if
// macOS is blocking access to input devices.
IOReturn session_start(const session_callbacks *callbacks, bool send_to_yarg, int max_wiimotes, CFRunLoopRef run_loop);

// These can be read from any thread.
session_wiimote session_status(int player);
bool session_any_connected(void);
uint64_t session_input_reports(void);

// Whether anything received the guitar recently. Only meaningful while a guitar is
// sending, because the check relies on the system reporting that nothing is listening.
bool session_yarg_listening(void);

#endif
