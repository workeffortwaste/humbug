#include "monitor.h"

#include <arpa/inet.h>

#include <charconv>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <utility>

namespace humbug::cast {
namespace {
using Clock = std::chrono::steady_clock;
Endpoint explicit_endpoint(const std::string& value) {
    const auto colon = value.rfind(':');
    if (colon == std::string::npos)
        throw std::runtime_error("Cast endpoint must be IPv4:port");
    const auto address = value.substr(0, colon);
    const auto port_text = value.substr(colon + 1);
    in_addr parsed{};
    unsigned port = 0;
    const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1 || error != std::errc{} ||
        end != port_text.data() + port_text.size() || port == 0 || port > 65535)
        throw std::runtime_error("Cast endpoint must be IPv4:port");
    return {value, value, address, static_cast<std::uint16_t>(port)};
}
} // namespace

Monitor::Monitor(bool discovery, std::vector<std::string> endpoints, std::string interface)
    : discovery_(discovery), endpoints_(std::move(endpoints)), interface_(std::move(interface)) {
    for (const auto& endpoint : endpoints_)
        explicit_endpoint(endpoint);
    thread_ = std::thread([this] { run(); });
}
Monitor::~Monitor() {
    stop_ = true;
    if (thread_.joinable())
        thread_.join();
}
bool Monitor::take(std::vector<Observation>& observations, std::string& error) {
    std::lock_guard lock(mutex_);
    if (!dirty_)
        return false;
    observations = observations_;
    error = error_;
    dirty_ = false;
    return true;
}
void Monitor::publish(const std::map<std::string, Observation>& observations, const std::string& error) {
    std::vector<Observation> snapshot;
    snapshot.reserve(observations.size());
    for (const auto& [id, observation] : observations)
        snapshot.push_back(observation);
    std::lock_guard lock(mutex_);
    observations_ = std::move(snapshot);
    error_ = error;
    dirty_ = true;
}
void Monitor::run() {
    std::map<std::string, Observation> observations;
    std::map<std::string, unsigned> misses;
    std::set<std::string> explicit_ids;
    std::string error;
    for (const auto& value : endpoints_) {
        const auto endpoint = explicit_endpoint(value);
        observations.emplace(endpoint.id, Observation{endpoint, {}, false, {}});
        explicit_ids.insert(endpoint.id);
    }
    auto next_discovery = Clock::time_point{};
    while (!stop_) {
        if (discovery_ && Clock::now() >= next_discovery) {
            try {
                std::set<std::string> seen = explicit_ids;
                for (auto endpoint : discover(stop_, interface_)) {
                    if (endpoint.id.empty())
                        continue;
                    seen.insert(endpoint.id);
                    auto [it, inserted] =
                        observations.try_emplace(endpoint.id, Observation{endpoint, {}, false, {}});
                    if (!inserted)
                        it->second.endpoint = std::move(endpoint);
                }
                if (stop_)
                    break;
                for (auto it = observations.begin(); it != observations.end();) {
                    if (seen.contains(it->first)) {
                        misses.erase(it->first);
                        ++it;
                    } else if (++misses[it->first] >= 2) {
                        // Keep a receiver through one missed multicast discovery.
                        misses.erase(it->first);
                        it = observations.erase(it);
                    } else
                        ++it;
                }
                error.clear();
            } catch (const std::exception& e) {
                if (!stop_) {
                    error = std::string("Cast discovery failed: ") + e.what();
                    std::cerr << "Humbug: " << error << '\n';
                }
            }
            next_discovery = Clock::now() + std::chrono::seconds(30);
        }
        for (auto& [id, observation] : observations) {
            if (stop_)
                break;
            try {
                observation.media = read(observation.endpoint, stop_, &observation.media);
                observation.connected = true;
                observation.error.clear();
            } catch (const std::exception& e) {
                if (!stop_ && (observation.connected || observation.error != e.what()))
                    std::cerr << "Humbug Cast " << observation.endpoint.name << ": " << e.what() << '\n';
                observation.connected = false;
                observation.error = e.what();
            }
            // Publish each result so a slow receiver does not delay the other streams.
            publish(observations, error);
        }
        if (observations.empty())
            publish(observations, error);
        for (int i = 0; i < 20 && !stop_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
} // namespace humbug::cast
