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

// Keeps a running Host listed on the account: tells the account server where the PC can be
// reached (home network, router port opened with UPnP, and the address STUN saw) and opens
// the way whenever a Switch of the same account asks for it.
class Presence {
public:
    struct Status {
        bool online = false;        // the account server answered the last call
        bool login_expired = false; // the saved login is no longer accepted: log in again
        bool router_port = false;   // the router opened a port for this PC
        std::string message;        // last error, in Portuguese; "" when all is well
    };

    Presence(Http& http, Host& host, std::string token, std::string host_id_hex,
             std::string name, std::function<void(const Status&)> on_status = {},
             std::string server_url = DefaultServerUrl());
    ~Presence();

    void Start();
    void Stop(); // also takes the PC off the account's list and closes the router port

    Status GetStatus() const;

private:
    void Loop();
    bool WaitFor(int64_t ms); // false when stopping

    Http& http;
    Host& host;
    std::string token;
    std::string host_id;
    std::string name;
    std::function<void(const Status&)> on_status;
    std::string server_url;

    std::thread thread;
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::condition_variable wake;
    mutable std::mutex status_mutex;
    Status status;
};

} // namespace CharisStream
