#include "humbug_ui.h"
#include "humbug_art.h"

#if defined(__has_include)
#if __has_include("src/misc/cache/instance/lv_image_cache.h")
#include "src/misc/cache/instance/lv_image_cache.h"
#elif __has_include("src/misc/cache/lv_image_cache.h")
#include "src/misc/cache/lv_image_cache.h"
#endif
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    DIM = 128,
    TITLE_CAP = 161,
    ARTIST_CAP = 97,
    ALERT_CAP = 64,
    ALERT_PIPS = 3,
    ALERT_SECONDS = 4,
    SECOND_MS = 1000
};
enum { BUTTON_MARGIN = 12, BUTTON_GAP = 4 };
typedef struct {
    char title[TITLE_CAP], artist[ARTIST_CAP];
    bool auto_stop;
} block_alert;
enum {
    TRACK_X = 16,
    TRACK_Y = 63,
    TRACK_WIDTH = 288,
    TRACK_SCALE = 2,
    MARQUEE_GAP = 24,
    MARQUEE_HOLD_MS = 900,
    MARQUEE_PIXELS_PER_SECOND = 25
};
_Static_assert((int)HB_ROLE_COUNT < DIM, "Theme roles must fit below the dim palette bank");

typedef struct {
    char title[TITLE_CAP];
    uint64_t stream, track;
    uint32_t start;
    bool active, playing_view;
} marquee_state;

typedef enum { VIEW_DASH, VIEW_PLAYING, VIEW_OPTIONS } view_id;
typedef enum { MODAL_NONE, MODAL_BLOCK, MODAL_RESET } modal_id;
typedef enum { OPT_CHRISTMAS, OPT_CUSTOM, OPT_AUTO_STOP, OPT_THEME, OPT_RESET, OPT_COUNT } option_id;

struct humbug_ui {
    lv_obj_t* canvas;
    lv_draw_buf_t* buf;
    humbug_event_cb callback;
    void* user;
    humbug_room rooms[HUMBUG_MAX_ROOMS];
    size_t room_count, selected, theme;
    view_id view;
    modal_id modal;
    option_id focus;
    humbug_options options;
    bool online, blocker_on, block_pending;
    uint64_t total, today;
    char latest[TITLE_CAP], artist[ARTIST_CAP];
    block_alert alerts[ALERT_CAP];
    size_t alert_head, alert_count;
    uint32_t alert_index, alert_total, alert_started;
    unsigned alert_pips;
    lv_timer_t *animation_timer, *alert_timer;
    marquee_state marquee;
};

static const humbug_room* selected_room(const humbug_ui* ui) {
    return ui->selected < ui->room_count ? &ui->rooms[ui->selected] : NULL;
}

static bool same_room(const humbug_room* left, const humbug_room* right) {
    return left->id == right->id && left->track_token == right->track_token &&
           left->provider == right->provider && left->playback == right->playback &&
           left->online == right->online && left->progress_valid == right->progress_valid &&
           left->levels_valid == right->levels_valid && left->elapsed_seconds == right->elapsed_seconds &&
           left->duration_seconds == right->duration_seconds &&
           memcmp(left->levels, right->levels, sizeof left->levels) == 0 &&
           strncmp(left->name, right->name, sizeof left->name - 1) == 0 &&
           strncmp(left->title, right->title, sizeof left->title - 1) == 0 &&
           strncmp(left->artist, right->artist, sizeof left->artist - 1) == 0;
}

bool humbug_ui_can_block(const humbug_ui* ui) {
    const humbug_room* r = selected_room(ui);
    return ui->view == VIEW_PLAYING && !ui->modal && !ui->block_pending && ui->callback && ui->online && r &&
           r->online && r->id && r->track_token &&
           (r->playback == HUMBUG_PLAYING || r->playback == HUMBUG_PAUSED) && r->title[0];
}

static uint8_t* pixel(humbug_ui* ui, int x, int y) {
    return lv_draw_buf_goto_xy(ui->buf, (uint32_t)x, (uint32_t)y);
}

static void fill(humbug_ui* ui, int x, int y, int w, int h, unsigned role) {
    for (int j = 0; j < h; ++j)
        memset(pixel(ui, x, y + j), (int)role, (size_t)w);
}

static void sprite_at_width(humbug_ui* ui, enum hb_sprite_id id, int x, int y, int width) {
    const hb_sprite* s = &hb_sprites[id];
    /* Remove a center strip, keeping both bevels and the shadow intact. */
    const int trim = s->width - width;
    const int left_end = width / 2, right_start = left_end + trim;
    int row = 0, col = 0;
    for (size_t i = 0; i < s->size; i += 2) {
        int run = s->rle[i];
        const uint8_t role = s->rle[i + 1];
        while (run) {
            int step = run < (int)s->width - col ? run : (int)s->width - col;
            if (role && !trim) {
                memset(pixel(ui, x + col, y + row), role, (size_t)step);
            } else if (role) {
                const int end = col + step;
                const int left = end < left_end ? end : left_end;
                const int right = col > right_start ? col : right_start;
                if (col < left)
                    memset(pixel(ui, x + col, y + row), role, (size_t)(left - col));
                if (right < end)
                    memset(pixel(ui, x + right - trim, y + row), role, (size_t)(end - right));
            }
            run -= step;
            col += step;
            if (col == s->width) {
                col = 0;
                ++row;
            }
        }
    }
}

static void sprite_at(humbug_ui* ui, enum hb_sprite_id id, int x, int y) {
    sprite_at_width(ui, id, x, y, hb_sprites[id].width);
}

