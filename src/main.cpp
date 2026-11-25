/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "app.h"
#include <exception>
#include <iostream>

int main() {
    try {
        app::Application application;
        return application.run();
    } catch (const std::exception& e) {
        std::cerr << "Humbug: " << e.what() << '\n';
        return 1;
    }
}
