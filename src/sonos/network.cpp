#include "network.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace humbug {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxBody = 1024 * 1024;
constexpr std::size_t kMaxHeaders = 16384;

std::string lower(std::string s) {
    for (auto& ch : s)
        if (ch >= 'A' && ch <= 'Z')
            ch += 'a' - 'A';
    return s;
}

unsigned long number(std::string_view s, int base = 10) {
    unsigned long value{};
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (error != std::errc{} || end != s.data() + s.size() || s.empty())
        throw std::runtime_error("Invalid HTTP number");
    return value;
}

sockaddr_in address(const std::string& host, std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1)
        throw std::runtime_error("Expected a numeric IPv4 LAN address: " + host);
    return a;
}

void wait_fd(int fd, short events, Clock::time_point deadline, const std::atomic_bool& stop) {
    while (!stop) {
        if (Clock::now() >= deadline)
            throw std::runtime_error("Network operation timed out");
        pollfd p{fd, events, 0};
        const auto result = poll(&p, 1, 50);
        if (result > 0)
            return;
        if (result < 0 && errno != EINTR)
            throw std::runtime_error("Socket poll failed");
    }
    throw std::runtime_error("Network operation cancelled");
}

void send_all(int fd, const std::string& data, Clock::time_point deadline, const std::atomic_bool& stop) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        wait_fd(fd, POLLOUT, deadline, stop);
        const auto n = send(fd, data.data() + sent, data.size() - sent, socket_send_flags);
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (n <= 0)
            throw std::runtime_error("HTTP send failed");
        sent += static_cast<std::size_t>(n);
    }
}

HttpMessage parse_headers(const std::string& data) {
    HttpMessage m;
    std::istringstream input(data);
    std::getline(input, m.first);
    if (!m.first.empty() && m.first.back() == '\r')
        m.first.pop_back();
    std::string line;
    while (std::getline(input, line)) {
        if (line == "\r" || line.empty())
            break;
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            throw std::runtime_error("Malformed HTTP header");
        auto value = line.substr(colon + 1);
        const auto first = value.find_first_not_of(" \t");
        value.erase(0, first == std::string::npos ? value.size() : first);
        const auto last = value.find_last_not_of(" \t\r");
        value.erase(last == std::string::npos ? 0 : last + 1);
        if (!m.headers.emplace(lower(line.substr(0, colon)), value).second)
            throw std::runtime_error("Duplicate HTTP header");
    }
    return m;
}

std::optional<std::string> chunks(std::string_view data) {
    std::string result;
    std::size_t pos = 0;
    for (;;) {
        const auto eol = data.find("\r\n", pos);
        if (eol == std::string::npos)
            return std::nullopt;
        auto size_text = data.substr(pos, eol - pos);
        size_text = size_text.substr(0, size_text.find(';'));
        const auto size = number(size_text, 16);
        if (size > kMaxBody - result.size())
            throw std::runtime_error("HTTP body too large");
        pos = eol + 2;
        if (size == 0) {
            if (data.size() < pos + 2)
                return std::nullopt;
            if (data.compare(pos, 2, "\r\n") == 0)
                return result;
            const auto trailers = data.find("\r\n\r\n", pos);
            if (trailers != std::string::npos)
                return result;
            return std::nullopt;
        }
        if (data.size() - pos < size + 2)
            return std::nullopt;
        if (data.compare(pos + size, 2, "\r\n") != 0)
            throw std::runtime_error("Malformed HTTP chunk");
        result.append(data.substr(pos, size));
        pos += size + 2;
    }
}

HttpMessage receive(int fd, Clock::time_point deadline, const std::atomic_bool& stop, bool incoming = false) {
    std::string data;
    std::optional<HttpMessage> message;
    std::size_t start = 0;
    for (;;) {
        if (message) {
            const auto& headers = message->headers;
            const auto body = std::string_view(data).substr(start);
            if (const auto it = headers.find("transfer-encoding"); it != headers.end()) {
                if (lower(it->second) != "chunked" || headers.contains("content-length"))
                    throw std::runtime_error("Unsupported HTTP framing");
                if (auto decoded = chunks(body)) {
                    message->body = std::move(*decoded);
                    return *message;
                }
            } else if (const auto it = headers.find("content-length"); it != headers.end()) {
                const auto size = number(it->second);
                if (size > kMaxBody)
                    throw std::runtime_error("HTTP body too large");
                if (body.size() >= size) {
                    message->body = body.substr(0, size);
                    return *message;
                }
            } else if (incoming) {
                throw std::runtime_error("NOTIFY requires Content-Length");
            }
        }
        wait_fd(fd, POLLIN, deadline, stop);
        std::array<char, 8192> buffer{};
        const auto n = recv(fd, buffer.data(), buffer.size(), 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (n < 0)
            throw std::runtime_error("HTTP receive failed");
        if (n == 0) {
            if (!message || message->headers.contains("content-length") ||
                message->headers.contains("transfer-encoding"))
                throw std::runtime_error("Truncated HTTP response");
            message->body = data.substr(start);
            return *message;
        }
        data.append(buffer.data(), static_cast<std::size_t>(n));
        if (data.size() > kMaxBody + kMaxHeaders)
            throw std::runtime_error("HTTP message too large");
        if (!message) {
            const auto end = data.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (data.size() > kMaxHeaders)
                    throw std::runtime_error("HTTP headers too large");
            } else {
                if (end > kMaxHeaders)
                    throw std::runtime_error("HTTP headers too large");
                message = parse_headers(data.substr(0, end + 2));
                start = end + 4;
            }
        }
    }
}
} // namespace

