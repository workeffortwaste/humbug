/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "cardputer_screen.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace app {
class ScreenManager {
public:
    ScreenManager(viewmodel::BaseViewModel& vm, screen::CardputerScreen::ThemeChangedCallback theme_changed,
                  screen::CardputerScreen::OptionsChangedCallback options_changed);
    void set_notice_callback(screen::CardputerScreen::NoticeCallback notice_changed);
    bool start(std::size_t theme, humbug_options options);
    bool press_key(std::uint32_t key, bool long_pressed);

private:
    viewmodel::BaseViewModel& view_model_;
    screen::CardputerScreen::ThemeChangedCallback theme_changed_;
    screen::CardputerScreen::NoticeCallback notice_changed_;
    screen::CardputerScreen::OptionsChangedCallback options_changed_;
    std::unique_ptr<screen::CardputerScreen> screen_;
};
} // namespace app