static void sprite_draw(humbug_ui* ui, enum hb_sprite_id id, int x, int y, int extra) {
    const hb_sprite* s = &hb_sprites[id];
    int row = 0, col = 0;
    for (size_t i = 0; i < s->size; i += 2) {
        int run = s->rle[i];
        const uint8_t role = s->rle[i + 1];
        while (run) {
            int step = run < (int)s->width - col ? run : (int)s->width - col;
            if (role) {
                const int middle = (int)s->width / 2;
                const int offset = col > middle ? extra : 0;
                const int stretch = col <= middle && middle < col + step ? extra : 0;
                memset(pixel(ui, x + col + offset, y + row), role, (size_t)(step + stretch));
            }
            run -= step;
            col += step;
            if (col == (int)s->width) {
                col = 0;
                ++row;
            }
        }
    }
}

static void sprite(humbug_ui* ui, enum hb_sprite_id id) {
    sprite_at(ui, id, hb_sprites[id].x, hb_sprites[id].y);
}

static unsigned glyph(char ch) {
    unsigned c = (unsigned char)ch;
    if (c == '[')
        c = '(';
    else if (c == ']')
        c = ')';
    if (c >= 'a' && c <= 'z')
        c -= 'a' - 'A';
    if (c < 32 || c >= 127)
        return '?';
    if (c == ' ')
        return c;
    for (int i = 0; i < 7; ++i)
        if (hb_glyphs[c][i])
            return c;
    return '?';
}

/* Convert a UTF-8 source into the display repertoire. A valid non-ASCII
 * code point consumes one replacement glyph. ASCII advances stay fixed. */
static void ascii_text(char* dst, size_t cap, const char* src) {
    size_t n = 0;
    if (src) {
        const unsigned char* s = (const unsigned char*)src;
        while (*s && n + 1 < cap) {
            const unsigned char c = *s++;
            if (c < 0x80)
                dst[n++] = (char)glyph((char)c);
            else {
                unsigned tail = c >= 0xc2 && c <= 0xdf   ? 1
                                : c >= 0xe0 && c <= 0xef ? 2
                                : c >= 0xf0 && c <= 0xf4 ? 3
                                                         : 0;
                while (tail && (*s & 0xc0) == 0x80) {
                    ++s;
                    --tail;
                }
                dst[n++] = '?';
            }
        }
    }
    dst[n] = 0;
}

typedef enum { LEFT, CENTER, RIGHT } align_text;
static int text_width(const char* s, int scale, bool number) {
    int w = 0;
    for (; *s; ++s)
        w += number && *s == ',' ? 3 : 6;
    return w ? (w - 1) * scale : 0;
}

static void draw_text(humbug_ui* ui, const char* s, int x, int y, int left, int right, int scale,
                      unsigned role, bool number) {
    for (; *s && x < right; ++s) {
        const bool comma = number && *s == ',';
        const int advance = (comma ? 3 : 6) * scale;
        if (x + advance <= left) {
            x += advance;
            continue;
        }
        const uint8_t* bits = hb_glyphs[glyph(*s)];
        const int first = comma ? 1 : 0;
        const int last = comma ? 3 : 5;
        for (int i = first; i < last; ++i) {
            const int start = x + (i - first) * scale;
            const int a = start > left ? start : left;
            const int end = start + scale;
            const int b = end < right ? end : right;
            if (a >= b)
                continue;
            for (int j = 0; j < 7; ++j)
                if (bits[j] & (16 >> i))
                    fill(ui, a, y + j * scale, b - a, scale, role);
        }
        x += advance;
    }
}

static void text(humbug_ui* ui, const char* s, int x, int y, int w, int scale, unsigned role,
                 align_text align, bool number) {
    const int width = text_width(s, scale, number);
    if (width > w)
        return;
    const int left = x;
    if (align == CENTER)
        x += (w - width) / 2;
    if (align == RIGHT)
        x += w - width;
    draw_text(ui, s, x, y, left, left + w, scale, role, number);
}

static void marquee_title(humbug_ui* ui, marquee_state* state, bool animate, const char* source, int x, int y,
                          int available, int scale, align_text align, uint64_t stream, uint64_t track) {
    char title[TITLE_CAP];
    ascii_text(title, sizeof title, source);
    const int width = text_width(title, scale, false);
    if (width <= available) {
        state->active = false;
        text(ui, title, x, y, available, scale, HB_INK, align, false);
        return;
    }

    int offset = 0;
    if (animate) {
        if (!state->active || (ui->view == VIEW_PLAYING) != state->playing_view || stream != state->stream ||
            track != state->track || strcmp(title, state->title) != 0) {
            memcpy(state->title, title, strlen(title) + 1);
            state->stream = stream;
            state->track = track;
            state->playing_view = ui->view == VIEW_PLAYING;
            state->start = lv_tick_get();
        }
        state->active = true;
        const uint32_t elapsed = lv_tick_elaps(state->start);
        if (elapsed > MARQUEE_HOLD_MS)
            offset = (int)(((uint64_t)(elapsed - MARQUEE_HOLD_MS) * MARQUEE_PIXELS_PER_SECOND / 1000) %
                           (unsigned)(width + MARQUEE_GAP));
    }
    draw_text(ui, title, x - offset, y, x, x + available, scale, HB_INK, false);
    if (offset)
        draw_text(ui, title, x - offset + width + MARQUEE_GAP, y, x, x + available, scale, HB_INK, false);
}

static void label(humbug_ui* ui, const char* s, int x, int y, int w, unsigned role) {
    char clean[TITLE_CAP];
    ascii_text(clean, sizeof clean, s);
    const size_t capacity = (size_t)(w + 1) / 6;
    if (strlen(clean) > capacity && capacity >= 3) {
        memcpy(clean + capacity - 3, "...", 4);
    }
    text(ui, clean, x, y, w, 1, role, LEFT, false);
}

