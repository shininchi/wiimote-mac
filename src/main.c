// SPDX-License-Identifier: GPL-3.0-or-later
// Terminal tool: shows what a Wiimote and its add-on send to this Mac and how steady
// the connection is, and can send a guitar to YARG.
#include <CoreFoundation/CoreFoundation.h>

#include <crt_externs.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pair.h"
#include "session.h"
#include "wiimote.h"

// Private libSystem call, also used by Qt and LLVM. May be missing on other versions.
extern int responsibility_spawnattrs_setdisclaim(posix_spawnattr_t *attributes, int disclaim) __attribute__((weak_import));

#define DISCLAIMED_ENV "WIIMOTE_MAC_DISCLAIMED"
#define DISPLAY_INTERVAL 0.05
// Reports should arrive every 10ms. A gap this long would be felt while playing.
#define NOTICEABLE_GAP 0.050

// Upper limits, in ms, of the time-between-reports buckets. The last is open ended.
#define GAP_BUCKETS 7
static const double gap_bucket_limits[GAP_BUCKETS - 1] = {7, 12, 20, 35, 50, 100};

typedef struct {
    bool seen;
    uint8_t min, max;
} range;

static struct {
    const wiimote *wm;
    bool debug;
    bool show_live_line;
    bool line_open;
    bool tilted;
    bool seen_tilt;

    double connected_at, last_report_at, longest_gap;
    double total_connected_time;
    uint64_t noticeable_gaps, connections, send_problems, invalid_frames_counted;
    uint64_t gap_counts[GAP_BUCKETS];

    bool seen_green, seen_red, seen_yellow, seen_blue, seen_orange;
    bool seen_strum_up, seen_strum_down, seen_plus, seen_minus;
    range whammy, stick_x, stick_y, accel_x, accel_y, accel_z;
} app;

static double now_seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void track(range *r, uint8_t value) {
    if (!r->seen || value < r->min) r->min = value;
    if (!r->seen || value > r->max) r->max = value;
    r->seen = true;
}

// Ends the live status line so that a message can be printed on its own line.
static void end_line(void) {
    if (app.line_open) {
        printf("\n");
        app.line_open = false;
    }
}

static void print_hex(const char *label, const uint8_t *data, size_t len) {
    end_line();
    printf("%s", label);
    for (size_t i = 0; i < len; i++) {
        printf(" %02X", data[i]);
    }
    printf("\n");
}

static void track_guitar(const wm_guitar *g) {
    app.seen_green |= g->green;
    app.seen_red |= g->red;
    app.seen_yellow |= g->yellow;
    app.seen_blue |= g->blue;
    app.seen_orange |= g->orange;
    app.seen_strum_up |= g->strum_up;
    app.seen_strum_down |= g->strum_down;
    app.seen_plus |= g->plus;
    app.seen_minus |= g->minus;
    track(&app.whammy, g->whammy);
    track(&app.stick_x, g->stick_x);
    track(&app.stick_y, g->stick_y);
}

static void print_status_line(void) {
    const wiimote *w = app.wm;
    char line[160];
    if (w->ext == WM_EXT_GUITAR) {
        const wm_guitar *g = &w->guitar;
        snprintf(line, sizeof(line), "%c %c %c %c %c | %-4s | whammy %2u | %c %c | %-4s | accel %3u %3u %3u",
                 g->green ? 'G' : '.', g->red ? 'R' : '.', g->yellow ? 'Y' : '.', g->blue ? 'B' : '.',
                 g->orange ? 'O' : '.', g->strum_up ? "UP" : g->strum_down ? "DOWN" : "-", g->whammy,
                 g->minus ? '-' : '.', g->plus ? '+' : '.', app.tilted ? "TILT" : "-", w->accel_x, w->accel_y,
                 w->accel_z);
    } else if (w->ext == WM_EXT_NONE) {
        snprintf(line, sizeof(line), "buttons %04X | accel %3u %3u %3u", w->buttons, w->accel_x, w->accel_y,
                 w->accel_z);
    } else {
        int n = snprintf(line, sizeof(line), "raw");
        for (int i = 0; i < 6; i++) {
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %02X", w->ext_raw[i]);
        }
        snprintf(line + n, sizeof(line) - (size_t)n, " | accel %3u %3u %3u", w->accel_x, w->accel_y, w->accel_z);
    }
    printf("\r%-70s", line);
    fflush(stdout);
    app.line_open = true;
}

static void on_connected(void *ctx) {
    (void)ctx;
    app.connections++;
    app.connected_at = now_seconds();
    app.last_report_at = 0;
    end_line();
    printf("Wiimote connected.\n");
}

