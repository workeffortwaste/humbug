#include "sonos/blocklist.h"
#include "sonos/monitor.h"
#include "sonos/network.h"
#include "sonos/protocol.h"
#include "sonos/skip_history.h"
#include "fake_cast.h"

#include <tinyxml2.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}
// Socket flags preserve the intended blocking modes and prevent descriptor leaks across exec.
void socket_flags() {
    auto listener = humbug::make_socket(SOCK_STREAM, false);
    auto outgoing = humbug::make_socket(SOCK_STREAM, false);
    auto nonblocking = humbug::make_socket(SOCK_DGRAM);
    const auto check_flags = [](const humbug::Socket& socket, bool expected_nonblocking) {
        const auto descriptor_flags = fcntl(socket.get(), F_GETFD);
        const auto status_flags = fcntl(socket.get(), F_GETFL);
        require(descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC), "socket close-on-exec");
        require(status_flags >= 0 && bool(status_flags & O_NONBLOCK) == expected_nonblocking,
                "socket nonblocking mode");
    };
    check_flags(listener, false);
    check_flags(nonblocking, true);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                listen(listener.get(), 1) == 0,
            "socket test listener");
    socklen_t length = sizeof(address);
    require(getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
                connect(outgoing.get(), reinterpret_cast<sockaddr*>(&address), length) == 0,
            "socket test connection");
    auto accepted = humbug::accept_socket(listener.get(), nullptr, nullptr);
    require(accepted.get() >= 0, "socket test accept");
    check_flags(accepted, true);
#ifdef __APPLE__
    int suppress_sigpipe = 0;
    socklen_t option_length = sizeof(suppress_sigpipe);
    require(getsockopt(accepted.get(), SOL_SOCKET, SO_NOSIGPIPE, &suppress_sigpipe, &option_length) == 0 &&
                suppress_sigpipe,
            "accepted socket suppresses SIGPIPE");
#endif
}
template <class F> void rejects(F f, const std::string& message) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
}
std::string escaped(const std::string& value) {
    tinyxml2::XMLDocument d;
    auto* e = d.NewElement("x");
    d.InsertEndChild(e);
    e->SetText(value.c_str());
    tinyxml2::XMLPrinter p;
    e->Accept(&p);
    std::string s = p.CStr();
    return s.substr(3, s.rfind("</x>") - 3);
}
std::string metadata(const std::string& title, const std::string& artist = "Artist",
                     bool creator_only = false) {
    const std::string artist_tag = creator_only ? "dc:creator" : "upnp:artist";
    return "<DIDL-Lite><item><dc:title>" + escaped(title) +
           "</dc:title>"
           "<" +
           artist_tag + ">" + escaped(artist) + "</" + artist_tag +
           ">"
           "<upnp:album>Album</upnp:album></item></DIDL-Lite>";
}
std::string event(const std::string& title, int track = 1, const std::string& state = "PLAYING",
                  const std::string& artist = "Artist") {
    return "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\"><e:property><LastChange>" +
           escaped("<Event><InstanceID val=\"0\"><TransportState val=\"" + state +
                   "\"/><CurrentTrack val=\"" + std::to_string(track) + "\"/><CurrentTrackURI val=\"x:" +
                   std::to_string(track) + "\"/><CurrentTrackMetaData val=\"" +
                   escaped(metadata(title, artist)) + "\"/></InstanceID></Event>") +
           "</LastChange></e:property></e:propertyset>";
}

class FakeSonos {
public:
    explicit FakeSonos(std::uint32_t initial_sequence = 0, bool interactive = false)
        : initial_sequence_(initial_sequence), interactive_(interactive) {
        server_ = humbug::make_socket(SOCK_STREAM, false);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(server_.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0, "fake bind");
        require(listen(server_.get(), 16) == 0, "fake listen");
        socklen_t len = sizeof(a);
        getsockname(server_.get(), reinterpret_cast<sockaddr*>(&a), &len);
        base = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port));
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeSonos() {
        done_ = true;
        if (thread_.joinable())
            thread_.join();
    }
    std::string base;
    std::atomic_int next{0}, stops{0}, renewals{0}, unsubscribes{0}, subscriptions{0};
    std::atomic_int advance_limit{0};
    std::atomic_bool repeat_next{false};
    std::function<void()> on_next;
    std::atomic_bool fail_next{false};
    std::atomic_bool next_available{true}, stop_available{true}, fail_stop{false}, reject_next{false},
        stopped{false};
    std::atomic_bool chunked_position{false};
    std::atomic_int elapsed{23}, duration{240};
    std::atomic_bool creator_only{false};
    std::atomic_bool offline{false}, fail_renewal{false}, fail_topology_renewal{false}, grouped{false};
    std::string failure() {
        std::lock_guard lock(mutex_);
        return failure_;
    }
    void send_event(const std::string& body, std::uint32_t seq, int which = 1) {
        std::string callback, sid;
        {
            std::lock_guard lock(mutex_);
            callback = callbacks_.at(which);
            sid = sids_.at(which);
        }
        notify(callback, sid, body, seq);
    }
    void set_track(std::string title, int number = 1) {
        std::lock_guard lock(mutex_);
        title_ = std::move(title);
        number_ = number;
    }
    void simulate_track(int which, const std::string& title, const std::string& artist) {
        std::lock_guard event_lock(event_mutex_);
        stopped = false;
        std::string callback, sid;
        int number;
        std::uint32_t sequence;
        {
            std::lock_guard lock(mutex_);
            auto& track = simulated_[which];
            track.title = title;
            track.artist = artist;
            number = ++track.number;
            if (!callbacks_.contains(which))
                return;
            callback = callbacks_.at(which);
            sid = sids_.at(which);
            sequence = ++track.sequence;
        }
        notify(callback, sid, event(title, number, "PLAYING", artist), sequence);
    }
    void resume(int which) {
        std::lock_guard event_lock(event_mutex_);
        stopped = false;
        std::string callback, sid, title, artist;
        int number;
        std::uint32_t sequence;
        {
            std::lock_guard lock(mutex_);
            auto& track = simulated_.at(which);
            callback = callbacks_.at(which);
            sid = sids_.at(which);
            title = track.title;
            artist = track.artist;
            number = track.number;
            sequence = ++track.sequence;
        }
        notify(callback, sid, event(title, number, "PLAYING", artist), sequence);
    }
    void regroup() {
        grouped = true;
        send_event("<propertyset><property><ZoneGroupState>" +
                       escaped("<ZoneGroupState>" + topology() + "</ZoneGroupState>") +
                       "</ZoneGroupState></property></propertyset>",
                   1, 0);
    }
    void reject_unknown_sid() {
        std::string callback;
        {
            std::lock_guard lock(mutex_);
            callback = callbacks_.at(1);
        }
        std::atomic_bool stop{false};
        const auto r = humbug::request(
            callback, "NOTIFY",
            {{"SID", "uuid:not-subscribed"}, {"SEQ", "1"}, {"NT", "upnp:event"}, {"NTS", "upnp:propchange"}},
            event("Wrong"), stop);
        require(r.status() == 412, "unknown subscription SID rejected");
    }

