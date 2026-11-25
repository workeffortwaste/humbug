#pragma once

#include "sonos/types.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace humbug::cast {

struct Endpoint {
    std::string id, name, address;
    std::uint16_t port = 8009;
};

struct Media {
    std::string application, transport;
    std::int64_t session = 0, item = 0;
    Track track;
    std::string state;
    bool can_skip = false;
};

std::vector<Endpoint> discover(const std::atomic_bool& stop, const std::string& interface = {});
Media read(const Endpoint& endpoint, const std::atomic_bool& stop, const Media* previous = nullptr);
void next(const Endpoint& endpoint, const Media& expected, const std::atomic_bool& stop);
// Returns true when playback was stopped rather than advanced.
bool block(const Endpoint& endpoint, const Media& expected, const std::atomic_bool& stop,
           bool force_stop = false);

} // namespace humbug::cast
