#include "asset_manager.h"
#include "base_viewmodel.h"
#include "cardputer_screen.h"
#include "screen_manager.h"
#include "theme.h"
#include "toast.h"

#include <png.h>

#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace {

constexpr std::size_t pixel_count = HUMBUG_WIDTH * HUMBUG_HEIGHT;
std::array<std::uint16_t, pixel_count> pixels{};
std::array<std::uint16_t, pixel_count> draw_buffer{};

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

void flush(lv_display_t* display, const lv_area_t* area, std::uint8_t* bytes) {
    const auto width = static_cast<std::uint32_t>(area->x2 - area->x1 + 1);
    const auto stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_RGB565);
    for (int y = area->y1; y <= area->y2; ++y)
        std::memcpy(pixels.data() + y * HUMBUG_WIDTH + area->x1,
                    bytes + static_cast<std::size_t>(y - area->y1) * stride, width * sizeof(std::uint16_t));
    lv_display_flush_ready(display);
}

void capture(lv_display_t* display, const std::filesystem::path& directory, const char* name) {
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(display);
    std::vector<std::uint8_t> rgb(pixel_count * 3);
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const auto color = pixels[i];
        rgb[i * 3] = static_cast<std::uint8_t>(((color >> 11) & 31) * 255 / 31);
        rgb[i * 3 + 1] = static_cast<std::uint8_t>(((color >> 5) & 63) * 255 / 63);
        rgb[i * 3 + 2] = static_cast<std::uint8_t>((color & 31) * 255 / 31);
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = HUMBUG_WIDTH;
    image.height = HUMBUG_HEIGHT;
    image.format = PNG_FORMAT_RGB;
    const auto path = directory / (std::string(name) + ".png");
    require(png_image_write_to_file(&image, path.string().c_str(), 0, rgb.data(), 0, nullptr) != 0,
            "cannot write " + path.string() + ": " + image.message);
    std::cout << path.string() << '\n';
}

std::string theme_directory(std::size_t index) {
    std::string result = std::to_string(index) + "-";
    for (const unsigned char ch : std::string(humbug_ui_theme_name(index)))
        result += ch == ' ' ? '-' : static_cast<char>(std::tolower(ch));
    return result;
}

humbug::Group group(const char* id, const char* room, humbug::Provider provider) {
    humbug::Group result;
    result.id = id;
    result.rooms = {room};
    result.track = {"Last Christmas", "Wham!", ""};
    result.track.elapsed_seconds = 102;
    result.track.duration_seconds = 298;
    result.state = "PLAYING";
    result.connected = true;
    result.occurrence = 1;
    result.provider = provider;
    return result;
}

