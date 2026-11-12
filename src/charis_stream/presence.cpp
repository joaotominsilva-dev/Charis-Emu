// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>

#include "charis_stream/presence.h"
#include "charis_stream/upnp.h"

namespace CharisStream {

namespace {

constexpr int64_t MappingRetryMs = 10 * 60 * 1000;   // the router said no: ask again later
constexpr int64_t MappingRefreshMs = 20 * 60 * 1000; // leases are an hour
constexpr int64_t AfterRequestMs = 1000; // a Switch is connecting: stay quick for the next step
constexpr int64_t MaxBackoffMs = 30000;

} // namespace

Presence::Presence(Http& http_, Host& host_, std::string token_, std::string host_id_,
                   std::string name_, std::function<void(const Status&)> on_status_,
                   std::string server_url_)
    : http{http_}, host{host_}, token{std::move(token_)}, host_id{std::move(host_id_)},
      name{std::move(name_)}, on_status{std::move(on_status_)},
      server_url{std::move(server_url_)} {}

Presence::~Presence() {
    Stop();
}

void Presence::Start() {
    if (running.exchange(true)) {
        return;
    }
    thread = std::thread([this] { Loop(); });
}

void Presence::Stop() {
    if (!running.exchange(false)) {
        if (thread.joinable()) {
            thread.join();
        }
        return;
    }
    {
        std::scoped_lock lock{mutex};
        wake.notify_all();
    }
    if (thread.joinable()) {
        thread.join();
    }
}

Presence::Status Presence::GetStatus() const {
    std::scoped_lock lock{status_mutex};
    return status;
}

bool Presence::WaitFor(int64_t ms) {
    std::unique_lock lock{mutex};
    wake.wait_for(lock, std::chrono::milliseconds(ms), [this] { return !running.load(); });
    return running;
}

void Presence::Loop() {
    Account account{http, server_url};
    PortMapping mapping;
    int64_t next_mapping_try = 0;
    int64_t next_mapping_refresh = 0;
    int64_t backoff_ms = 2000;
    bool mapped = false;

    const auto publish = [this](const Status& next) {
        bool changed = false;
        {
            std::scoped_lock lock{status_mutex};
            changed = next.online != status.online || next.login_expired != status.login_expired ||
                      next.router_port != status.router_port || next.message != status.message;
            status = next;
        }
        if (changed && on_status) {
            on_status(next);
        }
    };

    while (running) {
        const int64_t now = NowMs();
        Status next = GetStatus();

        // Open (and keep open) a port on the home router when it lets us
        if (!mapped && now >= next_mapping_try) {
            std::string error;
            mapped = host.Port() != 0 && mapping.Map(host.Port(), error);
            next_mapping_try = now + MappingRetryMs;
            next_mapping_refresh = now + MappingRefreshMs;
        } else if (mapped && now >= next_mapping_refresh) {
            mapped = mapping.Refresh();
            next_mapping_refresh = now + MappingRefreshMs;
            if (!mapped) {
                next_mapping_try = now;
            }
        }
        next.router_port = mapped && mapping.IsReachable();
        if (!running) {
            break;
        }

        std::vector<Address> addresses = host.LocalAddresses();
        if (next.router_port && addresses.size() < 4) {
            addresses.push_back(mapping.External());
        }
        std::vector<ConnectRequest> requests;
        int next_poll_ms = 10000;
        const std::string error = account.HostPoll(token, host_id, name, addresses,
                                                   host.PublicAddress(), requests, next_poll_ms);
        int64_t wait_ms = next_poll_ms;
        if (error.empty()) {
            next.online = true;
            next.message.clear();
            backoff_ms = 2000;
            for (const ConnectRequest& request : requests) {
                std::vector<Address> targets = request.local;
                targets.push_back(request.public_address);
                host.Punch(targets);
            }
            if (!requests.empty()) {
                wait_ms = AfterRequestMs;
            }
        } else if (account.LoginExpired()) {
            next.online = false;
            next.login_expired = true;
            next.message = error;
            publish(next);
            break; // nothing more to do until the user logs in again
        } else {
            next.online = false;
            next.message = error;
            wait_ms = backoff_ms;
            backoff_ms = std::min(backoff_ms * 2, MaxBackoffMs);
        }
        publish(next);
        if (!WaitFor(wait_ms)) {
            break;
        }
    }
    mapping.Remove();
}

} // namespace CharisStream
