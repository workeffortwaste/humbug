#include "sonos/monitor.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <utf8proc.h>

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t interrupted = 0;
volatile std::sig_atomic_t resized = 0;

void interrupt(int) {
    interrupted = 1;
}

void resize(int) {
    resized = 1;
}

class TerminalScreen {
public:
    TerminalScreen() : active_(isatty(STDOUT_FILENO) != 0) {
        if (active_)
            std::cout << "\x1b[?1049h\x1b[2J\x1b[H" << std::flush;
    }

    ~TerminalScreen() {
        if (active_)
            std::cout << "\x1b[0m\x1b[?25h\x1b[?1049l" << std::flush;
    }

    bool active() const {
        return active_;
    }

    std::size_t columns() const {
        winsize size{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
            return size.ws_col;
        return 80;
    }

    void draw(const std::string& frame) {
        if (!active_ || frame == last_frame_)
            return;
        const auto output = "\x1b[H" + frame + "\x1b[J";
        for (std::size_t written = 0; written < output.size();) {
            const auto count = write(STDOUT_FILENO, output.data() + written, output.size() - written);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::system_error(count < 0 ? errno : EIO, std::generic_category(),
                                        "Cannot draw terminal screen");
            written += static_cast<std::size_t>(count);
        }
        last_frame_ = frame;
    }

    void invalidate() {
        last_frame_.clear();
    }

private:
    bool active_;
    std::string last_frame_;
};

class TerminalInput {
public:
    TerminalInput() {
        if (!isatty(STDIN_FILENO))
            return;
        if (tcgetattr(STDIN_FILENO, &original_) != 0)
            throw std::system_error(errno, std::generic_category(), "Cannot read terminal settings");
        auto settings = original_;
        settings.c_lflag &= ~(ICANON | ECHO);
        settings.c_cc[VMIN] = 1;
        settings.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &settings) != 0)
            throw std::system_error(errno, std::generic_category(), "Cannot set terminal input mode");
        active_ = true;
    }

    ~TerminalInput() {
        if (active_ && tcsetattr(STDIN_FILENO, TCSANOW, &original_) != 0)
            std::cerr << "Humbug: Cannot restore terminal settings: "
                      << std::generic_category().message(errno) << '\n';
    }

private:
    termios original_{};
    bool active_ = false;
};

std::string terminal_text(std::string_view text, std::size_t max_columns, std::size_t& columns) {
    std::string result;
    columns = 0;
    std::size_t i = 0;
    for (; i < text.size() && columns < max_columns;) {
        utf8proc_int32_t character;
        const auto decoded = utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t*>(text.data() + i), text.size() - i, &character);
        const auto length = decoded > 0 ? static_cast<std::size_t>(decoded) : 1;
        const bool invalid = decoded < 0;
        const bool control = !invalid && (character < 0x20 || (character >= 0x7f && character <= 0x9f));
        const auto width = invalid || control ? 1U : static_cast<unsigned>(utf8proc_charwidth(character));
        if (columns + width > max_columns)
            break;

        if (control) {
            result += ' ';
        } else if (invalid) {
            result += '?';
        } else {
            result.append(text.substr(i, length));
        }
        i += length;
        columns += width;
    }
    if (i < text.size() && max_columns > 3) {
        for (int j = 0; j < 3 && !result.empty(); ++j) {
            auto start = result.size() - 1;
            while (start > 0 && (static_cast<unsigned char>(result[start]) & 0xc0) == 0x80)
                --start;
            utf8proc_int32_t character;
            utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t*>(result.data() + start),
                             result.size() - start, &character);
            columns -= static_cast<std::size_t>(utf8proc_charwidth(character));
            result.resize(start);
        }
        result += "...";
        columns += 3;
    }
    return result;
}

void print_line(std::ostream& out, std::string_view value, std::size_t width, bool dim = false,
                std::size_t dim_prefix = 0, bool dim_keys = false) {
    std::size_t used = 0;
    if (width < 4) {
        out << terminal_text(value, width, used) << "\x1b[K\n";
        return;
    }
    const auto text = terminal_text(value, width - 4, used);
    auto styled = text;
    const auto dim_range = [&](std::size_t start, std::size_t length) {
        if (start < text.size()) {
            styled.insert(start + std::min(length, text.size() - start), "\x1b[0m");
            styled.insert(start, "\x1b[2m");
        }
    };
    if (dim_keys) {
        dim_range(value.find("Esc "), 3);
        dim_range(value.find("b Block"), 1);
        dim_range(0, 1);
    } else if (dim_prefix) {
        dim_range(0, dim_prefix);
    }
    out << "\x1b[2m│\x1b[0m " << (dim ? "\x1b[2m" : "") << styled
        << (dim ? "\x1b[0m" : "") << std::string(width - 4 - used, ' ')
        << " \x1b[2m│\x1b[0m\x1b[K\n";
}

