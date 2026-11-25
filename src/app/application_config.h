/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <string>

namespace app {

struct ApplicationConfig {
    bool dark_mode = false;
    // Original catalog ID, retained across compact runtime palette indices.
    std::size_t theme_index = 0;
    bool christmas_blocking = true;
    bool custom_blocking = true;
    bool auto_stop = true;
    bool logging = false;
};

bool load_application_config(const std::string& path, ApplicationConfig& config, std::string& error);
bool save_application_config(const std::string& path, const ApplicationConfig& config, std::string& error);

} // namespace app
