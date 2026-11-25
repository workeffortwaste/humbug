#pragma once

#include "humbug_ui.h"
#include "sonos/types.h"

#include <functional>
#include <string>
#include <unordered_map>

namespace viewmodel {
class BaseViewModel;
}

namespace screen {

class CardputerScreen {
public:
    using ThemeChangedCallback = std::function<void(std::size_t)>;
    using NoticeCallback = std::function<void(const std::string&)>;
    using OptionsChangedCallback = std::function<void(humbug_options)>;

    CardputerScreen(viewmodel::BaseViewModel& view_model, ThemeChangedCallback theme_changed,
                    NoticeCallback notice_changed, OptionsChangedCallback options_changed = {});
    ~CardputerScreen();

    bool build(std::size_t theme, humbug_options options = {true, true, true});
    lv_obj_t* root() const {
        return root_;
    }
    bool press(humbug_key key);

private:
    struct BlockTarget {
        std::string group_id;
        std::string identity;
        std::uint64_t occurrence;
        std::uint64_t token;
    };

    static void state_changed(lv_observer_t* observer, lv_subject_t* subject);
    static void handle_event(void* user, humbug_event event, std::uint64_t room_id, std::uint64_t track_token,
                             std::size_t theme);
    void update();
    void dispatch_event(humbug_event event, std::uint64_t room_id, std::uint64_t track_token,
                        std::size_t theme);
    viewmodel::BaseViewModel& view_model_;
    ThemeChangedCallback theme_changed_;
    NoticeCallback notice_changed_;
    OptionsChangedCallback options_changed_;
    lv_obj_t* root_{nullptr};
    humbug_ui* ui_{nullptr};
    std::unordered_map<std::uint64_t, BlockTarget> block_targets_;
    std::uint64_t last_block_sequence_{0};
    std::string latest_title_;
    std::string latest_artist_;
    std::string last_notice_;
    std::string last_history_error_;
};

} // namespace screen
