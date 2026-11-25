#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>

namespace humbug {

class Socket {
public:
    explicit Socket(int fd = -1) : fd_(fd) {}
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int get() const {
        return fd_;
    }

private:
    int fd_;
};

Socket make_socket(int type, bool nonblocking = true);
Socket accept_socket(int listener, sockaddr* address, socklen_t* length, bool nonblocking = true);
#ifdef __APPLE__
inline constexpr int socket_send_flags = 0;
#else
inline constexpr int socket_send_flags = MSG_NOSIGNAL;
#endif

struct Url {
    std::string host, path;
    std::uint16_t port = 80;
};
Url parse_url(const std::string& value);
std::string resolve_url(const std::string& base, const std::string& path);

struct HttpMessage {
    std::string first, body;
    std::map<std::string, std::string> headers;
    int status() const;
};

HttpMessage request(const std::string& url, const std::string& method,
                    const std::map<std::string, std::string>& headers, const std::string& body,
                    const std::atomic_bool& stop,
                    std::chrono::milliseconds timeout = std::chrono::seconds(3));
std::vector<std::string> discover(const std::atomic_bool& stop);
std::string local_address(const std::string& remote);

struct Notification {
    std::string path, source;
    HttpMessage message;
};

class EventListener {
public:
    using Handler = std::function<int(Notification)>;
    EventListener(const std::string& address, std::uint16_t port, const std::atomic_bool& stop,
                  Handler handler);
    ~EventListener();
    std::uint16_t port() const {
        return port_;
    }
    bool failed() const {
        return failed_;
    }

private:
    void run();
    Socket socket_;
    const std::atomic_bool& stop_;
    std::atomic_bool done_{false};
    std::atomic_bool failed_{false};
    Handler handler_;
    std::thread thread_;
    std::uint16_t port_ = 0;
};

} // namespace humbug
