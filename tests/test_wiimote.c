// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the protocol code against a simulated Wiimote.
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../src/wiimote.h"

#define MAX_SENT 32

typedef struct {
    uint8_t reports[MAX_SENT][32];
    size_t lengths[MAX_SENT];
    int count;
} sent_log;

static void record(void *ctx, const uint8_t *report, size_t len) {
    sent_log *log = ctx;
    assert(log->count < MAX_SENT);
    assert(len <= sizeof(log->reports[0]));
    memcpy(log->reports[log->count], report, len);
    log->lengths[log->count] = len;
    log->count++;
}

static const uint8_t *last_sent(const sent_log *log) {
    assert(log->count > 0);
    return log->reports[log->count - 1];
}

static void feed_status(wiimote *w, bool extension) {
    const uint8_t report[] = {0x20, 0x00, 0x00, (uint8_t)(extension ? 0x02 : 0x00), 0x00, 0x00, 0xC0};
    assert(!wm_handle_report(w, report, sizeof(report)));
}

static void feed_write_ack(wiimote *w, uint8_t error) {
    const uint8_t report[] = {0x22, 0x00, 0x00, 0x16, error};
    assert(!wm_handle_report(w, report, sizeof(report)));
}

static void feed_ext_id(wiimote *w, const uint8_t id[6]) {
    uint8_t report[22] = {0x21, 0x00, 0x00, 0x50, 0x00, 0xFA};
    memcpy(&report[6], id, 6);
    assert(!wm_handle_report(w, report, sizeof(report)));
}

// Takes a Wiimote from start to streaming, checking each message it sends.
static void connect_with(wiimote *w, sent_log *log, const uint8_t id[6]) {
    wm_init(w, record, log);
    wm_start(w);
    assert(log->count == 2);
    assert(log->reports[0][0] == 0x11 && log->reports[0][1] == 0x10);
    assert(log->reports[1][0] == 0x15);

    feed_status(w, true);
    const uint8_t *init1 = last_sent(log);
    assert(init1[0] == 0x16 && init1[1] == 0x04 && init1[2] == 0xA4 && init1[4] == 0xF0 && init1[5] == 1 &&
           init1[6] == 0x55);
    assert(log->lengths[log->count - 1] == 22);

    feed_write_ack(w, 0);
    const uint8_t *init2 = last_sent(log);
    assert(init2[0] == 0x16 && init2[4] == 0xFB && init2[6] == 0x00);

    feed_write_ack(w, 0);
    const uint8_t *read = last_sent(log);
    assert(read[0] == 0x17 && read[2] == 0xA4 && read[4] == 0xFA && read[6] == 6);

    feed_ext_id(w, id);
    const uint8_t *mode = last_sent(log);
    assert(mode[0] == 0x12 && mode[1] == 0x04 && mode[2] == 0x35);
    assert(w->stage == WM_STAGE_RUNNING);
}

static const uint8_t GUITAR_ID[6] = {0x00, 0x00, 0xA4, 0x20, 0x01, 0x03};

// A guitar at rest: every button bit is high, because they are active low.
static void make_guitar_report(uint8_t report[22]) {
    memset(report, 0, 22);
    report[0] = 0x35;
    report[3] = 128, report[4] = 129, report[5] = 150;
    report[6] = 0x20, report[7] = 0x1F, report[8] = 0x0F, report[9] = 0x0F;
    report[10] = 0xFF, report[11] = 0xFF;
}

static void test_guitar_detection_and_rest_state(void) {
    wiimote w;
    sent_log log = {0};
    connect_with(&w, &log, GUITAR_ID);
    assert(w.ext == WM_EXT_GUITAR);

    uint8_t report[22];
    make_guitar_report(report);
    assert(wm_handle_report(&w, report, sizeof(report)));
    wm_guitar g = w.guitar;
    assert(!g.green && !g.red && !g.yellow && !g.blue && !g.orange);
    assert(!g.strum_up && !g.strum_down && !g.plus && !g.minus);
    assert(g.whammy == 0x0F && g.stick_x == 0x20 && g.stick_y == 0x1F);
    assert(w.accel_x == 128 && w.accel_y == 129 && w.accel_z == 150);
    assert(w.input_reports == 1);
}

