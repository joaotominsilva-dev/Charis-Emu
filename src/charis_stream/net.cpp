// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_type = int;
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using socklen_type = socklen_t;
#define CLOSE_SOCKET close
#endif

#include "charis_stream/crypto.h"
#include "charis_stream/net.h"

namespace CharisStream {

namespace {

constexpr uint32_t StunCookie = 0x2112A442;

void EnsureSockets() {
    InitSockets();
}

sockaddr_in ToSockaddr(const Address& address) {
    sockaddr_in out{};
    out.sin_family = AF_INET;
    out.sin_addr.s_addr = address.ip;
    out.sin_port = htons(address.port);
    return out;
}

} // namespace

void InitSockets() {
#ifdef _WIN32
    static const bool started = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    (void)started;
#endif
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string Address::IpString() const {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
    char text[24];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return text;
}

std::string Address::ToString() const {
    if (!Valid()) {
        return {};
    }
    return IpString() + ":" + std::to_string(port);
}

Address Address::Parse(const std::string& text) {
    unsigned a = 0, b = 0, c = 0, d = 0, p = 0;
    char extra = 0;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u:%u%c", &a, &b, &c, &d, &p, &extra) != 5 ||
        a > 255 || b > 255 || c > 255 || d > 255 || p == 0 || p > 65535) {
        return {};
    }
    Address out;
    uint8_t* bytes = reinterpret_cast<uint8_t*>(&out.ip);
    bytes[0] = static_cast<uint8_t>(a);
    bytes[1] = static_cast<uint8_t>(b);
    bytes[2] = static_cast<uint8_t>(c);
    bytes[3] = static_cast<uint8_t>(d);
    out.port = static_cast<uint16_t>(p);
    return out;
}

std::vector<Address> Address::ParseList(const std::string& comma_separated) {
    std::vector<Address> out;
    size_t start = 0;
    while (start <= comma_separated.size()) {
        const size_t end = comma_separated.find(',', start);
        const Address address = Parse(comma_separated.substr(
            start, end == std::string::npos ? std::string::npos : end - start));
        if (address.Valid()) {
            out.push_back(address);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

std::string Address::JoinList(const std::vector<Address>& list) {
    std::string out;
    for (const Address& address : list) {
        if (!address.Valid()) {
            continue;
        }
        if (!out.empty()) {
            out += ',';
        }
        out += address.ToString();
    }
    return out;
}

UdpSocket::~UdpSocket() {
    Close();
}

bool UdpSocket::Open(uint16_t wanted_port, std::string& error) {
    EnsureSockets();
    const auto handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle < 0) {
        error = "Não foi possível abrir a conexão de rede.";
        return false;
    }
    const int yes = 1;
    setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
    const int buffer = 4 * 1024 * 1024;
    setsockopt(handle, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer),
               sizeof(buffer));
    setsockopt(handle, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer),
               sizeof(buffer));
#ifdef _WIN32
    // Without this, Windows reports an unreachable peer as an error on the next receive
    DWORD off = 0;
    DWORD returned = 0;
    WSAIoctl(handle, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof(off), nullptr,
             0, &returned, nullptr, nullptr);
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(wanted_port);
    if (bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        CLOSE_SOCKET(handle);
        error = "A porta de rede " + std::to_string(wanted_port) + " já está em uso.";
        return false;
    }
    socklen_type size = sizeof(address);
    getsockname(handle, reinterpret_cast<sockaddr*>(&address), &size);
    port = ntohs(address.sin_port);
    fd = static_cast<intptr_t>(handle);
    return true;
}

void UdpSocket::Close() {
    if (fd >= 0) {
        CLOSE_SOCKET(fd);
        fd = -1;
    }
}

void UdpSocket::SendTo(const Address& to, const uint8_t* data, size_t size) const {
    if (fd < 0 || !to.Valid()) {
        return;
    }
    const sockaddr_in address = ToSockaddr(to);
    sendto(fd, reinterpret_cast<const char*>(data), static_cast<int>(size), 0,
           reinterpret_cast<const sockaddr*>(&address), sizeof(address));
}

long UdpSocket::ReceiveFrom(uint8_t* buffer, size_t capacity, Address& from,
                            int timeout_ms) const {
    if (fd < 0) {
        return -1;
    }
#ifdef _WIN32
    WSAPOLLFD pfd{static_cast<SOCKET>(fd), POLLRDNORM, 0};
    const int ready = WSAPoll(&pfd, 1, timeout_ms);
#else
    pollfd pfd{static_cast<int>(fd), POLLIN, 0};
    const int ready = poll(&pfd, 1, timeout_ms);
#endif
    if (ready <= 0) {
        return ready;
    }
    sockaddr_in address{};
    socklen_type size = sizeof(address);
    const auto received = recvfrom(fd, reinterpret_cast<char*>(buffer), static_cast<int>(capacity),
                                   0, reinterpret_cast<sockaddr*>(&address), &size);
    if (received < 0) {
        return 0; // a refused datagram is not a reason to stop
    }
    from.ip = address.sin_addr.s_addr;
    from.port = ntohs(address.sin_port);
    return static_cast<long>(received);
}

// No packet is sent: connecting a UDP socket only asks the system which address it would use.
uint32_t PrimaryLocalIp() {
    EnsureSockets();
    const auto handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle < 0) {
        return 0;
    }
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(0x08080808);
    target.sin_port = htons(53);
    uint32_t ip = 0;
    if (connect(handle, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
        sockaddr_in local{};
        socklen_type size = sizeof(local);
        if (getsockname(handle, reinterpret_cast<sockaddr*>(&local), &size) == 0) {
            ip = local.sin_addr.s_addr;
        }
    }
    CLOSE_SOCKET(handle);
    return ip;
}

uint32_t BroadcastIp() {
    return htonl(INADDR_BROADCAST);
}

bool ResolveHost(const std::string& name, uint32_t& ip) {
    EnsureSockets();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        return false;
    }
    ip = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(result);
    return true;
}

