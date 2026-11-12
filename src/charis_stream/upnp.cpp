// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_type = SOCKET;
using socklen_type = int;
constexpr socket_type BadSocket = INVALID_SOCKET;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_type = int;
using socklen_type = socklen_t;
constexpr socket_type BadSocket = -1;
#define CLOSE_SOCKET close
#endif

#include "charis_stream/upnp.h"

namespace CharisStream {

namespace {

constexpr int HttpTimeoutMs = 2500;
constexpr int DiscoverMs = 2200;
constexpr char Description[] = "Charis Switch";

void SetBlocking(socket_type handle, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    ioctlsocket(handle, FIONBIO, &mode);
#else
    const int flags = fcntl(handle, F_GETFL, 0);
    fcntl(handle, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

bool WaitFor(socket_type handle, bool write, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(handle, &set);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = select(static_cast<int>(handle) + 1, write ? nullptr : &set,
                             write ? &set : nullptr, nullptr, &timeout);
    return ready > 0;
}

bool ParseIp(const std::string& text, uint32_t& ip) {
    in_addr address{};
    if (inet_pton(AF_INET, text.c_str(), &address) != 1) {
        return false;
    }
    ip = address.s_addr;
    return true;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// "http://host:port/path" -> parts; false for anything else
bool ParseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
    const std::string prefix = "http://";
    if (Lower(url.substr(0, prefix.size())) != prefix) {
        return false;
    }
    const size_t host_begin = prefix.size();
    const size_t slash = url.find('/', host_begin);
    const std::string authority =
        url.substr(host_begin, slash == std::string::npos ? std::string::npos : slash - host_begin);
    path = slash == std::string::npos ? "/" : url.substr(slash);
    const size_t colon = authority.find(':');
    host = authority.substr(0, colon);
    port = 80;
    if (colon != std::string::npos) {
        const int value = std::atoi(authority.c_str() + colon + 1);
        if (value <= 0 || value > 65535) {
            return false;
        }
        port = static_cast<uint16_t>(value);
    }
    return !host.empty();
}

// Text between <tag> and </tag>, searching from `from`; "" when missing.
std::string XmlValue(const std::string& xml, const std::string& tag, size_t from = 0,
                     size_t until = std::string::npos) {
    const std::string open = "<" + tag + ">";
    const size_t begin = xml.find(open, from);
    if (begin == std::string::npos || (until != std::string::npos && begin >= until)) {
        return {};
    }
    const size_t value = begin + open.size();
    const size_t end = xml.find("</" + tag + ">", value);
    if (end == std::string::npos) {
        return {};
    }
    return Trim(xml.substr(value, end - value));
}

std::string Dechunk(const std::string& body) {
    std::string out;
    size_t at = 0;
    while (at < body.size()) {
        const size_t line_end = body.find("\r\n", at);
        if (line_end == std::string::npos) {
            break;
        }
        const unsigned long size = std::strtoul(body.c_str() + at, nullptr, 16);
        at = line_end + 2;
        if (size == 0 || at + size > body.size()) {
            break;
        }
        out.append(body, at, size);
        at += size + 2;
    }
    return out;
}

// One HTTP/1.1 exchange with Connection: close. Returns the status code, 0 on failure.
int HttpExchange(const std::string& host, uint16_t port, const std::string& request,
                 std::string& body) {
    uint32_t ip = 0;
    if (!ParseIp(host, ip) && !ResolveHost(host, ip)) {
        return 0;
    }
    const socket_type handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == BadSocket) {
        return 0;
    }
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = ip;
    target.sin_port = htons(port);
    SetBlocking(handle, false);
    connect(handle, reinterpret_cast<sockaddr*>(&target), sizeof(target));
    bool connected = WaitFor(handle, true, HttpTimeoutMs);
    if (connected) {
        int error = 0;
        socklen_type size = sizeof(error);
        getsockopt(handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size);
        connected = error == 0;
    }
    if (!connected) {
        CLOSE_SOCKET(handle);
        return 0;
    }
    size_t sent = 0;
    while (sent < request.size()) {
        if (!WaitFor(handle, true, HttpTimeoutMs)) {
            CLOSE_SOCKET(handle);
            return 0;
        }
        const auto count = send(handle, request.data() + sent,
                                static_cast<int>(request.size() - sent), 0);
        if (count <= 0) {
            CLOSE_SOCKET(handle);
            return 0;
        }
        sent += static_cast<size_t>(count);
    }
    std::string response;
    char buffer[4096];
    while (response.size() < 512 * 1024 && WaitFor(handle, false, HttpTimeoutMs)) {
        const auto count = recv(handle, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            break;
        }
        response.append(buffer, static_cast<size_t>(count));
        // Some routers keep the connection open: stop once Content-Length is complete
        const size_t header_end = response.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            const std::string headers = Lower(response.substr(0, header_end));
            const size_t length_at = headers.find("\ncontent-length:");
            if (length_at != std::string::npos) {
                const unsigned long length =
                    std::strtoul(headers.c_str() + length_at + 16, nullptr, 10);
                if (response.size() >= header_end + 4 + length) {
                    break;
                }
            }
        }
    }
    CLOSE_SOCKET(handle);

    int status = 0;
    if (std::sscanf(response.c_str(), "HTTP/%*d.%*d %d", &status) != 1) {
        return 0;
    }
    const size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return 0;
    }
    body = response.substr(header_end + 4);
    if (Lower(response.substr(0, header_end)).find("transfer-encoding: chunked") !=
        std::string::npos) {
        body = Dechunk(body);
    }
    return status;
}

// The address this PC uses to talk to `ip` (right one when there are several adapters).
uint32_t LocalIpToward(uint32_t ip) {
    const socket_type handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == BadSocket) {
        return 0;
    }
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = ip;
    target.sin_port = htons(1900);
    uint32_t local_ip = 0;
    if (connect(handle, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
        sockaddr_in local{};
        socklen_type size = sizeof(local);
        if (getsockname(handle, reinterpret_cast<sockaddr*>(&local), &size) == 0) {
            local_ip = local.sin_addr.s_addr;
        }
    }
    CLOSE_SOCKET(handle);
    return local_ip;
}

} // namespace

