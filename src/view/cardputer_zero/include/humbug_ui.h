#ifndef HUMBUG_UI_H
#define HUMBUG_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { HUMBUG_WIDTH = 320, HUMBUG_HEIGHT = 170, HUMBUG_MAX_ROOMS = 64 };
typedef enum { HUMBUG_SONOS, HUMBUG_CAST, HUMBUG_OTHER } humbug_provider;
typedef enum { HUMBUG_IDLE, HUMBUG_PLAYING, HUMBUG_PAUSED, HUMBUG_STOPPED } humbug_playback;
typedef enum {
    HUMBUG_KEY_EXIT,
    HUMBUG_KEY_OPTIONS,
    HUMBUG_KEY_BLOCK,
    HUMBUG_KEY_DASH,
    HUMBUG_KEY_PLAYING,
    HUMBUG_KEY_ENTER,
    HUMBUG_KEY_UP,
    HUMBUG_KEY_DOWN,
    HUMBUG_KEY_LEFT,
    HUMBUG_KEY_RIGHT,
    HUMBUG_KEY_BACK,
    HUMBUG_KEY_THEME = HUMBUG_KEY_OPTIONS
} humbug_key;
typedef enum {
    HUMBUG_EVENT_EXIT,
    HUMBUG_EVENT_BLOCK,
    HUMBUG_EVENT_THEME,
    HUMBUG_EVENT_INSERT_CLOSED,
    HUMBUG_EVENT_OPTIONS_CHANGED,
    HUMBUG_EVENT_RESET_CUSTOM
} humbug_event;
typedef struct humbug_ui humbug_ui;

typedef struct {
    bool christmas_blocklist, custom_blocklist, auto_stop;
} humbug_options;

typedef struct {
    uint64_t id;          /* Stable, nonzero, unique room ID within the app. */
    uint64_t track_token; /* Nonzero while this specific track can be blocked. */
    humbug_provider provider;
    humbug_playback playback;
    bool online;
    bool progress_valid;
    bool levels_valid;
    uint32_t elapsed_seconds, duration_seconds;
    uint8_t levels[2][8];                  /* Each meter column is 0–6. */
    char name[65], title[161], artist[97]; /* UTF-8, always NUL terminated. */
} humbug_room;

typedef void (*humbug_event_cb)(void* user, humbug_event event, uint64_t room_id, uint64_t track_token,
                                size_t theme);

/* Call LVGL and all UI functions on the same UI thread. The caller owns the
 * parent. Deleting the parent also destroys the view and invalidates ui. */
humbug_ui* humbug_ui_create(lv_obj_t* parent, humbug_event_cb cb, void* user);
void humbug_ui_destroy(humbug_ui* ui);
lv_obj_t* humbug_ui_object(humbug_ui* ui);

/* Updates copy their input. Subsequent snapshots retain the selected room
 * by ID if it still exists. Keep list order stable to preserve the 1/x order. */
bool humbug_ui_set_rooms(humbug_ui* ui, const humbug_room* rooms, size_t count);
void humbug_ui_set_status(humbug_ui* ui, bool online, bool blocker_on);
/* Keep Block disabled until the host reports that its request has finished. */
void humbug_ui_set_block_pending(humbug_ui* ui, bool pending);
void humbug_ui_set_counts(humbug_ui* ui, uint64_t all_time, uint64_t today);
void humbug_ui_set_latest(humbug_ui* ui, const char* title, const char* artist);
/* Call on the UI thread after a block is confirmed. Metadata is copied.
 * Up to 64 alerts, including the visible one, are kept in arrival order.
 * Excess alerts still update the latest track. Counters remain host-owned. */
void humbug_ui_notify_block(humbug_ui* ui, const char* title, const char* artist);
void humbug_ui_notify_auto_stop(humbug_ui* ui, const char* title, const char* artist);
void humbug_ui_set_options(humbug_ui* ui, humbug_options options);
humbug_options humbug_ui_get_options(const humbug_ui* ui);
bool humbug_ui_can_show_block(const humbug_ui* ui);

size_t humbug_ui_theme_count(void);
const char* humbug_ui_theme_name(size_t index);
/* Source IDs are stable catalog indices; runtime indices are compacted to embedded themes. */
size_t humbug_ui_theme_source_count(void);
size_t humbug_ui_theme_source_index(size_t index);
size_t humbug_ui_theme_index_from_source(size_t source_index);
bool humbug_ui_set_theme(humbug_ui* ui, size_t index);
size_t humbug_ui_theme(const humbug_ui* ui);
/* Returns true when the key was consumed. Block authorisation is enforced here. */
bool humbug_ui_press(humbug_ui* ui, humbug_key key);
bool humbug_ui_can_block(const humbug_ui* ui);

#ifdef __cplusplus
}
#endif
#endif
