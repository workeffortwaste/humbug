#include "skip_history.h"
#include "network.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>

namespace humbug {
std::string local_day() {
    const auto now = std::time(nullptr);
    std::tm local{};
    char date[16];
    if (!localtime_r(&now, &local) || !std::strftime(date, sizeof(date), "%Y-%m-%d", &local))
        throw std::runtime_error("Cannot determine local date for block history");
    return date;
}

std::string default_history_path() {
    if (const auto* xdg = std::getenv("XDG_STATE_HOME"); xdg && std::filesystem::path(xdg).is_absolute())
        return std::string(xdg) + "/humbug/skips.log";
    if (const auto* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.local/state/humbug/skips.log";
    throw std::runtime_error("Set HOME or XDG_STATE_HOME to store block history");
}

SkipHistory::SkipHistory(std::string path) {
    try {
        path_ = path.empty() ? default_history_path() : std::move(path);
        if (std::filesystem::exists(path_)) {
            std::ifstream input(path_);
            std::string line;
            while (std::getline(input, line)) {
                std::istringstream row(line);
                std::string day;
                Track track;
                if (!(row >> day >> std::ws) || row.peek() != '"' ||
                    !(row >> std::quoted(track.title) >> std::ws) || day.size() != 10 || day[4] != '-' ||
                    day[7] != '-' || row.peek() != '"' || !(row >> std::quoted(track.artist) >> std::ws) ||
                    !row.eof())
                    throw std::runtime_error("Invalid entry in block history");
                auto song = track.title;
                if (!track.artist.empty())
                    song += " - " + track.artist;
                count(day, std::move(track), std::move(song));
            }
            if (!input.eof() || input.bad())
                throw std::runtime_error("Cannot read block history");
        }
        loaded_ = true;
    } catch (const std::runtime_error& e) {
        stats_.error = e.what();
    }
}

void SkipHistory::count(const std::string& day, Track track, std::string song) {
    ++daily_[day];
    ++stats_.total;
    stats_.recent.insert(stats_.recent.begin(), std::move(song));
    if (stats_.recent.size() > 3)
        stats_.recent.pop_back();
    stats_.latest = std::move(track);
}

BlockStats SkipHistory::stats(const std::string& day) const {
    auto result = stats_;
    if (auto it = daily_.find(day); it != daily_.end())
        result.today = it->second;
    return result;
}

void SkipHistory::record(const Track& track, const std::string& day) {
    Track saved;
    saved.title = track.title.empty() ? "Unknown song" : track.title;
    saved.artist = track.artist;
    const auto line_break = [](char c) { return c == '\r' || c == '\n'; };
    std::replace_if(saved.title.begin(), saved.title.end(), line_break, ' ');
    std::replace_if(saved.artist.begin(), saved.artist.end(), line_break, ' ');
    auto song = saved.title;
    if (!saved.artist.empty())
        song += " - " + saved.artist;
    count(day, saved, std::move(song));
    if (!loaded_)
        return;
    try {
        const auto parent = std::filesystem::path(path_).parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent);
        const Socket fd(open(path_.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (fd.get() < 0)
            throw std::runtime_error("Cannot open block history for writing");
        std::ostringstream row;
        row << day << ' ' << std::quoted(saved.title) << ' ' << std::quoted(saved.artist) << '\n';
        const auto text = row.str();
        for (std::size_t written = 0; written < text.size();) {
            const auto n = write(fd.get(), text.data() + written, text.size() - written);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::runtime_error("Cannot write block history");
            written += static_cast<std::size_t>(n);
        }
        if (fsync(fd.get()) < 0)
            throw std::runtime_error("Cannot flush block history");
    } catch (const std::runtime_error& e) {
        stats_.error = e.what();
    }
}
} // namespace humbug
