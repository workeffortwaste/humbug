#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace humbug {

struct Track {
    std::string title{}, artist{}, album{}, uri{}, number{};
    std::optional<std::uint32_t> elapsed_seconds{}, duration_seconds{};
    std::string identity() const;
    bool blockable() const {
        return !title.empty() && !artist.empty();
    }
};

struct Service {
    std::string type, control, events;
};

struct Device {
    std::string uuid, name, location;
    Service transport, topology;
};

enum class Provider { Sonos, Cast };

struct Group {
    std::string id, coordinator, location;
    std::vector<std::string> rooms;
    Track track;
    std::string state = "UNKNOWN";
    std::string error;
    bool connected = false;
    std::uint64_t occurrence = 0;
    Provider provider = Provider::Sonos;
    bool can_skip = true;
    bool can_stop = false;
};

struct BlockStats {
    std::uint64_t today = 0, total = 0;
    std::vector<std::string> recent;
    std::string error;
    std::optional<Track> latest;
};

struct BlockedSong {
    std::uint64_t sequence = 0;
    Track track;
    std::string room;
    bool auto_stop = false;
};

struct Snapshot {
    std::vector<Group> groups;
    std::string status = "Discovering Sonos speakers...";
    std::string notice;
    BlockStats blocks;
    bool blocker_active = false;
    std::vector<BlockedSong> blocked_songs;
    bool notice_is_error = true;
    bool block_processing = false; // At least one manual block request is queued or executing.
};

struct BlockCommand {
    std::string group, identity;
    std::uint64_t occurrence = 0;
};

struct Options {
    std::string blocklist;
    bool explicit_blocklist = false;
    bool christmas_blocking = true;
    bool custom_blocking = true;
    bool auto_stop = true;
    std::string listen_address;
    std::uint16_t listen_port = 0;
    // Explicit description URLs bypass multicast, useful on routed LANs and in tests.
    std::vector<std::string> devices;
    bool sonos_enabled = true;
    bool discover_cast = true;
    std::vector<std::string> cast_devices;
    std::string cast_interface;
    std::chrono::seconds discovery_interval{30};
    std::chrono::seconds subscription_timeout{300};
    std::string history_path;
    bool log_notices = true;
};

std::string default_blocklist_path();
Options environment_options();

} // namespace humbug
