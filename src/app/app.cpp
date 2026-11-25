/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"

#include "application_config.h"
#include "asset_manager.h"
#include "logger.h"
#include "linux_input.h"
#include "screenshot_service.h"
#include "screen_manager.h"
#include "base_viewmodel.h"
#include "theme.h"
#include "toast.h"

#include <cstdlib>
#include <csignal>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#if USE_DESKTOP
#include "desktop_simulator_frame.h"
#endif

#if !USE_DESKTOP
#if APP_USE_DRM
#include "src/drivers/display/drm/lv_linux_drm.h"
#else
#include "src/drivers/display/fb/lv_linux_fbdev.h"
#endif
#endif

#ifndef APP_FRAMEBUFFER_DEVICE
#define APP_FRAMEBUFFER_DEVICE "/dev/fb0"
#endif

#ifndef APP_DRM_DEVICE
#define APP_DRM_DEVICE "/dev/dri/card0"
#endif

#ifndef APP_DRM_CONNECTOR_ID
#define APP_DRM_CONNECTOR_ID -1
#endif

#ifndef APP_CONFIG_FILE
#define APP_CONFIG_FILE "humbug.conf"
#endif

namespace app {
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) {
    interrupted = 1;
}

void quit_requested_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* running = static_cast<bool*>(lv_observer_get_user_data(observer));
    if (running && lv_subject_get_int(subject)) {
        *running = false;
    }
}

struct ConfigPersistence {
    std::string config_path;
    ApplicationConfig config;
};

std::string writable_config_path() {
#if USE_DESKTOP
    return APP_CONFIG_FILE;
#else
    if (const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME")) {
        const std::filesystem::path root(xdg_config_home);
        if (!root.empty() && root.is_absolute()) {
            return (root / "humbug" / "humbug.conf").string();
        }
    }
    if (const char* home = std::getenv("HOME")) {
        const std::filesystem::path root(home);
        if (!root.empty() && root.is_absolute()) {
            return (root / ".config" / "humbug" / "humbug.conf").string();
        }
    }
    return APP_CONFIG_FILE;
#endif
}

void persist_config(ConfigPersistence& persistence) {
    std::string error;
    if (save_application_config(persistence.config_path, persistence.config, error)) {
        LOG_INFO("saved config: {} (dark_mode={}, theme={})", persistence.config_path,
                 persistence.config.dark_mode ? "yes" : "no", persistence.config.theme_index);
    } else {
        LOG_WARN("failed to save config {}: {}", persistence.config_path, error);
    }
}

void persist_dark_mode_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* persistence = static_cast<ConfigPersistence*>(lv_observer_get_user_data(observer));
    if (!persistence) {
        return;
    }
    const bool dark_mode = lv_subject_get_int(subject) != 0;
    if (dark_mode == persistence->config.dark_mode)
        return;
    persistence->config.dark_mode = dark_mode;
    persist_config(*persistence);
}

class ShortcutController {
public:
    ShortcutController(viewmodel::BaseViewModel& view_model, AssetManager& assets,
                       ScreenManager& screen_manager)
        : view_model_(view_model), screen_manager_(screen_manager),
          toast_(std::make_unique<view::widgets::Toast>(lv_layer_top(), view_model, assets)) {
        toast_->build();
        platform::set_global_key_listener(global_key_listener, this);
        screen_manager_.set_notice_callback([this](const std::string& message) {
            toast_->show(message, view::widgets::ToastTone::Error, 4500);
        });
    }

    ~ShortcutController() {
        platform::clear_global_key_listener(global_key_listener, this);
    }

private:
    bool handle_key(uint32_t key, bool long_pressed) {
        if (key == platform::kKeyPrintScreen) {
            if (!long_pressed) {
                const auto result = platform::screenshot::capture_active_screen();
                if (result.success) {
                    LOG_INFO("screenshot saved: {}", result.path);
                    toast_->show_highlighted("Saved to ", result.path, "", view::widgets::ToastTone::Success,
                                             5200, true);
                } else {
                    LOG_WARN("screenshot failed: {}", result.error);
                    toast_->show("Screenshot failed: " + result.error, view::widgets::ToastTone::Error, 3200);
                }
            }
            return true;
        }

        if (key == LV_KEY_ESC && long_pressed) {
            view_model_.request_quit();
            return true;
        }

        return screen_manager_.press_key(key, long_pressed);
    }

    static bool global_key_listener(uint32_t key, const char*, bool long_pressed, void* user_data) {
        auto* controller = static_cast<ShortcutController*>(user_data);
        return controller && controller->handle_key(key, long_pressed);
    }

    viewmodel::BaseViewModel& view_model_;
    ScreenManager& screen_manager_;
    std::unique_ptr<view::widgets::Toast> toast_;
};

#if !USE_DESKTOP
lv_display_t* init_device_display() {
#if APP_USE_DRM
    auto* display = lv_linux_drm_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_drm_set_file(display, APP_DRM_DEVICE, APP_DRM_CONNECTOR_ID) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#else
    auto* display = lv_linux_fbdev_create();
    if (!display) {
        return nullptr;
    }

    if (lv_linux_fbdev_set_file(display, APP_FRAMEBUFFER_DEVICE) != LV_RESULT_OK) {
        lv_display_delete(display);
        return nullptr;
    }

    platform::init_key_input(display);
    return display;
#endif
}
#endif

} // namespace