Socket::~Socket() {
    if (fd_ >= 0)
        close(fd_);
}
Socket::Socket(Socket&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
Socket& Socket::operator=(Socket&& o) noexcept {
    if (this != &o) {
        if (fd_ >= 0)
            close(fd_);
        fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
}

#ifdef __APPLE__
namespace {
void configure_socket(int fd, bool nonblocking, bool stream) {
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        throw std::runtime_error("Cannot set close-on-exec on socket: " + std::string(std::strerror(errno)));
    if (nonblocking) {
        const auto flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            throw std::runtime_error("Cannot set nonblocking socket mode: " + std::string(std::strerror(errno)));
    }
    if (stream) {
        const int enabled = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
            throw std::runtime_error("Cannot suppress socket SIGPIPE: " + std::string(std::strerror(errno)));
    }
}
} // namespace
#endif

Socket make_socket(int type, bool nonblocking) {
#ifdef __APPLE__
    Socket socket(::socket(AF_INET, type, 0));
#else
    Socket socket(::socket(AF_INET, type | SOCK_CLOEXEC | (nonblocking ? SOCK_NONBLOCK : 0), 0));
#endif
    if (socket.get() < 0)
        throw std::runtime_error("Cannot create network socket: " + std::string(std::strerror(errno)));
#ifdef __APPLE__
    configure_socket(socket.get(), nonblocking, type == SOCK_STREAM);
#endif
    return socket;
}

Socket accept_socket(int listener, sockaddr* address, socklen_t* length, bool nonblocking) {
#ifdef __APPLE__
    Socket socket(::accept(listener, address, length));
    if (socket.get() >= 0)
        configure_socket(socket.get(), nonblocking, true);
#else
    Socket socket(::accept4(listener, address, length, SOCK_CLOEXEC | (nonblocking ? SOCK_NONBLOCK : 0)));
#endif
    return socket;
}

Url parse_url(const std::string& value) {
    if (!value.starts_with("http://") || value.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("Expected an HTTP LAN URL");
    const auto slash = value.find('/', 7);
    auto host = value.substr(7, slash == std::string::npos ? slash : slash - 7);
    Url url;
    url.path = slash == std::string::npos ? "/" : value.substr(slash);
    const auto colon = host.find(':');
    if (colon != std::string::npos) {
        const auto port = number(host.substr(colon + 1));
        if (port == 0 || port > 65535)
            throw std::runtime_error("Invalid URL port");
        url.port = static_cast<std::uint16_t>(port);
        host.resize(colon);
    }
    address(host, url.port);
    url.host = host;
    return url;
}

std::string resolve_url(const std::string& base, const std::string& path) {
    if (path.starts_with("http://")) {
        parse_url(path);
        return path;
    }
    const auto u = parse_url(base);
    return "http://" + u.host + ":" + std::to_string(u.port) +
           (path.starts_with('/') ? path : u.path.substr(0, u.path.rfind('/') + 1) + path);
}

int HttpMessage::status() const {
    std::istringstream input(first);
    std::string protocol;
    int code{};
    if (!(input >> protocol >> code) || !protocol.starts_with("HTTP/") || code < 100 || code > 599)
        throw std::runtime_error("Invalid HTTP status");
    return code;
}

HttpMessage request(const std::string& value, const std::string& method,
                    const std::map<std::string, std::string>& headers, const std::string& body,
                    const std::atomic_bool& stop, std::chrono::milliseconds timeout) {
    const auto url = parse_url(value);
    auto socket = make_socket(SOCK_STREAM);
    const auto a = address(url.host, url.port);
    const auto deadline = Clock::now() + timeout;
    if (connect(socket.get(), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) < 0) {
        if (errno != EINPROGRESS)
            throw std::runtime_error("Cannot connect to " + url.host);
        wait_fd(socket.get(), POLLOUT, deadline, stop);
        int error{};
        socklen_t size = sizeof(error);
        if (getsockopt(socket.get(), SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error)
            throw std::runtime_error("Cannot connect to " + url.host);
    }
    std::string wire = method + " " + url.path + " HTTP/1.1\r\nHost: " + url.host + ":" +
                       std::to_string(url.port) + "\r\nConnection: close\r\n";
    for (const auto& [key, val] : headers) {
        if ((key + val).find_first_of("\r\n") != std::string::npos)
            throw std::runtime_error("Invalid outgoing HTTP header");
        wire += key + ": " + val + "\r\n";
    }
    wire += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    send_all(socket.get(), wire, deadline, stop);
    return receive(socket.get(), deadline, stop);
}

std::vector<std::string> discover(const std::atomic_bool& stop) {
    auto socket = make_socket(SOCK_DGRAM);
    const auto a = address("239.255.255.250", 1900);
    const std::string query = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                              "MAN: \"ssdp:discover\"\r\nMX: 1\r\n"
                              "ST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
    if (sendto(socket.get(), query.data(), query.size(), 0, reinterpret_cast<const sockaddr*>(&a),
               sizeof(a)) < 0)
        throw std::runtime_error("SSDP send failed");
    std::set<std::string> urls;
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    while (!stop && Clock::now() < deadline) {
        pollfd p{socket.get(), POLLIN, 0};
        const int ready = poll(&p, 1, 50);
        if (ready < 0 && errno != EINTR)
            throw std::runtime_error("SSDP poll failed");
        if (ready <= 0)
            continue;
        std::array<char, 8192> buf{};
        const auto n = recv(socket.get(), buf.data(), buf.size(), 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (n < 0)
            throw std::runtime_error("SSDP read failed");
        try {
            auto m = parse_headers(std::string(buf.data(), static_cast<std::size_t>(n)));
            if (auto it = m.headers.find("location"); it != m.headers.end())
                urls.insert(it->second);
        } catch (const std::exception& e) {
            std::cerr << "Ignoring malformed SSDP reply: " << e.what() << '\n';
        }
    }
    return {urls.begin(), urls.end()};
}

std::string local_address(const std::string& remote) {
    auto socket = make_socket(SOCK_DGRAM);
    const auto a = address(parse_url(remote).host, 1400);
    if (connect(socket.get(), reinterpret_cast<const sockaddr*>(&a), sizeof(a)) < 0)
        throw std::runtime_error("Cannot determine LAN callback address");
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (getsockname(socket.get(), reinterpret_cast<sockaddr*>(&local), &len) < 0)
        throw std::runtime_error("Cannot read LAN callback address");
    std::array<char, INET_ADDRSTRLEN> text{};
    if (!inet_ntop(AF_INET, &local.sin_addr, text.data(), text.size()))
        throw std::runtime_error("Cannot format callback address");
    return text.data();
}

EventListener::EventListener(const std::string& host, std::uint16_t port, const std::atomic_bool& stop,
                             Handler handler)
    : socket_(make_socket(SOCK_STREAM)), stop_(stop), handler_(std::move(handler)) {
    auto a = address(host.empty() ? "0.0.0.0" : host, port);
    if (bind(socket_.get(), reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 || listen(socket_.get(), 16) < 0)
        throw std::runtime_error("Cannot bind Sonos callback listener");
    socklen_t len = sizeof(a);
    if (getsockname(socket_.get(), reinterpret_cast<sockaddr*>(&a), &len) < 0)
        throw std::runtime_error("Cannot read callback port");
    port_ = ntohs(a.sin_port);
    thread_ = std::thread([this] { run(); });
}
EventListener::~EventListener() {
    done_ = true;
    if (thread_.joinable())
        thread_.join();
}
void EventListener::run() {
    while (!stop_ && !done_) {
        pollfd p{socket_.get(), POLLIN, 0};
        const auto ready = poll(&p, 1, 50);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (ready > 0 && !(p.revents & POLLIN))) {
            std::cerr << "Sonos callback listener poll failed\n";
            failed_ = true;
            return;
        }
        if (!ready)
            continue;
        sockaddr_in remote{};
        socklen_t size = sizeof(remote);
        Socket client;
        try {
            client = accept_socket(socket_.get(), reinterpret_cast<sockaddr*>(&remote), &size);
        } catch (const std::exception& e) {
            std::cerr << "Sonos callback listener accept failed: " << e.what() << '\n';
            failed_ = true;
            return;
        }
        if (client.get() < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            std::cerr << "Sonos callback listener accept failed: " << std::strerror(errno) << '\n';
            failed_ = true;
            return;
        }
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        int status = 400;
        try {
            auto message = receive(client.get(), deadline, stop_, true);
            std::istringstream line(message.first);
            std::string method, path, version;
            line >> method >> path >> version;
            if (method == "NOTIFY" && version == "HTTP/1.1") {
                std::array<char, INET_ADDRSTRLEN> source{};
                inet_ntop(AF_INET, &remote.sin_addr, source.data(), source.size());
                status = handler_({path, source.data(), std::move(message)});
            } else {
                status = 405;
            }
        } catch (const std::exception& e) {
            if (!stop_)
                std::cerr << "Rejected Sonos callback: " << e.what() << '\n';
        }
        try {
            send_all(client.get(),
                     "HTTP/1.1 " + std::to_string(status) +
                         " Result\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                     deadline, stop_);
        } catch (const std::exception& e) {
            if (!stop_)
                std::cerr << "Cannot acknowledge Sonos callback: " << e.what() << '\n';
        }
    }
}
} // namespace humbug
