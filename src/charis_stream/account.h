// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <string>
#include <vector>

#include "charis_stream/net.h"

namespace CharisStream {

// Where the Charis account server lives
constexpr char AccountServerUrl[] = "https://charis-conta.netlify.app/api/";
// AccountServerUrl, or the CHARIS_ACCOUNT_URL environment variable when set (for testing)
std::string DefaultServerUrl();

// How each app makes an HTTPS request (libcurl on the Switch, Qt on the PC).
class Http {
public:
    virtual ~Http() = default;
    // POST a form-encoded body; false when the server could not be reached at all.
    virtual bool Post(const std::string& url, const std::string& body, std::string& response) = 0;
    // From another thread: abort the request in progress and fail the next ones at once
    // (the app is closing and should not wait for a slow network).
    virtual void Cancel() {}
};

struct HostEntry {
    std::string id; // 16 hex characters
    std::string name;
    std::vector<Address> local;
    Address public_address;
    bool online = false;
};

struct ConnectRequest {
    std::string client_id;
    std::string name;
    Address public_address;
    std::vector<Address> local;
};

// Calls to the account server. Every call returns "" when it worked, or a message in
// Portuguese that can be shown to the user as it is.
class Account {
public:
    explicit Account(Http& http, std::string base_url = DefaultServerUrl());

    // device: "pc" or "switch", and its name, for the account history
    std::string Register(const std::string& user, const std::string& auth_hex, std::string& token,
                         const std::string& device = {}, const std::string& device_name = {});
    std::string Login(const std::string& user, const std::string& auth_hex, std::string& token,
                      const std::string& device = {}, const std::string& device_name = {});

    // PC: keep its addresses up to date and collect the Switches asking to connect.
    // next_poll_ms: when to call again (sooner while a Switch of the account is looking).
    std::string HostPoll(const std::string& token, const std::string& host_id,
                         const std::string& name, const std::vector<Address>& local,
                         const Address& public_address, std::vector<ConnectRequest>& requests,
                         int& next_poll_ms, const std::string& game = {});
    std::string HostRemove(const std::string& token, const std::string& host_id);

    // Switch: the PCs of the account, and asking one of them to open the way
    std::string ListHosts(const std::string& token, std::vector<HostEntry>& hosts,
                          const std::string& client_id = {}, const std::string& client_name = {});
    std::string RequestConnect(const std::string& token, const std::string& host_id,
                               const std::string& client_id, const std::string& name,
                               const Address& public_address, const std::vector<Address>& local);

    // PC: for the account history. via is "casa" or "internet".
    std::string ReportSession(const std::string& token, const std::string& host_id,
                              const std::string& client_name, const std::string& via,
                              int seconds, const std::string& game);
    std::string ReportGame(const std::string& token, const std::string& host_id,
                           const std::string& title, const std::string& program_hex, int seconds,
                           bool streamed);

    // True when the last error means the saved login is no longer accepted
    bool LoginExpired() const {
        return login_expired;
    }
    // True when the last call did not reach a working account server (no internet, or the
    // server is down): the account itself may be fine, and home play still works
    bool Unavailable() const {
        return unavailable;
    }

private:
    using Fields = std::map<std::string, std::string>;
    std::string Call(const std::string& route, const Fields& params, Fields& answer);

    Http& http;
    std::string base_url;
    bool login_expired = false;
    bool unavailable = false;
};

std::string UrlEncode(const std::string& text);
std::string UrlDecode(const std::string& text);

} // namespace CharisStream