private:
    humbug::Socket server_;
    std::atomic_bool done_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::mutex event_mutex_;
    std::map<int, std::string> callbacks_, sids_;
    struct SimulatedTrack {
        std::string title = "Song & one", artist = "Artist";
        int number = 1;
        std::uint32_t sequence = 0;
    };
    std::map<int, SimulatedTrack> simulated_{{1, {}}, {2, {}}};
    std::string title_ = "Song & one", failure_;
    int number_ = 1, sid_counter_ = 0;
    std::uint32_t initial_sequence_;
    bool interactive_;
    std::string topology() const {
        if (grouped)
            return "<ZoneGroups><ZoneGroup Coordinator=\"RINCON_2\" ID=\"g3\">"
                   "<ZoneGroupMember UUID=\"RINCON_1\" ZoneName=\"Kitchen\" Location=\"" +
                   base + "/device1\"/><ZoneGroupMember UUID=\"RINCON_2\" ZoneName=\"Office\" Location=\"" +
                   base + "/device2\"/></ZoneGroup></ZoneGroups>";
        return "<ZoneGroups><ZoneGroup Coordinator=\"RINCON_1\" ID=\"g1\">"
               "<ZoneGroupMember UUID=\"RINCON_1\" ZoneName=\"Kitchen\" Location=\"" +
               base + "/device1\"/><ZoneGroupMember UUID=\"RINCON_B\" ZoneName=\"Dining\" Location=\"" +
               base +
               "/deviceB\"/><Satellite UUID=\"SUB\" ZoneName=\"Sub\"/>"
               "</ZoneGroup><ZoneGroup Coordinator=\"RINCON_2\" ID=\"g2\">"
               "<ZoneGroupMember UUID=\"RINCON_2\" ZoneName=\"Office\" Location=\"" +
               base + "/device2\"/></ZoneGroup></ZoneGroups>";
    }
    std::string description(int group) const {
        std::string out = "<root><device><UDN>uuid:RINCON_" + std::to_string(group) +
                          "</UDN><roomName>Room</roomName><serviceList>";
        for (const std::string name : {"AVTransport", "ZoneGroupTopology"}) {
            const auto path = "/" + std::to_string(group) + "/" + name;
            out += "<service><serviceType>urn:schemas-upnp-org:service:" + name +
                   ":1</serviceType><controlURL>" + path + "</controlURL><eventSubURL>" + path +
                   "/Event</eventSubURL></service>";
        }
        return out + "</serviceList></device></root>";
    }
    void notify(const std::string& callback, const std::string& sid, const std::string& body,
                std::uint32_t seq) {
        std::atomic_bool stop{false};
        const auto r = humbug::request(
            callback, "NOTIFY",
            {{"SID", sid}, {"SEQ", std::to_string(seq)}, {"NT", "upnp:event"}, {"NTS", "upnp:propchange"}},
            body, stop);
        require(r.status() == 200, "fake event callback rejected: " + r.first);
    }
    void serve() {
        try {
            while (!done_) {
                pollfd p{server_.get(), POLLIN, 0};
                if (poll(&p, 1, 25) <= 0)
                    continue;
                humbug::Socket client = humbug::accept_socket(server_.get(), nullptr, nullptr, false);
                timeval timeout{2, 0};
                setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                std::string data;
                char buffer[8192];
                std::size_t end;
                while ((end = data.find("\r\n\r\n")) == std::string::npos) {
                    const auto n = recv(client.get(), buffer, sizeof(buffer), 0);
                    if (n <= 0)
                        throw std::runtime_error("fake request truncated");
                    data.append(buffer, static_cast<std::size_t>(n));
                }
                std::istringstream input(data.substr(0, end));
                std::string method, path, version, line;
                input >> method >> path >> version;
                std::getline(input, line);
                std::map<std::string, std::string> headers;
                while (std::getline(input, line)) {
                    const auto colon = line.find(':');
                    if (colon == std::string::npos)
                        continue;
                    auto val = line.substr(colon + 2);
                    if (val.ends_with('\r'))
                        val.pop_back();
                    headers[line.substr(0, colon)] = val;
                }
                const auto size = std::stoul(headers.at("Content-Length"));
                while (data.size() - end - 4 < size) {
                    const auto n = recv(client.get(), buffer, sizeof(buffer), 0);
                    if (n <= 0)
                        throw std::runtime_error("fake request body truncated");
                    data.append(buffer, static_cast<std::size_t>(n));
                }
                const int which = path.find('2') != std::string::npos ? 2 : 1;
                std::string title, artist = "Artist";
                int track;
                {
                    std::lock_guard lock(mutex_);
                    title = interactive_ ? simulated_.at(which).title : title_;
                    artist = interactive_ ? simulated_.at(which).artist : "Artist";
                    track = interactive_ ? simulated_.at(which).number : number_;
                }
                std::string body, extra;
                int status = 200;
                bool advance = false, chunked = false;
                if (offline)
                    status = 503;
                else if (method == "GET")
                    body = description(which);
                else if (method == "SUBSCRIBE") {
                    std::string sid;
                    if (headers.contains("SID")) {
                        sid = headers.at("SID");
                        ++renewals;
                        if (fail_renewal.exchange(false))
                            status = 412;
                        if (path.find("ZoneGroupTopology") != std::string::npos &&
                            fail_topology_renewal.exchange(false))
                            status = 412;
                    } else {
                        ++subscriptions;
                        sid = "uuid:test-" + std::to_string(++sid_counter_);
                        auto cb = headers.at("CALLBACK");
                        cb = cb.substr(1, cb.size() - 2);
                        {
                            const auto index =
                                path.find("ZoneGroupTopology") == std::string::npos ? which : 0;
                            std::lock_guard lock(mutex_);
                            callbacks_[index] = cb;
                            sids_[index] = sid;
                            if (interactive_ && index != 0)
                                simulated_.at(index).sequence = 0;
                        }
                        const auto payload =
                            path.find("ZoneGroupTopology") != std::string::npos
                                ? "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\"><e:property>"
                                  "<ZoneGroupState>" +
                                      escaped("<ZoneGroupState>" + topology() + "</ZoneGroupState>") +
                                      "</ZoneGroupState></e:property></e:propertyset>"
                                : event(title, track, "PLAYING", artist);
                        // Deliberately deliver the initial callback BEFORE the SUBSCRIBE response.
                        notify(cb, sid, payload,
                               path.find("ZoneGroupTopology") != std::string::npos ? 0 : initial_sequence_);
                    }
                    extra = "SID: " + sid + "\r\nTIMEOUT: Second-3\r\n";
                } else if (method == "UNSUBSCRIBE") {
                    ++unsubscribes;
                    if (interactive_) {
                        const auto sid = headers.at("SID");
                        std::lock_guard lock(mutex_);
                        for (auto it = sids_.begin(); it != sids_.end(); ++it) {
                            if (it->second == sid) {
                                callbacks_.erase(it->first);
                                sids_.erase(it);
                                break;
                            }
                        }
                    }
                } else if (method == "POST") {
                    const auto action = headers.at("SOAPACTION");
                    if (action.find("#GetZoneGroupState") != std::string::npos)
                        body = "<r><ZoneGroupState>" + escaped(topology()) + "</ZoneGroupState></r>";
                    else if (action.find("#GetTransportInfo") != std::string::npos)
                        body = std::string("<r><CurrentTransportState>") + (stopped ? "STOPPED" : "PLAYING") +
                               "</CurrentTransportState></r>";
                    else if (action.find("#GetPositionInfo") != std::string::npos) {
                        const auto seconds = elapsed.load();
                        const auto relative_time = seconds == -3 ? "1:02:03.250"
                                                   : seconds < 0 ? "NOT_IMPLEMENTED"
                                                                 : "0:00:" + std::to_string(seconds);
                        body = "<r><Track>" + std::to_string(track) +
                               "</Track><TrackURI>x:" + std::to_string(track) + "</TrackURI><TrackMetaData>" +
                               escaped(metadata(title, artist, creator_only)) + "</TrackMetaData>" +
                               "<RelTime>" + relative_time +
                               "</RelTime>"
                               "<TrackDuration>" +
                               (duration < 0 ? std::string("NOT_IMPLEMENTED") : "0:04:00") +
                               "</TrackDuration></r>";
                        chunked = chunked_position;
                    } else if (action.find("#GetCurrentTransportActions") != std::string::npos)
                        body = std::string("<r><Actions>") + (next_available ? "Next," : "") +
                               (stop_available ? "Stop" : "") + "</Actions></r>";
                    else if (action.find("#Next") != std::string::npos) {
                        if (on_next)
                            on_next();
                        ++next;
                        if (fail_next)
                            status = 500;
                        else if (reject_next) {
                            status = 500;
                            body = "<Envelope><Body><Fault><errorCode>701</errorCode>"
                                   "<errorDescription>Transition not available</errorDescription>"
                                   "</Fault></Body></Envelope>";
                        } else
                            advance = interactive_ && (!advance_limit || next <= advance_limit);
                    } else if (action.find("#Stop") != std::string::npos) {
                        ++stops;
                        if (fail_stop)
                            status = 500;
                        else
                            stopped = true;
                    } else
                        throw std::runtime_error("Forbidden action sent: " + action);
                } else
                    throw std::runtime_error("Unexpected fake HTTP method: " + method);
                std::string wire = "HTTP/1.1 " + std::to_string(status) + " Result\r\n" + extra;
                if (chunked) {
                    std::ostringstream encoded;
                    const auto middle = body.size() / 2;
                    encoded << "Transfer-Encoding: chunked\r\n\r\n"
                            << std::hex << middle << "\r\n"
                            << body.substr(0, middle) << "\r\n"
                            << body.size() - middle << "\r\n"
                            << body.substr(middle) << "\r\n0\r\n\r\n";
                    wire += encoded.str();
                } else {
                    wire += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
                }
                require(send(client.get(), wire.data(), wire.size(), humbug::socket_send_flags) ==
                            static_cast<ssize_t>(wire.size()),
                        "fake response send");
                if (advance)
                    simulate_track(which, repeat_next ? title : "Next track " + std::to_string(track + 1),
                                   artist);
            }
        } catch (const std::exception& e) {
            std::lock_guard lock(mutex_);
            failure_ = e.what();
        }
    }
};