int Application::run() {
    logger::Logger::init();
    logger::Logger::set_tag("humbug");

    lv_init();

    viewmodel::BaseViewModel view_model;
    const std::string user_config_path = writable_config_path();
    std::string loaded_config_path = APP_CONFIG_FILE;
    if (user_config_path != APP_CONFIG_FILE) {
        std::error_code filesystem_error;
        if (std::filesystem::is_regular_file(user_config_path, filesystem_error)) {
            loaded_config_path = user_config_path;
        }
    }

    ApplicationConfig config;
    std::string config_error;
    const bool config_loaded = load_application_config(loaded_config_path, config, config_error);
    if (config_loaded) {
        view_model.set_dark_mode(config.dark_mode);
    } else {
        LOG_WARN("failed to load config {}: {}; using defaults", loaded_config_path, config_error);
    }

    if (config_loaded && config.logging) {
        const auto log_path = (std::filesystem::path(user_config_path).parent_path() / "humbug.log").string();
        std::string log_error;
        if (logger::Logger::set_file(log_path, log_error)) {
            logger::Logger::set_level(logger::LogLevel::DEBUG);
            LOG_INFO("file logging enabled: {}", log_path);
        } else {
            LOG_WARN("failed to enable file logging: {}", log_error);
        }
    }
    if (config_loaded) {
        LOG_INFO("loaded config: {} (dark_mode={})", loaded_config_path, config.dark_mode ? "yes" : "no");
    }

    AssetManager assets;
    for (const auto& root : assets.roots()) {
        LOG_INFO("asset root: {}", root.string());
    }

    ConfigPersistence config_persistence{user_config_path, config};

#if USE_DESKTOP
    DesktopSimulatorFrame simulator_frame(assets);
    auto* display = simulator_frame.display();
#else
    auto* display = init_device_display();
#endif
    if (!display) {
        LOG_ERROR("failed to initialize display");
        return 1;
    }
    const auto display_width = lv_display_get_horizontal_resolution(display);
    const auto display_height = lv_display_get_vertical_resolution(display);
    if (display_width != view::kScreenWidth || display_height != view::kScreenHeight) {
        LOG_ERROR("Cardputer UI requires {}x{} display, got {}x{}", view::kScreenWidth, view::kScreenHeight,
                  display_width, display_height);
        lv_display_delete(display);
        return 1;
    }

    auto* standard_font = assets.load_standard_font(14);
    view::apply_lvgl_theme(display, view_model.is_dark_mode(),
                           standard_font ? standard_font : LV_FONT_DEFAULT);

#if USE_DESKTOP
    simulator_frame.bind_dark_mode(view_model.dark_mode_subject());
#endif

    auto monitor_options = humbug::environment_options();
    if (!std::getenv("HUMBUG_CHRISTMAS_BLOCKING"))
        monitor_options.christmas_blocking = config.christmas_blocking;
    monitor_options.custom_blocking = config.custom_blocking;
    monitor_options.auto_stop = config.auto_stop;

    ScreenManager screen_manager(
        view_model,
        [&](std::size_t theme) {
            const auto source_index = humbug_ui_theme_source_index(theme);
            if (config_persistence.config.theme_index == source_index)
                return;
            config_persistence.config.theme_index = source_index;
            persist_config(config_persistence);
        },
        [&](humbug_options options) {
            config_persistence.config.christmas_blocking = options.christmas_blocklist;
            config_persistence.config.custom_blocking = options.custom_blocklist;
            config_persistence.config.auto_stop = options.auto_stop;
            persist_config(config_persistence);
        });
    ShortcutController shortcuts(view_model, assets, screen_manager);
    if (!screen_manager.start(humbug_ui_theme_index_from_source(config.theme_index),
                              {monitor_options.christmas_blocking, monitor_options.custom_blocking,
                               monitor_options.auto_stop})) {
        LOG_ERROR("failed to initialize Cardputer UI");
        return 1;
    }
    interrupted = 0;
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    view_model.start(std::move(monitor_options));

    auto* dark_mode_observer = lv_subject_add_observer(view_model.dark_mode_subject(),
                                                       persist_dark_mode_observer, &config_persistence);

    bool running = true;
    auto* quit_observer =
        lv_subject_add_observer(view_model.quit_requested_subject(), quit_requested_observer, &running);

    LOG_INFO("LVGL app started at {}x{}", lv_display_get_horizontal_resolution(display),
             lv_display_get_vertical_resolution(display));
    while (running && !interrupted
#if USE_DESKTOP
           && simulator_frame.process_events()
#endif
    ) {
        view_model.tick();
        lv_timer_handler();
        lv_delay_ms(5);
    }
    view_model.stop();

    if (quit_observer) {
        lv_observer_remove(quit_observer);
    }
    if (dark_mode_observer) {
        lv_observer_remove(dark_mode_observer);
    }

    return 0;
}

} // namespace app