static void grouped(uint64_t v, char out[27]) {
    char digits[21];
    snprintf(digits, sizeof digits, "%" PRIu64, v);
    size_t len = strlen(digits), k = 0;
    for (size_t i = 0; i < len; ++i) {
        if (i && (len - i) % 3 == 0)
            out[k++] = ',';
        out[k++] = digits[i];
    }
    out[k] = 0;
}

static void count(humbug_ui* ui, uint64_t n, int y, unsigned role) {
    char buf[27];
    grouped(n, buf);
    const int w = 106;
    if (text_width(buf, 2, true) <= w) {
        text(ui, buf, 199, y, w, 2, role, LEFT, true);
        return;
    }
    if (text_width(buf, 1, true) > w) {
        for (size_t i = strlen(buf); i > 0; --i) {
            buf[i - 1] = '+';
            buf[i] = 0;
            if (text_width(buf, 1, true) <= w && (i < 2 || buf[i - 2] != ','))
                break;
        }
    }
    text(ui, buf, 199, y + 3, w, 1, role, LEFT, true);
}

static void header(humbug_ui* ui) {
    /* Center the badge faces in the header panel, excluding its two shadow rows. */
    const int badge_y = (hb_sprites[HB_SPR_HEADER].height - 2 - 16) / 2;
    const char* status = "CONNECTED";
    enum { PAD = 6, DOT = 3, GAP = 5, SHADOW = 1, BADGE_GAP = 5 };
    const int status_width = text_width(status, 1, false);
    const int badge_width = PAD + DOT + GAP + status_width + PAD + SHADOW;
    const hb_sprite* blocker = &hb_sprites[ui->blocker_on ? HB_SPR_BLOCKER : HB_SPR_BLOCKER_OFF];
    const int badge_x = blocker->x - BADGE_GAP - badge_width;
    sprite(ui, HB_SPR_HEADER);
    sprite_at_width(ui, HB_SPR_CONNECTION, badge_x, badge_y, badge_width);
    sprite_at(ui, ui->blocker_on ? HB_SPR_BLOCKER : HB_SPR_BLOCKER_OFF, blocker->x, badge_y);
    text(ui, "HUMBUG", 10, 5, 125, 2, HB_INK, LEFT, false);
    fill(ui, badge_x + PAD, badge_y + 7, DOT, DOT, ui->online ? HB_GREEN : HB_WARN);
    text(ui, status, badge_x + PAD + DOT + GAP, badge_y + 4, status_width, 1,
         ui->online ? HB_MUTED : HB_EDGE, LEFT, false);
    text(ui, "BLOCKING", blocker->x, badge_y + 4, blocker->width - SHADOW, 1,
         ui->blocker_on ? HB_GREEN_INK : HB_EDGE, CENTER, false);
}

static void nav(humbug_ui* ui) {
    enum { BUTTON_COUNT = 5, TEXT_PADDING = 5 };
    static const char* const labels[] = {"4 DASH", "5 PLAYING", "6 SETUP", "7 BLOCK", "8 EXIT"};
    int widths[BUTTON_COUNT];
    int total_width = 0;
    for (int j = 0; j < BUTTON_COUNT; ++j) {
        const bool cycle = ui->view == VIEW_PLAYING && ui->room_count > 1 && j == 1;
        const char* s = cycle ? "5 NEXT" : labels[j];
        widths[j] = text_width(s, 1, false) + 2 * TEXT_PADDING;
        total_width += widths[j];
    }
    const int remaining =
        HUMBUG_WIDTH - 2 * BUTTON_MARGIN - (BUTTON_COUNT - 1) * BUTTON_GAP - total_width;
    for (int j = 0; j < BUTTON_COUNT; ++j)
        widths[j] += remaining / BUTTON_COUNT + (j < remaining % BUTTON_COUNT ? 1 : 0);
    int x = BUTTON_MARGIN;
    sprite(ui, HB_SPR_FOOTER);
    for (int j = 0; j < BUTTON_COUNT; ++j) {
        const bool active = j == (ui->view == VIEW_PLAYING ? 1 : ui->view == VIEW_OPTIONS ? 2 : 0);
        const bool disabled = j == 3 && !humbug_ui_can_block(ui);
        const bool cycle = ui->view == VIEW_PLAYING && ui->room_count > 1 && j == 1;
        const char* s = cycle ? "5 NEXT" : labels[j];
        sprite_at_width(ui, active ? HB_SPR_KEY_69_ACTIVE : HB_SPR_KEY_69, x, 151, widths[j]);
        text(ui, s, x, 155, widths[j], 1,
             active     ? HB_ACTIVE_INK
             : disabled ? HB_EDGE
                        : HB_MUTED,
             CENTER, false);
        x += widths[j];
        if (j < BUTTON_COUNT - 1)
            x += BUTTON_GAP;
    }
}

static void cassette_label(humbug_ui* ui) {
    sprite(ui, HB_SPR_CASSETTE);
    switch (ui->theme) {
    case HB_THEME_STYLE_BLANK_TAPE:
        sprite(ui, HB_SPR_CASSETTE_BLANK_LABEL);
        text(ui, "C60", 143, 49, 18, 1, HB_LABEL_INK, LEFT, false);
        break;
    case HB_THEME_STYLE_BOOTLEG:
        sprite(ui, HB_SPR_CASSETTE_BOOTLEG_SCRATCH);
        text(ui, "DFCD", 49, 48, 60, 1, HB_ACTIVE, LEFT, false);
        text(ui, "SIDE A", 121, 48, 49, 1, HB_LABEL_INK, LEFT, false);
        break;
    default:
        text(ui, ui->theme == HB_THEME_STYLE_SIDE_B ? "B" : "A", 31, 48, 10, 2, HB_LABEL_INK, LEFT,
             false);
        text(ui, "HUMBUG", 49, 48, 60, 1, HB_LABEL_INK, LEFT, false);
        text(ui, ui->theme == HB_THEME_STYLE_SIDE_B ? "SIDE B" : "SIDE A", 121, 48, 49, 1,
             HB_LABEL_INK, LEFT, false);
        break;
    }
}