static void on_disconnected(void *ctx) {
    (void)ctx;
    app.total_connected_time += now_seconds() - app.connected_at;
    app.connected_at = 0;
    end_line();
    printf("Wiimote disconnected. Waiting for it to come back...\n");
}

static void on_extension_changed(void *ctx, const wiimote *wm) {
    (void)ctx;
    end_line();
    printf("Plugged into the Wiimote: %s\n", wm_ext_name(wm->ext));
    if (wm->ext == WM_EXT_UNKNOWN) {
        print_hex("  Add-on ID:", wm->ext_id, sizeof(wm->ext_id));
    } else if (wm->ext != WM_EXT_GUITAR && wm->ext != WM_EXT_NONE) {
        printf("  This tool only decodes guitars so far, so it will show raw data.\n");
    }
}

static void on_input(void *ctx, const wiimote *wm, bool tilted) {
    (void)ctx;
    app.wm = wm;
    app.tilted = tilted;
    app.seen_tilt |= tilted;

    double now = now_seconds();
    if (app.last_report_at > 0) {
        double gap = now - app.last_report_at;
        if (gap > app.longest_gap) app.longest_gap = gap;
        if (gap > NOTICEABLE_GAP) app.noticeable_gaps++;
        int bucket = 0;
        while (bucket < GAP_BUCKETS - 1 && gap * 1000 > gap_bucket_limits[bucket]) {
            bucket++;
        }
        app.gap_counts[bucket]++;
    }
    app.last_report_at = now;

    if (wm->ext == WM_EXT_GUITAR) {
        track_guitar(&wm->guitar);
    }
    track(&app.accel_x, wm->accel_x);
    track(&app.accel_y, wm->accel_y);
    track(&app.accel_z, wm->accel_z);
}

static void on_report_seen(void *ctx, bool outgoing, const uint8_t *report, size_t len) {
    (void)ctx;
    if (app.debug) {
        print_hex(outgoing ? "->" : "<-", report, len);
    }
}

static void on_problem(void *ctx, const char *message) {
    (void)ctx;
    app.send_problems++;
    end_line();
    printf("%s\n", message);
}

static void on_pair_status(pair_event event, const char *message) {
    (void)event;
    end_line();
    printf("%s\n", message);
}

static void display_timer(CFRunLoopTimerRef timer, void *info) {
    (void)timer, (void)info;
    if (app.wm && session_is_connected() && app.show_live_line) {
        print_status_line();
    }
}

static void add_timer(double interval, CFRunLoopTimerCallBack callback) {
    CFRunLoopTimerRef timer =
        CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + interval, interval, 0, 0, callback, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopDefaultMode);
    CFRelease(timer);
}

static void handle_signal(int signal_number) {
    (void)signal_number;
    CFRunLoopStop(CFRunLoopGetMain());
}

static void stop_timer(CFRunLoopTimerRef timer, void *info) {
    (void)timer, (void)info;
    CFRunLoopStop(CFRunLoopGetMain());
}

static void print_control(const char *name, bool seen) {
    printf("  [%c] %s\n", seen ? 'x' : ' ', name);
}

static void print_range(const char *name, const range *r) {
    if (r->seen) {
        printf("  %-9s %3u to %3u\n", name, r->min, r->max);
    }
}

static void print_summary(void) {
    end_line();
    printf("\n--- Summary ---\n");
    if (app.connections == 0) {
        printf("No Wiimote was found.\n");
        printf("If you haven't paired it yet, run this with --pair.\n");
        return;
    }

    uint64_t reports = session_input_reports();
    double connected = app.total_connected_time;
    if (app.connected_at > 0) {
        connected += now_seconds() - app.connected_at;
    }
    printf("Connection\n");
    printf("  Times connected:    %llu\n", (unsigned long long)app.connections);
    printf("  Time connected:     %.1f s\n", connected);
    printf("  Input reports:      %llu (%.0f per second, should be about 100)\n", (unsigned long long)reports,
           connected > 0 ? (double)reports / connected : 0);
    printf("  Longest gap:        %.0f ms\n", app.longest_gap * 1000);
    printf("  Gaps over %.0f ms:    %llu\n", NOTICEABLE_GAP * 1000, (unsigned long long)app.noticeable_gaps);
    printf("  Problems:           %llu\n", (unsigned long long)app.send_problems);
    printf("  Glitched frames:    %llu\n", (unsigned long long)(app.wm ? app.wm->invalid_ext_frames : 0));

    printf("\nTime between reports\n");
    for (int i = 0; i < GAP_BUCKETS; i++) {
        double share = reports > 0 ? 100.0 * (double)app.gap_counts[i] / (double)reports : 0;
        if (i < GAP_BUCKETS - 1) {
            printf("  up to %3.0f ms  %8llu  %5.1f%%\n", gap_bucket_limits[i], (unsigned long long)app.gap_counts[i], share);
        } else {
            printf("  over  %3.0f ms  %8llu  %5.1f%%\n", gap_bucket_limits[i - 1], (unsigned long long)app.gap_counts[i], share);
        }
    }

    if (app.whammy.seen) {
        printf("\nGuitar controls seen\n");
        print_control("Green", app.seen_green);
        print_control("Red", app.seen_red);
        print_control("Yellow", app.seen_yellow);
        print_control("Blue", app.seen_blue);
        print_control("Orange", app.seen_orange);
        print_control("Strum up", app.seen_strum_up);
        print_control("Strum down", app.seen_strum_down);
        print_control("Minus / Select", app.seen_minus);
        print_control("Plus / Start", app.seen_plus);
        print_control("Tilt", app.seen_tilt);
        printf("\nRanges (a control that was moved should show a spread)\n");
        print_range("Whammy", &app.whammy);
        print_range("Stick X", &app.stick_x);
        print_range("Stick Y", &app.stick_y);
    } else {
        printf("\nRanges\n");
    }
    print_range("Accel X", &app.accel_x);
    print_range("Accel Y", &app.accel_y);
    print_range("Accel Z", &app.accel_z);
}