bool IsPrivateIp(uint32_t ip_network_order) {
    const uint32_t ip = ntohl(ip_network_order);
    const auto in = [ip](uint32_t base, int bits) {
        return (ip >> (32 - bits)) == (base >> (32 - bits));
    };
    return ip == 0 || in(0x0A000000, 8) || in(0xAC100000, 12) || in(0xC0A80000, 16) ||
           in(0x64400000, 10) || in(0x7F000000, 8) || in(0xA9FE0000, 16);
}

PortMapping::~PortMapping() {
    Remove();
}

bool PortMapping::Discover() {
    InitSockets();
    const socket_type handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == BadSocket) {
        return false;
    }
    const uint32_t local_ip = PrimaryLocalIp();
    if (local_ip != 0) {
        in_addr interface_address{};
        interface_address.s_addr = local_ip;
        setsockopt(handle, IPPROTO_IP, IP_MULTICAST_IF,
                   reinterpret_cast<const char*>(&interface_address), sizeof(interface_address));
    }
    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);
    const char* targets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };
    for (const char* target : targets) {
        char search[256];
        const int size = std::snprintf(search, sizeof(search),
                                       "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                                       "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n",
                                       target);
        sendto(handle, search, size, 0, reinterpret_cast<sockaddr*>(&group), sizeof(group));
    }

    std::vector<std::string> locations;
    int64_t until = NowMs() + DiscoverMs;
    char buffer[2048];
    while (NowMs() < until) {
        if (!WaitFor(handle, false, static_cast<int>(std::max<int64_t>(1, until - NowMs())))) {
            break;
        }
        const auto count = recv(handle, buffer, sizeof(buffer) - 1, 0);
        if (count <= 0) {
            continue;
        }
        buffer[count] = 0;
        const std::string reply = buffer;
        const std::string lower = Lower(reply);
        const size_t at = lower.find("\nlocation:");
        if (at == std::string::npos) {
            continue;
        }
        const size_t end = reply.find('\r', at + 10);
        const std::string location = Trim(reply.substr(at + 10, end - (at + 10)));
        if (std::find(locations.begin(), locations.end(), location) == locations.end()) {
            locations.push_back(location);
        }
        // Other devices may answer too; once the first reply is in, wait only a moment more
        if (locations.size() == 1) {
            until = std::min(until, NowMs() + 300);
        }
    }
    CLOSE_SOCKET(handle);

    const char* services[] = {
        "urn:schemas-upnp-org:service:WANIPConnection:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };
    for (const std::string& location : locations) {
        std::string host, path;
        uint16_t port = 0;
        if (!ParseUrl(location, host, port, path)) {
            continue;
        }
        std::string xml;
        const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" +
                                    std::to_string(port) +
                                    "\r\nConnection: close\r\nUser-Agent: Charis/1.0 UPnP/1.1\r\n\r\n";
        if (HttpExchange(host, port, request, xml) != 200) {
            continue;
        }
        std::string base_host = host;
        uint16_t base_port = port;
        if (const std::string base = XmlValue(xml, "URLBase"); !base.empty()) {
            std::string unused_path;
            ParseUrl(base, base_host, base_port, unused_path);
        }
        for (const char* service : services) {
            size_t at = 0;
            while ((at = xml.find("<service>", at)) != std::string::npos) {
                const size_t end = xml.find("</service>", at);
                if (XmlValue(xml, "serviceType", at, end) == service) {
                    std::string control = XmlValue(xml, "controlURL", at, end);
                    if (control.empty()) {
                        break;
                    }
                    if (Lower(control.substr(0, 7)) == "http://") {
                        if (!ParseUrl(control, control_host, control_port, control_path)) {
                            break;
                        }
                    } else {
                        control_host = base_host;
                        control_port = base_port;
                        control_path = control[0] == '/' ? control : "/" + control;
                    }
                    service_type = service;
                    return true;
                }
                at = end == std::string::npos ? xml.size() : end;
            }
        }
    }
    return false;
}

