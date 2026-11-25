#include "cast.h"

#include "sonos/network.h"

#include <nlohmann/json.hpp>
#include <mdns.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

namespace humbug::cast {
namespace {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr auto connection_ns = "urn:x-cast:com.google.cast.tp.connection";
constexpr auto heartbeat_ns = "urn:x-cast:com.google.cast.tp.heartbeat";
constexpr auto receiver_ns = "urn:x-cast:com.google.cast.receiver";
constexpr auto media_ns = "urn:x-cast:com.google.cast.media";

std::optional<std::uint32_t> media_seconds(const json& value) {
    if (value.is_null())
        return std::nullopt;
    if (!value.is_number())
        throw std::runtime_error("Invalid Cast media time");
    const auto seconds = value.get<double>();
    if (!std::isfinite(seconds) || seconds < 0 || seconds > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Invalid Cast media time");
    return static_cast<std::uint32_t>(seconds);
}

#ifndef __APPLE__
class BlockSigpipe {
public:
    BlockSigpipe() {
        sigemptyset(&set_);
        sigaddset(&set_, SIGPIPE);
        sigset_t pending{};
        if (sigpending(&pending) != 0 || pthread_sigmask(SIG_BLOCK, &set_, &previous_) != 0)
            throw std::runtime_error("Cannot protect Cast connection from SIGPIPE");
        was_pending_ = sigismember(&pending, SIGPIPE) == 1;
    }
    ~BlockSigpipe() {
        if (!was_pending_) {
            timespec timeout{};
            sigtimedwait(&set_, nullptr, &timeout);
        }
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

private:
    sigset_t set_{}, previous_{};
    bool was_pending_ = false;
};
#endif

void varint(std::string& out, std::uint64_t n) {
    while (n >= 128) {
        out.push_back(static_cast<char>((n & 127) | 128));
        n >>= 7;
    }
    out.push_back(static_cast<char>(n));
}
void field(std::string& out, unsigned number, const std::string& value) {
    varint(out, number * 8 + 2);
    varint(out, value.size());
    out += value;
}
std::uint64_t read_varint(const std::string& in, std::size_t& pos) {
    std::uint64_t result = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (pos == in.size())
            throw std::runtime_error("Truncated Cast message");
        const auto b = static_cast<unsigned char>(in[pos++]);
        if (shift == 63 && b > 1)
            throw std::runtime_error("Invalid Cast varint");
        result |= std::uint64_t(b & 127) << shift;
        if (!(b & 128))
            return result;
    }
    throw std::runtime_error("Invalid Cast varint");
}
struct Message {
    std::string source, destination, name_space, payload;
};

struct DiscoveryState {
    struct Instance {
        std::string host, id, name;
        std::uint16_t port = 0;
    };
    std::set<std::string> instances;
    std::map<std::string, Instance> details;
    std::map<std::string, std::string> addresses;
};
std::string dns_name(mdns_string_t name) {
    std::string result(name.str ? name.str : "", name.length);
    for (auto& ch : result)
        if (ch >= 'A' && ch <= 'Z')
            ch += 'a' - 'A';
    return result;
}
int discovery_record(int, const sockaddr*, size_t, mdns_entry_type_t, uint16_t, uint16_t type,
                     uint16_t rclass, uint32_t ttl, const void* data, size_t size, size_t name_offset, size_t,
                     size_t offset, size_t length, void* user) {
    auto& state = *static_cast<DiscoveryState*>(user);
    if (!ttl || (rclass & 0x7fff) != MDNS_CLASS_IN || state.instances.size() > 256 ||
        state.details.size() > 256 || state.addresses.size() > 256)
        return 0;
    std::array<char, 1024> buffer{};
    const auto name = dns_name(mdns_string_extract(data, size, &name_offset, buffer.data(), buffer.size()));
    if (name.empty())
        return 0;
    if (type == MDNS_RECORDTYPE_PTR && name == "_googlecast._tcp.local.") {
        const auto instance =
            dns_name(mdns_record_parse_ptr(data, size, offset, length, buffer.data(), buffer.size()));
        if (!instance.empty())
            state.instances.insert(instance);
    } else if (type == MDNS_RECORDTYPE_SRV) {
        const auto srv = mdns_record_parse_srv(data, size, offset, length, buffer.data(), buffer.size());
        auto& item = state.details[name];
        item.host = dns_name(srv.name);
        item.port = srv.port;
    } else if (type == MDNS_RECORDTYPE_TXT) {
        std::array<mdns_record_txt_t, 64> records{};
        const auto count = mdns_record_parse_txt(data, size, offset, length, records.data(), records.size());
        auto& item = state.details[name];
        for (std::size_t i = 0; i < count; ++i) {
            const auto key = std::string(records[i].key.str, records[i].key.length);
            const auto value =
                std::string(records[i].value.str ? records[i].value.str : "", records[i].value.length);
            if (key == "id")
                item.id = value;
            else if (key == "fn")
                item.name = value;
        }
    } else if (type == MDNS_RECORDTYPE_A && length == 4) {
        sockaddr_in address{};
        mdns_record_parse_a(data, size, offset, length, &address);
        std::array<char, INET_ADDRSTRLEN> ip{};
        if (inet_ntop(AF_INET, &address.sin_addr, ip.data(), ip.size()))
            state.addresses[name] = ip.data();
    }
    return 0;
}
Message decode(const std::string& in) {
    Message result;
    bool version = false, type = false;
    std::size_t pos = 0;
    while (pos < in.size()) {
        const auto key = read_varint(in, pos);
        const auto tag = key >> 3, wire = key & 7;
        if (!tag)
            throw std::runtime_error("Invalid Cast field");
        if (wire == 0) {
            const auto v = read_varint(in, pos);
            if (tag == 1) {
                if (v != 0 || version)
                    throw std::runtime_error("Unsupported Cast version");
                version = true;
            }
            if (tag == 5) {
                if (v != 0 || type)
                    throw std::runtime_error("Unsupported Cast payload");
                type = true;
            }
        } else if (wire == 2) {
            const auto length = read_varint(in, pos);
            if (length > in.size() - pos)
                throw std::runtime_error("Truncated Cast field");
            auto value = in.substr(pos, static_cast<std::size_t>(length));
            pos += static_cast<std::size_t>(length);
            auto* target = tag == 2   ? &result.source
                           : tag == 3 ? &result.destination
                           : tag == 4 ? &result.name_space
                           : tag == 6 ? &result.payload
                                      : nullptr;
            if (target) {
                if (!target->empty())
                    throw std::runtime_error("Duplicate Cast field");
                *target = std::move(value);
            }
        } else if (wire == 1 || wire == 5) {
            const std::size_t count = wire == 1 ? 8 : 4;
            if (in.size() - pos < count)
                throw std::runtime_error("Truncated Cast field");
            pos += count;
        } else
            throw std::runtime_error("Unsupported Cast wire type");
    }
    if (!version || !type || result.source.empty() || result.destination.empty() ||
        result.name_space.empty() || result.payload.empty())
        throw std::runtime_error("Incomplete Cast message");
    return result;
}
std::string encode(const std::string& destination, const std::string& name_space, const json& payload) {
    std::string message;
    varint(message, 8);
    varint(message, 0);
    field(message, 2, "sender-0");
    field(message, 3, destination);
    field(message, 4, name_space);
    varint(message, 40);
    varint(message, 0);
    field(message, 6, payload.dump());
    if (message.size() > 65536)
        throw std::runtime_error("Cast message too large");
    const auto length = htonl(static_cast<std::uint32_t>(message.size()));
    return std::string(reinterpret_cast<const char*>(&length), sizeof(length)) + message;
}

class Connection {
public:
    Connection(const Endpoint& endpoint, const std::atomic_bool& stop)
        : stop_(stop), deadline_(Clock::now() + std::chrono::seconds(4)),
          socket_(make_socket(SOCK_STREAM)),
          context_(SSL_CTX_new(TLS_client_method()), SSL_CTX_free), ssl_(nullptr, SSL_free) {
        if (socket_.get() < 0 || !context_)
            throw std::runtime_error("Cannot create Cast connection");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(endpoint.port);
        if (inet_pton(AF_INET, endpoint.address.c_str(), &address.sin_addr) != 1 || !endpoint.port)
            throw std::runtime_error("Invalid Cast endpoint");
        if (::connect(socket_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 &&
            errno != EINPROGRESS)
            throw std::runtime_error("Cannot connect to Cast receiver");
        wait(POLLOUT);
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(socket_.get(), SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error)
            throw std::runtime_error("Cannot connect to Cast receiver");
        SSL_CTX_set_verify(context_.get(), SSL_VERIFY_NONE, nullptr);
        ssl_.reset(SSL_new(context_.get()));
        if (!ssl_ || SSL_set_fd(ssl_.get(), socket_.get()) != 1)
            throw std::runtime_error("Cannot initialize Cast TLS");
        for (;;) {
            const auto result = SSL_connect(ssl_.get());
            if (result == 1)
                break;
            ssl_wait(result);
        }
        send("receiver-0", connection_ns, {{"type", "CONNECT"}});
    }
    void send(const std::string& destination, const std::string& name_space, const json& value) {
        const auto data = encode(destination, name_space, value);
        std::size_t written = 0;
        while (written < data.size()) {
            check_deadline();
            const auto n =
                SSL_write(ssl_.get(), data.data() + written, static_cast<int>(data.size() - written));
            if (n > 0)
                written += static_cast<std::size_t>(n);
            else
                ssl_wait(n);
        }
    }
    json request(const std::string& destination, const std::string& name_space, json value,
                 const std::string& expected) {
        const auto id = ++request_id_;
        value["requestId"] = id;
        send(destination, name_space, value);
        for (;;) {
            auto message = receive();
            if (message.destination != "sender-0" && message.destination != "*")
                continue;
            json body;
            try {
                body = json::parse(message.payload);
            } catch (const json::exception&) {
                throw std::runtime_error("Malformed Cast JSON");
            }
            if (!body.is_object())
                throw std::runtime_error("Invalid Cast JSON message");
            const auto type = body.value("type", std::string{});
            if (message.name_space == heartbeat_ns && type == "PING") {
                send("receiver-0", heartbeat_ns, {{"type", "PONG"}});
                continue;
            }
            if (message.name_space != name_space || message.source != destination ||
                !body.contains("requestId") || !body["requestId"].is_number_integer() ||
                body["requestId"].get<std::int64_t>() != id)
                continue;
            if (type != expected) {
                if (type == "INVALID_REQUEST" || type == "INVALID_PLAYER_STATE" || type == "LOAD_FAILED" ||
                    type == "LOAD_CANCELLED")
                    throw std::runtime_error("Cast receiver rejected request: " + type);
                throw std::runtime_error("Unexpected Cast receiver response");
            }
            return body;
        }
    }
    Media status(const Media* previous = nullptr) {
        const auto receiver = request("receiver-0", receiver_ns, {{"type", "GET_STATUS"}}, "RECEIVER_STATUS");
        Media media;
        if (!receiver.contains("status") || !receiver["status"].is_object())
            throw std::runtime_error("Invalid Cast receiver status");
        const auto& apps = receiver["status"].value("applications", json::array());
        if (!apps.is_array())
            throw std::runtime_error("Invalid Cast applications");
        for (const auto& app : apps) {
            if (!app.is_object() || !app.contains("namespaces") || !app["namespaces"].is_array())
                continue;
            for (const auto& ns : app["namespaces"]) {
                if (ns.is_object() && ns.value("name", std::string{}) == media_ns) {
                    media.transport = app.value("transportId", std::string{});
                    media.application = app.value("sessionId", std::string{});
                    break;
                }
            }
            if (!media.transport.empty())
                break;
        }
        if (media.transport.empty() || media.application.empty())
            return media;
        if (previous && previous->application == media.application && previous->transport == media.transport)
            media = *previous;
        send(media.transport, connection_ns, {{"type", "CONNECT"}});
        const auto response = request(media.transport, media_ns, {{"type", "GET_STATUS"}}, "MEDIA_STATUS");
        update(media, response);
        return media;
    }
    void advance(const Media& expected) {
        auto current = status(&expected);
        if (current.application != expected.application || current.transport != expected.transport ||
            current.session != expected.session || current.item != expected.item ||
            current.state != "PLAYING" || current.track.identity() != expected.track.identity() ||
            !current.can_skip)
            throw std::runtime_error("Cast track or session changed; skip cancelled");
        advance_current(current);
    }
    bool block(const Media& expected, bool force_stop) {
        auto current = status(&expected);
        if (current.application != expected.application || current.transport != expected.transport ||
            current.session <= 0 || current.session != expected.session || current.item != expected.item ||
            current.state != "PLAYING" || current.track.identity() != expected.track.identity())
            throw std::runtime_error("Cast track or session changed; block cancelled");
        if (current.can_skip && !force_stop) {
            advance_current(current);
            return false;
        }
        const auto response =
            request(current.transport, media_ns, {{"type", "STOP"}, {"mediaSessionId", current.session}},
                    "MEDIA_STATUS");
        if (!response.contains("status") || !response["status"].is_array())
            throw std::runtime_error("Cast stop was not confirmed; it will not be retried");
        const auto& statuses = response["status"];
        if (!statuses.empty() && (statuses.size() != 1 || !statuses[0].is_object() ||
                                  statuses[0].value("mediaSessionId", std::int64_t{0}) != current.session ||
                                  statuses[0].value("playerState", std::string{}) != "IDLE"))
            throw std::runtime_error("Cast stop was not confirmed; it will not be retried");
        return true;
    }

private:
    void advance_current(const Media& current) {
        const auto response = request(
            current.transport, media_ns,
            {{"type", "QUEUE_UPDATE"}, {"mediaSessionId", current.session}, {"jump", 1}}, "MEDIA_STATUS");
        if (confirms_advance(current, response))
            return;

        const auto confirmation_deadline = Clock::now() + std::chrono::seconds(2);
        while (Clock::now() < confirmation_deadline && Clock::now() < deadline_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (advanced(current, status(&current)))
                return;
        }
        throw std::runtime_error("Cast skip was not confirmed; it will not be retried");
    }
    static bool confirms_advance(const Media& current, const json& response) {
        if (!response.contains("status") || !response["status"].is_array() || response["status"].empty())
            return false;
        if (!response["status"][0].is_object())
            throw std::runtime_error("Invalid Cast media status");
        auto changed = current;
        update(changed, response);
        return advanced(current, changed);
    }
    static bool advanced(const Media& current, const Media& changed) {
        return (changed.session > 0 && changed.session != current.session) ||
               (changed.item > 0 && changed.item != current.item) ||
               (changed.track.blockable() && changed.track.identity() != current.track.identity());
    }
    void check_deadline() const {
        if (stop_)
            throw std::runtime_error("Cast connection cancelled");
        if (Clock::now() >= deadline_)
            throw std::runtime_error("Cast connection timed out");
    }
    void wait(short events) {
        while (!stop_) {
            check_deadline();
            pollfd fd{socket_.get(), events, 0};
            const auto result = poll(&fd, 1, 50);
            if (result > 0)
                return;
            if (result < 0 && errno != EINTR)
                throw std::runtime_error("Cast socket poll failed");
        }
        throw std::runtime_error("Cast connection cancelled");
    }
    void ssl_wait(int result) {
        const auto error = SSL_get_error(ssl_.get(), result);
        if (error == SSL_ERROR_WANT_READ)
            wait(POLLIN);
        else if (error == SSL_ERROR_WANT_WRITE)
            wait(POLLOUT);
        else
            throw std::runtime_error("Cast TLS connection failed");
    }
    void read_exact(char* buffer, std::size_t size) {
        while (size) {
            check_deadline();
            const auto n = SSL_read(ssl_.get(), buffer, static_cast<int>(size));
            if (n > 0) {
                buffer += n;
                size -= static_cast<std::size_t>(n);
            } else
                ssl_wait(n);
        }
    }
    Message receive() {
        std::uint32_t size = 0;
        read_exact(reinterpret_cast<char*>(&size), sizeof(size));
        size = ntohl(size);
        if (!size || size > 65536)
            throw std::runtime_error("Invalid Cast message size");
        std::string data(size, '\0');
        read_exact(data.data(), data.size());
        return decode(data);
    }
    static void update(Media& media, const json& response) {
        if (!response.contains("status") || !response["status"].is_array())
            throw std::runtime_error("Invalid Cast media status");
        const auto& statuses = response["status"];
        if (statuses.empty()) {
            media.session = 0;
            media.item = 0;
            media.state = "IDLE";
            media.track = {};
            media.can_skip = false;
            return;
        }
        const auto& status = statuses[0];
        if (!status.is_object())
            throw std::runtime_error("Invalid Cast media status");
        if (!status.contains("mediaSessionId"))
            throw std::runtime_error("Cast media status has no session ID");
        const auto session = status.value("mediaSessionId", media.session);
        if (session != media.session) {
            media.track = {};
            media.item = 0;
            media.can_skip = false;
        }
        media.session = session;
        const auto item = status.value("currentItemId", media.item);
        if ((status.contains("currentItemId") && item != media.item) ||
            (!status.contains("currentItemId") && !status.contains("media")))
            media.track = {};
        media.item = item;
        media.state = status.value("playerState", media.state);
        if (status.contains("supportedMediaCommands"))
            media.can_skip = (status.at("supportedMediaCommands").get<std::int64_t>() & 64) != 0;
        if (status.contains("media") && status["media"].is_object()) {
            const auto& info = status["media"];
            const auto uri = info.value("contentId", media.track.uri);
            if (uri != media.track.uri)
                media.track = {};
            media.track.uri = uri;
            media.track.number = std::to_string(media.item);
            if (info.contains("duration"))
                media.track.duration_seconds = media_seconds(info["duration"]);
            if (info.contains("metadata") && info["metadata"].is_object()) {
                const auto& metadata = info["metadata"];
                media.track.title = metadata.value("title", std::string{});
                media.track.artist = metadata.value("artist", std::string{});
                media.track.album = metadata.value("albumName", std::string{});
            }
        }
        media.track.elapsed_seconds =
            status.contains("currentTime") ? media_seconds(status["currentTime"]) : std::nullopt;
    }
#ifndef __APPLE__
    BlockSigpipe sigpipe_;
#endif
    const std::atomic_bool& stop_;
    Clock::time_point deadline_;
    Socket socket_;
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context_;
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl_;
    std::int64_t request_id_ = 0;
};
} // namespace

std::vector<Endpoint> discover(const std::atomic_bool& stop, const std::string& interface) {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    if (!interface.empty() && inet_pton(AF_INET, interface.c_str(), &local.sin_addr) != 1)
        throw std::runtime_error("Cast interface must be a numeric IPv4 address");
    Socket socket(mdns_socket_open_ipv4(interface.empty() ? nullptr : &local));
    if (socket.get() < 0)
        throw std::runtime_error("Cannot open Cast mDNS socket");
    alignas(4) std::array<char, 65536> buffer{};
    constexpr auto service = "_googlecast._tcp.local.";
    if (mdns_query_send(socket.get(), MDNS_RECORDTYPE_PTR, service, std::strlen(service), buffer.data(),
                        buffer.size(), 0) < 0)
        throw std::runtime_error("Cannot send Cast mDNS query");
    DiscoveryState state;
    std::set<std::string> queried_instances, queried_hosts;
    const auto end = Clock::now() + std::chrono::milliseconds(1500);
    while (!stop && Clock::now() < end) {
        pollfd fd{socket.get(), POLLIN, 0};
        const auto result = poll(&fd, 1, 50);
        if (result < 0 && errno != EINTR)
            throw std::runtime_error("Cast mDNS poll failed");
        if (result > 0)
            mdns_query_recv(socket.get(), buffer.data(), buffer.size(), discovery_record, &state, 0);
        for (const auto& instance : state.instances) {
            if (!queried_instances.insert(instance).second)
                continue;
            if (mdns_query_send(socket.get(), MDNS_RECORDTYPE_ANY, instance.c_str(), instance.size(),
                                buffer.data(), buffer.size(), 0) < 0)
                throw std::runtime_error("Cannot resolve Cast service");
        }
        for (const auto& [name, item] : state.details) {
            if (item.host.empty() || !queried_hosts.insert(item.host).second)
                continue;
            if (mdns_query_send(socket.get(), MDNS_RECORDTYPE_A, item.host.c_str(), item.host.size(),
                                buffer.data(), buffer.size(), 0) < 0)
                throw std::runtime_error("Cannot resolve Cast address");
        }
    }
    std::vector<Endpoint> result;
    for (const auto& instance : state.instances) {
        const auto it = state.details.find(instance);
        if (it == state.details.end() || it->second.host.empty() || !it->second.port)
            continue;
        const auto address = state.addresses.find(it->second.host);
        if (address == state.addresses.end())
            continue;
        result.push_back({it->second.id.empty() ? instance : it->second.id,
                          it->second.name.empty() ? instance : it->second.name, address->second,
                          it->second.port});
    }
    return result;
}

Media read(const Endpoint& endpoint, const std::atomic_bool& stop, const Media* previous) {
    try {
        return Connection(endpoint, stop).status(previous);
    } catch (const json::exception&) {
        throw std::runtime_error("Invalid Cast status fields");
    }
}
void next(const Endpoint& endpoint, const Media& expected, const std::atomic_bool& stop) {
    try {
        Connection(endpoint, stop).advance(expected);
    } catch (const json::exception&) {
        throw std::runtime_error("Invalid Cast status fields");
    }
}
bool block(const Endpoint& endpoint, const Media& expected, const std::atomic_bool& stop, bool force_stop) {
    try {
        return Connection(endpoint, stop).block(expected, force_stop);
    } catch (const json::exception&) {
        throw std::runtime_error("Invalid Cast status fields");
    }
}

} // namespace humbug::cast
