// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "charis_stream/protocol.h"

namespace CharisStream {

using Key32 = std::array<uint8_t, 32>;
using Tag16 = std::array<uint8_t, 16>;

// Everything a device keeps after the user typed the password once.
struct AccountKeys {
    Key32 stream_key{};   // never leaves the devices; authenticates and encrypts the stream
    Tag16 tag{};          // public label of the account, so a Switch only lists its own PCs
    std::string auth_hex; // what the account server gets instead of the password
};

// Slow on purpose (Argon2id, 8 MB): about a second on the Switch.
AccountKeys DeriveAccountKeys(const std::string& username, const std::string& password);
Tag16 TagFromKey(const Key32& stream_key);

void RandomBytes(uint8_t* out, size_t size);
std::string ToHex(const uint8_t* data, size_t size);
bool FromHex(const std::string& hex, uint8_t* out, size_t size);

Tag16 HelloProof(const Key32& stream_key, const uint8_t client_nonce[16]);
Tag16 WelcomeProof(const Key32& stream_key, const uint8_t client_nonce[16],
                   const uint8_t host_nonce[16]);

Tag16 LaunchingProof(const Key32& stream_key, const uint8_t client_nonce[16]);

struct SessionKeys {
    Key32 client_to_host{};
    Key32 host_to_client{};
};
SessionKeys DeriveSessionKeys(const Key32& stream_key, const uint8_t client_nonce[16],
                              const uint8_t host_nonce[16]);

constexpr size_t SealOverhead = sizeof(Header) + 16;

// Writes header + mac + ciphertext into out (payload size + SealOverhead bytes); returns the size.
size_t Seal(uint8_t* out, Type type, uint64_t counter, const Key32& key, const uint8_t* payload,
            size_t size);
// Checks and decrypts a packet into payload_out (at least size bytes). Returns the payload size
// or -1 when the packet is not authentic.
long Open(const uint8_t* packet, size_t size, const Key32& key, uint8_t* payload_out);

bool EqualConstantTime(const uint8_t* a, const uint8_t* b, size_t size);

} // namespace CharisStream