static void dash(humbug_ui* ui) {
    cassette_label(ui);
    sprite(ui, HB_SPR_COUNTERS);
    sprite(ui, HB_SPR_LATEST);
    text(ui, "BLOCKED ALL TIME", 200, 36, 107, 1, HB_MUTED, LEFT, false);
    count(ui, ui->total, 46, HB_INK);
    text(ui, "BLOCKED TODAY", 200, 67, 106, 1, HB_ACTIVE, LEFT, false);
    count(ui, ui->today, 77, HB_ACTIVE);
    text(ui, "LAST BLOCKED", 208, 107, 95, 1, HB_ACTIVE, LEFT, false);
    marquee_title(ui, &ui->marquee, !ui->modal, ui->latest[0] ? ui->latest : "NO TRACKS YET", 201, 119, 102,
                  1, LEFT, 0, 0);
    label(ui, ui->artist, 201, 130, 102, HB_MUTED);
}

static void time_text(uint32_t value, char out[12]) {
    uint32_t min = value / 60;
    if (min > 99)
        strcpy(out, "99:59+");
    else
        snprintf(out, 12, "%" PRIu32 ":%02" PRIu32, min, value % 60);
}

static const char* provider_name(humbug_provider provider) {
    switch (provider) {
    case HUMBUG_SONOS:
        return "SONOS";
    case HUMBUG_CAST:
        return "CAST";
    default:
        return "ROOM";
    }
}

static void playback(humbug_ui* ui) {
    const humbug_room* r = selected_room(ui);
    sprite(ui, HB_SPR_ROOM);
    sprite(ui, HB_SPR_DECK);
    sprite(ui, HB_SPR_TIMELINE);
    sprite(ui, HB_SPR_SPEAKER);
    sprite_at(ui, HB_SPR_SPEAKER, 272, 98);
    text(ui, r ? provider_name(r->provider) : "DEVICE", 19, 36, 35, 1, HB_MUTED, LEFT, false);
    label(ui, r ? r->name : "NO STREAMS FOUND", 70, 36, 193, HB_INK);
    char index[16];
    snprintf(index, sizeof index, "%u/%u", r ? (unsigned)ui->selected + 1 : 0, (unsigned)ui->room_count);
    text(ui, index, 268, 36, 34, 1, HB_ACTIVE_INK, CENTER, false);
    const bool playing = r && r->online && r->playback == HUMBUG_PLAYING;
    const bool paused = r && r->online && r->playback == HUMBUG_PAUSED;
    const bool stopped = r && r->online && r->playback == HUMBUG_STOPPED;
    marquee_title(ui, &ui->marquee, !ui->modal, r && r->title[0] ? r->title : "NOTHING PLAYING", TRACK_X,
                  TRACK_Y, TRACK_WIDTH, TRACK_SCALE, CENTER, r ? r->id : 0, r ? r->track_token : 0);
    char artist[ARTIST_CAP];
    ascii_text(artist, sizeof artist, r ? r->artist : "");
    if (strlen(artist) > 48)
        memcpy(artist + 45, "...", 4);
    text(ui, artist, 16, 83, 288, 1, HB_ACTIVE, CENTER, false);
    for (int side = 0; side < 2; ++side) {
        int start = side ? 221 : 60;
        for (int col = 0; col < 8; ++col)
            for (int row = 0; row < 6; ++row) {
                static const uint8_t loop[] = {1, 2, 4, 6, 5, 3, 2, 4};
                unsigned level = 0;
                if (playing)
                    level = r->levels_valid ? r->levels[side][col]
                                            : loop[(lv_tick_get() / 160 + col * 3 + side * 3) % sizeof loop];
                unsigned role = level > (unsigned)row ? (row < 3   ? HB_GREEN
                                                         : row < 5 ? HB_ACTIVE
                                                                   : HB_WARN)
                                                      : HB_SHELL_LOW;
                fill(ui, start + col * 5, 118 - row * 3, 3, 2, role);
            }
    }
    if (playing || paused)
        sprite_at(ui, playing ? HB_SPR_PLAY : HB_SPR_PAUSE, 136, 103);
    if (stopped)
        fill(ui, 136, 104, 5, 5, HB_WARN);
    const char* status = playing           ? "PLAYING"
                         : paused          ? "PAUSED"
                         : stopped         ? "STOPPED"
                         : r && !r->online ? "OFFLINE"
                                           : "";
    text(ui, status, 144, 103, 45, 1, paused ? HB_ACTIVE : HB_MUTED, CENTER, false);
    if (r && r->online && r->progress_valid && r->duration_seconds) {
        uint32_t elapsed =
            r->elapsed_seconds < r->duration_seconds ? r->elapsed_seconds : r->duration_seconds;
        char a[12], b[12];
        time_text(elapsed, a);
        time_text(r->duration_seconds, b);
        text(ui, a, 16, 133, 35, 1, HB_ACTIVE, LEFT, false);
        text(ui, b, 267, 133, 36, 1, HB_MUTED, RIGHT, false);
        const int n = (int)(204 * (uint64_t)elapsed / r->duration_seconds);
        if (n)
            fill(ui, 57, 135, n, 3, playing ? HB_GREEN : HB_ACTIVE);
        fill(ui, 56 + n, 133, 3, 6, HB_INK);
    } else {
        text(ui, "--:--", 16, 133, 35, 1, HB_MUTED, LEFT, false);
        text(ui, "--:--", 267, 133, 36, 1, HB_MUTED, RIGHT, false);
    }
}

