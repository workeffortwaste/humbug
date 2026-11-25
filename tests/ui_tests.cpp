#include "application_config.h"
#include "base_viewmodel.h"
#include "cardputer_screen.h"
#include "humbug_art.h"
#include "humbug_generated.h"
#include "humbug_ui.h"
#include "logger.h"
#include "screen_manager.h"
#include "theme.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

bool canvas_has_text(lv_obj_t* canvas, const std::string& text, std::uint32_t x, std::uint32_t y,
                     std::uint8_t role, std::uint32_t scale = 1) {
    auto* draw = lv_canvas_get_draw_buf(canvas);
    for (const unsigned char character : text) {
        for (std::uint32_t row = 0; row < 7; ++row) {
            for (std::uint32_t column = 0; column < 5; ++column) {
                if (!(hb_glyphs[character][row] & (16 >> column)))
                    continue;
                for (std::uint32_t dy = 0; dy < scale; ++dy) {
                    for (std::uint32_t dx = 0; dx < scale; ++dx) {
                        const auto* pixel = static_cast<const std::uint8_t*>(
                            lv_draw_buf_goto_xy(draw, x + column * scale + dx, y + row * scale + dy));
                        if (*pixel != role)
                            return false;
                    }
                }
            }
        }
        x += 6 * scale;
    }
    return true;
}

void require_canvas_text(lv_obj_t* canvas, const std::string& text, std::uint32_t x, std::uint32_t y,
                         std::uint8_t role, std::uint32_t scale = 1) {
    if (!canvas_has_text(canvas, text, x, y, role, scale))
        throw std::runtime_error("canvas text not rendered: " + text);
}

humbug::Group group(std::string id, std::string room) {
    humbug::Group result;
    result.id = std::move(id);
    result.rooms = {std::move(room)};
    result.track = {"Example track", "Example artist", "Example album"};
    result.state = "PLAYING";
    result.connected = true;
    result.occurrence = 3;
    return result;
}