void print_border(std::ostream& out, std::string_view title, std::size_t width, bool top = false) {
    if (width < 6) {
        print_line(out, title, width);
        return;
    }
    std::size_t used = 0;
    const auto label = terminal_text(title, width - 5, used);
    out << "\x1b[2m";
    if (label.empty()) {
        out << "└";
        for (std::size_t i = 0; i < width - 2; ++i)
            out << "─";
        out << "┘";
    } else {
        out << (top ? "┌" : "├") << "─ " << label << ' ';
        for (std::size_t i = 0; i < width - 5 - used; ++i)
            out << "─";
        out << (top ? "┐" : "┤");
    }
    out << "\x1b[0m\x1b[K\n";
}

void print_header(std::ostream& out, bool online, bool blocker_active, std::size_t width) {
    constexpr std::string_view prefix = "─ Humbug ";
    constexpr std::size_t prefix_columns = 9;
    const std::string_view connection = online ? "Connected" : "Disconnected";
    const std::string_view blocker = blocker_active ? "Blocking" : "Not blocking";
    const auto status_columns = connection.size() + blocker.size() + 10;
    if (width < 2 + prefix_columns + status_columns) {
        constexpr std::string_view device_label = "Device ";
        constexpr std::string_view status_label = "Status ";
        print_border(out, "Humbug", width, true);
        print_line(out, std::string(device_label) + std::string(connection), width, false,
                   device_label.size());
        print_line(out, std::string(status_label) + std::string(blocker), width, false,
                   status_label.size());
        return;
    }
    out << "\x1b[2m┌" << prefix;
    for (std::size_t i = 0; i < width - 2 - prefix_columns - status_columns; ++i)
        out << "─";
    out << "┤ " << (online ? "\x1b[0m" : "\x1b[2m") << connection
        << "\x1b[2m ├─┤ " << (blocker_active ? "\x1b[0m" : "\x1b[2m") << blocker
        << "\x1b[2m ├─┐\x1b[0m\x1b[K\n";
}

std::string rooms_text(const humbug::Group& group) {
    std::string result;
    for (const auto& room : group.rooms) {
        if (!result.empty())
            result += " + ";
        result += room;
    }
    return result.empty() ? "Stream" : result;
}

std::string playback_text(const humbug::Group& group) {
    if (!group.connected)
        return "Offline";
    if (group.state == "PLAYING")
        return "Playing";
    if (group.state == "PAUSED_PLAYBACK")
        return "Paused";
    if (group.state == "STOPPED")
        return "Stopped";
    return {};
}

bool can_block(const humbug::Group& group) {
    return !group.id.empty() && group.connected &&
           (group.state == "PLAYING" || group.state == "PAUSED_PLAYBACK") &&
           group.track.blockable() && (group.can_skip || group.can_stop);
}

void render(const humbug::Snapshot& state, std::size_t selected, TerminalScreen& terminal) {
    const auto columns = std::min<std::size_t>(terminal.columns(), 80);
    std::ostringstream frame;

    const bool online = std::any_of(state.groups.begin(), state.groups.end(),
                                    [](const auto& group) { return group.connected; });
    print_header(frame, online, state.blocker_active, columns);
    if (state.notice_is_error && !state.notice.empty())
        print_line(frame, "Error: " + state.notice, columns);

    constexpr std::string_view total_label = "Blocked all time ";
    constexpr std::string_view today_label = "Blocked today ";
    print_line(frame, std::string(total_label) + std::to_string(state.blocks.total), columns, false,
               total_label.size());
    print_line(frame, std::string(today_label) + std::to_string(state.blocks.today), columns, false,
               today_label.size());

    const humbug::Track* latest_track = nullptr;
    if (!state.blocked_songs.empty())
        latest_track = &state.blocked_songs.back().track;
    else if (state.blocks.latest)
        latest_track = &*state.blocks.latest;
    constexpr std::string_view latest_label = "Last blocked ";
    std::string latest = std::string(latest_label) + "No tracks yet";
    if (latest_track && !latest_track->title.empty()) {
        latest = std::string(latest_label);
        if (!latest_track->artist.empty())
            latest += latest_track->artist + " / ";
        latest += latest_track->title;
    }
    print_line(frame, latest, columns, false, latest_label.size());
    if (!state.blocks.error.empty())
        print_line(frame, "Error: " + state.blocks.error, columns);
    print_border(frame, "Now playing", columns);

    if (state.groups.empty()) {
        print_line(frame, "0/0  No streams found", columns);
        print_line(frame, "Nothing playing", columns);
    } else {
        const auto& group = state.groups[selected];
        print_line(frame, std::to_string(selected + 1) + "/" +
                       std::to_string(state.groups.size()) + "  " + rooms_text(group),
                   columns);
        std::string song = "Nothing playing";
        if (!group.track.title.empty()) {
            song = (group.track.artist.empty() ? "Artist not provided" : group.track.artist) +
                   " / " + group.track.title;
        }
        auto prefix = playback_text(group);
        if (!prefix.empty())
            prefix += ' ';
        print_line(frame, prefix + song, columns, false, prefix.size());
        if (!group.error.empty())
            print_line(frame, "Error: " + group.error, columns);
    }

    const bool block_enabled = !state.groups.empty() && can_block(state.groups[selected]);
    print_border(frame, "Controls", columns);
    print_line(frame, "n Next stream    b Block    Esc Exit", columns, false, 0, true);
    if (!block_enabled)
        print_line(frame, "Block unavailable", columns, true);
    print_border(frame, {}, columns);
    terminal.draw(frame.str());
}

