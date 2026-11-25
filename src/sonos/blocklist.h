#pragma once

#include "types.h"
#include <filesystem>

namespace humbug {
enum class RuleKind { Artist, Track, Exact, Keyword };
struct Rule {
    RuleKind kind;
    std::string value, title;
};
std::vector<Rule> parse_rules(const std::string& contents);
bool blocked(const Track& track, const std::vector<Rule>& rules);
std::string exact_rule(const Track& track);

class Blocklist {
public:
    Blocklist(std::string path, bool required, bool christmas_blocking = true);
    bool matches(const Track& track) const;
    void add(const Track& track);
    void set_enabled(bool christmas, bool custom);
    void reset_saved_tracks();

private:
    void save(std::string next);
    std::string path_, contents_;
    bool existed_ = false;
    bool christmas_blocking_;
    bool custom_blocking_ = true;
    std::vector<Rule> rules_, christmas_rules_;
};
} // namespace humbug