static void test_each_guitar_control_alone(void) {
    wiimote w;
    sent_log log = {0};
    connect_with(&w, &log, GUITAR_ID);

    static const struct {
        int byte;
        uint8_t bit;
        size_t field;
    } controls[] = {
        {11, 0x10, offsetof(wm_guitar, green)},    {11, 0x40, offsetof(wm_guitar, red)},
        {11, 0x08, offsetof(wm_guitar, yellow)},   {11, 0x20, offsetof(wm_guitar, blue)},
        {11, 0x80, offsetof(wm_guitar, orange)},   {11, 0x01, offsetof(wm_guitar, strum_up)},
        {10, 0x40, offsetof(wm_guitar, strum_down)}, {10, 0x10, offsetof(wm_guitar, minus)},
        {10, 0x04, offsetof(wm_guitar, plus)},
    };
    static const size_t all_fields[] = {
        offsetof(wm_guitar, green),    offsetof(wm_guitar, red),        offsetof(wm_guitar, yellow),
        offsetof(wm_guitar, blue),     offsetof(wm_guitar, orange),     offsetof(wm_guitar, strum_up),
        offsetof(wm_guitar, strum_down), offsetof(wm_guitar, minus),    offsetof(wm_guitar, plus),
    };

    for (size_t c = 0; c < sizeof(controls) / sizeof(controls[0]); c++) {
        uint8_t report[22];
        make_guitar_report(report);
        report[controls[c].byte] &= (uint8_t)~controls[c].bit;
        assert(wm_handle_report(&w, report, sizeof(report)));
        for (size_t f = 0; f < sizeof(all_fields) / sizeof(all_fields[0]); f++) {
            bool value = *(bool *)((char *)&w.guitar + all_fields[f]);
            assert(value == (all_fields[f] == controls[c].field));
        }
    }
}

static void test_whammy_range(void) {
    wiimote w;
    sent_log log = {0};
    connect_with(&w, &log, GUITAR_ID);
    for (uint8_t value = 0; value <= 31; value++) {
        uint8_t report[22];
        make_guitar_report(report);
        report[9] = value | 0xE0; // the unused high bits must be ignored
        assert(wm_handle_report(&w, report, sizeof(report)));
        assert(w.guitar.whammy == value);
    }
}

static void test_other_addons_are_identified(void) {
    static const struct {
        uint8_t id[6];
        wm_ext_type type;
    } cases[] = {
        {{0x01, 0x00, 0xA4, 0x20, 0x01, 0x03}, WM_EXT_DRUMS},
        {{0x03, 0x00, 0xA4, 0x20, 0x01, 0x03}, WM_EXT_TURNTABLE},
        {{0x00, 0x00, 0xA4, 0x20, 0x00, 0x00}, WM_EXT_NUNCHUK},
        {{0x00, 0x00, 0xA4, 0x20, 0x01, 0x01}, WM_EXT_CLASSIC},
        {{0x01, 0x00, 0xA4, 0x20, 0x01, 0x01}, WM_EXT_CLASSIC_PRO},
        {{0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}, WM_EXT_UNKNOWN},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        wiimote w;
        sent_log log = {0};
        connect_with(&w, &log, cases[i].id);
        assert(w.ext == cases[i].type);

        uint8_t report[22];
        make_guitar_report(report);
        assert(wm_handle_report(&w, report, sizeof(report)));
        assert(!w.guitar.green && w.guitar.whammy == 0); // only guitars decode as a guitar
        assert(w.ext_raw[3] == 0x0F);
    }
}

static void test_no_extension_streams_buttons_and_motion(void) {
    wiimote w;
    sent_log log = {0};
    wm_init(&w, record, &log);
    wm_start(&w);
    feed_status(&w, false);
    const uint8_t *mode = last_sent(&log);
    assert(mode[0] == 0x12 && mode[2] == 0x31);
    assert(w.ext == WM_EXT_NONE && w.stage == WM_STAGE_RUNNING);

    const uint8_t report[] = {0x31, 0x00, WM_BTN_A, 10, 20, 30};
    assert(wm_handle_report(&w, report, sizeof(report)));
    assert(w.buttons == WM_BTN_A);
    assert(w.accel_x == 10 && w.accel_y == 20 && w.accel_z == 30);
}

static void test_unplugging_and_replugging(void) {
    wiimote w;
    sent_log log = {0};
    connect_with(&w, &log, GUITAR_ID);

    uint8_t report[22];
    make_guitar_report(report);
    report[11] &= (uint8_t)~0x10;
    wm_handle_report(&w, report, sizeof(report));
    assert(w.guitar.green);

    feed_status(&w, false); // the Wiimote announces the unplug by itself
    assert(w.ext == WM_EXT_NONE && !w.guitar.green);
    assert(last_sent(&log)[2] == 0x31);

    feed_status(&w, true); // and the replug
    assert(w.stage == WM_STAGE_EXT_INIT_1);
    feed_write_ack(&w, 0);
    feed_write_ack(&w, 0);
    feed_ext_id(&w, GUITAR_ID);
    assert(w.ext == WM_EXT_GUITAR && w.stage == WM_STAGE_RUNNING);
    assert(last_sent(&log)[2] == 0x35);
}

