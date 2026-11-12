// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "charis_stream/account.h"
#include "charis_stream/host.h"

namespace CharisStream {

// What the PC keeps to stay on the account. auth_hex lets it log in again by itself (when the
// account server was down at login, or the token expired), so the user never has to.
struct Credentials {
    std::string user;
    std::string auth_hex;
    std::string token;          // empty: not confirmed by the account server yet
    bool create_account = false; // made while the server was down: register it when it is back
};

// Keeps a running Host listed on the account: tells the account server where the PC can be
// reached (home network, router port opened with UPnP, and the address STUN saw) and opens
// the way whenever a Switch of the same account asks for it. Home play needs none of this:
// the Host answers the Switch on the home network by itself.
class Presence {
public:
    struct Status {
        bool online = false;        // the account server answered the last call
        bool login_expired = false; // the server refused the saved login: log in again
        bool router_port = false;   // the router opened a port for this PC
        std::string message;        // last error, in Portuguese; "" when all is well
    };

    Presence(Http& http, Host& host, Credentials credentials, std::string host_id_hex,
             std::string name, std::function<void(const Status&)> on_status = {},
             std::function<void(const std::string& token)> on_token = {},
             std::string server_url = DefaultServerUrl());
    ~Presence();

    void Start();
    void Stop(); // also closes the router port

    Status GetStatus() const;
    std::string Token() const;
    // The game running now ("" for none), shown on the account's web panel
    void SetGame(const std::string& title);

private:
    void Loop();
    bool WaitFor(int64_t ms); // false when stopping
    bool EnsureToken(Account& account, Status& next);

    Http& http;
    Host& host;
    std::string host_id;
    std::string name;
    std::function<void(const Status&)> on_status;
    std::function<void(const std::string&)> on_token;
    std::string server_url;

    std::thread thread;
    std::atomic<bool> running{false};
    mutable std::mutex mutex; // wake, poke, game, credentials
    std::condition_variable wake;
    bool poke = false;
    std::string game;
    Credentials credentials;
    mutable std::mutex status_mutex;
    Status status;
};

} // namespace CharisStream
