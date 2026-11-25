#include "blocklist.h"
#include "christmas_blocklist.h"
#include "network.h"

#include <utf8proc.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace humbug {
namespace {
std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
std::string lower(std::string s) {
    for (auto& ch : s)
        if (ch >= 'A' && ch <= 'Z')
            ch += 'a' - 'A';
    return s;
}

std::string normalize_metadata(std::string_view text) {
    if (text.empty())
        return {};
    utf8proc_uint8_t* mapped = nullptr;
    constexpr auto options = static_cast<utf8proc_option_t>(UTF8PROC_CASEFOLD | UTF8PROC_COMPAT |
                                                            UTF8PROC_DECOMPOSE | UTF8PROC_STRIPMARK);
    auto remaining =
        utf8proc_map(reinterpret_cast<const utf8proc_uint8_t*>(text.data()), text.size(), &mapped, options);
    const std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> buffer(mapped, &std::free);
    if (remaining < 0)
        throw std::runtime_error(utf8proc_errmsg(remaining));

    std::string out;
    bool space = false;
    for (auto p = buffer.get(); remaining > 0;) {
        utf8proc_int32_t ch;
        const auto size = utf8proc_iterate(p, remaining, &ch);
        const auto* start = p;
        p += size;
        remaining -= size;
        const auto category = utf8proc_category(ch);
        if (!(category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_LO) &&
            !(category >= UTF8PROC_CATEGORY_ND && category <= UTF8PROC_CATEGORY_NO)) {
            space = !out.empty();
            continue;
        }
        if (space)
            out += ' ';
        space = false;
        out.append(reinterpret_cast<const char*>(start), size);
    }
    return out;
}

std::string compact(std::string normalized) {
    std::erase(normalized, ' ');
    return normalized;
}

bool matches_keyword(std::string_view normalized, std::string_view keyword) {
    for (auto pos = normalized.find(keyword); pos != std::string_view::npos;
         pos = normalized.find(keyword, pos + 1)) {
        const auto end = pos + keyword.size();
        if ((pos == 0 || normalized[pos - 1] == ' ') && (end == normalized.size() || normalized[end] == ' '))
            return true;
    }
    return false;
}

std::vector<std::string> title_variants(std::string_view title) {
    std::vector<std::string> variants{compact(std::string(title))};
    // Keep real subtitles intact; only remove a recognized recording suffix.
    const auto is_version = [](std::string_view version) {
        return version == "live" || version.starts_with("live at ") || version == "acoustic version" ||
               version == "radio edit" || version == "remaster" || version == "remastered" ||
               version.starts_with("remastered ") || version.ends_with(" remaster") ||
               version.ends_with(" remastered");
    };
    for (auto pos = title.find(' '); pos != std::string_view::npos; pos = title.find(' ', pos + 1)) {
        if (is_version(title.substr(pos + 1)))
            variants.push_back(compact(std::string(title.substr(0, pos))));
    }
    return variants;
}

std::string escape(const std::string& s) {
    std::string out;
    for (const auto ch : s) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            out += ch;
        }
    }
    return out;
}
std::string unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') {
            out += s[i];
            continue;
        }
        if (++i == s.size())
            throw std::runtime_error("Incomplete blocklist escape");
        switch (s[i]) {
        case '\\':
            out += '\\';
            break;
        case 't':
            out += '\t';
            break;
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        default:
            throw std::runtime_error("Unknown blocklist escape");
        }
    }
    return out;
}
std::pair<bool, std::string> read(const std::string& path) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec)
        throw std::runtime_error("Cannot inspect blocklist: " + ec.message());
    if (!exists)
        return {false, ""};
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot read blocklist: " + path);
    std::string s{std::istreambuf_iterator<char>(in), {}};
    if (in.bad())
        throw std::runtime_error("Blocklist read failed");
    return {true, s};
}
} // namespace

std::string default_blocklist_path() {
    if (const auto* xdg = std::getenv("XDG_CONFIG_HOME");
        xdg && *xdg && std::filesystem::path(xdg).is_absolute())
        return std::string(xdg) + "/humbug/custom.blocklist";
    if (const auto* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config/humbug/custom.blocklist";
    throw std::runtime_error("Set HOME, XDG_CONFIG_HOME or an explicit blocklist path");
}

std::vector<Rule> parse_rules(const std::string& contents) {
    std::vector<Rule> rules;
    std::istringstream input(contents);
    std::string line;
    while (std::getline(input, line)) {
        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed.starts_with('#'))
            continue;
        if (line.ends_with('\r'))
            line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            throw std::runtime_error("Expected blocklist field:value");
        auto field = lower(trim(line.substr(0, colon)));
        auto v = line.substr(colon + 1);
        if (field == "exact") {
            const auto tab = v.find('\t');
            if (tab == std::string::npos || v.find('\t', tab + 1) != std::string::npos)
                throw std::runtime_error("Exact rule needs escaped artist<TAB>title");
            auto artist = unescape(v.substr(0, tab)), title = unescape(v.substr(tab + 1));
            if (artist.empty() || title.empty())
                throw std::runtime_error("Empty exact track rule");
            rules.push_back({RuleKind::Exact, artist, title});
        } else {
            if (v.find('\t') != std::string::npos)
                throw std::runtime_error("Unexpected tab in blocklist rule: " + field);
            RuleKind kind;
            if (field == "artist")
                kind = RuleKind::Artist;
            else if (field == "track")
                kind = RuleKind::Track;
            else if (field == "keyword")
                kind = RuleKind::Keyword;
            else
                throw std::runtime_error("Invalid blocklist rule: " + field);
            v = trim(v);
            if (kind == RuleKind::Artist || kind == RuleKind::Track)
                v = compact(normalize_metadata(v));
            else
                v = normalize_metadata(v);
            if (v.empty())
                throw std::runtime_error("Empty blocklist rule: " + field);
            rules.push_back({kind, std::move(v), ""});
        }
    }
    return rules;
}

