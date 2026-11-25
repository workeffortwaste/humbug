#include "cardputer_screen.h"

#include "base_viewmodel.h"
#include "logger.h"

#include <utf8proc.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace screen {
namespace {

constexpr std::size_t kMaxRooms = HUMBUG_MAX_ROOMS;

std::uint64_t hash_text(std::string_view text, std::uint64_t hash) {
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t room_id(std::string_view id) {
    const auto hash = hash_text(id, 14695981039346656037ULL);
    return hash ? hash : 1;
}

std::uint64_t track_token(const humbug::Group& group, std::string_view identity) {
    auto hash = hash_text(group.id, 14695981039346656037ULL);
    hash ^= 0xff;
    hash *= 1099511628211ULL;
    hash = hash_text(identity, hash);
    hash ^= group.occurrence;
    hash *= 1099511628211ULL;
    return hash ? hash : 1;
}

template <std::size_t Size> void copy_text(char (&destination)[Size], std::string_view source) {
    const auto length = std::min(source.size(), Size - 1);
    std::memcpy(destination, source.data(), length);
    destination[length] = '\0';
}

std::string transliterate_display_text(std::string_view text) {
    if (text.empty())
        return {};

    utf8proc_uint8_t* mapped = nullptr;
    constexpr auto options = static_cast<utf8proc_option_t>(UTF8PROC_COMPAT | UTF8PROC_DECOMPOSE |
                                                            UTF8PROC_STRIPMARK);
    auto remaining =
        utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(text.data()), text.size(), &mapped, options);
    const std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> buffer(mapped, &std::free);
    if (remaining < 0) {
        LOG_WARN("failed to transliterate display text: {}", utf8proc_errmsg(remaining));
        return std::string(text);
    }

    struct mapping {
        utf8proc_int32_t character;
        std::string_view ascii;
    };
    static constexpr mapping latin_mappings[] = {
        {0x00c6, "AE"}, {0x00e6, "ae"}, {0x00d0, "D"},  {0x00f0, "d"},  {0x00d8, "O"},
        {0x00f8, "o"},  {0x00de, "TH"}, {0x00fe, "th"}, {0x00df, "ss"}, {0x1e9e, "SS"},
        {0x0152, "OE"}, {0x0153, "oe"}, {0x0141, "L"},  {0x0142, "l"},  {0x0110, "D"},
        {0x0111, "d"},  {0x0126, "H"},  {0x0127, "h"},  {0x0131, "i"},  {0x014a, "NG"},
        {0x014b, "ng"}, {0x0138, "k"},  {0x0166, "T"},  {0x0167, "t"},
    };

    std::string result;
    for (auto* position = buffer.get(); remaining > 0;) {
        utf8proc_int32_t character;
        const auto size = utf8proc_iterate(position, remaining, &character);
        if (size <= 0) {
            LOG_WARN("failed to decode transliterated display text");
            result += '?';
            break;
        }
        if (character <= 0x7f) {
            result += static_cast<char>(character);
        } else {
            bool replaced = false;
            for (const auto& entry : latin_mappings) {
                if (entry.character == character) {
                    result.append(entry.ascii);
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                result += '?';
        }
        position += size;
        remaining -= size;
    }
    return result;
}

std::string group_name(const humbug::Group& group) {
    std::string name;
    for (const auto& room : group.rooms) {
        if (!name.empty())
            name += " + ";
        name += room;
    }
    return name.empty() ? "STREAM" : name;
}

} // namespace

CardputerScreen::CardputerScreen(viewmodel::BaseViewModel& view_model, ThemeChangedCallback theme_changed,
                                 NoticeCallback notice_changed, OptionsChangedCallback options_changed)
    : view_model_(view_model), theme_changed_(std::move(theme_changed)),
      notice_changed_(std::move(notice_changed)), options_changed_(std::move(options_changed)) {}

CardputerScreen::~CardputerScreen() {
    if (root_ && lv_obj_is_valid(root_)) {
        lv_obj_delete(root_);
    }
}

bool CardputerScreen::build(std::size_t theme, humbug_options options) {
    root_ = lv_obj_create(nullptr);
    if (!root_) {
        LOG_ERROR("failed to create Cardputer screen root");
        return false;
    }
    lv_obj_remove_style_all(root_);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(root_, HUMBUG_WIDTH, HUMBUG_HEIGHT);

    ui_ = humbug_ui_create(root_, handle_event, this);
    if (!ui_) {
        LOG_ERROR("failed to create Cardputer LVGL UI");
        lv_obj_delete(root_);
        root_ = nullptr;
        return false;
    }
    humbug_ui_set_options(ui_, options);
    if (!humbug_ui_set_theme(ui_, theme)) {
        LOG_ERROR("invalid Cardputer theme index: {}", theme);
        lv_obj_delete(root_);
        root_ = nullptr;
        ui_ = nullptr;
        return false;
    }

    if (!lv_subject_add_observer_obj(view_model_.state_subject(), state_changed, root_, this)) {
        LOG_ERROR("failed to subscribe Cardputer UI to app state");
        lv_obj_delete(root_);
        root_ = nullptr;
        ui_ = nullptr;
        return false;
    }
    update();
    return true;
}

bool CardputerScreen::press(humbug_key key) {
    if (!ui_)
        return false;
    const bool modal_was_open = !humbug_ui_can_show_block(ui_);
    const bool consumed = humbug_ui_press(ui_, key);
    if (!modal_was_open && consumed && key == HUMBUG_KEY_DASH) {
        view_model_.show_dashboard();
    } else if (!modal_was_open && consumed && key == HUMBUG_KEY_PLAYING) {
        view_model_.show_or_cycle_playing();
    } else if (!modal_was_open && consumed && key == HUMBUG_KEY_OPTIONS) {
        view_model_.show_options();
    }
    return consumed;
}

void CardputerScreen::state_changed(lv_observer_t* observer, lv_subject_t*) {
    auto* screen = static_cast<CardputerScreen*>(lv_observer_get_user_data(observer));
    if (screen)
        screen->update();
}

void CardputerScreen::handle_event(void* user, humbug_event event, std::uint64_t room_id,
                                   std::uint64_t track_token, std::size_t theme) {
    auto* screen = static_cast<CardputerScreen*>(user);
    if (screen)
        screen->dispatch_event(event, room_id, track_token, theme);
}

void CardputerScreen::dispatch_event(humbug_event event, std::uint64_t id, std::uint64_t token,
                                     std::size_t theme) {
    switch (event) {
    case HUMBUG_EVENT_EXIT:
        view_model_.request_quit();
        return;
    case HUMBUG_EVENT_THEME:
        if (theme_changed_)
            theme_changed_(theme);
        return;
    case HUMBUG_EVENT_OPTIONS_CHANGED: {
        const auto options = humbug_ui_get_options(ui_);
        view_model_.set_blocklists(options.christmas_blocklist, options.custom_blocklist);
        view_model_.set_auto_stop(options.auto_stop);
        if (options_changed_)
            options_changed_(options);
        return;
    }
    case HUMBUG_EVENT_RESET_CUSTOM:
        view_model_.reset_custom_blocklist();
        return;
    case HUMBUG_EVENT_INSERT_CLOSED:
        return;
    case HUMBUG_EVENT_BLOCK: {
        // Map the UI token back to the snapshot identity before sending the command.
        const auto target = block_targets_.find(id);
        if (target == block_targets_.end() || target->second.token != token) {
            LOG_WARN("discarded stale Cardputer block request");
            humbug_ui_set_block_pending(ui_, false);
            return;
        }
        if (!view_model_.block_for_group(target->second.group_id, target->second.identity,
                                         target->second.occurrence))
            humbug_ui_set_block_pending(ui_, false);
        return;
    }
    }
}

void CardputerScreen::update() {
    if (!ui_)
        return;

    const auto& state = view_model_.snapshot();
    humbug_ui_set_block_pending(ui_, state.block_processing);
    const auto count = std::min(state.groups.size(), kMaxRooms);
    std::vector<humbug_room> rooms(count);
    std::unordered_map<std::uint64_t, BlockTarget> targets;
    std::unordered_set<std::uint64_t> ids;
    bool collision = false;
    bool online = false;

    for (std::size_t i = 0; i < count; ++i) {
        const auto& group = state.groups[i];
        auto& room = rooms[i];
        const auto id = room_id(group.id);
        if (!ids.insert(id).second) {
            collision = true;
            break;
        }

        room.id = id;
        room.provider = group.provider == humbug::Provider::Cast ? HUMBUG_CAST : HUMBUG_SONOS;
        room.online = group.connected;
        room.playback = group.state == "PLAYING"           ? HUMBUG_PLAYING
                        : group.state == "PAUSED_PLAYBACK" ? HUMBUG_PAUSED
                        : group.state == "STOPPED"         ? HUMBUG_STOPPED
                                                           : HUMBUG_IDLE;
        if (group.track.elapsed_seconds && group.track.duration_seconds &&
            *group.track.duration_seconds > 0) {
            room.progress_valid = true;
            room.elapsed_seconds = *group.track.elapsed_seconds;
            room.duration_seconds = *group.track.duration_seconds;
        }
        const auto name = group_name(group);
        copy_text(room.name, transliterate_display_text(name));
        copy_text(room.title, transliterate_display_text(group.track.title));
        copy_text(room.artist, transliterate_display_text(group.track.artist.empty() && !group.track.title.empty()
                                                              ? "ARTIST NOT PROVIDED"
                                                              : group.track.artist));
        online = online || group.connected;

        if (group.connected && group.state == "PLAYING" && group.track.blockable() &&
            (group.can_skip || group.can_stop)) {
            const auto identity = group.track.identity();
            const auto token = track_token(group, identity);
            room.track_token = token;
            targets.emplace(id, BlockTarget{group.id, identity, group.occurrence, token});
        }
    }

    if (collision) {
        LOG_ERROR("Cardputer stream IDs collided; showing an empty stream list");
        rooms.clear();
        targets.clear();
        online = false;
    } else if (state.groups.size() > kMaxRooms) {
        LOG_WARN("Cardputer UI displays the first {} of {} streams", kMaxRooms, state.groups.size());
    }

    if (!humbug_ui_set_rooms(ui_, rooms.data(), rooms.size())) {
        LOG_ERROR("failed to update Cardputer stream list");
        return;
    }
    block_targets_ = std::move(targets);
    humbug_ui_set_status(ui_, online, state.blocker_active);
    humbug_ui_set_counts(ui_, state.blocks.total, state.blocks.today);

    if (!state.blocked_songs.empty()) {
        const auto& latest = state.blocked_songs.back();
        latest_title_ = transliterate_display_text(latest.track.title);
        latest_artist_ = transliterate_display_text(latest.track.artist);
    } else if (state.blocks.latest) {
        latest_title_ = transliterate_display_text(state.blocks.latest->title);
        latest_artist_ = transliterate_display_text(state.blocks.latest->artist);
    } else {
        latest_title_.clear();
        latest_artist_.clear();
    }
    humbug_ui_set_latest(ui_, latest_title_.c_str(), latest_artist_.c_str());

    for (const auto& blocked : state.blocked_songs) {
        if (blocked.sequence <= last_block_sequence_)
            continue;
        last_block_sequence_ = blocked.sequence;
        const auto title = transliterate_display_text(blocked.track.title);
        const auto artist = transliterate_display_text(blocked.track.artist);
        if (blocked.auto_stop)
            humbug_ui_notify_auto_stop(ui_, title.c_str(), artist.c_str());
        else
            humbug_ui_notify_block(ui_, title.c_str(), artist.c_str());
    }

    const auto toast_notice = state.notice_is_error ? state.notice : std::string{};
    if (toast_notice != last_notice_) {
        last_notice_ = toast_notice;
        if (!last_notice_.empty() && notice_changed_)
            notice_changed_(last_notice_);
    }
    if (state.blocks.error != last_history_error_) {
        last_history_error_ = state.blocks.error;
        if (!last_history_error_.empty() && notice_changed_)
            notice_changed_("History error: " + last_history_error_);
    }
}

} // namespace screen