// Capture each theme's normal and failure states for visual inspection.
void run_theme(lv_display_t* display, const std::filesystem::path& root, std::size_t theme) {
    const auto directory = root / theme_directory(theme);
    std::filesystem::create_directories(directory);
    viewmodel::BaseViewModel model;
    app::AssetManager assets;
    view::widgets::Toast toast(lv_layer_top(), model, assets);
    toast.build();
    screen::CardputerScreen screen(
        model, [](std::size_t) {},
        [&](const std::string& message) { toast.show(message, view::widgets::ToastTone::Error, 4500); });
    require(screen.build(theme), "cannot build screen");
    lv_screen_load(screen.root());
    const auto snap = [&](const char* name) { capture(display, directory, name); };
    const auto press = [&](humbug_key key) { require(screen.press(key), "navigation key was not consumed"); };

    snap("dashboard-offline-empty");
    press(HUMBUG_KEY_PLAYING);
    snap("playing-empty");
    press(HUMBUG_KEY_DASH);

    humbug::Snapshot state;
    state.groups = {group("kitchen", "Kitchen", humbug::Provider::Sonos),
                    group("office", "Office", humbug::Provider::Cast)};
    state.blocks.total = 25184;
    state.blocks.today = 1000;
    state.blocks.latest = humbug::Track{"Previous track", "Previous artist", ""};
    state.blocker_active = true;
    model.apply(state);
    snap("dashboard-online");
    state.blocker_active = false;
    model.apply(state);
    snap("dashboard-blocker-off");
    state.blocker_active = true;
    model.apply(state);

    press(HUMBUG_KEY_PLAYING);
    snap("playing-sonos");
    press(HUMBUG_KEY_BLOCK);
    require(toast.visible() && model.snapshot().notice == "Block unavailable: monitor not running",
            "block error was not shown");
    snap("playing-block-error");
    toast.hide();
    state.block_processing = true;
    model.apply(state);
    snap("playing-block-pending");
    state.block_processing = false;
    for (const auto& entry : std::array<std::pair<const char*, const char*>, 3>{
             {{"playing-paused", "PAUSED_PLAYBACK"}, {"playing-stopped", "STOPPED"}, {"playing-idle", "UNKNOWN"}}}) {
        state.groups[0].state = entry.second;
        model.apply(state);
        snap(entry.first);
    }
    state.groups[0].state = "PLAYING";
    state.groups[0].connected = false;
    model.apply(state);
    snap("playing-room-offline");
    state.groups[0].connected = true;
    state.groups[0].track.elapsed_seconds.reset();
    model.apply(state);
    snap("playing-no-progress");
    press(HUMBUG_KEY_PLAYING);
    snap("playing-cast");

    press(HUMBUG_KEY_DASH);
    state.blocked_songs = {{1, {"Last Christmas", "Wham!", ""}, "Kitchen"},
                           {2, {"Second track", "Second artist", ""}, "Office"}};
    model.apply(state);
    snap("alert-block-queued");
    for (int seconds = 1; seconds <= 3; ++seconds) {
        lv_tick_inc(1000);
        lv_timer_handler();
        const auto name = "alert-block-" + std::to_string(seconds) + "s";
        snap(name.c_str());
    }
    press(HUMBUG_KEY_ENTER);
    snap("alert-block-next");
    press(HUMBUG_KEY_ENTER);
    state.blocked_songs.push_back({3, {"Auto stopped", "Artist", ""}, "Kitchen", true});
    model.apply(state);
    snap("alert-auto-stop");
    press(HUMBUG_KEY_ENTER);

    press(HUMBUG_KEY_OPTIONS);
    snap("settings-christmas");
    press(HUMBUG_KEY_ENTER);
    snap("settings-christmas-off");
    press(HUMBUG_KEY_DOWN);
    snap("settings-custom");
    press(HUMBUG_KEY_ENTER);
    snap("settings-custom-off");
    press(HUMBUG_KEY_DOWN);
    snap("settings-auto-stop");
    press(HUMBUG_KEY_ENTER);
    snap("settings-auto-stop-off");
    press(HUMBUG_KEY_DOWN);
    snap("settings-theme");
    press(HUMBUG_KEY_DOWN);
    snap("settings-reset");
    press(HUMBUG_KEY_ENTER);
    snap("reset-confirmation");
    press(HUMBUG_KEY_DASH);
    snap("reset-cancelled");
    press(HUMBUG_KEY_ENTER);
    press(HUMBUG_KEY_EXIT);
    require(toast.visible() && model.snapshot().notice.find("Could not reset custom blocklist") == 0,
            "reset error was not shown");
    snap("reset-confirmed-error");
    toast.hide();

    // Keep rare error toasts visible for review without needing real device failures.
    const std::array<std::pair<const char*, const char*>, 17> errors = {{
        {"error-block-unavailable", "Block unavailable: monitor not running"},
        {"error-block-stream-changed", "Block unavailable: stream changed"},
        {"error-block-track-changed", "Block unavailable: track changed"},
        {"error-block-cancelled", "Block cancelled: song/group changed"},
        {"error-block-action", "Block saved; BLOCK ACTION FAILED: connection failed"},
        {"error-block-save", "BLOCK NOT SAVED: permission denied; skipped"},
        {"error-block-save-and-action", "BLOCK NOT SAVED: permission denied; BLOCK ACTION FAILED: connection failed"},
        {"error-auto-block", "Automatic block action failed in Kitchen: connection failed"},
        {"error-cast-disconnected", "Cast disconnected: Office"},
        {"error-group", "Group Kitchen: connection failed"},
        {"error-no-speakers", "No Sonos speakers found; retrying discovery"},
        {"error-discovery", "Discovery/topology failed: connection failed"},
        {"error-upnp-callback", "No initial UPnP callback from Kitchen; check firewall and callback address/port 1400"},
        {"error-subscription", "Subscription lost: connection failed"},
        {"error-upnp", "Invalid/lost UPnP state: connection failed"},
        {"error-event-queue", "Event queue overflow; resynchronising"},
        {"error-reset", "Could not reset custom blocklist: permission denied"},
    }};
    for (const auto& [name, message] : errors) {
        state.notice = message;
        state.notice_is_error = true;
        model.apply(state);
        require(toast.visible(), std::string("notice was not shown: ") + name);
        snap(name);
        toast.hide();
    }
    state.notice.clear();
    state.blocks.error = "history file unavailable";
    model.apply(state);
    require(toast.visible(), "history error was not shown");
    snap("error-history");
    toast.hide();
    toast.show("Screenshot failed: Unable to open PNG file", view::widgets::ToastTone::Error, 3200);
    snap("error-screenshot");
    toast.show_highlighted("Saved to ", "/home/user/Pictures/Screenshots/humbug.png", "",
                           view::widgets::ToastTone::Success, 5200, true);
    snap("screenshot-saved");
}

