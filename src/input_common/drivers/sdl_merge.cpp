// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>

#include "input_common/drivers/sdl_merge.h"

namespace InputCommon {

namespace {
std::atomic_bool merge_gamepads{false};
} // namespace

void SetMergeGamepads(bool enabled) {
    merge_gamepads = enabled;
}

bool GetMergeGamepads() {
    return merge_gamepads;
}

} // namespace InputCommon
