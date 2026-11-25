#pragma once

#include "network.h"
#include "types.h"

#include <stdexcept>

namespace humbug {

class SoapError : public std::runtime_error {
public:
    SoapError(const std::string& message, std::string code)
        : std::runtime_error(message), code(std::move(code)) {}
    std::string code;
};

Device parse_device(const std::string& xml, const std::string& location);
std::vector<Group> parse_topology(const std::string& xml);
Track parse_metadata(const std::string& xml);
std::string xml_value(const std::string& xml, const std::string& name);
struct TransportUpdate {
    std::optional<std::string> state;
    std::optional<Track> track;
    std::optional<std::string> uri, number;
};
TransportUpdate parse_event(const std::string& xml);
std::string topology_event(const std::string& xml);
std::string soap(const Service& service, const std::string& action, const std::atomic_bool& stop);
Track position(const Device& device, const std::atomic_bool& stop);
void refresh(Group& group, const Device& device, const std::atomic_bool& stop);
bool merge(Group& group, const TransportUpdate& update);

} // namespace humbug