static void test_failures_do_not_advance_and_tick_retries(void) {
    wiimote w;
    sent_log log = {0};
    wm_init(&w, record, &log);
    wm_start(&w);
    feed_status(&w, true);
    feed_write_ack(&w, 0x03); // a failed write
    assert(w.stage == WM_STAGE_EXT_INIT_1);

    int before = log.count;
    wm_tick(&w); // first sighting of this stage
    assert(log.count == before);
    wm_tick(&w);
    assert(log.count == before);
    wm_tick(&w); // still stuck, so start over
    assert(log.count == before + 1);
    assert(last_sent(&log)[0] == 0x15);
    assert(w.stage == WM_STAGE_STATUS);

    // A failed ID read must not be taken as an add-on either.
    feed_status(&w, true);
    feed_write_ack(&w, 0);
    feed_write_ack(&w, 0);
    uint8_t failed_read[22] = {0x21, 0x00, 0x00, 0x57, 0x00, 0xFA};
    wm_handle_report(&w, failed_read, sizeof(failed_read));
    assert(w.stage == WM_STAGE_EXT_READ_ID);

    // Neither must a read of some other address.
    uint8_t other_read[22] = {0x21, 0x00, 0x00, 0x50, 0x00, 0x20};
    wm_handle_report(&w, other_read, sizeof(other_read));
    assert(w.stage == WM_STAGE_EXT_READ_ID);
}

static void test_running_wiimote_is_not_restarted_by_tick(void) {
    wiimote w;
    sent_log log = {0};
    connect_with(&w, &log, GUITAR_ID);
    int before = log.count;
    for (int i = 0; i < 10; i++) {
        wm_tick(&w);
    }
    assert(log.count == before);
}

static void test_acks_for_other_reports_are_ignored(void) {
    wiimote w;
    sent_log log = {0};
    wm_init(&w, record, &log);
    wm_start(&w);
    feed_status(&w, true);
    int before = log.count;
    const uint8_t led_ack[] = {0x22, 0x00, 0x00, 0x11, 0x00};
    const uint8_t mode_ack[] = {0x22, 0x00, 0x00, 0x12, 0x00};
    wm_handle_report(&w, led_ack, sizeof(led_ack));
    wm_handle_report(&w, mode_ack, sizeof(mode_ack));
    assert(log.count == before && w.stage == WM_STAGE_EXT_INIT_1);
}

static void test_short_and_unknown_reports_are_harmless(void) {
    wiimote w;
    sent_log log = {0};
    connect_with(&w, &log, GUITAR_ID);
    uint8_t report[22];
    make_guitar_report(report);
    for (size_t len = 0; len < sizeof(report); len++) {
        wm_handle_report(&w, report, len);
    }
    const uint8_t ids[] = {0x20, 0x21, 0x22, 0x30, 0x31, 0x37, 0x3D, 0xFF};
    for (size_t i = 0; i < sizeof(ids); i++) {
        for (size_t len = 0; len < 6; len++) {
            uint8_t data[6] = {ids[i]};
            wm_handle_report(&w, data, len);
        }
    }
}

static void test_glitched_guitar_frames_are_ignored(void) {
    wiimote w;
    sent_log log = {0};
    connect_with(&w, &log, GUITAR_ID);

    uint8_t report[22];
    make_guitar_report(report);
    assert(wm_handle_report(&w, report, sizeof(report)));
    assert(w.guitar.whammy == 0x0F);

    // All zeros would otherwise read as every button held and the whammy at 0.
    uint8_t zeros[22] = {0x35};
    assert(wm_handle_report(&w, zeros, sizeof(zeros)));
    assert(!w.guitar.green && !w.guitar.orange && !w.guitar.strum_up && !w.guitar.plus);
    assert(w.guitar.whammy == 0x0F);
    assert(w.invalid_ext_frames == 1);

    make_guitar_report(report);
    report[11] &= (uint8_t)~0x10;
    assert(wm_handle_report(&w, report, sizeof(report)));
    assert(w.guitar.green && w.invalid_ext_frames == 1);
}