static void print_usage(const char *program) {
    printf("Usage: %s [--pair] [--yarg] [--no-display] [--debug] [--seconds N]\n\n", program);
    printf("  --yarg       Send the guitar to YARG, through the add-on\n");
    printf("  --pair       Pair a new Wiimote first. Only needed once per Wiimote\n");
    printf("  --no-display Hide the live status line, to measure without its overhead\n");
    printf("  --debug      Print the setup messages sent to and from the Wiimote\n");
    printf("  --seconds N  Stop after N seconds instead of waiting for Ctrl-C\n");
}

// macOS checks Bluetooth permission against the app that launched a process, such
// as the terminal, and kills the process if that app doesn't declare Bluetooth use.
// Restarting with the launcher's responsibility disclaimed makes macOS check this
// tool's own Info.plist instead. Keeps the same process ID and terminal.
static void become_responsible_for_permissions(char **argv) {
    if (getenv(DISCLAIMED_ENV) || !responsibility_spawnattrs_setdisclaim) {
        return;
    }
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) {
        return;
    }
    setenv(DISCLAIMED_ENV, "1", 1);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETEXEC);
    responsibility_spawnattrs_setdisclaim(&attributes, 1);
    pid_t pid;
    int error = posix_spawn(&pid, path, NULL, &attributes, argv, *_NSGetEnviron());
    // Only reached if the restart failed.
    posix_spawnattr_destroy(&attributes);
    printf("Could not restart to request Bluetooth permission (%s)\n", strerror(error));
}

int main(int argc, char **argv) {
    double seconds = 0;
    bool pair = false;
    bool no_display = false;
    bool use_yarg = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pair") == 0) {
            pair = true;
        } else if (strcmp(argv[i], "--yarg") == 0) {
            use_yarg = true;
        } else if (strcmp(argv[i], "--no-display") == 0) {
            no_display = true;
        } else if (strcmp(argv[i], "--debug") == 0) {
            app.debug = true;
        } else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = atof(argv[++i]);
        } else {
            print_usage(argv[0]);
            return strcmp(argv[i], "--help") == 0 ? 0 : 2;
        }
    }
    if (pair) {
        become_responsible_for_permissions(argv);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    app.show_live_line = isatty(STDOUT_FILENO) && !no_display;
    if (use_yarg) {
        printf("Sending the guitar to YARG.\n");
    }

    session_callbacks callbacks = {
        .connected = on_connected,
        .disconnected = on_disconnected,
        .extension_changed = on_extension_changed,
        .input = on_input,
        .report_seen = on_report_seen,
        .problem = on_problem,
    };
    IOReturn started = session_start(&callbacks, use_yarg, CFRunLoopGetMain());
    if (started == kIOReturnNotPermitted) {
        printf("macOS blocked access to input devices.\n");
        printf("Allow your terminal app in System Settings > Privacy & Security > Input Monitoring,\n");
        printf("then quit and reopen it and run this again.\n");
        return 1;
    }
    if (started != kIOReturnSuccess) {
        printf("Could not start listening for devices (error 0x%08X)\n", started);
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    add_timer(DISPLAY_INTERVAL, display_timer);
    if (seconds > 0) {
        add_timer(seconds, stop_timer);
    }

    if (pair) {
        printf("Ctrl-C to finish.\n");
        pair_start(on_pair_status);
    } else {
        printf("Waiting for a paired Wiimote. Press any button on it to connect. Ctrl-C to finish.\n");
    }
    CFRunLoopRun();

    print_summary();
    return 0;
}