// Reset keys must cancel or confirm without navigating away or exiting.
void check_reset_keys() {
    viewmodel::BaseViewModel model;
    app::ScreenManager manager(model, [](std::size_t) {}, [](humbug_options) {});
    require(manager.start(0, {true, true, true}) && manager.press_key('6', false), "cannot open settings");
    for (int i = 0; i < 4; ++i)
        require(manager.press_key(LV_KEY_DOWN, false), "cannot select reset");
    require(manager.press_key(LV_KEY_ENTER, false) && manager.press_key('4', false),
            "cannot cancel reset with 4");
    require(model.current_page() == model::AppPage::Options && model.snapshot().notice.empty(),
            "4 must cancel reset without navigating");
    require(manager.press_key(LV_KEY_ENTER, false) && manager.press_key('8', false),
            "cannot confirm reset with 8");
    require(model.snapshot().notice.find("Could not reset custom blocklist") == 0 &&
                lv_subject_get_int(model.quit_requested_subject()) == 0,
            "8 must request reset without exiting");
}

// Generate CLI states alongside the device screenshots and surface runner failures.
void capture_cli(const std::filesystem::path& output) {
    std::vector<std::string> arguments = {
        HUMBUG_DEBUG_PYTHON, HUMBUG_DEBUG_CLI_SCRIPT, "--debug", "--cli", HUMBUG_DEBUG_CLI,
        "--simulator", HUMBUG_DEBUG_SIMULATOR, "--samples", HUMBUG_DEBUG_SAMPLES,
        "--output-dir", (output / "cli").string(),
    };
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    pid_t pid;
    const int error = posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
    if (error)
        throw std::system_error(error, std::generic_category(), "cannot start CLI screenshot runner");
    int status;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited == -1 && errno == EINTR);
    if (waited == -1)
        throw std::system_error(errno, std::generic_category(), "cannot wait for CLI screenshot runner");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "CLI screenshot runner failed");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [output-directory]\n";
        return 2;
    }
    try {
        const std::filesystem::path output = argc == 2 ? argv[1] : "debug";
        lv_init();
        auto* display = lv_display_create(HUMBUG_WIDTH, HUMBUG_HEIGHT);
        require(display != nullptr, "cannot create display");
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(display, draw_buffer.data(), nullptr, sizeof(draw_buffer),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(display, flush);
        view::apply_lvgl_theme(display, false, LV_FONT_DEFAULT);
        for (std::size_t theme = 0; theme < humbug_ui_theme_count(); ++theme)
            run_theme(display, output, theme);
        check_reset_keys();
        lv_display_delete(display);
        lv_deinit();
        capture_cli(output);
    } catch (const std::exception& error) {
        std::cerr << "Debug screenshot runner: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