static void dim(humbug_ui* ui) {
    /* A blended overlay would introduce colors outside the LCD and CGA palettes. */
    const bool checkerboard = hb_checkerboard_scrim[ui->theme];
    for (int y = 0; y < HUMBUG_HEIGHT; ++y) {
        uint8_t* row = pixel(ui, 0, y);
        if (checkerboard) {
            for (int x = y % 2; x < HUMBUG_WIDTH; x += 2)
                row[x] = HB_SHADOW;
        } else {
            for (int x = 0; x < HUMBUG_WIDTH; ++x)
                row[x] += (uint8_t)DIM;
        }
    }
}

static void dock_buttons(humbug_ui* ui, const char* left, const char* right) {
    const int width = (HUMBUG_WIDTH - 2 * BUTTON_MARGIN - BUTTON_GAP) / 2;
    const int right_x = BUTTON_MARGIN + width + BUTTON_GAP;
    sprite(ui, HB_SPR_FOOTER);
    sprite_at_width(ui, HB_SPR_KEY_140, BUTTON_MARGIN, 151, width);
    sprite_at_width(ui, HB_SPR_KEY_154_ACTIVE, right_x, 151, width);
    text(ui, left, BUTTON_MARGIN, 155, width, 1, HB_MUTED, CENTER, false);
    text(ui, right, right_x, 155, width, 1, HB_ACTIVE_INK, CENTER, false);
}

static void deck_dock(humbug_ui* ui) {
    const block_alert* alert = &ui->alerts[ui->alert_head];
    dim(ui);
    sprite(ui, HB_SPR_DECK_DOCK);
    text(ui, "TRACK BLOCKED", 17, 89, 150, 1, HB_WARN, LEFT, false);

    char index[22];
    snprintf(index, sizeof index, "%" PRIu32 "/%" PRIu32, ui->alert_index, ui->alert_total);
    int badge_width = text_width(index, 1, false) + 10;
    if (badge_width < 34)
        badge_width = 34;
    const int badge_x = 303 - badge_width;
    sprite_draw(ui, HB_SPR_COUNTER_BADGE, badge_x, 85, badge_width - 34);
    text(ui, index, badge_x, 89, badge_width, 1, HB_ACTIVE_INK, CENTER, false);
    if (alert->auto_stop) {
        const int stop_width = text_width("AUTO STOP", 1, false) + 10;
        const int stop_x = badge_x - stop_width - 4;
        sprite_draw(ui, HB_SPR_COUNTER_BADGE, stop_x, 85, stop_width - 34);
        text(ui, "AUTO STOP", stop_x, 89, stop_width, 1, HB_ACTIVE_INK, CENTER, false);
    }

    const char* title = alert->title[0] ? alert->title : "UNKNOWN TRACK";
    char clean[TITLE_CAP];
    ascii_text(clean, sizeof clean, title);
    if (text_width(clean, 2, false) <= 286)
        text(ui, clean, 17, 106, 286, 2, HB_INK, LEFT, false);
    else
        label(ui, title, 17, 109, 286, HB_INK);
    label(ui, alert->artist, 17, 128, 198, HB_MUTED);

    for (unsigned i = 0; i < ALERT_PIPS; ++i)
        fill(ui, 225 + i * 27, 130, 24, 4, i < ui->alert_pips ? HB_ACTIVE : HB_SHELL);

    dock_buttons(ui, "4 DISMISS ALL", "8 CONTINUE");
}

static void outline(humbug_ui* ui, int x, int y, int w, int h, unsigned role) {
    fill(ui, x, y, w, 1, role);
    fill(ui, x, y + h - 1, w, 1, role);
    fill(ui, x, y + 1, 1, h - 2, role);
    fill(ui, x + w - 1, y + 1, 1, h - 2, role);
}

static void checkbox(humbug_ui* ui, int y, bool checked, bool focused) {
    const unsigned role = focused ? HB_ACTIVE : HB_MUTED;
    outline(ui, 27, y, 11, 11, role);
    if (!checked)
        return;
    for (int i = 0; i < 3; ++i)
        fill(ui, 29 + i, y + 5 + i, 1, 2, role);
    for (int i = 0; i < 4; ++i)
        fill(ui, 31 + i, y + 7 - i, 1, 2, role);
}

static void arrow(humbug_ui* ui, int x, int y, bool right, unsigned role) {
    for (int i = 0; i < 3; ++i) {
        const int xx = x + (right ? 2 - i : i);
        fill(ui, xx, y - i, 1, 1, role);
        fill(ui, xx, y + i, 1, 1, role);
    }
}

static void options_page(humbug_ui* ui) {
    static const char* const labels[] = {"CHRISTMAS BLOCKLIST", "CUSTOM BLOCKLIST",
                                         "AUTO-STOP AFTER 10 SKIPS", "THEME",
                                         "RESET CUSTOM BLOCKLIST"};
    sprite(ui, HB_SPR_OPTIONS_PANEL);
    text(ui, "OPTIONS", 20, 37, 141, 1, HB_INK, LEFT, false);
    for (int i = 0; i < OPT_COUNT; ++i) {
        const bool focused = ui->focus == (option_id)i;
        const int y = 54 + i * 16;
        if (focused) {
            fill(ui, 19, y, 282, 16, HB_PANEL);
            outline(ui, 19, y, 282, 16, HB_ACTIVE);
        }
        text(ui, labels[i], i <= OPT_CUSTOM || i == OPT_AUTO_STOP ? 45 : 27, y + 5, 230, 1,
             focused ? HB_ACTIVE : HB_INK, LEFT,
             false);
        if (i == OPT_CHRISTMAS)
            checkbox(ui, y + 2, ui->options.christmas_blocklist, focused);
        if (i == OPT_CUSTOM)
            checkbox(ui, y + 2, ui->options.custom_blocklist, focused);
        if (i == OPT_AUTO_STOP)
            checkbox(ui, y + 2, ui->options.auto_stop, focused);
        if (i == OPT_THEME) {
            text(ui, hb_theme_names[ui->theme], 142, y + 5, 137, 1, focused ? HB_ACTIVE : HB_MUTED, RIGHT,
                 false);
            arrow(ui, 133, y + 8, false, focused ? HB_ACTIVE : HB_EDGE);
            arrow(ui, 287, y + 8, true, focused ? HB_ACTIVE : HB_EDGE);
        }
    }
}

