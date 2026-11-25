#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "humbug_ui.h"
#include "sample_rooms.h"
#include "src/drivers/display/fb/lv_linux_fbdev.h"

static volatile sig_atomic_t stop;
static void request_stop(int signal_number) {
    (void)signal_number;
    stop = 1;
}

static uint32_t tick(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);
}
static void on_action(void* arg, humbug_event action, uint64_t id, uint64_t token, size_t theme) {
    humbug_ui* ui = *(humbug_ui**)arg;
    if (action == HUMBUG_EVENT_EXIT)
        stop = 1;
    if (action == HUMBUG_EVENT_THEME)
        printf("Theme: %s\n", humbug_ui_theme_name(theme));
    if (action == HUMBUG_EVENT_OPTIONS_CHANGED) {
        humbug_options options = humbug_ui_get_options(ui);
        printf("Demo options: Christmas=%d, Custom=%d\n", options.christmas_blocklist,
               options.custom_blocklist);
    }
    if (action == HUMBUG_EVENT_RESET_CUSTOM)
        puts("Demo only: Reset custom blocklist requested");
    if (action == HUMBUG_EVENT_BLOCK)
        printf("Demo only: Block ROOM=%" PRIu64 " TRACK=%" PRIu64 "\n", id, token);
}
static bool translate(unsigned code, humbug_key* key) {
    switch (code) {
    case KEY_4:
    case KEY_KP4:
        *key = HUMBUG_KEY_DASH;
        break;
    case KEY_5:
    case KEY_KP5:
        *key = HUMBUG_KEY_PLAYING;
        break;
    case KEY_6:
    case KEY_KP6:
        *key = HUMBUG_KEY_OPTIONS;
        break;
    case KEY_7:
    case KEY_KP7:
        *key = HUMBUG_KEY_BLOCK;
        break;
    case KEY_8:
    case KEY_KP8:
        *key = HUMBUG_KEY_EXIT;
        break;
    case KEY_ENTER:
    case KEY_KPENTER:
        *key = HUMBUG_KEY_ENTER;
        break;
    case KEY_UP:
    case KEY_F:
        *key = HUMBUG_KEY_UP;
        break;
    case KEY_DOWN:
    case KEY_X:
        *key = HUMBUG_KEY_DOWN;
        break;
    case KEY_LEFT:
    case KEY_Z:
        *key = HUMBUG_KEY_LEFT;
        break;
    case KEY_RIGHT:
    case KEY_C:
        *key = HUMBUG_KEY_RIGHT;
        break;
    case KEY_ESC:
        *key = HUMBUG_KEY_BACK;
        break;
    default:
        return false;
    }
    return true;
}
int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s /dev/fb0 /dev/input/eventN [theme-index]\n", argv[0]);
        return 2;
    }
    int keyboard = open(argv[2], O_RDONLY | O_NONBLOCK);
    if (keyboard < 0) {
        perror("keyboard");
        return 1;
    }
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t* display = lv_linux_fbdev_create();
    if (!display) {
        close(keyboard);
        return 1;
    }
    lv_linux_fbdev_set_file(display, argv[1]);
    if (lv_display_get_horizontal_resolution(display) != HUMBUG_WIDTH ||
        lv_display_get_vertical_resolution(display) != HUMBUG_HEIGHT) {
        fprintf(stderr, "Expected an active 320 x 170 framebuffer. Check the OS driver/rotation.\n");
        lv_display_delete(display);
        close(keyboard);
        return 1;
    }
    humbug_ui* ui = NULL;
    ui = humbug_ui_create(lv_display_get_screen_active(display), on_action, &ui);
    if (!ui) {
        lv_display_delete(display);
        close(keyboard);
        return 1;
    }
    if (argc >= 4) {
        char* end;
        long theme = strtol(argv[3], &end, 10);
        if (*end || end == argv[3] || theme < 0 || !humbug_ui_set_theme(ui, (size_t)theme))
            fprintf(stderr, "Invalid theme index. Using Side A.\n");
    }
    humbug_ui_set_status(ui, true, true);
    humbug_ui_set_counts(ui, 25184, 1000);
    humbug_ui_set_latest(ui, sample_rooms[0].title, sample_rooms[0].artist);
    humbug_ui_set_rooms(ui, sample_rooms, sizeof sample_rooms / sizeof sample_rooms[0]);
    signal(SIGINT, request_stop);
    signal(SIGTERM, request_stop);
    while (!stop) {
        struct input_event event;
        ssize_t n;
        while ((n = read(keyboard, &event, sizeof event)) == (ssize_t)sizeof event) {
            humbug_key key;
            if (event.type == EV_KEY && event.value == 1 && translate(event.code, &key))
                humbug_ui_press(ui, key);
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
            perror("keyboard read");
            break;
        }
        uint32_t delay = lv_timer_handler();
        struct timespec sleep_for = {.tv_nsec = (long)(delay > 12 ? 12 : delay ? delay : 1) * 1000000};
        nanosleep(&sleep_for, NULL);
    }
    humbug_ui_destroy(ui);
    lv_display_delete(display);
    close(keyboard);
    return 0;
}