void print_help() {
    std::cout << "Usage: humbug-cli [--no-christmas] [--config FILE] [--device HTTP_URL] "
                 "[--listen IPV4] [--port PORT]\n"
                 "Automatically skips blocked songs in ALL groups.\n"
                 "Use --no-christmas or HUMBUG_CHRISTMAS_BLOCKING=0 to disable the built-in Christmas list.\n"
                 "Keys: n next stream, b block, Esc exit.\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        auto options = humbug::environment_options();
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                print_help();
                return 0;
            }
            if (arg == "--no-christmas") {
                options.christmas_blocking = false;
                continue;
            }
            if (i + 1 == argc)
                throw std::runtime_error("Missing value for " + arg);
            const std::string value = argv[++i];
            if (arg == "--config") {
                options.blocklist = value;
                options.explicit_blocklist = true;
            } else if (arg == "--device")
                options.devices.push_back(value);
            else if (arg == "--listen")
                options.listen_address = value;
            else if (arg == "--port") {
                unsigned port{};
                auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), port);
                if (ec != std::errc{} || end != value.data() + value.size() || port > 65535)
                    throw std::runtime_error("Invalid listener port");
                options.listen_port = static_cast<std::uint16_t>(port);
            } else
                throw std::runtime_error("Unknown option: " + arg);
        }

        std::signal(SIGINT, interrupt);
        std::signal(SIGTERM, interrupt);
        std::signal(SIGWINCH, resize);
        TerminalScreen terminal;
        TerminalInput terminal_input;
        options.log_notices = !terminal.active();
        humbug::Monitor monitor(options);
        monitor.start();
        humbug::Snapshot state;
        std::string selected;
        std::size_t index = 0;
        bool input_open = true, redraw = terminal.active();
        enum class EscapeState { none, pending, sequence };
        auto escape_state = EscapeState::none;
        std::chrono::steady_clock::time_point escape_deadline;
        while (!interrupted) {
            if (resized) {
                resized = 0;
                terminal.invalidate();
                redraw = terminal.active();
            }
            if (escape_state != EscapeState::none &&
                std::chrono::steady_clock::now() >= escape_deadline) {
                if (escape_state == EscapeState::pending)
                    interrupted = 1;
                escape_state = EscapeState::none;
                if (interrupted)
                    break;
            }
            if (monitor.take(state)) {
                for (std::size_t i = 0; i < state.groups.size(); ++i)
                    if (state.groups[i].id == selected)
                        index = i;
                if (index >= state.groups.size())
                    index = 0;
                redraw = terminal.active();
                if (state.status == "Monitor failed" && !terminal.active()) {
                    monitor.stop();
                    return 1;
                }
            }
            if (redraw) {
                render(state, index, terminal);
                redraw = false;
                if (state.status == "Monitor failed") {
                    monitor.stop();
                    return 1;
                }
            }

            pollfd input{input_open ? STDIN_FILENO : -1, POLLIN, 0};
            if (poll(&input, 1, 50) <= 0)
                continue;
            char buffer[128];
            const auto n = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (n <= 0) {
                input_open = false;
                continue;
            }
            for (ssize_t i = 0; i < n; ++i) {
                const char key = buffer[i];
                if (escape_state == EscapeState::pending) {
                    if (key == '[' || key == 'O') {
                        escape_state = EscapeState::sequence;
                        escape_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
                    } else {
                        interrupted = 1;
                    }
                    continue;
                }
                if (escape_state == EscapeState::sequence) {
                    if (key >= '@' && key <= '~')
                        escape_state = EscapeState::none;
                    else
                        escape_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
                    continue;
                }
                if (key == '\x1b') {
                    escape_state = EscapeState::pending;
                    escape_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                    continue;
                }
                if (state.groups.empty())
                    continue;
                if (key == 'n')
                    index = (index + 1) % state.groups.size();
                if (key == 'b' && can_block(state.groups[index])) {
                    const auto& group = state.groups[index];
                    monitor.block({group.id, group.track.identity(), group.occurrence});
                }
                selected = state.groups[index].id;
                redraw = terminal.active();
            }
        }
        monitor.stop();
    } catch (const std::exception& e) {
        std::cerr << "Humbug: " << e.what() << '\n';
        return 1;
    }
}