static void reset_dock(humbug_ui* ui) {
    dim(ui);
    sprite(ui, HB_SPR_DECK_DOCK);
    text(ui, "RESET CUSTOM BLOCKLIST?", 17, 89, 286, 1, HB_WARN, LEFT, false);
    text(ui, "REMOVE MANUALLY BLOCKED SONGS", 17, 106, 286, 1, HB_INK, LEFT, false);
    dock_buttons(ui, "4 CANCEL", "8 RESET");
}

static void present(humbug_ui* ui) {
    /* A software image decoder may have cached an expanded indexed image. */
    lv_image_cache_drop(ui->buf);
    lv_draw_buf_flush_cache(ui->buf, NULL);
    lv_obj_invalidate(ui->canvas);
}

static void repaint(humbug_ui* ui) {
    fill(ui, 0, 0, HUMBUG_WIDTH, HUMBUG_HEIGHT, HB_BG);
    header(ui);
    if (ui->view == VIEW_PLAYING)
        playback(ui);
    else if (ui->view == VIEW_OPTIONS)
        options_page(ui);
    else
        dash(ui);
    nav(ui);
    if (ui->modal == MODAL_BLOCK)
        deck_dock(ui);
    else if (ui->modal == MODAL_RESET)
        reset_dock(ui);
    present(ui);
    const humbug_room* r = selected_room(ui);
    const bool animate_levels =
        ui->view == VIEW_PLAYING && r && r->online && r->playback == HUMBUG_PLAYING && !r->levels_valid;
    if (!ui->modal && ui->view != VIEW_OPTIONS && (ui->marquee.active || animate_levels))
        lv_timer_resume(ui->animation_timer);
    else
        lv_timer_pause(ui->animation_timer);
}

static void animation_tick(lv_timer_t* timer) {
    humbug_ui* ui = lv_timer_get_user_data(timer);
    repaint(ui);
}

static void start_alert(humbug_ui* ui) {
    ui->modal = MODAL_BLOCK;
    ui->alert_started = lv_tick_get();
    ui->alert_pips = ALERT_PIPS;
    lv_timer_set_period(ui->alert_timer, SECOND_MS);
    lv_timer_reset(ui->alert_timer);
    lv_timer_resume(ui->alert_timer);
}

static void clear_alerts(humbug_ui* ui) {
    ui->alert_head = ui->alert_count = 0;
    ui->alert_index = ui->alert_total = 0;
    ui->modal = MODAL_NONE;
    lv_timer_pause(ui->alert_timer);
}

static void continue_alert(humbug_ui* ui) {
    if (--ui->alert_count) {
        ui->alert_head = (ui->alert_head + 1) % ALERT_CAP;
        ++ui->alert_index;
        start_alert(ui);
    } else
        clear_alerts(ui);
    repaint(ui);
}

static void alert_tick(lv_timer_t* timer) {
    humbug_ui* ui = lv_timer_get_user_data(timer);
    const uint32_t elapsed_ms = lv_tick_elaps(ui->alert_started);
    const uint32_t elapsed = elapsed_ms / SECOND_MS;
    if (elapsed >= ALERT_SECONDS) {
        continue_alert(ui);
        return;
    }
    lv_timer_set_period(timer, SECOND_MS - elapsed_ms % SECOND_MS);
    if (ui->alert_pips != ALERT_PIPS - elapsed) {
        ui->alert_pips = ALERT_PIPS - elapsed;
        repaint(ui);
    }
}

static void apply_palette(humbug_ui* ui) {
    for (unsigned index = 1; index < HB_ROLE_COUNT; ++index) {
        uint32_t rgb = hb_palettes[ui->theme][index];
        uint8_t r = (uint8_t)(rgb >> 16), g = (uint8_t)(rgb >> 8), b = (uint8_t)rgb;
        lv_draw_buf_set_palette(ui->buf, (uint8_t)index, lv_color32_make(r, g, b, 255));
        lv_draw_buf_set_palette(ui->buf, (uint8_t)(index + DIM),
                                lv_color32_make((uint8_t)((r * 43 + 17 * 57) / 100),
                                                (uint8_t)((g * 43 + 12 * 57) / 100),
                                                (uint8_t)((b * 43 + 23 * 57) / 100), 255));
    }
}

static void deleted(lv_event_t* e) {
    humbug_ui* ui = lv_event_get_user_data(e);
    lv_timer_delete(ui->animation_timer);
    lv_timer_delete(ui->alert_timer);
    lv_image_cache_drop(ui->buf);
    lv_draw_buf_destroy(ui->buf);
    free(ui);
}

