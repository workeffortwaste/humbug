#pragma once

#include "cast.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace humbug::cast {

struct Observation {
    Endpoint endpoint;
    Media media;
    bool connected = false;
    std::string error;
};

class Monitor {
public:
    Monitor(bool discovery, std::vector<std::string> endpoints, std::string interface);
    ~Monitor();
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;
    bool take(std::vector<Observation>& observations, std::string& error);

private:
    void run();
    void publish(const std::map<std::string, Observation>& observations, const std::string& error);
    std::atomic_bool stop_{false};
    bool discovery_;
    std::vector<std::string> endpoints_;
    std::string interface_;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<Observation> observations_;
    std::string error_;
    bool dirty_ = false;
};

} // namespace humbug::cast
