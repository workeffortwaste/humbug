/* Headless LVGL v9.3 RGB565 renderer and input-state checks. */
#include "humbug_ui.h"
#include "humbug_fonts.h"
#include "humbug_art.h"
#include "sample_rooms.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t output[HUMBUG_WIDTH * HUMBUG_HEIGHT];
static uint16_t buffer[HUMBUG_WIDTH * HUMBUG_HEIGHT];
static unsigned flushes, blocks, exits, theme_events, option_events, resets;
static uint64_t id, token;

static void flush(lv_display_t* display, const lv_area_t* area, uint8_t* bytes) {
    int width = area->x2 - area->x1 + 1;
    unsigned stride = lv_draw_buf_width_to_stride((uint32_t)width, LV_COLOR_FORMAT_RGB565);
    assert(area->x1 >= 0 && area->y1 >= 0 && area->x2 < HUMBUG_WIDTH && area->y2 < HUMBUG_HEIGHT);
    for (int y = area->y1; y <= area->y2; ++y)
        memcpy(output + y * HUMBUG_WIDTH + area->x1, bytes + (size_t)(y - area->y1) * stride,
               (size_t)width * 2);
    ++flushes;
    lv_display_flush_ready(display);
}

static void action(void* user, humbug_event event, uint64_t room_id, uint64_t track_token, size_t theme) {
    (void)user;
    if (event == HUMBUG_EVENT_BLOCK) {
        ++blocks;
        id = room_id;
        token = track_token;
    }
    if (event == HUMBUG_EVENT_EXIT)
        ++exits;
    if (event == HUMBUG_EVENT_OPTIONS_CHANGED)
        ++option_events;
    if (event == HUMBUG_EVENT_RESET_CUSTOM)
        ++resets;
    if (event == HUMBUG_EVENT_THEME) {
        ++theme_events;
        assert(theme < humbug_ui_theme_count());
    }
}

static void snap(lv_display_t* display, const char* dir, const char* name) {
    char path[1024];
    assert(snprintf(path, sizeof path, "%s/%s.ppm", dir, name) < (int)sizeof path);
    lv_refr_now(display);
    FILE* fp = fopen(path, "wb");
    assert(fp);
    fprintf(fp, "P6\n%d %d\n255\n", HUMBUG_WIDTH, HUMBUG_HEIGHT);
    for (size_t i = 0; i < sizeof output / sizeof *output; ++i) {
        unsigned v = output[i];
        unsigned char px[] = {((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31};
        assert(fwrite(px, 1, 3, fp) == 3);
    }
    assert(!fclose(fp));
}

static uint32_t fake_ms;
static uint32_t clock_ms(void) {
    return fake_ms;
}

static void advance_ms(uint32_t ms) {
    fake_ms += ms;
    lv_timer_handler();
}

static unsigned role_at(humbug_ui* ui, unsigned x, unsigned y) {
    lv_draw_buf_t* draw = lv_canvas_get_draw_buf(humbug_ui_object(ui));
    return *(uint8_t*)lv_draw_buf_goto_xy(draw, x, y);
}

static void check_label(humbug_ui* ui, const char* label, int x, int y, int scale, unsigned role) {
    for (int n = 0; label[n]; ++n) {
        unsigned character = (unsigned char)label[n];
        if (character == '[')
            character = '(';
        else if (character == ']')
            character = ')';
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col) {
                const bool on = (hb_glyphs[character][row] & (16 >> col)) != 0;
                assert((role_at(ui, x + (6 * n + col) * scale, y + row * scale) == role) == on);
            }
    }
}

static void check_pips(humbug_ui* ui, unsigned count) {
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned x = 225 + 27 * i; x < 225 + 27 * i + 24; ++x)
            assert(role_at(ui, x, 130) == (i < count ? HB_ACTIVE : HB_SHELL));
        if (i < 2)
            for (unsigned x = 225 + 27 * i + 24; x < 225 + 27 * (i + 1); ++x)
                assert(role_at(ui, x, 130) == role_at(ui, 224, 130));
    }
}