humbug_ui* humbug_ui_create(lv_obj_t* parent, humbug_event_cb cb, void* user) {
    if (!parent)
        return NULL;
    humbug_ui* ui = calloc(1, sizeof *ui);
    if (!ui)
        return NULL;
    ui->buf = lv_draw_buf_create(HUMBUG_WIDTH, HUMBUG_HEIGHT, LV_COLOR_FORMAT_I8, LV_STRIDE_AUTO);
    if (!ui->buf) {
        free(ui);
        return NULL;
    }
    ui->animation_timer = lv_timer_create(animation_tick, 40, ui);
    if (!ui->animation_timer) {
        lv_draw_buf_destroy(ui->buf);
        free(ui);
        return NULL;
    }
    lv_timer_pause(ui->animation_timer);
    ui->alert_timer = lv_timer_create(alert_tick, SECOND_MS, ui);
    if (!ui->alert_timer) {
        lv_timer_delete(ui->animation_timer);
        lv_draw_buf_destroy(ui->buf);
        free(ui);
        return NULL;
    }
    lv_timer_pause(ui->alert_timer);
    ui->canvas = lv_canvas_create(parent);
    if (!ui->canvas) {
        lv_timer_delete(ui->animation_timer);
        lv_timer_delete(ui->alert_timer);
        lv_draw_buf_destroy(ui->buf);
        free(ui);
        return NULL;
    }
    ui->callback = cb;
    ui->options = (humbug_options){true, true, true};
    ui->user = user;
    lv_canvas_set_draw_buf(ui->canvas, ui->buf);
    lv_obj_set_pos(ui->canvas, 0, 0);
    lv_obj_remove_flag(ui->canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui->canvas, deleted, LV_EVENT_DELETE, ui);
    apply_palette(ui);
    repaint(ui);
    return ui;
}
void humbug_ui_destroy(humbug_ui* ui) {
    if (ui)
        lv_obj_delete(ui->canvas);
}
lv_obj_t* humbug_ui_object(humbug_ui* ui) {
    return ui->canvas;
}

bool humbug_ui_set_rooms(humbug_ui* ui, const humbug_room* rooms, size_t count) {
    if (count > HUMBUG_MAX_ROOMS || (count && !rooms))
        return false;
    for (size_t i = 0; i < count; ++i) {
        if (!rooms[i].id)
            return false;
        for (size_t j = 0; j < i; ++j)
            if (rooms[j].id == rooms[i].id)
                return false;
    }
    const humbug_room* cur = selected_room(ui);
    const uint64_t id = cur ? cur->id : 0;
    const bool count_changed = ui->room_count != count;
    bool rooms_changed = count_changed;
    size_t selected = 0;
    for (size_t i = 0; i < count; ++i) {
        if (i < ui->room_count && !same_room(&ui->rooms[i], &rooms[i]))
            rooms_changed = true;
        if (rooms[i].id == id)
            selected = i;
    }
    for (size_t i = 0; i < count; ++i) {
        ui->rooms[i] = rooms[i];
        ui->rooms[i].name[sizeof ui->rooms[i].name - 1] = 0;
        ui->rooms[i].title[sizeof ui->rooms[i].title - 1] = 0;
        ui->rooms[i].artist[sizeof ui->rooms[i].artist - 1] = 0;
    }
    if (ui->selected != selected)
        rooms_changed = true;
    ui->room_count = count;
    ui->selected = selected;
    if (rooms_changed && ui->view == VIEW_PLAYING)
        repaint(ui);
    return true;
}
void humbug_ui_set_status(humbug_ui* ui, bool online, bool blocker_on) {
    if (ui->online == online && ui->blocker_on == blocker_on)
        return;
    ui->online = online;
    ui->blocker_on = blocker_on;
    repaint(ui);
}
void humbug_ui_set_block_pending(humbug_ui* ui, bool pending) {
    if (ui->block_pending == pending)
        return;
    ui->block_pending = pending;
    if (ui->view == VIEW_PLAYING)
        repaint(ui);
}
void humbug_ui_set_counts(humbug_ui* ui, uint64_t all_time, uint64_t today) {
    if (ui->total == all_time && ui->today == today)
        return;
    ui->total = all_time;
    ui->today = today;
    if (ui->view == VIEW_DASH)
        repaint(ui);
}
void humbug_ui_set_latest(humbug_ui* ui, const char* title, const char* artist) {
    const char* next_title = title ? title : "";
    const char* next_artist = artist ? artist : "";
    const bool changed = strncmp(ui->latest, next_title, sizeof ui->latest - 1) != 0 ||
                         strncmp(ui->artist, next_artist, sizeof ui->artist - 1) != 0;
    snprintf(ui->latest, sizeof ui->latest, "%s", next_title);
    snprintf(ui->artist, sizeof ui->artist, "%s", next_artist);
    if (ui->modal == MODAL_BLOCK || (changed && ui->view == VIEW_DASH))
        repaint(ui);
}
static void notify_block(humbug_ui* ui, const char* title, const char* artist, bool auto_stop) {
    if (ui->alert_count < ALERT_CAP && ui->alert_total < UINT32_MAX) {
        const size_t slot = (ui->alert_head + ui->alert_count) % ALERT_CAP;
        block_alert* alert = &ui->alerts[slot];
        snprintf(alert->title, sizeof alert->title, "%s", title ? title : "");
        snprintf(alert->artist, sizeof alert->artist, "%s", artist ? artist : "");
        alert->auto_stop = auto_stop;
        if (!ui->alert_count)
            ui->alert_index = 1;
        ++ui->alert_count;
        ++ui->alert_total;
        if (ui->modal == MODAL_NONE)
            start_alert(ui);
    }
    humbug_ui_set_latest(ui, title, artist);
}
void humbug_ui_notify_block(humbug_ui* ui, const char* title, const char* artist) {
    notify_block(ui, title, artist, false);
}
void humbug_ui_notify_auto_stop(humbug_ui* ui, const char* title, const char* artist) {
    notify_block(ui, title, artist, true);
}
void humbug_ui_set_options(humbug_ui* ui, humbug_options options) {
    if (ui->options.christmas_blocklist == options.christmas_blocklist &&
        ui->options.custom_blocklist == options.custom_blocklist && ui->options.auto_stop == options.auto_stop)
        return;
    ui->options = options;
    if (ui->view == VIEW_OPTIONS)
        repaint(ui);
}
humbug_options humbug_ui_get_options(const humbug_ui* ui) {
    return ui->options;
}
bool humbug_ui_can_show_block(const humbug_ui* ui) {
    return ui->modal == MODAL_NONE;
}
size_t humbug_ui_theme_count(void) {
    return HB_THEME_COUNT;
}
const char* humbug_ui_theme_name(size_t index) {
    return index < HB_THEME_COUNT ? hb_theme_names[index] : NULL;
}

