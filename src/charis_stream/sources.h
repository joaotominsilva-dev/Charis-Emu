// SPDX-FileCopyrightText: 2026 Charis
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "charis_stream/account.h"
#include "charis_stream/host.h"

namespace CharisStream {

// Windows: the main monitor captured on the GPU (ddagrab), scaled there and encoded by NVENC.
std::unique_ptr<VideoSource> MakeScreenVideoSource();
// Windows: everything the PC plays (WASAPI loopback), encoded as Opus.
std::unique_ptr<AudioSource> MakeLoopbackAudioSource();
// Windows: HTTPS through WinHTTP (system certificates and proxy). Safe to use from any thread.
std::unique_ptr<Http> MakeSystemHttp();

// Windows: the Switch drives the PC's own mouse and keyboard (desktop mode).
class DesktopInput {
public:
    // Applies what changed since the previous state; call from one thread. Typed keys always,
    // the mouse only when with_pointer (desktop mode).
    void Apply(const InputState& state, bool with_pointer = true);
    // Lets go of any button still held (the Switch disconnected or left desktop mode).
    void Release();

private:
    bool have_previous = false;
    uint16_t mouse_x = 0;
    uint16_t mouse_y = 0;
    uint8_t mouse_buttons = 0;
    int32_t wheel = 0;
    uint32_t last_key_sequence = 0;
};

} // namespace CharisStream