// Settings must survive restarts, including configs using a removed theme.
void test_config_theme_persistence() {
    const auto path = std::filesystem::temp_directory_path() / "humbug-ui-theme-test.conf";
    struct RemoveFile {
        std::filesystem::path path;
        ~RemoveFile() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{path};

    app::ApplicationConfig written;
    require(written.auto_stop, "auto-stop is enabled by default");
    written.dark_mode = true;
    written.theme_index = humbug_ui_theme_source_index(humbug_ui_theme_count() - 1);
    written.custom_blocking = false;
    written.auto_stop = false;
    written.logging = true;
    std::string error;
    require(app::save_application_config(path.string(), written, error), "save theme config");

    app::ApplicationConfig loaded;
    require(app::load_application_config(path.string(), loaded, error), "load theme config");
    require(loaded.dark_mode && loaded.theme_index == written.theme_index,
            "saved theme and existing dark-mode settings both round-trip");
    require(loaded.christmas_blocking && !loaded.custom_blocking,
            "independent blocklist settings round-trip");
    require(!loaded.auto_stop, "auto-stop setting round-trips");
    require(loaded.logging, "file logging setting round-trips");

    {
        std::ofstream legacy(path);
        legacy << "[application]\ntheme=2\n";
    }
    loaded = {};
    require(app::load_application_config(path.string(), loaded, error),
            "load config with a theme removed from the embedded set");
    require(loaded.theme_index == humbug_ui_theme_source_index(0),
            "removed saved themes fall back to Side A");
    require(!loaded.logging, "missing logging setting remains disabled");
}

// Enabled file logging must actually persist debug entries.
void test_logger_file_output() {
    const auto path = std::filesystem::temp_directory_path() / "humbug-ui-logging-test.log";
    struct RemoveLogFile {
        std::filesystem::path path;
        ~RemoveLogFile() {
            logger::Logger::close_file();
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{path};

    std::string error;
    require(logger::Logger::set_file(path.string(), error), "open log file");
    logger::Logger::set_level(logger::LogLevel::DEBUG);
    logger::Logger::debug("file logging test");
    logger::Logger::close_file();
    logger::Logger::set_level(logger::LogLevel::INFO);

    std::ifstream input(path);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    require(contents.find("[D] file logging test") != std::string::npos, "write debug entry to log file");
}

} // namespace

int main() {
    try {
        test_logger_file_output();
        test_config_theme_persistence();
        lv_init();
        auto* display = lv_display_create(view::kScreenWidth, view::kScreenHeight);
        require(display != nullptr, "create 320x170 display");
        std::vector<std::uint32_t> pixels(view::kScreenWidth * view::kScreenHeight);
        lv_display_set_buffers(display, pixels.data(), nullptr,
                               static_cast<std::uint32_t>(pixels.size() * sizeof(std::uint32_t)),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(display, [](lv_display_t* active, const lv_area_t*, std::uint8_t*) {
            lv_display_flush_ready(active);
        });
        view::apply_lvgl_theme(display, false, LV_FONT_DEFAULT);

        {
            viewmodel::BaseViewModel view_model;
            std::size_t persisted_theme = 0;
            std::vector<humbug_options> options_changes;
            std::vector<std::string> notices;
            screen::CardputerScreen cardputer(
                view_model, [&](std::size_t theme) { persisted_theme = theme; },
                [&](const std::string& message) { notices.push_back(message); },
                [&](humbug_options options) { options_changes.push_back(options); });
            require(cardputer.build(0), "create cassette screen");
            lv_screen_load(cardputer.root());
            lv_obj_update_layout(cardputer.root());

            require(lv_obj_get_width(cardputer.root()) == HUMBUG_WIDTH &&
                        lv_obj_get_height(cardputer.root()) == HUMBUG_HEIGHT,
                    "screen uses native Cardputer dimensions");
            auto* canvas = lv_obj_get_child(cardputer.root(), 0);
            require(lv_obj_check_type(canvas, &lv_canvas_class) && lv_obj_get_width(canvas) == HUMBUG_WIDTH &&
                        lv_obj_get_height(canvas) == HUMBUG_HEIGHT,
                    "indexed LVGL canvas fills the native display");
            require(humbug_ui_theme_count() == 4 &&
                        std::string(humbug_ui_theme_name(0)) == "Side A" &&
                        std::string(humbug_ui_theme_name(1)) == "Side B" &&
                        std::string(humbug_ui_theme_name(2)) == "Blank Tape" &&
                        std::string(humbug_ui_theme_name(3)) == "Bootleg",
                    "the cassette theme names are correct");
            require_canvas_text(canvas, "4 DASH", 17, 155, HB_ACTIVE_INK);
            require_canvas_text(canvas, "5 PLAYING", 73, 155, HB_MUTED);
            require_canvas_text(canvas, "6 SETUP", 146, 155, HB_MUTED);
            require_canvas_text(canvas, "7 BLOCK", 207, 155, HB_EDGE);
            require_canvas_text(canvas, "8 EXIT", 268, 155, HB_MUTED);

            const auto badge_has_role = [&](std::uint8_t role) {
                auto* draw = lv_canvas_get_draw_buf(canvas);
                for (std::uint32_t y = 4; y < 21; ++y) {
                    for (std::uint32_t x = 238; x < 311; ++x) {
                        const auto* pixel = static_cast<const std::uint8_t*>(lv_draw_buf_goto_xy(draw, x, y));
                        if (*pixel == role)
                            return true;
                    }
                }
                return false;
            };
            require(!badge_has_role(HB_GREEN) && !badge_has_role(HB_GREEN_HI) &&
                        !badge_has_role(HB_GREEN_LO) && badge_has_role(HB_EDGE) &&
                        *static_cast<const std::uint8_t*>(
                            lv_draw_buf_goto_xy(lv_canvas_get_draw_buf(canvas), 241, 8)) == HB_WELL,
                    "inactive blocker badge uses disabled button colours, not green");
            const auto header_pixel = [&](std::uint32_t x, std::uint32_t y) {
                return *static_cast<const std::uint8_t*>(
                    lv_draw_buf_goto_xy(lv_canvas_get_draw_buf(canvas), x, y));
            };
            require_canvas_text(canvas, "OFFLINE", 185, 8, HB_MUTED);
            require(header_pixel(170, 12) == HB_PANEL && header_pixel(171, 12) == HB_UNDER &&
                        header_pixel(177, 12) == HB_WARN && header_pixel(231, 12) == HB_UNDER,
                    "offline badge fits its label and keeps its right edge");
            require_canvas_text(canvas, "BLOCKER ON", 244, 8, HB_EDGE);

            require(cardputer.press(HUMBUG_KEY_PLAYING), "open playback without streams");
            require_canvas_text(canvas, "DEVICE", 19, 36, HB_MUTED);
            require(!canvas_has_text(canvas, "DEVICE ---", 19, 36, HB_MUTED),
                    "device heading omits placeholder dashes");
            require_canvas_text(canvas, "NO STREAMS FOUND", 70, 36, HB_INK);
            require(!canvas_has_text(canvas, "IDLE", 155, 103, HB_MUTED),
                    "empty playback status does not show IDLE");
            require(cardputer.press(HUMBUG_KEY_DASH), "return to dashboard");

            // Connection and blocker indicators must follow backend state independently.
            humbug::Snapshot snapshot;
            snapshot.groups = {group("kitchen", "Kitchen"), group("office", "Office")};
            snapshot.groups[1].track.title = "Office track";
            snapshot.blocks.today = 9;
            snapshot.blocks.total = 21;
            snapshot.blocker_active = true;
            view_model.apply(snapshot);
            require(badge_has_role(HB_GREEN) && badge_has_role(HB_GREEN_HI),
                    "active blocker badge keeps its green styling");
            require_canvas_text(canvas, "BLOCKER ON", 244, 8, HB_GREEN_INK);
            require_canvas_text(canvas, "ONLINE", 191, 8, HB_MUTED);
            require(header_pixel(176, 12) == HB_PANEL && header_pixel(177, 12) == HB_UNDER &&
                        header_pixel(183, 12) == HB_GREEN && header_pixel(231, 12) == HB_UNDER,
                    "online badge is one glyph narrower without moving its right edge");
            snapshot.blocker_active = false;
            view_model.apply(snapshot);
            require(!badge_has_role(HB_GREEN) && !badge_has_role(HB_GREEN_HI) && !badge_has_role(HB_GREEN_LO),
                    "blocker badge returns to disabled styling when turned off");
            snapshot.blocker_active = true;
            view_model.apply(snapshot);

            require(cardputer.press(HUMBUG_KEY_PLAYING), "open playback page");
            require_canvas_text(canvas, "SONOS", 19, 36, HB_MUTED);
            require_canvas_text(canvas, "--:--", 11, 133, HB_MUTED);
            // Animation must run while playing and stop when playback is inactive.
            const auto eq_pixels = [&] {
                std::vector<std::uint8_t> result;
                auto* draw = lv_canvas_get_draw_buf(canvas);
                for (const auto x : {60, 221})
                    for (std::uint32_t y = 103; y < 120; ++y)
                        for (std::uint32_t column = 0; column < 38; ++column)
                            result.push_back(
                                *static_cast<const std::uint8_t*>(lv_draw_buf_goto_xy(draw, x + column, y)));
                return result;
            };
            const auto eq_start = eq_pixels();
            lv_tick_inc(160);
            lv_timer_handler();
            require(eq_pixels() != eq_start, "playing EQ animates without a new playback snapshot");
            lv_tick_inc(1120);
            lv_timer_handler();
            require(eq_pixels() == eq_start, "decorative EQ repeats its small loop");
            for (const auto* state : {"PAUSED_PLAYBACK", "STOPPED"}) {
                snapshot.groups[0].state = state;
                view_model.apply(snapshot);
                if (snapshot.groups[0].state == "STOPPED") {
                    require_canvas_text(canvas, "STOPPED", 146, 103, HB_MUTED);
                    for (std::uint32_t y = 104; y < 109; ++y)
                        for (std::uint32_t x = 136; x < 141; ++x)
                            require(header_pixel(x, y) == HB_WARN,
                                    "stop square uses the blocked warning colour");
                    require(!cardputer.press(HUMBUG_KEY_BLOCK), "stopped playback cannot be blocked");
                }
                const auto still = eq_pixels();
                require(still != eq_start, "inactive EQ bars are unlit");
                lv_tick_inc(160);
                lv_timer_handler();
                require(eq_pixels() == still, "inactive EQ bars do not animate");
            }
            snapshot.groups[0].state = "PLAYING";
            snapshot.groups[0].connected = false;
            view_model.apply(snapshot);
            require_canvas_text(canvas, "OFFLINE", 146, 103, HB_MUTED);
            const auto offline_eq = eq_pixels();
            lv_tick_inc(160);
            lv_timer_handler();
            require(eq_pixels() == offline_eq, "offline EQ does not animate");
            snapshot.groups[0].connected = true;
            view_model.apply(snapshot);
            snapshot.groups[0].track.elapsed_seconds = 23;
            snapshot.groups[0].track.duration_seconds = 240;
            view_model.apply(snapshot);
            require_canvas_text(canvas, "0:23", 11, 133, HB_ACTIVE);
            require_canvas_text(canvas, "4:00", 280, 133, HB_MUTED);
            require(*static_cast<const std::uint8_t*>(
                        lv_draw_buf_goto_xy(lv_canvas_get_draw_buf(canvas), 55, 135)) == HB_GREEN,
                    "available timing fills the progress bar");
            snapshot.groups[0].track.elapsed_seconds.reset();
            view_model.apply(snapshot);
            require_canvas_text(canvas, "--:--", 11, 133, HB_MUTED);
            require_canvas_text(canvas, "5 NEXT", 77, 155, HB_ACTIVE_INK);
            require(view_model.current_page() == model::AppPage::NowPlaying,
                    "playback page is synchronized with the app model");
            require(cardputer.press(HUMBUG_KEY_PLAYING) && view_model.selected()->id == "office",
                    "repeated playback key cycles selected groups");

            // Display transliteration must never change metadata used for matching.
            snapshot.groups[1].track.title = "Héroes";
            snapshot.groups[1].track.artist = "Björk [ð Þ Æ ß ẞ Ø œ Ł đ Ħ ı Ŋ]";
            view_model.apply(snapshot);
            require_canvas_text(canvas, "HEROES", 125, 63, HB_INK, 2);
            require_canvas_text(canvas, "BJORK (D TH AE SS SS O OE L D H I NG)", 49, 83, HB_ACTIVE);
            require(view_model.snapshot().groups[1].track.title == "Héroes" &&
                        view_model.snapshot().groups[1].track.artist == "Björk [ð Þ Æ ß ẞ Ø œ Ł đ Ħ ı Ŋ]",
                    "display transliteration does not alter original metadata");
            snapshot.groups[1].track.title = "Office track";
            view_model.apply(snapshot);
            // Long titles scroll within their bounds without restarting on unchanged snapshots.
            const auto title_pixels = [&] {
                std::vector<std::uint8_t> result;
                auto* draw = lv_canvas_get_draw_buf(canvas);
                for (std::uint32_t y = 55; y < 83; ++y)
                    for (std::uint32_t x = 0; x < HUMBUG_WIDTH; ++x)
                        result.push_back(*static_cast<const std::uint8_t*>(lv_draw_buf_goto_xy(draw, x, y)));
                return result;
            };
            const auto short_title = title_pixels();
            snapshot.groups[1].track.title = std::string(25, 'W');
            view_model.apply(snapshot);
            const auto start = title_pixels();
            lv_tick_inc(400);
            lv_timer_handler();
            require(title_pixels() == start, "marquee initially holds the first character");
            lv_tick_inc(900);
            lv_timer_handler();
            const auto scrolled = title_pixels();
            require(scrolled != start, "overlong title scrolls without a new playback snapshot");
            for (std::size_t y = 0; y < 28; ++y) {
                for (std::size_t x = 0; x < HUMBUG_WIDTH; ++x) {
                    if (x >= 16 && x < 294 && y >= 8 && y < 22) {
                        require((scrolled[y * HUMBUG_WIDTH + x] == HB_INK) ==
                                    (start[y * HUMBUG_WIDTH + x + 10] == HB_INK),
                                "marquee glyphs move left within the title line");
                    } else if (x < 16 || x >= 304 || y < 8 || y >= 22) {
                        require(scrolled[y * HUMBUG_WIDTH + x] == short_title[y * HUMBUG_WIDTH + x],
                                "marquee stays clipped to one line inside the deck");
                    }
                }
            }
            view_model.apply(snapshot);
            require(title_pixels() == scrolled, "unchanged snapshots do not restart the marquee");
            ++snapshot.groups[1].occurrence;
            view_model.apply(snapshot);
            require(title_pixels() == start, "a new occurrence restarts the marquee");
            snapshot.groups[1].track.title = "Office track";
            view_model.apply(snapshot);
            lv_tick_inc(1500);
            lv_timer_handler();
            require(title_pixels() == short_title, "short titles remain centered and still");

            // Incomplete metadata or unavailable actions must not enable an unsafe block.
            const auto artist = snapshot.groups[1].track.artist;
            snapshot.groups[1].track.artist.clear();
            view_model.apply(snapshot);
            require(!view_model.can_block() && !cardputer.press(HUMBUG_KEY_BLOCK),
                    "the artist placeholder does not make an unidentified song blockable");
            snapshot.groups[1].track.artist = artist;

            snapshot.groups[1].provider = humbug::Provider::Cast;
            snapshot.groups[1].can_skip = false;
            view_model.apply(snapshot);
            require_canvas_text(canvas, "CAST", 19, 36, HB_MUTED);
            require(!view_model.can_block() && !cardputer.press(HUMBUG_KEY_BLOCK),
                    "Block is disabled when neither action is available");
            snapshot.groups[1].can_stop = true;
            view_model.apply(snapshot);
            require(view_model.can_block() && cardputer.press(HUMBUG_KEY_BLOCK),
                    "Cast Block remains enabled when Stop is the fallback");
            snapshot.groups[1].can_skip = true;
            snapshot.groups[1].can_stop = false;
            view_model.apply(snapshot);

            // Reordering groups must not redirect a block to a different stream.
            std::swap(snapshot.groups[0], snapshot.groups[1]);
            view_model.apply(snapshot);
            require(view_model.selected()->id == "office",
                    "selection remains on the same group after reordering");
            require(cardputer.press(HUMBUG_KEY_BLOCK) &&
                        view_model.snapshot().notice == "Block unavailable: monitor not running",
                    "block events resolve the current room and track before acting");
            require(!notices.empty() && notices.back() == view_model.snapshot().notice,
                    "block operation failures are surfaced to the user");

            // Queued confirmations must remain readable and suppress unrelated navigation.
            const auto notice_count = notices.size();
            snapshot.notice = "Block saved; skipped";
            snapshot.notice_is_error = false;
            snapshot.blocked_songs = {{1, {std::string(100, 'A'), "Artist one", ""}, "Kitchen"},
                                      {2, {"Second blocked", "Artist two", ""}, "Office"}};
            view_model.apply(snapshot);
            require(notices.size() == notice_count,
                    "successful blocks show the insert modal without an extra toast");
            require_canvas_text(canvas, std::string(44, 'A') + "...", 17, 109, HB_INK);
            require_canvas_text(canvas, "ARTIST ONE", 17, 128, HB_MUTED);
            require_canvas_text(canvas, "1/2", 277, 89, HB_ACTIVE_INK);
            const auto require_pips = [&](unsigned active) {
                for (unsigned i = 0; i < 3; ++i) {
                    const auto x = 225 + i * 27;
                    require(header_pixel(x, 130) == (i < active ? HB_ACTIVE : HB_SHELL),
                            "Deck Dock countdown pips show remaining seconds");
                }
            };
            require_pips(3);
            require(!cardputer.press(HUMBUG_KEY_PLAYING) && view_model.selected()->id == "office",
                    "the Now Playing shortcut is ignored while the Deck Dock is open");
            require(!cardputer.press(HUMBUG_KEY_THEME),
                    "theme changes are ignored while the Deck Dock is open");
            for (unsigned remaining = 2; remaining > 0; --remaining) {
                lv_tick_inc(1000);
                lv_timer_handler();
                require_pips(remaining);
                require_canvas_text(canvas, std::string(44, 'A') + "...", 17, 109, HB_INK);
            }
            lv_tick_inc(1000);
            lv_timer_handler();
            require_canvas_text(canvas, "2/2", 277, 89, HB_ACTIVE_INK);
            require_canvas_text(canvas, "SECOND BLOCKED", 17, 106, HB_INK, 2);
            require_canvas_text(canvas, "ARTIST TWO", 17, 128, HB_MUTED);
            require_pips(3);
            require(cardputer.press(HUMBUG_KEY_EXIT), "the on-screen Continue key advances the alert");
            require(!cardputer.press(HUMBUG_KEY_ENTER),
                    "a dismissed alert does not leave a stale active modal");
            snapshot.blocked_songs.push_back({3, {"Third blocked", "Artist three", ""}, "Office", true});
            view_model.apply(snapshot);
            require_canvas_text(canvas, "THIRD BLOCKED", 17, 106, HB_INK, 2);
            require_canvas_text(canvas, "AUTO STOP", 207, 89, HB_ACTIVE_INK);
            require(cardputer.press(HUMBUG_KEY_DASH) &&
                        view_model.current_page() == model::AppPage::NowPlaying,
                    "the on-screen Dismiss key closes the alert without navigating away");
            snapshot.notice = "BLOCK NOT SAVED: test failure";
            snapshot.notice_is_error = true;
            view_model.apply(snapshot);
            require(notices.back() == snapshot.notice, "blocklist save failures still show a toast");
            require(cardputer.press(HUMBUG_KEY_DASH) && header_pixel(100, 30) == HB_SHELL_LOW &&
                        header_pixel(100, 141) == HB_SHELL_LOW,
                    "the dashboard cassette face spans rows 30 through 141");
            // Options and reset confirmation must preserve independent settings and safe defaults.
            require(cardputer.press(HUMBUG_KEY_OPTIONS) &&
                        view_model.current_page() == model::AppPage::Options,
                    "Setup action opens the Options page");
            require_canvas_text(canvas, "6 SETUP", 146, 155, HB_ACTIVE_INK);
            require_canvas_text(canvas, "OPTIONS", 20, 37, HB_INK);
            require(header_pixel(100, 29) == HB_BG && header_pixel(100, 30) == HB_EDGE &&
                        header_pixel(100, 141) == HB_EDGE && header_pixel(100, 142) == HB_BG,
                    "Options aligns with the cassette face without extending below it");
            require(header_pixel(23, 132) == HB_WELL, "Options leaves the former control hint area clear");
            require(!cardputer.press(HUMBUG_KEY_BLOCK) && cardputer.press(HUMBUG_KEY_ENTER) &&
                        options_changes.size() == 1 && !options_changes.back().christmas_blocklist &&
                        options_changes.back().custom_blocklist,
                    "Christmas toggle does not change Custom");
            require(cardputer.press(HUMBUG_KEY_DOWN) && cardputer.press(HUMBUG_KEY_ENTER) &&
                        options_changes.size() == 2 && !options_changes.back().christmas_blocklist &&
                        !options_changes.back().custom_blocklist,
                    "Custom can be disabled independently");
            require(cardputer.press(HUMBUG_KEY_DOWN), "select auto-stop above Theme");
            require_canvas_text(canvas, "AUTO-STOP AFTER 10 SKIPS", 45, 91, HB_ACTIVE);
            require(cardputer.press(HUMBUG_KEY_ENTER) && options_changes.size() == 3 &&
                        !options_changes.back().auto_stop,
                    "auto-stop can be disabled independently");
            require(cardputer.press(HUMBUG_KEY_DOWN) && cardputer.press(HUMBUG_KEY_RIGHT) &&
                        persisted_theme == 1,
                    "theme changes on its Options row");
            require(cardputer.press(HUMBUG_KEY_LEFT) && cardputer.press(HUMBUG_KEY_LEFT) &&
                        persisted_theme == humbug_ui_theme_count() - 1,
                    "the theme picker wraps to the last retained palette");
            require_canvas_text(canvas, "BOOTLEG", 208, 107, HB_ACTIVE);
            require(cardputer.press(HUMBUG_KEY_RIGHT) && persisted_theme == 0,
                    "the theme picker wraps back to Side A");
            require(cardputer.press(HUMBUG_KEY_RIGHT) && persisted_theme == 1 &&
                        cardputer.press(HUMBUG_KEY_RIGHT) && persisted_theme == 2,
                    "the theme picker selects the second and third retained palettes");
            require(std::string(humbug_ui_theme_name(persisted_theme)) == "Blank Tape",
                    "Blank Tape is selectable");
            require(cardputer.press(HUMBUG_KEY_DOWN) && cardputer.press(HUMBUG_KEY_ENTER),
                    "the retained monochrome theme can open the reset prompt");
            require(cardputer.press(HUMBUG_KEY_BACK) && cardputer.press(HUMBUG_KEY_UP),
                    "return to the Theme row");
            require(cardputer.press(HUMBUG_KEY_RIGHT) && persisted_theme == 3,
                    "Bootleg is selectable");
            require(std::string(humbug_ui_theme_name(persisted_theme)) == "Bootleg",
                    "the picker reaches the last retained theme");
            require_canvas_text(canvas, "BOOTLEG", 208, 107, HB_ACTIVE);
            require(cardputer.press(HUMBUG_KEY_DOWN) && cardputer.press(HUMBUG_KEY_ENTER),
                    "reset opens confirmation");
            const auto before_cancel = notices.size();
            require(cardputer.press(HUMBUG_KEY_ENTER) && notices.size() == before_cancel,
                    "Enter cancels without resetting");
            require(cardputer.press(HUMBUG_KEY_ENTER) && cardputer.press(HUMBUG_KEY_DASH) &&
                        notices.size() == before_cancel && view_model.current_page() == model::AppPage::Options,
                    "4 cancels without navigating away");
            require(cardputer.press(HUMBUG_KEY_ENTER) && cardputer.press(HUMBUG_KEY_BACK) &&
                        notices.size() == before_cancel,
                    "Escape cancels without resetting");
            require(cardputer.press(HUMBUG_KEY_ENTER) && cardputer.press(HUMBUG_KEY_EXIT) &&
                        notices.size() == before_cancel + 1 &&
                        notices.back().starts_with("Could not reset custom blocklist"),
                    "8 confirms reset without exiting");
            snapshot.blocks.error = "history file unavailable";
            view_model.apply(snapshot);
            require(notices.back() == "History error: history file unavailable",
                    "history write failures are surfaced to the user");
            snapshot.blocks.error.clear();
            view_model.apply(snapshot);
            require(cardputer.press(HUMBUG_KEY_PLAYING) && view_model.selected()->id == "office",
                    "playback room navigation stays aligned with the app model");
            require(cardputer.press(HUMBUG_KEY_DASH) &&
                        view_model.current_page() == model::AppPage::Dashboard,
                    "dashboard page is synchronized with the app model");
            require_canvas_text(canvas, "THIRD BLOCKED", 201, 119, HB_INK);
            require_canvas_text(canvas, "ARTIST THREE", 201, 130, HB_MUTED);

            // The dashboard must animate long titles without moving short titles or artist text.
            snapshot.blocked_songs.clear();
            snapshot.blocks.latest.emplace();
            snapshot.blocks.latest->title = "Short title";
            snapshot.blocks.latest->artist = "The Example Artist with a long name";
            view_model.apply(snapshot);
            const auto latest_panel = [&] {
                std::vector<std::uint8_t> result;
                auto* draw = lv_canvas_get_draw_buf(canvas);
                for (std::uint32_t y = 101; y < 141; ++y)
                    for (std::uint32_t x = 194; x < 310; ++x)
                        result.push_back(*static_cast<const std::uint8_t*>(lv_draw_buf_goto_xy(draw, x, y)));
                return result;
            };
            const auto short_panel = latest_panel();
            lv_tick_inc(1300);
            lv_timer_handler();
            require(latest_panel() == short_panel, "short blocked titles stay still");
            require_canvas_text(canvas, "SHORT TITLE", 201, 119, HB_INK);
            require_canvas_text(canvas, "THE EXAMPLE AR...", 201, 130, HB_MUTED);

            snapshot.blocks.latest->title = "This track title is far too long to fit";
            view_model.apply(snapshot);
            const auto latest_start = latest_panel();
            require_canvas_text(canvas, "THIS TRACK TITLE ", 201, 119, HB_INK);
            lv_tick_inc(1300);
            lv_timer_handler();
            const auto latest_scrolled = latest_panel();
            require(latest_scrolled != latest_start, "long blocked titles scroll without new snapshots");
            for (std::size_t y = 0; y < 40; ++y) {
                for (std::size_t x = 0; x < 116; ++x) {
                    if (y >= 18 && y < 25 && x >= 7 && x < 99) {
                        require((latest_scrolled[y * 116 + x] == HB_INK) ==
                                    (latest_start[y * 116 + x + 10] == HB_INK),
                                "blocked title glyphs move left within the title line");
                    } else if (y < 18 || y >= 25 || x < 7 || x >= 109) {
                        require(latest_scrolled[y * 116 + x] == latest_start[y * 116 + x],
                                "blocked title stays clipped and does not move the artist");
                    }
                }
            }
            view_model.apply(snapshot);
            require(latest_panel() == latest_scrolled,
                    "unchanged history snapshots do not restart the blocked title marquee");
            require(cardputer.press(HUMBUG_KEY_PLAYING) && view_model.selected()->id == "office",
                    "opening playback from the dashboard selects the first room");
            require(cardputer.press(HUMBUG_KEY_BACK) &&
                        lv_subject_get_int(view_model.quit_requested_subject()) == 0,
                    "short Back does not exit the app");
            require(cardputer.press(HUMBUG_KEY_EXIT) &&
                        lv_subject_get_int(view_model.quit_requested_subject()) != 0,
                    "Exit is forwarded to the app");
        }

        {
            viewmodel::BaseViewModel view_model;
            std::size_t theme = 0;
            std::vector<humbug_options> options;
            app::ScreenManager manager(view_model, [&](std::size_t value) { theme = value; },
                                       [&](humbug_options value) { options.push_back(value); });
            require(manager.start(0, {true, true, true}) && manager.press_key('6', false),
                    "open Setup through the input router");
            auto* canvas = lv_obj_get_child(lv_screen_active(), 0);
            require(manager.press_key('x', false) && manager.press_key('X', false) &&
                        manager.press_key(LV_KEY_ENTER, false) && options.size() == 1 &&
                        !options.back().auto_stop,
                    "lowercase and uppercase X navigate to auto-stop");
            require(manager.press_key('f', false) && manager.press_key('F', false),
                    "lowercase and uppercase F navigate up");
            require_canvas_text(canvas, "CHRISTMAS BLOCKLIST", 45, 59, HB_ACTIVE);
            require(manager.press_key(LV_KEY_DOWN, false) && manager.press_key(LV_KEY_DOWN, false) &&
                        manager.press_key(LV_KEY_DOWN, false),
                    "D-pad navigation remains available");
            require(manager.press_key('c', false) && theme == 1 && manager.press_key('Z', false) &&
                        theme == 0 && manager.press_key('C', false) && theme == 1 &&
                        manager.press_key('z', false) && theme == 0,
                    "letter keys change themes in both cases");
            require(manager.press_key(LV_KEY_DOWN, false) && manager.press_key(LV_KEY_ENTER, false),
                    "open reset confirmation through the input router");
            require_canvas_text(canvas, "4 CANCEL", 61, 155, HB_MUTED);
            require_canvas_text(canvas, "8 RESET", 214, 155, HB_ACTIVE_INK);
            require(manager.press_key('4', false) && view_model.current_page() == model::AppPage::Options,
                    "4 cancels reset through the input router");
            require(manager.press_key(LV_KEY_ENTER, false) && manager.press_key('8', false) &&
                        view_model.snapshot().notice.starts_with("Could not reset custom blocklist") &&
                        lv_subject_get_int(view_model.quit_requested_subject()) == 0,
                    "8 confirms reset through the input router without exiting");
        }

        lv_display_delete(display);
        std::cout << "Cardputer UI, model bridge and theme persistence tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << "UI test failure: " << error.what() << '\n';
        return 1;
    }
}