static void test_guitar_packet(void) {
    uint8_t highest = 0;
    wm_guitar g = {.green = true, .orange = true, .strum_down = true, .minus = true, .whammy = 15, .stick_x = 33};
    wm_bridge_packet p = wm_guitar_to_packet(&g, &highest);
    assert(p.magic == WM_BRIDGE_MAGIC);
    assert(p.buttons == ((1u << 4) | (1u << 8) | (1u << 1) | (1u << 10)));
    assert(p.whammy == 0 && p.tilt == 0);

    // Each control has its own bit, as Bridge.cs expects.
    wm_guitar each[] = {{.strum_up = true, .stick_x = 33}, {.strum_down = true, .stick_x = 33},
                        {.green = true, .stick_x = 33},    {.red = true, .stick_x = 33},
                        {.yellow = true, .stick_x = 33},   {.blue = true, .stick_x = 33},
                        {.orange = true, .stick_x = 33},   {.plus = true, .stick_x = 33},
                        {.minus = true, .stick_x = 33}};
    const int bits[] = {0, 1, 4, 5, 6, 7, 8, 9, 10};
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        assert(wm_guitar_to_packet(&each[i], &highest).buttons == 1u << bits[i]);
    }

    wm_guitar whammy = {.whammy = 16};
    assert(wm_guitar_to_packet(&whammy, &highest).whammy == 0); // rest noise
    whammy.whammy = 21;
    assert(wm_guitar_to_packet(&whammy, &highest).whammy == 0.5f);
    whammy.whammy = 26;
    assert(wm_guitar_to_packet(&whammy, &highest).whammy == 1.0f);
    whammy.whammy = 31; // a guitar with more travel rescales
    assert(wm_guitar_to_packet(&whammy, &highest).whammy == 1.0f);
    whammy.whammy = 26;
    assert(wm_guitar_to_packet(&whammy, &highest).whammy < 1.0f);
    wm_guitar centered = {.stick_x = 33};
    assert(wm_guitar_to_packet(&centered, &highest).buttons == 0);
    wm_guitar left = {.stick_x = 4};
    assert(wm_guitar_to_packet(&left, &highest).buttons == 1u << 2);
    wm_guitar right = {.stick_x = 62};
    assert(wm_guitar_to_packet(&right, &highest).buttons == 1u << 3);
    wm_guitar drift = {.stick_x = 20};
    assert(wm_guitar_to_packet(&drift, &highest).buttons == 0);
    wm_guitar drift_right = {.stick_x = 44};
    assert(wm_guitar_to_packet(&drift_right, &highest).buttons == 0);

    assert(sizeof(wm_bridge_packet) == 16);
}

static bool feed_tilt(wm_tilt *tilt, uint8_t value, int reports) {
    bool active = false;
    for (int i = 0; i < reports; i++) {
        active = wm_tilt_update(tilt, value);
    }
    return active;
}

static void test_tilt(void) {
    wm_tilt tilt = {0};
    assert(!feed_tilt(&tilt, 128, 50)); // level
    assert(!feed_tilt(&tilt, 121, 100)); // the neck held a bit high while playing
    assert(!feed_tilt(&tilt, 155, 100)); // pointing down

    tilt = (wm_tilt){0};
    feed_tilt(&tilt, 128, 50);
    assert(!feed_tilt(&tilt, 90, 3)); // a hard strum's jolt
    assert(!feed_tilt(&tilt, 128, 30));
    for (int i = 0; i < 100; i++) { // repeated strumming averages out near level
        assert(!wm_tilt_update(&tilt, i % 2 ? 105 : 151));
    }

    tilt = (wm_tilt){0};
    feed_tilt(&tilt, 128, 50);
    assert(feed_tilt(&tilt, 102, 40)); // raised
    assert(tilt.active);
    assert(feed_tilt(&tilt, 111, 40)); // dipping between the two limits holds it
    assert(!feed_tilt(&tilt, 128, 40)); // back to level releases

    tilt = (wm_tilt){0};
    feed_tilt(&tilt, 128, 50);
    assert(!feed_tilt(&tilt, 112, 100)); // about 38 degrees is no longer enough

    tilt = (wm_tilt){0};
    assert(!feed_tilt(&tilt, 102, 11)); // starting off raised still takes the hold time
    assert(feed_tilt(&tilt, 102, 1));
}

static void test_sync_pairing_key_is_reversed_address(void) {
    const uint8_t address[6] = {0x00, 0x1B, 0x63, 0x84, 0x45, 0xE6};
    // The PIN bytes are E6 45 84 63 1B 00, first byte lowest in the integer.
    assert(wm_sync_pairing_key(address) == 0x00001B638445E6ULL);
}

int main(void) {
    test_guitar_detection_and_rest_state();
    test_each_guitar_control_alone();
    test_whammy_range();
    test_other_addons_are_identified();
    test_no_extension_streams_buttons_and_motion();
    test_unplugging_and_replugging();
    test_failures_do_not_advance_and_tick_retries();
    test_running_wiimote_is_not_restarted_by_tick();
    test_acks_for_other_reports_are_ignored();
    test_short_and_unknown_reports_are_harmless();
    test_glitched_guitar_frames_are_ignored();
    test_guitar_packet();
    test_tilt();
    test_sync_pairing_key_is_reversed_address();
    printf("All tests passed\n");
    return 0;
}
