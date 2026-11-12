// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

#include "charis_stream/net.h"

namespace CharisStream {

// Asks the home router (UPnP IGD) to forward a UDP port to this PC, so a Switch away from home
// reaches it directly, the way an opened port does for Moonlight, without the user touching
// the router. Plain HTTP on the home network; blocking, call from a worker thread.
class PortMapping {
public:
    ~PortMapping();

    // Maps internal_port on this PC. True when the router accepted; then External() is the
    // address on the internet (its IP may still be private when the provider adds its own NAT,
    // see IsReachable).
    bool Map(uint16_t internal_port, std::string& error);
    // Renews the lease; call every few minutes.
    bool Refresh();
    void Remove();

    Address External() const {
        return external;
    }
    bool IsReachable() const;

private:
    bool Discover();
    bool AddMapping(uint16_t external_port, uint32_t lease_seconds, int& upnp_error);
    bool QueryExternalIp();

    std::string control_host; // "192.168.0.1"
    uint16_t control_port = 0;
    std::string control_path;
    std::string service_type;
    uint32_t internal_ip = 0;
    uint16_t internal_port = 0;
    uint32_t lease_seconds = 3600;
    Address external;
    bool mapped = false;
};

// True for addresses that cannot be reached from the internet (home and provider networks).
bool IsPrivateIp(uint32_t ip_network_order);

} // namespace CharisStream