namespace {

int Soap(const std::string& host, uint16_t port, const std::string& path,
         const std::string& service, const std::string& action, const std::string& arguments,
         std::string& response) {
    const std::string body =
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:" +
        action + " xmlns:u=\"" + service + "\">" + arguments + "</u:" + action +
        "></s:Body></s:Envelope>\r\n";
    const std::string request = "POST " + path + " HTTP/1.1\r\nHost: " + host + ":" +
                                std::to_string(port) +
                                "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
                                "SOAPAction: \"" +
                                service + "#" + action +
                                "\"\r\nConnection: close\r\nContent-Length: " +
                                std::to_string(body.size()) + "\r\n\r\n" + body;
    return HttpExchange(host, port, request, response);
}

} // namespace

bool PortMapping::AddMapping(uint16_t external_port, uint32_t lease, int& upnp_error) {
    in_addr address{};
    address.s_addr = internal_ip;
    char internal_text[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &address, internal_text, sizeof(internal_text));
    const std::string arguments =
        "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(external_port) +
        "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" +
        std::to_string(internal_port) + "</NewInternalPort><NewInternalClient>" + internal_text +
        "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>" +
        Description + "</NewPortMappingDescription><NewLeaseDuration>" + std::to_string(lease) +
        "</NewLeaseDuration>";
    std::string response;
    const int status = Soap(control_host, control_port, control_path, service_type,
                            "AddPortMapping", arguments, response);
    upnp_error = status == 200 ? 0 : std::atoi(XmlValue(response, "errorCode").c_str());
    if (status != 200 && upnp_error == 0) {
        upnp_error = -1;
    }
    return status == 200;
}

bool PortMapping::QueryExternalIp() {
    std::string response;
    if (Soap(control_host, control_port, control_path, service_type, "GetExternalIPAddress", "",
             response) != 200) {
        return false;
    }
    uint32_t ip = 0;
    if (!ParseIp(XmlValue(response, "NewExternalIPAddress"), ip)) {
        return false;
    }
    external.ip = ip;
    return true;
}

bool PortMapping::Map(uint16_t port, std::string& error) {
    Remove();
    internal_port = port;
    if (!Discover()) {
        error = "O roteador não respondeu ao UPnP.";
        return false;
    }
    uint32_t router_ip = 0;
    if (!ParseIp(control_host, router_ip) && !ResolveHost(control_host, router_ip)) {
        error = "O roteador não respondeu ao UPnP.";
        return false;
    }
    internal_ip = LocalIpToward(router_ip);
    if (internal_ip == 0) {
        error = "O roteador não respondeu ao UPnP.";
        return false;
    }
    lease_seconds = 3600;
    for (uint16_t external_port = port; external_port < port + 10; ++external_port) {
        int upnp_error = 0;
        bool added = AddMapping(external_port, lease_seconds, upnp_error);
        if (!added && upnp_error == 725) { // the router only takes permanent mappings
            lease_seconds = 0;
            added = AddMapping(external_port, lease_seconds, upnp_error);
        }
        if (added) {
            external.port = external_port;
            mapped = true;
            QueryExternalIp();
            return true;
        }
        if (upnp_error != 718) { // 718: the port is taken by another PC; try the next one
            break;
        }
    }
    error = "O roteador recusou abrir a porta.";
    return false;
}

bool PortMapping::Refresh() {
    if (!mapped) {
        return false;
    }
    int upnp_error = 0;
    if (!AddMapping(external.port, lease_seconds, upnp_error)) {
        return false;
    }
    QueryExternalIp();
    return true;
}

void PortMapping::Remove() {
    if (!mapped) {
        return;
    }
    mapped = false;
    const std::string arguments = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" +
                                  std::to_string(external.port) +
                                  "</NewExternalPort><NewProtocol>UDP</NewProtocol>";
    std::string response;
    Soap(control_host, control_port, control_path, service_type, "DeletePortMapping", arguments,
         response);
    external = {};
}

bool PortMapping::IsReachable() const {
    return mapped && external.Valid() && !IsPrivateIp(external.ip);
}

} // namespace CharisStream
