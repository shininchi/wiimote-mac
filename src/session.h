// Finds a paired Wiimote, keeps it connected and optionally sends its guitar to YARG.
// Shared by the terminal tool and the menu bar app.
#ifndef SESSION_H
#define SESSION_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOReturn.h>

#include "wiimote.h"

// All of these are called on the run loop passed to session_start.
typedef struct {
    void *ctx;
    void (*connected)(void *ctx);
    void (*disconnected)(void *ctx);
    void (*extension_changed)(void *ctx, const wiimote *wm);
    void (*input)(void *ctx, const wiimote *wm, bool tilted);
    void (*report_seen)(void *ctx, bool outgoing, const uint8_t *report, size_t len); // setup messages only
    void (*problem)(void *ctx, const char *message);
} session_callbacks;

// Starts listening for a Wiimote. Returns kIOReturnNotPermitted if macOS is
// blocking access to input devices.
IOReturn session_start(const session_callbacks *callbacks, bool send_to_yarg, CFRunLoopRef run_loop);

// These can be read from any thread.
bool session_is_connected(void);
wm_ext_type session_extension(void);
uint64_t session_input_reports(void);

// Whether anything received the guitar recently. Only meaningful while a guitar is
// sending, because the check relies on the system reporting that nothing is listening.
bool session_yarg_listening(void);

#endif