bool blocked(const Track& t, const std::vector<Rule>& rules) {
    std::optional<std::string> artist, title, album;
    std::optional<std::vector<std::string>> variants;
    for (const auto& r : rules) {
        if (r.kind == RuleKind::Exact) {
            if (t.artist == r.value && t.title == r.title)
                return true;
        } else if (r.kind == RuleKind::Artist) {
            if (!artist)
                artist = compact(normalize_metadata(t.artist));
            if (*artist == r.value)
                return true;
        } else if (r.kind == RuleKind::Track) {
            if (!title)
                title = normalize_metadata(t.title);
            if (!variants)
                variants = title_variants(*title);
            if (std::find(variants->begin(), variants->end(), r.value) != variants->end())
                return true;
        } else if (r.kind == RuleKind::Keyword) {
            if (!title)
                title = normalize_metadata(t.title);
            if (!album)
                album = normalize_metadata(t.album);
            if (matches_keyword(*title, r.value) || matches_keyword(*album, r.value))
                return true;
        }
    }
    return false;
}
std::string exact_rule(const Track& t) {
    if (!t.blockable())
        throw std::runtime_error("Cannot block a track without artist and title");
    return "exact:" + escape(t.artist) + '\t' + escape(t.title) + '\n';
}

Blocklist::Blocklist(std::string path, bool required, bool christmas_blocking)
    : path_(std::move(path)), christmas_blocking_(christmas_blocking) {
    auto [exists, contents] = read(path_);
    if (!exists && required)
        throw std::runtime_error("Blocklist does not exist: " + path_);
    existed_ = exists;
    contents_ = std::move(contents);
    rules_ = parse_rules(contents_);
    christmas_rules_ = parse_rules(christmas_blocklist);
}

bool Blocklist::matches(const Track& track) const {
    return (custom_blocking_ && blocked(track, rules_)) ||
           (christmas_blocking_ && blocked(track, christmas_rules_));
}

void Blocklist::set_enabled(bool christmas, bool custom) {
    christmas_blocking_ = christmas;
    custom_blocking_ = custom;
}

void Blocklist::add(const Track& t) {
    const auto line = exact_rule(t);
    for (const auto& r : rules_)
        if (r.kind == RuleKind::Exact && r.value == t.artist && r.title == t.title) {
            save(contents_);
            return;
        }
    auto next = contents_;
    if (!next.empty() && !next.ends_with('\n'))
        next += '\n';
    next += line;
    save(std::move(next));
}

void Blocklist::reset_saved_tracks() {
    std::string next;
    for (std::size_t start = 0; start < contents_.size();) {
        const auto newline = contents_.find('\n', start);
        const auto end = newline == std::string::npos ? contents_.size() : newline + 1;
        const auto line = contents_.substr(start, end - start);
        const auto colon = line.find(':');
        if (colon == std::string::npos || lower(trim(line.substr(0, colon))) != "exact")
            next += line;
        start = end;
    }
    if (next != contents_)
        save(std::move(next));
}

void Blocklist::save(std::string next) {
    const auto parent = std::filesystem::path(path_).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent);
    Socket lock(open((path_ + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (lock.get() < 0 || flock(lock.get(), LOCK_EX | LOCK_NB) < 0)
        throw std::runtime_error("Blocklist is locked or unwritable");
    const auto [exists, contents] = read(path_);
    // Do not overwrite edits made after this instance loaded the file.
    if (exists != existed_ || contents != contents_)
        throw std::runtime_error("Blocklist changed externally; restart to load it before saving rules");
    if (next == contents_)
        return;
    auto next_rules = parse_rules(next);
    std::string temporary = path_ + ".XXXXXX";
    Socket fd(mkstemp(temporary.data()));
    if (fd.get() < 0)
        throw std::runtime_error("Cannot create temporary blocklist");
    try {
        std::size_t offset = 0;
        while (offset < next.size()) {
            const auto n = write(fd.get(), next.data() + offset, next.size() - offset);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                throw std::runtime_error("Cannot write blocklist");
            offset += static_cast<std::size_t>(n);
        }
        if (fsync(fd.get()) < 0)
            throw std::runtime_error("Cannot flush blocklist");
        if (read(path_) != std::make_pair(existed_, contents_))
            throw std::runtime_error("Blocklist changed while saving");
        if (rename(temporary.c_str(), path_.c_str()) < 0)
            throw std::runtime_error("Cannot replace blocklist");
        contents_ = next;
        rules_ = std::move(next_rules);
        existed_ = true;
        Socket directory(open((parent.empty() ? "." : parent.string()).c_str(), O_DIRECTORY | O_RDONLY));
        if (directory.get() < 0 || fsync(directory.get()) < 0)
            throw std::runtime_error("Blocklist written but directory flush failed; durability uncertain");
    } catch (...) {
        unlink(temporary.c_str());
        throw;
    }
}
} // namespace humbug
