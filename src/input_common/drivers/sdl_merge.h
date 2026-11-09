// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace InputCommon {

// When on, every connected SDL controller drives the first one. Streaming apps such as
// Moonlight can send the two halves of a Joy-Con pair as two controllers; this joins them.
void SetMergeGamepads(bool enabled);
bool GetMergeGamepads();

} // namespace InputCommon