size_t humbug_ui_theme_source_count(void) {
    return HB_THEME_SOURCE_COUNT;
}

size_t humbug_ui_theme_source_index(size_t index) {
    return index < HB_THEME_COUNT ? hb_theme_source_indices[index] : HB_THEME_SOURCE_COUNT;
}

size_t humbug_ui_theme_index_from_source(size_t source_index) {
    for (size_t index = 0; index < HB_THEME_COUNT; ++index)
        if (hb_theme_source_indices[index] == source_index)
            return index;
    return HB_THEME_COUNT;
}

size_t humbug_ui_theme(const humbug_ui* ui) {
    return ui->theme;
}
bool humbug_ui_set_theme(humbug_ui* ui, size_t index) {
    if (index >= HB_THEME_COUNT)
        return false;
    if (index == ui->theme)
        return true;
    const bool scrim_changed = hb_checkerboard_scrim[ui->theme] != hb_checkerboard_scrim[index];
    ui->theme = index;
    apply_palette(ui);
    if (ui->view != VIEW_PLAYING || (ui->modal && scrim_changed))
        repaint(ui);
    else
        present(ui);
    return true;
}

static bool option_key(humbug_ui* ui, humbug_key key) {
    switch (key) {
    case HUMBUG_KEY_UP:
    case HUMBUG_KEY_DOWN:
        ui->focus = (option_id)((ui->focus + (key == HUMBUG_KEY_UP ? OPT_COUNT - 1 : 1)) % OPT_COUNT);
        repaint(ui);
        return true;
    case HUMBUG_KEY_LEFT:
    case HUMBUG_KEY_RIGHT:
        if (ui->focus != OPT_THEME)
            return false;
        humbug_ui_set_theme(ui,
                            (ui->theme + (key == HUMBUG_KEY_LEFT ? HB_THEME_COUNT - 1 : 1)) % HB_THEME_COUNT);
        if (ui->callback)
            ui->callback(ui->user, HUMBUG_EVENT_THEME, 0, 0, ui->theme);
        return true;
    case HUMBUG_KEY_ENTER:
        if (ui->focus == OPT_THEME)
            return false;
        if (ui->focus == OPT_RESET) {
            ui->modal = MODAL_RESET;
            repaint(ui);
            return true;
        }
        if (ui->focus == OPT_CHRISTMAS)
            ui->options.christmas_blocklist = !ui->options.christmas_blocklist;
        else if (ui->focus == OPT_CUSTOM)
            ui->options.custom_blocklist = !ui->options.custom_blocklist;
        else
            ui->options.auto_stop = !ui->options.auto_stop;
        repaint(ui);
        if (ui->callback)
            ui->callback(ui->user, HUMBUG_EVENT_OPTIONS_CHANGED, 0, 0, ui->theme);
        return true;
    default:
        return false;
    }
}

bool humbug_ui_press(humbug_ui* ui, humbug_key key) {
    if (ui->modal == MODAL_BLOCK) {
        /* Key 4 maps to DASH and dismisses; key 8 maps to EXIT and continues. */
        switch (key) {
        case HUMBUG_KEY_BACK:
        case HUMBUG_KEY_DASH:
            clear_alerts(ui);
            repaint(ui);
            return true;
        case HUMBUG_KEY_ENTER:
        case HUMBUG_KEY_EXIT:
            continue_alert(ui);
            return true;
        default:
            return false;
        }
    }
    if (ui->modal == MODAL_RESET) {
        if (key != HUMBUG_KEY_ENTER && key != HUMBUG_KEY_BACK && key != HUMBUG_KEY_DASH &&
            key != HUMBUG_KEY_EXIT)
            return false;
        const bool confirmed = key == HUMBUG_KEY_EXIT;
        ui->modal = MODAL_NONE;
        if (ui->alert_count)
            start_alert(ui);
        repaint(ui);
        if (confirmed && ui->callback)
            ui->callback(ui->user, HUMBUG_EVENT_RESET_CUSTOM, 0, 0, ui->theme);
        return true;
    }
    switch (key) {
    case HUMBUG_KEY_EXIT:
        if (ui->callback)
            ui->callback(ui->user, HUMBUG_EVENT_EXIT, 0, 0, ui->theme);
        return true;
    case HUMBUG_KEY_BACK:
        return true;
    case HUMBUG_KEY_OPTIONS:
        if (ui->view != VIEW_OPTIONS) {
            ui->view = VIEW_OPTIONS;
            ui->focus = OPT_CHRISTMAS;
            ui->marquee.active = false;
            repaint(ui);
        }
        return true;
    case HUMBUG_KEY_BLOCK:
        if (!humbug_ui_can_block(ui))
            return false;
        {
            const humbug_room* r = selected_room(ui);
            humbug_ui_set_block_pending(ui, true);
            ui->callback(ui->user, HUMBUG_EVENT_BLOCK, r->id, r->track_token, ui->theme);
        }
        return true;
    case HUMBUG_KEY_PLAYING:
        if (ui->view != VIEW_PLAYING) {
            ui->view = VIEW_PLAYING;
            ui->selected = 0;
        } else if (ui->room_count)
            ui->selected = (ui->selected + 1) % ui->room_count;
        repaint(ui);
        return true;
    case HUMBUG_KEY_DASH:
        ui->view = VIEW_DASH;
        repaint(ui);
        return true;
    default:
        return ui->view == VIEW_OPTIONS && option_key(ui, key);
    }
}
