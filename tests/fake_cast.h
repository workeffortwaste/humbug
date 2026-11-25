#pragma once

#include "sonos/network.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

class FakeCast {
    using Json = nlohmann::json;

public:
    enum class Next { Advance, DelayedAdvance, Reject, LostResponse, LostWithoutAdvance, Unchanged };
    enum class Stop { Confirm, Reject, LostResponse, Unchanged };
    FakeCast()
        : context_(SSL_CTX_new(TLS_server_method()), SSL_CTX_free),
          socket_(humbug::make_socket(SOCK_STREAM, false)) {
        if (!context_ || socket_.get() < 0)
            throw std::runtime_error("Cannot initialize fake Cast");
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
            EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY* key = nullptr;
        if (!generator || EVP_PKEY_keygen_init(generator.get()) <= 0 ||
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(generator.get(), NID_X9_62_prime256v1) <= 0 ||
            EVP_PKEY_keygen(generator.get(), &key) <= 0)
            throw std::runtime_error("Cannot generate fake Cast TLS key");
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> owned_key(key, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
        if (!cert || X509_set_version(cert.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1 ||
            !X509_gmtime_adj(X509_getm_notBefore(cert.get()), -60) ||
            !X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) || X509_set_pubkey(cert.get(), key) != 1)
            throw std::runtime_error("Cannot create fake Cast certificate");
        auto* name = X509_get_subject_name(cert.get());
        if (!name ||
            X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>("Fake Cast"), -1, -1, 0) != 1 ||
            X509_set_issuer_name(cert.get(), name) != 1 || X509_sign(cert.get(), key, EVP_sha256()) <= 0 ||
            SSL_CTX_use_certificate(context_.get(), cert.get()) != 1 ||
            SSL_CTX_use_PrivateKey(context_.get(), key) != 1)
            throw std::runtime_error("Cannot configure fake Cast TLS certificate");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(socket_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(socket_.get(), 16) != 0)
            throw std::runtime_error("Cannot listen for fake Cast");
        socklen_t length = sizeof(address);
        if (getsockname(socket_.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0)
            throw std::runtime_error("Cannot inspect fake Cast socket");
        endpoint = "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        thread_ = std::thread([this] { serve(); });
    }
    ~FakeCast() {
        done_ = true;
        if (thread_.joinable())
            thread_.join();
    }
    std::string endpoint;
    std::atomic_int next_count{0}, stop_count{0};
    std::atomic_bool supported{true};
    std::atomic_bool omit_media{false};
    std::atomic_bool omit_time{false};
    std::atomic_bool repeat_next{false};
    std::atomic_bool vary_next_titles{false};
    std::atomic<Next> behavior{Next::Advance};
    std::atomic<Stop> stop_behavior{Stop::Confirm};
    void song(const std::string& title, const std::string& artist = "Artist") {
        std::lock_guard lock(mutex_);
        title_ = title;
        artist_ = artist;
        ++item_;
        stopped_ = false;
    }
    void song_at(int item, const std::string& title, const std::string& artist = "Artist") {
        std::lock_guard lock(mutex_);
        title_ = title;
        artist_ = artist;
        item_ = item;
        stopped_ = false;
    }
    void resume() {
        std::lock_guard lock(mutex_);
        stopped_ = false;
    }
    std::string failure() {
        std::lock_guard lock(mutex_);
        return failure_;
    }

private:
    static void varint(std::string& out, std::uint64_t value) {
        while (value >= 128) {
            out.push_back(static_cast<char>((value & 127) | 128));
            value >>= 7;
        }
        out.push_back(static_cast<char>(value));
    }
    static std::uint64_t read_varint(const std::string& data, std::size_t& pos) {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64 && pos < data.size(); shift += 7) {
            const auto byte = static_cast<unsigned char>(data[pos++]);
            value |= std::uint64_t(byte & 127) << shift;
            if (!(byte & 128))
                return value;
        }
        throw std::runtime_error("Invalid fake Cast message");
    }
    static void field(std::string& out, unsigned tag, const std::string& value) {
        varint(out, tag * 8 + 2);
        varint(out, value.size());
        out += value;
    }
    static bool receive(SSL* ssl, std::string& data, std::size_t size) {
        data.resize(size);
        for (std::size_t pos = 0; pos < size;) {
            const int count = SSL_read(ssl, data.data() + pos, static_cast<int>(size - pos));
            if (count <= 0)
                return false;
            pos += static_cast<std::size_t>(count);
        }
        return true;
    }
    static bool send(SSL* ssl, const std::string& source, const std::string& name_space,
                     const Json& payload) {
        std::string data;
        varint(data, 8);
        varint(data, 0);
        field(data, 2, source);
        field(data, 3, "sender-0");
        field(data, 4, name_space);
        varint(data, 40);
        varint(data, 0);
        field(data, 6, payload.dump());
        const auto length = htonl(static_cast<std::uint32_t>(data.size()));
        data.insert(0, reinterpret_cast<const char*>(&length), sizeof(length));
        for (std::size_t pos = 0; pos < data.size();) {
            const int count = SSL_write(ssl, data.data() + pos, static_cast<int>(data.size() - pos));
            if (count <= 0)
                return false;
            pos += static_cast<std::size_t>(count);
        }
        return true;
    }
    Json status(std::int64_t request, bool resolve_delayed_advance = true) {
        std::lock_guard lock(mutex_);
        if (resolve_delayed_advance && delayed_advance_) {
            if (delayed_status_checks_ > 0) {
                --delayed_status_checks_;
            } else {
                ++item_;
                title_ = "Next track";
                delayed_advance_ = false;
            }
        }
        if (stopped_)
            return {{"type", "MEDIA_STATUS"}, {"requestId", request}, {"status", Json::array()}};
        Json response = {
            {"type", "MEDIA_STATUS"},
            {"requestId", request},
            {"status",
             Json::array(
                 {{{"mediaSessionId", 1},
                   {"currentItemId", item_},
                   {"playerState", "PLAYING"},
                   {"currentTime", 23.8},
                   {"supportedMediaCommands", supported ? 64 : 0},
                   {"media",
                    {{"contentId", "track:" + std::to_string(item_)},
                     {"duration", 240.5},
                     {"metadata", {{"title", title_}, {"artist", artist_}, {"albumName", "Album"}}}}}}})}};
        if (omit_media)
            response["status"][0].erase("media");
        if (omit_time)
            response["status"][0].erase("currentTime");
        return response;
    }
    void connection(int fd) {
        timeval timeout{2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(context_.get()), SSL_free);
        if (!ssl || SSL_set_fd(ssl.get(), fd) != 1 || SSL_accept(ssl.get()) != 1)
            return;
        while (!done_) {
            std::string bytes;
            if (!receive(ssl.get(), bytes, 4))
                return;
            std::uint32_t length;
            std::memcpy(&length, bytes.data(), 4);
            length = ntohl(length);
            if (!length || length > 65536 || !receive(ssl.get(), bytes, length))
                throw std::runtime_error("Invalid fake Cast frame");
            std::size_t pos = 0;
            std::string destination, name_space, payload;
            while (pos < bytes.size()) {
                const auto key = read_varint(bytes, pos);
                if ((key & 7) == 0) {
                    read_varint(bytes, pos);
                    continue;
                }
                if ((key & 7) != 2)
                    throw std::runtime_error("Invalid fake Cast field");
                const auto size = read_varint(bytes, pos);
                if (size > bytes.size() - pos)
                    throw std::runtime_error("Truncated fake Cast field");
                const auto value = bytes.substr(pos, static_cast<std::size_t>(size));
                pos += static_cast<std::size_t>(size);
                if ((key >> 3) == 3)
                    destination = value;
                if ((key >> 3) == 4)
                    name_space = value;
                if ((key >> 3) == 6)
                    payload = value;
            }
            const auto message = Json::parse(payload);
            const auto type = message.at("type").get<std::string>();
            if (type == "CONNECT" || type == "PONG")
                continue;
            const auto request = message.at("requestId").get<std::int64_t>();
            if (type == "GET_STATUS" && destination == "receiver-0") {
                const Json response = {
                    {"type", "RECEIVER_STATUS"},
                    {"requestId", request},
                    {"status",
                     {{"applications",
                       Json::array({{{"sessionId", "session"},
                                     {"transportId", "app-1"},
                                     {"namespaces",
                                      Json::array({{{"name", "urn:x-cast:com.google.cast.media"}}})}}})}}}};
                if (!send(ssl.get(), destination, name_space, response))
                    return;
            } else if (type == "GET_STATUS" && destination == "app-1") {
                if (!send(ssl.get(), destination, name_space, status(request)))
                    return;
            } else if (type == "QUEUE_UPDATE" && destination == "app-1") {
                if (message.value("jump", 0) != 1 || message.value("mediaSessionId", 0) != 1)
                    throw std::runtime_error("Invalid fake Cast Next command");
                ++next_count;
                if (behavior == Next::Reject || !supported) {
                    if (!send(ssl.get(), destination, name_space,
                              {{"type", "INVALID_REQUEST"}, {"requestId", request}}))
                        return;
                } else {
                    if (behavior == Next::DelayedAdvance) {
                        std::lock_guard lock(mutex_);
                        delayed_advance_ = true;
                        delayed_status_checks_ = 1;
                    } else if (behavior != Next::Unchanged && behavior != Next::LostWithoutAdvance) {
                        if (repeat_next) {
                            std::lock_guard lock(mutex_);
                            ++item_;
                            stopped_ = false;
                        } else if (vary_next_titles) {
                            std::lock_guard lock(mutex_);
                            ++item_;
                            title_ = "Next track " + std::to_string(item_);
                            stopped_ = false;
                        } else {
                            song("Next track");
                        }
                    }
                    if (behavior == Next::LostResponse || behavior == Next::LostWithoutAdvance)
                        return;
                    if (!send(ssl.get(), destination, name_space, status(request, false)))
                        return;
                }
            } else if (type == "STOP" && destination == "app-1") {
                if (message.value("mediaSessionId", 0) != 1)
                    throw std::runtime_error("Invalid fake Cast Stop command");
                ++stop_count;
                if (stop_behavior == Stop::Reject) {
                    if (!send(ssl.get(), destination, name_space,
                              {{"type", "INVALID_REQUEST"}, {"requestId", request}}))
                        return;
                } else {
                    if (stop_behavior != Stop::Unchanged) {
                        std::lock_guard lock(mutex_);
                        stopped_ = true;
                    }
                    if (stop_behavior == Stop::LostResponse)
                        return;
                    if (!send(ssl.get(), destination, name_space, status(request)))
                        return;
                }
            } else
                throw std::runtime_error("Unexpected fake Cast command");
        }
    }
    void serve() {
        sigset_t signals{};
        sigemptyset(&signals);
        sigaddset(&signals, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        try {
            while (!done_) {
                pollfd fd{socket_.get(), POLLIN, 0};
                if (poll(&fd, 1, 50) <= 0)
                    continue;
                humbug::Socket client = humbug::accept_socket(socket_.get(), nullptr, nullptr, false);
                if (client.get() >= 0)
                    connection(client.get());
            }
        } catch (const std::exception& e) {
            std::lock_guard lock(mutex_);
            failure_ = e.what();
        }
    }
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context_;
    humbug::Socket socket_;
    std::atomic_bool done_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::string title_ = "Song", artist_ = "Artist", failure_;
    int item_ = 1;
    bool stopped_ = false;
    bool delayed_advance_ = false;
    int delayed_status_checks_ = 0;
};
