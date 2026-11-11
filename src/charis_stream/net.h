// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace CharisStream {

// IPv4 only: the Switch has no IPv6.
struct Address {
    uint32_t ip = 0;   // network byte order
    uint16_t port = 0; // host byte order

    bool Valid() const {
        return ip != 0 && port != 0;
    }
    bool operator==(const Address& other) const {
        return ip == other.ip && port == other.port;
    }
    bool operator!=(const Address& other) const {
        return !(*this == other);
    }
    std::string ToString() const;         // "1.2.3.4:5678", or "" when not valid
    std::string IpString() const;
    static Address Parse(const std::string& text);
    static std::vector<Address> ParseList(const std::string& comma_separated);
    static std::string JoinList(const std::vector<Address>& list);
};

int64_t NowMs();

class UdpSocket {
public:
    ~UdpSocket();
    bool Open(uint16_t port, std::string& error); // port 0: any free port
    void Close();
    bool IsOpen() const {
        return fd >= 0;
    }
    uint16_t Port() const {
        return port;
    }
    void SendTo(const Address& to, const uint8_t* data, size_t size) const;
    // Waits up to timeout_ms. Returns the datagram size, 0 on timeout, -1 on error.
    long ReceiveFrom(uint8_t* buffer, size_t capacity, Address& from, int timeout_ms) const;

private:
    intptr_t fd = -1;
    uint16_t port = 0;
};

// The address this device uses on its network (0 when there is none).
uint32_t PrimaryLocalIp();
uint32_t BroadcastIp();
bool ResolveHost(const std::string& name, uint32_t& ip);

// STUN (RFC 5389) binding request: how a device learns the address its router gives it.
constexpr size_t StunRequestSize = 20;
void BuildStunRequest(uint8_t out[StunRequestSize]);
bool IsStunPacket(const uint8_t* data, size_t size);
bool ParseStunResponse(const uint8_t* data, size_t size, Address& mapped);

struct StunServer {
    const char* host;
    uint16_t port;
};
constexpr StunServer StunServers[] = {
    {"stun.l.google.com", 19302},
    {"stun.cloudflare.com", 3478},
};

} // namespace CharisStream