/* Timed confirmations must stay ordered and prevent actions while the modal is open. */
static void queue_test(humbug_ui* ui, lv_display_t* display, const char* dir) {
    humbug_ui_notify_block(ui, sample_rooms[0].title, sample_rooms[0].artist);
    check_label(ui, "LAST CHRISTMAS", 17, 106, 2, HB_INK);
    check_label(ui, "1/1", 277, 89, 1, HB_ACTIVE_INK);
    check_pips(ui, 3);
    assert(!humbug_ui_can_block(ui));
    assert(!humbug_ui_press(ui, HUMBUG_KEY_BLOCK));
    assert(!humbug_ui_press(ui, HUMBUG_KEY_OPTIONS));

    advance_ms(999);
    check_pips(ui, 3);
    advance_ms(1);
    humbug_ui_notify_block(ui, "Next Track", "Second");
    humbug_ui_notify_block(ui, "Third Track", "Third");
    humbug_ui_notify_block(ui, "Fourth Track", "Fourth");
    humbug_ui_notify_block(ui, "Fifth Track", "Fifth");
    humbug_ui_notify_block(ui, "Sixth Track", "Sixth");
    check_label(ui, "LAST CHRISTMAS", 17, 106, 2, HB_INK);
    check_label(ui, "1/6", 277, 89, 1, HB_ACTIVE_INK);
    check_pips(ui, 2);
    snap(display, dir, "deck-dock");

    advance_ms(999);
    check_pips(ui, 2);
    advance_ms(1);
    check_pips(ui, 1);
    advance_ms(999);
    check_pips(ui, 1);
    advance_ms(1);
    check_pips(ui, 0);
    check_label(ui, "LAST CHRISTMAS", 17, 106, 2, HB_INK);
    advance_ms(999);
    check_pips(ui, 0);
    advance_ms(1);
    check_pips(ui, 3);
    check_label(ui, "NEXT TRACK", 17, 106, 2, HB_INK);
    check_label(ui, "2/6", 277, 89, 1, HB_ACTIVE_INK);
    assert(!humbug_ui_press(ui, HUMBUG_KEY_PLAYING));
    check_label(ui, "2/6", 277, 89, 1, HB_ACTIVE_INK);
    assert(humbug_ui_press(ui, HUMBUG_KEY_EXIT));
    check_label(ui, "THIRD TRACK", 17, 106, 2, HB_INK);
    assert(humbug_ui_press(ui, HUMBUG_KEY_ENTER));
    check_label(ui, "FOURTH TRACK", 17, 106, 2, HB_INK);
    assert(humbug_ui_press(ui, HUMBUG_KEY_DASH));
    advance_ms(5000);
    assert(!humbug_ui_press(ui, HUMBUG_KEY_ENTER));

    /* Fill the bounded queue, drop overflow, then refill across ring wrap. */
    for (int i = 1; i <= 64; ++i) {
        char label[16];
        snprintf(label, sizeof label, "TRACK %d", i);
        humbug_ui_notify_block(ui, label, "Artist");
    }
    humbug_ui_notify_block(ui, "OVERFLOW", "Artist");
    for (int i = 1; i <= 96; ++i) {
        char label[16];
        snprintf(label, sizeof label, "TRACK %d", i);
        check_label(ui, label, 17, 106, 2, HB_INK);
        if (i == 33)
            check_label(ui, "33/96", 269, 89, 1, HB_ACTIVE_INK);
        assert(humbug_ui_press(ui, HUMBUG_KEY_ENTER));
        if (i <= 32) {
            snprintf(label, sizeof label, "TRACK %d", i + 64);
            humbug_ui_notify_block(ui, label, "Artist");
        }
    }
    assert(!humbug_ui_press(ui, HUMBUG_KEY_ENTER));

    humbug_ui_notify_block(ui, "Final Track", "Artist");
    check_label(ui, "1/1", 277, 89, 1, HB_ACTIVE_INK);
    advance_ms(3999);
    check_pips(ui, 0);
    check_label(ui, "FINAL TRACK", 17, 106, 2, HB_INK);
    advance_ms(1);
    assert(!humbug_ui_press(ui, HUMBUG_KEY_ENTER));
}

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "--sample-rooms")) {
        for (size_t i = 0; i < sizeof sample_rooms / sizeof sample_rooms[0]; ++i) {
            const char* fields[] = {sample_rooms[i].name, sample_rooms[i].title, sample_rooms[i].artist};
            for (size_t j = 0; j < sizeof fields / sizeof fields[0]; ++j)
                if (fwrite(fields[j], 1, strlen(fields[j]) + 1, stdout) != strlen(fields[j]) + 1)
                    return 1;
        }
        return fflush(stdout) == 0 ? 0 : 1;
    }
    const char* dir = argc > 1 ? argv[1] : ".";
    const bool capture_theme_screenshots = argc == 3 && !strcmp(argv[2], "--theme-screenshots");
    if (argc > 3 || (argc == 3 && !capture_theme_screenshots)) {
        fprintf(stderr, "Usage: %s [output-directory] [--theme-screenshots] | --sample-rooms\n", argv[0]);
        return 2;
    }
    lv_init();
    lv_tick_set_cb(clock_ms);
    lv_display_t* display = lv_display_create(HUMBUG_WIDTH, HUMBUG_HEIGHT);
    assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof buffer, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t* screen = lv_display_get_screen_active(display);
    humbug_ui* ui = humbug_ui_create(screen, action, NULL);
    assert(ui);
    assert(!humbug_ui_press(ui, HUMBUG_KEY_BLOCK));
    assert(humbug_ui_set_rooms(ui, sample_rooms, 3));
    humbug_ui_set_status(ui, true, true);
    check_label(ui, "CONNECTED", 182, 8, 1, HB_MUTED);
    assert(role_at(ui, 175, 12) == HB_GREEN);
    for (unsigned x = 242; x < 247; ++x)
        assert(role_at(ui, x, 12) == HB_PANEL);
    check_label(ui, "BLOCKING", 253, 8, 1, HB_GREEN_INK);
    assert(role_at(ui, 246, 12) == HB_PANEL && role_at(ui, 247, 12) == HB_GREEN);
    for (unsigned x = 248; x < 253; ++x)
        assert(role_at(ui, x, 12) == HB_GREEN);
    for (unsigned x = 300; x < 304; ++x)
        assert(role_at(ui, x, 12) == HB_GREEN);
    assert(role_at(ui, 273, 6) == HB_GREEN && role_at(ui, 273, 16) == HB_GREEN);
    assert(role_at(ui, 306, 12) == role_at(ui, 306, 159));
    assert(role_at(ui, 307, 12) == HB_PANEL && role_at(ui, 307, 159) == HB_PANEL);
    humbug_ui_set_counts(ui, 25184, 1000);
    humbug_ui_set_latest(ui, sample_rooms[0].title, sample_rooms[0].artist);
    assert(!humbug_ui_can_block(ui));
    snap(display, dir, "dashboard");
    assert(flushes > 0);
    uint16_t saved = output[(HUMBUG_HEIGHT - 1) * HUMBUG_WIDTH];
    lv_draw_buf_t* draw = lv_canvas_get_draw_buf(humbug_ui_object(ui));
    uint8_t first[HUMBUG_WIDTH * HUMBUG_HEIGHT];
    for (unsigned y = 0; y < HUMBUG_HEIGHT; ++y)
        memcpy(first + y * HUMBUG_WIDTH, lv_draw_buf_goto_xy(draw, 0, y), HUMBUG_WIDTH);
    /* Retained themes must map to their original palette indices and render their own artwork. */
    static const size_t expected_theme_indices[] = {0, 7, 26, 14};
    static const char* const expected_theme_names[] = {"Side A", "Side B", "Blank Tape", "Bootleg"};
    assert(humbug_ui_theme_count() == sizeof expected_theme_indices / sizeof *expected_theme_indices);
    assert(humbug_ui_theme_count() == sizeof expected_theme_names / sizeof *expected_theme_names);
    for (size_t i = 0; i < humbug_ui_theme_count(); ++i) {
        assert(humbug_ui_theme_source_index(i) == expected_theme_indices[i]);
        assert(humbug_ui_theme_index_from_source(expected_theme_indices[i]) == i);
        assert(!strcmp(humbug_ui_theme_name(i), expected_theme_names[i]));
    }
    assert(humbug_ui_theme_source_index(humbug_ui_theme_count()) ==
           humbug_ui_theme_source_count());
    assert(humbug_ui_theme_index_from_source(2) == humbug_ui_theme_count());
    assert(humbug_ui_set_theme(ui, 1) && theme_events == 0);
    assert(humbug_ui_theme(ui) == 1);
    snap(display, dir, "black-chrome");
    assert(saved != output[(HUMBUG_HEIGHT - 1) * HUMBUG_WIDTH]);
    for (unsigned y = 0; y < HUMBUG_HEIGHT; ++y)
        for (unsigned x = 0; x < HUMBUG_WIDTH; ++x)
            if (x < 10 || x >= 189 || y < 30 || y >= 145)
                assert(first[y * HUMBUG_WIDTH + x] == role_at(ui, x, y));
    for (size_t i = 0; i < humbug_ui_theme_count(); ++i) {
        assert(humbug_ui_set_theme(ui, i));
        lv_refr_now(display);
        if (i == 0) {
            check_label(ui, "A", 31, 48, 2, HB_LABEL_INK);
            check_label(ui, "HUMBUG", 49, 48, 1, HB_LABEL_INK);
            check_label(ui, "SIDE A", 121, 48, 1, HB_LABEL_INK);
        } else if (i == HB_THEME_STYLE_SIDE_B) {
            check_label(ui, "B", 31, 48, 2, HB_LABEL_INK);
            check_label(ui, "HUMBUG", 49, 48, 1, HB_LABEL_INK);
            check_label(ui, "SIDE B", 121, 48, 1, HB_LABEL_INK);
        } else if (i == HB_THEME_STYLE_BLANK_TAPE) {
            check_label(ui, "C60", 143, 49, 1, HB_LABEL_INK);
        } else if (i == HB_THEME_STYLE_BOOTLEG) {
            check_label(ui, "DFCD", 49, 48, 1, HB_ACTIVE);
            check_label(ui, "SIDE A", 121, 48, 1, HB_LABEL_INK);
            assert(role_at(ui, 40, 45) == HB_CASSETTE_SCRATCH);
        }
        if (capture_theme_screenshots) {
            char name[48];
            assert(snprintf(name, sizeof name, "%03zu-dashboard", i) < (int)sizeof name);
            snap(display, dir, name);
            assert(humbug_ui_press(ui, HUMBUG_KEY_PLAYING));
            assert(snprintf(name, sizeof name, "%03zu-playing", i) < (int)sizeof name);
            snap(display, dir, name);
            assert(humbug_ui_press(ui, HUMBUG_KEY_DASH));
            humbug_ui_notify_block(ui, "Last Christmas", "Wham!");
            assert(!humbug_ui_can_show_block(ui));
            assert(snprintf(name, sizeof name, "%03zu-blocked", i) < (int)sizeof name);
            snap(display, dir, name);
            assert(humbug_ui_press(ui, HUMBUG_KEY_ENTER));
        }
    }
    assert(!humbug_ui_set_theme(ui, humbug_ui_theme_count()));
    assert(humbug_ui_set_theme(ui, 0));
    /* Independent switches and reset confirmation must not accidentally change other settings. */
    assert(humbug_ui_get_options(ui).christmas_blocklist);
    assert(humbug_ui_get_options(ui).custom_blocklist);
    assert(humbug_ui_get_options(ui).auto_stop);
    assert(humbug_ui_press(ui, HUMBUG_KEY_OPTIONS));
    assert(humbug_ui_theme(ui) == 0 && !humbug_ui_can_block(ui));
    assert(!humbug_ui_press(ui, HUMBUG_KEY_BLOCK));
    assert(!humbug_ui_press(ui, HUMBUG_KEY_RIGHT));
    snap(display, dir, "options");
    assert(humbug_ui_press(ui, HUMBUG_KEY_ENTER) && option_events == 1);
    assert(!humbug_ui_get_options(ui).christmas_blocklist);
    assert(humbug_ui_get_options(ui).custom_blocklist);
    assert(humbug_ui_press(ui, HUMBUG_KEY_DOWN));
    assert(humbug_ui_press(ui, HUMBUG_KEY_ENTER) && option_events == 2);
    assert(!humbug_ui_get_options(ui).custom_blocklist);
    assert(humbug_ui_press(ui, HUMBUG_KEY_DOWN));
    assert(humbug_ui_press(ui, HUMBUG_KEY_DOWN));
    assert(humbug_ui_press(ui, HUMBUG_KEY_LEFT) && humbug_ui_theme(ui) == humbug_ui_theme_count() - 1 &&
           theme_events == 1);
    assert(humbug_ui_press(ui, HUMBUG_KEY_RIGHT) && humbug_ui_theme(ui) == 0 && theme_events == 2);
    assert(humbug_ui_press(ui, HUMBUG_KEY_RIGHT) && humbug_ui_theme(ui) == 1 && theme_events == 3);
    snap(display, dir, "options-theme");
    assert(humbug_ui_press(ui, HUMBUG_KEY_LEFT) && humbug_ui_theme(ui) == 0 && theme_events == 4);
    assert(humbug_ui_press(ui, HUMBUG_KEY_DOWN));
    check_label(ui, "RESET CUSTOM BLOCKLIST", 27, 123, 1, HB_ACTIVE);
    for (unsigned y = 120; y < 132; ++y)
        for (unsigned x = 252; x < 294; ++x)
            assert(role_at(ui, x, y) == HB_PANEL);
    assert(humbug_ui_press(ui, HUMBUG_KEY_ENTER));
    check_label(ui, "RESET CUSTOM BLOCKLIST?", 17, 89, 1, HB_WARN);
    check_label(ui, "REMOVE MANUALLY BLOCKED SONGS", 17, 106, 1, HB_INK);
    check_label(ui, "4 CANCEL", 61, 155, 1, HB_MUTED);
    check_label(ui, "8 RESET", 214, 155, 1, HB_ACTIVE_INK);
    assert(role_at(ui, 17, 128) == HB_WELL);
    snap(display, dir, "options-reset");
    assert(humbug_ui_press(ui, HUMBUG_KEY_ENTER) && resets == 0);
    assert(humbug_ui_press(ui, HUMBUG_KEY_ENTER));
    assert(!humbug_ui_press(ui, HUMBUG_KEY_RIGHT));
    assert(humbug_ui_press(ui, HUMBUG_KEY_DASH) && resets == 0 && exits == 0);
    assert(humbug_ui_press(ui, HUMBUG_KEY_ENTER));
    assert(humbug_ui_press(ui, HUMBUG_KEY_BACK) && resets == 0);
    assert(humbug_ui_press(ui, HUMBUG_KEY_ENTER));
    assert(!humbug_ui_press(ui, HUMBUG_KEY_BLOCK));
    humbug_ui_notify_block(ui, "During Reset", "Artist");
    advance_ms(5000);
    assert(humbug_ui_press(ui, HUMBUG_KEY_EXIT) && resets == 1 && exits == 0);
    check_label(ui, "DURING RESET", 17, 106, 2, HB_INK);
    check_pips(ui, 3);
    humbug_ui_notify_block(ui, "Pending", "Artist");
    assert(humbug_ui_press(ui, HUMBUG_KEY_BACK));
    advance_ms(5000);
    assert(humbug_ui_press(ui, HUMBUG_KEY_BACK) && exits == 0);
    assert(humbug_ui_press(ui, HUMBUG_KEY_EXIT) && exits == 1);
    assert(!humbug_ui_get_options(ui).custom_blocklist);
    humbug_ui_set_options(ui, (humbug_options){true, true, true});
    assert(option_events == 2 && humbug_ui_get_options(ui).christmas_blocklist &&
           humbug_ui_get_options(ui).custom_blocklist);
    assert(humbug_ui_press(ui, HUMBUG_KEY_DASH));
    assert(!humbug_ui_press(ui, HUMBUG_KEY_RIGHT));
    assert(humbug_ui_press(ui, HUMBUG_KEY_PLAYING));
    /* Progress endpoints and long room names must fit within the playback layout. */
    humbug_room progress = sample_rooms[0];
    progress.progress_valid = true;
    progress.elapsed_seconds = 102;
    progress.duration_seconds = 298;
    assert(humbug_ui_set_rooms(ui, &progress, 1));
    check_label(ui, "1:42", 16, 133, 1, HB_ACTIVE);
    check_label(ui, "4:58", 280, 133, 1, HB_MUTED);
    assert(role_at(ui, 52, 136) == HB_PANEL && role_at(ui, 53, 136) == HB_SHADOW);
    assert(role_at(ui, 265, 136) == HB_SHADOW && role_at(ui, 266, 136) == HB_PANEL);
    progress.elapsed_seconds = 0;
    assert(humbug_ui_set_rooms(ui, &progress, 1));
    assert(role_at(ui, 56, 136) == HB_INK);
    progress.elapsed_seconds = 298;
    assert(humbug_ui_set_rooms(ui, &progress, 1));
    assert(role_at(ui, 260, 136) == HB_INK);
    humbug_room stopped = sample_rooms[0];
    stopped.playback = HUMBUG_STOPPED;
    stopped.provider = HUMBUG_OTHER;
    strcpy(stopped.name, "[]");
    assert(humbug_ui_set_rooms(ui, &stopped, 1));
    check_label(ui, "[]", 70, 36, 1, HB_INK);
    strcpy(stopped.name, "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456");
    assert(humbug_ui_set_rooms(ui, &stopped, 1));
    check_label(ui, "ROOM", 19, 36, 1, HB_MUTED);
    check_label(ui, "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456", 70, 36, 1, HB_INK);
    assert(role_at(ui, 62, 34) == HB_EDGE && role_at(ui, 85, 34) == HB_PANEL);
    strcpy(stopped.name, "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567");
    assert(humbug_ui_set_rooms(ui, &stopped, 1));
    check_label(ui, "ABCDEFGHIJKLMNOPQRSTUVWXYZ123...", 70, 36, 1, HB_INK);
    check_label(ui, "STOPPED", 146, 103, 1, HB_MUTED);
    for (unsigned y = 104; y < 109; ++y)
        for (unsigned x = 136; x < 141; ++x)
            assert(role_at(ui, x, y) == HB_WARN);
    assert(!humbug_ui_can_block(ui) && !humbug_ui_press(ui, HUMBUG_KEY_BLOCK));
    assert(humbug_ui_set_rooms(ui, sample_rooms, 3));
    assert(humbug_ui_can_block(ui));
    assert(humbug_ui_press(ui, HUMBUG_KEY_BLOCK) && blocks == 1 && id == 1 && token == 101);
    assert(!humbug_ui_can_block(ui) && !humbug_ui_press(ui, HUMBUG_KEY_BLOCK));
    humbug_ui_set_block_pending(ui, false);
    check_label(ui, "SONOS", 19, 36, 1, HB_MUTED);
    snap(display, dir, "playing-sonos");
    /* Unchanged snapshots should not trigger unnecessary display refreshes. */
    const unsigned frame_count = flushes;
    assert(humbug_ui_set_rooms(ui, sample_rooms, 3));
    humbug_ui_set_status(ui, true, true);
    humbug_ui_set_counts(ui, 25184, 1000);
    assert(humbug_ui_set_theme(ui, humbug_ui_theme(ui)));
    lv_refr_now(display);
    assert(flushes == frame_count);
    assert(humbug_ui_press(ui, HUMBUG_KEY_PLAYING));
    assert(humbug_ui_press(ui, HUMBUG_KEY_BLOCK) && blocks == 2 && id == 2 && token == 202);
    humbug_ui_set_block_pending(ui, false);
    check_label(ui, "CAST", 19, 36, 1, HB_MUTED);
    snap(display, dir, "playing-cast");
    assert(humbug_ui_press(ui, HUMBUG_KEY_DASH));
    queue_test(ui, display, dir);
    assert(humbug_ui_press(ui, HUMBUG_KEY_PLAYING));
    assert(humbug_ui_press(ui, HUMBUG_KEY_PLAYING));
    humbug_ui_notify_block(ui, sample_rooms[1].title, sample_rooms[1].artist);
    assert(!humbug_ui_can_block(ui));
    assert(humbug_ui_press(ui, HUMBUG_KEY_ENTER));
    assert(humbug_ui_press(ui, HUMBUG_KEY_PLAYING));
    assert(humbug_ui_press(ui, HUMBUG_KEY_BLOCK) && blocks == 3 && id == 3 && token == 303);
    humbug_ui_set_block_pending(ui, false);
    assert(humbug_ui_press(ui, HUMBUG_KEY_PLAYING));
    assert(humbug_ui_press(ui, HUMBUG_KEY_BLOCK) && blocks == 4 && id == 1 && token == 101);
    humbug_ui_set_block_pending(ui, false);
    assert(humbug_ui_press(ui, HUMBUG_KEY_PLAYING));
    /* Room reordering must preserve the selected Block target. */
    humbug_room shuffled[] = {sample_rooms[2], sample_rooms[0], sample_rooms[1]};
    assert(humbug_ui_set_rooms(ui, shuffled, 3));
    assert(humbug_ui_press(ui, HUMBUG_KEY_BLOCK) && id == 2 && token == 202);
    humbug_ui_set_block_pending(ui, false);
    assert(humbug_ui_set_rooms(ui, NULL, 0));
    assert(!humbug_ui_can_block(ui) && !humbug_ui_press(ui, HUMBUG_KEY_BLOCK));
    snap(display, dir, "no-rooms");
    assert(humbug_ui_press(ui, HUMBUG_KEY_DASH));
    humbug_ui_set_counts(ui, UINT64_MAX, UINT64_MAX);
    humbug_ui_set_status(ui, false, false);
    check_label(ui, "CONNECTED", 182, 8, 1, HB_EDGE);
    assert(role_at(ui, 175, 12) == HB_WARN);
    for (unsigned x = 242; x < 247; ++x)
        assert(role_at(ui, x, 12) == HB_PANEL);
    check_label(ui, "BLOCKING", 253, 8, 1, HB_EDGE);
    assert(role_at(ui, 246, 12) == HB_PANEL && role_at(ui, 247, 12) == HB_UNDER);
    assert(role_at(ui, 247, 12) == role_at(ui, 198, 159));
    assert(role_at(ui, 253, 5) == role_at(ui, 204, 152));
    assert(role_at(ui, 304, 12) == role_at(ui, 252, 159));
    for (unsigned x = 248; x < 253; ++x)
        assert(role_at(ui, x, 12) == HB_WELL);
    for (unsigned x = 300; x < 304; ++x)
        assert(role_at(ui, x, 12) == HB_WELL);
    assert(role_at(ui, 273, 6) == HB_WELL && role_at(ui, 273, 16) == HB_WELL);
    assert(!humbug_ui_press(ui, HUMBUG_KEY_BLOCK));
    snap(display, dir, "offline");
    assert(humbug_ui_press(ui, HUMBUG_KEY_EXIT));

    /* LVGL label font check as well as the native Canvas renderer. */
    lv_obj_t* label = lv_label_create(screen);
    lv_obj_set_style_text_font(label, &humbug_pixel_14, 0);
    lv_label_set_text(label, "HUMBUG");
    lv_obj_set_pos(label, 4, 60);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    snap(display, dir, "font-check");
    lv_obj_delete(label);
    /* Destroying the UI must cancel pending timers before their next tick. */
    humbug_ui_notify_block(ui, "Delete Timer", "Artist");
    humbug_ui_destroy(ui);
    advance_ms(5000);
    lv_display_delete(display);
    lv_deinit();
    puts("LVGL smoke checks passed");
    return 0;
}
