// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <vector>

#include "charis_stream/fec.h"

namespace CharisStream::Fec {

namespace {

struct Tables {
    uint8_t exp[512];
    uint8_t log[256];
    uint8_t inv[256];
    uint8_t mul[256][256];

    Tables() {
        unsigned x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) {
                x ^= 0x11d;
            }
        }
        for (int i = 255; i < 512; ++i) {
            exp[i] = exp[i - 255];
        }
        log[0] = 0;
        inv[0] = 0;
        for (int a = 1; a < 256; ++a) {
            inv[a] = exp[255 - log[a]];
        }
        for (int a = 0; a < 256; ++a) {
            for (int b = 0; b < 256; ++b) {
                mul[a][b] = (a == 0 || b == 0) ? 0 : exp[log[a] + log[b]];
            }
        }
    }
};

const Tables& T() {
    static const Tables tables;
    return tables;
}

// Row j (parity), column i (data): 1 / (x_j + y_i) with x_j = k + j and y_i = i
uint8_t Coefficient(int k, int j, int i) {
    return T().inv[static_cast<uint8_t>((k + j) ^ i)];
}

void MulAdd(uint8_t* out, const uint8_t* in, uint8_t c, size_t length) {
    if (c == 0) {
        return;
    }
    if (c == 1) {
        for (size_t b = 0; b < length; ++b) {
            out[b] ^= in[b];
        }
        return;
    }
    const uint8_t* row = T().mul[c];
    for (size_t b = 0; b < length; ++b) {
        out[b] ^= row[in[b]];
    }
}

} // namespace

void Encode(const uint8_t* const* data, int k, uint8_t* const* parity, int p, size_t length) {
    for (int j = 0; j < p; ++j) {
        std::memset(parity[j], 0, length);
        for (int i = 0; i < k; ++i) {
            MulAdd(parity[j], data[i], Coefficient(k, j, i), length);
        }
    }
}

bool Decode(uint8_t* const* data, const bool* data_present, int k, const uint8_t* const* parity,
            const bool* parity_present, int p, size_t length) {
    int missing[MaxData];
    int m = 0;
    for (int i = 0; i < k; ++i) {
        if (!data_present[i]) {
            missing[m++] = i;
        }
    }
    if (m == 0) {
        return true;
    }
    int rows[MaxData];
    int r = 0;
    for (int j = 0; j < p && r < m; ++j) {
        if (parity_present[j]) {
            rows[r++] = j;
        }
    }
    if (r < m) {
        return false;
    }

    // What the missing pieces must add up to in each parity row
    std::vector<uint8_t> sums(static_cast<size_t>(m) * length);
    for (int row = 0; row < m; ++row) {
        uint8_t* sum = sums.data() + static_cast<size_t>(row) * length;
        std::memcpy(sum, parity[rows[row]], length);
        for (int i = 0; i < k; ++i) {
            if (data_present[i]) {
                MulAdd(sum, data[i], Coefficient(k, rows[row], i), length);
            }
        }
    }

    // Invert the m x m piece of the matrix (Gauss-Jordan; every square piece of a Cauchy
    // matrix can be inverted)
    uint8_t a[MaxData][MaxData];
    uint8_t b[MaxData][MaxData];
    for (int row = 0; row < m; ++row) {
        for (int col = 0; col < m; ++col) {
            a[row][col] = Coefficient(k, rows[row], missing[col]);
            b[row][col] = row == col ? 1 : 0;
        }
    }
    const Tables& t = T();
    for (int col = 0; col < m; ++col) {
        int pivot = col;
        while (pivot < m && a[pivot][col] == 0) {
            ++pivot;
        }
        if (pivot == m) {
            return false;
        }
        if (pivot != col) {
            for (int c = 0; c < m; ++c) {
                std::swap(a[pivot][c], a[col][c]);
                std::swap(b[pivot][c], b[col][c]);
            }
        }
        const uint8_t scale = t.inv[a[col][col]];
        for (int c = 0; c < m; ++c) {
            a[col][c] = t.mul[scale][a[col][c]];
            b[col][c] = t.mul[scale][b[col][c]];
        }
        for (int row = 0; row < m; ++row) {
            const uint8_t factor = a[row][col];
            if (row == col || factor == 0) {
                continue;
            }
            for (int c = 0; c < m; ++c) {
                a[row][c] ^= t.mul[factor][a[col][c]];
                b[row][c] ^= t.mul[factor][b[col][c]];
            }
        }
    }

    for (int col = 0; col < m; ++col) {
        uint8_t* out = data[missing[col]];
        std::memset(out, 0, length);
        for (int row = 0; row < m; ++row) {
            MulAdd(out, sums.data() + static_cast<size_t>(row) * length, b[col][row], length);
        }
    }
    return true;
}

} // namespace CharisStream::Fec
