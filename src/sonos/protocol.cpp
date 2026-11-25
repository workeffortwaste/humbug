#include "protocol.h"

#include <tinyxml2.h>

#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace humbug {
namespace {
using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

std::string local(const char* name) {
    std::string s = name ? name : "";
    const auto colon = s.find(':');
    return colon == std::string::npos ? s : s.substr(colon + 1);
}
XMLElement* find(XMLElement* e, const std::string& name) {
    if (!e)
        return nullptr;
    if (local(e->Name()) == name)
        return e;
    for (auto* child = e->FirstChildElement(); child; child = child->NextSiblingElement())
        if (auto* result = find(child, name))
            return result;
    return nullptr;
}
std::string text(XMLElement* e) {
    return e && e->GetText() ? e->GetText() : "";
}
std::string attr(XMLElement* e, const char* name) {
    return e && e->Attribute(name) ? e->Attribute(name) : "";
}
void parse(XMLDocument& d, const std::string& xml, const std::string& stage) {
    const auto context = "Malformed Sonos XML at " + stage + " (" + std::to_string(xml.size()) + " bytes): ";
    if (xml.size() > 1024 * 1024)
        throw std::runtime_error(context + "size limit exceeded");
    if (xml.find("<!DOCTYPE") != std::string::npos)
        throw std::runtime_error(context + "DOCTYPE is not allowed");
    const auto error = d.Parse(xml.data(), xml.size());
    if (error != tinyxml2::XML_SUCCESS)
        throw std::runtime_error(context + d.ErrorName() + " at line " + std::to_string(d.ErrorLineNum()));
    if (!d.RootElement())
        throw std::runtime_error(context + "missing root element");
}
std::string value(XMLElement* e, const std::string& name) {
    return text(find(e, name));
}
std::string uuid(std::string s) {
    if (s.starts_with("uuid:"))
        s.erase(0, 5);
    return s;
}
std::optional<std::uint32_t> time_seconds(const std::string& value) {
    if (value.empty() || value == "NOT_IMPLEMENTED")
        return std::nullopt;
    const auto first = value.find(':');
    const auto second = first == std::string::npos ? first : value.find(':', first + 1);
    if (first == std::string::npos || second == std::string::npos)
        throw std::runtime_error("Invalid Sonos track time");
    const auto fraction = value.find('.', second + 1);
    const auto seconds_end = fraction == std::string::npos ? value.size() : fraction;
    const auto component = [&](std::size_t begin, std::size_t end) {
        std::uint32_t n{};
        const auto [p, error] = std::from_chars(value.data() + begin, value.data() + end, n);
        if (begin == end || error != std::errc{} || p != value.data() + end)
            throw std::runtime_error("Invalid Sonos track time");
        return n;
    };
    const auto hours = component(0, first);
    const auto minutes = component(first + 1, second);
    const auto seconds = component(second + 1, seconds_end);
    if (fraction != std::string::npos) {
        if (fraction + 1 == value.size())
            throw std::runtime_error("Invalid Sonos track time");
        for (auto i = fraction + 1; i < value.size(); ++i)
            if (value[i] < '0' || value[i] > '9')
                throw std::runtime_error("Invalid Sonos track time");
    }
    const auto total = std::uint64_t(hours) * 3600 + minutes * 60ULL + seconds;
    if (minutes >= 60 || seconds >= 60 || total > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Invalid Sonos track time");
    return static_cast<std::uint32_t>(total);
}
} // namespace

std::string Track::identity() const {
    std::ostringstream s;
    for (const auto* field : {&uri, &number, &artist, &title, &album})
        s << field->size() << ':' << *field;
    return s.str();
}

std::string xml_value(const std::string& xml, const std::string& name) {
    XMLDocument d;
    parse(d, xml, "response field " + name);
    return value(d.RootElement(), name);
}

Device parse_device(const std::string& xml, const std::string& location) {
    XMLDocument d;
    parse(d, xml, "device description");
    Device result;
    result.location = location;
    result.uuid = uuid(value(d.RootElement(), "UDN"));
    result.name = value(d.RootElement(), "roomName");
    if (result.name.empty())
        result.name = value(d.RootElement(), "friendlyName");
    auto base = value(d.RootElement(), "URLBase");
    if (base.empty())
        base = location;
    const auto visit = [&](const auto& self, XMLElement* e) -> void {
        if (local(e->Name()) == "service") {
            const auto type = value(e, "serviceType");
            auto* service = type.find(":AVTransport:") != std::string::npos         ? &result.transport
                            : type.find(":ZoneGroupTopology:") != std::string::npos ? &result.topology
                                                                                    : nullptr;
            if (service) {
                service->type = type;
                const auto control = value(e, "controlURL"), events = value(e, "eventSubURL");
                if (control.empty() || events.empty())
                    throw std::runtime_error("Sonos service has missing URLs");
                service->control = resolve_url(base, control);
                service->events = resolve_url(base, events);
                if (parse_url(service->control).host != parse_url(location).host ||
                    parse_url(service->events).host != parse_url(location).host)
                    throw std::runtime_error("Sonos service URL points to a different host");
            }
        }
        for (auto* c = e->FirstChildElement(); c; c = c->NextSiblingElement())
            self(self, c);
    };
    visit(visit, d.RootElement());
    if (result.uuid.empty() || result.transport.control.empty())
        throw std::runtime_error("Not a Sonos transport device");
    return result;
}

std::vector<Group> parse_topology(const std::string& xml) {
    XMLDocument d;
    parse(d, xml, "topology envelope or document");
    if (auto* wrapped = find(d.RootElement(), "ZoneGroupState"); wrapped && !wrapped->FirstChildElement()) {
        const auto nested = text(wrapped);
        d.Clear();
        parse(d, nested, "embedded ZoneGroupState");
    }
    auto* groups = find(d.RootElement(), "ZoneGroups");
    if (!groups)
        throw std::runtime_error("Missing Sonos ZoneGroups");
    std::vector<Group> result;
    for (auto* e = groups->FirstChildElement(); e; e = e->NextSiblingElement()) {
        if (local(e->Name()) != "ZoneGroup")
            continue;
        Group g;
        g.coordinator = uuid(attr(e, "Coordinator"));
        g.id = attr(e, "ID");
        if (g.id.empty())
            g.id = g.coordinator;
        for (auto* m = e->FirstChildElement(); m; m = m->NextSiblingElement()) {
            if (local(m->Name()) != "ZoneGroupMember")
                continue;
            if (uuid(attr(m, "UUID")) == g.coordinator)
                g.location = attr(m, "Location");
            if (attr(m, "Invisible") == "1")
                continue;
            const auto name = attr(m, "ZoneName");
            if (!name.empty())
                g.rooms.push_back(name);
        }
        if (g.coordinator.empty() || g.location.empty())
            throw std::runtime_error("Incomplete Sonos playback group");
        parse_url(g.location);
        if (!g.rooms.empty())
            result.push_back(std::move(g));
    }
    return result;
}

Track parse_metadata(const std::string& xml) {
    if (xml.empty() || xml == "NOT_IMPLEMENTED")
        return {};
    XMLDocument d;
    parse(d, xml, "track metadata DIDL");
    auto* item = find(d.RootElement(), "item");
    if (!item)
        throw std::runtime_error("Missing DIDL track item");
    auto artist = value(item, "artist");
    // Some Sonos sources put the performer in dc:creator instead of upnp:artist.
    if (artist.empty())
        artist = value(item, "creator");
    return {value(item, "title"), std::move(artist), value(item, "album"), value(item, "res"), ""};
}

TransportUpdate parse_event(const std::string& xml) {
    XMLDocument outer, change;
    parse(outer, xml, "AVTransport event envelope");
    auto* last = find(outer.RootElement(), "LastChange");
    if (!last)
        throw std::runtime_error("Missing AVTransport LastChange");
    parse(change, text(last), "AVTransport LastChange");
    XMLElement* instance = nullptr;
    for (auto* e = change.RootElement()->FirstChildElement(); e; e = e->NextSiblingElement()) {
        if (local(e->Name()) == "InstanceID" && attr(e, "val") == "0") {
            instance = e;
            break;
        }
    }
    if (!instance)
        throw std::runtime_error("Missing AVTransport instance zero");
    TransportUpdate u;
    for (auto* e = instance->FirstChildElement(); e; e = e->NextSiblingElement()) {
        const auto name = local(e->Name());
        const auto v = e->Attribute("val") ? attr(e, "val") : text(e);
        if (name == "TransportState")
            u.state = v;
        else if (name == "CurrentTrackMetaData")
            u.track = parse_metadata(v);
        else if (name == "CurrentTrackURI")
            u.uri = v;
        else if (name == "CurrentTrack")
            u.number = v;
    }
    return u;
}

std::string topology_event(const std::string& xml) {
    return xml_value(xml, "ZoneGroupState");
}

std::string soap(const Service& service, const std::string& action, const std::atomic_bool& stop) {
    if (action != "GetTransportInfo" && action != "GetPositionInfo" && action != "GetZoneGroupState" &&
        action != "GetCurrentTransportActions" && action != "Next" && action != "Stop")
        throw std::runtime_error("Unsupported Sonos action: " + action);
    const auto arguments = action == "GetZoneGroupState" ? "" : "<InstanceID>0</InstanceID>";
    const auto body = "<?xml version=\"1.0\"?><s:Envelope "
                      "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:" +
                      action + " xmlns:u=\"" + service.type + "\">" + arguments + "</u:" + action +
                      "></s:Body></s:Envelope>";
    const auto r = request(service.control, "POST",
                           {{"Content-Type", "text/xml; charset=\"utf-8\""},
                            {"SOAPACTION", "\"" + service.type + "#" + action + "\""}},
                           body, stop);
    if (!r.body.empty()) {
        XMLDocument d;
        parse(d, r.body, action + " SOAP response");
        if (find(d.RootElement(), "Fault"))
            throw SoapError(action + " returned a SOAP fault: " + value(d.RootElement(), "errorDescription"),
                            value(d.RootElement(), "errorCode"));
    }
    if (r.status() < 200 || r.status() >= 300)
        throw std::runtime_error(action + " failed: HTTP " + std::to_string(r.status()));
    return r.body;
}

Track position(const Device& device, const std::atomic_bool& stop) {
    const auto xml = soap(device.transport, "GetPositionInfo", stop);
    auto track = parse_metadata(xml_value(xml, "TrackMetaData"));
    track.uri = xml_value(xml, "TrackURI");
    track.number = xml_value(xml, "Track");
    track.elapsed_seconds = time_seconds(xml_value(xml, "RelTime"));
    track.duration_seconds = time_seconds(xml_value(xml, "TrackDuration"));
    return track;
}

bool merge(Group& g, const TransportUpdate& u) {
    const auto previous = g.track.identity();
    const bool restarted =
        u.state && *u.state == "PLAYING" && (g.state == "TRANSITIONING" || g.state == "STOPPED");
    if (u.state)
        g.state = *u.state;
    if (u.track || u.uri || u.number) {
        const auto& uri = u.uri ? *u.uri : u.track && !u.track->uri.empty() ? u.track->uri : g.track.uri;
        const auto& number = u.number ? *u.number : g.track.number;
        const bool same_item = uri == g.track.uri && number == g.track.number;
        auto track = u.track ? *u.track : same_item && !restarted ? g.track : Track{};
        track.uri = uri;
        track.number = number;
        if (u.track && same_item && !restarted && track.identity() == previous) {
            if (!track.elapsed_seconds)
                track.elapsed_seconds = g.track.elapsed_seconds;
            if (!track.duration_seconds)
                track.duration_seconds = g.track.duration_seconds;
        }
        // A URI/index-only event must not attach the previous song to the new item.
        g.track = std::move(track);
    }
    if (previous != g.track.identity() || restarted) {
        ++g.occurrence;
        return true;
    }
    return false;
}

void refresh(Group& group, const Device& device, const std::atomic_bool& stop) {
    const auto state = xml_value(soap(device.transport, "GetTransportInfo", stop), "CurrentTransportState");
    if (state.empty())
        throw std::runtime_error("Missing Sonos transport state");
    const auto track = position(device, stop);
    merge(group, {state, track, track.uri, track.number});
    group.track.elapsed_seconds = track.elapsed_seconds;
    group.track.duration_seconds = track.duration_seconds;
}
} // namespace humbug
