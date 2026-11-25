#include "monitor.h"

#include "blocklist.h"
#include "network.h"
#include "protocol.h"
#include "skip_history.h"
#include "cast/monitor.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <utility>
#include <variant>

namespace humbug {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kTransportCheckInterval = std::chrono::seconds(3);
constexpr unsigned kAutoStopSkipLimit = 10;
struct Subscription {
    Service service;
    std::string group, path, sid, source;
    bool topology = false, initial = false, warned = false;
    std::optional<std::uint32_t> sequence;
    Clock::time_point renew{}, expires{}, created = Clock::now();
};
struct Target {
    std::string source, sid;
};

std::uint32_t sequence(const std::string& value) {
    std::uint32_t n{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (error != std::errc{} || end != value.data() + value.size() || value.empty())
        throw std::runtime_error("Invalid UPnP sequence number");
    return n;
}
std::chrono::seconds lifetime(const HttpMessage& r) {
    const auto it = r.headers.find("timeout");
    if (it == r.headers.end() || !it->second.starts_with("Second-"))
        throw std::runtime_error("Missing or invalid subscription TIMEOUT");
    if (it->second == "Second-infinite")
        return std::chrono::hours(24);
    const auto seconds = sequence(it->second.substr(7));
    if (!seconds)
        throw std::runtime_error("Zero subscription lifetime");
    return std::chrono::seconds(seconds);
}
void success(const HttpMessage& r, const std::string& operation) {
    if (r.status() < 200 || r.status() >= 300)
        throw std::runtime_error(operation + " returned HTTP " + std::to_string(r.status()));
}
bool has_action(const std::string& actions, const std::string& action) {
    std::size_t start = 0;
    while (start <= actions.size()) {
        const auto end = actions.find(',', start);
        auto item = actions.substr(start, end == std::string::npos ? end : end - start);
        item.erase(item.begin(),
                   std::find_if(item.begin(), item.end(), [](unsigned char c) { return !std::isspace(c); }));
        item.erase(
            std::find_if(item.rbegin(), item.rend(), [](unsigned char c) { return !std::isspace(c); }).base(),
            item.end());
        if (item == action)
            return true;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}
} // namespace

Options environment_options() {
    Options o;
    if (const auto* path = std::getenv("HUMBUG_BLOCKLIST")) {
        o.blocklist = path;
        o.explicit_blocklist = true;
        if (o.blocklist.empty())
            throw std::runtime_error("HUMBUG_BLOCKLIST cannot be empty");
    }
    if (const auto* enabled = std::getenv("HUMBUG_CHRISTMAS_BLOCKING")) {
        const std::string_view value(enabled);
        if (value != "0" && value != "1")
            throw std::runtime_error("HUMBUG_CHRISTMAS_BLOCKING must be 0 or 1");
        o.christmas_blocking = value == "1";
    }
    if (const auto* ip = std::getenv("HUMBUG_LISTEN_ADDRESS"))
        o.listen_address = ip;
    if (const auto* device = std::getenv("HUMBUG_DEVICE")) {
        parse_url(device);
        o.devices.emplace_back(device);
    }
    if (const auto* device = std::getenv("HUMBUG_CAST_DEVICE")) {
        o.discover_cast = false;
        o.cast_devices.emplace_back(device);
    }
    if (const auto* interface = std::getenv("HUMBUG_CAST_INTERFACE"))
        o.cast_interface = interface;
    if (const auto* disabled = std::getenv("HUMBUG_CAST_DISABLED")) {
        const std::string value(disabled);
        if (value != "0" && value != "1")
            throw std::runtime_error("HUMBUG_CAST_DISABLED must be 0 or 1");
        if (value == "1") {
            if (!o.cast_devices.empty())
                throw std::runtime_error("Cannot configure a Cast device when Cast is disabled");
            o.discover_cast = false;
        }
    }
    if (const auto* disabled = std::getenv("HUMBUG_SONOS_DISABLED")) {
        const std::string value(disabled);
        if (value != "0" && value != "1")
            throw std::runtime_error("HUMBUG_SONOS_DISABLED must be 0 or 1");
        o.sonos_enabled = value == "0";
    }
    if (const auto* port = std::getenv("HUMBUG_LISTEN_PORT")) {
        const auto n = sequence(port);
        if (n > 65535)
            throw std::runtime_error("HUMBUG_LISTEN_PORT must be 0..65535");
        o.listen_port = static_cast<std::uint16_t>(n);
    }
    return o;
}

struct Monitor::Impl {
    explicit Impl(Options o) : options(std::move(o)) {}
    Options options;
    std::atomic_bool stopping{false};
    std::thread worker;
    bool started = false;
    std::mutex mutex;
    Snapshot published, state;
    bool dirty = true, overflow = false;
    std::size_t pending_blocks = 0;
    struct ResetCustomBlocklist {};
    using Command = std::variant<BlockCommand, ResetCustomBlocklist>;
    std::deque<Command> commands;
    std::optional<std::pair<bool, bool>> pending_blocklists;
    std::optional<bool> pending_auto_stop;
    std::deque<Notification> events;
    std::size_t queued_bytes = 0;
    std::map<std::string, Target> targets;
    std::map<std::string, Device> devices;
    std::map<std::string, Subscription> subscriptions;
    std::map<std::string, std::pair<std::string, std::uint64_t>> attempted;
    std::map<std::string, cast::Media> cast_attempted;
    std::map<std::string, unsigned> consecutive_skips;
    std::map<std::string, Track> last_skipped;
    std::set<std::string> auto_stopped;
    std::map<std::string, std::pair<std::string, std::uint64_t>> unsupported;
    std::unique_ptr<EventListener> listener;
    std::unique_ptr<Blocklist> rules;
    std::unique_ptr<SkipHistory> history;
    std::unique_ptr<cast::Monitor> cast_monitor;
    std::map<std::string, cast::Observation> cast_observations;
    std::vector<Group> cast_groups;
    std::string cast_discovery_error;
    std::string history_day;
    std::vector<BlockedSong> blocked_songs;
    std::uint64_t block_sequence = 0;
    Clock::time_point next_day_check{};
    std::uint64_t token = std::random_device{}();
    std::string topology_location;
    Clock::time_point next_discovery{};
    Clock::time_point next_transport_check = Clock::now() + kTransportCheckInterval;
    std::chrono::seconds retry{1};

    void publish() {
        std::lock_guard lock(mutex);
        published = state;
        published.groups.insert(published.groups.end(), cast_groups.begin(), cast_groups.end());
        published.block_processing = pending_blocks != 0;
        dirty = true;
    }
    void notice(const std::string& text, bool background = true, bool is_error = true) {
        if (!background || state.notice.empty()) {
            state.notice = text;
            state.notice_is_error = is_error;
        }
        if (options.log_notices)
            std::cerr << "Humbug: " << text << '\n';
        publish();
    }
    Device load(const std::string& url) {
        auto r = request(url, "GET", {}, "", stopping);
        success(r, "Device description");
        return parse_device(r.body, url);
    }
    Group* group(const std::string& id) {
        const auto it =
            std::find_if(state.groups.begin(), state.groups.end(), [&](const auto& g) { return g.id == id; });
        return it == state.groups.end() ? nullptr : &*it;
    }
    Group* cast_group(const std::string& id) {
        const auto it =
            std::find_if(cast_groups.begin(), cast_groups.end(), [&](const auto& g) { return g.id == id; });
        return it == cast_groups.end() ? nullptr : &*it;
    }
    static bool same_cast_media(const cast::Media& a, const cast::Media& b) {
        return a.application == b.application && a.transport == b.transport && a.session == b.session &&
               a.item == b.item && a.track.identity() == b.track.identity();
    }
    static bool same_song(const Track& a, const Track& b) {
        return !a.title.empty() && !a.artist.empty() && a.title == b.title && a.artist == b.artist;
    }
    bool already_attempted(const Group& g, const std::string& identity, std::uint64_t occurrence) const {
        if (g.provider == Provider::Cast) {
            const auto prior = cast_attempted.find(g.id);
            const auto current = cast_observations.find(g.id);
            return prior != cast_attempted.end() && current != cast_observations.end() &&
                   same_cast_media(prior->second, current->second.media);
        }
        return attempted.contains(g.id) && attempted.at(g.id) == std::make_pair(identity, occurrence);
    }
    void update_cast() {
        if (!cast_monitor)
            return;
        std::vector<cast::Observation> observations;
        std::string error;
        if (!cast_monitor->take(observations, error))
            return;
        if (!error.empty() && error != cast_discovery_error)
            notice(error);
        cast_discovery_error = std::move(error);
        std::vector<Group> groups;
        std::map<std::string, cast::Observation> updated;
        for (auto& observation : observations) {
            const auto id = "cast:" + observation.endpoint.id;
            Group g;
            g.id = id;
            g.provider = Provider::Cast;
            g.rooms = {observation.endpoint.name};
            g.connected = observation.connected;
            g.error = observation.error;
            const auto previous = cast_observations.find(id);
            if (previous != cast_observations.end() && !observation.connected)
                observation.media = previous->second.media;
            g.track = observation.media.track;
            g.state = observation.media.state == "PAUSED" ? "PAUSED_PLAYBACK" : observation.media.state;
            g.can_skip = observation.media.can_skip && observation.media.session > 0;
            // Cast does not advertise a STOP capability; the receiver may reject it.
            g.can_stop = observation.media.session > 0;
            const auto prior_attempt = cast_attempted.find(id);
            if (observation.connected && observation.media.session > 0 &&
                observation.media.track.blockable() && prior_attempt != cast_attempted.end() &&
                !same_cast_media(prior_attempt->second, observation.media)) {
                cast_attempted.erase(prior_attempt);
                attempted.erase(id);
            }
            if (auto* old = cast_group(id)) {
                g.occurrence = old->occurrence;
                if (previous != cast_observations.end() && observation.connected &&
                    !same_cast_media(previous->second.media, observation.media))
                    ++g.occurrence;
                if (old->connected && !g.connected)
                    notice("Cast disconnected: " + observation.endpoint.name);
            } else {
                g.occurrence = 1;
            }
            updated.emplace(id, std::move(observation));
            groups.push_back(std::move(g));
        }
        for (const auto& [id, observation] : cast_observations) {
            if (!updated.contains(id)) {
                attempted.erase(id);
                cast_attempted.erase(id);
                consecutive_skips.erase(id);
                last_skipped.erase(id);
                auto_stopped.erase(id);
                unsupported.erase(id);
            }
        }
        cast_observations = std::move(updated);
        cast_groups = std::move(groups);
        publish();
    }
    int notification(Notification n) {
        const auto& h = n.message.headers;
        const auto sid = h.find("sid"), seq = h.find("seq");
        if (sid == h.end() || sid->second.empty() || seq == h.end() || !h.contains("nt") ||
            h.at("nt") != "upnp:event" || !h.contains("nts") || h.at("nts") != "upnp:propchange")
            return 412;
        try {
            sequence(seq->second);
        } catch (const std::exception&) {
            return 400;
        }
        std::lock_guard lock(mutex);
        const auto target = targets.find(n.path);
        if (target == targets.end() || target->second.source != n.source ||
            (!target->second.sid.empty() && target->second.sid != sid->second))
            return 412;
        std::size_t bytes = n.message.body.size() + n.message.first.size() + n.path.size() + n.source.size();
        for (const auto& [key, value] : h)
            bytes += key.size() + value.size();
        if (events.size() >= 256 || bytes > 4 * 1024 * 1024 - queued_bytes) {
            overflow = true;
            return 503;
        }
        queued_bytes += bytes;
        events.push_back(std::move(n));
        return 200;
    }
    void subscribe(const std::string& key, const Service& service, const std::string& id, bool topology) {
        if (service.events.empty())
            throw std::runtime_error("Device has no event subscription URL");
        Subscription s;
        s.service = service;
        s.group = id;
        s.topology = topology;
        s.path = "/sonos/" + std::to_string(++token);
        s.source = parse_url(service.events).host;
        {
            std::lock_guard lock(mutex);
            targets.emplace(s.path, Target{s.source, ""});
        }
        try {
            const auto ip =
                options.listen_address.empty() ? local_address(service.events) : options.listen_address;
            const auto callback = "<http://" + ip + ":" + std::to_string(listener->port()) + s.path + ">";
            const auto r =
                request(service.events, "SUBSCRIBE",
                        {{"CALLBACK", callback},
                         {"NT", "upnp:event"},
                         {"TIMEOUT", "Second-" + std::to_string(options.subscription_timeout.count())}},
                        "", stopping);
            success(r, "SUBSCRIBE");
            const auto sid = r.headers.find("sid");
            if (sid == r.headers.end() || sid->second.empty())
                throw std::runtime_error("SUBSCRIBE did not return SID");
            s.sid = sid->second;
            const auto duration = lifetime(r);
            s.expires = Clock::now() + duration;
            s.renew = Clock::now() + std::chrono::milliseconds(duration) * 2 / 3;
            {
                std::lock_guard lock(mutex);
                targets.at(s.path).sid = s.sid;
            }
            subscriptions.emplace(key, std::move(s));
        } catch (...) {
            std::lock_guard lock(mutex);
            targets.erase(s.path);
            throw;
        }
    }
    void unsubscribe(const std::string& key) {
        auto it = subscriptions.find(key);
        if (it == subscriptions.end())
            return;
        auto s = it->second;
        {
            std::lock_guard lock(mutex);
            targets.erase(s.path);
        }
        subscriptions.erase(it);
        if (!stopping) {
            try {
                success(request(s.service.events, "UNSUBSCRIBE", {{"SID", s.sid}}, "", stopping),
                        "UNSUBSCRIBE");
            } catch (const std::exception& e) {
                notice(std::string("Unsubscribe failed: ") + e.what(), true);
            }
        }
    }
    void reconcile(std::vector<Group> groups) {
        std::set<std::string> keep{"topology"};
        for (auto& g : groups) {
            keep.insert(g.id);
            if (auto* old = group(g.id);
                old && old->coordinator == g.coordinator && old->location == g.location) {
                g.track = old->track;
                g.occurrence = old->occurrence;
                g.state = old->state;
                g.connected = old->connected;
                g.error = old->error;
            } else {
                unsubscribe(g.id);
                devices.erase(g.id);
                attempted.erase(g.id);
                consecutive_skips.erase(g.id);
                last_skipped.erase(g.id);
                auto_stopped.erase(g.id);
            }
        }
        std::vector<std::string> remove;
        for (const auto& [key, s] : subscriptions)
            if (!keep.contains(key))
                remove.push_back(key);
        for (const auto& key : remove) {
            unsubscribe(key);
            devices.erase(key);
            attempted.erase(key);
            consecutive_skips.erase(key);
            last_skipped.erase(key);
            auto_stopped.erase(key);
        }
        state.groups = std::move(groups);
        for (auto& g : state.groups) {
            if (stopping)
                return;
            try {
                if (!devices.contains(g.id))
                    devices.emplace(g.id, load(g.location));
                const auto& d = devices.at(g.id);
                if (d.uuid != g.coordinator)
                    throw std::runtime_error("Coordinator UUID mismatch");
                if (!subscriptions.contains(g.id)) {
                    refresh(g, d, stopping);
                    subscribe(g.id, d.transport, g.id, false);
                    g.connected = false;
                    g.error = "Waiting for initial UPnP notification";
                }
            } catch (const std::exception& e) {
                g.connected = false;
                g.error = e.what();
                notice("Group " + g.id + ": " + e.what());
            }
        }
        publish();
    }
    void scan() {
        auto urls = options.devices.empty() ? discover(stopping) : options.devices;
        if (!topology_location.empty() &&
            std::find(urls.begin(), urls.end(), topology_location) == urls.end())
            urls.push_back(topology_location);
        if (urls.empty())
            throw std::runtime_error("No Sonos speakers found; retrying discovery");
        std::string errors;
        for (const auto& url : urls) {
            if (stopping)
                return;
            try {
                auto d = load(url);
                if (d.topology.control.empty())
                    continue;
                auto groups = parse_topology(soap(d.topology, "GetZoneGroupState", stopping));
                if (const auto it = subscriptions.find("topology");
                    it != subscriptions.end() && it->second.service.events != d.topology.events)
                    unsubscribe("topology");
                const bool new_topology_subscription = !subscriptions.contains("topology");
                if (new_topology_subscription)
                    subscribe("topology", d.topology, "", true);
                if (new_topology_subscription) {
                    for (auto& old : state.groups) {
                        old.connected = false;
                        old.error = "Waiting for topology event resynchronisation";
                    }
                    publish();
                }
                reconcile(std::move(groups));
                topology_location = url;
                retry = std::chrono::seconds(1);
                next_discovery = Clock::now() + options.discovery_interval;
                return;
            } catch (const std::exception& e) {
                errors += url + ": " + e.what() + "; ";
            }
        }
        throw std::runtime_error("Discovery/topology failed: " + errors);
    }
    void maintain() {
        std::vector<std::string> failed;
        for (auto& [key, s] : subscriptions) {
            if (stopping)
                return;
            if (!s.initial && !s.warned && Clock::now() - s.created > std::chrono::seconds(5)) {
                s.warned = true;
                notice("No initial UPnP callback from " + s.source +
                       "; check firewall and callback address/port " + std::to_string(listener->port()));
                if (auto* g = group(s.group)) {
                    g->connected = false;
                    g->error = "No UPnP callback; check firewall";
                }
                failed.push_back(key);
                continue;
            }
            if (Clock::now() < s.renew)
                continue;
            try {
                const auto r =
                    request(s.service.events, "SUBSCRIBE",
                            {{"SID", s.sid},
                             {"TIMEOUT", "Second-" + std::to_string(options.subscription_timeout.count())}},
                            "", stopping);
                success(r, "Subscription renewal");
                if (const auto sid = r.headers.find("sid"); sid != r.headers.end() && sid->second != s.sid)
                    throw std::runtime_error("Renewal changed subscription SID");
                const auto duration = lifetime(r);
                s.expires = Clock::now() + duration;
                s.renew = Clock::now() + std::chrono::milliseconds(duration) * 2 / 3;
            } catch (const std::exception& e) {
                notice("Subscription lost: " + std::string(e.what()));
                if (auto* g = group(s.group)) {
                    g->connected = false;
                    g->error = e.what();
                }
                if (s.topology) {
                    for (auto& g : state.groups) {
                        g.connected = false;
                        g.error = "Topology subscription lost";
                    }
                }
                failed.push_back(key);
            }
        }
        for (const auto& key : failed)
            unsubscribe(key);
        if (!failed.empty()) {
            next_discovery = Clock::now() + retry;
            retry = std::min(retry * 2, std::chrono::seconds(30));
            publish();
        }
    }
    void process(Notification n) {
        auto it = std::find_if(subscriptions.begin(), subscriptions.end(),
                               [&](const auto& p) { return p.second.path == n.path; });
        if (it == subscriptions.end())
            return;
        auto& s = it->second;
        if (n.message.headers.at("sid") != s.sid)
            return;
        const auto seq = sequence(n.message.headers.at("seq"));
        bool gap = false;
        if (s.sequence) {
            // Sonos/UPnP wraps from UINT32_MAX to 1; zero is the initial event.
            const auto delta = seq - *s.sequence;
            if (!delta || delta > 0x7fffffffU || seq == 0)
                return;
            const auto expected = *s.sequence == UINT32_MAX ? 1U : *s.sequence + 1U;
            gap = seq != expected;
        }
        const char* stage = s.topology ? "topology event" : "AVTransport event";
        try {
            if (s.topology) {
                const bool bootstrap = !s.initial;
                const auto topology = topology_event(n.message.body);
                if (!topology.empty())
                    reconcile(parse_topology(topology));
                if (bootstrap) {
                    for (auto& g : state.groups) {
                        const auto device = devices.find(g.id);
                        if (device == devices.end() || !subscriptions.contains(g.id))
                            continue;
                        stage = "topology bootstrap transport refresh";
                        refresh(g, device->second, stopping);
                        g.connected = true;
                        g.error.clear();
                    }
                }
                if (gap)
                    next_discovery = Clock::now();
            } else if (auto* g = group(s.group)) {
                const auto update = parse_event(n.message.body);
                const bool changed_item = (update.uri && *update.uri != g->track.uri) ||
                                          (update.number && *update.number != g->track.number);
                if (gap || !s.initial || (changed_item && !update.track)) {
                    stage = "AVTransport event transport refresh";
                    refresh(*g, devices.at(g->id), stopping);
                } else {
                    merge(*g, update);
                }
                g->connected = true;
                g->error.clear();
            }
            s.sequence = seq;
            s.initial = true;
            publish();
        } catch (const std::exception& e) {
            const auto key = it->first;
            std::cerr << "Humbug UPnP diagnostic: stage=" << stage
                      << " service=" << (s.topology ? "ZoneGroupTopology" : "AVTransport")
                      << " sequence=" << seq << " initial=" << (s.initial ? "no" : "yes")
                      << " gap=" << (gap ? "yes" : "no") << " event_bytes=" << n.message.body.size() << '\n';
            notice("Invalid/lost UPnP state: " + std::string(e.what()), true);
            if (auto* g = group(s.group)) {
                g->connected = false;
                g->error = e.what();
            }
            unsubscribe(key);
            next_discovery = Clock::now();
        }
    }
    void check_sonos_transport() {
        const auto topology = subscriptions.find("topology");
        if (topology == subscriptions.end() || !topology->second.initial)
            return;
        bool changed = false;
        // A dropped final NOTIFY cannot be detected by sequence numbers alone.
        for (auto& g : state.groups) {
            if (stopping)
                return;
            const auto subscription = subscriptions.find(g.id);
            if (subscription == subscriptions.end() || !subscription->second.initial)
                continue;
            const auto occurrence = g.occurrence;
            const auto playback = g.state;
            const auto elapsed = g.track.elapsed_seconds;
            const auto duration = g.track.duration_seconds;
            try {
                refresh(g, devices.at(g.id), stopping);
                changed |= !g.connected || !g.error.empty() || occurrence != g.occurrence ||
                           playback != g.state || elapsed != g.track.elapsed_seconds ||
                           duration != g.track.duration_seconds;
                g.connected = true;
                g.error.clear();
            } catch (const std::exception& e) {
                if (stopping)
                    return;
                if (g.connected || g.error != e.what()) {
                    std::cerr << "Humbug Sonos " << g.id << " transport check: " << e.what() << '\n';
                    g.connected = false;
                    g.error = e.what();
                    changed = true;
                }
            }
        }
        if (changed)
            publish();
    }
    bool revalidate(Group& g, const std::string& identity) {
        const auto& d = devices.at(g.id);
        // Verify current coordinator before sending any mutation.
        const auto topology = soap(subscriptions.at("topology").service, "GetZoneGroupState", stopping);
        const auto current = parse_topology(topology);
        const auto it =
            std::find_if(current.begin(), current.end(), [&](const auto& v) { return v.id == g.id; });
        if (it == current.end() || it->coordinator != g.coordinator || it->location != g.location) {
            next_discovery = Clock::now();
            return false;
        }
        refresh(g, d, stopping);
        return g.state == "PLAYING" && g.track.identity() == identity;
    }
    void record_block(const Group& g, bool auto_stop = false) {
        std::string room;
        for (const auto& name : g.rooms)
            room += (room.empty() ? "" : " + ") + name;
        {
            std::lock_guard lock(mutex);
            blocked_songs.push_back({++block_sequence, g.track, std::move(room), auto_stop});
        }
        try {
            history->record(g.track, local_day());
            update_history();
        } catch (const std::runtime_error& e) {
            state.blocks.error = e.what();
            std::cerr << "Humbug block history: " << state.blocks.error << '\n';
        }
    }
    bool block_sonos(Group& g, const std::string& identity, std::uint64_t occurrence,
                     bool force_stop = false, bool record_result = true) {
        const auto token = std::make_pair(identity, occurrence);
        if (attempted.contains(g.id) && attempted.at(g.id) == token)
            return false;
        if (!revalidate(g, identity) || g.occurrence != occurrence)
            throw std::runtime_error("Track/group changed; block cancelled");
        const auto& transport = devices.at(g.id).transport;
        const auto actions = xml_value(soap(transport, "GetCurrentTransportActions", stopping), "Actions");
        if (!revalidate(g, identity) || g.occurrence != occurrence)
            throw std::runtime_error("Track/group changed; block cancelled");
        // Mark before sending a mutation: a lost response must not trigger a retry.
        attempted[g.id] = token;
        bool use_stop = force_stop || !has_action(actions, "Next");
        if (!use_stop) {
            try {
                soap(transport, "Next", stopping);
            } catch (const SoapError& e) {
                // Only an explicit unsupported/invalid transition fault is safe to follow with Stop.
                if (e.code != "401" && e.code != "701")
                    throw;
                use_stop = true;
            }
        }
        if (use_stop) {
            const auto current_actions =
                xml_value(soap(transport, "GetCurrentTransportActions", stopping), "Actions");
            if (!has_action(current_actions, "Stop"))
                throw std::runtime_error("Receiver does not allow Stop");
            if (!revalidate(g, identity) || g.occurrence != occurrence)
                throw std::runtime_error("Track/group changed; stop cancelled");
            soap(transport, "Stop", stopping);
            const auto state =
                xml_value(soap(transport, "GetTransportInfo", stopping), "CurrentTransportState");
            if (state != "STOPPED")
                throw std::runtime_error("Sonos stop was not confirmed; it will not be retried");
            g.state = state;
        }
        if (!use_stop)
            last_skipped[g.id] = g.track;
        if (record_result)
            record_block(g, force_stop && use_stop);
        return use_stop;
    }
    bool revalidate_cast(const Group& g) {
        const auto& observation = cast_observations.at(g.id);
        const auto current = cast::read(observation.endpoint, stopping, &observation.media);
        return current.state == "PLAYING" && current.session > 0 &&
               same_cast_media(current, observation.media);
    }
    bool block_cast(Group& g, const std::string& identity, std::uint64_t occurrence,
                    bool force_stop = false, bool record_result = true) {
        const auto token = std::make_pair(identity, occurrence);
        if (already_attempted(g, identity, occurrence))
            return false;
        const auto& observation = cast_observations.at(g.id);
        attempted[g.id] = token;
        cast_attempted[g.id] = observation.media;
        const bool stopped = cast::block(observation.endpoint, observation.media, stopping, force_stop);
        if (stopped)
            g.state = "STOPPED";
        else
            last_skipped[g.id] = g.track;
        if (record_result)
            record_block(g, force_stop && stopped);
        return stopped;
    }
    void update_history() {
        history_day = local_day();
        auto stats = history->stats(history_day);
        if (!stats.error.empty() && stats.error != state.blocks.error)
            std::cerr << "Humbug block history: " << stats.error << '\n';
        state.blocks = std::move(stats);
        publish();
    }
    void block(const BlockCommand& c) {
        auto* g = group(c.group);
        if (!g)
            g = cast_group(c.group);
        if (!g || !g->connected || g->occurrence != c.occurrence || g->track.identity() != c.identity ||
            !g->track.blockable() || (!g->can_skip && !g->can_stop)) {
            notice("Block cancelled: displayed song/group is no longer available", false);
            return;
        }
        try {
            if (!(g->provider == Provider::Cast ? revalidate_cast(*g) : revalidate(*g, c.identity)) ||
                g->occurrence != c.occurrence) {
                notice("Block cancelled: song/group changed", false);
                return;
            }
        } catch (const std::exception& e) {
            notice("Block cancelled: " + std::string(e.what()), false);
            return;
        }
        const auto track = g->track;
        std::string action;
        bool action_failed = false;
        try {
            const bool already = already_attempted(*g, c.identity, c.occurrence);
            const bool stopped = g->provider == Provider::Cast ? block_cast(*g, c.identity, c.occurrence)
                                                               : block_sonos(*g, c.identity, c.occurrence);
            action = already   ? "; block already attempted for this song"
                     : stopped ? "; playback stopped"
                               : "; skipped";
        } catch (const std::exception& e) {
            action_failed = true;
            action = "; BLOCK ACTION FAILED: " + std::string(e.what());
        }
        std::string saved = "Block saved";
        bool save_failed = false;
        try {
            rules->add(track);
        } catch (const std::exception& e) {
            save_failed = true;
            saved = "BLOCK NOT SAVED: " + std::string(e.what());
        }
        notice(saved + action, false, save_failed || action_failed);
    }
    void automatic() {
        const auto apply = [&](std::vector<Group>& groups) {
            for (auto& g : groups) {
                if (stopping)
                    return;
                if (!g.connected || g.state != "PLAYING")
                    continue;
                if (auto_stopped.contains(g.id)) {
                    try {
                        if (!(g.provider == Provider::Cast ? revalidate_cast(g)
                                                          : revalidate(g, g.track.identity())))
                            continue;
                        auto_stopped.erase(g.id);
                        attempted.erase(g.id);
                        cast_attempted.erase(g.id);
                    } catch (const std::exception& e) {
                        g.error = "Could not verify playback after auto-stop: " + std::string(e.what());
                        notice(g.error + " in " + g.id, false);
                        continue;
                    }
                }
                if (!rules->matches(g.track)) {
                    last_skipped.erase(g.id);
                    if (g.track.blockable())
                        consecutive_skips.erase(g.id);
                    continue;
                }
                const auto token = std::make_pair(g.track.identity(), g.occurrence);
                if (already_attempted(g, token.first, token.second))
                    continue;
                if (g.provider == Provider::Cast && !g.track.blockable()) {
                    if (!unsupported.contains(g.id) || unsupported.at(g.id) != token) {
                        unsupported[g.id] = token;
                        g.error = "Automatic skip unavailable: Cast artist or title missing";
                        notice(g.error + " in " + g.id, false);
                    }
                    continue;
                }
                if (!g.can_skip && !g.can_stop) {
                    if (!unsupported.contains(g.id) || unsupported.at(g.id) != token) {
                        unsupported[g.id] = token;
                        g.error = "Automatic block unavailable: receiver cannot skip or stop";
                        notice(g.error + " in " + g.id, g.provider != Provider::Cast);
                    }
                    continue;
                }
                unsupported.erase(g.id);
                const auto previous_skip = last_skipped.find(g.id);
                const bool repeated_song =
                    previous_skip != last_skipped.end() && same_song(previous_skip->second, g.track);
                const bool limit_reached =
                    !repeated_song && options.auto_stop &&
                    consecutive_skips[g.id] >= kAutoStopSkipLimit;
                try {
                    const bool stopped = g.provider == Provider::Cast
                                             ? block_cast(g, token.first, token.second,
                                                          repeated_song || limit_reached, !repeated_song)
                                             : block_sonos(g, token.first, token.second,
                                                           repeated_song || limit_reached, !repeated_song);
                    if (stopped) {
                        consecutive_skips.erase(g.id);
                        if (limit_reached)
                            auto_stopped.insert(g.id);
                    } else if (options.auto_stop && consecutive_skips[g.id] < kAutoStopSkipLimit) {
                        ++consecutive_skips[g.id];
                    }
                    if (repeated_song) {
                        publish();
                        continue;
                    }
                    if (limit_reached)
                        notice("Auto-stop: stopped playback after 10 consecutive blocked songs in " + g.id,
                               false, false);
                    else
                        notice(std::string("Automatically ") + (stopped ? "stopped" : "skipped") +
                                   " blocked song in " + g.id,
                               true, false);
                } catch (const std::exception& e) {
                    if (g.provider == Provider::Sonos)
                        attempted[g.id] = token;
                    g.error = "Automatic block action failed: " + std::string(e.what());
                    notice("Automatic block action failed in " + g.id + ": " + e.what(),
                           !limit_reached && g.provider != Provider::Cast);
                }
            }
        };
        if (subscriptions.contains("topology") && subscriptions.at("topology").initial)
            apply(state.groups);
        apply(cast_groups);
    }
    void health() {
        const bool healthy =
            subscriptions.contains("topology") && subscriptions.at("topology").initial &&
            std::all_of(state.groups.begin(), state.groups.end(), [](const auto& g) { return g.connected; });
        const std::string status =
            state.groups.empty()
                ? (options.sonos_enabled ? "No Sonos playback groups found" : "No streams found")
            : healthy ? "Live UPnP events"
                      : "Disconnected / waiting for UPnP events";
        const bool cast_healthy =
            !cast_groups.empty() &&
            std::all_of(cast_groups.begin(), cast_groups.end(), [](const auto& g) { return g.connected; });
        const bool active = (options.christmas_blocking || options.custom_blocking) &&
                            (state.groups.empty() || healthy) && (cast_groups.empty() || cast_healthy) &&
                            (!state.groups.empty() || !cast_groups.empty());
        const auto combined_status = state.groups.empty() && cast_healthy ? "Live Cast sessions" : status;
        if (state.status != combined_status || state.blocker_active != active) {
            state.status = combined_status;
            state.blocker_active = active;
            publish();
        }
    }
    void run() {
        try {
            if (options.listen_address == "0.0.0.0")
                throw std::runtime_error("Choose a reachable LAN callback address or leave it automatic");
            if (!options.listen_address.empty())
                parse_url("http://" + options.listen_address + "/");
            for (const auto& device : options.devices)
                parse_url(device);
            if (!options.sonos_enabled && !options.devices.empty())
                throw std::runtime_error("Cannot configure Sonos devices when Sonos is disabled");
            if (options.explicit_blocklist && options.blocklist.empty())
                throw std::runtime_error("Explicit blocklist path cannot be empty");
            if (options.discovery_interval.count() <= 0 || options.subscription_timeout.count() <= 0)
                throw std::runtime_error("Discovery interval and subscription lifetime must be positive");
            if (options.blocklist.empty())
                options.blocklist = default_blocklist_path();
            history = std::make_unique<SkipHistory>(options.history_path);
            update_history();
            rules = std::make_unique<Blocklist>(options.blocklist, options.explicit_blocklist,
                                                options.christmas_blocking);
            rules->set_enabled(options.christmas_blocking, options.custom_blocking);
            if (options.discover_cast || !options.cast_devices.empty())
                cast_monitor = std::make_unique<cast::Monitor>(options.discover_cast, options.cast_devices,
                                                               options.cast_interface);
            if (options.sonos_enabled)
                listener = std::make_unique<EventListener>(
                    options.listen_address, options.listen_port, stopping,
                    [this](Notification n) { return notification(std::move(n)); });
            while (!stopping) {
                if (Clock::now() >= next_day_check) {
                    if (history_day != local_day())
                        update_history();
                    next_day_check = Clock::now() + std::chrono::seconds(1);
                }
                if (listener && listener->failed())
                    throw std::runtime_error("UPnP callback listener failed");
                if (options.sonos_enabled && Clock::now() >= next_discovery) {
                    try {
                        scan();
                    } catch (const std::exception& e) {
                        if (stopping)
                            break;
                        for (auto& g : state.groups) {
                            g.connected = false;
                            g.error = e.what();
                        }
                        if (std::string(e.what()) != "No Sonos speakers found; retrying discovery")
                            notice(e.what(), true);
                        std::vector<std::string> keys;
                        for (const auto& [key, s] : subscriptions)
                            keys.push_back(key);
                        for (const auto& key : keys)
                            unsubscribe(key);
                        next_discovery = Clock::now() + retry;
                        retry = std::min(retry * 2, std::chrono::seconds(30));
                    }
                }
                if (options.sonos_enabled)
                    maintain();
                update_cast();
                std::deque<Notification> received;
                std::deque<Command> pending;
                std::optional<std::pair<bool, bool>> blocklists;
                std::optional<bool> auto_stop;
                bool lost;
                {
                    // Drain shared queues before processing network events and commands.
                    std::lock_guard lock(mutex);
                    received.swap(events);
                    queued_bytes = 0;
                    pending.swap(commands);
                    blocklists.swap(pending_blocklists);
                    auto_stop.swap(pending_auto_stop);
                    lost = std::exchange(overflow, false);
                }
                if (blocklists) {
                    options.christmas_blocking = blocklists->first;
                    options.custom_blocking = blocklists->second;
                    rules->set_enabled(blocklists->first, blocklists->second);
                }
                if (auto_stop) {
                    options.auto_stop = *auto_stop;
                    if (!*auto_stop)
                        consecutive_skips.clear();
                }
                if (lost) {
                    // Discard potentially stale state and resubscribe before blocking again.
                    notice("Event queue overflow; resynchronising");
                    for (auto& g : state.groups)
                        g.connected = false;
                    for (auto& g : cast_groups)
                        g.connected = false;
                    std::vector<std::string> keys;
                    for (const auto& [key, s] : subscriptions)
                        keys.push_back(key);
                    for (const auto& key : keys)
                        unsubscribe(key);
                    next_discovery = Clock::now();
                } else {
                    for (auto& event : received) {
                        if (stopping)
                            break;
                        process(std::move(event));
                    }
                    if (options.sonos_enabled && Clock::now() >= next_transport_check) {
                        check_sonos_transport();
                        next_transport_check = Clock::now() + kTransportCheckInterval;
                    }
                }
                for (const auto& command : pending) {
                    if (stopping)
                        break;
                    if (const auto* request = std::get_if<BlockCommand>(&command)) {
                        if (!lost)
                            block(*request);
                        {
                            std::lock_guard lock(mutex);
                            --pending_blocks;
                        }
                        publish();
                    } else {
                        try {
                            rules->reset_saved_tracks();
                            notice("Manually blocked songs cleared", false, false);
                        } catch (const std::exception& e) {
                            notice("Could not reset custom blocklist: " + std::string(e.what()), false);
                        }
                    }
                }
                if (!lost)
                    automatic();
                health();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        } catch (const std::exception& e) {
            if (!stopping) {
                for (auto& g : state.groups)
                    g.connected = false;
                state.status = "Monitor failed";
                state.blocker_active = false;
                {
                    std::lock_guard lock(mutex);
                    pending_blocks = 0;
                }
                notice(e.what(), false);
            }
        }
        const auto cleanup_deadline = Clock::now() + std::chrono::milliseconds(750);
        std::atomic_bool cleanup_stop{false};
        for (const auto& [key, s] : subscriptions) {
            if (Clock::now() >= cleanup_deadline) {
                std::cerr << "Humbug shutdown: remaining subscriptions will expire after cleanup deadline\n";
                break;
            }
            try {
                success(request(s.service.events, "UNSUBSCRIBE", {{"SID", s.sid}}, "", cleanup_stop,
                                std::chrono::milliseconds(150)),
                        "UNSUBSCRIBE");
            } catch (const std::exception& e) {
                std::cerr << "Humbug shutdown: " << e.what() << '\n';
            }
        }
        listener.reset();
        cast_monitor.reset();
    }
};

Monitor::Monitor(Options o) : impl_(std::make_unique<Impl>(std::move(o))) {}
Monitor::~Monitor() {
    stop();
}
void Monitor::start() {
    if (impl_->started)
        throw std::runtime_error("Create a new Monitor to restart monitoring");
    impl_->started = true;
    impl_->stopping = false;
    impl_->worker = std::thread([this] { impl_->run(); });
}
void Monitor::stop() {
    impl_->stopping = true;
    if (impl_->worker.joinable())
        impl_->worker.join();
}
bool Monitor::take(Snapshot& snapshot) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->dirty)
        return false;
    snapshot = impl_->published;
    snapshot.blocked_songs.swap(impl_->blocked_songs);
    impl_->dirty = false;
    return true;
}
void Monitor::block(BlockCommand c) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->commands.size() >= 16)
        throw std::runtime_error("Too many pending Block requests");
    impl_->commands.push_back(std::move(c));
    ++impl_->pending_blocks;
    impl_->published.block_processing = true;
    impl_->dirty = true;
}
void Monitor::set_blocklists(bool christmas, bool custom) {
    std::lock_guard lock(impl_->mutex);
    impl_->pending_blocklists.emplace(christmas, custom);
}
void Monitor::set_auto_stop(bool enabled) {
    std::lock_guard lock(impl_->mutex);
    impl_->pending_auto_stop = enabled;
}
void Monitor::reset_custom_blocklist() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->commands.size() >= 16)
        throw std::runtime_error("Too many pending Block requests");
    impl_->commands.emplace_back(Impl::ResetCustomBlocklist{});
}
} // namespace humbug