template <class F> humbug::Snapshot wait(humbug::Monitor& m, F predicate) {
    humbug::Snapshot s;
    std::vector<humbug::BlockedSong> notifications;
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (m.take(s))
            notifications.insert(notifications.end(), s.blocked_songs.begin(), s.blocked_songs.end());
        if (predicate(s)) {
            s.blocked_songs = std::move(notifications);
            return s;
        }
        std::this_thread::sleep_for(20ms);
    }
    throw std::runtime_error("Timed out waiting for state: " + s.status + " " + s.notice);
}
// Exercise parsing and matching boundaries before network integration tests.
void protocol_tests() {
    FakeSonos server;
    humbug::Device device;
    device.transport = {"urn:schemas-upnp-org:service:AVTransport:1", server.base + "/1/AVTransport", {}};
    std::atomic_bool stop{false};
    server.elapsed = -3;
    require(humbug::position(device, stop).elapsed_seconds == 3723,
            "Sonos hours and fractional seconds are parsed");
    server.elapsed = 99;
    rejects([&] { humbug::position(device, stop); }, "invalid Sonos track time is reported");
    server.elapsed = -1;
    server.duration = -1;
    const auto unknown = humbug::position(device, stop);
    require(!unknown.elapsed_seconds && !unknown.duration_seconds, "unsupported Sonos timing is unavailable");
    const std::string topology = "<ZoneGroups><ZoneGroup Coordinator=\"RINCON_1\" ID=\"g1\">"
                                 "<ZoneGroupMember UUID=\"RINCON_1\" ZoneName=\"Kitchen\" "
                                 "Location=\"http://127.0.0.1:1400/device\"/></ZoneGroup></ZoneGroups>";
    for (const auto& xml : {topology, "<ZoneGroupState>" + topology + "</ZoneGroupState>",
                            "<r><ZoneGroupState>" + escaped(topology) + "</ZoneGroupState></r>"}) {
        const auto groups = humbug::parse_topology(xml);
        require(groups.size() == 1 && groups[0].rooms[0] == "Kitchen",
                "parse direct and escaped topology variants");
    }
    const auto t = humbug::parse_metadata(metadata("A & B &lt;literal&gt;", "Björk"));
    require(t.title == "A & B &lt;literal&gt;" && t.artist == "Björk", "XML decoded exactly once");
    const auto credited =
        humbug::parse_metadata("<item><dc:title>Song</dc:title><dc:creator>Uploader</dc:creator>"
                               "<upnp:artist>Performer</upnp:artist></item>");
    require(credited.artist == "Performer", "explicit artist takes priority over creator");
    rejects([] { humbug::parse_metadata("<DIDL-Lite>"); }, "malformed XML rejected");
    const auto check_xml_error = [](const std::function<void()>& operation, const std::string& stage) {
        try {
            operation();
            throw std::runtime_error("expected malformed XML to fail");
        } catch (const std::runtime_error& e) {
            const std::string error = e.what();
            require(error.find(stage) != std::string::npos, "XML error identifies the parsing stage");
            require(error.find("XML_ERROR") != std::string::npos, "XML error names the parser failure");
            require(error.find("private-song-title") == std::string::npos,
                    "XML error does not expose response content");
        }
    };
    check_xml_error([] { humbug::parse_metadata("<item>private-song-title"); }, "track metadata DIDL");
    check_xml_error([] { humbug::parse_event("<propertyset>private-song-title"); },
                    "AVTransport event envelope");
    check_xml_error(
        [] {
            humbug::parse_event("<propertyset><LastChange>&lt;Event&gt;private-song-title"
                                "</LastChange></propertyset>");
        },
        "AVTransport LastChange");
    check_xml_error(
        [] {
            humbug::parse_topology("<ZoneGroupState>&lt;ZoneGroups&gt;private-song-title"
                                   "</ZoneGroupState>");
        },
        "embedded ZoneGroupState");
    for (const auto* field : {"artist-contains", "TRACK-CONTAINS", "album-contains"})
        rejects([&] { humbug::parse_rules(std::string(field) + ":value\n"); },
                "removed substring rules are rejected");
    const humbug::Track exact{"Title\t\\\n", "Artist & é", "", "", ""};
    auto rules = humbug::parse_rules(humbug::exact_rule(exact));
    require(humbug::blocked(exact, rules), "exact pair escaped UTF-8 round trip");
    auto other = exact;
    other.artist = "Other";
    require(!humbug::blocked(other, rules), "same title by other artist not blocked");
    rejects([] { humbug::parse_rules("exact:a\t\n"); }, "empty exact title rejected");
    rejects([] { humbug::parse_rules("genre:no"); }, "unknown rules rejected");
    rejects([] { humbug::parse_rules("title:unknown"); }, "unknown title prefix rejected");
    rejects([] { humbug::parse_rules("track:Artist\tSong"); }, "tab in track rule rejected");
    rules = humbug::parse_rules("artist:Élodie Noël\ntrack:Christmas Song One\nkeyword:yule\n");
    require(humbug::blocked({"Song", "ELODIE NOEL"}, rules), "artist rule normalizes accents and case");
    require(!humbug::blocked({"Song", "Elodie Noel Tribute"}, rules),
            "artist rule matches the full credit");
    require(humbug::blocked({"Christmas Song One - Remastered", "Other"}, rules),
            "track rule matches versions");
    require(humbug::blocked({"Christmas Song One - Live at the Hall", "Other"}, rules),
            "track rule matches recording suffixes");
    require(!humbug::blocked({"Christmas Song One Is Playing", "Other"}, rules),
            "track rule matches the full title");
    require(humbug::blocked({"Song", "Other", "A Yule Album"}, rules), "keyword matches album words");
    require(!humbug::blocked({"Yuletidesong", "Other"}, rules), "keyword respects word boundaries");
    humbug::Group group;
    humbug::merge(group, humbug::parse_event(event("Song & one")));
    const auto identity = group.track.identity();
    auto update =
        humbug::parse_event("<propertyset><property><LastChange>&lt;Event&gt;&lt;InstanceID val=\"0\"&gt;"
                            "&lt;TransportState val=\"PAUSED_PLAYBACK\"/&gt;&lt;/InstanceID&gt;&lt;/Event&gt;"
                            "</LastChange></property></propertyset>");
    humbug::merge(group, update);
    require(group.state == "PAUSED_PLAYBACK" && group.track.identity() == identity,
            "partial event retains metadata and updates playback state");
    group.track.elapsed_seconds = 12;
    group.track.duration_seconds = 240;
    humbug::merge(group, humbug::parse_event(event("Song & one")));
    require(group.track.elapsed_seconds == 12 && group.track.duration_seconds == 240,
            "same-track metadata events retain timing");
    humbug::merge(group, humbug::parse_event(event("New song")));
    require(!group.track.elapsed_seconds && !group.track.duration_seconds,
            "new metadata on the same URI does not retain previous timing");
    const auto occurrence = group.occurrence;
    humbug::merge(group, {std::string("TRANSITIONING"), {}, {}, {}});
    humbug::merge(group, {std::string("PLAYING"), {}, {}, {}});
    require(group.occurrence == occurrence + 1,
            "repeated same-metadata song after a transport transition is a new occurrence");
}
// Playback actions, rule persistence and history must remain independent on failure.
void integration(const std::filesystem::path& dir, bool fail_save, bool fail_skip) {
    FakeSonos server;
    server.fail_next = fail_skip;
    server.chunked_position = fail_save;
    const auto path = dir / (fail_save ? "unwritable" : fail_skip ? "skip-failure" : "rules");
    std::atomic_bool skipped_before_save{false};
    server.on_next = [&] {
        if (server.next == 0)
            skipped_before_save = !std::filesystem::exists(path);
    };
    if (fail_save) {
        std::ofstream out(path);
        out << "# initial\n";
    }
    humbug::Options o;
    o.discover_cast = false;
    o.blocklist = path.string();
    o.history_path = path.string() + ".history";
    o.devices = {server.base + "/device1"};
    o.listen_address = "127.0.0.1";
    humbug::Monitor monitor(o);
    monitor.start();
    auto s =
        wait(monitor, [](const auto& v) { return v.groups.size() == 2 && v.status == "Live UPnP events"; });
    require(s.groups[0].rooms.size() == 2, "group member names retained, satellite omitted");
    require(s.groups[1].rooms[0] == "Office", "coordinator discovered from topology alone");
    server.reject_unknown_sid();
    if (fail_save) {
        std::ofstream out(path, std::ios::app);
        out << "# external change\n";
    }
    const auto g = s.groups[0];
    monitor.block({g.id, g.track.identity(), g.occurrence});
    s = wait(monitor, [fail_save, fail_skip](const auto& v) {
        return v.notice.find(fail_save   ? "NOT SAVED"
                             : fail_skip ? "BLOCK ACTION FAILED"
                                         : "Block saved") != std::string::npos;
    });
    require(server.next >= 1, "Block attempts Next even when persistence fails");
    if (!fail_save)
        require(skipped_before_save, "manual Block attempts Next before saving the rule");
    require(s.notice_is_error == (fail_save || fail_skip),
            "only failed manual block operations request an error toast");
    if (fail_skip)
        require(s.blocked_songs.empty(), "failed skips never emit success popups");
    else {
        require(!s.blocked_songs.empty() && s.blocked_songs.front().track.title == g.track.title &&
                    s.blocked_songs.front().room == "Kitchen + Dining",
                "successful skip emits the actual song and room for its popup");
    }
    if (fail_save)
        require(s.blocks.total == 1, "successful skip counts even if rule save fails");
    if (fail_skip)
        require(s.blocks.total == 0 && server.stops == 0,
                "failed Next is not counted and never triggers Stop");
    if (!fail_save) {
        humbug::Blocklist saved(path.string(), true);
        require(saved.matches(g.track), "saved rule matches after reopening");
    } else {
        std::ifstream in(path);
        const std::string contents{std::istreambuf_iterator<char>(in), {}};
        require(contents == "# initial\n# external change\n", "external edit never overwritten");
    }
    if (!fail_save && !fail_skip) {
        // The global rule must also skip in the other group, not only the selected one.
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (server.next < 2 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(20ms);
        require(server.next == 2, "shared rule applied across all groups");
        auto stats =
            s.blocks.total == 2 ? s : wait(monitor, [](const auto& v) { return v.blocks.total == 2; });
        require(stats.blocks.today == 2 &&
                    stats.blocks.recent ==
                        std::vector<std::string>{"Song & one - Artist", "Song & one - Artist"},
                "manual and automatic skips count each occurrence");
        require(stats.blocks.latest && stats.blocks.latest->title == "Song & one" &&
                    stats.blocks.latest->artist == "Artist",
                "the monitor publishes separate title and artist fields for the latest block");
        server.send_event(event("Song & one"), 1);
        server.send_event(event("Song & one"), 1);
        std::this_thread::sleep_for(100ms);
        require(server.next == 2, "duplicate events do not cause repeated skips");
        server.set_track("Replacement", 2);
        monitor.block({g.id, g.track.identity(), g.occurrence});
        wait(monitor, [](const auto& v) { return v.notice.find("cancelled") != std::string::npos; });
        require(server.next == 2, "stale Block never skips replacement");
        server.send_event(event("Replacement", 2), 3);
        wait(monitor,
             [](const auto& v) { return !v.groups.empty() && v.groups[0].track.title == "Replacement"; });
        std::this_thread::sleep_for(2200ms);
        require(server.renewals > 0, "subscriptions renewed");
    }
    const auto before = std::chrono::steady_clock::now();
    monitor.stop();
    humbug::SkipHistory restored(o.history_path);
    require(restored.stats(humbug::local_day()).total == (fail_skip   ? 0u
                                                          : fail_save ? 1u
                                                                      : 2u),
            "history survives restart without counting duplicates or failed skips");
    require(std::chrono::steady_clock::now() - before < 2s, "bounded monitor shutdown");
    require(server.failure().empty(), server.failure());
    require(server.unsubscribes >= 3, "subscriptions unsubscribed at shutdown");
}

// Daily counts reset without losing total history or corrupting quoted metadata.
void history_days(const std::filesystem::path& dir) {
    const auto path = (dir / "calendar-history").string();
    humbug::SkipHistory history(path);
    history.record({"First - Live", "Artist", ""}, "2026-01-01");
    history.record({"Second", "Artist", ""}, "2026-01-01");
    require(history.stats("2026-01-02").today == 0, "new local day resets today only");
    history.record({"Third \"quoted\"\nline", "Artist", ""}, "2026-01-02");
    history.record({"Fourth - Live", "Artist - Duo \"quoted\"", ""}, "2026-01-02");
    const humbug::SkipHistory restored(path);
    const auto stats = restored.stats("2026-01-02");
    require(stats.error.empty() && stats.today == 2 && stats.total == 4 &&
                stats.recent == std::vector<std::string>{"Fourth - Live - Artist - Duo \"quoted\"",
                                                         "Third \"quoted\" line - Artist", "Second - Artist"},
            "only the latest three songs are displayed, newest first, across restarts");
    require(stats.latest && stats.latest->title == "Fourth - Live" &&
                stats.latest->artist == "Artist - Duo \"quoted\"",
            "new history rows preserve separators and quotes in both metadata fields");
    const auto invalid_path = dir / "invalid-history";
    {
        std::ofstream out(invalid_path);
        out << "2026-01-01 \"First - Artist\"\n";
    }
    require(!humbug::SkipHistory(invalid_path.string()).stats("2026-01-01").error.empty(),
            "history entries without separate artist fields are rejected");
}

// Renewal failures, sequence wrap and regrouping must not leave stale playback state.
void recovery(const std::filesystem::path& dir) {
    FakeSonos server(UINT32_MAX);
    humbug::Options o;
    o.discover_cast = false;
    o.blocklist = (dir / "recovery-rules").string();
    o.history_path = o.blocklist + ".history";
    o.devices = {server.base + "/device1"};
    o.listen_address = "127.0.0.1";
    o.discovery_interval = 1s;
    humbug::Monitor m(o);
    m.start();
    wait(m, [](const auto& s) { return s.status == "Live UPnP events"; });
    server.fail_topology_renewal = true;
    wait(m, [](const auto& s) {
        return !s.groups.empty() && s.status != "Live UPnP events" && !s.groups[0].connected;
    });
    auto restored = wait(m, [](const auto& s) { return s.status == "Live UPnP events"; });
    require(restored.groups[0].connected, "topology re-subscription revalidates existing room events");
    server.set_track("Wrapped", 2);
    server.send_event(event("Wrapped", 2), 1);
    wait(m, [](const auto& s) { return !s.groups.empty() && s.groups[0].track.title == "Wrapped"; });
    server.send_event(event("Stale", 1), UINT32_MAX);
    server.send_event(event("Wrapped", 2), 1);
    std::this_thread::sleep_for(100ms);
    humbug::Snapshot s;
    m.take(s);
    if (!s.groups.empty())
        require(s.groups[0].track.title == "Wrapped", "stale sequence ignored after wrap");
    const auto before = server.subscriptions.load();
    server.fail_renewal = true;
    const auto renewal_deadline = std::chrono::steady_clock::now() + 6s;
    while (server.subscriptions <= before && std::chrono::steady_clock::now() < renewal_deadline)
        std::this_thread::sleep_for(20ms);
    require(server.subscriptions > before, "failed renewal creates a fresh subscription");
    wait(m, [](const auto& v) { return v.status == "Live UPnP events"; });
    server.offline = true;
    wait(m, [](const auto& v) {
        return !v.groups.empty() && v.status != "Live UPnP events" && !v.groups[0].connected;
    });
    server.offline = false;
    wait(m, [](const auto& v) { return v.status == "Live UPnP events"; });
    server.regroup();
    wait(m, [](const auto& v) {
        return v.groups.size() == 1 && v.groups[0].id == "g3" && v.groups[0].coordinator == "RINCON_2" &&
               v.groups[0].connected;
    });
    m.stop();
    require(server.failure().empty(), server.failure());
}

// The preset must catch intended variants without blocking unrelated titles or custom rules.
void christmas_blocklist(const std::filesystem::path& dir) {
    const auto path = dir / "christmas-custom-rules";
    {
        std::ofstream out(path);
        out << "artist:custom artist\n";
    }
    const humbug::Blocklist enabled(path.string(), true);
    const humbug::Blocklist disabled(path.string(), true, false);
    humbug::Track track;
    track.artist = "Cover Artist";
    for (const auto* title : {
             "jingle bell ROCK — 2004 Remaster",
             "Mary Did You Know",
             "Rockin\xe2\x80\x99 Around the Christmas Tree",
             "The Christmas Song",
             "Christmas Eve/Sarajevo 12/24 (Instrumental)",
             "Underneath the Tree",
             "An Original CHRISTMAS.Tree Carol",
             "X‑MAS Medley",
             "New SANTA—CLAUS Song",
             "In Dulci Jubilo",
             "Glittery",
             "MUST.BE.SANTA!",
             "All I Want For Christmas (Is My Two Front Teeth)",
             "Over The River And Through The Woods",
             "Auld Lang Syne",
             "Hallelujah",
             "Holiday",
             "Scrooge",
             "take me home (4 christmas)",
         }) {
        track.title = title;
        require(enabled.matches(track), std::string("built-in Christmas match: ") + title);
        require(!disabled.matches(track), std::string("Christmas list disabled: ") + title);
    }
    for (const auto* title : {
             "Do You Want to Build a Snowman?",
             "Jingle Bell Rock Is Playing",
             "Midwinter Blues",
             "Yuletidesong",
         }) {
        track.title = title;
        require(!enabled.matches(track), std::string("no blanket Christmas match: ") + title);
    }
    track.title = "Feeling Good";
    for (const auto* artist : {"MICHAEL BUBLÉ", "Michael—Buble", "MiChAeL BuBlE\xcc\x81"}) {
        track.artist = artist;
        require(enabled.matches(track), "built-in artist matching folds accents, punctuation and case");
        require(!disabled.matches(track), "disabling Christmas blocking also disables artist matches");
    }
    track.artist = "Michael Bublé Tribute";
    require(!enabled.matches(track), "artist rules match the full credit, not a substring");
    track.album = "The Noël & CHRISTMAS Collection";
    require(enabled.matches(track) && !disabled.matches(track),
            "Christmas album keyword uses the same toggle");
    track.album.clear();
    track.artist = "Custom Artist";
    require(disabled.matches(track), "custom rules remain active with Christmas blocking off");

    const auto shared_path = dir / "shared-format.blocklist";
    {
        std::ofstream out(shared_path);
        out << "track:Christmas Song One\nkeyword:yule\n";
    }
    const humbug::Blocklist shared(shared_path.string(), true, false);
    require(shared.matches({"Christmas Song One - Remastered", "Other"}),
            "custom track uses the same title matching as the bundled list");
    require(shared.matches({"Song", "Other", "A Yule Album"}),
            "custom keyword uses the same album matching as the bundled list");

    humbug::Blocklist editable(path.string(), true);
    const humbug::Track saved{"Ordinary song", "Other artist"};
    const humbug::Track preset{"Jingle Bell Rock", "Cover Artist"};
    editable.add(saved);
    editable.add(saved);
    {
        std::ifstream in(path);
        const std::string contents{std::istreambuf_iterator<char>(in), {}};
        const auto line = humbug::exact_rule(saved);
        require(contents.find(line) != std::string::npos &&
                    contents.find(line, contents.find(line) + line.size()) == std::string::npos,
                "manual blocks persist exactly once in the custom file");
    }
    editable.set_enabled(true, false);
    require(!editable.matches(saved) && editable.matches(preset),
            "disabling Custom still permits preset matches");
    editable.set_enabled(false, true);
    require(editable.matches(saved) && !editable.matches(preset),
            "disabling Christmas still permits custom matches");
    editable.reset_saved_tracks();
    const humbug::Blocklist reopened(path.string(), true);
    require(!reopened.matches(saved) && reopened.matches(preset) && reopened.matches(track),
            "reset persists removal of saved tracks without altering preset or hand-written rules");
}

// The fake streams must behave independently and survive application restarts.
void interactive_simulation(const std::filesystem::path& dir) {
    FakeSonos server(0, true);
    humbug::Options options;
    options.discover_cast = false;
    options.blocklist = (dir / "simulation-rules").string();
    options.history_path = options.blocklist + ".history";
    options.devices = {server.base + "/device1"};
    options.listen_address = "127.0.0.1";
    humbug::Monitor monitor(options);
    monitor.start();
    wait(monitor, [](const auto& s) { return s.status == "Live UPnP events"; });
    server.simulate_track(1, "Simulated song", "Simulated artist");
    const auto state = wait(monitor, [](const auto& s) {
        return s.groups.size() == 2 && s.groups[0].track.title == "Simulated song" &&
               s.groups[0].track.artist == "Simulated artist";
    });
    const auto& group = state.groups[0];
    monitor.block({group.id, group.track.identity(), group.occurrence});
    const auto advanced = wait(monitor, [](const auto& s) {
        return s.blocks.total == 1 && s.groups.size() == 2 && s.groups[0].track.title == "Next track 3";
    });
    require(advanced.groups[1].track.title == "Song & one",
            "simulated Next only advances its selected stream");
    monitor.stop();
    server.simulate_track(1, "After restart", "Another artist");
    humbug::Monitor restarted(options);
    restarted.start();
    const auto restored = wait(restarted, [](const auto& s) {
        return s.status == "Live UPnP events" && !s.groups.empty() &&
               s.groups[0].track.title == "After restart";
    });
    require(restored.groups[0].track.artist == "Another artist",
            "simulator retains track changes between app sessions");
    server.simulate_track(2, "Last Christmas", "Cover Artist");
    const auto christmas = wait(restarted, [](const auto& s) {
        return s.blocks.total == 2 && s.groups.size() == 2 && s.groups[1].track.title == "Next track 3";
    });
    require(christmas.groups[0].track.title == "After restart",
            "built-in rule skips Christmas covers in a non-selected stream");
    require(!humbug::Blocklist(options.blocklist, true, false).matches(christmas.blocks.latest.value()),
            "built-in matches do not create custom rules");
    restarted.stop();
    require(server.failure().empty(), server.failure());
}

void repeat_blocked_song_stops_silently(const std::filesystem::path& dir) {
    FakeSonos server(0, true);
    server.repeat_next = true;
    server.simulate_track(2, "Allowed", "Other");
    humbug::Options options;
    options.discover_cast = false;
    options.blocklist = (dir / "repeat-stop-rules").string();
    options.history_path = options.blocklist + ".history";
    options.devices = {server.base + "/device1"};
    options.listen_address = "127.0.0.1";
    options.log_notices = false;
    {
        std::ofstream out(options.blocklist);
        out << "artist:Artist\n";
    }
    humbug::Monitor monitor(options);
    monitor.start();
    const auto stopped = wait(monitor, [&](const auto& state) {
        return server.next == 1 && server.stops == 1 && state.blocks.total == 1 &&
               !state.groups.empty() && state.groups[0].state == "STOPPED";
    });
    require(stopped.blocked_songs.size() == 1 && server.next == 1 && server.stops == 1,
            "a repeated blocked song is stopped without a second block record");
    monitor.stop();
    require(server.failure().empty(), server.failure());
}

// Limit runaway skips, while preserving disabled, unavailable-stop and resume behaviour.
void auto_stop_limit(const std::filesystem::path& dir, bool enabled, bool stop_available) {
    FakeSonos server(0, true);
    server.simulate_track(2, "Allowed", "Other");
    server.stop_available = stop_available;
    if (!enabled)
        server.advance_limit = 25;
    humbug::Options options;
    options.discover_cast = false;
    options.auto_stop = enabled;
    options.blocklist = (dir / ("auto-stop-" + std::to_string(enabled) +
                                std::to_string(stop_available))).string();
    options.history_path = options.blocklist + ".history";
    options.devices = {server.base + "/device1"};
    options.listen_address = "127.0.0.1";
    {
        std::ofstream out(options.blocklist);
        out << "artist:Artist\n";
    }
    humbug::Monitor monitor(options);
    monitor.start();
    if (!enabled) {
        wait(monitor, [&](const auto& s) { return server.next >= 26 && s.blocks.total >= 26; });
        require(server.stops == 0, "disabled auto-stop allows more than 10 skips");
        monitor.set_auto_stop(true);
        server.advance_limit = 0;
        server.simulate_track(1, "Another blocked song", "Artist");
        wait(monitor, [&](const auto& s) {
            return server.stops == 1 && s.groups[0].state == "STOPPED";
        });
        require(server.next == 46, "enabling auto-stop starts a fresh consecutive-skip count");
    } else if (!stop_available) {
        wait(monitor, [](const auto& s) {
            return s.notice.find("Receiver does not allow Stop") != std::string::npos;
        });
        std::this_thread::sleep_for(250ms);
        require(server.next == 10 && server.stops == 0,
                "unavailable auto-stop reports failure and does not continue skipping");
    } else {
        const auto stopped = wait(monitor, [&](const auto& s) {
            return server.stops == 1 && s.blocks.total == 11;
        });
        require(server.next == 10 && !stopped.blocked_songs.empty() &&
                    stopped.blocked_songs.back().auto_stop,
                "ten skips are followed by a labeled auto-stop on the next blocked song");
        server.resume(1);
        wait(monitor, [&](const auto& s) {
            return server.stops == 2 && s.blocks.total == 42 && s.groups[0].state == "STOPPED";
        });
        require(server.next == 40, "resuming the same blocked song starts blocking again");
    }
    monitor.stop();
    require(server.failure().empty(), server.failure());
}

// An allowed track breaks the consecutive-skip sequence.
void auto_stop_reset_on_allowed(const std::filesystem::path& dir) {
    FakeSonos server(0, true);
    server.simulate_track(2, "Allowed", "Other");
    server.advance_limit = 10;
    humbug::Options options;
    options.discover_cast = false;
    options.log_notices = false;
    options.blocklist = (dir / "auto-stop-reset").string();
    options.history_path = options.blocklist + ".history";
    options.devices = {server.base + "/device1"};
    options.listen_address = "127.0.0.1";
    {
        std::ofstream out(options.blocklist);
        out << "artist:Artist\n";
    }
    humbug::Monitor monitor(options);
    monitor.start();
    wait(monitor, [&](const auto& s) { return server.next == 11 && s.blocks.total == 11; });
    server.simulate_track(1, "Allowed", "Other");
    wait(monitor, [](const auto& s) {
        return s.groups.size() == 2 && s.groups[0].track.title == "Allowed";
    });
    server.advance_limit = 0;
    server.simulate_track(1, "Blocked again", "Artist");
    wait(monitor, [&](const auto& s) {
        return server.stops == 1 && s.groups[0].state == "STOPPED";
    });
    require(server.next == 31, "an allowed song resets the consecutive-skip count");
    monitor.stop();
    require(server.failure().empty(), server.failure());
}

// A Cast action must not alter or disconnect concurrently monitored Sonos groups.
void coexistence(const std::filesystem::path& dir) {
    FakeSonos sonos;
    FakeCast cast;
    humbug::Options options;
    options.discover_cast = false;
    options.cast_devices = {cast.endpoint};
    options.devices = {sonos.base + "/device1"};
    options.listen_address = "127.0.0.1";
    options.blocklist = (dir / "coexist-rules").string();
    options.history_path = options.blocklist + ".history";
    humbug::Monitor monitor(options);
    monitor.start();
    const auto state = wait(monitor, [](const auto& s) {
        return s.groups.size() == 3 && s.groups[0].connected && s.groups[1].connected &&
               s.groups[2].connected && s.groups[2].provider == humbug::Provider::Cast;
    });
    const auto& cast_group = state.groups[2];
    monitor.block({cast_group.id, cast_group.track.identity(), cast_group.occurrence});
    const auto blocked = wait(monitor, [](const auto& s) { return s.blocks.total == 1; });
    require(cast.next_count == 1 && sonos.next == 0 && blocked.groups.size() == 3 &&
                blocked.groups[0].connected,
            "Cast block does not disrupt or mutate Sonos streams");
    monitor.stop();
    require(sonos.failure().empty(), sonos.failure());
    require(cast.failure().empty(), cast.failure());
}

// Fallback Stop must respect device capabilities and count only confirmed actions.
void stop_fallback(const std::filesystem::path& dir, bool automatic, bool reject_next, bool stop_available,
                   bool fail_stop) {
    FakeSonos server;
    server.next_available = reject_next;
    server.reject_next = reject_next;
    server.stop_available = stop_available;
    server.fail_stop = fail_stop;
    const auto suffix = std::to_string(automatic) + std::to_string(reject_next) +
                        std::to_string(stop_available) + std::to_string(fail_stop);
    humbug::Options options;
    options.discover_cast = false;
    options.devices = {server.base + "/device1"};
    options.listen_address = "127.0.0.1";
    options.blocklist = (dir / ("stop-sonos-" + suffix)).string();
    options.history_path = options.blocklist + ".history";
    if (automatic) {
        std::ofstream out(options.blocklist);
        out << "exact:Artist\tSong & one\n";
    }
    humbug::Monitor monitor(options);
    monitor.start();
    if (!automatic) {
        const auto initial = wait(
            monitor, [](const auto& s) { return s.groups.size() == 2 && s.status == "Live UPnP events"; });
        const auto group = initial.groups[0];
        monitor.block({group.id, group.track.identity(), group.occurrence});
    }
    const bool success = stop_available && !fail_stop;
    const auto result = wait(monitor, [success](const auto& s) {
        return success ? s.blocks.total == 1
                       : s.notice.find("BLOCK ACTION FAILED") != std::string::npos ||
                             s.notice.find("block action failed") != std::string::npos;
    });
    require((stop_available ? server.stops >= 1 && server.stops <= 2 : server.stops == 0) &&
                server.next == (reject_next ? 1 : 0) && result.blocks.total == (success ? 1u : 0u),
            "Sonos uses Stop only when available and counts confirmed stops");
    if (!success) {
        std::this_thread::sleep_for(500ms);
        require(server.stops <= 2, "failed Sonos Stop is not retried for either stream");
    }
    monitor.stop();
    require(server.failure().empty(), server.failure());
}

// A stale selection must never stop or save a rule for replacement playback.
void stale_stop(const std::filesystem::path& dir) {
    FakeSonos server;
    server.next_available = false;
    humbug::Options options;
    options.discover_cast = false;
    options.devices = {server.base + "/device1"};
    options.listen_address = "127.0.0.1";
    options.blocklist = (dir / "stale-stop-rules").string();
    options.history_path = options.blocklist + ".history";
    humbug::Monitor monitor(options);
    monitor.start();
    const auto initial =
        wait(monitor, [](const auto& s) { return s.groups.size() == 2 && s.status == "Live UPnP events"; });
    const auto group = initial.groups[0];
    server.set_track("Replacement", 2);
    monitor.block({group.id, group.track.identity(), group.occurrence});
    wait(monitor, [](const auto& s) { return s.notice.find("cancelled") != std::string::npos; });
    require(server.stops == 0 && server.next == 0 && !std::filesystem::exists(options.blocklist),
            "stale Sonos block neither saves nor stops replacement playback");
    monitor.stop();
    require(server.failure().empty(), server.failure());
}

// Periodic checks recover missed events without confusing progress updates for new tracks.
void missed_track_update(const std::filesystem::path& dir) {
    FakeSonos server;
    server.creator_only = true;
    humbug::Options options;
    options.discover_cast = false;
    options.devices = {server.base + "/device1"};
    options.listen_address = "127.0.0.1";
    options.blocklist = (dir / "missed-track-rules").string();
    options.history_path = options.blocklist + ".history";
    humbug::Monitor monitor(options);
    monitor.start();
    const auto initial =
        wait(monitor, [](const auto& s) { return s.status == "Live UPnP events" && !s.groups.empty(); });
    require(initial.groups.front().track.artist == "Artist",
            "Sonos creator-only metadata supplies the artist");
    require(initial.groups.front().track.elapsed_seconds == 23 &&
                initial.groups.front().track.duration_seconds == 240,
            "Sonos position and duration reach the published snapshot");
    server.elapsed = 45;
    const auto progress = wait(monitor, [](const auto& s) {
        return !s.groups.empty() && s.groups.front().track.elapsed_seconds == 45;
    });
    require(progress.groups.front().track.identity() == initial.groups.front().track.identity() &&
                progress.groups.front().occurrence == initial.groups.front().occurrence,
            "position-only polls publish without changing track identity or occurrence");
    server.elapsed = -1;
    const auto unavailable = wait(
        monitor, [](const auto& s) { return !s.groups.empty() && !s.groups.front().track.elapsed_seconds; });
    require(unavailable.groups.front().track.duration_seconds == 240,
            "unsupported elapsed time clears stale position");
    server.set_track("Next song without a notification", 2);
    const auto updated = wait(
        monitor, [](const auto& s) { return !s.groups.empty() && s.groups.front().track.number == "2"; });
    const auto& group = updated.groups.front();
    require(group.track.title == "Next song without a notification" && group.track.artist == "Artist" &&
                group.track.blockable() && group.occurrence != initial.groups.front().occurrence,
            "transport check recovers the new title, artist and Block target without an event");
    monitor.block({group.id, group.track.identity(), group.occurrence});
    const auto result = wait(monitor, [](const auto& s) { return s.notice == "Block saved; skipped"; });
    require(server.next >= 1 && result.blocks.total >= 1, "recovered creator-only track can be blocked");
    monitor.stop();
    require(server.failure().empty(), server.failure());
}

int simulate() {
    FakeSonos server(0, true);
    std::cout << "Fake Sonos streams: Kitchen + Dining (1), Office (2)\n"
              << "Set HUMBUG_DEVICE=" << server.base << "/device1\n"
              << "Commands: track <1|2> <artist>|<title>, unsupported, supported, quit\n"
              << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit")
            break;
        if (!server.failure().empty())
            throw std::runtime_error(server.failure());
        if (line == "unsupported") {
            server.next_available = false;
            continue;
        }
        if (line == "supported") {
            server.next_available = true;
            continue;
        }
        std::istringstream input(line);
        std::string command, value;
        int which = 0;
        input >> command >> which;
        std::getline(input >> std::ws, value);
        const auto separator = value.find('|');
        if (command != "track" || (which != 1 && which != 2) || separator == std::string::npos ||
            separator == 0 || separator + 1 == value.size()) {
            std::cerr << "Use: track <1|2> <artist>|<title>\n";
            continue;
        }
        server.simulate_track(which, value.substr(separator + 1), value.substr(0, separator));
        std::cout << "Track updated for stream " << which << '\n' << std::flush;
    }
    if (!server.failure().empty())
        throw std::runtime_error(server.failure());
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--simulate") {
        try {
            return simulate();
        } catch (const std::exception& e) {
            std::cerr << "Simulator failure: " << e.what() << '\n';
            return 1;
        }
    }
    if (argc != 1) {
        std::cerr << "Usage: humbug_tests [--simulate]\n";
        return 2;
    }
    char temporary[] = "/tmp/humbug-tests-XXXXXX";
    const auto* name = mkdtemp(temporary);
    if (!name)
        return 1;
    const std::filesystem::path dir(name);
    try {
        socket_flags();
        protocol_tests();
        history_days(dir);
        repeat_blocked_song_stops_silently(dir);
        christmas_blocklist(dir);
        integration(dir, false, false);
        integration(dir, true, false);
        integration(dir, false, true);
        stop_fallback(dir, false, false, true, false);
        stop_fallback(dir, true, false, true, false);
        stop_fallback(dir, false, true, true, false);
        stop_fallback(dir, false, false, false, false);
        stop_fallback(dir, false, false, true, true);
        stale_stop(dir);
        missed_track_update(dir);
        recovery(dir);
        interactive_simulation(dir);
        auto_stop_limit(dir, true, true);
        auto_stop_limit(dir, true, false);
        auto_stop_limit(dir, false, true);
        auto_stop_reset_on_allowed(dir);
        coexistence(dir);
        std::filesystem::remove_all(dir);
        std::cout << "Protocol, persistence and fake-Sonos event integration tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << "Test failure: " << e.what() << '\n';
        std::filesystem::remove_all(dir);
        return 1;
    }
}