void BuildStunRequest(uint8_t out[StunRequestSize]) {
    out[0] = 0x00; // binding request
    out[1] = 0x01;
    out[2] = 0x00; // no attributes
    out[3] = 0x00;
    const uint32_t cookie = htonl(StunCookie);
    std::memcpy(out + 4, &cookie, 4);
    RandomBytes(out + 8, 12);
}

bool IsStunPacket(const uint8_t* data, size_t size) {
    if (size < StunRequestSize) {
        return false;
    }
    uint32_t cookie;
    std::memcpy(&cookie, data + 4, 4);
    return ntohl(cookie) == StunCookie && (data[0] & 0xC0) == 0;
}

bool ParseStunResponse(const uint8_t* data, size_t size, Address& mapped) {
    if (!IsStunPacket(data, size) || data[0] != 0x01 || data[1] != 0x01) {
        return false;
    }
    size_t offset = 20;
    while (offset + 4 <= size) {
        const uint16_t type = static_cast<uint16_t>(data[offset] << 8 | data[offset + 1]);
        const uint16_t length = static_cast<uint16_t>(data[offset + 2] << 8 | data[offset + 3]);
        const uint8_t* value = data + offset + 4;
        if (offset + 4 + length > size) {
            break;
        }
        // XOR-MAPPED-ADDRESS (0x0020) or MAPPED-ADDRESS (0x0001), IPv4
        if ((type == 0x0020 || type == 0x0001) && length >= 8 && value[1] == 0x01) {
            uint16_t port = static_cast<uint16_t>(value[2] << 8 | value[3]);
            uint8_t ip[4] = {value[4], value[5], value[6], value[7]};
            if (type == 0x0020) {
                port ^= static_cast<uint16_t>(StunCookie >> 16);
                ip[0] ^= 0x21;
                ip[1] ^= 0x12;
                ip[2] ^= 0xA4;
                ip[3] ^= 0x42;
            }
            std::memcpy(&mapped.ip, ip, 4);
            mapped.port = port;
            return mapped.Valid();
        }
        offset += 4 + ((length + 3u) & ~3u);
    }
    return false;
}

} // namespace CharisStream
