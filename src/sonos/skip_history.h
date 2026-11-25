#pragma once

#include "types.h"

#include <map>
#include <string>

namespace humbug {
class SkipHistory {
public:
    explicit SkipHistory(std::string path);
    void record(const Track& track, const std::string& day);
    BlockStats stats(const std::string& day) const;

private:
    void count(const std::string& day, Track track, std::string song);
    std::string path_;
    std::map<std::string, std::uint64_t> daily_;
    BlockStats stats_;
    bool loaded_ = false;
};

std::string local_day();
std::string default_history_path();
} // namespace humbug
