// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

// What the Switch provides, with stand-ins on Linux so the app can be tested on a PC.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "charis_stream/account.h"

namespace Platform {

void Init();
void Exit();
bool Running(); // false when the system asks the app to close

// Saved login and preferences
struct Saved {
    std::string user;
    std::string token;
    std::string key_hex;   // stream key
    std::string auth_hex;  // logs in again by itself when the server was down at login
    bool create_pending = false;
    std::string client_id; // 16 hex, made once
    std::string last_host; // id of the PC used last
    int quality = 0;       // 0 automatic, 1 best, 2 light
};
Saved Load();
void Store(const Saved& saved);

// HTTPS through libcurl (the Switch's own TLS service on the console)
std::unique_ptr<CharisStream::Http> MakeHttp();

// The system keyboard. Returns false when the user cancelled.
bool Keyboard(const std::string& header, const std::string& guide, const std::string& initial,
              bool password, int max_length, std::string& out);

bool Docked();                   // on the TV: ask the PC for 1080p
void KeepAwake(bool streaming);  // no auto sleep or dimming while playing
std::string DeviceName();        // "Switch" with the console's nickname when available
std::string FontPath(bool bold);

} // namespace Platform
