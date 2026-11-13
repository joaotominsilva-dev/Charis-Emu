// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__SWITCH__)
#include <switch.h>
#else
#include <cstdio>
#endif

#include "charis_stream/crypto.h"

extern "C" {
#include "charis_stream/third_party/monocypher.h"
}

namespace CharisStream {

namespace {

// Keyed Blake2b over a label and up to two byte strings, cut to out_size.
void Derive(uint8_t* out, size_t out_size, const uint8_t* key, size_t key_size, const char* label,
            const uint8_t* a = nullptr, size_t a_size = 0, const uint8_t* b = nullptr,
            size_t b_size = 0) {
    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, out_size, key, key_size);
    crypto_blake2b_update(&ctx, reinterpret_cast<const uint8_t*>(label), std::strlen(label));
    if (a_size > 0) {
        crypto_blake2b_update(&ctx, a, a_size);
    }
    if (b_size > 0) {
        crypto_blake2b_update(&ctx, b, b_size);
    }
    crypto_blake2b_final(&ctx, out);
}

void MakeNonce(uint8_t nonce[24], Type type, uint64_t counter) {
    std::memset(nonce, 0, 24);
    std::memcpy(nonce, &counter, sizeof(counter));
    nonce[8] = static_cast<uint8_t>(type);
}

std::string Canonical(const std::string& username) {
    std::string out;
    for (const char c : username) {
        if (c != ' ') {
            out += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
    }
    return out;
}

} // namespace

AccountKeys DeriveAccountKeys(const std::string& username, const std::string& password) {
    const std::string user = Canonical(username);
    const std::string salt_input = "charis-stream-account-v1:" + user;
    uint8_t salt[16];
    crypto_blake2b(salt, sizeof(salt), reinterpret_cast<const uint8_t*>(salt_input.data()),
                   salt_input.size());

    crypto_argon2_config config{};
    config.algorithm = CRYPTO_ARGON2_ID;
    config.nb_blocks = 8192; // 8 MB
    config.nb_passes = 3;
    config.nb_lanes = 1;
    crypto_argon2_inputs inputs{};
    inputs.pass = reinterpret_cast<const uint8_t*>(password.data());
    inputs.pass_size = static_cast<uint32_t>(password.size());
    inputs.salt = salt;
    inputs.salt_size = sizeof(salt);

    std::vector<uint8_t> work(static_cast<size_t>(config.nb_blocks) * 1024);
    uint8_t master[32];
    crypto_argon2(master, sizeof(master), work.data(), config, inputs, crypto_argon2_no_extras);

    AccountKeys keys;
    uint8_t auth[32];
    Derive(auth, sizeof(auth), master, sizeof(master), "auth");
    Derive(keys.stream_key.data(), keys.stream_key.size(), master, sizeof(master), "stream");
    keys.auth_hex = ToHex(auth, sizeof(auth));
    keys.tag = TagFromKey(keys.stream_key);
    crypto_wipe(master, sizeof(master));
    crypto_wipe(auth, sizeof(auth));
    return keys;
}

Tag16 TagFromKey(const Key32& stream_key) {
    Tag16 tag;
    Derive(tag.data(), tag.size(), stream_key.data(), stream_key.size(), "tag");
    return tag;
}

void RandomBytes(uint8_t* out, size_t size) {
#if defined(_WIN32)
    BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#elif defined(__SWITCH__)
    randomGet(out, size);
#else
    FILE* file = std::fopen("/dev/urandom", "rb");
    if (file == nullptr || std::fread(out, 1, size, file) != size) {
        std::abort(); // never continue with predictable nonces
    }
    std::fclose(file);
#endif
}

std::string ToHex(const uint8_t* data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

bool FromHex(const std::string& hex, uint8_t* out, size_t size) {
    if (hex.size() != size * 2) {
        return false;
    }
    const auto value = [](char c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (size_t i = 0; i < size; ++i) {
        const int high = value(hex[i * 2]);
        const int low = value(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>(high * 16 + low);
    }
    return true;
}

Tag16 HelloProof(const Key32& stream_key, const uint8_t client_nonce[16]) {
    Tag16 proof;
    Derive(proof.data(), proof.size(), stream_key.data(), stream_key.size(), "hello", client_nonce,
           16);
    return proof;
}

Tag16 LaunchingProof(const Key32& stream_key, const uint8_t client_nonce[16]) {
    Tag16 proof;
    Derive(proof.data(), proof.size(), stream_key.data(), stream_key.size(), "launching",
           client_nonce, 16);
    return proof;
}

Tag16 WelcomeProof(const Key32& stream_key, const uint8_t client_nonce[16],
                   const uint8_t host_nonce[16]) {
    Tag16 proof;
    Derive(proof.data(), proof.size(), stream_key.data(), stream_key.size(), "welcome",
           client_nonce, 16, host_nonce, 16);
    return proof;
}

SessionKeys DeriveSessionKeys(const Key32& stream_key, const uint8_t client_nonce[16],
                              const uint8_t host_nonce[16]) {
    SessionKeys keys;
    Derive(keys.client_to_host.data(), 32, stream_key.data(), stream_key.size(), "c2h",
           client_nonce, 16, host_nonce, 16);
    Derive(keys.host_to_client.data(), 32, stream_key.data(), stream_key.size(), "h2c",
           client_nonce, 16, host_nonce, 16);
    return keys;
}

size_t Seal(uint8_t* out, Type type, uint64_t counter, const Key32& key, const uint8_t* payload,
            size_t size) {
    WriteHeader(out, type, counter);
    uint8_t nonce[24];
    MakeNonce(nonce, type, counter);
    crypto_aead_lock(out + SealOverhead, out + sizeof(Header), key.data(), nonce, out,
                     sizeof(Header), payload, size);
    return size + SealOverhead;
}

long Open(const uint8_t* packet, size_t size, const Key32& key, uint8_t* payload_out) {
    if (size < SealOverhead) {
        return -1;
    }
    Header header;
    std::memcpy(&header, packet, sizeof(header));
    uint8_t nonce[24];
    MakeNonce(nonce, header.type, header.counter);
    const size_t payload = size - SealOverhead;
    if (crypto_aead_unlock(payload_out, packet + sizeof(Header), key.data(), nonce, packet,
                           sizeof(Header), packet + SealOverhead, payload) != 0) {
        return -1;
    }
    return static_cast<long>(payload);
}

bool EqualConstantTime(const uint8_t* a, const uint8_t* b, size_t size) {
    uint8_t difference = 0;
    for (size_t i = 0; i < size; ++i) {
        difference |= a[i] ^ b[i];
    }
    return difference == 0;
}

} // namespace CharisStream
