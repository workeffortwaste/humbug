/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT
 */
#include "screen_manager.h"

#include "lvgl.h"

#include <utility>

namespace app {
ScreenManager::ScreenManager(viewmodel::BaseViewModel& vm,
                             screen::CardputerScreen::ThemeChangedCallback theme_changed,
                             screen::CardputerScreen::OptionsChangedCallback options_changed)
    : view_model_(vm), theme_changed_(std::move(theme_changed)),
      options_changed_(std::move(options_changed)) {}

void ScreenManager::set_notice_callback(screen::CardputerScreen::NoticeCallback notice_changed) {
    notice_changed_ = std::move(notice_changed);
}

bool ScreenManager::start(std::size_t theme, humbug_options options) {
    screen_ = std::make_unique<screen::CardputerScreen>(view_model_, theme_changed_, notice_changed_,
                                                        options_changed_);
    if (!screen_->build(theme, options)) {
        screen_.reset();
        return false;
    }
    lv_screen_load(screen_->root());
    return true;
}

bool ScreenManager::press_key(std::uint32_t key, bool long_pressed) {
    if (!screen_)
        return false;

    humbug_key action;
    switch (key) {
    case '4':
        action = HUMBUG_KEY_DASH;
        break;
    case LV_KEY_ESC:
        action = HUMBUG_KEY_BACK;
        break;
    case LV_KEY_UP:
    case 'f':
    case 'F':
        action = HUMBUG_KEY_UP;
        break;
    case LV_KEY_DOWN:
    case 'x':
    case 'X':
        action = HUMBUG_KEY_DOWN;
        break;
    case LV_KEY_LEFT:
    case 'z':
    case 'Z':
        action = HUMBUG_KEY_LEFT;
        break;
    case LV_KEY_RIGHT:
    case 'c':
    case 'C':
        action = HUMBUG_KEY_RIGHT;
        break;
    case '5':
        action = HUMBUG_KEY_PLAYING;
        break;
    case '6':
        action = HUMBUG_KEY_OPTIONS;
        break;
    case '7':
        action = HUMBUG_KEY_BLOCK;
        break;
    case '8':
        action = HUMBUG_KEY_EXIT;
        break;
    case LV_KEY_ENTER:
        action = HUMBUG_KEY_ENTER;
        break;
    default:
        return false;
    }

    if (!long_pressed)
        screen_->press(action);
    return true;
}
} // namespace app
