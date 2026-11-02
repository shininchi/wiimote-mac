// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PAIR_H
#define PAIR_H

#include <stdbool.h>

typedef enum {
    PAIR_PROGRESS, // Still going. Show the message.
    PAIR_PAIRED,   // Done. The Wiimote will now connect.
    PAIR_STOPPED,  // Gave up because of a problem the message explains.
} pair_event;

typedef void (*pair_status_fn)(pair_event event, const char *message);

// Starts searching for a Wiimote in sync mode and pairs with the first one found.
// Runs on the main run loop, and reports on it too.
void pair_start(pair_status_fn status);

// Cancels pairing if it's in progress.
void pair_stop(void);

#endif
