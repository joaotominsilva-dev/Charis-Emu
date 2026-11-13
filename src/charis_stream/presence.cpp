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

Presence::Presence(Http& http_, Host& host_, Credentials credentials_, std::string host_id_,
                   std::string name_, std::function<void(const Status&)> on_status_,
                   std::function<void(const std::string&)> on_token_, std::string server_url_)
    : http{http_}, host{host_}, host_id{std::move(host_id_)}, name{std::move(name_)},
      on_status{std::move(on_status_)}, on_token{std::move(on_token_)},
      server_url{std::move(server_url_)}, credentials{std::move(credentials_)} {}

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
    if (running.exchange(false)) {
        std::unique_lock lock{mutex};
        wake.notify_all();
        // A moment to finish cleanly (close the router port); a slow network does not get to
        // hold up the app any longer than that
        if (!wake.wait_for(lock, std::chrono::milliseconds(1200), [this] { return finished; })) {
            cancel = true;
            lock.unlock();
            http.Cancel();
        }
    }
    if (thread.joinable()) {
        thread.join();
    }
}

Presence::Status Presence::GetStatus() const {
    std::scoped_lock lock{status_mutex};
    return status;
}

std::string Presence::Token() const {
    std::scoped_lock lock{mutex};
    return credentials.token;
}

void Presence::SetGame(const std::string& title) {
    std::scoped_lock lock{mutex};
    if (game != title) {
        game = title;
        poke = true; // tell the account right away
        wake.notify_all();
    }
}

bool Presence::WaitFor(int64_t ms) {
    std::unique_lock lock{mutex};
    wake.wait_for(lock, std::chrono::milliseconds(ms), [this] { return !running.load() || poke; });
    // Stop waits on the same condition for the loop to finish
    poke = false;
    return running;
}

// Gets a token when there is none (the server was down at login, or the old one expired).
// False while it cannot; next tells why.
bool Presence::EnsureToken(Account& account, Status& next) {
    Credentials current;
    {
        std::scoped_lock lock{mutex};
        current = credentials;
    }
    if (!current.token.empty()) {
        return true;
    }
    if (current.user.empty() || current.auth_hex.empty()) {
        next.login_expired = true;
        next.message = "Entre na conta de novo.";
        return false;
    }
    std::string token;
    std::string error;
    if (current.create_account) {
        error = account.Register(current.user, current.auth_hex, token, "pc", name);
        if (!error.empty() && !account.Unavailable()) {
            // Already there (made on the Switch meanwhile, with the same password): log in
            error = account.Login(current.user, current.auth_hex, token, "pc", name);
        }
    } else {
        error = account.Login(current.user, current.auth_hex, token, "pc", name);
    }
    if (!error.empty()) {
        next.online = false;
        next.message = error;
        next.login_expired = !account.Unavailable();
        return false;
    }
    {
        std::scoped_lock lock{mutex};
        credentials.token = token;
        credentials.create_account = false;
    }
    if (on_token) {
        on_token(token);
    }
    return true;
}

void Presence::Loop() {
    Run();
    std::scoped_lock lock{mutex};
    finished = true;
    wake.notify_all();
}

void Presence::Run() {
    Account account{http, server_url};
    PortMapping mapping;
    mapping.SetCancelFlag(&cancel);
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

        int64_t wait_ms = backoff_ms;
        if (!EnsureToken(account, next)) {
            if (next.login_expired) {
                publish(next);
                break; // a wrong password needs the user
            }
            backoff_ms = std::min(backoff_ms * 2, MaxBackoffMs);
            publish(next);
            if (!WaitFor(wait_ms)) {
                break;
            }
            continue;
        }

        std::vector<Address> addresses = host.LocalAddresses();
        if (next.router_port && addresses.size() < 4) {
            addresses.push_back(mapping.External());
        }
        std::string token;
        std::string playing;
        {
            std::scoped_lock lock{mutex};
            token = credentials.token;
            playing = game;
        }
        std::vector<ConnectRequest> requests;
        int next_poll_ms = 10000;
        const std::string error = account.HostPoll(token, host_id, name, addresses,
                                                   host.PublicAddress(), requests, next_poll_ms,
                                                   playing);
        // 10 s is the server's "nobody is looking" pace; anything shorter means a Switch is
        wait_ms = next_poll_ms >= 10000 ? std::max(next_poll_ms, idle_poll_ms.load())
                                        : next_poll_ms;
        if (error.empty()) {
            next.online = true;
            next.login_expired = false;
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
            // The token is too old: log in again with the saved code on the next round
            std::scoped_lock lock{mutex};
            credentials.token.clear();
            wait_ms = 500;
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
