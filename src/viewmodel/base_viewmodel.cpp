/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT
 */
#include "base_viewmodel.h"

#include <algorithm>
#include <iostream>

namespace viewmodel {
namespace {
std::string unavailable_reason(const humbug::Group* group) {
    if (!group)
        return "No stream";
    if (!group->connected)
        return "Stream not connected";
    if (group->state != "PLAYING")
        return "No song playing";
    if (!group->track.blockable())
        return "Artist or title missing";
    if (!group->can_skip && !group->can_stop)
        return "Receiver cannot skip or stop";
    return {};
}
} // namespace

BaseViewModel::BaseViewModel() = default;
BaseViewModel::~BaseViewModel() {
    stop();
}
void BaseViewModel::start(humbug::Options options) {
    monitor_ = std::make_unique<humbug::Monitor>(std::move(options));
    monitor_->start();
}
void BaseViewModel::stop() {
    if (monitor_)
        monitor_->stop();
}
void BaseViewModel::changed() {
    revision_.set(revision_.value() == INT32_MAX ? 0 : revision_.value() + 1);
}
const humbug::Group* BaseViewModel::selected() const {
    return selected_ < state_.groups.size() ? &state_.groups[selected_] : nullptr;
}
void BaseViewModel::tick() {
    if (!monitor_)
        return;
    humbug::Snapshot next;
    if (monitor_->take(next))
        apply(std::move(next));
}
void BaseViewModel::apply(humbug::Snapshot next) {
    const auto id = selected() ? selected()->id : "";
    state_ = std::move(next);
    for (std::size_t i = 0; i < state_.groups.size(); ++i)
        if (state_.groups[i].id == id) {
            selected_ = i;
            break;
        }
    if (selected_ >= state_.groups.size())
        selected_ = 0;
    changed();
}
void BaseViewModel::show_or_cycle_playing() {
    if (page_ != model::AppPage::NowPlaying) {
        page_ = model::AppPage::NowPlaying;
        selected_ = 0;
    } else if (state_.groups.size() > 1) {
        selected_ = (selected_ + 1) % state_.groups.size();
    } else {
        return;
    }
    changed();
}
bool BaseViewModel::can_block() const {
    return block_unavailable_reason().empty();
}
std::string BaseViewModel::block_unavailable_reason() const {
    return unavailable_reason(selected());
}
void BaseViewModel::show_dashboard() {
    if (page_ == model::AppPage::Dashboard)
        return;
    page_ = model::AppPage::Dashboard;
    changed();
}
void BaseViewModel::show_options() {
    if (page_ == model::AppPage::Options)
        return;
    page_ = model::AppPage::Options;
    changed();
}
void BaseViewModel::set_blocklists(bool christmas, bool custom) {
    if (monitor_)
        monitor_->set_blocklists(christmas, custom);
}
void BaseViewModel::set_auto_stop(bool enabled) {
    if (monitor_)
        monitor_->set_auto_stop(enabled);
}
void BaseViewModel::reset_custom_blocklist() {
    try {
        if (!monitor_)
            throw std::runtime_error("monitor not running");
        monitor_->reset_custom_blocklist();
    } catch (const std::runtime_error& error) {
        state_.notice = "Could not reset custom blocklist: " + std::string(error.what());
        state_.notice_is_error = true;
        changed();
    }
}
void BaseViewModel::block() {
    if (page_ != model::AppPage::NowPlaying)
        return;
    if (const auto reason = block_unavailable_reason(); !reason.empty()) {
        state_.notice_is_error = true;
        state_.notice = "Block unavailable: " + reason;
        changed();
        return;
    }
    if (!monitor_) {
        state_.notice_is_error = true;
        state_.notice = "Block unavailable: monitor not running";
        changed();
        return;
    }
    const auto* g = selected();
    block_for_group(g->id, g->track.identity(), g->occurrence);
}
bool BaseViewModel::block_for_group(const std::string& group_id, const std::string& identity,
                                    std::uint64_t occurrence) {
    if (page_ != model::AppPage::NowPlaying)
        return false;

    const auto found = std::find_if(state_.groups.begin(), state_.groups.end(),
                                    [&](const auto& group) { return group.id == group_id; });
    if (found == state_.groups.end()) {
        state_.notice_is_error = true;
        state_.notice = "Block unavailable: stream changed";
        changed();
        return false;
    }
    if (const auto reason = unavailable_reason(&*found); !reason.empty()) {
        state_.notice_is_error = true;
        state_.notice = "Block unavailable: " + reason;
        changed();
        return false;
    }
    if (found->track.identity() != identity || found->occurrence != occurrence) {
        state_.notice_is_error = true;
        state_.notice = "Block unavailable: track changed";
        changed();
        return false;
    }
    if (!monitor_) {
        state_.notice_is_error = true;
        state_.notice = "Block unavailable: monitor not running";
        changed();
        return false;
    }
    try {
        monitor_->block({found->id, identity, occurrence});
        return true;
    } catch (const std::exception& e) {
        state_.notice_is_error = true;
        state_.notice = e.what();
        std::cerr << "Humbug: " << e.what() << '\n';
        changed();
        return false;
    }
}
std::string BaseViewModel::room_title() const {
    const auto* g = selected();
    if (!g)
        return "No streams";
    std::string title = std::to_string(selected_ + 1) + "/" + std::to_string(state_.groups.size()) + "  ";
    for (const auto& room : g->rooms) {
        if (title.back() != ' ')
            title += " + ";
        title += room;
    }
    return title;
}
} // namespace viewmodel
