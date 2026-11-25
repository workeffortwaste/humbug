#include "fake_cast.h"

#include "cast/cast.h"
#include "sonos/monitor.h"

#include <mdns.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
using namespace std::chrono_literals;
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <class F> void rejects(F operation, const std::string& message) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
}
template <class F> humbug::Snapshot wait(humbug::Monitor& monitor, F predicate) {
    humbug::Snapshot state;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        monitor.take(state);
        if (predicate(state))
            return state;
        std::this_thread::sleep_for(20ms);
    }
    throw std::runtime_error("Timed out waiting for Cast state: " + state.status + " " + state.notice);
}
int discovery_reply(int socket, const sockaddr* from, size_t from_length, mdns_entry_type_t entry,
                    uint16_t query, uint16_t type, uint16_t, uint32_t, const void* data, size_t size,
                    size_t name_offset, size_t, size_t, size_t, void* user) {
    if (entry != MDNS_ENTRYTYPE_QUESTION || type != MDNS_RECORDTYPE_PTR)
        return 0;
    std::array<char, 256> name_buffer{};
    const auto name = mdns_string_extract(data, size, &name_offset, name_buffer.data(), name_buffer.size());
    if (std::string(name.str, name.length) != "_googlecast._tcp.local.")
        return 0;
    const auto port = *static_cast<std::uint16_t*>(user);
    mdns_record_t answer{};
    answer.name = {MDNS_STRING_CONST("_googlecast._tcp.local.")};
    answer.type = MDNS_RECORDTYPE_PTR;
    answer.data.ptr.name = {MDNS_STRING_CONST("Fake._googlecast._tcp.local.")};
    answer.ttl = 120;
    std::array<mdns_record_t, 4> additional{};
    additional[0].name = answer.data.ptr.name;
    additional[0].type = MDNS_RECORDTYPE_SRV;
    additional[0].data.srv.name = {MDNS_STRING_CONST("fake.local.")};
    additional[0].data.srv.port = port;
    additional[0].ttl = 120;
    additional[1].name = additional[0].data.srv.name;
    additional[1].type = MDNS_RECORDTYPE_A;
    additional[1].data.a.addr.sin_family = AF_INET;
    additional[1].data.a.addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    additional[1].ttl = 120;
    additional[2].name = answer.data.ptr.name;
    additional[2].type = MDNS_RECORDTYPE_TXT;
    additional[2].data.txt.key = {MDNS_STRING_CONST("id")};
    additional[2].data.txt.value = {MDNS_STRING_CONST("fake-id")};
    additional[2].ttl = 120;
    additional[3].name = answer.data.ptr.name;
    additional[3].type = MDNS_RECORDTYPE_TXT;
    additional[3].data.txt.key = {MDNS_STRING_CONST("fn")};
    additional[3].data.txt.value = {MDNS_STRING_CONST("Fake Cast")};
    additional[3].ttl = 120;
    alignas(4) std::array<char, 4096> response{};
    return mdns_query_answer_unicast(socket, from, from_length, response.data(), response.size(), query,
                                     MDNS_RECORDTYPE_PTR, name.str, name.length, answer, nullptr, 0,
                                     additional.data(), additional.size());
}
// Real loopback mDNS replies catch discovery errors without relying on LAN devices.
void discovery() {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(MDNS_PORT);
    humbug::Socket server(mdns_socket_open_ipv4(&local));
    require(server.get() >= 0, "open loopback mDNS responder");
    std::uint16_t port = 12345;
    std::jthread responder([&](std::stop_token stop) {
        alignas(4) std::array<char, 4096> buffer{};
        while (!stop.stop_requested()) {
            pollfd fd{server.get(), POLLIN, 0};
            if (poll(&fd, 1, 50) > 0)
                mdns_socket_listen(server.get(), buffer.data(), buffer.size(), discovery_reply, &port);
        }
    });
    std::atomic_bool stop{false};
    const auto found = humbug::cast::discover(stop, "127.0.0.1");
    require(found.size() == 1 && found[0].id == "fake-id" && found[0].name == "Fake Cast" &&
                found[0].address == "127.0.0.1" && found[0].port == port,
            "mDNS resolves Cast instance, TXT metadata, address and advertised port");
}
// Partial, stale and uncertain receiver replies must not cause unsafe mutations.
void protocol() {
    FakeCast fake;
    const auto colon = fake.endpoint.find(':');
    const humbug::cast::Endpoint endpoint{
        "test", "Fake", "127.0.0.1", static_cast<std::uint16_t>(std::stoi(fake.endpoint.substr(colon + 1)))};
    std::atomic_bool stop{false};
    auto media = humbug::cast::read(endpoint, stop);
    require(media.track.title == "Song" && media.track.artist == "Artist" && media.can_skip,
            "Cast metadata and queue capability are read");
    require(media.track.elapsed_seconds == 23 && media.track.duration_seconds == 240,
            "Cast current time and duration are read in seconds");
    fake.omit_media = true;
    const auto partial = humbug::cast::read(endpoint, stop, &media);
    require(partial.track.title == "Song" && partial.track.duration_seconds == 240,
            "partial Cast status retains the same item's metadata and duration");
    fake.omit_time = true;
    const auto no_time = humbug::cast::read(endpoint, stop, &partial);
    require(!no_time.track.elapsed_seconds, "missing Cast time clears stale position");
    fake.omit_time = false;
    fake.song("Replacement");
    const auto replacement = humbug::cast::read(endpoint, stop, &partial);
    require(replacement.track.title.empty() && !replacement.track.duration_seconds,
            "new item without metadata does not reuse the old track or duration");
    fake.omit_media = false;
    rejects([&] { humbug::cast::next(endpoint, media, stop); }, "stale Cast skip rejected");
    fake.supported = false;
    rejects([&] { humbug::cast::block(endpoint, media, stop); }, "stale Cast stop rejected");
    require(fake.next_count == 0 && fake.stop_count == 0, "stale Cast block sends no mutation");
    fake.supported = true;
    media = humbug::cast::read(endpoint, stop);
    fake.behavior = FakeCast::Next::Reject;
    rejects([&] { humbug::cast::next(endpoint, media, stop); }, "Cast rejection surfaced");
    fake.behavior = FakeCast::Next::Unchanged;
    rejects([&] { humbug::cast::next(endpoint, media, stop); }, "unchanged Cast status is uncertain");
    fake.behavior = FakeCast::Next::Advance;
    humbug::cast::next(endpoint, media, stop);
    require(fake.next_count == 3, "Cast queue update sent and confirmed");
    media = humbug::cast::read(endpoint, stop);
    fake.behavior = FakeCast::Next::DelayedAdvance;
    require(!humbug::cast::block(endpoint, media, stop) && fake.next_count == 4,
            "Cast block confirms an asynchronously advanced queue item");
    media = humbug::cast::read(endpoint, stop);
    fake.behavior = FakeCast::Next::LostResponse;
    rejects([&] { humbug::cast::next(endpoint, media, stop); }, "lost Cast response is uncertain");
    require(fake.next_count == 5 && fake.failure().empty(), "Cast server received requests");
}
// Recheck receiver capabilities at action time rather than trusting stale snapshots.
void capability_changes() {
    FakeCast fake;
    const auto colon = fake.endpoint.find(':');
    const humbug::cast::Endpoint endpoint{
        "test", "Fake", "127.0.0.1", static_cast<std::uint16_t>(std::stoi(fake.endpoint.substr(colon + 1)))};
    std::atomic_bool stop{false};
    auto media = humbug::cast::read(endpoint, stop);
    fake.supported = false;
    require(humbug::cast::block(endpoint, media, stop) && fake.stop_count == 1 && fake.next_count == 0,
            "fresh Cast capability selects Stop");
    fake.song("New song");
    media = humbug::cast::read(endpoint, stop);
    fake.supported = true;
    require(!humbug::cast::block(endpoint, media, stop) && fake.stop_count == 1 && fake.next_count == 1,
            "fresh Cast capability selects Next");
    require(fake.failure().empty(), fake.failure());
}
// Exercise the monitor end to end, including uncertain actions that must not repeat.
void integration(const std::filesystem::path& dir) {
    FakeCast fake;
    fake.supported = false;
    humbug::Options options;
    options.discover_cast = false;
    options.cast_devices = {fake.endpoint};
    options.sonos_enabled = false;
    options.blocklist = (dir / "rules").string();
    options.history_path = (dir / "history").string();
    humbug::Monitor monitor(options);
    monitor.start();
    auto state = wait(monitor, [](const auto& s) {
        return s.groups.size() == 1 && s.groups[0].connected && !s.groups[0].can_skip;
    });
    require(state.groups[0].provider == humbug::Provider::Cast, "Cast stream is identified");
    auto group = state.groups[0];
    require(!group.can_skip && group.can_stop && fake.next_count == 0,
            "Cast Stop is available when Next is unsupported");
    fake.supported = true;
    state = wait(monitor, [](const auto& s) { return !s.groups.empty() && s.groups[0].can_skip; });
    group = state.groups[0];
    fake.song("Replacement");
    monitor.block({group.id, group.track.identity(), group.occurrence});
    wait(monitor, [](const auto& s) { return s.notice.find("Block cancelled") != std::string::npos; });
    require(fake.next_count == 0 && !std::filesystem::exists(options.blocklist),
            "stale Cast block neither saves a rule nor sends Next");
    fake.song("Song");
    state = wait(monitor, [](const auto& s) {
        return !s.groups.empty() && s.groups[0].track.title == "Song" && s.groups[0].can_skip;
    });
    group = state.groups[0];
    monitor.block({group.id, group.track.identity(), group.occurrence});
    wait(monitor, [](const auto& s) {
        return s.blocks.total == 1 && !s.groups.empty() && s.groups[0].track.title == "Next track";
    });
    require(fake.next_count == 1, "manual Cast block skips and records once");
    fake.song("Song");
    wait(monitor, [](const auto& s) { return s.blocks.total == 2; });
    require(fake.next_count == 2, "blocklist automatically skips Cast");
    wait(monitor, [](const auto& s) {
        return !s.groups.empty() && s.groups[0].track.title == "Next track";
    });
    fake.behavior = FakeCast::Next::LostWithoutAdvance;
    fake.song("Song");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (fake.next_count < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    require(fake.next_count == 3, "uncertain automatic Cast skip was attempted");
    std::this_thread::sleep_for(2500ms);
    monitor.take(state);
    require(fake.next_count == 3 && fake.stop_count == 0 && state.blocks.total == 2,
            "uncertain Cast skip is not counted, retried, or followed by Stop");
    monitor.stop();
    require(fake.failure().empty(), fake.failure());
}
void repeat_blocked_song_stops_silently(const std::filesystem::path& dir) {
    FakeCast fake;
    fake.repeat_next = true;
    humbug::Options options;
    options.sonos_enabled = false;
    options.discover_cast = false;
    options.cast_devices = {fake.endpoint};
    options.blocklist = (dir / "repeat-stop-cast-rules").string();
    options.history_path = options.blocklist + ".history";
    options.log_notices = false;
    {
        std::ofstream out(options.blocklist);
        out << "artist:Artist\n";
    }
    humbug::Monitor monitor(options);
    monitor.start();
    const auto stopped = wait(monitor, [&](const auto& state) {
        return fake.next_count == 1 && fake.stop_count == 1 && state.blocks.total == 1;
    });
    require(stopped.blocks.total == 1 && fake.next_count == 1 && fake.stop_count == 1,
            "a repeated blocked Cast song is stopped without a second block record");
    monitor.stop();
    require(fake.failure().empty(), fake.failure());
}
// A previously blocked item must be eligible again after another track plays.
void revisit_blocked_tracks(const std::filesystem::path& dir) {
    FakeCast fake;
    fake.song_at(10, "A");
    humbug::Options options;
    options.sonos_enabled = false;
    options.discover_cast = false;
    options.cast_devices = {fake.endpoint};
    options.blocklist = (dir / "revisit-rules").string();
    options.history_path = (dir / "revisit-history").string();
    humbug::Monitor monitor(options);
    monitor.start();

    auto state = wait(monitor, [](const auto& s) {
        return !s.groups.empty() && s.groups[0].connected && s.groups[0].track.title == "A";
    });
    auto group = state.groups[0];
    monitor.block({group.id, group.track.identity(), group.occurrence});
    wait(monitor, [](const auto& s) { return s.blocks.total == 1; });

    fake.song_at(20, "B");
    state = wait(monitor, [](const auto& s) {
        return !s.groups.empty() && s.groups[0].track.title == "B";
    });
    group = state.groups[0];
    monitor.block({group.id, group.track.identity(), group.occurrence});
    wait(monitor, [](const auto& s) {
        return s.blocks.total == 2 && !s.groups.empty() && s.groups[0].track.title == "Next track";
    });

    fake.song_at(20, "B");
    wait(monitor, [](const auto& s) { return s.blocks.total == 3; });
    require(fake.next_count == 3, "revisiting the last blocked Cast item triggers a new skip");
    wait(monitor, [](const auto& s) {
        return !s.groups.empty() && s.groups[0].track.title == "Next track";
    });
    fake.song_at(10, "A");
    wait(monitor, [](const auto& s) { return s.blocks.total == 4; });
    require(fake.next_count == 4, "revisiting another blocked Cast item triggers a new skip");
    monitor.stop();
    require(fake.failure().empty(), fake.failure());
}
// Prevent an endless sequence of blocked Cast tracks while allowing playback to resume.
void auto_stop_limit(const std::filesystem::path& dir) {
    FakeCast fake;
    fake.vary_next_titles = true;
    humbug::Options options;
    options.sonos_enabled = false;
    options.discover_cast = false;
    options.cast_devices = {fake.endpoint};
    options.blocklist = (dir / "auto-stop-cast").string();
    options.history_path = options.blocklist + ".history";
    options.log_notices = false;
    {
        std::ofstream out(options.blocklist);
        out << "artist:Artist\n";
    }
    humbug::Monitor monitor(options);
    monitor.start();
    humbug::Snapshot state;
    bool labeled = false;
    const auto deadline = std::chrono::steady_clock::now() + 65s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (monitor.take(state))
            for (const auto& song : state.blocked_songs)
                labeled = labeled || song.auto_stop;
        if (fake.stop_count == 1 && state.blocks.total == 11 && labeled)
            break;
        std::this_thread::sleep_for(20ms);
    }
    require(fake.next_count == 10 && fake.stop_count == 1 && labeled,
            "Cast auto-stops after 10 skips and labels the blocked song");
    fake.resume();
    wait(monitor, [](const auto& s) { return s.blocks.total >= 12; });
    require(fake.next_count >= 11, "Cast resumes blocking the same media item after auto-stop");
    monitor.stop();
    require(fake.failure().empty(), fake.failure());
}
// A rejected skip still saves the rule, but must not be counted or retried.
void rejected_block(const std::filesystem::path& dir) {
    FakeCast fake;
    fake.behavior = FakeCast::Next::Reject;
    humbug::Options options;
    options.sonos_enabled = false;
    options.discover_cast = false;
    options.cast_devices = {fake.endpoint};
    options.blocklist = (dir / "rejected-rules").string();
    options.history_path = (dir / "rejected-history").string();
    humbug::Monitor monitor(options);
    monitor.start();
    const auto state = wait(monitor, [](const auto& s) { return !s.groups.empty() && s.groups[0].can_skip; });
    const auto& group = state.groups[0];
    monitor.block({group.id, group.track.identity(), group.occurrence});
    wait(monitor, [](const auto& s) { return s.notice.find("BLOCK ACTION FAILED") != std::string::npos; });
    std::this_thread::sleep_for(2500ms);
    humbug::Snapshot after;
    monitor.take(after);
    require(fake.next_count == 1 && after.blocks.total == 0 && std::filesystem::exists(options.blocklist),
            "rejected Cast skips remain saved, are not counted and are not retried");
    monitor.stop();
    require(fake.failure().empty(), fake.failure());
}
// Missing artist metadata must not cause a block against the wrong song.
void missing_metadata(const std::filesystem::path& dir) {
    FakeCast fake;
    fake.song("Song", "");
    humbug::Options options;
    options.sonos_enabled = false;
    options.discover_cast = false;
    options.cast_devices = {fake.endpoint};
    options.blocklist = (dir / "metadata-rules").string();
    options.history_path = (dir / "metadata-history").string();
    {
        std::ofstream out(options.blocklist);
        out << "track:Song\n";
    }
    humbug::Monitor monitor(options);
    monitor.start();
    wait(monitor,
         [](const auto& s) { return s.notice.find("artist or title missing") != std::string::npos; });
    require(fake.next_count == 0, "partial Cast metadata cannot trigger an automatic skip");
    fake.song("Song", "Artist");
    wait(monitor, [](const auto& s) { return s.blocks.total == 1; });
    require(fake.next_count == 1, "automatic blocking resumes when metadata is complete");
    monitor.stop();
    require(fake.failure().empty(), fake.failure());
}
// Count fallback stops only when confirmed and never retry an uncertain stop.
void stop_fallback(const std::filesystem::path& dir, bool automatic, FakeCast::Stop behavior) {
    FakeCast fake;
    fake.supported = false;
    fake.stop_behavior = behavior;
    humbug::Options options;
    options.sonos_enabled = false;
    options.discover_cast = false;
    options.cast_devices = {fake.endpoint};
    const auto suffix = std::to_string(static_cast<int>(behavior)) + (automatic ? "-auto" : "-manual");
    options.blocklist = (dir / ("stop-rules-" + suffix)).string();
    options.history_path = (dir / ("stop-history-" + suffix)).string();
    if (automatic) {
        std::ofstream out(options.blocklist);
        out << "exact:Artist\tSong\n";
    }
    humbug::Monitor monitor(options);
    monitor.start();
    if (!automatic) {
        const auto initial = wait(monitor, [](const auto& s) {
            return !s.groups.empty() && s.groups[0].connected && s.groups[0].can_stop;
        });
        const auto group = initial.groups[0];
        monitor.block({group.id, group.track.identity(), group.occurrence});
    }
    const bool success = behavior == FakeCast::Stop::Confirm;
    const auto state = wait(monitor, [success](const auto& s) {
        return success ? s.blocks.total == 1
                       : s.notice.find("block action failed") != std::string::npos ||
                             s.notice.find("BLOCK ACTION FAILED") != std::string::npos;
    });
    require(fake.next_count == 0 && fake.stop_count == 1 && state.blocks.total == (success ? 1u : 0u),
            "Cast fallback records only confirmed Stop");
    std::this_thread::sleep_for(2200ms);
    require(fake.stop_count == 1, "Cast Stop is not retried for the same occurrence");
    if (automatic && behavior == FakeCast::Stop::LostResponse) {
        fake.resume();
        wait(monitor, [](const auto& s) {
            return !s.groups.empty() && s.groups[0].state == "PLAYING";
        });
        std::this_thread::sleep_for(2200ms);
        require(fake.stop_count == 1, "idle Cast status does not clear an uncertain Stop attempt");
    }
    monitor.stop();
    require(fake.failure().empty(), fake.failure());
}
int simulate() {
    FakeCast fake;
    std::cout << "Fake Cast receiver. Set HUMBUG_CAST_DEVICE=" << fake.endpoint << '\n'
              << "Commands: track <artist>|<title>, unsupported, supported, quit\n"
              << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit")
            break;
        if (!fake.failure().empty())
            throw std::runtime_error(fake.failure());
        if (line == "unsupported")
            fake.supported = false;
        else if (line == "supported")
            fake.supported = true;
        else if (line.starts_with("track ")) {
            const auto value = line.substr(6);
            const auto separator = value.find('|');
            if (separator == std::string::npos || separator == 0 || separator + 1 == value.size()) {
                std::cerr << "Use: track <artist>|<title>\n";
                continue;
            }
            fake.song(value.substr(separator + 1), value.substr(0, separator));
        } else {
            std::cerr << "Commands: track <artist>|<title>, unsupported, supported, quit\n";
            continue;
        }
        std::cout << "Fake Cast updated\n" << std::flush;
    }
    if (!fake.failure().empty())
        throw std::runtime_error(fake.failure());
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--simulate")
            return simulate();
        if (argc != 1)
            throw std::runtime_error("Usage: humbug_cast_tests [--simulate]");
        char path[] = "/tmp/humbug-cast-tests-XXXXXX";
        const auto* name = mkdtemp(path);
        if (!name)
            throw std::runtime_error("Cannot create Cast test directory");
        const std::filesystem::path dir(name);
        try {
            discovery();
            protocol();
            capability_changes();
            integration(dir);
            repeat_blocked_song_stops_silently(dir);
            revisit_blocked_tracks(dir);
            auto_stop_limit(dir);
            stop_fallback(dir, false, FakeCast::Stop::Confirm);
            stop_fallback(dir, true, FakeCast::Stop::Confirm);
            stop_fallback(dir, false, FakeCast::Stop::Reject);
            stop_fallback(dir, true, FakeCast::Stop::Unchanged);
            stop_fallback(dir, true, FakeCast::Stop::LostResponse);
            rejected_block(dir);
            missing_metadata(dir);
        } catch (...) {
            std::filesystem::remove_all(dir);
            throw;
        }
        std::filesystem::remove_all(dir);
        std::cout << "Cast protocol and integration tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << "Cast test failure: " << e.what() << '\n';
        return 1;
    }
}
