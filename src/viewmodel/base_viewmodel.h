/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "subjects.h"
#include "app_page.h"
#include "sonos/monitor.h"

namespace viewmodel {
class BaseViewModel {
public:
    BaseViewModel();
    ~BaseViewModel();
    lv_subject_t* title_subject() {
        return title_.native();
    }
    lv_subject_t* dark_mode_subject() {
        return dark_mode_.native();
    }
    lv_subject_t* quit_requested_subject() {
        return quit_.native();
    }
    lv_subject_t* state_subject() {
        return revision_.native();
    }
    bool is_dark_mode() const {
        return dark_mode_.value();
    }
    void set_dark_mode(bool value) {
        dark_mode_.set(value);
    }
    void toggle_dark_mode() {
        dark_mode_.toggle();
        changed();
    }
    void request_quit() {
        quit_.set(true);
    }
    model::AppPage current_page() const {
        return page_;
    }
    void show_dashboard();
    void show_options();
    void show_or_cycle_playing();
    void set_blocklists(bool christmas, bool custom);
    void set_auto_stop(bool enabled);
    void reset_custom_blocklist();
    void start(humbug::Options options);
    void stop();
    void tick();
    void apply(humbug::Snapshot snapshot);
    void block();
    bool block_for_group(const std::string& group_id, const std::string& identity, std::uint64_t occurrence);
    bool can_block() const;
    std::string block_unavailable_reason() const;
    const humbug::Snapshot& snapshot() const {
        return state_;
    }
    const humbug::Group* selected() const;
    std::string room_title() const;

private:
    void changed();
    reactive::StringSubject<32> title_{"HUMBUG::"};
    reactive::BoolSubject dark_mode_{false}, quit_{false};
    reactive::IntSubject revision_{0};
    std::unique_ptr<humbug::Monitor> monitor_;
    humbug::Snapshot state_;
    std::size_t selected_ = 0;
    model::AppPage page_ = model::AppPage::Dashboard;
};
} // namespace viewmodel
