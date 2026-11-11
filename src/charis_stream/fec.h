// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

// Reed-Solomon erasure code (Cauchy matrix over GF(256)): with k data pieces and p parity
// pieces, any k of the k + p rebuild everything, so up to p lost pieces of a block, in any
// pattern, are recovered without asking the PC again.
namespace CharisStream::Fec {

constexpr int MaxData = 64;
constexpr int MaxParity = 64;

// Every piece is `length` bytes (shorter ones are zero padded by the caller).
void Encode(const uint8_t* const* data, int k, uint8_t* const* parity, int p, size_t length);

// data[i] must point to `length` writable bytes for every i; the missing ones are filled in.
// Returns false when fewer than k pieces arrived.
bool Decode(uint8_t* const* data, const bool* data_present, int k, const uint8_t* const* parity,
            const bool* parity_present, int p, size_t length);

} // namespace CharisStream::Fec
